

#ifndef __DMA_H
#define __DMA_H

#include "sys.h"

#define USART_TX_DMA_STREAM DMA1_Stream0
#define USART_TX_DMA_IRQn   DMA1_Stream0_IRQn

extern DMA_HandleTypeDef g_dma_handle;

uint8_t uart_tx_dma_init(void);
void    uart_tx_dma_irq_handler(void);

#endif
