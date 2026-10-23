/*
 * bg_scheduler.c — 时间预算后台调度器（通用引擎 + FTL 任务表/档位策略）
 *
 * 文件分两段：
 *   · 引擎：只管理「must-run pass + 预算 pass」的机械执行（interval 门 / charge 计账 /
 *     表序优先），不知道任何具体后台任务。
 *   · FTL 策略：s_bg_tasks 任务表 + 10 个 static 适配器（把异构 kick 包成
 *     void(ftl_ctx_t*)）+ 档位→预算映射（s_bg_level_cfg）+ 任务表装载
 *     （bg_scheduler_init）+ 档位选择 + Phase 3 入口。
 *
 * 两个 pass：
 *   · Pass 0（must-run）：正确性关键集无条件每轮跑——不 gate、不占预算。
 *   · Pass 1（预算）：按优先级（表序）执行预算任务；interval 未到 / 本档位掩掉 / 预算
 *     耗尽即截断。elapsed 预算时钟 = 平台 time_us；时钟缺失时退化为「无门控」。
 *
 * 正确性关键集（must_run）无条件每轮跑、不 gate、不占预算——删了系统会挂：
 *   cp_handle / rr / mig_handle / metadata_dir / journal need_erase / 紧急 CP。
 * 全表每项的 interval / must_run / budget 依据见 bg_scheduler.h 头注与下方各适配器注释。
 *
 * ⚠ 每个任务在表项 `.mask` 里携带自己的位；表序只决定执行顺序，不兼职位号。
 * ⚠ 预算时钟 = 平台 time_us（SysTick 1ms 量化）；确定性前台保护由「档位→条数/掩码」
 *   承担（见 s_bg_level_cfg 与 bg_scheduler.h「关键语义」）。
 */

#include "bg_scheduler.h"

#include "bbt.h"
#include "checkpoint.h"
#include "ftl.h"
#include "ftl_job.h" /* ftl_job_now_ms()：全仓唯一时钟换算 */
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "migrate.h"
#include "pending.h"
#include "platform.h" /* ssd_platform_get / ssd_platform_ops_t.time_us */
#include "read_recovery.h"

/* ════════════════════════════════════════════════════════════════════════
 * 定义：宏与档位预算表
 * ════════════════════════════════════════════════════════════════════════ */

/* 低于此值不再跑任何预算任务（≈6×tR 余量：onfi_1_0.c NAND_TIME_TR_US=38 ⇒ 250 ≈ 6×38）。
 * 语义：预算剩余太少时，再跑一个任务既不划算也无法被诚实度量。 */
#define BG_MIN_TASK_BUDGET_US (250U)

/* 任务位：每个任务一个独立位，由表项 `.mask` 声明；表序只决定执行顺序，
 * 不再兼职掩码位号（C9：同一语义只允许一套坐标）。 */
#define BG_TASK_CP_JOIN    (1U << 0) /* 在飞 CP 跨轮推进（must-run，表首） */
#define BG_TASK_RR         (1U << 1) /* Read Recovery 推进（must-run） */
#define BG_TASK_MIGRATE    (1U << 2) /* 搬移引擎推进 + 自持节拍（must-run） */
#define BG_TASK_METADATA   (1U << 3) /* 目录整理（must-run） */
#define BG_TASK_JRN_ERASE  (1U << 4) /* Journal 换块擦除（must-run） */
#define BG_TASK_CP_EMERG   (1U << 5) /* 紧急 Checkpoint（must-run） */
#define BG_TASK_L2P_FLUSH  (1U << 6) /* L2P 后台写回（预算） */
#define BG_TASK_JRN_FLUSH  (1U << 7) /* Journal 刷页（预算） */
#define BG_TASK_BBT        (1U << 8) /* BBT 持久化（预算） */
#define BG_TASK_CP_TRIGGER (1U << 9) /* Checkpoint 周期触发（预算） */

/* 档位预设：只列出**预算任务**的位；must-run 由 bg_task_t.must_run 无条件执行。 */
#define BG_TASK_BUDGETED_ALL \
    (BG_TASK_L2P_FLUSH | BG_TASK_JRN_FLUSH | BG_TASK_BBT | BG_TASK_CP_TRIGGER)
#define BG_IDLE_TASKS   (BG_TASK_BUDGETED_ALL)
#define BG_NORMAL_TASKS (BG_TASK_BUDGETED_ALL)
#define BG_BUSY_TASKS   (BG_TASK_L2P_FLUSH)

/* 档位 → 预算规模（IDLE/NORMAL/BUSY）。锚定 onfi_1_0.c 时序（NAND_TIME_TPROG_US=700）：
 *  IDLE/NORMAL 2000µs ≈ 2~3 页写 + GC 全表求值；BUSY 700µs ≈ 恰 1 页写。
 * 掩码：IDLE/NORMAL 放行全部预算任务；BUSY 仅 l2p flush——即「仅 must-run + 1 flush」。
 * 档位差异由总预算与各模块内**自有**的策略表承担（如 l2p 的档位→max_kick）。 */
static const bg_level_cfg_t s_bg_level_cfg[BG_LEVEL_NUM] = {
    [BG_LEVEL_IDLE]   = {.total_budget_us = 2000U,   .task_mask = BG_IDLE_TASKS},
    [BG_LEVEL_NORMAL] = {.total_budget_us = 2000U, .task_mask = BG_NORMAL_TASKS},
    [BG_LEVEL_BUSY]   = { .total_budget_us = 700U,   .task_mask = BG_BUSY_TASKS},
};

/* ════════════════════════════════════════════════════════════════════════
 * 引擎
 * ════════════════════════════════════════════════════════════════════════ */

void bg_scheduler_run(bg_scheduler_t *sched, ftl_ctx_t *ftl, const bg_level_cfg_t *cfg)
{
    /* 前置条件（判据 2）：调用者传 &ftl->bg 与自持的 s_bg_level_cfg[] 表项。 */

    /* 发布本轮配置：任务（如 l2p flush 的条数上界）从这里读，不再各自反查档位表。
     * 档位（sched->level）由策略层 bg_scheduler_service 在同轮先写入，本引擎不改。 */
    sched->cfg = cfg;

    /* Pass 0：must-run 正确性关键集（不 gate、不占预算）。
     * 表序即执行序：cp_handle（表首）必须最先——在飞的 CP 要跨轮推进，
     * 拖到预算 pass 里会被低优先级/预算耗尽饿死。 */
    for (BYTE i = 0U; i < sched->task_count; i++)
    {
        if ((sched->tasks[i].func != NULL) && sched->tasks[i].must_run)
        {
            sched->tasks[i].func(ftl);
        }
    }

    /* Pass 1：预算 + interval + 优先级（表按执行序预排序，线性扫）。 */
    const ssd_platform_ops_t *platform         = ssd_platform_get();
    const bool                clock_ok         = (platform != NULL) && (platform->time_us != NULL);
    DWORD                     budget_remaining = cfg->total_budget_us;

    for (BYTE i = 0U; i < sched->task_count; i++)
    {
        bg_task_t *task = &sched->tasks[i];

        if ((task->func == NULL) || task->must_run)
        {
            continue;
        }
        if ((cfg->task_mask & task->mask) == 0U)
        {
            continue; /* 本档位不跑该任务 */
        }

        /* interval 门：仅时钟可用时生效；last_run_ms==0 = 尚未运行 ⇒ 立即跑（哨兵） */
        if (clock_ok && (task->interval_ms != 0U))
        {
            DWORD now_ms = (platform->time_us() / 1000U);
            if ((task->last_run_ms != 0U) && ((now_ms - task->last_run_ms) < task->interval_ms))
            {
                continue;
            }
        }

        /* 执行 + 计账：charge = MIN(elapsed, budget_us)。时钟缺失（clock_ok=0 或恒 0）
         * ⇒ elapsed=0、永不截断 —— 退化为「无门控」，防刷屏靠任务内部事件门。 */
        DWORD now_us = clock_ok ? platform->time_us() : 0U;
        task->func(ftl);
        if (clock_ok)
        {
            DWORD elapsed = (platform->time_us() - now_us);
            DWORD charge  = (elapsed < task->budget_us) ? elapsed : task->budget_us;

            budget_remaining = (budget_remaining > charge) ? (budget_remaining - charge) : 0U;
            if (task->interval_ms != 0U)
            {
                task->last_run_ms = (platform->time_us() / 1000U);
            }
        }

        if (budget_remaining < BG_MIN_TASK_BUDGET_US)
        {
            break; /* 预算耗尽：截断 */
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * FTL 策略（适配器 / 任务表 / 装配 / Phase 3 入口）
 * ════════════════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * bg_info_build
 *   功能：构造 typed 模块入口的 bg_info_t（本轮动态信息）。
 *   参数：ftl —— 实例。
 *   返回：{now_ms, load_level}。load_level 取引擎本轮发布的档位（bg_scheduler_run
 *         在跑任何任务前写入），不在这里重新选档——重选是第二套读路径。
 * -------------------------------------------------------------------- */
static bg_info_t bg_info_build(const ftl_ctx_t *ftl)
{
    bg_info_t info;

    info.now_ms     = ftl_job_now_ms();
    info.load_level = ftl->bg.level;
    return info;
}

/* 构造 Checkpoint 触发参数（ADVANCE / EMERGENCY / TIMER 三个适配器共用）。 */
static cp_trigger_t bg_cp_trigger_build(const ftl_ctx_t *ftl, cp_reason_e reason, bool force)
{
    cp_trigger_t trigger;

    trigger.now_ms            = ftl_job_now_ms();
    trigger.total_writes      = ftl->total_writes;
    trigger.journal_seq       = jrn_get_sequence(ftl->journal);
    trigger.jrn_get_watermark = jrn_get_watermark(ftl->journal);
    trigger.emergency         = jrn_is_emergency(ftl->journal);
    trigger.force             = force;
    trigger.reason            = reason;
    return trigger;
}

/* ── 10 个适配器（包异构 kick → void(ftl_ctx_t*)）── */

/* 表首 · must_run（引擎 Pass 0 最先执行）—— 推进**已在飞**的 Checkpoint job。
 * ⚠ 必须排在最前、且不占预算：CP 可能在 BUSY 档被紧急水位起（journal≥95%），后续步骤
 *   要跨多轮跨档位走完——排在预算 pass 里会被低优先级/预算耗尽饿死。 */
static void bg_task_cp_join(ftl_ctx_t *ftl)
{
    const cp_trigger_t trigger = bg_cp_trigger_build(ftl, CP_REASON_ADVANCE, false);
    cp_status_t        status;

    cp_handle(ftl->cp_handle, &trigger, &status);
}

/* 表 1 · must_run —— 推进 Read Recovery job。
 * host 读在等结果（托管的子操作占着组在飞数），不推进即死等；任何档位都要跑。 */
static void bg_task_rr(ftl_ctx_t *ftl)
{
    const bg_info_t info = bg_info_build(ftl);
    rr_status_t     status;

    rr_handle(&ftl->read_recovery_handle, &info, &status);
}

/* 表 2 · must_run —— 推进搬移 job（GC/WL/预测共用，已在飞则续跑下一步）。
 * 写路径的紧急 GC 在 BUSY 档起（wb_handle(FLUSH) 分配不到块 ⇒ FTL_AGAIN），
 * 不推进即死锁；任何档位都要跑。 */
static void bg_task_migrate(ftl_ctx_t *ftl)
{
    const bg_info_t info = bg_info_build(ftl);
    mig_status_t    status;

    /* typed 入口：在飞推进 + 不在飞时按 handle 自持节拍评估 GC/WL/predict。 */
    mig_handle(ftl->mig_handle, &info, &status);
}

/* 表 3 · must_run —— 元数据目录整理 job。
 * compact_wanted 是 append_slot 在**前台写路径**上置的（池吃紧时换块），
 * 不推进就是写路径永久 FTL_AGAIN；任何档位都要跑。 */
static void bg_task_metadata(ftl_ctx_t *ftl)
{
    const bg_info_t info = bg_info_build(ftl);
    md_status_t     status;

    md_handle(ftl->metadata_dir, &info, &status);
}

/* 表 4 · must_run —— 补「未起的换块擦除」（表 9 的子集；表 9 会被 BUSY 掩掉，本项不会）。
 * 新环形块没擦过就不能往里写 ⇒ 任何档位都要补，否则 host 写路径卡死。
 * 判据（有无待擦块 / 是否已有 job 在飞）在 journal 模块内，本文件不读其内部字段。 */
static void bg_task_journal_erase(ftl_ctx_t *ftl)
{
    const bg_info_t     info    = bg_info_build(ftl);
    const jrn_trigger_e trigger = JOURNAL_TRIGGER_ERASE_ONLY;
    jrn_status_t        status;

    jrn_handle(ftl->journal, &trigger, &info, &status);
}

/* 表 5 · must_run —— 紧急 Checkpoint（Journal ≥ 95%）。
 * 紧急水位必须**任何档位都随时响应**（BUSY 档也一样）。cp_job_begin 只装配 job、
 * 单飞行（cp_handle.active → FTL_AGAIN）幂等 ⇒ 每轮检查无副作用。 */
static void bg_task_cp_emergency(ftl_ctx_t *ftl)
{
    const cp_trigger_t trigger = bg_cp_trigger_build(ftl, CP_REASON_EMERGENCY, false);
    cp_status_t        status;

    cp_handle(ftl->cp_handle, &trigger, &status);
}

/* 表 6 · 预算 — 后台 flush 脏 L2P region，条数上界取**本轮档位配置**（引擎发布的
 * sched->cfg，见 bg_scheduler.h）。BUSY 档只放行本项 ⇒ 最小预算 = 「must-run + 1 flush」。
 * ⚠ thrashing 降级：主动 flush 大概率白写且挤前台带宽 ⇒ 停主动 flush。 */
static void bg_task_l2p_flush(ftl_ctx_t *ftl)
{
    const bg_info_t info = bg_info_build(ftl);
    l2p_status_t    status;

    /* typed 入口：档位 → max_kick 的 static const 表在 l2p 模块内；
     * thrashing 降级也在模块内判定。 */
    const DWORD dim = ftl_topo_dim(ftl->topo);

    l2p_handle(ftl->cache, dim, &info, &status);
}

/* 表 9 · 预算 — 推进 Journal 刷页 job（job 已在飞则空操作）。
 * BUSY 档被掩掉（host 饱和档不主动起页写；页满时 jrn_append 自己会起）。 */
static void bg_task_journal_flush(ftl_ctx_t *ftl)
{
    const bg_info_t info = bg_info_build(ftl);
    jrn_status_t    status;

    jrn_handle(ftl->journal, NULL, &info, &status);
}

/* 表 10 · 预算 — BBT 持久化（攒批写 NAND BBT；job 起步/续跑，提交即返回）。
 * typed 入口：状态只走上报结构（本适配器不读 handle 内部）。 */
static void bg_task_bbt(ftl_ctx_t *ftl)
{
    const bg_info_t info = bg_info_build(ftl);
    bbt_status_t    status;

    bbt_handle(&ftl->alloc, &info, &status);
}

/* 表 11 · 预算 — Checkpoint 触发检查（T1 Journal 水位 / T2 写入量 / T3 定时 elapsed）。
 * 定时基准**无条件**重置（触发即重置，不看成败——否则失败的 job 会每轮重触发）；
 * job 装配由 cp_job_begin（提交即返回），成败由 [CP] 心跳判。 */
static void bg_task_cp_trigger(ftl_ctx_t *ftl)
{
    const cp_trigger_t trigger = bg_cp_trigger_build(ftl, CP_REASON_TIMER, false);
    cp_status_t        status;

    cp_handle(ftl->cp_handle, &trigger, &status);
}

/* ── 任务映射表（放最下面：表项引用上面的适配器）── */

/* 表序 = 执行序 = 优先级；任务位由 .mask 显式声明，不再由表下标推导。
 * 加任务 = 表里插一行 + 新增 BG_TASK_* 位 + 视需要加入档位预设 + 同步 BG_TASK_COUNT。 */
static const bg_task_t s_bg_tasks[] = {
  /* 表 0..5 · must-run 正确性关键集：无条件每轮跑、不 gate、不占预算 */
    {      .func = bg_task_cp_join,    .mask = BG_TASK_CP_JOIN,   .must_run = true}, /* 在飞 CP 跨轮推进，必须最先 */
    {           .func = bg_task_rr,         .mask = BG_TASK_RR,   .must_run = true}, /* host 读在等结果，不推进即死等 */
    {      .func = bg_task_migrate,    .mask = BG_TASK_MIGRATE,   .must_run = true}, /* 写路径紧急 GC 在 BUSY 起，不推进即死锁 */
    {     .func = bg_task_metadata,   .mask = BG_TASK_METADATA,   .must_run = true}, /* 前台写路径置的压缩请求，不推进即永久 FTL_AGAIN */
    {.func = bg_task_journal_erase,  .mask = BG_TASK_JRN_ERASE,   .must_run = true}, /* 新环形块没擦不能写，BUSY 也要补 */
    { .func = bg_task_cp_emergency,   .mask = BG_TASK_CP_EMERG,   .must_run = true}, /* Journal ≥95%，任何档位随时响应 */

  /* 表 6..9 · 预算任务：interval 到点 + 本档位掩码放行才跑，占预算。
  * GC/WL/predict 已收进 migrate handle 自持节拍（docs §4.4），不再单列任务。 */
    {    .func = bg_task_l2p_flush,  .mask = BG_TASK_L2P_FLUSH, .budget_us = 1500U},
    {.func = bg_task_journal_flush,  .mask = BG_TASK_JRN_FLUSH,  .budget_us = 700U},
    {          .func = bg_task_bbt,        .mask = BG_TASK_BBT,  .budget_us = 700U},
    {   .func = bg_task_cp_trigger, .mask = BG_TASK_CP_TRIGGER,  .budget_us = 100U},
};

/* 表项数必须与 BG_TASK_COUNT 一致（capacity 是 tasks[] 维度；错位会越界或漏装载）。 */
_Static_assert((sizeof(s_bg_tasks) / sizeof(s_bg_tasks[0])) == BG_TASK_COUNT,
               "任务表项数与 BG_TASK_COUNT 不一致");

/* ── 装配与 Phase 3 ── */

/* bg_scheduler_init — 把 s_bg_tasks 装载进实例的调度器。由 ftl_init 在装配末尾调用一次。
 * 表长是编译期常量（上面的 _Static_assert 已证明 == BG_TASK_COUNT）⇒ 无运行期边界参数。 */
void bg_scheduler_init(bg_scheduler_t *sched)
{
    const BYTE count = (BYTE)(sizeof(s_bg_tasks) / sizeof(s_bg_tasks[0]));

    for (BYTE i = 0U; i < count; i++)
    {
        sched->tasks[i] = s_bg_tasks[i];
    }
    sched->task_count = count;
    sched->cfg        = NULL;
    sched->level      = BG_LEVEL_IDLE;
}

/* bg_scheduler_level_select — 负载 → 档位（纯函数，无副作用）：
 * inflight 全空 = IDLE；pending > 半池 或 有未决组 = BUSY；中间 = NORMAL。 */
bg_level_e bg_scheduler_level_select(const ftl_ctx_t *ftl)
{
    const WORD pending = pend_get_count(&ftl->pending);
    const WORD queued  = pend_get_group_count(&ftl->pending);

    if ((pending == 0U) && (queued == 0U))
    {
        return BG_LEVEL_IDLE;
    }
    if ((pending > (FTL_PENDING_MAX / 2U)) || (queued != 0U))
    {
        return BG_LEVEL_BUSY;
    }
    return BG_LEVEL_NORMAL;
}

/* bg_scheduler_service — Phase 3：bg_suspend 判定 → 选档 → 取档位配置 → 引擎跑一轮。
 *
 * ⚠ bg_suspend 必须**最先** return（早于 must-run pass）：`bbt --erase` 之后介质已空
 *   而 RAM 状态陈旧，任何后台写（含 must-run 里的 journal/CP/元数据目录）都是往干净
 *   芯片上倒垃圾。Phase 1/2/2.5 不受影响（NAND 泵 + host IO 照常）。
 *
 * 空闲度用 pending（host IO 未决）+ groups（未完成的 host 请求）近似：
 *   全空 = 真空闲 → IDLE 全力后台；有未完成 host 请求 = host 饿着 → BUSY 最少后台
 *   （让路，否则抢走 drain 要用的 FC buffer）；中间 → NORMAL 按需后台。
 *
 * ⚠ **空闲度不计 completion_table**：后台 job（journal / RR / 搬移）的命令也登记在
 *   completion_table 里，计进来会让「真空闲」永不成立——一个后台 job 在飞就把模式
 *   从 IDLE 顶到 NORMAL，后台反而被自己的命令饿死。后台 job 的活跃度由各自的
 *   各模块**内部**的单飞行门（不在这里窥探 phase/active）单独管，与 host 侧解耦。 */
void bg_scheduler_service(ftl_ctx_t *ftl)
{
    if (ftl->bg_suspend != 0U)
    {
        return;
    }

    const bg_level_e level = bg_scheduler_level_select(ftl);
    /* 档位下标越界不会崩：会静默读到相邻静态数据 ⇒ 预算/掩码全错而无声（H31/H40）。 */
    /* 档位先发布到调度器：typed 模块入口的 bg_info_t.load_level 取这里。 */
    ftl->bg.level          = level;
    bg_scheduler_run(&ftl->bg, ftl, &s_bg_level_cfg[level]);
}
