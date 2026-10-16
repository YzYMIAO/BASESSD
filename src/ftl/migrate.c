/*
 * migrate.c — Super Block 搬移引擎（GC / 静态 WL / 慢死预测共用）
 *
 * 职责：
 *   把 victim SB 的有效 CW 逐页搬到 Cold 流新块，更新 L2P / Journal / 有效计数，
 *   最后擦除 victim 并归还（GC）/ 退役（WL）。三处搬移的差别只在**谁当 victim**
 *   （kind）与**擦完怎么处置**（post），故共用一台异步 job。
 *
 * 文件结构：
 *   1. 启动           mig_start（static）/ mig_begin_gc / mig_begin_wl / mig_begin_predict
 *   2. 推进           mig_step（static）/ mig_service
 *   3. 提交原语       mig_submit_read / mig_submit_write / mig_submit_erase（static）
 *   4. 收尾           mig_stall / mig_next_page / mig_finish（static）
 *   5. 完成服务       ftl_op_migrate（g_op_service[FTL_OP_GC] / [FTL_OP_WL]）
 *
 * 设计要点见 include/ftl/migrate.h（状态机、与同步版的差异、单飞行、F10 契约）。
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 *
 * ⚠ **本文件每条命令都走 ftl_async_* + ftl_op_submit（不同步等待）**——服务函数里再跑
 *   完成泵就是嵌套泵（见 ftl_op.h）。搬移主体（读/写/擦）已全部异步，本阶段的目的即此。
 *
 * ⚠ **L2P 反查/改写在独立的纯计算阶段**（MIG_READ_SETTLE / MIG_WRITE_SETTLE，阶段 5）：
 *   读完成时逐 CW 反查有效性、写完成时改 L2P——二者的 LBA 取自 victim 块的**页内 META**，
 *   与刚访问过的 host LBA 无必然联系 ⇒ region miss 是**真实可达**的。放进服务函数里做
 *   就得同步等加载（嵌套泵）；改到纯计算阶段后可以「先起异步加载、原地返回、下轮重扫」。
 *   ⚠ 这两个阶段**必须先就绪前置再动账**（否则半途而废会重复记账 / 位图一半有一半没有）。
 */

#include "ftl/migrate.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"                /* fc_command_release / fc_command_id_to_command */
#include "ftl/bbt.h"              /* bbt_mark_bad */
#include "ftl/ftl.h"              /* ftl_context_t */
#include "ftl/ftl_fc_interface.h" /* ftl_async_read_page / ftl_async_write_page_cw / ftl_async_erase_block */
#include "ftl/ftl_job.h"          /* 心跳（[JOB] 观测口）*/
#include "ftl/ftl_op.h"           /* ftl_op_submit / FTL_OP_GC / FTL_OP_WL */
#include "ftl/gc.h"               /* gc_select_victim / gc_decode_cw_lba */
#include "ftl/journal.h"          /* journal_kick / journal_append / JRN_IDLE */
#include "ftl/l2p_cache.h"        /* l2p_cache_read / l2p_cache_write */
#include "ftl/read_recovery.h"    /* rr_record_ecc / rr_predictive_scan / rr_retire_block */
#include "ftl/wear.h"             /* wear_mark_dirty */
#include "ftl/wear_leveling.h"    /* wl_should_trigger / wl_select_victim */

#include <string.h>

/* 前置声明：状态机主体定义在第 2 节（mig_start 要先见到它）。 */
static void mig_step(ftl_context_t *ftl);

/* victim 在指定 plane 上的全局块号（BST 索引）。
 * ⚠ **唯一真值是 `victim.plane_blocks[plane]`，不缓存在 job 里**：快路径
 *   （valid_total == 0 ⇒ 直接 MIG_ERASE）从不经过 READ 阶段，任何"读阶段填、擦阶段用"
 *   的缓存字段在快路径下都是未初始化的 0 ⇒ 擦除收尾会去改**块 0** 的元数据
 *   （erase_count / clear_valid / allocator_free_block）。原 gc_erase_and_free 也是
 *   每次现算（`ftl_gblock(plane, victim->plane_blocks[plane])`），照抄它的口径。
 * 调用点已保证 super_block_has_plane() 为真（哨兵 plane 不展开）。 */
static uint16_t mig_src_gblock(const mig_job_t *m, uint8_t plane)
{
    return ftl_gblock(plane, m->victim.plane_blocks[plane]);
}

/* ── 1. 启动 ── */

/* 提交一条搬移命令时用的 op tag：GC 与 WL 共用同一服务函数，但 tag 分开——
 * 日志/调试时能一眼看出这条命令属于哪条路径（g_op_service 两个槽指向同一函数）。 */
static ftl_op_id_t mig_op_tag(const mig_job_t *m)
{
    return (m->kind == MIG_KIND_GC) ? FTL_OP_GC : FTL_OP_WL;
}

/* --------------------------------------------------------------------
 * mig_start
 *   功能：装配并启动一个搬移 job（SELECT 阶段）。
 *   参数：ftl —— FTL 实例；victim —— 选中的 SB（只读，内部按值拷贝）；
 *         kind —— victim 来源；post —— 收尾方式。
 *   返回：1 = 已启动；0 = 未启动（job 忙 / 参数非法）。
 *   实现要点：**快路径**——双 plane valid_count 之和 == 0 ⇒ 直接进 ERASE（省整块 NAND
 *             读；这正是原先「写过」位图覆盖的主场景——整块空洞 plane，其根因 P2-6
 *             已由 v0.66 页打包写缓冲消除）。
 *             victim 按值拷贝：closed_sb_list 在搬移期间可能被 allocator 追加/挪动，
 *             持指针会让「本 job 的 victim」随列表漂移。
 * -------------------------------------------------------------------- */
static int mig_start(ftl_context_t *ftl, const super_block_t *victim, mig_kind_t kind, mig_post_t post)
{
    mig_job_t *m;
    uint32_t   valid_total = 0U;
    uint8_t    p;

    if ((ftl == NULL) || (victim == NULL) || (ftl->mig.active != 0U))
    {
        return 0;
    }

    m = &ftl->mig;

    memset(m, 0, sizeof(*m));
    m->victim       = *victim;
    m->victim.valid = 0U; /* 本引擎只处理 Closed SB（valid==0 是"已关闭"语义，见 allocator.h） */
    m->kind         = (uint8_t)kind;
    m->post         = (uint8_t)post;
    m->page         = 0U;
    m->plane        = 0U;
    m->active       = 1U;

    /* 快路径：无任何有效 CW ⇒ 无可搬移，直接进擦除。 */
    for (p = 0U; p < PLANE_NUM; p++)
    {
        if (super_block_has_plane(&m->victim, p) != 0U)
        {
            valid_total += ftl->alloc.blocks[ftl_gblock(p, m->victim.plane_blocks[p])].valid_count;
        }
    }
    m->phase = (valid_total == 0U) ? (uint8_t)MIG_ERASE : (uint8_t)MIG_READ;

    mig_step(ftl);
    return 1;
}

int mig_begin_gc(ftl_context_t *ftl)
{
    super_block_t *victim;

    if ((ftl == NULL) || (ftl->mig.active != 0U))
    {
        return 0;
    }

    if (gc_select_victim(&ftl->alloc, &victim) == 0)
    {
        /* 触发但选不出 victim = 卡死嫌疑（观测计数，见 ftl.h）。 */
        ftl->gc_novictim_count++;
        return 0;
    }

    return mig_start(ftl, victim, MIG_KIND_GC, MIG_POST_FREE);
}

int mig_begin_wl(ftl_context_t *ftl)
{
    wl_victim_t   victim;
    super_block_t sb;

    if ((ftl == NULL) || (ftl->mig.active != 0U))
    {
        return 0;
    }
    if (wl_should_trigger(ftl) == 0)
    {
        return 0;
    }
    if (wl_select_victim(ftl, &victim) == 0)
    {
        return 0;
    }

    /* wl_victim_t 只带 plane_blocks ⇒ 补上拓扑（super_block_page_pba 需要 sb->topo.lun）。 */
    memset(&sb, 0, sizeof(sb));
    sb.topo = ftl->topo;
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        sb.plane_blocks[p] = victim.plane_blocks[p];
    }

    return mig_start(ftl, &sb, MIG_KIND_WL, MIG_POST_RETIRE);
}

int mig_begin_predict(ftl_context_t *ftl)
{
    uint16_t      gblock;
    uint16_t      closed_idx;
    super_block_t sb;

    if ((ftl == NULL) || (ftl->mig.active != 0U))
    {
        return 0;
    }
    if (rr_predictive_scan(&ftl->alloc, &gblock) == 0)
    {
        return 0;
    }

    /* 慢死块必在 closed_sb_list 中（scan 只返回 CLOSED 块） */
    closed_idx = ftl_block_sb_id(&ftl->alloc.blocks[gblock]);
    ASSERT(closed_idx < allocator_closed_count(&ftl->alloc));

    memset(&sb, 0, sizeof(sb));
    sb.topo = ftl->topo;
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        sb.plane_blocks[p] = allocator_closed_at(&ftl->alloc, closed_idx)->plane_blocks[p];
    }

    return mig_start(ftl, &sb, MIG_KIND_PREDICT, MIG_POST_RETIRE);
}

/* ── 3. 提交原语 ── */

/* 提交一条 victim 页读（整页，4 CW）。数据在完成时由服务函数拷进 m->page_buf
 * ——**读**不像写那样在 prepare 阶段搬数据（见 ftl_async_read_page 的注释）。 */
static ftl_status_t mig_submit_read(ftl_context_t *ftl)
{
    mig_job_t    *m = &ftl->mig;
    fc_command_t *cmd;

    if (ftl_async_read_page(&cmd, m->src[m->plane], FTL_PAGE_CFG_DATA_ANY()) != FTL_OK)
    {
        return FTL_AGAIN; /* 命令池空 */
    }

    /* waiting 先于提交置位、提交失败再回滚（同 rr_submit_read）。 */
    m->waiting = 1U;

    if (ftl_op_submit(cmd, mig_op_tag(m), m) != FTL_OK)
    {
        m->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    /* ⚠ 卡死计数在**提交成功之后**才清零（不是准备成功时）：`ftl_op_submit` 自身失败
     *   （完成表满 / SQ 满）也是一轮"提交不出去"，若在此前清零则计数永远停在 1、
     *   中止判据 MIG_SUBMIT_STALL_MAX 永不触发。 */
    m->stall_rounds = 0U;

    return FTL_OK;
}

/* 提交一条目标页 CW 子集写。只编有效 CW（cw_bitmap 外的 CW 不建链、不编程）——
 * 整页重写会把无效 CW 连同其（陈旧的）META 一起搬过去，污染新页且多搬无用数据。
 * ⚠ 目标页的 plane 位就是 plane（allocator_alloc_pair 的契约），故写进去的 META
 *   逐 CW 仍是正确的扇区 LBA。 */
static ftl_status_t mig_submit_write(ftl_context_t *ftl)
{
    mig_job_t    *m = &ftl->mig;
    fc_command_t *cmd;

    if (ftl_async_write_page_cw(&cmd,
                                m->dst[m->plane],
                                m->page_buf[m->plane],
                                m->cw_bitmap[m->plane],
                                FTL_PAGE_CFG_DATA(ftl_lba_page(m->base[m->plane]))) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    m->waiting = 1U;

    if (ftl_op_submit(cmd, mig_op_tag(m), m) != FTL_OK)
    {
        m->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    m->stall_rounds = 0U; /* 见 mig_submit_read：清零在提交成功之后 */

    return FTL_OK;
}

/* 提交一条 victim 块擦除。 */
static ftl_status_t mig_submit_erase(ftl_context_t *ftl)
{
    mig_job_t    *m = &ftl->mig;
    fc_command_t *cmd;
    nand_pba_t    block_addr;

    block_addr = super_block_page_pba(ftl->alloc.ch, ftl->alloc.ce, &m->victim, m->plane, 0U);

    if (ftl_async_erase_block(&cmd, block_addr) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    m->waiting = 1U;

    if (ftl_op_submit(cmd, mig_op_tag(m), m) != FTL_OK)
    {
        m->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    m->stall_rounds = 0U; /* 见 mig_submit_read：清零在提交成功之后 */

    return FTL_OK;
}

/* ── 4. 收尾 ── */

/* 累计一次「提交不出去」；超限即中止 job。返回 1 = 已中止。 */
static int mig_stall(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    if (m->stall_rounds >= MIG_SUBMIT_STALL_MAX)
    {
        LOGW("[MIG] stall %u rounds — abort kind=%u phase=%u page=%u p0=%u p1=%u\r\n",
             (unsigned)m->stall_rounds,
             (unsigned)m->kind,
             (unsigned)m->phase,
             (unsigned)m->page,
             (unsigned)m->victim.plane_blocks[0],
             (unsigned)m->victim.plane_blocks[1]);
        m->aborted = 1U;
        m->phase   = (uint8_t)MIG_DONE;
        return 1;
    }
    m->stall_rounds++;
    return 0;
}

/* 推进到下一页（清本页状态）。读/写两阶段的页推进都走这里。 */
static void mig_next_page(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    m->page      = (uint8_t)(m->page + 1U);
    m->plane     = 0U;
    m->decode_ok = 0U;
    m->write_ok  = 0U;
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        m->cw_bitmap[p] = 0U;
        m->base[p]      = 0U;
    }
    m->phase = (uint8_t)MIG_READ;
}

/* --------------------------------------------------------------------
 * mig_finish
 *   功能：DONE——移除 closed 条目、记进展计数、复位 job。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 *   实现要点：定位 victim 在 closed_sb_list 的索引时**两 plane 都比对**（Policy B 下
 *             退化单 plane 条目的 plane_blocks[0] 是哨兵，只比 plane0 会命中另一条
 *             同哨兵条目——原 gc_erase_and_free 的同一坑）。
 *             aborted ⇒ 不做任何列表/计数变更（victim 未被擦，仍有效）。
 * -------------------------------------------------------------------- */
static void mig_finish(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    if (m->aborted == 0U)
    {
        uint16_t closed_idx = UINT16_MAX;

        if (allocator_closed_count(&ftl->alloc) > 0U)
        {
            for (uint16_t i = 0U; i < allocator_closed_count(&ftl->alloc); i++)
            {
                const super_block_t *e = allocator_closed_at(&ftl->alloc, i);

                if ((e->plane_blocks[0] == m->victim.plane_blocks[0]) &&
                    (e->plane_blocks[1] == m->victim.plane_blocks[1]))
                {
                    closed_idx = i;
                    break;
                }
            }
            if (closed_idx >= allocator_closed_count(&ftl->alloc))
            {
                closed_idx = UINT16_MAX; /* 未找到（非 Closed 列表成员）：无需移除 */
            }
        }

        /* 移除已回收条目（swap-remove）——防 closed_sb_count 只增不减导致列表爆炸
         * + 已回收 SB 被再次选为 victim 的无限 GC 空转。 */
        if (closed_idx != UINT16_MAX)
        {
            allocator_remove_closed_sb(&ftl->alloc, closed_idx);
        }

        if (m->kind == MIG_KIND_GC)
        {
            ftl->gc_reclaimed_count++; /* 进展计数（见 ftl.h：区分"在推进"与"卡死"） */
        }
        else if (m->kind == MIG_KIND_PREDICT)
        {
            /* 慢死预测命中：整 SB 已搬空并退役，属生命周期跃迁，留痕一次 */
            LOGW("[FTL] predictive relocate retired p0=%u p1=%u\r\n",
                 (unsigned)m->victim.plane_blocks[0],
                 (unsigned)m->victim.plane_blocks[1]);
        }
    }

    m->active       = 0U;
    m->phase        = (uint8_t)MIG_IDLE;
    m->waiting      = 0U;
    m->decode_ok    = 0U;
    m->write_ok     = 0U;
    m->stall_rounds = 0U;
}

/* ── 2. 推进 ── */

/* --------------------------------------------------------------------
 * mig_step
 *   功能：状态机主体——一次只提交**一条**命令就返回；纯计算阶段连续推进到下一个
 *         提交阶段或终态。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 *   实现要点：阶段序列见 migrate.h。提交失败（背压）⇒ 原地返回，由 mig_service 下轮
 *             重试同一阶段（幂等）；连续 MIG_SUBMIT_STALL_MAX 轮无进展 ⇒ 中止。
 * -------------------------------------------------------------------- */
static void mig_step(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    for (;;)
    {
        if (m->waiting != 0U)
        {
            return; /* 有命令在飞：等完成服务函数 */
        }

        switch ((mig_phase_t)m->phase)
        {
            case MIG_READ:
            {
                uint8_t p;

                if (m->page >= PAGES_PER_BLOCK)
                {
                    m->phase = (uint8_t)MIG_FLUSH_JRN; /* 全部页读完 */
                    break;
                }

                /* 找本页下一个有物理块的 plane */
                for (p = m->plane; p < PLANE_NUM; p++)
                {
                    if (super_block_has_plane(&m->victim, p) != 0U)
                    {
                        break;
                    }
                }
                m->plane = p;

                if (m->plane >= PLANE_NUM)
                {
                    /* 本页两 plane 都处理完 ⇒ 决定去向 */
                    uint8_t any = 0U;

                    for (p = 0U; p < PLANE_NUM; p++)
                    {
                        if (m->cw_bitmap[p] != 0U)
                        {
                            any = 1U;
                        }
                    }
                    if (any == 0U)
                    {
                        mig_next_page(ftl); /* 空洞 / 全被覆盖 */
                        break;
                    }

                    /* 成对分配目标页（Cold 流）。池耗尽 ⇒ 中止（不擦 victim，源数据仍在、
                     * L2P 未动，下轮 GC 再来）——与同步版"擦掉未搬页"的缺陷相反。 */
                    if (allocator_alloc_pair(&ftl->alloc, FTL_TEMP_COLD, m->dst) == NAND_PBA_INVALID)
                    {
                        LOGW("[MIG] no free block page=%u p0=%u p1=%u — abort, victim kept\r\n",
                             (unsigned)m->page,
                             (unsigned)m->victim.plane_blocks[0],
                             (unsigned)m->victim.plane_blocks[1]);
                        m->aborted = 1U;
                        m->phase   = (uint8_t)MIG_DONE;
                        break;
                    }

                    m->plane = 0U;
                    m->phase = (uint8_t)MIG_WRITE;
                    break;
                }

                /* 本页本 plane 的源地址（页基址）。全局块号不缓存 —— 现算，见 mig_src_gblock。 */
                m->src[m->plane] = super_block_page_pba(ftl->alloc.ch, ftl->alloc.ce, &m->victim, m->plane, m->page);
                memset(m->page_buf[m->plane], 0, PAGE_MAIN_SIZE);

                if (mig_submit_read(ftl) != FTL_OK)
                {
                    (void)mig_stall(ftl);
                }
                return;
            }

            case MIG_READ_SETTLE:
            {
                const uint8_t plane = m->plane;

                if (m->decode_ok == 0U)
                {
                    /* 读失败 / 全页空洞 / META 解不出：本 plane 无有效 CW */
                    m->cw_bitmap[plane] = 0U;
                }
                else
                {
                    uint8_t cw;

                    /* ── 就绪前置（阶段 5）──
                     * 本页逐 CW 的扇区 LBA 来自页内 META，与刚访问过的 host LBA **无必然
                     * 联系** ⇒ region miss 现实可达。未驻留 ⇒ 起异步加载并原地返回，
                     * 下轮重扫（重扫幂等：ready 命中不记账、不动 access_count）。 */
                    for (cw = 0U; cw < CW_NUM; cw++)
                    {
                        if (m->cw_lba[plane][cw] == FTL_LBA_INVALID)
                        {
                            continue;
                        }
                        if (l2p_cache_ready(&ftl->cache, m->cw_lba[plane][cw]) == 0)
                        {
                            m->stall_rounds = 0U; /* 加载在飞 = 有进展，不是提交卡死 */
                            return;
                        }
                    }

                    /* 全部就绪 ⇒ 一次性算出本 plane 的位图（不会半途而废） */
                    m->cw_bitmap[plane] = 0U;
                    for (cw = 0U; cw < CW_NUM; cw++)
                    {
                        nand_pba_t cur;

                        if (m->cw_lba[plane][cw] == FTL_LBA_INVALID)
                        {
                            continue;
                        }
                        /* 用**该 CW 自己的 META** 查，仍指向 (本页, 本 CW) 才算有效 */
                        if ((l2p_cache_read(&ftl->cache, m->cw_lba[plane][cw], &cur) == FTL_OK) &&
                            (cur == pba_with_cw(m->src[plane], cw)))
                        {
                            m->cw_bitmap[plane] |= (uint32_t)1U << (((uint32_t)plane * CW_NUM) + cw);
                        }
                    }
                }

                m->decode_ok = 0U;
                m->phase     = (uint8_t)MIG_READ;
                m->plane     = (uint8_t)(plane + 1U); /* 可能 == PLANE_NUM ⇒ 页结算 */
                break;
            }

            case MIG_WRITE_SETTLE:
            {
                const uint8_t plane = m->plane;

                if (m->write_ok != 0U)
                {
                    uint8_t cw;

                    /* 就绪前置（同 READ_SETTLE）：全部就绪才动账——半途而废会重复记账
                     * （valid_count 虚高、journal 重复条目）。 */
                    for (cw = 0U; cw < CW_NUM; cw++)
                    {
                        if ((m->cw_bitmap[plane] & ((uint32_t)1U << cw)) == 0U)
                        {
                            continue;
                        }
                        if (l2p_cache_ready(&ftl->cache, m->cw_lba[plane][cw]) == 0)
                        {
                            m->stall_rounds = 0U;
                            return;
                        }
                    }

                    for (cw = 0U; cw < CW_NUM; cw++)
                    {
                        nand_pba_t new_pba;
                        lba_t      l;

                        if ((m->cw_bitmap[plane] & ((uint32_t)1U << cw)) == 0U)
                        {
                            continue;
                        }

                        l       = m->cw_lba[plane][cw]; /* 用该 CW 自己的 META（不再 base+cw 推断） */
                        new_pba = pba_with_cw(m->dst[plane], cw);

                        (void)l2p_cache_write(&ftl->cache, l, new_pba);
                        journal_append(&ftl->journal,
                                       l,
                                       pba_with_cw(m->src[plane], cw),
                                       new_pba);

                        /* 有效计数（CW 级）：源块 -1，新块 +1 */
                        ftl_block_sub_valid(&ftl->alloc.blocks[mig_src_gblock(m, plane)]);
                        ftl_block_add_valid(&ftl->alloc.blocks[ftl_pba_gblock(new_pba)]);

                        m->migrated++;
                    }
                }

                m->write_ok = 0U;
                m->phase    = (uint8_t)MIG_WRITE;
                m->plane    = (uint8_t)(plane + 1U);
                break;
            }

            case MIG_WRITE:
            {
                uint8_t p;

                /* 找本页下一个有待写 CW 的 plane */
                for (p = m->plane; p < PLANE_NUM; p++)
                {
                    if (m->cw_bitmap[p] != 0U)
                    {
                        break;
                    }
                }
                m->plane = p;

                if (m->plane >= PLANE_NUM)
                {
                    mig_next_page(ftl);
                    break;
                }

                if (mig_submit_write(ftl) != FTL_OK)
                {
                    (void)mig_stall(ftl);
                }
                return;
            }

            case MIG_FLUSH_JRN:
                /* F10 契约：搬移条目先落盘再擦 victim。journal 是 write-back（page_buf
                 * 攒满才 flush），不在此强制落盘，掉电落在"条目未落盘 + victim 已擦除"
                 * 窗口 ⇒ 恢复回放指向已擦除块 = 数据损坏。
                 * 无搬移条目（migrated == 0）⇒ 无需屏障，直接擦。 */
                if (m->migrated == 0U)
                {
                    m->phase = (uint8_t)MIG_ERASE;
                    m->plane = 0U;
                    break;
                }

                journal_kick(&ftl->journal); /* job 空闲且有待写页 ⇒ 起异步刷页 */

                if (ftl->journal.job.phase != JRN_IDLE)
                {
                    m->stall_rounds = 0U; /* 有命令在飞 = 在推进，不计卡死 */
                    return;
                }
                if ((ftl->journal.job.need_erase != 0U) || (ftl->journal.current_entry != 0U))
                {
                    /* 想刷却刷不动（命令池空）⇒ 计卡死轮数；超限中止（**不擦 victim**） */
                    if (mig_stall(ftl) != 0)
                    {
                        return;
                    }
                    return;
                }

                m->stall_rounds = 0U; /* 屏障达成 */
                m->phase        = (uint8_t)MIG_ERASE;
                m->plane        = 0U;
                break;

            case MIG_ERASE:
            {
                uint8_t p;

                for (p = m->plane; p < PLANE_NUM; p++)
                {
                    if (super_block_has_plane(&m->victim, p) != 0U)
                    {
                        break;
                    }
                }
                m->plane = p;

                if (m->plane >= PLANE_NUM)
                {
                    m->phase = (uint8_t)MIG_DONE;
                    break;
                }

                if (mig_submit_erase(ftl) != FTL_OK)
                {
                    (void)mig_stall(ftl);
                }
                return;
            }

            case MIG_DONE:
                mig_finish(ftl);
                return;

            case MIG_IDLE:
            default:
                /* 不该发生：active 置位而 phase 非法 */
                ASSERT(m->active == 0U);
                return;
        }
    }
}

void mig_service(ftl_context_t *ftl)
{
    mig_job_t *m;

    if (ftl == NULL)
    {
        return;
    }

    m = &ftl->mig;

    if (m->active == 0U)
    {
        ftl_job_heartbeat_reset(&m->log_ms, &m->start_ms);
        return;
    }

    /* 心跳（见 ftl_job.h）：搬移最长（64 页 × 2 plane），没有它卡死不可见 */
    if (ftl_job_heartbeat(&m->log_ms, &m->start_ms) != 0)
    {
        LOGI("[MIG] job kind=%u phase=%u page=%u plane=%u aborted=%u age=%lums\r\n",
             (unsigned)m->kind,
             (unsigned)m->phase,
             (unsigned)m->page,
             (unsigned)m->plane,
             (unsigned)m->aborted,
             (unsigned long)ftl_job_age_ms(&m->start_ms));
    }

    mig_step(ftl);
}

/* ── 5. 完成服务（g_op_service[FTL_OP_GC] / [FTL_OP_WL]）── */

/* --------------------------------------------------------------------
 * ftl_op_migrate
 *   功能：搬移命令的完成服务函数——按 job.phase 判完成结果、改 phase、release 命令，
 *         再交回 mig_step 续跑。
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `mig_job_t *`（= &ftl->mig）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：
 *     · READ 成功 ⇒ 拷数据 + 解 META 得逐 CW 的扇区 LBA → 逐 CW 反查 L2P 得 cw_bitmap；
 *       顺带记 ECC 趋势 + read_count（慢死预测闭环）。失败 / 全页空洞 ⇒ 该 plane 无有效
 *       CW（cw_bitmap 保持 0），不影响另一 plane。
 *     · WRITE 成功 ⇒ 逐 CW l2p_cache_write + journal_append + 有效计数迁移（源块 -1、
 *       新块 +1）。失败 ⇒ **不动 L2P**（源页仍有效，下轮再搬）。
 *     · ERASE 成功 ⇒ erase_count++ / last_pe_ts / wear_mark_dirty / 清 valid / 归还
 *       Free Pool；WL 与慢死预测额外 rr_retire_block（RESERVED：不再分配、不标坏）。
 *       失败 ⇒ bbt_mark_bad。
 * -------------------------------------------------------------------- */
void ftl_op_migrate(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    mig_job_t    *m = (mig_job_t *)ctx;
    fc_command_t *cmd;

    ASSERT(completion != NULL);
    ASSERT(m != NULL);

    cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    /* 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出来的归属实例，
     * 二者必须一致——不一致说明派发路由错了。 */
    ASSERT(ftl != NULL);
    ASSERT(&ftl->mig == m);

    switch ((mig_phase_t)m->phase)
    {
        case MIG_READ:
        {
            const uint8_t plane = m->plane;

            m->decode_ok = 0U;

            if ((completion->status & FC_CW_STATUS_ERROR_MASK) == 0U)
            {
                (void)ftl_cmd_read_data(cmd, m->page_buf[plane], PAGE_MAIN_SIZE);
                if (gc_decode_cw_lba(cmd, m->src[plane], m->cw_lba[plane], &m->base[plane]) == 0)
                {
                    ftl_block_t *src_blk = &ftl->alloc.blocks[mig_src_gblock(m, plane)];

                    /* B4：搬移读记录 ECC 趋势 + read disturb 计数（慢死预测闭环）。
                     * 判定只用 ecc_bits 与上一次读数比较，不涉及时间维度。 */
                    rr_record_ecc(&src_blk->rr, fc_get_cmd_ecc_max(cmd));
                    src_blk->read_count++;
                    wear_mark_dirty(&ftl->alloc, mig_src_gblock(m, plane));

                    m->decode_ok = 1U;
                }
            }

            /* 逐 CW 反查 L2P 改在 MIG_READ_SETTLE（纯计算阶段，可挂起等 region 加载） */
            (void)fc_command_release(cmd);
            m->waiting      = 0U;
            m->stall_rounds = 0U;
            m->phase        = (uint8_t)MIG_READ_SETTLE;
            mig_step(ftl);
            return;
        }

        case MIG_WRITE:
        {
            m->write_ok = ((completion->status & FC_CW_STATUS_ERROR_MASK) == 0U) ? 1U : 0U;

            /* 改 L2P/Journal + 迁移有效计数改在 MIG_WRITE_SETTLE（纯计算，可挂起） */
            (void)fc_command_release(cmd);
            m->waiting      = 0U;
            m->stall_rounds = 0U;
            m->phase        = (uint8_t)MIG_WRITE_SETTLE;
            mig_step(ftl);
            return;
        }

        case MIG_ERASE:
        {
            uint8_t plane = m->plane;

            if ((completion->status & FC_CW_STATUS_ERROR_MASK) == 0U)
            {
                const uint16_t gsrc = mig_src_gblock(m, plane);
                ftl_block_t   *blk  = &ftl->alloc.blocks[gsrc];

                /* 擦除成功：擦除计数 +1，记录最近擦除时间戳（单调写入量），valid_count 归零，归还 Free Pool */
                blk->erase_count++;
                blk->last_pe_ts = ftl->total_writes;
                wear_mark_dirty(&ftl->alloc, gsrc);
                ftl_block_clear_valid(blk);
                allocator_free_block(&ftl->alloc, gsrc);

                /* WL / 慢死预测：搬空的块**退役**（RESERVED，不再分配新数据、不标坏、仍可读）。
                 * rr_retire_block 内部把 read_count 归零（read disturb 重计），它只拿到
                 * ftl_block_t* ⇒ 脏标记放在这里补。 */
                if ((mig_post_t)m->post == MIG_POST_RETIRE)
                {
                    rr_retire_block(blk);
                    wear_mark_dirty(&ftl->alloc, gsrc);
                }
            }
            else
            {
                /* 擦除失败：标记坏块（BBT 单数据源，后台持久化） */
                (void)bbt_mark_bad(&ftl->alloc, mig_src_gblock(m, plane));
            }

            (void)fc_command_release(cmd);
            m->waiting      = 0U;
            m->stall_rounds = 0U;
            m->plane        = (uint8_t)(plane + 1U);
            mig_step(ftl);
            return;
        }

        case MIG_IDLE:
        case MIG_READ_SETTLE:
        case MIG_WRITE_SETTLE:
        case MIG_FLUSH_JRN:
        case MIG_DONE:
        default:
            /* 不该发生：一条命令对应一个提交阶段（READ / WRITE / ERASE）。 */
            LOGE("[MIG] unexpected completion phase=%u\r\n", (unsigned)m->phase);
            (void)fc_command_release(cmd);
            m->waiting = 0U;
            m->aborted = 1U;
            m->phase   = (uint8_t)MIG_DONE;
            mig_step(ftl);
            return;
    }
}
