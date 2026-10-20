#include "platform.h"

#include "common.h"
#include "delay.h"

static const ssd_platform_ops_t *g_platform_ops;

/*
 * stm32_platform_time_us - 系统单调微秒计数。
 *
 * 基于平台毫秒节拍（1 ms，由 SysTick 驱动）换算为微秒；毫秒计数由 port 侧
 * delay_port_now_ms() 提供，本函数只做单位换算，故留在共享层。
 * 返回 QWORD 微秒，约 5849 年后回绕；FTL 层以 ms 为单位做差值，
 * 直接 (`QWORD`)time_us()/1000U 无溢出风险。
 */
static QWORD platform_time_us(void)
{
    return (QWORD)delay_port_now_ms() * 1000U;
}

static const ssd_platform_ops_t g_stm32_platform_ops = {
    .time_us = platform_time_us,
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
