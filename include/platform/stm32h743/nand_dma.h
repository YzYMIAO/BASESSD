#ifndef PLATFORM_STM32H743_NAND_DMA_H
#define PLATFORM_STM32H743_NAND_DMA_H

#include "nand/nand_config.h"

#include <stdint.h>

#if CONFIG_NAND_DMA

/*
 * DMA 传输结果枚举。
 */
typedef enum
{
    DMA_RESULT_OK    = 0U,
    DMA_RESULT_ERROR = 1U,
} nand_dma_result_t;

extern uint8_t g_nand_dma_enable;

nand_dma_result_t nand_dma_init(void);

nand_dma_result_t nand_dma_memory_to_fmc(volatile uint8_t *fmc_data_reg,
                                         const uint8_t    *buffer,
                                         uint16_t          length);
nand_dma_result_t nand_dma_fmc_to_memory(volatile uint8_t *fmc_data_reg,
                                         uint8_t          *buffer,
                                         uint16_t          length);

nand_dma_result_t nand_dma_memory_to_fmc_start_IT(volatile uint8_t *fmc_data_reg,
                                                  const uint8_t    *buffer,
                                                  uint16_t          length);
nand_dma_result_t nand_dma_fmc_to_memory_start_IT(volatile uint8_t *fmc_data_reg,
                                                  uint8_t          *buffer,
                                                  uint16_t          length);

uint8_t nand_dma_data_out_done(void);
uint8_t nand_dma_data_in_done(void);
void    nand_dma_clear_flags(void);
uint8_t nand_dma_is_out_busy(void);
uint8_t nand_dma_is_in_busy(void);
void    nand_dma_abort_out(void);
void    nand_dma_abort_in(void);
void    nand_dma_irq_handler_data_out(void);
void    nand_dma_irq_handler_data_in(void);
void    nand_dma_cache_clean(const uint8_t *buffer, uint16_t length);
void    nand_dma_cache_invalidate(uint8_t *buffer, uint16_t length);

#else /* !CONFIG_NAND_DMA */

/* DMA 未使能时：所有函数退化为空操作，g_nand_dma_enable 始终为 0。 */
#    define g_nand_dma_enable (0U)

/* 函数声明在 !CONFIG_NAND_DMA 时也保留，由编译隔离（nand_dma.c 整体跳过）或用户自行 stub。 */
typedef enum
{
    DMA_RESULT_OK    = 0U,
    DMA_RESULT_ERROR = 1U,
} nand_dma_result_t;

static inline nand_dma_result_t nand_dma_init(void)
{
    (void)0;
    return DMA_RESULT_OK;
}
static inline nand_dma_result_t nand_dma_memory_to_fmc(volatile uint8_t *a, const uint8_t *b, uint16_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return DMA_RESULT_OK;
}
static inline nand_dma_result_t nand_dma_fmc_to_memory(volatile uint8_t *a, uint8_t *b, uint16_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return DMA_RESULT_OK;
}
static inline nand_dma_result_t nand_dma_memory_to_fmc_start_IT(volatile uint8_t *a, const uint8_t *b, uint16_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return DMA_RESULT_OK;
}
static inline nand_dma_result_t nand_dma_fmc_to_memory_start_IT(volatile uint8_t *a, uint8_t *b, uint16_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return DMA_RESULT_OK;
}
static inline void nand_dma_clear_flags(void)
{
}
static inline uint8_t nand_dma_data_out_done(void)
{
    return 0U;
}
static inline uint8_t nand_dma_data_in_done(void)
{
    return 0U;
}
static inline uint8_t nand_dma_is_out_busy(void)
{
    return 0U;
}
static inline uint8_t nand_dma_is_in_busy(void)
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
static inline void nand_dma_cache_clean(const uint8_t *buffer, uint16_t length)
{
    (void)buffer;
    (void)length;
}
static inline void nand_dma_cache_invalidate(uint8_t *buffer, uint16_t length)
{
    (void)buffer;
    (void)length;
}

#endif /* CONFIG_NAND_DMA */

#endif /* PLATFORM_STM32H743_NAND_DMA_H */