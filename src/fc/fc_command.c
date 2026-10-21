/*
 * fc_command.c — FC 命令校验
 *
 * 校验：确认 command 的 operation 合法、需要数据的操作必须带 desc 链。
 *
 * 注：CQ 完成项的组装与 per-CW 状态聚合**不在本文件**——自 `83fe922`（2026-09-20）
 * 起改由 fc_complete_from_nand()（fc.c）内联完成。原 fc_command_status_init /
 * fc_command_status_set_cw 自此零生产调用者（只有 Level 1 单测在调），
 * 已于 2026-10-08 连同用例一并删除。
 */

#include "fc_command.h"

#include "assertion.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/* fc_cw_status_str - 把 FC_CW_STATUS_* 位解码成短字符串（日志用）
 *
 * 输出示例："READY|UNC" / "UNC" / "NAND|TIMEOUT"。
 * 位顺序按严重性/可读性排列；未知位以 "?" 占位。只写 buf，不分配资源。
 */
const char *fc_cw_status_str(DWORD status, char *buf, DWORD buflen)
{
    static const struct
    {
        DWORD       bit;
        const char *name;
    } tbl[] = {
        { FC_CW_STATUS_NAND_FAIL,    "NAND"},
        {   FC_CW_STATUS_TIMEOUT, "TIMEOUT"},
        { FC_CW_STATUS_CRC_ERROR,     "CRC"},
        {       FC_CW_STATUS_UNC,     "UNC"},
        {FC_CW_STATUS_MATA_ERROR,    "MATA"},
        {FC_CW_STATUS_RAID_ERROR,    "RAID"},
        {     FC_CW_STATUS_READY,   "READY"},
        { FC_CW_STATUS_PROTECTED,    "PROT"},
        {     FC_CW_STATUS_BLANK,   "BLANK"},
    };

    if ((buf == NULL) || (buflen == 0U))
    {
        return "";
    }
    buf[0] = '\0';

    DWORD used  = 0U;
    DWORD known = 0U;

    for (DWORD i = 0U; i < (sizeof(tbl) / sizeof(tbl[0])); i++)
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

/* --------------------------------------------------------------------
 * fc_cw_bitmap_is_plane_symmetric
 *   功能：判断 flat CW 掩码在**每个 plane 上的 CW 集合是否相同**。
 *   参数：mask —— flat 掩码（bit n = plane*CW_NUM + cw）。
 *   返回：1 = 对称（mask == 0 视为对称）；0 = 不对称。
 *   用途：RAID / stripe 的 XOR 按 CW 配对，要求两 plane 同集合
 *         （见 fc_data_path_generate_stripe_parity 的前置断言）。
 * -------------------------------------------------------------------- */
BYTE fc_cw_bitmap_is_plane_symmetric(DWORD mask)
{
    const DWORD plane_bits = ((1U << CW_NUM) - 1U);
    const DWORD low        = mask & plane_bits;
    BYTE        plane;

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
 *   返回：恒 FC_STATUS_OK。
 *   实现要点（v20：整函数转 ASSERT）：
 *     - 三处前置违反——command 为空 / operation ≥ OP_COUNT / 数据操作无 desc 链——
 *       全是**调用者 bug**：`operation` 全仓都是 FTL 的硬编码常量（见
 *       ftl_fc_interface.c 的 sync_page_prepare 实参），**从不是 host 输入**，
 *       属判据 2，故一律 ASSERT（配置错误绝不发到硬件，§十三 定调）。
 *     - 本函数退化为「恒 OK」的校验钩子：debug 下跑内层断言；release 下调用点
 *       fc_submit 的 ASSERT(... == OK) 整条裁空，无运行时开销。
 *     - has_data 覆盖数据输出型操作（READ_STATUS/READ_ID 等），它们也需要
 *       buffer；data_input_cycles>0 只是其子集。
 *     - **不校验 CW 集合**（v13，v28 起集合甚至不在命令里）：它是 FTL 的建链输入，
 *       FC 只认 desc 链（链长度即真值）。越段检查随枚举逻辑一并上移 FTL，见
 *       ftl_cmd_configure_descriptors()。本函数只确认「有数据的命令必须已有链」。
 * -------------------------------------------------------------------- */
fc_status_e fc_command_validate(const fc_command_t *command)
{
    ASSERT(command != NULL);
    ASSERT(command->config.operation < OP_COUNT);
    /* 非 SET/GET_FEATURES 且涉及数据传输的操作，必须已配置 desc 链（无 buffer 无法搬运数据） */
    ASSERT((command->config.operation == OP_SET_FEATURES) ||
           (command->config.operation == OP_GET_FEATURES) ||
           !nand_operation_has_data(command->config.operation) ||
           (command->desc_id != FC_DESCRIPTOR_ID_INVALID));
    /* NDEBUG 下断言整段裁空 → command 变未用形参，吞掉（同 fc_submit 的 (void)command_id;） */
    (void)command;

    return FC_STATUS_OK;
}
