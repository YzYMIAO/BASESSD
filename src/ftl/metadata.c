#include "ftl/metadata.h"
#include "ftl/l2p_cache.h"
#include "ftl/ftl_fc_interface.h"
#include "common/boot_policy.h"
#include "fc/fc.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* 计算 CRC 覆盖的字节数（从开头到 crc 字段之前） */
#define METADATA_DIR_CRC_OFFSET  (sizeof(uint32_t) * 3 + sizeof(metadata_entry_t) * METADATA_DIR_ENTRIES)
/* = 4 + 4 + 4 + 896 = 908 */

void metadata_dir_init(metadata_dir_cache_t *dir)
{
    uint16_t i;
    uint8_t  plane;
    uint16_t block;
    uint8_t  page;

    if (dir == NULL) return;

    dir->count           = 0U;
    dir->anchor_sequence = 0U;
    dir->dirty           = 0U;
    dir->change_count    = 0U;

    for (i = 0U; (i < L2P_REGIONS_TOTAL) && (i < METADATA_DIR_ENTRIES); i++)
    {
        plane = (uint8_t)(i & 1U);
        block = (uint16_t)(L2P_BASE_BLOCK + (i >> 6));
        page  = (uint8_t)(i & 0x3FU);

        dir->entries[dir->count].type = METADATA_TYPE_L2P_REGION;
        dir->entries[dir->count].id   = i;
        dir->entries[dir->count].paa  = paa_make(page, plane, block, 0, 0);
        dir->count++;
    }

    /* 当前 metadata 分区块指针（初始指向固定区域之后第一个可用块） */
    dir->cur_metadata_block = L2P_BASE_BLOCK + L2P_TABLE_BLOCKS;  /* Block 32 */
    dir->cur_metadata_page  = 0U;
    dir->cur_metadata_plane = 0U;
}

nand_paa_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
                                uint8_t type, uint16_t id)
{
    uint16_t i;

    if (dir == NULL) return NAND_PBA_INVALID;

    for (i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            return dir->entries[i].paa;
        }
    }

    return NAND_PBA_INVALID;
}

void metadata_dir_update(metadata_dir_cache_t *dir,
                          uint8_t type, uint16_t id, nand_paa_t new_paa)
{
    uint16_t i;

    if (dir == NULL) return;

    /* 查找现有 entry（覆盖旧位置） */
    for (i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            dir->entries[i].paa = new_paa;
            dir->dirty          = 1U;
            dir->change_count++;
            return;
        }
    }

    /* 新增 entry（往 entries 数组尾部追加） */
    if (dir->count < METADATA_DIR_ENTRIES)
    {
        dir->entries[dir->count].type = type;
        dir->entries[dir->count].id   = id;
        dir->entries[dir->count].paa  = new_paa;
        dir->count++;
        dir->dirty = 1U;
        dir->change_count++;
    }
    /* 满：不追加，调用者需先 flush_to_anchor 腾空间 */
}

nand_paa_t metadata_dir_alloc_region(metadata_dir_cache_t *dir,
                                      ftl_allocator_t *alloc,
                                      uint16_t region_id)
{
    uint8_t  plane;
    uint16_t block;
    uint8_t  page;
    nand_paa_t paa;

    if ((dir == NULL) || (alloc == NULL)) return NAND_PBA_INVALID;

    plane = (uint8_t)(region_id & 1U);  /* 偶 P0, 奇 P1 */

    /*
     * 在当前 metadata block 中顺序写入。
     * 如果当前 block 已满（page >= 64），从 Free Pool 取新 block。
     */
    if (dir->cur_metadata_page >= PAGES_PER_BLOCK)
    {
        /* 从 Free Pool 取一个好块。
         * 使用 allocator_alloc(FTL_TEMP_COLD) 分配一页，然后从中提取 block 号。
         * 这只占用了页分配中的 Plane 0 位置——后续写 region 时覆盖整页。 */
        nand_pba_t tmp_pba = NAND_PBA_INVALID;

        tmp_pba = allocator_alloc(alloc, FTL_TEMP_COLD);
        if (tmp_pba == NAND_PBA_INVALID)
        {
            return NAND_PBA_INVALID;  /* Free Pool 空 */
        }
        block = (uint16_t)pba_to_block(tmp_pba);
        dir->cur_metadata_block = block;
        dir->cur_metadata_page  = 0U;
        dir->cur_metadata_plane = plane;
    }

    page  = dir->cur_metadata_page;
    block = dir->cur_metadata_block;

    paa = paa_make(page, plane, block, 0, 0);

    /* 更新 Directory */
    metadata_dir_update(dir, METADATA_TYPE_L2P_REGION, region_id, paa);

    /* 推进 page 指针 */
    dir->cur_metadata_page++;

    return paa;
}

int metadata_dir_flush_to_anchor(metadata_dir_cache_t *dir)
{
    metadata_directory_t anchor;
    uint8_t              i;

    if ((dir == NULL) || (!dir->dirty)) return 0;

    /* 构造 Anchor 页 */
    anchor.magic       = ANCHOR_MAGIC;
    anchor.sequence    = ++dir->anchor_sequence;
    anchor.entry_count = dir->count;

    for (i = 0U; i < dir->count; i++)
    {
        anchor.entries[i].type = dir->entries[i].type;
        anchor.entries[i].id   = dir->entries[i].id;
        anchor.entries[i].paa  = dir->entries[i].paa;
    }

    /* 剩余 entries 填 0 */
    for (i = dir->count; i < METADATA_DIR_ENTRIES; i++)
    {
        anchor.entries[i].type = 0U;
        anchor.entries[i].id   = 0U;
        anchor.entries[i].paa  = NAND_PBA_INVALID;
    }

    /* CRC（crc 字段置 0 后计算） */
    anchor.crc = 0U;
    anchor.crc = boot_crc32(&anchor, METADATA_DIR_CRC_OFFSET);

    /*
     * 找到当前最新（sequence 最大）的 Anchor 页位置，在其后写入。
     * 简化：始终写入 ANCHOR_BLOCK0 的 Page 0/1/2（3 份冗余）。
     * 如果 sequence 溢出（几乎不可能），回绕覆盖。
     */
    {
        nand_pba_t pba;
        int ret;

        /* 3 份冗余写入 Anchor Block 0 的 Page 0/1/2 */
        for (i = 0U; i < ANCHOR_REDUNDANCY; i++)
        {
            pba = pba_make(0, 0, i, (uint8_t)(i & 1U), ANCHOR_BLOCK0, 0, 0);
            ret = ftl_internal_write_page(pba, &anchor, sizeof(anchor),
                                           FC_CRC_DISABLE, FC_RANDOM_DISABLE,
                                           FC_COMMAND_MODULE_METADATA, 0U);
            if (ret != 0)
            {
                /* 单份写失败不影响整体（有冗余） */
                continue;
            }
        }

        /* 同样写入 Anchor Block 1（双 Block 冗余） */
        for (i = 0U; i < ANCHOR_REDUNDANCY; i++)
        {
            pba = pba_make(0, 0, i, (uint8_t)(i & 1U), ANCHOR_BLOCK1, 0, 0);
            ret = ftl_internal_write_page(pba, &anchor, sizeof(anchor),
                                           FC_CRC_DISABLE, FC_RANDOM_DISABLE,
                                           FC_COMMAND_MODULE_METADATA, 0U);
            if (ret != 0)
            {
                continue;
            }
        }
    }

    dir->dirty        = 0U;
    dir->change_count = 0U;

    return 0;
}

int metadata_dir_load(metadata_dir_cache_t *dir)
{
    metadata_directory_t candidates[6];
    metadata_directory_t *best = NULL;
    uint8_t i;
    int found = 0;

    if (dir == NULL) return -1;

    memset(candidates, 0, sizeof(candidates));

    /* 扫描 2 Block × 3 页 = 6 个候选 */
    for (i = 0U; i < ANCHOR_PAGES; i++)
    {
        nand_pba_t pba;
        int        ret;

        pba = metadata_dir_anchor_paa(i);
        memset(&candidates[i], 0, sizeof(candidates[i]));

        ret = ftl_internal_read_page(pba, &candidates[i], sizeof(candidates[i]),
                                      FC_CRC_DISABLE, FC_RANDOM_DISABLE,
                                      FC_COMMAND_MODULE_METADATA, 0U);
        if (ret != 0)
        {
            continue;  /* 读失败，跳过 */
        }

        /* 验证 Magic */
        if (candidates[i].magic != ANCHOR_MAGIC)
        {
            continue;
        }

        /* 验证 CRC */
        {
            uint32_t saved_crc = candidates[i].crc;
            candidates[i].crc  = 0U;
            if (boot_crc32(&candidates[i], METADATA_DIR_CRC_OFFSET) != saved_crc)
            {
                candidates[i].crc = saved_crc;  /* 恢复 */
                continue;  /* CRC 错误，跳过 */
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

    if (!found)
    {
        return -1;  /* 无有效 Anchor */
    }

    /* 重建 SRAM cache */
    dir->count           = (uint16_t)(best->entry_count < METADATA_DIR_ENTRIES ?
                                      best->entry_count : METADATA_DIR_ENTRIES);
    dir->anchor_sequence = best->sequence;
    dir->dirty           = 0U;
    dir->change_count    = 0U;

    for (i = 0U; i < dir->count; i++)
    {
        dir->entries[i].type = best->entries[i].type;
        dir->entries[i].id   = best->entries[i].id;
        dir->entries[i].paa  = best->entries[i].paa;
    }

    /* 找到当前 metadata 分区块指针 */
    {
        uint16_t max_block = 0U;
        uint8_t  max_page  = 0U;

        for (i = 0U; i < dir->count; i++)
        {
            if (dir->entries[i].type == METADATA_TYPE_L2P_REGION)
            {
                uint16_t b = (uint16_t)paa_to_block(dir->entries[i].paa);
                uint8_t  p = (uint8_t)paa_to_page(dir->entries[i].paa);

                /* 跳过固定区域 Block 0~31 */
                if (b >= (L2P_BASE_BLOCK + L2P_TABLE_BLOCKS))
                {
                    if ((b > max_block) || ((b == max_block) && (p > max_page)))
                    {
                        max_block = b;
                        max_page  = p;
                    }
                }
            }
        }

        dir->cur_metadata_block = max_block;
        dir->cur_metadata_page  = max_page + 1U;
        dir->cur_metadata_plane = 0U;
    }

    return 0;
}

nand_paa_t metadata_dir_anchor_paa(uint8_t idx)
{
    uint8_t  block;
    uint8_t  page;
    uint8_t  plane;

    /*
     * idx 0/1/2 → Anchor Block 0, Page 0/1/2
     * idx 3/4/5 → Anchor Block 1, Page 0/1/2
     */
    if (idx < 3U)
    {
        block = ANCHOR_BLOCK0;
        page  = idx;
    }
    else
    {
        block = ANCHOR_BLOCK1;
        page  = (uint8_t)(idx - 3U);
    }

    plane = (uint8_t)(page & 1U);

    return paa_make(page, plane, block, 0, 0);
}