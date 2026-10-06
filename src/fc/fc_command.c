/*
 * fc_command.c — FC 命令校验与完成项状态维护
 *
 * 校验：确认 command 的 operation 合法、需要数据的操作必须带 desc 链。
 * 完成项：从 command 初始化 fc_command_status_t（命令级聚合状态），
 * 各 CW 完成时经 fc_command_status_set_cw() 逐 CW OR 合并状态位，
 * 并记录首个失败 descriptor（供错误路径查找）。
 */

#include "fc_command.h"

#include "common/assertion.h"
#include "nand/onfi_1_0.h"
#include "onfi_1_0.h"

#include <stddef.h>

/* --------------------------------------------------------------------
 * fc_cw_mask_is_plane_symmetric
 *   功能：判断 flat CW 掩码在**每个 plane 上的 CW 集合是否相同**。
 *   参数：mask —— flat 掩码（bit n = plane*CW_NUM + cw）。
 *   返回：1 = 对称（mask == 0 视为对称）；0 = 不对称。
 *   用途：RAID / stripe 的 XOR 按 CW 配对，要求两 plane 同集合
 *         （见 fc_data_path_generate_stripe_parity 的前置断言）。
 * -------------------------------------------------------------------- */
uint8_t fc_cw_mask_is_plane_symmetric(uint32_t mask)
{
    const uint32_t plane_bits = (uint32_t)((1U << CW_NUM) - 1U);
    const uint32_t low        = mask & plane_bits;
    uint8_t        plane;

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (((mask >> (plane * CW_NUM)) & plane_bits) != low)
        {
            return 0U;
        }
    }
    return 1U;
}

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
 *     - flat CW 掩码必须落在本命令的 plane 位段内（mask == 0 = 未指定，恒合法）：
 *       位宽上界 PLANE_NUM*CW_NUM 位；单 plane 命令的位段从 paa_to_plane(paa) 起
 *       （绝对编号——plane 1 的单 plane 命令写 0xF0，不是 0x0F）。
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

    if (command->param.cw_mask != 0U)
    {
        uint32_t plane_count = (command->config.multi_plane > 0U) ? (uint32_t)command->config.multi_plane : 1U;
        uint32_t allowed;

        if (plane_count > PLANE_NUM)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        allowed = ((uint32_t)((1U << (plane_count * CW_NUM)) - 1U)) << ((uint32_t)((plane_count == 1U) ? paa_to_plane(command->paa) : 0U) * CW_NUM);
        if ((command->param.cw_mask & ~allowed) != 0U)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
    }
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_command_status_init
 *   功能：从命令初始化 CQ 完成项（全清零 + 哨兵值）。
 *   参数：status —— 完成项；command —— 命令（可为 NULL）。
 *   返回：无。
 *   实现要点：
 *     - 整体清零：per-CW 细节不再进 CQ（只留在 desc 链），返回段 param 归零。
 *     - 初始 status = READY（FC_CW_STATUS_SUCCESS）。
 * -------------------------------------------------------------------- */
void fc_command_status_init(fc_command_status_t *status, const fc_command_t *command)
{
    /* status 为出参前置条件；command 可为 NULL 是有意语义（辨析 B），保留分支 */
    ASSERT(status != NULL);

    *status            = (fc_command_status_t){0};
    status->command_id = (command != NULL) ? command->command_id : FC_COMMAND_ID_INVALID;
    status->desc_id    = (command != NULL) ? command->desc_id : FC_DESCRIPTOR_ID_INVALID;
    status->module     = (command != NULL) ? command->module : 0U;
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
    /* status/cw 为前置条件（cw 索引恒 < CW_NUM=4，调用者按 desc 链枚举） */
    ASSERT(status != NULL);
    ASSERT(cw < 4U);
    (void)cw; /* release（NDEBUG）下 ASSERT 被裁掉，cw 仅用于前置条件 */

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
