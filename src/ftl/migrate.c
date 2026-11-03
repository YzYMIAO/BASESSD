/*
 * migrate.c — Super Block 搬移引擎（GC / 静态 WL / 预测性退役共用）
 *
 * 职责：
 *   把 victim SB 的有效 CW 逐页搬到 Cold 流新块，更新 L2P / Journal / 有效计数，
 *   最后擦除 victim 并归还（GC / 静态 WL）或退役（预测性退役）。三处搬移的差别只在**谁当 victim**
 *   （kind）与**擦完怎么处置**（post），故共用一台异步 job。
 *
 * 文件结构：
 *   1. 内部工具       mig_victim_from_planes / mig_present_plane_next / mig_lba_is_usable /
 *                     *_get_gblock / *_name（static）
 *   2. 提交原语       mig_command_submit / mig_read_submit / mig_write_submit / mig_erase_submit（static）
 *   3. 启动/触发      mig_job_start / GC 收益判据 / mig_gc_begin / mig_wl_begin / mig_predict_begin
 *   4. 收尾           mig_job_abort / mig_job_stall / mig_job_wait / mig_page_next / mig_dst_fail_cleanup
 *                     / mig_job_finish（static）
 *   5. 推进           5.1 映射提交 / 5.2 batch 辅助 / 5.3 各 phase 处理 + mig_job_step（static）
 *   6. typed handle   mig_init / mig_gc_request_urgent / mig_gc_evaluate / mig_handle
 *   7. 完成服务       ftl_migrate_callback（g_op_callbacks[FTL_OP_GC] / [FTL_OP_WL]）
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
 *   ⚠ MIG_WRITE_SETTLE 在动账前还要 `jrn_reserve` 预留 Journal 槽位；不足时只起
 *     异步刷页并挂起本 job，不同步刷 Journal。
 */

#include "migrate.h"

#include "assertion.h"
#include "bbt.h"              /* bbt_mark_bad */
#include "bst.h"              /* ftl_block_state / ftl_gblock */
#include "fc.h"               /* fc_command_release / fc_command_id_to_command */
#include "ftl.h"              /* ftl_ctx_t（仅完成服务的路由断言） */
#include "ftl_fc_interface.h" /* ftl_async_read_page / ftl_async_write_page_cw / ftl_async_erase_block */
#include "ftl_job.h"          /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_GC / FTL_OP_WL */
#include "ftl_type.h"         /* BG_LEVEL_* */
#include "gc.h"               /* gc_select_victim / gc_decode_cw_lba */
#include "journal.h"          /* jrn_handle / jrn_append / JRN_IDLE */
#include "l2p_cache.h"        /* l2p_read / l2p_write */
#include "log.h"
#include "read_recovery.h" /* rr_record_ecc / rr_predictive_scan / rr_retire_block */
#include "timer.h"         /* timer_scheduler_add（周期求值注册） */
#include "wear_leveling.h" /* wl_should_trigger / wl_select_victim */

#include <string.h>

/* ── 文件级静态数据 ── */

// page0 补零写的全零页（.bss，零初始化；单线程 superloop 共享）。
static BYTE s_zero_page[PAGE_MAIN_SIZE];

// abort 日志限流（RISK-MIG-05）：上一行日志的时刻 + 本窗口内被抑制的条数。
static DWORD s_abort_log_ms;
static DWORD s_abort_suppressed;

/* ── 前置声明 ── */

/**
 * @brief 状态机主体定义在第 5 节（mig_job_start 要先见到它）。
 */
static void mig_job_step(mig_handle_t *m);

/* ── 1. 内部工具 ── */

/**
 * @brief 用 plane_blocks[] + 写指针组装本引擎的 victim 视图。
 *
 * `next_page` **必须**一起带过来：它是该块已编程页数的上界，搬移只读 [0, next_page)。
 * 半写块（提前关闭 / GC 目的块）的空洞页物理上是擦除态，读回来 ECC 必失败；
 * 不按写指针截断就会把空洞当成「victim 读失败」并误标坏块（RISK-GC-11）。
 * temperature 不参与搬移决策，保持 memset 的默认值。
 *
 * @param sb           出参：组装出的 victim 视图（先整体清零，再填 plane_blocks 与 next_page）。
 * @param plane_blocks 各 plane 的 plane 内块号（按 PLANE_NUM 逐个拷入）。
 * @param next_page    该块已编程页数的上界（搬移只读 [0, next_page)）。
 */
static void mig_victim_from_planes(super_block_t *sb, const WORD plane_blocks[PLANE_NUM], BYTE next_page)
{
    memset(sb, 0, sizeof(*sb));
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        sb->plane_blocks[p] = plane_blocks[p];
    }
    sb->next_page = next_page;
}

/**
 * @brief 从 from 起找下一个**仍归本 victim 所有**的 plane；没有返回 PLANE_NUM。
 *
 * 只接受 BST state == CLOSED 的 plane：
 * · 哨兵 plane（退化单 plane 条目）不是本 victim 的物理块；
 * · 部分 plane 已擦除后 job 中止，重试时那些 plane 已是 FREE/RETIRED/BAD，
 * 再擦一次会重复计磨损 / 复活 BAD；
 * · 迁移期间被 RR 标 BAD 的 plane 不能再读/擦。
 *
 * @param alloc allocator（读 blocks[].state 判断 plane 是否仍可处理）。
 * @param m     job（提供 victim 视图）。
 * @param from  起始 plane 下标。
 *
 * @return 0..PLANE_NUM-1 —— 下一个仍归本 victim 且 state == CLOSED 的 plane 下标；
 *         PLANE_NUM —— 没有这样的 plane。
 */
static BYTE mig_present_plane_next(const alc_t *alloc, const mig_handle_t *m, BYTE from)
{
    for (BYTE p = from; p < PLANE_NUM; p++)
    {
        if (super_block_has_plane(&m->victim, p) == false)
        {
            continue;
        }

        const WORD gblock = ftl_gblock(p, m->victim.plane_blocks[p]);

        if (ftl_block_state(&alloc->blocks[gblock]) != FTL_BLOCK_STATE_CLOSED)
        {
            continue;
        }

        return p;
    }

    return PLANE_NUM;
}

/**
 * @brief 本 CW 的 META LBA 是否属于本实例且可查 L2P。
 * META 来自介质（H29）：未写过 / 越界 / 不属于本实例的条目一律跳过。
 *
 * @param m   job（取 topo 做实例路由判断）。
 * @param lba META 中解析出的扇区 LBA。
 *
 * @return true  —— 非 FTL_LBA_INVALID、在 L2P 全局范围内且路由到本实例；
 *         false —— 不可用于查 L2P。
 */
static bool mig_lba_is_usable(const mig_handle_t *m, DWORD lba)
{
    return (lba != FTL_LBA_INVALID) && ((QWORD)lba < L2P_GLOBAL_LBA_MAX) && lba_routes_to(lba, m->topo);
}

/**
 * @brief victim 的某 plane 对应的全局块号；不存在该 plane ⇒ UINT16_MAX（日志哨兵）。
 *
 * @param m     job（提供 victim 视图）。
 * @param plane plane 下标。
 *
 * @return 该 plane 的全局块号（BST 索引）；
 *         UINT16_MAX —— victim 不含该 plane（日志哨兵）。
 */
static WORD mig_victim_get_gblock(const mig_handle_t *m, BYTE plane)
{
    if (super_block_has_plane(&m->victim, plane) == false)
    {
        return UINT16_MAX;
    }
    return ftl_gblock(plane, m->victim.plane_blocks[plane]);
}

#if defined(DEBUG)
/**
 * @brief 目标页 PBA 的全局块号；该 plane 没有目标页 ⇒ UINT16_MAX。仅 DEBUG move 行使用。
 *
 * @param m     job（dst[] 与 dst_allocated）。
 * @param plane plane 下标。
 *
 * @return 目标页的全局块号；
 *         UINT16_MAX —— 尚未分配目的页 / victim 不含该 plane / dst 非法。
 */
static WORD mig_dst_get_gblock(const mig_handle_t *m, BYTE plane)
{
    // 没有分配过目的页时 dst[] 仍是 memset 的 0（= 合法块 0），不能靠 pba_is_valid；
    // absent plane 的 dst[] 同样不可信。
    if ((m->dst_allocated == false) || (super_block_has_plane(&m->victim, plane) == false) || (pba_is_valid(m->dst[plane]) == false))
    {
        return UINT16_MAX;
    }
    return ftl_pba_gblock(m->dst[plane]);
}
#endif

/**
 * @brief victim 来源的名字（日志用）。表序 == mig_kind_e。
 *
 * @param kind victim 来源枚举。
 *
 * @return "gc"   —— 空间回收；
 *         "wl"   —— 静态磨损均衡；
 *         "pred" —— 预测性退役；
 *         "?"    —— 越界值。
 */
static const char *mig_kind_name(mig_kind_e kind)
{
    static const char *const names[] = {"gc", "wl", "pred"};

    _Static_assert(sizeof(names) / sizeof(names[0]) == (MIG_KIND_PREDICT + 1U), "names 与 mig_kind_e 不同步");

    return ((unsigned)kind < (sizeof(names) / sizeof(names[0]))) ? names[kind] : "?";
}

/**
 * @brief GC 会话启动原因的名字（日志用）。表序 == mig_gc_start_reason_e。
 *
 * @param reason GC 会话启动原因枚举。
 *
 * @return "urgent"   —— 写路径分配不到页对；
 *         "periodic" —— 周期评估；
 *         "?"        —— 越界值。
 */
static const char *mig_gc_reason_name(mig_gc_start_reason_e reason)
{
    static const char *const names[] = {"urgent", "periodic"};

    _Static_assert(sizeof(names) / sizeof(names[0]) == (MIG_GC_START_PERIODIC + 1U), "names 与 mig_gc_start_reason_e 不同步");

    return ((unsigned)reason < (sizeof(names) / sizeof(names[0]))) ? names[reason] : "?";
}

/**
 * @brief GC 收益判据模式的名字（日志用）。表序 == gc_benefit_mode_e。
 *
 * @param mode 收益判据模式枚举。
 *
 * @return "off"  —— 不筛收益（host 正在写）；
 *         "idle" —— 空闲档收益判据；
 *         "emg"  —— 紧急档收益判据；
 *         "?"    —— 越界值。
 */
static const char *mig_gc_mode_name(gc_benefit_mode_e mode)
{
    static const char *const names[] = {"off", "idle", "emg"};

    _Static_assert(sizeof(names) / sizeof(names[0]) == (GC_BENEFIT_EMERGENCY + 1U), "names 与 gc_benefit_mode_e 不同步");

    return ((unsigned)mode < (sizeof(names) / sizeof(names[0]))) ? names[mode] : "?";
}

/* ── 2. 提交原语 ── */

/**
 * @brief 提交一条已建好的搬移命令：置 waiting、提交、失败回滚。
 * ⚠ stall_ms 只在**提交成功之后**清零：`ftl_op_submit` 自身失败（完成表满 / SQ 满）
 * 也属于"提交不出去"，提前清零会让 deadline 永不触发。
 *
 * @param m   job。
 * @param cmd 已建好的搬移命令；**所有权转移**给 op 层，提交失败时由本函数 release。
 *
 * @return FTL_OK    —— 已提交，waiting 置位；
 *         FTL_AGAIN —— 提交不出去（已 release 命令并回滚 waiting），调用方可重试。
 */
static ftl_status_e mig_command_submit(mig_handle_t *m, fc_command_t *cmd)
{
    m->waiting = true;

    // GC 与 WL 共用同一服务函数，但 op tag 分开——日志/调试时能看出命令属于哪条路径。
    const ftl_op_id_e op_tag = (m->kind == MIG_KIND_GC) ? FTL_OP_GC : FTL_OP_WL;

    if (ftl_op_submit(cmd, op_tag, m) != FTL_OK)
    {
        m->waiting = false;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    m->stall_ms    = 0U;
    m->wait_ms     = 0U;
    m->cmd_wait_ms = 0U;
    return FTL_OK;
}

/**
 * @brief 提交一条 victim 页读（整页，4 CW）。数据在完成时由服务函数拷进 m->page_buf
 * ——**读**不像写那样在 prepare 阶段搬数据（见 ftl_async_read_page 的注释）。
 *
 * @param m job（读 m->src[m->plane] 当前 plane）。
 *
 * @return FTL_OK    —— 已提交（命令所有权移交 op 层）；
 *         FTL_AGAIN —— 命令池空 / 提交背压，可重试；
 *         FTL_ERROR —— 永久错误，调用点应中止 job。
 */
static ftl_status_e mig_read_submit(mig_handle_t *m)
{
    fc_command_t      *cmd;
    const ftl_status_e ret = ftl_async_read_page(&cmd, m->src[m->plane], FTL_PAGE_CFG_DATA_ANY());
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_command_submit(m, cmd);
}

/**
 * @brief 提交一条目标页 CW 子集写。只编有效 CW（cw_bitmap 外的 CW 不建链、不编程）——
 * 整页重写会把无效 CW 连同其（陈旧的）META 一起搬过去，污染新页且多搬无用数据。
 * ⚠ 目标页的 plane 位就是 plane（alc_super_open_alloc_pair 的契约），故写进去的 META
 * 逐 CW 仍是正确的扇区 LBA。
 *
 * @param m job（目标页 m->dst[m->plane]、数据 m->page_buf、有效位 m->cw_bitmap）。
 *
 * @return FTL_OK    —— 已提交（命令所有权移交 op 层）；
 *         FTL_AGAIN —— 命令池空 / 提交背压，可重试；
 *         FTL_ERROR —— 永久错误，调用点应中止 job。
 */
static ftl_status_e mig_write_submit(mig_handle_t *m)
{
    // page0 异步写屏障：同 plane 的 page>0 必须等 page0 成功。
    // 若该 plane 的 page0 本来无数据，先写一页零补齐（不映射），再重试本页——
    // 这样 full rebuild 的「page0 空白⇒整块空白」捷径仍然成立。
    if ((pba_to_page(m->dst[m->plane]) > 0U) && (alc_super_open_get_pba_state(m->alloc, FTL_TEMP_COLD, m->dst[m->plane]) != ALC_SUPER_PBA_OPEN_PAGE0_DONE))
    {
        const DWORD   pba0 = super_block_page_pba(m->topo, &m->alloc->cold_sb, m->plane, 0U);
        fc_command_t *zero_cmd;

        const ftl_status_e zst = ftl_async_write_page_cw(&zero_cmd, pba0, s_zero_page, ftl_cw_plane_mask(m->plane), FTL_PAGE_CFG_DATA(0U)); /* 已知 LBA ⇒ META 非 FF，page0 不算空白 */
        if (zst != FTL_OK)
        {
            return zst;
        }

        m->dummy_page0 = true;
        m->dummy_pba   = pba0;
        return mig_command_submit(m, zero_cmd);
    }

    fc_command_t      *cmd;
    const ftl_status_e ret = ftl_async_write_page_cw(&cmd, m->dst[m->plane], m->page_buf[m->plane], m->cw_bitmap[m->plane], FTL_PAGE_CFG_DATA(ftl_lba_page(m->base[m->plane])));
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_command_submit(m, cmd);
}

/**
 * @brief 提交一条 victim 块擦除。
 *
 * @param m job（擦 m->victim 的当前 plane）。
 *
 * @return FTL_OK    —— 已提交（命令所有权移交 op 层）；
 *         FTL_AGAIN —— 命令池空 / 提交背压，可重试；
 *         FTL_ERROR —— 永久错误，调用点应中止 job。
 */
static ftl_status_e mig_erase_submit(mig_handle_t *m)
{
    const DWORD block_addr = super_block_page_pba(m->topo, &m->victim, m->plane, 0U);

    fc_command_t      *cmd;
    const ftl_status_e ret = ftl_async_erase_block(&cmd, block_addr);
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_command_submit(m, cmd);
}

/* ── 3. 启动/触发 ── */

/**
 * @brief 装配并启动一个搬移 job（SELECT 阶段）。
 *
 * @param m migrate handle
 * @param victim 选中的 SB（只读，内部按值拷贝）
 * @param kind victim 来源（同时决定 post / dst_high_wear）
 * @param reason 观测原因
 * @param triggers 触发计数快照。
 *
 * @note 实现要点：**快路径**——各 plane valid_count 之和 == 0 ⇒ 直接进 ERASE（省整块 NAND
 * 读；整块空洞 plane 的根因已由页打包写缓冲消除）。
 * victim 按值拷贝：closed_sb_list 在搬移期间可能被 allocator 追加/挪动，
 * 持指针会让「本 job 的 victim」随列表漂移（allocator.h 不变量③）。
 */
static void mig_job_start(mig_handle_t *m, const super_block_t *victim, mig_kind_e kind, const char *reason, DWORD triggers)
{
    // 前置条件：唯一调用者是本文件的 mig_gc_begin / mig_wl_begin / mig_predict_begin，
    // 各自在选块前已判过 active（单飞行）⇒ 这里不重复检查。

    // 收尾方式与目的块磨损档由 kind 唯一决定，避免调用者传入不一致的组合：
    // GC / 静态 WL → 归还 Free Pool；WL 目的块走高磨损档；
    // 预测性退役 → 退役、不入池。
    const mig_post_e post          = (kind == MIG_KIND_PREDICT) ? MIG_POST_RETIRE : MIG_POST_FREE;
    const bool       dst_high_wear = (kind == MIG_KIND_WL);

    // job 状态整体复位，但**窄上下文 / 自持节拍 / 单调计数 / 连续失败计数**必须跨 job 保留。
    alc_t *const       alloc                 = m->alloc;
    l2p_jrn_t *const   journal               = m->journal;
    l2p_cache_t *const cache                 = m->cache;
    const DWORD *const total_writes          = m->total_writes;
    const ftl_topo_t   topo                  = m->topo;
    const DWORD        gc_log                = m->gc_log_last_ms;
    const DWORD        wl_eval               = m->wl_eval_last_ms;
    const DWORD        wl_writes             = m->wl_last_writes;
    const DWORD        predict               = m->predict_last_ms;
    const DWORD        reclaimed             = m->reclaimed_count;
    const DWORD        moved_total           = m->moved_count;
    const DWORD        noop_total            = m->noop_count;
    const DWORD        novictim              = m->novictim_count;
    const DWORD        nogain_cnt            = m->nogain_count;
    const BYTE         read_fail_streak      = m->read_fail_streak;
    const bool         paused                = m->paused;
    const bg_level_e   saved_level           = m->level;
    const bool         timers_registered     = m->timers_registered;
    // 一旦某 victim 的 plane 基址不一致，batch 对整个 boot 关闭：否则每个 job 都会
    // 在同一页重复 abort（板端曾无限刷 `batch plane base mismatch`）。
    const bool batch_disabled                = m->batch_disabled;
    // GC 收益判据的状态（RISK-GC-13/14）：判据结论/退避/观测必须跨 job 存活。
    const DWORD             gc_writes_last   = m->gc_writes_last;
    const DWORD             gc_write_last_ms = m->gc_write_last_ms;
    const bool              host_writing     = m->host_writing;
    const bool              gc_nogain        = m->gc_nogain;
    const gc_benefit_mode_e nogain_mode      = m->gc_nogain_mode;
    const DWORD             nogain_ms        = m->gc_nogain_ms;
    const DWORD             nogain_writes    = m->gc_nogain_writes;
    const WORD              nogain_free      = m->gc_nogain_free;
    const WORD              best_valid_pct   = m->gc_best_valid_pct;
    const gc_benefit_mode_e last_mode        = m->gc_last_mode;
    // GC 会话状态属于连续活动，跨 job 的 memset 必须保留。
    const bool  gc_session_active            = m->gc_session_active;
    const DWORD gc_session_start_ms          = m->gc_session_start_ms;
    const DWORD gc_session_last_ms           = m->gc_session_last_ms;
    const DWORD gc_session_log_ms            = m->gc_session_log_ms;
    const DWORD gc_session_jobs              = m->gc_session_jobs;
    const DWORD gc_session_moved             = m->gc_session_moved;
    const DWORD gc_session_reclaimed         = m->gc_session_reclaimed;
    const DWORD gc_session_noop              = m->gc_session_noop;
    const DWORD gc_session_aborted           = m->gc_session_aborted;
    WORD        fail_gblock[PLANE_NUM];
    BYTE        fail_count[PLANE_NUM];

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        fail_gblock[plane] = m->fail_gblock[plane];
        fail_count[plane]  = m->fail_count[plane];
    }

    memset(m, 0, sizeof(*m));

    m->alloc                = alloc;
    m->journal              = journal;
    m->cache                = cache;
    m->total_writes         = total_writes;
    m->topo                 = topo;
    m->level                = saved_level;
    m->timers_registered    = timers_registered;
    m->gc_log_last_ms       = gc_log;
    m->wl_eval_last_ms      = wl_eval;
    m->wl_last_writes       = wl_writes;
    m->predict_last_ms      = predict;
    m->reclaimed_count      = reclaimed;
    m->moved_count          = moved_total;
    m->noop_count           = noop_total;
    m->novictim_count       = novictim;
    m->nogain_count         = nogain_cnt;
    m->gc_writes_last       = gc_writes_last;
    m->gc_write_last_ms     = gc_write_last_ms;
    m->host_writing         = host_writing;
    m->gc_nogain            = gc_nogain;
    m->gc_nogain_mode       = nogain_mode;
    m->gc_nogain_ms         = nogain_ms;
    m->gc_nogain_writes     = nogain_writes;
    m->gc_nogain_free       = nogain_free;
    m->gc_best_valid_pct    = best_valid_pct;
    m->gc_last_mode         = last_mode;
    m->gc_session_active    = gc_session_active;
    m->gc_session_start_ms  = gc_session_start_ms;
    m->gc_session_last_ms   = gc_session_last_ms;
    m->gc_session_log_ms    = gc_session_log_ms;
    m->gc_session_jobs      = gc_session_jobs;
    m->gc_session_moved     = gc_session_moved;
    m->gc_session_reclaimed = gc_session_reclaimed;
    m->gc_session_noop      = gc_session_noop;
    m->gc_session_aborted   = gc_session_aborted;
    // 系统性读失败计数必须跨 job 存活：它是「连续多个 victim 都读失败」的判据，
    // 一个 job 最多标 2 块（每 plane 一块）⇒ 若每个 job 都清零，阈值永远到不了。
    // `paused` 同理不能因 memset 丢失：熔断后任何路径误启动新 job 都不得复活 GC。
    m->read_fail_streak     = read_fail_streak;
    m->paused               = paused;
    m->batch_disabled       = batch_disabled;
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        m->fail_gblock[plane] = fail_gblock[plane];
        m->fail_count[plane]  = fail_count[plane];
    }

    m->victim         = *victim;
    m->victim.is_open = false; /* 本引擎只处理 Closed SB（见 allocator.h） */
    m->kind           = kind;
    m->log_reason     = reason;
    m->log_triggers   = triggers;
    m->post           = post;
    m->dst_high_wear  = dst_high_wear;
    m->active         = true;
    m->error          = FTL_OK;

    // 快路径：无任何有效 CW ⇒ 无可搬移，直接进擦除。
    alc_super_stats_t st;
    alc_super_get_stats(m->alloc, &m->victim, &st);
    m->job_valid_cw = st.valid_cw;
    // 有有效 CW ⇒ 必然写过至少一页。写指针为 0 说明 victim 构造时漏了 next_page，
    // 后果是「一页不读就擦」= 静默丢数据。
    ASSERT((st.valid_cw == 0U) || (m->victim.next_page > 0U));
    m->phase = (st.valid_cw == 0U) ? MIG_ERASE : MIG_READ;

    // 显式记 job 起点：心跳的懒初始化只在 >=1 s 后才发生，短 job 的 elapsed 否则恒 0。
    m->start_ms = ftl_job_now_ms();
    m->log_ms   = m->start_ms;

    // 每个真实 victim 只在 finish 打一行 move（触发/源/目的/结果）；
    // 空块（vcw==0）完全不进日志，由 GC 会话 noop= 聚合。
    mig_job_step(m);
}

/**
 * @brief 按触发位与 host 写流选收益判据模式（RISK-GC-13）。
 * 磨损均衡不在 GC 触发里，由独立 WL 路径处理，不在这里判 OFF。
 *
 * @param triggers     gc_should_trigger 返回的触发位掩码。
 * @param host_writing 最近一次求值的 host 写流判定。
 *
 * @return GC_BENEFIT_EMERGENCY —— 紧急水位触发；
 *         GC_BENEFIT_OFF       —— 水位触发且 host 正在写（不筛），或无触发；
 *         GC_BENEFIT_IDLE      —— 水位触发且 host 不写（筛收益）。
 */
static gc_benefit_mode_e mig_gc_mode_select(DWORD triggers, bool host_writing)
{
    if ((triggers & GC_TRIGGER_EMERGENCY) != 0U)
    {
        return GC_BENEFIT_EMERGENCY;
    }
    if ((triggers & GC_TRIGGER_WATERMARK) != 0U)
    {
        return host_writing ? GC_BENEFIT_OFF : GC_BENEFIT_IDLE;
    }
    return GC_BENEFIT_OFF;
}

/**
 * @brief 记录一次「空间 GC 没有值得搬的 victim」结论：退避 + 写路径 NO_SPACE 判据都依赖它。
 * ⚠ 只由空间 GC（mode != OFF）调用。
 *
 * @param m              job。
 * @param mode           产生该结论的收益判据模式。
 * @param best_valid_pct 本轮扫描的域内最低有效率（0..100）。
 * @param now_ms         结论时刻（退避起点；0 = 无时钟）。
 */
static void mig_gc_nogain_note(mig_handle_t *m, gc_benefit_mode_e mode, WORD best_valid_pct, DWORD now_ms)
{
    const bool first = (m->gc_nogain == false);

    m->novictim_count++;
    m->nogain_count++;
    m->gc_best_valid_pct = best_valid_pct;
    m->gc_last_mode      = mode;
    m->gc_nogain         = true;
    m->gc_nogain_mode    = mode;
    m->gc_nogain_ms      = now_ms;
    m->gc_nogain_writes  = *m->total_writes;
    m->gc_nogain_free    = m->alloc->free_count;

    if (first)
    {
        LOGD(LOG_TAG_GC "no-gain mode=%s minvr=%hu free=%hu writes=%lu — suspend until writes/emergency\r\n", mig_gc_mode_name(mode), best_valid_pct, m->alloc->free_count, *m->total_writes);
    }
}

/**
 * @brief 无收益结论后的复扫门（只挡 closed-list 扫描，不挡 10 ms 的 gc_should_trigger 计数器）。
 *
 * @param m        job。
 * @param mode     本轮触发位对应的收益判据模式。
 * @param triggers 本轮触发位掩码。
 * @param now_ms   当前时刻（0 = 无时钟）。
 *
 * @return true  —— 允许本轮扫描 closed list；
 *         false —— 仍处于 no-gain 退避窗口内，跳过扫描。
 */
static bool mig_gc_scan_is_allowed(const mig_handle_t *m, gc_benefit_mode_e mode, DWORD triggers, DWORD now_ms)
{
    if (m->gc_nogain == false)
    {
        return true;
    }
    if (mode == GC_BENEFIT_OFF)
    {
        return true; /* host 在写：不筛收益，必须重新扫 */
    }
    if (now_ms == 0U)
    {
        return true; /* 时钟缺失：保持「每轮求值」的旧语义 */
    }
    if (((triggers & GC_TRIGGER_EMERGENCY) != 0U) && (m->gc_nogain_mode != GC_BENEFIT_EMERGENCY))
    {
        return true; /* 刚从 IDLE 进入紧急：立即重扫 */
    }
    if (*m->total_writes != m->gc_nogain_writes)
    {
        return true; /* host 有新写入：可能产生了新垃圾 */
    }
    return (now_ms - m->gc_nogain_ms) >= GC_NOGAIN_BACKOFF_MS;
}

/**
 * @brief 尝试启动一个 GC job：选 victim、装配 job。
 *
 * @param m        job。
 * @param mode     收益判据模式（GC_BENEFIT_*）。
 * @param now_ms   当前时刻（0 = 无时钟）。
 * @param reason   启动原因（观测用）。
 * @param triggers 触发位快照（观测用）。
 *
 * @return true  —— 已启动 job；
 *         false —— 未启动（Journal 有写失败 pending / 选不出值得搬的 victim）。
 */
static bool mig_gc_begin(mig_handle_t *m, gc_benefit_mode_e mode, DWORD now_ms, mig_gc_start_reason_e reason, DWORD triggers)
{
    // 前置条件：唯一调用者 mig_handle 已在其顶部判过 active（单飞行）⇒ 不重复检查。
    if (jrn_has_write_failure(m->journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }

    gc_select_out_t   sel;
    const gc_select_e pick = gc_select_victim(m->alloc, mode, &sel);

    m->gc_last_mode      = mode;
    m->gc_best_valid_pct = sel.best_valid_pct;

    if (pick != GC_SELECT_OK)
    {
        if (pick == GC_SELECT_NOT_WORTHWHILE)
        {
            mig_gc_nogain_note(m, mode, sel.best_valid_pct, now_ms);
        }
        else
        {
            m->novictim_count++; /* 触发但根本没有候选 */
        }
        return false;
    }

    // 会话边界：只有**选中的 victim 自身有有效 CW** 才开新会话。
    // ⚠ 不能用 sel.best_valid_pct：它是「域内最低有效率」，只要存在任一空块就会
    // 为 0，即使选中的 victim 有大量有效数据。必须查 sel.victim 自身的 valid_cw。
    alc_super_stats_t vst;
    alc_super_get_stats(m->alloc, sel.victim, &vst);

    if ((m->gc_session_active == false) && (vst.valid_cw != 0U))
    {
        m->gc_session_active    = true;
        m->gc_session_start_ms  = now_ms;
        m->gc_session_last_ms   = now_ms;
        m->gc_session_log_ms    = now_ms;
        m->gc_session_jobs      = 0U;
        m->gc_session_moved     = 0U;
        m->gc_session_reclaimed = 0U;
        m->gc_session_noop      = 0U;
        m->gc_session_aborted   = 0U;
        // 只在会话起点打一次触发上下文；逐 victim 的 move 行降为 DEBUG，
        // 避免同一 reason/trig/mode 在大批搬移时反复刷 INFO。
        LOGI(LOG_TAG_GC "start reason=%s trig=0x%02lx mode=%s free=%hu\r\n", mig_gc_reason_name(reason), triggers, mig_gc_mode_name(mode), m->alloc->free_count);
    }

    if (m->gc_session_active)
    {
        m->gc_session_jobs++;
        m->gc_session_last_ms = now_ms;
    }

    mig_job_start(m, sel.victim, MIG_KIND_GC, mig_gc_reason_name(reason), triggers);
    // 不在启动时清 no-gain：job 可能先在 READ 阶段起来、随后 dest 分配失败并 abort。
    // 只有真正回收/搬移成功（mig_job_finish）才解除退避，见 mig_job_finish。
    return true;
}

/**
 * @brief 尝试启动一个静态磨损均衡 job。
 *
 * @param m job。
 *
 * @return true  —— 已启动 job；
 *         false —— 未启动（写失败 pending / 未到触发条件 / 容量紧张让给 GC / 选不出 victim）。
 */
static bool mig_wl_begin(mig_handle_t *m)
{
    // 前置条件：唯一调用者 mig_handle 已在其顶部判过 active（单飞行）⇒ 不重复检查。
    if (jrn_has_write_failure(m->journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }
    const DWORD total_writes = *m->total_writes;

    if (wl_should_trigger(m->alloc, total_writes) == false)
    {
        return false;
    }

    // 容量紧张时把单飞行引擎让给 GC：WL 只是磨损维护，GC 是前台写的前置条件。
    // （FREE_POOL_TARGET 也是 GC T1 的触发线；低于/等于它时不应再启动 WL。）
    if (m->alloc->free_count <= FREE_POOL_TARGET)
    {
        return false;
    }

    wl_victim_t victim;
    if (wl_select_victim(m->alloc, &victim) == false)
    {
        return false;
    }

    super_block_t sb;
    mig_victim_from_planes(&sb, victim.plane_blocks, victim.next_page);

    // 目标块方向：先把 host 的 cold SB 摘掉，让 job 的第一页新开 high-wear SB。
    alc_super_prepare_wl_destination(m->alloc);

    // 静态 WL 是空闲态慢过程，每个 job 一行，便于与 GC 区分。
    LOGI(LOG_TAG_WL "start free=%hu writes=%lu\r\n", m->alloc->free_count, total_writes);

    // 静态 WL 的方向：目标取高擦除空闲块（UBI 模型）。
    mig_job_start(m, &sb, MIG_KIND_WL, "wl", 0U);
    return true;
}

/**
 * @brief 尝试启动一个预测性退役搬移 job（victim 来自 rr_predictive_scan）。
 *
 * @param m job。
 *
 * @return true  —— 已启动 job；
 *         false —— 未启动（写失败 pending / scan 无命中 / closed 索引失效）。
 */
static bool mig_predict_begin(mig_handle_t *m)
{
    // 前置条件：唯一调用者 mig_handle 已在其顶部判过 active（单飞行）⇒ 不重复检查。
    if (jrn_has_write_failure(m->journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }

    WORD gblock;
    if (rr_predictive_scan(m->alloc, &gblock) == false)
    {
        return false;
    }

    // 预测性退役块必在 closed_sb_list 中（scan 只返回 CLOSED 块）。
    const WORD closed_idx   = ftl_block_sb_id(&m->alloc->blocks[gblock]);
    const WORD closed_count = m->alloc->closed_sb_count;

    ASSERT(closed_idx < closed_count);

    // ⚠ [待上板] release 运行期拒绝（H14）：ASSERT 在 NDEBUG 下裁空，越界会拿垃圾
    // plane_blocks 去擦块 = 静默介质破坏。
    if (closed_idx >= closed_count)
    {
        LOGE(LOG_TAG_MIG "predict: stale sb_id=%hu (closed=%hu) — ignored\r\n", closed_idx, closed_count);
        return false;
    }

    const super_block_t *closed = alc_super_closed_get_at(m->alloc, closed_idx);
    super_block_t        sb;

    mig_victim_from_planes(&sb, closed->plane_blocks, closed->next_page);

    mig_job_start(m, &sb, MIG_KIND_PREDICT, "pred", 0U);
    return true;
}

/* ── 4. 收尾 ── */

/**
 * @brief abort 日志限流（RISK-MIG-05）：坏块介质上 GC 会反复选中不可读 victim、每次
 * 重试都 abort；逐次 WRN 会把有效进展淹没。5 s 一行 + suppressed 计数；
 * `mark BAD` 仍逐块 LOGE（每个坏块只发生一次，是真正的结论）。
 *
 * @param out_suppressed 出参：本窗口内被抑制的条数（仅在返回 true 时写）。
 *
 * @return true  —— 本行应打印；
 *         false —— 本行被抑制（内部已累加计数）。
 */
static bool mig_abort_log_is_due(DWORD *out_suppressed)
{
    const DWORD now_ms = ftl_job_now_ms();

    // 有时间源时**按时间**限流（1 行/5 s），count 只作为无时钟时的兜底。
    // 不能把 count>=64 当成主判据：free=1 的 abort 循环一秒能撞几十次，64 次
    // 很快到，日志仍会刷屏。
    bool due;

    if (now_ms != 0U)
    {
        due = (s_abort_log_ms == 0U) || ((now_ms - s_abort_log_ms) >= FTL_JOB_LOG_SLOW_INTERVAL_MS);
    }
    else
    {
        due = (s_abort_suppressed >= 64U);
    }

    if (due == false)
    {
        s_abort_suppressed++;
        return false;
    }

    *out_suppressed    = s_abort_suppressed;
    s_abort_suppressed = 0U;
    s_abort_log_ms     = now_ms;
    return true;
}

/**
 * @brief 中止 job：不擦 victim、不动 L2P、不移除 closed 条目（源数据仍有效，下轮重试）。
 * 默认档限流打一行 abort；逐 plane 源块号属诊断细节，降到 LOGD。
 *
 * @param m      job。
 * @param reason 中止原因（日志用，静态字符串）。
 */
static void mig_job_abort(mig_handle_t *m, const char *reason)
{
    DWORD suppressed           = 0U;
    // 「没有目的页」且一个 CW 都没搬：这是满盘死路，不是 migrate 进展；
    // 不在 INFO/WARN 重复打，避免 free=1 时 abort 刷屏。其他原因（读失败/
    // 写失败/超时）仍保留限流 WARN。
    const bool no_free_no_move = ((m->migrated == 0U) && (strcmp(reason, "no free block") == 0));

    if ((no_free_no_move == false) && mig_abort_log_is_due(&suppressed))
    {
        char       stbuf[48];
        const WORD g0 = mig_victim_get_gblock(m, 0U);
        const WORD g1 = (PLANE_NUM > 1U) ? mig_victim_get_gblock(m, 1U) : UINT16_MAX;

        (void)fc_cmd_status_str(m->last_fc_status, stbuf, sizeof(stbuf));
        LOGW(LOG_TAG_MIG "abort kind=%s g0=%hu g1=%hu phase=%d page=%hhu plane=%hhu — %s fc=0x%08lx(%s) supp=%lu\r\n", mig_kind_name(m->kind), g0, g1, m->phase, m->page, m->plane, reason, m->last_fc_status, stbuf, suppressed);
    }

    if ((m->kind == MIG_KIND_GC) && m->gc_session_active)
    {
        m->gc_session_aborted++;
        m->gc_session_last_ms = ftl_job_now_ms();
    }

    m->aborted = true;
    m->error   = FTL_ERROR; /* status.error：最近一次中止的归因 */
    m->phase   = MIG_DONE;
}

/**
 * @brief 时钟在一个 deadline 起点之后是否已越过 timeout。0 起点 = 刚开始计时。
 *
 * @param start_ms   计时起点（0 = 首次进入，本函数置为当前时刻）；返回后保持起点。
 * @param timeout_ms 超时时长（ms）。
 *
 * @return true  —— 已越过 timeout；
 *         false —— 未超时 / 刚开始计时 / 平台时钟缺失（deadline 不生效）。
 */
static bool mig_deadline_is_expired(DWORD *start_ms, DWORD timeout_ms)
{
    const DWORD now = ftl_job_now_ms();

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

/**
 * @brief 提交不出去超过 FTL_SUBMIT_TIMEOUT_MS ⇒ 中止 job。返回 true = 已中止。
 *
 * @param m job。
 *
 * @return true  —— 已中止（timed_out / timeout_phase 已置）；
 *         false —— 未超时。
 */
static bool mig_job_stall(mig_handle_t *m)
{
    // cache 序列打开时被门控拒绝是预期等待（序列下一窗口收尾），不计提交 deadline。
    if (ftl_op_bg_submit_deferred() == true)
    {
        m->stall_ms = 0U;
        return false;
    }

    if (mig_deadline_is_expired(&m->stall_ms, FTL_SUBMIT_TIMEOUT_MS))
    {
        m->timed_out     = true;
        m->timeout_phase = m->phase;
        mig_job_abort(m, "submit timeout");
        return true;
    }
    return false;
}

/**
 * @brief 等外部模块超过 FTL_WAIT_TIMEOUT_MS ⇒ 中止 job（不擦 victim）。返回 true = 已中止。
 * ⚠ 与 mig_job_stall 分开：外部命令在飞也计时——"在飞"不等于"会完成"。
 * ⚠ 例外：等待原因是 **cache 序列门控**时不计时——门控等待必然在段边界结束
 *   （配额 / block 头 / 命令尾），与 mig_job_stall 同口径（见函数内注释）。
 *
 * @param m      job。
 * @param reason 中止原因（日志用，静态字符串）。
 *
 * @return true  —— 已中止（timed_out / timeout_phase 已置）；
 *         false —— 未超时。
 */
static bool mig_job_wait(mig_handle_t *m, const char *reason)
{
    // cache 序列打开时，等的是**门控**（Journal 刷页 / L2P 写回被序列挡住），不是
    // 在飞的外部命令：序列由配额 / block 头 / 命令尾收尾，等待必然在段边界结束。
    // 与 mig_job_stall 同口径不计超时；否则长顺序写（一段 ~300 ms 墙钟）会让本 job
    // 每 50 ms 中止一次，GC/WL 在 host 持续写下永远拿不到 Journal 容量。
    // ⚠ 这里**只**按序列判据挂起（不用 `ftl_op_bg_submit_deferred`）：本函数等的是
    //   外部模块的在飞命令，"在飞"不代表"会完成"，主机忙不是给它免死的理由
    //   （与 mig_job_stall 的提交背压判据区分开）。
    if (ftl_op_cache_seq_active() == true)
    {
        m->wait_ms = 0U;
        return false;
    }

    if (mig_deadline_is_expired(&m->wait_ms, FTL_WAIT_TIMEOUT_MS))
    {
        m->timed_out     = true;
        m->timeout_phase = m->phase;
        mig_job_abort(m, reason);
        return true;
    }
    return false;
}

/**
 * @brief 推进到下一页（清本页状态）。读/写两阶段的页推进都走这里。
 *
 * @param m job（推进 m->page，并清本页 plane / cw_bitmap / base / decode_ok / write_ok）。
 */
static void mig_page_next(mig_handle_t *m)
{
    m->page      = (m->page + 1U);
    m->plane     = 0U;
    m->decode_ok = false;
    m->write_ok  = false;
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        m->cw_bitmap[p] = 0U;
        m->base[p]      = 0U;
    }
    m->phase = MIG_READ;
}

/**
 * @brief 目标页失败处置（RISK-GC-02）：
 * - 命令从未提交（建链失败 / stall 超时）且本页对没有任何 plane 编程 ⇒ rewind；
 * - 命令已提交/可能半编程，或前序 plane 已编程 ⇒ 立即关闭 Cold SB。
 * 绝不能在同一个 Open SB 上继续写下一页。
 *
 * @param m                 job。
 * @param command_attempted true = 命令可能已提交或已半编程 ⇒ 关闭 Cold SB；
 *                          false = 本页对尚无 plane 编程 ⇒ rewind。
 */
static void mig_dst_fail_cleanup(mig_handle_t *m, bool command_attempted)
{
    // mig_write_step 只在上一 plane 的 WRITE_SETTLE 成功后才会推进到下一 plane，
    // 所以「前序 plane 的 cw_bitmap 非 0」⇒ 它已编程。
    bool pair_programmed = false;

    for (BYTE p = 0U; p < m->plane; p++)
    {
        if (m->cw_bitmap[p] != 0U)
        {
            pair_programmed = true;
            break;
        }
    }

    if (command_attempted || pair_programmed)
    {
        LOGD(LOG_TAG_MIG "dst failure plane=%hhu — close cold SB\r\n", m->plane);
        (void)alc_super_close(m->alloc, FTL_TEMP_COLD);
    }
    else
    {
        LOGD(LOG_TAG_MIG "dst failure plane=%hhu — rewind pair\r\n", m->plane);
        alc_super_open_rewind_pair(m->alloc, FTL_TEMP_COLD);
    }
}

/**
 * @brief DONE——移除 closed 条目、记进展计数、复位 job。
 *
 * @param m migrate handle。
 *
 * @note 实现要点：定位 victim 在 closed_sb_list 的索引由 allocator 统一完成（比对全部 plane；
 * Policy B 下退化单 plane 条目的哨兵 plane 相同，只比一个 plane 会命中另一条）。
 * aborted ⇒ 不做任何列表/计数变更（victim 未被擦，仍有效）。
 */
static void mig_job_finish(mig_handle_t *m)
{
    bool reclaimed_this = false;

    if (m->aborted == false)
    {
        // 整块迁移成功 ⇒ 该块的历史读失败只是瞬态，清零。
        // 只在**整个 job 完成**时清（所有已编程页都读过），不能在单页读成功或
        // 单 plane 擦成功时清：坏页之后的页永远读不到，按单页成功清零会让标坏阈值
        // 永不触发（RISK-GC-07）；逐 plane 清零会让系统性熔断永不触发（RISK-GC-10）。
        m->read_fail_streak = 0U;
        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (super_block_has_plane(&m->victim, plane) == false)
            {
                continue;
            }
            if (m->fail_gblock[plane] == ftl_gblock(plane, m->victim.plane_blocks[plane]))
            {
                m->fail_gblock[plane] = UINT16_MAX;
                m->fail_count[plane]  = 0U;
            }
        }

        WORD closed_idx;
        // 逐 plane 擦除时条目可能已被 alc_super_closed_drop_plane 摘空并移除
        // （最后一个 plane 摘除即删条目）⇒ 此时按原值查找必然失败。
        bool removed = m->closed_removed;

        // 移除已回收条目（swap-remove）——防 closed_sb_count 只增不减导致列表爆炸
        // + 已回收 SB 被再次选为 victim 的无限 GC 空转。
        // 擦除失败也移除：该 SB 已坏/不可再用，留在列表里只会反复被选。
        if ((removed == false) && alc_super_closed_find(m->alloc, &m->victim, &closed_idx))
        {
            alc_super_closed_remove(m->alloc, closed_idx);
            removed = true;
        }

        // 进展计数 / 预测日志只在**真正回收成功**时记：擦除失败或条目没找到都不算。
        if (removed && (m->erase_failed == false))
        {
            if (m->kind == MIG_KIND_GC)
            {
                m->reclaimed_count++; /* 进展计数（见 ftl.h：区分"在推进"与"卡死"） */
                reclaimed_this = true;
            }
            else if (m->kind == MIG_KIND_PREDICT)
            {
                // 预测性退役命中：整 SB 已搬空并退役，属生命周期跃迁，留痕一次
                for (BYTE p = 0U; p < PLANE_NUM; p++)
                {
                    if (super_block_has_plane(&m->victim, p) == false)
                    {
                        continue;
                    }
                    LOGW(LOG_TAG_FTL "predictive relocate retired g=%hu\r\n", mig_victim_get_gblock(m, p));
                }
            }
        }
    }

    // 累计观测（跨 job 单调）：用于确认「真的搬了数据」还是只回收空块。
    if (m->aborted == false)
    {
        m->moved_count += m->migrated;

        if ((m->kind == MIG_KIND_GC) && ((m->migrated != 0U) || reclaimed_this))
        {
            // 只有真正搬移/成功回收才解除 no-gain 退避；启动 job 不等价于有收益，
            // 否则「起 READ → dest 分配失败 → abort」会把边沿日志刷成每轮一行。
            m->gc_nogain = false;
        }
        if ((m->kind == MIG_KIND_GC) && reclaimed_this && (m->job_valid_cw == 0U))
        {
            m->noop_count++;
        }
    }

    // GC 会话计数：搬移量、成功回收、空块回收。abort 走 mig_job_abort 的单独分支。
    if ((m->aborted == false) && (m->kind == MIG_KIND_GC) && m->gc_session_active)
    {
        m->gc_session_moved += m->migrated;
        if (reclaimed_this)
        {
            m->gc_session_reclaimed++;
            if (m->job_valid_cw == 0U)
            {
                m->gc_session_noop++;
            }
        }
        m->gc_session_last_ms = ftl_job_now_ms();
    }

#if defined(DEBUG)
    // 每个真实 victim 只在 DEBUG 打一行，且只带会变化的字段：
    // 触发原因/trig/mode 已在 [GC] start 打一次，npg 通常是常量。
    // 空块（vcw==0）不逐条打；它们在 GC 会话的 noop= 里聚合。
    if ((m->aborted == false) && (m->migrated != 0U))
    {
        const WORD  src0    = mig_victim_get_gblock(m, 0U);
        const WORD  src1    = (PLANE_NUM > 1U) ? mig_victim_get_gblock(m, 1U) : UINT16_MAX;
        const WORD  dst0    = mig_dst_get_gblock(m, 0U);
        const WORD  dst1    = (PLANE_NUM > 1U) ? mig_dst_get_gblock(m, 1U) : UINT16_MAX;
        const DWORD elapsed = (m->start_ms != 0U) ? ftl_job_age_ms(&m->start_ms) : 0U;

        LOGD(LOG_TAG_MIG "move k=%s r=%s trig=0x%02lx src0=%hu src1=%hu dst0=%hu dst1=%hu v=%hu mv=%lu t=%lums\r\n", mig_kind_name(m->kind), m->log_reason, m->log_triggers, src0, src1, dst0, dst1, m->job_valid_cw, m->migrated, elapsed);
    }
#endif

    m->active             = false;
    m->phase              = MIG_IDLE;
    m->waiting            = false;
    m->decode_ok          = false;
    m->write_ok           = false;
    m->closed_removed     = false;
    m->stall_ms           = 0U;
    m->wait_ms            = 0U;
    m->cmd_wait_ms        = 0U;
    m->dummy_page0        = false;
    m->dummy_pba          = NAND_PBA_INVALID;
    m->batch_mode         = false;
    m->batch_count        = 0U;
    m->batch_outstanding  = 0U;
    m->batch_failed       = false;
    m->batch_cleanup_done = false;
    // fail_gblock[] / fail_count[] 跨 job 保留（RISK-GC-01/07）：只在本 job 正常完成
    // （上方整块迁移成功）或该 plane 标 BAD 时清零，不在这里无条件清。
}

/* ── 5. 推进 ── */

/* ── 5.1 页级映射提交（普通 WRITE_SETTLE / batch SETTLE 共用）── */

/**
 * @brief 提交单个 plane 的页级映射（Journal→L2P→BST）。调用前已做就绪/容量前置。
 * 普通路径传 m 的字段、batch 路径传 batch 页的字段，故按数组传入。
 *
 * @param m       job（更新有效计数与 migrated）。
 * @param plane   本 plane 下标。
 * @param bitmap  逐 CW 有效位（flat 绝对编号，bit = plane*CW_NUM+cw）。
 * @param cw_lba  逐 CW 的扇区 LBA（未写过 = FTL_LBA_INVALID）。
 * @param src     各 plane 的源 PBA。
 * @param dst     各 plane 的目标 PBA。
 */
static void mig_plane_meta_commit(mig_handle_t *m, BYTE plane, const DWORD bitmap[PLANE_NUM], const DWORD cw_lba[PLANE_NUM][CW_NUM], const DWORD src[PLANE_NUM], const DWORD dst[PLANE_NUM])
{
    for (BYTE cw = 0U; cw < CW_NUM; cw++)
    {
        if ((bitmap[plane] & ftl_cw_bit(plane, cw)) == 0U)
        {
            continue;
        }

        const DWORD lba = cw_lba[plane][cw];

        if (mig_lba_is_usable(m, lba) == false)
        {
            continue;
        }

        DWORD cur;

        if (l2p_read(m->cache, lba, &cur) != FTL_OK)
        {
            mig_job_abort(m, "l2p read failed");
            return;
        }

        const DWORD src_cw = pba_with_cw(src[plane], cw);

        if (cur != src_cw)
        {
            continue; /* host 写 / TRIM 已生效 */
        }

        const DWORD dst_cw = pba_with_cw(dst[plane], cw);

        if (jrn_append(m->journal, lba, src_cw, dst_cw) == false)
        {
            mig_job_abort(m, "journal append failed");
            return;
        }
        if (l2p_write(m->cache, lba, dst_cw) != FTL_OK)
        {
            mig_job_abort(m, "l2p write failed");
            return;
        }

        const WORD src_gblock = ftl_gblock(plane, m->victim.plane_blocks[plane]);
        const WORD dst_gblock = ftl_pba_gblock(dst_cw);

        ftl_block_sub_valid_if_live(&m->alloc->blocks[src_gblock]);
        ftl_block_add_valid(&m->alloc->blocks[dst_gblock]);
        m->migrated++;
    }
}

/* ── 5.2 cache program 批次辅助（GC/WL）── */

/**
 * @brief 当前页有数据的 plane 是否共享同一页基址。pair 命令只有一个 cmd->meta，
 * 两 plane 都有数据时基址不一致会把 plane1 的 META 写错。
 *
 * @param m job（本页 cw_bitmap / base）。
 *
 * @return true  —— 有数据的 plane 之间页基址不一致；
 *         false —— 一致或至多只有一个 plane 有数据。
 */
static bool mig_page_base_has_mismatch(const mig_handle_t *m)
{
    bool  have_base = false;
    DWORD pair_base = 0U;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (m->cw_bitmap[p] == 0U)
        {
            continue;
        }
        if (have_base == false)
        {
            pair_base = m->base[p];
            have_base = true;
        }
        else if (m->base[p] != pair_base)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 把当前页（数据已在 page_buf、元数据已在 m->cw_*）存入 batch 槽。
 *
 * @param m job（batch_count 由调用链保证 < MIG_BATCH_PAGES）。
 */
static void mig_batch_store_page(mig_handle_t *m)
{
    const BYTE j = m->batch_count; /* 调用链保证 < MIG_BATCH_PAGES */

    mig_batch_page_t *bp = &m->batch[j];

    memset(bp, 0, sizeof(*bp));

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        bp->base[p]      = m->base[p];
        bp->cw_bitmap[p] = m->cw_bitmap[p];
        bp->src[p]       = m->src[p];

        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            bp->cw_lba[p][cw] = m->cw_lba[p][cw];
        }

        if (super_block_has_plane(&m->victim, p))
        {
            (void)memcpy(m->batch_buf[j][p], m->page_buf[p], PAGE_MAIN_SIZE);
        }
        else
        {
            memset(m->batch_buf[j][p], 0, PAGE_MAIN_SIZE);
        }
    }
}

/**
 * @brief 批内页的 L2P 就绪前置 + Journal 总容量预留。返回 true = 可以提交。
 *
 * @param m job（batch_count 页）。
 *
 * @return true  —— L2P 全部就绪且 Journal 容量预留成功，可以提交；
 *         false —— 尚未就绪 / 容量不足（已挂起等待，超时会中止 job）。
 */
static bool mig_batch_prepare_commit(mig_handle_t *m)
{
    DWORD entries = 0U;

    for (BYTE j = 0U; j < m->batch_count; j++)
    {
        const mig_batch_page_t *bp = &m->batch[j];

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            for (BYTE cw = 0U; cw < CW_NUM; cw++)
            {
                if ((bp->cw_bitmap[p] & ftl_cw_bit(p, cw)) == 0U)
                {
                    continue;
                }
                const DWORD lba = bp->cw_lba[p][cw];

                if (mig_lba_is_usable(m, lba) == false)
                {
                    continue;
                }
                if (l2p_is_ready(m->cache, lba) == false)
                {
                    (void)mig_job_wait(m, "l2p load timeout");
                    return false;
                }
                entries++;
            }
        }
    }

    if (jrn_reserve(m->journal, (WORD)entries) == false)
    {
        (void)mig_job_wait(m, "journal capacity timeout");
        return false;
    }

    m->wait_ms = 0U;
    return true;
}

/**
 * @brief 释放 cmds[0..n) 中已建好的命令（NULL 条目跳过）。
 *
 * @param cmds 命令数组。
 * @param n    需要释放的条数。
 */
static void mig_batch_release_cmds(fc_command_t *cmds[], BYTE n)
{
    for (BYTE k = 0U; k < n; k++)
    {
        if (cmds[k] != NULL)
        {
            (void)fc_command_release(cmds[k]);
        }
    }
}

/**
 * @brief 归还本次已分配但不打算使用的 n 个目的页对。
 *
 * @param m job。
 * @param n 需要 rewind 的页对数。
 */
static void mig_batch_rewind_dst(mig_handle_t *m, BYTE n)
{
    for (BYTE k = 0U; k < n; k++)
    {
        alc_super_open_rewind_pair(m->alloc, FTL_TEMP_COLD);
    }
}

/**
 * @brief 建 N 条 pair 命令（不提交）。失败时释放已建命令，返回 FTL_AGAIN/FTL_ERROR。
 *
 * @param m    job。
 * @param n    本批页数（1..MIG_BATCH_PAGES）。
 * @param cmds 出参：建好的 n 条命令（失败时已释放前序命令）。
 *
 * @return FTL_OK    —— n 条命令全部建好；
 *         FTL_AGAIN —— 命令池 / 描述符 / 缓冲池空（瞬态背压）；
 *         FTL_ERROR —— 建链失败。
 */
static ftl_status_e mig_batch_build_cmds(mig_handle_t *m, BYTE n, fc_command_t *cmds[MIG_BATCH_PAGES])
{
    const bool use_cache = (n >= 2U);

    for (BYTE j = 0U; j < n; j++)
    {
        const mig_batch_page_t *bp = &m->batch[j];
        const BYTE              op = use_cache ? ftl_cache_prog_op(j, n) : OP_PROGRAM_PAGE_MULTI_PLANE;

        // 页基址取第一个有数据的 plane（两 plane 都有数据时 READ 阶段已检查相等）。
        DWORD base = 0U;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (bp->cw_bitmap[p] != 0U)
            {
                base = bp->base[p];
                break;
            }
        }

        const ftl_page_cfg_t *cfg = FTL_PAGE_CFG_DATA(ftl_lba_page(base));

        const ftl_status_e st = ftl_async_write_pair_cmd(&cmds[j], bp->dst, &m->batch_buf[j][0][0], FTL_CACHE_FULL_CW, op, cfg);

        if (st != FTL_OK)
        {
            mig_batch_release_cmds(cmds, n);
            return st;
        }

        // N>=2 是一条 cache program 序列（FIRST/中间/END）：整批标成一条 FC chain，
        // 末条 END，其余 MORE——链没结束时 FC 不让另一队列向该 LUN 插命令。
        cmds[j]->config.chain = (j + 1U < n) ? FC_CHAIN_MORE : FC_CHAIN_END;
    }

    return FTL_OK;
}

/**
 * @brief 为 N 页分配连续目的页对；失败时归还已分配的前缀。
 *
 * @param m job（batch[].dst 为出参）。
 * @param n 本批页数。
 *
 * @return true  —— n 个目的页对全部分配成功；
 *         false —— 分配失败（已归还已分配的前缀）。
 */
static bool mig_batch_alloc_dst(mig_handle_t *m, BYTE n)
{
    for (BYTE j = 0U; j < n; j++)
    {
        if (alc_super_open_alloc_pair(m->alloc, ALC_SUPER_ALLOC_COLD, m->batch[j].dst) != FTL_OK)
        {
            mig_batch_rewind_dst(m, j);
            return false;
        }
    }
    return true;
}

/* ── 5.3 各 phase 处理 + job 步进 ── */

/* ── 各 phase 处理函数 ──
 * 返回 true = 本轮到此为止（提交了一条命令 / 提交失败 / 原地等资源）；
 * 返回 false = 纯计算推进了一步，mig_job_step 继续处理下一个 phase。
 */

/**
 * @brief MIG_READ：读 victim 当前页的当前 plane（只读 [0, next_page)）。
 *
 * @param m job。
 *
 * @return true  —— 本轮到此为止（已提交 / 提交失败 / 原地等资源）；
 *         false —— 纯计算推进了一步。
 */
static bool mig_read_step(mig_handle_t *m)
{
    // 只读该块**已编程**的页 [0, next_page)。空洞页物理上是擦除态，读它只会得到
    // BLANK；跳过它们既省命令，也不给「把空洞误判成坏块」留窗口（RISK-GC-11）。
    // next_page == 0 只可能来自异常路径（正常 victim 至少写过 1 页）：保守按整块扫，
    // 读数由 FC 的擦除页识别兜底。
    BYTE page_limit = m->victim.next_page;

    // release 兜底（H14）：next_page 越界时按整块扫——**绝不能**让它把 page 推出
    // PAGES_PER_BLOCK（那会算出非法 PAA）。next_page == 0 只可能来自异常路径。
    if ((page_limit == 0U) || (page_limit > PAGES_PER_BLOCK))
    {
        page_limit = PAGES_PER_BLOCK;
    }

    if (m->page >= page_limit)
    {
        m->phase = (m->batch_count != 0U) ? MIG_WRITE_BATCH : MIG_FLUSH_JRN;
        return false;
    }

    // 找本页下一个有物理块的 plane
    m->plane = mig_present_plane_next(m->alloc, m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        // 本页各 plane 都处理完 ⇒ 决定去向
        bool any = false;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (m->cw_bitmap[p] != 0U)
            {
                any = true;
            }
        }

        if (any == false)
        {
            if (m->batch_mode)
            {
                // 空洞页不占 batch 槽；继续读下一页。
                m->plane = 0U;
                m->page++;
                m->phase = (m->page >= page_limit) ? MIG_WRITE_BATCH : MIG_READ;
                if (m->batch_count == 0U)
                {
                    m->batch_mode = false;
                    m->phase      = MIG_READ;
                }
                return false;
            }

            mig_page_next(m); /* 空洞 / 全被覆盖 */
            return false;
        }

        // 只有普通 GC/预测搬移走 batch；静态 WL 的 high-wear 目标选块与连续页假设
        // 冲突；room 已蕴含：SB 开着、所有 plane 的 page0 已成功、剩余 ≥4 页。
        if ((m->batch_mode == false) && (m->dst_high_wear == false) && (m->batch_disabled == false) && (alc_super_open_get_room(m->alloc, FTL_TEMP_COLD) >= MIG_BATCH_PAGES))
        {
            m->batch_mode = true;
        }

        if (m->batch_mode)
        {
            // pair 命令只有一个 cmd->meta：两 plane 都有数据时必须同页基址，
            // 否则 plane1 的 META 会被写成 plane0 的 LBA（静默错映射）。
            // 遇到这种页：已有缓冲 ⇒ 本 job 禁用 batch 并中止（victim 不擦）；
            // 无缓冲 ⇒ 本页走普通逐 plane 路径。
            if (mig_page_base_has_mismatch(m))
            {
                // 两 plane 的页基址不一致 ⇒ pair 命令无法安全写。对整个 boot 关闭
                // cache batch（跨 job 保留），否则同一页会反复 abort。
                m->batch_disabled = true;

                if (m->batch_count != 0U)
                {
                    m->batch_count = 0U;
                    m->batch_mode  = false;
                    mig_job_abort(m, "batch plane base mismatch");
                    return false;
                }
                m->batch_mode = false;
            }
        }

        if (m->batch_mode)
        {
            // 攒页：数据/元数据进 batch 槽，不分配目的页、不提交写命令。
            mig_batch_store_page(m);
            m->batch_count++;
            m->plane = 0U;
            m->page++;

            if ((m->batch_count >= MIG_BATCH_PAGES) || (m->page >= page_limit))
            {
                m->phase = MIG_WRITE_BATCH;
            }
            else
            {
                m->phase = MIG_READ;
            }
            return false;
        }

        // 成对分配目标页（Cold 流）。池耗尽 ⇒ 中止（不擦 victim，源数据仍在、
        // L2P 未动，下轮 GC 再来）——**不得**先擦后搬。
        // 静态 WL 走 high-wear 变体：目标取擦除最多的空闲块（RISK-WL-08）。
        const alc_super_alloc_mode_e dst_mode = m->dst_high_wear ? ALC_SUPER_ALLOC_COLD_HIGH_WEAR : ALC_SUPER_ALLOC_COLD;
        const ftl_status_e           dst_st   = alc_super_open_alloc_pair(m->alloc, dst_mode, m->dst);

        if (dst_st != FTL_OK)
        {
            // 满盘且选出的 victim 也换不到目的页 ⇒ 与「无收益 victim」同义：
            // 没有可回收的空间。必须置 no-gain，否则写路径会一直重试
            // 「选中 → 目的分配失败 → abort」（RISK-GC-14）。
            if (m->kind == MIG_KIND_GC)
            {
                mig_gc_nogain_note(m, GC_BENEFIT_EMERGENCY, m->gc_best_valid_pct, ftl_job_now_ms());
            }
            mig_job_abort(m, "no free block");
            return false;
        }

        m->dst_allocated = true;
        m->plane         = 0U;
        m->phase         = MIG_WRITE;
        return false;
    }

    // 本页本 plane 的源地址（页基址）。全局块号不缓存——用 ftl_gblock() 现算。
    m->src[m->plane] = super_block_page_pba(m->topo, &m->victim, m->plane, m->page);
    memset(m->page_buf[m->plane], 0, PAGE_MAIN_SIZE);

    const ftl_status_e ret = mig_read_submit(m);
    if (ret == FTL_AGAIN)
    {
        (void)mig_job_stall(m);
    }
    else if (ret != FTL_OK)
    {
        mig_job_abort(m, "read command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

/**
 * @brief MIG_READ_SETTLE：先做 L2P 就绪前置，再一次性算出本 plane 的 cw_bitmap。
 * 空页（decode_ok == false）直接置空位图；region 未驻留则原地返回、下轮重扫。
 *
 * @param m job。
 *
 * @return true  —— 本轮到此为止（L2P 尚未就绪）；
 *         false —— 纯计算推进了一步。
 */
static bool mig_read_settle_step(mig_handle_t *m)
{
    const BYTE plane = m->plane;

    if (m->decode_ok == false)
    {
        // 空页（无错误 + META 全 FF）：本 plane 无有效 CW。
        // NAND 读错误已在 mig_read_complete 里中止 job，不会走到这里。
        m->cw_bitmap[plane] = 0U;
    }
    else
    {
        // 就绪前置：本页逐 CW 的扇区 LBA 来自页内 META，与刚访问过的 host LBA
        // **无必然联系** ⇒ region miss 现实可达。未驻留 ⇒ 起异步加载并原地返回，
        // 下轮重扫（重扫幂等：ready 命中不记账、不动 access_count）。
        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            if (mig_lba_is_usable(m, m->cw_lba[plane][cw]) == false)
            {
                continue;
            }
            if (l2p_is_ready(m->cache, m->cw_lba[plane][cw]) == false)
            {
                // 加载在飞不计提交卡死，但**必须**计入独立的等待上限：
                // 加载永久卡住时 job 否则永不中止（RISK-MIG-01）。
                (void)mig_job_wait(m, "l2p load timeout");
                return true;
            }
        }

        m->wait_ms = 0U; /* 等待条件解除 */

        // 全部就绪 ⇒ 一次性算出本 plane 的位图（不会半途而废）
        m->cw_bitmap[plane] = 0U;
        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            DWORD cur;

            if (mig_lba_is_usable(m, m->cw_lba[plane][cw]) == false)
            {
                continue;
            }
            // 用**该 CW 自己的 META** 查，仍指向 (本页, 本 CW) 才算有效
            if ((l2p_read(m->cache, m->cw_lba[plane][cw], &cur) == FTL_OK) && (cur == pba_with_cw(m->src[plane], cw)))
            {
                m->cw_bitmap[plane] |= ftl_cw_bit(plane, cw);
            }
        }
    }

    m->decode_ok = false;
    m->phase     = MIG_READ;
    m->plane     = (plane + 1U); /* 可能 == PLANE_NUM ⇒ 页结算 */
    return false;
}

/**
 * @brief MIG_WRITE：对本页下一个有待写 CW 的 plane 提交一条 CW 子集写。
 *
 * @param m job。
 *
 * @return true  —— 本轮到此为止（已提交 / 提交失败 / 原地等资源）；
 *         false —— 纯计算推进了一步。
 */
static bool mig_write_step(mig_handle_t *m)
{
    // 找本页下一个有待写 CW 的 plane
    BYTE p;

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
        mig_page_next(m);
        return false;
    }

    const ftl_status_e ret = mig_write_submit(m);
    if (ret == FTL_AGAIN)
    {
        if (mig_job_stall(m))
        {
            // 命令从未提交：无 plane 编程则 rewind，否则关 Cold SB。
            mig_dst_fail_cleanup(m, false);
        }
    }
    else if (ret != FTL_OK)
    {
        mig_dst_fail_cleanup(m, false);
        mig_job_abort(m, "write command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

/**
 * @brief MIG_WRITE_SETTLE：先做 L2P 就绪与 Journal 容量前置，再提交本 plane 的页级映射。
 *
 * @param m job。
 *
 * @return true  —— 本轮到此为止（尚未就绪 / 容量不足 / 等待超时）；
 *         false —— 纯计算推进了一步。
 */
static bool mig_write_settle_step(mig_handle_t *m)
{
    const BYTE plane = m->plane;

    if (m->write_ok)
    {
        // 就绪前置（同 READ_SETTLE）：全部就绪才动账——半途而废会重复记账
        // （valid_count 虚高、journal 重复条目）。
        // ⚠ [待上板] 位图判据必须与 READ_SETTLE 的 `ftl_cw_bit(plane, cw)` 同源：
        // flat 绝对编号下 plane 1 的有效位在 bit 4..7，按 plane 内相对位读会整段漏账。
        // 验证法：各 plane 写满 → 触发 GC → 读回 plane 1 的 LBA（应仍正确）+ 看
        // journal / valid_count 迁到新页。
        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            if ((m->cw_bitmap[plane] & ftl_cw_bit(plane, cw)) == 0U)
            {
                continue;
            }
            if (l2p_is_ready(m->cache, m->cw_lba[plane][cw]) == false)
            {
                (void)mig_job_wait(m, "l2p load timeout");
                return true;
            }
        }

        // Journal 容量预留：本 plane 最多 append popcount(cw_bitmap) 条。不足时
        // jrn_reserve 只起异步刷页并返 false，本 job 挂起等下一轮 service；不在
        // migrate 里同步刷 Journal（那会在 service 调用栈里再开完成泵）。
        // ⚠ wait_ms 必须等容量也拿到才清：否则每轮 L2P ready 都会重置等待超时，
        // Journal 永久无容量时 job 永不中止。
        const BYTE n_entries = ftl_cw_bitmap_popcount(m->cw_bitmap[plane]);

        if (jrn_reserve(m->journal, n_entries) == false)
        {
            (void)mig_job_wait(m, "journal capacity timeout");
            return true;
        }

        m->wait_ms = 0U; /* L2P 与 Journal 容量均已就绪 */

        mig_plane_meta_commit(m, plane, m->cw_bitmap, m->cw_lba, m->src, m->dst);

        if (m->aborted)
        {
            return false;
        }

        // GC/WL 的 Journal 追加同样推进水位；漏掉会让 journal 只靠 T2/T3 触发 CP，
        // 大量搬移时可能撞 ring_full（新背压路径会把 host 写停到超时）。
        jrn_watermark_update(m->journal);
    }

    m->write_ok = false;
    m->phase    = MIG_WRITE;
    m->plane    = (plane + 1U);
    return false;
}

/**
 * @brief 提交一个已收集的 batch：N 页 pair 命令，一次全进 SQ 并标成一条 FC chain。
 * N>=2 用 cache program（首页 FIRST + 中间 CACHE + 末页 END）；N==1 用普通 op31。
 * 目的页预检保证同一 Open SB、页号 >=1 且 room>=N，因此不会跨块/落到 page0。
 *
 * @param m job（batch_count / batch[]）。
 *
 * @return true  —— 本轮到此为止（已提交 / 提交失败 / 背压原地等）；
 *         false —— 纯计算推进了一步。
 */
static bool mig_write_batch_step(mig_handle_t *m)
{
    const BYTE n = m->batch_count;

    if (n == 0U)
    {
        m->batch_mode = false;
        m->phase      = MIG_READ;
        return false;
    }

    if (mig_batch_prepare_commit(m) == false)
    {
        return true;
    }

    if (mig_batch_alloc_dst(m, n) == false)
    {
        m->batch_aborts++;
        mig_job_abort(m, "batch dst alloc failed");
        return false;
    }

    m->dst_allocated = true;

    fc_command_t *cmds[MIG_BATCH_PAGES] = {0};
    ftl_status_e  st                    = mig_batch_build_cmds(m, n, cmds);

    if (st == FTL_OK)
    {
        const ftl_op_id_e op_tag = (m->kind == MIG_KIND_GC) ? FTL_OP_GC : FTL_OP_WL;

        st = ftl_op_submit_batch(cmds, n, op_tag, m);

        if (st != FTL_OK)
        {
            mig_batch_release_cmds(cmds, n);
        }
    }

    if (st != FTL_OK)
    {
        mig_batch_rewind_dst(m, n);
        m->dst_allocated = false;

        if (st == FTL_AGAIN)
        {
            // 命令池/SQ 瞬态背压：保留 batch 缓冲，下一轮重建；超时才 abort。
            (void)mig_job_stall(m);
            return true;
        }

        m->batch_aborts++;
        mig_job_abort(m, "batch build/submit failed");
        return false;
    }

    m->waiting            = true;
    m->batch_outstanding  = n;
    m->batch_failed       = false;
    m->batch_cleanup_done = false;
    m->stall_ms           = 0U;
    if (n >= 2U)
    {
        m->batch_jobs++;
        m->batch_pages += n;
    }
    m->phase = MIG_WRITE_BATCH;
    return true;
}

/**
 * @brief 批次全部成功后的两遍式确认：就绪/Journal 全过后，逐页提交映射。
 *
 * @param m job（batch_count / batch[]）。
 *
 * @return true  —— 本轮到此为止（尚未就绪 / 容量不足 / 等待超时）；
 *         false —— 纯计算推进了一步。
 */
static bool mig_write_settle_batch_step(mig_handle_t *m)
{
    if (mig_batch_prepare_commit(m) == false)
    {
        if (m->aborted)
        {
            (void)alc_super_close(m->alloc, FTL_TEMP_COLD);
        }
        return true;
    }

    for (BYTE j = 0U; j < m->batch_count; j++)
    {
        const mig_batch_page_t *bp = &m->batch[j];

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            mig_plane_meta_commit(m, p, bp->cw_bitmap, bp->cw_lba, bp->src, bp->dst);

            if (m->aborted)
            {
                return false;
            }
        }
    }

    jrn_watermark_update(m->journal);

    m->batch_count   = 0U;
    m->batch_mode    = false;
    m->dst_allocated = false;
    m->plane         = 0U;
    m->phase         = MIG_READ;
    return false;
}

/**
 * @brief MIG_FLUSH_JRN：等 Journal job 空闲并确认搬移条目已落盘（F10 屏障）。
 *
 * @param m job。
 *
 * @return true  —— 本轮到此为止（Journal 在飞 / 刷不动 / 等待超时）；
 *         false —— 屏障达成或无需屏障，进入 ERASE。
 */
static bool mig_flush_jrn_step(mig_handle_t *m)
{
    // Journal 持久性失败 pending（页写失败 ⇒ 条目已丢）：
    // 在 CP 确认当前 L2P 已落盘之前，任何 victim 都不许擦——包括本 job 没有搬移
    // 任何 CW（migrated == 0）的情况，因为上一次 job 的迁移映射可能只在 RAM 里。
    if (jrn_has_write_failure(m->journal))
    {
        mig_job_abort(m, "journal write failure pending"); /* [待上板] */
        return false;
    }

    // F10 契约：搬移条目先落盘再擦 victim。journal 是 write-back（page_buf
    // 攒满才 flush），不在此强制落盘，掉电落在"条目未落盘 + victim 已擦除"
    // 窗口 ⇒ 恢复回放指向已擦除块 = 数据损坏。
    // 无搬移条目（migrated == 0）⇒ 无需屏障，直接擦。
    if (m->migrated == 0U)
    {
        m->phase = MIG_ERASE;
        m->plane = 0U;
        return false;
    }

    {
        // typed 入口：job 空闲且有待写页 ⇒ 起异步刷页。
        jrn_handle(m->journal, NULL);
    }

    if (jrn_is_idle(m->journal) == false)
    {
        // 有命令在飞不计提交卡死，但计入独立的等待上限：Journal job 永久卡住时
        // job 否则永不中止（RISK-MIG-01）。
        (void)mig_job_wait(m, "journal flush timeout");
        return true;
    }
    if ((jrn_has_erase_pending(m->journal)) || (jrn_has_pending_entries(m->journal)))
    {
        // 想刷却刷不动（命令池空）⇒ 计卡死轮数；超限中止（**不擦 victim**）
        (void)mig_job_stall(m);
        return true;
    }

    m->stall_ms = 0U;
    m->wait_ms  = 0U; /* 屏障达成 */
    m->phase    = MIG_ERASE;
    m->plane    = 0U;
    return false;
}

/**
 * @brief MIG_ERASE：逐个仍归本 victim 的 plane 提交异步擦除。
 *
 * @param m job。
 *
 * @return true  —— 本轮到此为止（已提交 / 提交失败 / 原地等资源）；
 *         false —— 纯计算推进了一步（无剩余 plane ⇒ DONE）。
 */
static bool mig_erase_step(mig_handle_t *m)
{
    m->plane = mig_present_plane_next(m->alloc, m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        m->phase = MIG_DONE;
        return false;
    }

    const ftl_status_e ret = mig_erase_submit(m);
    if (ret == FTL_AGAIN)
    {
        (void)mig_job_stall(m);
    }
    else if (ret != FTL_OK)
    {
        mig_job_abort(m, "erase command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

/**
 * @brief 状态机主体——一次只提交**一条**命令；纯计算阶段连续推进到下一个提交阶段或终态。
 *
 * @param m migrate handle。
 *
 * @note 实现要点：提交失败（背压）⇒ 原地返回，由 mig_handle 下轮重试同一阶段（幂等）；
 * 连续超过 FTL_SUBMIT_TIMEOUT_MS 无进展 ⇒ 中止。永久错误立即中止。
 */
static void mig_job_step(mig_handle_t *m)
{
    for (;;)
    {
        if (m->waiting)
        {
            // 批次已失败但仍有命令在飞：必须等它们全部完成再收尾，否则新 job 会把
            // 迟到完成按新 phase 误解释。NAND 本身保证完成；若连完成都丢了，
            // 用硬 deadline 熔断 GC（paused），迟到的完成由孤儿守卫 release。
            if ((m->phase == MIG_WRITE_BATCH) && m->batch_failed)
            {
                if (mig_deadline_is_expired(&m->cmd_wait_ms, FTL_WAIT_TIMEOUT_MS))
                {
                    m->paused        = true;
                    m->timed_out     = true;
                    m->timeout_phase = m->phase;
                    m->waiting       = false;
                    mig_job_abort(m, "cache batch drain timeout");
                    mig_job_step(m);
                }
                return;
            }

            // 完成丢失（例如 FC CQ 满时 CQ entry 被丢）时 waiting 永真，GC/WL 永久卡死。
            // 在飞也计时：超时后放弃本 job；若迟到的完成随后到达，回调的
            // (!active || !waiting) 分支会 release 命令。
            if (mig_deadline_is_expired(&m->cmd_wait_ms, FTL_WAIT_TIMEOUT_MS))
            {
                // 写命令可能已部分编程：超时后按“已提交”处置，关闭 Cold SB。
                if ((m->phase == MIG_WRITE) || (m->phase == MIG_WRITE_BATCH))
                {
                    mig_dst_fail_cleanup(m, true);
                }
                m->timed_out     = true;
                m->timeout_phase = m->phase;
                m->waiting       = false;
                mig_job_abort(m, "completion timeout");
            }
            return;
        }

        bool stop;

        switch (m->phase)
        {
            case MIG_READ:
                stop = mig_read_step(m);
                break;
            case MIG_READ_SETTLE:
                stop = mig_read_settle_step(m);
                break;
            case MIG_WRITE:
                stop = mig_write_step(m);
                break;
            case MIG_WRITE_SETTLE:
                stop = mig_write_settle_step(m);
                break;
            case MIG_WRITE_BATCH:
                stop = mig_write_batch_step(m);
                break;
            case MIG_WRITE_SETTLE_BATCH:
                stop = mig_write_settle_batch_step(m);
                break;
            case MIG_FLUSH_JRN:
                stop = mig_flush_jrn_step(m);
                break;
            case MIG_ERASE:
                stop = mig_erase_step(m);
                break;
            case MIG_DONE:
                mig_job_finish(m);
                return;
            case MIG_IDLE:
            default:
                // 不该发生：active 置位而 phase 非法
                ASSERT(m->active == false);
                return;
        }

        if (stop)
        {
            return;
        }
    }
}

/* ── 6. typed handle（端口装配 / 触发 / 自持节拍 / 上报）── */

// 装配：整块清零、绑定 alloc/journal/cache 端口、失败块表置无效；不碰介质。
void mig_init(mig_handle_t *m, alc_t *alloc, l2p_jrn_t *journal, l2p_cache_t *cache, const DWORD *total_writes)
{
    // 前置条件：ftl_init 调本函数前已由 contract_check_handles 证明各容器非空
    // （cache / journal / alloc / total_writes 均为实例内嵌对象的地址）⇒ 不重复检查。
    memset(m, 0, sizeof(*m));
    m->alloc        = alloc;
    m->journal      = journal;
    m->cache        = cache;
    m->total_writes = total_writes;
    m->topo         = alc_topo(alloc);
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        m->fail_gblock[plane] = UINT16_MAX;
    }
    m->error = FTL_OK;
}

// 置紧急 GC 请求位（wb_flush 取不到页对时调用）：只排队，下一轮求值才真正触发。
void mig_gc_request_urgent(mig_handle_t *m)
{
    // 句柄由调用者持有：生产路径恒为 &ftl->mig_*；wb_flush 的可选端口分支已在
    // 调用点判空，这里不重复返回码（H2）。NULL 写进来会落到 urgent_gc 的偏移
    // （本结构该字段偏移 0x4220）⇒ 直接踩 ITCM 代码，必须当场停。
    ASSERT(m != NULL);
    m->urgent_gc = true;
}

/**
 * @brief GC 求值：命中时返回 GC_TRIGGER_* 位掩码并打观测日志。
 *
 * 调用节拍由 timer 表的 10 ms 周期提供（GC_EVAL_INTERVAL_MS）；本函数不再自持节拍。
 *
 * @param m            job。
 * @param now_ms       当前时刻（0 = 无时钟）。
 * @param host_writing 出参：本轮的 host 写流判定。
 *
 * @return 非 0 —— gc_should_trigger 的触发位掩码（本轮可以尝试 GC）；
 *         0    —— 无触发 / 处于 no-gain 退避。
 */
static DWORD mig_gc_evaluate(mig_handle_t *m, DWORD now_ms, bool *host_writing)
{
    // host 写流判定（2 s 窗口，见 gc.h GC_HOST_WRITE_WINDOW_MS）。
    // 时钟缺失（now_ms == 0）按「在写」处理 ⇒ 收益判据 OFF，绝不卡写。
    const DWORD writes = *m->total_writes;

    if (writes != m->gc_writes_last)
    {
        m->gc_writes_last   = writes;
        m->gc_write_last_ms = now_ms;
    }
    *host_writing   = (now_ms == 0U) || ((m->gc_write_last_ms != 0U) && ((now_ms - m->gc_write_last_ms) < GC_HOST_WRITE_WINDOW_MS));
    m->host_writing = *host_writing;

    const DWORD triggers = gc_should_trigger(m->alloc);

    if (triggers == 0U)
    {
        m->gc_log_last_ms = 0U; /* 收敛：下次进入触发态立即打 */
        m->gc_nogain      = false;
        return 0U;
    }

    // NO_GAIN 退避：无新写入、非紧急时，避免每 10 ms 全表扫 closed list。
    const gc_benefit_mode_e mode = mig_gc_mode_select(triggers, *host_writing);

    if (mig_gc_scan_is_allowed(m, mode, triggers, now_ms) == false)
    {
        return 0U;
    }

    if ((m->gc_log_last_ms == 0U) || ((now_ms - m->gc_log_last_ms) >= GC_LOG_INTERVAL_MS))
    {
        m->gc_log_last_ms = now_ms;
        LOGD(LOG_TAG_GC "trig=0x%02lx free=%hu op=%hu/%hu writes=%lu reclaimed=%lu novictim=%lu nogain=%lu mode=%s hw=%u minvr=%hu\r\n", triggers, m->alloc->free_count, alc_op_get_count(m->alloc, 0U), alc_op_get_count(m->alloc, 1U), *m->total_writes, m->reclaimed_count, m->novictim_count, m->nogain_count, mig_gc_mode_name(m->gc_last_mode), (*host_writing ? 1U : 0U), m->gc_best_valid_pct);
    }

    return triggers;
}

/**
 * @brief GC 会话进展：只在会话活跃且距上次 >=5 s 时打；now_ms==0（无时钟）时不打。
 *
 * @param m      job。
 * @param now_ms 当前时刻（0 = 无时钟，不打日志）。
 */
static void mig_gc_session_progress(mig_handle_t *m, DWORD now_ms)
{
    if ((m->gc_session_active == false) || (now_ms == 0U) || (m->gc_session_moved == 0U))
    {
        return;
    }
    if ((m->gc_session_log_ms != 0U) && ((now_ms - m->gc_session_log_ms) < FTL_JOB_LOG_SLOW_INTERVAL_MS))
    {
        return;
    }

    m->gc_session_log_ms = now_ms;
    LOGI(LOG_TAG_GC "progress jobs=%lu moved=%lu rec=%lu noop=%lu abort=%lu free=%hu\r\n", m->gc_session_jobs, m->gc_session_moved, m->gc_session_reclaimed, m->gc_session_noop, m->gc_session_aborted, m->alloc->free_count);
}

/**
 * @brief GC 会话收尾：连续 GC_SESSION_IDLE_MS 没有 GC job 活动。
 *
 * @param m      job。
 * @param now_ms 当前时刻（0 = 无时钟，不收尾）。
 */
static void mig_gc_session_end(mig_handle_t *m, DWORD now_ms)
{
    if ((m->gc_session_active == false) || (now_ms == 0U))
    {
        return;
    }
    if ((now_ms - m->gc_session_last_ms) < GC_SESSION_IDLE_MS)
    {
        return;
    }

    // 只有真正发生了数据搬移的会话才打 end；纯空块/无搬移会话由 diag/nop 统计。
    if (m->gc_session_moved != 0U)
    {
        LOGI(LOG_TAG_GC "end jobs=%lu moved=%lu rec=%lu noop=%lu abort=%lu elapsed=%lums\r\n", m->gc_session_jobs, m->gc_session_moved, m->gc_session_reclaimed, m->gc_session_noop, m->gc_session_aborted, ftl_job_age_ms(&m->gc_session_start_ms));
    }

    m->gc_session_active    = false;
    m->gc_session_start_ms  = 0U;
    m->gc_session_last_ms   = 0U;
    m->gc_session_log_ms    = 0U;
    m->gc_session_jobs      = 0U;
    m->gc_session_moved     = 0U;
    m->gc_session_reclaimed = 0U;
    m->gc_session_noop      = 0U;
    m->gc_session_aborted   = 0U;
}

// 每轮入口：只做在飞推进、会话收尾、紧急 GC。周期求值见 mig_eval（timer 表调用）。
void mig_handle(mig_handle_t *m, bg_level_e level)
{
    const DWORD now_ms = ftl_job_now_ms();

    // 发布本轮档位：mig_eval 由 timer 表在 ftl_service 之后调用，从 handle 读取。
    m->level = level;

    // ① 在飞：只服务当前 job（提交即返回）。
    if (m->active)
    {
        // 心跳（见 ftl_job.h）：搬移最长（64 页 × 2 plane），没有它卡死不可见。
        // ⚠ 只报「真的卡住」：单个 victim 正常 1 s 内完成，逐 victim 的 1 s 心跳在持续
        // GC 时会变成 1 行/秒。job age ≥ SLOW_AFTER 才打；之后 helper 自动降到 5 s。
        if (ftl_job_heartbeat(&m->log_ms, &m->start_ms) && (ftl_job_age_ms(&m->start_ms) >= FTL_JOB_LOG_SLOW_AFTER_MS))
        {
            LOGI(LOG_TAG_MIG "job kind=%d phase=%d page=%hhu plane=%hhu aborted=%d age=%lums\r\n", m->kind, m->phase, m->page, m->plane, m->aborted, ftl_job_age_ms(&m->start_ms));
        }

        mig_job_step(m);
        mig_gc_session_progress(m, now_ms);

        return;
    }

    // 会话收尾必须在 paused 早退之前：熔断后仍要留下本次会话的结束行。
    mig_gc_session_end(m, now_ms);
    mig_gc_session_progress(m, now_ms);

    // ② 系统性命中熔断：不再启动任何 GC/WL/predict（重启才清）。
    if (m->paused)
    {
        return;
    }

    // ③ 紧急 GC：写路径分配不到页对（wbuf 调 mig_gc_request_urgent）时，必须在 BUSY 档也处理。
    // 否则 host 持续写 → load_level 恒 BUSY → 本函数每次早退 → urgent_gc 永不被消费
    // ⇒ free_count 卡在目标线附近、写请求永久 FTL_AGAIN（CDM "Preparing" 卡住的现场）。
    // BUSY 只让**周期评估**（T1/T4/WL/predict）让路，不让“写不进去”的急救让路。
    if (m->urgent_gc)
    {
        m->urgent_gc = false;

        // free 仍高于紧急线时按 OFF（不筛），因为 urgent 的语义是「写不进去」，
        // 不是「空闲补池」。
        const gc_benefit_mode_e mode = (m->alloc->free_count <= FREE_POOL_EMERGENCY) ? GC_BENEFIT_EMERGENCY : GC_BENEFIT_OFF;
        (void)mig_gc_begin(m, mode, now_ms, MIG_GC_START_URGENT, 0U);

        return;
    }

    // 平台时钟完全缺失时 timer 任务永不到期：退回每轮求值，保证 GC 不停摆
    // （与旧节拍「无时钟 ⇒ 每轮求值」的取舍一致）。
    if (now_ms == 0U)
    {
        mig_eval(m);
    }
}

// 周期求值（timer 表每 GC_EVAL_INTERVAL_MS 唤醒）：GC → WL 5s → predict 1s；每轮最多一个 job。
// ⚠ 时钟分配：timer 的 now_ms（TIM6）只是唤醒节拍，不参与 FTL 时间戳；本函数内所有
//   deadline/interval 一律读 ftl_job_now_ms()（SysTick），与 mig_handle / mig_job_* 同源。
void mig_eval(mig_handle_t *m)
{
    const DWORD now_ms = ftl_job_now_ms();
    // 与 mig_handle 的早退条件一致：在飞 / 熔断 / 紧急 GC 时不启动新工作。
    if (m->active || m->paused || m->urgent_gc)
    {
        return;
    }

    // ④ host 饱和档：不跑 WL/predict，也不做普通周期求值；但 free 已跌破
    // GC 目标线时必须做水位 GC——否则 BUSY 会一直挡到 wbuf 分配失败（free≈1），
    // 那时已没有目的页可分配，只能 no-gain。目标 200 的水位因此形同虚设。
    if (m->level == BG_LEVEL_BUSY)
    {
        if (m->alloc->free_count < FREE_POOL_TARGET)
        {
            bool        host_writing = false;
            const DWORD trig         = mig_gc_evaluate(m, now_ms, &host_writing);

            if (trig != 0U)
            {
                const gc_benefit_mode_e mode = mig_gc_mode_select(trig, host_writing);

                (void)mig_gc_begin(m, mode, now_ms, MIG_GC_START_PERIODIC, trig);
            }
        }

        return;
    }

    // ⑤ 周期评估 GC → WL → predict；每轮最多启动一个 job（单飞行）。
    {
        bool        host_writing = false;
        const DWORD trig         = mig_gc_evaluate(m, now_ms, &host_writing);

        if (trig != 0U)
        {
            const gc_benefit_mode_e mode = mig_gc_mode_select(trig, host_writing);

            (void)mig_gc_begin(m, mode, now_ms, MIG_GC_START_PERIODIC, trig);
        }
        else if ((m->wl_eval_last_ms == 0U) || ((now_ms - m->wl_eval_last_ms) >= WL_EVAL_INTERVAL_MS))
        {
            m->wl_eval_last_ms = now_ms;

            // WL 是**后台维护**，只在「上次求值以来 host 没有新编程 + 当前空闲档」时启动。
            // USB BOT 是串行的：pending 在两条命令之间会掉到 0，仅看 load_level 会误判 IDLE
            // ⇒ WL 在 host 持续写盘时启动，并占住单飞行引擎数秒（实测单 job 4~7s），
            // 同时把命令槽让不出去、GC 也起不来。用 total_writes 的窗口增量判断 host 是否在写。
            const DWORD writes     = *m->total_writes;
            const bool  host_quiet = (writes == m->wl_last_writes);

            m->wl_last_writes = writes;

            if (host_quiet && (m->level == BG_LEVEL_IDLE))
            {
                (void)mig_wl_begin(m);
            }
        }
        else if ((m->level == BG_LEVEL_IDLE) && ((m->predict_last_ms == 0U) || ((now_ms - m->predict_last_ms) >= RR_SCAN_INTERVAL_MS)))
        {
            m->predict_last_ms = now_ms;
            (void)mig_predict_begin(m);
        }
    }
}

static void mig_eval_task(void *context, DWORD now_ms)
{
    (void)now_ms; /* TIM6 只负责唤醒；FTL 时间戳由 mig_eval 内部读 SysTick。 */
    mig_eval((mig_handle_t *)context);
}

bool mig_timer_register(mig_handle_t *m)
{
    ASSERT(m != NULL);

    if (m->timers_registered)
    {
        return true;
    }

    DWORD task_id;

    if (!timer_scheduler_add(mig_eval_task, m, 0U, GC_EVAL_INTERVAL_MS, &task_id))
    {
        return false;
    }

    m->timers_registered = true;
    return true;
}

/* ── 7. 完成服务（g_op_callbacks[FTL_OP_GC] / [FTL_OP_WL]）── */

/**
 * @brief MIG_READ 完成：拷数据 + 解 META 得逐 CW 的扇区 LBA；顺带记 ECC 趋势 + read_count
 * （预测性退役闭环）。失败 / 全页空洞 ⇒ 该 plane 无有效 CW，不影响另一 plane。
 *
 * @param m          job。
 * @param cmd        完成的命令（本函数负责 release）。
 * @param completion FC 完成状态。
 */
static void mig_read_complete(mig_handle_t *m, fc_command_t *cmd, const fc_command_status_t *completion)
{
    const BYTE plane = m->plane;

    m->decode_ok = false;

    // 读失败与“空页”必须分开：前者页上可能仍有有效数据，擦 victim 会永久丢数据；
    // 后者（无错误 + META 全 FF）只是从未写过的 CW，跳过本页即可。
    // ⚠ 失败原因必须保留到日志：FC 状态错 / 短链读 / META 页基址损坏是三种不同的
    // 故障源，合并成一个 read_ok=false 会让 mark BAD 无法归因（RISK-MIG-06）。
    bool        read_ok = true;
    const char *cause   = "";

    m->last_fc_status = completion->status; /* 诊断：abort 日志带上原始状态位 */

    if (fc_cmd_status_has_error(completion->status))
    {
        read_ok = false;
        cause   = "fc_err";
    }
    else if (ftl_cmd_read_data(cmd, m->page_buf[plane], PAGE_MAIN_SIZE) != FTL_OK)
    {
        read_ok = false;
        cause   = "short_chain";
    }

    if (read_ok)
    {
        const gc_decode_e dr = gc_decode_cw_lba(cmd, m->src[plane], m->cw_lba[plane], &m->base[plane]);

        if (dr == GC_DECODE_OK)
        {
            const WORD gsrc = ftl_gblock(plane, m->victim.plane_blocks[plane]);
            const BYTE ecc  = fc_get_cmd_ecc_max(cmd);

            // B4：搬移读记录 ECC 趋势 + read disturb 计数（预测性退役闭环）。
            // 判定只用 ecc_bits 与上一次读数比较，不涉及时间维度。
            // 趋势/read_count 的写入点是 RR owner API，migrate 不直接摸 BST。
            rr_record_block_ecc(m->alloc, gsrc, ecc);

            m->decode_ok = true;
        }
        else if (dr == GC_DECODE_CORRUPT)
        {
            // 页内 META 页基址不一致：不能只跳过部分 CW（它们的 L2P 仍指向本页，
            // 随后擦 victim 会悬空）。整页按读失败中止。
            read_ok = false;
            cause   = "decode_corrupt";
        }
        // GC_DECODE_EMPTY：空页，decode_ok 保持 false，SETTLE 跳过本页。
    }

    // 逐 CW 反查 L2P 改在 MIG_READ_SETTLE（纯计算阶段，可挂起等 region 加载）
    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;

    if (read_ok == false)
    {
        // [待上板] 读错误 ⇒ 中止，victim 不擦。空页（decode false）不在此列。
        // 同一源块连续失败达上限 ⇒ 标 BAD 并摘除其 Closed plane（RISK-GC-01）：
        // 否则每轮 GC 都重选同一 victim、永久 abort，容量不再增长。
        // 其他 plane 仍可迁移；数据不因标坏而丢失（坏块本来就读不回来）。
        // ⚠ 计数按 **plane 的源块**分开：同一 victim 的两个 plane 块号不同，共用
        // 一个槽位会让交替失败互相清零。
        const WORD gsrc = ftl_gblock(plane, m->victim.plane_blocks[plane]);

        if (m->fail_gblock[plane] != gsrc)
        {
            m->fail_gblock[plane] = gsrc;
            m->fail_count[plane]  = 0U;
        }
        m->fail_count[plane]++;

        if (m->fail_count[plane] >= MIG_VICTIM_READ_FAIL_MAX)
        {
            // gblk 是全局块号（plane*BLOCKS_PER_PLANE+block）；与 abort 行的
            // g0/g1 同一坐标（RISK-MIG-06）。
            LOGW(LOG_TAG_MIG "victim read failed gblk=%hu x%hhu cause=%s — mark BAD\r\n", gsrc, m->fail_count[plane], cause);
            (void)bbt_mark_bad(m->alloc, gsrc);
            if (alc_super_closed_drop_plane(m->alloc, plane, m->victim.plane_blocks[plane]))
            {
                m->closed_removed = true;
            }
            m->fail_count[plane]  = 0U;
            m->fail_gblock[plane] = UINT16_MAX;

            // 系统性读失败熔断：连续多个不同 victim 都被标 BAD ⇒ 不是单块坏。
            // 继续 GC 会耗尽 Free/OP 并毁掉 BAD 图；停止后台搬移直到重启。
            if (m->read_fail_streak < UINT8_MAX)
            {
                m->read_fail_streak++;
            }
            if ((m->paused == false) && (m->read_fail_streak >= MIG_SYSTEMIC_READ_FAIL_MAX))
            {
                m->paused = true;
                LOGE(LOG_TAG_MIG "systemic victim read failures (%hhu consecutive) — suspend GC until reboot\r\n", m->read_fail_streak);
            }
        }

        mig_job_abort(m, cause); /* 三种原因直传日志，见 RISK-MIG-06 */
    }
    else
    {
        // 单页读成功**不**清零失败计数（RISK-GC-07）：坏页之前的页每轮都会成功读一遍，
        // 若按单页成功清零，第 2 次重试永远从 0 开始 ⇒ MIG_VICTIM_READ_FAIL_MAX 不可达。
        // 计数只在整个 job 完成（mig_job_finish）或该块标 BAD 时清。
        m->phase = MIG_READ_SETTLE;
    }

    mig_job_step(m);
}

/**
 * @brief MIG_WRITE 完成：只记成败，L2P/Journal/有效计数的改动在 MIG_WRITE_SETTLE。
 * 写失败 ⇒ **中止 job，victim 不擦**（源数据仍有效，下轮再搬）；绝不能继续到 ERASE。
 *
 * @param m          job。
 * @param cmd        完成的命令（本函数负责 release）。
 * @param completion FC 完成状态。
 */
static void mig_write_complete(mig_handle_t *m, fc_command_t *cmd, const fc_command_status_t *completion)
{
    const bool write_ok = (fc_cmd_status_is_success(completion->status));

    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;

    if (m->dummy_page0)
    {
        m->dummy_page0 = false;

        if (write_ok == false)
        {
            mig_dst_fail_cleanup(m, true);
            mig_job_abort(m, "page0 pad write failed");
            mig_job_step(m);
            return;
        }

        alc_super_open_note_page_programmed(m->alloc, FTL_TEMP_COLD, m->dummy_pba);
        m->phase = MIG_WRITE; /* 重试原 page>0 的写 */
        mig_job_step(m);
        return;
    }

    if (write_ok == false)
    {
        // [待上板] 写错误 ⇒ 中止，victim 不擦。
        // 命令已提交，目的页可能已半编程 ⇒ 不能 rewind；立即关闭 Cold SB
        // （RISK-GC-02）。关闭后失败页留在 SB 尾部，下一次 GC 换新的目的块；
        // 目的块可能已有成功迁移的页，**不能**标 BAD，旧块的已写页仍可读。
        mig_dst_fail_cleanup(m, true);
        mig_job_abort(m, "write failed");
        mig_job_step(m);
        return;
    }

    // page0 异步写屏障：只记**本 plane**；另一 plane 可能没有数据、page0 仍未编程。
    alc_super_open_note_page_programmed(m->alloc, FTL_TEMP_COLD, m->dst[m->plane]);

    m->write_ok = true;
    m->phase    = MIG_WRITE_SETTLE;
    mig_job_step(m);
}

/**
 * @brief MIG_WRITE_BATCH 完成：逐条 release，只有全部成功才进入两遍式确认。
 * 任一失败 ⇒ 立即关闭 cold SB 并 abort（victim 不擦）；其余在飞命令由孤儿守卫 release。
 * 没有映射被提交，因此未确认的 batch 页只是垃圾。
 *
 * @param m          job。
 * @param cmd        完成的命令（本函数负责 release）。
 * @param completion FC 完成状态。
 */
static void mig_write_batch_complete(mig_handle_t *m, fc_command_t *cmd, const fc_command_status_t *completion)
{
    (void)fc_command_release(cmd);

    m->last_fc_status = completion->status;

    if (fc_cmd_status_has_error(completion->status))
    {
        m->batch_failed = true;
    }

    if (m->batch_outstanding != 0U)
    {
        m->batch_outstanding--;
    }

    if (m->batch_failed)
    {
        if (m->batch_cleanup_done == false)
        {
            (void)alc_super_close(m->alloc, FTL_TEMP_COLD);
            m->batch_aborts++;
            m->batch_cleanup_done = true;
        }

        // 保持 active/waiting，等在飞命令全部完成后再收尾：否则新 job 启动后，
        // 迟到的完成会被当成新 job 的完成（phase 误解释）。
        if (m->batch_outstanding == 0U)
        {
            m->batch_count = 0U;
            m->batch_mode  = false;
            m->waiting     = false;
            mig_job_abort(m, "cache batch write failed");
            mig_job_step(m);
        }
        return;
    }

    if (m->batch_outstanding == 0U)
    {
        m->waiting = false;
        m->phase   = MIG_WRITE_SETTLE_BATCH;
        mig_job_step(m);
    }
}

/**
 * @brief MIG_ERASE 完成：成功 ⇒ erase_count++ / last_pe_ts / 清 valid，
 * 然后按 post 二选一（FREE 入池 / RETIRE 退役）；失败 ⇒ bbt_mark_bad。
 *
 * @param m          job。
 * @param cmd        完成的命令（本函数负责 release）。
 * @param completion FC 完成状态。
 */
static void mig_erase_complete(mig_handle_t *m, fc_command_t *cmd, const fc_command_status_t *completion)
{
    const BYTE  plane        = m->plane;
    const WORD  gsrc         = ftl_gblock(plane, m->victim.plane_blocks[plane]);
    const DWORD total_writes = *m->total_writes;

    if (fc_cmd_status_is_success(completion->status))
    {
        m->erased++;
        if (ftl_block_is_bad(&m->alloc->blocks[gsrc]))
        {
            // 迁移期间被 RR/BBT 标 BAD：擦成功也不能复活或入池。
            m->erase_failed = true;
            LOGW(LOG_TAG_MIG "erase ok but block %hu became BAD — not freed/retired\r\n", gsrc);
        }
        else
        {
            // ⚠ 不在这里清 `read_fail_streak`：擦除是**逐 plane** 的，一个 victim
            // 常常一侧 plane 健康（读完→擦成功）另一侧读失败；在此清零会让熔断
            // 计数每个 job 都归零，阈值永不触发（RISK-GC-10）。清零只在
            // mig_job_finish 的「整块迁移成功」分支。

            // 擦除成功：统一走 allocator 的擦除记账（含 !bad 断言 + 清 valid）。
            alc_bst_note_erase(m->alloc, gsrc, total_writes);

            // 去向二选一——**退役块不得先进池**：
            // 退役直接 RETIRED，与「state == FREE ⟺ 在池」不变量一致（allocator.h ②）。
            if (m->post == MIG_POST_RETIRE)
            {
                // 预测性退役：搬空的块**退役**（RETIRED，不再分配新数据、不标坏、仍可读）。
                // GC / 静态 WL 走 FREE 分支，块重新进入磨损轮转。
                // victim 正常是 CLOSED、从未入池 ⇒ remove_from_free 是文档化 no-op
                // （allocator.h 判据 4）；保留它是为了让「退役块绝不留在池里」不依赖调用顺序。
                // rr_retire_block 内部把 read_count 归零（read disturb 重计）。
                alc_free_remove(m->alloc, gsrc);
                rr_retire_block_at(m->alloc, gsrc);
            }
            else
            {
                alc_free_push(m->alloc, gsrc);
            }
        }
    }
    else
    {
        m->erase_failed = true;

        // 擦除失败：标记坏块（BBT 单数据源，后台持久化）。
        // ⚠ 已 BAD 时不再重复标记：bbt_mark_bad 会再次 bad_count++ 并从 Spare 补池。
        // ⚠ 必须逐块打一行：否则运行期 BAD 只留下游的 `[FTL] dangling`，无法归因到
        // 「GC 源块擦失败」还是「host 写/读失败」。
        if (ftl_block_is_bad(&m->alloc->blocks[gsrc]) == false)
        {
            LOGW(LOG_TAG_MIG "erase failed gblk=%hu — mark BAD\r\n", gsrc);
            (void)bbt_mark_bad(m->alloc, gsrc);
        }
        else
        {
            LOGW(LOG_TAG_MIG "erase failed on already-BAD block %hu — not re-marked\r\n", gsrc);
        }
    }

    // 逐 plane 摘除已处理的块：擦完后该块已归还 Free Pool（或已标坏），旧 Closed 条目
    // 不得继续引用它——否则块被重新分配并再次 Closed 后会被旧条目误认（RISK-GC-04）。
    // 条目在最后一个 plane 摘除时自动移除。
    if (alc_super_closed_drop_plane(m->alloc, plane, m->victim.plane_blocks[plane]))
    {
        m->closed_removed = true;
    }

    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;
    m->plane    = (plane + 1U);
    mig_job_step(m);
}

// FC 完成派发：按当前 phase 结算搬移命令再步进 job；ctx 为空 = 迟到完成，只释放命令不解引用。
void ftl_migrate_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx)
{
    // 形参由完成派发签名固定；release 下 ASSERT 裁空 ⇒ 显式丢弃（H30）。
    (void)ftl;
    mig_handle_t *m = (mig_handle_t *)ctx;

    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);

    // 旁表项缺失（ctx == NULL）= 重复/迟到的完成：只 release 命令，不解引用 m。
    // 对齐 read_recovery.c 的同族守卫；release 下 ASSERT 裁空，必须先判空。
    if (m == NULL)
    {
        LOGE(LOG_TAG_MIG "late completion cmd=%hu status=0x%02hx — released\r\n", completion->command_id, completion->status);
        (void)fc_command_release(cmd);
        return;
    }

    // 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出来的归属实例，
    // 二者必须一致——不一致说明派发路由错了。
    ASSERT(ftl->mig_handle == m);

    // 孤儿 / 重复完成：job 已收尾或当前没有在飞命令 ⇒ 只释放命令，不改状态。
    // 否则迟到的完成会按新 job 的 phase 被误解释（同 read_recovery.c 的守卫）。
    if ((m->active == false) || (m->waiting == false))
    {
        LOGW(LOG_TAG_MIG "orphan completion phase=%d active=%d waiting=%d — ignored\r\n", m->phase, m->active, m->waiting);
        (void)fc_command_release(cmd);
        return;
    }

    switch (m->phase)
    {
        case MIG_READ:
            mig_read_complete(m, cmd, completion);
            return;

        case MIG_WRITE:
            mig_write_complete(m, cmd, completion);
            return;

        case MIG_WRITE_BATCH:
            mig_write_batch_complete(m, cmd, completion);
            return;

        case MIG_ERASE:
            mig_erase_complete(m, cmd, completion);
            return;

        case MIG_IDLE:
        case MIG_READ_SETTLE:
        case MIG_WRITE_SETTLE:
        case MIG_FLUSH_JRN:
        case MIG_DONE:
        default:
            // 不该发生：一条命令对应一个提交阶段（READ / WRITE / ERASE）。
            LOGE(LOG_TAG_MIG "unexpected completion phase=%d\r\n", m->phase);
            (void)fc_command_release(cmd);
            m->waiting = false;
            mig_job_abort(m, "unexpected completion");
            mig_job_step(m);
            return;
    }
}
