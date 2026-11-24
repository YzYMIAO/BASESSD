/*
 * ftl_diag.c — FTL 诊断 / 统计 / 初始化审计
 *
 * 本文件只放**不参与正常数据面**的代码：
 *   · stats 表与注册（SSD_STATS_ENABLE）
 *   · debug-only 初始化契约审计（!NDEBUG；release 空实现）
 *   · CLI `ftl_ctx` / 断言终端的现场 dump
 *
 * 条件编译集中在这里：ftl.c 不再散落 SSD_STATS_ENABLE / !NDEBUG 大块。
 */

#include "assertion.h"
#include "ftl.h"
#include "ftl_geometry.h"
#include "ftl_op.h"
#include "log.h"
#include "metadata_job.h"
#include "ssd_compiler.h"
#include "stats.h"

#include <string.h>

#if SSD_STATS_ENABLE
#    define FTL_STATS_LIST(X)                                                                                      \
        X(total_writes, STATS_U32, STATS_GAUGE) /* 功能量：mig 用它判 host 写流，--clear 不得清零 */ \
        X(dangling_total, STATS_U32, STATS_COUNTER)                                                                \
        X(deferred_taken, STATS_U32, STATS_COUNTER)

#    define FTL_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(ftl_stats_s, field), width, kind},
static const stats_name_s s_stats_ftl_names[] = {
    FTL_STATS_LIST(FTL_STATS_NAME)};
#    undef FTL_STATS_NAME
#    define FTL_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(ftl_stats_s, field, width);
FTL_STATS_LIST(FTL_STATS_CHECK)
#    undef FTL_STATS_CHECK

#    define FTL_ALLOC_STATS_LIST(X)           \
        X(free_count, STATS_U16, STATS_GAUGE) \
        X(bad_count, STATS_U16, STATS_GAUGE)  \
        X(closed_sb_count, STATS_U16, STATS_GAUGE)

#    define FTL_ALLOC_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(alc_s, field), width, kind},
static const stats_name_s s_stats_alloc_names[] = {
    FTL_ALLOC_STATS_LIST(FTL_ALLOC_STATS_NAME)};
#    undef FTL_ALLOC_STATS_NAME
#    define FTL_ALLOC_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(alc_s, field, width);
FTL_ALLOC_STATS_LIST(FTL_ALLOC_STATS_CHECK)
#    undef FTL_ALLOC_STATS_CHECK

#    define FTL_STREAM_STATS_LIST(X)                       \
        X(prefetch_run, STATS_U32, STATS_COUNTER)          \
        X(prefetch_run_max, STATS_U32, STATS_GAUGE)        \
        X(read_batches, STATS_U32, STATS_COUNTER)          \
        X(read_pairs, STATS_U32, STATS_COUNTER)            \
        X(read_starts, STATS_U32, STATS_COUNTER)           \
        X(read_declines, STATS_U32, STATS_COUNTER)         \
        X(read_fallbacks, STATS_U32, STATS_COUNTER)        \
        X(seq_breaks, STATS_U32, STATS_COUNTER)            \
        X(wb_batches, STATS_U32, STATS_COUNTER)            \
        X(wb_pages, STATS_U32, STATS_COUNTER)              \
        X(wb_declines, STATS_U32, STATS_COUNTER)           \
        X(wb_aborts, STATS_U32, STATS_COUNTER)             \
        X(wb_starts, STATS_U32, STATS_COUNTER)             \
        X(wb_page_ends, STATS_U32, STATS_COUNTER)          \
        X(wb_pad_closes, STATS_U32, STATS_COUNTER)         \
        X(demand_window_open, STATS_U32, STATS_COUNTER)    \
        X(demand_window_blocked, STATS_U32, STATS_COUNTER) \
        X(demand_window_expired, STATS_U32, STATS_COUNTER)

#    define FTL_STREAM_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(ftl_cache_stream_s, field), width, kind},
static const stats_name_s s_stats_stream_names[] = {
    FTL_STREAM_STATS_LIST(FTL_STREAM_STATS_NAME)};
#    undef FTL_STREAM_STATS_NAME
#    define FTL_STREAM_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(ftl_cache_stream_s, field, width);
FTL_STREAM_STATS_LIST(FTL_STREAM_STATS_CHECK)
#    undef FTL_STREAM_STATS_CHECK

#    define FTL_JRN_STATS_LIST(X)                       \
        X(sequence_counter, STATS_U32, STATS_GAUGE)     \
        X(page_writes, STATS_U32, STATS_COUNTER)        \
        X(persisted_seq, STATS_U32, STATS_GAUGE)        \
        X(write_page_seq, STATS_U32, STATS_GAUGE)       \
        X(write_entry_count, STATS_U16, STATS_GAUGE)    \
        X(current_block, STATS_U8, STATS_GAUGE)         \
        X(current_page, STATS_U8, STATS_GAUGE)          \
        X(current_entry, STATS_U16, STATS_GAUGE)        \
        X(ring_full, STATS_U8, STATS_GAUGE)             \
        X(erase_fail_count, STATS_U8, STATS_GAUGE)      \
        X(erase_kick_blocked, STATS_U32, STATS_COUNTER) \
        X(write_fail_count, STATS_U32, STATS_GAUGE)

#    define FTL_JRN_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(jrn_state_s, field), width, kind},
static const stats_name_s s_stats_jrn_names[] = {
    FTL_JRN_STATS_LIST(FTL_JRN_STATS_NAME)};
#    undef FTL_JRN_STATS_NAME
#    define FTL_JRN_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(jrn_state_s, field, width);
FTL_JRN_STATS_LIST(FTL_JRN_STATS_CHECK)
#    undef FTL_JRN_STATS_CHECK

static const stats_name_s s_stats_gc_state_names[] = {
    {"paused", STATS_OFFSET_NESTED(mig_state_s, failure, mig_failure_s, paused), STATS_U8, STATS_GAUGE},
    {"read_fail_streak", STATS_OFFSET_NESTED(mig_state_s, failure, mig_failure_s, read_fail_streak), STATS_U8, STATS_GAUGE},
    {"inflight", STATS_OFFSET(mig_state_s, inflight), STATS_U16, STATS_GAUGE},
    {"last_mode", STATS_OFFSET_NESTED(mig_state_s, benefit, mig_gc_benefit_s, last_mode), STATS_U8, STATS_GAUGE},
    {"urgent_gc", STATS_OFFSET(mig_state_s, urgent_gc), STATS_U8, STATS_GAUGE},
    {"batch_disabled", STATS_OFFSET(mig_state_s, batch_disabled), STATS_U8, STATS_GAUGE},
};
STATS_CHECK_FIELD(mig_state_s, inflight, STATS_U16);
STATS_CHECK_FIELD(mig_state_s, urgent_gc, STATS_U8);
STATS_CHECK_FIELD(mig_state_s, batch_disabled, STATS_U8);
STATS_CHECK_FIELD(mig_failure_s, paused, STATS_U8);
STATS_CHECK_FIELD(mig_failure_s, read_fail_streak, STATS_U8);
STATS_CHECK_FIELD(mig_gc_benefit_s, last_mode, STATS_U8);

static const stats_name_s s_stats_rr_names[] = {
    {STATS_NAME_STR(already_bad_reads), STATS_OFFSET(rr_handle_s, already_bad_reads), STATS_U32, STATS_COUNTER},
};
STATS_CHECK_FIELD(rr_handle_s, already_bad_reads, STATS_U32);
#endif /* SSD_STATS_ENABLE */
/* debug dump 的模块过滤位图。类型放文件前部，不夹在函数之间。 */
typedef struct FTL_DBG_FILTER_S
{
    BOOL all;
    BOOL alloc;
    BOOL pend;
    BOOL seq;
    BOOL cache;
    BOOL jrn;
    BOOL cp;
    BOOL mig;
    BOOL md;
    BOOL wb;
    BOOL sched;
} ftl_dbg_filter_s;

/* ── 初始化契约自检（debug-only）──
 *
 * 只在 ftl_init + 设备级 owner 登记完成后跑一次，证明“初始化后不可变”的绑定。
 * 覆盖：句柄、storage 数组、模块窄上下文、topology 副本、PBA 布局、bg 任务表、
 *       L2P owner / completion route 身份、各 job 的初始 idle 状态。
 *
 * ⚠ 边界（不要据此删运行期断言）：
 *   · 不覆盖运行期可变状态（phase 重启、计数、池成员、valid_count）；
 *   · 不覆盖每次调用的入参前置条件（out 指针、enum/index、cmd/desc）；
 *   · release（NDEBUG）下本组函数整体不存在，不能替代 release 拒绝（判据 9）。
 *   · 下方 helper 全部包在 #if !defined(NDEBUG) 内，避免 release 的 unused 告警。
 *   · 审计通过后，下游对同一绑定**不逐次复检**——这些绑定在生命周期内不可变。
 */
#if !defined(NDEBUG)
/**
 * @brief 校验后台调度器任务表已装载且处于初始 IDLE 档。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_bg(const ftl_ctx_s *ftl)
{
    ASSERT((ftl->bg.tasks != NULL) && (ftl->bg.task_count > 0U) && (ftl->bg.task_count <= BG_POLICY_TASK_COUNT) && (ftl->bg.cfg == NULL) && (ftl->bg.level == BG_LEVEL_IDLE));

    for (BYTE i = 0U; i < ftl->bg.task_count; i++)
    {
        ASSERT(ftl->bg.tasks[i].func != NULL);
    }
}

/**
 * @brief 校验开机零态：pending 空、计数为 0、无在飞 loading / flush。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_boot_zero(const ftl_ctx_s *ftl)
{
    ASSERT((pend_slot_get_count(&ftl->pending) == 0U) && (pend_group_get_count(&ftl->pending) == 0U) && (ftl->stats.total_writes == 0U) && (ftl->cache->inflight.load == 0U) && (ftl->cache->inflight.flush == 0U) && (ftl->flags.bg_suspend == false) && (ftl->flags.l2p_defer == false));
}

/**
 * @brief 每个 ASSERT 按模块聚合：一条断言覆盖同一生命周期的一组字段。
 * 失败时 file:line 定位到模块组；需要逐字段定位时临时拆分。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_handles(const ftl_ctx_s *ftl)
{
    ASSERT((ftl->cache != NULL) && (ftl->metadata_dir != NULL) && (ftl->journal != NULL) && (ftl->cp_handle != NULL) && (ftl->mig_handle != NULL) && (ftl->wbuf != NULL) && (ftl->rr_page_scratch != NULL));

    ASSERT((ftl->table_refs.blocks != NULL) && (ftl->table_refs.free_blocks != NULL) && (ftl->table_refs.op_blocks != NULL) && (ftl->table_refs.spare_blocks != NULL) && (ftl->table_refs.closed_sb_list != NULL));

    ASSERT((ftl->alloc.blocks == ftl->table_refs.blocks) && (ftl->alloc.free_blocks == ftl->table_refs.free_blocks) && (ftl->alloc.op_blocks == ftl->table_refs.op_blocks) && (ftl->alloc.spare_blocks == ftl->table_refs.spare_blocks) && (ftl->alloc.closed_sb_list == ftl->table_refs.closed_sb_list));
}

/**
 * @brief 校验 topology 副本与实例路由一致（各模块 init 时都拷了一份）。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_topology(const ftl_ctx_s *ftl)
{
    const ftl_topo_s *topo = &ftl->topo;
    const BYTE        die  = topo_2_die(*topo);

    ASSERT((topo->ch < CH_NUM) && (topo->ce < CE_NUM) && (topo->lun < LUN_NUM) && (die < DIE_MAX) && (die_2_ch(die) == topo->ch) && (die_2_ce(die) == topo->ce) && (die_2_lun(die) == topo->lun));

    ASSERT((ftl->alloc.ch == topo->ch) && (ftl->alloc.ce == topo->ce) && (ftl->alloc.lun == topo->lun) && (ftl->cache->ch == topo->ch) && (ftl->cache->ce == topo->ce));

    // 各模块在 init 时拷贝 topology；副本必须与实例路由一致。
    ASSERT((ftl->metadata_dir->topo.ch == topo->ch) && (ftl->metadata_dir->topo.ce == topo->ce) && (ftl->metadata_dir->topo.lun == topo->lun) && (ftl->journal->topo.ch == topo->ch) && (ftl->journal->topo.ce == topo->ce) && (ftl->journal->topo.lun == topo->lun));

    ASSERT((ftl->cp_handle->topo.ch == topo->ch) && (ftl->cp_handle->topo.ce == topo->ce) && (ftl->cp_handle->topo.lun == topo->lun) && (ftl->mig_handle->topo.ch == topo->ch) && (ftl->mig_handle->topo.ce == topo->ce) && (ftl->mig_handle->topo.lun == topo->lun) && (ftl->read_recovery_handle.topo.ch == topo->ch) && (ftl->read_recovery_handle.topo.ce == topo->ce) && (ftl->read_recovery_handle.topo.lun == topo->lun));
}

/**
 * @brief 校验各模块窄上下文绑定的指针与本实例账本一致。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_module_contexts(const ftl_ctx_s *ftl)
{
    const md_dir_cache_s *md = ftl->metadata_dir;

    ASSERT((md->alloc == &ftl->alloc) && (md->total_writes == &ftl->stats.total_writes));

    ASSERT(ftl->journal->alloc == &ftl->alloc);

    const cp_handle_s *cp = ftl->cp_handle;

    ASSERT((cp->sched == &ftl->sched) && (cp->cache == ftl->cache) && (cp->journal == ftl->journal) && (cp->metadata_dir == ftl->metadata_dir) && (cp->alloc == &ftl->alloc));

    const mig_handle_s *mg = ftl->mig_handle;

    ASSERT((mg->alloc == &ftl->alloc) && (mg->journal == ftl->journal) && (mg->cache == ftl->cache) && (mg->total_writes == &ftl->stats.total_writes));

    const rr_handle_s *rr = &ftl->read_recovery_handle;

    ASSERT((rr->cache == ftl->cache) && (rr->alloc == &ftl->alloc) && (rr->page_scratch == ftl->rr_page_scratch) && (rr->pending == &ftl->pending));

    const wb_handle_s *wb = ftl->wbuf;

    ASSERT((wb->alloc == &ftl->alloc) && (wb->pending == &ftl->pending) && (wb->migrate == ftl->mig_handle));
}

/**
 * @brief 校验本实例的 owner / route 身份（只查本实例拥有的 region）。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_identity(const ftl_ctx_s *ftl)
{
    const ftl_topo_s topo = ftl->topo;
    const BYTE       die  = topo_2_die(topo);

    ASSERT(ftl->cache->owner_of_region != NULL);
    ASSERT(ftl_op_route_get(topo.ch, topo.ce, topo.lun) == ftl);

    // 只查本实例拥有的 region：其他 LUN 的 owner 可能尚未 boot。
    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const DWORD        global = l2p_local_to_global_region(local, die);
        const l2p_owner_s *owner  = ftl->cache->owner_of_region(ftl->cache->owner_ctx, global);
        const QWORD        lba    = (QWORD)global * L2P_ENTRIES_PER_REGION;

        ASSERT((owner != NULL) && (owner->die == die) && (owner->metadata_dir == ftl->metadata_dir) && (owner->alloc == &ftl->alloc) && (owner->topo.ch == ftl->cache->ch) && (owner->topo.ce == ftl->cache->ce) && (owner->topo.lun == ftl->topo.lun) && (owner->sched == &ftl->sched) && (topo_2_die(owner->topo) == die));

        ASSERT((ftl_op_route_get_region(global) == ftl) && (lba_2_ch(lba) == ftl->cache->ch) && (lba_2_ce(lba) == ftl->cache->ce));
    }
}

/**
 * @brief 校验各异步 job（journal / checkpoint / metadata / migrate / RR / wbuf）初始 idle。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_jobs_idle(const ftl_ctx_s *ftl)
{
    const jrn_handle_s *j = &ftl->journal->handle;

    ASSERT((j->job.phase == JRN_IDLE) && (j->state.current_block < JOURNAL_BLOCK_COUNT) && (j->state.current_page < PAGES_PER_BLOCK) && (j->state.current_entry == 0U) && (j->state.sequence_counter == 1U) && (j->state.merge_floor == 0U) && (j->job.need_erase == false) && (j->state.checkpoint_needed == false) && (j->state.emergency == false) && (j->state.write_fail_count == 0U) && (j->state.write_fail_pending == false) && (j->state.pre_erased_block == JRN_BLOCK_NONE) && (j->state.bad_mask == 0U) && (j->state.erase_fail_count == 0U) && (j->state.ring_full == false));

    const cp_handle_s *cp = ftl->cp_handle;

    ASSERT((cp->active == false) && (cp->waiting == false) && (cp->failed == false) && (cp->phase == CP_IDLE) && (cp->cp_state.advance_next == false) && (cp->cp_state.cursor_valid == false));

    ASSERT((md_is_busy(ftl->metadata_dir) == false) && (ftl->metadata_dir->count <= METADATA_DIR_ENTRIES) && md_cursor_bounds_ok(ftl->metadata_dir));

    ASSERT((ftl->mig_handle->job.active == false) && (ftl->mig_handle->job.waiting == false) && (ftl->mig_handle->job.phase == MIG_IDLE) && mig_fail_state_is_clean(ftl->mig_handle));

    ASSERT((ftl->read_recovery_handle.stage == RR_STAGE_IDLE) && (ftl->read_recovery_handle.waiting == false) && (ftl->read_recovery_handle.has_owner == false));

    ASSERT((ftl->wbuf->open == false) && (ftl->wbuf->occupied == 0U) && (ftl->wbuf->error == FTL_OK));
}

#endif /* !NDEBUG */

/* 模块缩写过滤器：NULL / "all" = 全部；未知返回 false。 */
static BOOL ftl_dbg_filter_init(const char *module, ftl_dbg_filter_s *flt)
{
    if ((module == NULL) || (strcmp(module, "all") == 0))
    {
        memset(flt, 0, sizeof(*flt));
        flt->all = true;
        return true;
    }

    memset(flt, 0, sizeof(*flt));
    if (strcmp(module, "alloc") == 0)
    {
        flt->alloc = true;
    }
    else if (strcmp(module, "pend") == 0)
    {
        flt->pend = true;
    }
    else if (strcmp(module, "seq") == 0)
    {
        flt->seq = true;
    }
    else if (strcmp(module, "cache") == 0)
    {
        flt->cache = true;
    }
    else if (strcmp(module, "jrn") == 0)
    {
        flt->jrn = true;
    }
    else if (strcmp(module, "cp") == 0)
    {
        flt->cp = true;
    }
    else if (strcmp(module, "mig") == 0)
    {
        flt->mig = true;
    }
    else if (strcmp(module, "md") == 0)
    {
        flt->md = true;
    }
    else if (strcmp(module, "wb") == 0)
    {
        flt->wb = true;
    }
    else if (strcmp(module, "sched") == 0)
    {
        flt->sched = true;
    }
    else
    {
        return false;
    }
    return true;
}

/**
 * @brief 注册模块级 stats（mig / alloc / stream / journal / gc / rr）。
 *
 * 只在 SSD_STATS_ENABLE 下生效；release 为空实现。
 *
 * @param ftl 实例
 */
void ftl_stats_register_modules(ftl_ctx_s *ftl)
{
#if SSD_STATS_ENABLE
    {
        const BYTE inst = topo_2_die(ftl->topo);

        // 拓扑不在扫描表里（异常/测试合成拓扑）：本实例的组不注册，记日志后继续运行。
        if (inst == FTL_DIE_INVALID)
        {
            LOGE("[FTL ] stats: topo not in scan table; groups skipped\r\n");
        }
        else
        {
            mig_stats_register(ftl->mig_handle, inst);
            (void)stats_register("ftl.stats", inst, &ftl->stats, s_stats_ftl_names, STATS_ENTRY_COUNT(s_stats_ftl_names));
            (void)stats_register("ftl.alloc", inst, &ftl->alloc, s_stats_alloc_names, STATS_ENTRY_COUNT(s_stats_alloc_names));
            (void)stats_register("ftl.stream", inst, &ftl->cache_stream, s_stats_stream_names, STATS_ENTRY_COUNT(s_stats_stream_names));
            (void)stats_register("ftl.jrn", inst, &ftl->journal->handle.state, s_stats_jrn_names, STATS_ENTRY_COUNT(s_stats_jrn_names));
            (void)stats_register("ftl.gc.state", inst, &ftl->mig_handle->state, s_stats_gc_state_names, STATS_ENTRY_COUNT(s_stats_gc_state_names));
            (void)stats_register("ftl.rr", inst, &ftl->read_recovery_handle, s_stats_rr_names, STATS_ENTRY_COUNT(s_stats_rr_names));
        }
    }
#else
    (void)ftl;
#endif
}

/**
 * @brief 注册 scheduler stats（必须在 ftl_sched_init 之后调用）。
 *
 * @param ftl 实例
 */
void ftl_stats_register_sched(ftl_ctx_s *ftl)
{
#if SSD_STATS_ENABLE
    if (topo_2_die(ftl->topo) != FTL_DIE_INVALID)
    {
        ftl_sched_stats_register(&ftl->sched, topo_2_die(ftl->topo));
    }
#else
    (void)ftl;
#endif
}

SSD_CODE_FAST void ftl_contract_audit(const ftl_ctx_s *ftl)
{
#if defined(NDEBUG)
    (void)ftl; /* release：审计不存在，行为与今天一致 */
#else
    ASSERT(ftl != NULL);
    ASSERT(pba_fields_layout_matches());

    contract_check_handles(ftl);
    contract_check_topology(ftl);
    contract_check_module_contexts(ftl);
    contract_check_identity(ftl);
    contract_check_bg(ftl);
    contract_check_jobs_idle(ftl);
    contract_check_boot_zero(ftl);
#endif
}

/**
 * @brief 打印 FTL 各模块的关键现场（可按模块缩写过滤）。
 *
 * @param module NULL / "all" = 全部；alloc / pend / seq / cache / jrn / cp / mig / md / wb / sched。
 * @return true = 参数合法并已输出；false = 未知模块。
 *
 * @note 只读标量快照，不调用会断言的池扫描；CLI `ftl_ctx` 与断言终端共用。
 */
BOOL ftl_debug_dump(const char *module)
{
    ftl_dbg_filter_s flt;

    if (ftl_dbg_filter_init(module, &flt) == false)
    {
        return false;
    }

    LOGI_IF(flt.all, LOG_TAG_FTL "DBG req=%hu die_max=%hhu\r\n", ftl_op_req_count(), DIE_MAX);

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *f = ftl_instance_get(die);

        if ((f->cache == NULL) || (f->journal == NULL) || (f->metadata_dir == NULL))
        {
            continue;
        }

        LOGI_IF(flt.all || flt.alloc, LOG_TAG_FTL "DBG d=%hhu alloc free=%hu bad=%hu closed=%hu op=%hu/%hu spare=%hu/%hu hot=%d/%hhu/%hhu cold=%d/%hhu\r\n", die, f->alloc.free_count, f->alloc.bad_count, f->alloc.closed_sb_count, alc_op_get_count(&f->alloc, 0U), alc_op_get_count(&f->alloc, 1U), alc_spare_get_count(&f->alloc, 0U), alc_spare_get_count(&f->alloc, 1U), (int)f->alloc.hot_sb.is_open, f->alloc.hot_sb.next_page, f->alloc.hot_sb.page0_programmed, (int)f->alloc.cold_sb.is_open, f->alloc.cold_sb.next_page);
        if (flt.all || flt.pend)
        {
            LOGI(LOG_TAG_FTL "DBG d=%hhu pend slot=%hu group=%hu req=%hu\r\n",
                 die,
                 pend_slot_get_count(&f->pending),
                 pend_group_get_count(&f->pending),
                 ftl_op_req_count());

            for (BYTE gi = 0U; gi < (BYTE)FTL_GROUP_MAX; gi++)
            {
                const pend_group_s *g = &f->pending.groups[gi];

                if (g->used == false)
                {
                    continue;
                }
                LOGI(LOG_TAG_FTL "DBG d=%hhu g=%hhu op=%d lba=%lu next=%lu/%lu out=%hu stalls=%hhu agg=%d\r\n",
                     die,
                     gi,
                     (int)g->op,
                     (DWORD)g->lba,
                     g->next,
                     g->total,
                     g->outstanding,
                     g->stalls,
                     (int)g->agg_status);
            }
        }
        if (flt.all || flt.seq)
        {
            LOGI(LOG_TAG_FTL "DBG d=%hhu seq kind=%d act=%d drain=%d own=%hu next=%lu wr=%hhu/%hhu head=%hhu close=%d/%d\r\n",
                 die,
                 (int)f->cache_stream.seq.kind,
                 (int)f->cache_stream.seq.active,
                 (int)f->cache_stream.seq.draining,
                 f->cache_stream.seq.owner_gi,
                 f->cache_stream.seq.next_lba,
                 f->cache_stream.seq.wr.count,
                 f->cache_stream.seq.wr_inflight,
                 f->cache_stream.seq.wr.head,
                 (int)f->cache_stream.seq.close_requested,
                 (int)f->cache_stream.seq.closing);
            if (f->cache_stream.seq.wr.count > 0U)
            {
                const ftl_cache_page_s *hp = &f->cache_stream.seq.wr.page[f->cache_stream.seq.wr.head];
                LOGI(LOG_TAG_FTL "DBG d=%hhu seq head lba=%lu st=%hhu proven=%d cw=0x%02hhx\r\n",
                     die,
                     hp->lba,
                     hp->state,
                     (int)hp->proven,
                     hp->cw_bitmap);
            }
        }
        LOGI_IF(flt.all || flt.cache, LOG_TAG_FTL "DBG d=%hhu cache load=%hhu flush=%hhu cmt=%lu flags def=%d bg=%d l2p=%d md=%d\r\n", die, f->cache->inflight.load, f->cache->inflight.flush, f->cache->stats.cmt_dirty, (int)f->flags.deferred_error, (int)f->flags.bg_suspend, (int)f->flags.l2p_defer, (int)f->flags.md_load_failed);
        LOGI_IF(flt.all || flt.jrn, LOG_TAG_FTL "DBG d=%hhu jrn seq=%lu water=%hhu idle=%d pend=%d emerg=%d\r\n", die, jrn_sequence_get(f->journal), jrn_watermark_get(f->journal), (int)jrn_is_idle(f->journal), (int)jrn_has_pending_entries(f->journal), (int)jrn_is_emergency(f->journal));
        LOGI_IF(flt.all || flt.cp, LOG_TAG_FTL "DBG d=%hhu cp act=%d/%d ph=%d\r\n", die, (int)f->cp_handle->active, (int)f->cp_handle->waiting, (int)f->cp_handle->phase);
        LOGI_IF(flt.all || flt.mig, LOG_TAG_FTL "DBG d=%hhu mig act=%d/%d ph=%d\r\n", die, (int)f->mig_handle->job.active, (int)f->mig_handle->job.waiting, (int)f->mig_handle->job.phase);
        LOGI_IF(flt.all || flt.md, LOG_TAG_FTL "DBG d=%hhu md idle=%d ph=%d\r\n", die, (int)md_handle_is_idle(f->metadata_dir), (int)f->metadata_dir->handle.phase);
        LOGI_IF(flt.all || flt.wb, LOG_TAG_FTL "DBG d=%hhu wb open=%d base=%lu occ=0x%02hhX own=0x%08lX err=%d\r\n", die, (int)f->wbuf->open, f->wbuf->base, f->wbuf->occupied, f->wbuf->owners, (int)f->wbuf->error);

        if ((flt.all || flt.sched) && (f->sched.items != NULL))
        {
            ftl_sched_debug_dump(f, die);
        }
    }
    return true;
}

void ftl_assert_context(void)
{
    // 故障现场必须可见：运行期若把级别调到 WARN/ERROR，临时放宽到 INFO；
    // LOG_OFF 是测试/产测的显式静默，保持原样。
    const log_level_e level = log_get_level();

    if ((level != LOG_OFF) && (level < LOG_INFO))
    {
        log_set_level(LOG_INFO);
    }

    (void)ftl_debug_dump(NULL);
}
