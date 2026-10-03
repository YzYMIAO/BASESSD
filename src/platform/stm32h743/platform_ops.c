#include "platform/platform.h"
#include "sys.h"

/*
 * stm32_platform_time_us - 系统单调微秒计数。
 *
 * 基于 HAL tick（1 ms 节拍，由 SysTick_Handler 的 HAL_IncTick 驱动），
 * 换算为微秒。HAL tick 由 sys_init() 中的 HAL_Init() 启动，本函数
 * 只允许在 sys_init() 之后调用。
 *
 * 返回 uint64_t 微秒，约 5849 年后回绕；FTL 层以 ms 为单位做差值，
 * 直接 (uint64)time_us()/1000U 无溢出风险。
 */
static uint64_t stm32_platform_time_us(void)
{
    return (uint64_t)HAL_GetTick() * 1000U;
}

static const ssd_platform_ops_t g_stm32_platform_ops = {
    .time_us = stm32_platform_time_us,
    /* log_write / memory_allocate / memory_free 暂不提供；register 只强制 time_us。 */
};

int stm32_platform_register(void)
{
    return ssd_platform_register(&g_stm32_platform_ops);
}
