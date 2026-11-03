/*
 * delay_port.c — 延时服务的 stm32h743 适配层
 *
 * 共享逻辑在 src/platform/delay.c（跨 target 编译一次，零 CMSIS 零 HAL）。
 * 本文件只提供该文件无法自行获得的 SoC 事实：时钟频率、系统节拍，以及
 * SysTick 计数器寄存器。
 *
 * 只使用 ARM 核心自带的 SysTick：HAL 已把它配置为 1 ms 周期，VAL/LOAD
 * 提供 1 ms 内的周期分辨率。DWT 是调试/跟踪单元，不作为产品时间源。
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
    /* 系统毫秒计数 = ARM 核心 SysTick 的 HAL tick（HAL_Init/HAL_RCC 配置）。 */
    return HAL_GetTick();
}

DWORD delay_port_systick_load(void)
{
    return SysTick->LOAD;
}

DWORD delay_port_systick_value(void)
{
    return SysTick->VAL;
}
