/*
 * fc_crc_engine.c — CRC-32 引擎实现
 *
 * 使用 STM32H743 硬件 CRC 外设（默认多项式 0x04C11DB7）
 * 对每个 CW 的 512 字节 USER 数据计算 CRC-32。
 *
 * CRC 覆盖范围（buffer 布局）：
 *   [META:4][CRC:4][USER:512][PARITY:8]  ← 528B CW
 *            ↑      ↑
 *            │      └──── 被 CRC 保护的数据（512B 用户数据）
 *            └─────────── CRC 值存放位置（4B，编码时 FC 写入）
 *
 * 编码流程中的位置：
 *   填写 USER 数据 → CRC 计算（写入 CRC 字段）
 *   → Random 加扰（全 CW） → ECC 编码 → RAID XOR（全 CW）
 *
 * 解码流程中的位置：
 *   ECC 解码 → Random 解扰（全 CW） → CRC 校验
 *
 * CRC 只保护 USER 数据（512B），META、CRC 字段本身和 PARITY 不被 CRC 覆盖。
 * ECC 保护全 CW（包括 CRC 字段和 PARITY），确保 CRC 值在存储/传输中不受损。
 *
 * 硬件访问说明：
 *   外设访问在平台层的 CRC 适配（走第三方 HAL CRC 驱动，基址由设备头宏提供）。
 *   本文件只负责覆盖范围与判据，不接触任何 HAL 类型。
 */

#include "fc_crc_engine.h"

#include "assertion.h"
#include "crc.h"

#include <stddef.h>

/* --------------------------------------------------------------------
 * fc_crc_init
 *   功能：初始化 CRC 硬件（转交平台层）。
 *   参数：无。
 *   返回：无。
 *   实现要点：
 *     - 时钟使能、HAL 配置、外设应答自检都在平台层 crc_init() 内；
 *       自检不过一律 ASSERT，不再有「失败静默继续」的降级路径。
 * -------------------------------------------------------------------- */
void fc_crc_init(void)
{
    crc_init();
}

/* --------------------------------------------------------------------
 * fc_crc_compute
 *   功能：对 512B USER 数据计算 CRC-32（硬件 CRC 外设）。
 *   参数：user_data —— USER 数据指针（buffer+8），**必须 4 字节对齐**。
 *   返回：CRC-32 值。
 *   实现要点：
 *     - 平台层每次先 RESET 引擎回 INIT 值，保证独立计算。
 *     - 对齐是硬性前置条件：HAL 的 WORDS 分支直接按 DWORD* 解引用。
 *       实测三个调用点（buffer 池 +8、测试夹具两块 malloc 缓冲）全部满足。
 * -------------------------------------------------------------------- */
DWORD fc_crc_compute(const BYTE *user_data)
{
    /* user_data 前置条件由调用者保证 */
    ASSERT(user_data != NULL);

    return crc_compute(user_data, FC_CRC_USER_SIZE);
}

/* --------------------------------------------------------------------
 * fc_crc_verify
 *   功能：校验 USER 数据的 CRC 与存储值是否一致。
 *   参数：user_data —— USER 指针；stored_crc —— 存储的 CRC 值。
 *   返回：FC_CRC_RESULT_OK / FC_CRC_RESULT_ERROR。
 * -------------------------------------------------------------------- */
BYTE fc_crc_verify(const BYTE *user_data, DWORD stored_crc)
{
    /* user_data 前置条件由调用者保证 */
    ASSERT(user_data != NULL);

    return (fc_crc_compute(user_data) == stored_crc) ? FC_CRC_RESULT_OK : FC_CRC_RESULT_ERROR;
}
