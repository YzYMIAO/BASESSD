/*
 * test_ftl_pure.c — L0 /ftl/pure/：不碰 NAND 的模块内部功能测试
 *
 * 资源：ftl_ut_pure_setup() 每个用例前重建测试自有 RAM 表；
 *       ftl_ut_pure_teardown() 检查 live 实例池/组/ctx/FC 归零。
 * 子套件仍按 mem/md/host/io 分组（同文件、四个 MunitTest[]）。
 */

#include "bbt.h"
#include "bg_scheduler.h"
#include "bst.h"
#include "checkpoint.h"
#include "fc_interface.h"
#include "ftl_config.h"
#include "ftl_op.h"
#include "ftl_ut_common.h"
#include "gc.h"
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "metadata_job.h"
#include "munit.h"
#include "pending.h"
#include "read_recovery.h"
#include "wbuf.h"
#include "wear_leveling.h"

#include <string.h>

/* 把每个 plane 的前 n 个块放进 Free Pool（测试自有表，plane 内块号 0..n-1）。 */
/* ── 参数表（与 FC UT 同样的 MunitParameterEnum 形式）── */
static const char        *s_gc_mode_values[] = {"off", "idle", "emergency", NULL};
static MunitParameterEnum s_gc_mode_params[] = {
    {(char *)"mode", s_gc_mode_values},
    {          NULL,             NULL}
};

static const char        *s_alloc_mode_values[] = {"hot", "cold", "high_wear", NULL};
static MunitParameterEnum s_alloc_mode_params[] = {
    {(char *)"mode", s_alloc_mode_values},
    {          NULL,                NULL}
};

static const char        *s_wbuf_n_values[] = {"1", "2", "4", "8", NULL};
static MunitParameterEnum s_wbuf_n_params[] = {
    {(char *)"n", s_wbuf_n_values},
    {       NULL,            NULL}
};

static const char        *s_bbt_plane_values[] = {"0", "1", NULL};
static MunitParameterEnum s_bbt_plane_params[] = {
    {(char *)"plane", s_bbt_plane_values},
    {           NULL,               NULL}
};

static WORD             s_cp_words[FTL_BLOCK_TOTAL];
static l2p_cache_line_t s_l2p_line_saved;
static DWORD            s_l2p_bitmap_saved[L2P_LOCAL_DIRTY_WORDS];
static BYTE             s_cp_page[PAGE_MAIN_SIZE];

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

    ftl_ut_trace_detail("addr: in lba=make(page_base=1236,cw=1) pba=make(plane=1,block=7,page=3,cw=2); out page_base=%lu cw=%hhu plane=%hhu pba.plane=%hhu pba.block=%hu pba.page=%hhu pba.cw=%hhu gblock(1,7)=%hu", ftl_lba_page_base(lba), ftl_lba_cw(lba), ftl_lba_plane(lba), pba_to_plane(pba), pba_to_block(pba), pba_to_page(pba), pba_to_cw(pba), ftl_gblock(1U, 7U));
    ftl_ut_trace_summary("addr: lba 0x%08lX -> page_base=%lu cw=%hhu plane=%hhu | pba 0x%08lX -> gblock=%hu", lba, ftl_lba_page_base(lba), ftl_lba_cw(lba), ftl_lba_plane(lba), pba, ftl_gblock(1U, 7U));

    return MUNIT_OK;
}

static MunitResult test_free_pool_invariants(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

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

    ftl_ut_trace_detail("free pool: fill=%u blocks; remove g=%hu; refill from op1 -> g=%hu; op1=%hu; removed block state=%d", (unsigned)FTL_UT_BLOCKS, ftl_gblock(0U, 1U), refilled, a->op_count[1], (int)ftl_block_state(&a->blocks[ftl_gblock(0U, 1U)]));
    ftl_ut_trace_summary("free pool: 0 -> %u (fill) -> %u (remove) -> %hu (refill g%hu)", (unsigned)FTL_UT_BLOCKS, (unsigned)(FTL_UT_BLOCKS - 1U), a->free_count, refilled);

    return MUNIT_OK;
}

static MunitResult test_super_block_open_close(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_t *a = ftl_ut_ram_alloc();

    mem_fill_free(a, 2U);

    const char            *mode_s = munit_parameters_get(params, "mode");
    alc_super_alloc_mode_e mode   = ALC_SUPER_ALLOC_HOT;
    BYTE                   temp   = FTL_TEMP_HOT;

    if (strcmp(mode_s, "cold") == 0)
    {
        mode = ALC_SUPER_ALLOC_COLD;
        temp = FTL_TEMP_COLD;
    }
    else if (strcmp(mode_s, "high_wear") == 0)
    {
        mode = ALC_SUPER_ALLOC_COLD_HIGH_WEAR;
        temp = FTL_TEMP_COLD;
    }

    DWORD out[PLANE_NUM];

    munit_assert_int(alc_super_open_alloc_pair(a, mode, out), ==, FTL_OK);

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        munit_assert_true(pba_is_valid(out[p]));
        munit_assert_uint32(pba_to_plane(out[p]), ==, p);
        munit_assert_uint32(ftl_block_state(&a->blocks[ftl_gblock(p, pba_to_block(out[p]))]), ==, FTL_BLOCK_STATE_OPEN);
    }

    /* rewind：紧邻一次取页对且未编程 ⇒ 再取应得到同一页，不烧块。 */
    alc_super_open_rewind_pair(a, temp);

    DWORD again[PLANE_NUM];

    munit_assert_int(alc_super_open_alloc_pair(a, mode, again), ==, FTL_OK);
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        munit_assert_uint32(again[p], ==, out[p]);
    }

    munit_assert_int(alc_super_close(a, temp), ==, FTL_OK);
    munit_assert_uint32(a->closed_sb_count, ==, 1U);

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        const WORD g = ftl_gblock(p, pba_to_block(out[p]));

        munit_assert_uint32(ftl_block_state(&a->blocks[g]), ==, FTL_BLOCK_STATE_CLOSED);
        munit_assert_uint32(ftl_block_sb_id(&a->blocks[g]), ==, 0U);
    }

    alc_super_closed_remove(a, 0U);
    munit_assert_uint32(a->closed_sb_count, ==, 0U);

    ftl_ut_trace_detail("super: mode=%s temp=%d pair=(0x%08lX,0x%08lX) rewind=(0x%08lX,0x%08lX) closed 0->1->%hu free=%hu", mode_s, (int)temp, out[0], out[1], again[0], again[1], a->closed_sb_count, a->free_count);
    ftl_ut_trace_summary("super[%s]: pair=(0x%08lX,0x%08lX) rewind=identical closed 0->1->0 free=%hu", mode_s, out[0], out[1], a->free_count);

    return MUNIT_OK;
}

static MunitResult test_closed_list_add_drop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

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

    ftl_ut_trace_detail("closed list: add blocks=(%hu,%hu) -> idx=%hu; drop plane0 -> count=%hu; drop plane1 -> count=%hu", blocks[0], blocks[1], idx, (WORD)1U, a->closed_sb_count);
    ftl_ut_trace_summary("closed list: add (blocks %hu/%hu) -> idx=%hu; drop plane0 keeps 1; drop plane1 -> 0", blocks[0], blocks[1], idx);

    return MUNIT_OK;
}

static MunitResult test_valid_count_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

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

    ftl_ut_trace_detail("valid guard: add x2 -> 2; sub_if_live -> 1; state -> BAD=%d; sub_if_live x2 -> %hu (no uint16 wrap)", (int)ftl_block_state(b), b->valid_count);
    ftl_ut_trace_summary("valid guard: 0 -> 2 (add x2) -> 1 (sub) -> %hu (BAD sub x2 no wrap)", b->valid_count);

    return MUNIT_OK;
}

static MunitResult test_op_spare_pools(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

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

    ftl_ut_trace_detail("op/spare: op0=%hu/%u op1=%hu/%u; spare1 push g=%hu pop g=%hu -> count=%hu; pop on empty=%d", alc_op_get_count(a, 0U), (unsigned)FTL_OP_BLOCKS_PER_PLANE, alc_op_get_count(a, 1U), (unsigned)FTL_OP_BLOCKS_PER_PLANE, g, out, alc_spare_get_count(a, 1U), 0);
    ftl_ut_trace_summary("op/spare: op0 0->%hu (cap %u) op1 0->%hu; spare1 push/pop g%hu -> %hu", alc_op_get_count(a, 0U), (unsigned)FTL_OP_BLOCKS_PER_PLANE, alc_op_get_count(a, 1U), g, alc_spare_get_count(a, 1U));

    return MUNIT_OK;
}

static MunitResult test_erase_stats(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

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

    ftl_ut_trace_detail("erase: valid 1->%hu read 7->%hu state OPEN->%d erase 0->%lu ts=%lu", b->valid_count, b->read_count, (int)ftl_block_state(b), b->erase_count, b->last_pe_ts);
    ftl_ut_trace_summary("erase: valid 1->%hu read 7->%hu erase 0->%lu ts=%lu", b->valid_count, b->read_count, b->erase_count, b->last_pe_ts);

    return MUNIT_OK;
}

static MunitResult test_bbt_mark_bad(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_t *a = ftl_ut_ram_alloc();

    const BYTE plane = (BYTE)(munit_parameters_get(params, "plane")[0] - '0');
    const WORD g     = ftl_gblock(plane, 200U);
    const WORD spare = ftl_gblock(plane, 150U);

    alc_free_push(a, g);
    munit_assert_true(alc_spare_push(a, plane, spare));

    const DWORD gen0 = a->bbt.dirty_gen;

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);

    munit_assert_true(ftl_block_is_bad(&a->blocks[g]));
    munit_assert_uint32(a->blocks[g].valid_count, ==, 0U);
    munit_assert_uint32(ftl_block_sb_id(&a->blocks[g]), ==, FTL_BLOCK_NO_SB);
    munit_assert_uint32(a->bad_count, ==, 1U);
    munit_assert_uint32(a->bbt.dirty_gen, ==, gen0 + 1U);

    /* Spare 第一道替换：弹出同 plane 块补 Free Pool。 */
    munit_assert_uint32(alc_spare_get_count(a, plane), ==, 0U);
    munit_assert_uint32(a->free_count, ==, 1U);
    munit_assert_uint32(a->free_blocks[0], ==, spare);
    munit_assert_uint32(ftl_block_state(&a->blocks[spare]), ==, FTL_BLOCK_STATE_FREE);

    /* 幂等：重复标坏不再计数/弹 Spare/推进 dirty_gen。 */
    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);
    munit_assert_uint32(a->bad_count, ==, 1U);
    munit_assert_uint32(a->bbt.dirty_gen, ==, gen0 + 1U);
    munit_assert_uint32(alc_spare_get_count(a, 0U), ==, 0U);
    munit_assert_uint32(a->free_count, ==, 1U);

    ftl_ut_trace_detail("bbt[plane=%hhu]: g=%hu spare=%hu; bad 0->%hu dirty_gen %lu->%lu; spare %u->%hu; free 0->%hu; idempotent re-mark keeps bad=%hu gen=%lu", plane, g, spare, a->bad_count, gen0, a->bbt.dirty_gen, 1U, alc_spare_get_count(a, plane), a->free_count, a->bad_count, a->bbt.dirty_gen);
    ftl_ut_trace_summary("bbt[plane=%hhu]: bad 0->%hu, dirty_gen %lu->%lu, spare 1->%hu, free 0->%hu (re-mark idempotent)", plane, a->bad_count, gen0, a->bbt.dirty_gen, alc_spare_get_count(a, plane), a->free_count);

    return MUNIT_OK;
}

static MunitResult test_bbt_metadata_bad(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_t *a = ftl_ut_ram_alloc();

    const WORD g = ftl_gblock(0U, JOURNAL_BLOCK0);

    ftl_block_set_state(&a->blocks[g], FTL_BLOCK_STATE_CLOSED);
    a->pools->dirty         = false;
    a->bbt.rebuild_required = false;

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);

    /* 元数据日志块丢失 ⇒ 候选池重算 + 下次开机 full rebuild；不弹用户 Spare。 */
    munit_assert_true(a->pools->dirty);
    munit_assert_true(a->bbt.rebuild_required);
    munit_assert_uint32(alc_spare_get_count(a, 0U), ==, 0U);
    munit_assert_uint32(a->bad_count, ==, 1U);

    ftl_ut_trace_detail("bbt metadata bad: g=%hu state JOURNAL; pools.dirty false->%d rebuild_required false->%d bad 0->%hu spare0=%hu", g, (int)a->pools->dirty, (int)a->bbt.rebuild_required, a->bad_count, alc_spare_get_count(a, 0U));
    ftl_ut_trace_summary("bbt metadata bad: g=%hu -> pools.dirty=%d rebuild_required=%d bad=0->%hu", g, (int)a->pools->dirty, (int)a->bbt.rebuild_required, a->bad_count);

    return MUNIT_OK;
}

static MunitResult test_cw_masks(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    const DWORD bit00 = ftl_cw_bit(0U, 0U);
    const DWORD bit12 = ftl_cw_bit(1U, 2U);

    munit_assert_uint32(bit00, ==, 1U);
    munit_assert_uint32(bit12, ==, (1UL << (CW_NUM + 2U)));

    const DWORD span = ftl_cw_span_mask(1U, 2U);

    munit_assert_uint32(span, ==, ((1UL << 1U) | (1UL << 2U)));

    const DWORD plane_mask1 = ftl_cw_plane_mask(1U);

    munit_assert_uint32(plane_mask1, ==, (ftl_cw_span_mask(0U, CW_NUM - 1U) << (1U * CW_NUM)));

    const DWORD bm = bit00 | bit12;

    munit_assert_uint32(ftl_cw_bitmap_popcount(bm), ==, 2U);
    munit_assert_uint32(ftl_cw_bitmap_plane_count(bm), ==, 2U);
    munit_assert_uint32(NAND_FIELD_MASK(4U), ==, 0x0FU);
    munit_assert_uint32(NAND_FIELD_MASK(9U), ==, 0x01FFU);

    ftl_ut_trace_detail("cw masks: bit(0,0)=0x%lX bit(1,2)=0x%lX span(1,2)=0x%lX plane1=0x%lX popcount(bm)=%hhu planes(bm)=%hhu fieldmask(4)=0x%lX fieldmask(9)=0x%lX", bit00, bit12, span, plane_mask1, ftl_cw_bitmap_popcount(bm), ftl_cw_bitmap_plane_count(bm), NAND_FIELD_MASK(4U), NAND_FIELD_MASK(9U));
    ftl_ut_trace_summary("cw masks: span=0x%lX plane1=0x%lX -> popcount=%hhu planes=%hhu", span, plane_mask1, ftl_cw_bitmap_popcount(bm), ftl_cw_bitmap_plane_count(bm));

    return MUNIT_OK;
}

static MunitResult test_alloc_spare_full_refill_none(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_t     *a     = ftl_ut_ram_alloc();
    const BYTE plane = 0U;

    for (WORD i = 0U; i < (WORD)FTL_SPARE_POOL_PER_PLANE; i++)
    {
        munit_assert_true(alc_spare_push(a, plane, ftl_gblock(plane, i)));
    }
    munit_assert_uint32(alc_spare_get_count(a, plane), ==, (DWORD)FTL_SPARE_POOL_PER_PLANE);
    munit_assert_false(alc_spare_push(a, plane, ftl_gblock(plane, (WORD)FTL_SPARE_POOL_PER_PLANE)));

    /* 清空 Spare 后 refill：Free Pool 也为空 ⇒ 无候选可补，返回 false。 */
    for (WORD i = 0U; i < (WORD)FTL_SPARE_POOL_PER_PLANE; i++)
    {
        WORD out = 0U;

        munit_assert_true(alc_spare_pop(a, plane, &out));
    }
    munit_assert_uint32(alc_spare_get_count(a, plane), ==, 0U);
    munit_assert_false(alc_spare_refill_from_free(a, plane));
    munit_assert_uint32(alc_spare_get_count(a, plane), ==, 0U);

    ftl_ut_trace_detail("spare: cap=%u; push on full=false; after pop-all count=%hu; refill_from_free(empty)=0", (unsigned)FTL_SPARE_POOL_PER_PLANE, alc_spare_get_count(a, plane));
    ftl_ut_trace_summary("spare: 0->%u (full) -> 0 (pop) ; refill with empty free=false", (unsigned)FTL_SPARE_POOL_PER_PLANE);

    return MUNIT_OK;
}

MunitTest g_ftl_pure_mem_tests[] = {
    {           (char *)"address_roundtrip",            test_address_roundtrip, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {        (char *)"free_pool_invariants",         test_free_pool_invariants, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {      (char *)"super_block_open_close",       test_super_block_open_close, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, s_alloc_mode_params},
    {        (char *)"closed_list_add_drop",         test_closed_list_add_drop, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {           (char *)"valid_count_guard",            test_valid_count_guard, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {              (char *)"op_spare_pools",               test_op_spare_pools, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {                    (char *)"cw_masks",                     test_cw_masks, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {                 (char *)"erase_stats",                  test_erase_stats, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {                (char *)"bbt_mark_bad",                 test_bbt_mark_bad, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,  s_bbt_plane_params},
    {            (char *)"bbt_metadata_bad",             test_bbt_metadata_bad, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {(char *)"alloc_spare_full_refill_none", test_alloc_spare_full_refill_none, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                NULL},
    {                                  NULL,                              NULL,              NULL,                 NULL,             MUNIT_TEST_OPTION_NONE,                NULL},
};

static MunitResult test_metadata_idle(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t *ftl = ftl_ut_live();

    ftl_ut_trace_begin();

    munit_assert_not_null(ftl->metadata_dir);
    munit_assert_true(md_handle_is_idle(ftl->metadata_dir));

    ftl_ut_trace_detail("metadata: handle idle=%d; live_blocks=%hu", (int)md_handle_is_idle(ftl->metadata_dir), md_live_block_count_get(ftl->metadata_dir));
    ftl_ut_trace_summary("metadata: idle=%d live_blocks=%hu (no live block expected at boot)", (int)md_handle_is_idle(ftl->metadata_dir), md_live_block_count_get(ftl->metadata_dir));

    return MUNIT_OK;
}

static MunitResult test_ring_geometry(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_ring_t ring;

    ftl_ut_trace_begin();
    memset(&ring, 0, sizeof(ring));
    ring.first_block      = 3U;
    ring.nominal_count    = 4U;
    ring.block_count      = 4U;
    ring.blocks[0]        = 3U;
    ring.blocks[1]        = 4U;
    ring.blocks[2]        = 5U;
    ring.blocks[3]        = 6U;
    ring.pages_per_record = 1U;
    ring.plane_mask       = 0x03U;

    munit_assert_uint32(md_ring_records_per_block(&ring), ==, (DWORD)PAGES_PER_BLOCK);
    munit_assert_uint32(md_ring_total_records(&ring), ==, (DWORD)(4U * PAGES_PER_BLOCK));
    munit_assert_true(md_ring_has_block(&ring, 5U));
    munit_assert_false(md_ring_has_block(&ring, 7U));
    munit_assert_uint32(md_ring_plane_count(ring.plane_mask), ==, (DWORD)PLANE_NUM);
    munit_assert_uint32(md_ring_plane_at(ring.plane_mask, 0U), ==, 0U);
    munit_assert_uint32(md_ring_plane_at(ring.plane_mask, 1U), ==, 1U);
    munit_assert_uint32(md_ring_plane_at(ring.plane_mask, (BYTE)PLANE_NUM), ==, (DWORD)PLANE_NUM);

    ftl_ut_trace_detail("ring: 4 blocks, 1 page/record, mask=0x%02hhX -> records/block=%hu total=%hu has(5)=%d has(7)=%d planes=%hhu at(0)=%hhu at(1)=%hhu at(NUM)=%hhu", ring.plane_mask, md_ring_records_per_block(&ring), md_ring_total_records(&ring), (int)md_ring_has_block(&ring, 5U), (int)md_ring_has_block(&ring, 7U), md_ring_plane_count(ring.plane_mask), md_ring_plane_at(ring.plane_mask, 0U), md_ring_plane_at(ring.plane_mask, 1U), md_ring_plane_at(ring.plane_mask, (BYTE)PLANE_NUM));
    ftl_ut_trace_summary("ring: 4x1 -> records/block=%hu total=%hu; mask 0x%02hhX -> planes=%hhu", md_ring_records_per_block(&ring), md_ring_total_records(&ring), ring.plane_mask, md_ring_plane_count(ring.plane_mask));

    return MUNIT_OK;
}

static MunitResult test_directory_commit(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    md_dir_cache_t *dir = ftl_ut_ram_md();

    munit_assert_uint32(md_entry_find(dir, METADATA_TYPE_L2P_REGION, 0U), ==, NAND_PBA_INVALID);

    const DWORD p0 = pba_make(0U, 0U, 0U, L2P_LOG_BLOCK0, 0U, 0U, 0U);
    const DWORD p1 = pba_make(0U, 0U, 0U, (WORD)(L2P_LOG_BLOCK0 + 1U), 0U, 0U, 0U);

    md_commit_region(dir, 0U, MD_COMMIT_PRIMARY, p0, NAND_PBA_INVALID);

    munit_assert_uint32(md_entry_find(dir, METADATA_TYPE_L2P_REGION, 0U), ==, p0);
    munit_assert_uint32(md_live_count_get(dir, ftl_gblock(0U, L2P_LOG_BLOCK0)), ==, 1U);

    /* 覆盖写：旧位置 -1、新位置 +1。 */
    md_commit_region(dir, 0U, MD_COMMIT_PRIMARY, p1, NAND_PBA_INVALID);

    munit_assert_uint32(md_entry_find(dir, METADATA_TYPE_L2P_REGION, 0U), ==, p1);
    munit_assert_uint32(md_live_count_get(dir, ftl_gblock(0U, L2P_LOG_BLOCK0)), ==, 0U);
    munit_assert_uint32(md_live_count_get(dir, ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 1U))), ==, 1U);

    /* 主备同批：两份都计入 live，dirty 置位。 */
    const DWORD p2 = pba_make(0U, 0U, 0U, (WORD)(L2P_LOG_BLOCK0 + 2U), 0U, 0U, 0U);
    const DWORD p3 = pba_make(0U, 0U, 0U, (WORD)(L2P_LOG_BLOCK0 + 3U), 0U, 0U, 0U);

    md_commit_region(dir, 1U, MD_COMMIT_PAIR, p2, p3);

    munit_assert_uint32(md_entry_find(dir, METADATA_TYPE_L2P_REGION, 1U), ==, p2);
    munit_assert_uint32(md_backup_find(dir, 1U), ==, p3);
    munit_assert_uint32(md_live_count_get(dir, ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 2U))), ==, 1U);
    munit_assert_uint32(md_live_count_get(dir, ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 3U))), ==, 1U);
    munit_assert_true(dir->dirty);

    ftl_ut_trace_detail("directory: r0 primary 0x%08lX -> 0x%08lX; r1 pair 0x%08lX/0x%08lX; live log0=%hu log1=%hu log2=%hu log3=%hu dirty=%d gen=%lu", p0, p1, p2, p3, md_live_count_get(dir, ftl_gblock(0U, L2P_LOG_BLOCK0)), md_live_count_get(dir, ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 1U))), md_live_count_get(dir, ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 2U))), md_live_count_get(dir, ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 3U))), (int)dir->dirty, dir->dirty_gen);
    ftl_ut_trace_summary("directory: r0 primary 0x%08lX->0x%08lX; r1 pair 0x%08lX/0x%08lX; dirty=%d gen=%lu", p0, p1, p2, p3, (int)dir->dirty, dir->dirty_gen);

    return MUNIT_OK;
}

static MunitResult test_journal_append(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_jrn_t journal;

    ftl_ut_trace_begin();
    memset(&journal, 0, sizeof(journal));

    jrn_init(&journal, ftl_ut_live()->topo, ftl_ut_ram_alloc());

    munit_assert_true(jrn_is_idle(&journal));
    munit_assert_uint32(jrn_sequence_get(&journal), ==, 1U); /* 0 保留作“无序号” */
    munit_assert_uint32(jrn_watermark_get(&journal), ==, 0U);

    munit_assert_true(jrn_reserve(&journal, 1U));

    const DWORD pba = pba_make(0U, 0U, 0U, 5U, 0U, 0U, 0U);

    munit_assert_true(jrn_append(&journal, 42U, NAND_PBA_INVALID, pba));
    munit_assert_uint32(jrn_sequence_get(&journal), ==, 2U);
    munit_assert_true(jrn_has_pending_entries(&journal));
    munit_assert_false(jrn_needs_checkpoint(&journal));

    ftl_ut_trace_detail("journal: init seq=1 wm=0; reserve(lba=42) -> pba=0x%08lX; seq 1->%lu wm=%hhu pending=%d need_cp=%d", pba, jrn_sequence_get(&journal), jrn_watermark_get(&journal), (int)jrn_has_pending_entries(&journal), (int)jrn_needs_checkpoint(&journal));
    ftl_ut_trace_summary("journal: seq 1->%lu watermark=%hhu pending=%d need_cp=%d", jrn_sequence_get(&journal), jrn_watermark_get(&journal), (int)jrn_has_pending_entries(&journal), (int)jrn_needs_checkpoint(&journal));

    return MUNIT_OK;
}

static MunitResult test_cp_encoders(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_t          *a = ftl_ut_ram_alloc();
    md_dir_cache_t *d = ftl_ut_ram_md();

    const WORD word = cp_block_word_pack((WORD)CP_BLOCK_WORD_MAX_VALID, (BYTE)FTL_BLOCK_STATE_BAD, 1U, CP_BLOCK_POOL_SPARE);

    munit_assert_uint32(cp_block_word_valid(word), ==, (DWORD)CP_BLOCK_WORD_MAX_VALID);
    munit_assert_uint32(cp_block_word_state(word), ==, (DWORD)FTL_BLOCK_STATE_BAD);
    munit_assert_uint32(cp_block_word_temp(word), ==, 1U);
    munit_assert_uint32(cp_block_word_pool(word), ==, (DWORD)CP_BLOCK_POOL_SPARE);

    /* 快照：CLOSED/有效计数/温度 + OP/Spare 池标签 + Directory 权威 LOG。 */
    const WORD g0          = ftl_gblock(0U, 0U);
    const WORD g_op        = ftl_gblock(0U, 50U);
    const WORD g_sp        = ftl_gblock(0U, 60U);
    const WORD g_log_block = (WORD)(L2P_LOG_BLOCK0 + 5U);
    const WORD g_log       = ftl_gblock(0U, g_log_block);

    ftl_block_set_state(&a->blocks[g0], FTL_BLOCK_STATE_CLOSED);
    ftl_block_set_valid(&a->blocks[g0], 100U);
    ftl_block_set_temp(&a->blocks[g0], FTL_TEMP_COLD);
    munit_assert_true(alc_op_push(a, 0U, g_op));
    munit_assert_true(alc_spare_push(a, 0U, g_sp));

    const DWORD pba = pba_make(0U, 0U, 0U, g_log_block, 0U, 0U, 0U);

    md_commit_region(d, 2U, MD_COMMIT_PRIMARY, pba, NAND_PBA_INVALID);

    cp_block_snapshot(a, d, s_cp_words);

    munit_assert_uint32(cp_block_word_state(s_cp_words[g0]), ==, (DWORD)FTL_BLOCK_STATE_CLOSED);
    munit_assert_uint32(cp_block_word_valid(s_cp_words[g0]), ==, 100U);
    munit_assert_uint32(cp_block_word_temp(s_cp_words[g0]), ==, (DWORD)FTL_TEMP_COLD);
    munit_assert_uint32(cp_block_word_pool(s_cp_words[g_op]), ==, (DWORD)CP_BLOCK_POOL_OP);
    munit_assert_uint32(cp_block_word_pool(s_cp_words[g_sp]), ==, (DWORD)CP_BLOCK_POOL_SPARE);
    munit_assert_uint32(cp_block_word_state(s_cp_words[g_log]), ==, (DWORD)FTL_BLOCK_STATE_LOG);
    munit_assert_uint32(cp_block_word_pool(s_cp_words[g_log]), ==, (DWORD)CP_BLOCK_POOL_NONE);

    /* 磨损页：erase/read/last_ecc/flags 的字节布局。 */
    a->blocks[g0].erase_count     = 7U;
    a->blocks[g0].read_count      = 3U;
    a->blocks[g0].rr.last_ecc     = 2U;
    a->blocks[g0].rr.rising_count = 5U;
    a->blocks[g0].rr.health       = RR_BLOCK_DYING;

    memset(s_cp_page, 0, sizeof(s_cp_page));
    cp_wear_page_build(a, 0U, s_cp_page);

    DWORD erase = 0U;
    WORD  read  = 0U;

    memcpy(&erase, &s_cp_page[0], sizeof(erase));
    memcpy(&read, &s_cp_page[4], sizeof(read));

    munit_assert_uint32(erase, ==, 7U);
    munit_assert_uint32(read, ==, 3U);
    munit_assert_uint32(s_cp_page[6], ==, 2U);
    munit_assert_uint32(s_cp_page[7], ==, (5U | ((DWORD)RR_BLOCK_DYING << 4)));

    ftl_ut_trace_detail("cp: word pack -> valid=%hu state=%hhu temp=%hhu pool=%hhu; snapshot g%hu CLOSED/100/COLD; op=%hhu spare=%hhu log_state=%hhu; wear erase=%lu read=%hu ecc=%u flags=0x%02X", cp_block_word_valid(word), cp_block_word_state(word), cp_block_word_temp(word), cp_block_word_pool(word), g0, cp_block_word_pool(s_cp_words[g_op]), cp_block_word_pool(s_cp_words[g_sp]), cp_block_word_state(s_cp_words[g_log]), erase, read, (unsigned)s_cp_page[6], (unsigned)s_cp_page[7]);
    ftl_ut_trace_summary("cp: encoders -> word valid=%hu state=%hhu temp=%hhu pool=%hhu; snapshot/wear bytes ok", cp_block_word_valid(word), cp_block_word_state(word), cp_block_word_temp(word), cp_block_word_pool(word));

    return MUNIT_OK;
}

static ftl_status_e ring_probe(void *ctx, WORD record_idx, bool full, DWORD *out_sequence)
{
    (void)full;

    const WORD written = *(const WORD *)ctx;

    if (record_idx >= written)
    {
        return FTL_ERROR;
    }
    *out_sequence = (DWORD)record_idx + 1U;
    return FTL_OK;
}

static MunitResult test_ring_locate(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_ring_t            ring;
    md_ring_locate_out_t out;
    WORD                 written;

    ftl_ut_trace_begin();
    memset(&ring, 0, sizeof(ring));
    ring.block_count      = 2U;
    ring.blocks[0]        = 0U;
    ring.blocks[1]        = 1U;
    ring.pages_per_record = 1U;
    ring.plane_mask       = 0x03U;

    written = 5U;
    memset(&out, 0, sizeof(out));
    munit_assert_int(md_ring_locate(&ring, ring_probe, &written, &out), ==, FTL_OK);
    munit_assert_uint32(out.newest_idx, ==, 4U);
    munit_assert_uint32(out.sequence, ==, 5U);
    munit_assert_uint32(out.write_idx, ==, 5U);

    written = 0U;
    memset(&out, 0, sizeof(out));
    munit_assert_int(md_ring_locate(&ring, ring_probe, &written, &out), ==, FTL_OK);
    munit_assert_uint32(out.newest_idx, ==, UINT16_MAX);
    munit_assert_uint32(out.sequence, ==, 0U);
    munit_assert_uint32(out.write_idx, ==, 0U);

    ftl_ut_trace_detail("ring locate: blocks=%hu pages/record=%hu mask=0x%02hhX; written=5 -> idx=4 seq=5 write_idx=5; written=0 -> idx=%u seq=%lu write_idx=%u", ring.block_count, ring.pages_per_record, ring.plane_mask, (unsigned)out.newest_idx, out.sequence, (unsigned)out.write_idx);
    ftl_ut_trace_summary("ring locate: written 5 -> idx=4 seq=5; written 0 -> idx=UINT16_MAX seq=0");

    return MUNIT_OK;
}

static MunitResult test_anchor_three_step(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_anchor_prepare_out_t prep;
    md_anchor_page_t        page;
    static md_anchor_page_t s_anchor_page;

    ftl_ut_trace_begin();

    alc_t          *a = ftl_ut_ram_alloc();
    md_dir_cache_t *d = ftl_ut_ram_md();

    munit_assert_true(md_pool_build_all(a));

    d->dirty               = true;
    d->anchor_cursor_valid = false;

    memset(&prep, 0, sizeof(prep));

    /* 冷启动游标会同步扫介质；纯 RAM 测试先给一个有效游标，跳过 locate。 */
    d->anchor_cursor_valid = true;
    d->anchor_write_idx    = 0U;
    d->anchor_sequence     = 0U;

    munit_assert_int(md_anchor_prepare(d, &prep), ==, MD_ANCHOR_READY);
    munit_assert_uint32(prep.seq, ==, 1U);
    munit_assert_uint32(prep.idx, ==, 0U);

    memset(&s_anchor_page, 0, sizeof(s_anchor_page));
    md_anchor_page(d, prep.seq, 0U, &s_anchor_page);

    munit_assert_uint32(s_anchor_page.magic, ==, (DWORD)ANCHOR_MAGIC);
    munit_assert_uint32(s_anchor_page.sequence, ==, prep.seq);
    munit_assert_uint32(s_anchor_page.chunk_index, ==, 0U);
    munit_assert_uint32(s_anchor_page.chunk_count, ==, (DWORD)ANCHOR_PAGE_COUNT);

    md_anchor_commit(d, &prep.ring, prep.idx, true);

    munit_assert_false(d->dirty);
    munit_assert_uint32(d->anchor_sequence, ==, prep.seq);

    (void)page;
    ftl_ut_trace_detail("anchor: dirty 1->%d; prepare seq=%lu idx=%hu; page magic=0x%08lX seq=%lu chunk=%hu/%hu; commit anchor_sequence=%lu", (int)d->dirty, prep.seq, prep.idx, s_anchor_page.magic, s_anchor_page.sequence, s_anchor_page.chunk_index, s_anchor_page.chunk_count, d->anchor_sequence);
    ftl_ut_trace_summary("anchor: dirty 1->%d seq 0->%lu idx=%hu chunks=%hu", (int)d->dirty, prep.seq, prep.idx, s_anchor_page.chunk_count);

    return MUNIT_OK;
}

static MunitResult test_log_alloc_rotate_release(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_t          *a = ftl_ut_ram_alloc();
    md_dir_cache_t *d = ftl_ut_ram_md();

    const WORD g0 = md_log_alloc(d, a, 0U);

    munit_assert_uint32(g0, ==, ftl_gblock(0U, L2P_LOG_BLOCK0));
    munit_assert_uint32(ftl_block_state(&a->blocks[g0]), ==, FTL_BLOCK_STATE_LOG);

    const WORD g1 = md_log_alloc(d, a, 0U);

    munit_assert_uint32(g1, ==, ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 1U)));

    md_log_release(d, g0);
    munit_assert_uint32(ftl_block_state(&a->blocks[g0]), ==, FTL_BLOCK_STATE_NONE);

    munit_assert_true(md_log_rotate(d, a));
    munit_assert_true(d->cur_block_valid);
    munit_assert_uint32(d->cur_metadata_page, ==, 0U);

    ftl_ut_trace_detail("md log: alloc g%hu -> g%hu; release g%hu -> state=%d; rotate -> cur=g%hu page=%hhu valid=%d", g0, g1, g0, (int)ftl_block_state(&a->blocks[g0]), d->cur_metadata_block, d->cur_metadata_page, (int)d->cur_block_valid);
    ftl_ut_trace_summary("md log: alloc g%hu->g%hu; release g%hu->NONE; rotate cur=g%hu page=%hhu", g0, g1, g0, d->cur_metadata_block, d->cur_metadata_page);

    return MUNIT_OK;
}

static MunitResult test_cp_init_timer(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    cp_handle_t h;

    ftl_ut_trace_begin();

    alc_t          *a = ftl_ut_ram_alloc();
    md_dir_cache_t *d = ftl_ut_ram_md();
    ftl_ctx_t      *f = ftl_ut_live();

    memset(&h, 0, sizeof(h));
    cp_init(&h, f->cache, f->journal, d, a);

    munit_assert_false(h.active);
    munit_assert_false(h.waiting);
    munit_assert_int(h.phase, ==, CP_IDLE);

    /* 定时器不是“置为当前时刻”，而是清 0；下个 job 起点由 cp_handle 写入。 */
    h.last_ms = 1234U;
    cp_timer_reset(&h);

    munit_assert_uint32(h.last_ms, ==, 0U);

    ftl_ut_trace_detail("cp init: active=%d waiting=%d phase=%d last_ms 1234->%lu", (int)h.active, (int)h.waiting, (int)h.phase, h.last_ms);
    ftl_ut_trace_summary("cp init: phase=%d active=%d waiting=%d; timer last_ms 1234->%lu", (int)h.phase, (int)h.active, (int)h.waiting, h.last_ms);

    return MUNIT_OK;
}

static MunitResult test_md_status_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_status_t out;
    bg_info_t   info;

    ftl_ut_trace_begin();

    md_dir_cache_t *d = ftl_ut_ram_md();

    d->handle.error = FTL_ERROR;

    memset(&info, 0, sizeof(info));
    memset(&out, 0, sizeof(out));

    md_handle(d, &info, &out);

    /* 当前语义：进入 IDLE 时清 error ⇒ 采样看不到上一次 abort（RISK-MD-17）。 */
    munit_assert_int(out.base.error, ==, FTL_OK);

    ftl_ut_trace_detail("md status: handle.error FTL_ERROR -> sampled base.error=%d (IDLE clears; RISK-MD-17)", (int)out.base.error);
    ftl_ut_trace_summary("md status: handle.error FTL_ERROR -> sampled=%d (IDLE clears)", (int)out.base.error);

    return MUNIT_OK;
}

static MunitResult test_journal_write_fail_clear(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_jrn_t journal;

    ftl_ut_trace_begin();

    memset(&journal, 0, sizeof(journal));
    jrn_init(&journal, ftl_ut_live()->topo, ftl_ut_ram_alloc());

    journal.handle.write_fail_pending = true;
    journal.handle.write_fail_seq     = 5U;

    /* CP 水位落后于失败点：保留 pending。 */
    jrn_write_failure_clear(&journal, 4U);
    munit_assert_true(jrn_has_write_failure(&journal));

    jrn_write_failure_clear(&journal, 5U);
    munit_assert_false(jrn_has_write_failure(&journal));

    munit_assert_false(jrn_needs_checkpoint(&journal));
    jrn_checkpoint_request(&journal);
    munit_assert_true(jrn_needs_checkpoint(&journal));

    ftl_ut_trace_detail("journal: write_fail_seq=5; clear(cp=4) -> pending=%d; clear(cp=5) -> pending=%d; needs_cp false->%d", 1, (int)jrn_has_write_failure(&journal), (int)jrn_needs_checkpoint(&journal));
    ftl_ut_trace_summary("journal: fail_seq=5, clear(4) keeps pending, clear(5) clears; needs_cp false->%d", (int)jrn_needs_checkpoint(&journal));

    return MUNIT_OK;
}

static MunitResult test_md_abort_keeps_victim(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    md_dir_cache_t *d       = ftl_ut_ram_md();
    const WORD      victim  = ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 5U));
    const WORD      pending = d->reclaim_pending_count;

    /* 构造「压缩写阶段取不到空槽」⇒ md_compact_write 返回 FAILED ⇒ md_handle_abort。
     * victim 仍有活 region：abort 只回 IDLE，不得把它登记进回收位图（更不得擦除）。 */
    d->l2p_live[victim] = 3U;
    d->cur_block_valid  = false;

    d->handle.phase  = MD_JOB_CP_WRITE;
    d->handle.victim = victim;
    d->handle.slot   = NAND_PBA_INVALID;
    d->handle.moved  = 7U;

    bg_info_t   info;
    md_status_t out;

    memset(&info, 0, sizeof(info));
    memset(&out, 0, sizeof(out));
    md_handle(d, &info, &out);

    munit_assert_int(d->handle.phase, ==, MD_JOB_IDLE);
    munit_assert_uint32(d->l2p_live[victim], ==, 3U);
    munit_assert_uint32(d->reclaim_pending_count, ==, pending);
    munit_assert_uint32(d->handle.victim, ==, UINT16_MAX);
    munit_assert_uint32(d->handle.moved, ==, 0U);

    ftl_ut_trace_detail("md abort: victim=g%hu live=3; phase CP_WRITE + no slot -> FAILED -> abort; victim=%hu live=%hu pending=%hu moved=%hu", victim, d->handle.victim, d->l2p_live[victim], d->reclaim_pending_count, d->handle.moved);
    ftl_ut_trace_summary("md abort: victim g%hu live 3 kept, pending %hu->%hu, phase IDLE", victim, pending, d->reclaim_pending_count);

    return MUNIT_OK;
}

static MunitResult test_journal_truncate_idempotent(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    l2p_jrn_t journal;
    DWORD     addr  = 0U;
    BYTE      block = JRN_BLOCK_NONE;

    memset(&journal, 0, sizeof(journal));
    jrn_init(&journal, ftl_ut_live()->topo, ftl_ut_ram_alloc());

    /* ① sequence_counter == 0：下溢保护，不截断。 */
    journal.handle.sequence_counter = 0U;
    munit_assert_int(jrn_truncate_plan(&journal, 0U, &addr, &block), ==, JRN_TRUNC_NO_TRUNCATE);

    /* ② 水位不足（counter-1 = 4，up_to = 3）。 */
    journal.handle.sequence_counter = 5U;
    munit_assert_int(jrn_truncate_plan(&journal, 3U, &addr, &block), ==, JRN_TRUNC_NO_TRUNCATE);

    /* ③ 水位足够：选择环形下一块预擦。 */
    munit_assert_int(jrn_truncate_plan(&journal, 5U, &addr, &block), ==, JRN_TRUNC_NEEDS_ERASE);
    munit_assert_uint32(block, !=, JRN_BLOCK_NONE);

    const BYTE  erased = block;
    const DWORD eaddr  = addr;

    journal.handle.checkpoint_needed = true;
    journal.handle.emergency         = true;
    jrn_truncate_commit(&journal, erased);

    munit_assert_uint32(journal.handle.pre_erased_block, ==, erased);
    munit_assert_false(journal.handle.checkpoint_needed);
    munit_assert_false(journal.handle.emergency);

    /* ④ 重复 plan：同一块已预擦 ⇒ ALREADY，不再返回地址。 */
    addr  = 0U;
    block = JRN_BLOCK_NONE;
    munit_assert_int(jrn_truncate_plan(&journal, 5U, &addr, &block), ==, JRN_TRUNC_ALREADY);

    /* ⑤ commit(NONE) 不清 pre_erased（否则同一块被双擦）。 */
    jrn_truncate_commit(&journal, JRN_BLOCK_NONE);
    munit_assert_uint32(journal.handle.pre_erased_block, ==, erased);
    munit_assert_int(jrn_truncate_plan(&journal, 5U, &addr, &block), ==, JRN_TRUNC_ALREADY);

    ftl_ut_trace_detail("truncate: counter=5 up_to=3 -> NO_TRUNCATE; up_to=5 -> NEEDS_ERASE block=%hhu addr=0x%08lX; commit -> ALREADY; commit(NONE) keeps pre_erased=%hhu", erased, eaddr, journal.handle.pre_erased_block);
    ftl_ut_trace_summary("journal truncate: plan/commit idempotent (block=%hhu, pre_erased kept on NONE)", erased);

    return MUNIT_OK;
}

MunitTest g_ftl_pure_md_tests[] = {
    {              (char *)"metadata_idle",               test_metadata_idle, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              (char *)"ring_geometry",               test_ring_geometry, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"directory_commit",            test_directory_commit, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"journal_append",              test_journal_append, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                (char *)"ring_locate",                 test_ring_locate, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"anchor_three_step",           test_anchor_three_step, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"log_alloc_rotate_release",    test_log_alloc_rotate_release, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              (char *)"cp_init_timer",               test_cp_init_timer, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {            (char *)"md_status_error",             test_md_status_error, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"journal_write_fail_clear",    test_journal_write_fail_clear, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"md_abort_keeps_victim",       test_md_abort_keeps_victim, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"journal_truncate_idempotent", test_journal_truncate_idempotent, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                (char *)"cp_encoders",                 test_cp_encoders, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                                 NULL,                             NULL,              NULL,                 NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

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

    ftl_ut_trace_detail("host idle: pending=%hu groups=%hu ctx=%hu wbuf_idle=%d bg_tasks=%hhu", pend_slot_get_count(&ftl->pending), pend_group_get_count(&ftl->pending), ftl_op_ctx_count(), (int)wb_is_idle(ftl->wbuf), ftl->bg.task_count);
    ftl_ut_trace_summary("host: pending=%hu groups=%hu ctx=%hu wbuf_idle=%d bg_tasks=%hhu", pend_slot_get_count(&ftl->pending), pend_group_get_count(&ftl->pending), ftl_op_ctx_count(), (int)wb_is_idle(ftl->wbuf), ftl->bg.task_count);

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

    ftl_ut_trace_detail("pending: alloc 0->%u (full); alloc-on-full=NULL; remove id=0x1234 -> count=%u; realloc ok", (unsigned)FTL_PENDING_MAX, (unsigned)FTL_PENDING_MAX - 1U);
    ftl_ut_trace_summary("pending: alloc 0->%u (full) -> %u (remove) -> %u (realloc)", (unsigned)FTL_PENDING_MAX, (unsigned)FTL_PENDING_MAX - 1U, (unsigned)FTL_PENDING_MAX);

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

    ftl_ut_trace_detail("group: alloc 0->%u (max); extra alloc=false; group[%hhu].total 7->%lu used=%d count=%hu", (unsigned)FTL_GROUP_MAX, gi[0], table.groups[gi[0]].total, (int)table.groups[gi[0]].used, pend_group_get_count(&table));
    ftl_ut_trace_summary("group: alloc %u (full) -> extra=false; group0 total stays %lu", (unsigned)FTL_GROUP_MAX, table.groups[gi[0]].total);

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

    ftl_ut_trace_detail("group: outstanding 2->1->0; done 0->0->%lu; groups 1->%hu; used=%d", s_group_done, pend_group_get_count(&table), (int)g->used);
    ftl_ut_trace_summary("group: outstanding 2->0; done 0->%lu; groups 1->%hu", s_group_done, pend_group_get_count(&table));

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

    /* 稀疏：同 pair 窗口内再写 CW5（lba 5），occupied 是两个离散位。 */
    BYTE src2[FTL_IU_BYTES];

    for (WORD i = 0U; i < FTL_IU_BYTES; i++)
    {
        src2[i] = (BYTE)(0xA5U ^ (i & 0xFFU));
    }
    tr.lba  = 5U;
    tr.data = src2;
    munit_assert_int(wb_handle(&h, &tr, NULL, NULL), ==, FTL_OK);
    munit_assert_uint32(h.occupied, ==, ((1UL << 0U) | (1UL << 5U)));
    munit_assert_true(wb_read(&h, 5U, out));
    munit_assert_memory_equal(FTL_IU_BYTES, src2, out);

    munit_assert_uint32(h.owners, ==, ftl_group_bit(gi));
    munit_assert_uint32(table.groups[gi].outstanding, ==, 2U);
    munit_assert_false(wb_is_idle(&h));

    ftl_ut_trace_detail("wbuf: occupied 0->0x%02hhX (cw0|cw5); owners=0x%08lX; outstanding 1->%hu; cw0/cw5 readback byte-equal", h.occupied, h.owners, table.groups[gi].outstanding);
    ftl_ut_trace_summary("wbuf: occupied 0->0x%02hhX owners=0x%08lX outstanding=%hu", h.occupied, h.owners, table.groups[gi].outstanding);

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

    ftl_ut_trace_detail("op ctx: add(60) -> count=1; take(60) -> count=0; add(61)+remove(61) -> count=%hu", ftl_op_ctx_count());
    ftl_ut_trace_summary("op ctx: count 0->1 (add 60) ->0 (take) ->0 (add 61 + remove)");

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

    ftl_ut_trace_detail("bg: task_count 0->%hhu; level_select(live)=%d; BG_TASK_COUNT=%u", sched.task_count, (int)level, (unsigned)BG_TASK_COUNT);
    ftl_ut_trace_summary("bg: task_count 0->%hhu level=%d", sched.task_count, (int)level);

    return MUNIT_OK;
}

static MunitResult test_op_ctx_capacity(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    int dummy = 0;

    ftl_ut_trace_begin();

    munit_assert_uint32(ftl_op_ctx_count(), ==, 0U);

    for (WORD i = 0U; i < FTL_OP_CTX_TABLE_SIZE; i++)
    {
        munit_assert_int(ftl_op_ctx_add(i, &dummy), ==, FTL_OK);
    }
    munit_assert_uint32(ftl_op_ctx_count(), ==, (DWORD)FTL_OP_CTX_TABLE_SIZE);

    /* 同 cmd_id 覆盖而不是追加：容量仍为 64。 */
    munit_assert_int(ftl_op_ctx_add(0U, &dummy), ==, FTL_OK);
    munit_assert_uint32(ftl_op_ctx_count(), ==, (DWORD)FTL_OP_CTX_TABLE_SIZE);

    for (WORD i = 0U; i < FTL_OP_CTX_TABLE_SIZE; i++)
    {
        ftl_op_ctx_remove(i);
    }
    munit_assert_uint32(ftl_op_ctx_count(), ==, 0U);

    ftl_ut_trace_detail("op ctx: count 0->%u (fill); same-id add(0) -> count=%u; remove all -> count=%hu", (unsigned)FTL_OP_CTX_TABLE_SIZE, (unsigned)FTL_OP_CTX_TABLE_SIZE, ftl_op_ctx_count());
    ftl_ut_trace_summary("op ctx: count 0->%u (fill) -> %u (same-id) -> 0 (remove)", (unsigned)FTL_OP_CTX_TABLE_SIZE, (unsigned)FTL_OP_CTX_TABLE_SIZE);

    return MUNIT_OK;
}

static MunitResult test_op_route(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    struct ftl_context *route[CH_NUM][CE_NUM][LUN_NUM];
    ftl_ctx_t          *live  = ftl_ut_live();
    ftl_ctx_t          *saved = ftl_op_route_get(0U, 0U, 0U);

    ftl_ut_trace_begin();

    munit_assert_not_null(saved);

    memset(route, 0, sizeof(route));
    route[0][0][0] = live;
    ftl_op_route_set(route);

    munit_assert_ptr_equal(ftl_op_route_get(0U, 0U, 0U), live);
    munit_assert_ptr_equal(ftl_op_route_get_region(0U), live);

    memset(route, 0, sizeof(route));
    route[0][0][0] = saved;
    ftl_op_route_set(route);

    ftl_ut_trace_detail("op route: (0,0,0) saved=%p -> live=%p; region0=%p; restored=%p", (void *)saved, (void *)live, (void *)ftl_op_route_get_region(0U), (void *)ftl_op_route_get(0U, 0U, 0U));
    ftl_ut_trace_summary("op route: (0,0,0) %p -> %p; region0 -> %p", (void *)saved, (void *)live, (void *)ftl_op_route_get_region(0U));

    return MUNIT_OK;
}

static MunitResult test_bg_level_select(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t  *f = ftl_ut_live();
    pend_req_t *slots[FTL_PENDING_MAX];

    ftl_ut_trace_begin();

    munit_assert_int(bg_scheduler_level_select(f), ==, BG_LEVEL_IDLE);

    for (WORD i = 0U; i < 8U; i++)
    {
        slots[i] = pend_alloc(&f->pending);
        munit_assert_not_null(slots[i]);
    }
    munit_assert_int(bg_scheduler_level_select(f), ==, BG_LEVEL_NORMAL);

    for (WORD i = 8U; i < 17U; i++)
    {
        slots[i] = pend_alloc(&f->pending);
        munit_assert_not_null(slots[i]);
    }
    munit_assert_int(bg_scheduler_level_select(f), ==, BG_LEVEL_BUSY);

    for (WORD i = 0U; i < 17U; i++)
    {
        const WORD idx = (WORD)(slots[i] - f->pending.slots);

        pend_release_index(&f->pending, idx);
    }

    munit_assert_int(bg_scheduler_level_select(f), ==, BG_LEVEL_IDLE);

    f->pending.groups[0].used = true;
    munit_assert_int(bg_scheduler_level_select(f), ==, BG_LEVEL_BUSY);
    f->pending.groups[0].used = false;

    ftl_ut_trace_detail("bg level: pending 0->idle; 8->normal; 17->busy; release->idle; group used->busy");
    ftl_ut_trace_summary("bg level: idle -> normal (pending 8) -> busy (17) -> idle -> busy (group used)");

    return MUNIT_OK;
}

static MunitResult test_wbuf_density(const MunitParameter params[], void *data)
{
    (void)data;

    BYTE         src[FTL_LBA_PER_PAIR * FTL_IU_BYTES];
    BYTE         out[FTL_IU_BYTES];
    wb_handle_t  h;
    pend_table_t table;
    BYTE         gi = 0U;
    const BYTE   n  = (BYTE)(munit_parameters_get(params, "n")[0] - '0');

    ftl_ut_trace_begin();

    memset(&h, 0, sizeof(h));
    memset(&table, 0, sizeof(table));
    pend_init(&table);
    munit_assert_true(pend_group_alloc(&table, &gi));
    wb_init(&h, ftl_ut_ram_alloc(), &table, NULL);

    for (WORD i = 0U; i < (WORD)(n * FTL_IU_BYTES); i++)
    {
        src[i] = (BYTE)(i & 0xFFU);
    }

    wb_trigger_t tr;

    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_APPEND;
    tr.lba  = 0U;
    tr.data = src;
    tr.n    = n;
    tr.gi   = gi;

    munit_assert_int(wb_handle(&h, &tr, NULL, NULL), ==, FTL_OK);
    munit_assert_uint32(h.occupied, ==, ((1UL << n) - 1UL));
    munit_assert_uint32(table.groups[gi].outstanding, ==, 1U);

    for (BYTE c = 0U; c < n; c++)
    {
        munit_assert_true(wb_has(&h, c));
        munit_assert_true(wb_read(&h, c, out));
        munit_assert_memory_equal(FTL_IU_BYTES, &src[c * FTL_IU_BYTES], out);
    }

    ftl_ut_trace_detail("wbuf density: n=%hhu occupied 0->0x%02hhX; outstanding=%hu; %hhu CW readback byte-equal", n, h.occupied, table.groups[gi].outstanding, n);
    ftl_ut_trace_summary("wbuf[n=%hhu]: occupied 0->0x%02hhX outstanding=%hu", n, h.occupied, table.groups[gi].outstanding);

    return MUNIT_OK;
}

static MunitResult test_group_failure_aggregation(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    pend_table_t          table;
    BYTE                  gi     = 0U;
    volatile BYTE         flag   = 0U;
    volatile ssd_status_e status = SSD_STATUS_OK;

    memset(&table, 0, sizeof(table));
    pend_init(&table);
    munit_assert_true(pend_group_alloc(&table, &gi));

    pend_group_t *g = &table.groups[gi];

    g->total         = 2U;
    g->next          = 2U;
    g->status_out    = &status;
    g->complete_flag = &flag;
    g->on_complete   = host_group_done;
    s_group_done     = 0U;

    pend_group_add_subop(&table, gi);
    pend_group_add_subop(&table, gi);
    munit_assert_uint32(g->outstanding, ==, 2U);

    /* 第一个子操作失败：聚合状态立即粘滞，但组尚未完成。 */
    pend_group_complete_subop(&table, gi, SSD_STATUS_IO_ERROR);
    munit_assert_int(g->agg_status, ==, SSD_STATUS_IO_ERROR);
    munit_assert_uint32(s_group_done, ==, 0U);

    /* 第二个成功也不能把错误清掉；组只完成一次。 */
    pend_group_complete_subop(&table, gi, SSD_STATUS_OK);
    munit_assert_uint32(s_group_done, ==, 1U);
    munit_assert_uint32(flag, ==, 1U);
    munit_assert_int(status, ==, SSD_STATUS_IO_ERROR);
    munit_assert_uint32(pend_group_get_count(&table), ==, 0U);

    ftl_ut_trace_detail("group failure: sub1=IO_ERROR -> agg=%d; sub2=OK -> agg=%d; done=%lu flag=%hu status=%d groups=%hu", (int)SSD_STATUS_IO_ERROR, (int)g->agg_status, s_group_done, flag, (int)status, pend_group_get_count(&table));
    ftl_ut_trace_summary("group: IO_ERROR sticky across OK subop; done once, status=IO_ERROR");

    return MUNIT_OK;
}

static MunitResult test_wbuf_drop_reset_owners(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    wb_handle_t  h;
    pend_table_t table;
    BYTE         gi = 0U;
    BYTE         src[2U * FTL_IU_BYTES];
    wb_trigger_t tr;

    memset(&h, 0, sizeof(h));
    memset(&table, 0, sizeof(table));
    pend_init(&table);
    munit_assert_true(pend_group_alloc(&table, &gi));
    wb_init(&h, ftl_ut_ram_alloc(), &table, NULL);

    volatile BYTE         flag   = 0U;
    volatile ssd_status_e status = SSD_STATUS_OK;
    pend_group_t         *g      = &table.groups[gi];

    g->total         = 0U;
    g->next          = 0U;
    g->status_out    = &status;
    g->complete_flag = &flag;

    memset(src, 0xA5, sizeof(src));
    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_APPEND;
    tr.lba  = 0U;
    tr.data = src;
    tr.n    = 2U;
    tr.gi   = gi;
    munit_assert_int(wb_handle(&h, &tr, NULL, NULL), ==, FTL_OK);
    munit_assert_uint32(h.occupied, ==, 0x03U);
    munit_assert_uint32(g->outstanding, ==, 1U);

    /* DROP 一个 CW：页仍有数据，owners/在飞数不变。 */
    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_DROP;
    tr.lba  = 0U;
    munit_assert_int(wb_handle(&h, &tr, NULL, NULL), ==, FTL_OK);
    munit_assert_uint32(h.occupied, ==, 0x02U);
    munit_assert_uint32(g->outstanding, ==, 1U);

    /* DROP 最后一个 CW：页清空 ⇒ 贡献组结算一次（OK），组完成。 */
    tr.lba = 1U;
    munit_assert_int(wb_handle(&h, &tr, NULL, NULL), ==, FTL_OK);
    munit_assert_uint32(h.occupied, ==, 0U);
    munit_assert_uint32(h.owners, ==, 0U);
    munit_assert_uint32(g->outstanding, ==, 0U);
    munit_assert_uint32(flag, ==, 1U);
    munit_assert_int(status, ==, SSD_STATUS_OK);
    munit_assert_true(wb_is_idle(&h));

    /* RESET：丢弃整页并按**写失败**结算。 */
    BYTE                  gi2     = 0U;
    volatile BYTE         flag2   = 0U;
    volatile ssd_status_e status2 = SSD_STATUS_OK;

    munit_assert_true(pend_group_alloc(&table, &gi2));
    pend_group_t *g2 = &table.groups[gi2];

    g2->total         = 0U;
    g2->next          = 0U;
    g2->status_out    = &status2;
    g2->complete_flag = &flag2;

    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_APPEND;
    tr.lba  = 0U;
    tr.data = src;
    tr.n    = 2U;
    tr.gi   = gi2;
    munit_assert_int(wb_handle(&h, &tr, NULL, NULL), ==, FTL_OK);

    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_RESET;
    munit_assert_int(wb_handle(&h, &tr, NULL, NULL), ==, FTL_OK);
    munit_assert_uint32(h.occupied, ==, 0U);
    munit_assert_uint32(h.owners, ==, 0U);
    munit_assert_uint32(g2->outstanding, ==, 0U);
    munit_assert_uint32(flag2, ==, 1U);
    munit_assert_int(status2, ==, SSD_STATUS_IO_ERROR);
    munit_assert_true(wb_is_idle(&h));

    ftl_ut_trace_detail("wbuf drop/reset: drop cw0 -> occ=0x02 out=1; drop cw1 -> occ=0 out=0 status=OK; append+reset -> status=IO_ERROR idle=1");
    ftl_ut_trace_summary("wbuf: DROP settles owners once on empty (OK); RESET settles as IO_ERROR");

    return MUNIT_OK;
}

static MunitResult test_host_submit_invalid_args(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    ftl_ctx_t   *ftl = ftl_ut_live();
    BYTE         buf[FTL_IU_BYTES];
    ftl_io_req_t req;

    const WORD slots0  = pend_slot_get_count(&ftl->pending);
    const WORD groups0 = pend_group_get_count(&ftl->pending);

    memset(buf, 0, sizeof(buf));
    memset(&req, 0, sizeof(req));

    req.buffer = buf;
    req.count  = 0U;
    munit_assert_int(ftl_submit_write(ftl, &req), ==, FTL_ERROR);
    munit_assert_int(ftl_submit_read(ftl, &req), ==, FTL_ERROR);

    req.count  = 1U;
    req.buffer = NULL;
    munit_assert_int(ftl_submit_write(ftl, &req), ==, FTL_ERROR);
    munit_assert_int(ftl_submit_read(ftl, &req), ==, FTL_ERROR);

    req.buffer = buf;
    req.lba    = FTL_LBA_MAX;
    req.count  = 1U;
    munit_assert_int(ftl_submit_write(ftl, &req), ==, FTL_ERROR);
    munit_assert_int(ftl_submit_read(ftl, &req), ==, FTL_ERROR);

    munit_assert_int(ftl_submit_write(NULL, &req), ==, FTL_ERROR);
    munit_assert_int(ftl_submit_read(NULL, &req), ==, FTL_ERROR);

    /* 非法参数必须在分配任何池资源之前拒绝。 */
    munit_assert_uint32(pend_slot_get_count(&ftl->pending), ==, slots0);
    munit_assert_uint32(pend_group_get_count(&ftl->pending), ==, groups0);
    munit_assert_uint32(ftl_op_ctx_count(), ==, 0U);

    ftl_ut_trace_detail("submit invalid: count=0, buffer=NULL, lba=FTL_LBA_MAX+1, ftl=NULL -> all ERROR; slots %hu->%hu groups %hu->%hu ctx=0", slots0, pend_slot_get_count(&ftl->pending), groups0, pend_group_get_count(&ftl->pending));
    ftl_ut_trace_summary("host submit: invalid args rejected before resource allocation");

    return MUNIT_OK;
}

MunitTest g_ftl_pure_host_tests[] = {
    {                (char *)"host_idle",                 test_host_idle, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {            (char *)"pending_slots",             test_pending_slots, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {         (char *)"group_exhaustion",          test_group_exhaustion, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {      (char *)"group_complete_once",       test_group_complete_once, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {(char *)"group_failure_aggregation", test_group_failure_aggregation, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {         (char *)"wbuf_append_read",          test_wbuf_append_read, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {   (char *)"wbuf_drop_reset_owners",    test_wbuf_drop_reset_owners, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {             (char *)"wbuf_density",              test_wbuf_density, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, s_wbuf_n_params},
    {                   (char *)"op_ctx",                    test_op_ctx, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {          (char *)"op_ctx_capacity",           test_op_ctx_capacity, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {                (char *)"bg_engine",                 test_bg_engine, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {                 (char *)"op_route",                  test_op_route, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {          (char *)"bg_level_select",           test_bg_level_select, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    { (char *)"host_submit_invalid_args",  test_host_submit_invalid_args, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {                               NULL,                           NULL,              NULL,                 NULL,             MUNIT_TEST_OPTION_NONE,            NULL},
};

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

    ftl_ut_trace_detail("policy: free=%hu gc=0x%02lX wl=%d load_inflight=%hhu", ftl->alloc.free_count, triggers, (int)wl, ftl->cache->load_inflight);
    ftl_ut_trace_summary("policy: free=%hu gc=0x%02lX wl=%d", ftl->alloc.free_count, triggers, (int)wl);

    return MUNIT_OK;
}

static MunitResult test_gc_trigger(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_t *a = ftl_ut_ram_alloc();

    munit_assert_uint32(gc_should_trigger(NULL), ==, 0U);

    /* 空池：紧急水位必命中。 */
    DWORD trig = gc_should_trigger(a);

    munit_assert_uint32((trig & GC_TRIGGER_EMERGENCY), !=, 0U);

    const DWORD trig_empty = trig; /* 证据：空池的首个判定 */

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

    ftl_ut_trace_detail("gc trigger: free 0 -> emergency=%d; free 40 -> 0; free %hu (<%u) -> watermark=%d trig=0x%02lX", (int)((trig_empty & GC_TRIGGER_EMERGENCY) != 0U), a->free_count, (unsigned)FREE_POOL_TARGET, (int)((trig & GC_TRIGGER_WATERMARK) != 0U), trig);
    ftl_ut_trace_summary("gc trigger: free 0 -> emergency; 40 -> 0; %hu -> watermark", a->free_count);

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

    const char       *mode_s = munit_parameters_get(params, "mode");
    gc_benefit_mode_e mode   = GC_BENEFIT_OFF;

    if (strcmp(mode_s, "idle") == 0)
    {
        mode = GC_BENEFIT_IDLE;
    }
    else if (strcmp(mode_s, "emergency") == 0)
    {
        mode = GC_BENEFIT_EMERGENCY;
    }

    memset(&out, 0, sizeof(out));
    munit_assert_int(gc_select_victim(a, mode, &out), ==, GC_SELECT_OK);
    munit_assert_not_null(out.victim);
    munit_assert_uint32(out.victim->plane_blocks[0], ==, 0U);

    ftl_ut_trace_detail("gc victim[%s]: valid a=10 b=%u -> best=%hu%% scanned=%hu eligible=%hu victim.plane0=%hu", mode_s, (unsigned)(FTL_CW_PER_BLOCK - 8U), out.best_valid_pct, out.scanned, out.eligible, out.victim->plane_blocks[0]);
    ftl_ut_trace_summary("gc victim[%s]: valid 10 vs %u -> best=%hu%% scanned=%hu", mode_s, (unsigned)(FTL_CW_PER_BLOCK - 8U), out.best_valid_pct, out.scanned);

    return MUNIT_OK;
}

static MunitResult test_wl_trigger_and_victim(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    WORD        blocks[PLANE_NUM];
    wl_victim_t victim;

    ftl_ut_trace_begin();

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

    ftl_ut_trace_detail("wl: writes %u -> no trigger; writes %u, delta %u -> trigger; victim valid=%hu plane=(%hu,%hu) next_page=%hhu", (unsigned)WL_MIN_WRITES, (unsigned)WL_MIN_WRITES, (unsigned)WL_THRESHOLD, victim.valid_cw, victim.plane_blocks[0], victim.plane_blocks[1], victim.next_page);
    ftl_ut_trace_summary("wl: writes %u -> no; delta=%u -> trigger; victim valid=%hu", (unsigned)WL_MIN_WRITES, (unsigned)WL_THRESHOLD, victim.valid_cw);

    return MUNIT_OK;
}

static MunitResult test_rr_health(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    rr_ecc_track_t track;

    ftl_ut_trace_begin();

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

    ftl_ut_trace_detail("rr: HEALTHY->WARNING(rising=%u)->CRITICAL(%u)->DYING(%u); low ecc -> health=%d last_ecc=%hhu", (unsigned)RR_ECC_RISING_THRESHOLD, (unsigned)RR_ECC_HIGH_WATER, (unsigned)RR_ECC_MAX, (int)rr_get_health(&track), track.last_ecc);
    ftl_ut_trace_summary("rr: HEALTHY->WARNING->CRITICAL->DYING; low ecc keeps health=%d last_ecc=%hhu", (int)rr_get_health(&track), track.last_ecc);

    return MUNIT_OK;
}

static MunitResult test_rr_predictive_retire(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_t     *a = ftl_ut_ram_alloc();
    const WORD g = ftl_gblock(0U, 300U);

    WORD blocks[PLANE_NUM];

    blocks[0] = 300U;
    blocks[1] = 301U;

    munit_assert_true(alc_super_closed_add(a, blocks, FTL_TEMP_COLD) < a->closed_sb_count);

    rr_record_block_ecc(a, g, (BYTE)RR_ECC_MAX);

    munit_assert_int(rr_get_health(&a->blocks[g].rr), ==, RR_BLOCK_DYING);

    WORD hit = 0U;

    munit_assert_true(rr_predictive_scan(a, &hit));
    munit_assert_uint32(hit, ==, g);

    rr_retire_block_at(a, g);

    munit_assert_uint32(ftl_block_state(&a->blocks[g]), ==, FTL_BLOCK_STATE_RETIRED);
    munit_assert_uint32(a->blocks[g].read_count, ==, 0U);
    munit_assert_int(rr_get_health(&a->blocks[g].rr), ==, RR_BLOCK_HEALTHY);

    ftl_ut_trace_detail("rr: g=%hu ecc=%u -> DYING; predictive hit=%hu; retire -> state=%d read_count=%hu health=%d", g, (unsigned)RR_ECC_MAX, hit, (int)ftl_block_state(&a->blocks[g]), a->blocks[g].read_count, (int)rr_get_health(&a->blocks[g].rr));
    ftl_ut_trace_summary("rr: predictive hit g=%hu -> RETIRED; read_count=%hu health=%d", g, a->blocks[g].read_count, (int)rr_get_health(&a->blocks[g].rr));

    return MUNIT_OK;
}

static MunitResult test_wl_victim_threshold(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    WORD        blocks[PLANE_NUM];
    wl_victim_t victim;

    ftl_ut_trace_begin();

    alc_t *a = ftl_ut_ram_alloc();

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        blocks[p]                               = p;
        a->blocks[ftl_gblock(p, p)].erase_count = 50U;
        ftl_block_set_valid(&a->blocks[ftl_gblock(p, p)], 10U); /* 3.9% < 70% */
    }

    munit_assert_true(alc_super_closed_add(a, blocks, FTL_TEMP_COLD) < a->closed_sb_count);

    memset(&victim, 0, sizeof(victim));
    munit_assert_false(wl_select_victim(a, &victim));

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        ftl_block_set_valid(&a->blocks[ftl_gblock(p, p)], 200U); /* 78% */
    }

    munit_assert_true(wl_select_victim(a, &victim));

    ftl_ut_trace_detail("wl victim: valid 10/%u (3%%) reject; valid 200/%u (78%%) accept -> valid_cw=%hu plane=(%hu,%hu)", (unsigned)FTL_CW_PER_BLOCK, (unsigned)FTL_CW_PER_BLOCK, victim.valid_cw, victim.plane_blocks[0], victim.plane_blocks[1]);
    ftl_ut_trace_summary("wl victim: valid 10 -> reject; valid 200 -> accept (%hu CW)", victim.valid_cw);

    return MUNIT_OK;
}

static MunitResult test_gc_decode_cw_lba(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    fc_command_t *cmd = fc_command_alloc();

    munit_assert_not_null(cmd);

    const DWORD pba  = pba_make(0U, 0U, 0U, 5U, 0U, 1U, 0U);
    const DWORD page = 4000U; /* CW_NUM 对齐 */

    cmd->paa = pba_to_paa(pba);

    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        munit_assert_int(fc_command_configure_descriptor(cmd, 0U, c, pba_to_paa(pba)), ==, FC_STATUS_OK);
    }

    DWORD cw[CW_NUM];
    DWORD base = 0U;

    /* OK：四个 CW 同页基址。 */
    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        BYTE *meta = fc_buffer_get_meta(cmd, c);
        DWORD v    = page + c;

        munit_assert_not_null(meta);
        memcpy(meta, &v, sizeof(v));
    }
    munit_assert_int(gc_decode_cw_lba(cmd, pba, cw, &base), ==, GC_DECODE_OK);
    munit_assert_uint32(base, ==, page);
    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        munit_assert_uint32(cw[c], ==, page + c);
    }

    /* EMPTY：全 0xFFFFFFFF。 */
    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        BYTE *meta = fc_buffer_get_meta(cmd, c);
        DWORD v    = FTL_LBA_INVALID;

        memcpy(meta, &v, sizeof(v));
    }
    munit_assert_int(gc_decode_cw_lba(cmd, pba, cw, &base), ==, GC_DECODE_EMPTY);

    /* CORRUPT：CW1 落在另一页基址。 */
    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        BYTE *meta = fc_buffer_get_meta(cmd, c);
        DWORD v    = page + c;

        if (c == 1U)
        {
            v = page + 8U + c; /* 换页基址但保持同 plane/cw */
        }
        memcpy(meta, &v, sizeof(v));
    }
    munit_assert_int(gc_decode_cw_lba(cmd, pba, cw, &base), ==, GC_DECODE_CORRUPT);

    /* 空洞：只有 CW2 写过。 */
    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        BYTE *meta = fc_buffer_get_meta(cmd, c);
        DWORD v    = (c == 2U) ? (page + 2U) : FTL_LBA_INVALID;

        memcpy(meta, &v, sizeof(v));
    }
    munit_assert_int(gc_decode_cw_lba(cmd, pba, cw, &base), ==, GC_DECODE_OK);
    munit_assert_uint32(base, ==, page);
    munit_assert_uint32(cw[0], ==, FTL_LBA_INVALID);
    munit_assert_uint32(cw[2], ==, page + 2U);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);

    ftl_ut_trace_detail("gc decode: pba=0x%08lX page=%lu; OK base=page cw=page+0..3; EMPTY all INVALID; CORRUPT cw1=page+8; HOLE base=%lu cw0=0x%08lX cw2=%lu", pba, page, base, cw[0], cw[2]);
    ftl_ut_trace_summary("gc decode: OK base=%lu; EMPTY/CORRUPT/HOLE ok", base);

    return MUNIT_OK;
}

static MunitResult test_l2p_resident_states(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    ftl_ctx_t   *f      = ftl_ut_live();
    l2p_cache_t *c      = f->cache;
    const DWORD  lba    = 0U;
    const DWORD  region = lba_to_global_region(lba);
    const WORD   entry  = lba_to_entry(lba);

    WORD idx = L2P_CACHE_SIZE;

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        if ((c->lines[i].state != L2P_CACHE_INVALID) && (c->lines[i].region_id == region))
        {
            idx = i;
            break;
        }
    }
    if (idx == L2P_CACHE_SIZE)
    {
        for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
        {
            if (c->lines[i].state == L2P_CACHE_INVALID)
            {
                idx = i;
                break;
            }
        }
    }
    if (idx == L2P_CACHE_SIZE)
    {
        idx = 0U;
    }

    l2p_cache_line_t *line = &c->lines[idx];

    memcpy(&s_l2p_line_saved, line, sizeof(*line));
    memcpy(s_l2p_bitmap_saved, c->region_dirty_bitmap, sizeof(s_l2p_bitmap_saved));

    memset(line, 0, sizeof(*line));
    line->region_id      = region;
    line->state          = L2P_CACHE_CLEAN;
    line->entries[entry] = 0x11111U;

    DWORD out = 0U;

    munit_assert_int(l2p_read(c, lba, &out), ==, FTL_OK);
    munit_assert_uint32(out, ==, 0x11111U);
    munit_assert_int(l2p_write(c, lba, 0x22222U), ==, FTL_OK);
    munit_assert_uint32(line->state, ==, L2P_CACHE_DIRTY);

    line->state = L2P_CACHE_FLUSHING;
    munit_assert_int(l2p_read(c, lba, &out), ==, FTL_OK);
    munit_assert_int(l2p_write(c, lba, 0x33333U), ==, FTL_AGAIN);

    line->state = L2P_CACHE_LOADING;
    munit_assert_int(l2p_read(c, lba, &out), ==, FTL_AGAIN);
    munit_assert_int(l2p_write(c, lba, 0x33333U), ==, FTL_AGAIN);

    /* 批量读/写：同一 pair 窗口的掩码路径。 */
    line->state          = L2P_CACHE_CLEAN;
    line->entries[entry] = 0x44444U;

    DWORD batch_out[FTL_LBA_PER_PAIR];

    memset(batch_out, 0, sizeof(batch_out));
    munit_assert_int(l2p_entries_read(c, 0U, 0x01U, batch_out), ==, FTL_OK);
    munit_assert_uint32(batch_out[0], ==, 0x44444U);

    const DWORD batch_in[FTL_LBA_PER_PAIR] = {0x55555U, 0U};

    munit_assert_int(l2p_entries_write(c, 0U, 0x01U, batch_in), ==, FTL_OK);
    munit_assert_uint32(line->entries[entry], ==, 0x55555U);

    memcpy(line, &s_l2p_line_saved, sizeof(*line));
    memcpy(c->region_dirty_bitmap, s_l2p_bitmap_saved, sizeof(s_l2p_bitmap_saved));

    ftl_ut_trace_detail("l2p: CLEAN read=0x%lX; write 0x%lX -> DIRTY; FLUSHING read=OK write=AGAIN; LOADING read/write=AGAIN; batch read=0x%lX write=0x%lX byte-equal", 0x11111UL, 0x22222UL, batch_out[0], batch_in[0]);
    ftl_ut_trace_summary("l2p: CLEAN 0x11111 -> DIRTY 0x22222; batch 0x%lX -> 0x%lX", batch_out[0], batch_in[0]);

    return MUNIT_OK;
}

static l2p_owner_t s_fake_owner;

static const l2p_owner_t *l2p_fake_owner_fn(void *ctx, DWORD global_region)
{
    (void)ctx;
    (void)global_region;
    return &s_fake_owner;
}

static MunitResult test_l2p_miss_unmapped_evict(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    ftl_ctx_t   *f = ftl_ut_live();
    l2p_cache_t *c = f->cache;

    /* 纯测试不做 NAND 写回：若存在脏/在飞行，跳过（不在 pure 层制造 I/O）。 */
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        const l2p_cache_state_e st = c->lines[i].state;

        if ((st == L2P_CACHE_DIRTY) || (st == L2P_CACHE_FLUSHING) || (st == L2P_CACHE_LOADING))
        {
            return MUNIT_SKIP;
        }
    }

    const DWORD region = L2P_GLOBAL_REGIONS - 1U;
    const DWORD lba    = region << L2P_REGION_SHIFT;

    l2p_owner_fn_t old_fn  = c->owner_of_region;
    void          *old_ctx = c->owner_ctx;

    memcpy(s_l2p_bitmap_saved, c->region_dirty_bitmap, sizeof(s_l2p_bitmap_saved));

    s_fake_owner.dim          = 0U;
    s_fake_owner.metadata_dir = ftl_ut_ram_md();
    s_fake_owner.alloc        = ftl_ut_ram_alloc();
    s_fake_owner.topo         = f->topo;
    c->owner_of_region        = l2p_fake_owner_fn;
    c->owner_ctx              = NULL;

    DWORD out = 0U;

    /* 未映射：起加载（无 NAND I/O）→ 本轮 AGAIN；重试 CLEAN 且条目 INVALID。 */
    munit_assert_int(l2p_read(c, lba, &out), ==, FTL_AGAIN);
    munit_assert_int(l2p_read(c, lba, &out), ==, FTL_OK);
    munit_assert_uint32(out, ==, NAND_PBA_INVALID);

    munit_assert_int(l2p_write(c, lba, 0x66666U), ==, FTL_OK);
    munit_assert_int(l2p_read(c, lba, &out), ==, FTL_OK);
    munit_assert_uint32(out, ==, 0x66666U);

    c->owner_of_region = old_fn;
    c->owner_ctx       = old_ctx;

    /* 清掉本测试构造的行，避免污染后续访问。 */
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        if (c->lines[i].region_id == region)
        {
            memset(&c->lines[i], 0, sizeof(c->lines[i]));
            break;
        }
    }

    memcpy(c->region_dirty_bitmap, s_l2p_bitmap_saved, sizeof(s_l2p_bitmap_saved));

    ftl_ut_trace_detail("l2p miss: region=%lu lba=%lu; read AGAIN->OK unmapped=0x%08lX; write 0x66666 -> read 0x%lX", region, lba, (DWORD)NAND_PBA_INVALID, out);
    ftl_ut_trace_summary("l2p miss: lba=%lu read AGAIN->OK unmapped; write 0x66666 -> read 0x%lX", lba, out);

    return MUNIT_OK;
}

MunitTest g_ftl_pure_io_tests[] = {
    {           (char *)"policy_entry",            test_policy_entry, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {             (char *)"gc_trigger",              test_gc_trigger, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {       (char *)"gc_select_victim",        test_gc_select_victim, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, s_gc_mode_params},
    {       (char *)"gc_decode_cw_lba",        test_gc_decode_cw_lba, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {  (char *)"wl_trigger_and_victim",   test_wl_trigger_and_victim, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {    (char *)"l2p_resident_states",     test_l2p_resident_states, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {(char *)"l2p_miss_unmapped_evict", test_l2p_miss_unmapped_evict, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {    (char *)"wl_victim_threshold",     test_wl_victim_threshold, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {              (char *)"rr_health",               test_rr_health, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {   (char *)"rr_predictive_retire",    test_rr_predictive_retire, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {                             NULL,                         NULL,              NULL,                 NULL,             MUNIT_TEST_OPTION_NONE,             NULL},
};
