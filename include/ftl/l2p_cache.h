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

#include "nand/nand_config.h"   /* nand_pba_t */
#include "ftl/ftl_type.h"       /* lba_t */
#include "ftl/metadata.h"       /* metadata_dir_cache_t */

#include <stdint.h>
#include <stddef.h>

/* ── 宏 ── */

#define L2P_ENTRIES_PER_REGION  (512U)                     /* 每 region 512 条 entry */
#define L2P_REGION_SIZE         (L2P_ENTRIES_PER_REGION * sizeof(uint32_t))  /* 2KB */
#define L2P_REGIONS_TOTAL       (2048U)                    /* 总 region 数 */
#define L2P_CACHE_SIZE          (64U)                      /* cache 行数（可配 32/64/128）*/
#define FTL_PBA_INVALID         (0xFFFFFFFFU)              /* 无效 PBA */

/* L2P Table NAND 固定位置：
 *   Region r → Block = L2P_BASE_BLOCK + (r / 64), Page = r % 64
 *   偶 Region → Plane 0, 奇 Region → Plane 1（双 Plane 交错）
 *   共 2048 regions / 64 pages-per-block = 32 blocks
 */
#define L2P_BASE_BLOCK          (0U)                       /* L2P 起始 Block */
#define L2P_TABLE_BLOCKS        (32U)                      /* 总占用 Block 数 */

/* 固定位置映射：Region r → PBA */
static inline nand_pba_t l2p_region_pba(uint8_t ch, uint8_t ce, uint16_t region_id)
{
    uint8_t  plane = (uint8_t)(region_id & 1U);               /* 偶 P0, 奇 P1 */
    uint16_t block = (uint16_t)(L2P_BASE_BLOCK + (region_id >> 6)); /* /64 */
    uint8_t  page  = (uint8_t)(region_id & 0x3FU);           /* %64 */
    return pba_make(ch, ce, page, plane, block, 0, 0);
}

/* ── Region 分解 ── */

#define L2P_REGION_SHIFT        (9U)                       /* lba / 512 */
#define L2P_REGION_MASK         (L2P_ENTRIES_PER_REGION - 1U)  /* lba % 512 */

static inline uint16_t lba_to_region(lba_t lba)
{
    return (uint16_t)(lba >> L2P_REGION_SHIFT);
}

static inline uint16_t lba_to_entry(lba_t lba)
{
    return (uint16_t)(lba & L2P_REGION_MASK);
}

/* ── Cache 状态 ── */

typedef enum
{
    L2P_CACHE_INVALID = 0,      /* cache 行无效，entries 全 FTL_PBA_INVALID */
    L2P_CACHE_CLEAN,            /* cache 行有效，未修改 */
    L2P_CACHE_DIRTY,            /* cache 行有效，已修改（需写回 NAND） */
    L2P_CACHE_FLUSHING,         /* 正在写回 NAND（不可驱逐，不可读写） */
} l2p_cache_state_t;

/* ── Cache Line ── */

typedef struct
{
    nand_pba_t entries[L2P_ENTRIES_PER_REGION];   /* 512 条 PBA = 2KB */
    uint16_t   region_id;                          /* 0..2047 */
    uint8_t    state;                              /* l2p_cache_state_t */
    uint16_t   access_count;                       /* CLOCK 访问计数 */
} l2p_cache_line_t;

/* ── Cache 上下文 ── */

typedef struct
{
    l2p_cache_line_t lines[L2P_CACHE_SIZE];        /* 64 × (2KB + 4B) ≈ 128 KB */
    uint16_t         clock_hand;                   /* CLOCK 替换指针 */
    uint32_t         hit_count;                    /* 统计：命中次数 */
    uint32_t         miss_count;                   /* 统计：未命中次数 */

    /* Metadata Directory 指针（由 ftl_context_t 提供） */
    metadata_dir_cache_t *md_dir;                  /* 用于 Region 位置查找 */
} l2p_cache_t;

/*
 * l2p_cache_set_md_dir - 设置 Metadata Directory 指针
 *
 * @param cache  cache 上下文
 * @param md_dir Metadata Directory 指针（来自 ftl_context_t）
 */
static inline void l2p_cache_set_md_dir(l2p_cache_t *cache, metadata_dir_cache_t *md_dir)
{
    if (cache != NULL)
    {
        cache->md_dir = md_dir;
    }
}

/* ── 接口 ── */

/*
 * l2p_cache_init - 初始化 cache，所有 line 置 INVALID
 *
 * @param cache  cache 上下文指针（由调用者提供存储空间）
 */
void l2p_cache_init(l2p_cache_t *cache);

/*
 * l2p_cache_read - 读取 LBA 对应的 PBA 映射
 *
 * 内部：
 *   hit  → access_count++，直接返回 entry
 *   miss → CLOCK 选 victim（DIRTY 需先 flush），标记新 region_id
 *          region 数据由后台加载（Step 2 暂不实现，返回 FTL_PBA_INVALID）
 *
 * @param cache  cache 上下文
 * @param lba    逻辑地址
 * @return PBA 映射值，FTL_PBA_INVALID 表示未映射或尚未加载
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
 * Step 2：暂不实现（返回 0 但不写 NAND，仅清除 DIRTY 标记）。
 * 后续接入 Region 写后：
 *   ① line->state = FLUSHING
 *   ② 通过 ftl_cmd_sys_write_page 写 Region 到 NAND
 *   ③ line->state = CLEAN
 *
 * @param cache     cache 上下文
 * @param line_idx  line 索引（0 ~ L2P_CACHE_SIZE-1）
 * @return 0 成功
 */
int l2p_cache_flush_one(l2p_cache_t *cache, int line_idx);

/*
 * l2p_cache_flush_all - 写回所有 DIRTY cache line
 */
int l2p_cache_flush_all(l2p_cache_t *cache);

#endif /* FTL_L2P_CACHE_H */