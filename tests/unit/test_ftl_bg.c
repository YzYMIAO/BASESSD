/*
 * test_ftl_bg.c — 时间预算后台调度器（v0.76）L0 纯 RAM 测试（/ftl/bg/ 套件，6 用例）
 *
 * 实现：bg_scheduler.h/c（通用引擎）+ ftl.c「后台调度器接线」。设计见
 * docs/design/ftl_optimization_plan.md §九 波 4.5 与 bg_scheduler.h 头注。覆盖：
 *   · table_wiring       —— ftl_bg_init 装配的任务表契约（14 项 / must_run / 优先级 / interval / budget）
 *   · interval_gate      —— 引擎 interval 门（last_run_ms==0 哨兵首次立即跑；未到跳过；到点再跑）
 *   · budget_truncation  —— 引擎每轮总预算耗尽即截断（charge = MIN(elapsed, budget_us)）
 *   · clock_missing      —— 平台时钟恒 0 的降级（interval 门失效 ⇒ 每轮跑；elapsed=0 ⇒ 不截断）
 *   · busy_must_run      —— 档位掩码语义：Pass 0 must-run 无视掩码/预算，Pass 1 按掩码截断
 *   · level_select       —— ftl_bg_level_select 纯函数（IDLE / NORMAL / BUSY 分档）
 *
 * ⚠ 范围约定：本套件**只测引擎 + 生产任务表 + 档位纯函数**，不驱动生产适配器（bg_a_*）。
 *   生产适配器会起 journal 擦 / mig 读 / checkpoint cp_step —— 都要真 NAND 完成链才能
 *   收尾，属异步套件（test_ftl_async 计划，见 ftl_ut_munit.md §4.9）职责；本套件用
 *   **全 14 项计数桩**（零副作用、零命令泄漏）把引擎语义钉死，生产表把 must_run /
 *   interval / 优先级钉在正确槽位由 table_wiring 断言。
 *
 * 时间注入 seam（§5.5 契约）：克隆 ssd_platform_ops_t 换 fake time_us 再
 * ssd_platform_register，用例后还原。fake 三型：静态（interval 门）、步进 +300µs（预算）、
 * 恒 0（时钟缺失）。
 */

#include "bg_scheduler.h"
#include "ftl.h"             /* ftl_context_t / ftl_bg_init / ftl_bg_level_select */
#include "ftl_test_common.h" /* 共享活实例夹具（ftl_fixture / ftl_async_fixture_reset） */
#include "munit.h"
#include "pending.h"  /* pending_add / pending_group_alloc（level_select 造负载） */
#include "platform.h" /* ssd_platform_ops_t / ssd_platform_register / _get */

/* ══════════════════════════════════════════════════════════════════════
 * §1  夹具 + 时钟 fake
 * ══════════════════════════════════════════════════════════════════════ */

/* setup 一律返回 NULL：夹具是**活实例**（见 ftl_test_common.h §1）。
 * 异步复位清 mig / cp_job / bbt / wear；再重填生产任务表（table_wiring 断言用）。 */
static void *bg_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    ftl_async_fixture_reset();
    ftl_bg_init(ftl_fixture());
    return NULL;
}

/* 时钟 fake 三型 + 平台换装（克隆现有 ops 换 time_us；首解析后缓存真 ops 跨用例自愈）。 */
static uint64_t g_fake_us;
static uint64_t bg_clock_static(void)
{
    return g_fake_us;
}
static uint64_t bg_clock_step300(void)
{
    g_fake_us += 300U;
    return g_fake_us;
}
static uint64_t bg_clock_zero(void)
{
    return 0U;
}

static ssd_platform_ops_t        g_bg_fake_ops;
static const ssd_platform_ops_t *g_bg_real_ops; /* 首次解析后不再动 */

static void bg_install_clock(uint64_t (*fn)(void))
{
    if (g_bg_real_ops == NULL)
    {
        g_bg_real_ops = ssd_platform_get();
        munit_assert_not_null(g_bg_real_ops);
    }
    g_bg_fake_ops         = *g_bg_real_ops; /* 克隆其余字段（log_write 等） */
    g_bg_fake_ops.time_us = fn;
    munit_assert_int(ssd_platform_register(&g_bg_fake_ops), ==, 0);
}

static void bg_restore_clock(void)
{
    (void)ssd_platform_register(g_bg_real_ops);
}

/* ══════════════════════════════════════════════════════════════════════
 * §2  全 14 项计数桩 + 本地桩任务表（引擎测试专用，零生产副作用）
 * ══════════════════════════════════════════════════════════════════════ */

static uint32_t g_bg_count[BG_TASK_COUNT];

#define BG_STUB_DEF(n)                          \
    static void bg_stub_##n(ftl_context_t *ftl) \
    {                                           \
        (void)ftl;                              \
        g_bg_count[n]++;                        \
    }

BG_STUB_DEF(0)
BG_STUB_DEF(1)
BG_STUB_DEF(2)
BG_STUB_DEF(3)
BG_STUB_DEF(4)
BG_STUB_DEF(5)
BG_STUB_DEF(6)
BG_STUB_DEF(7)
BG_STUB_DEF(8)
BG_STUB_DEF(9)
BG_STUB_DEF(10)
BG_STUB_DEF(11)
BG_STUB_DEF(12)
BG_STUB_DEF(13)

static const bg_task_func_t g_bg_stub_fn[BG_TASK_COUNT] = {
    bg_stub_0,
    bg_stub_1,
    bg_stub_2,
    bg_stub_3,
    bg_stub_4,
    bg_stub_5,
    bg_stub_6,
    bg_stub_7,
    bg_stub_8,
    bg_stub_9,
    bg_stub_10,
    bg_stub_11,
    bg_stub_12,
    bg_stub_13,
};

/* 本地 14 项桩表：must_run = 位 0..5（对齐生产契约）、interval = 仅位 6（10ms）/位 13
 * （1s，对齐生产表）、priority = 下标、budget 随意。 */
static void bg_fill_stub_table(bg_scheduler_t *s)
{
    uint8_t i;

    bg_scheduler_init(s);
    for (i = 0U; i < BG_TASK_COUNT; i++)
    {
        s->tasks[i].func        = g_bg_stub_fn[i];
        s->tasks[i].must_run    = (i < 6U) ? 1U : 0U;
        s->tasks[i].priority    = i;
        s->tasks[i].interval_ms = (i == 6U) ? GC_EVAL_INTERVAL_MS : (i == 13U) ? 1000U
                                                                               : 0U;
        s->tasks[i].budget_us   = 700U;
    }
    s->task_count = BG_TASK_COUNT;
}

static void bg_reset_counts(void)
{
    uint8_t i;

    for (i = 0U; i < BG_TASK_COUNT; i++)
    {
        g_bg_count[i] = 0U;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * §3  用例
 * ══════════════════════════════════════════════════════════════════════ */

/* table_wiring — 生产任务表契约：14 项；位 0..5 = must_run 正确性关键集、位 6..13 =
 * 预算任务按优先级升序；GC 求值 interval = GC_EVAL_INTERVAL_MS、RR 巡检 = 1s。 */
static MunitResult test_bg_table_wiring(const MunitParameter params[], void *fixture)
{
    ftl_context_t        *F                            = ftl_fixture();
    bg_scheduler_t       *bg                           = &F->bg;
    static const uint16_t expect_budget[BG_TASK_COUNT] = {
        0U,
        0U,
        0U,
        0U,
        0U,
        0U, /* must-run 不占预算 */
        400U,
        300U,
        1500U,
        700U,
        700U,
        700U,
        100U,
        300U,
    };
    uint8_t i;

    (void)params;
    (void)fixture;

    munit_assert_int(bg->task_count, ==, BG_TASK_COUNT);
    munit_assert_int(bg->task_count, ==, 14U);

    for (i = 0U; i < 6U; i++)
    {
        munit_assert_not_null(bg->tasks[i].func);
        munit_assert_int(bg->tasks[i].must_run, ==, 1);
        munit_assert_int(bg->tasks[i].interval_ms, ==, 0U); /* must-run 不受 interval 门控 */
    }
    for (i = 6U; i < BG_TASK_COUNT; i++)
    {
        munit_assert_not_null(bg->tasks[i].func);
        munit_assert_int(bg->tasks[i].must_run, ==, 0);
        munit_assert_int(bg->tasks[i].priority, ==, (uint8_t)(i - 6U)); /* 优先级升序 = 表序 */
        munit_assert_int(bg->tasks[i].budget_us, ==, expect_budget[i]);
    }

    munit_assert_int(bg->tasks[6].interval_ms, ==, GC_EVAL_INTERVAL_MS); /* 10ms */
    munit_assert_int(bg->tasks[13].interval_ms, ==, 1000U);              /* RR 巡检 1s */

    return MUNIT_OK;
}

/* interval_gate — 引擎 interval 门：last_run_ms==0 哨兵首次立即跑；未到跳过；到点再跑。
 * 用桩表 + 静态时钟（elapsed=0 ⇒ 预算不截断，interval 是唯一门）。 */
static MunitResult test_bg_interval_gate(const MunitParameter params[], void *fixture)
{
    bg_scheduler_t s;
    bg_level_cfg_t cfg = {2000U, 64U, 0x3FC0U}; /* 掩码放行位 6..13 全部预算任务 */
    uint8_t        i;

    (void)params;
    (void)fixture;

    bg_fill_stub_table(&s);
    bg_install_clock(bg_clock_static);

    /* 首次（last_run_ms==0 哨兵）：位 0..5 must-run + 位 6..13 预算全各跑一次 */
    g_fake_us = 100000U; /* 100 ms */
    bg_reset_counts();
    bg_scheduler_run(&s, ftl_fixture(), &cfg);
    for (i = 0U; i < BG_TASK_COUNT; i++)
    {
        munit_assert_int(g_bg_count[i], ==, 1);
    }

    /* +5ms：位 6（10ms 门）与位 13（1s 门）被拦；位 7..12（interval=0）照跑 */
    g_fake_us = 105000U;
    bg_reset_counts();
    bg_scheduler_run(&s, ftl_fixture(), &cfg);
    munit_assert_int(g_bg_count[6], ==, 0);
    munit_assert_int(g_bg_count[13], ==, 0);
    munit_assert_int(g_bg_count[7], ==, 1);
    munit_assert_int(g_bg_count[8], ==, 1);
    munit_assert_int(g_bg_count[12], ==, 1);

    /* 自 100ms 起 +11ms：位 6 放行（≥10ms）；位 13 仍被 1s 门拦 */
    g_fake_us = 111000U;
    bg_reset_counts();
    bg_scheduler_run(&s, ftl_fixture(), &cfg);
    munit_assert_int(g_bg_count[6], ==, 1);
    munit_assert_int(g_bg_count[13], ==, 0);

    bg_restore_clock();
    return MUNIT_OK;
}

/* budget_truncation — 引擎预算：charge = MIN(elapsed, budget_us)，每轮总预算耗尽即截断。
 * 步进时钟每读 +300µs ⇒ elapsed=300（位 12 单次钳 100）；预算不可测时永不截断。 */
static MunitResult test_bg_budget_truncation(const MunitParameter params[], void *fixture)
{
    bg_scheduler_t s;
    bg_level_cfg_t tiny = {400U, 64U, 0x3FC0U};
    bg_level_cfg_t full = {2000U, 64U, 0x3FC0U};

    (void)params;
    (void)fixture;

    bg_fill_stub_table(&s);
    bg_install_clock(bg_clock_step300);

    /* 预算 400µs：位 6 单次钳制 300 ⇒ 剩 100 < MIN_TASK_BUDGET_US ⇒ 截断，位 7..13 未跑 */
    g_fake_us = 0U;
    bg_reset_counts();
    bg_scheduler_run(&s, ftl_fixture(), &tiny);
    munit_assert_int(g_bg_count[6], ==, 1);
    munit_assert_int(g_bg_count[7], ==, 0);
    munit_assert_int(g_bg_count[13], ==, 0);
    munit_assert_int(s.round_total_us, ==, 400U);
    munit_assert_int(s.round_used_us, ==, 300U);

    /* 预算 2000µs：位 6..12 逐步执行（6×300 + 100），位 13 前耗尽（剩 100 < 250）截断 */
    g_fake_us = 0U;
    bg_reset_counts();
    bg_scheduler_run(&s, ftl_fixture(), &full);
    munit_assert_int(g_bg_count[6], ==, 1);
    munit_assert_int(g_bg_count[7], ==, 1);
    munit_assert_int(g_bg_count[12], ==, 1);
    munit_assert_int(g_bg_count[13], ==, 0);
    munit_assert_int(s.round_total_us, ==, 2000U);
    munit_assert_int(s.round_used_us, ==, 1900U);

    bg_restore_clock();
    return MUNIT_OK;
}

/* clock_missing — 平台时钟恒 0 降级：interval 门失效（last_run_ms 恒 0 ⇒ 哨兵每轮跑）、
 * elapsed=0 永不截断（退化为「无门控」，防刷屏靠各任务内部事件门——本套件用桩不涉）。 */
static MunitResult test_bg_clock_missing(const MunitParameter params[], void *fixture)
{
    bg_scheduler_t s;
    bg_level_cfg_t cfg = {2000U, 64U, 0x3FC0U};
    uint8_t        i;

    (void)params;
    (void)fixture;

    bg_fill_stub_table(&s);
    bg_install_clock(bg_clock_zero);

    g_fake_us = 0U;
    bg_reset_counts();
    bg_scheduler_run(&s, ftl_fixture(), &cfg);
    munit_assert_int(s.round_seq, ==, 1U);
    munit_assert_int(s.round_used_us, ==, 0U); /* 预算不可测 ⇒ 不截断 */
    for (i = 0U; i < BG_TASK_COUNT; i++)
    {
        munit_assert_int(g_bg_count[i], ==, 1); /* 每轮全跑（含位 6/13——门失效） */
    }

    bg_reset_counts();
    bg_scheduler_run(&s, ftl_fixture(), &cfg);
    munit_assert_int(s.round_seq, ==, 2U);
    munit_assert_int(s.round_used_us, ==, 0U);
    for (i = 0U; i < BG_TASK_COUNT; i++)
    {
        munit_assert_int(g_bg_count[i], ==, 1);
    }

    bg_restore_clock();
    return MUNIT_OK;
}

/* busy_must_run — 档位掩码语义（生产 BUSY 掩码 = 仅位 8 l2p flush）：
 * Pass 0 must-run（位 0..5）无视掩码/预算全跑；Pass 1 只放位 8，其余预算任务全掩。
 * 桩表 must_run/interval 对齐生产契约（见 bg_fill_stub_table）。 */
static MunitResult test_bg_busy_must_run(const MunitParameter params[], void *fixture)
{
    bg_scheduler_t s;
    bg_level_cfg_t busy = {700U, 1U, (1U << 8U)};
    uint8_t        i;

    (void)params;
    (void)fixture;

    bg_fill_stub_table(&s);
    bg_install_clock(bg_clock_static);
    g_fake_us = 100000U;

    bg_reset_counts();
    bg_scheduler_run(&s, ftl_fixture(), &busy);

    for (i = 0U; i < 6U; i++)
    {
        munit_assert_int(g_bg_count[i], ==, 1); /* must-run 无条件每轮跑 */
    }
    munit_assert_int(g_bg_count[8], ==, 1); /* 掩码放行的唯一预算任务（l2p flush） */
    munit_assert_int(g_bg_count[6], ==, 0); /* GC 求值被 BUSY 掩掉 */
    munit_assert_int(g_bg_count[7], ==, 0);
    munit_assert_int(g_bg_count[13], ==, 0);

    bg_restore_clock();
    return MUNIT_OK;
}

/* level_select — ftl_bg_level_select 纯函数分档：inflight 全空=IDLE；pending>MAX/2 或
 * 有未决组=BUSY；中间=NORMAL（与 v0.75 三级分档逐字等价）。 */
static MunitResult test_bg_level_select(const MunitParameter params[], void *fixture)
{
    ftl_context_t *F = ftl_fixture();
    uint8_t        i;

    (void)params;
    (void)fixture;

    /* 真空闲 → IDLE */
    munit_assert_int(ftl_bg_level_select(F), ==, BG_LEVEL_IDLE);

    /* pending=20 > MAX/2(=16) → BUSY */
    for (i = 0U; i < 20U; i++)
    {
        munit_assert_int(pending_add(F, (uint16_t)i, 1U, 0, NULL, 1U, (nand_pba_t){0U}, (nand_pba_t){0U}, 0U, PENDING_OP_WRITE, 0xFFU),
                         ==,
                         0);
    }
    munit_assert_int(ftl_bg_level_select(F), ==, BG_LEVEL_BUSY);

    /* 释放到 8 → NORMAL */
    for (i = 0U; i < 20U; i++)
    {
        pending_release_index(F, i);
    }
    for (i = 0U; i < 8U; i++)
    {
        munit_assert_int(pending_add(F, (uint16_t)i, 1U, 0, NULL, 1U, (nand_pba_t){0U}, (nand_pba_t){0U}, 0U, PENDING_OP_WRITE, 0xFFU),
                         ==,
                         0);
    }
    munit_assert_int(ftl_bg_level_select(F), ==, BG_LEVEL_NORMAL);

    /* pending=5 + 有未决组（queued != 0）→ BUSY */
    for (i = 0U; i < 8U; i++)
    {
        pending_release_index(F, i);
    }
    for (i = 0U; i < 5U; i++)
    {
        munit_assert_int(pending_add(F, (uint16_t)i, 1U, 0, NULL, 1U, (nand_pba_t){0U}, (nand_pba_t){0U}, 0U, PENDING_OP_WRITE, 0xFFU),
                         ==,
                         0);
    }
    munit_assert_int(pending_group_alloc(F), >=, 0);
    munit_assert_int(ftl_bg_level_select(F), ==, BG_LEVEL_BUSY);

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §4  套件
 * ══════════════════════════════════════════════════════════════════════ */

MunitTest g_ftl_bg_tests[] = {
    {     (char *)"table_wiring",      test_bg_table_wiring, bg_setup, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"interval_gate",     test_bg_interval_gate, bg_setup, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"budget_truncation", test_bg_budget_truncation, bg_setup, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"clock_missing",     test_bg_clock_missing, bg_setup, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"busy_must_run",     test_bg_busy_must_run, bg_setup, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"level_select",      test_bg_level_select, bg_setup, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                       NULL,                      NULL,     NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
