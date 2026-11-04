#include "platform.h"

#include "common.h"
#include "delay.h"

static const ssd_platform_ops_t *g_platform_ops;

/*
 * stm32_platform_time_us - 系统单调微秒计数（SysTick + HAL tick）。
 *
 * 只用 ARM 核心的 SysTick：HAL tick 给毫秒，SysTick 的 VAL/LOAD 给本毫秒
 * 内的周期分辨率。delay_time_now_us64() 返回 64 位微秒，不存在 DWT 那种
 * 32 位周期计数器短回绕问题。
 *
 * 只在主循环上下文调用（消费方：后台调度器的墙钟兜底与 µs 级测量）；
 * 非 ISR 安全。系统毫秒时基见 time_ms（SysTick/HAL tick）。
 */
static QWORD platform_time_us(void)
{
    return delay_time_now_us64();
}

static const ssd_platform_ops_t g_stm32_platform_ops = {
    .time_us = platform_time_us,
    .time_ms = delay_now_ms, /* SysTick/HAL tick：ARM 核心的系统毫秒时基 */
    /* log_write / memory_allocate / memory_free 暂不提供；register 只强制 time_us。 */
};

int stm32_platform_register(void)
{
    return ssd_platform_register(&g_stm32_platform_ops);
}

int ssd_platform_register(const ssd_platform_ops_t *ops)
{
    if ((ops == NULL) || (ops->time_us == NULL))
    {
        return FUNC_STATUS_ERROR;
    }

    g_platform_ops = ops;
    return FUNC_STATUS_SUCCESS;
}

const ssd_platform_ops_t *ssd_platform_get(void)
{
    return g_platform_ops;
}

DWORD platform_now_ms(void)
{
    if (g_platform_ops == NULL)
    {
        return 0U;
    }
    if (g_platform_ops->time_ms != NULL)
    {
        return g_platform_ops->time_ms();
    }
    if (g_platform_ops->time_us == NULL)
    {
        return 0U;
    }

    /* QWORD µs → DWORD ms 是接口边界上的有意窄化：所有消费方都用差值。 */
    return (DWORD)(g_platform_ops->time_us() / 1000U);
}
