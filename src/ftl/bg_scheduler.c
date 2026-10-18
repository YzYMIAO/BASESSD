/*
 * bg_scheduler.c — 时间预算后台调度器引擎（v0.76）
 *
 * 通用引擎（策略无关）：只管理「must-run pass + 预算 pass」的机械执行，不知道任何
 * 具体后台任务（任务表/适配器/档位预算全在 ftl.c）。见 include/ftl/bg_scheduler.h 头注。
 *
 * 两个 pass：
 *   · Pass 0（must-run）：正确性关键集无条件每轮跑——不 gate、不占预算。
 *   · Pass 1（预算）：按优先级（表序）执行预算任务；interval 未到 / 本档位掩掉 / 预算
 *     耗尽即截断。elapsed 预算时钟 = 平台 time_us；时钟缺失时退化为「无门控」。
 */

#include "bg_scheduler.h"

#include "assertion.h"
#include "platform.h" /* ssd_platform_get / ssd_platform_ops_t.time_us */

/* 低于此值不再跑任何预算任务（≈6×tR 余量：onfi_1_0.c NAND_TIME_TR_US=38 ⇒ 250 ≈ 6×38）。
 * 语义：预算剩余太少时，再跑一个任务既不划算也无法被诚实度量。 */
#define MIN_TASK_BUDGET_US (250U)

void bg_scheduler_init(bg_scheduler_t *sched)
{
    /* 前置条件（判据 2）：唯一调用者 ftl.c 传 &ftl->bg。 */
    ASSERT(sched != NULL);

    for (uint8_t i = 0U; i < BG_TASK_COUNT; i++)
    {
        sched->tasks[i].func        = NULL;
        sched->tasks[i].interval_ms = 0U;
        sched->tasks[i].last_run_ms = 0U;
        sched->tasks[i].budget_us   = 0U;
        sched->tasks[i].priority    = 0U;
        sched->tasks[i].must_run    = 0U;
    }
    sched->task_count     = 0U;
    sched->level          = BG_LEVEL_IDLE;
    sched->round_total_us = 0U;
    sched->round_used_us  = 0U;
    sched->round_seq      = 0U;
    sched->clock_ok       = 0U;
}

void bg_scheduler_run(bg_scheduler_t *sched, ftl_context_t *ftl, const bg_level_cfg_t *cfg)
{
    /* 前置条件（判据 2）：唯一调用者 ftl.c 传 &ftl->bg 与自持的 g_bg_level_cfg[] 表项。 */
    ASSERT(sched != NULL);
    ASSERT(cfg != NULL);

    const ssd_platform_ops_t *platform = ssd_platform_get();
    sched->clock_ok                    = (platform != NULL) && (platform->time_us != NULL);
    sched->round_seq++;
    sched->round_total_us = cfg->total_budget_us;
    sched->round_used_us  = 0U;

    /* ── Pass 0：must-run 正确性关键集（不 gate、不占预算）──
     * 表序即执行序：checkpoint_kick（位 0）必须最先——在飞的 CP 要跨轮推进，
     * 拖到预算 pass 里会被低优先级/预算耗尽饿死。 */
    for (uint8_t i = 0U; i < sched->task_count; i++)
    {
        if ((sched->tasks[i].func != NULL) && (sched->tasks[i].must_run != 0U))
        {
            sched->tasks[i].func(ftl);
        }
    }

    /* ── Pass 1：预算 + interval + 优先级（表按 priority 升序预排序，线性扫）── */
    uint32_t budget_remaining = cfg->total_budget_us;

    for (uint8_t i = 0U; i < sched->task_count; i++)
    {
        bg_task_t *task = &sched->tasks[i];

        if ((task->func == NULL) || (task->must_run != 0U))
        {
            continue;
        }
        if ((cfg->task_mask & (1U << i)) == 0U)
        {
            continue; /* 本档位不跑该任务 */
        }

        /* interval 门：仅时钟可用时生效；last_run_ms==0 = 尚未运行 ⇒ 立即跑（哨兵） */
        if ((sched->clock_ok != 0U) && (task->interval_ms != 0U))
        {
            uint32_t now_ms = (uint32_t)(platform->time_us() / 1000U);
            if ((task->last_run_ms != 0U) && ((now_ms - task->last_run_ms) < task->interval_ms))
            {
                continue;
            }
        }

        /* 执行 + 计账：charge = MIN(elapsed, budget_us)。时钟缺失（clock_ok=0 或恒 0）
         * ⇒ elapsed=0、永不截断 —— 退化为「无门控」，防刷屏靠任务内部事件门。 */
        uint32_t now_us = (sched->clock_ok != 0U) ? (uint32_t)platform->time_us() : 0U;
        task->func(ftl);
        if (sched->clock_ok != 0U)
        {
            uint32_t elapsed = (uint32_t)(platform->time_us() - now_us);
            uint32_t charge  = (elapsed < task->budget_us) ? elapsed : task->budget_us;

            budget_remaining = (budget_remaining > charge) ? (budget_remaining - charge) : 0U;
            if (task->interval_ms != 0U)
            {
                task->last_run_ms = (uint32_t)(platform->time_us() / 1000U);
            }
        }

        if (budget_remaining < MIN_TASK_BUDGET_US)
        {
            break; /* 预算耗尽：截断 */
        }
    }

    sched->round_used_us = cfg->total_budget_us - budget_remaining;
}
