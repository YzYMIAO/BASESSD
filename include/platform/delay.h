#ifndef PLATFORM_DELAY_H
#define PLATFORM_DELAY_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdbool.h>
#include <stdint.h>

/*
 * 平台延时/时钟抽象接口（共享逻辑在 src/platform/delay.c，SoC 适配在
 * src/platform/<soc>/delay_port.c）。
 *
 * 本头零 HAL 类型：核心模块（src/nand）以裸名 #include "delay.h" 消费，
 * 具体 SoC 的 DWT/SysTick/系统节拍来源细节一律留在实现目录。
 */

/* 默认用 SoC 侧已配置好的系统节拍；设为 1 可改用 DWT CYCCNT。 */
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
void delay_init(DWORD tick_clock_hz);
void delay_reconfigure(DWORD tick_clock_hz);
bool delay_is_initialized(void);

/*
 * 非阻塞超时的 DWT 周期时钟接口。
 * 这组接口只读取 CYCCNT，不会忙等待；start_cycles 允许在一次 32 位回绕内
 * 使用。NAND 的单次 tR/tPROG/tBERS 均远小于该范围。
 */
bool delay_dwt_cycles_now(DWORD *cycles);
bool delay_dwt_elapsed_us(DWORD start_cycles, WORD timeout_us);

/* 忙等待接口：适用于短硬件时序，不适合等待业务事件。 */
delay_status_t delay_us_busy(DWORD us);
delay_status_t delay_ms_busy(DWORD ms);
DWORD          delay_max_busy_us(void);

/* 兼容传统接口：忽略忙等待状态；短硬件时序优先使用上面的带状态接口。 */
void delay_us(DWORD us);
void delay_ms(DWORD ms);

/* 毫秒计数的回绕安全超时工具。 */
DWORD delay_now_ms(void);
bool  delay_elapsed_ms(DWORD start_ms, DWORD timeout_ms);

/*
 * ---- port 接口：由 src/platform/<soc>/delay_port.c 实现 ----
 * 共享逻辑（src/platform/delay.c）只通过这几个函数触碰 SoC —— 它自己不 include
 * 任何 SoC 头、不出现任何 HAL 类型，连 DWT/SysTick 寄存器都不直接读。
 */
DWORD delay_port_hclk_hz(void);       /* delay_init(0) 的「读 HCLK」回退 */
DWORD delay_port_core_clock_hz(void); /* CYCCNT 速率（SYSCLK，非 HCLK） */
DWORD delay_port_now_ms(void);        /* 系统单调毫秒计数 */
bool  delay_port_dwt_enable(void);    /* 使能 CYCCNT；返回硬件是否接受 */
DWORD delay_port_dwt_cycles(void);    /* DWT 自由运行周期计数器的当前值 */
DWORD delay_port_systick_load(void);  /* SysTick 重装值（一个周期的 tick 数） */
DWORD delay_port_systick_value(void); /* SysTick 当前计数值 */

#endif /* PLATFORM_DELAY_H */
