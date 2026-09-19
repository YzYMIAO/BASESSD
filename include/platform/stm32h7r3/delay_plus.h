#ifndef PLATFORM_STM32H7R3_DELAY_PLUS_H
#define PLATFORM_STM32H7R3_DELAY_PLUS_H

/*
 * 企业级时间服务接口
 *
 * 设计原则：
 * 1. 微秒级延时使用 DWT->CYCCNT 忙等待，不修改 SysTick，也不接管中断向量。
 * 2. 毫秒级业务等待应优先使用 RTOS 延时或事件；本模块只提供明确标注为
 *    busy 的阻塞接口，以及基于 HAL tick 的超时判断工具。
 * 3. 所有接口均不关闭全局中断。中断可以抢占忙等待，因此实际延时可能变长，
 *    但不会因为本模块而增加不可抢占区。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        DELAY_PLUS_OK = 0,
        DELAY_PLUS_ERR_INVALID_ARG,
        DELAY_PLUS_ERR_NOT_INITIALIZED,
        DELAY_PLUS_ERR_OUT_OF_RANGE
    } delay_plus_status_t;

    /* 使用 0 时自动读取 CMSIS 的 SystemCoreClock。 */
    void delay_plus_init(uint32_t core_clock_hz);

    /* 时钟频率发生变化后必须重新调用；参数 0 仍表示读取 SystemCoreClock。 */
    void delay_plus_reconfigure(uint32_t core_clock_hz);

    /* 返回 DWT 是否已经成功启用。 */
    bool delay_plus_is_initialized(void);

    /*
     * 微秒级忙等待。
     * 适用：外设复位、GPIO 时序、芯片手册规定的短稳定时间。
     * 约束：最大时长受 32 位 DWT 计数器限制，具体上限由
     * delay_plus_max_busy_us() 返回；禁止在此接口中等待业务事件。
     */
    delay_plus_status_t delay_plus_delay_us_busy(uint32_t us);

    /* 毫秒级忙等待，仅用于启动阶段或极短的硬件初始化等待。 */
    delay_plus_status_t delay_plus_delay_ms_busy(uint32_t ms);

    /* 返回 HAL 系统单调毫秒计数。HAL tick 的中断实现由应用或 RTOS 负责。 */
    uint32_t delay_plus_now_ms(void);

    /* 使用无符号回绕安全算法判断超时，适用于 uint32_t tick 回绕场景。 */
    bool delay_plus_elapsed_ms(uint32_t start_ms, uint32_t timeout_ms);

    /* 返回当前时钟配置下单次忙等待允许的最大微秒数。 */
    uint32_t delay_plus_max_busy_us(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_STM32H7R3_DELAY_PLUS_H */
