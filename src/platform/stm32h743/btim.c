#include "btim.h"

#include "watchdog.h"

#include <string.h>

/* TIM6 is used only as the scheduler's millisecond hardware heartbeat. */
TIM_HandleTypeDef g_btimx_handle = {0};

/* 和 UART CLI 命令表一样，这是产品固定功能的只读注册描述。 */
typedef struct
{
    const char          *name;
    uint32_t             first_delay_ms;
    uint32_t             period_ms;
    btim_task_callback_t callback;
    void                *context;
} btim_static_task_t;

/*
 * 调度过程会修改下次到期时间、运行次数和使能状态，因此这些变量不能放进 const
 * 注册表。将其独立出来后，静态注册和运行时状态的边界清晰，也避免意外改写配置。
 */
typedef struct
{
    btim_task_callback_t callback;
    void                *context;
    uint32_t             next_due_ms;
    uint32_t             period_ms;
    uint32_t             runs;
    uint8_t              enabled;
} btim_task_runtime_t;

#define BTIM_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/*
 * 固定任务注册表。
 *
 * 表在调度器模块内集中维护，便于审查一个产品镜像实际有哪些周期后台工作。
 * 每个功能模块只导出自己的 task callback：例如 watchdog.c 实现
 * storage_watchdog_task()，它不关心 TIM6、遍历顺序或其他任务。
 *
 * 未列出的槽位保持 callback=NULL，仍可由 btim_scheduler_add() 用于确实需要
 * 在运行中创建和撤销的临时任务。核心安全任务不使用动态槽位。
 */
static const btim_static_task_t g_btim_task_table[] = {
  /* 仅 IWDG 时不需要 WWDG 的毫秒级窗口检查；100 ms 足以监督 30 s 名义超时。 */
    {"watchdog", 100U, 100U, storage_watchdog_task, NULL},
};

/* 私有运行态：前 N 项对应静态表；其余槽位保留给 btim_scheduler_add()。 */
static btim_task_runtime_t g_btim_tasks[BTIM_SCHEDULER_MAX_TASKS];
static volatile uint32_t   g_btim_tick_ms;

static uint8_t btim_time_due(uint32_t now_ms, uint32_t due_ms)
{
    /* Signed difference makes deadline comparison work across uint32_t tick wrap. */
    return ((int32_t)(now_ms - due_ms) >= 0) ? 1U : 0U;
}

void btim_scheduler_init(void)
{
    uint32_t index;
    uint32_t now_ms;

    /* 固定表越界属于编译配置错误；此处防御性返回，避免运行时内存越界。 */
    if (BTIM_ARRAY_SIZE(g_btim_task_table) > BTIM_SCHEDULER_MAX_TASKS)
    {
        return;
    }

    now_ms = btim_scheduler_now_ms();
    memset(g_btim_tasks, 0, sizeof(g_btim_tasks));

    for (index = 0U; index < BTIM_ARRAY_SIZE(g_btim_task_table); index++)
    {
        const btim_static_task_t *config  = &g_btim_task_table[index];
        btim_task_runtime_t      *runtime = &g_btim_tasks[index];

        runtime->callback    = config->callback;
        runtime->context     = config->context;
        runtime->next_due_ms = now_ms + config->first_delay_ms;
        runtime->period_ms   = config->period_ms;
        runtime->enabled     = 1U;
    }
}

void btim_timx_int_init(uint16_t arr, uint16_t psc)
{
    g_btimx_handle.Instance               = BTIM_TIMX;
    g_btimx_handle.Init.Prescaler         = psc;
    g_btimx_handle.Init.Period            = arr;
    g_btimx_handle.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_Base_Init(&g_btimx_handle);
    (void)HAL_TIM_Base_Start_IT(&g_btimx_handle);
}

bool btim_scheduler_add(btim_task_callback_t callback, void *context, uint32_t first_delay_ms, uint32_t period_ms, uint32_t *task_id)
{
    uint32_t index;
    uint32_t now_ms;

    if (callback == NULL)
    {
        return false;
    }

    now_ms = btim_scheduler_now_ms();
    /* 固定任务的槽位不可被动态任务占用。 */
    for (index = (uint32_t)BTIM_ARRAY_SIZE(g_btim_task_table); index < BTIM_SCHEDULER_MAX_TASKS; index++)
    {
        if (g_btim_tasks[index].callback != NULL)
        {
            continue;
        }

        g_btim_tasks[index].callback    = callback;
        g_btim_tasks[index].context     = context;
        g_btim_tasks[index].next_due_ms = now_ms + first_delay_ms;
        g_btim_tasks[index].period_ms   = period_ms;
        g_btim_tasks[index].runs        = 0U;
        g_btim_tasks[index].enabled     = 1U;
        if (task_id != NULL)
        {
            *task_id = index;
        }
        return true;
    }

    return false;
}

bool btim_scheduler_enable(uint32_t task_id, bool enabled)
{
    if (task_id >= BTIM_SCHEDULER_MAX_TASKS || g_btim_tasks[task_id].callback == NULL)
    {
        return false;
    }

    g_btim_tasks[task_id].enabled = enabled ? 1U : 0U;
    return true;
}

bool btim_scheduler_remove(uint32_t task_id)
{
    /* 静态表项是产品核心配置，不能被运行时 remove() 意外删除。 */
    if ((task_id < BTIM_ARRAY_SIZE(g_btim_task_table)) || (task_id >= BTIM_SCHEDULER_MAX_TASKS))
    {
        return false;
    }

    memset(&g_btim_tasks[task_id], 0, sizeof(g_btim_tasks[task_id]));
    return true;
}

uint32_t btim_scheduler_now_ms(void)
{
    /* A naturally aligned 32-bit Cortex-M load is atomic; caller gets a time snapshot. */
    return g_btim_tick_ms;
}

void btim_scheduler_poll(void)
{
    uint32_t index;
    uint32_t now_ms = btim_scheduler_now_ms();

    for (index = 0U; index < BTIM_SCHEDULER_MAX_TASKS; index++)
    {
        btim_task_runtime_t *task = &g_btim_tasks[index];
        uint32_t             due_ms;

        if ((task->callback == NULL) || (task->enabled == 0U) || !btim_time_due(now_ms, task->next_due_ms))
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
            if (btim_time_due(now_ms, task->next_due_ms))
            {
                task->next_due_ms = now_ms + task->period_ms;
            }
        }

        task->runs++;
        task->callback(task->context, now_ms);
    }
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != BTIM_TIMX)
    {
        return;
    }

    BTIM_TIMX_CLK_ENABLE();
    /* TIM6 only increments time; keep it below urgent data-plane interrupts. */
    HAL_NVIC_SetPriority(BTIM_TIMX_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(BTIM_TIMX_IRQn);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == BTIM_TIMX)
    {
        /* The ISR's entire scheduler role is advancing a monotonically increasing tick. */
        g_btim_tick_ms++;
    }
}
