#include "delay_plus.h"

#include "common/common.h"
#include "stm32h7rsxx_hal.h"

static uint32_t s_core_clock_hz;
static bool     s_initialized;

/**
 * @brief 解析 CPU 时钟参数。
 * @param core_clock_hz 调用者提供的 CPU 时钟，单位 Hz；传入 0 表示使用
 *                      CMSIS 维护的 SystemCoreClock。
 * @return 实际用于换算延时的 CPU 时钟，单位 Hz。
 * @note 该函数不读取 RCC 寄存器，只相信 SystemCoreClock 当前值；时钟切换
 *       完成后应先更新 SystemCoreClock，再调用 delay_plus_reconfigure()。
 */
static uint32_t delay_plus_resolve_clock(uint32_t core_clock_hz)
{
    return (core_clock_hz != 0U) ? core_clock_hz : SystemCoreClock;
}

/**
 * @brief 启用 Cortex-M7 的 DWT 周期计数器。
 * @return true 表示 CYCCNT 已成功使能，false 表示硬件未接受使能位。
 * @note 实现原理是打开 CoreDebug->DEMCR 的 TRCENA 位，再打开 DWT->CTRL
 *       的 CYCCNTENA 位。该函数不关闭中断、不修改 SysTick 配置。
 */
static bool delay_plus_enable_dwt(void)
{
    /* DEMCR 的 TRCENA 是 DWT 计数器的总开关。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    /* 复位计数器，避免初始化前残留值影响第一次延时。 */
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    return (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U;
}

/**
 * @brief 初始化高精度延时服务。
 * @param core_clock_hz CPU 内核时钟频率，单位 Hz；传入 0 自动使用
 *                      SystemCoreClock。
 * @return 无。初始化结果通过 delay_plus_is_initialized() 查询。
 * @note 应在 HAL 时钟初始化完成后调用。若后续改变 PLL、分频或进入低功耗
 *       后恢复时钟，必须重新调用 delay_plus_reconfigure()。
 * @details 函数保存时钟频率，清零 DWT CYCCNT 并使能自由运行计数器，后续
 *          微秒延时通过读取该计数器的增量换算时间。
 */
void delay_plus_init(uint32_t core_clock_hz)
{
    s_core_clock_hz = delay_plus_resolve_clock(core_clock_hz);
    s_initialized   = (s_core_clock_hz != 0U) && delay_plus_enable_dwt();
}

/**
 * @brief 按新的 CPU 时钟重新配置延时服务。
 * @param core_clock_hz 新的 CPU 内核时钟，单位 Hz；传入 0 使用
 *                      SystemCoreClock。
 * @return 无。
 * @note 调用期间不应有其他线程同时执行微秒延时；推荐在系统时钟切换的
 *       临界初始化阶段调用。该函数不会替应用配置 SysTick。
 */
void delay_plus_reconfigure(uint32_t core_clock_hz)
{
    /* 动态调频、PLL 切换或从低功耗恢复后调用此函数。 */
    delay_plus_init(core_clock_hz);
}

/**
 * @brief 查询延时服务是否可用。
 * @param 无。
 * @return true 表示时钟有效且 DWT CYCCNT 已启用；false 表示尚未初始化或
 *         DWT 使能失败。
 */
bool delay_plus_is_initialized(void)
{
    return s_initialized;
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
uint32_t delay_plus_max_busy_us(void)
{
    uint64_t max_us;

    if ((s_core_clock_hz == 0U) || !s_initialized)
    {
        return FUNC_STATUS_SUCCESS;
    }

    /* DWT 为 32 位自由运行计数器，等待区间必须小于一个完整回绕周期。 */
    max_us = ((uint64_t)UINT32_MAX * 1000000ULL) / s_core_clock_hz;
    return (max_us > UINT32_MAX) ? UINT32_MAX : (uint32_t)max_us;
}

/**
 * @brief 执行微秒级忙等待。
 * @param us 延时时间，单位微秒；0 表示立即返回。
 * @return DELAY_PLUS_OK 成功；DELAY_PLUS_ERR_NOT_INITIALIZED 尚未初始化；
 *         DELAY_PLUS_ERR_OUT_OF_RANGE 超过当前计数器可表示范围。
 * @note 这是阻塞接口，会持续占用当前 CPU。只允许用于短硬件时序，禁止在
 *       中断服务函数中等待业务事件，也不建议在 RTOS 任务中长时间调用。
 *       中断仍可抢占本函数，因此实际延时可能大于请求值。
 * @details 函数把 us 转换为 CPU 周期数，向上取整后记录起始 CYCCNT；循环
 *          使用 (当前值 - 起始值) 的 uint32_t 差值，因而能够正确处理一次
 *          计数器回绕。最大误差主要来自函数调用和循环本身的固定开销。
 */
delay_plus_status_t delay_plus_delay_us_busy(uint32_t us)
{
    uint64_t cycles64;
    uint32_t cycles;
    uint32_t start;

    if (!s_initialized)
    {
        return DELAY_PLUS_ERR_NOT_INITIALIZED;
    }
    if (us == 0U)
    {
        return DELAY_PLUS_OK;
    }
    if (us > delay_plus_max_busy_us())
    {
        return DELAY_PLUS_ERR_OUT_OF_RANGE;
    }

    /* 向上取整，保证不会少等；64 位中间值避免乘法溢出。 */
    cycles64 = ((uint64_t)us * s_core_clock_hz + 999999ULL) / 1000000ULL;
    if ((cycles64 == 0U) || (cycles64 > UINT32_MAX))
    {
        return DELAY_PLUS_ERR_OUT_OF_RANGE;
    }
    cycles = (uint32_t)cycles64;
    start  = DWT->CYCCNT;

    /* uint32_t 减法天然支持一次回绕；调用者不得超过上限。 */
    while ((uint32_t)(DWT->CYCCNT - start) < cycles)
    {
        __NOP();
    }

    return DELAY_PLUS_OK;
}

/**
 * @brief 执行毫秒级忙等待。
 * @param ms 延时时间，单位毫秒；0 表示立即返回。
 * @return DELAY_PLUS_OK 成功；DELAY_PLUS_ERR_NOT_INITIALIZED 尚未初始化；
 *         DELAY_PLUS_ERR_OUT_OF_RANGE 超过 DWT 单次忙等待上限。
 * @note 该接口不会让出 CPU，也不会调用 RTOS 调度器。业务代码应优先使用
 *       vTaskDelay、osDelay、软件定时器或事件等待；本接口只适用于启动阶段
 *       的短暂、确定性硬件稳定时间。
 * @details 函数先用 64 位乘法检查 ms * 1000 是否越界，再委托微秒接口，避免
 *          直接使用 32 位乘法造成超时截断。
 */
delay_plus_status_t delay_plus_delay_ms_busy(uint32_t ms)
{
    uint32_t max_us;

    if (ms == 0U)
    {
        return DELAY_PLUS_OK;
    }
    max_us = delay_plus_max_busy_us();
    if ((max_us == 0U) || ((uint64_t)ms * 1000ULL > max_us))
    {
        return (max_us == 0U) ? DELAY_PLUS_ERR_NOT_INITIALIZED
                              : DELAY_PLUS_ERR_OUT_OF_RANGE;
    }

    return delay_plus_delay_us_busy(ms * 1000U);
}

/**
 * @brief 读取系统单调毫秒计数。
 * @param 无。
 * @return HAL tick 当前值，单位毫秒。
 * @note 计数器为 uint32_t，持续运行约 49.7 天后回绕；调用者应使用
 *       delay_plus_elapsed_ms() 进行差值比较，不要直接比较绝对大小。HAL
 *       tick 的中断更新由应用或 RTOS 负责，本模块不定义 SysTick_Handler。
 */
uint32_t delay_plus_now_ms(void)
{
    return HAL_GetTick();
}

/**
 * @brief 判断从起始时间到当前时间是否已经达到超时值。
 * @param start_ms 起始时间，通常由 delay_plus_now_ms() 获得。
 * @param timeout_ms 超时时间，单位毫秒。
 * @return true 表示已超时，false 表示尚未超时。
 * @note 使用 (now - start) 的无符号差值，因此可以正确处理一次 HAL tick
 *       回绕。timeout_ms 不应大于 UINT32_MAX；对于超长周期应使用 RTC 或
 *       64 位时间服务。
 * @details 该函数只读时钟，不阻塞、不等待，也不会清除或修改任何定时器。
 */
bool delay_plus_elapsed_ms(uint32_t start_ms, uint32_t timeout_ms)
{
    return (uint32_t)(delay_plus_now_ms() - start_ms) >= timeout_ms;
}
