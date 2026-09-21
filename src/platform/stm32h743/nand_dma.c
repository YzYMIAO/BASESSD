#include "nand_dma.h"

#include "stm32h7xx_hal.h"
#include "stm32h743xx.h"

#include <stddef.h>

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
 */

/* DMA2 stream handles — 不使用全局 g_dma_handle 避免与 UART 冲突 */
static DMA_HandleTypeDef g_nand_dma_in;   /* DMA2_Stream1: FMC→memory (READ)  */
static DMA_HandleTypeDef g_nand_dma_out;  /* DMA2_Stream0: memory→FMC (PROGRAM) */

uint8_t nand_dma_init(void)
{
    /* ── DATA_IN 方向: memory → FMC (PROGRAM) ── */
    g_nand_dma_out.Instance                 = DMA2_Stream0;
    g_nand_dma_out.Init.Request             = DMA_REQUEST_MEM2MEM;
    g_nand_dma_out.Init.Direction           = DMA_MEMORY_TO_MEMORY;
    g_nand_dma_out.Init.PeriphInc           = DMA_PINC_DISABLE;       /* FMC data reg 不自增 */
    g_nand_dma_out.Init.MemInc              = DMA_MINC_ENABLE;         /* buffer 自增 */
    g_nand_dma_out.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    g_nand_dma_out.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    g_nand_dma_out.Init.Mode                = DMA_NORMAL;
    g_nand_dma_out.Init.Priority            = DMA_PRIORITY_HIGH;
    g_nand_dma_out.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    g_nand_dma_out.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    g_nand_dma_out.Init.MemBurst            = DMA_MBURST_SINGLE;
    g_nand_dma_out.Init.PeriphBurst         = DMA_PBURST_SINGLE;

    if (HAL_DMA_Init(&g_nand_dma_out) != HAL_OK)
    {
        return 1U;
    }

    /* ── DATA_OUT 方向: FMC → memory (READ) ── */
    g_nand_dma_in.Instance                  = DMA2_Stream1;
    g_nand_dma_in.Init.Request              = DMA_REQUEST_MEM2MEM;
    g_nand_dma_in.Init.Direction            = DMA_MEMORY_TO_MEMORY;
    g_nand_dma_in.Init.PeriphInc            = DMA_PINC_DISABLE;
    g_nand_dma_in.Init.MemInc               = DMA_MINC_ENABLE;
    g_nand_dma_in.Init.PeriphDataAlignment  = DMA_PDATAALIGN_BYTE;
    g_nand_dma_in.Init.MemDataAlignment     = DMA_MDATAALIGN_BYTE;
    g_nand_dma_in.Init.Mode                 = DMA_NORMAL;
    g_nand_dma_in.Init.Priority             = DMA_PRIORITY_HIGH;
    g_nand_dma_in.Init.FIFOMode             = DMA_FIFOMODE_DISABLE;
    g_nand_dma_in.Init.FIFOThreshold        = DMA_FIFO_THRESHOLD_FULL;
    g_nand_dma_in.Init.MemBurst             = DMA_MBURST_SINGLE;
    g_nand_dma_in.Init.PeriphBurst          = DMA_PBURST_SINGLE;

    if (HAL_DMA_Init(&g_nand_dma_in) != HAL_OK)
    {
        /* 清理已初始化的 out */
        HAL_DMA_DeInit(&g_nand_dma_out);
        return 1U;
    }

    return 0U;
}

uint8_t nand_dma_memory_to_fmc(volatile uint8_t *fmc_data_reg,
                                const uint8_t *buffer, uint16_t length)
{
    if ((fmc_data_reg == NULL) || (buffer == NULL) || (length == 0U))
    {
        return 1U;
    }

    /* DMA 前 Clean D-Cache，让 DMA 看到 buffer 的最新数据 */
    nand_dma_cache_clean(buffer, length);

    /* 启动 DMA: memory → FMC data reg (memory-to-memory) */
    if (HAL_DMA_Start(&g_nand_dma_out,
                      (uint32_t)buffer,
                      (uint32_t)fmc_data_reg,
                      length) != HAL_OK)
    {
        return 1U;
    }

    /* 轮询等待完成（后续改为中断驱动） */
    if (HAL_DMA_PollForTransfer(&g_nand_dma_out, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY) != HAL_OK)
    {
        return 1U;
    }

    return 0U;
}

uint8_t nand_dma_fmc_to_memory(volatile uint8_t *fmc_data_reg,
                                uint8_t *buffer, uint16_t length)
{
    if ((fmc_data_reg == NULL) || (buffer == NULL) || (length == 0U))
    {
        return 1U;
    }

    /* 启动 DMA: FMC data reg → memory (memory-to-memory) */
    if (HAL_DMA_Start(&g_nand_dma_in,
                      (uint32_t)fmc_data_reg,
                      (uint32_t)buffer,
                      length) != HAL_OK)
    {
        return 1U;
    }

    /* 轮询等待完成 */
    if (HAL_DMA_PollForTransfer(&g_nand_dma_in, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY) != HAL_OK)
    {
        return 1U;
    }

    /* DMA 后 Invalidate D-Cache，让 CPU 从主存读取 */
    nand_dma_cache_invalidate(buffer, length);

    return 0U;
}

void nand_dma_cache_clean(const uint8_t *buffer, uint16_t length)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    /*
     * SCB_CleanDCache_by_Addr 需要 32 字节对齐的地址和大小（向上取整）。
     * FC Buffer Pool 中的 buffer 起始地址已对齐（528 = 16×33，16 字节对齐），
     * 但 D-Cache 行是 32 字节，用当前地址向下取整，长度向上取整到 32 倍数。
     */
    uint32_t addr = (uint32_t)buffer;
    uint32_t aligned_addr = addr & ~0x1FUL;
    uint32_t aligned_len = (uint32_t)((addr - aligned_addr) + length + 31U) & ~0x1FUL;
    SCB_CleanDCache_by_Addr((void *)aligned_addr, (int32_t)aligned_len);
#else
    (void)buffer;
    (void)length;
#endif
}

void nand_dma_cache_invalidate(uint8_t *buffer, uint16_t length)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    uint32_t addr = (uint32_t)buffer;
    uint32_t aligned_addr = addr & ~0x1FUL;
    uint32_t aligned_len = (uint32_t)((addr - aligned_addr) + length + 31U) & ~0x1FUL;
    SCB_InvalidateDCache_by_Addr((void *)aligned_addr, (int32_t)aligned_len);
#else
    (void)buffer;
    (void)length;
#endif
}