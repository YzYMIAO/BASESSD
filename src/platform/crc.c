/*
 * crc.c — 跨 target 共享的 CRC-32 服务层
 *
 * 只放平台抽象与硬件后端选择；软件 CRC 在 src/fc/fc_crc_engine.c。
 * 寄存器动作（HAL 配置、IDR 读写、DR 写入）全在
 * src/platform/<soc>/crc_port.c。上层只调本文件的 crc_init() /
 * crc_compute() / crc_compute_iso_hdlc()。
 *
 * 硬件配置：
 *   FC 数据面：CRC-32/MPEG-2，默认多项式 0x04C11DB7、INIT 0xFFFFFFFF、
 *              输入/输出不反相、按 32 位字喂入；
 *   boot/FTL 元数据：CRC-32/ISO-HDLC，通过输入/输出反相 + 最终 XOR 计算。
 *   两者不是同一变体，不能直接互换。
 *
 * PLATFORM_CRC_HW_SUPPORTED == 0 时本文件不访问硬件；FC 软件 CRC 会接管。
 */

#include "crc.h"

#include "assertion.h"

#include <stddef.h>

/**
 * @brief 初始化硬件 CRC 后端。
 * @note 硬件后端做时钟/外设初始化与 IDR 应答自检；无硬件时本函数空操作。
 *       软件后端的表的初始化在 FC CRC 引擎内完成。
 */
void crc_init(void)
{
#if (PLATFORM_CRC_HW_SUPPORTED != 0U)
    crc_port_init();

    /*
     * IDR 写读回环：唯一能证明外设真的应答的检查 —— 时钟没开时写被静默丢弃、
     * 读回恒 0，而 HAL 的 CRC 初始化自身不校验。
     */
    crc_port_set_idr(0xA5U);
    ASSERT(crc_port_get_idr() == 0xA5U);
#else
    /* 无硬件后端；由 FC 软件 CRC 处理。 */
#endif
}

/**
 * @brief 对 data 起 length_bytes 字节计算 CRC-32。
 * @param data 输入数据
 * @param length_bytes 字节数；必须是 4 的倍数
 * @return CRC-32/MPEG-2 值
 * @note 前置：data 非空、4 字节对齐；length_bytes 是 4 的倍数（违反一律 ASSERT）。
 *       本函数是硬件 MPEG-2 后端；软件 fallback 在 FC CRC 引擎。
 */
DWORD crc_compute(const BYTE *data, DWORD length_bytes)
{
    ASSERT(data != NULL);
    ASSERT((length_bytes % 4U) == 0U);

#if (PLATFORM_CRC_HW_SUPPORTED != 0U)
    return crc_port_compute(data, length_bytes);
#else
    ASSERT(false);
    return 0U;
#endif
}

/**
 * @brief 对 data 起 length_bytes 字节计算 CRC-32/ISO-HDLC。
 * @param data 输入数据
 * @param length_bytes 字节数
 * @return CRC-32/ISO-HDLC 值
 * @note 硬件后端会临时切换到 ISO-HDLC 配置，计算后恢复 MPEG-2 配置；
 *       仅允许在串行数据路径调用。
 */
DWORD crc_compute_iso_hdlc(const BYTE *data, DWORD length_bytes)
{
    ASSERT(data != NULL);

#if (PLATFORM_CRC_HW_SUPPORTED != 0U)
    return crc_port_compute_iso_hdlc(data, length_bytes);
#else
    ASSERT(false);
    return 0U;
#endif
}
