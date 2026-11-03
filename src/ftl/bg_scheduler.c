/*
 * bg_scheduler.c — 工作量配额后台调度器：引擎 + FTL 任务表/档位策略
 *
 * 本文件两段：
 *   · 引擎（本段）：机械执行「必跑任务（must-run）」与「预算任务」两批，不认识任何具体任务。
 *   · FTL 策略段（下方）：10 个适配器 + 任务表 s_bg_tasks + 档位表 s_bg_level_cfg
 *     + 装配 / 选档 / 后台调度步入口。
 *
 * 语义定义（must_run / units / grant_units / token bucket / 墙钟兜底）见 `bg_scheduler.h` 头注，
 * 设计说明见 `docs/design/ftl_design.md §3.12`——不在此重复，避免两份漂移。
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

// 单轮墙钟异常兜底：预算任务累计耗时超过此值即截断（只在任务之间检查，不能抢占）。
// 正常任务全部「提交即返回」，实测单轮远低于此值；超过说明某个 kick 异常膨胀。
#define BG_ROUND_HARD_LIMIT_US (4000U)

// 任务位：每个任务一个独立位，由表项 `.mask` 声明；表序只决定执行顺序，
// 不兼职掩码位号（C9：同一语义只允许一套坐标）。
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

// 档位预设：只列出**预算任务**的位；must-run 由 bg_task_t.must_run 无条件执行。
#define BG_TASK_BUDGETED_ALL (BG_TASK_L2P_FLUSH | BG_TASK_JRN_FLUSH | BG_TASK_BBT | BG_TASK_CP_TRIGGER)
#define BG_IDLE_TASKS        (BG_TASK_BUDGETED_ALL)
#define BG_NORMAL_TASKS      (BG_TASK_BUDGETED_ALL)
#define BG_BUSY_TASKS        (BG_TASK_L2P_FLUSH)

// 档位 → 每个 token interval 的 credit 配额。单位见 bg_task_t.units：一次 kick
// ≈ 1 条命令（L2P 的具体条数由模块内 max_kick 再限）。IDLE/NORMAL 放行全部 4 个
// 预算任务；BUSY 只放行 L2P 写回（1 个），即「仅 must-run + 1 flush」。
// ⚠ credit 由 timer 任务按 BG_TOKEN_INTERVAL_MS 补充；实际速率 = grant_units /
//   BG_TOKEN_INTERVAL_MS，与主循环频率无关。数值需上板按 GC/WL/host 延迟调。
static const bg_level_cfg_t s_bg_level_cfg[BG_LEVEL_NUM] = {
    [BG_LEVEL_IDLE]   = {.grant_units = 4U,   .task_mask = BG_IDLE_TASKS},
    [BG_LEVEL_NORMAL] = {.grant_units = 4U, .task_mask = BG_NORMAL_TASKS},
    [BG_LEVEL_BUSY]   = {.grant_units = 1U,   .task_mask = BG_BUSY_TASKS},
};

/* ════════════════════════════════════════════════════════════════════════
 * 引擎
 * ════════════════════════════════════════════════════════════════════════ */

// 执行一轮：先无条件跑 must-run（表序即优先级），再在总预算内按表序跑非 must-run。
void bg_scheduler_run(bg_scheduler_t *sched, ftl_ctx_t *ftl, const bg_level_cfg_t *cfg)
{
    // 发布本轮配置：任务（如 l2p flush 的条数上界）从这里读，不各自反查档位表。
    // 档位（sched->level）由策略层 bg_scheduler_service 在同轮先写入，本引擎不改。
    sched->cfg = cfg;

    // 必跑任务：正确性关键集（不 gate、不占预算）。
    // 表序即执行序：cp_handle（表首）必须最先——在飞的 CP 要跨轮推进，
    // 拖到预算任务里会被低优先级/预算耗尽饿死。
    for (BYTE i = 0U; i < sched->task_count; i++)
    {
        if ((sched->tasks[i].func != NULL) && sched->tasks[i].must_run)
        {
            sched->tasks[i].func(ftl);
        }
    }

    // 预算任务：按表序 + token bucket credit（表已按执行序预排序，线性扫）。
    // credit 由 bg_scheduler_refill 在 timer 任务里补充；must-run 不消耗 credit。
    const ssd_platform_ops_t *platform   = ssd_platform_get();
    const bool                clock_ok   = (platform != NULL) && (platform->time_us != NULL);
    const QWORD               start_us   = (clock_ok == true) ? platform->time_us() : 0U;
    DWORD                     units_left = sched->credits;

    for (BYTE i = 0U; i < sched->task_count; i++)
    {
        const bg_task_t *task = &sched->tasks[i];

        if ((task->func == NULL) || task->must_run)
        {
            continue;
        }
        if ((cfg->task_mask & task->mask) == 0U)
        {
            continue; /* 本档位不跑该任务 */
        }

        if (task->units > units_left)
        {
            break; /* 配额不足：截断（表序即优先级） */
        }

        task->func(ftl);
        units_left -= task->units;

        // 异常兜底：单轮墙钟超限即截断。不能抢占，只在任务之间检查；
        // 时钟缺失时失效，靠档位掩码与模块内事件门。
        if ((clock_ok == true) && ((platform->time_us() - start_us) >= BG_ROUND_HARD_LIMIT_US))
        {
            break;
        }
    }

    // 只把实际消费后的余额写回：被掩码跳过 / credit 不足的任务不扣。
    sched->credits = units_left;
}

// 补充预算 credit：timer 任务每 BG_TOKEN_INTERVAL_MS 调一次。
// 上限 = grant_units × BG_TOKEN_CAP_TICKS；grant_units 是编译期 1..4 的档位值，
// 乘法不会溢出。cfg 尚未发布（首次 bg_scheduler_run 之前）时不补。
void bg_scheduler_refill(bg_scheduler_t *sched)
{
    if (sched->cfg == NULL)
    {
        return;
    }

    const DWORD grant = sched->cfg->grant_units;
    const DWORD cap   = grant * BG_TOKEN_CAP_TICKS;

    if (sched->credits >= (cap - grant))
    {
        sched->credits = cap;
    }
    else
    {
        sched->credits += grant;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * FTL 策略（适配器 / 任务表 / 装配 / 后台调度步入口）
 * ════════════════════════════════════════════════════════════════════════ */

/* ── 10 个适配器（包异构 kick → void(ftl_ctx_t*)）── */

/**
 * @brief 表首 · must_run（引擎最先执行的一批）—— 推进**已在飞**的 Checkpoint job。
 *
 * ⚠ 必须排在最前、且不占预算：CP 可能在 BUSY 档被紧急水位起（journal≥95%），后续步骤
 * 要跨多轮跨档位走完——排在预算任务里会被低优先级/预算耗尽饿死。
 *
 * @param ftl FTL 上下文（推进其 cp_handle 上的在飞 job）。
 */
static void bg_task_cp_join(ftl_ctx_t *ftl)
{
    const cp_trigger_t trigger = ftl_cp_trigger_build(ftl, CP_REASON_ADVANCE, false);

    cp_handle(ftl->cp_handle, &trigger);
}

/**
 * @brief 表 1 · must_run —— 推进 Read Recovery job。
 * host 读在等结果（托管的子操作占着组在飞数），不推进即死等；任何档位都要跑。
 *
 * @param ftl FTL 上下文（推进其 read_recovery_handle）。
 */
static void bg_task_rr(ftl_ctx_t *ftl)
{
    rr_handle(&ftl->read_recovery_handle);
}

/**
 * @brief 表 2 · must_run —— 推进搬移 job（GC/WL/预测共用，已在飞则续跑下一步）。
 * 写路径的紧急 GC 在 BUSY 档起（wb_handle(FLUSH) 分配不到块 ⇒ FTL_AGAIN），
 * 不推进即死锁；任何档位都要跑。
 *
 * @param ftl FTL 上下文（推进其 mig_handle；不在飞时按其自持节拍评估）。
 */
static void bg_task_migrate(ftl_ctx_t *ftl)
{
    // 在飞推进 + 会话收尾 + 紧急 GC；周期评估由 timer 表的 mig_eval 任务负责。
    mig_handle(ftl->mig_handle, ftl->bg.level);
}

/**
 * @brief 表 3 · must_run —— 元数据目录整理 job。
 * compact_wanted 是 append_slot 在**前台写路径**上置的（池吃紧时换块），
 * 不推进就是写路径永久 FTL_AGAIN；任何档位都要跑。
 *
 * @param ftl FTL 上下文（推进其 metadata_dir）。
 */
static void bg_task_metadata(ftl_ctx_t *ftl)
{
    md_handle(ftl->metadata_dir);
}

/**
 * @brief 表 4 · must_run —— 补「未起的换块擦除」（表 7 的子集；表 7 会被 BUSY 掩掉，本项不会）。
 * 新环形块没擦过就不能往里写 ⇒ 任何档位都要补，否则 host 写路径卡死。
 * 判据（有无待擦块 / 是否已有 job 在飞）在 journal 模块内，本文件不读其内部字段。
 *
 * @param ftl FTL 上下文（其 journal 收到 JRN_TRIGGER_ERASE_ONLY）。
 */
static void bg_task_journal_erase(ftl_ctx_t *ftl)
{
    const jrn_trigger_e trigger = JRN_TRIGGER_ERASE_ONLY;

    jrn_handle(ftl->journal, &trigger);
}

/**
 * @brief 表 5 · must_run —— 紧急 Checkpoint（Journal ≥ 95%）。
 * 紧急水位必须**任何档位都随时响应**（BUSY 档也一样）。cp_job_begin 只装配 job、
 * 单飞行（cp_handle.active → FTL_AGAIN）幂等 ⇒ 每轮检查无副作用。
 *
 * @param ftl FTL 上下文（按紧急水位触发其 cp_handle）。
 */
static void bg_task_cp_emergency(ftl_ctx_t *ftl)
{
    const cp_trigger_t trigger = ftl_cp_trigger_build(ftl, CP_REASON_EMERGENCY, false);

    cp_handle(ftl->cp_handle, &trigger);
}

/**
 * @brief 表 6 · 预算 — 后台 flush 脏 L2P region，条数上界取**本轮档位配置**（引擎发布的
 * sched->cfg，见 bg_scheduler.h）。BUSY 档只放行本项 ⇒ 最小预算 = 「must-run + 1 flush」。
 * ⚠ thrashing 降级：主动 flush 大概率白写且挤前台带宽 ⇒ 停主动 flush。
 *
 * @param ftl FTL 上下文（读其 cache 与 topo 维度，并把本轮档位传给模块）。
 */
static void bg_task_l2p_flush(ftl_ctx_t *ftl)
{
    // 时间节拍由 timer 任务置位；未到期不踢，避免每轮扫一遍 owner 脏位图。
    if (ftl->l2p_flush_due == 0U)
    {
        return;
    }
    ftl->l2p_flush_due = 0U;

    // 档位 → max_kick 的 static const 表在 l2p 模块内；thrashing 降级也在模块内判定。
    const DWORD dim = ftl_topo_dim(ftl->topo);

    l2p_handle(ftl->cache, dim, ftl->bg.level);
}

/**
 * @brief 表 7 · 预算 — 推进 Journal 刷页 job（job 已在飞则空操作）。
 * BUSY 档被掩掉（host 饱和档不主动起页写；页满时 jrn_append 自己会起）。
 *
 * @param ftl FTL 上下文（推进其 journal 的刷页 job）。
 */
static void bg_task_journal_flush(ftl_ctx_t *ftl)
{
    jrn_handle(ftl->journal, NULL);
}

/**
 * @brief 表 8 · 预算 — BBT 持久化（攒批写 NAND BBT；job 起步/续跑，提交即返回）。
 * typed 入口：本适配器不读 handle 内部状态。
 *
 * @param ftl FTL 上下文（传其 alloc 给 bbt_handle）。
 */
static void bg_task_bbt(ftl_ctx_t *ftl)
{
    bbt_handle(&ftl->alloc);
}

/**
 * @brief 表 9 · 预算 — Checkpoint 触发检查（T1 Journal 水位 / T2 写入量 / T3 定时 elapsed）。
 * 定时基准**无条件**重置（触发即重置，不看成败——否则失败的 job 会每轮重触发）；
 * job 装配由 cp_job_begin（提交即返回），成败由 [CP] 心跳判。
 *
 * @param ftl FTL 上下文（按定时/水位/写入量触发其 cp_handle）。
 */
static void bg_task_cp_trigger(ftl_ctx_t *ftl)
{
    const cp_trigger_t trigger = ftl_cp_trigger_build(ftl, CP_REASON_TIMER, false);

    cp_handle(ftl->cp_handle, &trigger);
}

/* ── 任务映射表（放最下面：表项引用上面的适配器）── */

// 表序 = 执行序 = 优先级；任务位由 .mask 显式声明，不由表下标推导。
// 加任务 = 表里插一行 + 新增 BG_TASK_* 位 + 视需要加入档位预设 + 同步 BG_TASK_COUNT。
static const bg_task_t s_bg_tasks[] = {
  // 表 0..5 · must-run 正确性关键集：无条件每轮跑、不 gate、不占预算
    {      .func = bg_task_cp_join,    .mask = BG_TASK_CP_JOIN, .must_run = true}, /* 在飞 CP 跨轮推进，必须最先 */
    {           .func = bg_task_rr,         .mask = BG_TASK_RR, .must_run = true}, /* host 读在等结果，不推进即死等 */
    {      .func = bg_task_migrate,    .mask = BG_TASK_MIGRATE, .must_run = true}, /* 写路径紧急 GC 在 BUSY 起，不推进即死锁 */
    {     .func = bg_task_metadata,   .mask = BG_TASK_METADATA, .must_run = true}, /* 前台写路径置的压缩请求，不推进即永久 FTL_AGAIN */
    {.func = bg_task_journal_erase,  .mask = BG_TASK_JRN_ERASE, .must_run = true}, /* 新环形块没擦不能写，BUSY 也要补 */
    { .func = bg_task_cp_emergency,   .mask = BG_TASK_CP_EMERG, .must_run = true}, /* Journal ≥95%，任何档位随时响应 */

  // 表 6..9 · 预算任务：本档位掩码放行且配额足够才跑。
  // GC/WL/predict 的周期评估由 timer 表调用 mig_eval；本表只负责搬移推进与预算任务。
    {    .func = bg_task_l2p_flush,  .mask = BG_TASK_L2P_FLUSH,      .units = 1U},
    {.func = bg_task_journal_flush,  .mask = BG_TASK_JRN_FLUSH,      .units = 1U},
    {          .func = bg_task_bbt,        .mask = BG_TASK_BBT,      .units = 1U},
    {   .func = bg_task_cp_trigger, .mask = BG_TASK_CP_TRIGGER,      .units = 1U},
};

// 表项数必须与 BG_TASK_COUNT 一致（capacity 是 tasks[] 维度；错位会越界或漏装载）。
_Static_assert((sizeof(s_bg_tasks) / sizeof(s_bg_tasks[0])) == BG_TASK_COUNT, "任务表项数与 BG_TASK_COUNT 不一致");

/* ── 装配与后台调度步 ── */

// bg_scheduler_init — 把共享只读任务表 s_bg_tasks 绑定到实例的调度器（不复制）。由 ftl_init 在装配末尾调用一次。
// 表长是编译期常量（上面的 _Static_assert 已证明 == BG_TASK_COUNT）⇒ 无运行期边界参数。
void bg_scheduler_init(bg_scheduler_t *sched)
{
    sched->tasks      = s_bg_tasks; /* 共享只读表：不逐实例复制 */
    sched->task_count = (BYTE)(sizeof(s_bg_tasks) / sizeof(s_bg_tasks[0]));
    sched->cfg        = NULL;
    sched->level      = BG_LEVEL_IDLE;
    sched->credits    = 0U; /* 首次 refill 后才有预算任务可跑 */
}

// bg_scheduler_level_select — 负载 → 档位（纯函数，无副作用）：
// inflight 全空 = IDLE；pending > 半池 或 有未决组 = BUSY；中间 = NORMAL。
bg_level_e bg_scheduler_level_select(const ftl_ctx_t *ftl)
{
    const WORD pending = pend_slot_get_count(&ftl->pending);
    const WORD queued  = pend_group_get_count(&ftl->pending);

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

// bg_scheduler_service — [svc] 后台调度：bg_suspend 判定 → 选档 → 取档位配置 → 引擎跑一轮。
//
// ⚠ bg_suspend 必须**最先** return（早于必跑任务）：`bbt --erase` 之后介质已空
// 而 RAM 状态陈旧，任何后台写（含 must-run 里的 journal/CP/元数据目录）都是往干净
// 芯片上倒垃圾。推进 NAND / 完成泵 / host 补发不受影响（NAND 泵与 host IO 照常）。
//
// 空闲度用 pending（host IO 未决）+ groups（未完成的 host 请求）近似：
// 全空 = 真空闲 → IDLE 全力后台；有未完成 host 请求 = host 饿着 → BUSY 最少后台
// （让路，否则抢走 drain 要用的 FC buffer）；中间 → NORMAL 按需后台。
//
// ⚠ **空闲度不计 ftl_op ctx 旁表**：后台 job（journal / RR / 搬移）的命令也登记在
// ftl_op ctx 旁表里，计进来会让「真空闲」永不成立——一个后台 job 在飞就把模式
// 从 IDLE 顶到 NORMAL，后台反而被自己的命令饿死。后台 job 的活跃度由各自的
// 各模块**内部**的单飞行门（不在这里窥探 phase/active）单独管，与 host 侧解耦。
void bg_scheduler_service(ftl_ctx_t *ftl)
{
    if (ftl->bg_suspend == true)
    {
        return;
    }

    const bg_level_e level = bg_scheduler_level_select(ftl);
    // 档位下标越界不会崩：会静默读到相邻静态数据 ⇒ 预算/掩码全错而无声（H31/H40）。
    // 档位先发布到调度器：模块入口读 sched->level，不各自重选。
    ftl->bg.level          = level;
    bg_scheduler_run(&ftl->bg, ftl, &s_bg_level_cfg[level]);
}
