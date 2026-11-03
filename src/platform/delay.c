#include "delay.h"

#include "common.h"
/* 本文件是**跨 target 共享**的平台源文件：不 include 任何 SoC 头，也不出现
 * HAL 类型。SysTick 寄存器与时钟查询一律经 delay_port_* 下沉到
 * src/platform/<soc>/delay_port.c —— 内核头无法单独使用（它依赖设备头提供
 * IRQn_Type/__NVIC_PRIO_BITS），故共享层不能直接碰这些寄存器。
 *
 * 时基只用 ARM 核心自带的 SysTick：HAL 已把它配置为 1 ms 周期；
 * ms 计数读 HAL tick，µs 在本毫秒窗口内用 (LOAD - VAL) 插值。
 * DWT 是调试/跟踪单元，不是产品时间源，本模块不碰 DWT。 */

static bool  g_delay_initialized;
static DWORD g_systick_cycles_per_us;

static delay_status_e delay_systick_us_busy(DWORD us)
{
    DWORD reload;
    DWORD ticks;
    DWORD previous;
    DWORD current;
    DWORD elapsed = 0U;

    if (!g_delay_initialized)
    {
        return DELAY_ERR_NOT_INITIALIZED;
    }
    if ((us != 0U) && (us > (UINT32_MAX / g_systick_cycles_per_us)))
    {
        return DELAY_ERR_OUT_OF_RANGE;
    }

    reload   = delay_port_systick_load();
    ticks    = us * g_systick_cycles_per_us;
    previous = delay_port_systick_value();

    while (elapsed < ticks)
    {
        current = delay_port_systick_value();
        if (current != previous)
        {
            elapsed += (current < previous) ? (previous - current) : (reload + 1U - current + previous);
            previous = current;
        }
    }

    return DELAY_OK;
}

/*
 * 读取一致的 (ms, 本毫秒内已过周期数)。
 *
 * SysTick 从 LOAD 递减到 0 后重装；HAL_IncTick 与 VAL=LOAD 同相。
 * 先读 ms、再读 VAL、再读 ms：若 ms 在两次读之间变化，重读 VAL，
 * 避免把「上一毫秒的 VAL」配到「下一毫秒的 ms」。
 */
static DWORD delay_systick_read(DWORD *ms)
{
    const DWORD load     = delay_port_systick_load();
    const DWORD ms_first = delay_port_now_ms();
    DWORD       val      = delay_port_systick_value();

    *ms = delay_port_now_ms();
    if (*ms != ms_first)
    {
        *ms = delay_port_now_ms();
        val = delay_port_systick_value();
    }

    return load - val;
}

/**
 * @brief 初始化延时/微秒时间服务。
 * @param 无。
 * @return 无。初始化结果通过 delay_is_initialized() 查询。
 * @note SysTick 必须已由 HAL 配置为 1 ms 周期；本函数从 LOAD 反推每微秒
 *       周期数，不依赖 HCLK/SYSCLK 的软件假设。时钟树改变后 HAL 会重新
 *       配置 SysTick，重新调用本函数即可。
 */

void delay_init(void)
{
    const DWORD cycles_per_ms = delay_port_systick_load() + 1U;

    g_systick_cycles_per_us = cycles_per_ms / 1000U;
    g_delay_initialized     = (g_systick_cycles_per_us != 0U);
}

/**
 * @brief 查询延时服务是否可用。
 * @param 无。
 * @return true 表示 SysTick 已配置且速率有效；false 表示尚未初始化。
 */

bool delay_is_initialized(void)
{
    return g_delay_initialized;
}

bool delay_time_now_us(DWORD *us)
{
    DWORD ms;
    DWORD cycles;

    if ((us == NULL) || !g_delay_initialized)
    {
        return false;
    }

    cycles = delay_systick_read(&ms);
    *us    = (ms * 1000U) + (cycles / g_systick_cycles_per_us);
    return true;
}

QWORD delay_time_now_us64(void)
{
    DWORD ms;
    DWORD cycles;

    if (!g_delay_initialized)
    {
        return 0U;
    }

    cycles = delay_systick_read(&ms);
    return ((QWORD)ms * 1000U) + (cycles / g_systick_cycles_per_us);
}

bool delay_time_elapsed_us(DWORD start_us, WORD timeout_us)
{
    DWORD now_us;

    if (!delay_time_now_us(&now_us))
    {
        return false;
    }
    return (now_us - start_us) >= timeout_us;
}

DWORD delay_time_since_us(DWORD start_us)
{
    DWORD now_us;

    if (!delay_time_now_us(&now_us))
    {
        return 0U;
    }
    return now_us - start_us;
}

/**
 * @brief 获取单次忙等待允许的最大时长。
 * @param 无。
 * @return 最大微秒数；未初始化时返回 0。
 * @note SysTick 每 1 ms 重装一次，忙等循环自己累计跨重装的周期数；上限只
 *       取决于 `us * cycles_per_us` 是否溢出，调用者不得要求超过该上限的
 *       单次等待。长延时应拆分或使用任务延时、定时器和事件机制。
 */

DWORD delay_max_busy_us(void)
{
    if (!g_delay_initialized)
    {
        return FUNC_STATUS_SUCCESS;
    }

    return UINT32_MAX / g_systick_cycles_per_us;
}
/**
 * @brief 执行微秒级忙等待。
 * @param us 延时时间，单位微秒；0 表示立即返回。
 * @return DELAY_OK 成功；DELAY_ERR_NOT_INITIALIZED 尚未初始化；
 *         DELAY_ERR_OUT_OF_RANGE 超过当前计数器可表示范围。
 * @note 这是阻塞接口，会持续占用当前 CPU。只允许用于短硬件时序，禁止在
 *       中断服务函数中等待业务事件，也不建议在 RTOS 任务中长时间调用。
 *       中断仍可抢占本函数，因此实际延时可能大于请求值。
 * @details SysTick 只有 24 位，但忙等循环按重装边界累计周期数，因此可
 *          覆盖多次 1 ms 重装；误差主要来自函数调用和循环本身的固定开销。
 */

delay_status_e delay_us_busy(DWORD us)
{
    return delay_systick_us_busy(us);
}
/**
 * @brief 执行毫秒级忙等待。
 * @param ms 延时时间，单位毫秒；0 表示立即返回。
 * @return DELAY_OK 成功；DELAY_ERR_NOT_INITIALIZED 尚未初始化；
 *         DELAY_ERR_OUT_OF_RANGE 超过计数器单次忙等待上限。
 * @note 该接口不会让出 CPU，也不会调用 RTOS 调度器；只适用于启动阶段的
 *       短暂、确定性硬件稳定时间。
 * @details 先用 64 位乘法检查 ms * 1000 是否越界，再委托微秒接口，避免
 *          直接使用 32 位乘法造成超时截断。
 */

delay_status_e delay_ms_busy(DWORD ms)
{
    DWORD max_us;

    if (ms == 0U)
    {
        return DELAY_OK;
    }

    max_us = delay_max_busy_us();
    if ((max_us == 0U) || ((QWORD)ms * 1000ULL > max_us))
    {
        return (max_us == 0U) ? DELAY_ERR_NOT_INITIALIZED : DELAY_ERR_OUT_OF_RANGE;
    }

    return delay_us_busy(ms * 1000U);
}

void delay_us(DWORD us)
{
    (void)delay_us_busy(us);
}

void delay_ms(DWORD ms)
{
    (void)delay_ms_busy(ms);
}

/**
 * @brief 读取系统单调毫秒计数。
 * @param 无。
 * @return 当前毫秒计数。
 * @note 计数器为 DWORD，持续运行约 49.7 天后回绕；调用者应使用
 *       delay_elapsed_ms() 进行差值比较，不要直接比较绝对大小。该计数的
 *       硬件来源由 delay_port_now_ms() 提供（SysTick/HAL tick），本模块
 *       不定义 SysTick_Handler。
 */

DWORD delay_now_ms(void)
{
    return delay_port_now_ms();
}

/**
 * @brief 判断从起始时间到当前时间是否已经达到超时值。
 * @param start_ms 起始时间，通常由 delay_now_ms() 获得。
 * @param timeout_ms 超时时间，单位毫秒。
 * @return true 表示已超时，false 表示尚未超时。
 * @note 使用 (now - start) 的无符号差值，因此可以正确处理一次 HAL tick
 *       回绕。timeout_ms 不应大于 UINT32_MAX；对于超长周期应使用 RTC 或
 *       64 位时间服务。
 * @details 该函数只读时钟，不阻塞、不等待，也不会清除或修改任何定时器。
 */

bool delay_elapsed_ms(DWORD start_ms, DWORD timeout_ms)
{
    return (delay_now_ms() - start_ms) >= timeout_ms;
}
