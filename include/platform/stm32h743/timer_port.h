#ifndef PLATFORM_STM32H743_TIMER_PORT_H
#define PLATFORM_STM32H743_TIMER_PORT_H

#include "stm32h7xx_hal.h"

/*
 * 调度器毫秒心跳的硬件来源。抽象头 include/platform/timer.h 里没有这些宏 ——
 * 它们只服务于本目录下的 timer_port.c。
 *
 * With APB1=100 MHz and APB1 prescaler=2, TIM6 runs at 200 MHz. For a 1 ms
 * tick use arr=1000-1 and psc=200-1.
 */
#define TIMER_TIMX      TIM6
#define TIMER_TIMX_IRQn TIM6_DAC_IRQn
#define TIMER_TIMX_CLK_ENABLE()      \
    do                               \
    {                                \
        __HAL_RCC_TIM6_CLK_ENABLE(); \
    } while (0)

#endif /* PLATFORM_STM32H743_TIMER_PORT_H */
