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
 * 具体 SoC 的 SysTick/系统节拍来源细节一律留在实现目录。
 *
 * 时基只用 ARM 核心自带的 SysTick：ms 由 HAL tick 提供，µs 由 SysTick 的
 * LOAD/VAL 在 1 ms 窗口内插值。DWT 是调试/跟踪单元，不作为产品时间源。
 */

typedef enum
{
    DELAY_OK = 0,
    DELAY_ERR_NOT_INITIALIZED,
    DELAY_ERR_OUT_OF_RANGE
} delay_status_e;

/* SysTick 已由 HAL 配置为 1 ms；本函数从 LOAD 反推每微秒周期数。 */
void delay_init(void);
bool delay_is_initialized(void);

/*
 * 非阻塞微秒时间接口（SysTick VAL/LOAD）。
 * 32-bit 微秒时间戳约 71.6 min 回绕；超时与差值一律用无符号差值比较。
 * NAND 的单次 tR/tPROG/tBERS 均远小于该范围。
 */
bool delay_time_now_us(DWORD *us);
bool delay_time_elapsed_us(DWORD start_us, WORD timeout_us);
/* 从 start_us 到当前的微秒数（未初始化时返回 0；用于分段计时）。 */
DWORD delay_time_since_us(DWORD start_us);
/* 64-bit 单调微秒时间（FTL / 平台长时轴；未初始化时返回 0）。 */
QWORD delay_time_now_us64(void);

/* 忙等待接口：适用于短硬件时序，不适合等待业务事件。 */
delay_status_e delay_us_busy(DWORD us);
delay_status_e delay_ms_busy(DWORD ms);
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
 * 任何 SoC 头、不出现任何 HAL 类型，连 SysTick 寄存器都不直接读。
 */
DWORD delay_port_hclk_hz(void);       /* HCLK（启动横幅） */
DWORD delay_port_core_clock_hz(void); /* CPU 时钟 SYSCLK（启动横幅） */
DWORD delay_port_now_ms(void);        /* 系统单调毫秒计数（SysTick/HAL tick） */
DWORD delay_port_systick_load(void);  /* SysTick 重装值（1 ms 的周期数） */
DWORD delay_port_systick_value(void); /* SysTick 当前计数值 */

#endif /* PLATFORM_DELAY_H */
