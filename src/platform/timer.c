/*
 * timer.c — 毫秒级固定任务调度器（跨 target 共享逻辑）
 *
 * 本文件不 include 任何 SoC 头、不出现任何 HAL 类型：基本定时器的心跳来源
 * （哪个定时器、哪个中断号、时钟怎么开）全部下沉到 src/platform/<soc>/timer_port.c。
 * ISR 只推进毫秒计数；本文件维护任务表与到期判定，扫描在主循环。
 */

#include "timer.h"

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */
#include "watchdog.h"

#include <string.h>

/* 和 UART CLI 命令表一样，这是产品固定功能的只读注册描述。 */
typedef struct TIMER_STATIC_TASK_S
{
    const char              *name;
    DWORD                    first_delay_ms;
    DWORD                    period_ms;
    timer_task_callback_fn_t callback;
    void                    *context;
} timer_static_task_s;

/*
 * 调度过程会修改下次到期时间、运行次数和使能状态，因此这些变量不能放进 const
 * 注册表。将其独立出来后，静态注册和运行时状态的边界清晰，也避免意外改写配置。
 */
typedef struct TIMER_TASK_RUNTIME_S
{
    timer_task_callback_fn_t callback;
    void                    *context;
    DWORD                    next_due_ms;
    DWORD                    period_ms;
    DWORD                    runs;
    BYTE                     enabled;
} timer_task_runtime_s;

#define TIMER_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/*
 * 固定任务注册表。
 *
 * 表在调度器模块内集中维护，便于审查一个产品镜像实际有哪些周期后台工作。
 * 每个功能模块只导出自己的 task callback：例如 watchdog.c 实现
 * storage_watchdog_task()，它不关心硬件心跳来源、遍历顺序或其他任务。
 *
 * 未列出的槽位保持 callback=NULL，仍可由 timer_scheduler_add() 用于确实需要
 * 在运行中创建和撤销的临时任务。核心安全任务不使用动态槽位。
 */
static const timer_static_task_s g_timer_task_table[] = {
  /* 仅 IWDG 时不需要 WWDG 的毫秒级窗口检查；100 ms 足以监督 30 s 名义超时。 */
    {"watchdog", 100U, 100U, storage_watchdog_task, NULL},
};

/* 私有运行态：前 N 项对应静态表；其余槽位保留给 timer_scheduler_add()。 */
static timer_task_runtime_s g_timer_tasks[TIMER_SCHEDULER_MAX_TASKS];
static volatile DWORD       g_timer_tick_ms;

void timer_scheduler_tick(void)
{
    /*
     * 硬件心跳的唯一后果就是把毫秒计数加一。ISR 的全部调度职责到此为止——
     * 真正的到期扫描留给主循环的 timer_scheduler_poll()，避免在中断里跑回调。
     */
    g_timer_tick_ms++;
}

void timer_scheduler_init(void)
{
    DWORD index;
    DWORD now_ms;

    /* 固定表越界属于编译配置错误；此处防御性返回，避免运行时内存越界。 */
    if (TIMER_ARRAY_SIZE(g_timer_task_table) > TIMER_SCHEDULER_MAX_TASKS)
    {
        return;
    }

    now_ms = timer_scheduler_now_ms();
    memset(g_timer_tasks, 0, sizeof(g_timer_tasks));

    for (index = 0U; index < TIMER_ARRAY_SIZE(g_timer_task_table); index++)
    {
        const timer_static_task_s *config  = &g_timer_task_table[index];
        timer_task_runtime_s      *runtime = &g_timer_tasks[index];

        runtime->callback    = config->callback;
        runtime->context     = config->context;
        runtime->next_due_ms = now_ms + config->first_delay_ms;
        runtime->period_ms   = config->period_ms;
        runtime->enabled     = 1U;
    }
}

BOOL timer_scheduler_add(timer_task_callback_fn_t callback, void *context, DWORD first_delay_ms, DWORD period_ms, DWORD *task_id)
{
    DWORD index;
    DWORD now_ms;

    if (callback == NULL)
    {
        return false;
    }

    now_ms = timer_scheduler_now_ms();
    /* 固定任务的槽位不可被动态任务占用。 */
    for (index = TIMER_ARRAY_SIZE(g_timer_task_table); index < TIMER_SCHEDULER_MAX_TASKS; index++)
    {
        if (g_timer_tasks[index].callback != NULL)
        {
            continue;
        }

        g_timer_tasks[index].callback    = callback;
        g_timer_tasks[index].context     = context;
        g_timer_tasks[index].next_due_ms = now_ms + first_delay_ms;
        g_timer_tasks[index].period_ms   = period_ms;
        g_timer_tasks[index].runs        = 0U;
        g_timer_tasks[index].enabled     = 1U;
        if (task_id != NULL)
        {
            *task_id = index;
        }
        return true;
    }

    return false;
}

BOOL timer_scheduler_add_at(timer_task_callback_fn_t callback, void *context, DWORD first_abs_ms, DWORD period_ms, DWORD *task_id)
{
    const DWORD now_ms         = timer_scheduler_now_ms();
    // 绝对时刻已过 ⇒ 立即到期；比较走有符号差，DWORD 回绕安全。
    const DWORD first_delay_ms = (timer_time_due(now_ms, first_abs_ms) != 0U) ? 0U : (first_abs_ms - now_ms);

    return timer_scheduler_add(callback, context, first_delay_ms, period_ms, task_id);
}

BOOL timer_scheduler_enable(DWORD task_id, BOOL enabled)
{
    if (task_id >= TIMER_SCHEDULER_MAX_TASKS || g_timer_tasks[task_id].callback == NULL)
    {
        return false;
    }

    g_timer_tasks[task_id].enabled = enabled ? 1U : 0U;
    return true;
}

BOOL timer_scheduler_set_period(DWORD task_id, DWORD period_ms)
{
    if ((task_id >= TIMER_SCHEDULER_MAX_TASKS) || (g_timer_tasks[task_id].callback == NULL))
    {
        return false;
    }

    /* 下一次到期后按新周期重装；当前 next_due 不变。 */
    g_timer_tasks[task_id].period_ms = period_ms;
    return true;
}

BOOL timer_scheduler_remove(DWORD task_id)
{
    /* 静态表项是产品核心配置，不能被运行时 remove() 意外删除。 */
    if ((task_id < TIMER_ARRAY_SIZE(g_timer_task_table)) || (task_id >= TIMER_SCHEDULER_MAX_TASKS))
    {
        return false;
    }

    memset(&g_timer_tasks[task_id], 0, sizeof(g_timer_tasks[task_id]));
    return true;
}

WORD timer_scheduler_task_count(void)
{
    WORD count = 0U;

    for (DWORD index = 0U; index < TIMER_SCHEDULER_MAX_TASKS; index++)
    {
        if (g_timer_tasks[index].callback != NULL)
        {
            count++;
        }
    }

    return count;
}

DWORD timer_scheduler_now_ms(void)
{
    /* A naturally aligned 32-bit Cortex-M load is atomic; caller gets a time snapshot. */
    return g_timer_tick_ms;
}

void timer_scheduler_poll(void)
{
    DWORD index;
    DWORD now_ms = timer_scheduler_now_ms();

    for (index = 0U; index < TIMER_SCHEDULER_MAX_TASKS; index++)
    {
        timer_task_runtime_s *task = &g_timer_tasks[index];
        DWORD                 due_ms;

        if ((task->callback == NULL) || (task->enabled == 0U) || !timer_time_due(now_ms, task->next_due_ms))
        {
            continue;
        }

        /*
         * Re-arm before invoking the callback. A task may disable/remove itself,
         * and a long callback must not accumulate an unbounded number of catch-up runs.
         */
        due_ms = task->next_due_ms;
        if (task->period_ms == 0U)
        {
            task->enabled = 0U;
        }
        else
        {
            task->next_due_ms = due_ms + task->period_ms;
            if (timer_time_due(now_ms, task->next_due_ms))
            {
                task->next_due_ms = now_ms + task->period_ms;
            }
        }

        task->runs++;
        task->callback(task->context, now_ms);
    }
}
