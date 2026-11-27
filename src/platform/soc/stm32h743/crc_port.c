/*
 * crc_port.c — stm32h743 CRC-32 外设适配层
 *
 * 只做硬件相关的事：唯一持有 CRC 句柄、开时钟（MSP）、HAL 配置、IDR 读写、
 * 按字喂入计算。自检策略与对齐契约在共享 src/platform/crc.c。
 *
 * implements crc.h（端口适配：本文件实现抽象头的 port 接口）。
 */

#include "assertion.h"
#include "crc.h"
#include "sys_port.h" /* CRC_HandleTypeDef、UNUSED、__HAL_CRC_*、__HAL_RCC_CRC_CLK_ENABLE */

/* 8 路展开：每轮少 7 次循环判断；CRC_DR 写仍按 32-bit 顺序串行。 */
#define CRC_PORT_UNROLL (8U)

static CRC_HandleTypeDef s_crc_handle;

/*
 * HAL_CRC_MspInit
 *   HAL 弱回调（CRC 外设的底层硬件初始化）。仅 HAL_CRC_Init() 首次调用时触发
 *   —— s_crc_handle 是 static ⇒ 零初始化 ⇒ State == HAL_CRC_STATE_RESET。
 *   职责只有一件：开 CRC 外设时钟（AHB4 bit19）。
 */
void HAL_CRC_MspInit(CRC_HandleTypeDef *hcrc)
{
    UNUSED(hcrc);

    __HAL_RCC_CRC_CLK_ENABLE();
}

void crc_port_init(void)
{
    HAL_StatusTypeDef status;

    s_crc_handle.Instance                     = CRC;                       /* CMSIS 设备头定义的指针，非裸地址 */
    s_crc_handle.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_ENABLE; /* 0x04C11DB7 */
    s_crc_handle.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_ENABLE; /* 0xFFFFFFFF */
    s_crc_handle.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;
    s_crc_handle.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
    s_crc_handle.InputDataFormat              = CRC_INPUTDATA_FORMAT_WORDS;

    /*
     * ⚠ 必须先求值再断言：ASSERT 在 release(NDEBUG) 下整体裁剪为空，
     *   写成 ASSERT(HAL_CRC_Init(...) == HAL_OK) 会让初始化**根本不执行**。
     */
    status = HAL_CRC_Init(&s_crc_handle);
    ASSERT(status == HAL_OK);
    /* release（NDEBUG）下断言被裁空 ⇒ status 只剩「set but not used」告警
     *（判据 10）。显式丢弃，表明初始化调用本身必须保留。 */
    (void)status;
}

void crc_port_set_idr(DWORD value)
{
    __HAL_CRC_SET_IDR(&s_crc_handle, value);
}

DWORD crc_port_get_idr(void)
{
    return __HAL_CRC_GET_IDR(&s_crc_handle);
}

/**
 * @brief 用硬件 CRC 计算 length_bytes（4 的倍数）的 CRC-32。
 * @param data 4 字节对齐的输入缓冲
 * @param length_bytes 字节数；必须是 4 的倍数
 * @return CRC-32/MPEG-2 值
 * @note 直接写 CRC_DR，避免 HAL 的 InputDataFormat switch、状态字段和
 *       HAL_CRC_Calculate 调用开销；配置仍由 crc_port_init 负责。
 */
DWORD crc_port_compute(const BYTE *data, DWORD length_bytes)
{
    volatile uint32_t *dr    = &s_crc_handle.Instance->DR;
    const uint32_t    *words = (const uint32_t *)(const void *)data;
    DWORD              count = length_bytes / 4U;

    /* 入参由 crc_compute 校验；这里只做硬件动作。
     * 每次从 INIT 值重新起算，和软件后端的初始状态一致。 */
    __HAL_CRC_DR_RESET(&s_crc_handle);

    while (count >= CRC_PORT_UNROLL)
    {
        *dr = words[0];
        *dr = words[1];
        *dr = words[2];
        *dr = words[3];
        *dr = words[4];
        *dr = words[5];
        *dr = words[6];
        *dr = words[7];
        words += CRC_PORT_UNROLL;
        count -= CRC_PORT_UNROLL;
    }
    while (count != 0U)
    {
        *dr = *words++;
        count--;
    }

    return *dr;
}

/**
 * @brief 用硬件 CRC 计算 CRC-32/ISO-HDLC。
 * @param data 输入数据（不要求 4 字节对齐）
 * @param length_bytes 字节数
 * @return CRC-32/ISO-HDLC 值
 * @note 临时把 CRC 外设切到 ISO-HDLC 配置（字节输入、输入字节反相、
 *       输出反相），算完恢复 FC 的 MPEG-2 配置；调用者保证串行访问。
 */
DWORD crc_port_compute_iso_hdlc(const BYTE *data, DWORD length_bytes)
{
    CRC_HandleTypeDef    *hcrc         = &s_crc_handle;
    const CRC_InitTypeDef saved_init   = hcrc->Init;
    const uint32_t        saved_format = hcrc->InputDataFormat;
    HAL_StatusTypeDef     status;
    DWORD                 crc;

    /* 入参由 crc_compute_iso_hdlc 校验；这里只做硬件动作。 */
    hcrc->Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_BYTE;
    hcrc->Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_ENABLE;
    hcrc->InputDataFormat              = CRC_INPUTDATA_FORMAT_BYTES;

    status = HAL_CRC_Init(hcrc);
    ASSERT(status == HAL_OK);
    (void)status;

    crc = HAL_CRC_Calculate(hcrc, (const uint32_t *)(const void *)data, length_bytes);

    hcrc->Init            = saved_init;
    hcrc->InputDataFormat = saved_format;
    status                = HAL_CRC_Init(hcrc);
    ASSERT(status == HAL_OK);
    (void)status;

    return crc ^ 0xFFFFFFFFUL;
}
