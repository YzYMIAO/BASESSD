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

ftl_status_t ftl_cmd_setup_ppa_pair(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM])
{
    uint8_t plane;
    uint8_t plane_count;

    if ((cmd == NULL) || (pba == NULL))
    {
        return FTL_ERROR;
    }

    /* 拓扑取 plane0（同一命令两 plane 必属同一 ch/ce/lun）；
     * command->paa 仍写 plane0——它是 fc_command_plane_span 定 plane 段的输入，
     * 而各 plane 的实际行地址由 attach_cw 的 row_paa 逐条决定。 */
    cmd->config.ch  = (DWORD)pba_to_ch(pba[0]);
    cmd->config.ce  = (DWORD)pba_to_ce(pba[0]);
    cmd->config.lun = (DWORD)pba_to_lun(pba[0]);
    cmd->paa        = pba_to_paa(pba[0]);

    plane_count = (cmd->config.multi_plane > 0U) ? (uint8_t)cmd->config.multi_plane : 1U;

    for (plane = 0U; plane < plane_count; plane++)
    {
        uint8_t cw;

        for (cw = 0U; cw <= (uint8_t)cmd->config.cw; cw++)
        {
            if (fc_command_attach_cw(cmd, plane, cw, pba_to_paa(pba[plane])) != FC_STATUS_OK)
            {
                /* 半成品链由调用者 fc_command_release 统一回收 */
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
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