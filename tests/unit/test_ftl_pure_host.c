/*
 * test_ftl_pure_host.c — L0 /ftl/pure/host/：pending / group / wbuf / ftl_op / bg
 *
 * 只碰 RAM：池/组/写缓冲/完成旁表；不提交 FC 命令。
 */

#include "bg_scheduler.h"
#include "ftl_op.h"
#include "ftl_ut_common.h"
#include "munit.h"
#include "pending.h"
#include "wbuf.h"

#include <string.h>

static DWORD s_group_done;

static void host_group_done(void *ctx)
{
    (void)ctx;
    s_group_done++;
}

static MunitResult test_host_idle(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t *ftl = ftl_ut_live();

    ftl_ut_trace_begin();

    munit_assert_uint32(pend_slot_get_count(&ftl->pending), ==, 0U);
    munit_assert_uint32(pend_group_get_count(&ftl->pending), ==, 0U);
    munit_assert_uint32(ftl_op_ctx_count(), ==, 0U);
    munit_assert_true(wb_is_idle(ftl->wbuf));
    munit_assert_uint32(ftl->bg.task_count, ==, (DWORD)BG_TASK_COUNT);

    ftl_ut_trace_summary("host: pending=0 groups=0 ctx=0 bg_tasks=%u", ftl->bg.task_count);

    return MUNIT_OK;
}

static MunitResult test_pending_slots(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    pend_table_t table;

    ftl_ut_trace_begin();
    memset(&table, 0, sizeof(table));
    pend_init(&table);

    pend_req_t *slots[FTL_PENDING_MAX];

    for (WORD i = 0U; i < FTL_PENDING_MAX; i++)
    {
        slots[i] = pend_alloc(&table);
        munit_assert_not_null(slots[i]);
    }

    munit_assert_null(pend_alloc(&table));
    munit_assert_uint32(pend_slot_get_count(&table), ==, (DWORD)FTL_PENDING_MAX);

    /* 按 command_id 查找/移除；移除后槽位可再分配。 */
    slots[0]->command_id = 0x1234U;
    munit_assert_ptr_equal(pend_find(&table, 0x1234U), slots[0]);
    pend_remove(&table, 0x1234U);
    munit_assert_null(pend_find(&table, 0x1234U));
    munit_assert_uint32(pend_slot_get_count(&table), ==, (DWORD)(FTL_PENDING_MAX - 1U));
    munit_assert_not_null(pend_alloc(&table));

    ftl_ut_trace_summary("pending: full=%u then remove/alloc ok", FTL_PENDING_MAX);

    return MUNIT_OK;
}

static MunitResult test_group_exhaustion(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    pend_table_t table;
    BYTE         gi[FTL_GROUP_MAX];

    ftl_ut_trace_begin();
    memset(&table, 0, sizeof(table));
    pend_init(&table);

    for (WORD i = 0U; i < FTL_GROUP_MAX; i++)
    {
        munit_assert_true(pend_group_alloc(&table, &gi[i]));
    }

    table.groups[gi[0]].total = 7U;

    BYTE extra = 0xFFU;

    /* 组槽满：必须返回 false，且不得覆盖已有组（RISK-PEND-02）。 */
    munit_assert_false(pend_group_alloc(&table, &extra));
    munit_assert_uint32(table.groups[gi[0]].total, ==, 7U);
    munit_assert_true(table.groups[gi[0]].used);
    munit_assert_uint32(pend_group_get_count(&table), ==, (DWORD)FTL_GROUP_MAX);

    ftl_ut_trace_summary("group: exhaust=%u, no overwrite", FTL_GROUP_MAX);

    return MUNIT_OK;
}

static MunitResult test_group_complete_once(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    pend_table_t table;
    BYTE         gi = 0U;

    ftl_ut_trace_begin();
    memset(&table, 0, sizeof(table));
    pend_init(&table);
    s_group_done = 0U;

    munit_assert_true(pend_group_alloc(&table, &gi));

    pend_group_t *g = &table.groups[gi];

    g->total         = 2U;
    g->next          = 2U;
    g->on_complete   = host_group_done;
    g->complete_flag = NULL;

    pend_group_add_subop(&table, gi);
    pend_group_add_subop(&table, gi);
    munit_assert_uint32(g->outstanding, ==, 2U);

    pend_group_complete_subop(&table, gi, SSD_STATUS_OK);
    munit_assert_uint32(s_group_done, ==, 0U);
    pend_group_complete_subop(&table, gi, SSD_STATUS_OK);

    munit_assert_uint32(s_group_done, ==, 1U);
    munit_assert_false(g->used);
    munit_assert_uint32(pend_group_get_count(&table), ==, 0U);

    ftl_ut_trace_summary("group: complete once (done=%lu)", s_group_done);

    return MUNIT_OK;
}

static MunitResult test_wbuf_append_read(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    wb_handle_t  h;
    pend_table_t table;
    BYTE         gi = 0U;
    BYTE         src[FTL_IU_BYTES];
    BYTE         out[FTL_IU_BYTES];
    wb_trigger_t tr;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();
    memset(&h, 0, sizeof(h));
    memset(&table, 0, sizeof(table));
    pend_init(&table);

    munit_assert_true(pend_group_alloc(&table, &gi));
    wb_init(&h, ftl_ut_ram_alloc(), &table, NULL);

    for (WORD i = 0U; i < FTL_IU_BYTES; i++)
    {
        src[i] = (BYTE)(i & 0xFFU);
    }

    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_APPEND;
    tr.lba  = ftl_lba_pair_base(0U);
    tr.data = src;
    tr.n    = 1U;
    tr.gi   = gi;

    munit_assert_int(wb_handle(&h, &tr, NULL, NULL), ==, FTL_OK);
    munit_assert_true(wb_has(&h, 0U));
    munit_assert_true(wb_read(&h, 0U, out));
    munit_assert_memory_equal(FTL_IU_BYTES, src, out);

    munit_assert_uint32(h.owners, ==, ftl_group_bit(gi));
    munit_assert_uint32(table.groups[gi].outstanding, ==, 1U);
    munit_assert_false(wb_is_idle(&h));

    ftl_ut_trace_summary("wbuf: occupied=0x%02hhX owners=0x%08lX", h.occupied, h.owners);

    return MUNIT_OK;
}

static MunitResult test_op_ctx(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    int dummy = 0;

    ftl_ut_trace_begin();

    munit_assert_uint32(ftl_op_ctx_count(), ==, 0U);
    munit_assert_int(ftl_op_ctx_add(60U, &dummy), ==, FTL_OK);
    munit_assert_uint32(ftl_op_ctx_count(), ==, 1U);
    munit_assert_ptr_equal(ftl_op_ctx_take(60U), &dummy);
    munit_assert_uint32(ftl_op_ctx_count(), ==, 0U);

    ftl_op_ctx_add(61U, &dummy);
    ftl_op_ctx_remove(61U);
    munit_assert_uint32(ftl_op_ctx_count(), ==, 0U);

    ftl_ut_trace_summary("op ctx: add/take/remove ok");

    return MUNIT_OK;
}

static MunitResult test_bg_engine(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    bg_scheduler_t sched;
    bg_level_e     level;

    ftl_ut_trace_begin();
    memset(&sched, 0, sizeof(sched));
    bg_scheduler_init(&sched);

    munit_assert_uint32(sched.task_count, ==, (DWORD)BG_TASK_COUNT);

    level = bg_scheduler_level_select(ftl_ut_live());

    munit_assert_uint32((DWORD)level, <, (DWORD)BG_LEVEL_NUM);

    ftl_ut_trace_summary("bg: tasks=%hhu level=%d", sched.task_count, level);

    return MUNIT_OK;
}

MunitTest g_ftl_pure_host_tests[] = {
    {          (char *)"host_idle",           test_host_idle, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"pending_slots",       test_pending_slots, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"group_exhaustion",    test_group_exhaustion, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"group_complete_once", test_group_complete_once, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"wbuf_append_read",    test_wbuf_append_read, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"op_ctx",              test_op_ctx, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"bg_engine",           test_bg_engine, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                         NULL,                     NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
