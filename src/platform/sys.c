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

void sys_init(void)
{
    sys_mpu_config();
    sys_cache_enable();
    sys_port_hal_init();
    if (sys_stm32_clock_init(5U, 160U, 2U, 4U) != FUNC_STATUS_SUCCESS)
    {
        while (1)
        {
        }
    }
    delay_init();
    usart_init(115200U);
    /* 使能可配置故障（MemManage/BusFault/UsageFault），避免全部升级成 HardFault。 */
    fault_init();
}

void sys_mpu_config(void)
{
    sys_port_mpu_config();
}

void sys_cache_enable(void)
{
    sys_port_cache_enable();
}

BYTE get_icache_sta(void)
{
    return sys_port_icache_enabled();
}

BYTE get_dcache_sta(void)
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

void sys_nvic_set_vector_table(DWORD base_address, DWORD offset)
{
    sys_port_set_vector_table(base_address, offset);
}

void sys_wfi_set(void)
{
    sys_port_wfi();
}

void sys_intx_disable(void)
{
    sys_port_irq_disable();
}

void sys_intx_enable(void)
{
    sys_port_irq_enable();
}

void sys_msr_msp(DWORD address)
{
    sys_port_set_msp(address);
}

func_status_e sys_stm32_clock_init(DWORD pll_prediv, DWORD pll_mul, DWORD sysclk_div, DWORD peripheral_div)
{
    return sys_port_clock_init(pll_prediv, pll_mul, sysclk_div, peripheral_div);
}
