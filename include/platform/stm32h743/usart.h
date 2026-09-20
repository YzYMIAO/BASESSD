

#ifndef __USART_H
#define __USART_H

#include "sys.h"

#include <stdint.h>

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

/*
 * 使用静态队列，不依赖 malloc：当堆、FTL 或内存分配器异常时，调试路径仍可用。
 * 容量必须是 2 的幂，IRQ 中可通过 index & (capacity - 1) 快速回绕，避免除法。
 * 环形队列会空出一个槽位，以 head == tail 表示空，因此实际可用容量为 capacity - 1。
 */
#define USART_RX_RING_CAPACITY  256U
#define USART_TX_RING_CAPACITY  4096U
#define USART_CLI_LINE_CAPACITY 128U
#define UART_LOG_FLUSH_FOREVER  0U

typedef enum
{
    UART_LOG_ERROR = 0,
    UART_LOG_WARNING,
    UART_LOG_INFO,
    UART_LOG_DEBUG
} uart_log_level_t;

/*
 * 单调递增的诊断计数。它们不在读取后自动清零，复位后才开始新的观测周期。
 * status CLI 用这些值区分“协议处理慢造成的软件队列满”和“IRQ 响应慢造成的硬件 ORE”。
 */
typedef struct
{
    uint32_t rx_overflow;    /* Software RX ring was full. */
    uint32_t tx_dropped;     /* New log bytes rejected because TX ring was full. */
    uint32_t tx_context_errors; /* Logging attempted from an ISR or while interrupts were masked. */
    uint32_t tx_flush_timeouts; /* Flush could not observe queue empty, DMA idle, and TC before its deadline. */
    uint32_t tx_dma_errors;  /* DMA transfer errors; TXE interrupt fallback remains active. */
    uint32_t tx_transport_errors; /* A UART/DMA transport fault makes reliable delivery unprovable. */
    uint32_t overrun_errors; /* USART ORE: peripheral/FIFO was not drained in time. */
    uint32_t framing_errors; /* USART FE: stop bit was invalid. */
    uint32_t noise_errors;   /* USART NE: noise was detected while sampling. */
    uint32_t parity_errors;  /* USART PE: received parity did not match. */
} uart_stats_t;

extern UART_HandleTypeDef g_uart1_handle;

void usart_init(uint32_t baudrate);

/*
 * 仅由 USART1_IRQHandler 调用。该函数不解析命令、不 printf、不延时，
 * 只做 FIFO 与软件队列之间的字节搬运，并清除硬件错误标志。
 */
void usart_irq_handler(void);

/*
 * 可靠发送接口：正常任务上下文返回前，data 的全部字节均已进入 TX 队列。队列满时
 * 等待 DMA/USART IRQ 推进 tail，形成反压，不短写、不丢弃。不得从 ISR、持有不可等待
 * 锁的临界区或中断已屏蔽的上下文调用；这些调用返回 -1 并累计 tx_context_errors。
 */
int              uart_log_write(const char *data, uint32_t length);
int              uart_log_printf(uart_log_level_t level, const char *format, ...);
void             uart_log_set_level(uart_log_level_t level);
uart_log_level_t uart_log_get_level(void);
void             uart_get_stats(uart_stats_t *stats);

/* Wait until the TX queue is empty, DMA is idle, and USART TC confirms the final stop bit left TX. */
int uart_log_flush(uint32_t timeout_ms);

/*
 * 在主循环或唯一的低优先级控制台任务中周期调用，负责回显、组装一行并执行 CLI。
 * 绝不能在 ISR 中调用，因为命令匹配和日志输出都属于可能耗时的非实时工作。
 */
void uart_service_poll(void);

#endif
