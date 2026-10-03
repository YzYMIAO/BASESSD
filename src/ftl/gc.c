#include "ftl/gc.h"

#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/journal.h"
#include "ftl/read_recovery.h" /* rr_record_ecc（B4：GC 搬移读记 ECC/read disturb） */
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 内部同步写页（原 ftl_internal_write_page 内联化，随机地址 → addr 直填）── */
static ftl_status_t sync_write_page(nand_addr_t addr, const void *data, uint32_t length, uint8_t crc, uint8_t random, uint8_t module, lba_t lba)
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

/* ── 内部同步擦除（原 ftl_internal_erase_block 内联化，20ms 超时）── */
static ftl_status_t sync_erase_block(nand_addr_t addr)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_ERASE_BLOCK;
    cmd->config.cw          = 0U;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_METADATA;
    cmd->meta               = 0U;

    if (addr_to_cmd(cmd, &addr) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 20000U);
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

/*
 * gc_read_page_with_lba - 读取一页 NAND，返回用户数据 + 从 META 提取的 LBA
 *
 * 内部管理 FC 命令生命周期：
 *   alloc → config → submit → sync_wait → read_data → get_meta → release
 *
 * B4：本函数是 GC/WL 搬移读（gc_migrate_sb / wl_relocate）与掉电重建
 * （ftl_rebuild_closed_sb_list 反查）的唯一读入口。此前不记录 ECC/read_count，
 * 慢死预测（rr_record_ecc / read_count++）只挂 host 读 completion，导致
 * GC/WL 搬移读成为 read disturb 盲区。现加 ecc_out / read_count_out 输出。
 *
 * @param pba            物理地址
 * @param buffer         输出：用户数据 buffer（2048 字节，可为 NULL）
 * @param lba_out        输出：从 META 提取的 LBA（可为 NULL）
 * @param ecc_out        输出：本次读 4 CW 最大 ECC 纠正 bit 数（0..4，可为 NULL）
 * @param read_count_out 输出：1=本次读成功（调用方 ++ 该块 read_count，可为 NULL）
 * @return 0 成功, -1 失败
 */
ftl_status_t gc_read_page_with_lba(nand_pba_t pba, uint8_t *buffer, lba_t *lba_out, uint8_t *ecc_out, uint8_t *read_count_out)
{
    fc_command_t *cmd;
    uint32_t      meta_cw0;
    uint8_t       ecc_max = 0U;
    int           ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_GC;
    cmd->meta               = 0U;

    if (ftl_cmd_setup_ppa(cmd, pba) != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    {
        fc_command_status_t completion;
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

        /* B4：取 4 CW 最大 ECC 纠正 bit 数（与 host 读 completion 同口径，
         * ftl.c:107-119 的 rr_record_ecc 聚合逻辑一致） */
        {
            uint8_t k;
            for (k = 0U; k < 4U; k++)
            {
                uint8_t b = (k == 0U)   ? (uint8_t)completion.ecc_bit0
                            : (k == 1U) ? (uint8_t)completion.ecc_bit1
                            : (k == 2U) ? (uint8_t)completion.ecc_bit2
                                        : (uint8_t)completion.ecc_bit3;
                if (b > ecc_max)
                {
                    ecc_max = b;
                }
            }
        }
    }

    /* 读取用户数据 */
    if (buffer != NULL)
    {
        (void)ftl_cmd_read_data(cmd, buffer, 2048U);
    }

    /* 从 META 提取 LBA
     * CW0 的 META = lba_to_cw_meta(lba, 0) = (lba & ~3) | 0
     * 所以 lba = meta_cw0 & ~3 */
    {
        uint8_t *meta_ptr = fc_buffer_get_meta(cmd, 0U);
        if (meta_ptr != NULL)
        {
            /* meta_ptr 指向 CW buffer，CW 基址 4 字节对齐，memcpy 避免 alignment 警告 */
            memcpy(&meta_cw0, meta_ptr, sizeof(meta_cw0));
        }
        else
        {
            meta_cw0 = 0U;
        }
    }

    (void)fc_command_release(cmd);

    if (lba_out != NULL)
    {
        /* META[0:1] = cw_index, 高位 = LBA 基址 */
        *lba_out = (lba_t)(meta_cw0 & ~3UL);
    }
    if (ecc_out != NULL)
    {
        *ecc_out = ecc_max;
    }
    if (read_count_out != NULL)
    {
        *read_count_out = 1U; /* 读成功，调用方 ++ 该块 read_count */
    }

    return FTL_OK;
}

void gc_erase_and_free(ftl_allocator_t *alloc, super_block_t *victim, uint32_t now)
{
    uint8_t  plane;
    uint16_t closed_idx;

    if ((alloc == NULL) || (victim == NULL) || (victim->valid != 0U))
    {
        return;
    }

    closed_idx = 0U;
    if (alloc->closed_sb_count > 0U)
    {
        /* 定位 victim 在 closed_sb_list 中的索引（通过 plane_blocks[0] 比对） */
        for (closed_idx = 0U; closed_idx < alloc->closed_sb_count; closed_idx++)
        {
            if (alloc->closed_sb_list[closed_idx].plane_blocks[0] == victim->plane_blocks[0])
            {
                break;
            }
        }
        if (closed_idx >= alloc->closed_sb_count)
        {
            /* 未找到：可能是 WL 临时 SB，无需移除列表 */
            closed_idx = UINT16_MAX;
        }
    }
    else
    {
        closed_idx = UINT16_MAX;
    }

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint16_t    gblock;
        nand_addr_t block_addr;
        int         ret;

        gblock     = ftl_gblock(plane, victim->plane_blocks[plane]);
        block_addr = super_block_page_addr(victim, plane, 0U);

        ret = sync_erase_block(block_addr);
        if (ret == 0)
        {
            /* 擦除成功：擦除计数 +1，记录最近擦除时间戳（单调写入量），bitmap/VPC 归零，归还 Free Pool */
            alloc->blocks[gblock].erase_count++;
            alloc->blocks[gblock].last_pe_ts = now;
            ftl_block_clear_valid(&alloc->blocks[gblock]);
            allocator_free_block(alloc, gblock);
        }
        else
        {
            /* 擦除失败：标记坏块（BBT 单数据源，后台持久化） */
            (void)bbt_mark_bad(alloc, gblock, (uint8_t)FTL_BLOCK_BAD_ERASE);
        }
    }

    /* 从 Closed 列表移除已回收条目（swap-remove）—— 修复 closed_sb_count 只增不减导致的
     * 列表爆炸 + 已回收 SB 被再次选为 victim 的无限 GC 空转 */
    if (closed_idx != UINT16_MAX)
    {
        allocator_remove_closed_sb(alloc, closed_idx);
    }
}

int gc_select_victim(ftl_allocator_t *alloc, super_block_t **victim)
{
    uint16_t i;
    uint16_t best_valid = PLANE_NUM * PAGES_PER_BLOCK + 1U;
    int      found      = 0;

    if ((alloc == NULL) || (victim == NULL))
    {
        return FTL_OK;
    }

    for (i = 0U; i < alloc->closed_sb_count; i++)
    {
        super_block_t *sb = &alloc->closed_sb_list[i];
        uint16_t       valid;
        uint8_t        p;

        if (sb->valid != 0U)
        {
            continue;
        }

        /* 有效页数 = 双 Plane 各物理块 VPC 之和（真实计数，替代 next_page 近似） */
        valid = 0U;
        for (p = 0U; p < PLANE_NUM; p++)
        {
            valid = (uint16_t)(valid + alloc->blocks[ftl_gblock(p, sb->plane_blocks[p])].valid_count);
        }

        if (valid < best_valid)
        {
            best_valid = valid;
            *victim    = sb;
            found      = 1;
        }
    }

    return found;
}

int gc_migrate_sb(ftl_allocator_t *alloc, l2p_cache_t *cache, l2p_journal_t *journal, super_block_t *victim)
{
    uint8_t  plane;
    uint32_t migrated = 0U;

    if ((alloc == NULL) || (cache == NULL) || (victim == NULL))
    {
        return FTL_OK;
    }

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint16_t gblock_src = ftl_gblock(plane, victim->plane_blocks[plane]);
        uint64_t valid_map  = alloc->blocks[gblock_src].valid_map[plane];

        /*
         * 直接扫有效页位图：只搬置位页，跳过无效页（空洞/已覆盖）。
         * 相比逐页读 NAND META + L2P 反查，省去无效页的 NAND 读。
         */
        while (valid_map != 0ULL)
        {
            uint8_t page = (uint8_t)__builtin_ctzll(valid_map);
            valid_map &= (valid_map - 1ULL); /* 清最低置位 bit */

            {
                uint8_t    buf[2048];
                lba_t      lba;
                nand_pba_t pba;
                nand_pba_t old_pba;
                nand_pba_t new_pba;
                int        ret;

                pba = super_block_page_pba(alloc->ch, alloc->ce, victim, plane, page);

                /* 读页 + 获取 LBA + 验证有效性（bitmap 已保证置位，此处只取 LBA） */
                memset(buf, 0, sizeof(buf));
                {
                    uint8_t ecc;
                    uint8_t rc;
                    if (gc_read_page_with_lba(pba, buf, &lba, &ecc, &rc) != 0)
                    {
                        continue; /* 读失败，跳过 */
                    }

                    /* B4：GC 搬移读记录 ECC 趋势 + read disturb 计数（慢死预测闭环）。
                     * rr_record_ecc 的 last_update 传 victim 平均擦除后的单调写入
                     * 时间戳近似值不可得（本函数无 total_writes），传 0 表示
                     * "不追踪时间维度"——rising_count/health 判定只用 ecc_bits 本身。 */
                    if (rc != 0U)
                    {
                        ftl_block_t *src = &alloc->blocks[gblock_src];
                        rr_record_ecc(&src->rr, ecc, 0U);
                        src->read_count++;
                    }
                }

                /* 查 L2P：当前映射是否仍指向本 PBA（bitmap 与 L2P 双校验） */
                old_pba = l2p_cache_read(cache, lba);
                if (old_pba != pba)
                {
                    /* 已被覆盖，无效页 → 清 bitmap 保持同步 */
                    ftl_block_clr_valid(&alloc->blocks[gblock_src], plane, page);
                    continue;
                }

                /* 分配新 PBA（Cold Super Block） */
                new_pba = allocator_alloc(alloc, FTL_TEMP_COLD);
                if (new_pba == NAND_PBA_INVALID)
                {
                    break;
                }

                /* 写用户数据到新位置 */
                {
                    nand_addr_t waddr = addr_from_pba(new_pba);
                    ret               = sync_write_page(waddr, buf, sizeof(buf), FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_COMMAND_MODULE_GC, (lba_t)lba);
                }
                if (ret != 0)
                {
                    continue;
                }

                /* 更新 L2P Cache */
                l2p_cache_write(cache, lba, new_pba);

                /* Journal 记录 */
                if (journal != NULL)
                {
                    journal_append(journal, lba, old_pba, new_pba);
                }

                /* VPC/bitmap 维护：源块清位，新块置位（单点更新防漂移） */
                ftl_block_clr_valid(&alloc->blocks[gblock_src], plane, page);
                ftl_block_set_valid(&alloc->blocks[ftl_pba_gblock(new_pba)],
                                    (uint8_t)pba_to_plane(new_pba),
                                    (uint8_t)pba_to_page(new_pba));

                /* GC 搬移代数：新块记录被搬过一次 */
                alloc->blocks[ftl_pba_gblock(new_pba)].gc_generation++;

                migrated++;
            }
        }
    }

    /* F10：搬移条目先落盘再擦除 victim。journal 是 write-back（page_buf 攒
     * 满才 flush），若不在此强制 flush，掉电落在"搬移条目未落盘 + victim
     * 已擦除"窗口 → 恢复回放指向已擦除块 = 数据损坏。 */
    if ((journal != NULL) && (migrated > 0U))
    {
        journal_flush_page(journal);
    }

    return (int)migrated;
}

int gc_check_trigger(const ftl_allocator_t *alloc, uint32_t total_writes, uint32_t elapsed_ms)
{
    if (alloc == NULL)
    {
        return FTL_OK;
    }

    if (alloc->pre_cond_count < FREE_POOL_TARGET)
    {
        return 1;
    }
    if ((total_writes % GC_INTERVAL_WRITES) == 0)
    {
        return 1;
    }
    if (elapsed_ms >= GC_TIMER_INTERVAL_MS)
    {
        return 1;
    }

    return FTL_OK;
}