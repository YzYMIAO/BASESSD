/*
 * mpu_port.c — stm32h743 MPU 适配层
 *
 * 只做硬件相关的事：把中性参数填进 HAL 的 MPU 区域结构体并配置/使能。
 * 共享 src/platform/mpu.c 只转发 mpu_set_protection 到本文件。
 */

#include "mpu.h"
#include "sys_port.h"

/**
 * @brief   设置MPU区域保护
 * @param   baseaddr: 区域基地址
 * @param   size: 区域大小
 * @param   rnum: MPU区域编号
 * @arg     MPU_REGION_NUMBER0: MPU区域0
 * @arg     MPU_REGION_NUMBER1: MPU区域1
 * @arg     MPU_REGION_NUMBER2: MPU区域2
 * @arg     MPU_REGION_NUMBER3: MPU区域3
 * @arg     MPU_REGION_NUMBER4: MPU区域4
 * @arg     MPU_REGION_NUMBER5: MPU区域5
 * @arg     MPU_REGION_NUMBER6: MPU区域6
 * @arg     MPU_REGION_NUMBER7: MPU区域7
 * @arg     MPU_REGION_NUMBER8: MPU区域8
 * @arg     MPU_REGION_NUMBER9: MPU区域9
 * @arg     MPU_REGION_NUMBER10: MPU区域10
 * @arg     MPU_REGION_NUMBER11: MPU区域11
 * @arg     MPU_REGION_NUMBER12: MPU区域12
 * @arg     MPU_REGION_NUMBER13: MPU区域13
 * @arg     MPU_REGION_NUMBER14: MPU区域14
 * @arg     MPU_REGION_NUMBER15: MPU区域15
 * @param   de: 指令访问状态
 * @arg     MPU_INSTRUCTION_ACCESS_ENABLE: 使能指令访问
 * @arg     MPU_INSTRUCTION_ACCESS_DISABLE: 禁止指令访问
 * @param   ap: 访问权限
 * @arg     MPU_REGION_NO_ACCESS: 无访问
 * @arg     MPU_REGION_PRIV_RW: 仅支持特权读写访问
 * @arg     MPU_REGION_PRIV_RW_URO: 禁止用户写访问
 * @arg     MPU_REGION_FULL_ACCESS: 全访问
 * @arg     MPU_REGION_PRIV_RO: 仅支持特权读访问
 * @arg     MPU_REGION_PRIV_RO_URO: 只读
 * @param   sen: 可共享状态
 * @arg     MPU_ACCESS_SHAREABLE: 使能共享
 * @arg     MPU_ACCESS_NOT_SHAREABLE: 禁止共享
 * @param   cen: 可Cache状态
 * @arg     MPU_ACCESS_CACHEABLE: 使能Cache
 * @arg     MPU_ACCESS_NOT_CACHEABLE: 禁止Cache
 * @param   ben: 可缓冲状态
 * @arg     MPU_ACCESS_BUFFERABLE: 使能缓冲
 * @arg     MPU_ACCESS_NOT_BUFFERABLE: 禁止缓冲
 * @retval  无
 */
void mpu_port_set_protection(uint32_t baseaddr, uint32_t size, uint32_t rnum, uint8_t de, uint8_t ap, uint8_t sen, uint8_t cen, uint8_t ben)
{
    MPU_Region_InitTypeDef mpu_region_init_struct = {0};

    mpu_region_init_struct.Enable           = MPU_REGION_ENABLE;
    mpu_region_init_struct.Number           = rnum;
    mpu_region_init_struct.BaseAddress      = baseaddr;
    mpu_region_init_struct.Size             = size;
    mpu_region_init_struct.SubRegionDisable = 0x00;
    mpu_region_init_struct.TypeExtField     = MPU_TEX_LEVEL1;
    mpu_region_init_struct.AccessPermission = ap;
    mpu_region_init_struct.DisableExec      = de;
    mpu_region_init_struct.IsShareable      = sen;
    mpu_region_init_struct.IsCacheable      = cen;
    mpu_region_init_struct.IsBufferable     = ben;
    HAL_MPU_Disable();
    HAL_MPU_ConfigRegion(&mpu_region_init_struct);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}
