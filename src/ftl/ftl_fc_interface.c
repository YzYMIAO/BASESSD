#include "ftl/ftl_fc_interface.h"

#include "fc/fc.h"
#include "ftl/completion_table.h"
#include "ftl/ftl_type.h"
#include "nand/onfi_1_0.h"

#include <string.h>

ftl_status_t ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba)
{
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.ch  = (DWORD)pba_to_ch(pba);
    cmd->config.ce  = (DWORD)pba_to_ce(pba);
    cmd->config.lun = (DWORD)pba_to_lun(pba);
    cmd->paa        = pba_to_paa(pba);

    return (fc_command_configure_descriptors(cmd) == FC_STATUS_OK)
               ? FTL_OK
               : FTL_ERROR;
}

ftl_status_t ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length)
{
    return (fc_write_cmd_data(cmd, data, length) == FC_STATUS_OK)
               ? FTL_OK
               : FTL_ERROR;
}

ftl_status_t ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length)
{
    return (fc_read_cmd_data(cmd, buffer, length) == FC_STATUS_OK)
               ? FTL_OK
               : FTL_ERROR;
}

/* ── 内部同步 NAND 操作 ── */

/*
 * 同步等待指定 command_id 完成。
 *
 * 使用 completion_table 统一路由：
 *   ① 注册 ftl_sync_waiter_handler 到 completion_table
 *   ② 循环 fc_service() + completion_table_drain() 直到 done
 *   ③ 无需处理 "other" completion — completion_table_drain 会自动路由
 *     非目标 completion 到 default handler（ftl_handle_completion）
 *
 * @param cmd_id      要等待的 command_id
 * @param status_out  输出 completion status
 * @param timeout_us  超时（0 = 使用默认 10ms）
 * @return 0 完成, -1 超时
 */
ftl_status_t ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out, uint32_t timeout_us)
{
    ftl_sync_waiter_t waiter;
    uint32_t          elapsed  = 0U;
    uint32_t          max_wait = (timeout_us > 0U) ? timeout_us : 10000U;

    waiter.cmd_id = cmd_id;
    waiter.done   = 0U;

    if (completion_table_add(cmd_id, ftl_sync_waiter_handler, &waiter) != 0)
    {
        return FTL_ERROR; /* 路由表满 */
    }

    for (;;)
    {
        (void)fc_service();
        completion_table_drain();

        if (waiter.done != 0U)
        {
            break;
        }

        /* 简易超时：elapsed 递增（精确度取决于 fc_service 调用频率） */
        elapsed++;
        if (elapsed > max_wait)
        {
            completion_table_remove(cmd_id);
            return FTL_ERROR;
        }
    }

    if (status_out != NULL)
    {
        *status_out = waiter.completion;
    }

    return FTL_OK;
}

ftl_status_t ftl_internal_read_page(nand_pba_t pba, void *buffer, uint32_t length,
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

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
    cmd->config.crc         = (DWORD)crc;
    cmd->config.random      = (DWORD)random;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = (DWORD)lba;

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

ftl_status_t ftl_internal_write_page(nand_pba_t pba, const void *data, uint32_t length,
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

    if (ftl_cmd_setup_ppa(cmd, pba) != 0)
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

ftl_status_t ftl_internal_erase_block(nand_pba_t block_pba)
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

    if (ftl_cmd_setup_ppa(cmd, block_pba) != 0)
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

ftl_status_t ftl_internal_read_page_raw(nand_pba_t pba, void *buffer)
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
            uint32_t cw_user_size = FC_BUFFER_USER_SIZE;
            memcpy((uint8_t *)buffer + (uint32_t)cw * FC_BUFFER_POOL_CW_SIZE,
                   src,
                   cw_user_size);
        }
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}