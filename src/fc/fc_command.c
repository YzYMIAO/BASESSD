/*
 * fc_command.c — FC 命令校验
 *
 * 校验：确认 command 的 operation 合法、需要数据的操作必须带 desc 链。
 *
 * 注：CQ 完成项的组装与 per-CW 状态聚合**不在本文件**——由
 * fc_complete_from_nand()（fc.c）内联完成。
 */

#include "assertion.h"
#include "fc_interface.h"
#include "nand_interface.h"

#include <stddef.h>
#include <string.h>

_Static_assert(OP_COUNT <= 256U, "fc_cmd_cfg_s.operation is 8 bits");

// 状态位名表 → "A|B|?"。位顺序按严重性/可读性排列；未知位以 "?" 占位。
// 只写 buf，不分配资源。
typedef struct FC_STATUS_NAME_S
{
    DWORD       bit;
    const char *name;
} fc_status_name_s;
/**
 * @brief 把状态位集合格式化成 "A|B|?" 文本
 *
 * 只写 buf，不分配资源；buf 不足时截断。未知位置以 "?" 占位。
 *
 * @param tbl    位名表
 * @param count  表项数
 * @param status 状态位集合
 * @param buf    输出缓冲
 * @param buflen 缓冲长度（0 时返回空串）
 * @return buf（或 buflen 为 0 时的空串）
 */
static const char *fc_status_str_format(const fc_status_name_s *tbl, DWORD count, DWORD status, char *buf, DWORD buflen)
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
        memcpy(&buf[used], tbl[i].name, len);
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

static const fc_status_name_s s_cw_status_names[] = {
    {      FC_CW_STATUS_FAIL,  "FAIL"},
    {       FC_CW_STATUS_UNC,   "UNC"},
    { FC_CW_STATUS_CRC_ERROR,   "CRC"},
    {FC_CW_STATUS_META_ERROR,  "META"},
    {FC_CW_STATUS_RAID_ERROR,  "RAID"},
    {     FC_CW_STATUS_EMPTY, "EMPTY"},
};

static const fc_status_name_s s_cmd_status_names[] = {
    { FC_CMD_STATUS_NAND_FAIL,    "NAND"},
    {FC_CMD_STATUS_FAIL_CACHE,   "FAILC"},
    {   FC_CMD_STATUS_TIMEOUT, "TIMEOUT"},
    {FC_CMD_STATUS_DATA_ERROR,    "DATA"},
    {FC_CMD_STATUS_META_ERROR,    "META"},
    {    FC_CMD_STATUS_AREADY,    "ARDY"},
    {       FC_CMD_STATUS_RDY,     "RDY"},
    {        FC_CMD_STATUS_WP,      "WP"},
};
/**
 * @brief CW 状态位转文本（"FAIL|UNC|CRC|…"）
 *
 * @param status CW 状态位集合
 * @param buf    输出缓冲
 * @param buflen 缓冲长度
 * @return buf
 */
const char *fc_cw_status_str(DWORD status, char *buf, DWORD buflen)
{
    return fc_status_str_format(s_cw_status_names, (DWORD)(sizeof(s_cw_status_names) / sizeof(s_cw_status_names[0])), status, buf, buflen);
}
/**
 * @brief 命令状态位转文本（"NAND|FAILC|TIMEOUT|…"）
 *
 * @param status 命令状态位集合
 * @param buf    输出缓冲
 * @param buflen 缓冲长度
 * @return buf
 */
const char *fc_cmd_status_str(DWORD status, char *buf, DWORD buflen)
{
    return fc_status_str_format(s_cmd_status_names, (DWORD)(sizeof(s_cmd_status_names) / sizeof(s_cmd_status_names[0])), status, buf, buflen);
}

/**
 * @brief 判断 flat CW 位图是否各 plane 一致
 *
 * 以 plane 0 的低 CW_NUM 位为基准逐 plane 比较；mask == 0 视为对称。
 *
 * @param mask flat 绝对 CW 位图
 * @return true 对称；false 不对称
 */
BOOL fc_cw_bitmap_is_plane_symmetric(DWORD mask)
{
    const DWORD plane_bits = ((1U << CW_NUM) - 1U);
    const DWORD low        = mask & plane_bits;
    BYTE        plane;

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (((mask >> fc_cw_flat(plane, 0U)) & plane_bits) != low)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 命令前置条件校验
 *
 * 前置违反一律 ASSERT；release 下断言整段裁空（调用链保证前置）。
 *
 * @param cmd 命令
 */
void fc_command_validate(const fc_cmd_s *cmd)
{
    ASSERT(cmd->config.operation < OP_COUNT);
    // 非 SET/GET_FEATURES 且涉及数据传输的操作，必须已配置 desc 链（无 buffer 无法搬运数据）
    ASSERT((cmd->config.operation == OP_SET_FEATURES) || (cmd->config.operation == OP_GET_FEATURES) || (nand_operation_has_data(cmd->config.operation) == false) || (cmd->desc_id != FC_DESC_ID_INVALID));
    // NDEBUG 下断言整段裁空 → cmd 变未用形参，吞掉
    (void)cmd;
}
