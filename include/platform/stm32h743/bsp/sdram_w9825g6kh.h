

#ifndef PLATFORM_STM32H743_BSP_SDRAM_W9825G6KH_H
#define PLATFORM_STM32H743_BSP_SDRAM_W9825G6KH_H

#include "sys_port.h"

/*
 * ⚠ 当前未启用：FTL 为 DRAM-less（元数据常驻 AXI_RAM / SRAM2），
 * 没有任何模块使用 SDRAM；bsp/sdram_w9825g6kh.c 也未纳入构建
 * （src/platform/<soc>/makefile.mk 只通配 <soc>/*.c，不含 bsp/）。
 * 接口保留供将来需要外扩元数据缓存时接入。
 */

extern SDRAM_HandleTypeDef g_sdram_handle; /* SDRAM句柄 */

/* W9825G6KH：256 Mbit，4M x 16 bit，4 Bank，容量 32 MiB。 */
#define SDRAM_BASE_ADDR             (0xC0000000UL)
#define SDRAM_SIZE_BYTES            (32UL * 1024UL * 1024UL)
#define SDRAM_REFRESH_RATE          (761U) /* 64 ms / 8192 行 - 20，SDCLK=100 MHz */
#define SDRAM_AUTO_REFRESH_COMMANDS (8U)

/* FMC 时序参数，单位为 SDRAM 时钟周期（SDCLK=100 MHz，周期 10 ns）。 */
#define SDRAM_TMR_LOAD_TO_ACTIVE    (2U)
#define SDRAM_TMR_EXIT_SELF_REFRESH (8U)
#define SDRAM_TMR_SELF_REFRESH      (7U)
#define SDRAM_TMR_ROW_CYCLE         (7U)
#define SDRAM_TMR_WRITE_RECOVERY    (2U)
#define SDRAM_TMR_ROW_PRECHARGE     (2U)
#define SDRAM_TMR_ROW_TO_COLUMN     (2U)

/* SDRAM配置参数 */
#define SDRAM_MODEREG_BURST_LENGTH_1             ((WORD)0x0000)
#define SDRAM_MODEREG_BURST_LENGTH_2             ((WORD)0x0001)
#define SDRAM_MODEREG_BURST_LENGTH_4             ((WORD)0x0002)
#define SDRAM_MODEREG_BURST_LENGTH_8             ((WORD)0x0004)
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL      ((WORD)0x0000)
#define SDRAM_MODEREG_BURST_TYPE_INTERLEAVED     ((WORD)0x0008)
#define SDRAM_MODEREG_CAS_LATENCY_2              ((WORD)0x0020)
#define SDRAM_MODEREG_CAS_LATENCY_3              ((WORD)0x0030)
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD    ((WORD)0x0000)
#define SDRAM_MODEREG_WRITEBURST_MODE_PROGRAMMED ((WORD)0x0000)
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE     ((WORD)0x0200)

/******************************************************************************************/

BYTE sdram_init(void);
BYTE sdram_send_cmd(BYTE bankx, BYTE cmd, BYTE refresh, WORD regval);
void fmc_sdram_write_buffer(BYTE *pbuf, DWORD writeaddr, DWORD n);
void fmc_sdram_read_buffer(BYTE *pbuf, DWORD readaddr, DWORD n);
BYTE sdram_initialization_sequence(void);

#endif /* PLATFORM_STM32H743_BSP_SDRAM_W9825G6KH_H */
