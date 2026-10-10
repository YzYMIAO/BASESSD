/*
 * crc.c — STM32H743 硬件 CRC 外设封装（HAL 驱动）
 *
 * 平台层职责：唯一持有 CRC_HandleTypeDef，负责时钟使能（MSP）与外设应答自检。
 * 上层 src/fc/fc_crc_engine.c 只调 crc_init() / crc_compute()，不接触任何 HAL 类型。
 *
 * 配置：IEEE 802.3 CRC-32 —— 默认多项式 0x04C11DB7、INIT 0xFFFFFFFF、
 *       输入/输出均不反相、按 32 位字喂入。
 */

#include "crc.h"

#include "common/assertion.h"
#include "sys.h" /* CRC_HandleTypeDef、UNUSED、__HAL_CRC_*、__HAL_RCC_CRC_CLK_ENABLE */

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

/*
 * crc_init
 *   配置并初始化 CRC 外设，随后做一次写读回环自检。
 *   自检不过一律 ASSERT —— 外设不应答属于配置错误，不允许静默降级。
 */
void crc_init(void)
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

    /*
     * IDR 写读回环：唯一能证明外设真的应答的检查 —— 时钟没开时写被静默丢弃、
     * 读回恒 0，而 HAL_CRC_Init 自身不校验。此前 FC_CRC_BASE 写成了 ADC3 的地址，
     * 正是被这一环抓到、又被「失败静默继续」咽掉的。
     */
    __HAL_CRC_SET_IDR(&g_crc_handle, 0xA5U);
    ASSERT(__HAL_CRC_GET_IDR(&g_crc_handle) == 0xA5U);
}

/*
 * crc_compute
 *   对 data 起 length_bytes 字节算 CRC-32，每次从 INIT 值重新起算。
 *   前置：data 非空且 4 字节对齐、length_bytes 为 4 的倍数。
 */
uint32_t crc_compute(const uint8_t *data, uint32_t length_bytes)
{
    ASSERT(data != NULL);
    ASSERT((length_bytes % 4U) == 0U); /* WORDS 格式：长度必须是 4 的倍数 */

    /*
     * 4 字节对齐由调用者保证（见 include/fc/fc_crc_engine.h）。
     * 经 (const void *) 中转只为过 -Wcast-align。
     */
    return HAL_CRC_Calculate(&g_crc_handle, (const uint32_t *)(const void *)data, length_bytes / 4U);
}
