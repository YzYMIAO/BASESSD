#ifndef PLATFORM_DELAY_H
#define PLATFORM_DELAY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 平台延时/时钟抽象接口（实现在 src/platform/<soc>/delay.c）。
 *
 * 本头零 HAL 类型：核心模块（src/nand）以裸名 #include "delay.h" 消费，
 * 具体 SoC 的 DWT/SysTick/系统节拍来源细节一律留在实现目录。
 */

/* 默认使用 HAL 配置的 SysTick；设为 1 可改用 DWT CYCCNT。 */
#ifndef DELAY_USE_DWT
#    define DELAY_USE_DWT 0U
#endif

typedef enum
{
    DELAY_OK = 0,
    DELAY_ERR_NOT_INITIALIZED,
    DELAY_ERR_OUT_OF_RANGE
} delay_status_t;

/* tick_clock_hz 为 SysTick 时钟 Hz；传入 0 时读取当前 HCLK。 */
void delay_init(uint32_t tick_clock_hz);
void delay_reconfigure(uint32_t tick_clock_hz);
bool delay_is_initialized(void);

/*
 * 非阻塞超时的 DWT 周期时钟接口。
 * 这组接口只读取 CYCCNT，不会忙等待；start_cycles 允许在一次 32 位回绕内
 * 使用。NAND 的单次 tR/tPROG/tBERS 均远小于该范围。
 */
bool delay_dwt_cycles_now(uint32_t *cycles);
bool delay_dwt_elapsed_us(uint32_t start_cycles, uint16_t timeout_us);

/* 忙等待接口：适用于短硬件时序，不适合等待业务事件。 */
delay_status_t delay_us_busy(uint32_t us);
delay_status_t delay_ms_busy(uint32_t ms);
uint32_t       delay_max_busy_us(void);

/* 兼容传统接口：忽略忙等待状态；短硬件时序优先使用上面的带状态接口。 */
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

/* HAL Tick 的回绕安全超时工具。 */
uint32_t delay_now_ms(void);
bool     delay_elapsed_ms(uint32_t start_ms, uint32_t timeout_ms);

#endif /* PLATFORM_DELAY_H */
