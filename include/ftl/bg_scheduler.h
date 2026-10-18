#ifndef FTL_BG_SCHEDULER_H
#define FTL_BG_SCHEDULER_H

/*
 * bg_scheduler — 时间预算后台调度器（v0.76）
 *
 * 把 v0.75 的「三级后台模式」（IDLE/NORMAL/BUSY 按未决命令数分档 + 按"做几条"计数门控）
 * 升级为**每任务 interval + 每轮总预算 + 优先级**的任务表调度（对齐 §12.4 主流形态
 * `background_task_t` 参考；设计见 ftl_design.md §12.4）。
 *
 * 模块边界（本文件是**通用引擎**，零 kick 知识、策略无关）：
 *   · bg_scheduler.h/c —— 引擎：must-run pass + 预算 pass（interval 门 / charge 计账 /
 *     预算不足截断）。不 include 任何 kick 头，只前向声明 ftl_context_t。
 *   · ftl.c —— 接线：13 个 static 适配器（包异构 kick → void(ftl_context_t*)）+ 任务表
 *     （ftl_bg_init）+ 三级负载→预算映射（g_bg_level_cfg，IDLE/NORMAL/BUSY）+ 档位选择
 *     （ftl_bg_level_select）。档位策略是 FTL 域政策，引擎不持有。
 *
 * 关键语义：
 *   · must_run = 正确性关键集（rr / mig_service / metadata_dir / checkpoint_kick /
 *     紧急 CP / journal need_erase）：**无条件每轮跑、不受 interval 门控、不占预算**——
 *     它们是防死锁/防 host 读挂起的动作（删了系统会挂，见 ftl.c 三档函数注释）。
 *   · interval_ms = 最小执行间隔；0 = 不设调度器级间隔（任务内部事件门管节流，如
 *     journal 的页满门）。last_run_ms==0 作「尚未运行」哨兵（仿 gc_eval_last_ms）。
 *   · budget_us = 单次**计账上限**（charge = MIN(elapsed, budget_us)），不是执行中断/
 *     硬钳制：任务函数跑到返回为止，超出的时间只是不被计入 charge。
 *     ⚠ kick 全部「提交即返回」⇒ elapsed 只反映 CPU 时间（平台 time_us 实测 SysTick 1ms
 *     量化）；**确定性前台保护由档位 → l2p 条数/任务掩码承担**（见 ftl.c g_bg_level_cfg）。
 *   · 时钟缺失（time_us 缺失或恒 0）：interval 门与预算计账一并退化——last_run_ms 恒 0
 *     ⇒ 每轮都跑、elapsed=0 永不截断，即「无门控」，靠各任务内部单飞行/脏位/事件门防刷屏
 *     （与 GC 哨兵「每轮求值」/ CP T3「恒不触发」/ 心跳静默三分法一致）。
 */

#include <stdint.h>

/* 前向声明（ftl_context_t 在 ftl.h 中定义；本文件被 ftl.h include，
 * 用 struct 标签前向引用打破环，同 pending.h:42-45 手法） */
struct ftl_context;
typedef struct ftl_context ftl_context_t;

#define BG_TASK_COUNT (13U) /* 任务表容量（ftl.c ftl_bg_init 填满） */

/* ── 档位（负载 → 预算规模，见 ftl.c g_bg_level_cfg）── */
#define BG_LEVEL_IDLE   (0U)
#define BG_LEVEL_NORMAL (1U)
#define BG_LEVEL_BUSY   (2U)

/* 统一任务签名：适配器包异构 kick → void(ftl_context_t*) */
typedef void (*bg_task_func_t)(ftl_context_t *ftl);

typedef struct
{
    bg_task_func_t func;        /* 适配器（ftl.c 定义） */
    uint32_t       interval_ms; /* 最小执行间隔；0 = 不设调度器级间隔（内部事件门管） */
    uint32_t       last_run_ms; /* 上次运行时刻；0 = 尚未运行 ⇒ 立即跑（哨兵） */
    uint16_t       budget_us;   /* 单次计账上限：charge = MIN(elapsed, budget_us) */
    uint8_t        priority;    /* 越小越先跑（预算内按表序 = 优先级升序） */
    uint8_t        must_run;    /* 1 = 正确性关键集：不 gate、不占预算、无条件每轮跑 */
} bg_task_t;

typedef struct
{
    bg_task_t tasks[BG_TASK_COUNT];
    uint8_t   task_count;     /* ftl_bg_init 填充的实际项数 */
    uint8_t   level;          /* 本档位（观测/测试） */
    uint32_t  round_total_us; /* 本轮总预算（观测/测试） */
    uint32_t  round_used_us;  /* 本轮已消耗（观测/测试） */
    uint32_t  round_seq;      /* 单调轮次（观测/测试） */
    uint8_t   clock_ok;       /* 平台时钟可用性快照（降级分支用） */
} bg_scheduler_t;

/* 档位 → 预算规模（ftl.c 政策表，引擎不持有） */
typedef struct
{
    uint32_t total_budget_us; /* 每轮总预算（µs） */
    uint8_t  l2p_flush_max;   /* l2p flush 每轮条数上界（确定性前台保护下界） */
    uint32_t task_mask;       /* 本档位允许的 budgeted 任务掩码（bit = 任务下标） */
} bg_level_cfg_t;

/* 初始化调度器（清任务表 + 轮次计数；任务由 ftl.c ftl_bg_init 填充） */
void bg_scheduler_init(bg_scheduler_t *sched);

/* 跑一轮：Pass 0 must-run（无条件）+ Pass 1 预算/interval/优先级（预算不足截断）。
 * cfg 必须与 sched 当前档位配套（ftl.c 传 &g_bg_level_cfg[level]）。 */
void bg_scheduler_run(bg_scheduler_t *sched, ftl_context_t *ftl, const bg_level_cfg_t *cfg);

#endif /* FTL_BG_SCHEDULER_H */
