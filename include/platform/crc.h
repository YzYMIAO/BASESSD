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
 * 配置：CRC-32/MPEG-2 —— STM32 默认多项式 0x04C11DB7、INIT 0xFFFFFFFF、
 *       输入/输出均不反相、按 32 位字喂入；不是 IEEE 802.3 的反射形式。
 *
 * 后端选择（编译期）：
 *   PLATFORM_CRC_HW_SUPPORTED != 0：调用 SoC 的 crc_port_*，使用硬件 CRC；
 *   否则：本层不访问硬件，由 src/fc/fc_crc_engine.c 的软件 CRC 接管。
 * 软件实现和硬件实现必须输出完全相同的 CRC-32/MPEG-2；切换前先做已知答案对拍。
 */
#ifndef PLATFORM_CRC_HW_SUPPORTED
#    define PLATFORM_CRC_HW_SUPPORTED 0U
#endif

/**
 * @brief 初始化硬件 CRC 后端。
 * @note 硬件后端做时钟/外设初始化与 IDR 应答自检；无硬件时本函数空操作。
 *       软件后端的表的初始化在 FC CRC 引擎内完成。
 */
void crc_init(void);

/**
 * @brief 对 data 起 length_bytes 字节计算 CRC-32。
 * @param data 输入数据
 * @param length_bytes 字节数；必须是 4 的倍数
 * @return CRC-32/MPEG-2 值
 * @note 前置：data 非空、4 字节对齐；length_bytes 是 4 的倍数（违反一律 ASSERT）。
 *       本函数是硬件 MPEG-2 后端；软件 fallback 在 FC CRC 引擎。
 */
DWORD crc_compute(const BYTE *data, DWORD length_bytes);

/**
 * @brief 对 data 起 length_bytes 字节计算 CRC-32/ISO-HDLC。
 * @param data 输入数据
 * @param length_bytes 字节数
 * @return CRC-32/ISO-HDLC 值
 * @note 硬件后端会临时切换到 ISO-HDLC 配置，计算后恢复 MPEG-2 配置；
 *       仅允许在串行数据路径调用。
 */
DWORD crc_compute_iso_hdlc(const BYTE *data, DWORD length_bytes);

/* ─────────────────────────── port 接口 ───────────────────────────
 * 由 src/platform/<soc>/crc_port.c 实现；共享 src/platform/crc.c 只做
 * 自检策略与对齐契约，寄存器动作全在 port。
 */

void  crc_port_init(void);
void  crc_port_set_idr(DWORD value);
DWORD crc_port_get_idr(void);
DWORD crc_port_compute(const BYTE *data, DWORD length_bytes);
DWORD crc_port_compute_iso_hdlc(const BYTE *data, DWORD length_bytes);

#endif /* PLATFORM_CRC_H */
