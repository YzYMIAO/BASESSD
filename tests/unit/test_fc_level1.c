/*
 * test_fc_level1.c — Level 1 纯逻辑测试：fc_command + fc_queue + fc_scheduler
 *
 * 所有 Level 1 测试汇总在一个文件中，共 31 个测试函数。
 *
 * 资源需求：
 *   - fc_command / fc_queue：全部 stack 分配，零初始化依赖
 *   - fc_scheduler 部分测试：需要 fc_init() 初始化 command pool
 *     （suite 级一次性初始化，通过 setup_scheduler_suite 控制）
 *
 * 唯一链接依赖：g_nand_operation_map（.rodata, onfi_1_0.o）
 */

#include "common/log.h"
#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_queue.h"
#include "fc/fc_scheduler.h"
#include "munit.h"
#include "nand/onfi_1_0.h"

/**************************************************************************
 *  Part 1: fc_command 测试
 *
 *  fc_command_validate 守卫：
 *   守卫 1: command == NULL              → INVALID_ARGUMENT
 *   守卫 2: operation >= OP_COUNT        → INVALID_ARGUMENT
 *   守卫 3: has_data && desc_id==INVALID → INVALID_ARGUMENT
 *           例外：SET/GET_FEATURES 不走 desc
 *
 *  fc_command_status_init / set_cw：
 *   验证 CQ entry 初始化与逐 CW 写入。
 **************************************************************************/

/* ── fc_command_validate ── */

static MunitResult test_command_validate_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_command_validate(NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

static MunitResult test_command_validate_op_range(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t cmd = {0};

    /* 合法 OP：0 .. OP_COUNT-1 */
    for (uint8_t op = 0U; op < OP_COUNT; op++)
    {
        cmd.config.operation = op;
        cmd.desc_id          = 0U;
        munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_OK);
    }

    /* 非法 OP：>= OP_COUNT */
    cmd.config.operation = (uint8_t)OP_COUNT;
    munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_INVALID_ARGUMENT);

    cmd.config.operation = 0xFFU;
    munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_INVALID_ARGUMENT);

    return MUNIT_OK;
}

static MunitResult test_command_validate_desc_needed(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t cmd = {0};

    /* has_data + desc_id=INVALID → 拒绝 */
    cmd.config.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd.desc_id          = FC_DESCRIPTOR_ID_INVALID;
    munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_INVALID_ARGUMENT);

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

static MunitResult test_command_status_init_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_status_init(NULL, NULL);
    return MUNIT_OK;
}

static MunitResult test_command_status_init_cw1(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t cq_entry;
    fc_command_t        cmd = {0};

    cmd.command_id = 0x01U;
    cmd.desc_id    = 0x02U;
    cmd.module     = FC_COMMAND_MODULE_FTL;
    cmd.config.cw  = 0U; /* 1 CW */

    fc_command_status_init(&cq_entry, &cmd);

    munit_assert_uint32(cq_entry.command_id, ==, 0x01U);
    munit_assert_uint32(cq_entry.desc_id, ==, 0x02U);
    munit_assert_uint32(cq_entry.module, ==, FC_COMMAND_MODULE_FTL);

    munit_assert_uint32(cq_entry.status, ==, FC_CW_STATUS_READY);

    munit_assert_uint32(cq_entry.ecc_bit0, ==, 0U);
    munit_assert_uint32(cq_entry.ecc_bit1, ==, FC_CW_STATUS_UNUSED);
    munit_assert_uint32(cq_entry.ecc_bit2, ==, FC_CW_STATUS_UNUSED);
    munit_assert_uint32(cq_entry.ecc_bit3, ==, FC_CW_STATUS_UNUSED);

    return MUNIT_OK;
}

static MunitResult test_command_status_init_cw4(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t cq_entry;
    fc_command_t        cmd = {0};

    cmd.command_id = 0x01U;
    cmd.desc_id    = 0x02U;
    cmd.module     = FC_COMMAND_MODULE_FTL;
    cmd.config.cw  = 3U; /* 4 CW */

    fc_command_status_init(&cq_entry, &cmd);

    munit_assert_uint32(cq_entry.status, ==, FC_CW_STATUS_READY);

    munit_assert_uint32(cq_entry.ecc_bit0, ==, 0U);
    munit_assert_uint32(cq_entry.ecc_bit1, ==, 0U);
    munit_assert_uint32(cq_entry.ecc_bit2, ==, 0U);
    munit_assert_uint32(cq_entry.ecc_bit3, ==, 0U);

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

static MunitResult test_command_status_set_cw_overflow(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_status_t cq_entry = {0};

    fc_command_status_set_cw(&cq_entry, 4U, FC_CW_STATUS_NAND_FAIL, 0x50U);
    munit_assert_uint32(cq_entry.status, ==, 0U);

    fc_command_status_set_cw(&cq_entry, 255U, FC_CW_STATUS_NAND_FAIL, 0x60U);
    munit_assert_uint32(cq_entry.status, ==, 0U);

    return MUNIT_OK;
}

static MunitResult test_command_status_set_cw_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_status_set_cw(NULL, 0U, FC_CW_STATUS_SUCCESS, 0U);
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

static MunitResult test_queue_init_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_submission_queue_init(NULL, NULL, 0U), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

static MunitResult test_queue_push_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_submission_queue_push(NULL, 0x01U), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

static MunitResult test_queue_pop_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_submission_queue_pop(NULL, NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

static MunitResult test_queue_peek_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_uint32(fc_submission_queue_peek(NULL, 0U), ==, FC_COMMAND_ID_INVALID);
    return MUNIT_OK;
}

static MunitResult test_queue_pop_n_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_submission_queue_pop_n(NULL, NULL, 3U);
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

static MunitResult test_cqueue_init_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_completion_queue_init(NULL, NULL, 0U), ==, FC_STATUS_INVALID_ARGUMENT);
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

static MunitResult test_cqueue_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    munit_assert_int(fc_completion_queue_push(NULL, NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    munit_assert_int(fc_completion_queue_pop(NULL, NULL), ==, FC_STATUS_INVALID_ARGUMENT);

    return MUNIT_OK;
}

/**************************************************************************
 *  Part 3: fc_scheduler 测试
 *
 *  init / set_busy / clear_busy 是纯 stack 操作，零初始化依赖。
 *  select / select_chain 需要 fc_init() 初始化 command pool 以支持
 *  fc_command_id_to_command 查找。
 *
 *  suite 级一次性初始化通过 setup_scheduler_suite 的 static flag 控制。
 **************************************************************************/

#define SCHED_Q_DEPTH (16U)

static int g_scheduler_inited = 0;

static void print_resource_stats(const char *tag)
{
    fc_resource_stats_t stats;
    if (fc_get_resource_stats(&stats) == FC_STATUS_OK)
    {
        LOGI("[RES] %s: cmd=%u/%u desc=%u/%u buf=%u/%u sq=%u hsq=%u cq=%u\r\n",
             tag,
             (unsigned)stats.command_used,
             (unsigned)FC_COMMAND_POOL_DEPTH,
             (unsigned)stats.descriptor_used,
             (unsigned)FC_DESCRIPTOR_POOL_DEPTH,
             (unsigned)stats.buffer_used,
             (unsigned)FC_BUFFER_POOL_DEPTH,
             (unsigned)stats.sq_used,
             (unsigned)stats.hsq_used,
             (unsigned)stats.cq_used);
    }
}

static void *setup_scheduler_suite(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    if (!g_scheduler_inited)
    {
        fc_init();
        g_scheduler_inited = 1;
    }
    print_resource_stats("L1_sched");
    return NULL;
}

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
    munit_assert_uint32(scheduler.consecutive_write_count, ==, 0U);
    munit_assert_uint32(scheduler.busy[0][0][0], ==, 0U);

    return MUNIT_OK;
}

static MunitResult test_scheduler_init_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_submission_queue_t sq, hsq;

    munit_assert_int(fc_scheduler_init(NULL, NULL, NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    munit_assert_int(fc_scheduler_init(NULL, &sq, &hsq), ==, FC_STATUS_INVALID_ARGUMENT);

    return MUNIT_OK;
}

/* ── set_busy / clear_busy ── */

static MunitResult test_scheduler_set_clear_busy(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);
    fc_scheduler_init(&scheduler, &sq, &hsq);

    fc_scheduler_set_busy(&scheduler, 0U, 0U, 0U);
    munit_assert_uint32(scheduler.busy[0][0][0], ==, 1U);

    fc_scheduler_clear_busy(&scheduler, 0U, 0U, 0U);
    munit_assert_uint32(scheduler.busy[0][0][0], ==, 0U);

    /* 越界参数不崩溃 */
    fc_scheduler_set_busy(&scheduler, 9U, 9U, 9U);
    munit_assert_uint32(scheduler.busy[0][0][0], ==, 0U);

    fc_scheduler_clear_busy(&scheduler, 9U, 9U, 9U);

    return MUNIT_OK;
}

/* ── select ── */

static MunitResult test_scheduler_select_empty(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;
    fc_command_id_t       cmd_id;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);
    fc_scheduler_init(&scheduler, &sq, &hsq);

    munit_assert_int(fc_scheduler_select(&scheduler, &cmd_id), ==, FC_STATUS_QUEUE_EMPTY);

    return MUNIT_OK;
}

static MunitResult test_scheduler_select_hsq_first(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;
    fc_command_id_t       cmd_id;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);
    fc_scheduler_init(&scheduler, &sq, &hsq);

    fc_command_t *cmd_sq  = fc_command_alloc();
    fc_command_t *cmd_hsq = fc_command_alloc();
    munit_assert_not_null(cmd_sq);
    munit_assert_not_null(cmd_hsq);

    cmd_sq->config.ch     = 0U;
    cmd_sq->config.ce     = 0U;
    cmd_sq->config.lun    = 0U;
    cmd_sq->high_priority = FC_PRIORITY_SQ;
    fc_submission_queue_push(&sq, cmd_sq->command_id);

    cmd_hsq->config.ch     = 0U;
    cmd_hsq->config.ce     = 0U;
    cmd_hsq->config.lun    = 0U;
    cmd_hsq->high_priority = FC_PRIORITY_HSQ;
    fc_submission_queue_push(&hsq, cmd_hsq->command_id);

    /* HSQ 优先 */
    munit_assert_int(fc_scheduler_select(&scheduler, &cmd_id), ==, FC_STATUS_OK);
    munit_assert_uint32(cmd_id, ==, cmd_hsq->command_id);

    munit_assert_int(fc_scheduler_select(&scheduler, &cmd_id), ==, FC_STATUS_OK);
    munit_assert_uint32(cmd_id, ==, cmd_sq->command_id);

    fc_command_release(cmd_sq);
    fc_command_release(cmd_hsq);

    return MUNIT_OK;
}

static MunitResult test_scheduler_select_sq_order(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;
    fc_command_id_t       cmd_id;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);
    fc_scheduler_init(&scheduler, &sq, &hsq);

    fc_command_t *cmd_a = fc_command_alloc();
    fc_command_t *cmd_b = fc_command_alloc();
    munit_assert_not_null(cmd_a);
    munit_assert_not_null(cmd_b);

    cmd_a->config.ch  = 0U;
    cmd_a->config.ce  = 0U;
    cmd_a->config.lun = 0U;
    cmd_b->config.ch  = 0U;
    cmd_b->config.ce  = 0U;
    cmd_b->config.lun = 0U;

    fc_submission_queue_push(&sq, cmd_a->command_id);
    fc_submission_queue_push(&sq, cmd_b->command_id);

    munit_assert_int(fc_scheduler_select(&scheduler, &cmd_id), ==, FC_STATUS_OK);
    munit_assert_uint32(cmd_id, ==, cmd_a->command_id);

    munit_assert_int(fc_scheduler_select(&scheduler, &cmd_id), ==, FC_STATUS_OK);
    munit_assert_uint32(cmd_id, ==, cmd_b->command_id);

    fc_command_release(cmd_a);
    fc_command_release(cmd_b);

    return MUNIT_OK;
}

static MunitResult test_scheduler_select_busy_skip(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;
    fc_command_id_t       cmd_id;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);
    fc_scheduler_init(&scheduler, &sq, &hsq);

    fc_command_t *cmd_a = fc_command_alloc();
    fc_command_t *cmd_b = fc_command_alloc();
    munit_assert_not_null(cmd_a);
    munit_assert_not_null(cmd_b);

    cmd_a->config.ch  = 0U;
    cmd_a->config.ce  = 0U;
    cmd_a->config.lun = 0U;
    cmd_b->config.ch  = 0U;
    cmd_b->config.ce  = 0U;
    cmd_b->config.lun = 0U;

    fc_submission_queue_push(&sq, cmd_a->command_id);
    fc_submission_queue_push(&sq, cmd_b->command_id);

    fc_scheduler_set_busy(&scheduler, 0U, 0U, 0U);

    /* 两命令都指向 LUN0，LUN0 忙 → 全被旋转 → NOT_READY */
    munit_assert_int(fc_scheduler_select(&scheduler, &cmd_id), ==, FC_STATUS_NOT_READY);

    fc_scheduler_clear_busy(&scheduler, 0U, 0U, 0U);
    munit_assert_int(fc_scheduler_select(&scheduler, &cmd_id), ==, FC_STATUS_OK);
    munit_assert_uint32(cmd_id, ==, cmd_a->command_id);

    fc_command_release(cmd_a);
    fc_command_release(cmd_b);

    return MUNIT_OK;
}

/* ── select 参数校验 ── */

static MunitResult test_scheduler_select_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_scheduler_select(NULL, NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

/* ── 读饥饿保护（select_chain 实现） ── */

static MunitResult test_scheduler_starvation(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;
    fc_scheduler_chain_t  chain;
    uint16_t              i;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);
    fc_scheduler_init(&scheduler, &sq, &hsq);

    fc_command_t *write_cmds[3];
    /* push 3 个非读命令（PROGRAM），chain=0 单命令 */
    for (i = 0U; i < 3U; i++)
    {
        write_cmds[i] = fc_command_alloc();
        munit_assert_not_null(write_cmds[i]);
        write_cmds[i]->config.ch        = 0U;
        write_cmds[i]->config.ce        = 0U;
        write_cmds[i]->config.lun       = 0U;
        write_cmds[i]->config.operation = OP_NORMAL_PROGRAM_PAGE;
        write_cmds[i]->config.chain     = FC_CHAIN_END;
        write_cmds[i]->high_priority    = FC_PRIORITY_SQ;
        fc_submission_queue_push(&sq, write_cmds[i]->command_id);
    }

    /* select_chain 前 3 次都返回 PROGRAM（写计数递增） */
    for (i = 0U; i < 3U; i++)
    {
        munit_assert_int(fc_scheduler_select_chain(&scheduler, &chain), ==, FC_STATUS_OK);
        munit_assert_uint32(chain.count, ==, 1U);
        fc_command_t *cmd = fc_command_id_to_command(chain.ids[0U]);
        munit_assert_not_null(cmd);
        munit_assert_uint32(cmd->config.operation, ==, OP_NORMAL_PROGRAM_PAGE);
    }

    fc_command_t *rd_cmd, *wr_cmd;
    /* push 1 个读命令 + 1 个非读命令 */
    rd_cmd = fc_command_alloc();
    munit_assert_not_null(rd_cmd);
    rd_cmd->config.ch        = 0U;
    rd_cmd->config.ce        = 0U;
    rd_cmd->config.lun       = 0U;
    rd_cmd->config.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    rd_cmd->config.chain     = FC_CHAIN_END;
    rd_cmd->high_priority    = FC_PRIORITY_SQ;
    fc_submission_queue_push(&sq, rd_cmd->command_id);

    wr_cmd = fc_command_alloc();
    munit_assert_not_null(wr_cmd);
    wr_cmd->config.ch        = 0U;
    wr_cmd->config.ce        = 0U;
    wr_cmd->config.lun       = 0U;
    wr_cmd->config.operation = OP_NORMAL_PROGRAM_PAGE;
    wr_cmd->config.chain     = FC_CHAIN_END;
    wr_cmd->high_priority    = FC_PRIORITY_SQ;
    fc_submission_queue_push(&sq, wr_cmd->command_id);

    /* write_budget=3 已满，select_chain 应选出读命令（starvation 保护触发） */
    munit_assert_int(fc_scheduler_select_chain(&scheduler, &chain), ==, FC_STATUS_OK);
    munit_assert_uint32(chain.count, ==, 1U);
    fc_command_t *cmd = fc_command_id_to_command(chain.ids[0U]);
    munit_assert_not_null(cmd);
    munit_assert_uint32(cmd->config.operation, ==, OP_NORMAL_READ_PAGE_WITH_DATA_OUT);

    for (i = 0U; i < 3U; i++)
    {
        fc_command_release(write_cmds[i]);
    }
    fc_command_release(rd_cmd);
    fc_command_release(wr_cmd);

    return MUNIT_OK;
}

/* ── select_chain ── */

static MunitResult test_scheduler_select_chain_single(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;
    fc_scheduler_chain_t  chain;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);
    fc_scheduler_init(&scheduler, &sq, &hsq);

    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);
    cmd->config.ch    = 0U;
    cmd->config.ce    = 0U;
    cmd->config.lun   = 0U;
    cmd->config.chain = FC_CHAIN_END;
    fc_submission_queue_push(&sq, cmd->command_id);

    munit_assert_int(fc_scheduler_select_chain(&scheduler, &chain), ==, FC_STATUS_OK);
    munit_assert_uint32(chain.count, ==, 1U);
    munit_assert_uint32(chain.ids[0], ==, cmd->command_id);

    fc_command_release(cmd);

    return MUNIT_OK;
}

static MunitResult test_scheduler_select_chain_multi(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;
    fc_scheduler_chain_t  chain;
    uint16_t              i;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);
    fc_scheduler_init(&scheduler, &sq, &hsq);

    fc_command_t *cmds[3];
    for (i = 0U; i < 3U; i++)
    {
        cmds[i] = fc_command_alloc();
        munit_assert_not_null(cmds[i]);
        cmds[i]->config.ch    = 0U;
        cmds[i]->config.ce    = 0U;
        cmds[i]->config.lun   = 0U;
        cmds[i]->config.chain = (i < 2U) ? FC_CHAIN_MORE : FC_CHAIN_END;
        fc_submission_queue_push(&sq, cmds[i]->command_id);
    }

    munit_assert_int(fc_scheduler_select_chain(&scheduler, &chain), ==, FC_STATUS_OK);
    munit_assert_uint32(chain.count, ==, 3U);
    for (i = 0U; i < 3U; i++)
    {
        munit_assert_uint32(chain.ids[i], ==, cmds[i]->command_id);
    }

    for (i = 0U; i < 3U; i++)
    {
        fc_command_release(cmds[i]);
    }

    return MUNIT_OK;
}

/* ── fc_submit / fc_poll_completion / fc_command_configure_descriptors NULL 测试 ── */

static MunitResult test_submit_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_submit(NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

static MunitResult test_poll_completion_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_poll_completion(NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

static MunitResult test_configure_descriptors_null(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    munit_assert_int(fc_command_configure_descriptors(NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 *
 *  按模块分三个子套件，全部挂在 /fc-nand/ 根套件下。
 **************************************************************************/

MunitTest g_fc_command_tests[] = {
    {             "validate_null",            test_command_validate_null,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         "validate_op_range",        test_command_validate_op_range,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      "validate_desc_needed",     test_command_validate_desc_needed,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  "validate_set_get_no_desc", test_command_validate_set_get_no_desc,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "status_init_null",         test_command_status_init_null,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           "status_init_cw1",          test_command_status_init_cw1,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           "status_init_cw4",          test_command_status_init_cw4,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "status_set_cw_valid",      test_command_status_set_cw_valid,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "status_set_cw_overflow",   test_command_status_set_cw_overflow,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        "status_set_cw_null",       test_command_status_set_cw_null,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
 /* fc_submit / fc_poll_completion NULL 测试（需要 fc_init） */
    {               "submit_null",                      test_submit_null, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      "poll_completion_null",             test_poll_completion_null, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"configure_descriptors_null",       test_configure_descriptors_null, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                        NULL,                                  NULL,                  NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_fc_queue_tests[] = {
    {        "push_pop",    test_queue_push_pop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     "wrap_around", test_queue_wrap_around, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        "overflow",    test_queue_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {            "peek",        test_queue_peek, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      "peek_empty",  test_queue_peek_empty, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           "pop_n",       test_queue_pop_n, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      "empty_full",  test_queue_empty_full, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "init_null",   test_queue_init_null, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "push_null",   test_queue_push_null, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        "pop_null",    test_queue_pop_null, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "peek_null",   test_queue_peek_null, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      "pop_n_null",  test_queue_pop_n_null, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
 /* completion queue */
    {     "cqueue_init",       test_cqueue_init, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"cqueue_init_null",  test_cqueue_init_null, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { "cqueue_push_pop",   test_cqueue_push_pop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { "cqueue_overflow",   test_cqueue_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     "cqueue_null",       test_cqueue_null, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              NULL,                   NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_fc_scheduler_tests[] = {
    {               "init",                test_scheduler_init,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "init_null",           test_scheduler_init_null,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     "set_clear_busy",      test_scheduler_set_clear_busy,                  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "select_empty",        test_scheduler_select_empty, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   "select_hsq_first",    test_scheduler_select_hsq_first, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "select_sq_order",     test_scheduler_select_sq_order, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   "select_busy_skip",    test_scheduler_select_busy_skip, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        "select_null",         test_scheduler_select_null, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         "starvation",          test_scheduler_starvation, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"select_chain_single", test_scheduler_select_chain_single, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { "select_chain_multi",  test_scheduler_select_chain_multi, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                 NULL,                               NULL,                  NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};