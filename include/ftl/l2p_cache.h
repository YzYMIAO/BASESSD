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

#include "allocator.h"  /* alc_t */
#include "ftl_config.h" /* §一 容量/分解宏 + §三.2 DWORD / pba_* */
#include "ftl_module.h" /* bg_info_t / ftl_mod_status_t */
#include "ftl_type.h"   /* DWORD */
#include "metadata.h"   /* metadata_dir_* / md_dir_cache_t（Directory 访问） */

#include <stddef.h>
#include <stdint.h>

/* ── 宏 ──
 * L2P 容量与分解宏（L2P_ENTRIES_PER_REGION / L2P_REGION_SIZE /
 * L2P_REGIONS_PER_INSTANCE / L2P_REGION_SHIFT / L2P_REGION_MASK /
 * FTL_DIM_TOTAL）见 ftl/ftl_config.h §一；
 * 元数据块布局见 ftl/metadata.h「固定元数据区布局」。*/
#define L2P_CACHE_SIZE (64U) /* cache 行数（可配 32/64/128）*/

/* 在飞写回上限（背压闸门，见 l2p_cache_t.flush_inflight）：每次写回占 1 条命令 + 4 buffer，
 * 4 条在飞 = 16 buffer，占 FC buffer 池（128）的 1/8，给 host I/O 留足余量。 */
#define L2P_FLUSH_MAX_INFLIGHT (4U)

/* 后台主动 flush 的每轮条数上界（按档位；docs §4.7 的 static const 策略表）。
 * BUSY 档只放行 1 条 ⇒ 最小预算 = 「must-run + 1 flush」。 */
#define L2P_FLUSH_MAX_NORMAL (4U)
#define L2P_FLUSH_MAX_BUSY   (1U)

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
#define L2P_THRASH_WINDOW    (256U) /* 检测窗口：每 256 次访问结算一次 */
#define L2P_THRASH_ENTER_PCT (90U)  /* 进入阈值：窗口 miss 率 ≥ 90% */
#define L2P_THRASH_EXIT_PCT  (70U)  /* 退出阈值：窗口 miss 率 < 70%（滞回） */

/* ── 全局坐标（cache 跨 LUN 共享）──
 *
 * 数据通路一律用 **host 全局 LBA**；只有「映射页在 owner die 元数据区里的落盘位置」
 * 用局部 region（每个 die 只有 L2P_REGIONS_PER_INSTANCE 个槽）。
 * cache 行/脏位图用全局 region 键；metadata 的 lookup/append/commit 用局部 region。
 */
#define L2P_GLOBAL_REGIONS     (L2P_REGIONS_PER_INSTANCE * FTL_DIM_TOTAL)
#define L2P_GLOBAL_LBA_MAX     ((QWORD)L2P_GLOBAL_REGIONS * L2P_ENTRIES_PER_REGION)
#define L2P_DIRTY_BITMAP_WORDS (L2P_GLOBAL_REGIONS / 32U)
#define L2P_LOCAL_DIRTY_WORDS  (L2P_REGIONS_PER_INSTANCE / 32U)

_Static_assert((L2P_GLOBAL_REGIONS % 32U) == 0U, "全局 region 位图必须按 32 bit 对齐");
_Static_assert(L2P_GLOBAL_LBA_MAX <= (QWORD)UINT32_MAX, "全局 LBA 超出 DWORD 位宽");
_Static_assert(L2P_LOCAL_DIRTY_WORDS == 64U, "on-media Checkpoint 位图固定 64 word");

/* 拓扑 → dim（ftl_dim_* 的逆）：ch + CH*(ce + CE*lun)。 */
static inline DWORD ftl_topo_dim(ftl_topo_t topo)
{
    return topo.ch + (CH_NUM * (topo.ce + (CE_NUM * topo.lun)));
}

/* dim → (ch,ce,lun)：ftl_topo_dim 的逆。
 *
 * **所有** dim → 坐标的分解都走这三个：手写第二份就是第二套 region→die 坐标（C9）。
 * 返回值取自然宽度（0..*_NUM-1）⇒ 调用点不必窄化强转（H44）。 */
static inline BYTE ftl_dim_ch(DWORD dim)
{
    return (dim % CH_NUM);
}
static inline BYTE ftl_dim_ce(DWORD dim)
{
    return ((dim / CH_NUM) % CE_NUM);
}
static inline BYTE ftl_dim_lun(DWORD dim)
{
    return (dim / (CH_NUM * CE_NUM));
}

/* 全局 LBA → 全局 region */
static inline DWORD lba_to_global_region(DWORD glba)
{
    return (glba >> L2P_REGION_SHIFT);
}

/* 全局 region → owner die 内的局部 region（映射页落盘位置） */
static inline DWORD l2p_global_to_local_region(DWORD global_region)
{
    return global_region / FTL_DIM_TOTAL;
}

/* region 是否属于 dim 这个实例 */
static inline bool l2p_region_owned_by(DWORD global_region, DWORD dim)
{
    return (global_region % FTL_DIM_TOTAL) == dim;
}

/* ── Region 分解 ──
 * L2P_REGION_SHIFT（lba / 每 region 条目数）与 L2P_REGION_MASK（lba % 每 region
 * 条目数）见 ftl/ftl_config.h §一。*/

/*
 * 全局 LBA → (ch,ce,lun) 路由。
 *
 * 路由键 = **region 序号**（lba >> L2P_REGION_SHIFT），不是页号也不是条目号：
 * 一个 region（512 个 CW）整体落在同一个 die 上，region 的 flush 只碰一个实例。
 * 再把 region 序号按设备维度**分解**成三维：
 *     dim = region % FTL_DIM_TOTAL
 *     ch  = dim % CH_NUM
 *     ce  = (dim / CH_NUM) % CE_NUM
 *     lun = dim / (CH_NUM × CE_NUM)
 * FTL_DIM_TOTAL = 1 时恒为 (0,0,0)。
 */
static inline DWORD lba_route_dim(QWORD lba)
{
    return ((lba >> L2P_REGION_SHIFT) % FTL_DIM_TOTAL);
}
static inline BYTE lba_route_ch(QWORD lba)
{
    return ftl_dim_ch(lba_route_dim(lba));
}
static inline BYTE lba_route_ce(QWORD lba)
{
    return ftl_dim_ce(lba_route_dim(lba));
}
static inline BYTE lba_route_lun(QWORD lba)
{
    return ftl_dim_lun(lba_route_dim(lba));
}

/* 全局 LBA 是否路由到指定实例（META/journal 边界校验用；release 也生效）。 */
static inline bool lba_routes_to(DWORD glba, ftl_topo_t topo)
{
    return (lba_route_ch(glba) == topo.ch) && (lba_route_ce(glba) == topo.ce) &&
           (lba_route_lun(glba) == topo.lun);
}

static inline WORD lba_to_entry(DWORD lba)
{
    return (lba & L2P_REGION_MASK);
}

/* ── Cache 状态 ── */

typedef enum
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
 * g_op_callbacks[FTL_OP_L2P]（ftl_l2p_callback，l2p_cache.c）按 line->state 收尾。
 * ⇒ 对外 API 的失败语义变成三态，调用者必须能**挂起**（见下）。
 *
 * ⚠ 两条不变量（异步后仍成立，是正确性根基）：
 *   ① **可读不可写**：FLUSHING 行的 entries 是有效快照，读照常命中；写会让"落盘的是
 *      旧快照、RAM 是新值"分叉（完成时无法区分该置 CLEAN 还是 DIRTY）⇒ 写一律返
 *      FTL_AGAIN 让调用者下轮重试。LOADING 行两者都不行（entries 尚未有效）。
 *   ② **同一 region 只有一行**：l2p_find 匹配 CLEAN/DIRTY/FLUSHING/LOADING
 *      （不再是"只匹配 CLEAN/DIRTY"）。异步后 FLUSHING 窗口真实存在，若把它当未命中
 *      就会为同一 region 再分配一行 ⇒ 两行分叉、映射静默不一致。
 *
 * ⚠ **同步逃生口**（`*_sync`）：内部自旋 ftl_service_pump 直到就绪。只许结构性同步
 *   上下文调用（开机恢复 / CLI / checkpoint / HIL `fl`）——它们不在完成派发里，
 *   自旋不是嵌套泵。服务函数（g_op_callbacks 里的任何一个）**禁止**调用，由
 *   ftl_op_is_in_dispatch 断言兜住。服务函数里禁止自旋泵（嵌套派发）。
 */

/* ── Cache Line ── */

typedef struct
{
    DWORD             entries[L2P_ENTRIES_PER_REGION]; /* 512 条 PBA = 2KB */
    DWORD             region_id;                       /* **全局** region（0..L2P_GLOBAL_REGIONS-1） */
    l2p_cache_state_e state;                           /* 行状态 */
    WORD              access_count;                    /* CLOCK 访问计数 */

    /* 「本次加载正是为**当前这次访问**而起」（异步化的观测口径补偿）。
     * 异步后一次 miss 访问会跨两轮：第 1 轮起加载（计一次 miss）、第 2 轮重试命中。
     * 若重试也计一次 hit，则**同一次访问被记成 miss + hit** ⇒ 窗口 miss 率虚降一半
     * （随机读满盘：真值 100% → 观测 50%，跌破 L2P_THRASH_EXIT_PCT=70% ⇒
     * 降级永不触发）。故：加载完成时置 1，其后**第一次**命中消费它且不计命中，
     * 一次逻辑访问在窗口里只记一次 miss。
     * 行结构体当前 2080 B（64 行 ≈ 130 KiB）；本条与 `load_fail_count` /
     * 双副本字段共用尾部对齐填充。 */
    bool fresh;

    /* 1 = 本行最近一次加载在重试耗尽后降级为「全未映射」（映射已丢，RISK-BOOT-18）。
     * 开机恢复与 host 读路径据此区分「合法未映射」与「读不出来被迫当未映射」：
     * 后者 probe 计入 `l2p_degraded_cw`（范围化恢复，不回退全量重建），host 读
     * **返回 IO_ERROR** 而不是零填。每次 load_start / 加载成功清零；降级时置 1。 */
    bool degraded;

    /* 本 region 连续加载失败次数（RISK-L2P-01）。
     * 失败后先置 INVALID 重试，达到 L2P_LOAD_RETRY_MAX 才降级为「全未映射的
     * CLEAN 行」——既不每次访问都重读坏 region（活锁），也不把一次瞬态读错
     * 永久当成「从未写过」。换 region 时由 load_start 清零。
     * ⚠ 计数随行保存：行被驱逐后计数丢失（重试预算重置）。持续访问的坏 region
     *   通常会复用同一 INVALID 行，仍能达到上限；这是用 1B/行换「不做全 region 计数表」。 */
    BYTE load_fail_count;

    /* ── 双副本写回（RISK-BOOT-18 的 L2）──
     * 串行写：先主副本；主成功后提交备份副本。两份都成功才 commit（原子）；任一失败
     * 都保持 DIRTY、不登记新位置（旧 pair 仍是 Directory 真值，槽位作废）。 */
    DWORD flush_pba[L2P_REGION_COPIES]; /* 0=主 / 1=备 */
    BYTE  flush_issued;                 /* 已提交副数（1 或 2） */
    BYTE  flush_done;                   /* 已完成副数 */
    bool  flush_fail;                   /* true = 任一副本失败 ⇒ 不提交 */

    /* 本次加载读的是哪一份（主备回退，RISK-BOOT-18 的 L2）。 */
    DWORD load_pba;        /* 在飞/最近一次加载的 PBA */
    bool  load_use_backup; /* true = 当前这次读的是备份副本 */
} l2p_cache_line_t;

/* ── Owner 端口（docs §4.7）──
 *
 * cache 跨 LUN 共享后，load/flush/commit 需要知道某个全局 region 属于哪个
 * FTL 实例（owner die）。端口只暴露**共享服务的最小集**，不返回整个 ftl_context。
 * 回调由设备级 init 注入（main.c），内部复用 lba_route_* + ct_get_route。
 */
typedef struct
{
    DWORD           dim; /* 本 owner 的实例编号（global 路由键） */
    md_dir_cache_t *metadata_dir;
    alc_t          *alloc;
    ftl_topo_t      topo;
} l2p_owner_t;

typedef const l2p_owner_t *(*l2p_owner_fn_t)(void *ctx, DWORD global_region);

/* ── Cache 上下文 ── */

typedef struct
{
    l2p_cache_line_t lines[L2P_CACHE_SIZE]; /* 64 × (2KB + 4B) ≈ 128 KB */
    WORD             clock_hand;            /* CLOCK 替换指针 */
    DWORD            hit_count;             /* 统计：命中次数（开机累计，观测用） */
    DWORD            miss_count;            /* 统计：未命中次数（开机累计，观测用） */
    DWORD            eviction_count;        /* 统计：被驱逐的行数（开机累计） */
    int32_t          error;                 /* 最近一次硬失败（status.error） */

    /* ── Thrashing 检测窗口（阈值见文件头 L2P_THRASH_*）──
     * 与累计 hit/miss_count 互补：累计量随开机时长渐近，反映不了**当前**行为，
     * 降级判定必须看窗口。窗口满即结算并清零，miss_pct 保留最近一次结算结果。 */
    DWORD window_access; /* 本窗口累计访问数（hit+miss） */
    DWORD window_miss;   /* 本窗口累计 miss 数 */
    BYTE  miss_pct;      /* 最近一次窗口结算的 miss 率（0..100） */
    bool  thrashing;     /* true = 处于 thrashing 降级态（滞回保持） */

    /* Region 级脏标记（自上次 checkpoint 以来该 region 有过写）。
     * 与 line 级 state 互补：line DIRTY 只描述 resident 行，evict 兜底
     * flush 后行 CLEAN/失效，但 region 的"本次周期内被改过"仍由本 bitmap
     * 记住。Checkpoint job 以它填**本 die 的局部** region_bitmap 快照。
     * bit 下标 = **全局** region；flush 落盘后清对应全局位。 */
    DWORD region_dirty_bitmap[L2P_DIRTY_BITMAP_WORDS];

    /* Region 级「降级 = 映射已丢」标记（RISK-BOOT-18 的范围化恢复）。
     * line->degraded 只随 resident 行存在，行被驱逐/重载后就没了 ⇒ host 读会把
     * 丢失的 LBA 当“合法未映射”零填。本 bitmap 在**本次启动内**记住降级 region，
     * 由 `l2p_is_region_degraded()` 优先查询。⚠ **它不阻止加载**：降级后 region 仍
     * 可能被 flush 到新页（journal 回放 / host 写入），加载必须走 Directory 读回真页；
     * 位图只决定「未映射的 LBA 报 IO_ERROR 还是零填」。
     * ⚠ 不持久化：跨启动的丢失语义见 RISK-BOOT-20（主备副本已消除单副本失效）。
     * bit 下标 = **全局** region。 */
    DWORD region_degraded_bitmap[L2P_DIRTY_BITMAP_WORDS];

    /* 本 cache 服务的 (ch,ce)。cache 跨 LUN 共享，故不再有 lun 字段；
     * 每条 line 的 owner LUN 由 region 经 owner_of_region 解析。 */
    BYTE ch;
    BYTE ce;

    /* 在飞命令数。load_inflight 供观测；flush_inflight 是**背压闸门**
     * ——每次写回占 1 条命令 + 4 个 buffer（L2P region 页 = 2KB = 4 CW），不设上限会把
     * FC 池吃光 ⇒ host I/O 拿不到命令、全系统 FTL_AGAIN。上限见 L2P_FLUSH_MAX_INFLIGHT。
     * ⚠ 共享 cache 下这是**全 (ch,ce) 共用的 credit 池**（主流资源模型）；
     *   跨 LUN 公平性靠调度/后续实测，不做 per-owner 计数。 */
    BYTE load_inflight;
    BYTE flush_inflight;

    /* 心跳基准（见 ftl_job.h）：有在飞 region（load/flush）时每满 1s 打一行计数。
     * ⚠ 本模块没有"单 job"结构（在飞的是若干 cache line）⇒ 只报计数，不报 phase/age。 */
    DWORD job_log_ms;

    /* 全局 region → owner FTL 实例（按 lba_route_* / ct_get_route 解析）。
     * load/flush/commit 用 owner 的 metadata_dir / alloc / topo，cache 自身不再持有
     * 单一实例的依赖指针。设备级 init 注入一次，生命周期到复位。 */
    l2p_owner_fn_t owner_of_region;
    void          *owner_ctx;
} l2p_cache_t;

/* ── 接口 ── */

/*
 * l2p_init - 初始化共享 cache（设备级，每个 (ch,ce) 只调一次）
 *
 * 清全局脏位图 / 所有 line / 统计 / 在飞计数；写入 (ch,ce) 与 owner 解析回调。
 * ⚠ **不能**放进逐实例的 `ftl_init`：第二个实例会把第一个实例的 line 清掉。
 * ⚠ 本函数**不解析 owner**，只保存回调；owner 在具体 load/flush 时解析。
 *
 * @param cache          cache 上下文
 * @param ch/ce          本 cache 服务的通道/片选
 * @param owner_of_region 全局 region → owner FTL 实例的回调（非空）
 * @param owner_ctx      回调透传上下文
 */
void l2p_init(l2p_cache_t *cache, BYTE ch, BYTE ce, l2p_owner_fn_t owner_of_region, void *owner_ctx);

/*
 * l2p_preload_region - 开机恢复（F4）专用：把一个 region 页同步预载进 cache。
 *
 * 在 cache 中找一条 INVALID 行，先写 region_id，再把 pba 的 region 页同步读进 entries；
 * 成功置 CLEAN，失败置 INVALID（交给懒加载兜底）。
 * 若该 global_region 已有驻留行（journal 回放的 DIRTY / 已预载），直接返回 true，
 * 从而维持「一个 region 只占一行」的不变量。
 *
 * @param cache         共享 cache
 * @param global_region 全局 region（local × FTL_DIM_TOTAL + dim）
 * @param pba           Directory 给出的 region 页物理地址
 * @return true = 已驻留或预载成功；false = 无 INVALID 行 / 读失败
 */
bool l2p_preload_region(l2p_cache_t *cache, DWORD global_region, DWORD pba);

/*
 * l2p_read - 读取**全局 LBA** 对应的 PBA 映射（**三态**）
 *
 *   hit（CLEAN / DIRTY / FLUSHING）→ *out = entry，返回 FTL_OK
 *   miss → CLOCK 选 victim（DIRTY 先起异步写回、LOADING/FLUSHING 跳过）、
 *          起异步加载、置 LOADING，返回 FTL_AGAIN（**本次未取到值**）
 *   正在加载（LOADING）→ 返回 FTL_AGAIN
 *   越界 → *out = NAND_PBA_INVALID，返回 FTL_ERROR
 *
 * ⚠ FTL_OK 时 *out 可能是 NAND_PBA_INVALID —— 那是"**未映射**"，是合法结果；
 *   FTL_AGAIN 才是"**还不知道**"。二者绝不可混同（把后者当前者会让 host 读到假数据）。
 * ⚠ FTL_ERROR 也**会**写 *out（= INVALID）：调用点的既有口径是"读得 INVALID 与未映射
 *   同义"（见 TRIM job / rr_select_plane），不回填就是未初始化变量。
 *
 * @param cache  cache 上下文
 * @param lba    **全局** LBA（host LBA；数据通路不做局部/全局预转）
 * @param out    出参 PBA（FTL_OK 与 FTL_ERROR 时有效；FTL_AGAIN 时未动）
 * @return FTL_OK / FTL_AGAIN（region 加载中，稍后重试）/ FTL_ERROR（lba 越界；
 *         out 由调用者提供；cache 由 ftl_contract_audit 在 boot 期证明）
 */
ftl_status_e l2p_read(l2p_cache_t *cache, DWORD lba, DWORD *out);

/*
 * l2p_is_ready - 确保**全局 LBA** 所在 region 已在 RAM（就绪前置）
 *
 * 命中（CLEAN / DIRTY）⇒ 1；未命中 ⇒ 起异步加载并返回 0；LOADING / FLUSHING ⇒ 0。
 * 供"必须先就绪、再一次性应用多条改动"的调用者用（写确认 / 搬移收尾）：
 * 先对全部 LBA 过一遍本函数，全 1 时后续的 read/write 必不可能再返 FTL_AGAIN
 * ⇒ 一批改动**全有或全无**，重试不会重复记账。
 *
 * ⚠ **就绪探针，不是一次访问**：命中不记账（记账交给随后的 read/write）、不消费
 *   fresh、不动 access_count；未命中记一次 miss（因为加载是它起的）。如此
 *   "一次 host 写 = 1 miss + 1 hit"，thrashing 窗口口径与一次访问一致。
 *
 * @return true 就绪；false 未就绪（已在加载中，稍后重试）
 */
bool l2p_is_ready(l2p_cache_t *cache, DWORD lba);

/*
 * l2p_write - 写入**全局 LBA** → PBA 映射（**三态**）
 *
 * 只改 cache line 并置 DIRTY + 全局 region 脏位，不写 NAND（写回由 flush 起）。
 * 行未就绪（LOADING / FLUSHING）⇒ 返回 FTL_AGAIN，**不改任何状态**。
 * 越界 ⇒ FTL_OK（no-op，与旧行为一致：越界调用点都自带范围检查）。
 *
 * @return FTL_OK / FTL_AGAIN（行在飞，稍后重试）
 */
ftl_status_e l2p_write(l2p_cache_t *cache, DWORD lba, DWORD pba);

/*
 * l2p_read_entries / l2p_write_entries - 页内批量读/写（写确认收敛）
 *
 * `lba` 必须是页对基址（`FTL_LBA_PER_PAIR` 对齐），`mask` 的 bit0..7 对应
 * `lba+0..lba+7`；全部 LBA 必须落在同一 region（页对对齐 + region 大小是
 * `FTL_LBA_PER_PAIR` 的整数倍 ⇒ 编译期几何保证，见 l2p_cache.c 的 `_Static_assert`）。
 * 语义与逐条 `l2p_read`/`l2p_write` 一致：
 *   · 只处理 mask 置位的条目；每条仍调用 `l2p_note_hit`，thrashing/命中口径不变；
 *   · `l2p_read_entries` 的 miss 与逐条 `l2p_read` 一致：记一次 miss + 起异步加载，
 *     返 FTL_AGAIN；当前调用点先 `l2p_is_ready(base)`，正常不会走到该分支；
 *   · 行未就绪（LOADING；写还含 FLUSHING）⇒ FTL_AGAIN，且**不改任何状态**；
 *   · 写成功只置一次 `state=DIRTY` + region 脏位。
 * 用于 `ftl_confirm_write`：`l2p_find` 每页一次，而不是每 CW 一次。
 */
ftl_status_e l2p_read_entries(l2p_cache_t *cache, DWORD lba, DWORD mask, DWORD out[]);
ftl_status_e l2p_write_entries(l2p_cache_t *cache, DWORD lba, DWORD mask, const DWORD pba[]);

/* ── 同步逃生口（见文件头；服务函数禁用）── */

/*
 * l2p_read_sync / l2p_write_sync - 自旋等到就绪再读/写
 *
 * @param lba **全局** LBA
 * @return 同 l2p_read / l2p_write；自旋超预算 ⇒ FTL_ERROR。
 */
ftl_status_e l2p_read_sync(l2p_cache_t *cache, DWORD lba, DWORD *out);

/*
 * l2p_is_region_degraded - 该 region 是否已降级为「全未映射」（映射已丢）
 *
 * 两处消费者：
 *   · 开机恢复：journal 回放 / 块元数据 probe 据此走范围化恢复而不是全量重建；
 *   · host 读路径：未映射且 region 降级 ⇒ 返回 IO_ERROR，**不零填**
 *     （合法未映射仍是零填；RISK-BOOT-18）。
 *
 * @param cache         共享 L2P cache
 * @param global_region 全局 region（lba_to_global_region(lba)）
 * @return true = 发生过加载失败降级；false = 未驻留 / 合法未映射 / 正常加载
 */
bool         l2p_is_region_degraded(const l2p_cache_t *cache, DWORD global_region);
ftl_status_e l2p_write_sync(l2p_cache_t *cache, DWORD lba, DWORD pba);

/*
 * l2p_flush_kick - 起一行 DIRTY 的异步写回（提交即返回）
 *
 * ① md_append_slot_pair 一次取主/备两个空槽（块满先取新块）
 * ② ftl_async_write_page 建链主副本 + 把 entries 拷进 FC buffer 并提交
 * ③ line->state = FLUSHING；主副本成功后在完成回调里串行提交备副本
 * 完成时（ftl_l2p_callback）：两份都成功 ⇒ md_commit_region_pair + CLEAN + 清 region
 * 脏位；任一失败 ⇒ 不提交、置回 DIRTY 保留下轮重试（旧 pair 仍是 Directory 真值）。
 *
 * ⚠ **无条件追加**：绝不就地覆盖旧位置（NAND 只能 1→0，二次编程 = 静默损坏）。
 *   分配不到块（owner 的 Free Pool 空）⇒ FTL_AGAIN；owner 的 metadata_dir/alloc
 *   由全局 region 解析得到（不是 cache 自带指针）。**不回退任何固定位置**
 *   ——没有固定位置：未刷盘的 region 在 Directory 里就是 NAND_PBA_INVALID。
 * ⚠ 背压（**FTL_AGAIN**）：在飞写回数达 L2P_FLUSH_MAX_INFLIGHT、命令池空、取不到槽位
 *   ⇒ 行保持 DIRTY，下轮重试。只有地址/建链/提交的**硬失败**才是 FTL_ERROR。
 *   ⚠ 两个槽位在 append_slot_pair 时就已消耗：若其后建链/提交失败，两槽都作废（写指针已推进），
 *     但信息不丢——region 仍指向上一份有效记录。
 *
 * @return FTL_OK 已提交（含"本行非 DIRTY 无需写回"）/ FTL_AGAIN 背压 / FTL_ERROR 硬失败
 */
ftl_status_e l2p_flush_kick(l2p_cache_t *cache, int line_idx);

/*
 * l2p_flush_kick_owner - 后台写回推进（每轮 ftl_service 调一次，只处理本 owner）
 *
 * 从下标 0 起扫，最多启动 max_kick 行 DIRTY（受共享 L2P_FLUSH_MAX_INFLIGHT 闸门）；
 * 跳过不属于 owner_dim 的行——一个 LUN 的后台刷写不应替别的 LUN 做决定。
 *
 * @param owner_dim 本实例的 dim（ftl_topo_dim(ftl->topo)）
 * @param max_kick  本轮最多起几条（档位表见 l2p_cache.c 的 s_l2p_flush_max）
 */
void l2p_flush_kick_owner(l2p_cache_t *cache, DWORD owner_dim, BYTE max_kick);

/*
 * l2p_flush_owner_idle - 本 owner 是否已无待落盘的 L2P 行
 *
 * 异步 FLUSH job 的完成判据之一：没有 DIRTY / FLUSHING / LOADING 行。
 * LOADING 一并等待：行内容尚未有效，等它落定比提前完成安全（host 侧 BOT 串行，
 * 等待有界）。
 */
bool l2p_flush_owner_idle(const l2p_cache_t *cache, DWORD owner_dim);

/*
 * l2p_invalidate_owner - 只失效某个 owner 的行与脏位（恢复/格式化用）
 *
 * 共享 cache 下**不能**再按实例重新 init（会抹掉别的 LUN 刚回放的 DIRTY 行）。
 * 本函数只处理 l2p_region_owned_by(line->region_id, owner_dim) 的 resident 行：
 * entries 填 0xFF、region_id 清零、置 INVALID，并清该 owner 的全局脏位。
 * ⚠ 行处于 FLUSHING/LOADING 是编程错误（恢复期不应有在飞命令）⇒ ASSERT。
 */
void l2p_invalidate_owner(l2p_cache_t *cache, DWORD owner_dim);

/*
 * l2p_snapshot_owner_bitmap - 全局脏位图 → 本 die 的局部 2048-bit 快照
 *
 * Checkpoint 起点调用（先快照再 flush）。`out` 为 L2P_LOCAL_DIRTY_WORDS 个 word；
 * 映射关系：global_region = local_region * FTL_DIM_TOTAL + owner_dim。
 * DIM=1 时等价于整份位图拷贝。
 */
void l2p_snapshot_owner_bitmap(const l2p_cache_t *cache, DWORD owner_dim, DWORD out[L2P_LOCAL_DIRTY_WORDS]);

/* ── 模块 handle（typed 入口，docs §4.7） ── */
typedef struct
{
    ftl_mod_status_t base;

    BYTE  load_inflight;
    BYTE  flush_inflight;
    WORD  dirty_regions;  /* 本 (ch,ce) 全局脏位图的置位数 */
    WORD  resident_lines; /* 非 INVALID 行数 */
    bool  thrashing;
    BYTE  miss_pct;
    DWORD evictions;
} l2p_status_t;

/*
 * l2p_handle - 后台写回推进（每轮调用；不阻塞、不遍历 cache 内部）
 *
 * 按 info->load_level 查**模块内** static const 表决定本轮 max_kick；只处理
 * owner_dim 的脏行。同步逃生口（*_sync）自己用 drain deadline，不经过本入口。
 *
 * @param cache  共享 cache；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param dim    本实例的 owner dim（ftl_topo_dim(topo)）
 * @param info   本轮动态信息（now_ms / load_level）
 * @param out    出参：本轮结束后的上报（**非空**）
 */
void l2p_handle(l2p_cache_t *cache, DWORD dim, const bg_info_t *info, l2p_status_t *out);

/* ── 查询（替代外部直读 cache 内部） ── */
typedef struct
{
    DWORD region_id; /* 全局 region（INVALID 行无意义） */
    BYTE  state;     /* l2p_cache_state_e */
} l2p_line_info_t;

/* 读一行元信息（idx 越界/空行时 state == INVALID）。 */
void l2p_get_line(const l2p_cache_t *cache, WORD idx, l2p_line_info_t *out);

/* 失效一行（仅 CLEAN/INVALID；恢复/格式化用）。 */
void l2p_invalidate_line(l2p_cache_t *cache, WORD idx);

/* 是否有在飞写回（背压/进展判据；不暴露 flush_inflight 字段）。 */
bool l2p_has_flush_inflight(const l2p_cache_t *cache);

#endif /* FTL_L2P_CACHE_H */