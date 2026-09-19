

#ifndef PLATFORM_STM32H743_BSP_SDRAM_W9825G6KH_H
#define PLATFORM_STM32H743_BSP_SDRAM_W9825G6KH_H

#include "sys.h"

extern SDRAM_HandleTypeDef g_sdram_handle; /* SDRAM句柄 */

/* W9825G6KH：256 Mbit，4M x 16 bit，4 Bank，容量 32 MiB。 */
#define SDRAM_BASE_ADDR             (0xC0000000UL)
#define SDRAM_SIZE_BYTES            (32UL * 1024UL * 1024UL)
#define SDRAM_REFRESH_RATE          (839U) /* 64 ms / 8192 行，SDCLK=110 MHz */
#define SDRAM_AUTO_REFRESH_COMMANDS (8U)

/* FMC 时序参数，单位为 SDRAM 时钟周期（SDCLK=110 MHz，周期约 9.09 ns）。 */
#define SDRAM_TMR_LOAD_TO_ACTIVE    (2U)
#define SDRAM_TMR_EXIT_SELF_REFRESH (8U)
#define SDRAM_TMR_SELF_REFRESH      (7U)
#define SDRAM_TMR_ROW_CYCLE         (7U)
#define SDRAM_TMR_WRITE_RECOVERY    (2U)
#define SDRAM_TMR_ROW_PRECHARGE     (2U)
#define SDRAM_TMR_ROW_TO_COLUMN     (2U)

/* SDRAM配置参数 */
#define SDRAM_MODEREG_BURST_LENGTH_1             ((uint16_t)0x0000)
#define SDRAM_MODEREG_BURST_LENGTH_2             ((uint16_t)0x0001)
#define SDRAM_MODEREG_BURST_LENGTH_4             ((uint16_t)0x0002)
#define SDRAM_MODEREG_BURST_LENGTH_8             ((uint16_t)0x0004)
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL      ((uint16_t)0x0000)
#define SDRAM_MODEREG_BURST_TYPE_INTERLEAVED     ((uint16_t)0x0008)
#define SDRAM_MODEREG_CAS_LATENCY_2              ((uint16_t)0x0020)
#define SDRAM_MODEREG_CAS_LATENCY_3              ((uint16_t)0x0030)
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD    ((uint16_t)0x0000)
#define SDRAM_MODEREG_WRITEBURST_MODE_PROGRAMMED ((uint16_t)0x0000)
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE     ((uint16_t)0x0200)

/******************************************************************************************/

uint8_t sdram_init(void);
uint8_t sdram_send_cmd(uint8_t bankx, uint8_t cmd, uint8_t refresh, uint16_t regval);
void    fmc_sdram_write_buffer(uint8_t *pbuf, uint32_t writeaddr, uint32_t n);
void    fmc_sdram_read_buffer(uint8_t *pbuf, uint32_t readaddr, uint32_t n);
uint8_t sdram_initialization_sequence(void);

#endif /* PLATFORM_STM32H743_BSP_SDRAM_W9825G6KH_H */
