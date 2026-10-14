#ifndef FTL_L2P_CACHE_H
#define FTL_L2P_CACHE_H

/*
 * L2P Cache — DRAM-less SRAM Region Cache
 *
 * 全表存储于 NAND 元数据区（4MB），SRAM 中缓存最近访问的 region。
 * Region 分片：512 条 entry × 4B = 2KB/region，2048 region 覆盖全部 LBA 空间。
 *
 * 地址体系：
 *   lba_t → region_id (lba / 512) + entry_idx (lba % 512)
 *   entry = nand_pba_t（FTL 物理地址：CH/CE/LUN 拓扑 + 块/页/plane/CW）
 *   NAND_PBA_INVALID = 0xFFFFFFFF（未映射的 LBA）
 *
 * 替换策略：CLOCK（近似 LRU，O(1) amortized）
 * 写回策略：Write-back（Dirty 标记，延迟写回 NAND）
 */

#include "ftl/allocator.h"  /* ftl_allocator_t */
#include "ftl/ftl_config.h" /* §一 容量/分解宏 + §三.2 nand_pba_t / pba_* */
#include "ftl/ftl_type.h"   /* lba_t */
#include "ftl/metadata.h"   /* metadata_dir_cache_t + 块布局宏（L2P_BASE_BLOCK / L2P_BLOCKS）+ 打包放置公式 */

#include <stddef.h>
#include <stdint.h>

/* ── 宏 ──
 * L2P 容量与分解宏（L2P_ENTRIES_PER_REGION / L2P_REGION_SIZE /
 * L2P_REGIONS_PER_INSTANCE / L2P_REGION_SHIFT / L2P_REGION_MASK /
 * FTL_DIM_TOTAL）见 ftl/ftl_config.h §一；
 * L2P 块布局宏（L2P_BASE_BLOCK / L2P_BLOCKS）见 ftl/metadata.h「固定元数据区布局」。*/
#define L2P_CACHE_SIZE (64U) /* cache 行数（可配 32/64/128）*/

/* 在飞写回上限（背压闸门，见 l2p_cache_t.flush_inflight）：每次写回占 1 条命令 + 4 buffer，
 * 4 条在飞 = 16 buffer，占 FC buffer 池（128）的 1/8，给 host I/O 留足余量。 */
#define L2P_FLUSH_MAX_INFLIGHT (4U)

/* 同步逃生口的自旋预算（轮）。每轮 = 一次 ftl_service_pump（推 NAND + 抽干完成）。
 * 超预算即判失败返回——兜住"命令永不到达"这类编程错误，不让同步站点死等。 */
#define L2P_SYNC_GUARD_MAX (100000UL)

/* ── Thrashing 检测（P1-4，设计 §6.5）──
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

/* 固定位置映射：Region r → PBA（打包放置公式见 ftl/metadata.h，两处共用）
 * region_id 是**实例内**编号；各 die 的块号相同（分片模型），故不参与定位。*/
static inline nand_pba_t l2p_region_pba(uint8_t ch, uint8_t ce, uint8_t lun, uint16_t region_id)
{
    uint16_t block;
    uint8_t  page;
    uint8_t  plane;

    ftl_l2p_region_placement(region_id, &block, &page, &plane);
    return pba_make(ch, ce, lun, block, plane, page, 0U);
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
static inline uint32_t lba_route_ch(uint64_t lba)
{
    return lba_route_dim(lba) % CH_NUM;
}
static inline uint32_t lba_route_ce(uint64_t lba)
{
    return (lba_route_dim(lba) / CH_NUM) % CE_NUM;
}
static inline uint32_t lba_route_lun(uint64_t lba)
{
    return lba_route_dim(lba) / (CH_NUM * CE_NUM);
}

/*
 * 全局 LBA → 实例内 local LBA（本 (ch,ce,lun) 的表内偏移）。
 *
 * **按 region 粒度反交织**（不是条目粒度）：相邻 region 轮流分给各维度，
 * 同一实例内收到的 region 序号连续 ⇒ 本地序号 = 全局 region 序号 ÷ 维度数，
 * 再乘回每 region 条目数、补上 region 内偏移。
 * FTL_DIM_TOTAL = 1 时退化为恒等映射。
 */
static inline lba_t lba_to_local(uint64_t lba)
{
    uint64_t region = lba >> L2P_REGION_SHIFT;

    return (lba_t)(((region / FTL_DIM_TOTAL) * L2P_ENTRIES_PER_REGION) + (lba & L2P_REGION_MASK));
}

static inline uint16_t lba_to_region(lba_t local_lba)
{
    return (uint16_t)(local_lba >> L2P_REGION_SHIFT);
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

/* ── 异步化（v0.75 阶段 5）──
 *
 * 加载与写回都改为**提交即返回**：未命中 ⇒ 建命令 + ftl_op_submit(tag = FTL_OP_L2P,
 * ctx = 本 cache line) ⇒ 置 LOADING/FLUSHING ⇒ 返回 FTL_AGAIN；完成由
 * g_op_service[FTL_OP_L2P]（ftl_op_l2p，l2p_cache.c）按 line->state 收尾。
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
 *   自旋不是嵌套泵。服务函数（g_op_service 里的任何一个）**禁止**调用，由
 *   ftl_op_in_dispatch 断言兜住。这些站点在阶段 6/8 逐个 job 化后即可删除。
 */

/* ── Cache Line ── */

typedef struct
{
    nand_pba_t entries[L2P_ENTRIES_PER_REGION]; /* 512 条 PBA = 2KB */
    uint16_t   region_id;                       /* 0..L2P_REGIONS_PER_INSTANCE-1（本表内编号） */
    uint8_t    state;                           /* l2p_cache_state_t */
    uint16_t   access_count;                    /* CLOCK 访问计数 */

    /* 「本次加载正是为**当前这次访问**而起」（异步化的观测口径补偿）。
     * 异步后一次 miss 访问会跨两轮：第 1 轮起加载（计一次 miss）、第 2 轮重试命中。
     * 若重试也计一次 hit，则**同一次访问被记成 miss + hit** ⇒ 窗口 miss 率虚降一半
     * （随机读满盘：真值 100% → 观测 50%，跌破 L2P_THRASH_EXIT_PCT=70% ⇒ P1-4
     * 降级永不触发）。故：加载完成时置 1，其后**第一次**命中消费它且不计命中。
     * 与旧同步实现等价（旧实现同一调用内 miss→加载→返回，只记一次 miss）。
     * 代价 1 字节 —— 落在 entries 的 4 字节对齐填充里，结构体尺寸不变。 */
    uint8_t fresh;
} l2p_cache_line_t;

/* ── Cache 上下文 ── */

typedef struct
{
    l2p_cache_line_t lines[L2P_CACHE_SIZE]; /* 64 × (2KB + 4B) ≈ 128 KB */
    uint16_t         clock_hand;            /* CLOCK 替换指针 */
    uint32_t         hit_count;             /* 统计：命中次数（开机累计，观测用） */
    uint32_t         miss_count;            /* 统计：未命中次数（开机累计，观测用） */

    /* ── Thrashing 检测窗口（P1-4；阈值见文件头 L2P_THRASH_*）──
     * 与累计 hit/miss_count 互补：累计量随开机时长渐近，反映不了**当前**行为，
     * 降级判定必须看窗口。窗口满即结算并清零，miss_pct 保留最近一次结算结果。 */
    uint32_t window_access; /* 本窗口累计访问数（hit+miss） */
    uint32_t window_miss;   /* 本窗口累计 miss 数 */
    uint8_t  miss_pct;      /* 最近一次窗口结算的 miss 率（0..100） */
    uint8_t  thrashing;     /* 1 = 处于 thrashing 降级态（滞回保持） */

    /* Region 级脏标记（F4，自上次 checkpoint 以来本 region 有过写）。
     * 与 line 级 state 互补：line DIRTY 只描述 resident 行，evict 兜底
     * flush 后行 CLEAN/失效，但 region 的"本次周期内被改过"仍由本 bitmap
     * 记住。checkpoint_create 以它填 region_bitmap（先快照再 flush）。
     * bit=1：自上次 checkpoint 以来 region_id 有写（flush 落盘后清零）。 */
    uint32_t region_dirty_bitmap[L2P_REGIONS_PER_INSTANCE / 32U]; /* 2048/32 → 64 word = 256B */

    /* 本 cache 绑定的拓扑（ch/ce/lun，决定固定位置寻址与路由） */
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;

    /* 在飞命令数（v0.75 阶段 5）。load_inflight 供观测；flush_inflight 是**背压闸门**
     * ——每次写回占 1 条命令 + 4 个 buffer（L2P region 页 = 2KB = 4 CW），不设上限会把
     * FC 池吃光 ⇒ host I/O 拿不到命令、全系统 FTL_AGAIN。上限见 L2P_FLUSH_MAX_INFLIGHT。 */
    uint8_t load_inflight;
    uint8_t flush_inflight;

    /* Metadata Directory 指针（由 ftl_context_t 提供） */
    metadata_dir_cache_t *md_dir; /* 用于 Region 位置查找 */

    /* Allocator 指针（由 ftl_context_t 提供，用于 Region 动态分配） */
    ftl_allocator_t *alloc;
} l2p_cache_t;

/*
 * l2p_cache_set_md_dir - 设置 Metadata Directory 指针和 Allocator 指针
 *
 * @param cache  cache 上下文
 * @param md_dir Metadata Directory 指针（来自 ftl_context_t）
 * @param alloc  Allocator 指针（来自 ftl_context_t，用于 Region 动态分配）
 */
static inline void l2p_cache_set_md_dir(l2p_cache_t          *cache,
                                        metadata_dir_cache_t *md_dir,
                                        ftl_allocator_t      *alloc)
{
    if (cache != NULL)
    {
        cache->md_dir = md_dir;
        cache->alloc  = alloc;
    }
}

/*
 * l2p_cache_is_thrashing - 查询当前是否处于 thrashing 降级态（P1-4）
 *
 * 由 l2p_cache 内部按窗口 miss 率 + 滞回维护，调用点（ftl_service_normal）据此
 * 决定是否停掉主动后台 flush。纯读，不改状态。
 */
static inline uint8_t l2p_cache_is_thrashing(const l2p_cache_t *cache)
{
    return (cache != NULL) ? cache->thrashing : 0U;
}

/* ── 接口 ── */

/*
 * l2p_cache_init - 初始化 cache，所有 line 置 INVALID
 *
 * ⚠ **不碰 md_dir / alloc**：这两个是宿主注入的依赖（l2p_cache_set_md_dir），
 *   不是 cache 状态。本函数在三条开机恢复路径里都会调用，清它们会把 ftl_init
 *   绑好的 Directory 永久解绑（后果见 l2p_cache.c 内注释）。
 *
 * @param cache  cache 上下文指针（由调用者提供存储空间）
 * @param ch     本 cache 绑定的 CH（固定位置寻址 + 路由用）
 * @param ce     本 cache 绑定的 CE
 * @param lun    本 cache 绑定的 LUN
 */
void l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce, uint8_t lun);

/*
 * l2p_cache_read - 读取 LBA 对应的 PBA 映射（**三态**）
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
 * @param lba    逻辑地址
 * @param out    出参 PBA（FTL_OK 与 FTL_ERROR 时有效；FTL_AGAIN 时未动）
 * @return FTL_OK / FTL_AGAIN（region 加载中，稍后重试）/ FTL_ERROR（参数非法）
 */
ftl_status_t l2p_cache_read(l2p_cache_t *cache, lba_t lba, nand_pba_t *out);

/*
 * l2p_cache_ready - 确保 lba 所在 region 已在 RAM（就绪前置）
 *
 * 命中（CLEAN / DIRTY）⇒ 1；未命中 ⇒ 起异步加载并返回 0；LOADING / FLUSHING ⇒ 0。
 * 供"必须先就绪、再一次性应用多条改动"的调用者用（写确认 / 搬移收尾）：
 * 先对全部 LBA 过一遍本函数，全 1 时后续的 read/write 必不可能再返 FTL_AGAIN
 * ⇒ 一批改动**全有或全无**，重试不会重复记账。
 *
 * ⚠ **就绪探针，不是一次访问**：命中不记账（记账交给随后的 read/write）、不消费
 *   fresh、不动 access_count；未命中记一次 miss（因为加载是它起的）。如此
 *   "一次 host 写 = 1 miss + 1 hit"与旧同步实现逐字一致，thrashing 窗口口径不变。
 *
 * @return 1 就绪；0 未就绪（已在加载中，稍后重试）
 */
int l2p_cache_ready(l2p_cache_t *cache, lba_t lba);

/*
 * l2p_cache_write - 写入 LBA → PBA 映射（**三态**）
 *
 * 只改 cache line 并置 DIRTY + region 脏位，不写 NAND（写回由 flush 起）。
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
 * 完成时（ftl_op_l2p）：成功 ⇒ metadata_dir_commit_region + CLEAN + 清 region 脏位；
 * 失败 ⇒ 置回 DIRTY 保留下轮重试。
 *
 * ⚠ **无条件追加**：绝不就地覆盖旧位置（NAND 只能 1→0，二次编程 = 静默损坏）。
 *   分配不到块（Free Pool 空）或 md_dir/alloc 缺失 ⇒ FTL_ERROR，**不回退固定位置**
 *   ——固定层是哨兵（永不写入，读回全 0xFF = 未映射）。
 * ⚠ 背压：命令池空 / 在飞写回数达 L2P_FLUSH_MAX_INFLIGHT ⇒ FTL_AGAIN（行保持 DIRTY）。
 *   ⚠ 若已消耗槽位后提交失败，该槽位作废（append_slot 已推进写指针）——与旧实现的
 *     写失败分支同级，信息不丢（region 仍指向上一份有效记录）。
 *
 * @return FTL_OK 已提交（含"本行非 DIRTY 无需写回"）/ FTL_AGAIN 背压 / FTL_ERROR 硬失败
 */
ftl_status_t l2p_cache_flush_kick(l2p_cache_t *cache, int line_idx);

/*
 * l2p_flush_kick - 后台写回推进（每轮 ftl_service 调一次）
 *
 * 从下标 0 起扫，最多启动 max_kick 行 DIRTY（受 L2P_FLUSH_MAX_INFLIGHT 闸门）。
 * 取代旧的 l2p_cache_flush_all / 逐行同步 flush 循环。
 *
 * @param max_kick  本轮最多起几条（IDLE 传 L2P_CACHE_SIZE，NORMAL 传 BG_NORMAL_FLUSH_COUNT，
 *                  BUSY 传 1）
 */
void l2p_flush_kick(l2p_cache_t *cache, uint8_t max_kick);

/*
 * l2p_cache_flush_all_sync - 同步写回**全部** DIRTY 行（自旋等完成）
 *
 * 供 HIL `fl`（"把已持久化映射落盘"是显式的持久化请求，不是后台攒批）。
 * ⚠ 也等 **FLUSHING** 行（后台 kick 已起、尚未完成）——只处理 DIRTY 会让"已提交、
 *   未落盘"的行被跳过 ⇒ 返回时映射其实没落盘，与"落盘"的语义不符。LOADING 行不算
 *   （无脏数据可写）。
 * @return FTL_OK / FTL_ERROR（任一行写不成功）
 */
ftl_status_t l2p_cache_flush_all_sync(l2p_cache_t *cache);

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

#endif /* FTL_L2P_CACHE_H */