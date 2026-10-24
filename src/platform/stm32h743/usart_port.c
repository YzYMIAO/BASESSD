/*
 * usart_port.c — stm32h743 调试串口适配层
 *
 * 只做硬件相关的事：HAL 初始化、GPIO/时钟、DMA、USART 寄存器、中断入口，以及把内核
 * 内建（屏障/事件/PRIMASK）包装成中性接口。传输逻辑（环形队列、TX 状态机、日志 sink）
 * 全在 src/platform/usart.c；本文件通过 include/platform/usart.h 的 uart_* 共享入口
 * 与它交互，不重复实现任何队列/策略。
 */

#include "usart_port.h"

#include "dma.h"
#include "usart.h"

#include <stdint.h>
#include <string.h>

UART_HandleTypeDef g_uart1_handle = {0};

/* UART TX DMA 句柄定义在 dma.c；抽象 dma.h 只暴露裸签名，故在此本地声明。 */
extern DMA_HandleTypeDef g_dma_handle;

static void usart_tx_dma_complete_cb(DMA_HandleTypeDef *dma_handle);
static void usart_tx_dma_error_cb(DMA_HandleTypeDef *dma_handle);

BYTE usart_port_hw_init(DWORD baudrate)
{
    BYTE dma_ready;

    g_uart1_handle.Instance                    = USART_UX;
    g_uart1_handle.Init.BaudRate               = baudrate;
    g_uart1_handle.Init.WordLength             = UART_WORDLENGTH_8B;
    g_uart1_handle.Init.StopBits               = UART_STOPBITS_1;
    g_uart1_handle.Init.Parity                 = UART_PARITY_NONE;
    g_uart1_handle.Init.Mode                   = UART_MODE_TX_RX;
    g_uart1_handle.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    g_uart1_handle.Init.OverSampling           = UART_OVERSAMPLING_16;
    g_uart1_handle.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    g_uart1_handle.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    g_uart1_handle.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    (void)HAL_UART_Init(&g_uart1_handle);

    dma_ready = uart_tx_dma_init();
    if (dma_ready != 0U)
    {
        g_dma_handle.XferCpltCallback  = usart_tx_dma_complete_cb;
        g_dma_handle.XferErrorCallback = usart_tx_dma_error_cb;
    }

    /*
     * HAL 仅负责时钟、GPIO、BRR 和基本帧格式初始化。从这里开始 RXNE/TXE 的所有权
     * 交给自定义 IRQ 处理器；不能再同时调用 HAL_UART_Receive_IT()，否则两个状态机
     * 会竞争 RDR 和中断使能位。
     */
    USART_UX->CR1 |= USART_CR1_RXNEIE_RXFNEIE | USART_CR1_PEIE;
    USART_UX->CR3 |= USART_CR3_EIE;

    return dma_ready;
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    RCC_PeriphCLKInitTypeDef rcc_periph_clk_init_struct;
    GPIO_InitTypeDef         gpio_init_struct;

    memset(&rcc_periph_clk_init_struct, 0, sizeof(rcc_periph_clk_init_struct));
    memset(&gpio_init_struct, 0, sizeof(gpio_init_struct));

    if (huart->Instance != USART_UX)
    {
        return;
    }

    rcc_periph_clk_init_struct.PeriphClockSelection  = RCC_PERIPHCLK_USART1;
    /* USART1/6 时钟源=PCLK2=100 MHz；UART 波特率由该内核时钟分频得到。 */
    rcc_periph_clk_init_struct.Usart16ClockSelection = RCC_USART16CLKSOURCE_PCLK2;
    (void)HAL_RCCEx_PeriphCLKConfig(&rcc_periph_clk_init_struct);

    USART_UX_CLK_ENABLE();
    USART_TX_GPIO_CLK_ENABLE();
    USART_RX_GPIO_CLK_ENABLE();

    gpio_init_struct.Pin       = USART_TX_GPIO_PIN;
    gpio_init_struct.Mode      = GPIO_MODE_AF_PP;
    gpio_init_struct.Pull      = GPIO_PULLUP;
    gpio_init_struct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init_struct.Alternate = USART_TX_GPIO_AF;
    HAL_GPIO_Init(USART_TX_GPIO_PORT, &gpio_init_struct);

    gpio_init_struct.Pin       = USART_RX_GPIO_PIN;
    gpio_init_struct.Alternate = USART_RX_GPIO_AF;
    HAL_GPIO_Init(USART_RX_GPIO_PORT, &gpio_init_struct);

    HAL_NVIC_SetPriority(USART_UX_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(USART_UX_IRQn);
}

/* ─────────────────────────── 向量表入口 ─────────────────────────── */

/*
 * USART1 中断入口（向量表名由 startup_stm32h743xx.s 固定）。
 * 本文件即该向量的归属地：只把控制权交给下方的处理函数。
 */
void USART1_IRQHandler(void)
{
#if SYS_SUPPORT_OS
    OSIntEnter();
#endif

    /* Keep the IRQ bounded: it only drains/fills FIFOs and records errors. */
    usart_port_irq_handler();

#if SYS_SUPPORT_OS
    OSIntExit();
#endif
}

void usart_port_irq_handler(void)
{
    DWORD status      = USART_UX->ISR;
    DWORD clear_flags = 0U;
    DWORD error_kinds = 0U;
    BYTE  byte;

    /*
     * 先累计原因再清标志。即便出现 FE/NE/ORE，也要继续读取 RDR 抽干 RX FIFO，
     * 否则 RXNE 会持续触发中断，且下一字节更容易再次溢出。
     */
    if ((status & USART_ISR_PE) != 0U)
    {
        error_kinds |= UART_RX_ERROR_PARITY;
        clear_flags |= USART_ICR_PECF;
    }
    if ((status & USART_ISR_FE) != 0U)
    {
        error_kinds |= UART_RX_ERROR_FRAMING;
        clear_flags |= USART_ICR_FECF;
    }
    if ((status & USART_ISR_NE) != 0U)
    {
        error_kinds |= UART_RX_ERROR_NOISE;
        clear_flags |= USART_ICR_NECF;
    }
    if ((status & USART_ISR_ORE) != 0U)
    {
        error_kinds |= UART_RX_ERROR_OVERRUN;
        clear_flags |= USART_ICR_ORECF;
    }
    if (error_kinds != 0U)
    {
        uart_stats_count_rx_error(error_kinds);
    }

    /*
     * 一次 IRQ 尽量抽干硬件 RX FIFO，而非仅处理一个字节，降低高速连续收包的 ORE 风险。
     * 软件队列满时采取“丢弃新字节、保留旧命令”的策略；rx_overflow 用于后续诊断。
     */
    while ((USART_UX->ISR & USART_ISR_RXNE_RXFNE) != 0U)
    {
        byte = USART_UX->RDR;
        uart_rx_ring_push(byte);
    }

    if (clear_flags != 0U)
    {
        USART_UX->ICR = clear_flags;
    }

    if ((uart_tx_dma_ready() == 0U) && ((USART_UX->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0U) && ((USART_UX->ISR & USART_ISR_TXE_TXFNF) != 0U))
    {
        /*
         * TXE/TXFNF 表示 TDR 或 TX FIFO 尚有位置。连续填充可减少每个字符一次 IRQ 的开销；
         * 真正最后一位离开 TX 引脚需观察 TC，本日志路径不等待 TC。
         */
        while (((USART_UX->ISR & USART_ISR_TXE_TXFNF) != 0U) && (uart_tx_ring_pop(&byte) != 0U))
        {
            USART_UX->TDR = byte;
        }

        /* 软件队列为空时关闭 TXEIE，否则 TXE 始终为 1 会产生空转中断。 */
        if (uart_tx_ring_is_empty() != 0U)
        {
            USART_UX->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
        }
    }
}

/* ─────────────────────────── port 原语（供共享层调用）─────────────────────────── */

bool usart_port_tx_dma_start(const BYTE *buffer, DWORD length)
{
    /* TX Ring is cacheable AXI SRAM; DMA must see bytes written by the CPU before it starts. */
    SCB_CleanDCache_by_Addr((DWORD *)(uintptr_t)buffer, (int32_t)((length + 31U) & ~31U));
    if (HAL_DMA_Start_IT(&g_dma_handle, (uintptr_t)buffer, (DWORD)&USART_UX->TDR, length) != HAL_OK)
    {
        return false;
    }
    USART_UX->CR3 |= USART_CR3_DMAT;
    return true;
}

void usart_port_tx_dma_detach(void)
{
    USART_UX->CR3 &= ~USART_CR3_DMAT;
}

WORD usart_port_tx_dma_remaining(void)
{
    return __HAL_DMA_GET_COUNTER(&g_dma_handle);
}

void usart_port_tx_irq_enable(void)
{
    USART_UX->CR1 |= USART_CR1_TXEIE_TXFNFIE;
}

BYTE usart_port_tx_complete(void)
{
    return ((USART_UX->ISR & USART_ISR_TC) != 0U) ? 1U : 0U;
}

DWORD usart_port_now_ms(void)
{
    return HAL_GetTick();
}

void usart_port_memory_barrier(void)
{
    __DMB();
}

void usart_port_event_signal(void)
{
    __SEV();
}

void usart_port_event_wait(void)
{
    __WFE();
}

BYTE usart_port_context_is_atomic(void)
{
    return ((__get_IPSR() != 0U) || (__get_PRIMASK() != 0U)) ? 1U : 0U;
}

DWORD usart_port_irq_state(void)
{
    return __get_PRIMASK();
}

void usart_port_irq_disable(void)
{
    __disable_irq();
}

void usart_port_irq_restore(DWORD state)
{
    __set_PRIMASK(state);
}

/* ─────────────────────────── DMA 回调（HAL 签名）─────────────────────────── */

static void usart_tx_dma_complete_cb(DMA_HandleTypeDef *dma_handle)
{
    (void)dma_handle;
    uart_tx_dma_complete_handler();
}

static void usart_tx_dma_error_cb(DMA_HandleTypeDef *dma_handle)
{
    (void)dma_handle;
    uart_tx_dma_error_handler();
}
