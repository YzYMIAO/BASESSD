#ifndef FTL_BG_SCHEDULER_H
#define FTL_BG_SCHEDULER_H

/*
 * bg_scheduler — 工作量配额后台调度器（通用引擎 + FTL 任务表/档位策略）
 *
 * 一个模块同时承载两层内容：
 *   · 引擎：必跑任务 + 预算任务（工作量配额 + 墙钟兜底）。
 *   · FTL 策略：任务表 s_bg_tasks + 10 个 static 适配器（包异构 kick →
 *     void(ftl_ctx_t*)）+ 档位→配额映射（s_bg_level_cfg）+ 档位选择
 *     （bg_scheduler_level_select）+ 后台调度步入口（bg_scheduler_service）。
 * 设计见 docs/design/ftl_design.md §3.12。
 *
 * 引擎函数本身不知道任何具体后台任务；FTL 任务表与档位策略集中在本文件的
 * 「FTL 策略」段。两者放在同一编译单元，是为了让任务表的容量/掩码约束与
 * 引擎的 tasks[] 布局在同处可见（加任务时不会跨文件漂移）；代价是引擎不能再
 * 脱离 FTL 头文件独立编译。
 *
 * 关键语义：
 *   · must_run = 正确性关键集（清单见 s_bg_tasks）：**无条件每轮跑、不占配额**
 *     ——它们是防死锁/防 host 读挂起的动作，删了系统会挂。
 *   · task_mask = 本档位允许的预算任务位集合（BG_IDLE/NORMAL/BUSY_TASKS 预设）；
 *     每个表项用 `.mask` 声明自己的位，表序只决定执行顺序。
 *   · units = 单次调用占用的**工作量单位**（≈ 本轮最多提交的条数；L2P 的具体条数
 *     由模块内 max_kick 再收一层）。配额是**确定性**的：不读时钟、与机器速度无关。
 *   · total_units = 本档位每轮配额；配额不足即截断（表序 = 优先级）。
 *   · 时间只做**异常兜底**：单轮墙钟超过 BG_ROUND_HARD_LIMIT_US 即截断——只能在
 *     任务之间检查，不能抢占；时钟缺失时该兜底失效，靠档位掩码 + 模块内
 *     单飞行/脏位/事件门防刷屏。
 */

#include "ftl_type.h"  /* bg_level_e */
#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdbool.h>
#include <stdint.h>

/* tasks[] 容量。装载表的项数不得超出——超出即越界写 tasks[] 并踩掉 task_count。
 * 装载点 s_bg_tasks 用 _Static_assert 在编译期挡住（那里同时看得见表与容量）；
 * bg_scheduler_init 的装载数直接取表长，不接收运行期 count 参数。 */
#define BG_TASK_COUNT (10U)

/* ── 档位（负载 → 配额规模，见 bg_scheduler.c s_bg_level_cfg；
 *    bg_level_e / BG_LEVEL_NUM 在 ftl_type.h）── */

/* 统一任务签名：适配器包异构 kick → void(ftl_ctx_t*) */
typedef void (*bg_task_func_t)(ftl_ctx_t *ftl);

typedef struct
{
    bg_task_func_t func;     /* 适配器（FTL 策略段定义） */
    DWORD          mask;     /* 任务位（BG_TASK_*，定义见 bg_scheduler.c）；预算任务按档位 task_mask 选择 */
    WORD           units;    /* 单次调用的工作量单位（≈ 本轮最多提交的条数） */
    bool           must_run; /* true = 正确性关键集：不 gate、不占配额、无条件每轮跑 */
} bg_task_t;

/* 档位 → 工作量配额（FTL 策略表，引擎不持有） */
typedef struct
{
    DWORD total_units; /* 每轮工作量配额（单位见 bg_task_t.units） */
    DWORD task_mask;   /* 本档位允许的预算任务位集合（BG_TASK_*，定义见 bg_scheduler.c） */
} bg_level_cfg_t;

/* 每实例一份的后台调度器状态（任务表本身是共享只读，不逐实例复制） */
typedef struct
{
    const bg_task_t *tasks;      /* 指向静态任务表 s_bg_tasks（init 绑定一次） */
    BYTE             task_count; /* 已装载的项数 */
    /* 本轮生效的档位配置，由 bg_scheduler_run 在起点发布。
     * 任务读本轮策略参数走这里，不各自反查档位表——反查是第二套读取路径，
     * 与引擎实际使用的那份配置可能不同步。 */
    const bg_level_cfg_t *cfg;
    /* 本轮档位（BG_LEVEL_*），同样由 bg_scheduler_run 发布。
     * 模块入口的档位就取这里，避免适配器各自再选一次档。 */
    bg_level_e level;
} bg_scheduler_t;

/* ── 引擎接口 ── */

/**
 * @brief 跑一轮：先无条件跑 must-run，再按表序在总预算内跑其余任务（**表序即优先级**，预算不足截断）。
 *
 * cfg 必须与 sched 当前档位配套（本文件 bg_scheduler_service 传 &s_bg_level_cfg[level]）。
 * 档位本身由策略层在调用前写入 sched->level（模块入口据此读本轮档位）。
 *
 * @param sched 本实例的调度器（任务表与本轮配置的发布点）。
 * @param ftl   FTL 上下文（原样传给每个任务适配器）。
 * @param cfg   本轮档位配置（起点发布到 sched->cfg，任务从这里读本轮策略参数）。
 */
void bg_scheduler_run(bg_scheduler_t *sched, ftl_ctx_t *ftl, const bg_level_cfg_t *cfg);

/* ── FTL 策略接口 ── */

/**
 * @brief 装载内置任务表 s_bg_tasks：拷贝到 tasks[]、置 task_count、清 cfg。
 * 表长是编译期常量（_Static_assert 保证 == BG_TASK_COUNT）⇒ 函数无表参数。
 *
 * @param sched 本实例的调度器（被写入 tasks[] / task_count，并复位 cfg 与 level）。
 */
void bg_scheduler_init(bg_scheduler_t *sched);

/**
 * @brief 负载 → 后台档位（纯函数）
 *
 * 判据：
 *   inflight = pend_slot_get_count + pend_group_get_count
 *   inflight==0 ⇒ IDLE；pending > FTL_PENDING_MAX/2 或 queued!=0 ⇒ BUSY；否则 NORMAL。
 *
 * @param ftl FTL 上下文（读 pending 的 slot / group 计数）。
 *
 * @return BG_LEVEL_IDLE   pending 与 queued 全空（真空闲）
 *         BG_LEVEL_NORMAL 中间负载
 *         BG_LEVEL_BUSY   pending 过半池或有未决组
 */
bg_level_e bg_scheduler_level_select(const ftl_ctx_t *ftl);

/**
 * @brief [svc] 后台调度步入口：bg_suspend 判定 → 选档 → 档位配置 → 引擎跑一轮
 *
 * ⚠ bg_suspend 判定在函数内**最先**做（早于必跑任务）：`bbt --erase` 之后介质已空
 *   而 RAM 状态陈旧，任何后台写都是往干净芯片上倒垃圾。调用方不要在外层重复判。
 *
 * @param ftl  FTL 上下文（**非 NULL**；唯一调用点 ftl_service 已保证）
 */
void bg_scheduler_service(ftl_ctx_t *ftl);

#endif /* FTL_BG_SCHEDULER_H */
