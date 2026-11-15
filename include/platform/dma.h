#ifndef PLATFORM_DMA_H
#define PLATFORM_DMA_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * 平台 DMA 抽象接口。
 *
 * 实现在 src/platform/<soc>/：dma.c（UART1 TX）。
 * 本头零 HAL 类型：函数签名只含裸类型，供平台内部（usart.c）以裸名
 * #include "dma.h" 消费。
 *
 * NAND/FMC 数据搬移不走 DMA：FMC NAND 没有专用 DMA 请求线，当前数据面固定
 * 为 PIO（CPU 逐字节读写 FMC 数据窗口）。
 */

/* ── UART TX DMA（USART1；平台内部使用）── */

func_status_e uart_tx_dma_init(void);
void          uart_tx_dma_irq_handler(void);

/* port 接口：由 src/platform/<soc>/dma_port.c 实现；共享 src/platform/dma.c 只转发。 */

/**
 * @brief 初始化 USART1 TX DMA（DMA1_Stream0）
 *
 * 幂等：已成功初始化时直接返回成功，不重配在用的流。运行期重入会中止在飞
 * 传输，控制台 TX 状态机因等不到完成中断而永久阻塞。
 *
 * @return FUNC_STATUS_SUCCESS 已就绪（本次初始化成功或此前已成功）；
 *         FUNC_STATUS_ERROR HAL_DMA_DeInit/Init 失败。
 */
func_status_e dma_port_uart_tx_init(void);
void          dma_port_uart_tx_irq_handler(void);

#endif /* PLATFORM_DMA_H */
