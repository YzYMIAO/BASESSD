#ifndef FC_CRC_ENGINE_H_
#define FC_CRC_ENGINE_H_

#include <stdint.h>

/*
 * CRC 引擎 - 使用 STM32H743 硬件 CRC 外设。
 *
 * CRC 覆盖范围：每个 CW 中 512 字节的 USER 数据区域（buffer offset 8..519）。
 * META 和 PARITY 不被 CRC 保护。
 *
 * 编码路径：先算 CRC 存到 buffer[4..8]，再走加扰 → ECC → RAID。
 * 解码路径：ECC → 解扰后，校验 CRC 与 buffer[4..8] 是否一致。
 */

/* CW 内各区域偏移/大小 */
#define FC_CRC_META_SIZE         (4U)
#define FC_CRC_FIELD_SIZE        (4U)
#define FC_CRC_USER_SIZE         (512U)
#define FC_CRC_USER_OFFSET       (FC_CRC_META_SIZE + FC_CRC_FIELD_SIZE)  /* = 8 */

/* CRC 计算结果类型 */
#define FC_CRC_RESULT_OK         (0U)
#define FC_CRC_RESULT_ERROR      (1U)

/**
 * @brief   初始化 STM32H743 CRC 硬件外设
 * @note    在 fc_init() 中调用一次
 */
void fc_crc_init(void);

/**
 * @brief   对 USER 数据（512 字节）计算 CRC-32
 * @param   user_data  指向 USER 数据的指针（buffer + 8），需 4 字节对齐
 * @return  CRC-32 值
 */
uint32_t fc_crc_compute(const uint8_t *user_data);

/**
 * @brief   校验 CRC：重算 USER 数据的 CRC 并与存储值比较
 * @param   user_data    指向 USER 数据的指针（buffer + 8）
 * @param   stored_crc   之前存储的 CRC 值（buffer[4..8] 读取）
 * @return  FC_CRC_RESULT_OK 或 FC_CRC_RESULT_ERROR
 */
uint8_t fc_crc_verify(const uint8_t *user_data, uint32_t stored_crc);

#endif /* FC_CRC_ENGINE_H_ */