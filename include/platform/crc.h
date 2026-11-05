#ifndef PLATFORM_CRC_H
#define PLATFORM_CRC_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * 平台 CRC-32 抽象接口（实现在 src/platform/<soc>/crc.c，走第三方 HAL CRC 驱动）。
 *
 * 分层：实现目录是唯一持有厂商 HAL 的 CRC 句柄类型、管时钟、做外设自检的地方；
 * 上层（src/fc/fc_data_path.c）只决定「对哪段数据算、结果怎么判」。
 *
 * 刻意**不** include "sys.h"：唯一使用者 src/fc/fc_data_path.c 要保持零 HAL 依赖，
 * 故本头只暴露两个裸类型签名，不泄漏任何 HAL 类型。
 *
 * 配置：IEEE 802.3 CRC-32 —— 默认多项式 0x04C11DB7、INIT 0xFFFFFFFF、
 *       输入/输出均不反相、按 32 位字喂入。
 */

/* 初始化 CRC 外设（含时钟使能 + 外设应答自检）。
 * 由 fc_init() 经 fc_crc_init() 调用一次。自检不过一律 ASSERT。 */
void crc_init(void);

/* 对 data 起 length_bytes 字节计算 CRC-32。
 * 前置：data 非空、4 字节对齐；length_bytes 是 4 的倍数（违反一律 ASSERT）。 */
DWORD crc_compute(const BYTE *data, DWORD length_bytes);

/* ─────────────────────────── port 接口 ───────────────────────────
 * 由 src/platform/<soc>/crc_port.c 实现；共享 src/platform/crc.c 只做
 * 自检策略与对齐契约，寄存器动作全在 port。
 */

void  crc_port_init(void);
void  crc_port_set_idr(DWORD value);
DWORD crc_port_get_idr(void);
DWORD crc_port_compute(const BYTE *data, DWORD length_bytes);

#endif /* PLATFORM_CRC_H */
