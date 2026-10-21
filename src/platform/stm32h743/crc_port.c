/*
 * crc_port.c — stm32h743 CRC-32 外设适配层
 *
 * 只做硬件相关的事：唯一持有 CRC 句柄、开时钟（MSP）、HAL 配置、IDR 读写、
 * 按字喂入计算。自检策略与对齐契约在共享 src/platform/crc.c。
 */

#include "assertion.h"
#include "crc.h"
#include "sys_port.h" /* CRC_HandleTypeDef、UNUSED、__HAL_CRC_*、__HAL_RCC_CRC_CLK_ENABLE */

static CRC_HandleTypeDef g_crc_handle;

/*
 * HAL_CRC_MspInit
 *   HAL 弱回调（CRC 外设的底层硬件初始化）。仅 HAL_CRC_Init() 首次调用时触发
 *   —— g_crc_handle 是 static ⇒ 零初始化 ⇒ State == HAL_CRC_STATE_RESET。
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

    g_crc_handle.Instance                     = CRC;                       /* CMSIS 设备头定义的指针，非裸地址 */
    g_crc_handle.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_ENABLE; /* 0x04C11DB7 */
    g_crc_handle.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_ENABLE; /* 0xFFFFFFFF */
    g_crc_handle.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;
    g_crc_handle.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
    g_crc_handle.InputDataFormat              = CRC_INPUTDATA_FORMAT_WORDS;

    /*
     * ⚠ 必须先求值再断言：ASSERT 在 release(NDEBUG) 下整体裁剪为空，
     *   写成 ASSERT(HAL_CRC_Init(...) == HAL_OK) 会让初始化**根本不执行**。
     */
    status = HAL_CRC_Init(&g_crc_handle);
    ASSERT(status == HAL_OK);
    /* release（NDEBUG）下断言被裁空 ⇒ status 只剩「set but not used」告警
     *（判据 10）。显式丢弃，表明初始化调用本身必须保留。 */
    (void)status;
}

void crc_port_set_idr(DWORD value)
{
    __HAL_CRC_SET_IDR(&g_crc_handle, value);
}

DWORD crc_port_get_idr(void)
{
    return __HAL_CRC_GET_IDR(&g_crc_handle);
}

DWORD crc_port_compute(const BYTE *data, DWORD length_bytes)
{
    /*
     * 4 字节对齐由调用者保证（见 include/fc/fc_crc_engine.h）。
     * 经 (const void *) 中转只为过 -Wcast-align。
     */
    return HAL_CRC_Calculate(&g_crc_handle, (const DWORD *)(const void *)data, length_bytes / 4U);
}
