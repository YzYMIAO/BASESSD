#ifndef FC_CRC_ENGINE_H_
#define FC_CRC_ENGINE_H_

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stddef.h>
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
 * 软件实现在 src/fc/fc_crc_engine.c；硬件后端在 src/platform/stm32h743/crc_port.c。
 * 硬件 CRC 可用时优先硬件，否则使用 FC 内的软件查表实现；两者必须 bit-exact。
 */

/* CW 内各区域偏移/大小 */
#define FC_CRC_META_SIZE   (4U)
#define FC_CRC_FIELD_SIZE  (4U)
#define FC_CRC_USER_SIZE   (512U)
#define FC_CRC_USER_OFFSET (FC_CRC_META_SIZE + FC_CRC_FIELD_SIZE) /* = 8 */

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

/**
 * @brief   计算 CRC-32/ISO-HDLC（启动/FTL 元数据使用）。
 * @param   data 输入数据；为 NULL 且 size == 0 时返回 0
 * @param   size 字节数
 * @return  CRC-32/ISO-HDLC 值
 * @note    硬件 ISO-HDLC 后端可用时走硬件，否则走 FC 软件 slice-by-4；
 *          两个后端必须 bit-exact。它与 FC CW 的 CRC-32/MPEG-2 不是同一变体。
 */
DWORD fc_crc32_iso_hdlc(const void *data, size_t size);

#endif /* FC_CRC_ENGINE_H_ */