#include "ftl/gc.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/journal.h"
#include "fc/fc.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/*
 * 读取一页 NAND，返回用户数据 + 从 META 提取的 LBA。
 *
 * 内部管理 FC 命令生命周期：
 *   alloc → config → submit → sync_wait → read_data → get_meta → release
 *
 * @param pba         物理地址
 * @param buffer      输出：用户数据 buffer（2048 字节，可为 NULL）
 * @param lba_out     输出：从 META 提取的 LBA（可为 NULL）
 * @return 0 成功, -1 失败
 */
static int gc_read_page_with_lba(nand_pba_t pba, uint8_t *buffer, lba_t *lba_out)
{
    fc_command_t       *cmd;
    uint32_t            meta_cw0;
    int                 ret;

    cmd = fc_command_alloc();
    if (cmd == NULL) return -1;

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
        return -1;
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    {
        fc_command_status_t completion;
        ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
        if (ret != 0)
        {
            (void)fc_command_release(cmd);
            return -1;
        }

        if (completion.status & FC_CW_STATUS_NAND_FAIL)
        {
            (void)fc_command_release(cmd);
            return -1;
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

    return 0;
}

void gc_erase_and_free(ftl_allocator_t *alloc, super_block_t *victim)
{
    uint8_t plane;

    if ((alloc == NULL) || (victim == NULL) || (victim->valid != 0U)) return;

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint16_t    block;
        nand_pba_t  block_pba;
        int         ret;

        block = victim->plane_blocks[plane];
        block_pba = pba_make(alloc->ch, alloc->ce, 0, plane, block, 0, 0);

        ret = ftl_internal_erase_block(block_pba);
        if (ret == 0)
        {
            alloc->erase_counts[block]++;
            allocator_free_block(alloc, block);
        }
        else
        {
            allocator_mark_bad(alloc, block);
        }
    }
}

int gc_select_victim(ftl_allocator_t *alloc, super_block_t **victim)
{
    uint16_t       i;
    uint16_t       best_valid = PAGES_PER_BLOCK + 1U;
    int            found      = 0;

    if ((alloc == NULL) || (victim == NULL)) return 0;

    for (i = 0U; i < alloc->closed_sb_count; i++)
    {
        super_block_t *sb = &alloc->closed_sb_list[i];

        if (sb->valid != 0U) continue;

        if (sb->next_page < best_valid)
        {
            best_valid = sb->next_page;
            *victim    = sb;
            found      = 1;
        }
    }

    return found;
}

int gc_migrate_sb(ftl_allocator_t *alloc, l2p_cache_t *cache,
                   l2p_journal_t *journal, super_block_t *victim)
{
    uint8_t  plane;
    uint8_t  page;
    uint32_t migrated = 0U;

    if ((alloc == NULL) || (cache == NULL) || (victim == NULL)) return 0;

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (page = 0U; page < victim->next_page; page++)
        {
            uint8_t    buf[2048];
            lba_t      lba;
            nand_pba_t pba;
            nand_pba_t old_pba;
            nand_pba_t new_pba;
            int        ret;

            pba = super_block_page_pba(alloc->ch, alloc->ce,
                                       victim, plane, page);

            /* 读页 + 获取 LBA + 验证有效性（一步完成） */
            memset(buf, 0, sizeof(buf));
            if (gc_read_page_with_lba(pba, buf, &lba) != 0)
            {
                continue;  /* 读失败，跳过 */
            }

            /* 查 L2P：当前映射是否仍指向本 PBA */
            old_pba = l2p_cache_read(cache, lba);
            if (old_pba != pba)
            {
                continue;  /* 已被覆盖，无效页 */
            }

            /* 分配新 PBA（Cold Super Block） */
            new_pba = allocator_alloc(alloc, FTL_TEMP_COLD);
            if (new_pba == NAND_PBA_INVALID)
            {
                break;
            }

            /* 写用户数据到新位置 */
            ret = ftl_internal_write_page(new_pba, buf, sizeof(buf),
                                           FC_CRC_ENABLE, FC_RANDOM_ENABLE,
                                           FC_COMMAND_MODULE_GC, (lba_t)lba);
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

            migrated++;
        }
    }

    return (int)migrated;
}

int gc_check_trigger(const ftl_allocator_t *alloc, uint32_t total_writes, uint32_t elapsed_ms)
{
    if (alloc == NULL) return 0;

    if (alloc->pre_cond_count < PRE_COND_LOW_WATER) return 1;
    if ((total_writes % GC_INTERVAL_WRITES) == 0) return 1;
    if (elapsed_ms >= GC_TIMER_INTERVAL_MS) return 1;

    return 0;
}