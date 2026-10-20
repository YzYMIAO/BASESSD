/*
 * delay_port.c — 延时服务的 stm32h743 适配层
 *
 * 共享逻辑在 src/platform/delay.c（跨 target 编译一次，零 CMSIS 零 HAL）。
 * 本文件只提供该文件无法自行获得的 SoC 事实：时钟频率、系统节拍，以及
 * DWT / SysTick 计数器寄存器。
 *
 * 为什么是两个时钟而不是一个：CYCCNT 跟随内核时钟（SYSCLK），而 delay_init(0)
 * 的「读 HCLK」哨兵要的是 HCLK —— 两者在 H7 上并不相等。
 *
 * 为什么不把 core_cm7.h 交给共享层直接用：该头需要设备头提供 IRQn_Type 与
 * __NVIC_PRIO_BITS，裸包含编译不过，所以寄存器访问留在本文件。
 */

#include "delay.h"
#include "sys_port.h"

DWORD delay_port_hclk_hz(void)
{
    return HAL_RCC_GetHCLKFreq();
}

DWORD delay_port_core_clock_hz(void)
{
    return HAL_RCC_GetSysClockFreq();
}

DWORD delay_port_now_ms(void)
{
    return HAL_GetTick();
}

/*
 * 启用 Cortex-M7 的 DWT 周期计数器。
 *
 * 原理：先打开 CoreDebug->DEMCR 的 TRCENA 位，再打开 DWT->CTRL 的 CYCCNTENA 位。
 * 不关中断、不改 SysTick 配置。返回 true 表示硬件接受了使能位。
 */
bool delay_port_dwt_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    return (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U;
}

DWORD delay_port_dwt_cycles(void)
{
    return DWT->CYCCNT;
}

DWORD delay_port_systick_load(void)
{
    return SysTick->LOAD;
}

DWORD delay_port_systick_value(void)
{
    return SysTick->VAL;
}
