#include "fc_crc_engine.h"

#include "assertion.h"
#include "crc.h"
#if defined(FC_NAND_TEST_ENABLE)
#    include "fault_inject.h"
#endif

/* ══════════════════════════════════════════════════════════
 * CRC 引擎 — FC CW 数据面
 *
 * 本文件只保留 CRC-32/MPEG-2；启动/FTL 元数据用的 ISO-HDLC
 * 已下沉到 src/platform/crc.c。硬件可用时优先走平台硬件，
 * 否则走本文件的 MPEG-2 软件查表实现；两条路径必须 bit-exact。
 * ══════════════════════════════════════════════════════════ */

#define FC_CRC_MPEG2_TABLE_SIZE (256U)
#define FC_CRC_MPEG2_POLY       (0x04C11DB7UL)
static DWORD s_crc_mpeg2_table[FC_CRC_MPEG2_TABLE_SIZE];
static BOOL  s_crc_mpeg2_ready = false;

#if (PLATFORM_CRC_HW_SUPPORTED != 0U)
static BOOL s_hw_ready = false;
#endif

/* ── CRC-32/MPEG-2 软件实现 ───────────────────────────────────────── */

/**
 * @brief 建立 MPEG-2 非反射 CRC 表。
 * @note 每个表项是「一个字节从高位移入」后的 8 bit 余式；表只建一次。
 */
static void fc_crc_mpeg2_table_init(void)
{
    for (DWORD i = 0U; i < FC_CRC_MPEG2_TABLE_SIZE; i++)
    {
        DWORD value = i << 24U;

        for (DWORD bit = 0U; bit < 8U; bit++)
        {
            value = ((value & 0x80000000UL) != 0U) ? ((value << 1U) ^ FC_CRC_MPEG2_POLY) : (value << 1U);
        }
        s_crc_mpeg2_table[i] = value;
    }
    s_crc_mpeg2_ready = true;
}

/**
 * @brief 软件计算 CRC-32/MPEG-2（按 STM32 硬件 word 的 MSB-first 顺序）。
 * @param data 输入数据
 * @param length_bytes 字节数；必须是 4 的倍数
 * @return CRC-32 值
 * @note STM32 按 32-bit word 喂入时先处理 word 的最高字节，故每个 4B 组按
 *       data[3]、data[2]、data[1]、data[0] 的顺序送入 CRC。
 */
static DWORD fc_crc_mpeg2_sw_compute(const BYTE *data, DWORD length_bytes)
{
    DWORD crc = 0xFFFFFFFFUL;

    // 入参由 fc_crc_compute / KAT 保证：非空且长度是 4 的倍数。
    if (s_crc_mpeg2_ready == false)
    {
        fc_crc_mpeg2_table_init();
    }

    for (DWORD i = 0U; i < length_bytes; i += 4U)
    {
        crc ^= (DWORD)data[i + 3U] << 24U;
        crc = (crc << 8U) ^ s_crc_mpeg2_table[(crc >> 24U) & 0xFFU];

        crc ^= (DWORD)data[i + 2U] << 24U;
        crc = (crc << 8U) ^ s_crc_mpeg2_table[(crc >> 24U) & 0xFFU];

        crc ^= (DWORD)data[i + 1U] << 24U;
        crc = (crc << 8U) ^ s_crc_mpeg2_table[(crc >> 24U) & 0xFFU];

        crc ^= (DWORD)data[i] << 24U;
        crc = (crc << 8U) ^ s_crc_mpeg2_table[(crc >> 24U) & 0xFFU];
    }
    return crc;
}

/* ── 公共接口 ─────────────────────────────────────────────────────── */

/**
 * @brief   初始化 CRC 后端（硬件优先，软件 fallback）。
 * @note    在 fc_init() 中调用一次。硬件后端的时钟/外设自检在平台层；
 *          软件后端只建表。自检不过一律 ASSERT。
 */
void fc_crc_init(void)
{
    /* 平台 CRC 服务负责硬件初始化与 ISO-HDLC KAT；软件构建也跑 KAT。 */
    crc_init();
#if (PLATFORM_CRC_HW_SUPPORTED != 0U)
    s_hw_ready = true;
#endif

#ifndef NDEBUG
    {
        static const BYTE kat4[4] = {0x41U, 0x42U, 0x43U, 0x44U};
        const DWORD       sw4     = fc_crc_mpeg2_sw_compute(kat4, (DWORD)sizeof(kat4));

        ASSERT(sw4 == 0xCF534AE1UL);
#    if (PLATFORM_CRC_HW_SUPPORTED != 0U)
        {
            static const BYTE kat32[32] = {0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU};
            const DWORD       sw32      = fc_crc_mpeg2_sw_compute(kat32, (DWORD)sizeof(kat32));
            const DWORD       hw4       = crc_compute(kat4, (DWORD)sizeof(kat4));
            const DWORD       hw32      = crc_compute(kat32, (DWORD)sizeof(kat32));

            ASSERT(hw4 == sw4);
            ASSERT(hw32 == sw32);
        }
#    endif
    }
#endif
}

/**
 * @brief   对 USER 数据（512 字节）计算 CRC-32
 * @param   user_data  指向 USER 数据的指针（buffer + 8）
 * @return  CRC-32 值
 * @note    user_data **必须 4 字节对齐**——硬性前置条件：硬件按 32 位字喂入，
 *          HAL 直接按 DWORD* 解引用。违反属未对齐访问。
 */
DWORD fc_crc_compute(const BYTE *user_data)
{
    ASSERT(user_data != NULL);

#if defined(FC_NAND_TEST_ENABLE)
    if (fi_take(FI_CRC_COMPUTE_FAIL) == true)
    {
        return 0U;
    }
#endif

#if (PLATFORM_CRC_HW_SUPPORTED != 0U)
    if (s_hw_ready == true)
    {
        return crc_compute(user_data, FC_CRC_USER_SIZE);
    }
#endif
    return fc_crc_mpeg2_sw_compute(user_data, FC_CRC_USER_SIZE);
}

/**
 * @brief   校验 CRC：重算 USER 数据的 CRC 并与存储值比较
 * @param   user_data    指向 USER 数据的指针（buffer + 8），必须 4 字节对齐
 * @param   stored_crc   之前存储的 CRC 值（buffer[4..8] 读取）
 * @return  FC_CRC_RESULT_OK 或 FC_CRC_RESULT_ERROR
 */
BYTE fc_crc_verify(const BYTE *user_data, DWORD stored_crc)
{
    // user_data 的非空检查由 fc_crc_compute 的入口断言负责。
    return (fc_crc_compute(user_data) == stored_crc) ? FC_CRC_RESULT_OK : FC_CRC_RESULT_ERROR;
}
