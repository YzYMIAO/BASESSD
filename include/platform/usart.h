#ifndef PLATFORM_USART_H
#define PLATFORM_USART_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdbool.h>
#include <stdint.h>

/*
 * 平台调试串口抽象接口（共享逻辑在 src/platform/usart.c，SoC 适配在
 * src/platform/<soc>/usart_port.c）。
 *
 * 本头零 HAL 类型：外设句柄、GPIO/时钟宏、中断号一律留在 SoC 侧
 * （include/platform/<soc>/usart_port.h）。
 *
 * 分层：
 *   - 传输层（本头 + src/platform/usart.c）—— 环形队列、非阻塞日志 sink、
 *     newlib _write 钩子、诊断计数；不含任何命令行语义。
 *   - CLI（include/hil/cli.h + src/hil/cli.c）—— 行编辑、命令表、
 *     wr/rd/tr/fl/info/test 等应用命令；消费本头的 usart_rx_read / usart_log_write。
 */

/*
 * 使用静态队列，不依赖 malloc：当堆、FTL 或内存分配器异常时，调试路径仍可用。
 * 容量必须是 2 的幂，IRQ 中可通过 index & (capacity - 1) 快速回绕，避免除法。
 * 环形队列会空出一个槽位，以 head == tail 表示空，因此实际可用容量为 capacity - 1。
 */
#ifndef USART_RX_RING_CAPACITY
#    define USART_RX_RING_CAPACITY 256U
#endif
#ifndef USART_TX_RING_CAPACITY
#    define USART_TX_RING_CAPACITY 4096U
#endif
#define USART_LOG_FLUSH_FOREVER 0U

/*
 * 单调递增的诊断计数。它们不在读取后自动清零，复位后才开始新的观测周期。
 * status CLI 用这些值区分“协议处理慢造成的软件队列满”和“IRQ 响应慢造成的硬件 ORE”。
 */
typedef struct USART_STATISTICS_S
{
    DWORD rx_overflow;         /* 软件 RX 环已满。 */
    DWORD tx_dropped;          /* TX 环已满，新日志字节被拒绝。 */
    DWORD tx_context_errors;   /* 在 ISR 中或中断被屏蔽时尝试写日志。 */
    DWORD tx_flush_timeouts;   /* flush 在期限内未观察到「队列空 + DMA 空闲 + TC」。 */
    DWORD tx_dma_errors;       /* DMA 传输错误；TXE 中断回退仍在工作。 */
    DWORD tx_transport_errors; /* 串口/DMA 传输故障使可靠送达无法证明。 */
    DWORD overrun_errors;      /* USART ORE：外设/FIFO 未被及时抽干。 */
    DWORD framing_errors;      /* USART FE：停止位无效。 */
    DWORD noise_errors;        /* USART NE：采样时检测到噪声。 */
    DWORD parity_errors;       /* USART PE：接收校验位不匹配。 */
} usart_stats_s;

/* 接收错误种类位掩码（中性命名，port 侧把 USART 硬件标志翻译成这些位）。 */
#define USART_RX_ERROR_PARITY  (1U << 0)
#define USART_RX_ERROR_FRAMING (1U << 1)
#define USART_RX_ERROR_NOISE   (1U << 2)
#define USART_RX_ERROR_OVERRUN (1U << 3)

/*
 * ---- 公开传输接口 ----
 */

/**
 * @brief 初始化串口（波特率）并置就绪位；由 sys_init 调用。
 *
 * 硬件初始化失败时不置就绪位，后续发送路径直接返回空写（不会静默使用未初始化的外设）。
 *
 * @param baudrate 控制台波特率。
 * @return FUNC_STATUS_SUCCESS 串口可用；FUNC_STATUS_ERROR 硬件初始化失败。
 */
func_status_e usart_init(DWORD baudrate);

/*
 * 可靠发送接口：正常任务上下文返回前，data 的全部字节均已进入 TX 队列。队列满时
 * 等待 DMA/USART IRQ 推进 tail，形成反压，不短写、不丢弃。不得从 ISR、持有不可等待
 * 锁的临界区或中断已屏蔽的上下文调用；这些调用返回 USART_LOG_INVALID 并累计
 * tx_context_errors。
 *
 * @return 实际接受的字节数（0..length）；失败返回 USART_LOG_INVALID（显式哨兵）。
 */
#define USART_LOG_INVALID (UINT32_MAX)
DWORD usart_log_write(const char *data, DWORD length);

/* 等待 TX 队列空、DMA 空闲，且 USART TC 确认最后停止位已离开 TX。
 * @return true = 已排空；false = 未就绪 / 上下文非法 / 超时。 */
BOOL usart_log_flush(DWORD timeout_ms);

void usart_get_stats(usart_stats_s *stats);

/*
 * 从 RX 软件队列取出一个字节。返回非 0 = 取到（*byte 有效），0 = 队列为空。
 * 仅由主循环 CLI（cli_service_poll）消费；USART IRQ 是唯一生产者。
 */
BYTE usart_rx_read(BYTE *byte);

/*
 * ---- 共享入口：由 port 侧的中断/回调调用 ----
 */

/* RX 生产者：IRQ 抽干硬件 FIFO 时逐字节调用；队列满时丢弃新字节并累计 rx_overflow。 */
void usart_rx_ring_push(BYTE byte);

/* TX 消费者：TXE 回退路径逐字节取出发送；返回非 0 = 取到，0 = 队列为空。 */
BYTE usart_tx_ring_pop(BYTE *byte);

BYTE usart_tx_ring_is_empty(void);

/* DMA 传输是否可用（dma_usart_tx_init 是否成功）；供 IRQ 决定走 DMA 还是 TXE 回退。 */
BYTE usart_tx_dma_ready(void);

/* 把 port 侧翻译好的错误位累计进诊断计数。 */
void usart_stats_count_rx_error(DWORD kinds);

/* DMA 完成/出错回调的簿记入口；由 port 侧 HAL 签名的回调转发。 */
void usart_tx_dma_complete_handler(void);
void usart_tx_dma_error_handler(void);

/*
 * ---- port 接口：由 src/platform/<soc>/usart_port.c 实现 ----
 * 共享逻辑只通过这几个函数触碰 SoC —— 它自己不 include 任何 SoC 头、不出现任何
 * HAL 类型，连 USART 寄存器、DMA 句柄与内核内建（屏障/事件/PRIMASK）都不直接使用。
 */

/**
 * @brief 硬件初始化（串口外设 + GPIO + DMA + 接收中断使能）。
 *
 * @param baudrate   控制台波特率。
 * @param dma_ready  出参：TX DMA 是否可用；DMA 失败降级为 TXE 中断回退，不算初始化失败。
 * @return FUNC_STATUS_SUCCESS 串口外设与时钟配置成功；FUNC_STATUS_ERROR HAL 或时钟配置失败。
 */
func_status_e usart_port_hw_init(DWORD baudrate, BYTE *dma_ready);

/* 串口中断入口：抽干 RX FIFO、清错误标志、TXE 回退填充；由 USART1_IRQHandler 调用。 */
void usart_port_irq_handler(void);

/* TX DMA 启动：D-cache 清理 + DMA 启动 + CR3 DMAT。返回 true = 已启动。 */
BOOL usart_port_tx_dma_start(const BYTE *buffer, DWORD length);

/* 断开 TX DMA（清 CR3 DMAT）。 */
void usart_port_tx_dma_detach(void);

/* DMA 剩余未搬运字节数（出错时估算已搬运前缀）。 */
WORD usart_port_tx_dma_remaining(void);

/* 打开 TXE 中断（TXE 回退路径的驱动源）。 */
void usart_port_tx_irq_enable(void);

/* TX 完成标志（最后一个停止位是否已离开引脚）。 */
BYTE usart_port_tx_complete(void);

/* 系统单调毫秒计数（flush 超时用；SysTick/HAL tick）。 */
DWORD usart_port_now_ms(void);

/* 内核内建包装：屏障 / 事件信号 / 事件等待。 */
void usart_port_memory_barrier(void);
void usart_port_event_signal(void);
void usart_port_event_wait(void);

/* 当前是否处于原子上下文（在 ISR 中或中断已屏蔽）。 */
BYTE usart_port_context_is_atomic(void);

/* 中断状态保存/关闭/恢复（PRIMASK）。 */
DWORD usart_port_irq_state(void);
void  usart_port_irq_disable(void);
void  usart_port_irq_restore(DWORD state);

#endif /* PLATFORM_USART_H */
