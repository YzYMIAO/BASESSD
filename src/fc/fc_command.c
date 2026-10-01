/*
 * fc_command.c — FC 命令校验与完成项状态维护
 *
 * 校验：确认 command 的 operation 合法、需要数据的操作必须带 desc 链。
 * 完成项：从 command 初始化 fc_command_status_t（命令级聚合状态），
 * 各 CW 完成时经 fc_command_status_set_cw() 逐 CW OR 合并状态位，
 * 并记录首个失败 descriptor（供错误路径查找）。
 */

#include "fc_command.h"

#include "nand/onfi_1_0.h"
#include "onfi_1_0.h"

#include <stddef.h>

/* --------------------------------------------------------------------
 * fc_command_validate
 *   功能：校验命令配置合法性。
 *   参数：command —— FC 命令。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT。
 *   实现要点：
 *     - operation 必须 < OP_COUNT。
 *     - 非 SET/GET_FEATURES 且涉及数据传输（has_data）的操作，
 *       必须已配置 desc 链（desc_id 有效）——无 buffer 无法搬运数据。
 *     - has_data 覆盖数据输出型操作（READ_STATUS/READ_ID 等），
 *       它们也需要 buffer；data_input_cycles>0 只是其子集。
 * -------------------------------------------------------------------- */
fc_status_t fc_command_validate(const fc_command_t *command)
{
    const nand_operation_map_t *op;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (command->config.operation >= OP_COUNT)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    op = &g_nand_operation_map[command->config.operation];
    if ((command->config.operation != OP_SET_FEATURES) &&
        (command->config.operation != OP_GET_FEATURES) &&
        nand_operation_has_data((uint8_t)command->config.operation) &&
        (command->desc_id == FC_DESCRIPTOR_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    /* data_input_cycles > 0 is a subset of nand_operation_has_data() — the latter also
     * includes data-output operations (READ_STATUS, READ_ID, etc.) that need a buffer. */
    (void)op;
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_command_status_init
 *   功能：从命令初始化 CQ 完成项（全清零 + 哨兵值）。
 *   参数：status —— 完成项；command —— 命令（可为 NULL）。
 *   返回：无。
 *   实现要点：
 *     - 未使用的 CW ECC 槽位置 FC_CW_STATUS_UNUSED（按 cw 数裁剪）。
 *     - 初始 status = READY（FC_CW_STATUS_SUCCESS）。
 * -------------------------------------------------------------------- */
void fc_command_status_init(fc_command_status_t *status, const fc_command_t *command)
{
    uint8_t count;

    if (status == NULL)
    {
        return;
    }
    *status            = (fc_command_status_t){0};
    status->command_id = (command != NULL) ? command->command_id : FC_COMMAND_ID_INVALID;
    status->desc_id    = (command != NULL) ? command->desc_id : FC_DESCRIPTOR_ID_INVALID;
    status->module     = (command != NULL) ? command->module : 0U;
    count              = (command != NULL) ? (uint8_t)(command->config.cw + 1U) : 0U;
    status->ecc_bit0   = 0U;
    status->ecc_bit1   = (count > 1U) ? 0U : FC_CW_STATUS_UNUSED;
    status->ecc_bit2   = (count > 2U) ? 0U : FC_CW_STATUS_UNUSED;
    status->ecc_bit3   = (count > 3U) ? 0U : FC_CW_STATUS_UNUSED;
    status->status     = FC_CW_STATUS_READY;
}

/* --------------------------------------------------------------------
 * fc_command_status_set_cw
 *   功能：将单个 CW 的状态合并进命令级聚合状态。
 *   参数：status —— 完成项；cw —— CW 索引（<4）；
 *         cw_status —— FC_CW_STATUS_* 位；descriptor_id —— 该 CW 描述符。
 *   返回：无。
 *   实现要点：
 *     - 聚合 status |= cw_status（任意 CW 置位即置位）。
 *     - 失败位（NAND_FAIL/TIMEOUT/CRC/UNC/MATA/RAID）出现时记录
 *       desc_id——仅第一个失败 CW 覆盖 desc_id（后续失败保持首个）。
 * -------------------------------------------------------------------- */
void fc_command_status_set_cw(fc_command_status_t *status, uint8_t cw, uint8_t cw_status, fc_descriptor_id_t descriptor_id)
{
    if ((status == NULL) || (cw >= 4U))
    {
        return;
    }

    /* OR per-CW status bits into the aggregated command-level status. */
    status->status |= cw_status;

    /* Record the first-failing descriptor's ID for error-path lookup. */
    if ((cw_status & (FC_CW_STATUS_NAND_FAIL | FC_CW_STATUS_TIMEOUT | FC_CW_STATUS_CRC_ERROR |
                      FC_CW_STATUS_UNC | FC_CW_STATUS_MATA_ERROR | FC_CW_STATUS_RAID_ERROR)) != 0U)
    {
        /* Only overwrite desc_id on first error; later CW errors keep the first. */
        if ((status->status & (FC_CW_STATUS_NAND_FAIL | FC_CW_STATUS_TIMEOUT | FC_CW_STATUS_CRC_ERROR |
                               FC_CW_STATUS_UNC | FC_CW_STATUS_MATA_ERROR | FC_CW_STATUS_RAID_ERROR)) == cw_status)
        {
            status->desc_id = descriptor_id;
        }
    }
}
