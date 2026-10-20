#ifndef PLATFORM_QSPI_H
#define PLATFORM_QSPI_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * 平台 QSPI 抽象接口（共享逻辑在 src/platform/qspi.c，SoC 适配在
 * src/platform/<soc>/qspi_port.c）。
 *
 * 本头零 HAL 类型：QSPI 句柄、GPIO/时钟宏、命令/地址/数据模式枚举一律留在
 * SoC 侧（include/platform/<soc>/qspi_port.h）。共享层只做空参检查与转发。
 */

BYTE qspi_wait_flag(DWORD flag, BYTE sta, DWORD wtime);            /* 等待某个状态 */
BYTE qspi_init(void);                                              /* 初始化QSPI */
BYTE qspi_send_cmd(BYTE cmd, DWORD addr, BYTE mode, BYTE dmcycle); /* 发送命令 */
BYTE qspi_receive(BYTE *buf, DWORD datalen);                       /* 接收数据 */
BYTE qspi_transmit(BYTE *buf, DWORD datalen);                      /* 发送数据 */

/* ─────────────────────────── port 接口 ───────────────────────────
 * 由 src/platform/<soc>/qspi_port.c 实现。
 */

BYTE qspi_port_wait_flag(DWORD flag, BYTE sta, DWORD wtime);
BYTE qspi_port_init(void);
BYTE qspi_port_send_cmd(BYTE cmd, DWORD addr, BYTE mode, BYTE dmcycle);
BYTE qspi_port_receive(BYTE *buf, DWORD datalen);
BYTE qspi_port_transmit(BYTE *buf, DWORD datalen);

#endif /* PLATFORM_QSPI_H */
