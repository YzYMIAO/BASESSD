/*
 * dma_port.c — stm32h743 DMA 适配层（USART1 TX）
 *
 * 只做硬件相关的事：DMA1_Stream0 句柄、HAL 配置、NVIC、IRQ 转发。
 * 共享 src/platform/dma.c 只转发 dma_usart_tx_* 到本文件。
 */

#include "dma_port.h" /* g_dma_handle（__HAL_LINKDMA 需要） */

#include "common.h"
#include "dma.h"
#include "sys_port.h"   /* DMA1_Stream0 / HAL_DMA_* */
#include "usart_port.h" /* g_uart1_handle（__HAL_LINKDMA 需要） */

DMA_HandleTypeDef g_dma_handle;

/* UART TX DMA 只初始化一次。重入 DeInit 会中止在飞传输：完成中断不再到来，
 * 共享 TX 状态机的 `active` 永远不会清零，环形缓冲写满后 usart_log_write()
 * 永久等在事件上 —— 控制台静默失效，且没有任何错误码暴露。 */
static BYTE s_usart_tx_dma_initialized;

/**
 * @brief 初始化 USART1 TX DMA（DMA1_Stream0）
 *
 * 幂等：已成功初始化时直接返回成功，不重配在用的流。
 *
 * @return FUNC_STATUS_SUCCESS 已就绪（本次初始化成功或此前已成功）；
 *         FUNC_STATUS_ERROR HAL_DMA_DeInit/Init 失败。
 */
func_status_e dma_port_usart_tx_init(void)
{
    if (s_usart_tx_dma_initialized != 0U)
    {
        return FUNC_STATUS_SUCCESS;
    }

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
        return FUNC_STATUS_ERROR;
    }
    __HAL_LINKDMA(&g_uart1_handle, hdmatx, g_dma_handle);
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 4U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    s_usart_tx_dma_initialized = 1U;
    return FUNC_STATUS_SUCCESS;
}

void dma_port_usart_tx_irq_handler(void)
{
    HAL_DMA_IRQHandler(&g_dma_handle);
}

/*
 * DMA1_Stream0 中断入口（向量表名由 startup_stm32h743xx.s 固定）。
 * 本文件即该向量的归属地：USART1 TX 方向的 DMA 完成/错误都从这里进来。
 */
void DMA1_Stream0_IRQHandler(void)
{
    dma_usart_tx_irq_handler();
}
