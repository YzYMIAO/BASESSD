#ifndef FTL_BBT_H
#define FTL_BBT_H

/*
 * BBT（Bad Block Table）— 坏块表持久化
 *
 * 三个阶段：
 *   1. 出厂扫描：遍历所有 Block，读第 0 页判断坏块标记
 *   2. 运行时隔离：PROGRAM/ERASE 失败 → BBT 标记 + allocator 跳过
 *   3. 替换机制：Block Sparing — 从 Spare Pool 取好块补充 Free Pool
 *
 * BBT 存储在固定元数据区的 BBT 块（主 + 备），每份冗余 3 页——
 * 块号、块数、冗余倍数均由 ftl/ftl_config.h 按几何推导，见 §二/§三。
 * bad_map 覆盖全部 FTL_BLOCK_TOTAL 个 Block（NAND 持久化格式，保持不变）。
 * RAM 运行时坏块位统一存于 ftl_block_t.state（Block State Table 单数据源），
 * bbt_save 时从 BST 重建 bad_map。
 */

#include "ftl/allocator.h"
#include "ftl/ftl_config.h" /* BBT_BLOCK / BBT_BLOCKS / BBT_REDUNDANCY / BBT_BITMAP_SIZE */

#include <stddef.h>
#include <stdint.h>

/* ── BBT 尺寸与块位置（BBT_BLOCK / BBT_BLOCK_BACKUP / BBT_BLOCKS /
 *    BBT_REDUNDANCY / BBT_TOTAL_PAGES / BBT_MAX_BLOCKS / BBT_BITMAP_SIZE）
 *    见 ftl/ftl_config.h ── */

/* ── BBT Header 格式 ── */
#define BBT_MAGIC (0x424254ABUL) /* "BBT\xAB" */

typedef struct
{
    uint32_t magic;        /* BBT_MAGIC */
    uint32_t sequence;     /* 单调递增，最新有效 */
    uint32_t total_blocks; /* 总 Block 数 */
    uint32_t bad_count;    /* 坏块数 */
    uint32_t reserved[3];
    uint8_t  bad_map[BBT_BITMAP_SIZE];  /* 4096 bit = 512B */
    uint32_t crc;                       /* 校验和 */
} __attribute__((packed)) bbt_header_t; /* 4+4+4+4+12+512+4 = 544B */

/* ── 接口 ── */

/*
 * bbt_init - BBT 初始化
 *
 * 从 NAND Block 36 加载 BBT。如果无有效 BBT（首次使用），
 * 执行出厂扫描构建初始 BBT。坏块位写入 alloc->blocks[].state。
 *
 * @param alloc  Allocator 上下文（blocks、bad_count 等输出）
 * @return 0 成功，-1 失败
 */
ftl_status_t bbt_init(ftl_allocator_t *alloc);

/*
 * bbt_scan_factory - 出厂坏块扫描
 *
 * 遍历所有 Block 的 Page 0，读数据。
 * 扫描策略（因 FC 层无独立 OOB 接口）：
 *   尝试以 BCH ECC 读取该页。
 *     - 返回 NAND_FAIL/UNC → 出厂坏块
 *     - 读取成功 → 好块
 *
 * 设计文档中的 OOB[0] != 0xFF 检测需要 FC 层支持
 * 单独的 OOB/Spare 区读取，当前 fallback 到 ECC 检测。
 *
 * @param alloc   Allocator 上下文（blocks 输出）
 * @return 检测到的坏块数
 */
int bbt_scan_factory(ftl_allocator_t *alloc);

/*
 * bbt_mark_bad - 运行时标记坏块
 *
 * ① ftl_block_t.state 置位 + bad_reason（BST 单数据源）
 * ② bbt_flush() — 后台写回 NAND
 *
 * @param alloc  Allocator 上下文
 * @param gblock 坏 Block 编号（全局 Block 号）
 * @param reason 坏块原因（FTL_BLOCK_BAD_*，见 ftl/bst.h）
 * @return 0 成功，-1 失败
 */
ftl_status_t bbt_mark_bad(ftl_allocator_t *alloc, uint16_t gblock, uint8_t reason);

/*
 * bbt_is_bad - 查询 Block 是否坏块
 *
 * @param bad_map  BBT bitmap
 * @param block    Block 编号
 * @return 1 坏块，0 好块
 */
static inline int bbt_is_bad(const uint8_t *bad_map, uint16_t block)
{
    if (bad_map == NULL)
    {
        return 0;
    }
    return (bad_map[block >> 3] >> (block & 7U)) & 1U;
}

/*
 * bbt_set_bad - 在 bitmap 中将 Block 标记为坏块
 *
 * @param bad_map  BBT bitmap
 * @param block    Block 编号
 */
static inline void bbt_set_bad(uint8_t *bad_map, uint16_t block)
{
    if (bad_map != NULL)
    {
        bad_map[block >> 3] |= (uint8_t)(1U << (block & 7U));
    }
}

/*
 * bbt_save - 将 BBT 写回 NAND
 *
 * 写 3 页冗余到 Block 36 + 3 页到备用 Block 39。
 * CRC 保护 + sequence 递增。
 *
 * @param alloc  Allocator 上下文
 * @param ch     CH
 * @param ce     CE
 * @return 0 成功，-1 失败
 */
ftl_status_t bbt_save(const ftl_allocator_t *alloc);

/*
 * bbt_flush - 将脏 BBT 写回 NAND（后台调用，两阶段持久化）
 *
 * 当 g_bbt_dirty 置位时，调用 bbt_save 将 BBT bitmap 写入 Block 36 + 39。
 * 不需要每次写失败都立即写 BBT，后台攒批批量写入。
 *
 * @param alloc  Allocator 上下文
 * @return FTL_OK 成功（含无需写入），FTL_ERROR 写入失败
 */
ftl_status_t bbt_flush(ftl_allocator_t *alloc);

/*
 * bbt_load - 从 NAND 加载 BBT
 *
 * 扫描 6 页，选 magic+CRC+sequence 最大的。
 *
 * @param bad_map     输出：bt_bitmap[512]
 * @param bad_count   输出：坏块数
 * @param sequence    输出：最新 sequence
 * @param topo        拓扑信息（BBT 所在芯片路由）
 * @return FTL_OK 成功，FTL_ERROR 无有效 BBT
 */
ftl_status_t bbt_load(uint8_t *bad_map, uint32_t *bad_count, uint32_t *sequence, ftl_topology_t topo);

/*
 * bbt_spare_pool_refill - 从 Spare Pool 补充 Free Pool
 *
 * PROGRAM/ERASE 失败标记坏块后，从 Spare Pool 取一个好块
 * 加入 alloc->free_blocks 保容量水位。
 *
 * @param alloc  Allocator 上下文
 * @return 0 补充成功，-1 Spare Pool 耗尽
 */
ftl_status_t bbt_spare_pool_refill(ftl_allocator_t *alloc);

#endif /* FTL_BBT_H */