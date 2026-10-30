/*
 * fc_command.c — FC 命令校验
 *
 * 校验：确认 command 的 operation 合法、需要数据的操作必须带 desc 链。
 *
 * 注：CQ 完成项的组装与 per-CW 状态聚合**不在本文件**——由
 * fc_complete_from_nand()（fc.c）内联完成。
 */

#include "fc_command.h"

#include "assertion.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

// 状态位名表 → "A|B|?"。位顺序按严重性/可读性排列；未知位以 "?" 占位。
// 只写 buf，不分配资源。
typedef struct
{
    DWORD       bit;
    const char *name;
} fc_status_name_t;

static const char *fc_status_str_format(const fc_status_name_t *tbl, DWORD count, DWORD status, char *buf, DWORD buflen)
{
    if ((buf == NULL) || (buflen == 0U))
    {
        return "";
    }
    buf[0] = '\0';

    DWORD used  = 0U;
    DWORD known = 0U;

    for (DWORD i = 0U; i < count; i++)
    {
        if ((status & tbl[i].bit) == 0U)
        {
            continue;
        }
        known |= tbl[i].bit;

        const DWORD len = (DWORD)strlen(tbl[i].name);
        const DWORD sep = (used != 0U) ? 1U : 0U;

        if ((used + sep + len) >= buflen)
        {
            break;
        }
        if (sep != 0U)
        {
            buf[used++] = '|';
        }
        (void)memcpy(&buf[used], tbl[i].name, len);
        used += len;
    }

    if ((status & ~known) != 0U)
    {
        if ((used + 2U) < buflen)
        {
            buf[used++] = '|';
            buf[used++] = '?';
        }
    }
    buf[used] = '\0';
    return buf;
}

static const fc_status_name_t s_cw_status_names[] = {
    {      FC_CW_STATUS_FAIL,  "FAIL"},
    {       FC_CW_STATUS_UNC,   "UNC"},
    { FC_CW_STATUS_CRC_ERROR,   "CRC"},
    {FC_CW_STATUS_MATA_ERROR,  "MATA"},
    {FC_CW_STATUS_RAID_ERROR,  "RAID"},
    {     FC_CW_STATUS_EMPTY, "EMPTY"},
};

static const fc_status_name_t s_cmd_status_names[] = {
    { FC_CMD_STATUS_NAND_FAIL,    "NAND"},
    {FC_CMD_STATUS_FAIL_CACHE,   "FAILC"},
    {   FC_CMD_STATUS_TIMEOUT, "TIMEOUT"},
    {FC_CMD_STATUS_DATA_ERROR,    "DATA"},
    {FC_CMD_STATUS_MATA_ERROR,    "MATA"},
    {    FC_CMD_STATUS_AREADY,    "ARDY"},
    {       FC_CMD_STATUS_RDY,     "RDY"},
    {        FC_CMD_STATUS_WP,      "WP"},
};

const char *fc_cw_status_str(DWORD status, char *buf, DWORD buflen)
{
    return fc_status_str_format(s_cw_status_names, (DWORD)(sizeof(s_cw_status_names) / sizeof(s_cw_status_names[0])), status, buf, buflen);
}

const char *fc_cmd_status_str(DWORD status, char *buf, DWORD buflen)
{
    return fc_status_str_format(s_cmd_status_names, (DWORD)(sizeof(s_cmd_status_names) / sizeof(s_cmd_status_names[0])), status, buf, buflen);
}

// 以 plane 0 的低 CW_NUM 位为基准逐 plane 比较；mask == 0 时各 plane 均为 0，判为对称。
bool fc_cw_bitmap_is_plane_symmetric(DWORD mask)
{
    const DWORD plane_bits = ((1U << CW_NUM) - 1U);
    const DWORD low        = mask & plane_bits;
    BYTE        plane;

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (((mask >> (plane * CW_NUM)) & plane_bits) != low)
        {
            return false;
        }
    }
    return true;
}

// 实现要点：只做前置条件断言，恒返回 FC_STATUS_OK；release 下断言整段裁空。
fc_status_e fc_command_validate(const fc_command_t *command)
{
    ASSERT(command != NULL);
    ASSERT(command->config.operation < OP_COUNT);
    // 非 SET/GET_FEATURES 且涉及数据传输的操作，必须已配置 desc 链（无 buffer 无法搬运数据）
    ASSERT((command->config.operation == OP_SET_FEATURES) || (command->config.operation == OP_GET_FEATURES) || (nand_operation_has_data(command->config.operation) == false) || (command->desc_id != FC_DESCRIPTOR_ID_INVALID));
    // NDEBUG 下断言整段裁空 → command 变未用形参，吞掉（同 fc_submit 的 (void)command_id;）
    (void)command;

    return FC_STATUS_OK;
}
