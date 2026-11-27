#ifndef PLATFORM_BOARDS_APOLLO_V2_BOARD_CONFIG_H
#define PLATFORM_BOARDS_APOLLO_V2_BOARD_CONFIG_H

/*
 * Apollo V2（STM32H743 核心板）板级常量。
 *
 * 只放「这块板」决定的量：调试串口使用的实例与引脚、控制台波特率、心跳定时器
 * 与看门狗参数。SoC 端口（src/platform/soc/stm32h743/）与共享服务通过本头取得
 * 板级差异；换板只替换本文件，不碰 SoC 端口与共享逻辑。
 *
 * HSE 晶振频率不在此定义：HAL 通过 stm32h7xx_hal_conf.h 的 HSE_VALUE（25 MHz）
 * 使用它，另立第二套会漂移。时钟树推导见 sys_port.c 的 sys_port_clock_init()。
 */

#include "stm32h7xx_hal.h"

/* 调试串口：USART1。PA9 是 MCU 发出的 TX，PA10 是 MCU 接收的 RX；
 * 外部模块的 RX 接 PA9，TX 接 PA10，双方必须共地。 */
#define USART_UX      USART1
#define USART_UX_IRQn USART1_IRQn

#define USART_TX_GPIO_PORT GPIOA
#define USART_TX_GPIO_PIN  GPIO_PIN_9
#define USART_TX_GPIO_AF   GPIO_AF7_USART1
#define USART_RX_GPIO_PORT GPIOA
#define USART_RX_GPIO_PIN  GPIO_PIN_10
#define USART_RX_GPIO_AF   GPIO_AF7_USART1

#define USART_TX_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOA_CLK_ENABLE(); \
    } while (0)
#define USART_RX_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOA_CLK_ENABLE(); \
    } while (0)
#define USART_UX_CLK_ENABLE()          \
    do                                 \
    {                                  \
        __HAL_RCC_USART1_CLK_ENABLE(); \
    } while (0)

/* 控制台波特率（sys_init 调 usart_init 使用）。 */
#define BOARD_CONSOLE_BAUD 115200U

/*
 * TIM6 是 timer 调度器的 1 ms 心跳：APB1=100 MHz 且 APB1 分频=2 ⇒ 内核时钟
 * 200 MHz，arr=1000-1、psc=200-1。系统毫秒时基由 SysTick/HAL tick 提供。
 */
#define BOARD_TIM6_ARR (1000U - 1U)
#define BOARD_TIM6_PSC (200U - 1U)

/*
 * IWDG：/256 与 Reload=3749，按典型 LSI=32 kHz 名义超时约 30 s：
 * T=(3749+1)*256/32000。LSI 有频偏，量产前应按数据手册频率范围验证
 * 最短/最长复位时间，不得当作精确的 30 s 定时器。
 */
#define BOARD_IWDG_PRESCALER IWDG_PRESCALER_256
#define BOARD_IWDG_RELOAD    3749U

#endif /* PLATFORM_BOARDS_APOLLO_V2_BOARD_CONFIG_H */
