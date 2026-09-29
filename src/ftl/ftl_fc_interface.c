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