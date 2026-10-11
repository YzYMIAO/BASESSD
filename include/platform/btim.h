#ifndef PLATFORM_BTIM_H
#define PLATFORM_BTIM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 平台毫秒调度器抽象接口（共享逻辑在 src/platform/btim.c，SoC 适配在
 * src/platform/<soc>/btim_port.c）。
 *
 * 本头零 HAL 类型：定时器句柄、中断号与时钟使能宏一律留在 SoC 侧。
 */

/* A fixed table keeps scheduling deterministic and avoids dynamic allocation in firmware. */
#define BTIM_SCHEDULER_MAX_TASKS 16U

/*
 * Callback context is supplied by the owner. now_ms is a snapshot taken before
 * task-table iteration, so a callback should not assume it changes while it runs.
 * Callbacks execute in main/task context and may not block for long periods.
 */
typedef void (*btim_task_callback_t)(void *context, uint32_t now_ms);

/*
 * 按给定时基启动硬件心跳（周期中断）并开始计时。
 * 参数含义与具体定时器有关，例如「重装值 / 预分频」，由 SoC 侧解释。
 */
void btim_timx_int_init(uint16_t arr, uint16_t psc);

/*
 * 初始化 btim.c 内部的固定任务表。
 * 核心后台任务由 btim.c 的 g_btim_tasks 集中注册；各业务模块只实现自己的
 * callback，main.c 只负责按顺序启动硬件、功能模块和调度器。
 */
void btim_scheduler_init(void);

/* 可选运行时注册接口，适合确实随模块生命周期出现/撤销的临时任务。 */
bool btim_scheduler_add(btim_task_callback_t callback, void *context, uint32_t first_delay_ms, uint32_t period_ms, uint32_t *task_id);

/* Enable/disable a registered task without losing its timing configuration. */
bool btim_scheduler_enable(uint32_t task_id, bool enabled);

/* Remove a task and return its static slot to the table. */
bool btim_scheduler_remove(uint32_t task_id);

/* Return the hardware-heartbeat-derived monotonic millisecond time base. */
uint32_t btim_scheduler_now_ms(void);

/*
 * Execute due tasks. Call frequently from the main loop or one scheduler task;
 * never call this from the heartbeat ISR. Each delayed periodic task runs at most once
 * per poll, preventing a long pause from producing an unsafe callback burst.
 */
void btim_scheduler_poll(void);

/*
 * ---- 共享入口：由 port 侧的中断回调调用 ----
 */

/* 硬件心跳到来一次：推进共享毫秒计数（唯一写入者）。 */
void btim_scheduler_tick(void);

/*
 * ---- port 接口：由 src/platform/<soc>/btim_port.c 实现 ----
 */

/* 定时器中断入口：共享逻辑不持有定时器句柄。 */
void btim_port_irq_handler(void);

#endif /* PLATFORM_BTIM_H */
