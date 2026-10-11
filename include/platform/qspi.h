#ifndef PLATFORM_QSPI_H
#define PLATFORM_QSPI_H

#include <stdint.h>

/*
 * 平台 QSPI 抽象接口（共享逻辑在 src/platform/qspi.c，SoC 适配在
 * src/platform/<soc>/qspi_port.c）。
 *
 * 本头零 HAL 类型：QSPI 句柄、GPIO/时钟宏、命令/地址/数据模式枚举一律留在
 * SoC 侧（include/platform/<soc>/qspi_port.h）。共享层只做空参检查与转发。
 */

uint8_t qspi_wait_flag(uint32_t flag, uint8_t sta, uint32_t wtime);               /* 等待某个状态 */
uint8_t qspi_init(void);                                                          /* 初始化QSPI */
uint8_t qspi_send_cmd(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle); /* 发送命令 */
uint8_t qspi_receive(uint8_t *buf, uint32_t datalen);                             /* 接收数据 */
uint8_t qspi_transmit(uint8_t *buf, uint32_t datalen);                            /* 发送数据 */

/* ─────────────────────────── port 接口 ───────────────────────────
 * 由 src/platform/<soc>/qspi_port.c 实现。
 */

uint8_t qspi_port_wait_flag(uint32_t flag, uint8_t sta, uint32_t wtime);
uint8_t qspi_port_init(void);
uint8_t qspi_port_send_cmd(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle);
uint8_t qspi_port_receive(uint8_t *buf, uint32_t datalen);
uint8_t qspi_port_transmit(uint8_t *buf, uint32_t datalen);

#endif /* PLATFORM_QSPI_H */
