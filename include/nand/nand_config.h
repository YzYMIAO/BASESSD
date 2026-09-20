#ifndef NAND_CONFIG_H
#define NAND_CONFIG_H

#include <stdint.h>

#define PAGE_SIZE (2112)
#define PAGE_MAIN_SIZE  (2048)
#define PAGE_SPARE_SIZE (64)
#define PAGES_PER_BLOCK (64)
#define BLOCKS_PER_PLANE (2048)
#define PLANE_NUM (2)
#define DIE_NUM   (1)
#define LUN_NUM   (1)
#define CH_NUM    (1)
#define CE_NUM    (1)
#define CW_NUM    (4U)

#define CW_WIDTH         (2U)  /* 4个codeword,各个cw的起始位置在4等分处。 */
#define PAGE_ADDR_WIDTH  (6U)  /* PA0~PA5，每 Block 64 页。 */
#define PLANE_ADDR_WIDTH (1U)  /* BA6，Plane 选择。 */
#define BLOCK_ADDR_WIDTH (11U) /* BA7~BA17，每 Plane BLOCKS_PER_PLANE 个 Block。 */
#define DIE_ADDR_WIDTH   (0U)
#define LUN_ADDR_WIDTH   (0U)

/*
 * 软件 PAA 打包布局（低位优先）：
 * [CW][PAGE][PLANE][BLOCK][LUN]
 *
 * 注意：这是 FC/FTL 的逻辑地址编码，不是 NAND 五个地址周期的原始排列。
 * NAND 行地址由 paa_to_nand_row() 按 MT29F4G08 的 BA/PA 规则重新编码。
 */
#define CW_WIDTH_SHIFT  (0U)
#define PAA_PAGE_SHIFT  (CW_WIDTH_SHIFT + CW_WIDTH)
#define PAA_PLANE_SHIFT (PAA_PAGE_SHIFT + PAGE_ADDR_WIDTH)
#define PAA_BLOCK_SHIFT (PAA_PLANE_SHIFT + PLANE_ADDR_WIDTH)
#define PAA_LUN_SHIFT   (PAA_BLOCK_SHIFT + BLOCK_ADDR_WIDTH)

#define NAND_FIELD_MASK(width) ((width) == 0U ? 0UL : ((1UL << (width)) - 1UL))
#define PAA_PAGE_MASK          NAND_FIELD_MASK(PAGE_ADDR_WIDTH)
#define PAA_PLANE_MASK         NAND_FIELD_MASK(PLANE_ADDR_WIDTH)
#define PAA_BLOCK_MASK         NAND_FIELD_MASK(BLOCK_ADDR_WIDTH)
#define PAA_LUN_MASK           NAND_FIELD_MASK(LUN_ADDR_WIDTH)

typedef uint32_t nand_paa_t;

static inline uint32_t paa_to_page(nand_paa_t paa)
{
    return (paa >> PAA_PAGE_SHIFT) & PAA_PAGE_MASK;
}

static inline uint32_t paa_to_plane(nand_paa_t paa)
{
    return (paa >> PAA_PLANE_SHIFT) & PAA_PLANE_MASK;
}

static inline uint32_t paa_to_block(nand_paa_t paa)
{
    return (paa >> PAA_BLOCK_SHIFT) & PAA_BLOCK_MASK;
}

static inline uint32_t paa_to_lun(nand_paa_t paa)
{
    return (paa >> PAA_LUN_SHIFT) & PAA_LUN_MASK;
}

static inline nand_paa_t paa_make(uint32_t page, uint32_t plane, uint32_t block, uint32_t lun, uint32_t cw)
{
    return ((cw & NAND_FIELD_MASK(CW_WIDTH)) << CW_WIDTH_SHIFT) |
           ((page & PAA_PAGE_MASK) << PAA_PAGE_SHIFT) |
           ((plane & PAA_PLANE_MASK) << PAA_PLANE_SHIFT) |
           ((block & PAA_BLOCK_MASK) << PAA_BLOCK_SHIFT) |
           ((lun & PAA_LUN_MASK) << PAA_LUN_SHIFT);
}

static inline uint8_t paa_is_valid(uint32_t page, uint32_t plane, uint32_t block, uint32_t lun, uint32_t cw)
{
    return (uint8_t)((page < PAGES_PER_BLOCK) && (plane < PLANE_NUM) && (block < BLOCKS_PER_PLANE) &&
                     (lun < LUN_NUM) && (cw < CW_NUM));
}

/* MT29F4G08 行地址：PA[5:0] -> bit[5:0]，Plane/BA6 -> bit[6]，BA[17:7] -> bit[17:7]。 */
static inline uint32_t paa_to_nand_row(nand_paa_t paa)
{
    return paa_to_page(paa) |
           (paa_to_plane(paa) << PAGE_ADDR_WIDTH) |
           (paa_to_block(paa) << (PAGE_ADDR_WIDTH + PLANE_ADDR_WIDTH));
}

static inline uint16_t paa_to_cw(nand_paa_t paa)
{
    return (uint16_t)((paa >> CW_WIDTH_SHIFT) & NAND_FIELD_MASK(CW_WIDTH));
}

#endif
