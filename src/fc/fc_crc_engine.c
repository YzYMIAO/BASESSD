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

/**
 * @brief 初始化硬件 CRC 外设
 *
 * 时钟使能、HAL 配置与外设应答自检都在平台层 crc_init() 内；自检不过
 * 一律 ASSERT，不再有「失败静默继续」的降级路径。
 */
void fc_crc_init(void)
{
    crc_init();
}

/**
 * @brief 计算 CW 的 512B USER 数据 CRC-32
 *
 * 平台层每次先 RESET 引擎回 INIT 值，保证独立计算；user_data 须 4 字节对齐
 * （HAL 的 WORDS 分支直接按 DWORD* 解引用）。
 *
 * @param user_data USER 区首地址
 * @return CRC-32
 */
DWORD fc_crc_compute(const BYTE *user_data)
{
    // user_data 前置条件由调用者保证
    ASSERT(user_data != NULL);

    return crc_compute(user_data, FC_CRC_USER_SIZE);
}

/**
 * @brief 校验 USER 数据 CRC 是否等于存储值
 *
 * @param user_data  USER 区首地址
 * @param stored_crc 读回的 CRC 字段
 * @return FC_CRC_RESULT_OK / FC_CRC_RESULT_ERROR
 */
BYTE fc_crc_verify(const BYTE *user_data, DWORD stored_crc)
{
    // user_data 前置条件由调用者保证
    ASSERT(user_data != NULL);

    return (fc_crc_compute(user_data) == stored_crc) ? FC_CRC_RESULT_OK : FC_CRC_RESULT_ERROR;
}
