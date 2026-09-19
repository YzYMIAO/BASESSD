#ifndef SSD_STM32H743_BTIM_H
#define SSD_STM32H743_BTIM_H

#include "sys.h"

#include <stdbool.h>
#include <stdint.h>

/* TIM6 supplies the periodic hardware update event for the software scheduler. */
#define BTIM_TIMX      TIM6
#define BTIM_TIMX_IRQn TIM6_DAC_IRQn
#define BTIM_TIMX_CLK_ENABLE()       \
    do                               \
    {                                \
        __HAL_RCC_TIM6_CLK_ENABLE(); \
    } while (0)

/* A fixed table keeps scheduling deterministic and avoids dynamic allocation in firmware. */
#define BTIM_SCHEDULER_MAX_TASKS 16U

/*
 * Callback context is supplied by the owner. now_ms is a snapshot taken before
 * task-table iteration, so a callback should not assume it changes while it runs.
 * Callbacks execute in main/task context and may not block for long periods.
 */
typedef void (*btim_task_callback_t)(void *context, uint32_t now_ms);

extern TIM_HandleTypeDef g_btimx_handle;

/*
 * With APB1=100 MHz and APB1 prescaler=2, TIM6 runs at 200 MHz. For a 1 ms
 * tick use arr=1000-1 and psc=200-1.
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

/* Return the TIM6-derived monotonic millisecond time base. */
uint32_t btim_scheduler_now_ms(void);

/*
 * Execute due tasks. Call frequently from the main loop or one scheduler task;
 * never call this from a TIM6 ISR. Each delayed periodic task runs at most once
 * per poll, preventing a long pause from producing an unsafe callback burst.
 */
void btim_scheduler_poll(void);

#endif /* SSD_STM32H743_BTIM_H */
