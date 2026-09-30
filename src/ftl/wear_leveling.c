#include "ftl/wear_leveling.h"

#include "fc/fc.h"
#include "ftl/ftl.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/gc.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 内部同步写页（原 ftl_internal_write_page 内联化，随机地址 → addr 直填）── */
static ftl_status_t sync_write_page(nand_addr_t addr, const void *data, uint32_t length,
                                    uint8_t crc, uint8_t random, uint8_t module, lba_t lba)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.cw          = 3U;
    cmd->config.crc         = (DWORD)crc;
    cmd->config.random      = (DWORD)random;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = (DWORD)lba;

    if (addr_to_cmd(cmd, &addr) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U))
    {
        (void)ftl_cmd_write_data(cmd, data, length);
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (completion.status & FC_CW_STATUS_NAND_FAIL)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

int wl_should_trigger(const struct ftl_context *ftl)
{
    uint32_t max_ec;
    uint32_t min_ec;
    uint32_t avg_ec;
    uint32_t low_zone;
    uint32_t low_count;

    if (ftl == NULL)
    {
        return FTL_OK;
    }

    /* WL 最小间隔检查 */
    if (ftl->total_writes < WL_MIN_INTERVAL)
    {
        return FTL_OK;
    }

    max_ec = wl_max_erase(ftl);
    min_ec = wl_min_erase(ftl);
    avg_ec = wl_avg_erase(ftl);

    /* 条件 1：最大/最小差值 */
    if ((max_ec - min_ec) >= WL_DELTA_THRESHOLD)
    {
        return 1;
    }

    /* 条件 2：最大/平均比值 */
    if ((avg_ec > 0U) && (max_ec >= avg_ec * WL_MAX_AVG_RATIO))
    {
        return 1;
    }

    /* 条件 3：低擦除区 Block 占比 */
    low_zone  = min_ec + WL_LOW_ZONE_WIDTH;
    low_count = wl_count_blocks_below(ftl, low_zone);
    if (low_count > (uint32_t)ftl->alloc.total_blocks * WL_LOW_BLOCK_PCT / 100U)
    {
        return 1;
    }

    return FTL_OK;
}

int wl_select_victim(const struct ftl_context *ftl, wl_victim_t *victim)
{
    uint16_t    i;
    uint32_t    min_ec;
    wl_victim_t best;
    uint16_t    total_pages_per_sb;
    uint16_t    min_valid_pages;

    if ((ftl == NULL) || (victim == NULL))
    {
        return FTL_OK;
    }

    memset(&best, 0, sizeof(best));
    min_ec = wl_min_erase(ftl);

    total_pages_per_sb = (uint16_t)(PLANE_NUM * PAGES_PER_BLOCK);
    min_valid_pages    = (uint16_t)(total_pages_per_sb * WL_MIN_VALID_RATIO / 100U);

    for (i = 0U; i < ftl->alloc.closed_sb_count; i++)
    {
        const super_block_t *sb = &ftl->alloc.closed_sb_list[i];
        uint32_t             sb_ec;

        /* 跳过仍为 Open 的 SB */
        if (sb->valid != 0U)
        {
            continue;
        }

        /* 计算该 Super Block 的平均擦除次数（全局 Block 号索引 BST） */
        {
            uint32_t ec_sum = 0U;
            uint8_t  p;

            for (p = 0U; p < PLANE_NUM; p++)
            {
                ec_sum += ftl->alloc.blocks[ftl_gblock(p, sb->plane_blocks[p])].erase_count;
            }
            sb_ec = ec_sum / PLANE_NUM;
        }

        /* 不在低擦除区 → 跳过（擦除够多了） */
        if (sb_ec > (min_ec + WL_LOW_ZONE_WIDTH))
        {
            continue;
        }

        /*
         * 有效页 = 双 Plane 各物理块 VPC 之和（真实计数）。
         * 有效页越多（越冷）越优先。
         */
        {
            uint16_t valid = 0U;
            uint8_t  p;

            for (p = 0U; p < PLANE_NUM; p++)
            {
                valid = (uint16_t)(valid + ftl->alloc.blocks[ftl_gblock(p, sb->plane_blocks[p])].valid_count);
            }

            if (valid < min_valid_pages)
            {
                continue;
            }

            if (valid > best.valid_pages)
            {
                best.found           = 1U;
                best.avg_erase       = sb_ec;
                best.valid_pages     = valid;
                best.plane_blocks[0] = sb->plane_blocks[0];
                best.plane_blocks[1] = sb->plane_blocks[1];
            }
        }
    }

    if (!best.found)
    {
        return FTL_OK;
    }

    *victim = best;
    return 1;
}

int wl_relocate(struct ftl_context *ftl, const wl_victim_t *victim)
{
    uint8_t  plane;
    uint32_t migrated = 0U;

    if ((ftl == NULL) || (victim == NULL) || (!victim->found))
    {
        return FTL_OK;
    }

    /* Step 1: 遍历双 Plane 有效页位图，只搬置位页（免 NAND 读无效页） */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint16_t    gblock_src = ftl_gblock(plane, victim->plane_blocks[plane]);
        uint64_t    valid_map  = ftl->alloc.blocks[gblock_src].valid_map[plane];

        while (valid_map != 0ULL)
        {
            uint8_t page = (uint8_t)__builtin_ctzll(valid_map);
            valid_map &= (valid_map - 1ULL); /* 清最低置位 bit */

            {
                uint8_t    buf[FTL_READ_USER_SIZE];
                lba_t      lba;
                nand_pba_t pba;
                nand_pba_t old_pba;
                nand_pba_t new_pba;
                int        ret;

                pba = pba_make(ftl->topo.ch, ftl->topo.ce, page, plane, victim->plane_blocks[plane], 0, 0);

                /* 读页 + 从 META 提取 LBA（复用 GC 辅助函数） */
                memset(buf, 0, sizeof(buf));
                if (gc_read_page_with_lba(pba, buf, &lba) != 0)
                {
                    continue; /* 读失败，跳过 */
                }

                /* 查 L2P：当前映射是否仍指向本 PBA */
                old_pba = l2p_cache_read(&ftl->cache, lba);
                if (old_pba != pba)
                {
                    /* 已被覆盖，无效页 → 清 bitmap 保持同步 */
                    ftl_block_clr_valid(&ftl->alloc.blocks[gblock_src], plane, page);
                    continue;
                }

                /* 分配 Cold Super Block 新页 */
                new_pba = allocator_alloc(&ftl->alloc, FTL_TEMP_COLD);
                if (new_pba == NAND_PBA_INVALID)
                {
                    break;
                }

                /* 写入新位置 */
                {
                    nand_addr_t waddr = addr_from_pba(new_pba);
                    ret = sync_write_page(waddr, buf, FTL_READ_USER_SIZE,
                                          FC_CRC_ENABLE, FC_RANDOM_ENABLE,
                                          FC_COMMAND_MODULE_GC, lba);
                }
                if (ret != 0)
                {
                    continue;
                }

                /* 更新 L2P Cache */
                l2p_cache_write(&ftl->cache, lba, new_pba);

                /* Journal 记录 */
                journal_append(&ftl->journal, lba, old_pba, new_pba);

                /* VPC/bitmap 维护：源块清位，新块置位（单点更新防漂移） */
                ftl_block_clr_valid(&ftl->alloc.blocks[gblock_src], plane, page);
                ftl_block_set_valid(&ftl->alloc.blocks[ftl_pba_gblock(new_pba)],
                                    (uint8_t)pba_to_plane(new_pba),
                                    (uint8_t)pba_to_page(new_pba));

                migrated++;
            }
        }
    }

    /* Step 2: 构造临时 Super Block，复用 gc_erase_and_free 擦除+归还 */
    {
        super_block_t tmp_sb;

        tmp_sb.plane_blocks[0] = victim->plane_blocks[0];
        tmp_sb.plane_blocks[1] = victim->plane_blocks[1];
        tmp_sb.valid = 0U; /* gc_erase_and_free 要求 valid==0 */

        gc_erase_and_free(&ftl->alloc, &tmp_sb, ftl->total_writes);
    }

    return (int)migrated;
}

int wl_execute(struct ftl_context *ftl)
{
    wl_victim_t victim;

    if (ftl == NULL)
    {
        return FTL_OK;
    }

    if (!wl_should_trigger(ftl))
    {
        return FTL_OK;
    }
    if (!wl_select_victim(ftl, &victim))
    {
        return FTL_OK;
    }

    return wl_relocate(ftl, &victim);
}

uint32_t wl_max_erase(const struct ftl_context *ftl)
{
    uint16_t i;
    uint32_t max_ec = 0U;

    if ((ftl == NULL) || (ftl->alloc.blocks == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count > max_ec)
        {
            max_ec = b->erase_count;
        }
    }

    return max_ec;
}

uint32_t wl_min_erase(const struct ftl_context *ftl)
{
    uint16_t i;
    uint32_t min_ec = UINT32_MAX;

    if ((ftl == NULL) || (ftl->alloc.blocks == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count < min_ec)
        {
            min_ec = b->erase_count;
        }
    }

    return (min_ec == UINT32_MAX) ? 0U : min_ec;
}

uint32_t wl_avg_erase(const struct ftl_context *ftl)
{
    uint16_t i;
    uint32_t sum   = 0U;
    uint16_t count = 0U;

    if ((ftl == NULL) || (ftl->alloc.blocks == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        sum += b->erase_count;
        count++;
    }

    if (count == 0U)
    {
        return 0U;
    }
    return sum / count;
}

uint32_t wl_count_blocks_below(const struct ftl_context *ftl, uint32_t threshold)
{
    uint16_t i;
    uint32_t count = 0U;

    if ((ftl == NULL) || (ftl->alloc.blocks == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count < threshold)
        {
            count++;
        }
    }

    return count;
}