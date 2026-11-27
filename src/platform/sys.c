/*
 * sys.c — 跨 target 共享的系统服务层
 *
 * 职责：上电初始化序列的编排（顺序即契约），以及 sys_* 公开接口到
 * include/platform/sys.h 里 sys_port_* 接口的一行转发。所有寄存器 / 内核内建 /
 * 时钟树 / MPU 区域表都在 src/platform/<soc>/sys_port.c。
 *
 * 本文件零 SoC 头、零内核头：SCB、PRIMASK、MSP、WFI 一律经 sys_port_* 下沉。
 */

#include "sys.h"

#include "common.h"
#include "delay.h"
#include "fault.h"
#include "usart.h"

func_status_e sys_init(DWORD console_baud)
{
    sys_mpu_config();
    sys_cache_enable();
    sys_port_hal_init();
    if (sys_port_clock_init() != FUNC_STATUS_SUCCESS)
    {
        return FUNC_STATUS_ERROR;
    }
    delay_init();
    if (usart_init(console_baud) != FUNC_STATUS_SUCCESS)
    {
        return FUNC_STATUS_ERROR;
    }
    /* 使能可配置故障（MemManage/BusFault/UsageFault），避免全部升级成 HardFault。 */
    fault_init();
    return FUNC_STATUS_SUCCESS;
}

void sys_mpu_config(void)
{
    sys_port_mpu_config();
}

void sys_cache_enable(void)
{
    sys_port_cache_enable();
}

BYTE sys_icache_enabled(void)
{
    return sys_port_icache_enabled();
}

BYTE sys_dcache_enabled(void)
{
    return sys_port_dcache_enabled();
}

DWORD sys_mpu_region_count(void)
{
    return sys_port_mpu_region_count();
}

void sys_clock_tree_get(sys_clock_tree_s *out)
{
    sys_port_clock_tree_get(out);
}
