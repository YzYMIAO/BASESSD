/*
 * mpu.c — 跨 target 共享的 MPU 服务层
 *
 * 本层只提供稳定入口，把请求转发给 port 侧（src/platform/<soc>/mpu_port.c）
 * 的 HAL/寄存器实现。
 */

#include "mpu.h"

void mpu_set_protection(uint32_t baseaddr, uint32_t size, uint32_t rnum, uint8_t de, uint8_t ap, uint8_t sen, uint8_t cen, uint8_t ben)
{
    mpu_port_set_protection(baseaddr, size, rnum, de, ap, sen, cen, ben);
}
