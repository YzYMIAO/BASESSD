/*
 * bg_wiring.c — FTL 后台调度接线（策略层：适配器 / 档位预算 / 任务表 / Phase 3 入口）
 *
 * 引擎在 bg_scheduler.c（通用、零 kick 知识、策略无关）；本文件只做三件事：
 *   ① 13 个 static 适配器：把异构 kick（签名/参数各异）包成 void(ftl_context_t*)；
 *   ② g_bg_level_cfg[3]：档位 → 每轮总预算 + l2p flush 条数上界 + 预算任务掩码；
 *   ③ s_bg_tasks 任务表 + ftl_bg_level_select 负载→档位（纯函数）+ ftl_bg_service Phase 3。
 *
 * 正确性关键集（must_run）无条件每轮跑、不 gate、不占预算——删了系统会挂：
 *   checkpoint_kick / rr / mig_service / metadata_dir / journal need_erase / 紧急 CP。
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
 * 同时用作调度器 interval_ms 与 bg_a_rr_predict 的内部门。 */
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
 * ftl_gc_evaluate
 *   功能：GC 的**统一触发求值点**——IDLE 与 NORMAL 两条后台路径都从这里过。
 *   参数：ftl —— FTL 实例。
 *   返回：GC_TRIGGER_* 位掩码；0 = 不触发（含节拍未到，此时不打印）。
 *   实现要点：触发只看**空间压力与磨损**（gc_check_triggers 的 T1/T4/T5，见 gc.h）。
 *             命中即打一行观测日志（victim 行由 gc_select_victim 自己打）。
 *   ⚠ **BUSY 不调本函数**（host 饿着，后台让路）；"无块可写"由 ftl_issue_subop 的
 *     紧急 GC 兜底（⚠ 本仓自己的**写路径按需 GC**；SPDK 无此路径——它的 GC 全在
 *     core poller 里、判据是自由 band 水位，写路径取不到块就返 NULL 不就地回收）。
 *   ⚠ **节拍门（GC_EVAL_INTERVAL_MS）**：本函数含 T5 的**全表扫描**
 *     （gc_table_extremes 遍历 FTL_BLOCK_TOTAL 个块），无门则是**每轮扫一遍全表**。
 *     触发判据本身是事件（空间压力/磨损），但**成本**远高于主流：SPDK FTL 的判据是
 *     O(1)（`ftl_needs_reloc` 只读事件驱动的 `dev->num_free` 计数，其 core poller 周期
 *     0 = 每轮求值也无所谓）；本仓 T5 要全表扫描 ⇒ 门控是**成本补偿**。
 *     与 rr_last_scan_ms / cp_timer_start 的节拍门同族。
 *     代价（如实登记）：T1/T4 的空间压力响应也被门到同一周期——IDLE 补池突发与
 *     NORMAL 的「一轮回收一个 SB」最坏延迟 GC_EVAL_INTERVAL_MS。真空闲下按需兜底
 *     仍在写路径（ftl_issue_subop），且 10ms 远小于搬一个 SB 的 NAND 时间。
 *   ⚠ 本门与调度器的 `interval_ms = GC_EVAL_INTERVAL_MS` 同源同值。保留双门是因为
 *     本函数的成本是 O(全表)：调度器 interval 一旦被误置 0，双门仍挡住逐轮全表扫描。
 * -------------------------------------------------------------------- */
static uint32_t ftl_gc_evaluate(ftl_context_t *ftl)
{
    uint32_t now_ms = ftl_now_ms();

    /* 节拍门。⚠ gc_eval_last_ms == 0 是「尚未求值」哨兵：平台时钟缺失时
     * ftl_now_ms() 恒 0，此时**每轮都求值**——GC 不能因缺时钟而停摆。 */
    if ((ftl->gc_eval_last_ms != 0U) && ((now_ms - ftl->gc_eval_last_ms) < GC_EVAL_INTERVAL_MS))
    {
        return 0U;
    }
    ftl->gc_eval_last_ms = now_ms;

    uint32_t triggers = gc_check_triggers(&ftl->alloc);

    /* 观测口：位图见 gc.h GC_TRIGGER_*（多位可同时命中）。日志走**自己的节拍**
     * （GC_LOG_INTERVAL_MS，与求值同一套时基），不是逐次求值都打。
     * ⚠ 求值节拍不能兼作日志节拍：前者管「多久算一次」，10ms 拍仍是 100 行/秒。
     * 语义 = 进入触发态立即一行 + 触发态内每 GC_LOG_INTERVAL_MS 一行心跳；
     * 退出触发态即归零 ⇒ 下次进入必然立即打（故无需另存触发掩码）。
     * ⚠ 行内带 `reclaimed` / `novictim` 两个**进展计数**（mig_finish / mig_begin_gc 自增）：
     * 只有它们能区分「触发但在推进」与「触发但卡死」——`trig` 常亮对两者一视同仁，
     * 而 `[GC] victim` 行（gc.c）在卡死时**根本不出现**，其"消失"也与"已收敛"同形。 */
    if (triggers != 0U)
    {
        if ((ftl->gc_log_last_ms == 0U) || ((now_ms - ftl->gc_log_last_ms) >= GC_LOG_INTERVAL_MS))
        {
            ftl->gc_log_last_ms = now_ms;
            LOGI("[GC] trig=0x%02x free=%u op=%u/%u writes=%lu reclaimed=%lu novictim=%lu\r\n",
                 (unsigned)triggers,
                 (unsigned)ftl->alloc.free_count,
                 (unsigned)allocator_op_count(&ftl->alloc, 0U),
                 (unsigned)allocator_op_count(&ftl->alloc, 1U),
                 (unsigned long)ftl->total_writes,
                 (unsigned long)ftl->gc_reclaimed_count,
                 (unsigned long)ftl->gc_novictim_count);
        }
    }
    else
    {
        /* 收敛：归零哨兵，使下一次进入触发态的第一行立即出现 */
        ftl->gc_log_last_ms = 0U;
    }

    return triggers;
}

/* ════════════════════════════════════════════════════════════════════════
 * 档位 → 预算规模 与 任务表
 * ════════════════════════════════════════════════════════════════════════ */

/* l2p_flush_max 是 uint8_t；容量宏超 255 即静默截断（编译期证不了位域/窄化，故显式断言）。 */
_Static_assert(L2P_CACHE_SIZE <= UINT8_MAX, "L2P_CACHE_SIZE 超出 l2p_flush_max 的 uint8_t 值域");

/* 档位 → 预算规模（IDLE/NORMAL/BUSY）。锚定 onfi_1_0.c 时序（NAND_TIME_TPROG_US=700）：
 *  IDLE/NORMAL 2000µs ≈ 2~3 页写 + GC 全表求值；BUSY 700µs ≈ 恰 1 页写。
 * 掩码：IDLE 位 6..12 全 budgeted（含 RR 巡检）；NORMAL 位 6..11（无 RR 巡检）；
 *       BUSY 仅位 8（l2p flush）—— 即「仅 must-run + 1 flush」，最小预算。 */
static const bg_level_cfg_t g_bg_level_cfg[BG_LEVEL_NUM] = {
    [BG_LEVEL_IDLE]   = {.total_budget_us = 2000U,        .l2p_flush_max = L2P_CACHE_SIZE, .task_mask = (0x7FU << 6U)},
    [BG_LEVEL_NORMAL] = {.total_budget_us = 2000U, .l2p_flush_max = BG_NORMAL_FLUSH_COUNT, .task_mask = (0x3FU << 6U)},
    [BG_LEVEL_BUSY]   = { .total_budget_us = 700U,   .l2p_flush_max = BG_BUSY_FLUSH_COUNT,    .task_mask = (1U << 8U)},
};

/* ── 13 个适配器（包异构 kick → void(ftl_context_t*)）── */

/* 表首 · must_run（引擎 Pass 0 最先执行）—— 推进**已在飞**的 Checkpoint job。
 * ⚠ 必须排在最前、且不占预算：CP 可能在 BUSY 档被紧急水位起（journal≥95%），后续步骤
 *   要跨多轮跨档位走完——排在预算 pass 里会被低优先级/预算耗尽饿死。 */
static void bg_a_checkpoint(ftl_context_t *ftl)
{
    checkpoint_kick(ftl);
}

/* 表 1 · must_run —— 推进 Read Recovery job。
 * host 读在等结果（托管的子操作占着组在飞数），不推进即死等；任何档位都要跑。 */
static void bg_a_rr(ftl_context_t *ftl)
{
    rr_kick(ftl);
}

/* 表 2 · must_run —— 推进搬移 job（GC/WL/预测共用，已在飞则续跑下一步）。
 * 写路径的紧急 GC 在 BUSY 档起（ftl_wbuf_flush 分配不到块 ⇒ FTL_AGAIN），
 * 不推进即死锁；任何档位都要跑。 */
static void bg_a_mig_service(ftl_context_t *ftl)
{
    mig_service(ftl);
}

/* 表 3 · must_run —— 元数据目录整理 job。
 * compact_wanted 是 append_slot 在**前台写路径**上置的（池吃紧时换块），
 * 不推进就是写路径永久 FTL_AGAIN；任何档位都要跑。 */
static void bg_a_metadata_dir(ftl_context_t *ftl)
{
    metadata_dir_kick(ftl->metadata_dir, &ftl->alloc);
}

/* 表 4 · must_run —— 补「未起的换块擦除」（表 9 的子集；表 9 会被 BUSY 掩掉，本项不会）。
 * 新环形块没擦过就不能往里写 ⇒ 任何档位都要补，否则 host 写路径卡死。
 * 判据（有无待擦块 / 是否已有 job 在飞）在 journal 模块内，本文件不读其内部字段。 */
static void bg_a_journal_erase(ftl_context_t *ftl)
{
    journal_kick_erase_only(ftl->journal);
}

/* 表 5 · must_run —— 紧急 Checkpoint（Journal ≥ 95%）。
 * 紧急水位必须**任何档位都随时响应**（BUSY 档也一样）。checkpoint_begin 只装配 job、
 * 单飞行（cp_job.active → FTL_AGAIN）幂等 ⇒ 每轮检查无副作用。 */
static void bg_a_emergency_cp(ftl_context_t *ftl)
{
    if (checkpoint_is_emergency(ftl))
    {
        (void)checkpoint_begin(ftl);
    }
}

/* 表 6 · 预算 — GC 统一触发判定（内部 GC_EVAL_INTERVAL_MS 门与调度器 interval 双门，
 * 见 ftl_gc_evaluate 头注）。命中即**起一个** GC 搬移 job（单飞行）。 */
static void bg_a_gc_eval(ftl_context_t *ftl)
{
    if (ftl_gc_evaluate(ftl) != 0U)
    {
        (void)mig_begin_gc(ftl);
    }
}

/* 表 7 · 预算 — 磨损均衡（触发判定在 mig_begin_wl 内部；interval=0 交内部门） */
static void bg_a_wl(ftl_context_t *ftl)
{
    (void)mig_begin_wl(ftl);
}

/* 表 8 · 预算 — 后台 flush 脏 L2P region，条数上界取**本轮档位配置**（引擎发布的
 * sched->cfg，见 bg_scheduler.h）。BUSY 档只放行本项 ⇒ 最小预算 = 「must-run + 1 flush」。
 * ⚠ thrashing 降级：主动 flush 大概率白写且挤前台带宽 ⇒ 停主动 flush。 */
static void bg_a_l2p_flush(ftl_context_t *ftl)
{
    if (ftl->cache->thrashing != 0U)
    {
        return;
    }
    l2p_flush_kick_owner(ftl->cache, ftl_topo_dim(ftl->topo), ftl->bg.cfg->l2p_flush_max);
}

/* 表 9 · 预算 — 推进 Journal 刷页 job（job 已在飞则空操作）。
 * BUSY 档被掩掉（host 饱和档不主动起页写；页满时 journal_append 自己会起）。 */
static void bg_a_journal_flush(ftl_context_t *ftl)
{
    journal_kick(ftl->journal);
}

/* 表 10 · 预算 — BBT 持久化（攒批写 NAND BBT；job 起步/续跑，提交即返回） */
static void bg_a_bbt(ftl_context_t *ftl)
{
    bbt_kick(&ftl->alloc);
}

/* 表 11 · 预算 — Checkpoint 触发检查（T1 Journal 水位 / T2 写入量 / T3 定时 elapsed）。
 * 定时基准**无条件**重置（触发即重置，不看成败——否则失败的 job 会每轮重触发）；
 * job 装配由 checkpoint_begin（提交即返回），成败由 [CP] 心跳判。 */
static void bg_a_cp_trigger(ftl_context_t *ftl)
{
    uint32_t now_ms  = ftl_now_ms();
    uint32_t elapsed = now_ms - ftl->cp_timer_start;

    if (checkpoint_check_trigger(ftl, elapsed))
    {
        (void)checkpoint_begin(ftl);
        ftl->cp_timer_start = now_ms;
    }
}

/* 表 12 · 预算 — 慢死预测巡检（一次只搬一个 SB，防长时间阻塞）。仅 IDLE 档。
 * 内部门与调度器 interval 同值：时钟缺失降级下调度器 interval 门失效，内部门兜底。 */
static void bg_a_rr_predict(ftl_context_t *ftl)
{
    uint32_t now_ms = ftl_now_ms();

    /* ⚠ 哨兵 rr_last_scan_ms == 0 = 尚未巡检 ⇒ 立即跑。缺哨兵项时，时钟缺失
     * （now_ms 恒 0）下 0-0 >= 1000 恒假 ⇒ 巡检永久停摆，与「内部门兜底」相反。
     * ftl_gc_evaluate 的同类门用同一哨兵语义。 */
    if ((ftl->rr_last_scan_ms == 0U) || ((now_ms - ftl->rr_last_scan_ms) >= RR_SCAN_INTERVAL_MS))
    {
        ftl->rr_last_scan_ms = now_ms;
        (void)mig_begin_predict(ftl);
    }
}

/* 任务表：表序 = 执行序 = 优先级（引擎线性扫 ⇒ 掩码位下标 = 表下标）。
 * 加任务 = 在表里插一行 + 同步 g_bg_level_cfg 的 task_mask。 */
static const bg_task_t s_bg_tasks[] = {
  /* 表 0..5 · must-run 正确性关键集：无条件每轮跑、不 gate、不占预算 */
    {.func = bg_a_checkpoint, .must_run = true}, /* 在飞 CP 跨轮推进，必须最先 */
    {.func = bg_a_rr, .must_run = true}, /* host 读在等结果，不推进即死等 */
    {.func = bg_a_mig_service, .must_run = true}, /* 写路径紧急 GC 在 BUSY 起，不推进即死锁 */
    {.func = bg_a_metadata_dir, .must_run = true}, /* 前台写路径置的压缩请求，不推进即永久 FTL_AGAIN */
    {.func = bg_a_journal_erase, .must_run = true}, /* 新环形块没擦不能写，BUSY 也要补 */
    {.func = bg_a_emergency_cp, .must_run = true}, /* Journal ≥95%，任何档位随时响应 */

  /* 表 6..12 · 预算任务：interval 到点 + 本档位掩码放行才跑，占预算 */
    {.func = bg_a_gc_eval, .interval_ms = GC_EVAL_INTERVAL_MS, .budget_us = 400U},
    {.func = bg_a_wl, .interval_ms = WL_EVAL_INTERVAL_MS, .budget_us = 300U},
    {.func = bg_a_l2p_flush, .budget_us = 1500U},
    {.func = bg_a_journal_flush, .budget_us = 700U},
    {.func = bg_a_bbt, .budget_us = 700U},
    {.func = bg_a_cp_trigger, .budget_us = 100U},
    {.func = bg_a_rr_predict, .interval_ms = RR_SCAN_INTERVAL_MS, .budget_us = 300U},
};

/* 表项数超出 tasks[] 容量 ⇒ 装载时越界写并踩掉 task_count，且编译期无声 ⇒ 此处挡住。 */
_Static_assert((sizeof(s_bg_tasks) / sizeof(s_bg_tasks[0])) <= BG_TASK_COUNT,
               "任务表项数超出 bg_scheduler_t.tasks 容量");

/* ── 装配与 Phase 3 ── */

/* ftl_bg_init —— 把 s_bg_tasks 装载进本实例的调度器。由 ftl_init 在装配末尾调用一次。 */
void ftl_bg_init(ftl_context_t *ftl)
{
    ASSERT(ftl != NULL);
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
 *   「单飞行门」（journal.job.phase / rr.stage / mig.active）单独管，与 host 侧解耦。 */
void ftl_bg_service(ftl_context_t *ftl)
{
    ASSERT(ftl != NULL);

    if (ftl->bg_suspend != 0U)
    {
        return;
    }

    const uint8_t level = ftl_bg_level_select(ftl);
    /* 档位下标越界不会崩：会静默读到相邻静态数据 ⇒ 预算/掩码全错而无声（H31/H40）。 */
    ASSERT(level < BG_LEVEL_NUM);
    bg_scheduler_run(&ftl->bg, ftl, &g_bg_level_cfg[level]);
}
