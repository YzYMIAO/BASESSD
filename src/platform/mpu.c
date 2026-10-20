/*
 * mpu.c — 跨 target 共享的 MPU 服务层
 *
 * 本层只提供稳定入口，把请求转发给 port 侧（src/platform/<soc>/mpu_port.c）
 * 的 HAL/寄存器实现。
 */

#include "mpu.h"

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

void mpu_set_protection(DWORD baseaddr, DWORD size, DWORD rnum, BYTE de, BYTE ap, BYTE sen, BYTE cen, BYTE ben)
{
    mpu_port_set_protection(baseaddr, size, rnum, de, ap, sen, cen, ben);
}
