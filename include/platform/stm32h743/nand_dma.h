#ifndef PLATFORM_STM32H743_NAND_DMA_H
#define PLATFORM_STM32H743_NAND_DMA_H

#include <stdint.h>

/*
 * DMA 传输结果枚举。
 */
typedef enum
{
    DMA_RESULT_OK       = 0U,
    DMA_RESULT_ERROR    = 1U,
} nand_dma_result_t;

/*
 * FMC NAND DMA 驱动。
 *
 * STM32H743 的 FMC NAND 接口没有专用硬件 DMA 请求线（如 USART/SPI 的
 * DMA_REQUEST_xxx）。NAND 数据寄存器通过内存映射访问（FMC 地址空间），
 * 因此 DMA 传输使用 memory-to-memory 模式，由软件触发。
 *
 * 使用独立 DMA2 Stream (不与 UART TX 的 DMA1_Stream0 冲突)：
 *   - DMA2_Stream0: FMC NAND DATA IN  (memory → FMC data reg, PROGRAM)
 *   - DMA2_Stream1: FMC NAND DATA OUT (FMC data reg → memory, READ)
 *
 * DMA 传输前/后调用者必须处理 D-Cache 一致性：
 *   - DATA_IN (CPU写→DMA读→FMC发): 前 Clean D-Cache
 *   - DATA_OUT (FMC收→DMA写→CPU读): 后 Invalidate D-Cache
 *
 * 传输模式：
 *   - 轮询版 (nand_dma_memory_to_fmc / nand_dma_fmc_to_memory):
 *     同步阻塞，DMA 期间 CPU 空转。
 *   - 中断版 (nand_dma_*_start_IT + nand_dma_*_done):
 *     非阻塞，启动后返回，完成时 ISR 设标志，handler 轮询检查。
 *     中断版失败可回退到轮询版或 PIO。
 */

nand_dma_result_t nand_dma_init(void);

/* ── 轮询版（保留做后备） ── */

nand_dma_result_t nand_dma_memory_to_fmc(volatile uint8_t *fmc_data_reg,
                                const uint8_t *buffer, uint16_t length);
nand_dma_result_t nand_dma_fmc_to_memory(volatile uint8_t *fmc_data_reg,
                                uint8_t *buffer, uint16_t length);

/* ── 中断版（非阻塞） ── */

/*
 * 非阻塞启动 DMA（memory → FMC，PROGRAM 方向）。
 * 启动后立即返回，CPU 可处理其他事务。
 * 后续调用 nand_dma_data_out_done() 检查完成标志。
 */
nand_dma_result_t nand_dma_memory_to_fmc_start_IT(volatile uint8_t *fmc_data_reg,
                                          const uint8_t *buffer, uint16_t length);

/*
 * 非阻塞启动 DMA（FMC → memory，READ 方向）。
 * 启动后立即返回。
 * 后续调用 nand_dma_data_in_done() 检查完成标志。
 * 完成后需调用 nand_dma_cache_invalidate() 保证 D-Cache 一致性。
 */
nand_dma_result_t nand_dma_fmc_to_memory_start_IT(volatile uint8_t *fmc_data_reg,
                                          uint8_t *buffer, uint16_t length);

/* DMA 传输是否已完成（0=未完成, 1=已完成） */
uint8_t nand_dma_data_out_done(void);
uint8_t nand_dma_data_in_done(void);

/* DMAC 中断服务入口（由 stm32h7rsxx_it.c 的 DMA2_Stream0/1_IRQHandler 调用） */
void nand_dma_irq_handler_data_out(void);
void nand_dma_irq_handler_data_in(void);

/* ── Cache 维护工具 ── */

void nand_dma_cache_clean(const uint8_t *buffer, uint16_t length);
void nand_dma_cache_invalidate(uint8_t *buffer, uint16_t length);

#endif /* PLATFORM_STM32H743_NAND_DMA_H */