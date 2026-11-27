#ifndef PLATFORM_SYS_H
#define PLATFORM_SYS_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * 平台系统服务抽象接口（共享逻辑在 src/platform/sys.c，SoC 适配在
 * src/platform/<soc>/sys_port.c）。
 *
 * 本头零 HAL 类型：厂商设备头、寄存器位、内核内建（SCB / PRIMASK / MSP / WFI）
 * 一律留在 SoC 侧（include/platform/<soc>/sys_port.h）。共享层只做启动编排与转发。
 */

/* ─────────────────────────── 公开系统接口 ─────────────────────────── */

/**
 * @brief 上电初始化序列：MPU → Cache → HAL → 时钟 → delay → 串口 → 故障捕获。
 *
 * 任一步失败立即返回，由调用者决定停机与报告；不再在共享层内部死循环。
 *
 * @param console_baud 控制台波特率（板级常量由 board_config.h 提供）。
 * @return FUNC_STATUS_SUCCESS 全部完成；FUNC_STATUS_ERROR 时钟或串口初始化失败。
 */
func_status_e sys_init(DWORD console_baud);

void sys_mpu_config(void);
void sys_cache_enable(void);

BYTE  sys_icache_enabled(void);
BYTE  sys_dcache_enabled(void);
DWORD sys_mpu_region_count(void);

/* 时钟树快照（单位 Hz）。各字段由 SoC 侧经 HAL 实读回读；共享层只转发，
 * 不解析任何 RCC 寄存器。FMC / USART1 两项是 NAND 时序与控制台波特率的基准。 */
typedef struct SYS_CLOCK_TREE_S
{
    DWORD sysclk_hz; /* SYSCLK / CPU */
    DWORD hclk_hz;   /* AHB */
    DWORD pclk1_hz;  /* APB1 */
    DWORD pclk2_hz;  /* APB2 */
    DWORD pclk3_hz;  /* APB3 */
    DWORD pclk4_hz;  /* APB4 */
    DWORD fmc_hz;    /* FMC 内核时钟 */
    DWORD qspi_hz;   /* QSPI 内核时钟 */
    DWORD usart_hz;  /* USART1 内核时钟 */
} sys_clock_tree_s;

void sys_clock_tree_get(sys_clock_tree_s *out);

/* ─────────────────────────── port 接口 ───────────────────────────
 * 由 src/platform/<soc>/sys_port.c 实现；共享层只通过这些函数触碰 SoC。
 */

void          sys_port_mpu_config(void);
void          sys_port_cache_enable(void);
BYTE          sys_port_icache_enabled(void);
BYTE          sys_port_dcache_enabled(void);
DWORD         sys_port_mpu_region_count(void);
void          sys_port_clock_tree_get(sys_clock_tree_s *out);
void          sys_port_hal_init(void);
func_status_e sys_port_clock_init(void);

#endif /* PLATFORM_SYS_H */
