#ifndef PLATFORM_MPU_H
#define PLATFORM_MPU_H

#include <stdint.h>

/*
 * 平台 MPU 抽象接口（共享逻辑在 src/platform/mpu.c，SoC 适配在
 * src/platform/<soc>/mpu_port.c）。
 *
 * 本头零 HAL 类型：区域类型/访问权限等枚举一律留在 SoC 侧。
 * 参数沿用厂商封装的中性类型（基址、大小、区域号 + 各能力位）。
 */

/* 设置 MPU 区域保护。 */
void mpu_set_protection(uint32_t baseaddr, uint32_t size, uint32_t rnum, uint8_t de, uint8_t ap, uint8_t sen, uint8_t cen, uint8_t ben);

/* ─────────────────────────── port 接口 ───────────────────────────
 * 由 src/platform/<soc>/mpu_port.c 实现；共享 src/platform/mpu.c 只转发。
 */
void mpu_port_set_protection(uint32_t baseaddr, uint32_t size, uint32_t rnum, uint8_t de, uint8_t ap, uint8_t sen, uint8_t cen, uint8_t ben);

#endif /* PLATFORM_MPU_H */
