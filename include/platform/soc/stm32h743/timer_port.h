#ifndef PLATFORM_STM32H743_TIMER_PORT_H
#define PLATFORM_STM32H743_TIMER_PORT_H

#include "stm32h7xx_hal.h"

/*
 * 调度器毫秒心跳的硬件来源。抽象头 include/platform/timer.h 里没有这些宏 ——
 * 它们只服务于本目录下的 timer_port.c。
 *
 * APB1=100 MHz 且 APB1 分频=2 时 TIM6 内核时钟为 200 MHz；1 ms 节拍取
 * arr=1000-1、psc=200-1（板级重装/预分频见 board_config.h 的 BOARD_TIM6_*）。
 */
#define TIMER_TIMX      TIM6
#define TIMER_TIMX_IRQn TIM6_DAC_IRQn
#define TIMER_TIMX_CLK_ENABLE()      \
    do                               \
    {                                \
        __HAL_RCC_TIM6_CLK_ENABLE(); \
    } while (0)

#endif /* PLATFORM_STM32H743_TIMER_PORT_H */
