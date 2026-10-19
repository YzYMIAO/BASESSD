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
 *   lba_t → region_id (lba / 512) + entry_idx (lba % 512)
 *   entry = nand_pba_t（FTL 物理地址：CH/CE/LUN 拓扑 + 块/页/plane/CW）
 *   NAND_PBA_INVALID = 0xFFFFFFFF（未映射的 LBA）
 *
 * 替换策略：CLOCK（近似 LRU，O(1) amortized）
 * 写回策略：Write-back（Dirty 标记，延迟写回 NAND）
 */

#include "allocator.h"  /* ftl_allocator_t */
#include "ftl_config.h" /* §一 容量/分解宏 + §三.2 nand_pba_t / pba_* */
#include "ftl_type.h"   /* lba_t */
#include "metadata.h"   /* metadata_dir_* / metadata_dir_cache_t（Directory 访问） */

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
#define L2P_GLOBAL_LBA_MAX     ((uint64_t)L2P_GLOBAL_REGIONS * L2P_ENTRIES_PER_REGION)
#define L2P_DIRTY_BITMAP_WORDS (L2P_GLOBAL_REGIONS / 32U)
#define L2P_LOCAL_DIRTY_WORDS  (L2P_REGIONS_PER_INSTANCE / 32U)

_Static_assert((L2P_GLOBAL_REGIONS % 32U) == 0U, "全局 region 位图必须按 32 bit 对齐");
_Static_assert(L2P_GLOBAL_LBA_MAX <= (uint64_t)UINT32_MAX, "全局 LBA 超出 lba_t 位宽");
_Static_assert(L2P_LOCAL_DIRTY_WORDS == 64U, "on-media Checkpoint 位图固定 64 word");

/* 拓扑 → dim（ftl_dim_* 的逆）：ch + CH*(ce + CE*lun)。 */
static inline uint32_t ftl_topo_dim(ftl_topology_t topo)
{
    return (uint32_t)topo.ch + (CH_NUM * ((uint32_t)topo.ce + (CE_NUM * (uint32_t)topo.lun)));
}

/* dim → (ch,ce,lun)：ftl_topo_dim 的逆。
 *
 * **所有** dim → 坐标的分解都走这三个：手写第二份就是第二套 region→die 坐标（C9）。
 * 返回值取自然宽度（0..*_NUM-1）⇒ 调用点不必窄化强转（H44）。 */
static inline uint8_t ftl_dim_ch(uint32_t dim)
{
    return (uint8_t)(dim % CH_NUM);
}
static inline uint8_t ftl_dim_ce(uint32_t dim)
{
    return (uint8_t)((dim / CH_NUM) % CE_NUM);
}
static inline uint8_t ftl_dim_lun(uint32_t dim)
{
    return (uint8_t)(dim / (CH_NUM * CE_NUM));
}

/* 全局 LBA → 全局 region */
static inline uint32_t lba_to_global_region(lba_t glba)
{
    return (uint32_t)(glba >> L2P_REGION_SHIFT);
}

/* 全局 region → owner die 内的局部 region（映射页落盘位置） */
static inline uint32_t l2p_global_to_local_region(uint32_t global_region)
{
    return global_region / FTL_DIM_TOTAL;
}

/* region 是否属于 dim 这个实例 */
static inline bool l2p_region_owned_by(uint32_t global_region, uint32_t dim)
{
    return (uint32_t)(global_region % FTL_DIM_TOTAL) == dim;
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
static inline uint32_t lba_route_dim(uint64_t lba)
{
    return (uint32_t)((lba >> L2P_REGION_SHIFT) % FTL_DIM_TOTAL);
}
static inline uint8_t lba_route_ch(uint64_t lba)
{
    return ftl_dim_ch(lba_route_dim(lba));
}
static inline uint8_t lba_route_ce(uint64_t lba)
{
    return ftl_dim_ce(lba_route_dim(lba));
}
static inline uint8_t lba_route_lun(uint64_t lba)
{
    return ftl_dim_lun(lba_route_dim(lba));
}

/* 全局 LBA 是否路由到指定实例（META/journal 边界校验用；release 也生效）。 */
static inline bool lba_routes_to(lba_t glba, ftl_topology_t topo)
{
    return (lba_route_ch(glba) == topo.ch) && (lba_route_ce(glba) == topo.ce) &&
           (lba_route_lun(glba) == topo.lun);
}

static inline uint16_t lba_to_entry(lba_t lba)
{
    return (uint16_t)(lba & L2P_REGION_MASK);
}

/* ── Cache 状态 ── */

typedef enum
{
    L2P_CACHE_INVALID = 0, /* cache 行无效，entries 全 NAND_PBA_INVALID */
    L2P_CACHE_CLEAN,       /* cache 行有效，未修改 */
    L2P_CACHE_DIRTY,       /* cache 行有效，已修改（需写回 NAND） */
    L2P_CACHE_FLUSHING,    /* 正在写回 NAND（不可驱逐、不可写；**可读**——entries 仍有效） */
    L2P_CACHE_LOADING,     /* 正在从 NAND 加载 region（不可驱逐、不可读写——entries 尚未有效） */
} l2p_cache_state_t;

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
 *   ② **同一 region 只有一行**：l2p_cache_find 匹配 CLEAN/DIRTY/FLUSHING/LOADING
 *      （不再是"只匹配 CLEAN/DIRTY"）。异步后 FLUSHING 窗口真实存在，若把它当未命中
 *      就会为同一 region 再分配一行 ⇒ 两行分叉、映射静默不一致。
 *
 * ⚠ **同步逃生口**（`*_sync`）：内部自旋 ftl_service_pump 直到就绪。只许结构性同步
 *   上下文调用（开机恢复 / CLI / checkpoint / HIL `fl`）——它们不在完成派发里，
 *   自旋不是嵌套泵。服务函数（g_op_callbacks 里的任何一个）**禁止**调用，由
 *   ftl_op_in_dispatch 断言兜住。服务函数里禁止自旋泵（嵌套派发）。
 */

/* ── Cache Line ── */

typedef struct
{
    nand_pba_t entries[L2P_ENTRIES_PER_REGION]; /* 512 条 PBA = 2KB */
    uint32_t   region_id;                       /* **全局** region（0..L2P_GLOBAL_REGIONS-1） */
    uint8_t    state;                           /* l2p_cache_state_t */
    uint16_t   access_count;                    /* CLOCK 访问计数 */

    /* 「本次加载正是为**当前这次访问**而起」（异步化的观测口径补偿）。
     * 异步后一次 miss 访问会跨两轮：第 1 轮起加载（计一次 miss）、第 2 轮重试命中。
     * 若重试也计一次 hit，则**同一次访问被记成 miss + hit** ⇒ 窗口 miss 率虚降一半
     * （随机读满盘：真值 100% → 观测 50%，跌破 L2P_THRASH_EXIT_PCT=70% ⇒
     * 降级永不触发）。故：加载完成时置 1，其后**第一次**命中消费它且不计命中，
     * 一次逻辑访问在窗口里只记一次 miss。
     * 代价：与 load_fail_count 一起占尾部对齐填充，行结构体按 4B 对齐后
     * 由 2056B 增至 2060B（64 行共 +256B）。 */
    uint8_t fresh;

    /* 本 region 连续加载失败次数（RISK-L2P-01）。
     * 失败后先置 INVALID 重试，达到 L2P_LOAD_RETRY_MAX 才降级为「全未映射的
     * CLEAN 行」——既不每次访问都重读坏 region（活锁），也不把一次瞬态读错
     * 永久当成「从未写过」。换 region 时由 load_start 清零。
     * ⚠ 计数随行保存：行被驱逐后计数丢失（重试预算重置）。持续访问的坏 region
     *   通常会复用同一 INVALID 行，仍能达到上限；这是用 1B/行换「不做全 region 计数表」。 */
    uint8_t load_fail_count;
} l2p_cache_line_t;

/* ── Owner 解析回调 ──
 *
 * cache 跨 LUN 共享后，load/flush/commit 需要知道某个全局 region 属于哪个
 * FTL 实例（owner die）。回调由设备级 init 注入（main.c），内部复用
 * lba_route_* + completion_table_route；l2p_cache.h 只前置声明 ftl_context，
 * 不反向包含 ftl.h（避免循环依赖）。
 */
struct ftl_context;
typedef struct ftl_context *(*l2p_owner_fn_t)(void *ctx, uint32_t global_region);

/* ── Cache 上下文 ── */

typedef struct
{
    l2p_cache_line_t lines[L2P_CACHE_SIZE]; /* 64 × (2KB + 4B) ≈ 128 KB */
    uint16_t         clock_hand;            /* CLOCK 替换指针 */
    uint32_t         hit_count;             /* 统计：命中次数（开机累计，观测用） */
    uint32_t         miss_count;            /* 统计：未命中次数（开机累计，观测用） */

    /* ── Thrashing 检测窗口（阈值见文件头 L2P_THRASH_*）──
     * 与累计 hit/miss_count 互补：累计量随开机时长渐近，反映不了**当前**行为，
     * 降级判定必须看窗口。窗口满即结算并清零，miss_pct 保留最近一次结算结果。 */
    uint32_t window_access; /* 本窗口累计访问数（hit+miss） */
    uint32_t window_miss;   /* 本窗口累计 miss 数 */
    uint8_t  miss_pct;      /* 最近一次窗口结算的 miss 率（0..100） */
    uint8_t  thrashing;     /* 1 = 处于 thrashing 降级态（滞回保持） */

    /* Region 级脏标记（自上次 checkpoint 以来该 region 有过写）。
     * 与 line 级 state 互补：line DIRTY 只描述 resident 行，evict 兜底
     * flush 后行 CLEAN/失效，但 region 的"本次周期内被改过"仍由本 bitmap
     * 记住。Checkpoint job 以它填**本 die 的局部** region_bitmap 快照。
     * bit 下标 = **全局** region；flush 落盘后清对应全局位。 */
    uint32_t region_dirty_bitmap[L2P_DIRTY_BITMAP_WORDS];

    /* 本 cache 服务的 (ch,ce)。cache 跨 LUN 共享，故不再有 lun 字段；
     * 每条 line 的 owner LUN 由 region 经 owner_of_region 解析。 */
    uint8_t ch;
    uint8_t ce;

    /* 在飞命令数。load_inflight 供观测；flush_inflight 是**背压闸门**
     * ——每次写回占 1 条命令 + 4 个 buffer（L2P region 页 = 2KB = 4 CW），不设上限会把
     * FC 池吃光 ⇒ host I/O 拿不到命令、全系统 FTL_AGAIN。上限见 L2P_FLUSH_MAX_INFLIGHT。
     * ⚠ 共享 cache 下这是**全 (ch,ce) 共用的 credit 池**（主流资源模型）；
     *   跨 LUN 公平性靠调度/后续实测，不做 per-owner 计数。 */
    uint8_t load_inflight;
    uint8_t flush_inflight;

    /* 心跳基准（见 ftl_job.h）：有在飞 region（load/flush）时每满 1s 打一行计数。
     * ⚠ 本模块没有"单 job"结构（在飞的是若干 cache line）⇒ 只报计数，不报 phase/age。 */
    uint32_t job_log_ms;

    /* 全局 region → owner FTL 实例（按 lba_route_* / completion_table_route 解析）。
     * load/flush/commit 用 owner 的 metadata_dir / alloc / topo，cache 自身不再持有
     * 单一实例的依赖指针。设备级 init 注入一次，生命周期到复位。 */
    l2p_owner_fn_t owner_of_region;
    void          *owner_ctx;
} l2p_cache_t;

/* ── 接口 ── */

/*
 * l2p_cache_init - 初始化共享 cache（设备级，每个 (ch,ce) 只调一次）
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
void l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce, l2p_owner_fn_t owner_of_region, void *owner_ctx);

/*
 * l2p_cache_preload_region - 开机恢复（F4）专用：把一个 region 页同步预载进 cache。
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
bool l2p_cache_preload_region(l2p_cache_t *cache, uint32_t global_region, nand_pba_t pba);

/*
 * l2p_cache_read - 读取**全局 LBA** 对应的 PBA 映射（**三态**）
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
 *   同义"（见 ftl_trim / rr_select_plane），不回填就是未初始化变量。
 *
 * @param cache  cache 上下文
 * @param lba    **全局** LBA（host LBA；数据通路不做局部/全局预转）
 * @param out    出参 PBA（FTL_OK 与 FTL_ERROR 时有效；FTL_AGAIN 时未动）
 * @return FTL_OK / FTL_AGAIN（region 加载中，稍后重试）/ FTL_ERROR（lba 越界；
 *         cache/out 为**前置条件**（判据 2），非法即 ASSERT）
 */
ftl_status_t l2p_cache_read(l2p_cache_t *cache, lba_t lba, nand_pba_t *out);

/*
 * l2p_cache_ready - 确保**全局 LBA** 所在 region 已在 RAM（就绪前置）
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
bool l2p_cache_ready(l2p_cache_t *cache, lba_t lba);

/*
 * l2p_cache_write - 写入**全局 LBA** → PBA 映射（**三态**）
 *
 * 只改 cache line 并置 DIRTY + 全局 region 脏位，不写 NAND（写回由 flush 起）。
 * 行未就绪（LOADING / FLUSHING）⇒ 返回 FTL_AGAIN，**不改任何状态**。
 * 越界 ⇒ FTL_OK（no-op，与旧行为一致：越界调用点都自带范围检查）。
 *
 * @return FTL_OK / FTL_AGAIN（行在飞，稍后重试）
 */
ftl_status_t l2p_cache_write(l2p_cache_t *cache, lba_t lba, nand_pba_t pba);

/* ── 同步逃生口（见文件头；服务函数禁用）── */

/*
 * l2p_cache_read_sync / l2p_cache_write_sync - 自旋等到就绪再读/写
 *
 * @param lba **全局** LBA
 * @return 同 l2p_cache_read / l2p_cache_write；自旋超预算 ⇒ FTL_ERROR。
 */
ftl_status_t l2p_cache_read_sync(l2p_cache_t *cache, lba_t lba, nand_pba_t *out);
ftl_status_t l2p_cache_write_sync(l2p_cache_t *cache, lba_t lba, nand_pba_t pba);

/*
 * l2p_cache_flush_kick - 起一行 DIRTY 的异步写回（提交即返回）
 *
 * ① metadata_dir_append_slot 取 L2P 日志块的下一空槽（块满先取新块）
 * ② ftl_async_write_page 建链 + 把 entries 拷进 FC buffer
 * ③ line->state = FLUSHING；ftl_op_submit(tag = FTL_OP_L2P, ctx = line)
 * 完成时（ftl_l2p_callback）：成功 ⇒ metadata_dir_commit_region + CLEAN + 清 region 脏位；
 * 失败 ⇒ 置回 DIRTY 保留下轮重试。
 *
 * ⚠ **无条件追加**：绝不就地覆盖旧位置（NAND 只能 1→0，二次编程 = 静默损坏）。
 *   分配不到块（owner 的 Free Pool 空）⇒ FTL_AGAIN；owner 的 metadata_dir/alloc
 *   由全局 region 解析得到（不是 cache 自带指针）。**不回退任何固定位置**
 *   ——没有固定位置：未刷盘的 region 在 Directory 里就是 NAND_PBA_INVALID。
 * ⚠ 背压（**FTL_AGAIN**）：在飞写回数达 L2P_FLUSH_MAX_INFLIGHT、命令池空、取不到槽位
 *   ⇒ 行保持 DIRTY，下轮重试。只有地址/建链/提交的**硬失败**才是 FTL_ERROR。
 *   ⚠ 槽位在 append_slot 时就已消耗：若其后建链/提交失败，该槽作废（写指针已推进），
 *     但信息不丢——region 仍指向上一份有效记录。
 *
 * @return FTL_OK 已提交（含"本行非 DIRTY 无需写回"）/ FTL_AGAIN 背压 / FTL_ERROR 硬失败
 */
ftl_status_t l2p_cache_flush_kick(l2p_cache_t *cache, int line_idx);

/*
 * l2p_flush_kick_owner - 后台写回推进（每轮 ftl_service 调一次，只处理本 owner）
 *
 * 从下标 0 起扫，最多启动 max_kick 行 DIRTY（受共享 L2P_FLUSH_MAX_INFLIGHT 闸门）；
 * 跳过不属于 owner_dim 的行——一个 LUN 的后台刷写不应替别的 LUN 做决定。
 *
 * @param owner_dim 本实例的 dim（ftl_topo_dim(ftl->topo)）
 * @param max_kick  本轮最多起几条（IDLE 传 L2P_CACHE_SIZE，NORMAL 传 BG_NORMAL_FLUSH_COUNT，
 *                  BUSY 传 1）
 */
void l2p_flush_kick_owner(l2p_cache_t *cache, uint32_t owner_dim, uint8_t max_kick);

/*
 * l2p_cache_flush_owner_sync - 同步写回**本 owner 的**全部 DIRTY/FLUSHING 行
 *
 * 供 HIL `fl`（"把已持久化映射落盘"是显式的持久化请求，不是后台攒批）。
 * ⚠ 也等 **FLUSHING** 行（后台 kick 已起、尚未完成）——只处理 DIRTY 会让"已提交、
 *   未落盘"的行被跳过 ⇒ 返回时映射其实没落盘，与"落盘"的语义不符。LOADING 行不算
 *   （无脏数据可写）。别的 LUN 的行由它们自己的 CP/flush 处理。
 * @return FTL_OK / FTL_ERROR（任一行写不成功）
 */
ftl_status_t l2p_cache_flush_owner_sync(l2p_cache_t *cache, uint32_t owner_dim);

/*
 * l2p_cache_flush_one_sync - 同步写回**一行**（自旋等完成）
 *
 * 供 checkpoint：Header 落盘前，快照里的每个 region 必须**已持久**（异步写回会让
 * Header 承诺的映射还没落盘 ⇒ 掉电后 Anchor/Header 指向不存在的记录）。
 * 传入的行若正在 FLUSHING 则等它结束；结束为 CLEAN ⇒ OK，回 DIRTY（写失败）⇒ ERROR。
 *
 * @return FTL_OK / FTL_ERROR
 */
ftl_status_t l2p_cache_flush_one_sync(l2p_cache_t *cache, int line_idx);

/*
 * l2p_cache_invalidate_owner - 只失效某个 owner 的行与脏位（恢复/格式化用）
 *
 * 共享 cache 下**不能**再按实例重新 init（会抹掉别的 LUN 刚回放的 DIRTY 行）。
 * 本函数只处理 l2p_region_owned_by(line->region_id, owner_dim) 的 resident 行：
 * entries 填 0xFF、region_id 清零、置 INVALID，并清该 owner 的全局脏位。
 * ⚠ 行处于 FLUSHING/LOADING 是编程错误（恢复期不应有在飞命令）⇒ ASSERT。
 */
void l2p_cache_invalidate_owner(l2p_cache_t *cache, uint32_t owner_dim);

/*
 * l2p_cache_snapshot_owner_bitmap - 全局脏位图 → 本 die 的局部 2048-bit 快照
 *
 * Checkpoint 起点调用（先快照再 flush）。`out` 为 L2P_LOCAL_DIRTY_WORDS 个 word；
 * 映射关系：global_region = local_region * FTL_DIM_TOTAL + owner_dim。
 * DIM=1 时等价于整份位图拷贝。
 */
void l2p_cache_snapshot_owner_bitmap(const l2p_cache_t *cache, uint32_t owner_dim, uint32_t out[L2P_LOCAL_DIRTY_WORDS]);

#endif /* FTL_L2P_CACHE_H */