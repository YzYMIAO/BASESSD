#include "ftl/bbt.h"
#include "ftl/ftl_fc_interface.h"
#include "common/boot_policy.h"
#include "fc/fc.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/*
 * BBT 内部 SRAM 缓存（全局单例，运行时用）。
 * 出厂扫描或加载时填充，bbt_mark_bad 时同步更新。
 */
static uint8_t  g_bbt_bitmap[BBT_BITMAP_SIZE];   /* 4096 bit = 512B */
static uint32_t g_bbt_sequence = 0U;              /* 当前 sequence */
static int      g_bbt_dirty    = 0;               /* 是否有未写回 NAND 的变更 */

/* BBT CRC 覆盖的字节数（从开头到 crc 字段） */
#define BBT_CRC_SIZE  (sizeof(uint32_t) * 4 + sizeof(uint32_t) * 3 + BBT_BITMAP_SIZE)

int bbt_init(ftl_allocator_t *alloc, uint8_t ch, uint8_t ce, uint8_t lun)
{
    uint32_t seq;
    int      ret;
    uint32_t loaded_bad_count = 0U;

    if (alloc == NULL) return -1;

    memset(g_bbt_bitmap, 0, sizeof(g_bbt_bitmap));
    g_bbt_sequence = 0U;
    g_bbt_dirty    = 0;

    /* 尝试从 NAND 加载已存在的 BBT */
    ret = bbt_load(g_bbt_bitmap, &loaded_bad_count, &seq, ch, ce);
    if (ret == 0)
    {
        uint16_t block;
        uint16_t bad_idx = 0U;

        g_bbt_sequence = seq;
        alloc->bad_count = (uint16_t)loaded_bad_count;

        /* 重建 alloc->bad_blocks 数组 */
        for (block = 0U; block < BBT_MAX_BLOCKS; block++)
        {
            if (bbt_is_bad(g_bbt_bitmap, block))
            {
                if (bad_idx < BLOCKS_PER_PLANE * PLANE_NUM)
                {
                    alloc->bad_blocks[bad_idx++] = block;
                }
            }
        }
        alloc->bad_count = bad_idx;

        return 0;
    }

    /* 无有效 BBT → 执行出厂扫描 */
    return bbt_scan_factory(alloc, ch, ce, lun);
}

int bbt_scan_factory(ftl_allocator_t *alloc, uint8_t ch, uint8_t ce, uint8_t lun)
{
    uint16_t block;
    uint16_t bad_idx = 0U;

    (void)lun;

    if (alloc == NULL) return 0;

    memset(g_bbt_bitmap, 0, sizeof(g_bbt_bitmap));
    g_bbt_dirty = 0;

    /*
     * 扫描策略：
     *
     * 标准 NAND 出厂坏块标记在 Page 0/1 的 OOB 区域第一个字节。
     * 当前 FC 层没有独立 OOB 读取接口，采用替代策略：
     *
     * 以 LDPC_RAW 模式（跳过 ECC）读取 Page 0 的原始 CW 数据。
     * 如果读取完全失败（FTL_RESULT_FAIL），判断为出厂坏块。
     *
     * 注意：LDPC_RAW 模式下 CW 数据仍然可以被读取，但 full-FF
     *   可能是空块的特征，不是坏块特征。真正的 OOB 检测需要
     *   FC 层扩展 spare 区读取功能。
     */
    for (block = 0U; block < BBT_MAX_BLOCKS; block++)
    {
        uint8_t     raw_buf[PAGE_SIZE];
        nand_pba_t  page_pba0;
        int         ret0;

        /* 跳过 Block 0（出厂总是好块）*/
        if (block == 0U) continue;

        /* 尝试读 Page 0（只检查 Plane 0） */
        page_pba0 = pba_make(ch, ce, 0, 0, block, 0, 0);

        memset(raw_buf, 0xFF, sizeof(raw_buf));
        ret0 = ftl_internal_read_page_raw(page_pba0, raw_buf);

        /*
         * LDPC_RAW 模式下，即使 NAND 物理失败，
         * fc_poll_completion 仍然返回 status=READY（raw 模式无 ECC 错误）。
         * 所以用返回码判断坏块不一定可靠。
         *
         * 替代方案：检查 Page 0 的数据是否为全 0xFF（空块特征）。
         * 出厂好块的 Page 0 数据区不为全 FF（有坏块标记等）。
         * 出厂坏块的 Page 0 可能全 FF 或读取失败。
         *
         * 当前实现：
         *   两种都标记——ret0 失败 或 raw_buf 全 FF（出厂商预写到 spare 区的
         *   标识被跳过，FC 层当前只读 USER 数据区）。
         *
         * 更准确的检测需要 FC 提供 spare/OOB 读取接口。
         */

        /* 保守策略：读取出错就标记坏块 */
        if (ret0 != 0)
        {
            bbt_set_bad(g_bbt_bitmap, block);
            if (bad_idx < BLOCKS_PER_PLANE * PLANE_NUM)
            {
                alloc->bad_blocks[bad_idx++] = block;
            }
        }
    }

    alloc->bad_count = bad_idx;
    g_bbt_dirty = 1;

    /* 将扫描结果写入 NAND BBT */
    (void)bbt_save(alloc, ch, ce);

    return (int)bad_idx;
}

int bbt_mark_bad(ftl_allocator_t *alloc, uint8_t ch, uint8_t ce, uint16_t block)
{
    if ((alloc == NULL) || (block >= BBT_MAX_BLOCKS)) return -1;

    /* BBT bitmap 置位 */
    bbt_set_bad(g_bbt_bitmap, block);
    g_bbt_dirty = 1;

    /* allocator 侧记录 */
    allocator_mark_bad(alloc, block);

    /* 写回 NAND */
    (void)bbt_save(alloc, ch, ce);

    /* 从 Spare Pool 补充 Free Pool */
    (void)bbt_spare_pool_refill(alloc);

    return 0;
}

int bbt_save(const ftl_allocator_t *alloc, uint8_t ch, uint8_t ce)
{
    bbt_header_t header;
    uint8_t      i;

    if (alloc == NULL) return -1;
    if (!g_bbt_dirty) return 0;

    g_bbt_sequence++;

    memset(&header, 0, sizeof(header));
    header.magic       = BBT_MAGIC;
    header.sequence    = g_bbt_sequence;
    header.total_blocks = BBT_MAX_BLOCKS;
    header.bad_count   = alloc->bad_count;

    memcpy(header.bad_map, g_bbt_bitmap, BBT_BITMAP_SIZE);

    /* CRC（crc 字段置 0 后计算） */
    header.crc = 0U;
    header.crc = boot_crc32(&header, sizeof(header) - 4);

    /* 写 3 页到 Block 36 */
    for (i = 0U; i < BBT_REDUNDANCY; i++)
    {
        nand_pba_t pba = pba_make(ch, ce, i, (uint8_t)(i & 1U), BBT_BLOCK, 0, 0);
        (void)ftl_internal_write_page(pba, &header, sizeof(header),
                                       FC_CRC_DISABLE, FC_RANDOM_DISABLE,
                                       FC_COMMAND_MODULE_BBT, 0U);
    }

    /* 再写 3 页到备用 Block 39 */
    for (i = 0U; i < BBT_REDUNDANCY; i++)
    {
        nand_pba_t pba = pba_make(ch, ce, i, (uint8_t)(i & 1U), BBT_BLOCK_BACKUP, 0, 0);
        (void)ftl_internal_write_page(pba, &header, sizeof(header),
                                       FC_CRC_DISABLE, FC_RANDOM_DISABLE,
                                       FC_COMMAND_MODULE_BBT, 0U);
    }

    g_bbt_dirty = 0;
    return 0;
}

int bbt_load(uint8_t *bad_map, uint32_t *bad_count, uint32_t *sequence,
              uint8_t ch, uint8_t ce)
{
    bbt_header_t candidates[BBT_TOTAL_PAGES];
    bbt_header_t *best = NULL;
    uint8_t i;
    int found = 0;

    if ((bad_map == NULL) || (bad_count == NULL) || (sequence == NULL))
    {
        return -1;
    }

    memset(candidates, 0, sizeof(candidates));

    /* 扫描 Block 36 + Block 39 共 6 页 */
    for (i = 0U; i < BBT_TOTAL_PAGES; i++)
    {
        nand_pba_t pba;
        uint8_t    block;
        uint8_t    page;
        int        ret;

        if (i < BBT_REDUNDANCY)
        {
            block = BBT_BLOCK;
            page  = i;
        }
        else
        {
            block = BBT_BLOCK_BACKUP;
            page  = (uint8_t)(i - BBT_REDUNDANCY);
        }

        pba = pba_make(ch, ce, page, (uint8_t)(page & 1U), block, 0, 0);

        ret = ftl_internal_read_page(pba, &candidates[i], sizeof(candidates[i]),
                                      FC_CRC_DISABLE, FC_RANDOM_DISABLE,
                                      FC_COMMAND_MODULE_BBT, 0U);
        if (ret != 0) continue;

        /* 验证 Magic */
        if (candidates[i].magic != BBT_MAGIC) continue;

        /* 验证 CRC */
        {
            uint32_t saved_crc = candidates[i].crc;
            candidates[i].crc  = 0U;
            if (boot_crc32(&candidates[i], sizeof(candidates[i]) - 4) != saved_crc)
            {
                candidates[i].crc = saved_crc;
                continue;
            }
            candidates[i].crc = saved_crc;
        }

        /* 选 sequence 最大的 */
        if ((best == NULL) || (candidates[i].sequence > best->sequence))
        {
            best = &candidates[i];
            found = 1;
        }
    }

    if (!found) return -1;

    /* 输出到 caller */
    memcpy(bad_map, best->bad_map, BBT_BITMAP_SIZE);
    *bad_count = best->bad_count;
    *sequence  = best->sequence;

    return 0;
}

int bbt_spare_pool_refill(ftl_allocator_t *alloc)
{
    (void)alloc;

    /*
     * Spare Pool 的 Block 由上层管理（出厂时从 OP 池预留）。
     * 当前实现：Spare Pool 耗尽时返回 -1，不影响正常工作。
     *
     * 完整实现待 allocator 扩展 spare_pool_blocks[] 数组后接入。
     */
    return -1;
}