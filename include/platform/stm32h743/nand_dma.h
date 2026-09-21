#ifndef PLATFORM_STM32H743_NAND_DMA_H
#define PLATFORM_STM32H743_NAND_DMA_H

#include <stdint.h>

/*
 * FMC NAND DMA 驱动。
 *
 * STM32H743 的 FMC NAND 接口没有专用硬件 DMA 请求线（如 USART/SPI 的
 * DMA_REQUEST_xxx）。NAND 数据寄存器通过内存映射访问（FMC 地址空间），
 * 因此 DMA 传输使用 memory-to-memory 模式，由软件触发。
 *
 * 使用独立 DMA2 Stream (不与 UART TX 的 DMA1_Stream0 冲突)：
 *   - DMA2_Stream0: FMC NAND DATA IN  (memory → FMC data reg)
 *   - DMA2_Stream1: FMC NAND DATA OUT (FMC data reg → memory)
 *
 * DMA 传输前/后调用者必须处理 D-Cache 一致性：
 *   - DATA_IN (CPU写→DMA读→FMC发): 前 Clean D-Cache
 *   - DATA_OUT (FMC收→DMA写→CPU读): 后 Invalidate D-Cache
 *
 * 当前实现使用轮询等待 DMA 完成（PollForTransfer）。
 * 后续可扩展为中断驱动 → callback → nand_engine_step 恢复。
 */

uint8_t nand_dma_init(void);

/*
 * 使用 DMA 将数据从 memory 传输到 FMC NAND 数据寄存器（PROGRAM 方向）。
 * 等价于 CPU for 循环写入 *config->data = buffer[i]。
 *
 * @param fmc_data_reg  FMC NAND 数据寄存器地址 (MT29F4G08ABADAWP_FMC_BASE_ADDR)
 * @param buffer        源数据缓冲区（必须 D-Cache 已 Clean）
 * @param length        传输字节数
 * @return              0=成功, 1=失败
 */
uint8_t nand_dma_memory_to_fmc(volatile uint8_t *fmc_data_reg,
                                const uint8_t *buffer, uint16_t length);

/*
 * 使用 DMA 将数据从 FMC NAND 数据寄存器传输到 memory（READ 方向）。
 * 等价于 CPU for 循环读取 buffer[i] = *config->data。
 *
 * @param fmc_data_reg  FMC NAND 数据寄存器地址
 * @param buffer        目标数据缓冲区（传输后需要 D-Cache Invalidate）
 * @param length        传输字节数
 * @return              0=成功, 1=失败
 */
uint8_t nand_dma_fmc_to_memory(volatile uint8_t *fmc_data_reg,
                                uint8_t *buffer, uint16_t length);

/*
 * DMA 传输前 Clean D-Cache 缓冲区，确保 DMA 看到最新数据。
 * DATA_IN (memory→FMC) 前调用。
 */
void nand_dma_cache_clean(const uint8_t *buffer, uint16_t length);

/*
 * DMA 传输后 Invalidate D-Cache 缓冲区，确保 CPU 读取时从主存加载。
 * DATA_OUT (FMC→memory) 后调用。
 */
void nand_dma_cache_invalidate(uint8_t *buffer, uint16_t length);

#endif /* PLATFORM_STM32H743_NAND_DMA_H */