#include "platform.h"

#include "common.h"
#include "delay.h"

static const ssd_platform_ops_t *g_platform_ops;

/*
 * stm32_platform_time_us - 系统单调微秒计数（DWT CYCCNT 累加）。
 *
 * CYCCNT 是 32 位、按 CPU 时钟递增，2^32 周 ≈ 8.9 s @480 MHz，不能直接当
 * 全局时钟。这里维护 64 位微秒累加器：每次取 CYCCNT 增量按核频折成微秒，
 * 余数（cycles % cycles_per_us）留到下次，避免高频调用累积截断误差。
 *
 * 增量 ≥ 2^31 cycles 时无法区分「调用间隔超过半个回绕」与「计数器被复位」
 * ⇒ 放弃本次增量、以毫秒节拍重同步（取较大值保持单调）。DWT 未就绪
 * （delay_init 尚未执行 / 使能失败）时退回毫秒节拍。
 *
 * 只在主循环上下文调用（消费方：ftl_job_now_ms 与后台调度器的墙钟兜底）；
 * 非 ISR 安全。
 */
static QWORD platform_time_us(void)
{
    static QWORD s_us;
    static DWORD s_last_cycles;
    static DWORD s_rem_cycles;
    static bool  s_valid;

    const DWORD cycles_per_us = delay_port_core_clock_hz() / 1000000U;
    DWORD       cycles        = 0U;

    if ((cycles_per_us == 0U) || (delay_dwt_cycles_now(&cycles) == false))
    {
        return (QWORD)delay_port_now_ms() * 1000U;
    }

    const QWORD ms_us = (QWORD)delay_port_now_ms() * 1000U;

    if (s_valid == false)
    {
        s_valid       = true;
        s_last_cycles = cycles;
        s_us          = ms_us;
        return s_us;
    }

    const DWORD delta_cycles = cycles - s_last_cycles; /* 无符号差值：回绕安全 */

    if (delta_cycles >= 0x80000000U)
    {
        /* 间隔超过半个回绕（或计数器被复位）：无法可信折算，重同步。 */
        s_last_cycles = cycles;
        s_rem_cycles  = 0U;
        s_us          = (ms_us > s_us) ? ms_us : s_us;
        return s_us;
    }

    s_last_cycles = cycles;
    s_rem_cycles += delta_cycles;
    s_us += s_rem_cycles / cycles_per_us;
    s_rem_cycles %= cycles_per_us;
    return s_us;
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
