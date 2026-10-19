/*
 * migrate.c — Super Block 搬移引擎（GC / 静态 WL / 慢死预测共用）
 *
 * 职责：
 *   把 victim SB 的有效 CW 逐页搬到 Cold 流新块，更新 L2P / Journal / 有效计数，
 *   最后擦除 victim 并归还（GC / 静态 WL）或退役（慢死预测）。三处搬移的差别只在**谁当 victim**
 *   （kind）与**擦完怎么处置**（post），故共用一台异步 job。
 *
 * 文件结构：
 *   1. 内部工具       mig_src_gblock / mig_victim_from_planes / mig_next_*（static）
 *   2. 提交原语       mig_submit / mig_submit_read / mig_submit_write / mig_submit_erase（static）
 *   3. 启动           mig_start（static）/ mig_begin_gc / mig_begin_wl / mig_begin_predict
 *   4. 收尾           mig_abort / mig_stall / mig_next_page / mig_finish（static）
 *   5. 推进           ／各 phase 处理函数（static）/ mig_step（static）/ migrate_handle
 *   6. 完成服务       ftl_migrate_callback（g_op_callbacks[FTL_OP_GC] / [FTL_OP_WL]）
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
#include "bg_scheduler.h"     /* BG_LEVEL_*（ftl_bg_info_t.load_level） */
#include "bst.h"              /* ftl_block_state / ftl_gblock */
#include "fc.h"               /* fc_command_release / fc_command_id_to_command */
#include "ftl.h"              /* ftl_context_t（仅完成服务的路由断言） */
#include "ftl_fc_interface.h" /* ftl_async_read_page / ftl_async_write_page_cw / ftl_async_erase_block */
#include "ftl_job.h"          /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_GC / FTL_OP_WL */
#include "gc.h"               /* gc_select_victim / gc_decode_cw_lba */
#include "journal.h"          /* journal_handle / journal_append / JRN_IDLE */
#include "l2p_cache.h"        /* l2p_cache_read / l2p_cache_write */
#include "log.h"
#include "read_recovery.h" /* rr_record_ecc / rr_predictive_scan / rr_retire_block */
#include "wear_leveling.h" /* wl_should_trigger / wl_select_victim */

#include <string.h>

/* 前置声明：状态机主体定义在第 5 节（mig_start 要先见到它）。 */
static void mig_step(migrate_handle_t *m);

/* ── 1. 内部工具 ── */

/* victim 在指定 plane 上的全局块号（BST 索引）。
 * ⚠ **唯一真值是 `victim.plane_blocks[plane]`，不缓存在 job 里**：快路径
 *   （valid_total == 0 ⇒ 直接 MIG_ERASE）从不经过 READ 阶段，任何"读阶段填、擦阶段用"
 *   的缓存字段在快路径下都是未初始化的 0 ⇒ 擦除收尾会去改**块 0** 的元数据
 *   （erase_count / clear_valid / allocator_free_block）。
 * 调用点已保证 super_block_has_plane() 为真（哨兵 plane 不展开）。 */
static uint16_t mig_src_gblock(const migrate_handle_t *m, uint8_t plane)
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

/* 从 from 起找下一个**仍归本 victim 所有**的 plane；没有返回 PLANE_NUM。
 *
 * 只接受 BST state == CLOSED 的 plane：
 *   · 哨兵 plane（退化单 plane 条目）不是本 victim 的物理块；
 *   · 部分 plane 已擦除后 job 中止，重试时那些 plane 已是 FREE/RETIRED/BAD，
 *     再擦一次会重复计磨损 / 复活 BAD；
 *   · 迁移期间被 RR 标 BAD 的 plane 不能再读/擦。 */
static uint8_t mig_next_present_plane(const ftl_allocator_t *alloc, const migrate_handle_t *m, uint8_t from)
{
    for (uint8_t p = from; p < PLANE_NUM; p++)
    {
        if (!super_block_has_plane(&m->victim, p))
        {
            continue;
        }

        const uint16_t g = ftl_gblock(p, m->victim.plane_blocks[p]);

        if (ftl_block_state(&alloc->blocks[g]) != FTL_BLOCK_STATE_CLOSED)
        {
            continue;
        }

        return p;
    }

    return PLANE_NUM;
}

/* 从 from 起找下一个**本页有有效 CW** 的 plane；没有返回 PLANE_NUM。 */
static uint8_t mig_next_data_plane(const migrate_handle_t *m, uint8_t from)
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
 * 日志/调试时能一眼看出这条命令属于哪条路径（g_op_callbacks 两个槽指向同一函数）。 */
static ftl_op_id_t mig_op_tag(const migrate_handle_t *m)
{
    return (m->kind == MIG_KIND_GC) ? FTL_OP_GC : FTL_OP_WL;
}

/* 提交一条已建好的搬移命令：置 waiting、提交、失败回滚。
 * ⚠ stall_ms 只在**提交成功之后**清零：`ftl_op_submit` 自身失败（完成表满 / SQ 满）
 *   也属于"提交不出去"，提前清零会让 deadline 永不触发。 */
static ftl_status_t mig_submit(migrate_handle_t *m, fc_command_t *cmd)
{
    m->waiting = true;

    if (ftl_op_submit(cmd, mig_op_tag(m), m) != FTL_OK)
    {
        m->waiting = false;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    m->stall_ms = 0U;
    m->wait_ms  = 0U;
    return FTL_OK;
}

/* 提交一条 victim 页读（整页，4 CW）。数据在完成时由服务函数拷进 m->page_buf
 * ——**读**不像写那样在 prepare 阶段搬数据（见 ftl_async_read_page 的注释）。 */
static ftl_status_t mig_submit_read(migrate_handle_t *m)
{
    fc_command_t      *cmd;
    const ftl_status_t ret = ftl_async_read_page(&cmd, m->src[m->plane], FTL_PAGE_CFG_DATA_ANY());
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_submit(m, cmd);
}

/* 提交一条目标页 CW 子集写。只编有效 CW（cw_bitmap 外的 CW 不建链、不编程）——
 * 整页重写会把无效 CW 连同其（陈旧的）META 一起搬过去，污染新页且多搬无用数据。
 * ⚠ 目标页的 plane 位就是 plane（allocator_alloc_pair 的契约），故写进去的 META
 *   逐 CW 仍是正确的扇区 LBA。 */
static ftl_status_t mig_submit_write(migrate_handle_t *m)
{
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

    return mig_submit(m, cmd);
}

/* 提交一条 victim 块擦除。 */
static ftl_status_t mig_submit_erase(migrate_handle_t *m)
{
    const nand_pba_t block_addr = super_block_page_pba(m->port.topo, &m->victim, m->plane, 0U);

    fc_command_t      *cmd;
    const ftl_status_t ret = ftl_async_erase_block(&cmd, block_addr);
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_submit(m, cmd);
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
static void mig_start(migrate_handle_t *m, const super_block_t *victim, mig_kind_t kind, mig_post_t post)
{
    /* 前置条件（判据 2）：唯一调用者是本文件的 mig_begin_*，各自已断言 ftl / victim；
     * active 是运行时状态（单飞行），由调用者在选块**之前**判过 ⇒ 这里是死守卫。 */
    ASSERT(m != NULL);
    ASSERT(victim != NULL);
    ASSERT(!m->active);

    /* job 状态整体复位，但**端口 / 自持节拍 / 单调计数**必须跨 job 保留。 */
    const migrate_port_t port      = m->port;
    const uint32_t       gc_eval   = m->gc_eval_last_ms;
    const uint32_t       gc_log    = m->gc_log_last_ms;
    const uint32_t       wl_eval   = m->wl_eval_last_ms;
    const uint32_t       predict   = m->predict_last_ms;
    const uint32_t       reclaimed = m->reclaimed_count;
    const uint32_t       novictim  = m->novictim_count;

    memset(m, 0, sizeof(*m));

    m->port            = port;
    m->gc_eval_last_ms = gc_eval;
    m->gc_log_last_ms  = gc_log;
    m->wl_eval_last_ms = wl_eval;
    m->predict_last_ms = predict;
    m->reclaimed_count = reclaimed;
    m->novictim_count  = novictim;

    m->victim          = *victim;
    m->victim.is_open  = false; /* 本引擎只处理 Closed SB（见 allocator.h） */
    m->kind            = (uint8_t)kind;
    m->post            = (uint8_t)post;
    m->active          = true;
    m->error           = (int32_t)FTL_OK;
    m->fail_gblock     = UINT16_MAX;
    m->dst_fail_gblock = UINT16_MAX;

    /* 快路径：无任何有效 CW ⇒ 无可搬移，直接进擦除。 */
    allocator_sb_stats_t st;
    allocator_sb_stats(m->port.alloc, &m->victim, &st);
    m->phase = (st.valid_cw == 0U) ? (uint8_t)MIG_ERASE : (uint8_t)MIG_READ;

    mig_step(m);
}

static bool mig_begin_gc(migrate_handle_t *m)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    ASSERT(m != NULL);
    if (m->active)
    {
        return false;
    }
    if (journal_write_failed(m->port.journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }

    const super_block_t *victim;
    if (!gc_select_victim(m->port.alloc, &victim))
    {
        /* 触发但选不出 victim = 卡死嫌疑（观测计数，见 ftl.h）。 */
        m->novictim_count++;
        return false;
    }

    mig_start(m, victim, MIG_KIND_GC, MIG_POST_FREE);
    return true;
}

static bool mig_begin_wl(migrate_handle_t *m)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    ASSERT(m != NULL);
    if (m->active)
    {
        return false;
    }
    if (journal_write_failed(m->port.journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }
    const uint32_t total_writes = *m->port.total_writes;

    if (!wl_should_trigger(m->port.alloc, total_writes))
    {
        return false;
    }

    wl_victim_t victim;
    if (!wl_select_victim(m->port.alloc, &victim))
    {
        return false;
    }

    super_block_t sb;
    mig_victim_from_planes(&sb, victim.plane_blocks);

    mig_start(m, &sb, MIG_KIND_WL, MIG_POST_FREE);
    return true;
}

static bool mig_begin_predict(migrate_handle_t *m)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    ASSERT(m != NULL);
    if (m->active)
    {
        return false;
    }
    if (journal_write_failed(m->port.journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }

    uint16_t gblock;
    if (!rr_predictive_scan(m->port.alloc, &gblock))
    {
        return false;
    }

    /* 慢死块必在 closed_sb_list 中（scan 只返回 CLOSED 块）。 */
    const uint16_t closed_idx   = ftl_block_sb_id(&m->port.alloc->blocks[gblock]);
    const uint16_t closed_count = m->port.alloc->closed_sb_count;

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

    const super_block_t *closed = allocator_closed_at(m->port.alloc, closed_idx);
    super_block_t        sb;

    mig_victim_from_planes(&sb, closed->plane_blocks);

    mig_start(m, &sb, MIG_KIND_PREDICT, MIG_POST_RETIRE);
    return true;
}

/* ── 4. 收尾 ── */

/* 中止 job：不擦 victim、不动 L2P、不移除 closed 条目（源数据仍有效，下轮重试）。 */
static void mig_abort(migrate_handle_t *m, const char *reason)
{
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
    m->error   = (int32_t)FTL_ERROR; /* status.error：最近一次中止的归因 */
    m->phase   = (uint8_t)MIG_DONE;
}

/* 时钟在一个 deadline 起点之后是否已越过 timeout。0 起点 = 刚开始计时。 */
static bool mig_deadline_expired(uint32_t *start_ms, uint32_t timeout_ms)
{
    const uint32_t now = ftl_job_now_ms();

    if (now == 0U)
    {
        return false; /* 平台时钟缺失：deadline 不生效（本板不可达，见 migrate.h） */
    }
    if (*start_ms == 0U)
    {
        *start_ms = now;
        return false;
    }
    return (now - *start_ms) >= timeout_ms;
}

/* 提交不出去超过 FTL_SUBMIT_TIMEOUT_MS ⇒ 中止 job。返回 true = 已中止。 */
static bool mig_stall(migrate_handle_t *m)
{
    if (mig_deadline_expired(&m->stall_ms, FTL_SUBMIT_TIMEOUT_MS))
    {
        m->timed_out     = 1U;
        m->timeout_phase = m->phase;
        mig_abort(m, "submit timeout");
        return true;
    }
    return false;
}

/* 等外部模块超过 FTL_WAIT_TIMEOUT_MS ⇒ 中止 job（不擦 victim）。返回 true = 已中止。
 * ⚠ 与 mig_stall 分开：外部命令在飞也计时——"在飞"不等于"会完成"。 */
static bool mig_wait(migrate_handle_t *m, const char *reason)
{
    if (mig_deadline_expired(&m->wait_ms, FTL_WAIT_TIMEOUT_MS))
    {
        m->timed_out     = 1U;
        m->timeout_phase = m->phase;
        mig_abort(m, reason);
        return true;
    }
    return false;
}

/* 推进到下一页（清本页状态）。读/写两阶段的页推进都走这里。 */
static void mig_next_page(migrate_handle_t *m)
{
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
static void mig_finish(migrate_handle_t *m)
{
    if (!m->aborted)
    {
        uint16_t closed_idx;
        /* 逐 plane 擦除时条目可能已被 allocator_closed_drop_plane 摘空并移除
         * （最后一个 plane 摘除即删条目）⇒ 此时按原值查找必然失败。 */
        bool removed = m->closed_removed;

        /* 移除已回收条目（swap-remove）——防 closed_sb_count 只增不减导致列表爆炸
         * + 已回收 SB 被再次选为 victim 的无限 GC 空转。
         * 擦除失败也移除：该 SB 已坏/不可再用，留在列表里只会反复被选。 */
        if ((!removed) && allocator_find_closed_sb(m->port.alloc, &m->victim, &closed_idx))
        {
            allocator_remove_closed_sb(m->port.alloc, closed_idx);
            removed = true;
        }

        /* 进展计数 / 预测日志只在**真正回收成功**时记：擦除失败或条目没找到都不算。 */
        if (removed && (!m->erase_failed))
        {
            if (m->kind == MIG_KIND_GC)
            {
                m->reclaimed_count++; /* 进展计数（见 ftl.h：区分"在推进"与"卡死"） */
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
    }

    m->active          = false;
    m->phase           = (uint8_t)MIG_IDLE;
    m->waiting         = false;
    m->decode_ok       = false;
    m->write_ok        = false;
    m->closed_removed  = false;
    m->stall_ms        = 0U;
    m->wait_ms         = 0U;
    m->fail_gblock     = UINT16_MAX;
    m->fail_count      = 0U;
    m->dst_fail_gblock = UINT16_MAX;
    m->dst_fail_count  = 0U;
}

/* ── 5. 推进 ── */

/* --------------------------------------------------------------------
 * 各 phase 处理函数
 *   返回 true = 本轮到此为止（提交了一条命令 / 提交失败 / 原地等资源）；
 *   返回 false = 纯计算推进了一步，mig_step 继续处理下一个 phase。
 * -------------------------------------------------------------------- */

static bool mig_step_read(migrate_handle_t *m)
{
    if (m->page >= PAGES_PER_BLOCK)
    {
        m->phase = (uint8_t)MIG_FLUSH_JRN; /* 全部页读完 */
        return false;
    }

    /* 找本页下一个有物理块的 plane */
    m->plane = mig_next_present_plane(m->port.alloc, m, m->plane);

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
            mig_next_page(m); /* 空洞 / 全被覆盖 */
            return false;
        }

        /* 成对分配目标页（Cold 流）。池耗尽 ⇒ 中止（不擦 victim，源数据仍在、
         * L2P 未动，下轮 GC 再来）——**不得**先擦后搬。 */
        if (allocator_alloc_pair(m->port.alloc, FTL_TEMP_COLD, m->dst) != FTL_OK)
        {
            mig_abort(m, "no free block");
            return false;
        }

        m->plane = 0U;
        m->phase = (uint8_t)MIG_WRITE;
        return false;
    }

    /* 本页本 plane 的源地址（页基址）。全局块号不缓存——现算，见 mig_src_gblock。 */
    m->src[m->plane] = super_block_page_pba(m->port.topo, &m->victim, m->plane, m->page);
    memset(m->page_buf[m->plane], 0, PAGE_MAIN_SIZE);

    const ftl_status_t ret = mig_submit_read(m);
    if (ret == FTL_AGAIN)
    {
        (void)mig_stall(m);
    }
    else if (ret != FTL_OK)
    {
        mig_abort(m, "read command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

/* 本 CW 的 META LBA 是否属于本实例且可查 L2P。
 * META 来自介质（H29）：未写过 / 越界 / 不属于本实例的条目一律跳过。 */
static bool mig_lba_usable(const migrate_handle_t *m, lba_t lba)
{
    return (lba != FTL_LBA_INVALID) &&
           ((uint64_t)lba < L2P_GLOBAL_LBA_MAX) &&
           lba_routes_to(lba, m->port.topo);
}

static bool mig_step_read_settle(migrate_handle_t *m)
{
    const uint8_t plane = m->plane;

    if (!m->decode_ok)
    {
        /* 空页（无错误 + META 全 FF）：本 plane 无有效 CW。
         * NAND 读错误已在 mig_complete_read 里中止 job，不会走到这里。 */
        m->cw_bitmap[plane] = 0U;
    }
    else
    {
        /* 就绪前置：本页逐 CW 的扇区 LBA 来自页内 META，与刚访问过的 host LBA
         * **无必然联系** ⇒ region miss 现实可达。未驻留 ⇒ 起异步加载并原地返回，
         * 下轮重扫（重扫幂等：ready 命中不记账、不动 access_count）。 */
        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            if (!mig_lba_usable(m, m->cw_lba[plane][cw]))
            {
                continue;
            }
            if (!l2p_cache_ready(m->port.cache, m->cw_lba[plane][cw]))
            {
                /* 加载在飞不计提交卡死，但**必须**计入独立的等待上限：
                 * 加载永久卡住时 job 否则永不中止（RISK-MIG-01）。 */
                (void)mig_wait(m, "l2p load timeout");
                return true;
            }
        }

        m->wait_ms = 0U; /* 等待条件解除 */

        /* 全部就绪 ⇒ 一次性算出本 plane 的位图（不会半途而废） */
        m->cw_bitmap[plane] = 0U;
        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            nand_pba_t cur;

            if (!mig_lba_usable(m, m->cw_lba[plane][cw]))
            {
                continue;
            }
            /* 用**该 CW 自己的 META** 查，仍指向 (本页, 本 CW) 才算有效 */
            if ((l2p_cache_read(m->port.cache, m->cw_lba[plane][cw], &cur) == FTL_OK) &&
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

static bool mig_step_write(migrate_handle_t *m)
{
    /* 找本页下一个有待写 CW 的 plane */
    m->plane = mig_next_data_plane(m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        mig_next_page(m);
        return false;
    }

    const ftl_status_t ret = mig_submit_write(m);
    if (ret == FTL_AGAIN)
    {
        (void)mig_stall(m);
    }
    else if (ret != FTL_OK)
    {
        mig_abort(m, "write command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

static bool mig_step_write_settle(migrate_handle_t *m)
{
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
            if (!l2p_cache_ready(m->port.cache, m->cw_lba[plane][cw]))
            {
                (void)mig_wait(m, "l2p load timeout");
                return true;
            }
        }

        m->wait_ms = 0U; /* 等待条件解除 */

        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            if ((m->cw_bitmap[plane] & ftl_cw_bit(plane, cw)) == 0U)
            {
                continue;
            }

            const lba_t l = m->cw_lba[plane][cw]; /* 用该 CW 自己的 META（不再 base+cw 推断） */

            if (!mig_lba_usable(m, l))
            {
                continue;
            }

            /* **提交时重查**：READ_SETTLE 到本阶段之间，host 覆盖写 / TRIM 可能已把
             * 该 LBA 改到别处。只有 L2P 仍指向本 victim 的 CW 才能迁移；否则旧数据
             * 覆盖新映射、valid_count 双降。被抢先的 CW 跳过（它已不再引用 victim）。 */
            nand_pba_t cur;

            if (l2p_cache_read(m->port.cache, l, &cur) != FTL_OK)
            {
                mig_abort(m, "l2p read failed"); /* [待上板] */
                return false;
            }

            const nand_pba_t src_cw = pba_with_cw(m->src[plane], cw);

            if (cur != src_cw)
            {
                continue; /* host 写 / TRIM 已生效：该 CW 不再引用 victim */
            }

            const nand_pba_t dst_cw = pba_with_cw(m->dst[plane], cw);

            /* journal 先于 L2P（与 host 写路径同序）：append 失败 ⇒ 该映射未持久化，
             * 立即中止，victim 不擦。 */
            if (!journal_append(m->port.journal, l, src_cw, dst_cw))
            {
                mig_abort(m, "journal append failed"); /* [待上板] */
                return false;
            }
            if (l2p_cache_write(m->port.cache, l, dst_cw) != FTL_OK)
            {
                mig_abort(m, "l2p write failed"); /* [待上板] */
                return false;
            }

            /* 有效计数（CW 级）：源块 -1，新块 +1。
             * ⚠ 源块可能在迁移期间被 RR 标 BAD（valid_count 已被清 0）；此时不能再
             *   减（debug 下会撞 sub_valid 断言，release 下会回绕）。数据已读到并写进
             *   目的页，L2P 更新仍然正确；只跳过源侧计数。 */
            const uint16_t src_gblock = mig_src_gblock(m, plane);
            const uint16_t dst_gblock = ftl_pba_gblock(dst_cw);
            ftl_block_t   *src_blk    = &m->port.alloc->blocks[src_gblock];

            if ((!ftl_block_is_bad(src_blk)) && (src_blk->valid_count > 0U))
            {
                ftl_block_sub_valid(src_blk);
            }
            ftl_block_add_valid(&m->port.alloc->blocks[dst_gblock]);

            m->migrated++;
        }
    }

    m->write_ok = false;
    m->phase    = (uint8_t)MIG_WRITE;
    m->plane    = (uint8_t)(plane + 1U);
    return false;
}

static bool mig_step_flush_jrn(migrate_handle_t *m)
{
    /* Journal 持久性失败 pending（页写失败 ⇒ 条目已丢）：
     * 在 CP 确认当前 L2P 已落盘之前，任何 victim 都不许擦——包括本 job 没有搬移
     * 任何 CW（migrated == 0）的情况，因为上一次 job 的迁移映射可能只在 RAM 里。 */
    if (journal_write_failed(m->port.journal))
    {
        mig_abort(m, "journal write failure pending"); /* [待上板] */
        return false;
    }

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

    {
        /* typed 入口：job 空闲且有待写页 ⇒ 起异步刷页。info 由 migrate 的调用方
         * 在本阶段后续传入；这里位移推进不依赖动态信息。 */
        journal_status_t jst;
        journal_handle(m->port.journal, NULL, NULL, &jst);
    }

    if (!journal_is_idle(m->port.journal))
    {
        /* 有命令在飞不计提交卡死，但计入独立的等待上限：Journal job 永久卡住时
         * job 否则永不中止（RISK-MIG-01）。 */
        (void)mig_wait(m, "journal flush timeout");
        return true;
    }
    if ((journal_erase_pending(m->port.journal)) || (journal_has_pending_entries(m->port.journal)))
    {
        /* 想刷却刷不动（命令池空）⇒ 计卡死轮数；超限中止（**不擦 victim**） */
        (void)mig_stall(m);
        return true;
    }

    m->stall_ms = 0U;
    m->wait_ms  = 0U; /* 屏障达成 */
    m->phase    = (uint8_t)MIG_ERASE;
    m->plane    = 0U;
    return false;
}

static bool mig_step_erase(migrate_handle_t *m)
{
    m->plane = mig_next_present_plane(m->port.alloc, m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        m->phase = (uint8_t)MIG_DONE;
        return false;
    }

    const ftl_status_t ret = mig_submit_erase(m);
    if (ret == FTL_AGAIN)
    {
        (void)mig_stall(m);
    }
    else if (ret != FTL_OK)
    {
        mig_abort(m, "erase command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

/* --------------------------------------------------------------------
 * mig_step
 *   功能：状态机主体——一次只提交**一条**命令；纯计算阶段连续推进到下一个
 *         提交阶段或终态。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 *   实现要点：提交失败（背压）⇒ 原地返回，由 migrate_handle 下轮重试同一阶段（幂等）；
 *             连续超过 FTL_SUBMIT_TIMEOUT_MS 无进展 ⇒ 中止。永久错误立即中止。
 * -------------------------------------------------------------------- */
static void mig_step(migrate_handle_t *m)
{
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
                stop = mig_step_read(m);
                break;
            case MIG_READ_SETTLE:
                stop = mig_step_read_settle(m);
                break;
            case MIG_WRITE:
                stop = mig_step_write(m);
                break;
            case MIG_WRITE_SETTLE:
                stop = mig_step_write_settle(m);
                break;
            case MIG_FLUSH_JRN:
                stop = mig_step_flush_jrn(m);
                break;
            case MIG_ERASE:
                stop = mig_step_erase(m);
                break;
            case MIG_DONE:
                mig_finish(m);
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

static void mig_service_step(migrate_handle_t *m)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    ASSERT(m != NULL);

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
             ftl_job_age_ms(&m->start_ms));
    }

    mig_step(m);
}

/* ── 7. typed handle（端口装配 / 触发 / 自持节拍 / 上报）── */

void migrate_init(migrate_handle_t *m, const migrate_port_t *port)
{
    ASSERT(m != NULL);
    ASSERT(port != NULL);
    ASSERT(port->alloc != NULL);
    ASSERT(port->journal != NULL);
    ASSERT(port->cache != NULL);
    ASSERT(port->total_writes != NULL);

    memset(m, 0, sizeof(*m));
    m->port            = *port;
    m->fail_gblock     = UINT16_MAX;
    m->dst_fail_gblock = UINT16_MAX;
    m->error           = (int32_t)FTL_OK;
}

void migrate_trigger(migrate_handle_t *m, migrate_reason_t reason)
{
    ASSERT(m != NULL);

    switch (reason)
    {
        case MIGRATE_REASON_GC_URGENT:
            m->urgent_gc = 1U;
            break;
        default:
            /* 值域外：显式忽略（调用者传错原因不应 brick 设备） */
            break;
    }
}

/* GC 求值（自持节拍）：命中时返回 GC_TRIGGER_* 位掩码并打观测日志。 */
static uint32_t mig_gc_evaluate(migrate_handle_t *m, uint32_t now_ms)
{
    /* 节拍门。0 作「尚未求值」哨兵：平台时钟缺失时 now_ms 恒 0 ⇒ 每轮求值
     * ——GC 不能因缺时钟而停摆（与定时触发的取舍方向相反）。 */
    if ((m->gc_eval_last_ms != 0U) && ((now_ms - m->gc_eval_last_ms) < GC_EVAL_INTERVAL_MS))
    {
        return 0U;
    }
    m->gc_eval_last_ms = now_ms;

    const uint32_t triggers = gc_check_triggers(m->port.alloc);

    if (triggers != 0U)
    {
        if ((m->gc_log_last_ms == 0U) || ((now_ms - m->gc_log_last_ms) >= GC_LOG_INTERVAL_MS))
        {
            const uint16_t op0    = allocator_op_count(m->port.alloc, 0U);
            const uint16_t op1    = allocator_op_count(m->port.alloc, 1U);
            const uint32_t writes = *m->port.total_writes;

            m->gc_log_last_ms = now_ms;
            LOGI("[GC] trig=0x%02lx free=%u op=%u/%u writes=%lu reclaimed=%lu novictim=%lu\r\n",
                 triggers,
                 (unsigned)m->port.alloc->free_count,
                 (unsigned)op0,
                 (unsigned)op1,
                 writes,
                 m->reclaimed_count,
                 m->novictim_count);
        }
    }
    else
    {
        m->gc_log_last_ms = 0U; /* 收敛：下次进入触发态立即打 */
    }

    return triggers;
}

static void migrate_status_fill(const migrate_handle_t *m, const ftl_bg_info_t *info, migrate_status_t *out)
{
    ASSERT(m != NULL);
    ASSERT(out != NULL);

    out->base.critical = true; /* 写路径紧急 GC 在 BUSY 档起，不推进即死锁 */
    out->base.progress = m->reclaimed_count;
    out->base.next_ms  = 0U;
    out->base.error    = m->error;

    if (info != NULL)
    {
        out->base.wants_run = m->active || (info->load_level != BG_LEVEL_BUSY);
    }
    else
    {
        out->base.wants_run = m->active;
    }

    if (m->waiting)
    {
        out->base.state = FTL_MODULE_WAITING;
    }
    else if (m->active)
    {
        out->base.state = FTL_MODULE_RUNNING;
    }
    else if (m->error != (int32_t)FTL_OK)
    {
        out->base.state = FTL_MODULE_FAILED;
    }
    else
    {
        out->base.state = FTL_MODULE_IDLE;
    }

    out->phase         = m->phase;
    out->kind          = m->kind;
    out->migrated      = (uint16_t)m->migrated;
    out->erased        = m->erased;
    out->aborted       = m->aborted;
    out->erase_failed  = m->erase_failed;
    out->timed_out     = (m->timed_out != 0U);
    out->timeout_phase = m->timeout_phase;
    out->reclaimed     = m->reclaimed_count;
    out->novictim      = m->novictim_count;
}

void migrate_handle(migrate_handle_t *m, const ftl_bg_info_t *info, migrate_status_t *out)
{
    ASSERT(m != NULL);
    ASSERT(out != NULL);
    ASSERT(info != NULL); /* must-run 适配器每轮都构造 info */

    /* ① 在飞：只服务当前 job（提交即返回）。 */
    if (m->active)
    {
        mig_service_step(m);
        migrate_status_fill(m, info, out);
        return;
    }

    /* ② 不在飞：host 饱和档让路，不评估新工作。 */
    if (info->load_level == BG_LEVEL_BUSY)
    {
        migrate_status_fill(m, info, out);
        return;
    }

    const uint32_t now_ms = info->now_ms;

    /* ③ 紧急/显式触发优先；否则按自持节拍评估 GC → WL → predict。
     *    每轮最多启动一个 job（单飞行）。 */
    if (m->urgent_gc != 0U)
    {
        m->urgent_gc = 0U;
        (void)mig_begin_gc(m);
    }
    else if (mig_gc_evaluate(m, now_ms) != 0U)
    {
        (void)mig_begin_gc(m);
    }
    else if ((m->wl_eval_last_ms == 0U) ||
             ((now_ms - m->wl_eval_last_ms) >= WL_EVAL_INTERVAL_MS))
    {
        m->wl_eval_last_ms = now_ms;
        (void)mig_begin_wl(m);
    }
    else if ((info->load_level == BG_LEVEL_IDLE) &&
             ((m->predict_last_ms == 0U) || ((now_ms - m->predict_last_ms) >= RR_SCAN_INTERVAL_MS)))
    {
        m->predict_last_ms = now_ms;
        (void)mig_begin_predict(m);
    }

    migrate_status_fill(m, info, out);
}

bool migrate_is_active(const migrate_handle_t *m)
{
    ASSERT(m != NULL);
    return m->active;
}

uint32_t migrate_reclaimed_count(const migrate_handle_t *m)
{
    ASSERT(m != NULL);
    return m->reclaimed_count;
}

uint32_t migrate_novictim_count(const migrate_handle_t *m)
{
    ASSERT(m != NULL);
    return m->novictim_count;
}

/* ── 6. 完成服务（g_op_callbacks[FTL_OP_GC] / [FTL_OP_WL]）── */

/* MIG_READ 完成：拷数据 + 解 META 得逐 CW 的扇区 LBA；顺带记 ECC 趋势 + read_count
 * （慢死预测闭环）。失败 / 全页空洞 ⇒ 该 plane 无有效 CW，不影响另一 plane。 */
static void mig_complete_read(migrate_handle_t          *m,
                              fc_command_t              *cmd,
                              const fc_command_status_t *completion)
{
    const uint8_t plane = m->plane;

    m->decode_ok = false;

    /* 读失败与“空页”必须分开：前者页上可能仍有有效数据，擦 victim 会永久丢数据；
     * 后者（无错误 + META 全 FF）只是从未写过的 CW，跳过本页即可。 */
    bool read_ok = !fc_cw_status_has_error(completion->status);

    if (read_ok && (ftl_cmd_read_data(cmd, m->page_buf[plane], PAGE_MAIN_SIZE) != FTL_OK))
    {
        read_ok = false;
    }

    if (read_ok)
    {
        const gc_decode_result_t dr = gc_decode_cw_lba(cmd, m->src[plane], m->cw_lba[plane], &m->base[plane]);

        if (dr == GC_DECODE_OK)
        {
            const uint16_t gsrc    = mig_src_gblock(m, plane);
            ftl_block_t   *src_blk = &m->port.alloc->blocks[gsrc];
            const uint8_t  ecc     = fc_get_cmd_ecc_max(cmd);

            /* B4：搬移读记录 ECC 趋势 + read disturb 计数（慢死预测闭环）。
             * 判定只用 ecc_bits 与上一次读数比较，不涉及时间维度。 */
            rr_record_ecc(&src_blk->rr, ecc);
            src_blk->read_count++;

            m->decode_ok = true;
        }
        else if (dr == GC_DECODE_CORRUPT)
        {
            /* 页内 META 页基址不一致：不能只跳过部分 CW（它们的 L2P 仍指向本页，
             * 随后擦 victim 会悬空）。整页按读失败中止。 */
            read_ok = false;
        }
        /* GC_DECODE_EMPTY：空页，decode_ok 保持 false，SETTLE 跳过本页。 */
    }

    /* 逐 CW 反查 L2P 改在 MIG_READ_SETTLE（纯计算阶段，可挂起等 region 加载） */
    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;

    if (!read_ok)
    {
        /* [待上板] 读错误 ⇒ 中止，victim 不擦。空页（decode false）不在此列。
         * 同一源块连续失败达上限 ⇒ 标 BAD 并摘除其 Closed plane（RISK-GC-01）：
         * 否则每轮 GC 都重选同一 victim、永久 abort，容量不再增长。
         * 其他 plane 仍可迁移；数据不因标坏而丢失（坏块本来就读不回来）。 */
        const uint16_t gsrc = mig_src_gblock(m, plane);

        if (m->fail_gblock != gsrc)
        {
            m->fail_gblock = gsrc;
            m->fail_count  = 0U;
        }
        m->fail_count++;

        if (m->fail_count >= MIG_VICTIM_READ_FAIL_MAX)
        {
            LOGE("[MIG] victim read failed blk=%u x%u — mark BAD\r\n",
                 (unsigned)gsrc,
                 (unsigned)m->fail_count);
            (void)bbt_mark_bad(m->port.alloc, gsrc);
            if (allocator_closed_drop_plane(m->port.alloc, plane, m->victim.plane_blocks[plane]))
            {
                m->closed_removed = true;
            }
            m->fail_count  = 0U;
            m->fail_gblock = UINT16_MAX;
        }

        mig_abort(m, "victim read failed");
    }
    else
    {
        /* 只清**同一块**的连续失败计数：另一个 plane 的正常读不能把本块的失败
         * 计数冲掉（否则两 plane 交替时永远达不到标坏阈值）。 */
        if (m->fail_gblock == mig_src_gblock(m, plane))
        {
            m->fail_count  = 0U;
            m->fail_gblock = UINT16_MAX;
        }
        m->phase = (uint8_t)MIG_READ_SETTLE;
    }

    mig_step(m);
}

/* MIG_WRITE 完成：只记成败，L2P/Journal/有效计数的改动在 MIG_WRITE_SETTLE。
 * 写失败 ⇒ **中止 job，victim 不擦**（源数据仍有效，下轮再搬）；绝不能继续到 ERASE。 */
static void mig_complete_write(migrate_handle_t          *m,
                               fc_command_t              *cmd,
                               const fc_command_status_t *completion)
{
    const bool write_ok = !fc_cw_status_has_error(completion->status);

    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;

    if (!write_ok)
    {
        /* [待上板] 写错误 ⇒ 中止，victim 不擦。
         * 同一目的块连续失败达上限 ⇒ 关闭 Cold SB（RISK-GC-02）：目的块可能已有
         * 成功迁移的页，**不能**标 BAD；关闭后下一次 GC 换新的目的块，旧块的
         * 已写页仍可读，残留页由 GC 正常回收。 */
        const uint16_t gdst = ftl_pba_gblock(m->dst[m->plane]);

        if (m->dst_fail_gblock != gdst)
        {
            m->dst_fail_gblock = gdst;
            m->dst_fail_count  = 0U;
        }
        m->dst_fail_count++;

        if (m->dst_fail_count >= MIG_DST_WRITE_FAIL_MAX)
        {
            LOGE("[MIG] dst write failed blk=%u x%u — close cold SB\r\n",
                 (unsigned)gdst,
                 (unsigned)m->dst_fail_count);
            (void)allocator_close_open_sb(m->port.alloc, FTL_TEMP_COLD);
            m->dst_fail_count  = 0U;
            m->dst_fail_gblock = UINT16_MAX;
        }

        mig_abort(m, "write failed");
        mig_step(m);
        return;
    }

    m->write_ok        = true;
    m->dst_fail_count  = 0U;
    m->dst_fail_gblock = UINT16_MAX;
    m->phase           = (uint8_t)MIG_WRITE_SETTLE;
    mig_step(m);
}

/* MIG_ERASE 完成：成功 ⇒ erase_count++ / last_pe_ts / 清 valid，
 * 然后按 post 二选一（FREE 入池 / RETIRE 退役）；失败 ⇒ bbt_mark_bad。 */
static void mig_complete_erase(migrate_handle_t          *m,
                               fc_command_t              *cmd,
                               const fc_command_status_t *completion)
{
    const uint8_t  plane        = m->plane;
    const uint16_t gsrc         = mig_src_gblock(m, plane);
    const uint32_t total_writes = *m->port.total_writes;
    ftl_block_t   *blk          = &m->port.alloc->blocks[gsrc];

    if (!fc_cw_status_has_error(completion->status))
    {
        m->erased++;
        if (ftl_block_is_bad(blk))
        {
            /* 迁移期间被 RR/BBT 标 BAD：擦成功也不能复活或入池。 */
            m->erase_failed = true;
            LOGW("[MIG] erase ok but block %u became BAD — not freed/retired\r\n",
                 (unsigned)gsrc);
        }
        else
        {
            /* 擦除成功：统一走 allocator 的擦除记账（含 !bad 断言 + 清 valid）。 */
            allocator_note_erase(m->port.alloc, gsrc, total_writes);

            /* 去向二选一——**退役块不得先进池**：
             * 退役直接 RETIRED，与「state == FREE ⟺ 在池」不变量一致（allocator.h ②）。 */
            if ((mig_post_t)m->post == MIG_POST_RETIRE)
            {
                /* 慢死预测：搬空的块**退役**（RETIRED，不再分配新数据、不标坏、仍可读）。
                 * GC / 静态 WL 走 FREE 分支，块重新进入磨损轮转。
                 * victim 正常是 CLOSED、从未入池 ⇒ remove_from_free 是文档化 no-op
                 * （allocator.h 判据 4）；保留它是为了让「退役块绝不留在池里」不依赖调用顺序。
                 * rr_retire_block 内部把 read_count 归零（read disturb 重计）。 */
                allocator_remove_from_free(m->port.alloc, gsrc);
                rr_retire_block(blk);
            }
            else
            {
                allocator_free_block(m->port.alloc, gsrc);
            }
        }
    }
    else
    {
        m->erase_failed = true;

        /* 擦除失败：标记坏块（BBT 单数据源，后台持久化）。
         * ⚠ 已 BAD 时不再重复标记：bbt_mark_bad 会再次 bad_count++ 并从 Spare 补池。 */
        if (!ftl_block_is_bad(blk))
        {
            (void)bbt_mark_bad(m->port.alloc, gsrc);
        }
        else
        {
            LOGW("[MIG] erase failed on already-BAD block %u — not re-marked\r\n",
                 (unsigned)gsrc);
        }
    }

    /* 逐 plane 摘除已处理的块：擦完后该块已归还 Free Pool（或已标坏），旧 Closed 条目
     * 不得继续引用它——否则块被重新分配并再次 Closed 后会被旧条目误认（RISK-GC-04）。
     * 条目在最后一个 plane 摘除时自动移除。 */
    if (allocator_closed_drop_plane(m->port.alloc, plane, m->victim.plane_blocks[plane]))
    {
        m->closed_removed = true;
    }

    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;
    m->plane    = (uint8_t)(plane + 1U);
    mig_step(m);
}

/* --------------------------------------------------------------------
 * ftl_migrate_callback
 *   功能：搬移命令的完成服务函数——按 job.phase 判完成结果、改 phase、release 命令，
 *         再交回 mig_step 续跑。
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `migrate_handle_t *`（= m）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 * -------------------------------------------------------------------- */
void ftl_migrate_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    /* 形参由完成派发签名固定；release 下 ASSERT 裁空 ⇒ 显式丢弃（H30）。 */
    (void)ftl;
    migrate_handle_t *m = (migrate_handle_t *)ctx;

    ASSERT(completion != NULL);
    ASSERT(m != NULL);

    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    /* 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出来的归属实例，
     * 二者必须一致——不一致说明派发路由错了。 */
    ASSERT(m != NULL);
    ASSERT(ftl->migrate_handle == m);

    /* 孤儿 / 重复完成：job 已收尾或当前没有在飞命令 ⇒ 只释放命令，不改状态。
     * 否则迟到的完成会按新 job 的 phase 被误解释（同 read_recovery.c 的守卫）。 */
    if ((!m->active) || (!m->waiting))
    {
        LOGW("[MIG] orphan completion phase=%u active=%u waiting=%u — ignored\r\n",
             (unsigned)m->phase,
             (unsigned)m->active,
             (unsigned)m->waiting);
        (void)fc_command_release(cmd);
        return;
    }

    switch ((mig_phase_t)m->phase)
    {
        case MIG_READ:
            mig_complete_read(m, cmd, completion);
            return;

        case MIG_WRITE:
            mig_complete_write(m, cmd, completion);
            return;

        case MIG_ERASE:
            mig_complete_erase(m, cmd, completion);
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
            mig_abort(m, "unexpected completion");
            mig_step(m);
            return;
    }
}
