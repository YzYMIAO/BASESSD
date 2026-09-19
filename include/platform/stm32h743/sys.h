#ifndef PLATFORM_STM32H743_SYS_H
#define PLATFORM_STM32H743_SYS_H

#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"

#include <stdint.h>

#define SYS_SUPPORT_OS 0U

void    sys_init(void);
void    sys_mpu_config(void);
void    sys_cache_enable(void);
uint8_t get_icache_sta(void);
uint8_t get_dcache_sta(void);
void    sys_nvic_set_vector_table(uint32_t base_address, uint32_t offset);
void    sys_wfi_set(void);
void    sys_intx_disable(void);
void    sys_intx_enable(void);
void    sys_msr_msp(uint32_t address);

/* 参数顺序：预分频、倍频、SYSCLK分频、外设输出分频；HSE=25 MHz 时传入 5,160,2,4 得到 CPU=400 MHz。 */
uint8_t sys_stm32_clock_init(uint32_t pll_prediv, uint32_t pll_mul, uint32_t sysclk_div, uint32_t peripheral_div);

#endif /* PLATFORM_STM32H743_SYS_H */
