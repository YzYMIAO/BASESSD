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
 *   entry = nand_pba_t（含 CH/CE 拓扑 + PAA 芯片内地址）
 *   FTL_PBA_INVALID = 0xFFFFFFFF（未映射的 LBA）
 *
 * 替换策略：CLOCK（近似 LRU，O(1) amortized）
 * 写回策略：Write-back（Dirty 标记，延迟写回 NAND）
 */

#include "ftl/allocator.h"    /* ftl_allocator_t */
#include "ftl/ftl_config.h"   /* L2P 容量/块布局宏 + 打包放置公式 */
#include "ftl/ftl_type.h"     /* lba_t */
#include "ftl/metadata.h"     /* metadata_dir_cache_t */
#include "nand/nand_config.h" /* nand_pba_t */

#include <stddef.h>
#include <stdint.h>

/* ── 宏 ──
 * L2P 容量与块布局宏（L2P_ENTRIES_PER_REGION / L2P_REGION_SIZE /
 * L2P_REGIONS_PER_INSTANCE / L2P_BASE_BLOCK / L2P_BLOCKS / L2P_REGION_SHIFT /
 * L2P_REGION_MASK / FTL_DIM_TOTAL）见 ftl/ftl_config.h。*/
#define L2P_CACHE_SIZE  (64U)         /* cache 行数（可配 32/64/128）*/
#define FTL_PBA_INVALID (0xFFFFFFFFU) /* 无效 PBA */

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

/* 固定位置映射：Region r → PBA（打包放置公式见 ftl_config.h §六，两处共用）
 * region_id 是**实例内**编号；各 die 的块号相同（分片模型），故不参与定位。*/
static inline nand_pba_t l2p_region_pba(uint8_t ch, uint8_t ce, uint8_t lun, uint16_t region_id)
{
    uint16_t block;
    uint8_t  page;
    uint8_t  plane;

    ftl_l2p_region_placement(region_id, &block, &page, &plane);
    return pba_from_addr(&(nand_addr_t){.ch = ch, .ce = ce, .lun = lun, .block = block, .plane = plane, .page = page});
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
    L2P_CACHE_INVALID = 0, /* cache 行无效，entries 全 FTL_PBA_INVALID */
    L2P_CACHE_CLEAN,       /* cache 行有效，未修改 */
    L2P_CACHE_DIRTY,       /* cache 行有效，已修改（需写回 NAND） */
    L2P_CACHE_FLUSHING,    /* 正在写回 NAND（不可驱逐，不可读写） */
} l2p_cache_state_t;

/* ── Cache Line ── */

typedef struct
{
    nand_pba_t entries[L2P_ENTRIES_PER_REGION]; /* 512 条 PBA = 2KB */
    uint16_t   region_id;                       /* 0..L2P_REGIONS_PER_INSTANCE-1（本表内编号） */
    uint8_t    state;                           /* l2p_cache_state_t */
    uint16_t   access_count;                    /* CLOCK 访问计数 */
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
 * @param cache  cache 上下文指针（由调用者提供存储空间）
 * @param ch     本 cache 绑定的 CH（固定位置寻址 + 路由用）
 * @param ce     本 cache 绑定的 CE
 * @param lun    本 cache 绑定的 LUN
 */
void l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce, uint8_t lun);

/*
 * l2p_cache_read - 读取 LBA 对应的 PBA 映射
 *
 * 内部：
 *   hit  → access_count++，直接返回 entry
 *   miss → CLOCK 选 victim（DIRTY 需先 flush），标记新 region_id，
 *          region 数据从 NAND L2P Table 加载（sync_read_page：
 *          经 metadata_dir_lookup 查位置，未分配时回退固定位置映射）
 *
 * @param cache  cache 上下文
 * @param lba    逻辑地址
 * @return PBA 映射值，FTL_PBA_INVALID 表示未映射或加载失败
 */
nand_pba_t l2p_cache_read(l2p_cache_t *cache, lba_t lba);

/*
 * l2p_cache_write - 写入 LBA → PBA 映射
 *
 * 只修改 cache line 并标记 DIRTY，不写 NAND。
 * flush 由 l2p_cache_flush_one / flush_all 触发。
 *
 * @param cache  cache 上下文
 * @param lba    逻辑地址
 * @param pba    要写入的 PBA 映射值
 */
void l2p_cache_write(l2p_cache_t *cache, lba_t lba, nand_pba_t pba);

/*
 * l2p_cache_flush_one - 写回指定索引的 DIRTY cache line 到 NAND
 *
 * ① line->state = FLUSHING（防 evict 选中）
 * ② metadata_dir_append_slot 取 L2P 日志块的下一空槽（块满先取新块）
 * ③ sync_write_page 把整页 Region 追加写入该槽
 * ④ metadata_dir_commit_region 登记新位置（旧位置活计数 -1）
 * ⑤ line->state = CLEAN
 *
 * ⚠ **无条件追加**：绝不就地覆盖旧位置（NAND 只能 1→0，二次编程 = 静默损坏）。
 *   分配不到块（Free Pool 空）或 md_dir/alloc 缺失 ⇒ 返回 FTL_ERROR，**不回退固定
 *   位置**——固定层是哨兵（永不写入，读回全 0xFF = 未映射）。
 *
 * @param cache     cache 上下文
 * @param line_idx  line 索引（0 ~ L2P_CACHE_SIZE-1）
 * @return 0 成功
 */
ftl_status_t l2p_cache_flush_one(l2p_cache_t *cache, int line_idx);

/*
 * l2p_cache_flush_all - 写回所有 DIRTY cache line
 */
ftl_status_t l2p_cache_flush_all(l2p_cache_t *cache);

#endif /* FTL_L2P_CACHE_H */