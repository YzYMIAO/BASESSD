#include "common.h"
#include "dma.h" /* 抽象头提供 CONFIG_NAND_DMA 默认值，必须在 #if 之前 */

#if CONFIG_NAND_DMA

#    include "stm32h743xx.h"
#    include "stm32h7xx_hal.h"

#    include <stddef.h>

/*
 * FMC NAND DMA 驱动实现。
 *
 * DMA 配置要点：
 *   - FMC NAND 没有专用 DMA 请求线，使用 memory-to-memory 模式软件触发
 *   - 使用 DMA2（不与 UART TX 的 DMA1 冲突）
 *   - 每次传输前 Clean D-Cache，传输后 Invalidate D-Cache
 *
 * Cache 维护说明（STM32H743 D-Cache 行大小 = 32 字节）：
 *   - SCB_CleanDCache_by_Addr() 以 32 字节对齐的地址和大小操作
 *   - 传入的 buffer 可能未对齐，需对齐到 32 字节边界
 *   - 实际 buffer 在 FC Buffer Pool 中，起始地址由链接器决定
 *
 * 传输模式：
 *   轮询版：HAL_DMA_PollForTransfer 同步等待，DMA 期间 CPU 空转。
 *   中断版：HAL_DMA_Start_IT 非阻塞启动，ISR 设完成标志。
 *           handler 在后续 step 中检查标志，步进逻辑与 WAIT_READY 一致。
 */

/* DMA2 stream handles — 不使用全局 g_dma_handle 避免与 UART 冲突 */
static DMA_HandleTypeDef g_nand_dma_in;  /* DMA2_Stream1: FMC→memory (READ)  */
static DMA_HandleTypeDef g_nand_dma_out; /* DMA2_Stream0: memory→FMC (PROGRAM) */

/* ── 中断完成标志（volatile 防编译器优化） ── */
static volatile BYTE g_dma_data_out_done; /* PROGRAM 方向 DMA 完成标志 */
static volatile BYTE g_dma_data_in_done;  /* READ 方向 DMA 完成标志 */

/* 运行态 DMA 使能标志。默认等于 CONFIG_NAND_DMA。
 * 可在 nand 空闲时动态切换。 */
BYTE g_nand_dma_enable = CONFIG_NAND_DMA;

/*
 * HAL_DMA_MspInit — DMA 硬件平台初始化（弱函数重写）。
 *
 * HAL_DMA_Init() 内部自动调用本函数。这里不调 HAL_NVIC 是为了和中断版
 * DMA 一致（ISR 已注册），轮询版没有 MspInit 也能工作但时钟必须打开。
 */
void HAL_DMA_MspInit(DMA_HandleTypeDef *hdma)
{
    static BYTE dma2_clock_enabled = 0U;

    if (dma2_clock_enabled == 0U)
    {
        __HAL_RCC_DMA2_CLK_ENABLE();
        dma2_clock_enabled = 1U;
    }

    if (hdma->Instance == DMA2_Stream0)
    {
        HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5U, 0U);
        HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    }
    else if (hdma->Instance == DMA2_Stream1)
    {
        HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 5U, 0U);
        HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
    }
}

static nand_dma_result_t nand_dma_init_stream(DMA_HandleTypeDef *handle, DMA_Stream_TypeDef *instance)
{
    handle->Instance                 = instance;
    handle->Init.Request             = DMA_REQUEST_MEM2MEM;
    handle->Init.Direction           = DMA_MEMORY_TO_MEMORY;
    handle->Init.PeriphInc           = DMA_PINC_DISABLE; /* FMC data reg 不自增 */
    handle->Init.MemInc              = DMA_MINC_ENABLE;  /* buffer 自增 */
    handle->Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    handle->Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    handle->Init.Mode                = DMA_NORMAL;
    handle->Init.Priority            = DMA_PRIORITY_HIGH;
    handle->Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    handle->Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    handle->Init.MemBurst            = DMA_MBURST_SINGLE;
    handle->Init.PeriphBurst         = DMA_PBURST_SINGLE;

    return (HAL_DMA_Init(handle) == HAL_OK) ? DMA_RESULT_OK : DMA_RESULT_ERROR;
}

nand_dma_result_t nand_dma_init(void)
{
    if ((nand_dma_init_stream(&g_nand_dma_out, DMA2_Stream0) != 0U) ||
        (nand_dma_init_stream(&g_nand_dma_in, DMA2_Stream1) != 0U))
    {
        return DMA_RESULT_ERROR;
    }

    /* 清初始标志 */
    g_dma_data_out_done = 0U;
    g_dma_data_in_done  = 0U;

    return DMA_RESULT_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 轮询版（保留做后备，DMA 失败时回退到 PIO 前使用）
 * ══════════════════════════════════════════════════════════════════ */

nand_dma_result_t nand_dma_memory_to_fmc(volatile BYTE *fmc_data_reg,
                                         const BYTE    *buffer,
                                         WORD           length)
{
    if ((fmc_data_reg == NULL) || (buffer == NULL) || (length == 0U))
    {
        return DMA_RESULT_ERROR;
    }

    /* DMA 前 Clean D-Cache，让 DMA 看到 buffer 的最新数据 */
    nand_dma_cache_clean(buffer, length);

    /* 启动 DMA: memory → FMC data reg (memory-to-memory) */
    if (HAL_DMA_Start(&g_nand_dma_out,
                      (DWORD)buffer,
                      (DWORD)fmc_data_reg,
                      length) != HAL_OK)
    {
        return DMA_RESULT_ERROR;
    }

    /* 轮询等待完成 */
    if (HAL_DMA_PollForTransfer(&g_nand_dma_out, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY) != HAL_OK)
    {
        return DMA_RESULT_ERROR;
    }

    return DMA_RESULT_OK;
}

nand_dma_result_t nand_dma_fmc_to_memory(volatile BYTE *fmc_data_reg,
                                         BYTE          *buffer,
                                         WORD           length)
{
    if ((fmc_data_reg == NULL) || (buffer == NULL) || (length == 0U))
    {
        return DMA_RESULT_ERROR;
    }

    /* 启动 DMA: FMC data reg → memory (memory-to-memory) */
    if (HAL_DMA_Start(&g_nand_dma_in,
                      (DWORD)fmc_data_reg,
                      (DWORD)buffer,
                      length) != HAL_OK)
    {
        return DMA_RESULT_ERROR;
    }

    /* 轮询等待完成 */
    if (HAL_DMA_PollForTransfer(&g_nand_dma_in, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY) != HAL_OK)
    {
        return DMA_RESULT_ERROR;
    }

    /* DMA 后 Invalidate D-Cache，让 CPU 从主存读取 */
    nand_dma_cache_invalidate(buffer, length);

    return DMA_RESULT_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 中断版（非阻塞）
 * ══════════════════════════════════════════════════════════════════ */

nand_dma_result_t nand_dma_memory_to_fmc_start_IT(volatile BYTE *fmc_data_reg,
                                                  const BYTE    *buffer,
                                                  WORD           length)
{
    if ((fmc_data_reg == NULL) || (buffer == NULL) || (length == 0U))
    {
        return DMA_RESULT_ERROR;
    }

    /* DMA 前 Clean D-Cache */
    nand_dma_cache_clean(buffer, length);

    g_dma_data_out_done = 0U;

    if (HAL_DMA_Start_IT(&g_nand_dma_out,
                         (DWORD)buffer,
                         (DWORD)fmc_data_reg,
                         length) != HAL_OK)
    {
        return DMA_RESULT_ERROR;
    }

    return DMA_RESULT_OK;
}

nand_dma_result_t nand_dma_fmc_to_memory_start_IT(volatile BYTE *fmc_data_reg,
                                                  BYTE          *buffer,
                                                  WORD           length)
{
    if ((fmc_data_reg == NULL) || (buffer == NULL) || (length == 0U))
    {
        return DMA_RESULT_ERROR;
    }

    g_dma_data_in_done = 0U;

    if (HAL_DMA_Start_IT(&g_nand_dma_in,
                         (DWORD)fmc_data_reg,
                         (DWORD)buffer,
                         length) != HAL_OK)
    {
        return DMA_RESULT_ERROR;
    }

    return DMA_RESULT_OK;
}

BYTE nand_dma_data_out_done(void)
{
    return g_dma_data_out_done;
}

BYTE nand_dma_data_in_done(void)
{
    return g_dma_data_in_done;
}

void nand_dma_clear_flags(void)
{
    g_dma_data_out_done = 0U;
    g_dma_data_in_done  = 0U;
}

BYTE nand_dma_is_out_busy(void)
{
    /* 读 DMA2 Stream0 CR.EN 位 — 1 = 传输进行中 */
    return (DMA2_Stream0->CR & DMA_SxCR_EN) != 0U ? 1U : 0U;
}

BYTE nand_dma_is_in_busy(void)
{
    /* 读 DMA2 Stream1 CR.EN 位 — 1 = 传输进行中 */
    return (DMA2_Stream1->CR & DMA_SxCR_EN) != 0U ? 1U : 0U;
}

void nand_dma_abort_out(void)
{
    if (HAL_DMA_Abort(&g_nand_dma_out) != HAL_OK)
    {
        /* Abort 失败时强制停止，不阻塞 */
        (void)HAL_DMA_Abort_IT(&g_nand_dma_out);
    }
    g_dma_data_out_done = 0U;
}

void nand_dma_abort_in(void)
{
    if (HAL_DMA_Abort(&g_nand_dma_in) != HAL_OK)
    {
        (void)HAL_DMA_Abort_IT(&g_nand_dma_in);
    }
    g_dma_data_in_done = 0U;
}

/*
 * DMA2_Stream0 传输完成处理（PROGRAM 方向：memory→FMC）。
 * 由本文件的 DMA2_Stream0_IRQHandler() 调用。
 */
void nand_dma_irq_handler_data_out(void)
{
    /*
     * 在 HAL_DMA_IRQHandler 清除标志前先捕获 TCIF。
     * STM32H7 HAL 的 IRQHandler 内部会调用 __HAL_DMA_CLEAR_FLAG，
     * 如果在之后才检查标志就永远读不到了。
     */
    DWORD tcif = __HAL_DMA_GET_FLAG(&g_nand_dma_out, DMA_FLAG_TCIF0_4);

    HAL_DMA_IRQHandler(&g_nand_dma_out);

    if (tcif != 0U)
    {
        /* 不重复清标志（HAL 已清），只设软件完成标志 */
        g_dma_data_out_done = 1U;
    }
}

/*
 * DMA2_Stream1 传输完成处理（READ 方向：FMC→memory）。
 * 由本文件的 DMA2_Stream1_IRQHandler() 调用。
 *
 * READ 方向需要在 ISR 中 Invalidate D-Cache，
 * 因为 DMA 写入 buffer 后 D-Cache 中的副本已过时，
 * CPU 下一次读 buffer 时必须从主存加载。
 */
void nand_dma_irq_handler_data_in(void)
{
    /* 同 data_out：HAL 清除标志前先捕获 */
    DWORD tcif = __HAL_DMA_GET_FLAG(&g_nand_dma_in, DMA_FLAG_TCIF1_5);

    HAL_DMA_IRQHandler(&g_nand_dma_in);

    if (tcif != 0U)
    {
        g_dma_data_in_done = 1U;
    }
}

/* ─────────────────────────── 向量表入口 ─────────────────────────── */

/*
 * DMA2_Stream0 中断入口（向量表名由 startup_stm32h743xx.s 固定）。
 * 本文件即该向量的归属地：NAND PROGRAM 方向（memory→FMC）的 DMA 完成。
 */
void DMA2_Stream0_IRQHandler(void)
{
    nand_dma_irq_handler_data_out();
}

/*
 * DMA2_Stream1 中断入口（向量表名由 startup_stm32h743xx.s 固定）。
 * 本文件即该向量的归属地：NAND READ 方向（FMC→memory）的 DMA 完成。
 */
void DMA2_Stream1_IRQHandler(void)
{
    nand_dma_irq_handler_data_in();
}

/* ══════════════════════════════════════════════════════════════════
 * D-Cache 维护（轮询版和中断版共用）
 * ══════════════════════════════════════════════════════════════════ */

void nand_dma_cache_clean(const BYTE *buffer, WORD length)
{
#    if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    /*
     * SCB_CleanDCache_by_Addr 需要 32 字节对齐的地址和大小（向上取整）。
     * FC Buffer Pool 中的 buffer 起始地址已对齐（528 = 16×33，16 字节对齐），
     * 但 D-Cache 行是 32 字节，用当前地址向下取整，长度向上取整到 32 倍数。
     */
    DWORD addr         = (DWORD)buffer;
    DWORD aligned_addr = addr & ~0x1FUL;
    DWORD aligned_len  = ((addr - aligned_addr) + length + 31U) & ~0x1FUL;
    SCB_CleanDCache_by_Addr((void *)aligned_addr, (int32_t)aligned_len);
#    else
    (void)buffer;
    (void)length;
#    endif
}

void nand_dma_cache_invalidate(BYTE *buffer, WORD length)
{
#    if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    DWORD addr         = (DWORD)buffer;
    DWORD aligned_addr = addr & ~0x1FUL;
    DWORD aligned_len  = ((addr - aligned_addr) + length + 31U) & ~0x1FUL;
    SCB_InvalidateDCache_by_Addr((void *)aligned_addr, (int32_t)aligned_len);
#    else
    (void)buffer;
    (void)length;
#    endif
}

#endif /* CONFIG_NAND_DMA */
