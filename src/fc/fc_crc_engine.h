#ifndef FC_CRC_ENGINE_H_
#define FC_CRC_ENGINE_H_

#include "fc_interface.h" /* FC_CRC_* 布局常量 */
#include "ssd_types.h"    /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * CRC 引擎 - 使用 STM32H743 硬件 CRC 外设（经平台层 HAL 驱动封装）。
 *
 * CRC 覆盖范围：每个 CW 中 512 字节的 USER 数据区域（buffer offset 8..519）。
 * META 和 PARITY 不被 CRC 保护。
 *
 * 编码路径：先算 CRC 存到 buffer[4..8]，再走加扰 → ECC → RAID。
 * 解码路径：ECC → 解扰后，校验 CRC 与 buffer[4..8] 是否一致。
 *
 * 本文件只覆盖 FC CW 的 CRC-32/MPEG-2。ISO-HDLC 已下沉到
 * src/platform/crc.c（boot/FTL 元数据共用）。
 */

/* CRC 计算结果类型 */
#define FC_CRC_RESULT_OK    (0U)
#define FC_CRC_RESULT_ERROR (1U)

/**
 * @brief   初始化 CRC 后端（硬件优先，软件 fallback）。
 * @note    在 fc_init() 中调用一次。硬件后端的时钟/外设自检在平台层；
 *          软件后端只建表。自检不过一律 ASSERT。
 */
void fc_crc_init(void);

/**
 * @brief   对 USER 数据（512 字节）计算 CRC-32
 * @param   user_data  指向 USER 数据的指针（buffer + 8）
 * @return  CRC-32 值
 * @note    user_data **必须 4 字节对齐**——硬性前置条件：硬件按 32 位字喂入，
 *          HAL 直接按 DWORD* 解引用。违反属未对齐访问。
 */
DWORD fc_crc_compute(const BYTE *user_data);

/**
 * @brief   校验 CRC：重算 USER 数据的 CRC 并与存储值比较
 * @param   user_data    指向 USER 数据的指针（buffer + 8），必须 4 字节对齐
 * @param   stored_crc   之前存储的 CRC 值（buffer[4..8] 读取）
 * @return  FC_CRC_RESULT_OK 或 FC_CRC_RESULT_ERROR
 */
BYTE fc_crc_verify(const BYTE *user_data, DWORD stored_crc);

#endif /* FC_CRC_ENGINE_H_ */
