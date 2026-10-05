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
 *   STM32H743 CRC 外设基地址 0x58026000，此处不使用 HAL 以保持模块独立性。
 *   各寄存器偏移详见 STM32H7x3 参考手册第 20 章。
 */

#include "fc_crc_engine.h"

#include "common/assertion.h"
#include "common/common.h"

#include <stddef.h>
#include <string.h>

/* ── STM32H743 CRC 寄存器偏移（不使用 HAL/CMSIS 以保持模块独立） ── */

/* CRC 外设基地址 */
#define FC_CRC_BASE (0x58026000UL)

/* CRC 寄存器偏移 */
#define FC_CRC_DR_OFFSET   (0x00UL) /* Data Register      — 写输入数据，读 CRC 结果 */
#define FC_CRC_IDR_OFFSET  (0x04UL) /* Independent Data Register */
#define FC_CRC_CR_OFFSET   (0x08UL) /* Control Register   — 包含 RESET 位 */
#define FC_CRC_INIT_OFFSET (0x10UL) /* Initial Value Register */
#define FC_CRC_POL_OFFSET  (0x14UL) /* Polynomial Register */

/* CRC 控制寄存器 (CR) 位定义 */
#define FC_CRC_CR_RESET (1UL << 0) /* RESET 位：重置 CRC 计算器至 INIT 值 */

/* 寄存器访问宏 */
#define FC_CRC_REG(offset) (*(volatile uint32_t *)(FC_CRC_BASE + (offset)))

/* ── 全局状态 ── */
static uint8_t g_crc_ready = 0U; /* 非零表示初始化成功 */

/* --------------------------------------------------------------------
 * fc_crc_init
 *   功能：初始化 STM32H743 CRC 硬件外设。
 *   参数：无。
 *   返回：无。
 *   实现要点：
 *     - 直接操作 RCC 寄存器使能 CRC 时钟（AHB4ENR bit19）。
 *     - 默认配置即 IEEE 802.3 CRC-32（多项式 0x04C11DB7、INIT 0xFFFFFFFF、
 *       不反相），无需改动；写 INIT + RESET 确认外设可访问。
 *     - 用 IDR（8 位只读影子寄存器）写读回环验证外设响应，置 g_crc_ready。
 * -------------------------------------------------------------------- */
void fc_crc_init(void)
{
    uint32_t idr;

    /* 使能 CRC 外设时钟（RCC 寄存器直接操作） */
    /* RCC 基地址 0x58024400，AHB4 外设时钟使能寄存器 RCC_AHB4ENR 偏移 0xE0 */
    /* CRCEN 位于 RCC_AHB4ENR 的 bit 19 */
    *(volatile uint32_t *)0x580244E0UL |= (1UL << 19);

    /*
     * 配置 CRC 为默认模式：
     *   - 多项式：0x04C11DB7（默认，IEEE 802.3 CRC-32）
     *   - 初始值：0xFFFFFFFF（默认）
     *   - 输入/输出不反相
     * CRC 外设默认即为上述配置，上电复位后已生效。
     * 此处通过写入 INIT 寄存器确认其可访问性，作为初始化验证。
     */
    FC_CRC_REG(FC_CRC_INIT_OFFSET) = 0xFFFFFFFFUL;
    FC_CRC_REG(FC_CRC_CR_OFFSET)   = FC_CRC_CR_RESET;

    /* 简单验证：写入 IDR（8 位只读寄存器），读回确认外设响应 */
    FC_CRC_REG(FC_CRC_IDR_OFFSET) = 0xA5U;
    idr                           = FC_CRC_REG(FC_CRC_IDR_OFFSET) & 0xFFU;

    g_crc_ready = (idr == 0xA5U) ? 1U : 0U;
}

/* --------------------------------------------------------------------
 * fc_crc_compute
 *   功能：对 512B USER 数据计算 CRC-32（硬件 CRC 外设）。
 *   参数：user_data —— USER 数据指针（buffer+8，可未 4 字节对齐）。
 *   返回：CRC-32 值；参数非法或未初始化返回 0。
 *   实现要点：
 *     - 每次先 RESET 引擎回 INIT 值（保证独立计算）。
 *     - 数据可能未对齐，逐字 memcpy 到临时变量再写 DR 保证对齐访问。
 *     - 32 位写入 DR 即自动喂入 CRC 引擎。
 * -------------------------------------------------------------------- */
uint32_t fc_crc_compute(const uint8_t *user_data)
{
    uint32_t word;
    uint16_t i;

    /* user_data 前置条件由调用者保证；g_crc_ready 是硬件初始化状态（运行时），保留返回码 */
    ASSERT(user_data != NULL);
    if (g_crc_ready == 0U)
    {
        return 0U;
    }

    /* 重置 CRC 引擎至 INIT 值 (0xFFFFFFFF) */
    FC_CRC_REG(FC_CRC_CR_OFFSET) = FC_CRC_CR_RESET;

    /*
     * user_data 可能未对齐到 4 字节边界（FTL 通过 buffer_id + 8 偏移获取），
     * 此处逐字 memcpy 到临时变量后写入 CRC->DR，保证对齐访问。
     * CRC_DR 地址 0x58026000，写入 32 位数据即自动计算 CRC。
     */
    for (i = 0U; i < (FC_CRC_USER_SIZE / 4U); i++)
    {
        (void)memcpy(&word, user_data + i * 4U, sizeof(word));
        FC_CRC_REG(FC_CRC_DR_OFFSET) = word;
    }

    /* 读出 CRC 计算结果 */
    return FC_CRC_REG(FC_CRC_DR_OFFSET);
}

/* --------------------------------------------------------------------
 * fc_crc_verify
 *   功能：校验 USER 数据的 CRC 与存储值是否一致。
 *   参数：user_data —— USER 指针；stored_crc —— 存储的 CRC 值。
 *   返回：FC_CRC_RESULT_OK / FC_CRC_RESULT_ERROR。
 * -------------------------------------------------------------------- */
uint8_t fc_crc_verify(const uint8_t *user_data, uint32_t stored_crc)
{
    /* user_data 前置条件由调用者保证 */
    ASSERT(user_data != NULL);

    return (fc_crc_compute(user_data) == stored_crc) ? FC_CRC_RESULT_OK : FC_CRC_RESULT_ERROR;
}