#ifndef FTL_BG_SCHEDULER_H
#define FTL_BG_SCHEDULER_H

/*
 * bg_scheduler — 时间预算后台调度器引擎
 *
 * 模块边界（本文件是**通用引擎**，零 kick 知识、策略无关）：
 *   · bg_scheduler.h/c —— 引擎：must-run pass + 预算 pass（interval 门 / charge 计账 /
 *     预算不足截断）。不 include 任何 kick 头，只前向声明 ftl_context_t。
 *   · bg_wiring.c —— 接线：任务表 s_bg_tasks + 13 个 static 适配器（包异构 kick →
 *     void(ftl_context_t*)）+ 档位→预算映射（g_bg_level_cfg）+ 档位选择
 *     （ftl_bg_level_select）+ Phase 3 入口（ftl_bg_service）。档位策略是 FTL 域政策，
 *     引擎不持有。设计见 docs/design/ftl_design.md §3.12。
 *
 * 关键语义：
 *   · must_run = 正确性关键集（清单见 bg_wiring.c 的任务表）：**无条件每轮跑、不受
 *     interval 门控、不占预算**——它们是防死锁/防 host 读挂起的动作，删了系统会挂。
 *   · interval_ms = 最小执行间隔；0 = 不设调度器级间隔（任务内部事件门管节流，如
 *     journal 的页满门）。last_run_ms == 0 作「尚未运行」哨兵 ⇒ 立即跑。
 *   · budget_us = 单次**计账上限**（charge = MIN(elapsed, budget_us)），不是执行中断/
 *     硬钳制：任务函数跑到返回为止，超出的时间只是不被计入 charge。
 *     ⚠ kick 全部「提交即返回」⇒ elapsed 只反映 CPU 时间（平台 time_us 实测 SysTick
 *     1ms 量化）；**确定性前台保护由档位 → l2p 条数/任务掩码承担**（见 bg_wiring.c）。
 *   · 时钟缺失（time_us 缺失或恒 0）：interval 门与预算计账一并失效——last_run_ms 恒 0
 *     ⇒ 每轮都跑、elapsed=0 永不截断，即「无门控」，靠各任务内部单飞行/脏位/事件门
 *     防刷屏。⚠ 本板 `ssd_platform_register` 强制 time_us 非空 ⇒ 该分支不可达。
 */

#include <stdbool.h>
#include <stdint.h>

/* 前向声明（ftl_context_t 在 ftl.h 中定义；本文件被 ftl.h include，
 * 用 struct 标签前向引用打破环） */
struct ftl_context;
typedef struct ftl_context ftl_context_t;

/* tasks[] 容量。装载表的项数不得超出——超出即越界写 tasks[] 并踩掉 task_count。
 * 装载点 bg_wiring.c 用 _Static_assert 在编译期挡住（那里同时看得见表与容量），
 * bg_scheduler_init 另有一条运行期 ASSERT 兜其他调用者（见其注释）。 */
#define BG_TASK_COUNT (13U)

/* ── 档位（负载 → 预算规模，见 bg_wiring.c g_bg_level_cfg）── */
#define BG_LEVEL_IDLE   (0U)
#define BG_LEVEL_NORMAL (1U)
#define BG_LEVEL_BUSY   (2U)
#define BG_LEVEL_NUM    (3U) /* 值域上界：档位表数组维度 + 档位下标的边界断言 */

/* 统一任务签名：适配器包异构 kick → void(ftl_context_t*) */
typedef void (*bg_task_func_t)(ftl_context_t *ftl);

typedef struct
{
    bg_task_func_t func;        /* 适配器（bg_wiring.c 定义） */
    uint32_t       interval_ms; /* 最小执行间隔；0 = 不设调度器级间隔（内部事件门管） */
    uint32_t       last_run_ms; /* 上次运行时刻；0 = 尚未运行 ⇒ 立即跑（哨兵） */
    uint16_t       budget_us;   /* 单次计账上限：charge = MIN(elapsed, budget_us) */
    bool           must_run;    /* true = 正确性关键集：不 gate、不占预算、无条件每轮跑 */
} bg_task_t;

/* 档位 → 预算规模（bg_wiring.c 政策表，引擎不持有） */
typedef struct
{
    uint32_t total_budget_us; /* 每轮总预算（µs） */
    uint8_t  l2p_flush_max;   /* l2p flush 每轮条数上界（确定性前台保护下界） */
    uint32_t task_mask;       /* 本档位允许的 budgeted 任务掩码（bit = 任务下标） */
} bg_level_cfg_t;

typedef struct
{
    bg_task_t tasks[BG_TASK_COUNT];
    uint8_t   task_count; /* 已装载的项数 */
    /* 本轮生效的档位配置，由 bg_scheduler_run 在起点发布。
     * 任务读本轮策略参数走这里，不各自反查档位表——反查是第二套读取路径，
     * 与引擎实际使用的那份配置可能不同步。 */
    const bg_level_cfg_t *cfg;
} bg_scheduler_t;

/* 装载任务表：拷贝 table[0..count) 到 tasks[]、置 task_count、清 cfg。
 * ⚠ count > BG_TASK_COUNT 会越界写 tasks[] ⇒ 调用者先用 _Static_assert 在编译期挡住
 * （见 bg_wiring.c s_bg_tasks）。 */
void bg_scheduler_init(bg_scheduler_t *sched, const bg_task_t *table, uint8_t count);

/* 跑一轮：Pass 0 must-run（无条件）+ Pass 1 预算/interval（**表序即优先级**，预算不足截断）。
 * cfg 必须与 sched 当前档位配套（bg_wiring.c 传 &g_bg_level_cfg[level]）。 */
void bg_scheduler_run(bg_scheduler_t *sched, ftl_context_t *ftl, const bg_level_cfg_t *cfg);

#endif /* FTL_BG_SCHEDULER_H */
