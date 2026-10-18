/*
 * migrate.c — Super Block 搬移引擎（GC / 静态 WL / 慢死预测共用）
 *
 * 职责：
 *   把 victim SB 的有效 CW 逐页搬到 Cold 流新块，更新 L2P / Journal / 有效计数，
 *   最后擦除 victim 并归还（GC）/ 退役（WL）。三处搬移的差别只在**谁当 victim**
 *   （kind）与**擦完怎么处置**（post），故共用一台异步 job。
 *
 * 文件结构：
 *   1. 内部工具       mig_src_gblock / mig_victim_from_planes / mig_next_*（static）
 *   2. 提交原语       mig_submit / mig_submit_read / mig_submit_write / mig_submit_erase（static）
 *   3. 启动           mig_start（static）/ mig_begin_gc / mig_begin_wl / mig_begin_predict
 *   4. 收尾           mig_abort / mig_stall / mig_next_page / mig_finish（static）
 *   5. 推进           ／各 phase 处理函数（static）/ mig_step（static）/ mig_service
 *   6. 完成服务       ftl_op_migrate（g_op_service[FTL_OP_GC] / [FTL_OP_WL]）
 *
 * 设计要点见 include/ftl/migrate.h（状态机、单飞行、F10 契约）。
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 *
 * ⚠ **本文件每条命令都走 ftl_async_* + ftl_op_submit（不同步等待）**——服务函数里再跑
 *   完成泵就是嵌套泵（见 ftl_op.h）。搬移主体（读/写/擦）全部异步。
 *
 * ⚠ **L2P 反查/改写在独立的纯计算阶段**（MIG_READ_SETTLE / MIG_WRITE_SETTLE）：
 *   读完成时逐 CW 反查有效性、写完成时改 L2P——二者的 LBA 取自 victim 块的**页内 META**，
 *   与刚访问过的 host LBA 无必然联系 ⇒ region miss 是**真实可达**的。放进服务函数里做
 *   就得同步等加载（嵌套泵）；放在纯计算阶段可以「先起异步加载、原地返回、下轮重扫」。
 *   ⚠ 这两个阶段**必须先就绪前置再动账**（否则半途而废会重复记账 / 位图一半有一半没有）。
 */

#include "migrate.h"

#include "assertion.h"
#include "bbt.h"              /* bbt_mark_bad */
#include "fc.h"               /* fc_command_release / fc_command_id_to_command */
#include "ftl.h"              /* ftl_context_t */
#include "ftl_fc_interface.h" /* ftl_async_read_page / ftl_async_write_page_cw / ftl_async_erase_block */
#include "ftl_job.h"          /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_GC / FTL_OP_WL */
#include "gc.h"               /* gc_select_victim / gc_decode_cw_lba */
#include "journal.h"          /* journal_kick / journal_append / JRN_IDLE */
#include "l2p_cache.h"        /* l2p_cache_read / l2p_cache_write */
#include "log.h"
#include "read_recovery.h" /* rr_record_ecc / rr_predictive_scan / rr_retire_block */
#include "wear_leveling.h" /* wl_should_trigger / wl_select_victim */

#include <string.h>

/* 前置声明：状态机主体定义在第 5 节（mig_start 要先见到它）。 */
static void mig_step(ftl_context_t *ftl);

/* ── 1. 内部工具 ── */

/* victim 在指定 plane 上的全局块号（BST 索引）。
 * ⚠ **唯一真值是 `victim.plane_blocks[plane]`，不缓存在 job 里**：快路径
 *   （valid_total == 0 ⇒ 直接 MIG_ERASE）从不经过 READ 阶段，任何"读阶段填、擦阶段用"
 *   的缓存字段在快路径下都是未初始化的 0 ⇒ 擦除收尾会去改**块 0** 的元数据
 *   （erase_count / clear_valid / allocator_free_block）。
 * 调用点已保证 super_block_has_plane() 为真（哨兵 plane 不展开）。 */
static uint16_t mig_src_gblock(const mig_job_t *m, uint8_t plane)
{
    return ftl_gblock(plane, m->victim.plane_blocks[plane]);
}

/* 用 plane_blocks[] 组装本引擎的 victim 视图。
 * temperature / next_page 不参与搬移决策，保持 memset 的默认值。 */
static void mig_victim_from_planes(super_block_t *sb, const uint16_t plane_blocks[PLANE_NUM])
{
    memset(sb, 0, sizeof(*sb));
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        sb->plane_blocks[p] = plane_blocks[p];
    }
}

/* 从 from 起找下一个**有物理块**的 plane；没有返回 PLANE_NUM。
 * 退化单 plane 条目的哨兵 plane 不展开（super_block_has_plane）。 */
static uint8_t mig_next_present_plane(const mig_job_t *m, uint8_t from)
{
    uint8_t p;

    for (p = from; p < PLANE_NUM; p++)
    {
        if (super_block_has_plane(&m->victim, p))
        {
            break;
        }
    }
    return p;
}

/* 从 from 起找下一个**本页有有效 CW** 的 plane；没有返回 PLANE_NUM。 */
static uint8_t mig_next_data_plane(const mig_job_t *m, uint8_t from)
{
    uint8_t p;

    for (p = from; p < PLANE_NUM; p++)
    {
        if (m->cw_bitmap[p] != 0U)
        {
            break;
        }
    }
    return p;
}

/* ── 2. 提交原语 ── */

/* 提交一条搬移命令时用的 op tag：GC 与 WL 共用同一服务函数，但 tag 分开——
 * 日志/调试时能一眼看出这条命令属于哪条路径（g_op_service 两个槽指向同一函数）。 */
static ftl_op_id_t mig_op_tag(const mig_job_t *m)
{
    return (m->kind == MIG_KIND_GC) ? FTL_OP_GC : FTL_OP_WL;
}

/* 提交一条已建好的搬移命令：置 waiting、提交、失败回滚。
 * ⚠ stall_rounds 只在**提交成功之后**清零：`ftl_op_submit` 自身失败（完成表满 / SQ 满）
 *   也是一轮"提交不出去"，提前清零会让 MIG_SUBMIT_STALL_MAX 永不触发。 */
static ftl_status_t mig_submit(ftl_context_t *ftl, fc_command_t *cmd)
{
    mig_job_t *m = &ftl->mig;

    m->waiting = true;

    if (ftl_op_submit(cmd, mig_op_tag(m), m) != FTL_OK)
    {
        m->waiting = false;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    m->stall_rounds = 0U;
    return FTL_OK;
}

/* 提交一条 victim 页读（整页，4 CW）。数据在完成时由服务函数拷进 m->page_buf
 * ——**读**不像写那样在 prepare 阶段搬数据（见 ftl_async_read_page 的注释）。 */
static ftl_status_t mig_submit_read(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    fc_command_t      *cmd;
    const ftl_status_t ret = ftl_async_read_page(&cmd, m->src[m->plane], FTL_PAGE_CFG_DATA_ANY());
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_submit(ftl, cmd);
}

/* 提交一条目标页 CW 子集写。只编有效 CW（cw_bitmap 外的 CW 不建链、不编程）——
 * 整页重写会把无效 CW 连同其（陈旧的）META 一起搬过去，污染新页且多搬无用数据。
 * ⚠ 目标页的 plane 位就是 plane（allocator_alloc_pair 的契约），故写进去的 META
 *   逐 CW 仍是正确的扇区 LBA。 */
static ftl_status_t mig_submit_write(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    fc_command_t      *cmd;
    const ftl_status_t ret = ftl_async_write_page_cw(&cmd,
                                                     m->dst[m->plane],
                                                     m->page_buf[m->plane],
                                                     m->cw_bitmap[m->plane],
                                                     FTL_PAGE_CFG_DATA(ftl_lba_page(m->base[m->plane])));
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_submit(ftl, cmd);
}

/* 提交一条 victim 块擦除。 */
static ftl_status_t mig_submit_erase(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    const nand_pba_t block_addr = super_block_page_pba(ftl->topo, &m->victim, m->plane, 0U);

    fc_command_t      *cmd;
    const ftl_status_t ret = ftl_async_erase_block(&cmd, block_addr);
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_submit(ftl, cmd);
}

/* ── 3. 启动 ── */

/* --------------------------------------------------------------------
 * mig_start
 *   功能：装配并启动一个搬移 job（SELECT 阶段）。
 *   参数：ftl —— FTL 实例；victim —— 选中的 SB（只读，内部按值拷贝）；
 *         kind —— victim 来源；post —— 收尾方式。三者都是调用者保证的前置条件。
 *   实现要点：**快路径**——各 plane valid_count 之和 == 0 ⇒ 直接进 ERASE（省整块 NAND
 *             读；整块空洞 plane 的根因已由页打包写缓冲消除）。
 *             victim 按值拷贝：closed_sb_list 在搬移期间可能被 allocator 追加/挪动，
 *             持指针会让「本 job 的 victim」随列表漂移（allocator.h 不变量③）。
 * -------------------------------------------------------------------- */
static void mig_start(ftl_context_t *ftl, const super_block_t *victim, mig_kind_t kind, mig_post_t post)
{
    /* 前置条件（判据 2）：唯一调用者是本文件的 mig_begin_*，各自已断言 ftl / victim；
     * active 是运行时状态（单飞行），由调用者在选块**之前**判过 ⇒ 这里是死守卫。 */
    ASSERT(ftl != NULL);
    ASSERT(victim != NULL);
    ASSERT(!ftl->mig.active);

    mig_job_t *m = &ftl->mig;

    memset(m, 0, sizeof(*m));
    m->victim         = *victim;
    m->victim.is_open = false; /* 本引擎只处理 Closed SB（见 allocator.h） */
    m->kind           = (uint8_t)kind;
    m->post           = (uint8_t)post;
    m->active         = true;

    /* 快路径：无任何有效 CW ⇒ 无可搬移，直接进擦除。 */
    allocator_sb_stats_t st;
    allocator_sb_stats(&ftl->alloc, &m->victim, &st);
    m->phase = (st.valid_cw == 0U) ? (uint8_t)MIG_ERASE : (uint8_t)MIG_READ;

    mig_step(ftl);
}

bool mig_begin_gc(ftl_context_t *ftl)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    ASSERT(ftl != NULL);
    if (ftl->mig.active)
    {
        return false;
    }

    const super_block_t *victim;
    if (!gc_select_victim(&ftl->alloc, &victim))
    {
        /* 触发但选不出 victim = 卡死嫌疑（观测计数，见 ftl.h）。 */
        ftl->gc_novictim_count++;
        return false;
    }

    mig_start(ftl, victim, MIG_KIND_GC, MIG_POST_FREE);
    return true;
}

bool mig_begin_wl(ftl_context_t *ftl)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    ASSERT(ftl != NULL);
    if (ftl->mig.active)
    {
        return false;
    }
    if (!wl_should_trigger(ftl))
    {
        return false;
    }

    wl_victim_t victim;
    if (!wl_select_victim(ftl, &victim))
    {
        return false;
    }

    super_block_t sb;
    mig_victim_from_planes(&sb, victim.plane_blocks);

    mig_start(ftl, &sb, MIG_KIND_WL, MIG_POST_RETIRE);
    return true;
}

bool mig_begin_predict(ftl_context_t *ftl)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    ASSERT(ftl != NULL);
    if (ftl->mig.active)
    {
        return false;
    }

    uint16_t gblock;
    if (!rr_predictive_scan(&ftl->alloc, &gblock))
    {
        return false;
    }

    /* 慢死块必在 closed_sb_list 中（scan 只返回 CLOSED 块）。 */
    const uint16_t closed_idx   = ftl_block_sb_id(&ftl->alloc.blocks[gblock]);
    const uint16_t closed_count = ftl->alloc.closed_sb_count;

    ASSERT(closed_idx < closed_count);

    /* ⚠ [待上板] release 运行期拒绝（H14）：ASSERT 在 NDEBUG 下裁空，越界会拿垃圾
     * plane_blocks 去擦块 = 静默介质破坏。 */
    if (closed_idx >= closed_count)
    {
        LOGE("[MIG] predict: stale sb_id=%u (closed=%u) — ignored\r\n",
             (unsigned)closed_idx,
             (unsigned)closed_count);
        return false;
    }

    super_block_t sb;
    mig_victim_from_planes(&sb, allocator_closed_at(&ftl->alloc, closed_idx)->plane_blocks);

    mig_start(ftl, &sb, MIG_KIND_PREDICT, MIG_POST_RETIRE);
    return true;
}

/* ── 4. 收尾 ── */

/* 中止 job：不擦 victim、不动 L2P、不移除 closed 条目（源数据仍有效，下轮重试）。 */
static void mig_abort(ftl_context_t *ftl, const char *reason)
{
    mig_job_t *m = &ftl->mig;

    LOGW("[MIG] abort kind=%u phase=%u page=%u plane=%u — %s\r\n",
         (unsigned)m->kind,
         (unsigned)m->phase,
         (unsigned)m->page,
         (unsigned)m->plane,
         reason);
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        if (!super_block_has_plane(&m->victim, p))
        {
            continue;
        }
        LOGW("[MIG]   victim plane%u block=%u\r\n",
             (unsigned)p,
             (unsigned)m->victim.plane_blocks[p]);
    }

    m->aborted = true;
    m->phase   = (uint8_t)MIG_DONE;
}

/* 累计一次「提交不出去」；超限即中止 job。返回 true = 已中止。 */
static bool mig_stall(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    if (m->stall_rounds >= MIG_SUBMIT_STALL_MAX)
    {
        mig_abort(ftl, "submit stalled");
        return true;
    }
    m->stall_rounds++;
    return false;
}

/* 推进到下一页（清本页状态）。读/写两阶段的页推进都走这里。 */
static void mig_next_page(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    m->page      = (uint8_t)(m->page + 1U);
    m->plane     = 0U;
    m->decode_ok = false;
    m->write_ok  = false;
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
 *   实现要点：定位 victim 在 closed_sb_list 的索引由 allocator 统一完成（比对全部 plane；
 *             Policy B 下退化单 plane 条目的哨兵 plane 相同，只比一个 plane 会命中另一条）。
 *             aborted ⇒ 不做任何列表/计数变更（victim 未被擦，仍有效）。
 * -------------------------------------------------------------------- */
static void mig_finish(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    if (!m->aborted)
    {
        uint16_t closed_idx;

        /* 移除已回收条目（swap-remove）——防 closed_sb_count 只增不减导致列表爆炸
         * + 已回收 SB 被再次选为 victim 的无限 GC 空转。 */
        if (allocator_find_closed_sb(&ftl->alloc, &m->victim, &closed_idx))
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
            for (uint8_t p = 0U; p < PLANE_NUM; p++)
            {
                if (!super_block_has_plane(&m->victim, p))
                {
                    continue;
                }
                LOGW("[FTL] predictive relocate retired plane%u=%u\r\n",
                     (unsigned)p,
                     (unsigned)m->victim.plane_blocks[p]);
            }
        }
    }

    m->active       = false;
    m->phase        = (uint8_t)MIG_IDLE;
    m->waiting      = false;
    m->decode_ok    = false;
    m->write_ok     = false;
    m->stall_rounds = 0U;
}

/* ── 5. 推进 ── */

/* --------------------------------------------------------------------
 * 各 phase 处理函数
 *   返回 true = 本轮到此为止（提交了一条命令 / 提交失败 / 原地等资源）；
 *   返回 false = 纯计算推进了一步，mig_step 继续处理下一个 phase。
 * -------------------------------------------------------------------- */

static bool mig_step_read(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    if (m->page >= PAGES_PER_BLOCK)
    {
        m->phase = (uint8_t)MIG_FLUSH_JRN; /* 全部页读完 */
        return false;
    }

    /* 找本页下一个有物理块的 plane */
    m->plane = mig_next_present_plane(m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        /* 本页各 plane 都处理完 ⇒ 决定去向 */
        bool any = false;

        for (uint8_t p = 0U; p < PLANE_NUM; p++)
        {
            if (m->cw_bitmap[p] != 0U)
            {
                any = true;
            }
        }
        if (!any)
        {
            mig_next_page(ftl); /* 空洞 / 全被覆盖 */
            return false;
        }

        /* 成对分配目标页（Cold 流）。池耗尽 ⇒ 中止（不擦 victim，源数据仍在、
         * L2P 未动，下轮 GC 再来）——**不得**先擦后搬。 */
        if (allocator_alloc_pair(&ftl->alloc, FTL_TEMP_COLD, m->dst) != FTL_OK)
        {
            mig_abort(ftl, "no free block");
            return false;
        }

        m->plane = 0U;
        m->phase = (uint8_t)MIG_WRITE;
        return false;
    }

    /* 本页本 plane 的源地址（页基址）。全局块号不缓存——现算，见 mig_src_gblock。 */
    m->src[m->plane] = super_block_page_pba(ftl->topo, &m->victim, m->plane, m->page);
    memset(m->page_buf[m->plane], 0, PAGE_MAIN_SIZE);

    const ftl_status_t ret = mig_submit_read(ftl);
    if (ret == FTL_AGAIN)
    {
        (void)mig_stall(ftl);
    }
    else if (ret != FTL_OK)
    {
        mig_abort(ftl, "read command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

static bool mig_step_read_settle(ftl_context_t *ftl)
{
    mig_job_t    *m     = &ftl->mig;
    const uint8_t plane = m->plane;

    if (!m->decode_ok)
    {
        /* 读失败 / 全页空洞 / META 解不出：本 plane 无有效 CW */
        m->cw_bitmap[plane] = 0U;
    }
    else
    {
        /* 就绪前置：本页逐 CW 的扇区 LBA 来自页内 META，与刚访问过的 host LBA
         * **无必然联系** ⇒ region miss 现实可达。未驻留 ⇒ 起异步加载并原地返回，
         * 下轮重扫（重扫幂等：ready 命中不记账、不动 access_count）。 */
        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            if ((m->cw_lba[plane][cw] == FTL_LBA_INVALID) ||
                ((uint64_t)m->cw_lba[plane][cw] >= L2P_GLOBAL_LBA_MAX) ||
                !lba_routes_to(m->cw_lba[plane][cw], ftl->topo))
            {
                continue;
            }
            if (!l2p_cache_ready(ftl->cache, m->cw_lba[plane][cw]))
            {
                m->stall_rounds = 0U; /* 加载在飞 = 有进展，不是提交卡死 */
                return true;
            }
        }

        /* 全部就绪 ⇒ 一次性算出本 plane 的位图（不会半途而废） */
        m->cw_bitmap[plane] = 0U;
        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            nand_pba_t cur;

            if ((m->cw_lba[plane][cw] == FTL_LBA_INVALID) ||
                ((uint64_t)m->cw_lba[plane][cw] >= L2P_GLOBAL_LBA_MAX) ||
                !lba_routes_to(m->cw_lba[plane][cw], ftl->topo))
            {
                continue;
            }
            /* 用**该 CW 自己的 META** 查，仍指向 (本页, 本 CW) 才算有效 */
            if ((l2p_cache_read(ftl->cache, m->cw_lba[plane][cw], &cur) == FTL_OK) &&
                (cur == pba_with_cw(m->src[plane], cw)))
            {
                m->cw_bitmap[plane] |= ftl_cw_bit(plane, cw);
            }
        }
    }

    m->decode_ok = false;
    m->phase     = (uint8_t)MIG_READ;
    m->plane     = (uint8_t)(plane + 1U); /* 可能 == PLANE_NUM ⇒ 页结算 */
    return false;
}

static bool mig_step_write(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    /* 找本页下一个有待写 CW 的 plane */
    m->plane = mig_next_data_plane(m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        mig_next_page(ftl);
        return false;
    }

    const ftl_status_t ret = mig_submit_write(ftl);
    if (ret == FTL_AGAIN)
    {
        (void)mig_stall(ftl);
    }
    else if (ret != FTL_OK)
    {
        mig_abort(ftl, "write command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

static bool mig_step_write_settle(ftl_context_t *ftl)
{
    mig_job_t    *m     = &ftl->mig;
    const uint8_t plane = m->plane;

    if (m->write_ok)
    {
        /* 就绪前置（同 READ_SETTLE）：全部就绪才动账——半途而废会重复记账
         * （valid_count 虚高、journal 重复条目）。 */
        /* ⚠ [待上板] 位图判据必须与 READ_SETTLE 的 `ftl_cw_bit(plane, cw)` 同源：
         * flat 绝对编号下 plane 1 的有效位在 bit 4..7，按 plane 内相对位读会整段漏账。
         * 验证法：各 plane 写满 → 触发 GC → 读回 plane 1 的 LBA（应仍正确）+ 看
         * journal / valid_count 迁到新页。 */
        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            if ((m->cw_bitmap[plane] & ftl_cw_bit(plane, cw)) == 0U)
            {
                continue;
            }
            if (!l2p_cache_ready(ftl->cache, m->cw_lba[plane][cw]))
            {
                m->stall_rounds = 0U;
                return true;
            }
        }

        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            if ((m->cw_bitmap[plane] & ftl_cw_bit(plane, cw)) == 0U)
            {
                continue;
            }

            const lba_t l = m->cw_lba[plane][cw]; /* 用该 CW 自己的 META（不再 base+cw 推断） */

            const nand_pba_t new_pba = pba_with_cw(m->dst[plane], cw);

            (void)l2p_cache_write(ftl->cache, l, new_pba);
            journal_append(ftl->journal,
                           l,
                           pba_with_cw(m->src[plane], cw),
                           new_pba);

            /* 有效计数（CW 级）：源块 -1，新块 +1 */
            ftl_block_sub_valid(&ftl->alloc.blocks[mig_src_gblock(m, plane)]);
            ftl_block_add_valid(&ftl->alloc.blocks[ftl_pba_gblock(new_pba)]);

            m->migrated++;
        }
    }

    m->write_ok = false;
    m->phase    = (uint8_t)MIG_WRITE;
    m->plane    = (uint8_t)(plane + 1U);
    return false;
}

static bool mig_step_flush_jrn(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    /* F10 契约：搬移条目先落盘再擦 victim。journal 是 write-back（page_buf
     * 攒满才 flush），不在此强制落盘，掉电落在"条目未落盘 + victim 已擦除"
     * 窗口 ⇒ 恢复回放指向已擦除块 = 数据损坏。
     * 无搬移条目（migrated == 0）⇒ 无需屏障，直接擦。 */
    if (m->migrated == 0U)
    {
        m->phase = (uint8_t)MIG_ERASE;
        m->plane = 0U;
        return false;
    }

    journal_kick(ftl->journal); /* job 空闲且有待写页 ⇒ 起异步刷页 */

    if (ftl->journal->job.phase != JRN_IDLE)
    {
        m->stall_rounds = 0U; /* 有命令在飞 = 在推进，不计卡死 */
        return true;
    }
    if ((ftl->journal->job.need_erase != 0U) || (ftl->journal->current_entry != 0U))
    {
        /* 想刷却刷不动（命令池空）⇒ 计卡死轮数；超限中止（**不擦 victim**） */
        (void)mig_stall(ftl);
        return true;
    }

    m->stall_rounds = 0U; /* 屏障达成 */
    m->phase        = (uint8_t)MIG_ERASE;
    m->plane        = 0U;
    return false;
}

static bool mig_step_erase(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    m->plane = mig_next_present_plane(m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        m->phase = (uint8_t)MIG_DONE;
        return false;
    }

    const ftl_status_t ret = mig_submit_erase(ftl);
    if (ret == FTL_AGAIN)
    {
        (void)mig_stall(ftl);
    }
    else if (ret != FTL_OK)
    {
        mig_abort(ftl, "erase command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

/* --------------------------------------------------------------------
 * mig_step
 *   功能：状态机主体——一次只提交**一条**命令；纯计算阶段连续推进到下一个
 *         提交阶段或终态。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 *   实现要点：提交失败（背压）⇒ 原地返回，由 mig_service 下轮重试同一阶段（幂等）；
 *             连续 MIG_SUBMIT_STALL_MAX 轮无进展 ⇒ 中止。永久错误立即中止。
 * -------------------------------------------------------------------- */
static void mig_step(ftl_context_t *ftl)
{
    mig_job_t *m = &ftl->mig;

    for (;;)
    {
        if (m->waiting)
        {
            return; /* 有命令在飞：等完成服务函数 */
        }

        bool stop;

        switch ((mig_phase_t)m->phase)
        {
            case MIG_READ:
                stop = mig_step_read(ftl);
                break;
            case MIG_READ_SETTLE:
                stop = mig_step_read_settle(ftl);
                break;
            case MIG_WRITE:
                stop = mig_step_write(ftl);
                break;
            case MIG_WRITE_SETTLE:
                stop = mig_step_write_settle(ftl);
                break;
            case MIG_FLUSH_JRN:
                stop = mig_step_flush_jrn(ftl);
                break;
            case MIG_ERASE:
                stop = mig_step_erase(ftl);
                break;
            case MIG_DONE:
                mig_finish(ftl);
                return;
            case MIG_IDLE:
            default:
                /* 不该发生：active 置位而 phase 非法 */
                ASSERT(!m->active);
                return;
        }

        if (stop)
        {
            return;
        }
    }
}

void mig_service(ftl_context_t *ftl)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    ASSERT(ftl != NULL);

    mig_job_t *m = &ftl->mig;

    if (!m->active)
    {
        m->log_ms   = 0U;
        m->start_ms = 0U;
        return;
    }

    /* 心跳（见 ftl_job.h）：搬移最长（64 页 × 2 plane），没有它卡死不可见 */
    if (ftl_job_heartbeat(&m->log_ms, &m->start_ms))
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

/* ── 6. 完成服务（g_op_service[FTL_OP_GC] / [FTL_OP_WL]）── */

/* MIG_READ 完成：拷数据 + 解 META 得逐 CW 的扇区 LBA；顺带记 ECC 趋势 + read_count
 * （慢死预测闭环）。失败 / 全页空洞 ⇒ 该 plane 无有效 CW，不影响另一 plane。 */
static void mig_complete_read(ftl_context_t             *ftl,
                              mig_job_t                 *m,
                              fc_command_t              *cmd,
                              const fc_command_status_t *completion)
{
    const uint8_t plane = m->plane;

    m->decode_ok = false;

    if (!fc_cw_status_has_error(completion->status))
    {
        (void)ftl_cmd_read_data(cmd, m->page_buf[plane], PAGE_MAIN_SIZE);
        if (gc_decode_cw_lba(cmd, m->src[plane], m->cw_lba[plane], &m->base[plane]))
        {
            ftl_block_t *src_blk = &ftl->alloc.blocks[mig_src_gblock(m, plane)];

            /* B4：搬移读记录 ECC 趋势 + read disturb 计数（慢死预测闭环）。
             * 判定只用 ecc_bits 与上一次读数比较，不涉及时间维度。 */
            rr_record_ecc(&src_blk->rr, fc_get_cmd_ecc_max(cmd));
            src_blk->read_count++;

            m->decode_ok = true;
        }
    }

    /* 逐 CW 反查 L2P 改在 MIG_READ_SETTLE（纯计算阶段，可挂起等 region 加载） */
    (void)fc_command_release(cmd);
    m->waiting      = false;
    m->stall_rounds = 0U;
    m->phase        = (uint8_t)MIG_READ_SETTLE;
    mig_step(ftl);
}

/* MIG_WRITE 完成：只记成败，L2P/Journal/有效计数的改动在 MIG_WRITE_SETTLE。
 * 失败 ⇒ **不动 L2P**（源页仍有效，下轮再搬）。 */
static void mig_complete_write(ftl_context_t             *ftl,
                               mig_job_t                 *m,
                               fc_command_t              *cmd,
                               const fc_command_status_t *completion)
{
    m->write_ok = !fc_cw_status_has_error(completion->status);

    (void)fc_command_release(cmd);
    m->waiting      = false;
    m->stall_rounds = 0U;
    m->phase        = (uint8_t)MIG_WRITE_SETTLE;
    mig_step(ftl);
}

/* MIG_ERASE 完成：成功 ⇒ erase_count++ / last_pe_ts / 清 valid，
 * 然后按 post 二选一（FREE 入池 / RETIRE 退役）；失败 ⇒ bbt_mark_bad。 */
static void mig_complete_erase(ftl_context_t             *ftl,
                               mig_job_t                 *m,
                               fc_command_t              *cmd,
                               const fc_command_status_t *completion)
{
    const uint8_t plane = m->plane;

    if (!fc_cw_status_has_error(completion->status))
    {
        const uint16_t gsrc = mig_src_gblock(m, plane);
        ftl_block_t   *blk  = &ftl->alloc.blocks[gsrc];

        /* 擦除成功：擦除计数 +1，记录最近擦除时间戳（单调写入量），valid_count 归零 */
        blk->erase_count++;
        blk->last_pe_ts = ftl->total_writes;
        ftl_block_clear_valid(blk);

        /* 去向二选一——**退役块不得先进池**：
         * 退役直接 RETIRED，与「state == FREE ⟺ 在池」不变量一致（allocator.h ②）。 */
        if ((mig_post_t)m->post == MIG_POST_RETIRE)
        {
            /* WL / 慢死预测：搬空的块**退役**（RETIRED，不再分配新数据、不标坏、仍可读）。
             * victim 正常是 CLOSED、从未入池 ⇒ remove_from_free 是文档化 no-op
             * （allocator.h 判据 4）；保留它是为了让「退役块绝不留在池里」不依赖调用顺序。
             * rr_retire_block 内部把 read_count 归零（read disturb 重计），它只拿到
             * ftl_block_t* ⇒ 脏标记放在这里补。 */
            allocator_remove_from_free(&ftl->alloc, gsrc);
            rr_retire_block(blk);
        }
        else
        {
            allocator_free_block(&ftl->alloc, gsrc);
        }
    }
    else
    {
        /* 擦除失败：标记坏块（BBT 单数据源，后台持久化） */
        (void)bbt_mark_bad(&ftl->alloc, mig_src_gblock(m, plane));
    }

    (void)fc_command_release(cmd);
    m->waiting      = false;
    m->stall_rounds = 0U;
    m->plane        = (uint8_t)(plane + 1U);
    mig_step(ftl);
}

/* --------------------------------------------------------------------
 * ftl_op_migrate
 *   功能：搬移命令的完成服务函数——按 job.phase 判完成结果、改 phase、release 命令，
 *         再交回 mig_step 续跑。
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `mig_job_t *`（= &ftl->mig）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 * -------------------------------------------------------------------- */
void ftl_op_migrate(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    mig_job_t *m = (mig_job_t *)ctx;

    ASSERT(completion != NULL);
    ASSERT(m != NULL);

    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    /* 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出来的归属实例，
     * 二者必须一致——不一致说明派发路由错了。 */
    ASSERT(ftl != NULL);
    ASSERT(&ftl->mig == m);

    switch ((mig_phase_t)m->phase)
    {
        case MIG_READ:
            mig_complete_read(ftl, m, cmd, completion);
            return;

        case MIG_WRITE:
            mig_complete_write(ftl, m, cmd, completion);
            return;

        case MIG_ERASE:
            mig_complete_erase(ftl, m, cmd, completion);
            return;

        case MIG_IDLE:
        case MIG_READ_SETTLE:
        case MIG_WRITE_SETTLE:
        case MIG_FLUSH_JRN:
        case MIG_DONE:
        default:
            /* 不该发生：一条命令对应一个提交阶段（READ / WRITE / ERASE）。 */
            LOGE("[MIG] unexpected completion phase=%u\r\n", (unsigned)m->phase);
            (void)fc_command_release(cmd);
            m->waiting = false;
            mig_abort(ftl, "unexpected completion");
            mig_step(ftl);
            return;
    }
}
