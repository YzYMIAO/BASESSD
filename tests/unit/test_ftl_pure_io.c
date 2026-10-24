/*
 * test_ftl_pure_io.c — L0 /ftl/pure/io/：L2P 驻留行 / GC / WL / RR 策略
 *
 * 只碰 RAM：策略函数与驻留 cache 行；不提交 NAND 命令。
 */

#include "ftl_ut_common.h"
#include "gc.h"
#include "l2p_cache.h"
#include "munit.h"
#include "read_recovery.h"
#include "wear_leveling.h"

#include <string.h>

static void io_fill_free(alc_t *a, WORD n)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD i = 0U; i < n; i++)
        {
            alc_free_push(a, ftl_gblock(p, i));
        }
    }
}

static MunitResult test_policy_entry(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t *ftl = ftl_ut_live();

    ftl_ut_trace_begin();

    const DWORD triggers = gc_should_trigger(&ftl->alloc);
    const bool  wl       = wl_should_trigger(&ftl->alloc, ftl->total_writes);

    munit_assert_uint32(triggers & ~(GC_TRIGGER_WATERMARK | GC_TRIGGER_EMERGENCY), ==, 0U);
    munit_assert_uint32(ftl->cache->load_inflight, ==, 0U);

    ftl_ut_trace_summary("policy: gc_triggers=0x%02lX wl=%d", triggers, (int)wl);

    return MUNIT_OK;
}

static MunitResult test_gc_trigger(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t *a = ftl_ut_ram_alloc();

    munit_assert_uint32(gc_should_trigger(NULL), ==, 0U);

    /* 空池：紧急水位必命中。 */
    DWORD trig = gc_should_trigger(a);

    munit_assert_uint32((trig & GC_TRIGGER_EMERGENCY), !=, 0U);

    /* 补到 TARGET 以上：不再触发。 */
    io_fill_free(a, 20U);
    munit_assert_uint32(a->free_count, >=, (DWORD)FREE_POOL_TARGET);
    munit_assert_uint32(gc_should_trigger(a), ==, 0U);

    /* 掉到 TARGET 以下（仍高于紧急线）：只命中 WATERMARK。 */
    while (a->free_count >= (WORD)FREE_POOL_TARGET)
    {
        alc_free_remove(a, a->free_blocks[0]);
    }
    trig = gc_should_trigger(a);
    munit_assert_uint32((trig & GC_TRIGGER_WATERMARK), !=, 0U);

    ftl_ut_trace_summary("gc trigger: free=%hu trig=0x%02lX", a->free_count, trig);

    return MUNIT_OK;
}

static MunitResult test_gc_select_victim(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    WORD            blocks_a[PLANE_NUM];
    WORD            blocks_b[PLANE_NUM];
    gc_select_out_t out;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t *a = ftl_ut_ram_alloc();

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        blocks_a[p] = 0U;
        blocks_b[p] = 1U;
    }

    const WORD ia = alc_super_closed_add(a, blocks_a, FTL_TEMP_HOT);
    const WORD ib = alc_super_closed_add(a, blocks_b, FTL_TEMP_HOT);

    munit_assert_uint32(ia, !=, ib);

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        ftl_block_set_valid(&a->blocks[ftl_gblock(p, 0U)], 10U);
        ftl_block_set_valid(&a->blocks[ftl_gblock(p, 1U)], (WORD)(FTL_CW_PER_BLOCK - 8U));
    }

    memset(&out, 0, sizeof(out));
    munit_assert_int(gc_select_victim(a, GC_BENEFIT_OFF, &out), ==, GC_SELECT_OK);
    munit_assert_not_null(out.victim);
    munit_assert_uint32(out.victim->plane_blocks[0], ==, 0U);

    ftl_ut_trace_summary("gc victim: valid=%hu scanned=%hu", out.best_valid_pct, out.scanned);

    return MUNIT_OK;
}

static MunitResult test_wl_trigger_and_victim(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    WORD        blocks[PLANE_NUM];
    wl_victim_t victim;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    alc_t *a = ftl_ut_ram_alloc();

    /* 启动门以下不触发。 */
    munit_assert_false(wl_should_trigger(a, WL_MIN_WRITES - 1U));

    /* Free Pool：高擦除空闲块（plane 内块 10/11）。 */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD b = 10U; b <= 11U; b++)
        {
            const WORD g = ftl_gblock(p, b);

            a->blocks[g].erase_count = 100U; /* 先与 used SB 持平，差值 0 */
            alc_free_push(a, g);
        }
    }

    /* Closed SB：低擦除（100）+ 有效 200/256 = 78%。 */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        blocks[p] = p;

        const WORD g = ftl_gblock(p, p);

        a->blocks[g].erase_count = 100U;
        ftl_block_set_valid(&a->blocks[g], 200U);
    }

    const WORD idx = alc_super_closed_add(a, blocks, FTL_TEMP_COLD);

    munit_assert_true(idx < a->closed_sb_count);
    munit_assert_false(wl_should_trigger(a, WL_MIN_WRITES));

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD b = 10U; b <= 11U; b++)
        {
            a->blocks[ftl_gblock(p, b)].erase_count = 100U + WL_THRESHOLD;
        }
    }

    munit_assert_true(wl_should_trigger(a, WL_MIN_WRITES));

    memset(&victim, 0, sizeof(victim));
    munit_assert_true(wl_select_victim(a, &victim));

    ftl_ut_trace_summary("wl: trigger at delta=%u, victim valid=%hu", WL_THRESHOLD, victim.valid_cw);

    return MUNIT_OK;
}

static MunitResult test_rr_health(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    rr_ecc_track_t track;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

    memset(&track, 0, sizeof(track));

    munit_assert_int(rr_get_health(&track), ==, RR_BLOCK_HEALTHY);

    /* 连续上升 → WARNING。 */
    for (BYTE i = 1U; i <= RR_ECC_RISING_THRESHOLD; i++)
    {
        rr_record_ecc(&track, i);
    }
    munit_assert_int(rr_get_health(&track), ==, RR_BLOCK_WARNING);

    /* 高水位 → CRITICAL；再高 → DYING。 */
    rr_record_ecc(&track, RR_ECC_HIGH_WATER);
    munit_assert_int(rr_get_health(&track), ==, RR_BLOCK_CRITICAL);

    rr_record_ecc(&track, RR_ECC_MAX);
    munit_assert_int(rr_get_health(&track), ==, RR_BLOCK_DYING);

    /* 单调：低 ECC 不回落。 */
    rr_record_ecc(&track, 0U);
    munit_assert_int(rr_get_health(&track), ==, RR_BLOCK_DYING);

    ftl_ut_trace_summary("rr: health=%d last_ecc=%hhu", rr_get_health(&track), track.last_ecc);

    return MUNIT_OK;
}

MunitTest g_ftl_pure_io_tests[] = {
    {         (char *)"policy_entry",          test_policy_entry, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"gc_trigger",            test_gc_trigger, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"gc_select_victim",      test_gc_select_victim, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"wl_trigger_and_victim", test_wl_trigger_and_victim, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {            (char *)"rr_health",             test_rr_health, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                           NULL,                       NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
