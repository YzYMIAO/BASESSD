#ifndef PLATFORM_STM32H743_USART_PORT_H
#define PLATFORM_STM32H743_USART_PORT_H

/*
 * stm32h743 调试串口 port 头：只放 SoC 相关的宏与句柄。
 * 传输/CLI 的抽象接口在 include/platform/usart.h 与 include/hil/cli.h。
 */

#include "sys_port.h" /* HAL 类型：UART_HandleTypeDef、GPIOA、__HAL_RCC_* */

/*
 * 板级调试串口：USART1。
 * 阿波罗 V2 的 USART1：PA9 是 MCU 发出的 TX，PA10 是 MCU 接收的 RX。
 * 外部模块的 RX 应接 PA9，外部模块的 TX 应接 PA10，并且双方必须共地。
 */
#define USART_TX_GPIO_PORT GPIOA
#define USART_TX_GPIO_PIN  GPIO_PIN_9
#define USART_TX_GPIO_AF   GPIO_AF7_USART1
#define USART_TX_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOA_CLK_ENABLE(); \
    } while (0)
#define USART_RX_GPIO_PORT GPIOA
#define USART_RX_GPIO_PIN  GPIO_PIN_10
#define USART_RX_GPIO_AF   GPIO_AF7_USART1
#define USART_RX_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOA_CLK_ENABLE(); \
    } while (0)
#define USART_UX      USART1
#define USART_UX_IRQn USART1_IRQn
#define USART_UX_CLK_ENABLE()          \
    do                                 \
    {                                  \
        __HAL_RCC_USART1_CLK_ENABLE(); \
    } while (0)

/* HAL 串口句柄；定义在 usart_port.c，dma.c 的 __HAL_LINKDMA 亦引用。 */
extern UART_HandleTypeDef g_uart1_handle;

#endif /* PLATFORM_STM32H743_USART_PORT_H */
