#ifndef PLATFORM_SYS_H
#define PLATFORM_SYS_H

#include <stdint.h>

/*
 * 平台系统服务抽象接口（共享逻辑在 src/platform/sys.c，SoC 适配在
 * src/platform/<soc>/sys_port.c）。
 *
 * 本头零 HAL 类型：厂商设备头、寄存器位、内核内建（SCB / PRIMASK / MSP / WFI）
 * 一律留在 SoC 侧（include/platform/<soc>/sys_port.h）。共享层只做启动编排与转发。
 */

/* ─────────────────────────── 公开系统接口 ─────────────────────────── */

/* 上电初始化序列：MPU → Cache → HAL → 时钟 → delay → 串口 → 故障捕获。
 * 时钟配置失败即死循环（保持原行为）。 */
void sys_init(void);

void sys_mpu_config(void);
void sys_cache_enable(void);

uint8_t  get_icache_sta(void);
uint8_t  get_dcache_sta(void);
uint32_t sys_mpu_region_count(void);

/* 时钟树快照（单位 Hz）。各字段由 SoC 侧经 HAL 实读回读；共享层只转发，
 * 不解析任何 RCC 寄存器。FMC / USART1 两项是 NAND 时序与控制台波特率的基准。 */
typedef struct
{
    uint32_t sysclk_hz; /* SYSCLK / CPU */
    uint32_t hclk_hz;   /* AHB */
    uint32_t pclk1_hz;  /* APB1 */
    uint32_t pclk2_hz;  /* APB2 */
    uint32_t pclk3_hz;  /* APB3 */
    uint32_t pclk4_hz;  /* APB4 */
    uint32_t fmc_hz;    /* FMC 内核时钟 */
    uint32_t qspi_hz;   /* QSPI 内核时钟 */
    uint32_t uart_hz;   /* USART1 内核时钟 */
} sys_clock_tree_t;

void sys_clock_tree_get(sys_clock_tree_t *out);

void sys_nvic_set_vector_table(uint32_t base_address, uint32_t offset);
void sys_wfi_set(void);
void sys_intx_disable(void);
void sys_intx_enable(void);
void sys_msr_msp(uint32_t address);

/* 参数顺序：预分频、倍频、SYSCLK分频、外设输出分频；HSE=25 MHz 时传入 5,160,2,4 得到 CPU=400 MHz。
 * 返回 0 = 成功（FUNC_STATUS_SUCCESS 语义），非 0 = 失败。 */
uint8_t sys_stm32_clock_init(uint32_t pll_prediv, uint32_t pll_mul, uint32_t sysclk_div, uint32_t peripheral_div);

/* ─────────────────────────── port 接口 ───────────────────────────
 * 由 src/platform/<soc>/sys_port.c 实现；共享层只通过这些函数触碰 SoC。
 */

void     sys_port_mpu_config(void);
void     sys_port_cache_enable(void);
uint8_t  sys_port_icache_enabled(void);
uint8_t  sys_port_dcache_enabled(void);
uint32_t sys_port_mpu_region_count(void);
void     sys_port_clock_tree_get(sys_clock_tree_t *out);
void     sys_port_set_vector_table(uint32_t base_address, uint32_t offset);
void     sys_port_wfi(void);
void     sys_port_irq_disable(void);
void     sys_port_irq_enable(void);
void     sys_port_set_msp(uint32_t address);
void     sys_port_hal_init(void);
uint8_t  sys_port_clock_init(uint32_t pll_prediv, uint32_t pll_mul, uint32_t sysclk_div, uint32_t peripheral_div);

#endif /* PLATFORM_SYS_H */
