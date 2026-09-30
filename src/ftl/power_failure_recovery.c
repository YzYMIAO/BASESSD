#include "ftl/power_failure_recovery.h"

#include "fc/fc.h"
#include "ftl/checkpoint.h"
#include "ftl/ftl.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/metadata.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 内部同步读页（原 ftl_internal_read_page 内联化，元数据路径）── */
static ftl_status_t sync_read_page(nand_addr_t addr, void *buffer, uint32_t length)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
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

    if ((buffer != NULL) && (length > 0U))
    {
        (void)ftl_cmd_read_data(cmd, buffer, length);
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* ── 内部同步读全页（原 ftl_internal_read_page_raw 内联化，LDPC_RAW）── */
static ftl_status_t sync_read_page_raw(nand_addr_t addr, void *buffer)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;
    uint8_t             cw;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_LDPC_RAW;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_BBT;
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

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    for (cw = 0U; cw < CW_NUM; cw++)
    {
        uint8_t *src = fc_get_cmd_data_ptr(cmd, cw);
        if (src != NULL)
        {
            memcpy((uint8_t *)buffer + (uint32_t)cw * FC_BUFFER_POOL_CW_SIZE,
                   src,
                   FC_BUFFER_USER_SIZE);
        }
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* ── Journal Block 页地址辅助（双 Block 双 Plane 交错）── */
static nand_addr_t journal_page_addr(const ftl_context_t *ftl, uint8_t b, uint8_t page)
{
    nand_addr_t addr;

    addr.ch     = ftl->topo.ch;
    addr.ce     = ftl->topo.ce;
    addr.lun    = ftl->topo.lun;
    addr.block  = (b == 0U) ? JOURNAL_BLOCK0 : JOURNAL_BLOCK1;
    addr.plane  = (uint8_t)(page & 1U);
    addr.page   = page;
    addr.cw     = 0U;

    return addr;
}

ftl_status_t ftl_boot_recovery(struct ftl_context *ftl, ftl_boot_result_t *result)
{
    l2p_checkpoint_header_t cp;
    ftl_boot_result_t       local_result;

    if ((ftl == NULL) || (result == NULL))
    {
        return FTL_ERROR;
    }

    memset(&local_result, 0, sizeof(local_result));

    /* ① 扫描 Checkpoint */
    if (checkpoint_find_latest(&cp, ftl->topo) != FTL_OK)
    {
        /* 无有效 Checkpoint → 出厂首次启动 */
        int ret = ftl_boot_fresh_start(ftl);
        if (ret == FTL_OK)
        {
            local_result.boot_type       = BOOT_FRESH_START;
            local_result.cp_sequence     = 0U;
            local_result.journal_entries = 0U;
            *result                      = local_result;
        }
        return ret;
    }

    /* ② 扫描 Journal 获取最后 sequence（直接读 header 不修改状态） */
    {
        uint32_t journal_last_seq = 0U;

        /*
         * 从 Journal 最新有效页读最后 sequence。
         * 不调 journal_replay——后者会修改 journal 内部状态。
         * 用临时 scan 读各页的 header 取最大 page_seq。
         */
        {
            uint8_t b, page;

            for (b = 0U; b < 2U; b++)
            {
                for (page = 0U; page < PAGES_PER_BLOCK; page++)
                {
                    uint8_t     page_data[JOURNAL_PAGE_SIZE];
                    nand_addr_t pba;
                    int         ret;
                    uint32_t    magic;

                    pba = journal_page_addr(ftl, b, page);

                    ret = sync_read_page(pba, page_data, sizeof(page_data));
                    if (ret != 0)
                    {
                        continue;
                    }

                    memcpy(&magic, page_data + 0, sizeof(magic));
                    if (magic != JOURNAL_MAGIC)
                    {
                        continue;
                    }

                    {
                        uint32_t ps;
                        memcpy(&ps, page_data + 4, sizeof(ps));
                        if (ps > journal_last_seq)
                        {
                            journal_last_seq = ps;
                        }
                    }
                }
            }
        }

        if (journal_last_seq <= cp.journal_sequence)
        {
            /* 干净关机 */
            int ret = ftl_boot_clean_start(ftl, cp.sequence);
            if (ret == FTL_OK)
            {
                local_result.boot_type       = BOOT_CLEAN_START;
                local_result.cp_sequence     = cp.sequence;
                local_result.journal_entries = 0U;
                *result                      = local_result;
            }
            return ret;
        }
        else
        {
            /* 异常掉电 */
            int ret = ftl_boot_power_failure(ftl, cp.journal_sequence, &local_result);
            if (ret == FTL_OK)
            {
                local_result.boot_type   = BOOT_POWER_FAILURE;
                local_result.cp_sequence = cp.sequence;
                *result                  = local_result;
            }
            return ret;
        }
    }
}

ftl_status_t ftl_boot_fresh_start(struct ftl_context *ftl)
{
    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    /* ① L2P Cache 全 INVALID */
    l2p_cache_init(&ftl->cache);

    /* ② Allocator 已由 ftl_init 初始化，此处不需重复做 */

    /* ③ Journal 已由 ftl_init 初始化（空） */
    /* ④ checkpoint sequence = 0 */
    ftl->cp_sequence    = 0U;
    ftl->cp_timer_start = 0U;

    return FTL_OK;
}

ftl_status_t ftl_boot_clean_start(struct ftl_context *ftl, uint32_t cp_sequence)
{
    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    /* ① Cache 全 INVALID（首次 miss 时按需从 NAND L2P Table 加载） */
    l2p_cache_init(&ftl->cache);

    /* ② Allocator 已由 ftl_init 初始化（free pool + 各列表已就绪）*/

    /* ③ 写指针恢复：干净关机时 next_page 由 allocator 在内存中跟踪 */
    /* 当前 allocator 的 open SB 在 init 时未初始化，等首次 alloc 分配新 SB */
    /*  clean start 场景不需要扫描 NAND */

    /* ④ Controller → Ready */
    ftl->cp_sequence    = cp_sequence;
    ftl->cp_timer_start = 0U;

    return FTL_OK;
}

ftl_status_t ftl_boot_power_failure(struct ftl_context *ftl,
                           uint32_t            cp_sequence,
                           ftl_boot_result_t  *result)
{
    uint32_t entry_count;

    if ((ftl == NULL) || (result == NULL))
    {
        return FTL_ERROR;
    }

    /* ① Cache 全 INVALID */
    l2p_cache_init(&ftl->cache);

    /* ② Journal 回放（sequence > cp_sequence 的条目 → L2P Cache） */
    /* journal_replay 已实现扫描 J0/J1，提取 entry 并 l2p_cache_write */
    {
        (void)journal_replay(&ftl->journal, &ftl->cache);

        /*
         * 估算回放的 entry 数：
         *   replay 后的 current_entry/current_page 减去初始值
         *   简化：如果 current_entry > 0 说明有数据回放
         */
        if (ftl->journal.current_entry > 0U)
        {
            entry_count = (uint32_t)ftl->journal.current_page *
                              JOURNAL_ENTRIES_PER_PAGE +
                          ftl->journal.current_entry;
        }
        else
        {
            entry_count = 0U;
        }

        /*
         * journal_replay 写入了 L2P Cache，所有 line 标记 DIRTY。
         * 这些 DIRTY entries 会在 ftl_service IDLE 模式中由 background flush
         * 逐步写回 NAND L2P Table。
         */
    }

    /* ③ Allocator 已初始化 + 写指针扫描恢复 */
    {
        /* Hot Super Block 写指针扫描 */
        if (ftl->alloc.hot_sb.valid != 0U)
        {
            ftl_boot_scan_write_pointer(&ftl->alloc,
                                        0,
                                        0,
                                        &ftl->alloc.hot_sb);
        }

        /* Cold Super Block 写指针扫描 */
        if (ftl->alloc.cold_sb.valid != 0U)
        {
            ftl_boot_scan_write_pointer(&ftl->alloc,
                                        0,
                                        0,
                                        &ftl->alloc.cold_sb);
        }
    }

    /* ④ Controller → Ready */
    ftl->cp_sequence    = cp_sequence;
    ftl->cp_timer_start = 0U;

    /* ⑤ 输出结果 */
    result->boot_type       = BOOT_POWER_FAILURE;
    result->cp_sequence     = cp_sequence;
    result->journal_entries = entry_count;

    return FTL_OK;
}

void ftl_boot_scan_write_pointer(ftl_allocator_t *alloc,
                                 uint8_t          ch,
                                 uint8_t          ce,
                                 super_block_t   *sb)
{
    uint8_t page;
    uint8_t plane;

    if ((alloc == NULL) || (sb == NULL) || (sb->valid == 0U))
    {
        return;
    }

    /*
     * 扫描 NAND 找第一个全 0xFF 页（= 未写入的空白页）。
     * 从 Page 0 开始逐 Plane 扫描，每种 mode 尝试一次读。
     *
     * 注意：使用标准 BCH ECC 读，如果页已写但 ECC 失败，
     * 返回 -1 表示该页非空白。全 0xFF 的未写页在 LDPC_RAW 下
     * 读出的数据也全 0xFF。
     *
     * 同时重建 BST 有效页位图：扫描到的非空页置位（掉电后
     * bitmap/VPC 是 RAM 态，冷启动为零，必须从这里重建）。
     */
    for (page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            uint8_t     buf[16]; /* 读 16 字节就够了 */
            nand_addr_t pba;
            int         ret;

            pba = addr_from_paa(ch, ce, paa_make(page, plane, sb->plane_blocks[plane], 0U, 0U));

            memset(buf, 0xFF, sizeof(buf));

            /*
             * 用 LDPC_RAW 模式读（跳过 ECC，获得原始数据）。
             * 如果完全全 0xFF → 空页。
             */
            ret = sync_read_page_raw(pba, buf);

            /*
             * 即使 LDPC_RAW 返回失败，也检查数据是否全 FF。
             * 全 FF + 读失败 → 出厂坏块或空块
             * 全 FF + 读成功 → 空页
             */
            {
                uint8_t i;
                uint8_t all_ff = 1U;

                for (i = 0U; i < sizeof(buf); i++)
                {
                    if (buf[i] != 0xFFU)
                    {
                        all_ff = 0U;
                        break;
                    }
                }

                /*
                 * 找到第一个全 0xFF 页 → 写指针位置。
                 * 只检查 Plane 0（双 Plane 同步写入）。
                 */
                if (all_ff)
                {
                    sb->next_page = page;
                    return;
                }
            }

            /* 非空页：写指针之前的数据页，重建 BST 有效位图 + 置 OPEN 状态 */
            if (alloc->blocks != NULL)
            {
                uint16_t    gblock = ftl_gblock(plane, sb->plane_blocks[plane]);
                ftl_block_t *b     = &alloc->blocks[gblock];

                ftl_block_set_valid(b, plane, page);
                if (!ftl_block_is_bad(b))
                {
                    ftl_block_state_set(b, FTL_BLOCK_STATE_OPEN);
                    ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE | FTL_BLOCK_STATE_CLOSED);
                }
            }

            (void)ret; /* ret 不重要，有数据就继续扫 */
        }
    }

    /*
     * 全 Block 都没找到空页 → Block 已写满。
     * next_page 保持 PAGES_PER_BLOCK（=64，已满）。
     * 调用者会在 allocator_alloc 中检测到写满并分配新 SB。
     */
    sb->next_page = PAGES_PER_BLOCK;
}