#ifndef PLATFORM_DMA_H
#define PLATFORM_DMA_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * 平台 DMA 抽象接口。
 *
 * 实现在 src/platform/<soc>/：dma.c（UART1 TX）、nand_dma.c（NAND/FMC 数据搬移）。
 * 本头零 HAL 类型：两段函数签名都只含裸类型，供核心模块（src/nand）与平台内部
 * （usart.c）共同以裸名 #include "dma.h" 消费。
 */

/* ── UART TX DMA（USART1；平台内部使用）── */

BYTE uart_tx_dma_init(void);
void uart_tx_dma_irq_handler(void);

/* port 接口：由 src/platform/<soc>/dma_port.c 实现；共享 src/platform/dma.c 只转发。 */
BYTE dma_port_uart_tx_init(void);
void dma_port_uart_tx_irq_handler(void);

/* ── NAND 数据搬移模式 ──
 *
 * CONFIG_NAND_DMA
 *   0 = PIO（默认），CPU 逐字节读写 FMC 寄存器。
 *   1 = DMA2 **memory-to-memory**（软件触发）：FMC NAND **没有专用 DMA
 *       请求线**（`nand_dma.c` 用 DMA_REQUEST_MEM2MEM），只是把 FMC data
 *       register 当普通源/目的地址搬；不改变每字节 FMC 访问本质。
 *
 * DMA 模式下 DATA_IN/DATA_OUT 阶段用 DMA 启动后 CPU 可处理其他事务，
 * engine step 轮询完成标志。Command/Address 阶段仍走 PIO。
 *
 * 运行时可通过全局变量 g_nand_dma_enable 动态切换。
 */
#ifndef CONFIG_NAND_DMA
#    define CONFIG_NAND_DMA (0U)
#endif

#if CONFIG_NAND_DMA

/*
 * DMA 传输结果枚举。
 */
typedef enum
{
    DMA_RESULT_OK    = 0U,
    DMA_RESULT_ERROR = 1U,
} nand_dma_result_e;

extern BYTE g_nand_dma_enable;

nand_dma_result_e nand_dma_init(void);

nand_dma_result_e nand_dma_memory_to_fmc(volatile BYTE *fmc_data_reg, const BYTE *buffer, WORD length);
nand_dma_result_e nand_dma_fmc_to_memory(volatile BYTE *fmc_data_reg, BYTE *buffer, WORD length);

nand_dma_result_e nand_dma_memory_to_fmc_start_IT(volatile BYTE *fmc_data_reg, const BYTE *buffer, WORD length);
nand_dma_result_e nand_dma_fmc_to_memory_start_IT(volatile BYTE *fmc_data_reg, BYTE *buffer, WORD length);

BYTE nand_dma_data_out_done(void);
BYTE nand_dma_data_in_done(void);
void nand_dma_clear_flags(void);
BYTE nand_dma_is_out_busy(void);
BYTE nand_dma_is_in_busy(void);
void nand_dma_abort_out(void);
void nand_dma_abort_in(void);
void nand_dma_irq_handler_data_out(void);
void nand_dma_irq_handler_data_in(void);
void nand_dma_cache_clean(const BYTE *buffer, WORD length);
void nand_dma_cache_invalidate(BYTE *buffer, WORD length);

#else /* !CONFIG_NAND_DMA */

/* DMA 未使能时：所有函数退化为空操作，g_nand_dma_enable 始终为 0。 */
#    define g_nand_dma_enable (0U)

/* 函数声明在 !CONFIG_NAND_DMA 时也保留，由编译隔离（nand_dma.c 整体跳过）或用户自行 stub。 */
typedef enum
{
    DMA_RESULT_OK    = 0U,
    DMA_RESULT_ERROR = 1U,
} nand_dma_result_e;

static inline nand_dma_result_e nand_dma_init(void)
{
    (void)0;
    return DMA_RESULT_OK;
}
static inline nand_dma_result_e nand_dma_memory_to_fmc(volatile BYTE *a, const BYTE *b, WORD c)
{
    (void)a;
    (void)b;
    (void)c;
    return DMA_RESULT_OK;
}
static inline nand_dma_result_e nand_dma_fmc_to_memory(volatile BYTE *a, BYTE *b, WORD c)
{
    (void)a;
    (void)b;
    (void)c;
    return DMA_RESULT_OK;
}
static inline nand_dma_result_e nand_dma_memory_to_fmc_start_IT(volatile BYTE *a, const BYTE *b, WORD c)
{
    (void)a;
    (void)b;
    (void)c;
    return DMA_RESULT_OK;
}
static inline nand_dma_result_e nand_dma_fmc_to_memory_start_IT(volatile BYTE *a, BYTE *b, WORD c)
{
    (void)a;
    (void)b;
    (void)c;
    return DMA_RESULT_OK;
}
static inline void nand_dma_clear_flags(void)
{
}
static inline BYTE nand_dma_data_out_done(void)
{
    return 0U;
}
static inline BYTE nand_dma_data_in_done(void)
{
    return 0U;
}
static inline BYTE nand_dma_is_out_busy(void)
{
    return 0U;
}
static inline BYTE nand_dma_is_in_busy(void)
{
    return 0U;
}
static inline void nand_dma_abort_out(void)
{
}
static inline void nand_dma_abort_in(void)
{
}
static inline void nand_dma_irq_handler_data_out(void)
{
}
static inline void nand_dma_irq_handler_data_in(void)
{
}
static inline void nand_dma_cache_clean(const BYTE *buffer, WORD length)
{
    (void)buffer;
    (void)length;
}
static inline void nand_dma_cache_invalidate(BYTE *buffer, WORD length)
{
    (void)buffer;
    (void)length;
}

#endif /* CONFIG_NAND_DMA */

#endif /* PLATFORM_DMA_H */
