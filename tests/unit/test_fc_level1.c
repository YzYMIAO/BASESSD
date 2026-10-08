/*
 * test_fc_level1.c — Level 1 纯逻辑测试：fc_command + fc_queue + fc_scheduler
 *
 * 所有 Level 1 测试汇总在一个文件中，共 16 个测试函数。
 *
 * 资源需求：
 *   - fc_queue：全部 stack 分配，零初始化依赖
 *   - fc_command 的 status_init 用例：需要 fc_init() 初始化 command pool
 *     （suite 级一次性初始化，通过 setup_fc_suite 控制）
 *
 * 唯一链接依赖：g_nand_operation_map（.rodata, onfi_1_0.o）
 *
 * 注（v8 / v13 / v15 / v20 之后）：
 *   - 调度器的选择逻辑（HSQ 优先 / 按 LUN 保序 / 链打包 / per-LUN FIFO 挂起）
 *     已搬进 fc.c 的 static fc_poll_fill，只能经 fc_service() 触达（会推真 NAND），
 *     不再属 Level 1 纯逻辑范畴——相关用例移至 Level 2/3。
 *   - fc_command_validate 的负向守卫全部转 ASSERT；param.cw_mask 由 FTL 读，
 *     FC 侧不再校验。
 *   - fc_command_t 不再携带 command_id（v15），ID 由指针派生。
 */

#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_queue.h"
#include "fc/fc_scheduler.h"
#include "munit.h"
#include "nand/onfi_1_0.h"

/**************************************************************************
 *  Part 1: fc_command 测试
 *
 *  fc_command_validate（v20 起恒返回 FC_STATUS_OK）：
 *   三条守卫（command==NULL / operation>=OP_COUNT / 数据操作无 desc 链）
 *   已全部转 ASSERT —— 负向用例随之删除，只保留正向「合法 OP 恒 OK」。
 *   注：v13 起 param.cw_mask 的读者是 FTL，FC 侧不再校验它。
 *
 *  fc_command_status_init / set_cw：
 *   验证 CQ entry 初始化、command_id 由指针派生（v15）、逐 CW 状态聚合。
 **************************************************************************/

/* status_init 的两个用例要经 fc_command_to_id 反查命令池，需先 fc_init()。 */
static int g_fc_inited = 0;

static void *setup_fc_suite(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    if (!g_fc_inited)
    {
        fc_init();
        g_fc_inited = 1;
    }
    return NULL;
}

/* ── fc_command_validate ── */

static MunitResult test_command_validate_op_range(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t cmd = {0};

    /* 合法 OP：0 .. OP_COUNT-1 恒 OK（非法 OP 现由 ASSERT 拦截，不再是返回码） */
    for (uint8_t op = 0U; op < OP_COUNT; op++)
    {
        cmd.config.operation = op;
        cmd.desc_id          = 0U;
        munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

static MunitResult test_command_validate_set_get_no_desc(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t cmd = {0};

    cmd.desc_id = FC_DESCRIPTOR_ID_INVALID;

    cmd.config.operation = OP_SET_FEATURES;
    munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_OK);

    cmd.config.operation = OP_GET_FEATURES;
    munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_OK);

    return MUNIT_OK;
}

/* ── fc_command_status_init ── */

/* 池内命令：command_id 由指针派生（v15 起 fc_command_t 不再有该字段）。 */
static MunitResult test_command_status_init_pool_cmd(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t cq_entry;
    fc_command_id_t     expect_id = FC_COMMAND_ID_INVALID;

    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->desc_id = 0x02U;
    cmd->module  = FC_COMMAND_MODULE_FTL;

    munit_assert_uint32(fc_command_to_id(cmd, &expect_id), ==, 1U);

    fc_command_status_init(&cq_entry, cmd);

    munit_assert_uint32(cq_entry.command_id, ==, expect_id);
    munit_assert_uint32(cq_entry.desc_id, ==, 0x02U);
    munit_assert_uint32(cq_entry.module, ==, FC_COMMAND_MODULE_FTL);

    munit_assert_uint32(cq_entry.status, ==, FC_CW_STATUS_READY);

    /* CQ 的 param 是命令级返回段（读命令=失败掩码 / FEATURES=寄存器值），
     * 由完成路径填；status_init 只清零，不预置 per-CW ECC（已从 CQ 删除）。 */
    munit_assert_uint32(cq_entry.param.cw_mask, ==, 0U);

    fc_command_release(cmd);

    return MUNIT_OK;
}

/* 栈上命令不在命令池内 → 派生不出 ID，落哨兵。 */
static MunitResult test_command_status_init_stack_cmd(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t cq_entry;
    fc_command_t        cmd = {0};

    cmd.desc_id = 0x02U;
    cmd.module  = FC_COMMAND_MODULE_FTL;

    fc_command_status_init(&cq_entry, &cmd);

    munit_assert_uint32(cq_entry.command_id, ==, FC_COMMAND_ID_INVALID);
    munit_assert_uint32(cq_entry.desc_id, ==, 0x02U);
    munit_assert_uint32(cq_entry.module, ==, FC_COMMAND_MODULE_FTL);
    munit_assert_uint32(cq_entry.status, ==, FC_CW_STATUS_READY);
    munit_assert_uint32(cq_entry.param.cw_mask, ==, 0U);

    return MUNIT_OK;
}

/* ── fc_command_status_set_cw ── */

static MunitResult test_command_status_set_cw_valid(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t cq_entry = {0};

    fc_command_status_set_cw(&cq_entry, 0U, FC_CW_STATUS_SUCCESS, 0x10U);
    munit_assert_uint32(cq_entry.status, ==, FC_CW_STATUS_SUCCESS);
    munit_assert_uint32(cq_entry.desc_id, ==, 0U); /* success 不记录 desc_id */

    fc_command_status_set_cw(&cq_entry, 1U, FC_CW_STATUS_NAND_FAIL, 0x20U);
    munit_assert_uint32(cq_entry.status, ==, (uint8_t)(FC_CW_STATUS_SUCCESS | FC_CW_STATUS_NAND_FAIL));
    munit_assert_uint32(cq_entry.desc_id, ==, 0x20U); /* 错误位记录 desc_id */

    fc_command_status_set_cw(&cq_entry, 3U, FC_CW_STATUS_UNC, 0x40U);
    munit_assert_uint32(cq_entry.status, ==, (uint8_t)(FC_CW_STATUS_SUCCESS | FC_CW_STATUS_NAND_FAIL | FC_CW_STATUS_UNC));
    munit_assert_uint32(cq_entry.desc_id, ==, 0x20U); /* 保留第一个错误的 desc_id */

    return MUNIT_OK;
}

/**************************************************************************
 *  Part 2: fc_queue 测试
 *
 *  测试环形 FIFO 的核心操作：push / pop / peek / pop_n，
 *  以及边界条件：空、满、环绕、NULL 输入。
 **************************************************************************/

#define TEST_Q_DEPTH (8U)

/* ── 基础 FIFO 顺序 ── */

static MunitResult test_queue_push_pop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;
    fc_command_id_t       val;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    munit_assert_int(fc_submission_queue_push(&q, 0x01U), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0x02U), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0x03U), ==, FC_STATUS_OK);

    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x01U);
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x02U);
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x03U);

    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_QUEUE_EMPTY);

    return MUNIT_OK;
}

/* ── 环形缓冲环绕 ── */

static MunitResult test_queue_wrap_around(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;
    fc_command_id_t       val;
    uint16_t              i;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    for (i = 0U; i < TEST_Q_DEPTH; i++)
    {
        munit_assert_int(fc_submission_queue_push(&q, (fc_command_id_t)(0x10U + i)), ==, FC_STATUS_OK);
    }

    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x10U);

    munit_assert_int(fc_submission_queue_push(&q, 0xAAU), ==, FC_STATUS_OK);

    /* head=1, tail=0 → 顺序：0x11..0x17, 0xAA */
    for (i = 0x11U; i <= 0x17U; i++)
    {
        munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
        munit_assert_uint32(val, ==, (fc_command_id_t)i);
    }
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0xAAU);
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_QUEUE_EMPTY);

    return MUNIT_OK;
}

/* ── 队列满 ── */

static MunitResult test_queue_overflow(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;
    uint16_t              i;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    for (i = 0U; i < TEST_Q_DEPTH; i++)
    {
        munit_assert_int(fc_submission_queue_push(&q, (fc_command_id_t)i), ==, FC_STATUS_OK);
    }

    munit_assert_int(fc_submission_queue_push(&q, 0xFFU), ==, FC_STATUS_QUEUE_FULL);

    /* pop 后恢复可用 */
    fc_command_id_t val;
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0xFFU), ==, FC_STATUS_OK);

    return MUNIT_OK;
}

/* ── peek ── */

static MunitResult test_queue_peek(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    munit_assert_int(fc_submission_queue_push(&q, 0x01U), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0x02U), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0x03U), ==, FC_STATUS_OK);

    munit_assert_uint32(fc_submission_queue_peek(&q, 0U), ==, 0x01U);
    munit_assert_uint32(fc_submission_queue_peek(&q, 0U), ==, 0x01U);
    munit_assert_uint32(fc_submission_queue_peek(&q, 1U), ==, 0x02U);
    munit_assert_uint32(fc_submission_queue_peek(&q, 2U), ==, 0x03U);

    return MUNIT_OK;
}

static MunitResult test_queue_peek_empty(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);
    munit_assert_uint32(fc_submission_queue_peek(&q, 0U), ==, FC_COMMAND_ID_INVALID);

    return MUNIT_OK;
}

/* ── pop_n ── */

static MunitResult test_queue_pop_n(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;
    fc_command_id_t       ids[5];
    fc_command_id_t       val;
    uint16_t              i;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    for (i = 0U; i < 5U; i++)
    {
        munit_assert_int(fc_submission_queue_push(&q, (fc_command_id_t)(0x10U + i)), ==, FC_STATUS_OK);
    }

    fc_submission_queue_pop_n(&q, ids, 3U);
    munit_assert_uint32(ids[0], ==, 0x10U);
    munit_assert_uint32(ids[1], ==, 0x11U);
    munit_assert_uint32(ids[2], ==, 0x12U);

    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x13U);
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x14U);
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_QUEUE_EMPTY);

    return MUNIT_OK;
}

/* ── 空/满 │ NULL 输入 ── */

static MunitResult test_queue_empty_full(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;
    uint16_t              i;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    munit_assert_uint32(fc_submission_queue_peek(&q, 0U), ==, FC_COMMAND_ID_INVALID);

    for (i = 0U; i < TEST_Q_DEPTH; i++)
    {
        munit_assert_int(fc_submission_queue_push(&q, (fc_command_id_t)i), ==, FC_STATUS_OK);
    }

    munit_assert_int(fc_submission_queue_push(&q, 0xFFU), ==, FC_STATUS_QUEUE_FULL);

    return MUNIT_OK;
}

/**************************************************************************
 *  Part 2b: fc_completion_queue 测试
 *
 *  CQ 存储 fc_command_status_t，FIFO 语义与 submission_queue 相同。
 **************************************************************************/

#define TEST_CQ_DEPTH (8U)

static MunitResult test_cqueue_init(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t   storage[TEST_CQ_DEPTH];
    fc_completion_queue_t q;

    munit_assert_int(fc_completion_queue_init(&q, storage, TEST_CQ_DEPTH), ==, FC_STATUS_OK);
    munit_assert_uint32(q.depth, ==, TEST_CQ_DEPTH);
    munit_assert_uint32(q.count, ==, 0U);

    return MUNIT_OK;
}

static MunitResult test_cqueue_push_pop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t   storage[TEST_CQ_DEPTH];
    fc_completion_queue_t q;
    fc_command_status_t   entry  = {0};
    fc_command_status_t   popped = {0};

    munit_assert_int(fc_completion_queue_init(&q, storage, TEST_CQ_DEPTH), ==, FC_STATUS_OK);

    entry.command_id = 0x01U;
    entry.status     = FC_CW_STATUS_READY;
    munit_assert_int(fc_completion_queue_push(&q, &entry), ==, FC_STATUS_OK);

    entry.command_id = 0x02U;
    munit_assert_int(fc_completion_queue_push(&q, &entry), ==, FC_STATUS_OK);

    munit_assert_int(fc_completion_queue_pop(&q, &popped), ==, FC_STATUS_OK);
    munit_assert_uint32(popped.command_id, ==, 0x01U);

    munit_assert_int(fc_completion_queue_pop(&q, &popped), ==, FC_STATUS_OK);
    munit_assert_uint32(popped.command_id, ==, 0x02U);

    munit_assert_int(fc_completion_queue_pop(&q, &popped), ==, FC_STATUS_QUEUE_EMPTY);

    return MUNIT_OK;
}

static MunitResult test_cqueue_overflow(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t   storage[TEST_CQ_DEPTH];
    fc_completion_queue_t q;
    uint16_t              i;

    munit_assert_int(fc_completion_queue_init(&q, storage, TEST_CQ_DEPTH), ==, FC_STATUS_OK);

    fc_command_status_t entry = {0};
    entry.status              = FC_CW_STATUS_READY;

    for (i = 0U; i < TEST_CQ_DEPTH; i++)
    {
        entry.command_id = (fc_command_id_t)i;
        munit_assert_int(fc_completion_queue_push(&q, &entry), ==, FC_STATUS_OK);
    }

    entry.command_id = 0xFFU;
    munit_assert_int(fc_completion_queue_push(&q, &entry), ==, FC_STATUS_QUEUE_FULL);

    return MUNIT_OK;
}

/**************************************************************************
 *  Part 3: fc_scheduler 测试
 *
 *  v8 后调度器只剩 fc_scheduler_init —— 绑定 SQ/HSQ 两个队列指针，纯 stack
 *  操作，零初始化依赖。
 *
 *  选择逻辑（HSQ 优先 / 按 LUN 保序 / 链打包 / per-LUN FIFO 挂起）已搬进
 *  fc.c 的 static fc_poll_fill，只能经 fc_service() 触达（会推真 NAND）——
 *  其覆盖属 Level 2/3，不在本文件。
 **************************************************************************/

#define SCHED_Q_DEPTH (16U)

/* ── init ── */

static MunitResult test_scheduler_init(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);

    munit_assert_int(fc_scheduler_init(&scheduler, &sq, &hsq), ==, FC_STATUS_OK);

    munit_assert_ptr_equal(scheduler.sq, &sq);
    munit_assert_ptr_equal(scheduler.hsq, &hsq);

    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 *
 *  按模块分三个子套件，全部挂在 /fc-nand/ 根套件下。
 **************************************************************************/

MunitTest g_fc_command_tests[] = {
    {       "validate_op_range",        test_command_validate_op_range,           NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"validate_set_get_no_desc", test_command_validate_set_get_no_desc,           NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "status_init_pool_cmd",     test_command_status_init_pool_cmd, setup_fc_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   "status_init_stack_cmd",    test_command_status_init_stack_cmd, setup_fc_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     "status_set_cw_valid",      test_command_status_set_cw_valid,           NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                      NULL,                                  NULL,           NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_fc_queue_tests[] = {
    {       "push_pop",    test_queue_push_pop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "wrap_around", test_queue_wrap_around, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "overflow",    test_queue_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           "peek",        test_queue_peek, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     "peek_empty",  test_queue_peek_empty, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "pop_n",       test_queue_pop_n, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     "empty_full",  test_queue_empty_full, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
 /* completion queue */
    {    "cqueue_init",       test_cqueue_init, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"cqueue_push_pop",   test_cqueue_push_pop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"cqueue_overflow",   test_cqueue_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             NULL,                   NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_fc_scheduler_tests[] = {
    {"init", test_scheduler_init, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  NULL,                NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};