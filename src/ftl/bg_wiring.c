/*
 * bg_wiring.c — FTL 后台调度接线（策略层：适配器 / 档位预算 / 任务表 / Phase 3 入口）
 *
 * 引擎在 bg_scheduler.c（通用、零 kick 知识、策略无关）；本文件只做三件事：
 *   ① 10 个 static 适配器：把异构 kick（签名/参数各异）包成 void(ftl_context_t*)；
 *   ② g_bg_level_cfg[3]：档位 → 每轮总预算 + l2p flush 条数上界 + 预算任务掩码；
 *   ③ s_bg_tasks 任务表 + ftl_bg_level_select 负载→档位（纯函数）+ ftl_bg_service Phase 3。
 *
 * 正确性关键集（must_run）无条件每轮跑、不 gate、不占预算——删了系统会挂：
 *   checkpoint_handle / rr / migrate_handle / metadata_dir / journal need_erase / 紧急 CP。
 * 全表每项的 interval / must_run / budget 依据见 bg_scheduler.h 头注与下方各适配器注释。
 *
 * ⚠ 掩码位下标 = 任务表下标（s_bg_tasks 的表序；g_bg_level_cfg 的 task_mask 按它写）。
 * ⚠ 预算时钟 = 平台 time_us（SysTick 1ms 量化）；确定性前台保护由「档位→条数/掩码」
 *   承担（见 g_bg_level_cfg 与 bg_scheduler.h「关键语义」）。
 */

#include "assertion.h"
#include "bbt.h"
#include "checkpoint.h"
#include "ftl.h"
#include "gc.h"
#include "journal.h"
#include "l2p_cache.h"
#include "log.h"
#include "metadata.h"
#include "migrate.h"
#include "pending.h"
#include "platform.h"
#include "read_recovery.h"
#include "wear_leveling.h" /* WL_EVAL_INTERVAL_MS */

/* 慢死预测巡检节拍：≥1s 才扫一轮（read disturb 是慢变量，频繁全扫无谓开销）。
 * 同时用作调度器 interval_ms 与 migrate_handle 的 predict 求值 的内部门。 */
#define RR_SCAN_INTERVAL_MS (1000U)

/* --------------------------------------------------------------------
 * ftl_now_ms
 *   功能：当前单调毫秒（经平台抽象 time_us 换算）。
 *   参数：无。
 *   返回：毫秒；平台未注册（time_us 缺失）时返回 0。
 *   实现要点：返回 0 的后果按调用者分岔——T3 定时因 elapsed = 0 - start < 阈值恒不
 *             触发；GC 求值的节拍门则因 0 作「尚未求值」哨兵而退化为「每轮求值」
 *             （GC 不能因缺时钟停摆）。
 * -------------------------------------------------------------------- */
static uint32_t ftl_now_ms(void)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();

    if ((platform == NULL) || (platform->time_us == NULL))
    {
        return 0U;
    }
    return (uint32_t)(platform->time_us() / 1000U);
}

/* --------------------------------------------------------------------
 * bg_info
 *   功能：构造 typed 模块入口的 ftl_bg_info_t（本轮动态信息）。
 *   参数：ftl —— 实例。
 *   返回：{now_ms, load_level}。load_level 取引擎本轮发布的档位（bg_scheduler_run
 *         在跑任何任务前写入），不在这里重新选档——重选是第二套读路径。
 * -------------------------------------------------------------------- */
static ftl_bg_info_t bg_info(const ftl_context_t *ftl)
{
    ftl_bg_info_t info;

    info.now_ms     = ftl_now_ms();
    info.load_level = ftl->bg.level;
    return info;
}

/* ════════════════════════════════════════════════════════════════════════
 * 档位 → 预算规模 与 任务表
 * ════════════════════════════════════════════════════════════════════════ */

/* 任务 ID（表序 = 执行序 = 优先级；掩码位用**具名 ID**，不再写裸下标）。
 * ⚠ 加任务 = 在枚举 + 表里各插一行 + 同步 g_bg_level_cfg 的掩码。 */
typedef enum
{
    BG_TASK_CP_JOIN = 0, /* 在飞 CP 跨轮推进（must-run，表首） */
    BG_TASK_RR,          /* Read Recovery 推进（must-run） */
    BG_TASK_MIGRATE,     /* 搬移引擎推进 + 自持节拍（must-run） */
    BG_TASK_METADATA,    /* 目录整理（must-run） */
    BG_TASK_JRN_ERASE,   /* Journal 换块擦除（must-run） */
    BG_TASK_CP_EMERG,    /* 紧急 Checkpoint（must-run） */
    BG_TASK_L2P_FLUSH,   /* L2P 后台写回（预算） */
    BG_TASK_JRN_FLUSH,   /* Journal 刷页（预算） */
    BG_TASK_BBT,         /* BBT 持久化（预算） */
    BG_TASK_CP_TRIGGER,  /* Checkpoint 周期触发（预算） */
    BG_TASK_NUM
} bg_task_id_t;

#define BG_TASK_BIT(id) (1U << (uint32_t)(id))

_Static_assert((uint32_t)BG_TASK_NUM <= BG_TASK_COUNT, "任务 ID 数超出调度器 tasks[] 容量");

/* 档位 → 预算规模（IDLE/NORMAL/BUSY）。锚定 onfi_1_0.c 时序（NAND_TIME_TPROG_US=700）：
 *  IDLE/NORMAL 2000µs ≈ 2~3 页写 + GC 全表求值；BUSY 700µs ≈ 恰 1 页写。
 * 掩码：IDLE/NORMAL 位 6..9（l2p / journal / bbt / cp 触发）全放行；
 *       BUSY 仅位 6（l2p flush）—— 即「仅 must-run + 1 flush」，最小预算。
 * 档位差异由总预算与各模块内**自有**的策略表承担（如 l2p 的档位→max_kick）。 */
static const bg_level_cfg_t g_bg_level_cfg[BG_LEVEL_NUM] = {
    [BG_LEVEL_IDLE] =
        {.total_budget_us = 2000U,
                         .task_mask       = BG_TASK_BIT(BG_TASK_L2P_FLUSH) | BG_TASK_BIT(BG_TASK_JRN_FLUSH) |
                         BG_TASK_BIT(BG_TASK_BBT) | BG_TASK_BIT(BG_TASK_CP_TRIGGER)           },
    [BG_LEVEL_NORMAL] =
        {.total_budget_us = 2000U,
                         .task_mask       = BG_TASK_BIT(BG_TASK_L2P_FLUSH) | BG_TASK_BIT(BG_TASK_JRN_FLUSH) |
                         BG_TASK_BIT(BG_TASK_BBT) | BG_TASK_BIT(BG_TASK_CP_TRIGGER)           },
    [BG_LEVEL_BUSY] = { .total_budget_us = 700U, .task_mask = BG_TASK_BIT(BG_TASK_L2P_FLUSH)},
};

/* ── 10 个适配器（包异构 kick → void(ftl_context_t*)）── */

/* 表首 · must_run（引擎 Pass 0 最先执行）—— 推进**已在飞**的 Checkpoint job。
 * ⚠ 必须排在最前、且不占预算：CP 可能在 BUSY 档被紧急水位起（journal≥95%），后续步骤
 *   要跨多轮跨档位走完——排在预算 pass 里会被低优先级/预算耗尽饿死。 */
static checkpoint_trigger_t bg_cp_trigger(const ftl_context_t *ftl, checkpoint_reason_t reason, bool force)
{
    checkpoint_trigger_t trigger;

    trigger.now_ms            = ftl_now_ms();
    trigger.total_writes      = ftl->total_writes;
    trigger.journal_seq       = journal_sequence(ftl->journal);
    trigger.journal_watermark = journal_watermark(ftl->journal);
    trigger.emergency         = journal_is_emergency(ftl->journal);
    trigger.force             = force;
    trigger.reason            = (uint8_t)reason;
    return trigger;
}

static void bg_a_checkpoint(ftl_context_t *ftl)
{
    const checkpoint_trigger_t trigger = bg_cp_trigger(ftl, CHECKPOINT_REASON_ADVANCE, false);
    checkpoint_status_t        status;

    checkpoint_handle(ftl->checkpoint_handle, &trigger, &status);
}

/* 表 1 · must_run —— 推进 Read Recovery job。
 * host 读在等结果（托管的子操作占着组在飞数），不推进即死等；任何档位都要跑。 */
static void bg_a_rr(ftl_context_t *ftl)
{
    const ftl_bg_info_t info = bg_info(ftl);
    rr_status_t         status;

    rr_handle(&ftl->read_recovery_handle, &info, &status);
}

/* 表 2 · must_run —— 推进搬移 job（GC/WL/预测共用，已在飞则续跑下一步）。
 * 写路径的紧急 GC 在 BUSY 档起（wbuf_handle(FLUSH) 分配不到块 ⇒ FTL_AGAIN），
 * 不推进即死锁；任何档位都要跑。 */
static void bg_a_mig_service(ftl_context_t *ftl)
{
    const ftl_bg_info_t info = bg_info(ftl);
    migrate_status_t    status;

    /* typed 入口：在飞推进 + 不在飞时按 handle 自持节拍评估 GC/WL/predict。 */
    migrate_handle(ftl->migrate_handle, &info, &status);
}

/* 表 3 · must_run —— 元数据目录整理 job。
 * compact_wanted 是 append_slot 在**前台写路径**上置的（池吃紧时换块），
 * 不推进就是写路径永久 FTL_AGAIN；任何档位都要跑。 */
static void bg_a_metadata_dir(ftl_context_t *ftl)
{
    const ftl_bg_info_t info = bg_info(ftl);
    metadata_status_t   status;

    metadata_handle(ftl->metadata_dir, &info, &status);
}

/* 表 4 · must_run —— 补「未起的换块擦除」（表 9 的子集；表 9 会被 BUSY 掩掉，本项不会）。
 * 新环形块没擦过就不能往里写 ⇒ 任何档位都要补，否则 host 写路径卡死。
 * 判据（有无待擦块 / 是否已有 job 在飞）在 journal 模块内，本文件不读其内部字段。 */
static void bg_a_journal_erase(ftl_context_t *ftl)
{
    const ftl_bg_info_t     info    = bg_info(ftl);
    const journal_trigger_t trigger = JOURNAL_TRIGGER_ERASE_ONLY;
    journal_status_t        status;

    journal_handle(ftl->journal, &trigger, &info, &status);
}

/* 表 5 · must_run —— 紧急 Checkpoint（Journal ≥ 95%）。
 * 紧急水位必须**任何档位都随时响应**（BUSY 档也一样）。cp_begin 只装配 job、
 * 单飞行（checkpoint_handle.active → FTL_AGAIN）幂等 ⇒ 每轮检查无副作用。 */
static void bg_a_emergency_cp(ftl_context_t *ftl)
{
    const checkpoint_trigger_t trigger = bg_cp_trigger(ftl, CHECKPOINT_REASON_EMERGENCY, false);
    checkpoint_status_t        status;

    checkpoint_handle(ftl->checkpoint_handle, &trigger, &status);
}

/* 表 6 · 预算 — 后台 flush 脏 L2P region，条数上界取**本轮档位配置**（引擎发布的
 * sched->cfg，见 bg_scheduler.h）。BUSY 档只放行本项 ⇒ 最小预算 = 「must-run + 1 flush」。
 * ⚠ thrashing 降级：主动 flush 大概率白写且挤前台带宽 ⇒ 停主动 flush。 */
static void bg_a_l2p_flush(ftl_context_t *ftl)
{
    const ftl_bg_info_t info = bg_info(ftl);
    l2p_status_t        status;

    /* typed 入口：档位 → max_kick 的 static const 表在 l2p 模块内；
     * thrashing 降级也在模块内判定。 */
    const uint32_t dim = ftl_topo_dim(ftl->topo);

    l2p_handle(ftl->cache, dim, &info, &status);
}

/* 表 9 · 预算 — 推进 Journal 刷页 job（job 已在飞则空操作）。
 * BUSY 档被掩掉（host 饱和档不主动起页写；页满时 journal_append 自己会起）。 */
static void bg_a_journal_flush(ftl_context_t *ftl)
{
    const ftl_bg_info_t info = bg_info(ftl);
    journal_status_t    status;

    journal_handle(ftl->journal, NULL, &info, &status);
}

/* 表 10 · 预算 — BBT 持久化（攒批写 NAND BBT；job 起步/续跑，提交即返回）。
 * typed 入口：状态只走上报结构（本适配器不读 handle 内部）。 */
static void bg_a_bbt(ftl_context_t *ftl)
{
    const ftl_bg_info_t info = bg_info(ftl);
    bbt_status_t        status;

    bbt_handle(&ftl->alloc, &info, &status);
}

/* 表 11 · 预算 — Checkpoint 触发检查（T1 Journal 水位 / T2 写入量 / T3 定时 elapsed）。
 * 定时基准**无条件**重置（触发即重置，不看成败——否则失败的 job 会每轮重触发）；
 * job 装配由 cp_begin（提交即返回），成败由 [CP] 心跳判。 */
static void bg_a_cp_trigger(ftl_context_t *ftl)
{
    const checkpoint_trigger_t trigger = bg_cp_trigger(ftl, CHECKPOINT_REASON_TIMER, false);
    checkpoint_status_t        status;

    checkpoint_handle(ftl->checkpoint_handle, &trigger, &status);
}

/* 任务表：表序 = 执行序 = 优先级（引擎线性扫 ⇒ 掩码位下标 = 表下标）。
 * 加任务 = 在表里插一行 + 同步 g_bg_level_cfg 的 task_mask。 */
static const bg_task_t s_bg_tasks[] = {
  /* 表 0..5 · must-run 正确性关键集：无条件每轮跑、不 gate、不占预算 */
    {   .func = bg_a_checkpoint,   .must_run = true}, /* 在飞 CP 跨轮推进，必须最先 */
    {           .func = bg_a_rr,   .must_run = true}, /* host 读在等结果，不推进即死等 */
    {  .func = bg_a_mig_service,   .must_run = true}, /* 写路径紧急 GC 在 BUSY 起，不推进即死锁 */
    { .func = bg_a_metadata_dir,   .must_run = true}, /* 前台写路径置的压缩请求，不推进即永久 FTL_AGAIN */
    {.func = bg_a_journal_erase,   .must_run = true}, /* 新环形块没擦不能写，BUSY 也要补 */
    { .func = bg_a_emergency_cp,   .must_run = true}, /* Journal ≥95%，任何档位随时响应 */

  /* 表 6..9 · 预算任务：interval 到点 + 本档位掩码放行才跑，占预算。
  * GC/WL/predict 已收进 migrate handle 自持节拍（docs §4.4），不再单列任务。 */
    {    .func = bg_a_l2p_flush, .budget_us = 1500U},
    {.func = bg_a_journal_flush,  .budget_us = 700U},
    {          .func = bg_a_bbt,  .budget_us = 700U},
    {   .func = bg_a_cp_trigger,  .budget_us = 100U},
};

/* 表项数必须与任务 ID 数一致（掩码按 ID 写；错位会静默跑错任务）。 */
_Static_assert((sizeof(s_bg_tasks) / sizeof(s_bg_tasks[0])) == BG_TASK_NUM,
               "任务表项数与 bg_task_id_t 不一致");

/* ── 装配与 Phase 3 ── */

/* ftl_bg_init —— 把 s_bg_tasks 装载进本实例的调度器。由 ftl_init 在装配末尾调用一次。 */
void ftl_bg_init(ftl_context_t *ftl)
{
    bg_scheduler_init(&ftl->bg, s_bg_tasks, (uint8_t)(sizeof(s_bg_tasks) / sizeof(s_bg_tasks[0])));
}

/* ftl_bg_level_select — 负载 → 档位（纯函数，无副作用）：
 * inflight 全空 = IDLE；pending > 半池 或 有未决组 = BUSY；中间 = NORMAL。 */
uint8_t ftl_bg_level_select(const ftl_context_t *ftl)
{
    const uint16_t pending = pending_count(ftl);
    const uint16_t queued  = pending_group_count(ftl);

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

/* ftl_bg_service — Phase 3：bg_suspend 判定 → 选档 → 取档位配置 → 引擎跑一轮。
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
void ftl_bg_service(ftl_context_t *ftl)
{
    if (ftl->bg_suspend != 0U)
    {
        return;
    }

    const uint8_t level = ftl_bg_level_select(ftl);
    /* 档位下标越界不会崩：会静默读到相邻静态数据 ⇒ 预算/掩码全错而无声（H31/H40）。 */
    /* 档位先发布到调度器：typed 模块入口的 ftl_bg_info_t.load_level 取这里。 */
    ftl->bg.level       = level;
    bg_scheduler_run(&ftl->bg, ftl, &g_bg_level_cfg[level]);
}
