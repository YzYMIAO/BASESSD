#ifndef PLATFORM_STM32H743_DMA_PORT_H
#define PLATFORM_STM32H743_DMA_PORT_H

/*
 * stm32h743 DMA 端口头：只暴露跨文件可见的 HAL 句柄。
 * 抽象接口在 include/platform/dma.h；共享逻辑在 src/platform/dma.c。
 */

#include "sys_port.h" /* DMA_HandleTypeDef */

/* USART1 TX DMA 句柄；定义在 dma_port.c，usart_port.c 的 __HAL_LINKDMA 亦引用。 */
extern DMA_HandleTypeDef g_dma_handle;

#endif /* PLATFORM_STM32H743_DMA_PORT_H */
