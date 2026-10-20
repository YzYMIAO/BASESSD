/*
 * usart.c — 跨 target 共享的调试串口传输层
 *
 * 职责：RX/TX 环形队列、非阻塞日志 sink、newlib _write 钩子、诊断计数、TX 状态机。
 * 本文件**不含**任何命令行语义（CLI 在应用层的 cli.c），也不 include 任何 SoC 头、
 * 不出现任何 HAL 类型或内核内建：一切寄存器/DMA/缓存/屏障/PRIMASK 操作都通过
 * include/platform/usart.h 里的 usart_port_* 接口下沉到 src/platform/<soc>/usart_port.c。
 *
 * 队列模型（无锁 SPSC，靠 port 侧屏障保证可见性）：
 *   RX：USART IRQ 是唯一生产者（uart_rx_ring_push），主循环 CLI 是唯一消费者（uart_rx_read）。
 *   TX：普通固件上下文是唯一生产者（uart_log_write），USART IRQ 是唯一消费者（uart_tx_ring_pop）。
 */

#include "usart.h"

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stddef.h>
#include <stdint.h>

#define USART_RX_RING_MASK (USART_RX_RING_CAPACITY - 1U)
#define USART_TX_RING_MASK (USART_TX_RING_CAPACITY - 1U)

#if ((USART_RX_RING_CAPACITY & USART_RX_RING_MASK) != 0U)
#    error "USART_RX_RING_CAPACITY must be a power of two"
#endif

#if ((USART_TX_RING_CAPACITY & USART_TX_RING_MASK) != 0U)
#    error "USART_TX_RING_CAPACITY must be a power of two"
#endif

/*
 * RX 环形队列：USART IRQ 是唯一生产者，uart_rx_read() 是唯一消费者。
 * IRQ 只发布 g_rx_head，主循环只推进 g_rx_tail，这是无锁 SPSC 队列成立的前提。
 */
static BYTE          g_rx_ring[USART_RX_RING_CAPACITY];
static volatile WORD g_rx_head;
static volatile WORD g_rx_tail;

/*
 * TX 环形队列：普通固件上下文是生产者，USART IRQ 是消费者。
 * uart_log_write() 用极短的关中断区保护“入队 + 打开 TXEIE/DMA”的交接，防止日志丢在队列中。
 */
/* The padding makes final cache-line maintenance safe when a DMA segment ends at ring end. */
static BYTE          g_tx_ring[USART_TX_RING_CAPACITY + 31U] __attribute__((aligned(32)));
static volatile WORD g_tx_head;
static volatile WORD g_tx_tail;
static volatile WORD g_tx_dma_length;
static volatile BYTE g_tx_dma_active;
static BYTE          g_tx_dma_ready;
static volatile BYTE g_tx_transport_fault;

static uart_stats_t g_uart_stats;
static BYTE         g_usart_ready;

/* ─────────────────────────── 队列原语（供 port / CLI 调用）─────────────────────────── */

BYTE uart_rx_read(BYTE *byte)
{
    WORD tail = g_rx_tail;

    if (tail == g_rx_head)
    {
        return 0U;
    }

    /*
     * IRQ 必须先写 ring[head]，再更新 head；屏障防止编译器/CPU 重排这两个动作。
     * 这里的屏障与生产者中的屏障配对，确保消费者不会读到尚未写完的数据。
     */
    *byte = g_rx_ring[tail];
    usart_port_memory_barrier();
    g_rx_tail = (tail + 1U) & USART_RX_RING_MASK;
    return 1U;
}

void uart_rx_ring_push(BYTE byte)
{
    WORD head = g_rx_head;
    WORD next = (head + 1U) & USART_RX_RING_MASK;

    if (next == g_rx_tail)
    {
        /* Policy: preserve the oldest unprocessed CLI input and drop new data. */
        g_uart_stats.rx_overflow++;
        return;
    }

    g_rx_ring[head] = byte;
    usart_port_memory_barrier();
    g_rx_head = next;
}

BYTE uart_tx_ring_pop(BYTE *byte)
{
    WORD tail = g_tx_tail;

    if (tail == g_tx_head)
    {
        return 0U;
    }

    /* 先取出数据再推进 tail；推进 tail 后，该槽位才可以被生产者重新使用。 */
    *byte = g_tx_ring[tail];
    usart_port_memory_barrier();
    g_tx_tail = (tail + 1U) & USART_TX_RING_MASK;
    usart_port_event_signal();
    return 1U;
}

BYTE uart_tx_ring_is_empty(void)
{
    return (g_tx_tail == g_tx_head) ? 1U : 0U;
}

BYTE uart_tx_dma_ready(void)
{
    return g_tx_dma_ready;
}

void uart_stats_count_rx_error(DWORD kinds)
{
    if ((kinds & UART_RX_ERROR_PARITY) != 0U)
    {
        g_uart_stats.parity_errors++;
    }
    if ((kinds & UART_RX_ERROR_FRAMING) != 0U)
    {
        g_uart_stats.framing_errors++;
    }
    if ((kinds & UART_RX_ERROR_NOISE) != 0U)
    {
        g_uart_stats.noise_errors++;
    }
    if ((kinds & UART_RX_ERROR_OVERRUN) != 0U)
    {
        g_uart_stats.overrun_errors++;
    }
}

/* ─────────────────────────── TX 状态机（策略在共享侧）─────────────────────────── */

/* Called with IRQs masked by uart_log_write(), or from the DMA completion ISR. */
static void uart_tx_dma_start_locked(void)
{
    DWORD tail;
    DWORD length;

    if ((g_tx_dma_ready == 0U) || (g_tx_dma_active != 0U) || (g_tx_tail == g_tx_head))
    {
        return;
    }
    tail   = g_tx_tail;
    length = (g_tx_head > tail) ? (g_tx_head - tail) : (USART_TX_RING_CAPACITY - tail);

    g_tx_dma_length = length;
    g_tx_dma_active = 1U;
    if (!usart_port_tx_dma_start(&g_tx_ring[tail], length))
    {
        g_tx_dma_active = 0U;
        g_uart_stats.tx_dma_errors++;
        g_uart_stats.tx_transport_errors++;
        g_tx_transport_fault = 1U;
        /* No tail movement occurred; the TXE fallback drains the retained bytes. */
        g_tx_dma_ready       = 0U;
        usart_port_tx_irq_enable();
        usart_port_event_signal();
        return;
    }
}

void uart_tx_dma_complete_handler(void)
{
    WORD transferred;

    usart_port_tx_dma_detach();
    transferred     = g_tx_dma_length;
    g_tx_dma_length = 0U;
    g_tx_tail       = ((g_tx_tail + transferred) & USART_TX_RING_MASK);
    usart_port_memory_barrier();
    g_tx_dma_active = 0U;
    usart_port_event_signal();
    uart_tx_dma_start_locked();
}

void uart_tx_dma_error_handler(void)
{
    WORD remaining;
    WORD transferred;

    usart_port_tx_dma_detach();
    remaining = usart_port_tx_dma_remaining();
    if (remaining > g_tx_dma_length)
    {
        remaining = g_tx_dma_length;
    }
    transferred     = (g_tx_dma_length - remaining);
    g_tx_tail       = ((g_tx_tail + transferred) & USART_TX_RING_MASK);
    g_tx_dma_length = 0U;
    g_tx_dma_active = 0U;
    g_uart_stats.tx_dma_errors++;
    g_uart_stats.tx_transport_errors++;
    g_tx_transport_fault = 1U;
    /* The transferred prefix has reached the UART FIFO; TXE sends only the remainder. */
    g_tx_dma_ready       = 0U;
    usart_port_tx_irq_enable();
    usart_port_event_signal();
}

/* ─────────────────────────── 初始化 / 发送 / 计数 ─────────────────────────── */

void usart_init(DWORD baudrate)
{
    g_tx_dma_ready = usart_port_hw_init(baudrate);
    g_usart_ready  = 1U;
}

/*
 * newlib-nano 的 printf 最终会调用 _write()。把它接到软件 TX 队列后，现有 printf
 * 自动变成非阻塞日志。
 * ⚠ libc ABI 要求 `int _write()` 且失败返回 -1（H46 的明确例外）；项目内部的
 *   `uart_log_write()` 已改为 `DWORD` + `UART_LOG_INVALID` 哨兵，本函数只做 ABI 转换。
 *
 * 行尾归一化：裸机串口终端（PuTTY/minicom 等）要求 \r\n 才能正确换行。
 * printf/puts/fprintf 在 newlib-nano 中只输出 \n，此处扫描 buffer，
 * 在裸 \n（前面不是 \r）前插入 \r，同时避免已有 \r\n 被重复加 \r。
 * 所有走 printf 的输出（LOGI/munit 测试报告等）自动受益，
 * 直接调 uart_log_write() 的不受影响。
 */
int _write(int file, char *buffer, int length)
{
    int          i;
    BYTE         needs_cr;
    int          src_i;
    unsigned int dst_len;

    (void)file;

    if ((buffer == NULL) || (length <= 0))
    {
        return 0;
    }

    /* ── 快速路径：buffer 中没有裸 \n，原样发送 ── */
    needs_cr = 0U;
    for (i = 0; i < length; i++)
    {
        if ((buffer[i] == '\n') && ((i == 0) || (buffer[i - 1] != '\r')))
        {
            needs_cr = 1U;
            break;
        }
    }
    if (needs_cr == 0U)
    {
        const DWORD written = uart_log_write(buffer, length);

        return (written == UART_LOG_INVALID) ? -1 : (int)written;
    }

    /* ── 慢速路径：在裸 \n 前插入 \r ── */
#define WRITE_BUF_SIZE 512U
    static char converted[WRITE_BUF_SIZE];
    bool        ok = true;
    dst_len        = 0;

    for (src_i = 0; src_i < length; src_i++)
    {
        if ((buffer[src_i] == '\n') && ((src_i == 0) || (buffer[src_i - 1] != '\r')))
        {
            /* 需要写 \r\n，检查剩余空间 */
            if (dst_len + 2 > WRITE_BUF_SIZE)
            {
                /* 刷新已转换部分再继续 */
                ok      = (uart_log_write(converted, dst_len) != UART_LOG_INVALID) && ok;
                dst_len = 0;
            }
            converted[dst_len++] = '\r';
            converted[dst_len++] = '\n';
        }
        else
        {
            if (dst_len + 1 > WRITE_BUF_SIZE)
            {
                ok      = (uart_log_write(converted, dst_len) != UART_LOG_INVALID) && ok;
                dst_len = 0;
            }
            converted[dst_len++] = buffer[src_i];
        }
    }

    /* 刷剩余 */
    if (dst_len > 0)
    {
        ok = (uart_log_write(converted, dst_len) != UART_LOG_INVALID) && ok;
    }

    /* 告知上层全部发出（不重试）；libc `_write` 约定失败返回 -1。 */
    return ok ? length : -1;
#undef WRITE_BUF_SIZE
}

DWORD uart_log_write(const char *data, DWORD length)
{
    DWORD written = 0U;

    if ((!g_usart_ready) || (data == NULL) || (length == 0U))
    {
        return 0U;
    }

    /* A DMA transport error has an ambiguous byte boundary; do not claim later writes are reliable. */
    if (g_tx_transport_fault != 0U)
    {
        return UART_LOG_INVALID;
    }

    if (usart_port_context_is_atomic() != 0U)
    {
        g_uart_stats.tx_context_errors++;
        return UART_LOG_INVALID;
    }

    while (written < length)
    {
        DWORD primask = usart_port_irq_state();

        if (g_tx_transport_fault != 0U)
        {
            return UART_LOG_INVALID;
        }

        /* Publish bytes and arm the consumer atomically; never wait with IRQs masked. */
        usart_port_irq_disable();
        while (written < length)
        {
            WORD head = g_tx_head;
            WORD next = (head + 1U) & USART_TX_RING_MASK;

            if (next == g_tx_tail)
            {
                break;
            }

            g_tx_ring[head] = data[written++];
            usart_port_memory_barrier();
            g_tx_head = next;
        }

        if (g_tx_dma_ready != 0U)
        {
            uart_tx_dma_start_locked();
        }
        else
        {
            usart_port_tx_irq_enable();
        }
        usart_port_irq_restore(primask);

        if (written < length)
        {
            /* Consumers signal the event after every tail advance, so no release is missed. */
            usart_port_event_wait();
        }
    }

    return written;
}

bool uart_log_flush(DWORD timeout_ms)
{
    DWORD start_tick;

    if ((g_usart_ready == 0U) || (usart_port_context_is_atomic() != 0U))
    {
        return false;
    }

    start_tick = usart_port_now_ms();
    for (;;)
    {
        BYTE  empty;
        BYTE  dma_active;
        DWORD primask = usart_port_irq_state();

        usart_port_irq_disable();
        empty      = (g_tx_head == g_tx_tail) ? 1U : 0U;
        dma_active = g_tx_dma_active;
        if (g_tx_transport_fault != 0U)
        {
            usart_port_irq_restore(primask);
            return false;
        }
        if ((empty != 0U) && (dma_active == 0U) && (usart_port_tx_complete() != 0U))
        {
            usart_port_irq_restore(primask);
            return true;
        }
        usart_port_irq_restore(primask);

        if ((timeout_ms != UART_LOG_FLUSH_FOREVER) && ((usart_port_now_ms() - start_tick) >= timeout_ms))
        {
            g_uart_stats.tx_flush_timeouts++;
            return false;
        }
        if ((empty == 0U) || (dma_active != 0U))
        {
            /* tail updates signal the event; TC-only completion is polled below. */
            usart_port_event_wait();
        }
    }
}

void uart_get_stats(uart_stats_t *stats)
{
    if (stats != NULL)
    {
        *stats = g_uart_stats;
    }
}
