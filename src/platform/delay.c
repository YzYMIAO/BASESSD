#include "delay.h"

#include "common.h"
/* 本文件是**跨 target 共享**的平台源文件：不 include 任何 SoC 头，也不出现
 * HAL 类型。计数器寄存器（DWT/SysTick）与时钟查询一律经 delay_port_* 下沉到
 * src/platform/<soc>/delay_port.c —— 内核头无法单独使用（它依赖设备头提供
 * IRQn_Type/__NVIC_PRIO_BITS），故共享层不能直接碰这些寄存器。 */

static DWORD g_core_clock_hz;
static bool  g_delay_initialized;
static DWORD g_dwt_cycles_per_us;
static bool  g_dwt_initialized;

#if !DELAY_USE_DWT
static DWORD g_systick_cycles_per_us;
#endif

static DWORD delay_resolve_clock(DWORD core_clock_hz)
{
    return (core_clock_hz != 0U) ? core_clock_hz : delay_port_hclk_hz();
}

#if !DELAY_USE_DWT
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
            elapsed += (current < previous) ? (previous - current)
                                            : (reload + 1U - current + previous);
            previous = current;
        }
    }

    return DELAY_OK;
}
#endif
/**
 * @brief 初始化高精度延时服务。
 * @param core_clock_hz CPU 内核时钟频率，单位 Hz；传入 0 自动使用
 *                      SystemCoreClock。
 * @return 无。初始化结果通过 delay_is_initialized() 查询。
 * @note 应在 HAL 时钟初始化完成后调用。若后续改变 PLL、分频或进入低功耗
 *       后恢复时钟，必须重新调用 delay_reconfigure()。
 * @details 函数保存时钟频率，清零 DWT CYCCNT 并使能自由运行计数器，后续
 *          微秒延时通过读取该计数器的增量换算时间。
 */

void delay_init(DWORD core_clock_hz)
{
    g_core_clock_hz     = delay_resolve_clock(core_clock_hz);
    /* CYCCNT follows the Cortex-M7 core clock, not the AHB/SysTick clock. */
    g_dwt_cycles_per_us = delay_port_core_clock_hz() / 1000000U;
    g_dwt_initialized   = (g_dwt_cycles_per_us != 0U) && delay_port_dwt_enable();

#if DELAY_USE_DWT
    g_delay_initialized = (g_core_clock_hz != 0U) && g_dwt_initialized;
#else
    g_systick_cycles_per_us = g_core_clock_hz / 1000000U;
    g_delay_initialized     = (g_systick_cycles_per_us != 0U) && (delay_port_systick_load() != 0U);
#endif
}

/**
 * @brief 按新的 CPU 时钟重新配置延时服务。
 * @param core_clock_hz 新的 CPU 内核时钟，单位 Hz；传入 0 使用
 *                      SystemCoreClock。
 * @return 无。
 * @note 调用期间不应有其他线程同时执行微秒延时；推荐在系统时钟切换的
 *       临界初始化阶段调用。该函数不会替应用配置 SysTick。
 */

void delay_reconfigure(DWORD core_clock_hz)
{
    /* 动态调频、PLL 切换或从低功耗恢复后调用此函数。 */
    delay_init(core_clock_hz);
}
/**
 * @brief 查询延时服务是否可用。
 * @param 无。
 * @return true 表示时钟有效且 DWT CYCCNT 已启用；false 表示尚未初始化或
 *         DWT 使能失败。
 */

bool delay_is_initialized(void)
{
    return g_delay_initialized;
}

bool delay_dwt_cycles_now(DWORD *cycles)
{
    if ((cycles == NULL) || !g_dwt_initialized)
    {
        return false;
    }
    *cycles = delay_port_dwt_cycles();
    return true;
}

bool delay_dwt_elapsed_us(DWORD start_cycles, WORD timeout_us)
{
    QWORD timeout_cycles;

    if (!g_dwt_initialized)
    {
        return false;
    }
    timeout_cycles = (QWORD)timeout_us * g_dwt_cycles_per_us;
    return (delay_port_dwt_cycles() - start_cycles) >= timeout_cycles;
}
/**
 * @brief 获取单次 DWT 忙等待允许的最大时长。
 * @param 无。
 * @return 最大微秒数；未初始化时返回 0。
 * @note DWT CYCCNT 是 32 位计数器。为使无符号差值在一次回绕内仍然可靠，
 *       调用者不得要求超过该上限的单次等待。长延时应拆分或使用任务延时、
 *       定时器和事件机制。
 * @details 计算公式为 UINT32_MAX * 1,000,000 / core_clock_hz，使用 64 位
 *          中间结果避免计算溢出。
 */

DWORD delay_max_busy_us(void)
{
    if (!g_delay_initialized || (g_core_clock_hz == 0U))
    {
        return FUNC_STATUS_SUCCESS;
    }

#if DELAY_USE_DWT
    QWORD max_us;
    /* DWT 为 32 位自由运行计数器，等待区间必须小于一个完整回绕周期。 */

    max_us = ((QWORD)UINT32_MAX * 1000000ULL) / g_core_clock_hz;
    return (max_us > UINT32_MAX) ? UINT32_MAX : max_us;
#else
    return UINT32_MAX / g_systick_cycles_per_us;
#endif
}
/**
 * @brief 执行微秒级忙等待。
 * @param us 延时时间，单位微秒；0 表示立即返回。
 * @return DELAY_OK 成功；DELAY_ERR_NOT_INITIALIZED 尚未初始化；
 *         DELAY_ERR_OUT_OF_RANGE 超过当前计数器可表示范围。
 * @note 这是阻塞接口，会持续占用当前 CPU。只允许用于短硬件时序，禁止在
 *       中断服务函数中等待业务事件，也不建议在 RTOS 任务中长时间调用。
 *       中断仍可抢占本函数，因此实际延时可能大于请求值。
 * @details 函数把 us 转换为 CPU 周期数，向上取整后记录起始 CYCCNT；循环
 *          使用 (当前值 - 起始值) 的 DWORD 差值，因而能够正确处理一次
 *          计数器回绕。最大误差主要来自函数调用和循环本身的固定开销。
 */

delay_status_e delay_us_busy(DWORD us)
{
#if DELAY_USE_DWT
    QWORD cycles64;
    DWORD start;
    DWORD cycles;

    if (!g_delay_initialized)
    {
        return DELAY_ERR_NOT_INITIALIZED;
    }
    if (us == 0U)
    {
        return DELAY_OK;
    }
    if (us > delay_max_busy_us())
    {
        return DELAY_ERR_OUT_OF_RANGE;
    }

    cycles64 = ((QWORD)us * g_core_clock_hz + 999999ULL) / 1000000ULL;
    if ((cycles64 == 0U) || (cycles64 > UINT32_MAX))
    {
        return DELAY_ERR_OUT_OF_RANGE;
    }

    cycles = cycles64;
    start  = delay_port_dwt_cycles();
    /* 空循环体即可：delay_port_dwt_cycles() 是外部函数，编译器不能把它提到循环外。 */
    while ((delay_port_dwt_cycles() - start) < cycles)
    {
    }

    return DELAY_OK;
#else
    return delay_systick_us_busy(us);
#endif
}
/**
 * @brief 执行毫秒级忙等待。
 * @param ms 延时时间，单位毫秒；0 表示立即返回。
 * @return DELAY_OK 成功；DELAY_ERR_NOT_INITIALIZED 尚未初始化；
 *         DELAY_ERR_OUT_OF_RANGE 超过计数器单次忙等待上限。
 * @note 该接口不会让出 CPU，也不会调用 RTOS 调度器。业务代码应优先使用
 *       vTaskDelay、osDelay、软件定时器或事件等待；本接口只适用于启动阶段
 *       的短暂、确定性硬件稳定时间。
 * @details 函数先用 64 位乘法检查 ms * 1000 是否越界，再委托微秒接口，避免
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
 *       硬件来源由 delay_port_now_ms() 提供，本模块不定义 SysTick_Handler。
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
