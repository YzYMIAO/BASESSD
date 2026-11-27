#ifndef PLATFORM_STM32H743_USART_PORT_H
#define PLATFORM_STM32H743_USART_PORT_H

/*
 * stm32h743 调试串口 port 头：只放 SoC 相关的句柄与端口原型。
 * 板级接线（实例、引脚、时钟使能）在 board_config.h；
 * 传输/CLI 的抽象接口在 include/platform/usart.h 与 include/hil/cli.h。
 */

#include "board_config.h" /* USART_UX 等板级接线 */
#include "sys_port.h"     /* HAL 类型：UART_HandleTypeDef */

/* HAL 串口句柄；定义在 usart_port.c，dma_port.c 的 __HAL_LINKDMA 亦引用。 */
extern UART_HandleTypeDef g_uart1_handle;

#endif /* PLATFORM_STM32H743_USART_PORT_H */
