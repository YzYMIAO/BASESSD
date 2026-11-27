/*
 * l2p_cache.c — 共享 L2P Region 缓存（CLOCK + 异步加载/写回 + thrashing 检测）
 *
 * 职责：
 *   把 L2P 映射表按 region 缓存进 SRAM，提供全局 LBA → PBA 的读/写；
 *   未命中经 owner 的 Metadata Directory 异步加载，脏行走 owner 的 L2P 日志块追加写回。
 *
 * 关键机制：
 *   · 共享 cache：同一 (ch,ce) 下所有 LUN 共用一份；键是全局 region，加载/写回用 owner die 的局部 region。
 *   · CLOCK 驱逐 + CMT（entry 级写缓存）；dirty 计数 / owner 计数 / 两个 bitmap 只在插入/清除闸门维护。
 *   · 加载与写回都「提交即返回」，完成由 g_op_callbacks[FTL_OP_L2P] 收尾。
 *
 * 与其他模块：
 *   metadata   —— 目录定位 / region 记账
 *   journal    —— 写回 WAL 次序（jrn_sequence_get / jrn_persisted_seq）
 *   ftl_op     —— 异步提交与完成派发
 *   allocator  —— 无直接依赖（经 owner）
 *
 * 入口：l2p_init / l2p_handle / l2p_read / l2p_write / l2p_entries_* / l2p_owner_* / l2p_region_*。
 */

#include "l2p_cache.h"

#include "assertion.h"
#include "bst.h"          /* ftl_block_state / ftl_block_state_name（降级现场诊断） */
#include "delay.h"        /* delay_time_now_us（加载延迟直方图） */
#include "fc_interface.h" /* fc_resource_stats_get（写回前的资源预检） */
#include "ftl.h"          /* ftl_ctx_s / ftl_service_pump（同步逃生口用） */
#include "ftl_fc_interface.h"
#include "ftl_op.h"   /* ftl_op_submit / ftl_op_is_in_dispatch */
#include "ftl_type.h" /* BG_LEVEL_* / BG_LEVEL_NUM（档位 → max_kick 表） */
#include "journal.h"  /* jrn_sequence_get / jrn_persisted_seq（WAL 次序闸门） */
#include "log.h"
#include "metadata_job.h" /* md_handle（整理 job 入口） */
#include "onfi_1_0.h"
#include "ssd_compiler.h"
#include "stats.h"

#include <string.h>

/* 行指针 → 同下标的查找元数据。region_id/state/access_count 只在 tags[] 里存放
 * （C9：不得在 l2p_cache_line_s 里保留第二份）。查找/CLOCK 只碰 tags[]，
 * 数据行只在命中 / 加载 / 写回时访问。 */
#define L2P_TAG(cache, line) (&(cache)->tags[(WORD)((line) - (cache)->lines)])

/* LBA → CMT set。用宏：无契约、参数仅出现一次；宏体只引用参数与 L2P_CMT_SETS。
 * 偏离 CERT C PRE00-C / MISRA C:2012 Dir 4.9，护栏见 H42（docs/guides/coding_rules.md）。 */
#define L2P_CMT_SET(lba) ((WORD)((lba) & (L2P_CMT_SETS - 1U)))

// 页内批量 API 的几何前提：一个 pair（FTL_LBA_PER_PAIR 个 LBA）必须完整落在同一
// L2P region 内（base 对齐 ⇒ base..base+7 同 region），否则批量读写会跨 region。
// 这是编译期事实，不是运行期检查（C7：恒真的运行期断言不算守卫）。
_Static_assert((L2P_ENTRIES_PER_REGION % FTL_LBA_PER_PAIR) == 0U, "FTL_LBA_PER_PAIR 必须整除 L2P_ENTRIES_PER_REGION");
_Static_assert((FTL_LBA_PER_PAIR & (FTL_LBA_PER_PAIR - 1U)) == 0U, "FTL_LBA_PER_PAIR 必须是 2 的幂（掩码/页基址依赖）");
_Static_assert(FTL_LBA_PER_PAIR < 32U, "1U << FTL_LBA_PER_PAIR 会溢出 DWORD");

// 在飞快照位容量必须覆盖写回并发上限：每个在飞写回占一个表项，
// 表满时 `md_slot_pair_append` 只能按背压拒绝——不能出现"取到槽却登记不上"。
_Static_assert(MD_SLOT_RESV_MAX >= (L2P_FLUSH_MAX_INFLIGHT * L2P_REGION_COPIES), "MD_SLOT_RESV_MAX 小于「写回并发上限 × 副本数」");

// 同一 region 连续加载失败多少次后降级为「全未映射的 CLEAN 行」。
// 取 3：一次瞬态读错（NAND 忙 / 命令超时）不应永久丢映射；持续失败则必须止损，
// 否则每次访问都重读坏 region、host 永久停在 FTL_AGAIN。
#define L2P_LOAD_RETRY_MAX (3U)

/* 后台每轮最多启动的 CMT 整页写回数：避免一次扫描把 FC 资源/行都占满。 */
#define L2P_CMT_WB_MAX (2U)

/* 预期 gate / submit 背压的日志节拍：10 s 一行；`suppressed=N` 携带被跳过的量级。
 * 满盘读校验时 gate 失败可持续数分钟，5 s 一行会让该行占日志主体。 */
#define L2P_LOAD_FAIL_LOG_INTERVAL_MS (10000U)

/* 加载启动失败原因：与 `load_fail_*` 计数同源，日志按 gate / submit / 建链分因。 */
typedef enum L2P_LOAD_FAIL_E
{
    L2P_LOAD_FAIL_NO_CMD = 0,
    L2P_LOAD_FAIL_BUILD,
    L2P_LOAD_FAIL_GATE,
    L2P_LOAD_FAIL_SUBMIT,
    L2P_LOAD_FAIL_INVALID_PRIMARY,
    L2P_LOAD_FAIL_INVALID_BACKUP
} l2p_load_fail_e;

// 设备级共享 L2P cache：一个**实际存在的 target** 一份，同 target 的多个 LUN 共用。
// 槽位按 (ch,ce) 扫描顺序紧凑编号；NAND_BOARD_TARGET_MAX 是板级上界，
// 稀疏板卡用它把常驻 cache 从 CH×CE 矩形收紧到实际 target 数。
// 定义与放置（普通 .bss → AXI）都归本模块；`owner` 端口按 die 登记。
static l2p_cache_s s_l2p_cache[NAND_BOARD_TARGET_MAX];

static BYTE s_target_ch[NAND_BOARD_TARGET_MAX];

static BYTE s_target_ce[NAND_BOARD_TARGET_MAX];

static BYTE s_target_count;

static l2p_owner_s s_l2p_owner[FTL_DIE_MAX];

/* ── 统计（模块自持；docs/design/common/statistics_design.md）── */
#if SSD_STATS_ENABLE
#    define L2P_STATS_LIST(X)                             \
        X(hit_count, STATS_U32, STATS_COUNTER)            \
        X(miss_count, STATS_U32, STATS_COUNTER)           \
        X(eviction_count, STATS_U32, STATS_COUNTER)       \
        X(wal_defer_count, STATS_U32, STATS_COUNTER)      \
        X(hint_hits, STATS_U32, STATS_COUNTER)            \
        X(hint_misses, STATS_U32, STATS_COUNTER)          \
        X(cmt_hits, STATS_U32, STATS_COUNTER)             \
        X(cmt_misses, STATS_U32, STATS_COUNTER)           \
        X(cmt_wr_hits, STATS_U32, STATS_COUNTER)          \
        X(cmt_wr_misses, STATS_U32, STATS_COUNTER)        \
        X(cmt_dirty, STATS_U32, STATS_GAUGE)              \
        X(cmt_writebacks, STATS_U32, STATS_COUNTER)       \
        X(cmt_evict_clean, STATS_U32, STATS_COUNTER)      \
        X(cmt_evict_dirty, STATS_U32, STATS_COUNTER)      \
        X(cmt_wb_defer, STATS_U32, STATS_COUNTER)         \
        X(load_start, STATS_U32, STATS_COUNTER)           \
        X(load_done_ok, STATS_U32, STATS_COUNTER)         \
        X(load_done_err, STATS_U32, STATS_COUNTER)        \
        X(load_fail_no_cmd, STATS_U32, STATS_COUNTER)     \
        X(load_fail_build, STATS_U32, STATS_COUNTER)      \
        X(load_fail_gate, STATS_U32, STATS_COUNTER)       \
        X(load_fail_submit, STATS_U32, STATS_COUNTER)     \
        X(degrade, STATS_U32, STATS_COUNTER)              \
        X(flush_start, STATS_U32, STATS_COUNTER)          \
        X(flush_ok, STATS_U32, STATS_COUNTER)             \
        X(flush_fail, STATS_U32, STATS_COUNTER)           \
        X(flush_defer_inflight, STATS_U32, STATS_COUNTER) \
        X(flush_defer_resource, STATS_U32, STATS_COUNTER) \
        X(flush_defer_md_slot, STATS_U32, STATS_COUNTER)  \
        X(flush_defer_build, STATS_U32, STATS_COUNTER)    \
        X(flush_defer_submit, STATS_U32, STATS_COUNTER)   \
        X(flush_backup_defer, STATS_U32, STATS_COUNTER)   \
        X(flush_backup_resume, STATS_U32, STATS_COUNTER)  \
        X(evict_fail, STATS_U32, STATS_COUNTER)

#    define L2P_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(l2p_cache_stats_s, field), width, kind},
static const stats_name_s s_stats_l2p_names[] = {
    L2P_STATS_LIST(L2P_STATS_NAME)};
#    undef L2P_STATS_NAME
#    define L2P_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(l2p_cache_stats_s, field, width);
L2P_STATS_LIST(L2P_STATS_CHECK)
#    undef L2P_STATS_CHECK

#    define L2P_PREFETCH_STATS_LIST(X)        \
        X(calls, STATS_U32, STATS_COUNTER)    \
        X(started, STATS_U32, STATS_COUNTER)  \
        X(skipped, STATS_U32, STATS_COUNTER)  \
        X(resident, STATS_U32, STATS_COUNTER) \
        X(hits, STATS_U32, STATS_COUNTER)

#    define L2P_PREFETCH_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(l2p_prefetch_stats_s, field), width, kind},
static const stats_name_s s_stats_l2p_prefetch_names[] = {
    L2P_PREFETCH_STATS_LIST(L2P_PREFETCH_STATS_NAME)};
#    undef L2P_PREFETCH_STATS_NAME
#    define L2P_PREFETCH_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(l2p_prefetch_stats_s, field, width);
L2P_PREFETCH_STATS_LIST(L2P_PREFETCH_STATS_CHECK)
#    undef L2P_PREFETCH_STATS_CHECK

#    define L2P_STATS_HIST_LIST(X)     X(load_lat_us)
#    define L2P_STATS_HIST_NAME(field) {STATS_NAME_STR(field), STATS_OFFSET(l2p_cache_stats_s, field)},
static const stats_hist_name_s s_stats_l2p_hist_names[] = {
    L2P_STATS_HIST_LIST(L2P_STATS_HIST_NAME)};
#    undef L2P_STATS_HIST_NAME
STATS_CHECK_HIST_FIELD(l2p_cache_stats_s, load_lat_us);
#endif /* SSD_STATS_ENABLE */

// CLOCK 选行：跳过 LOADING/FLUSHING；access_count>0 递减后看下一行；INVALID 直接占用；
// DIRTY 先 kick 异步写回再跳过；access_count==0 的 CLEAN 可占用。
// 返回 NULL 是设计内行为（引用位全 1 时先递减），持续 NULL 才是写回通道堵死。
static DWORD s_evict_fail_ms;

static DWORD s_evict_fail_n;

// load 启动失败的限流日志（单线程 superloop；多实例共享一个时间戳即可）。
//
// 覆盖「拿不到命令 / 提交背压」两种失败（"无行可驱逐"由 l2p_evict_fail_log
// 带现场计数打印）。持续失败时 confirm park 会超时并让 host 写失败，没有日志
// 就没有线索。1 s 一行；时钟缺失时 64 次一行，避免刷屏。
/* ══════════════════════════════════════════════════════════
 * 4. 加载
 * ══════════════════════════════════════════════════════════ */

static DWORD s_load_fail_ms;

static DWORD s_load_fail_n;

// region 降级日志的限流。
//
// 一次 full metadata rebuild 可能让大量 region 同时降级；逐 region 打 LOGE 会
// 写满 UART TX 环并拖慢重建。5 s 一行 + suppressed 计数；时钟缺失时 64 次一行。
static DWORD s_degrade_log_ms;

static DWORD s_degrade_n;

// 档位 → 每轮主动 flush 条数上界（模块内 static const 策略表）。
static const BYTE s_l2p_flush_max[BG_LEVEL_NUM] = {
    [BG_LEVEL_IDLE]   = L2P_CACHE_SIZE,
    [BG_LEVEL_NORMAL] = L2P_FLUSH_MAX_NORMAL,
    [BG_LEVEL_BUSY]   = L2P_FLUSH_MAX_BUSY,
};

/* ── 前向声明（按新布局：被后文调用；环断点取声明少的方向） ── */
static l2p_cache_line_s  *l2p_line_find(l2p_cache_s *cache, DWORD region_id);
static const l2p_owner_s *l2p_owner_get(const l2p_cache_s *cache, DWORD global_region);

/* ── hint ── */

/**
 * @brief region_id → hint 槽位
 *
 * 低位与高位混合，避免连续 region 在 256 槽里按固定步长冲突。
 *
 * @param region_id 全局 region
 * @return 槽位（0..L2P_LINE_HINT_SLOTS-1）
 */
static WORD l2p_hint_slot(DWORD region_id)
{
    return (WORD)((region_id ^ (region_id >> 8U)) & (L2P_LINE_HINT_SLOTS - 1U));
}

/* ── access ── */

/**
 * @brief Thrashing 窗口结算。
 *
 * 每次 cache 访问（命中或未命中）调用一次；窗口满 L2P_THRASH_WINDOW 时按 miss 率
 * 与滞回阈值更新 thrashing 标志并清零窗口。仅状态**翻转**时打一行观测日志
 * （阈值附近不抖动 ⇒ 不会刷屏）。
 *
 * @param cache   cache 上下文
 * @param is_miss true = 本次访问未命中
 */
static void l2p_access_note(l2p_cache_s *cache, BOOL is_miss)
{
    cache->thrash.window_access++;
    if (is_miss)
    {
        cache->thrash.window_miss++;
    }

    if (cache->thrash.window_access < L2P_THRASH_WINDOW)
    {
        return;
    }

    cache->thrash.miss_pct = ((cache->thrash.window_miss * 100U) / cache->thrash.window_access);

    BOOL was_thrashing = cache->thrash.thrashing;

    cache->thrash.window_access = 0U;
    cache->thrash.window_miss   = 0U;

    // 滞回：进入 ≥90%，退出 <70%，中间保持原态
    if (cache->thrash.miss_pct >= L2P_THRASH_ENTER_PCT)
    {
        cache->thrash.thrashing = true;
    }
    else if (cache->thrash.miss_pct < L2P_THRASH_EXIT_PCT)
    {
        cache->thrash.thrashing = false;
    }

    // 观测口：miss= 为最近窗口 miss 率；hit/miss= 为开机累计。
    LOGI_IF(cache->thrash.thrashing != was_thrashing, LOG_TAG_L2P "thrash=%d miss=%hhu%% hit=%lu miss_total=%lu\r\n", cache->thrash.thrashing, cache->thrash.miss_pct, cache->stats.hit_count, cache->stats.miss_count);
}

/* ── hit ── */

/**
 * @brief 命中记账（read / write 共用）。
 *
 * CLOCK 的引用位“命中置 1”；若本行正是为当前这次访问刚加载完的（fresh），
 * 这一击是那次 miss 的重试 ⇒ 只消费 fresh，不计命中。
 *
 * @param cache cache 上下文
 * @param line  命中的 cache 行
 */
static void l2p_hit_note(l2p_cache_s *cache, l2p_cache_line_s *line)
{
    L2P_TAG(cache, line)->access_count = 1U;

    // 预取行的第一次 host 命中是真实收益：清标志并计数。
    // 预取完成时 fresh=false ⇒ 下面继续计 hit；host 加载的 fresh 语义不变。
    if (line->prefetch)
    {
        line->prefetch = false;
        cache->prefetch.hits++;
    }

    if (line->fresh)
    {
        line->fresh = false;
        return;
    }

    cache->stats.hit_count++;
    l2p_access_note(cache, false);
}

/* ── degrade ── */

/**
 * @brief 打一条「region 降级为全未映射」限流日志。
 *
 * @param region_id  降级的全局 region
 * @param fail_count 连续加载失败次数
 */
static SSD_CODE_FAST void l2p_degrade_log(DWORD region_id, BYTE fail_count)
{
    LOGE_RATE(log_rate_due(&s_degrade_log_ms, &s_degrade_n, LOG_RATE_SLOW_INTERVAL_MS), s_degrade_n, LOG_TAG_L2P "region %lu load failed %hu times — degrade to unmapped", region_id, fail_count);
}

/* ── evict ── */

/**
 * @brief 打一条「无可驱逐行」限流日志，并输出各状态行数。
 *
 * @param cache     cache 上下文
 * @param region_id 触发驱逐失败的全局 region
 */
static SSD_CODE_FAST void l2p_evict_fail_log(const l2p_cache_s *cache, DWORD region_id)
{
    // 先判限流再扫行状态：抑制时不做 O(L2P_CACHE_SIZE) 统计，所以保留 early-return。
    const DWORD suppressed = s_evict_fail_n;

    if (log_rate_due(&s_evict_fail_ms, &s_evict_fail_n, LOG_RATE_SLOW_INTERVAL_MS) == false)
    {
        return;
    }

    WORD inv      = 0U;
    WORD clean    = 0U;
    WORD dirty    = 0U;
    WORD loading  = 0U;
    WORD flushing = 0U;

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        switch (cache->tags[i].state)
        {
            case L2P_CACHE_INVALID:
                inv++;
                break;
            case L2P_CACHE_CLEAN:
                clean++;
                break;
            case L2P_CACHE_DIRTY:
                dirty++;
                break;
            case L2P_CACHE_LOADING:
                loading++;
                break;
            case L2P_CACHE_FLUSHING:
                flushing++;
                break;
            default:
                break;
        }
    }

    LOGW(LOG_TAG_L2P "no evictable line: region=%lu inv=%hu clean=%hu dirty=%hu loading=%hu flushing=%hu suppressed=%lu\r\n", region_id, inv, clean, dirty, loading, flushing, suppressed);
}

/* ── fc ── */

/**
 * @brief demand L2P load 是否还有 FC 余量（给 CP/metadata 留保底）
 *
 * @return true = 提交后仍留出 FTL_FC_CP_*_HEADROOM；false = 应背压
 */
static BOOL l2p_fc_demand_headroom_is_sufficient(void)
{
    fc_resource_stats_s st;

    fc_resource_stats_get(&st);

    return (st.command_used + 1U + FTL_FC_CP_CMD_HEADROOM <= FC_COMMAND_POOL_DEPTH) && (st.descriptor_used + CW_NUM + FTL_FC_CP_DESC_HEADROOM <= FC_DESCRIPTOR_POOL_DEPTH) && (st.buffer_used + CW_NUM + FTL_FC_CP_BUFS_HEADROOM <= FC_BUFFER_POOL_DEPTH);
}

/* ── sync ── */

/**
 * @brief 同步逃生口的一次泵：推 NAND + 抽完成表，并推进 owner 的目录整理 job。
 *
 * @param cache cache 上下文（可为 NULL）
 * @param owner owner 实例（可为 NULL；非空时推进其 metadata_dir 的 job）
 */
static void l2p_sync_pump(l2p_cache_s *cache, const l2p_owner_s *owner)
{
    ASSERT(ftl_op_is_in_dispatch() == false);

    ftl_service_pump();

    if ((cache != NULL) && (owner != NULL))
    {
        md_handle(owner->metadata_dir);
    }
}

/* ── stats ── */

#if SSD_STATS_ENABLE
/**
 * @brief 注册一个 target 的 L2P 统计（值在 cache 本体，不复制）
 *
 * @param cache 共享 cache
 * @param inst  实例号（打印为 ftl.l2p[inst]，取 target 紧凑下标）
 */
static void l2p_stats_register(l2p_cache_s *cache, BYTE inst)
{
    (void)stats_register("ftl.l2p", inst, &cache->stats, s_stats_l2p_names, STATS_ENTRY_COUNT(s_stats_l2p_names));
    (void)stats_register_hist("ftl.l2p", inst, &cache->stats, s_stats_l2p_hist_names, STATS_ENTRY_COUNT(s_stats_l2p_hist_names));
    (void)stats_register("ftl.l2p.prefetch", inst, &cache->prefetch, s_stats_l2p_prefetch_names, STATS_ENTRY_COUNT(s_stats_l2p_prefetch_names));
}
#endif /* SSD_STATS_ENABLE */

/* ── cache ── */

/**
 * @brief 取 (ch,ce) 对应的共享 cache（定义在 l2p_cache.c）
 *
 * @param topo 本实例拓扑（ch/ce 必须在范围内）
 * @return 共享 cache；该 (ch,ce) 不存在 / 无 present LUN 时返回 NULL
 */
l2p_cache_s *l2p_cache_workspace_get(ftl_topo_s topo)
{
    ASSERT(topo.ch < CH_NUM);
    ASSERT(topo.ce < CE_NUM);

    for (BYTE i = 0U; i < s_target_count; i++)
    {
        if ((s_target_ch[i] == topo.ch) && (s_target_ce[i] == topo.ce))
        {
            return &s_l2p_cache[i];
        }
    }

    return NULL;
}

/* ── cmt ── */

/**
 * @brief 推进 LRU 时间戳（16-bit 回绕时整体右移，保持相对顺序）
 *
 * @param cache cache
 * @param e     被访问 / 插入的条目
 */
static void l2p_cmt_touch(l2p_cache_s *cache, l2p_cmt_entry_s *e)
{
    if (cache->cmt_clock == UINT16_MAX)
    {
        for (WORD set = 0U; set < L2P_CMT_SETS; set++)
        {
            for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
            {
                cache->cmt[set][way].stamp = (WORD)(cache->cmt[set][way].stamp >> 1U);
            }
        }
        cache->cmt_clock = (WORD)(UINT16_MAX / 2U);
    }

    cache->cmt_clock++;
    e->stamp = cache->cmt_clock;
}

/**
 * @brief 退役一个 dirty CMT 条目的两份计数（唯一释放闸门）
 *
 * `cmt_dirty_owner[die]` 与 `stats.cmt_dirty` 必须与 dirty 条目数守恒：
 * 少计会让 `l2p_flush_owner_idle` 提前为真（FLUSH/CP 在未落盘时完成 ⇒ 重启丢映射）；
 * 多计会让 idle 永不为真（反复等超时 / CP 中止）。原实现用 `if (n > 0) n--`
 * 静默夹取，把账本分叉吞掉——这里 debug 断言 + release 留痕。
 *
 * @param cache cache
 * @param lba   即将退役的 dirty 条目 LBA
 */
static void l2p_cmt_dirty_release(l2p_cache_s *cache, DWORD lba)
{
    const BYTE owner = lba_2_die(lba);

    ASSERT(cache->cmt_dirty_owner[owner] > 0U);
    ASSERT(cache->stats.cmt_dirty > 0U);

    if (cache->cmt_dirty_owner[owner] > 0U)
    {
        cache->cmt_dirty_owner[owner]--;
    }
    else
    {
        LOGE(LOG_TAG_L2P "cmt dirty owner underflow owner=%hhu\r\n", owner);
    }

    if (cache->stats.cmt_dirty > 0U)
    {
        cache->stats.cmt_dirty--;
    }
    else
    {
        LOGE(LOG_TAG_L2P "cmt dirty total underflow\r\n");
    }
}

/**
 * @brief 该 LBA 是否已有槽位或可无写回地插入（空 / CLEAN way）
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @return true 可更新/插入；false set 的 4 路全是 DIRTY
 */
static BOOL l2p_cmt_has_room(const l2p_cache_s *cache, DWORD lba)
{
    const WORD set = L2P_CMT_SET(lba);

    for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
    {
        const l2p_cmt_entry_s *e = &cache->cmt[set][way];

        if ((e->lba == lba) || (e->lba == FTL_LBA_INVALID) || (e->state == L2P_CMT_CLEAN))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 把本 region 的 dirty CMT 合并进 line（不清 CMT，等写回 commit）
 *
 * @param cache cache
 * @param line  目标行
 */
static SSD_CODE_FAST void l2p_cmt_merge_region(l2p_cache_s *cache, l2p_cache_line_s *line)
{
    const DWORD region = L2P_TAG(cache, line)->region_id;
    DWORD       seq    = line->dirty_seq;
    BOOL        found  = false;

    for (WORD set = 0U; set < L2P_CMT_SETS; set++)
    {
        for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
        {
            const l2p_cmt_entry_s *e = &cache->cmt[set][way];

            if ((e->lba == FTL_LBA_INVALID) || (e->state != L2P_CMT_DIRTY) || (lba_to_global_region(e->lba) != region))
            {
                continue;
            }
            line->entries[lba_to_entry(e->lba)] = e->pba;
            if (e->dirty_seq > seq)
            {
                seq = e->dirty_seq;
            }
            found = true;
        }
    }

    if (found == false)
    {
        /* 索引位脏但条目已清：清索引位，避免背景写回空转。 */
        l2p_bitmap_clear(cache->cmt_dirty_region_bitmap, region);
        return;
    }

    line->dirty_seq             = seq;
    L2P_TAG(cache, line)->state = L2P_CACHE_DIRTY;
}

/**
 * @brief 开关 CMT（仅 boot journal replay 使用）
 *
 * @param cache   cache
 * @param enabled true = 正常；false = replay 旁路（不查不填 CMT）
 */
SSD_CODE_FAST void l2p_cmt_set_enabled(l2p_cache_s *cache, BOOL enabled)
{
    cache->cmt_enabled = enabled;
}

/**
 * @brief 按 LBA 找 CMT 槽位（命中可选更新 LRU）
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @param touch true = 命中时更新 LRU
 * @return 条目指针；NULL = 未命中
 */
static l2p_cmt_entry_s *l2p_cmt_find(l2p_cache_s *cache, DWORD lba, BOOL touch)
{
    const WORD set = L2P_CMT_SET(lba);

    for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
    {
        l2p_cmt_entry_s *e = &cache->cmt[set][way];

        if (e->lba == lba)
        {
            if (touch)
            {
                l2p_cmt_touch(cache, e);
            }
            return e;
        }
    }
    return NULL;
}

/**
 * @brief 清一个 CMT 槽位并结算 dirty 计数
 *
 * @param cache cache
 * @param e     槽位（空槽时无动作）
 */
static SSD_CODE_FAST void l2p_cmt_drop(l2p_cache_s *cache, l2p_cmt_entry_s *e)
{
    if (e->lba == FTL_LBA_INVALID)
    {
        return;
    }

    if (e->state == L2P_CMT_DIRTY)
    {
        l2p_cmt_dirty_release(cache, e->lba);
    }

    e->lba       = FTL_LBA_INVALID;
    e->pba       = NAND_PBA_INVALID;
    e->dirty_seq = 0U;
    e->state     = L2P_CMT_CLEAN;
    e->stamp     = 0U;
}

/**
 * @brief 确保该 LBA 可写 CMT；无槽位时触发一个 dirty victim 整页写回
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @return true 可写；false 已触发写回 / 无可写回 victim，调用者下轮重试
 */
static SSD_CODE_FAST BOOL l2p_cmt_room_reserve(l2p_cache_s *cache, DWORD lba)
{
    if (l2p_cmt_has_room(cache, lba))
    {
        return true;
    }

    // 本 set 的 LRU dirty victim：先写回它的整个 translation page，腾出槽位。
    // 跳过已有加载/写回在飞的 region，尝试同 set 的其他 dirty way——
    // 否则一个在飞的 victim 会把 host 写一直卡在容量检查上。
    const WORD             set           = L2P_CMT_SET(lba);
    const l2p_cmt_entry_s *victim        = NULL;
    DWORD                  victim_region = 0U;

    for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
    {
        const l2p_cmt_entry_s *e = &cache->cmt[set][way];

        if ((e->lba == FTL_LBA_INVALID) || (e->state != L2P_CMT_DIRTY))
        {
            continue;
        }

        const DWORD             region = lba_to_global_region(e->lba);
        const l2p_cache_line_s *line   = l2p_line_find(cache, region);

        if ((line != NULL) && ((L2P_TAG(cache, line)->state == L2P_CACHE_LOADING) || (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING)))
        {
            continue;
        }
        if ((victim == NULL) || (e->stamp < victim->stamp))
        {
            victim        = e;
            victim_region = region;
        }
    }

    if (victim != NULL)
    {
        cache->stats.cmt_evict_dirty++;
        (void)l2p_region_flush_kick(cache, victim_region);
    }
    else
    {
        cache->stats.cmt_wb_defer++;
    }
    return false;
}

/**
 * @brief CMT 读命中（DIRTY 允许 INVALID 负条目）
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @param out   输出 PBA
 * @return true 命中；false 未命中
 */
static BOOL l2p_cmt_lookup(l2p_cache_s *cache, DWORD lba, DWORD *out)
{
    l2p_cmt_entry_s *e = l2p_cmt_find(cache, lba, true);

    if (e == NULL)
    {
        return false;
    }
    /* CLEAN 只缓存有效映射；DIRTY 允许 INVALID（TRIM 负条目）。 */
    if ((e->state == L2P_CMT_CLEAN) && (e->pba == NAND_PBA_INVALID))
    {
        l2p_cmt_drop(cache, e);
        return false;
    }
    *out = e->pba;
    return true;
}

/**
 * @brief 取一个可写槽位：同 LBA > 空槽 > LRU CLEAN；不淘汰 DIRTY
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @return 槽位；NULL = 4 路全 DIRTY（调用者需触发整页写回）
 */
static l2p_cmt_entry_s *l2p_cmt_claim(l2p_cache_s *cache, DWORD lba)
{
    l2p_cmt_entry_s *e = l2p_cmt_find(cache, lba, false);

    if (e != NULL)
    {
        return e;
    }

    const WORD set = L2P_CMT_SET(lba);

    e = NULL;
    for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
    {
        l2p_cmt_entry_s *cand = &cache->cmt[set][way];

        if (cand->lba == FTL_LBA_INVALID)
        {
            e = cand;
            break;
        }
        if ((cand->state == L2P_CMT_CLEAN) && ((e == NULL) || (cand->stamp < e->stamp)))
        {
            e = cand;
        }
    }

    if (e == NULL)
    {
        return NULL; /* 4 路全 dirty：不许单独淘汰 */
    }
    if (e->lba != FTL_LBA_INVALID)
    {
        cache->stats.cmt_evict_clean++;
    }

    e->lba       = lba;
    e->pba       = NAND_PBA_INVALID;
    e->dirty_seq = 0U;
    e->state     = L2P_CMT_CLEAN;
    e->stamp     = 0U;
    return e;
}

/**
 * @brief 整页写回 commit 后结算本 region 的 dirty CMT
 *
 * 有效 PBA 转 CLEAN（值已在页上，保留读缓存）；INVALID 负条目直接丢弃。
 *
 * @param cache  cache
 * @param region 全局 region
 * @return true = 本 region 至少 retire 了一个 dirty 条目
 */
static SSD_CODE_FAST BOOL l2p_cmt_clear_region(l2p_cache_s *cache, DWORD region)
{
    BOOL retired = false;

    for (WORD set = 0U; set < L2P_CMT_SETS; set++)
    {
        for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
        {
            l2p_cmt_entry_s *e = &cache->cmt[set][way];

            if ((e->lba == FTL_LBA_INVALID) || (e->state != L2P_CMT_DIRTY) || (lba_to_global_region(e->lba) != region))
            {
                continue;
            }

            retired = true;
            if (e->pba != NAND_PBA_INVALID)
            {
                l2p_cmt_dirty_release(cache, e->lba);
                e->state     = L2P_CMT_CLEAN;
                e->dirty_seq = 0U;
            }
            else
            {
                l2p_cmt_drop(cache, e);
            }
        }
    }
    l2p_bitmap_clear(cache->cmt_dirty_region_bitmap, region);
    return retired;
}

/**
 * @brief 使一个 region 的 CMT 失效
 *
 * @param cache         cache
 * @param region        全局 region
 * @param lba           失败页基址（仅 selective==true 时使用）
 * @param cw_bitmap     失败页 CW 位图（仅 selective==true 时使用）
 * @param selective     true = 只清失败页 LBA 范围的 dirty；false = 保留全部 dirty
 */
static SSD_CODE_FAST void l2p_cmt_invalidate_region(l2p_cache_s *cache, DWORD region, DWORD lba, DWORD cw_bitmap, BOOL selective)
{
    BOOL any_dirty = false;

    for (WORD set = 0U; set < L2P_CMT_SETS; set++)
    {
        for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
        {
            l2p_cmt_entry_s *e = &cache->cmt[set][way];

            if ((e->lba == FTL_LBA_INVALID) || (lba_to_global_region(e->lba) != region))
            {
                continue;
            }

            if (e->state == L2P_CMT_CLEAN)
            {
                l2p_cmt_drop(cache, e);
                continue;
            }

            if (selective && (e->lba >= lba))
            {
                const DWORD off = e->lba - lba;

                if ((off < FTL_LBA_PER_PAIR) && ((cw_bitmap & (1UL << off)) != 0U))
                {
                    l2p_cmt_drop(cache, e);
                    continue;
                }
            }
            any_dirty = true;
        }
    }

    if (any_dirty == false)
    {
        l2p_bitmap_clear(cache->cmt_dirty_region_bitmap, region);
    }
}

/**
 * @brief 使一个 owner 的 CMT 全部失效（format / rebuild）
 *
 * @param cache     cache
 * @param owner_die owner die
 */
static SSD_CODE_FAST void l2p_cmt_invalidate_owner(l2p_cache_s *cache, BYTE owner_die)
{
    for (WORD set = 0U; set < L2P_CMT_SETS; set++)
    {
        for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
        {
            l2p_cmt_entry_s *e = &cache->cmt[set][way];

            if ((e->lba != FTL_LBA_INVALID) && (lba_2_die(e->lba) == owner_die))
            {
                l2p_cmt_drop(cache, e);
            }
        }
    }

    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        l2p_bitmap_clear(cache->cmt_dirty_region_bitmap, l2p_local_to_global_region(local, owner_die));
    }

    cache->cmt_dirty_owner[owner_die] = 0U;
}

/**
 * @brief 把 LBA 标记为 DIRTY（已有条目原地更新；否则占用空/CLEAN 槽）
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @param pba   新 PBA（允许 INVALID）
 * @param seq   Journal 序号（WAL 水位）
 * @return true 已写入；false 无槽位（调用者走 line 回退）
 */
static BOOL l2p_cmt_install_dirty(l2p_cache_s *cache, DWORD lba, DWORD pba, DWORD seq)
{
    l2p_cmt_entry_s *e = l2p_cmt_claim(cache, lba);

    if (e == NULL)
    {
        return false;
    }

    if (e->state != L2P_CMT_DIRTY)
    {
        const BYTE owner = lba_2_die(lba);

        cache->cmt_dirty_owner[owner]++;
        cache->stats.cmt_dirty++;
    }

    e->pba       = pba;
    e->dirty_seq = seq;
    e->state     = L2P_CMT_DIRTY;
    l2p_cmt_touch(cache, e);

    const DWORD region = lba_to_global_region(lba);

    l2p_bitmap_set(cache->region_dirty_bitmap, region);
    l2p_bitmap_set(cache->cmt_dirty_region_bitmap, region);
    return true;
}

/**
 * @brief 读填充 CLEAN 条目（只接受 line CLEAN 的页上真值）
 *
 * 同 LBA 的 DIRTY 条目绝不覆盖；INVALID 表示删除 CLEAN 槽。
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @param pba   新值
 */
static void l2p_cmt_update_clean(l2p_cache_s *cache, DWORD lba, DWORD pba)
{
    if (cache->cmt_enabled == false)
    {
        return;
    }

    l2p_cmt_entry_s *e = l2p_cmt_find(cache, lba, false);

    if (e != NULL)
    {
        if (e->state == L2P_CMT_DIRTY)
        {
            // CLEAN 填充只应来自已落盘 line；若碰到 dirty 条目，绝不拿旧值覆盖。
            return;
        }
        if (pba == NAND_PBA_INVALID)
        {
            l2p_cmt_drop(cache, e);
            return;
        }
        e->pba       = pba;
        e->dirty_seq = 0U;
        e->state     = L2P_CMT_CLEAN;
        l2p_cmt_touch(cache, e);
        return;
    }

    if (pba == NAND_PBA_INVALID)
    {
        return;
    }

    e = l2p_cmt_claim(cache, lba);
    if (e == NULL)
    {
        return; /* 读缓存填充失败：line 仍是权威 */
    }
    e->pba   = pba;
    e->state = L2P_CMT_CLEAN;
    l2p_cmt_touch(cache, e);
}

/* ── dirty ── */

/**
 * @brief 记录本行映射依赖的 Journal 序号上界（WAL 次序）
 *
 * 所有调用点都满足「先 jrn_append / jrn_append_seq、后写 L2P」。这里取当前的
 * `sequence_counter`（下一条待分配序号）：区域写回时要求 Journal 落盘水位 ≥ 它，
 * 即覆盖本行更新的条目都已持久化。
 *
 * @param cache     cache
 * @param line      目标行
 * @param region_id 全局 region（用于解析 owner 的 Journal）
 */
static DWORD l2p_dirty_seq(l2p_cache_s *cache, DWORD region_id)
{
    const l2p_owner_s *owner = l2p_owner_get(cache, region_id);

    return ((owner != NULL) && (owner->journal != NULL)) ? jrn_sequence_get(owner->journal) : 0U;
}

/**
 * @brief 统计全局脏位图的置位数。
 *
 * @param cache cache 上下文
 * @return 脏位图全部 word 的置位总数
 */
static SSD_CODE_FAST WORD l2p_dirty_count(const l2p_cache_s *cache)
{
    WORD count = 0U;

    for (DWORD i = 0U; i < L2P_DIRTY_BITMAP_WORDS; i++)
    {
        count = (WORD)(count + ftl_cw_bitmap_popcount(cache->region_dirty_bitmap[i]));
    }
    return count;
}

/* ── line ── */

/**
 * @brief 清掉某个 line 的 hint（只清仍指向它的那个槽）
 *
 * @param cache cache
 * @param idx   line 下标
 */
static SSD_CODE_FAST void l2p_line_hint_clear(l2p_cache_s *cache, WORD idx)
{
    if (idx >= L2P_CACHE_SIZE)
    {
        return;
    }

    // 即使行已 INVALID，也要清掉仍指向它的旧 hint；hint 只是提示，清错最多多一次回退。
    const WORD slot = l2p_hint_slot(cache->tags[idx].region_id);

    if (cache->line_hint[slot] == (BYTE)idx)
    {
        cache->line_hint[slot] = UINT8_MAX;
    }
}

/**
 * @brief 内部查找：在 cache 中查找 region_id 对应的 line。
 *
 * ⚠ **匹配四种状态**（CLEAN / DIRTY / FLUSHING / LOADING），只排除 INVALID：
 * 异步化后 FLUSHING / LOADING 的窗口是**真实存在**的（跨多轮），若把它们当未命中，
 * 就会为同一 region 再分配一行 ⇒ 同一 region 两行分叉（一行 CLEAN 一行 DIRTY），
 * 后续读按**下标序**取第一行 ⇒ 写进另一行 ⇒ 映射静默不一致。
 * 命中后各调用者再按 state 判"能不能读/写"（见 l2p_read / write）。
 * 查找先走 `line_hint`（region→line 下标的提示），命中仍用 tags[] 校验；
 * hint 空/冲突/陈旧时退回线性扫描，因此 hint 只影响性能，不影响正确性。
 *
 * @param cache     cache 上下文
 * @param region_id 全局 region
 * @return 命中的行；NULL = 该 region 未驻留
 */
static l2p_cache_line_s *l2p_line_find(l2p_cache_s *cache, DWORD region_id)
{
    // 先查 hint（region → line 下标）；tags[] 仍是唯一权威，命中必须再校验。
    const WORD slot = l2p_hint_slot(region_id);
    const BYTE hint = cache->line_hint[slot];

    if (hint < L2P_CACHE_SIZE)
    {
        const l2p_tag_s *tag = &cache->tags[hint];

        if ((tag->region_id == region_id) && (tag->state != L2P_CACHE_INVALID))
        {
            cache->stats.hint_hits++;
            return &cache->lines[hint];
        }
    }
    cache->stats.hint_misses++;

    // hint 空/冲突/陈旧：退回线性扫描，命中后顺手修正 hint。
    for (WORD idx = 0U; idx < L2P_CACHE_SIZE; idx++)
    {
        const l2p_tag_s *tag = &cache->tags[idx];

        if ((tag->region_id == region_id) && (tag->state != L2P_CACHE_INVALID))
        {
            cache->line_hint[slot] = (BYTE)idx;
            return &cache->lines[idx];
        }
    }

    return NULL;
}

/**
 * @brief 读一行诊断信息
 *
 * 下标越界返回 INVALID（诊断入口不中断）。
 *
 * @param cache cache
 * @param idx   行下标
 * @param out   输出：行信息
 */
SSD_CODE_FAST void l2p_line_get(const l2p_cache_s *cache, WORD idx, l2p_line_info_s *out)
{
    if (idx >= L2P_CACHE_SIZE)
    {
        out->region_id = 0U;
        out->state     = L2P_CACHE_INVALID;
        return;
    }

    out->region_id = cache->tags[idx].region_id;
    out->state     = (l2p_cache_state_e)cache->tags[idx].state;
}

/**
 * @brief CLOCK 选一个可驱逐的 cache 行。
 *
 * @param cache       cache 上下文
 * @param flush_dirty true = 遇 DIRTY 行先起异步写回再继续扫描；false = 跳过 DIRTY
 * @return 选中的行；NULL = 当前没有可驱逐行（全在飞 / 脏行都刚起了写回）
 */
static l2p_cache_line_s *l2p_line_evict(l2p_cache_s *cache, BOOL flush_dirty)
{
    WORD start = cache->clock_hand;

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        const WORD idx = ((start + i) % L2P_CACHE_SIZE);
        l2p_tag_s *tag = &cache->tags[idx];

        if ((tag->state == L2P_CACHE_FLUSHING) || (tag->state == L2P_CACHE_LOADING))
        {
            continue; /* 有命令在飞：行内容正在被 NAND 改写，不可复用 */
        }

        if (tag->state == L2P_CACHE_INVALID)
        {
            // 空行，直接占用
            cache->clock_hand = ((idx + 1U) % L2P_CACHE_SIZE);
            return &cache->lines[idx];
        }

        if (tag->access_count > 0U)
        {
            // 第二次机会：减计数，检查下一个
            tag->access_count--;
            continue;
        }

        // access_count == 0 且不是 INVALID/在飞 → 选中
        cache->clock_hand = ((idx + 1U) % L2P_CACHE_SIZE);

        if (tag->state == L2P_CACHE_DIRTY)
        {
            // 脏行：先起异步写回再复用（F4 关键前置）。
            //
            // ⚠ **绝不能**在此丢弃脏行：置 INVALID 会丢 RAM 脏数据，同时留下
            // 「bit 置位但无 resident DIRTY 行」的孤儿 bit——Checkpoint 按 bitmap
            // 假声明该区已落盘 ⇒ 重启后坏读。起 kick 后本行变 FLUSHING ⇒ 本轮跳过，
            // 继续找别的候选；kick 失败（背压/无依赖）⇒ 行仍 DIRTY，同样跳过。
            if (flush_dirty)
            {
                (void)l2p_flush_kick(cache, idx);
            }
            continue;
        }

        // 标记为 INVALID，等待调用者重新初始化
        l2p_line_hint_clear(cache, idx);
        tag->state                 = L2P_CACHE_INVALID;
        cache->lines[idx].prefetch = false;
        cache->stats.eviction_count++;
        return &cache->lines[idx];
    }

    // CLOCK 第二轮：第一轮已经沿途把引用位递减。顺序访问流里「64 行引用位全 1」
    // 完全正常（每个 region 恰好命中一次），若就此返回 NULL，调用者要白等一轮
    // FTL_AGAIN、并打一条 `no evictable line` WRN。再扫一圈只选 CLEAN + ref==0；
    // DIRTY / 在飞行仍不碰（它们该走 kick / 等完成）。
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        const WORD idx = ((cache->clock_hand + i) % L2P_CACHE_SIZE);
        l2p_tag_s *tag = &cache->tags[idx];

        if ((tag->state == L2P_CACHE_CLEAN) && (tag->access_count == 0U))
        {
            cache->clock_hand = ((idx + 1U) % L2P_CACHE_SIZE);
            l2p_line_hint_clear(cache, idx);
            tag->state                 = L2P_CACHE_INVALID;
            cache->lines[idx].prefetch = false;
            cache->stats.eviction_count++;
            return &cache->lines[idx];
        }
    }

    return NULL; /* 无候选（全在飞 / 脏行都已起写回） */
}

/**
 * @brief 某 LBA 所属 region 的 line 进度代数（GC 的依赖活性判据）
 *
 * 复用 l2p_line_find 定位行（会更新 hint 与命中统计；GC 等待是低频路径，
 * 统计只用于 CLI 展示，无算法消费者）。region 未驻留返回 0。
 * load 与 flush 每完成一次都会递增；别的 region 不会推进本行。
 *
 * @param cache 共享 cache
 * @param lba   host LBA
 * @return 该 region 所在行的进度代数；未驻留为 0
 */
DWORD l2p_line_progress(l2p_cache_s *cache, DWORD lba)
{
    ASSERT(cache != NULL);

    const l2p_cache_line_s *line = l2p_line_find(cache, lba_to_global_region(lba));

    return (line != NULL) ? line->line_generation : 0U;
}

/**
 * @brief 使一行作废（格式化 / 重建用）
 *
 * 清状态、降级与 Journal 依赖标记。
 *
 * @param cache cache
 * @param idx   行下标
 */
SSD_CODE_FAST void l2p_line_invalidate(l2p_cache_s *cache, WORD idx)
{
    // 行作废时其 region 的 CMT 条目也必须失效：PFR 用本函数作废不在 CP bitmap
    // 的 CLEAN 行，若保留 CMT 会把旧映射继续返回给 host。
    if (cache->tags[idx].state != L2P_CACHE_INVALID)
    {
        // 契约「仅 CLEAN/INVALID」：DIRTY/FLUSHING 行作废会静默丢弃
        // 未落盘映射 —— 调用者自持下标与状态，debug 停在现场。
        ASSERT(cache->tags[idx].state == L2P_CACHE_CLEAN);
        // 只清 CLEAN；boot replay 期 CMT 已停用，不应有 dirty 条目。
        l2p_cmt_invalidate_region(cache, cache->tags[idx].region_id, 0U, 0U, false);
    }

    l2p_line_hint_clear(cache, idx);
    cache->tags[idx].state      = (BYTE)L2P_CACHE_INVALID;
    cache->lines[idx].degraded  = false;
    cache->lines[idx].prefetch  = false;
    cache->lines[idx].dirty_seq = 0U; /* 行内容作废：不再有 Journal 依赖 */
}

/* ── owner ── */

/* 后台主动写回 CMT 的 watermark（本仓自定）：留 25% 槽位给突发写，
 * 低于水位时只由容量/eviction、host FLUSH、CP 强制写回，保留按
 * translation page 合并的机会，降低写放大。 */

/**
 * @brief 全局 region → owner FTL 实例（设备级注入的解析回调）。
 *
 * @param cache         cache 上下文
 * @param global_region 全局 region
 * @return owner 实例（恒非空；owner 表与 topology 的一致性由 ftl_contract_verify 在 boot 期证明）。
 *
 * @note 共享 cache 的所有 metadata I/O 都以 owner 的目录/分配器为准。
 */
static const l2p_owner_s *l2p_owner_get(const l2p_cache_s *cache, DWORD global_region)
{
    return cache->owner_of_region(cache->owner_ctx, global_region);
}

/**
 * @brief 按 owner 踢写回（先 dirty CMT 整页，再 DIRTY line）
 *
 * CMT 仅在 force_cmt 或达到写回水位时主动搬；每轮最多 L2P_CMT_WB_MAX 个
 * region。line 最多 max_kick 条。遇 AGAIN 停止该分支，硬失败行跳过。
 *
 * @param cache     cache
 * @param owner_die owner die
 * @param max_kick  本轮最多提交条数
 */
/**
 * @brief 判断本轮是否允许启动 CMT 整页写回（**不含 thrashing 门**）。
 *
 * thrashing 只作观测/预取判据，不得挡住必需的写回；L2P-17 的回归点就在这里。
 */
static BOOL l2p_owner_flush_is_ready(const l2p_cache_s *cache, BYTE owner_die, BOOL force_cmt)
{
    return ((cache->cmt_enabled != false) && (cache->cmt_dirty_owner[owner_die] > 0U) && (cache->inflight.flush < L2P_FLUSH_MAX_INFLIGHT) &&
            (force_cmt || (cache->stats.cmt_dirty >= L2P_CMT_WB_WATERMARK)));
}

/**
 * @brief 全局 region → owner die 端口的唯一解析点
 *
 * @param ctx          未使用（设备级共享 cache 只有一个 owner 表）
 * @param global_region 全局 region
 * @return owner 端口；未登记的 die 即 boot 顺序错误
 */
static const l2p_owner_s *s_owner_of_region(void *ctx, DWORD global_region)
{
    (void)ctx;

    ASSERT(DIE_MAX != 0U);

    const BYTE die = (BYTE)(global_region % DIE_MAX);

    ASSERT(die < FTL_DIE_MAX);
    ASSERT(s_l2p_owner[die].metadata_dir != NULL);

    return &s_l2p_owner[die];
}

/**
 * @brief 登记某个 die 的 owner 端口（boot 时由 ftl_instance_init 调用）
 *
 * @param die   紧凑 die 号（< FTL_DIE_MAX）
 * @param owner owner 端口内容（非空；按值拷贝）
 */
void l2p_owner_register(BYTE die, const l2p_owner_s *owner)
{
    ASSERT(die < FTL_DIE_MAX);
    ASSERT(owner != NULL);

    s_l2p_owner[die] = *owner;
}

/**
 * @brief 作废一个 owner 的全部行（格式化 / 重建）
 *
 * 恢复期不应有在飞命令；FLUSHING/LOADING 行重写会泄漏在飞计数 ⇒ ASSERT。
 *
 * @param cache     cache
 * @param owner_die owner die
 */
SSD_CODE_FAST void l2p_owner_invalidate(l2p_cache_s *cache, BYTE owner_die)
{
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_s *line = &cache->lines[i];

        if (L2P_TAG(cache, line)->state == L2P_CACHE_INVALID)
        {
            continue; /* 空行：region_id 无语义，无需处理 */
        }
        if (l2p_region_owned_by(L2P_TAG(cache, line)->region_id, owner_die) == false)
        {
            continue; /* 别的 LUN 的行，不能碰 */
        }

        ASSERT((L2P_TAG(cache, line)->state == L2P_CACHE_CLEAN) || (L2P_TAG(cache, line)->state == L2P_CACHE_DIRTY));

        memset(line->entries, 0xFF, sizeof(line->entries));
        l2p_line_hint_clear(cache, i);
        cache->tags[i].region_id    = 0U;
        cache->tags[i].state        = L2P_CACHE_INVALID;
        cache->tags[i].access_count = 0U;
        line->fresh                 = false;
        line->prefetch              = false;
        line->dirty_seq             = 0U;
#if SSD_STATS_ENABLE
        line->load_timed = false;
#endif
    }

    // 清本 owner 的全局脏位：local 0..L2P_REGIONS_PER_INSTANCE-1
    // → global = local * DIE_MAX + owner_die。
    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const DWORD global_region = l2p_local_to_global_region(local, owner_die);

        l2p_bitmap_clear(cache->region_dirty_bitmap, global_region);
        l2p_bitmap_clear(cache->region_degraded_bitmap, global_region);
    }

    l2p_cmt_invalidate_owner(cache, owner_die);
}

/**
 * @brief 按 owner 过滤并截取本 die 的局部脏位图
 *
 * Checkpoint 起点调用（先快照再 flush）；DIE_MAX=1 时等价于整份位图拷贝。
 *
 * @param cache     cache
 * @param owner_die owner die
 * @param out       输出：本 die 的局部位图
 */
SSD_CODE_FAST void l2p_owner_bitmap_snapshot(const l2p_cache_s *cache, BYTE owner_die, DWORD out[L2P_LOCAL_DIRTY_WORDS])
{
    memset(out, 0, L2P_LOCAL_DIRTY_WORDS * sizeof(DWORD));

    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const DWORD global = l2p_local_to_global_region(local, owner_die);

        if (l2p_bitmap_test_bit(cache->region_dirty_bitmap, global))
        {
            l2p_bitmap_set(out, local);
        }
    }
}

/**
 * @brief Checkpoint DONE 前的最终覆盖检查
 *
 * @param cache     cache
 * @param owner_die CP 的 die
 * @param snapshot  本 die 的局部脏位快照
 * @return true = 每个 snapshot 位都已 commit 或行已 CLEAN；false = 有未落盘位
 */
SSD_CODE_FAST BOOL l2p_owner_snapshot_is_covered(l2p_cache_s *cache, BYTE owner_die, const DWORD snapshot[L2P_LOCAL_DIRTY_WORDS])
{
    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        if (l2p_bitmap_test_bit(snapshot, local) == false)
        {
            continue;
        }

        const DWORD global = l2p_local_to_global_region(local, owner_die);

        if (l2p_bitmap_test_bit(cache->region_dirty_bitmap, global) == false)
        {
            continue; /* 本次 CP 已 commit（行可能已被驱逐/重载） */
        }

        const l2p_cache_line_s *line = l2p_line_find(cache, global);

        if ((line != NULL) && (L2P_TAG(cache, line)->state != L2P_CACHE_CLEAN))
        {
            return false; /* DIRTY / FLUSHING / LOADING：尚未落定 */
        }

        if ((line != NULL) && (l2p_cmt_region_dirty(cache, global) == false))
        {
            continue; /* CLEAN 且无 dirty CMT */
        }
        return false; /* 实时脏位仍在 / 无行但 dirty CMT 未清 */
    }
    return true;
}

SSD_CODE_FAST void l2p_owner_flush_kick(l2p_cache_s *cache, BYTE owner_die, BYTE max_kick, BOOL force_cmt)
{
    // 心跳（见 log.h / timer.h）：有在飞 region（load / flush）时每满 1s 打一行计数。
    // ⚠ 本模块没有"单 job"结构（在飞的是若干 cache line）⇒ 只报计数，不报 phase/age。
    if ((cache->inflight.flush == 0U) && (cache->inflight.load == 0U))
    {
        cache->job_log_ms = 0U; /* 无在飞：清基准 */
    }
    else if (log_tick(&cache->job_log_ms))
    {
        LOGI(LOG_TAG_L2P "job inflight load=%hhu flush=%hhu cmt=%lu reg=%hu\r\n", cache->inflight.load, cache->inflight.flush, cache->stats.cmt_dirty, l2p_dirty_count(cache));
    }

    // CMT dirty 是写缓存占用，优先按 translation page 整页写回。按 owner 计数
    // O(1) 判空；写回在飞/资源不足时本轮停手，由完成回调释放 credit。
    if (l2p_owner_flush_is_ready(cache, owner_die, force_cmt) != false)
    {
        // 强制路径（host FLUSH / IDLE 排空）可以吃满 flush credit；后台只起 2 个。
        const BYTE wb_budget = (force_cmt != false) ? L2P_FLUSH_MAX_NORMAL : L2P_CMT_WB_MAX;
        const BYTE wb_max    = (max_kick < wb_budget) ? max_kick : wb_budget;
        BYTE       started   = 0U;

        // 按 region 索引扫（2048 bit），不再扫 4096 个 CMT 条目；命中 dirty
        // region 就直接驱动整页写回。比逐条目扫描少一半迭代，也天然去重。
        for (DWORD local = 0U; (local < L2P_REGIONS_PER_INSTANCE) && (started < wb_max); local++)
        {
            const DWORD region = l2p_local_to_global_region(local, owner_die);

            if (l2p_cmt_region_dirty(cache, region) == false)
            {
                continue;
            }

            const l2p_cache_line_s *line = l2p_line_find(cache, region);

            if ((line != NULL) && ((L2P_TAG(cache, line)->state == L2P_CACHE_LOADING) || (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING)))
            {
                continue; /* 该 region 已在飞，换下一个 */
            }

            const ftl_status_e st = l2p_region_flush_kick(cache, region);

            if (st == FTL_OK)
            {
                started++;
            }
            else if (st == FTL_AGAIN)
            {
                // 无行可驱逐 / FC 背压：停止本轮 CMT 扫描，但仍要走下面的
                // line 写回——DIRTY line 腾出行后，下一轮才能继续搬 CMT。
                started = wb_max;
            }
            // FTL_ERROR（孤儿位/硬失败）：跳过，交给 CP 或下一轮暴露。
        }
    }

    for (WORD i = 0U; (i < L2P_CACHE_SIZE) && (max_kick > 0U); i++)
    {
        const l2p_cache_line_s *line = &cache->lines[i];

        // 只刷自己的行：共享 cache 里别的 LUN 的行由它们自己的 CP/flush 处理。
        if ((L2P_TAG(cache, line)->state != L2P_CACHE_DIRTY) || (l2p_region_owned_by(L2P_TAG(cache, line)->region_id, owner_die) == false))
        {
            continue;
        }

        const ftl_status_e st = l2p_flush_kick(cache, i);

        if (st == FTL_AGAIN)
        {
            return; /* 闸门满 / 池空：本轮到此为止，下轮再来 */
        }
        if (st == FTL_OK)
        {
            max_kick--;
        }
        // FTL_ERROR：本行无法写回（无依赖 / 无槽）——跳过它，试下一行
    }
}

/* ── load ── */

/**
 * @brief 失败原因名（日志用）
 *
 * @param cause 失败原因
 * @return 稳定的短名；未枚举值返回 "unknown"
 */
static const char *l2p_load_fail_name(l2p_load_fail_e cause)
{
    switch (cause)
    {
        case L2P_LOAD_FAIL_NO_CMD:
            return "no_command";
        case L2P_LOAD_FAIL_BUILD:
            return "build";
        case L2P_LOAD_FAIL_GATE:
            return "gate";
        case L2P_LOAD_FAIL_SUBMIT:
            return "submit";
        case L2P_LOAD_FAIL_INVALID_PRIMARY:
            return "invalid_primary_pba";
        case L2P_LOAD_FAIL_INVALID_BACKUP:
            return "invalid_backup_pba";
        default:
            return "unknown";
    }
}

/**
 * @brief 一次加载失败的统一结算（重试预算 → 降级）
 *
 * 读完成失败与「Directory PBA 损坏、根本没发出读」两条路径共用：重试
 * L2P_LOAD_RETRY_MAX 次后把行降级为全未映射 CLEAN，并置 cache 级降级位
 * （host 读**未映射** LBA 时返回 IO_ERROR，不零填）。两条路径必须逐字同口径
 * ——否则一条路径降级、另一条永远重试。
 *
 * @param cache cache 上下文
 * @param line  失败的行（region_id 已设置）
 */
static void l2p_load_fail_settle(l2p_cache_s *cache, l2p_cache_line_s *line)
{
    memset(line->entries, 0xFF, sizeof(line->entries));
    line->load_fail_count++;

    if (line->load_fail_count < L2P_LOAD_RETRY_MAX)
    {
        l2p_line_hint_clear(cache, (WORD)(line - cache->lines));
        L2P_TAG(cache, line)->state = L2P_CACHE_INVALID;
        line->prefetch              = false;
    }
    else
    {
        STATS_INC(cache->stats.degrade);
        L2P_TAG(cache, line)->state = L2P_CACHE_CLEAN;
        line->fresh                 = true;
        line->prefetch              = false;
        line->degraded              = true;
        // 记到 cache 级位图：行被驱逐/重载后仍能识别“映射已丢”。
        l2p_bitmap_set(cache->region_degraded_bitmap, L2P_TAG(cache, line)->region_id);
        l2p_degrade_log(L2P_TAG(cache, line)->region_id, line->load_fail_count);
    }
}

/**
 * @brief 打一条「加载启动失败」限流日志。
 *
 * @param region_id 加载失败的全局 region
 * @param cause     失败原因（枚举，打印名见 `l2p_load_fail_name`）
 */
static SSD_CODE_FAST void l2p_load_fail_log(DWORD region_id, l2p_load_fail_e cause)
{
    // gate / submit 背压是**预期**等待：统一 10 s 一行的慢节拍，避免 host 大顺序读写
    // + GC 期间刷屏；其余失败（含 no_command）仍按 1 s，便于把真正的资源耗尽与
    // 序列门控分开看。
    const BOOL  backpressure = (cause == L2P_LOAD_FAIL_GATE) || (cause == L2P_LOAD_FAIL_SUBMIT);
    const DWORD interval     = (backpressure != false) ? L2P_LOAD_FAIL_LOG_INTERVAL_MS : LOG_RATE_INTERVAL_MS;

    LOGW_RATE(log_rate_due(&s_load_fail_ms, &s_load_fail_n, interval), s_load_fail_n, LOG_TAG_L2P "load start failed region=%lu cause=%s", region_id, l2p_load_fail_name(cause));
}

/**
 * @brief 提交一次 region 页读：首次加载与「主副本失败 → 备份副本」共用。
 * 成功：行置 LOADING、load_inflight++、记录本次 PBA；
 * 失败：行回到 INVALID（调用者下轮重试），返回 false。
 *
 * @param cache       cache 上下文
 * @param line        目标 cache 行
 * @param pba         要读取的 region 页物理地址
 * @param log_failure true = 失败时打限流日志（预取路径传 false）
 * @return true = 读命令已提交、行进入 LOADING
 *         false = 未提交，行回到 INVALID
 */
static BOOL l2p_load_submit(l2p_cache_s *cache, l2p_cache_line_s *line, DWORD pba, BOOL log_failure)
{
    fc_cmd_s  *cmd      = NULL;
    const WORD line_idx = (WORD)(line - cache->lines);

    line->load_pba = pba;

    // ⚠ 读的数据**留在 desc 链上**（与写不对称）⇒ 目标缓冲须活到完成、
    // ftl_cmd_read_data 须早于 release —— 二者都在 ftl_l2p_op_callback 里，缓冲就是 line->entries
    // （行在 LOADING 期间不可驱逐、不可写 ⇒ 地址稳定）。
    /* demand load 是 GC / 写确认的前台依赖：用 CRIT 额度（命令池最后 2 槽）构造；
     * 预取加载是后台优化，保持 BG，不能占 critical 额度。 */
    const fc_cmd_class_e cls   = (line->prefetch != false) ? FC_CMD_CLASS_BG : FC_CMD_CLASS_CRIT;
    const fc_cmd_class_e saved = ftl_cmd_class_set(cls);
    ftl_cmd_lease_s      lease;
    ftl_status_e         st = FTL_AGAIN;

    // demand load 给 CP/metadata 留保底；预取是 BG 额度，不占 CRIT headroom。
    const BOOL demand_headroom = (line->prefetch != false) || (l2p_fc_demand_headroom_is_sufficient() != false);

    if ((demand_headroom != false) && (ftl_cmd_lease_claim(&lease, cls) != false))
    {
        st = ftl_async_read_page_leased(&cmd, &lease, pba, FTL_PAGE_CFG_META());
        if (st != FTL_OK)
        {
            ftl_cmd_lease_release(&lease);
        }
    }

    (void)ftl_cmd_class_set(saved);

    if (st != FTL_OK)
    {
        /* FTL_AGAIN = 命令槽额度不足（含 CRIT 额度）；FTL_ERROR = 地址/desc/buffer
         * 建链失败。资源池的细分口径由 FC 的 fc.alloc.* 统计负责，本层不再重算。 */
        const l2p_load_fail_e reason = (st == FTL_AGAIN) ? L2P_LOAD_FAIL_NO_CMD : L2P_LOAD_FAIL_BUILD;

        if (reason == L2P_LOAD_FAIL_NO_CMD)
        {
            STATS_INC(cache->stats.load_fail_no_cmd);
        }
        else
        {
            STATS_INC(cache->stats.load_fail_build);
        }

        l2p_line_hint_clear(cache, line_idx);
        L2P_TAG(cache, line)->state = L2P_CACHE_INVALID; /* 回滚占位（区域未加载，等于没占用） */
        line->prefetch              = false;
        if (log_failure)
        {
            l2p_load_fail_log(L2P_TAG(cache, line)->region_id, reason);
        }
        return false;
    }

    const ftl_op_id_e op = (line->prefetch != false) ? FTL_OP_L2P_PREFETCH : FTL_OP_L2P;

    if (ftl_op_submit(cmd, op, line) != FTL_OK)
    {
        const BOOL gate_blocked = (ftl_op_gate_is_blocking(cmd) != false);

#if SSD_STATS_ENABLE
        if (gate_blocked != false)
        {
            STATS_INC(cache->stats.load_fail_gate);
        }
        else
        {
            STATS_INC(cache->stats.load_fail_submit);
        }
#endif
        (void)fc_command_release(cmd->cmd_id);
        l2p_line_hint_clear(cache, line_idx);
        L2P_TAG(cache, line)->state = L2P_CACHE_INVALID;
        line->prefetch              = false;
        if (log_failure)
        {
            l2p_load_fail_log(L2P_TAG(cache, line)->region_id, (gate_blocked != false) ? L2P_LOAD_FAIL_GATE : L2P_LOAD_FAIL_SUBMIT);
        }
        return false;
    }

    STATS_INC(cache->stats.load_start);
#if SSD_STATS_ENABLE
    line->load_timed = delay_time_now_us(&line->load_start_us);
#endif
    L2P_TAG(cache, line)->state = L2P_CACHE_LOADING;
    cache->inflight.load++;
    return true;
}

/**
 * @brief 为一个 region 建立加载状态并提交读命令（或直接合成未映射行）。
 *
 * 未映射（主备 PBA 均为 NAND_PBA_INVALID）⇒ 不提交 NAND 读，直接置全 INVALID 的 CLEAN 行。
 * ⚠ 「两份 PBA 都无效」与「两份 PBA 都损坏」必须分开：前者是合法未映射（零填），
 *   后者是位置丢失（按加载失败重试/降级，host 读 IO_ERROR）——混同会静默零填
 *
 * @param cache       cache 上下文
 * @param line        已选中的 cache 行
 * @param region_id   全局 region
 * @param log_failure true = 加载启动失败时打限流日志
 * @param prefetch    true = 预取加载（fresh 保持 false）
 * @return true = 已就绪（CLEAN）或读命令已提交（LOADING）
 *         false = 读命令提交失败 / PBA 损坏，行回到 INVALID 或已降级
 */
static BOOL l2p_load_begin(l2p_cache_s *cache, l2p_cache_line_s *line, DWORD region_id, BOOL log_failure, BOOL prefetch)
{
    const l2p_owner_s *owner        = l2p_owner_get(cache, region_id);
    DWORD              local_region = l2p_global_to_local_region(region_id);
    const WORD         line_idx     = (WORD)(line - cache->lines);

    // 占位：entries 先全 INVALID（加载失败时保持此态 = 全未映射）
    memset(line->entries, 0xFF, sizeof(line->entries));
    // 失败计数只在**换 region** 时清零：同一 region 的重试必须累计，否则
    // L2P_LOAD_RETRY_MAX 永远达不到（每次 load_start 都清零）。
    if (L2P_TAG(cache, line)->region_id != region_id)
    {
        line->load_fail_count = 0U;
    }
    l2p_line_hint_clear(cache, line_idx);
    L2P_TAG(cache, line)->region_id            = region_id;
    cache->line_hint[l2p_hint_slot(region_id)] = (BYTE)line_idx;
    L2P_TAG(cache, line)->access_count         = 1U;
    L2P_TAG(cache, line)->state                = L2P_CACHE_LOADING;
    line->dirty_seq                            = 0U;    /* 介质内容，无 Journal 依赖 */
    line->fresh                                = false; /* host 完成时置 true；预取完成时保持 false */
    line->degraded                             = false; /* 新一次加载尝试：降级标志只反映最近一次结果 */
    line->prefetch                             = prefetch;

    // ⚠ 不能因为 region 在 `region_degraded_bitmap` 里就跳过加载：降级只说明
    // **部分**旧映射丢了，region 页本身仍可能有效——journal 回放或 host 写入后
    // CP 会把它 flush 到新页，Directory 已指向新位置。整行短路成 INVALID 会让
    // 已恢复的映射在行驱逐后消失（host 误报 IO_ERROR，GC 还可能漏搬有效 CW）。
    // 位图只用于 host 读的「未映射 ⇒ IO_ERROR」判定。

    DWORD primary = md_entry_find(owner->metadata_dir, METADATA_TYPE_L2P_REGION, local_region);
    DWORD backup  = md_backup_find(owner->metadata_dir, local_region);
    BOOL  corrupt = false;

    // Directory 是介质读回（H29）：非 INVALID 也必须过 pba_is_valid 才能进 FC
    // （PBA 字段越界 ⇒ pba_to_ch/ce/lun 造出越界路由字段 ⇒ FC 的 suspended[][]
    // 越界）。损坏的副本按缺失处理，并记住「曾损坏」以区分合法未映射。
    if ((primary != NAND_PBA_INVALID) && (pba_is_valid(primary) == false))
    {
        l2p_load_fail_log(region_id, L2P_LOAD_FAIL_INVALID_PRIMARY);
        primary = NAND_PBA_INVALID;
        corrupt = true;
    }
    if ((backup != NAND_PBA_INVALID) && (pba_is_valid(backup) == false))
    {
        l2p_load_fail_log(region_id, L2P_LOAD_FAIL_INVALID_BACKUP);
        backup  = NAND_PBA_INVALID;
        corrupt = true;
    }

    if ((primary == NAND_PBA_INVALID) && (backup == NAND_PBA_INVALID))
    {
        if (corrupt == true)
        {
            // 位置损坏 ≠ 从未写过：按加载失败重试/降级，**不得**合成未映射行
            // （那会让 host 读零填，映射丢失变成静默数据丢失）。
            l2p_load_fail_settle(cache, line);
            return false;
        }

        // 未映射：该 region 从未写过，Directory 里没有它的位置。entries 已是全
        // NAND_PBA_INVALID（上面 memset），直接置 CLEAN 并返回——**不提交读命令**。
        // 主流 DRAM-less 的未映射路径同样不产生 NAND I/O；读"某个固定擦除页"会把
        // 介质的空白内容当成映射来源，凭空构造未写过的表项。
        // host：fresh=true（下一次命中是本次 miss 的重试，不计 hit）；
        // 预取：fresh=false（未来 host 命中是真实预取收益，保留 prefetch 标志到首次命中）。
        L2P_TAG(cache, line)->state = L2P_CACHE_CLEAN;
        line->fresh                 = (prefetch == false);
        return true;
    }

    // 主副本优先；主副本缺失/损坏（旧介质单副本 / 校验不过）时用备份。
    const BOOL  use_backup = (primary == NAND_PBA_INVALID);
    const DWORD pba        = use_backup ? backup : primary;

    line->load_use_backup = use_backup;

    return l2p_load_submit(cache, line, pba, log_failure);
}

/**
 * @brief 为 region 起一次懒加载：先选可驱逐行，再建加载状态。
 *
 * @param cache     cache 上下文
 * @param region_id 全局 region
 * @return true = 加载已起（或未映射直接 CLEAN）
 *         false = 无可驱逐行 / 提交失败
 */
static BOOL l2p_load_start(l2p_cache_s *cache, DWORD region_id)
{
    const l2p_owner_s *owner = l2p_owner_get(cache, region_id);
    ftl_sched_s       *sched = (owner != NULL) ? owner->sched : NULL;

    if ((sched != NULL) && (ftl_sched_l2p_load_begin(sched, region_id, false) == false))
    {
        return false; /* L2P_LOAD class credit / item 池满：调用者稍后重试 */
    }

    l2p_cache_line_s *line = l2p_line_evict(cache, true);

    if (line == NULL)
    {
        // 现场细节（各状态行数）由 l2p_evict_fail_log 打出，不再另发通用行。
        STATS_INC(cache->stats.evict_fail);
        l2p_evict_fail_log(cache, region_id);
        if (sched != NULL)
        {
            ftl_sched_l2p_load_finish(sched, region_id, false);
        }
        return false;
    }

    const BOOL started = l2p_load_begin(cache, line, region_id, true, false);

    // 未映射直接合成 CLEAN / 启动失败：本次没有异步完成可等，立即释放 item。
    if ((sched != NULL) && ((started == false) || (L2P_TAG(cache, line)->state != L2P_CACHE_LOADING)))
    {
        ftl_sched_l2p_load_finish(sched, region_id, false);
    }
    return started;
}

/* ── region ── */

/**
 * @brief 同步预载一个 region（按 Directory 的 PBA 直接读）
 *
 * 无空行返回 false（交给懒加载）。
 *
 * @param cache         cache
 * @param global_region 全局 region
 * @param primary       主副本 PBA
 * @param backup        备份副本 PBA（NAND_PBA_INVALID = 无）
 * @return FTL_OK 已载入/已降级；FTL_AGAIN 无空行；FTL_ERROR 地址非法/提交层失败
 */
SSD_CODE_FAST ftl_status_e l2p_region_preload(l2p_cache_s *cache, DWORD global_region, DWORD primary, DWORD backup)
{
    // global_region 来自调用者（PFR 的 local×DIE_MAX+die）；越界写入 tags[] 会让
    // 该行永远无法再被命中，只白占一行 ⇒ 运行期拒绝（H29 同口径）。
    if (global_region >= L2P_GLOBAL_REGIONS)
    {
        return FTL_ERROR;
    }

    // PBA 来自 Metadata Directory（介质读回，H29 不可信输入）⇒ 运行期拒绝而非 ASSERT。
    if ((pba_is_valid(primary) == false) && (pba_is_valid(backup) == false))
    {
        return FTL_ERROR;
    }

    // 已有驻留行（journal 回放的 DIRTY / 已预载）⇒ 维持「一 region 一行」。
    if (l2p_line_find(cache, global_region) != NULL)
    {
        return FTL_OK;
    }

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_s *line = &cache->lines[i];

        if (L2P_TAG(cache, line)->state != L2P_CACHE_INVALID)
        {
            continue;
        }

        // 先写 region_id 再读：读失败时置 INVALID，find 会忽略该行，
        // 后续预载/懒加载可安全复用。旧 hint 先清，避免指向已复用的下标。
        l2p_line_hint_clear(cache, i);
        L2P_TAG(cache, line)->region_id = global_region;

        BOOL completion_failed = false;

        // 主/备交替重试：完成态错误可按 L2P 加载重试语义收敛，提交层硬失败
        // （completion.status == 0 的 FTL_ERROR）则绝不能降级掩盖。
        for (BYTE attempt = 0U; attempt < L2P_LOAD_RETRY_MAX; attempt++)
        {
            const BOOL  use_primary = ((attempt & 1U) == 0U) || (pba_is_valid(backup) == false);
            const DWORD pba         = use_primary ? primary : backup;

            if (pba_is_valid(pba) == false)
            {
                continue;
            }

            fc_cmd_status_s comp;

            memset(&comp, 0, sizeof(comp));

            if (ftl_sync_read_page_status(pba, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META(), &comp) == FTL_OK)
            {
                L2P_TAG(cache, line)->state                    = L2P_CACHE_CLEAN;
                cache->line_hint[l2p_hint_slot(global_region)] = (BYTE)i;
                line->fresh                                    = false; /* 预载不是本次访问的加载 */
                line->degraded                                 = false;
                line->prefetch                                 = false;
                line->dirty_seq                                = 0U; /* 介质内容，无 Journal 依赖 */
                line->load_fail_count                          = 0U;
                return FTL_OK;
            }

            if (fc_cmd_status_has_error(comp.status) == false)
            {
                /* 建链/提交层失败（没有 CQE 错误位）：快照位置不可信，交给 PFR 回退。 */
                L2P_TAG(cache, line)->state = L2P_CACHE_INVALID;
                line->degraded              = false;
                line->prefetch              = false;
                return FTL_ERROR;
            }

            completion_failed = true;
        }

        if (completion_failed == false)
        {
            L2P_TAG(cache, line)->state = L2P_CACHE_INVALID;
            line->degraded              = false;
            line->prefetch              = false;
            return FTL_ERROR;
        }

        /* 主/备都持续完成态失败：按范围化恢复降级为全 INVALID CLEAN 行。
         * 该 region 的旧映射丢失，但其它 region 的快照仍可用。 */
        memset(line->entries, 0xFF, sizeof(line->entries));
        L2P_TAG(cache, line)->state                    = L2P_CACHE_CLEAN;
        cache->line_hint[l2p_hint_slot(global_region)] = (BYTE)i;
        line->fresh                                    = false;
        line->degraded                                 = true;
        line->prefetch                                 = false;
        line->dirty_seq                                = 0U;
        line->load_fail_count                          = L2P_LOAD_RETRY_MAX;
        l2p_bitmap_set(cache->region_degraded_bitmap, global_region);
        return FTL_OK;
    }

    return FTL_AGAIN; /* 无 INVALID 行（全 DIRTY/在飞）：交给懒加载 */
}

/**
 * @brief 异步预取一个 region（best-effort）
 *
 * thrashing / 在飞上限 / host 预算不足时跳过；已驻留直接返回 true。
 *
 * @param cache         cache
 * @param global_region 全局 region
 * @return true 已驻留或已起加载；false 跳过
 */
BOOL l2p_region_prefetch(l2p_cache_s *cache, DWORD global_region)
{
    ASSERT(cache != NULL);

    cache->prefetch.calls++;

    if (global_region >= L2P_GLOBAL_REGIONS)
    {
        cache->prefetch.skipped++;
        return false;
    }

    // 同一 region 只允许一行；已驻留/LOADING/FLUSHING 都不再起第二条命令。
    if (l2p_line_find(cache, global_region) != NULL)
    {
        cache->prefetch.resident++;
        return true;
    }

    // 预取是 best-effort：不制造写回、不与 host 抢资源、不加剧 thrashing。
    if (cache->thrash.thrashing || (cache->inflight.load >= L2P_PREFETCH_MAX_INFLIGHT) || (ftl_fc_host_budget_ok() == false))
    {
        cache->prefetch.skipped++;
        return false;
    }

    // 预取是 best-effort：登记独立 L2P_LOAD item，不吃 demand load 的 credit。
    const l2p_owner_s *owner = l2p_owner_get(cache, global_region);
    ftl_sched_s       *sched = (owner != NULL) ? owner->sched : NULL;

    if ((sched != NULL) && (ftl_sched_l2p_load_begin(sched, global_region, true) == false))
    {
        cache->prefetch.skipped++;
        return false;
    }

    // 预取只选 INVALID/CLEAN，遇 DIRTY 跳过且不 kick 写回。
    l2p_cache_line_s *line = l2p_line_evict(cache, false);

    if (line == NULL)
    {
        if (sched != NULL)
        {
            ftl_sched_l2p_load_finish(sched, global_region, true);
        }
        cache->prefetch.skipped++;
        return false;
    }

    const BOOL started = l2p_load_begin(cache, line, global_region, false, true);

    // 未映射直接合成 CLEAN / 启动失败：没有异步完成可等，立即释放 item。
    if ((sched != NULL) && ((started == false) || (L2P_TAG(cache, line)->state != L2P_CACHE_LOADING)))
    {
        ftl_sched_l2p_load_finish(sched, global_region, true);
    }

    if (started == false)
    {
        cache->prefetch.skipped++;
        return false;
    }

    cache->prefetch.started++;
    return true;
}

/**
 * @brief 启动一个 translation page 的 CMT 整页写回
 *
 * @param cache  cache
 * @param region 全局 region
 * @return FTL_OK 已提交；FTL_AGAIN 加载/写回在飞或背压；FTL_ERROR 硬失败
 */
SSD_CODE_FAST ftl_status_e l2p_region_flush_kick(l2p_cache_s *cache, DWORD region)
{
    l2p_cache_line_s *line = l2p_line_find(cache, region);

    const BOOL cmt_dirty  = l2p_cmt_region_dirty(cache, region);
    const BOOL live_dirty = l2p_region_dirty_live(cache, region);

    if (line != NULL)
    {
        const BYTE state = L2P_TAG(cache, line)->state;

        if (state == L2P_CACHE_FLUSHING)
        {
            return FTL_AGAIN;
        }
        if (state == L2P_CACHE_LOADING)
        {
            if (cmt_dirty == false)
            {
                l2p_bitmap_clear(cache->cmt_dirty_region_bitmap, region); /* 索引位自愈 */
                return (live_dirty != false) ? FTL_ERROR : FTL_AGAIN;
            }
            return FTL_AGAIN;
        }
        if ((state == L2P_CACHE_CLEAN) && (cmt_dirty == false))
        {
            l2p_bitmap_clear(cache->cmt_dirty_region_bitmap, region); /* 索引位自愈 */
            // 已 commit（实时位已清）⇒ OK；实时位未清但无数据可写 ⇒ 孤儿位。
            return (live_dirty != false) ? FTL_ERROR : FTL_OK;
        }
        return l2p_flush_kick(cache, (int)(line - cache->lines));
    }

    if (cmt_dirty == false)
    {
        l2p_bitmap_clear(cache->cmt_dirty_region_bitmap, region); /* 索引位自愈 */
        return (live_dirty != false) ? FTL_ERROR : FTL_OK;
    }

    // CMT 脏但 line 不在：这是一次**正确性依赖**的加载（要拿到旧页合并 CMT），
    // 必须和 host demand load 走同一个 scheduler credit；否则 flush 路径会绕开
    // credit 无限起加载（现场 cache load=22 vs credit 8+2），把 FC 命令/buffer
    // 占满，后续 load 建链失败 → host 读组无进展。登记后完成口也能正确结算 item。
    const l2p_owner_s *owner = l2p_owner_get(cache, region);
    ftl_sched_s       *sched = (owner != NULL) ? owner->sched : NULL;

    if ((sched != NULL) && (ftl_sched_l2p_load_begin(sched, region, false) == false))
    {
        cache->stats.cmt_wb_defer++;
        return FTL_AGAIN; /* load credit / item 池满：保持 READY，下轮再试 */
    }

    line = l2p_line_evict(cache, true);
    if (line == NULL)
    {
        cache->stats.cmt_wb_defer++;
        if (sched != NULL)
        {
            ftl_sched_l2p_load_finish(sched, region, false);
        }
        return FTL_AGAIN;
    }

    const BOOL started = l2p_load_begin(cache, line, region, true, false);

    // 未映射直接合成 CLEAN / 启动失败：没有异步完成可等，立即释放 item。
    if ((sched != NULL) && ((started == false) || (L2P_TAG(cache, line)->state != L2P_CACHE_LOADING)))
    {
        ftl_sched_l2p_load_finish(sched, region, false);
    }

    if (started == false)
    {
        cache->stats.cmt_wb_defer++;
        return FTL_AGAIN;
    }

    // Directory 无页时 load_begin 直接合成 CLEAN：当场合并并提交写回。
    if (L2P_TAG(cache, line)->state == L2P_CACHE_CLEAN)
    {
        return l2p_flush_kick(cache, (int)(line - cache->lines));
    }
    return FTL_OK; /* LOADING：完成后下一轮再合并写回 */
}

/**
 * @brief 取 region 当前行状态（不更新 hint / 命中统计）
 *
 * @param cache  cache
 * @param region 全局 region
 * @return 行状态；region 未驻留返回 L2P_CACHE_INVALID
 */
l2p_cache_state_e l2p_region_state_get(const l2p_cache_s *cache, DWORD region)
{
    if (region >= L2P_GLOBAL_REGIONS)
    {
        return L2P_CACHE_INVALID;
    }

    // 先查 hint（与 l2p_line_find 同一索引），只读、不更新 hint/命中统计：
    // scheduler 每轮对每个 READY flush item 都要问一次，线性扫描 64 行太贵。
    const WORD slot = l2p_hint_slot(region);
    const BYTE hint = cache->line_hint[slot];

    if (hint < L2P_CACHE_SIZE)
    {
        const l2p_tag_s *tag = &cache->tags[hint];

        if ((tag->region_id == region) && (tag->state != L2P_CACHE_INVALID))
        {
            return (l2p_cache_state_e)tag->state;
        }
    }

    // hint 空/冲突/陈旧：线性兜底，tags[] 仍是唯一权威。
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        const l2p_tag_s *tag = &cache->tags[i];

        if ((tag->region_id == region) && (tag->state != L2P_CACHE_INVALID))
        {
            return (l2p_cache_state_e)tag->state;
        }
    }
    return L2P_CACHE_INVALID;
}

/* ── note ── */

/**
 * @brief 记录本行映射依赖的 Journal 序号上界（WAL 次序）
 *
 * @param cache     cache
 * @param line      目标行
 * @param region_id 全局 region（用于解析 owner 的 Journal）
 */
static void l2p_note_dirty_seq(l2p_cache_s *cache, l2p_cache_line_s *line, DWORD region_id)
{
    line->dirty_seq = l2p_dirty_seq(cache, region_id);
}

/**
 * @brief 把 region 标记为「映射已丢」（degraded）：host 读其**未映射** LBA 报 IO_ERROR。
 *
 * @param cache         共享 L2P cache。
 * @param global_region 全局 region 号。
 * @param lba           失败页的页对基址（全局 LBA）。
 * @param cw_bitmap     失败页的 CW 位图（bit0..7 对应 lba+0..lba+7）。
 *
 * @note 唯一调用者是 cache 写序列的失败撤销：已回执的页被器件 FAILC
 *       判失败时，把该页 LBA 从 L2P 作废，再用本标记让读**可见地失败**，而不是零填。
 *       CLEAN CMT 全部失效；DIRTY 只清失败页 LBA 范围（其余是同 region 更早的成功写）。
 */
SSD_CODE_FAST void l2p_note_region_lost(l2p_cache_s *cache, DWORD global_region, DWORD lba, DWORD cw_bitmap)
{
    if ((cache == NULL) || (global_region >= L2P_GLOBAL_REGIONS))
    {
        return;
    }

    l2p_bitmap_set(cache->region_degraded_bitmap, global_region);

    // CLEAN 条目全部失效；DIRTY 只清失败页 LBA 范围（它们指向失败页），
    // 同 region 其他 dirty 是更早的成功写，保留。
    l2p_cmt_invalidate_region(cache, global_region, lba, cw_bitmap & ((1U << FTL_LBA_PER_PAIR) - 1U), true);

    // 失败页的 dirty 若已是该 region 最后一批未落盘数据，清掉实时脏位，
    // 避免 Checkpoint 把「无 dirty」的 region 当成未覆盖的孤儿。
    // 注意：CLEAN line 不持有 dirty，只有 DIRTY/FLUSHING/LOADING line 需要保留位。
    if (l2p_cmt_region_dirty(cache, global_region) == false)
    {
        const l2p_cache_line_s *line = l2p_line_find(cache, global_region);

        const BYTE state = (line != NULL) ? L2P_TAG(cache, line)->state : L2P_CACHE_INVALID;

        if ((state != L2P_CACHE_DIRTY) && (state != L2P_CACHE_FLUSHING))
        {
            l2p_bitmap_clear(cache->region_dirty_bitmap, global_region);
        }
    }
}

/* ── entries ── */

/**
 * @brief 批量读页对内多个 IU 的映射
 *
 * @param cache cache
 * @param lba   页对基址（FTL_LBA_PER_PAIR 对齐）
 * @param mask  逐 IU 掩码
 * @param out   输出：PBA 数组
 * @return FTL_OK；FTL_AGAIN 行在飞；FTL_ERROR 越界
 */
ftl_status_e l2p_entries_read(l2p_cache_s *cache, DWORD lba, DWORD mask, DWORD out[])
{
    mask &= ((1U << FTL_LBA_PER_PAIR) - 1U);

    if (mask == 0U)
    {
        return FTL_OK;
    }
    if (((QWORD)lba + FTL_LBA_PER_PAIR) > L2P_GLOBAL_LBA_MAX)
    {
        return FTL_ERROR;
    }

    const DWORD region_id = lba_to_global_region(lba);
    const WORD  base_idx  = lba_to_entry(lba);

    l2p_cache_line_s *line = l2p_line_find(cache, region_id);

    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((mask & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD l = lba + bit;

        if (cache->cmt_enabled != false)
        {
            const l2p_cmt_entry_s *e = l2p_cmt_find(cache, l, true);

            if (e != NULL)
            {
                out[bit] = e->pba;
                continue;
            }
        }

        if ((line == NULL) || (L2P_TAG(cache, line)->state == L2P_CACHE_LOADING))
        {
            if (line == NULL)
            {
                cache->stats.miss_count++;
                l2p_access_note(cache, true);
                (void)l2p_load_start(cache, region_id);
            }
            return FTL_AGAIN;
        }

        l2p_hit_note(cache, (l2p_cache_line_s *)line);
        out[bit] = line->entries[base_idx + bit];
    }

    return FTL_OK;
}

/**
 * @brief 批量写页对内多个 IU 的映射
 *
 * 失败不产生半个状态；lba 必须按 FTL_LBA_PER_PAIR 对齐。
 *
 * @param cache cache
 * @param lba   页对基址
 * @param mask  逐 IU 掩码
 * @param pba   新 PBA 数组
 * @return FTL_OK；FTL_AGAIN 背压；FTL_ERROR 越界
 */
ftl_status_e l2p_entries_write(l2p_cache_s *cache, DWORD lba, DWORD mask, const DWORD pba[])
{
    mask &= ((1U << FTL_LBA_PER_PAIR) - 1U);

    if (mask == 0U)
    {
        return FTL_OK;
    }
    if (((QWORD)lba + FTL_LBA_PER_PAIR) > L2P_GLOBAL_LBA_MAX)
    {
        return FTL_OK; /* 越界 no-op，与 l2p_write 一致 */
    }

    const DWORD region_id = lba_to_global_region(lba);
    const WORD  base_idx  = lba_to_entry(lba);

    l2p_cache_line_s *line = l2p_line_find(cache, region_id);

    if ((line != NULL) && ((L2P_TAG(cache, line)->state == L2P_CACHE_LOADING) || (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING)))
    {
        return FTL_AGAIN;
    }

    const DWORD seq = l2p_dirty_seq(cache, region_id);

    if (cache->cmt_enabled == false)
    {
        if (line == NULL)
        {
            return FTL_AGAIN; /* replay 旁路仍要求 line */
        }

        for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
        {
            if ((mask & (1U << bit)) == 0U)
            {
                continue;
            }
            l2p_hit_note(cache, line);
            line->entries[base_idx + bit] = pba[bit];
        }

        L2P_TAG(cache, line)->state = L2P_CACHE_DIRTY;
        l2p_bitmap_set(cache->region_dirty_bitmap, region_id);
        l2p_note_dirty_seq(cache, line, region_id);
        return FTL_OK;
    }

    // 第一遍只判定：无 line 且 CMT 缺项 ⇒ 起加载；CLEAN line 上缺槽 ⇒ 整批 line 回退。
    BOOL need_line_fallback = false;

    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((mask & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD l = lba + bit;

        if (l2p_cmt_find(cache, l, false) != NULL)
        {
            continue;
        }
        if (line == NULL)
        {
            cache->stats.miss_count++;
            l2p_access_note(cache, true);
            (void)l2p_load_start(cache, region_id);
            return FTL_AGAIN;
        }
        if (L2P_TAG(cache, line)->state == L2P_CACHE_DIRTY)
        {
            continue; /* line 自己持有 dirty */
        }
        if (l2p_cmt_has_room(cache, l) == false)
        {
            need_line_fallback = true;
            break;
        }
    }

    if (need_line_fallback)
    {
        // 批量一次全走 line，避免部分 CMT 插入后失败；CMT 已有条目保持 dirty 状态，
        // 只同步值（flush merge 以 CMT dirty 为准，不能让它覆盖新值）。
        for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
        {
            if ((mask & (1U << bit)) == 0U)
            {
                continue;
            }

            const DWORD      l = lba + bit;
            l2p_cmt_entry_s *e = l2p_cmt_find(cache, l, false);

            l2p_hit_note(cache, line);
            line->entries[base_idx + bit] = pba[bit];

            if (e != NULL)
            {
                cache->stats.cmt_wr_hits++;
                if (e->state == L2P_CMT_DIRTY)
                {
                    e->pba       = pba[bit];
                    e->dirty_seq = seq;
                    l2p_cmt_touch(cache, e);
                }
                else
                {
                    // 值现在由 line 持有；CLEAN 缓存（页值）必须失效，读走 line。
                    l2p_cmt_drop(cache, e);
                }
            }
            else
            {
                cache->stats.cmt_wr_misses++;
            }
            /* 无 CMT 条目：只写 line，不创建 CLEAN（line 值不是页上真值）。 */
        }

        L2P_TAG(cache, line)->state = L2P_CACHE_DIRTY;
        l2p_bitmap_set(cache->region_dirty_bitmap, region_id);
        l2p_note_dirty_seq(cache, line, region_id);
        (void)l2p_cmt_room_reserve(cache, lba);
        return FTL_OK;
    }

    BOOL line_written  = false;
    BOOL dirty_written = false;

    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((mask & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD l = lba + bit;

        if (line != NULL)
        {
            l2p_hit_note(cache, line);
        }
        else
        {
            cache->stats.hit_count++;
            l2p_access_note(cache, false);
        }

        if (l2p_cmt_find(cache, l, false) != NULL)
        {
            cache->stats.cmt_wr_hits++;
            if (l2p_cmt_install_dirty(cache, l, pba[bit], seq))
            {
                dirty_written = true;
            }
        }
        else if ((line != NULL) && (L2P_TAG(cache, line)->state == L2P_CACHE_DIRTY))
        {
            cache->stats.cmt_wr_misses++;
            line->entries[base_idx + bit] = pba[bit];
            line_written                  = true;
            dirty_written                 = true;
            /* line 持有 dirty；不创建 CLEAN CMT（见 l2p_cmt_update_clean 契约）。 */
        }
        else
        {
            cache->stats.cmt_wr_misses++;
            if (l2p_cmt_install_dirty(cache, l, pba[bit], seq))
            {
                dirty_written = true;
            }
        }
    }

    // 只有确实更新了 line/CMT 才置脏位；避免未来调用点绕过 ready 时留下孤儿位。
    if (dirty_written)
    {
        l2p_bitmap_set(cache->region_dirty_bitmap, region_id);
    }
    if (line_written)
    {
        l2p_note_dirty_seq(cache, line, region_id);
    }
    return FTL_OK;
}

/**
 * @brief 批量写确认的就绪探针（旧值来源 + CMT 容量一次判完）
 *
 * @param cache cache
 * @param lba   页对基址（全局 LBA，FTL_LBA_PER_PAIR 对齐）
 * @param mask  条目掩码
 * @return true = 整批可写；false = 已起加载/写回，下一轮重试
 */
BOOL l2p_entries_is_ready(l2p_cache_s *cache, DWORD lba, DWORD mask)
{
    mask &= ((1U << FTL_LBA_PER_PAIR) - 1U);

    if (mask == 0U)
    {
        return true;
    }
    if (((QWORD)lba + FTL_LBA_PER_PAIR) > L2P_GLOBAL_LBA_MAX)
    {
        return true; /* 越界：写是 no-op，与 l2p_is_ready 同口径 */
    }

    const DWORD       region = lba_to_global_region(lba);
    l2p_cache_line_s *line   = l2p_line_find(cache, region);

    if ((line != NULL) && ((L2P_TAG(cache, line)->state == L2P_CACHE_LOADING) || (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING)))
    {
        return false;
    }
    if (cache->cmt_enabled == false)
    {
        if (line != NULL)
        {
            return true; /* replay 旁路：只写 line */
        }
        cache->stats.miss_count++;
        l2p_access_note(cache, true);
        (void)l2p_load_start(cache, region);
        return false;
    }

    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((mask & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD l = lba + bit;

        if (l2p_cmt_find(cache, l, false) != NULL)
        {
            continue; /* 原地更新，不需要新槽位 */
        }
        if (line == NULL)
        {
            cache->stats.miss_count++;
            l2p_access_note(cache, true);
            (void)l2p_load_start(cache, region);
            return false;
        }
        if (L2P_TAG(cache, line)->state == L2P_CACHE_DIRTY)
        {
            continue; /* line 自己持有 dirty */
        }
        if (l2p_cmt_room_reserve(cache, l) == false)
        {
            return false;
        }
    }

    return true;
}

/* ── flush ── */

/**
 * @brief 写回资源预检。
 *
 * 一次 region 页写需要 1 条命令 + CW_NUM 个描述符 + CW_NUM 个 buffer，且必须能进
 * SQ/HSQ。这些资源与 metadata 日志槽位来自**不同的池**：不预检就 append_slot，
 * 一旦 FC 池空（FTL_AGAIN），槽位已推进且无法回收 ⇒ 持续背压下白烧日志块。
 * 单线程 superloop 且预检与建链之间不泵 ⇒ 资源不会被别人拿走。
 *
 * @return true = 命令/描述符/buffer/SQ/HSQ 都够一次 region 页写
 *         false = 有任一资源不足
 */
static BOOL l2p_flush_resources_is_available(void)
{
    fc_resource_stats_s stats;

    fc_resource_stats_get(&stats);

    return ((stats.command_used + L2P_REGION_COPIES + FTL_FC_CP_CMD_HEADROOM) <= FC_COMMAND_POOL_DEPTH) && (stats.descriptor_used + CW_NUM + FTL_FC_CP_DESC_HEADROOM <= FC_DESCRIPTOR_POOL_DEPTH) && (stats.buffer_used + CW_NUM + FTL_FC_CP_BUFS_HEADROOM <= FC_BUFFER_POOL_DEPTH) && (stats.sq_used < FC_SQ_DEPTH) && (stats.hsq_used < FC_HSQ_DEPTH);
}

/**
 * @brief 双副本写回收尾：commit 或作废，并释放槽位/在飞计数
 *
 * @param cache 共享 L2P cache
 * @param md    该 region owner 的 Directory
 * @param line  写回行（调用前 flush_pending_cmd 必须为 NULL）
 */
static void l2p_flush_finalize(l2p_cache_s *cache, md_dir_cache_s *md, l2p_cache_line_s *line)
{
    ASSERT(line->flush_pending_cmd == NULL);

    if ((line->flush_fail == false) && (line->flush_issued == L2P_REGION_COPIES))
    {
        STATS_INC(cache->stats.flush_ok);
        const WORD  local_region = l2p_global_to_local_region(L2P_TAG(cache, line)->region_id);
        const DWORD primary      = line->flush_pba[L2P_COPY_PRIMARY];
        const DWORD backup       = line->flush_pba[L2P_COPY_BACKUP];

        // 顺序不可倒：先写后记账（掉电最多多留一份旧记录）。
        md_commit_region(md, local_region, MD_COMMIT_PAIR, primary, backup);

        L2P_TAG(cache, line)->state = L2P_CACHE_CLEAN;
        line->dirty_seq             = 0U; /* 已落盘：行不再依赖 Journal */

        // 落盘成功：合并进来的 dirty CMT 已进页 —— 有效值转 CLEAN，
        // INVALID 丢弃；再清实时脏位。
        if (l2p_cmt_clear_region(cache, L2P_TAG(cache, line)->region_id))
        {
            cache->stats.cmt_writebacks++;
        }
        l2p_bitmap_clear(cache->region_dirty_bitmap, L2P_TAG(cache, line)->region_id);
    }
    else
    {
        // 任一副本失败：不提交；旧 pair 仍是 Directory 真值，槽位作废重试。
        STATS_INC(cache->stats.flush_fail);
        L2P_TAG(cache, line)->state = L2P_CACHE_DIRTY;
    }

    // 终态（commit 或失败）已定：释放槽位预留。此后该块的回收判据回到
    // l2p_live —— 成功时新位置已计入，失败时槽里的数据作废、无人引用。
    md_slot_release(md, line->flush_pba[L2P_COPY_PRIMARY], line->flush_pba[L2P_COPY_BACKUP]);

    ASSERT(cache->inflight.flush > 0U);
    cache->inflight.flush--;
}

/**
 * @brief 该 owner 是否没有未落定的行
 *
 * DIRTY/FLUSHING/LOADING 任一存在即不空闲。
 *
 * @param cache     cache
 * @param owner_die owner die
 * @return true 空闲；false 仍有未落定行
 */
SSD_CODE_FAST BOOL l2p_flush_owner_idle(const l2p_cache_s *cache, BYTE owner_die)
{
    if (cache->cmt_dirty_owner[owner_die] != 0U)
    {
        return false;
    }

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        const l2p_cache_line_s *line = &cache->lines[i];

        if (l2p_region_owned_by(L2P_TAG(cache, line)->region_id, owner_die) == false)
        {
            continue;
        }

        if ((L2P_TAG(cache, line)->state == L2P_CACHE_DIRTY) || (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING) || (L2P_TAG(cache, line)->state == L2P_CACHE_LOADING))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 把一个 region 的 translation page 异步写回
 *
 * 先把本 region 的 dirty CMT 合并进 line（CLEAN → DIRTY），再走 WAL / 双副本流程。
 * 报 FTL_ERROR 会让 CP 中止；槽位在 append_slot 时已消耗，失败即作废，
 * Directory 保留旧记录。
 *
 * @param cache    cache
 * @param line_idx 行下标
 * @return FTL_OK；FTL_AGAIN 背压；FTL_ERROR 地址/建链硬失败
 */
SSD_CODE_FAST ftl_status_e l2p_flush_kick(l2p_cache_s *cache, int line_idx)
{
    l2p_cache_line_s *line = &cache->lines[line_idx];

    // CMT dirty 合并到本行后再走原有 WAL/双副本流程；CLEAN line 因此变 DIRTY。
    if ((L2P_TAG(cache, line)->state == L2P_CACHE_CLEAN) || (L2P_TAG(cache, line)->state == L2P_CACHE_DIRTY))
    {
        const DWORD region = L2P_TAG(cache, line)->region_id;

        if (l2p_cmt_region_dirty(cache, region))
        {
            l2p_cmt_merge_region(cache, line);
        }
    }
    if (L2P_TAG(cache, line)->state != L2P_CACHE_DIRTY)
    {
        return FTL_OK; /* 不需要写回（含"已在飞"/"已干净"） */
    }

    // 映射页属于哪个 owner die 由全局 region 解析；写回必须落到 owner 的目录/分配器。
    const l2p_owner_s *owner = l2p_owner_get(cache, L2P_TAG(cache, line)->region_id);

    // WAL 次序：Journal 条目未落盘前不能写区域页，否则 valid_count
    // 会低于映射。Journal 写失败时放行：失败页条目已丢，继续退让会让
    // CP 与失败标记互相等待；此时 CP 本身就是恢复机制。
    const BOOL wal_failed = (owner->journal != NULL) && jrn_has_write_failure(owner->journal);

    if ((owner->journal != NULL) && (line->dirty_seq != 0U) && (wal_failed == false) && (!jrn_seq_ge(jrn_persisted_seq(owner->journal), line->dirty_seq)))
    {
        cache->stats.wal_defer_count++;
        return FTL_AGAIN;
    }

    if (cache->inflight.flush >= L2P_FLUSH_MAX_INFLIGHT)
    {
        STATS_INC(cache->stats.flush_defer_inflight);
        return FTL_AGAIN; /* 共享 credit 池满：让 host I/O 先拿 FC 资源 */
    }

    // 先确认 FC 侧拿得到命令/描述符/buffer/队列槽位，再消耗 metadata 槽位。
    if (l2p_flush_resources_is_available() == false)
    {
        STATS_INC(cache->stats.flush_defer_resource);
        return FTL_AGAIN;
    }

    // 双副本：一次预留主/备两个槽（剩余页不足先换块，避免烧掉半对）。
    DWORD region_addr[L2P_REGION_COPIES];

    if (md_slot_pair_append(owner->metadata_dir, owner->alloc, region_addr) == false)
    {
        // 瞬态：Free Pool 空（等 GC 补池）或目录整理 job 在飞（等它走完）⇒ 背压。
        // ⚠ 不能报 FTL_ERROR：l2p_owner_flush_kick 会跳过本行、cp_regions_flush 会**中止
        // Checkpoint**——一次正常的后台压缩不该把 CP 打回去。
        STATS_INC(cache->stats.flush_defer_md_slot);
        return FTL_AGAIN;
    }

    for (BYTE copy = 0U; copy < L2P_REGION_COPIES; copy++)
    {
        line->flush_pba[copy] = region_addr[copy];
    }

    const DWORD primary_pba = region_addr[L2P_COPY_PRIMARY];
    const DWORD backup_pba  = region_addr[L2P_COPY_BACKUP];

    line->flush_issued = 1U; /* 先提交主副本；备副本在主副本成功后串行提交 */
    line->flush_done   = 0U;
    line->flush_fail   = false;

    // 建链 + 把 entries 拷进 FC buffer（写的数据在 prepare 时就 memcpy 走了 ⇒
    // 提交后 line->entries 可自由改写，这正是"可读"而不"可写"的由来）。
    fc_cmd_s          *cmd          = NULL;
    const ftl_status_e build_status = ftl_async_write_page(&cmd, primary_pba, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META());
    if (build_status != FTL_OK)
    {
        // 命令池空是瞬态背压（FTL_AGAIN）；只有地址/建链的硬失败才是 FTL_ERROR。
        // 混淆二者会让 cp_regions_flush 中止 Checkpoint（H38）。
        // 两槽已烧、没有命令在飞 ⇒ 立刻释放槽位预留，否则该块永久不可回收。
        STATS_INC(cache->stats.flush_defer_build);
        md_slot_release(owner->metadata_dir, primary_pba, backup_pba);
        return build_status;
    }

    // 备副本命令槽一次预留：主副本在飞期间命令池可能被 host/GC 占满，备副本
    // 不能等到主副本完成才去抢命令（抢不到会作废已编程的主副本）。
    fc_cmd_s *backup_cmd = ftl_cmd_alloc_class(FC_CMD_CLASS_CRIT);

    if (backup_cmd == NULL)
    {
        (void)fc_command_release(cmd->cmd_id);
        STATS_INC(cache->stats.flush_defer_resource);
        md_slot_release(owner->metadata_dir, primary_pba, backup_pba);
        return FTL_AGAIN;
    }

    line->flush_pending_cmd     = backup_cmd;
    line->flush_backup_deferred = false;
    L2P_TAG(cache, line)->state = L2P_CACHE_FLUSHING;

    if (ftl_op_submit(cmd, FTL_OP_L2P, line) != FTL_OK)
    {
        STATS_INC(cache->stats.flush_defer_submit);
        (void)fc_command_release(cmd->cmd_id);
        (void)fc_command_release(backup_cmd->cmd_id);
        line->flush_pending_cmd     = NULL;
        line->flush_backup_deferred = false;
        L2P_TAG(cache, line)->state = L2P_CACHE_DIRTY;                 /* 回滚：行仍脏，下轮重试 */
        md_slot_release(owner->metadata_dir, primary_pba, backup_pba); /* 无命令在飞 ⇒ 释放预留*/
        return FTL_AGAIN;
    }

    STATS_INC(cache->stats.flush_start); /* 已提交才算 start：ok + fail + 在飞 == start */
    cache->inflight.flush++;
    return FTL_OK;
}

/**
 * @brief 重试本 die 已预留的备副本命令提交
 *
 * 备副本提交被 cache 序列门控拒绝（FTL_AGAIN）时命令保留在行内；本函数每轮
 * 重试。只有硬失败才把写回判为失败，避免作废已编程的主副本。
 *
 * @param cache 共享 L2P cache（非空）
 * @param die   本 service 轮的 die
 */
void l2p_flush_resume(l2p_cache_s *cache, BYTE die)
{
    ASSERT(cache != NULL);
    ASSERT(die < DIE_MAX);

    if (cache->inflight.flush_pending[die] == 0U)
    {
        return;
    }

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_s *line = &cache->lines[i];

        if ((line->flush_pending_cmd == NULL) || (line->flush_backup_deferred == false))
        {
            continue; /* 无预留 / 主副本仍在飞（备副本未建链） / 其它 die 的行 */
        }
        if (l2p_region_owned_by(L2P_TAG(cache, line)->region_id, die) == false)
        {
            continue;
        }
        ASSERT(L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING);

        fc_cmd_s *const    cmd = line->flush_pending_cmd;
        const ftl_status_e st  = ftl_op_submit(cmd, FTL_OP_L2P, line);

        if (st == FTL_OK)
        {
            line->flush_pending_cmd     = NULL;
            line->flush_backup_deferred = false;
            line->flush_issued          = L2P_REGION_COPIES;
            ASSERT(cache->inflight.flush_pending[die] > 0U);
            cache->inflight.flush_pending[die]--;
            STATS_INC(cache->stats.flush_backup_resume);
        }
        else if (st == FTL_ERROR)
        {
            (void)fc_command_release(cmd->cmd_id);
            line->flush_pending_cmd     = NULL;
            line->flush_backup_deferred = false;
            line->flush_fail            = true;
            ASSERT(cache->inflight.flush_pending[die] > 0U);
            cache->inflight.flush_pending[die]--;

            const l2p_owner_s *const owner = l2p_owner_get(cache, L2P_TAG(cache, line)->region_id);

            ASSERT(owner != NULL);

            md_dir_cache_s *const md = owner->metadata_dir;

            l2p_flush_finalize(cache, md, line);
        }
        else
        {
            /* FTL_AGAIN：序列门控仍关，下一轮再试。 */
        }
    }
}

/* ── is ── */

/**
 * @brief 就绪探针：CLEAN/DIRTY 才算就绪（FLUSHING 不算）
 *
 * miss 时起加载并记一次 miss。
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @return true 行可读写；false 未驻留 / 在飞
 */
BOOL l2p_is_ready(l2p_cache_s *cache, DWORD lba)
{
    // lba 是 host 输入：越界按数据错误处理（write no-op / read FTL_ERROR），不做断言。
    if ((QWORD)lba >= L2P_GLOBAL_LBA_MAX)
    {
        return true; /* 越界：调用者的 write 会是 no-op、read 会返 FTL_ERROR ⇒ 无需就绪 */
    }

    const DWORD       region = lba_to_global_region(lba);
    l2p_cache_line_s *line   = l2p_line_find(cache, region);

    if (line != NULL)
    {
        const BYTE state = L2P_TAG(cache, line)->state;

        // LOADING / FLUSHING 期间行不可写（正在读入或正在提交旧快照）。
        if ((state == L2P_CACHE_LOADING) || (state == L2P_CACHE_FLUSHING))
        {
            return false;
        }
        // CMT 已有条目 ⇒ 更新原地，不需要 line 槽位。
        if ((cache->cmt_enabled != false) && (l2p_cmt_find(cache, lba, false) != NULL))
        {
            return true;
        }
        if (state == L2P_CACHE_DIRTY)
        {
            return true; /* line 自己持有 dirty，写走 line */
        }

        // CLEAN line 且 CMT 缺项：需要空/clean way；全 dirty 时先触发整页写回。
        if (cache->cmt_enabled == false)
        {
            return true; /* replay 旁路：只写 line */
        }
        return l2p_cmt_room_reserve(cache, lba);
    }

    // 无 line：CMT 命中即有旧值、且更新不需要新槽位。
    if ((cache->cmt_enabled != false) && (l2p_cmt_find(cache, lba, false) != NULL))
    {
        return true;
    }

    // miss：**本函数起了加载 ⇒ 记一次 miss**（这次访问的"发现未驻留"发生在此）。
    // 随后的重试命中会被 fresh 吃掉、不计 hit ⇒ 一次访问总账 = 1 miss。
    cache->stats.miss_count++;
    l2p_access_note(cache, true);
    (void)l2p_load_start(cache, region);
    return false;
}

/**
 * @brief 是否驻留且 entries 可信（CLEAN/DIRTY/FLUSHING；LOADING 不算）
 *
 * 不触发加载、不记账。
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @return true 驻留；false 未驻留 / 加载中
 */
BOOL l2p_is_resident(l2p_cache_s *cache, DWORD lba)
{
    ASSERT(cache != NULL);

    if ((QWORD)lba >= L2P_GLOBAL_LBA_MAX)
    {
        return false;
    }

    const l2p_cache_line_s *line = l2p_line_find(cache, lba_to_global_region(lba));

    if (line == NULL)
    {
        return false;
    }

    /* FLUSHING 的 entries 仍是有效快照（l2p_read 也允许读）⇒ 算驻留。 */
    return (L2P_TAG(cache, line)->state == L2P_CACHE_CLEAN) || (L2P_TAG(cache, line)->state == L2P_CACHE_DIRTY) || (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING);
}

/**
 * @brief region 是否曾降级
 *
 * 以 cache 级位图为权威（行被驱逐后仍保留），再兜底扫行。
 *
 * @param cache         cache
 * @param global_region 全局 region
 * @return true 已降级；false 未降级 / 越界
 */
BOOL l2p_is_region_degraded(const l2p_cache_s *cache, DWORD global_region)
{
    if ((cache == NULL) || (global_region >= L2P_GLOBAL_REGIONS))
    {
        return false;
    }

    // cache 级位图是权威：行被驱逐/重载后 line->degraded 会丢，位图不会。
    if (l2p_bitmap_test_bit(cache->region_degraded_bitmap, global_region))
    {
        return true;
    }

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        const l2p_cache_line_s *line = &cache->lines[i];

        if ((L2P_TAG(cache, line)->state != L2P_CACHE_INVALID) && (L2P_TAG(cache, line)->region_id == global_region))
        {
            return line->degraded;
        }
    }

    return false;
}

/* ── read ── */

/**
 * @brief 读一个 IU 的映射
 *
 * 越界回填 INVALID 并返回 ERROR；未驻留起加载、LOADING 未就绪，均返回 AGAIN。
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @param out   输出：PBA
 * @return FTL_OK；FTL_AGAIN 加载在飞；FTL_ERROR 越界/硬失败
 */
ftl_status_e l2p_read(l2p_cache_s *cache, DWORD lba, DWORD *out)
{
    if ((QWORD)lba >= L2P_GLOBAL_LBA_MAX)
    {
        // ⚠ **必须**回填 out：三态化后 FTL_ERROR 的调用者仍会读 *out（"读得 INVALID
        // 与未映射同义"是调用点的既有口径，如 TRIM job / rr_select_plane）。
        // 不回填就是**未初始化变量**——旧单返回值实现此处返回 NAND_PBA_INVALID，
        // 语义必须保持。
        *out = NAND_PBA_INVALID;
        return FTL_ERROR;
    }

    // LBA→(ch,ce) 路由由 ftl_contract_verify 在 boot 期对全部 region 起点证明；
    // 此处不再逐次复检（跨通道/跨 CE 误用是编程错误，boot 审计已覆盖）。

    DWORD cmt_pba;

    if ((cache->cmt_enabled != false) && l2p_cmt_lookup(cache, lba, &cmt_pba))
    {
        cache->stats.cmt_hits++;
        cache->stats.hit_count++;
        l2p_access_note(cache, false);
        *out = cmt_pba;
        return FTL_OK;
    }
    if (cache->cmt_enabled != false)
    {
        cache->stats.cmt_misses++;
    }

    DWORD region_id = lba_to_global_region(lba);
    WORD  entry_idx = lba_to_entry(lba);

    l2p_cache_line_s *line = l2p_line_find(cache, region_id);
    if (line != NULL)
    {
        if (L2P_TAG(cache, line)->state == L2P_CACHE_LOADING)
        {
            return FTL_AGAIN; /* 尚未有效：本次给不出值 */
        }

        // CLEAN / DIRTY / FLUSHING：entries 都是有效快照（FLUSHING 只是"正在落盘"）
        l2p_hit_note(cache, line);

        *out = line->entries[entry_idx];

        // 只有 CLEAN line 的 entries 等于页上真值；DIRTY/FLUSHING 的值由 line
        // 自己持有，不能写成 CLEAN CMT（否则 CLEAN=已落盘的语义被破坏）。
        if (L2P_TAG(cache, line)->state == L2P_CACHE_CLEAN)
        {
            l2p_cmt_update_clean(cache, lba, *out);
        }
        return FTL_OK;
    }

    // miss：起异步加载，本轮给不出值
    cache->stats.miss_count++;
    l2p_access_note(cache, true);
    (void)l2p_load_start(cache, region_id);
    return FTL_AGAIN;
}

/**
 * @brief 同步读映射（read + pump 循环）
 *
 * 直到出结果或耗尽 guard；超时打日志返回 FTL_ERROR。
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @param out   输出：PBA
 * @return FTL_OK；FTL_ERROR 越界 / 超时 / 硬失败
 */
SSD_CODE_FAST ftl_status_e l2p_read_sync(l2p_cache_s *cache, DWORD lba, DWORD *out)
{
    DWORD              guard = L2P_SYNC_GUARD_MAX;
    const l2p_owner_s *owner = NULL;

    if ((QWORD)lba < L2P_GLOBAL_LBA_MAX)
    {
        owner = l2p_owner_get(cache, lba_to_global_region(lba));
    }

    while (guard-- > 0U)
    {
        const ftl_status_e st = l2p_read(cache, lba, out);

        if (st != FTL_AGAIN)
        {
            return st;
        }
        l2p_sync_pump(cache, owner);
    }

    LOGE(LOG_TAG_L2P "read_sync timeout lba=%lu\r\n", lba);
    return FTL_ERROR;
}

/* ── write ── */

/**
 * @brief 写一个 IU 的映射
 *
 * 越界 no-op 返回 OK；未驻留起加载返回 AGAIN；命中则改 entry、置 DIRTY 与
 * region 脏位。
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @param pba   新 PBA
 * @return FTL_OK；FTL_AGAIN 加载在飞；FTL_ERROR 硬失败
 */
ftl_status_e l2p_write(l2p_cache_s *cache, DWORD lba, DWORD pba)
{
    // lba 是 host 输入：越界按数据错误处理（no-op），不做断言。
    if ((QWORD)lba >= L2P_GLOBAL_LBA_MAX)
    {
        return FTL_OK; /* 越界 no-op：数据通路的调用点都自带范围检查 */
    }

    const DWORD       region_id = lba_to_global_region(lba);
    l2p_cache_line_s *line      = l2p_line_find(cache, region_id);

    if ((line != NULL) && ((L2P_TAG(cache, line)->state == L2P_CACHE_LOADING) || (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING)))
    {
        return FTL_AGAIN; /* 有命令在飞：不可写 */
    }

    const DWORD seq = l2p_dirty_seq(cache, region_id);

    if (cache->cmt_enabled != false)
    {
        // CMT 已有条目：原地 dirty；无 line 也可写（line 可能已被驱逐）。
        if (l2p_cmt_find(cache, lba, false) != NULL)
        {
            cache->stats.cmt_wr_hits++;
            (void)l2p_cmt_install_dirty(cache, lba, pba, seq);
            if (line != NULL)
            {
                l2p_hit_note(cache, line);
            }
            else
            {
                cache->stats.hit_count++;
                l2p_access_note(cache, false);
            }
            return FTL_OK;
        }

        if (line == NULL)
        {
            // 冷写：先读 region 页拿 old PBA；调用者下轮重试。
            (void)l2p_load_start(cache, region_id);
            return FTL_AGAIN;
        }

        cache->stats.cmt_wr_misses++;
        l2p_hit_note(cache, line);

        if (L2P_TAG(cache, line)->state == L2P_CACHE_DIRTY)
        {
            // line 已持有 dirty：写 line。CMT 无同 LBA 条目（有则前面 CMT 分支已处理）；
            // line 的值不是页上真值，不得写成 CLEAN CMT。
            line->entries[lba_to_entry(lba)] = pba;
            l2p_note_dirty_seq(cache, line, region_id);
            l2p_bitmap_set(cache->region_dirty_bitmap, region_id);
            return FTL_OK;
        }

        if (l2p_cmt_install_dirty(cache, lba, pba, seq))
        {
            return FTL_OK;
        }

        // 容量竞态：set 全 dirty 且 ready→write 之间槽位被占。回退 line DIRTY，
        // 不返回 AGAIN（Journal 可能已经 append）。
        line->entries[lba_to_entry(lba)] = pba;
        L2P_TAG(cache, line)->state      = L2P_CACHE_DIRTY;
        l2p_note_dirty_seq(cache, line, region_id);
        l2p_bitmap_set(cache->region_dirty_bitmap, region_id);

        (void)l2p_cmt_room_reserve(cache, lba);
        return FTL_OK;
    }

    // CMT 禁用（boot replay）：保持原 line 写路径。
    if (line == NULL)
    {
        (void)l2p_load_start(cache, region_id);
        return FTL_AGAIN;
    }

    // ⚠ 必须喂 thrashing 窗口：写路径的一次访问 = l2p_is_ready（未驻留时记 miss）
    // + 本函数（记 hit）。只加 hit_count 不发访问事件会让随机写满盘的窗口 miss 率
    // 从 50% 虚升到 100% ⇒ 越过 L2P_THRASH_ENTER_PCT ⇒ 写密集下误触发降级。
    l2p_hit_note(cache, line);

    line->entries[lba_to_entry(lba)] = pba;
    L2P_TAG(cache, line)->state      = L2P_CACHE_DIRTY;
    l2p_bitmap_set(cache->region_dirty_bitmap, region_id);
    l2p_note_dirty_seq(cache, line, region_id);
    return FTL_OK;
}

/**
 * @brief 同步写映射（等 region 就绪 + pump 后 l2p_write）
 *
 * @param cache cache
 * @param lba   全局 LBA
 * @param pba   新 PBA
 * @return FTL_OK；FTL_ERROR 越界 / 超时 / 硬失败
 */
SSD_CODE_FAST ftl_status_e l2p_write_sync(l2p_cache_s *cache, DWORD lba, DWORD pba)
{
    DWORD              guard = L2P_SYNC_GUARD_MAX;
    const l2p_owner_s *owner = NULL;

    if ((QWORD)lba < L2P_GLOBAL_LBA_MAX)
    {
        owner = l2p_owner_get(cache, lba_to_global_region(lba));
    }

    while (guard-- > 0U)
    {
        if (l2p_is_ready(cache, lba))
        {
            return l2p_write(cache, lba, pba);
        }
        l2p_sync_pump(cache, owner);
    }

    LOGE(LOG_TAG_L2P "write_sync timeout lba=%lu\r\n", lba);
    return FTL_ERROR;
}

/* ── test ── */

#if defined(FC_NAND_TEST_ENABLE)
void l2p_test_cmt_dirty_release(l2p_cache_s *cache, DWORD lba)
{
    l2p_cmt_dirty_release(cache, lba);
}

BOOL l2p_test_cmt_room(const l2p_cache_s *cache, DWORD lba)
{
    return l2p_cmt_has_room(cache, lba);
}

BOOL l2p_test_owner_flush_ready(const l2p_cache_s *cache, BYTE owner_die, BOOL force_cmt)
{
    return l2p_owner_flush_is_ready(cache, owner_die, force_cmt);
}

#endif

/* ── init ── */

/**
 * @brief 初始化共享 L2P cache（设备级一次）
 *
 * 清行与位图；metadata 依赖不在 cache 里，由 owner_of_region 按需解析。
 *
 * @param cache           cache
 * @param ch              本 cache 服务的 ch
 * @param ce              本 cache 服务的 ce
 * @param owner_of_region region → owner 解析回调
 * @param owner_ctx       回调上下文
 */
SSD_CODE_FAST void l2p_init(l2p_cache_s *cache, BYTE ch, BYTE ce, l2p_owner_fn_t owner_of_region, void *owner_ctx)
{
    ASSERT(cache != NULL);
    ASSERT(owner_of_region != NULL);

    cache->ch              = ch;
    cache->ce              = ce;
    cache->owner_of_region = owner_of_region;
    cache->owner_ctx       = owner_ctx;

    // 全局 Region 级脏标记：开机全清（F4）
    memset(cache->region_dirty_bitmap, 0, sizeof(cache->region_dirty_bitmap));
    // 降级标记是 RAM 态：开机全清，本次启动内由加载失败置位。
    memset(cache->region_degraded_bitmap, 0, sizeof(cache->region_degraded_bitmap));

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_s *line = &cache->lines[i];

        // 全部 entry 填 NAND_PBA_INVALID
        memset(line->entries, 0xFF, sizeof(line->entries));
        cache->tags[i].region_id    = 0U;
        cache->tags[i].state        = L2P_CACHE_INVALID;
        cache->tags[i].access_count = 0U;
        line->fresh                 = false;
        line->prefetch              = false;
        line->dirty_seq             = 0U;
        line->flush_pending_cmd     = NULL;
        line->flush_backup_deferred = false;
#if SSD_STATS_ENABLE
        line->load_timed = false;
#endif
    }

    // entry 级 CMT：显式置空（空槽判据只有 lba == FTL_LBA_INVALID）
    for (WORD set = 0U; set < L2P_CMT_SETS; set++)
    {
        for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
        {
            l2p_cmt_entry_s *e = &cache->cmt[set][way];

            e->lba       = FTL_LBA_INVALID;
            e->pba       = NAND_PBA_INVALID;
            e->dirty_seq = 0U;
            e->stamp     = 0U;
            e->state     = L2P_CMT_CLEAN;
        }
    }
    memset(cache->line_hint, 0xFF, sizeof(cache->line_hint));
    memset(cache->cmt_dirty_region_bitmap, 0, sizeof(cache->cmt_dirty_region_bitmap));
    memset(cache->cmt_dirty_owner, 0, sizeof(cache->cmt_dirty_owner));
    cache->cmt_clock   = 0U;
    cache->cmt_enabled = true;

    cache->clock_hand            = 0U;
    cache->stats.hit_count       = 0U;
    cache->stats.miss_count      = 0U;
    cache->stats.hint_hits       = 0U;
    cache->stats.hint_misses     = 0U;
    cache->stats.cmt_hits        = 0U;
    cache->stats.cmt_misses      = 0U;
    cache->stats.cmt_wr_hits     = 0U;
    cache->stats.cmt_wr_misses   = 0U;
    cache->stats.cmt_dirty       = 0U;
    cache->stats.cmt_writebacks  = 0U;
    cache->stats.cmt_evict_clean = 0U;
    cache->stats.cmt_evict_dirty = 0U;
    cache->stats.cmt_wb_defer    = 0U;
    cache->prefetch.calls        = 0U;
    cache->prefetch.started      = 0U;
    cache->prefetch.skipped      = 0U;
    cache->prefetch.resident     = 0U;
    cache->prefetch.hits         = 0U;
    cache->stats.wal_defer_count = 0U;

    // 在飞计数归零：设备级 init 发生在任何命令之前
    cache->inflight.load  = 0U;
    cache->inflight.flush = 0U;
    memset(cache->inflight.flush_pending, 0, sizeof(cache->inflight.flush_pending));

    // Thrashing 检测窗口：开机从「非降级」起步
    cache->thrash.window_access = 0U;
    cache->thrash.window_miss   = 0U;
    cache->thrash.miss_pct      = 0U;
    cache->thrash.thrashing     = false;
}

/* ── device ── */

/**
 * @brief 设备级初始化实际存在的 target 共享 cache（一次）
 *
 * 只给「至少有一个 present LUN」的 (ch,ce) 分配 cache 槽；稀疏拓扑下不为
 * 不存在的 target 付 200 KiB 常驻内存与启动 memset。
 */
void l2p_device_init(void)
{
    s_target_count = 0U;

    for (BYTE ch = 0U; ch < CH_NUM; ch++)
    {
        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            BOOL present = false;

            for (BYTE lun = 0U; lun < LUN_NUM; lun++)
            {
                if (g_chcelun_table[ch][ce][lun] != FTL_DIE_INVALID)
                {
                    present = true;
                    break;
                }
            }

            if (present == false)
            {
                continue;
            }

            if (s_target_count >= NAND_BOARD_TARGET_MAX)
            {
                /* 板级上界配错：该 target 无 cache 槽，实例 boot 会因取不到 cache 失败。 */
                LOGE("[L2P ] target count exceeds board max; ch=%hhu ce=%hhu skipped\r\n", ch, ce);
                continue;
            }

            s_target_ch[s_target_count] = ch;
            s_target_ce[s_target_count] = ce;
            l2p_init(&s_l2p_cache[s_target_count], ch, ce, s_owner_of_region, NULL);
#if SSD_STATS_ENABLE
            l2p_stats_register(&s_l2p_cache[s_target_count], s_target_count);
#endif
            s_target_count++;
        }
    }
}

/* ── handle ── */

/**
 * @brief L2P 后台写回入口
 *
 * 按档位踢本 owner 的 dirty CMT / DIRTY line。dirty CMT 只在达到写回水位时
 * 主动搬（低于水位由容量 eviction / host FLUSH / CP 强制）；DIRTY line 始终
 * 写回。thrashing 只作为观测统计，不门控 flush。
 *
 * @param cache cache
 * @param die   目标 die
 * @param level 本轮后台档位
 */
SSD_CODE_FAST void l2p_handle(l2p_cache_s *cache, BYTE die, bg_level_e level)
{
    const BYTE max_kick  = s_l2p_flush_max[level];
    // IDLE 档没有 host 写压力：把 dirty CMT 排空，既降低掉电窗口，也不与前台抢带宽。
    const BOOL force_cmt = (level == BG_LEVEL_IDLE);

    l2p_owner_flush_kick(cache, die, max_kick, force_cmt);
}

/* ── callback ── */

/**
 * @brief L2P 命令完成服务：按 phase 结算 load / flush
 *
 * LOADING → 成功把整页 region 拷进 line->entries（须早于 release：读的数据在
 * desc 链上），置 CLEAN；失败同样置 CLEAN（entries 保持全 INVALID = 未映射）。
 * FLUSHING → 成功 md_commit_region 登记新位置 + CLEAN + 清 region 脏位；
 * 失败置回 DIRTY 保留下轮重试。其余状态是编程错误（debug 断言）。
 * 每条命令自己 release。
 *
 * @param ftl        完成派发解析出的实例
 * @param completion FC 完成状态
 * @param ctx        l2p_cache_s *
 */
void ftl_l2p_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    l2p_cache_line_s *line  = (l2p_cache_line_s *)ctx;
    l2p_cache_s      *cache = ftl->cache;

    ASSERT(line != NULL);

    fc_cmd_s *cmd = fc_command_lookup(completion->cmd_id);

    // ftl 恒非空（路由表早于逐实例 boot 设好，见 ftl_op.h）；NULL = 编程错误 ⇒
    // ASSERT 而非 if-guard：release 下解引用留现场，比静默吞完成可定位。

    ASSERT(&ftl->cache->lines[0] <= line);
    ASSERT(line < &ftl->cache->lines[L2P_CACHE_SIZE]);

    // 命令按 PBA 的 ch/ce/lun 路由回 owner；line 的全局 region 必须属于同一个 owner。
    ASSERT(topo_2_die(ftl->topo) == (L2P_TAG(cache, line)->region_id % DIE_MAX));
    ASSERT(l2p_owner_get(ftl->cache, L2P_TAG(cache, line)->region_id)->die == topo_2_die(ftl->topo));

    // 完成只可能属于一次在飞的 load/flush；行在这期间不可驱逐、不可改写状态。
    ASSERT((L2P_TAG(cache, line)->state == L2P_CACHE_LOADING) || (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING));

    const BYTE failed = fc_cmd_status_has_error(completion->status) ? 1U : 0U;

    if (L2P_TAG(cache, line)->state == L2P_CACHE_LOADING)
    {
        const l2p_owner_s *const owner = l2p_owner_get(cache, L2P_TAG(cache, line)->region_id);

        // 失败结算会把 line->prefetch 清掉；item 结算必须用本次加载登记时的类别。
        const BOOL load_prefetch = line->prefetch;

#if SSD_STATS_ENABLE
        // 先摘掉本次时间戳：失败分支可能立刻重提交备份（会写新的 load_start_us），
        // 不能把新时间戳当成这次完成的起点。
        const BOOL  load_was_timed  = line->load_timed;
        const DWORD load_started_us = line->load_start_us;

        line->load_timed = false;
#endif
        BYTE load_failed   = failed;
        BOOL load_finished = true;

        if (load_failed == 0U)
        {
            // 数据在 desc 链上 ⇒ 必须早于 release 拷出。
            if (ftl_cmd_read_data(cmd, line->entries, L2P_REGION_SIZE) != FTL_OK)
            {
                LOGW(LOG_TAG_L2P "load copy failed region=%lu\r\n", L2P_TAG(cache, line)->region_id);
                load_failed = 1U;
            }
        }

        if (load_failed == 0U)
        {
            line->load_fail_count       = 0U;
            line->degraded              = false;
            L2P_TAG(cache, line)->state = L2P_CACHE_CLEAN;

            // host 加载：下一次命中是本次 miss 的重试，不计 hit。
            // 预取：保留 prefetch 标志，下一次 host 命中计真实收益。
            line->fresh = (line->prefetch == false);

            // 从备份副本读成功 ⇒ 交换主/备，后续加载不必先读坏主副本。
            // 两份 PBA 都已计入 l2p_live，交换不改变账本，只置 Directory 脏。
            if (line->load_use_backup)
            {
                const WORD  local   = l2p_global_to_local_region(L2P_TAG(cache, line)->region_id);
                const DWORD primary = md_entry_find(ftl->metadata_dir, METADATA_TYPE_L2P_REGION, local);
                const DWORD backup  = md_backup_find(ftl->metadata_dir, local);

                // 交换会把两个 PBA 写进 Directory（介质事实）⇒ 非 INVALID 者必须合法，
                // 否则坏 PBA 会被“洗白”成主副本。
                if ((primary != NAND_PBA_INVALID) && (backup != NAND_PBA_INVALID) && (pba_is_valid(primary) == true) && (pba_is_valid(backup) == true) && (primary != backup))
                {
                    md_commit_region(ftl->metadata_dir, local, MD_COMMIT_PAIR, backup, primary);
                    LOGW(LOG_TAG_L2P "region %lu primary unreadable — backup OK\r\n", L2P_TAG(cache, line)->region_id);
                }
                // primary == INVALID 是「旧介质单副本 / 尚未 flush」的正常路径，不是
                // 读失败，不打 WRN（否则日志会把合法状态说成故障）。
            }
        }
        else
        {
            // 主副本失败且存在有效的另一份 ⇒ 立即改读备份（不消耗重试预算）。
            // 备份同样来自 Directory（H29）⇒ 非 INVALID 也要过 pba_is_valid。
            const WORD  local   = l2p_global_to_local_region(L2P_TAG(cache, line)->region_id);
            const DWORD backup  = md_backup_find(ftl->metadata_dir, local);
            BOOL        retried = false;

            if ((line->load_use_backup == false) && (backup != NAND_PBA_INVALID) && (pba_is_valid(backup) == true) && (backup != line->load_pba))
            {
                line->load_use_backup = true;
                retried               = l2p_load_submit(ftl->cache, line, backup, true);
            }

            if (retried == false)
            {
                // 两份都失败：统一结算（重试上限 → 降级）。见 l2p_load_fail_settle 的契约。
                l2p_load_fail_settle(cache, line);
            }
            else
            {
                load_finished = false; /* 备份读已提交：等它的完成再结算 item */
            }
        }

        ASSERT(ftl->cache->inflight.load > 0U);
        ftl->cache->inflight.load--;
        line->line_generation++;
        if (load_failed != 0U)
        {
            STATS_INC(cache->stats.load_done_err);
        }
        else
        {
            STATS_INC(cache->stats.load_done_ok);
        }
#if SSD_STATS_ENABLE
        if (load_was_timed != false)
        {
            DWORD now_us = 0U;

            if (delay_time_now_us(&now_us) != false)
            {
                stats_hist_observe(&cache->stats.load_lat_us, now_us - load_started_us);
            }
        }
#endif
        if ((load_finished != false) && (owner->sched != NULL))
        {
            ftl_sched_l2p_load_finish(owner->sched, L2P_TAG(cache, line)->region_id, load_prefetch);
        }
    }
    else if (L2P_TAG(cache, line)->state == L2P_CACHE_FLUSHING)
    {
        // 双副本串行：先主后备。按命令自己的 PBA 识别是哪一份。
        const DWORD pba  = ftl_cmd_pba(cmd);
        const BYTE  copy = (pba == line->flush_pba[L2P_COPY_BACKUP]) ? L2P_COPY_BACKUP : L2P_COPY_PRIMARY;

        line->flush_done++;
        line->line_generation++; /* flush 副本完成：line 向 ready 推进 */

        if (failed != 0U)
        {
            line->flush_fail = true;

            // 主副本失败：预留的备副本命令不再需要，必须先释放，
            // 否则收尾条件（pending == NULL）永远不成立。
            if (line->flush_pending_cmd != NULL)
            {
                (void)fc_command_release(line->flush_pending_cmd->cmd_id);
                line->flush_pending_cmd     = NULL;
                line->flush_backup_deferred = false;
            }
        }

        // 主副本成功 ⇒ 用预留命令提交备份副本。
        if ((line->flush_fail == false) && (copy == L2P_COPY_PRIMARY) && (line->flush_issued == 1U))
        {
            fc_cmd_s *cmd2 = line->flush_pending_cmd;

            ASSERT(cmd2 != NULL); /* 写回起点已预留 */

            const DWORD        backup_pba = line->flush_pba[L2P_COPY_BACKUP];
            const ftl_status_e build      = ftl_async_write_page_reserved(cmd2, backup_pba, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META());

            if (build == FTL_OK)
            {
                const ftl_status_e submit = ftl_op_submit(cmd2, FTL_OP_L2P, line);

                if (submit == FTL_OK)
                {
                    line->flush_pending_cmd     = NULL;
                    line->flush_backup_deferred = false;
                    line->flush_issued          = L2P_REGION_COPIES;
                }
                else if (submit == FTL_AGAIN)
                {
                    // 序列门控：命令与数据已备好，保留到下一轮 resume 重试；
                    // 主副本已编程，不能作废整对。
                    const BYTE owner_die = topo_2_die(ftl->topo);

                    ASSERT(cache->inflight.flush_pending[owner_die] < L2P_FLUSH_MAX_INFLIGHT);
                    line->flush_backup_deferred = true;
                    cache->inflight.flush_pending[owner_die]++;
                    STATS_INC(cache->stats.flush_backup_defer);
                }
                else
                {
                    (void)fc_command_release(cmd2->cmd_id);
                    line->flush_pending_cmd     = NULL;
                    line->flush_backup_deferred = false;
                    line->flush_fail            = true;
                }
            }
            else
            {
                /* 建链硬失败：helper 已回收命令。 */
                line->flush_pending_cmd     = NULL;
                line->flush_backup_deferred = false;
                line->flush_fail            = true;
            }
        }

        // 已发出的副本全部完成、且没有待提交的备副本 ⇒ 收尾。
        if ((line->flush_pending_cmd == NULL) && (line->flush_done >= line->flush_issued))
        {
            l2p_flush_finalize(cache, ftl->metadata_dir, line);
        }
    }
    else
    {
        // 不该发生：LOADING / FLUSHING 期间行不可被改写状态。
        LOGE(LOG_TAG_L2P "unexpected completion state=%d\r\n", L2P_TAG(cache, line)->state);
    }

    (void)fc_command_release(cmd->cmd_id);
}
