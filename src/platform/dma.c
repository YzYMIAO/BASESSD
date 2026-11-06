/*
 * dma.c — 跨 target 共享的 DMA 服务层
 *
 * 本层只提供 UART TX DMA 的稳定入口，把请求转发给 port 侧
 * （src/platform/<soc>/dma_port.c）的 HAL/寄存器实现；NAND/FMC 数据搬移的
 * 声明与空实现仍在 include/platform/dma.h（Stage 3 下沉到 <soc>/）。
 */

#include "dma.h"

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

func_status_e uart_tx_dma_init(void)
{
    return dma_port_uart_tx_init();
}

void uart_tx_dma_irq_handler(void)
{
    dma_port_uart_tx_irq_handler();
}
