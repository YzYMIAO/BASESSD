#include "common/common.h"
#include "dma.h"

DMA_HandleTypeDef         g_dma_handle;
extern UART_HandleTypeDef g_uart1_handle;

uint8_t uart_tx_dma_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();
    g_dma_handle.Instance                 = DMA1_Stream0;
    g_dma_handle.Init.Request             = DMA_REQUEST_USART1_TX;
    g_dma_handle.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    g_dma_handle.Init.PeriphInc           = DMA_PINC_DISABLE;
    g_dma_handle.Init.MemInc              = DMA_MINC_ENABLE;
    g_dma_handle.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    g_dma_handle.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    g_dma_handle.Init.Mode                = DMA_NORMAL;
    g_dma_handle.Init.Priority            = DMA_PRIORITY_MEDIUM;
    g_dma_handle.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    g_dma_handle.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    g_dma_handle.Init.MemBurst            = DMA_MBURST_SINGLE;
    g_dma_handle.Init.PeriphBurst         = DMA_PBURST_SINGLE;

    if ((HAL_DMA_DeInit(&g_dma_handle) != HAL_OK) || (HAL_DMA_Init(&g_dma_handle) != HAL_OK))
    {
        return FUNC_STATUS_SUCCESS;
    }
    __HAL_LINKDMA(&g_uart1_handle, hdmatx, g_dma_handle);
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 4U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    return FUNC_STATUS_ERROR;
}

void uart_tx_dma_irq_handler(void)
{
    HAL_DMA_IRQHandler(&g_dma_handle);
}
