/*
 * usart.c — 跨 target 共享的调试串口传输层
 *
 * 职责：RX/TX 环形队列、非阻塞日志 sink、newlib _write 钩子、诊断计数、TX 状态机。
 * 本文件**不含**任何命令行语义（CLI 在应用层的 cli.c），也不 include 任何 SoC 头、
 * 不出现任何 HAL 类型或内核内建：一切寄存器/DMA/缓存/屏障/PRIMASK 操作都通过
 * include/platform/usart.h 里的 usart_port_* 接口下沉到 src/platform/<soc>/usart_port.c。
 *
 * 队列模型（无锁 SPSC，靠 port 侧屏障保证可见性）：
 *   RX：USART IRQ 是唯一生产者（usart_rx_ring_push），主循环 CLI 是唯一消费者（usart_rx_read）。
 *   TX：普通固件上下文是唯一生产者（usart_log_write），USART IRQ 是唯一消费者（usart_tx_ring_pop）。
 */

#include "usart.h"

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */
#include "stats.h"

#include <stddef.h>
#include <stdint.h>

#define USART_RX_RING_MASK (USART_RX_RING_CAPACITY - 1U)

#define USART_TX_RING_MASK (USART_TX_RING_CAPACITY - 1U)
#define WRITE_BUF_SIZE     512U

/*
 * RX 环形队列：USART IRQ 是唯一生产者，usart_rx_read() 是唯一消费者。
 * IRQ 只发布 s_rx_head，主循环只推进 s_rx_tail，这是无锁 SPSC 队列成立的前提。
 */
static BYTE s_rx_ring[USART_RX_RING_CAPACITY];

static volatile WORD s_rx_head;

static volatile WORD s_rx_tail;

static volatile WORD s_tx_head;

static volatile WORD s_tx_tail;

static volatile WORD s_tx_dma_length;

static volatile BYTE s_tx_dma_active;

static BYTE s_tx_dma_ready;

static volatile BYTE s_tx_transport_fault;

static usart_stats_s s_usart_stats;

static BYTE s_usart_ready;

#if SSD_STATS_ENABLE
#    define UART_STATS_LIST(X)                           \
        X(rx_overflow, STATS_U32, STATS_COUNTER)         \
        X(tx_dropped, STATS_U32, STATS_COUNTER)          \
        X(tx_context_errors, STATS_U32, STATS_COUNTER)   \
        X(tx_flush_timeouts, STATS_U32, STATS_COUNTER)   \
        X(tx_dma_errors, STATS_U32, STATS_COUNTER)       \
        X(tx_transport_errors, STATS_U32, STATS_COUNTER) \
        X(overrun_errors, STATS_U32, STATS_COUNTER)      \
        X(framing_errors, STATS_U32, STATS_COUNTER)      \
        X(noise_errors, STATS_U32, STATS_COUNTER)        \
        X(parity_errors, STATS_U32, STATS_COUNTER)

#    define UART_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(usart_stats_s, field), width, kind},
static const stats_name_s s_stats_usart_names[] = {
    UART_STATS_LIST(UART_STATS_NAME)};
#    undef UART_STATS_NAME
#    define UART_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(usart_stats_s, field, width);
UART_STATS_LIST(UART_STATS_CHECK)
#    undef UART_STATS_CHECK
#endif /* SSD_STATS_ENABLE */

#if ((USART_RX_RING_CAPACITY & USART_RX_RING_MASK) != 0U)
#    error "USART_RX_RING_CAPACITY must be a power of two"
#endif

#if ((USART_TX_RING_CAPACITY & USART_TX_RING_MASK) != 0U)
#    error "USART_TX_RING_CAPACITY must be a power of two"
#endif

/*
 * TX 环形队列：普通固件上下文是生产者，USART IRQ 是消费者。
 * usart_log_write() 用极短的关中断区保护“入队 + 打开 TXEIE/DMA”的交接，防止日志丢在队列中。
 */
/* 尾部 padding 保证 DMA 段恰好在环末尾结束时，最后的 cache line 维护仍安全。 */
static BYTE s_tx_ring[USART_TX_RING_CAPACITY + 31U] __attribute__((aligned(32)));

/* ─────────────────────────── TX 状态机（策略在共享侧）─────────────────────────── */

/* 调用时中断已由 usart_log_write() 屏蔽，或来自 DMA 完成 ISR。 */
static void usart_tx_dma_start_locked(void)
{
    if ((s_tx_dma_ready == 0U) || (s_tx_dma_active != 0U) || (s_tx_tail == s_tx_head))
    {
        return;
    }

    const DWORD tail   = s_tx_tail;
    const DWORD length = (s_tx_head > tail) ? (s_tx_head - tail) : (USART_TX_RING_CAPACITY - tail);

    s_tx_dma_length = length;
    s_tx_dma_active = 1U;
    if (!usart_port_tx_dma_start(&s_tx_ring[tail], length))
    {
        s_tx_dma_active = 0U;
        s_usart_stats.tx_dma_errors++;
        s_usart_stats.tx_transport_errors++;
        s_tx_transport_fault = 1U;
        /* tail 未推进：保留的字节由 TXE 回退路径继续发。 */
        s_tx_dma_ready       = 0U;
        usart_port_tx_irq_enable();
        usart_port_event_signal();
        return;
    }
}

/* ─────────────────────────── 队列原语（供 port / CLI 调用）─────────────────────────── */

BYTE usart_rx_read(BYTE *byte)
{
    WORD tail = s_rx_tail;

    if (tail == s_rx_head)
    {
        return 0U;
    }

    /*
     * IRQ 必须先写 ring[head]，再更新 head；屏障防止编译器/CPU 重排这两个动作。
     * 这里的屏障与生产者中的屏障配对，确保消费者不会读到尚未写完的数据。
     */
    *byte = s_rx_ring[tail];
    usart_port_memory_barrier();
    s_rx_tail = (tail + 1U) & USART_RX_RING_MASK;
    return 1U;
}

void usart_rx_ring_push(BYTE byte)
{
    WORD head = s_rx_head;
    WORD next = (head + 1U) & USART_RX_RING_MASK;

    if (next == s_rx_tail)
    {
        /* 策略：保留最旧的未处理 CLI 输入，丢弃新数据。 */
        s_usart_stats.rx_overflow++;
        return;
    }

    s_rx_ring[head] = byte;
    usart_port_memory_barrier();
    s_rx_head = next;
}

BYTE usart_tx_ring_pop(BYTE *byte)
{
    WORD tail = s_tx_tail;

    if (tail == s_tx_head)
    {
        return 0U;
    }

    /* 先取出数据再推进 tail；推进 tail 后，该槽位才可以被生产者重新使用。 */
    *byte = s_tx_ring[tail];
    usart_port_memory_barrier();
    s_tx_tail = (tail + 1U) & USART_TX_RING_MASK;
    usart_port_event_signal();
    return 1U;
}

BYTE usart_tx_ring_is_empty(void)
{
    return (s_tx_tail == s_tx_head) ? 1U : 0U;
}

BYTE usart_tx_dma_ready(void)
{
    return s_tx_dma_ready;
}

void usart_stats_count_rx_error(DWORD kinds)
{
    if ((kinds & USART_RX_ERROR_PARITY) != 0U)
    {
        s_usart_stats.parity_errors++;
    }
    if ((kinds & USART_RX_ERROR_FRAMING) != 0U)
    {
        s_usart_stats.framing_errors++;
    }
    if ((kinds & USART_RX_ERROR_NOISE) != 0U)
    {
        s_usart_stats.noise_errors++;
    }
    if ((kinds & USART_RX_ERROR_OVERRUN) != 0U)
    {
        s_usart_stats.overrun_errors++;
    }
}

void usart_tx_dma_complete_handler(void)
{
    usart_port_tx_dma_detach();

    const WORD transferred = s_tx_dma_length;

    s_tx_dma_length = 0U;
    s_tx_tail       = ((s_tx_tail + transferred) & USART_TX_RING_MASK);
    usart_port_memory_barrier();
    s_tx_dma_active = 0U;
    usart_port_event_signal();
    usart_tx_dma_start_locked();
}

void usart_tx_dma_error_handler(void)
{
    usart_port_tx_dma_detach();

    WORD remaining = usart_port_tx_dma_remaining();

    if (remaining > s_tx_dma_length)
    {
        remaining = s_tx_dma_length;
    }
    const WORD transferred = (s_tx_dma_length - remaining);

    s_tx_tail       = ((s_tx_tail + transferred) & USART_TX_RING_MASK);
    s_tx_dma_length = 0U;
    s_tx_dma_active = 0U;
    s_usart_stats.tx_dma_errors++;
    s_usart_stats.tx_transport_errors++;
    s_tx_transport_fault = 1U;
    /* 已搬运前缀已进入 UART FIFO；TXE 只补发剩余部分。 */
    s_tx_dma_ready       = 0U;
    usart_port_tx_irq_enable();
    usart_port_event_signal();
}

/* ─────────────────────────── 初始化 / 发送 / 计数 ─────────────────────────── */

func_status_e usart_init(DWORD baudrate)
{
    BYTE dma_ready = 0U;

    if (usart_port_hw_init(baudrate, &dma_ready) != FUNC_STATUS_SUCCESS)
    {
        return FUNC_STATUS_ERROR;
    }

    s_tx_dma_ready = dma_ready;
    s_usart_ready  = 1U;
#if SSD_STATS_ENABLE
    (void)stats_register("uart", STATS_INST_NONE, &s_usart_stats, s_stats_usart_names, STATS_ENTRY_COUNT(s_stats_usart_names));
#endif
    return FUNC_STATUS_SUCCESS;
}

/*
 * newlib 的 printf 最终会调用 _write()。把它接到软件 TX 队列后，现有 printf
 * 自动变成非阻塞日志。
 * ⚠ libc ABI 要求 `int _write()` 且失败返回 -1（H46 的明确例外）；项目内部的
 *   `usart_log_write()` 已改为 `DWORD` + `USART_LOG_INVALID` 哨兵，本函数只做 ABI 转换。
 *
 * 行尾归一化：裸机串口终端（PuTTY/minicom 等）要求 \r\n 才能正确换行。
 * printf/puts/fprintf 在 newlib 中只输出 \n，此处扫描 buffer，
 * 在裸 \n（前面不是 \r）前插入 \r，同时避免已有 \r\n 被重复加 \r。
 * 所有走 printf 的输出（LOGI/munit 测试报告等）自动受益，
 * 直接调 usart_log_write() 的不受影响。
 */
int _write(int file, char *buffer, int length)
{
    (void)file;

    if ((buffer == NULL) || (length <= 0))
    {
        return 0;
    }

    /* ── 快速路径：buffer 中没有裸 \n，原样发送 ── */
    BYTE needs_cr = 0U;

    for (int i = 0; i < length; i++)
    {
        if ((buffer[i] == '\n') && ((i == 0) || (buffer[i - 1] != '\r')))
        {
            needs_cr = 1U;
            break;
        }
    }
    if (needs_cr == 0U)
    {
        const DWORD written = usart_log_write(buffer, length);

        return (written == USART_LOG_INVALID) ? -1 : (int)written;
    }

    /* ── 慢速路径：在裸 \n 前插入 \r ── */
    static char  converted[WRITE_BUF_SIZE];
    unsigned int dst_len = 0U;
    BOOL         ok      = true;

    for (int src_i = 0; src_i < length; src_i++)
    {
        if ((buffer[src_i] == '\n') && ((src_i == 0) || (buffer[src_i - 1] != '\r')))
        {
            /* 需要写 \r\n，检查剩余空间 */
            if (dst_len + 2 > WRITE_BUF_SIZE)
            {
                /* 刷新已转换部分再继续 */
                ok      = (usart_log_write(converted, dst_len) != USART_LOG_INVALID) && ok;
                dst_len = 0;
            }
            converted[dst_len++] = '\r';
            converted[dst_len++] = '\n';
        }
        else
        {
            if (dst_len + 1 > WRITE_BUF_SIZE)
            {
                ok      = (usart_log_write(converted, dst_len) != USART_LOG_INVALID) && ok;
                dst_len = 0;
            }
            converted[dst_len++] = buffer[src_i];
        }
    }

    /* 刷剩余 */
    if (dst_len > 0)
    {
        ok = (usart_log_write(converted, dst_len) != USART_LOG_INVALID) && ok;
    }

    /* 告知上层全部发出（不重试）；libc `_write` 约定失败返回 -1。 */
    return ok ? length : -1;
}

DWORD usart_log_write(const char *data, DWORD length)
{
    if ((!s_usart_ready) || (data == NULL) || (length == 0U))
    {
        return 0U;
    }

    /* DMA 传输错误使字节边界不明确；不能声称之后的写入仍可靠。 */
    if (s_tx_transport_fault != 0U)
    {
        return USART_LOG_INVALID;
    }

    if (usart_port_context_is_atomic() != 0U)
    {
        s_usart_stats.tx_context_errors++;
        return USART_LOG_INVALID;
    }

    DWORD written = 0U;

    while (written < length)
    {
        const DWORD primask = usart_port_irq_state();

        if (s_tx_transport_fault != 0U)
        {
            return USART_LOG_INVALID;
        }

        /* 原子地发布字节并武装消费者；绝不在中断屏蔽下等待。 */
        usart_port_irq_disable();
        while (written < length)
        {
            const WORD head = s_tx_head;
            const WORD next = (head + 1U) & USART_TX_RING_MASK;

            if (next == s_tx_tail)
            {
                break;
            }

            s_tx_ring[head] = data[written++];
            usart_port_memory_barrier();
            s_tx_head = next;
        }

        if (s_tx_dma_ready != 0U)
        {
            usart_tx_dma_start_locked();
        }
        else
        {
            usart_port_tx_irq_enable();
        }
        usart_port_irq_restore(primask);

        if (written < length)
        {
            /* 消费者每次推进 tail 后都发事件，因此不会错过唤醒。 */
            usart_port_event_wait();
        }
    }

    return written;
}

BOOL usart_log_flush(DWORD timeout_ms)
{
    if ((s_usart_ready == 0U) || (usart_port_context_is_atomic() != 0U))
    {
        return false;
    }

    const DWORD start_tick = usart_port_now_ms();

    for (;;)
    {
        const DWORD primask = usart_port_irq_state();

        usart_port_irq_disable();
        const BYTE empty      = (s_tx_head == s_tx_tail) ? 1U : 0U;
        const BYTE dma_active = s_tx_dma_active;
        if (s_tx_transport_fault != 0U)
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

        if ((timeout_ms != USART_LOG_FLUSH_FOREVER) && ((usart_port_now_ms() - start_tick) >= timeout_ms))
        {
            s_usart_stats.tx_flush_timeouts++;
            return false;
        }
        if ((empty == 0U) || (dma_active != 0U))
        {
            /* tail 推进发事件；只有 TC 的完成在这里轮询。 */
            usart_port_event_wait();
        }
    }
}

void usart_get_stats(usart_stats_s *stats)
{
    if (stats == NULL)
    {
        return;
    }

    /* ISR 会并发累加计数；取整块快照必须屏蔽中断，避免读到撕裂的字段组合。 */
    const DWORD primask = usart_port_irq_state();

    usart_port_irq_disable();
    *stats = s_usart_stats;
    usart_port_irq_restore(primask);
}
