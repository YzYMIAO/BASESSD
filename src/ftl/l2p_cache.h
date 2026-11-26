#ifndef FTL_L2P_CACHE_H
#define FTL_L2P_CACHE_H

/*
 * L2P Cache — DRAM-less SRAM Region Cache
 *
 * 全表存储于 NAND 元数据区（4MB），SRAM 中缓存最近访问的 region。
 * Region 分片：512 条 entry × 4B = 2KB/region，2048 region 覆盖全部 LBA 空间。
 * 从未写过的 region 在 Directory 里登记为 NAND_PBA_INVALID：加载时直接合成
 * 全 INVALID 的 cache 行，**不产生 NAND 读**（对齐 DRAM-less 主流的"未映射无 I/O"）。
 *
 * 地址体系：
 *   DWORD → region_id (lba / 512) + entry_idx (lba % 512)
 *   entry = DWORD（FTL 物理地址：CH/CE/LUN 拓扑 + 块/页/plane/CW）
 *   NAND_PBA_INVALID = 0xFFFFFFFF（未映射的 LBA）
 *
 * 替换策略：CLOCK（近似 LRU，O(1) amortized）
 * 写回策略：Write-back（Dirty 标记，延迟写回 NAND）
 */

#include "allocator.h"    /* alc_s */
#include "fc_interface.h" /* fc_cmd_s —— 预留的备副本命令句柄 */
#include "ftl_capacity.h" /* FTL_LBA_MAX_AT_MAX（键空间上限自检） */
#include "ftl_config.h"   /* §三.2 DWORD / pba_*（几何量见 ftl_geometry.h） */
#include "ftl_type.h"     /* DWORD */
#include "metadata.h"     /* metadata_dir_* / md_dir_cache_s（Directory 访问） */
#include "stats.h"        /* SSD_STATS_ENABLE（统计专用字段按档裁剪） */

#include <stddef.h>
#include <stdint.h>

/* ── 宏 ──
 * L2P 容量与分解宏（L2P_ENTRIES_PER_REGION / L2P_REGION_SIZE /
 * L2P_REGIONS_PER_INSTANCE / L2P_REGION_SHIFT / L2P_REGION_MASK /
 * DIE_MAX）见 ftl/ftl_geometry.h；
 * 元数据块布局见 ftl/metadata.h「固定元数据区布局」。*/
#define L2P_CACHE_SIZE (64U) /* cache 行数（可配 32/64/128）*/

/* 在飞写回上限（背压闸门，见 l2p_cache_s.flush_inflight）：每次写回占 1 条命令 + 4 buffer，
 * 4 条在飞 = 16 buffer，占 FC buffer 池（128）的 1/8，给 host I/O 留足余量。 */
#define L2P_FLUSH_MAX_INFLIGHT (4U)

#define L2P_PREFETCH_MAX_INFLIGHT (2U) /* 本仓自定：host load + prefetch 合计上限 */

/* 后台主动 flush 的每轮条数上界（按档位；docs §4.7 的 static const 策略表）。
 * BUSY 档只放行 1 条 ⇒ 最小预算 = 「must-run + 1 flush」。 */
#define L2P_FLUSH_MAX_NORMAL (4U)

#define L2P_FLUSH_MAX_BUSY (1U)

/* L2P 后台写回节拍：timer 任务每 N ms 置 ftl->l2p_flush_due，sched 后台策略消费。
 * 只 gate「要不要踢一次」，具体条数仍由上面的 max_kick 与 token bucket 限。 */
#define L2P_FLUSH_INTERVAL_MS (10U)

/* 同步逃生口的自旋预算（轮）。每轮 = 一次 ftl_service_pump（推 NAND + 抽干完成）。
 * 超预算即判失败返回——兜住"命令永不到达"这类编程错误，不让同步站点死等。 */
#define L2P_SYNC_GUARD_MAX (100000UL)

/* ── Thrashing 检测（设计 §6.5）──
 * 工作集远大于 64 行 cache（64×512 = 32K entry = 16 MiB LBA 空间，而盘 512 MiB）时，
 * 全盘随机访问几乎每次 miss ⇒ 「evict→flush→load」反复。检测到后**降级**：
 * 停掉 NORMAL 模式的**主动**后台 flush（脏行只由驱逐路径按需写回），把 NAND 带宽让给
 * 前台 IO，并避免「本会被驱逐的行被提前 flush 一次、随后又因驱逐再 flush」的重复写。
 *
 * 窗口化统计（不是开机累计）：每 L2P_THRASH_WINDOW 次访问结算一次 miss 率，带**滞回**
 * （进入 ≥90%，退出 <70%），避免在阈值附近抖动导致降级开关反复翻转。 */
#define L2P_THRASH_WINDOW (256U) /* 检测窗口：每 256 次访问结算一次 */

#define L2P_THRASH_ENTER_PCT (90U) /* 进入阈值：窗口 miss 率 ≥ 90% */

#define L2P_THRASH_EXIT_PCT (70U) /* 退出阈值：窗口 miss 率 < 70%（滞回） */

/* ── 全局坐标（cache 跨 LUN 共享）──
 *
 * 数据通路一律用 **host 全局 LBA**；只有「映射页在 owner die 元数据区里的落盘位置」
 * 用局部 region（每个 die 只有 L2P_REGIONS_PER_INSTANCE 个槽）。
 * cache 行/脏位图用全局 region 键；metadata 的 lookup/append/commit 用局部 region。
 */
/* 等长键空间：位图按静态上限分配，运行时只用前 DIE_MAX 个 die 的 region。 */
#define L2P_GLOBAL_REGIONS_MAX (L2P_REGIONS_PER_INSTANCE * FTL_DIE_MAX)

#define L2P_GLOBAL_REGIONS (L2P_REGIONS_PER_INSTANCE * DIE_MAX)

#define L2P_GLOBAL_LBA_MAX_AT_MAX ((QWORD)L2P_GLOBAL_REGIONS_MAX * L2P_ENTRIES_PER_REGION)

/* 主机范围必须落在共享 cache 的键空间内（C9：同一语义只允许一套上限）。 */
_Static_assert(FTL_LBA_MAX_AT_MAX <= L2P_GLOBAL_LBA_MAX_AT_MAX, "FTL_LBA_MAX 超出共享 cache 的全局 LBA 上限");

#define L2P_GLOBAL_LBA_MAX ((QWORD)L2P_GLOBAL_REGIONS * L2P_ENTRIES_PER_REGION)

#define L2P_DIRTY_BITMAP_WORDS (L2P_GLOBAL_REGIONS_MAX / 32U)

#define L2P_LOCAL_DIRTY_WORDS (L2P_REGIONS_PER_INSTANCE / 32U)

/* region line 查找的 hint 表：region_id → line 下标的**提示**，不是权威索引。
 * 命中仍需用 tags[] 校验；陈旧/冲突只退化为线性扫描，不影响正确性。 */
#define L2P_LINE_HINT_SLOTS (256U)

/* ── entry 级 CMT（Cached Mapping Table）──
 *
 * 写回单位：dirty 值驻留 CMT，按 translation page 合并后整页写回。
 * region line 退化为读缓存与写回缓冲区（CLEAN/DIRTY 语义见文件内不变量）。
 * 依据：DFTL 的 CMT + AppL `fine_cache`（dirty entry + 驱逐时整页写回）。
 * 冷写 miss 仍先读 region 页取 old PBA；4-way 组相联只是容量/命中结构，
 * 不是功能边界。
 *
 * 空槽以 `lba == FTL_LBA_INVALID` 为唯一判据（C9）。CLEAN 条目只保存有效 PBA；
 * DIRTY 条目允许 `NAND_PBA_INVALID`（TRIM 负条目）。 */
#define L2P_CMT_SETS (1024U)

#define L2P_CMT_WAYS (4U)

#define L2P_CMT_SIZE (L2P_CMT_SETS * L2P_CMT_WAYS)

/* CMT 主动写回水位：host 写 admission 与后台写回共用同一个阈值。
 * 本仓自拟 75%：留 1/4 容量吸收在飞写与驱逐路径的突发。 */
#define L2P_CMT_WB_WATERMARK ((L2P_CMT_SIZE * 3U) / 4U)

/* ── Region 分解 ──
 * L2P_REGION_SHIFT（lba / 每 region 条目数）与 L2P_REGION_MASK（lba % 每 region
 * 条目数）见 ftl/ftl_geometry.h。*/

/* ── Cache 状态 ── */

typedef enum L2P_CACHE_STATE_E
{
    L2P_CACHE_INVALID = 0, /* cache 行无效，entries 全 NAND_PBA_INVALID */
    L2P_CACHE_CLEAN,       /* cache 行有效，未修改 */
    L2P_CACHE_DIRTY,       /* cache 行有效，已修改（需写回 NAND） */
    L2P_CACHE_FLUSHING,    /* 正在写回 NAND（不可驱逐、不可写；**可读**——entries 仍有效） */
    L2P_CACHE_LOADING,     /* 正在从 NAND 加载 region（不可驱逐、不可读写——entries 尚未有效） */
} l2p_cache_state_e;

/* ── 异步化 ──
 *
 * 加载与写回都改为**提交即返回**：未命中 ⇒ 建命令 + ftl_op_submit(tag = FTL_OP_L2P,
 * ctx = 本 cache line) ⇒ 置 LOADING/FLUSHING ⇒ 返回 FTL_AGAIN；完成由
 * g_op_callbacks[FTL_OP_L2P]（ftl_l2p_op_callback，l2p_cache.c）按 line->state 收尾。
 * ⇒ 对外 API 的失败语义变成三态，调用者必须能**挂起**（见下）。
 *
 * ⚠ 两条不变量（异步后仍成立，是正确性根基）：
 *   ① **可读不可写**：FLUSHING 行的 entries 是有效快照，读照常命中；写会让"落盘的是
 *      旧快照、RAM 是新值"分叉（完成时无法区分该置 CLEAN 还是 DIRTY）⇒ 写一律返
 *      FTL_AGAIN 让调用者下轮重试。LOADING 行两者都不行（entries 尚未有效）。
 *   ② **同一 region 只有一行**：l2p_line_find 匹配 CLEAN/DIRTY/FLUSHING/LOADING
 *      （不再是"只匹配 CLEAN/DIRTY"）。异步后 FLUSHING 窗口真实存在，若把它当未命中
 *      就会为同一 region 再分配一行 ⇒ 两行分叉、映射静默不一致。
 *
 * ⚠ **同步逃生口**（`*_sync`）：内部自旋 ftl_service_pump 直到就绪。只许结构性同步
 *   上下文调用（开机恢复 / CLI / checkpoint / HIL `fl`）——它们不在完成派发里，
 *   自旋不是嵌套泵。服务函数（g_op_callbacks 里的任何一个）**禁止**调用，由
 *   ftl_op_is_in_dispatch 断言兜住。服务函数里禁止自旋泵（嵌套派发）。
 */

/* ── 查找索引 / 数据行（SoA）──
 *
 * tags[] 连续存放查找元数据（region_id/state/access_count），lines[] 才放 2 KB
 * 映射数据 + 与查找无关的完成/写回状态。查找与 CLOCK 只扫 tags[]（64 × 8 B = 512 B，
 * 约 16 条 D-Cache line）；数据行只在命中 / 加载 / 写回时访问。
 * 依据：DFTL 的 GTD/CMT 分层、AppL coarse_cache 的 GTD 直接索引；
 * 具体「tags[] + lines[]」结构是本仓自拟，不是文献规定的标准形态。 */
typedef struct L2P_TAG_S
{
    DWORD region_id;    /* **全局** region（0..L2P_GLOBAL_REGIONS-1） */
    WORD  access_count; /* CLOCK 访问计数 */
    BYTE  state;        /* l2p_cache_state_e（-fshort-enums：1 B） */
} l2p_tag_s;

typedef struct L2P_CACHE_LINE_S
{
    DWORD entries[L2P_ENTRIES_PER_REGION]; /* 512 条 PBA = 2KB */

    /* 「本次加载正是为**当前这次访问**而起」（异步化的观测口径补偿）。
     * 异步后一次 miss 访问会跨两轮：第 1 轮起加载（计一次 miss）、第 2 轮重试命中。
     * 若重试也计一次 hit，则**同一次访问被记成 miss + hit** ⇒ 窗口 miss 率虚降一半
     * （随机读满盘：真值 100% → 观测 50%，跌破 L2P_THRASH_EXIT_PCT=70% ⇒
     * 降级永不触发）。故：加载完成时置 1，其后**第一次**命中消费它且不计命中，
     * 一次逻辑访问在窗口里只记一次 miss。
     * 本条与 `load_fail_count` / 双副本字段共用尾部对齐填充。 */
    BOOL fresh;

    /* 1 = 本行最近一次加载在重试耗尽后降级为「全未映射」（映射已丢）。
     * 开机恢复与 host 读路径据此区分「合法未映射」与「读不出来被迫当未映射」：
     * 后者 probe 计入 `l2p_degraded_cw`（范围化恢复，不回退全量重建），host 读
     * **返回 IO_ERROR** 而不是零填。每次 load_start / 加载成功清零；降级时置 1。 */
    BOOL degraded;

    /* 本 region 连续加载失败次数。
     * 失败后先置 INVALID 重试，达到 L2P_LOAD_RETRY_MAX 才降级为「全未映射的
     * CLEAN 行」——既不每次访问都重读坏 region（活锁），也不把一次瞬态读错
     * 永久当成「从未写过」。换 region 时由 load_start 清零。
     * ⚠ 计数随行保存：行被驱逐后计数丢失（重试预算重置）。持续访问的坏 region
     *   通常会复用同一 INVALID 行，仍能达到上限；这是用 1B/行换「不做全 region 计数表」。 */
    BYTE load_fail_count;

    /* ── 双副本写回（L2）──
     * 串行写：先主副本；主成功后提交备份副本。两份都成功才 commit（原子）；任一失败
     * 都保持 DIRTY、不登记新位置（旧 pair 仍是 Directory 真值，槽位作废）。
     * 备副本命令在写回起点就预留（命令槽），提交被 cache 序列门控拒绝时保留重试，
     * 不丢弃已编程的主副本。 */
    DWORD     flush_pba[L2P_REGION_COPIES]; /* 0=主 / 1=备 */
    BYTE      flush_issued;                 /* 已提交副数（1 或 2） */
    BYTE      flush_done;                   /* 已完成副数 */
    BOOL      flush_fail;                   /* true = 任一副本失败 ⇒ 不提交 */
    BOOL      flush_backup_deferred;        /* true = 备副本已建链、提交被门控，等 resume */
    fc_cmd_s *flush_pending_cmd;            /* 预留的备副本命令；写回起点即置位 */

    /* 本次加载读的是哪一份（主备回退，的 L2）。 */
    DWORD load_pba;        /* 在飞/最近一次加载的 PBA */
    BOOL  load_use_backup; /* true = 当前这次读的是备份副本 */

    /* 本行映射所依赖的 Journal 序号上界（WAL 次序）：
     * 每次映射更新（jrn_append 之后）记录当时的 sequence_counter；区域写回
     * （l2p_flush_kick）要求 Journal 的持久化水位 ≥ 它，保证「日志先于数据」。
     * 0 = 无依赖（新加载 / 恢复生成的行）。 */
    DWORD dirty_seq;

    /* 本行由顺序读预取发起（本仓自定）。
     * 预取加载完成时 fresh=0，第一次 host 命中计 hit 并清本标志；
     * 异步失败/降级/行被驱逐/初始化时清。 */
    BOOL prefetch;

    /* 本行向 ready 推进的代数：load 或 flush 每完成一次（成功/失败都计）
     * 递增。GC 的 mig_job_wait 用它做 per-region 活性判据——别的 region
     * 加载/写回不会推进本行。 */
    DWORD line_generation;

#if SSD_STATS_ENABLE
    DWORD load_start_us; /* 本次加载提交时刻（µs） */
    BOOL  load_timed;    /* true = load_start_us 有效（时钟可用） */
#endif
} l2p_cache_line_s;

/* ── Owner 端口（docs §4.7）──
 *
 * cache 跨 LUN 共享后，load/flush/commit 需要知道某个全局 region 属于哪个
 * FTL 实例（owner die）。端口只暴露**共享服务的最小集**，不返回整个 ftl_context。
 * 回调由设备级 init 注入（main.c），内部复用 lba_2_* + ftl_op_route_get。
 */
/* 前向声明：owner 端口需要 Journal 的 WAL 水位（区域写回闸门）。
 * 不 include journal.h——journal.h 反向 include 本头，会成环。 */
typedef struct L2P_JOURNAL_S l2p_jrn_s;

typedef struct FTL_SCHEDULER_S ftl_sched_s;

/* 前向声明：行内持有「已预留、待提交」的备副本命令（不把 fc_interface.h 拉进本头）。 */
typedef struct FC_COMMAND_S fc_cmd_s;

typedef struct L2P_OWNER_S
{
    BYTE            die; /* 本 owner 的实例编号（global 路由键） */
    md_dir_cache_s *metadata_dir;
    alc_s          *alloc;
    ftl_topo_s      topo;
    l2p_jrn_s      *journal; /* 本 die 的 Journal：区域写回前查 WAL 水位（可为 NULL：纯 RAM 夹具） */
    ftl_sched_s    *sched;   /* 本 die 的 scheduler：demand load 登记/完成（纯 RAM 夹具可为 NULL） */
} l2p_owner_s;

typedef const l2p_owner_s *(*l2p_owner_fn_t)(void *ctx, DWORD global_region);

typedef enum L2P_CMT_STATE_E
{
    L2P_CMT_CLEAN = 0, /* 值来自 region 页，可被替换 */
    L2P_CMT_DIRTY,     /* 值新于 region 页，必须整页写回 */
} l2p_cmt_state_e;

typedef struct L2P_CMT_ENTRY_S
{
    DWORD lba;       /* 全局 LBA；FTL_LBA_INVALID = 空槽 */
    DWORD pba;       /* 映射；DIRTY 条目允许 NAND_PBA_INVALID */
    DWORD dirty_seq; /* DIRTY：Journal 持久化水位要求；CLEAN 为 0 */
    WORD  stamp;     /* LRU 时间戳（访问/插入时更新） */
    BYTE  state;     /* l2p_cmt_state_e */
    BYTE  pad;
} l2p_cmt_entry_s; /* 16 B × 4096 = 64 KiB */

/* ── Cache 上下文 ── */

typedef struct L2P_CACHE_STATISTICS_S
{
    DWORD hit_count;       /* 命中次数（开机累计） */
    DWORD miss_count;      /* 未命中次数（开机累计） */
    DWORD eviction_count;  /* 被驱逐行数（开机累计） */
    DWORD wal_defer_count; /* 区域写回被 WAL 次序闸门退让的次数 */
    DWORD hint_hits;       /* region line hint 命中次数 */
    DWORD hint_misses;     /* hint 未命中/冲突，退回线性扫描的次数 */
    DWORD cmt_hits;        /* 读路径 CMT 命中次数 */
    DWORD cmt_misses;      /* 读路径 CMT 未命中次数（随后走 region 路径） */
    DWORD cmt_wr_hits;     /* 写路径命中已有 CMT 条目的 LBA 数 */
    DWORD cmt_wr_misses;   /* 写路径未命中 CMT、走 line/插入的 LBA 数 */
    DWORD cmt_dirty;       /* 当前 dirty CMT 条目数 */
    DWORD cmt_writebacks;  /* 含 dirty CMT 的整页写回次数 */
    DWORD cmt_evict_clean; /* 为新条目替换 CLEAN CMT 槽的次数 */
    DWORD cmt_evict_dirty; /* 因容量触发的 dirty victim 整页写回次数 */
    DWORD cmt_wb_defer;    /* dirty victim 写回因在飞/背压退让的次数 */

#if SSD_STATS_ENABLE
    /* region 加载 / 写回的成功与失败原因（诊断满盘时“卡在哪一步”）。
     * 只服务 stats；关闭统计时整块裁掉（自增点经 STATS_INC 自动消失）。 */
    DWORD load_start;           /* 成功提交、进入 LOADING 的次数（含预取） */
    DWORD load_done_ok;         /* 加载完成且数据可用的次数 */
    DWORD load_done_err;        /* 加载完成但状态/拷贝失败（可能随后重试备份） */
    DWORD load_fail_no_cmd;     /* 命令额度不足（FTL_AGAIN） */
    DWORD load_fail_build;      /* 地址/desc/buffer 建链失败（FTL_ERROR） */
    DWORD load_fail_gate;       /* ftl_op_submit 被 cache 序列门控拒绝 */
    DWORD load_fail_submit;     /* ftl_op_submit 的其他拒绝（FC 队列/登记） */
    DWORD degrade;              /* 重试耗尽后降级为“全未映射”的次数 */
    DWORD flush_start;          /* region 写回 job 启动次数 */
    DWORD flush_ok;             /* 写回 job 成功（行回 CLEAN）的次数 */
    DWORD flush_fail;           /* 写回 job 失败（行回 DIRTY）的次数 */
    DWORD flush_defer_inflight; /* 在飞 flush 达上限，暂缓 */
    DWORD flush_defer_resource; /* FC 命令/desc/buffer 预检不足，暂缓 */
    DWORD flush_defer_md_slot;  /* md_slot_pair_append 拒绝（md job 忙/无日志块） */
    DWORD flush_defer_build;    /* ftl_async_write_page 建链失败 */
    DWORD flush_defer_submit;   /* ftl_op_submit 拒绝 */
    DWORD flush_backup_defer;   /* 备副本提交被序列门控拒绝，保留命令下轮重试 */
    DWORD flush_backup_resume;  /* 保留的备副本命令最终提交成功 */
    DWORD evict_fail;           /* l2p_line_evict 找不到可驱逐行 */

    stats_hist_s load_lat_us; /* region 加载延迟（提交 → 完成） */
#endif
} l2p_cache_stats_s;

typedef struct L2P_PREFETCH_STATISTICS_S
{
    DWORD calls;    /* 顺序读预取调用次数 */
    DWORD started;  /* 成功起加载 / 直接置 CLEAN 的次数 */
    DWORD skipped;  /* 资源/行/thrashing/越界跳过 */
    DWORD resident; /* 目标已驻留 */
    DWORD hits;     /* 预取行第一次被 host 命中的次数 */
} l2p_prefetch_stats_s;

typedef struct L2P_THRASH_S
{
    DWORD window_access; /* 本窗口累计访问数（hit+miss） */
    DWORD window_miss;   /* 本窗口累计 miss 数 */
    BYTE  miss_pct;      /* 最近一次窗口结算的 miss 率（0..100） */
    BOOL  thrashing;     /* true = 处于 thrashing 降级态（滞回保持） */
} l2p_thrash_s;

typedef struct L2P_INFLIGHT_S
{
    BYTE load;                       /* 在飞 region 加载数（观测） */
    BYTE flush;                      /* 在飞 region 写回数（背压闸门，上限 L2P_FLUSH_MAX_INFLIGHT） */
    BYTE flush_pending[FTL_DIE_MAX]; /* 按 owner die：备副本命令等待提交的写回数 */
} l2p_inflight_s;

typedef struct L2P_CACHE_S
{
    l2p_cache_line_s lines[L2P_CACHE_SIZE];           /* 64 × ~2KB ≈ 133 KB（映射数据 + 写回/完成状态） */
    l2p_tag_s        tags[L2P_CACHE_SIZE];            /* 查找索引：与 lines 同下标；region_id/state/access_count */
    BYTE             line_hint[L2P_LINE_HINT_SLOTS];  /* region → line 下标提示；0xFF = 空；以 tags[] 校验 */
    l2p_cmt_entry_s  cmt[L2P_CMT_SETS][L2P_CMT_WAYS]; /* entry 级写缓存（4-way） */
    WORD             cmt_clock;                       /* LRU 时间戳发生器（回绕时右移全部 stamp） */
    WORD             clock_hand;                      /* CLOCK 替换指针 */

    l2p_cache_stats_s    stats;
    l2p_prefetch_stats_s prefetch;
    l2p_thrash_s         thrash;

    /* Region 级脏标记（全局 region 下标）。 */
    DWORD region_dirty_bitmap[L2P_DIRTY_BITMAP_WORDS];

    /* Region 级「降级 = 映射已丢」标记（全局 region 下标，不持久化）。 */
    DWORD region_degraded_bitmap[L2P_DIRTY_BITMAP_WORDS];

    /* dirty CMT 的索引/计数：bitmap 与 owner 计数都由 dirty 插入 / 整页写回
     * 两个唯一闸门维护；`cmt_dirty` 是总数（stats 里另有观测副本）。 */
    DWORD cmt_dirty_region_bitmap[L2P_DIRTY_BITMAP_WORDS];
    DWORD cmt_dirty_owner[FTL_DIE_MAX];

    BOOL cmt_enabled; /* false = boot replay：不查不填 CMT，全部走 region line */

    BYTE ch; /* 本 cache 服务的 (ch,ce)，跨 LUN 共享 */
    BYTE ce;

    l2p_inflight_s inflight;

    /* 心跳基准：有在飞 region 时每满 1s 打一行计数。 */
    DWORD job_log_ms;

    l2p_owner_fn_t owner_of_region;
    void          *owner_ctx;
} l2p_cache_s;

/* ── 查询（替代外部直读 cache 内部） ── */
typedef struct L2P_LINE_INFORMATION_S
{
    DWORD             region_id; /* 全局 region（INVALID 行无意义） */
    l2p_cache_state_e state;     /* 行状态（枚举，不是裸 BYTE） */
} l2p_line_info_s;

_Static_assert((L2P_GLOBAL_REGIONS_MAX % 32U) == 0U, "全局 region 位图必须按 32 bit 对齐");

_Static_assert(L2P_GLOBAL_LBA_MAX_AT_MAX <= (QWORD)UINT32_MAX, "全局 LBA 超出 DWORD 位宽");

_Static_assert(L2P_LOCAL_DIRTY_WORDS == 64U, "on-media Checkpoint 位图固定 64 word");

_Static_assert(sizeof(l2p_tag_s) == 8U, "l2p_tag_s 必须保持 8 B（64 项 = 512 B，查找只碰 ~16 条 cache line）");

_Static_assert((L2P_LINE_HINT_SLOTS & (L2P_LINE_HINT_SLOTS - 1U)) == 0U, "hint 槽数必须是 2 的幂");

_Static_assert(L2P_CACHE_SIZE < UINT8_MAX, "hint 用 BYTE 存 line 下标，0xFF 是空槽");

_Static_assert((L2P_CMT_SETS & (L2P_CMT_SETS - 1U)) == 0U, "L2P_CMT_SETS 必须是 2 的幂");

_Static_assert(L2P_CMT_SIZE == 4096U, "CMT 共 4096 项（1024 set × 4 way，64 KiB）");

_Static_assert(sizeof(l2p_cmt_entry_s) == 16U, "l2p_cmt_entry_s 必须保持 16 B");

/**
 * @brief 全局 LBA 是否路由到指定实例（META/journal 边界校验用；release 也生效）。
 *
 * @param glba 全局 LBA
 * @param topo 目标实例的通道/片选/LUN 坐标
 * @return true = 路由到该实例
 *         false = 路由到其他实例
 */
static inline BOOL lba_routes_to(DWORD glba, ftl_topo_s topo)
{
    return (lba_2_ch(glba) == topo.ch) && (lba_2_ce(glba) == topo.ce) && (lba_2_lun(glba) == topo.lun);
}

/**
 * @brief 全局 LBA → region 内的 entry 下标。
 *
 * @param lba 全局 LBA
 * @return entry 下标（lba & L2P_REGION_MASK）
 */
static inline WORD lba_to_entry(DWORD lba)
{
    return (lba & L2P_REGION_MASK);
}

/**
 * @brief 同一实例 LBA 流的下一个 L2P region 基址（顺序流跨 region 预加载用）。
 *
 * @param lba 当前 LBA
 * @return 下一个 region 的起始 LBA；没有下一个（超出 L2P 键空间）⇒ FTL_LBA_INVALID
 *
 * @note DIE_MAX=1 时就是 `(region + 1) × 512 LBA`；DIE_MAX>1 时按 `DIE_MAX` 步进
 *       （该实例自己的下一个 region）。调用方仍需用 `lba_routes_to()` 校验归属
 *       （DIE_MAX>1 时可能落在别的实例上），并按主机容量决定是否值得预加载。
 */
static inline DWORD lba_next_region_base(DWORD lba)
{
    const DWORD next = lba_to_global_region(lba) + DIE_MAX;

    if (next >= L2P_GLOBAL_REGIONS)
    {
        return FTL_LBA_INVALID;
    }

    // LBA 是 32 位地址字（H9）；超大配置下键空间若超出 32 位则视为"没有下一个"。
    const QWORD base = (QWORD)next * L2P_ENTRIES_PER_REGION;

    return (base > (QWORD)UINT32_MAX) ? FTL_LBA_INVALID : (DWORD)base;
}

/**
 * @brief 全局 region → owner die 内的局部 region（映射页落盘位置）。
 *
 * @param global_region 全局 region
 * @return owner die 内的局部 region（global_region / DIE_MAX）
 */
static inline DWORD l2p_global_to_local_region(DWORD global_region)
{
    ASSERT(DIE_MAX != 0U);
    return global_region / DIE_MAX;
}

/**
 * @brief owner die 内的局部 region → 全局 region
 *
 * @param local_region 本 die 的 local region
 * @param die          owner die
 * @return 全局 region（local_region * DIE_MAX + die）
 */
static inline DWORD l2p_local_to_global_region(DWORD local_region, BYTE die)
{
    return (local_region * DIE_MAX) + die;
}

/**
 * @brief region 是否属于 die 这个实例。
 *
 * @param global_region 全局 region
 * @param die           全局实例编号
 * @return true = 属于该实例
 *         false = 属于其他实例
 */
static inline BOOL l2p_region_owned_by(DWORD global_region, BYTE die)
{
    ASSERT(DIE_MAX != 0U);
    return (global_region % DIE_MAX) == die;
}

/* ── 位图原语 ──
 * 脏位图是 flat 绝对编号（bit = 全局 region）。set/clear/test 集中在此，
 * 避免调用点各自手写 word 下标与移位（H15 / C9）。 */
/**
 * @brief 置位图中的一位。
 *
 * @param bitmap 位图基址
 * @param bit    位下标（flat 绝对编号）
 */
static inline void l2p_bitmap_set(DWORD *bitmap, DWORD bit)
{
    bitmap[bit / 32U] |= (1UL << (bit & 31U));
}

/**
 * @brief 清位图中的一位。
 *
 * @param bitmap 位图基址
 * @param bit    位下标（flat 绝对编号）
 */
static inline void l2p_bitmap_clear(DWORD *bitmap, DWORD bit)
{
    bitmap[bit / 32U] &= ~(1UL << (bit & 31U));
}

/**
 * @brief 测试一个 flat 位图位（bit = 全局 region / LBA 下标）
 *
 * @param bitmap 位图基址
 * @param bit    位下标
 * @return true = 置位
 */
static inline BOOL l2p_bitmap_test_bit(const DWORD *bitmap, DWORD bit)
{
    return (bitmap[bit / 32U] & (1UL << (bit & 31U))) != 0U;
}

/**
 * @brief 该 region 是否有 dirty CMT 条目（O(1) 位图查询）
 *
 * @param cache  共享 cache
 * @param region 全局 region
 * @return true = 至少一个 dirty 条目等待整页写回
 */
static inline BOOL l2p_cmt_region_dirty(const l2p_cache_s *cache, DWORD region)
{
    if (region >= L2P_GLOBAL_REGIONS)
    {
        return false;
    }
    return l2p_bitmap_test_bit(cache->cmt_dirty_region_bitmap, region);
}

/**
 * @brief 该 region 当前是否仍有未 commit 的脏数据（实时位）
 *
 * @param cache  共享 cache
 * @param region 全局 region
 * @return true = 仍有 line DIRTY 或 dirty CMT
 */
static inline BOOL l2p_region_dirty_live(const l2p_cache_s *cache, DWORD region)
{
    if (region >= L2P_GLOBAL_REGIONS)
    {
        return false;
    }
    return l2p_bitmap_test_bit(cache->region_dirty_bitmap, region);
}

/**
 * @brief 是否有在飞写回（背压/进展判据；不暴露 flush_inflight 字段）。
 *
 * @param cache 共享 cache
 * @return true = 有在飞写回
 *         false = 无在飞写回
 */
static inline BOOL l2p_has_flush_inflight(const l2p_cache_s *cache)
{
    return cache->inflight.flush != 0U;
}

/**
 * @brief 是否**确实有** L2P 加载/写回命令在飞（drain 的"确定性进展"判据）。
 *
 * 主流判据是「进展 = 已提交命令的完成」（队列/完成模型）：只有真在飞的命令才有
 * 确定完成时刻。用"我刚才想加载"当进展，会让卡死检测永久失效——本仓曾因此形成
 * 环形等待：加载启动失败 → 声称在飞 → drain 不计 stall →
 * 组永不结束 → 序列门控永不打开 → 脏行永远排不出去。
 *
 * @param cache L2P cache
 * @return true = 至少一条加载或写回命令在飞
 */
static inline BOOL l2p_cache_any_inflight(const l2p_cache_s *cache)
{
    return (cache->inflight.load > 0U) || (cache->inflight.flush > 0U);
}

/** @brief 某 LBA 所属 region 的 line 进度代数（GC 的依赖活性判据；未驻留 = 0）。 */
DWORD l2p_line_progress(l2p_cache_s *cache, DWORD lba);

/** @brief 取 region 当前行状态（不更新 hint / 命中统计；未驻留 = L2P_CACHE_INVALID）。 */
l2p_cache_state_e l2p_region_state_get(const l2p_cache_s *cache, DWORD region);

/** @brief 登记某个 die 的 owner 端口（boot 时由 ftl_instance_init 调用，按值拷贝）。 */
void l2p_owner_register(BYTE die, const l2p_owner_s *owner);

/** @brief 取 (ch,ce) 对应的共享 cache（定义在 l2p_cache.c）；不存在 / 无 present LUN ⇒ NULL。 */
l2p_cache_s *l2p_cache_workspace_get(ftl_topo_s topo);

/**
 * @brief 开机恢复（F4）专用：把一个 region 页同步预载进 cache。
 *
 * @note 该 region 已有驻留行（journal 回放 / 已预载）⇒ 直接返回 true，维持
 *       「一个 region 只占一行」。
 * @note 完成态错误会在主/备之间重试；持续失败 ⇒ 行降级为全 INVALID CLEAN 并置
 *       `region_degraded_bitmap`，返回 FTL_OK；调用者用 `l2p_is_region_degraded`
 *       区分「正常预载」与「范围化降级」。
 */
ftl_status_e l2p_region_preload(l2p_cache_s *cache, DWORD global_region, DWORD primary, DWORD backup);

/**
 * @brief 顺序读 best-effort 预取一个 region（非阻塞）。
 *
 * @note 会走 CLOCK 驱逐，但**不踢 DIRTY 写回**（预取不制造写回）；thrashing /
 *       在飞 load 达上限 / FC 资源不足 / 无 CLEAN 候选 ⇒ 跳过并计数。
 */
BOOL l2p_region_prefetch(l2p_cache_s *cache, DWORD global_region);

/**
 * @brief 读取全局 LBA 对应的 PBA 映射（三态）。
 *
 * @note FTL_OK 时 `*out` 可能是 `NAND_PBA_INVALID`（合法**未映射**）；FTL_AGAIN 才是
 *       「**还不知道**」。二者绝不可混同（把后者当前者会让 host 读到假数据）。
 * @note FTL_ERROR **也会**写 `*out`（= INVALID）：调用点既有口径是「读得 INVALID 与
 *       未映射同义」（见 TRIM job / rr_select_plane）。
 */
ftl_status_e l2p_read(l2p_cache_s *cache, DWORD lba, DWORD *out);

/**
 * @brief 确保全局 LBA 所在 region 已在 RAM（就绪前置）。
 *
 * @note 就绪探针，不是一次访问：命中不记账、不消费 fresh、不动 access_count；
 *       未命中记一次 miss（加载由它起），保持「一次 host 写 = 1 miss + 1 hit」。
 * @note 调用者先对全部 LBA 过一遍，全 true 时后续 read/write 必不再返 FTL_AGAIN
 *       ⇒ 一批改动全有或全无。
 */
BOOL l2p_is_ready(l2p_cache_s *cache, DWORD lba);

/**
 * @brief 纯驻留探针：只查 cache 行，不启动加载、不记账。
 *
 * @note 只说明「region 行可读」；CLEAN line 的 entries 对 dirty CMT 覆盖的 LBA 可能
 *       陈旧，取映射必须走 `l2p_read`（CMT 优先）。
 */
BOOL l2p_is_resident(l2p_cache_s *cache, DWORD lba);

/**
 * @brief 写入全局 LBA → PBA 映射（三态）。
 *
 * @note 优先写 dirty CMT；CMT 无槽位时回退为 line DIRTY（容量竞态兜底，不返 AGAIN）。
 *       只改 RAM，不写 NAND。
 * @note line LOADING / FLUSHING ⇒ FTL_AGAIN，**不改任何状态**；越界 ⇒ FTL_OK
 *       （no-op，调用点自带范围检查）。
 */
ftl_status_e l2p_write(l2p_cache_s *cache, DWORD lba, DWORD pba);

/**
 * @brief 页内批量读（写确认收敛的读侧）。
 *
 * @note `lba` 必须页对对齐、mask 内全部 LBA 落在同一 region（编译期几何保证）；
 *       只处理 mask 置位条目，语义与逐条 `l2p_read` 一致；行 LOADING ⇒ FTL_AGAIN
 *       且不改状态。
 */
ftl_status_e l2p_entries_read(l2p_cache_s *cache, DWORD lba, DWORD mask, DWORD out[]);

/**
 * @brief 页内批量写（写确认收敛）。
 *
 * @note 前置与 mask/几何约束同 `l2p_entries_read`；全部通过后才置一次
 *       `state=DIRTY` + region 脏位，失败不产生半个状态。
 */
ftl_status_e l2p_entries_write(l2p_cache_s *cache, DWORD lba, DWORD mask, const DWORD pba[]);

/**
 * @brief 批量写确认的就绪探针（旧值来源 + CMT 容量一次判完）。
 *
 * @note mask 内每个 LBA 都必须有 old 来源（CMT 或 line），且 CLEAN line 上缺失的
 *       CMT 条目必须有 empty/clean 槽位；任一条不满足 ⇒ 起加载 / 触发写回并返回
 *       false，整批不动。返回 true 后到 `l2p_entries_write` 之间无泵 ⇒ 不会返 AGAIN。
 */
BOOL l2p_entries_is_ready(l2p_cache_s *cache, DWORD lba, DWORD mask);

/**
 * @brief 开关 CMT（仅 boot journal replay 使用）。
 *
 * @note false 期间 read / is_ready / write 全部走 region line，不查不填 CMT；
 *       恢复后 CMT 保持为空。**禁止在运行期关闭**（会丢 dirty CMT）。
 */
void l2p_cmt_set_enabled(l2p_cache_s *cache, BOOL enabled);

#if defined(FC_NAND_TEST_ENABLE)
/** @brief 测试专用：驱动唯一 dirty 计数释放闸门（L2P-22 负例）。 */
void l2p_test_cmt_dirty_release(l2p_cache_s *cache, DWORD lba);

/** @brief 测试专用：查询 CMT set 是否还有可无写回插入的槽位（L2P-16）。 */
BOOL l2p_test_cmt_room(const l2p_cache_s *cache, DWORD lba);

/** @brief 测试专用：查询 CMT 写回闸门（不含 thrashing）。 */
BOOL l2p_test_owner_flush_ready(const l2p_cache_s *cache, BYTE owner_die, BOOL force_cmt);
#endif

/**
 * @brief 启动一个 translation page 的 CMT 整页写回。
 *
 * @note 行驻留 ⇒ 合并 dirty CMT 后走 `l2p_flush_kick`；行缺失 ⇒ 起异步加载，
 *       下一轮再合并写回。调用者（CP / 后台 flush）负责轮询 pending。
 */
ftl_status_e l2p_region_flush_kick(l2p_cache_s *cache, DWORD region);

/**
 * @brief Checkpoint DONE 前的最终覆盖检查。
 *
 * @note 每个 snapshot 置位 region 必须「实时脏位已清」或「行已 CLEAN」；否则 Header
 *       会声称一个没落盘的 region ⇒ 调用者中止 CP。
 */
BOOL l2p_owner_snapshot_is_covered(l2p_cache_s *cache, BYTE owner_die, const DWORD snapshot[L2P_LOCAL_DIRTY_WORDS]);

/* ── 同步逃生口（见文件头；服务函数禁用）── */

/** @brief 自旋等到就绪再读（同步逃生口；超预算 ⇒ FTL_ERROR）。 */
ftl_status_e l2p_read_sync(l2p_cache_s *cache, DWORD lba, DWORD *out);

/** @brief 自旋等到就绪再写（同步逃生口；超预算 ⇒ FTL_ERROR）。 */
ftl_status_e l2p_write_sync(l2p_cache_s *cache, DWORD lba, DWORD pba);

/**
 * @brief 把 region 的失败页标记为「映射已丢」并精确失效其 CMT。
 *
 * @note 失败页的 dirty 条目指向编程失败的 PBA，撤销完成前读必须失败；同 region
 *       其他 dirty 条目是更早的成功写，保留；CLEAN 条目全部失效。
 */
void l2p_note_region_lost(l2p_cache_s *cache, DWORD global_region, DWORD lba, DWORD cw_bitmap);

/**
 * @brief 该 region 是否已降级为「全未映射」（映射已丢）。
 *
 * @note host 读路径：未映射且 region 降级 ⇒ 返回 IO_ERROR，**不零填**（合法未映射
 *       仍是零填）；开机恢复据此走范围化恢复而不是全量重建。
 */
BOOL l2p_is_region_degraded(const l2p_cache_s *cache, DWORD global_region);

/**
 * @brief 把一个 region 的 translation page 异步写回（提交即返回）。
 *
 * @note **无条件追加**：绝不就地覆盖旧位置（NAND 只能 1→0，二次编程 = 静默损坏）。
 *       分配不到块（owner Free Pool 空）⇒ FTL_AGAIN；未刷盘的 region 在 Directory
 *       里就是 NAND_PBA_INVALID，没有固定位置可回退。
 * @note 背压（在飞达上限 / 命令池空 / 取不到槽位）⇒ FTL_AGAIN，行保持 DIRTY；
 *       只有地址/建链/提交的**硬失败**才是 FTL_ERROR。两个槽位在取槽时就已消耗：
 *       其后建链/提交失败 ⇒ 两槽作废，但 region 仍指向上一份有效记录。
 */
ftl_status_e l2p_flush_kick(l2p_cache_s *cache, int line_idx);

/**
 * @brief 后台写回推进（每轮 ftl_service 调一次，只处理本 owner）。
 *
 * @note 先按 owner 计数扫 dirty CMT（force_cmt 或达水位才主动写回），再扫最多
 *       max_kick 行 DIRTY（均受共享 L2P_FLUSH_MAX_INFLIGHT 闸门）；跳过不属于
 *       owner_die 的条目——一个 LUN 的后台刷写不替别的 LUN 做决定。
 */
void l2p_owner_flush_kick(l2p_cache_s *cache, BYTE owner_die, BYTE max_kick, BOOL force_cmt);

/** @brief 重试本 die 已预留的备副本命令提交（主副本完成后可能被序列门控拒绝）。 */
void l2p_flush_resume(l2p_cache_s *cache, BYTE die);

/**
 * @brief 本 owner 是否已无待落盘的 L2P 行。
 *
 * @note 异步 FLUSH 的完成判据：无 dirty CMT，且没有 DIRTY / FLUSHING / **LOADING**
 *       行；LOADING 一并等待（行内容尚未有效，等它落定比提前完成安全）。
 */
BOOL l2p_flush_owner_idle(const l2p_cache_s *cache, BYTE owner_die);

/**
 * @brief 只失效某个 owner 的行与脏位（恢复 / 格式化用）。
 *
 * @note 共享 cache 下**不能**再按实例重新 init（会抹掉别的 LUN 刚回放的 DIRTY 行）。
 * @note 行处于 FLUSHING / LOADING 是编程错误（恢复期不应有在飞命令）⇒ ASSERT。
 */
void l2p_owner_invalidate(l2p_cache_s *cache, BYTE owner_die);

/** @brief 全局脏位图 → 本 die 的局部 2048-bit 快照（Checkpoint 起点调用）。 */
void l2p_owner_bitmap_snapshot(const l2p_cache_s *cache, BYTE owner_die, DWORD out[L2P_LOCAL_DIRTY_WORDS]);

/** @brief 读一行元信息（idx 越界 / 空行时 state == INVALID）。 */
void l2p_line_get(const l2p_cache_s *cache, WORD idx, l2p_line_info_s *out);

/** @brief 失效一行（仅 CLEAN / INVALID；恢复 / 格式化用）。 */
void l2p_line_invalidate(l2p_cache_s *cache, WORD idx);

/* ── 接口 ── */

/**
 * @brief 初始化共享 cache（设备级，每个 (ch,ce) 只调一次）。
 *
 * @note **不能**放进逐实例的 `ftl_init`：第二个实例会把第一个实例的 line 清掉。
 * @note 本函数**不解析 owner**，只保存回调；owner 在具体 load/flush 时解析。
 */
void l2p_init(l2p_cache_s *cache, BYTE ch, BYTE ce, l2p_owner_fn_t owner_of_region, void *owner_ctx);

/** @brief 设备级初始化实际存在的 target 共享 cache（必须早于所有实例 boot）。 */
void l2p_device_init(void);

/**
 * @brief 后台写回推进（每轮调用；不阻塞、不遍历 cache 内部）。
 *
 * @note 按 level 查模块内 static const 表决定本轮 max_kick；只处理 owner_die 的脏行。
 *       同步逃生口（*_sync）自己用 drain deadline，不经过本入口。
 */
void l2p_handle(l2p_cache_s *cache, BYTE die, bg_level_e level);

#endif /* FTL_L2P_CACHE_H */
