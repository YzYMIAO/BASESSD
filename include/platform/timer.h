#ifndef PLATFORM_TIMER_H
#define PLATFORM_TIMER_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdbool.h>
#include <stdint.h>

/*
 * 平台毫秒调度器抽象接口（共享逻辑在 src/platform/timer.c，SoC 适配在
 * src/platform/<soc>/timer_port.c）。
 *
 * 时基是基本定时器的 1 ms 心跳（ISR 只递增毫秒计数）；到期扫描与回调在主循环。
 * 本头零 HAL 类型：定时器句柄、中断号与时钟使能宏一律留在 SoC 侧。
 */

/* A fixed table keeps scheduling deterministic and avoids dynamic allocation in firmware.
 * 64 槽：平台任务 + 8 个 FTL 实例 × 若干周期评估。 */
#define TIMER_SCHEDULER_MAX_TASKS 64U

/*
 * Callback context is supplied by the owner. now_ms is a snapshot taken before
 * task-table iteration, so a callback should not assume it changes while it runs.
 * Callbacks execute in main/task context and may not block for long periods.
 */
typedef void (*timer_task_callback_t)(void *context, DWORD now_ms);

/*
 * 按给定时基启动硬件心跳（周期中断）并开始计时。
 * 参数含义与具体定时器有关，例如「重装值 / 预分频」，由 SoC 侧解释。
 */
void timer_timx_int_init(WORD arr, WORD psc);

/*
 * 初始化 timer.c 内部的固定任务表。
 * 核心后台任务由 timer.c 的 g_timer_tasks 集中注册；各业务模块只实现自己的
 * callback，main.c 只负责按顺序启动功能模块和调度器。
 */
void timer_scheduler_init(void);

/* 可选运行时注册接口：first_delay_ms 相对当前时刻；period_ms == 0 ⇒ 一次性。 */
bool timer_scheduler_add(timer_task_callback_t callback, void *context, DWORD first_delay_ms, DWORD period_ms, DWORD *task_id);

/* 绝对时刻注册：first_abs_ms 早于当前时刻时按「立即到期」处理。 */
bool timer_scheduler_add_at(timer_task_callback_t callback, void *context, DWORD first_abs_ms, DWORD period_ms, DWORD *task_id);

/* Enable/disable a registered task without losing its timing configuration. */
bool timer_scheduler_enable(DWORD task_id, bool enabled);

/* 修改周期（下一次到期后生效）；period_ms == 0 将该任务改为一次性。 */
bool timer_scheduler_set_period(DWORD task_id, DWORD period_ms);

/* Remove a task and return its static slot to the table. */
bool timer_scheduler_remove(DWORD task_id);

/* 当前已注册任务数（含静态表）。 */
WORD timer_scheduler_task_count(void);

/* Return the hardware-heartbeat-derived monotonic millisecond time base. */
DWORD timer_scheduler_now_ms(void);

/*
 * Execute due tasks. Call frequently from the main loop or one scheduler task.
 * Each delayed periodic task runs at most once per poll, preventing a long pause
 * from producing an unsafe callback burst.
 */
void timer_scheduler_poll(void);

/*
 * ---- 共享入口：由 port 侧的中断回调调用 ----
 */

/* 硬件心跳到来一次：推进共享毫秒计数（唯一写入者）。 */
void timer_scheduler_tick(void);

/*
 * ---- port 接口：由 src/platform/<soc>/timer_port.c 实现 ----
 */

/* 定时器中断入口：共享逻辑不持有定时器句柄。 */
void timer_port_irq_handler(void);

#endif /* PLATFORM_TIMER_H */
