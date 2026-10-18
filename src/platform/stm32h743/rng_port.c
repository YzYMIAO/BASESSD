/*
 * rng_port.c — stm32h743 RNG 适配层
 *
 * 只做硬件相关的事：RNG 句柄、时钟源（PLL1_Q）、HAL 初始化、就绪标志、取数。
 * 重试策略与范围映射在共享 src/platform/rng.c。
 */

#include "common.h"
#include "rng.h"
#include "sys_port.h"

RNG_HandleTypeDef g_rng_handle; /* RNG句柄 */

void rng_port_init(void)
{
    g_rng_handle.Instance = RNG;
    HAL_RNG_DeInit(&g_rng_handle);
    HAL_RNG_Init(&g_rng_handle); /* 初始化RNG */
}

uint8_t rng_port_data_ready(void)
{
    return (__HAL_RNG_GET_FLAG(&g_rng_handle, RNG_FLAG_DRDY) != RESET) ? 1U : 0U;
}

uint32_t rng_port_get_random(void)
{
    uint32_t randomnum;

    HAL_RNG_GenerateRandomNumber(&g_rng_handle, &randomnum);

    return randomnum;
}

/**
 * @brief       RNG底层驱动，时钟源设置和使能
 * @note        此函数会被HAL_RNG_Init()调用
 * @param       hrng  : RNG句柄
 * @retval      无
 */
void HAL_RNG_MspInit(RNG_HandleTypeDef *hrng)
{
    UNUSED(hrng);
    RCC_PeriphCLKInitTypeDef rcc_periph_clk_init;

    /* RNG 时钟源选择 PLL1_Q，最终 RNG kernel clock=200 MHz。 */
    rcc_periph_clk_init.PeriphClockSelection = RCC_PERIPHCLK_RNG;    /* 设置RNG时钟源 */
    rcc_periph_clk_init.RngClockSelection    = RCC_RNGCLKSOURCE_PLL; /* RNG时钟源选择PLL */
    HAL_RCCEx_PeriphCLKConfig(&rcc_periph_clk_init);

    __HAL_RCC_RNG_CLK_ENABLE(); /* 使能RNG时钟 */
}
