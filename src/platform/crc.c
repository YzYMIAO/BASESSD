/*
 * crc.c — 跨 target 共享的 CRC-32 服务层
 *
 * 共享平台 CRC 服务：ISO-HDLC 的软件实现也在这里，硬件动作在
 * src/platform/<soc>/crc_port.c。
 *
 * 硬件配置：
 *   FC 数据面：CRC-32/MPEG-2，默认多项式 0x04C11DB7、INIT 0xFFFFFFFF、
 *              输入/输出不反相、按 32 位字喂入；
 *   boot/FTL 元数据：CRC-32/ISO-HDLC，通过输入/输出反相 + 最终 XOR 计算。
 *   两者不是同一变体，不能直接互换。
 *
 * PLATFORM_CRC_HW_SUPPORTED != 0 且硬件初始化成功时 ISO-HDLC 走硬件；
 * 否则走本文件的软件 slice-by-4。FC 的 MPEG-2 软件实现在 src/fc/fc_crc_engine.c。
 */

#include "crc.h"

#include "assertion.h"

#include <stddef.h>

/* CRC-32/ISO-HDLC 软件实现（boot / FTL 元数据共用）。 */
#define CRC_ISO_TABLE_SIZE (256U)
#define CRC_ISO_SLICES     (4U)
#define CRC_ISO_POLY_REFL  (0xEDB88320UL)
#define CRC_ISO_INIT       (0xFFFFFFFFUL)

static DWORD s_crc_iso_table[CRC_ISO_SLICES][CRC_ISO_TABLE_SIZE];
static BOOL  s_crc_iso_ready = false;

#if (PLATFORM_CRC_HW_SUPPORTED != 0U)
static BOOL s_hw_ready = false;
#endif

static void crc_iso_table_init(void)
{
    for (DWORD i = 0U; i < CRC_ISO_TABLE_SIZE; i++)
    {
        DWORD value = i;

        for (DWORD bit = 0U; bit < 8U; bit++)
        {
            value = ((value & 1U) != 0U) ? ((value >> 1U) ^ CRC_ISO_POLY_REFL) : (value >> 1U);
        }
        s_crc_iso_table[0][i] = value;
    }

    for (DWORD slice = 1U; slice < CRC_ISO_SLICES; slice++)
    {
        for (DWORD i = 0U; i < CRC_ISO_TABLE_SIZE; i++)
        {
            const DWORD previous = s_crc_iso_table[slice - 1U][i];

            s_crc_iso_table[slice][i] = (previous >> 8U) ^ s_crc_iso_table[0][previous & 0xFFU];
        }
    }
    s_crc_iso_ready = true;
}

static DWORD crc_iso_sw_compute(const BYTE *bytes, size_t size)
{
    DWORD  crc   = CRC_ISO_INIT;
    size_t index = 0U;

    if (s_crc_iso_ready == false)
    {
        crc_iso_table_init();
    }

    while ((size - index) >= 4U)
    {
        const DWORD word = (DWORD)bytes[index] |
                           ((DWORD)bytes[index + 1U] << 8U) |
                           ((DWORD)bytes[index + 2U] << 16U) |
                           ((DWORD)bytes[index + 3U] << 24U);

        crc ^= word;
        crc = s_crc_iso_table[3][crc & 0xFFU] ^
              s_crc_iso_table[2][(crc >> 8U) & 0xFFU] ^
              s_crc_iso_table[1][(crc >> 16U) & 0xFFU] ^
              s_crc_iso_table[0][(crc >> 24U) & 0xFFU];
        index += 4U;
    }

    while (index < size)
    {
        crc = (crc >> 8U) ^ s_crc_iso_table[0][(crc ^ bytes[index]) & 0xFFU];
        index++;
    }
    return ~crc;
}

/**
 * @brief 初始化 CRC 后端。
 * @note 硬件后端做时钟/外设初始化与 IDR 应答自检，并跑 ISO-HDLC KAT；
 *       无硬件时只跑软件 ISO-HDLC KAT，软件表在首次计算时惰性建立。
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
    s_hw_ready = true;
#else
    /* 无硬件后端；ISO-HDLC 由本文件的软件表处理，MPEG-2 由 FC 软件表处理。 */
#endif

#ifndef NDEBUG
    {
        static const BYTE kat[] = "123456789";
        const DWORD       sw    = crc_iso_sw_compute((const BYTE *)kat, sizeof(kat) - 1U);

        ASSERT(sw == 0xCBF43926UL);
#    if (PLATFORM_CRC_HW_SUPPORTED != 0U)
        {
            const DWORD hw = crc_port_compute_iso_hdlc((const BYTE *)kat, (DWORD)(sizeof(kat) - 1U));

            ASSERT(hw == sw);
        }
#    endif
    }
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
DWORD crc_compute_iso_hdlc(const void *data, DWORD length_bytes)
{
    if ((data == NULL) || (length_bytes == 0U))
    {
        return 0U;
    }

#if (PLATFORM_CRC_HW_SUPPORTED != 0U)
    if (s_hw_ready == true)
    {
        return crc_port_compute_iso_hdlc((const BYTE *)data, length_bytes);
    }
#endif
    return crc_iso_sw_compute((const BYTE *)data, length_bytes);
}
