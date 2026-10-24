/*
 * test_ftl_pure_mem.c — L0 /ftl/pure/mem/：allocator / BST / BBT / wear
 *
 * 只碰 RAM：测试自有表或纯策略函数；不调用 ftl_sync_* 或任何 NAND 路径。
 */

#include "bst.h"
#include "ftl_config.h"
#include "ftl_ut_common.h"
#include "munit.h"

/* 把每个 plane 的前 n 个块放进 Free Pool（测试自有表，plane 内块号 0..n-1）。 */
static void mem_fill_free(alc_t *a, WORD n)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD i = 0U; i < n; i++)
        {
            alc_free_push(a, ftl_gblock(p, i));
        }
    }
}

static MunitResult test_address_roundtrip(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    const DWORD lba = ftl_lba_make(1236U, 1U); /* page_base=1236（含 plane 位）, cw=1 */

    munit_assert_uint32(ftl_lba_page_base(lba), ==, 1236U);
    munit_assert_uint32(ftl_lba_cw(lba), ==, 1U);
    munit_assert_uint32(ftl_lba_plane(lba), ==, 1U);

    const DWORD pba = pba_make(0U, 0U, 0U, 7U, 1U, 3U, 2U);

    munit_assert_uint32(pba_to_plane(pba), ==, 1U);
    munit_assert_uint32(pba_to_block(pba), ==, 7U);
    munit_assert_uint32(pba_to_page(pba), ==, 3U);
    munit_assert_uint32(pba_to_cw(pba), ==, 2U);
    munit_assert_uint32(ftl_gblock(1U, 7U), ==, (DWORD)BLOCKS_PER_PLANE + 7U);

    ftl_ut_trace_summary("addr roundtrip: lba=0x%08lX pba=0x%08lX gblock=%u", lba, pba, ftl_gblock(1U, 7U));

    return MUNIT_OK;
}

static MunitResult test_free_pool_invariants(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t *a = ftl_ut_ram_alloc();

    munit_assert_uint32(a->free_count, ==, 0U);

    mem_fill_free(a, 4U);

    munit_assert_uint32(a->free_count, ==, (DWORD)(FTL_UT_BLOCKS));

    /* state == FREE ⟺ 在池；SB 归属已清。 */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD i = 0U; i < 4U; i++)
        {
            const WORD         g = ftl_gblock(p, i);
            const bst_block_t *b = &a->blocks[g];

            munit_assert_uint32(ftl_block_state(b), ==, FTL_BLOCK_STATE_FREE);
            munit_assert_uint32(ftl_block_sb_id(b), ==, FTL_BLOCK_NO_SB);
        }
    }

    /* 出池：只改池，不改状态（文档化语义）。 */
    alc_free_remove(a, ftl_gblock(0U, 1U));
    munit_assert_uint32(a->free_count, ==, (DWORD)(FTL_UT_BLOCKS - 1U));
    munit_assert_uint32(ftl_block_state(&a->blocks[ftl_gblock(0U, 1U)]), ==, FTL_BLOCK_STATE_FREE);

    /* OP refill：把 OP 块归还 Free Pool（LIFO，per-plane）。 */
    munit_assert_true(alc_op_push(a, 1U, ftl_gblock(1U, 4U)));
    const WORD refilled = alc_free_refill_from_op(a, 1U);

    munit_assert_uint32(refilled, ==, ftl_gblock(1U, 4U));
    munit_assert_uint32(a->free_count, ==, (DWORD)(FTL_UT_BLOCKS));
    munit_assert_uint32(a->op_count[1], ==, 0U);

    ftl_ut_trace_summary("free pool: count=%hu refill=g%hu", a->free_count, refilled);

    return MUNIT_OK;
}

static MunitResult test_super_block_open_close(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t *a = ftl_ut_ram_alloc();

    mem_fill_free(a, 2U);

    DWORD out[PLANE_NUM];

    munit_assert_int(alc_super_open_alloc_pair(a, ALC_SUPER_ALLOC_HOT, out), ==, FTL_OK);

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        munit_assert_true(pba_is_valid(out[p]));
        munit_assert_uint32(pba_to_plane(out[p]), ==, p);
        munit_assert_uint32(ftl_block_state(&a->blocks[ftl_gblock(p, pba_to_block(out[p]))]), ==, FTL_BLOCK_STATE_OPEN);
    }

    /* rewind：紧邻一次取页对且未编程 ⇒ 再取应得到同一页，不烧块。 */
    alc_super_open_rewind_pair(a, FTL_TEMP_HOT);

    DWORD again[PLANE_NUM];

    munit_assert_int(alc_super_open_alloc_pair(a, ALC_SUPER_ALLOC_HOT, again), ==, FTL_OK);
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        munit_assert_uint32(again[p], ==, out[p]);
    }

    munit_assert_int(alc_super_close(a, FTL_TEMP_HOT), ==, FTL_OK);
    munit_assert_uint32(a->closed_sb_count, ==, 1U);

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        const WORD g = ftl_gblock(p, pba_to_block(out[p]));

        munit_assert_uint32(ftl_block_state(&a->blocks[g]), ==, FTL_BLOCK_STATE_CLOSED);
        munit_assert_uint32(ftl_block_sb_id(&a->blocks[g]), ==, 0U);
    }

    alc_super_closed_remove(a, 0U);
    munit_assert_uint32(a->closed_sb_count, ==, 0U);

    ftl_ut_trace_summary("super: closed=0 after remove (free=%hu)", a->free_count);

    return MUNIT_OK;
}

static MunitResult test_closed_list_add_drop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t *a = ftl_ut_ram_alloc();

    WORD blocks[PLANE_NUM];

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        blocks[p] = 2U;
    }

    const WORD idx = alc_super_closed_add(a, blocks, FTL_TEMP_COLD);

    munit_assert_uint32(idx, ==, 0U);
    munit_assert_uint32(a->closed_sb_count, ==, 1U);

    /* 摘掉 plane0：条目仍在；再摘 plane1：条目移除。 */
    munit_assert_false(alc_super_closed_drop_plane(a, 0U, blocks[0]));
    munit_assert_uint32(a->closed_sb_count, ==, 1U);
    munit_assert_true(alc_super_closed_drop_plane(a, 1U, blocks[1]));
    munit_assert_uint32(a->closed_sb_count, ==, 0U);

    ftl_ut_trace_summary("closed list: add/drop plane ok");

    return MUNIT_OK;
}

static MunitResult test_valid_count_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t       *a = ftl_ut_ram_alloc();
    bst_block_t *b = &a->blocks[ftl_gblock(0U, 0U)];

    ftl_block_add_valid(b);
    ftl_block_add_valid(b);
    munit_assert_uint32(b->valid_count, ==, 2U);

    ftl_block_sub_valid_if_live(b);
    munit_assert_uint32(b->valid_count, ==, 1U);

    /* BAD 后不得再记账（防 uint16 回绕污染 GC/WL）。 */
    ftl_block_set_state(b, FTL_BLOCK_STATE_BAD);
    ftl_block_sub_valid_if_live(b);
    ftl_block_sub_valid_if_live(b);
    munit_assert_uint32(b->valid_count, ==, 1U);

    ftl_ut_trace_summary("valid guard: count=%hu (BAD no sub)", b->valid_count);

    return MUNIT_OK;
}

static MunitResult test_op_spare_pools(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t *a = ftl_ut_ram_alloc();

    /* OP：per-plane 独立计数 + 容量上限。 */
    munit_assert_true(alc_op_push(a, 0U, ftl_gblock(0U, 0U)));
    munit_assert_true(alc_op_push(a, 1U, ftl_gblock(1U, 0U)));
    munit_assert_uint32(alc_op_get_count(a, 0U), ==, 1U);
    munit_assert_uint32(alc_op_get_count(a, 1U), ==, 1U);

    while (alc_op_push(a, 0U, ftl_gblock(0U, 0U)))
    {
        /* 填满到容量上限 */
    }
    munit_assert_uint32(alc_op_get_count(a, 0U), ==, (DWORD)FTL_OP_BLOCKS_PER_PLANE);
    munit_assert_uint32(alc_op_get_count(a, 1U), ==, 1U);

    /* Spare：LIFO 弹出。 */
    const WORD g = ftl_gblock(1U, 3U);

    munit_assert_true(alc_spare_push(a, 1U, g));

    WORD out = 0U;
    munit_assert_true(alc_spare_pop(a, 1U, &out));
    munit_assert_uint32(out, ==, g);
    munit_assert_uint32(alc_spare_get_count(a, 1U), ==, 0U);
    munit_assert_false(alc_spare_pop(a, 1U, &out));

    ftl_ut_trace_summary("op/spare: op0=%hu op1=%hu spare1=%hu", alc_op_get_count(a, 0U), alc_op_get_count(a, 1U), alc_spare_get_count(a, 1U));

    return MUNIT_OK;
}

static MunitResult test_erase_stats(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t       *a = ftl_ut_ram_alloc();
    bst_block_t *b = &a->blocks[ftl_gblock(0U, 2U)];

    ftl_block_add_valid(b);
    b->read_count = 7U;
    ftl_block_set_state(b, FTL_BLOCK_STATE_OPEN);

    alc_bst_note_erase(a, ftl_gblock(0U, 2U), 1234U);

    munit_assert_uint32(b->erase_count, ==, 1U);
    munit_assert_uint32(b->last_pe_ts, ==, 1234U);
    munit_assert_uint32(b->valid_count, ==, 0U);
    munit_assert_uint32(b->read_count, ==, 0U);

    ftl_ut_trace_summary("erase: count=%lu ts=%lu", b->erase_count, b->last_pe_ts);

    return MUNIT_OK;
}

MunitTest g_ftl_pure_mem_tests[] = {
    {     (char *)"address_roundtrip",      test_address_roundtrip, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"free_pool_invariants",   test_free_pool_invariants, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"super_block_open_close", test_super_block_open_close, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"closed_list_add_drop",   test_closed_list_add_drop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"valid_count_guard",      test_valid_count_guard, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        (char *)"op_spare_pools",         test_op_spare_pools, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"erase_stats",            test_erase_stats, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                            NULL,                        NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
