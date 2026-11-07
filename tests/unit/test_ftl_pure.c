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
static l2p_cache_line_s s_l2p_line_saved;
static DWORD            s_l2p_bitmap_saved[L2P_LOCAL_DIRTY_WORDS];
static BYTE             s_cp_page[PAGE_MAIN_SIZE];

/* 把每个 plane 的前 n 个块放进 Free Pool（测试自有表，plane 内块号 0..n-1）。 */
static void mem_fill_free(alc_s *a, WORD n)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD i = 0U; i < n; i++)
        {
            alc_free_push(a, ftl_gblock(p, i));
        }
    }
}

/* per-plane 空闲计数的总和。它是 `free_count` 的**第二本账**（RISK-ALLOC-09 引入），
 * 两个量必须恒等 —— 漂移即「成对分配失败」的前兆，且编译期完全无声。 */
static WORD mem_plane_free_sum(const alc_s *a)
{
    WORD sum = 0U;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        sum = (WORD)(sum + alc_free_get_count(a, p));
    }
    return sum;
}

/* address_roundtrip — LBA/PBA 地址编码往返。
 *
 * 覆盖：LBA 的 page_base/cw/plane 拆分、PBA 的 plane/block/page/cw 拆分、
 *       ftl_gblock(plane, block) 的 plane-major 全局编号。
 * 判据：逐字段解回等于输入。地址/位图类错值在编译期完全无声（CLAUDE.md §3.2），
 *       这条用例是后面所有建链用例的底板。纯 RAM。 */

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

/* free_pool_invariants — Free Pool 账本不变量。
 *
 * 覆盖：入池/出池只维护池数组；state==FREE ⟺ 在池；出池不翻转 state（文档化语义）；
 *       OP refill 按 plane LIFO 归还；per-plane 计数（RISK-ALLOC-09 的第二本账）
 *       与 flat 池恒同步、出池/归还只动所属 plane。
 * 判据：free_count 与池内条目始终同步，refill 后 op_count 归零；两本账一旦漂移，
 *       成对分配会在低水位无声失败（编译期不可见）。
 * 备注：FTL_BLOCK_STATE_FREE 会说谎（allocator_init 早于目录加载），选块以池为准。 */

static MunitResult test_free_pool_invariants(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

    munit_assert_uint32(a->free_count, ==, 0U);

    mem_fill_free(a, 4U);

    munit_assert_uint32(a->free_count, ==, (DWORD)(FTL_UT_BLOCKS));

    /* per-plane 账与 flat 池必须同步（唯一闸门 alc_free_push/remove 维护两本账）。 */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        munit_assert_uint32(alc_free_get_count(a, p), ==, 4U);
    }
    munit_assert_uint32(mem_plane_free_sum(a), ==, a->free_count);
    munit_assert_uint32(alc_free_min_plane_count(a), ==, 4U);

    /* state == FREE ⟺ 在池；SB 归属已清。 */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD i = 0U; i < 4U; i++)
        {
            const WORD         g = ftl_gblock(p, i);
            const bst_block_s *b = &a->blocks[g];

            munit_assert_uint32(ftl_block_state(b), ==, FTL_BLOCK_STATE_FREE);
            munit_assert_uint32(ftl_block_sb_id(b), ==, FTL_BLOCK_NO_SB);
        }
    }

    /* 出池：只改池，不改状态（文档化语义）。 */
    alc_free_remove(a, ftl_gblock(0U, 1U));
    munit_assert_uint32(a->free_count, ==, (DWORD)(FTL_UT_BLOCKS - 1U));
    munit_assert_uint32(ftl_block_state(&a->blocks[ftl_gblock(0U, 1U)]), ==, FTL_BLOCK_STATE_FREE);

    /* 出池必须**只从所属 plane** 扣账：扣错 side 会让计数漂移，成对分配随后失败。 */
    munit_assert_uint32(alc_free_get_count(a, 0U), ==, 3U);
    munit_assert_uint32(alc_free_get_count(a, 1U), ==, 4U);
    munit_assert_uint32(mem_plane_free_sum(a), ==, a->free_count);
    munit_assert_uint32(alc_free_min_plane_count(a), ==, 3U);

    /* OP refill：把 OP 块归还 Free Pool（LIFO，per-plane）。 */
    munit_assert_true(alc_op_push(a, 1U, ftl_gblock(1U, 4U)));
    const WORD refilled = alc_free_refill_from_op(a, 1U);

    munit_assert_uint32(refilled, ==, ftl_gblock(1U, 4U));
    munit_assert_uint32(a->free_count, ==, (DWORD)(FTL_UT_BLOCKS));
    munit_assert_uint32(a->op_count[1], ==, 0U);

    /* refill 回到**借用它的那个 plane**（per-plane 池是 LIFO），两本账仍同步。 */
    munit_assert_uint32(alc_free_get_count(a, 1U), ==, 5U);
    munit_assert_uint32(mem_plane_free_sum(a), ==, a->free_count);
    munit_assert_uint32(alc_free_min_plane_count(a), ==, 3U);

    ftl_ut_trace_detail("free pool: fill=%u blocks; remove g=%hu; refill from op1 -> g=%hu; op1=%hu; removed block state=%d; plane0=%hu plane1=%hu fpmin=%hu", (unsigned)FTL_UT_BLOCKS, ftl_gblock(0U, 1U), refilled, a->op_count[1], (int)ftl_block_state(&a->blocks[ftl_gblock(0U, 1U)]), alc_free_get_count(a, 0U), alc_free_get_count(a, 1U), alc_free_min_plane_count(a));
    ftl_ut_trace_summary("free pool: 0 -> %u (fill) -> %u (remove) -> %hu (refill g%hu); fpmin=%hu", (unsigned)FTL_UT_BLOCKS, (unsigned)(FTL_UT_BLOCKS - 1U), a->free_count, refilled, alc_free_min_plane_count(a));

    return MUNIT_OK;
}

/* super_block_open_close — Super Block 开/关与 rewind（参数 mode=hot/cold/high_wear）。
 *
 * 覆盖：开 SB 时两 plane 各取同页号 PBA、块进 OPEN；rewind 后重取必须得到同一页
 *       （不烧块）；close 后两 plane 块进 CLOSED 且记录 sb_id；closed 条目可摘除。
 * 判据：PBA 合法、plane 位与入参一致、closed/free 计数同步。纯 RAM。 */

static MunitResult test_super_block_open_close(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

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

/* closed_list_add_drop — Closed SB 列表按 plane 摘除。
 *
 * 覆盖：加一个两 plane 的 closed 条目；只摘 plane0 时条目保留（另一 plane 仍有活
 *       数据），两个 plane 都摘掉才释放条目。
 * 判据：closed_sb_count 只在最后一个 plane 摘除时减少，防止半摘除导致条目泄漏
 *       或同一条目被二次回收。纯 RAM。 */

static MunitResult test_closed_list_add_drop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

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

/* valid_count_guard — 有效 CW 记账与 BAD 降级。
 *
 * 覆盖：valid_count 连续 +1；sub_if_live 在健康态减 1；块置 BAD 后继续 sub 必须无效。
 * 判据：BAD 块不得再记账——uint16 回绕会变成 65535，直接污染 GC 有效率与 WL 判据。
 * 备注：这是「状态不变量做 O(1) 守卫」（C12）在 BST 上的落点。纯 RAM。 */

static MunitResult test_valid_count_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s       *a = ftl_ut_ram_alloc();
    bst_block_s *b = &a->blocks[ftl_gblock(0U, 0U)];

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

/* op_spare_pools — OP / Spare 两池的 per-plane 计数与边界。
 *
 * 覆盖：OP 每个 plane 独立计数并填到 FTL_OP_BLOCKS_PER_PLANE 上限；Spare 是 LIFO，
 *       空池 pop 必须返回 false。
 * 判据：一个 plane 填满不影响另一个；空池不能凭空给出块（返回垃圾块会让分配器
 *       永久漏账）。纯 RAM。 */

static MunitResult test_op_spare_pools(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

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

/* erase_stats — 擦除后的块统计复位。
 *
 * 覆盖：alc_bst_note_erase 使 erase_count+1、记录 last_pe_ts、valid_count/read_count
 *       清零、state 置 OPEN。
 * 判据：擦除是「块内容作废」的唯一时刻；残留有效计数会让 GC/WL 把空块当热块。
 * 备注：erase_count 是磨损均衡的持久化输入（P0-1）。纯 RAM。 */

static MunitResult test_erase_stats(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s       *a = ftl_ut_ram_alloc();
    bst_block_s *b = &a->blocks[ftl_gblock(0U, 2U)];

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

/* bbt_mark_bad — 标坏的完整后果（参数 plane=0/1）。
 *
 * 覆盖：BST 置 BAD、清 valid/sb_id、bad_count 与 dirty_gen 推进；同 plane Spare 弹
 *       一块补 Free Pool；重复标坏保持幂等。
 * 判据：bad_count/dirty_gen/Spare/free 四项都恰好变化一次；幂等重标不得重复弹块。
 * 备注：Spare 是第一道替换，弹错 plane 会把好块配给无块可用的 plane。纯 RAM。 */

static MunitResult test_bbt_mark_bad(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

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

/* bbt_metadata_bad — 元数据块丢失的降级路径。
 *
 * 覆盖：标坏元数据日志块（JOURNAL_BLOCK0）时不弹用户 Spare，而是置 pools.dirty
 *       让候选池重算，并置 rebuild_required 要求下次开机全量重建。
 * 判据：若这里误弹用户 Spare，元数据块会从用户数据区拿走块，账本永久偏移。纯 RAM。 */

static MunitResult test_bbt_metadata_bad(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

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

/* cw_masks — CW flat 位图工具。
 *
 * 覆盖：ftl_cw_bit 的绝对编号（plane*CW_NUM+cw）、span/plane 掩码、
 *       popcount/plane_count、NAND_FIELD_MASK。
 * 判据：plane1 的 CW0 必须是 bit CW_NUM 而不是 bit0。flat 编号用错就是 plane 盲
 *       位图（H31），位会改到另一个 plane 上且不崩溃。纯 RAM。 */

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

/* alloc_spare_full_refill_none — Spare 池满与空 Free Pool 边界。
 *
 * 覆盖：Spare 满时再 push 必须失败；清空 Spare 后从 Free Pool refill，而 Free Pool
 *       也为空时 refill 返回 false 且池计数保持 0。
 * 判据：不能凭空造块——Spare 与 Free Pool 的账本必须守恒。纯 RAM。 */

static MunitResult test_alloc_spare_full_refill_none(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s     *a     = ftl_ut_ram_alloc();
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

/* ══ 子套件 /ftl/pure/mem/：allocator / BST / BBT / CW 位图（纯 RAM）══ */

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

/* metadata_idle — metadata job 的启动基线。
 *
 * 覆盖：metadata_dir 句柄存在、md_handle_is_idle 为真、live block 计数为 0。
 * 判据：live block 泄漏会让后续 reclaim 误判（把别人的块当自己的）；先钉住初始态。
 * 备注：纯 RAM，不触碰介质。 */

static MunitResult test_metadata_idle(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

    ftl_ut_trace_begin();

    munit_assert_not_null(ftl->metadata_dir);
    munit_assert_true(md_handle_is_idle(ftl->metadata_dir));

    ftl_ut_trace_detail("metadata: handle idle=%d; live_blocks=%hu", (int)md_handle_is_idle(ftl->metadata_dir), md_live_block_count_get(ftl->metadata_dir));
    ftl_ut_trace_summary("metadata: idle=%d live_blocks=%hu (no live block expected at boot)", (int)md_handle_is_idle(ftl->metadata_dir), md_live_block_count_get(ftl->metadata_dir));

    return MUNIT_OK;
}

/* ring_geometry — md_ring 的记录数/块包含/plane 掩码几何。
 *
 * 覆盖：records_per_block = PAGES_PER_BLOCK / pages_per_record；total_records =
 *       block_count × records_per_block；has_block 只认环内块；plane_at 越界返回
 *       PLANE_NUM（哨兵语义）。
 * 判据：几何量一旦算错，locate/回收会静默扫错块。纯 RAM。 */

static MunitResult test_ring_geometry(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_ring_s ring;

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

/* directory_commit — Directory 的 primary/pair 提交账本。
 *
 * 覆盖：primary 覆盖写要把旧位置 live 计数 -1、新位置 +1；PAIR 提交两份副本都计入
 *       live 并置 dirty；entry/backup 查询分别返回两份 PBA。
 * 判据：live 计数是元数据块回收的唯一依据，漏一次就永久漏账。纯 RAM。 */

static MunitResult test_directory_commit(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    md_dir_cache_s *dir = ftl_ut_ram_md();

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

/* journal_append — Journal 基本生命周期。
 *
 * 覆盖：init 后 idle、sequence 从 1 起（0 保留为「无序号」）；reserve + append 推进
 *       序号并置 pending；未到水位时 needs_checkpoint 为假。
 * 判据：sequence 是回放/截断的唯一顺序坐标；reserve 与 append 必须一一配对。纯 RAM。 */

static MunitResult test_journal_append(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_jrn_s journal;

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

/* cp_encoders — Checkpoint 编码器与磨损页字节布局。
 *
 * 覆盖：block word 的 valid/state/temp/pool 位段打包解包；快照写入 CLOSED/有效数/
 *       温度、OP/Spare 池标签，以及由 Directory 权威判定的 LOG 状态；磨损页固定
 *       字节布局（erase/read/last_ecc/flags）。
 * 判据：解码逐字段等于编码输入；布局是掉电恢复读回的唯一契约，错位即读成垃圾。
 * 备注：纯 RAM，字节布局是跨版本兼容点。 */

static MunitResult test_cp_encoders(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s          *a = ftl_ut_ram_alloc();
    md_dir_cache_s *d = ftl_ut_ram_md();

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

static ftl_status_e ring_probe(void *ctx, WORD record_idx, BOOL full, DWORD *out_sequence)
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

/* ring_locate — md_ring_locate 用 probe 回调定位最新记录。
 *
 * 覆盖：written=5 → newest_idx=4、sequence=5、write_idx=5（下一条）；空环 →
 *       newest_idx=UINT16_MAX、sequence=0、write_idx=0。
 * 判据：「空环」与「找不到最新记录」必须区分——前者可以继续格式化，后者要报错。
 * 备注：probe 回调在纯 RAM 下模拟介质页有效性。 */

static MunitResult test_ring_locate(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_ring_s            ring;
    md_ring_locate_out_s out;
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

/* dir_ckpt_three_step — 目录检查点的 prepare → page → commit 三步。
 *
 * 覆盖：prepare 分配序号/槽位并置 dirty；page 填 magic/sequence/chunk 头；
 *       commit 清 dirty 并推进 dir_ckpt_sequence。
 * 判据：任何一步漏掉都会让重启后的 目录检查点与 Directory 序号不一致（恢复选错记录）。
 * 备注：纯 RAM；冷启动 locate 在这里用有效游标跳过。 */

static MunitResult test_dir_ckpt_three_step(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_dir_ckpt_prepare_out_s prep;
    md_dir_ckpt_page_s        page;
    static md_dir_ckpt_page_s s_dir_ckpt_page;

    ftl_ut_trace_begin();

    alc_s          *a = ftl_ut_ram_alloc();
    md_dir_cache_s *d = ftl_ut_ram_md();

    munit_assert_true(md_pool_build_all(a));

    d->dirty                 = true;
    d->dir_ckpt_cursor_valid = false;

    memset(&prep, 0, sizeof(prep));

    /* 冷启动游标会同步扫介质；纯 RAM 测试先给一个有效游标，跳过 locate。 */
    d->dir_ckpt_cursor_valid = true;
    d->dir_ckpt_write_idx    = 0U;
    d->dir_ckpt_sequence     = 0U;

    munit_assert_int(md_dir_ckpt_prepare(d, &prep, MD_DIR_CKPT_OWNER_BOOT), ==, MD_DIR_CKPT_READY);
    munit_assert_uint32(prep.seq, ==, 1U);
    munit_assert_uint32(prep.idx, ==, 0U);

    memset(&s_dir_ckpt_page, 0, sizeof(s_dir_ckpt_page));
    md_dir_ckpt_page(d, prep.seq, 0U, &s_dir_ckpt_page);

    munit_assert_uint32(s_dir_ckpt_page.magic, ==, (DWORD)DIR_CKPT_MAGIC);
    munit_assert_uint32(s_dir_ckpt_page.sequence, ==, prep.seq);
    munit_assert_uint32(s_dir_ckpt_page.chunk_index, ==, 0U);
    munit_assert_uint32(s_dir_ckpt_page.chunk_count, ==, (DWORD)DIR_CKPT_PAGE_COUNT);

    md_dir_ckpt_commit(d, &prep.ring, prep.idx, true);

    munit_assert_false(d->dirty);
    munit_assert_uint32(d->dir_ckpt_sequence, ==, prep.seq);

    (void)page;
    ftl_ut_trace_detail("dir_ckpt: dirty 1->%d; prepare seq=%lu idx=%hu; page magic=0x%08lX seq=%lu chunk=%hu/%hu; commit dir_ckpt_sequence=%lu", (int)d->dirty, prep.seq, prep.idx, s_dir_ckpt_page.magic, s_dir_ckpt_page.sequence, s_dir_ckpt_page.chunk_index, s_dir_ckpt_page.chunk_count, d->dir_ckpt_sequence);
    ftl_ut_trace_summary("dir_ckpt: dirty 1->%d seq 0->%lu idx=%hu chunks=%hu", (int)d->dirty, prep.seq, prep.idx, s_dir_ckpt_page.chunk_count);

    return MUNIT_OK;
}

/* log_alloc_rotate_release — 元数据日志块的分配/释放/轮转。
 *
 * 覆盖：md_log_alloc 依次取 L2P log 区块并置 LOG 状态；release 归还为 NONE；
 *       rotate 切到新块且页游标归零。
 * 判据：日志块双分配或漏释放都会让 L2P 写回落到已作废的块。纯 RAM。 */

static MunitResult test_log_alloc_rotate_release(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s          *a = ftl_ut_ram_alloc();
    md_dir_cache_s *d = ftl_ut_ram_md();

    const WORD g0 = md_log_alloc(d, a, 0U);

    munit_assert_uint32(g0, ==, ftl_gblock(0U, L2P_LOG_BLOCK0));
    munit_assert_uint32(ftl_block_state(&a->blocks[g0]), ==, FTL_BLOCK_STATE_LOG);

    const WORD g1 = md_log_alloc(d, a, 0U);

    munit_assert_uint32(g1, ==, ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 1U)));

    md_log_release(d, g0);
    munit_assert_uint32(ftl_block_state(&a->blocks[g0]), ==, FTL_BLOCK_STATE_NONE);

    // 每 plane 一个当前写块（RISK-BOOT-18 L2）：逐 plane 换块并校验各自的游标，
    // 不写死 plane 下标（H39）。
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        munit_assert_true(md_log_rotate(d, a, p));
        munit_assert_true(d->cur_valid[p]);
        munit_assert_uint32(d->cur_page[p], ==, 0U);
        ftl_ut_trace_detail("md log: plane=%hhu cur=g%hu page=%hhu valid=%d", p, d->cur_block[p], d->cur_page[p], (int)d->cur_valid[p]);
    }

    munit_assert_uint32(ftl_block_state(&a->blocks[g1]), ==, FTL_BLOCK_STATE_LOG);
    ftl_ut_trace_summary("md log: alloc g%hu->g%hu; release g%hu->NONE; rotate all %u plane(s)", g0, g1, g0, (unsigned)PLANE_NUM);

    return MUNIT_OK;
}

/* cp_init_timer — Checkpoint 句柄初始态与定时器复位。
 *
 * 覆盖：cp_init 后 active/waiting=false、phase=IDLE；cp_timer_reset 把 last_ms 清 0。
 * 判据：reset 不是「写成当前时刻」——下个 job 的起点由 cp_handle 写入，写成当前
 *       会让定时判据永远差一拍（周期性 CP 不触发）。纯 RAM。 */

static MunitResult test_cp_init_timer(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    cp_handle_s h;

    ftl_ut_trace_begin();

    alc_s          *a = ftl_ut_ram_alloc();
    md_dir_cache_s *d = ftl_ut_ram_md();
    ftl_ctx_s      *f = ftl_ut_live();

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

/* md_status_error — 固定 metadata 错误采样的当前语义。
 *
 * 覆盖：handle.error 置 FTL_ERROR 后调用 md_handle；进入 IDLE 时 error 被清，采样
 *       得到 FTL_OK（RISK-MD-17）。
 * 判据：这是有意语义还是待修缺陷由 RISK-MD-17 跟踪；若将来改为粘滞错误，本用例
 *       会先红，避免行为变化无人发现。纯 RAM。 */

static MunitResult test_md_status_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    md_dir_cache_s *d = ftl_ut_ram_md();

    d->handle.error = FTL_ERROR;

    md_handle(d);

    /* 当前语义：进入 IDLE 时清 error ⇒ 采样看不到上一次 abort（RISK-MD-17）。 */
    munit_assert_int(d->handle.error, ==, FTL_OK);

    ftl_ut_trace_detail("md handle.error FTL_ERROR -> sampled=%d (IDLE clears; RISK-MD-17)", (int)d->handle.error);
    ftl_ut_trace_summary("md handle.error FTL_ERROR -> sampled=%d (IDLE clears)", (int)d->handle.error);

    return MUNIT_OK;
}

/* journal_write_fail_clear — Journal 写失败标记的清除条件。
 *
 * 覆盖：write_fail_seq=5 时，clear(cp=4) 保留 pending，clear(cp=5) 才清除；
 *       needs_checkpoint 由显式 request 置位。
 * 判据：只有 CP 水位追上失败点才允许清除，否则失败标记被悄悄丢掉、坏页仍可被选中。
 * 备注：纯 RAM，不制造真实 NAND 失败（失败注入归 FI-NA）。 */

static MunitResult test_journal_write_fail_clear(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_jrn_s journal;

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

/* md_abort_keeps_victim — 压缩写 abort 不得回收/擦除 victim。
 *
 * 覆盖：构造「压缩写阶段取不到空槽」使 md_compact_write 返回 FAILED，触发
 *       md_handle_abort；victim 仍有活 region。
 * 判据：abort 只回 IDLE、清 victim/moved，**不得**把 victim 登记进回收位图或擦除
 *       （否则活映射被静默丢掉，RISK-MD-06/16、BOOT-02）。纯 RAM。 */

static MunitResult test_md_abort_keeps_victim(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    md_dir_cache_s *d       = ftl_ut_ram_md();
    const WORD      victim  = ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 5U));
    const WORD      pending = d->reclaim_pending_count;

    /* 构造「压缩写阶段取不到空槽」⇒ md_compact_write 返回 FAILED ⇒ md_handle_abort。
     * victim 仍有活 region：abort 只回 IDLE，不得把它登记进回收位图（更不得擦除）。 */
    d->l2p_live[victim] = 3U;

    // 无当前写块（每 plane 一个，RISK-BOOT-18 L2）⇒ md_compact_write 取不到槽。
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        d->cur_valid[p] = false;
    }

    d->handle.phase  = MD_JOB_CP_WRITE;
    d->handle.victim = victim;
    d->handle.slot   = NAND_PBA_INVALID;
    d->handle.moved  = 7U;

    md_handle(d);

    munit_assert_int(d->handle.phase, ==, MD_JOB_IDLE);
    munit_assert_uint32(d->l2p_live[victim], ==, 3U);
    munit_assert_uint32(d->reclaim_pending_count, ==, pending);
    munit_assert_uint32(d->handle.victim, ==, UINT16_MAX);
    munit_assert_uint32(d->handle.moved, ==, 0U);

    ftl_ut_trace_detail("md abort: victim=g%hu live=3; phase CP_WRITE + no slot -> FAILED -> abort; victim=%hu live=%hu pending=%hu moved=%hu", victim, d->handle.victim, d->l2p_live[victim], d->reclaim_pending_count, d->handle.moved);
    ftl_ut_trace_summary("md abort: victim g%hu live 3 kept, pending %hu->%hu, phase IDLE", victim, pending, d->reclaim_pending_count);

    return MUNIT_OK;
}

/* journal_truncate_idempotent — Journal 截断计划/提交的幂等性。
 *
 * 覆盖：counter=0 防下溢不截断；水位不足不截断；足够时返回环形下一块；
 *       commit 记录 pre_erased 并清 checkpoint/emergency；重复 plan 返回 ALREADY；
 *       commit(NONE) 不清 pre_erased。
 * 判据：commit(NONE) 清掉预擦块会让同一块被双擦——这是块寿命的直接损失。纯 RAM。 */

static MunitResult test_journal_truncate_idempotent(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    l2p_jrn_s journal;
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

/* ══ 子套件 /ftl/pure/md/：metadata / Journal / Checkpoint / 目录检查点（纯 RAM）══ */

MunitTest g_ftl_pure_md_tests[] = {
    {              (char *)"metadata_idle",               test_metadata_idle, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              (char *)"ring_geometry",               test_ring_geometry, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"directory_commit",            test_directory_commit, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"journal_append",              test_journal_append, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                (char *)"ring_locate",                 test_ring_locate, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        (char *)"dir_ckpt_three_step",         test_dir_ckpt_three_step, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
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

/* host_idle — host 侧启动不变量。
 *
 * 覆盖：pending 槽/组、op ctx 全 0，wbuf idle，bg 任务表长度 = BG_TASK_COUNT。
 * 判据：任一项非 0 都说明初始化漏了配对释放或上一个用例污染了全局池。纯 RAM。 */

static MunitResult test_host_idle(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

    ftl_ut_trace_begin();

    munit_assert_uint32(pend_slot_get_count(&ftl->pending), ==, 0U);
    munit_assert_uint32(pend_group_get_count(&ftl->pending), ==, 0U);
    munit_assert_uint32(ftl_op_req_count(), ==, 0U);
    munit_assert_true(wb_is_idle(ftl->wbuf));
    munit_assert_uint32(ftl->bg.task_count, ==, (DWORD)BG_TASK_COUNT);

    ftl_ut_trace_detail("host idle: pending=%hu groups=%hu req=%hu wbuf_idle=%d bg_tasks=%hhu", pend_slot_get_count(&ftl->pending), pend_group_get_count(&ftl->pending), ftl_op_req_count(), (int)wb_is_idle(ftl->wbuf), ftl->bg.task_count);
    ftl_ut_trace_summary("host: pending=%hu groups=%hu req=%hu wbuf_idle=%d bg_tasks=%hhu", pend_slot_get_count(&ftl->pending), pend_group_get_count(&ftl->pending), ftl_op_req_count(), (int)wb_is_idle(ftl->wbuf), ftl->bg.task_count);

    return MUNIT_OK;
}

/* pending_slots — pending 槽表的满/查找/移除/再分配。
 *
 * 覆盖：alloc 到 FTL_PENDING_MAX 后返回 NULL；按 command_id 查找/移除；移除后槽位
 *       可再次分配。
 * 判据：槽泄漏的症状是全系统 FTL_AGAIN 且不报错。纯 RAM。 */

static MunitResult test_pending_slots(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    pend_ctx_s table;

    ftl_ut_trace_begin();
    memset(&table, 0, sizeof(table));
    pend_init(&table);

    pend_req_s *slots[FTL_PENDING_MAX];

    for (WORD i = 0U; i < FTL_PENDING_MAX; i++)
    {
        slots[i] = pend_alloc(&table, (WORD)i);
        munit_assert_not_null(slots[i]);
    }

    munit_assert_null(pend_alloc(&table, 0U));
    munit_assert_uint32(pend_slot_get_count(&table), ==, (DWORD)FTL_PENDING_MAX);

    /* 按 command_id 查找/移除；移除后槽位可再分配。 */
    munit_assert_ptr_equal(pend_find(&table, 0U), slots[0]);
    pend_remove(&table, 0U);
    munit_assert_null(pend_find(&table, 0U));
    munit_assert_uint32(pend_slot_get_count(&table), ==, (DWORD)(FTL_PENDING_MAX - 1U));
    munit_assert_not_null(pend_alloc(&table, 0U));

    ftl_ut_trace_detail("pending: alloc 0->%u (full); alloc-on-full=NULL; remove id=0 -> count=%u; realloc ok", (unsigned)FTL_PENDING_MAX, (unsigned)FTL_PENDING_MAX - 1U);
    ftl_ut_trace_summary("pending: alloc 0->%u (full) -> %u (remove) -> %u (realloc)", (unsigned)FTL_PENDING_MAX, (unsigned)FTL_PENDING_MAX - 1U, (unsigned)FTL_PENDING_MAX);

    return MUNIT_OK;
}

/* group_exhaustion — 组槽满时的拒绝语义。
 *
 * 覆盖：填满 FTL_GROUP_MAX 后额外 alloc 必须返回 false；已有组的字段不得被覆盖
 *       （RISK-PEND-02）。
 * 判据：覆盖会在飞组的完成回调永远不触发，host 请求悬挂。纯 RAM。 */

static MunitResult test_group_exhaustion(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    pend_ctx_s table;
    BYTE       gi[FTL_GROUP_MAX];

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

/* group_complete_once — 组完成只通知一次。
 *
 * 覆盖：两个子操作 outstanding 2→0；只有最后一个完成时回调一次、释放组槽。
 * 判据：早完成或重复通知会让上层把同一请求结算两次（buffer 归属错乱）。纯 RAM。 */

static MunitResult test_group_complete_once(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    pend_ctx_s table;
    BYTE       gi = 0U;

    ftl_ut_trace_begin();
    memset(&table, 0, sizeof(table));
    pend_init(&table);
    s_group_done = 0U;

    munit_assert_true(pend_group_alloc(&table, &gi));

    pend_group_s *g = &table.groups[gi];

    g->total         = 2U;
    g->next          = 2U;
    g->on_complete   = host_group_done;
    g->complete_flag = NULL;

    pend_group_add_subop(&table, gi);
    pend_group_add_subop(&table, gi);
    munit_assert_uint32(g->outstanding, ==, 2U);

    pend_group_complete_subop_multi(&table, ftl_group_bit(gi), SSD_STATUS_OK);
    munit_assert_uint32(s_group_done, ==, 0U);
    pend_group_complete_subop_multi(&table, ftl_group_bit(gi), SSD_STATUS_OK);

    munit_assert_uint32(s_group_done, ==, 1U);
    munit_assert_false(g->used);
    munit_assert_uint32(pend_group_get_count(&table), ==, 0U);

    ftl_ut_trace_detail("group: outstanding 2->1->0; done 0->0->%lu; groups 1->%hu; used=%d", s_group_done, pend_group_get_count(&table), (int)g->used);
    ftl_ut_trace_summary("group: outstanding 2->0; done 0->%lu; groups 1->%hu", s_group_done, pend_group_get_count(&table));

    return MUNIT_OK;
}

/* wbuf_append_read — 写缓冲的基本 append/read 与账本。
 *
 * 覆盖：单 CW 写入置 occupied 对应位、owners 记组、组在飞 +1；同 pair 窗口再写 CW5
 *       形成离散两位；逐 CW 读回字节一致。
 * 判据：occupied 是 flat 绝对编号（plane*CW_NUM+cw），位错了会把数据放到另一 plane。
 * 备注：纯 RAM，pend 表由测试自建。 */

static MunitResult test_wbuf_append_read(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    wb_handle_s  h;
    pend_ctx_s   table;
    BYTE         gi = 0U;
    BYTE         src[FTL_IU_BYTES];
    BYTE         out[FTL_IU_BYTES];
    wb_trigger_s tr;

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

    munit_assert_int(wb_handle(&h, &tr, NULL), ==, FTL_OK);
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
    munit_assert_int(wb_handle(&h, &tr, NULL), ==, FTL_OK);
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

/* op_ctx — request 表的 add/remove。
 *
 * 覆盖：add 后 count=1，remove 后归零。
 * 判据：ctx 用 cmd_id 关联完成回调，泄漏会让完成派发拿错上下文。纯 RAM。
 * 注：take 已内联进 ftl_op_drain（单调用的循环体步骤），其“取 {ctx,op} 后清项”
 *     语义由每个完成事件的正常派发路径覆盖，不再单独测。 */

static MunitResult test_op_ctx(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    int dummy = 0;

    ftl_ut_trace_begin();

    munit_assert_uint32(ftl_op_req_count(), ==, 0U);
    munit_assert_int(ftl_op_req_add(60U, FTL_OP_HOST_IO, &dummy), ==, FTL_OK);
    munit_assert_uint32(ftl_op_req_count(), ==, 1U);

    ftl_op_req_remove(60U);
    munit_assert_uint32(ftl_op_req_count(), ==, 0U);

    ftl_op_req_add(61U, FTL_OP_HOST_IO, &dummy);
    ftl_op_req_remove(61U);
    munit_assert_uint32(ftl_op_req_count(), ==, 0U);

    ftl_ut_trace_detail("op request: add(60) -> count=1; remove(60) -> count=0; add(61)+remove(61) -> count=%hu", ftl_op_req_count());
    ftl_ut_trace_summary("op request: count 0->1 (add 60) ->0 (remove) ->0 (add 61 + remove)");

    return MUNIT_OK;
}

/* bg_engine — 后台调度器引擎初始化。
 *
 * 覆盖：bg_scheduler_init 后任务表长度 = BG_TASK_COUNT；level_select 对活实例返回
 *       合法档位（< BG_LEVEL_NUM）。
 * 判据：任务表长度与档位枚举是预算调度的坐标，先钉住入口。纯 RAM。 */

static MunitResult test_bg_engine(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    bg_sched_s     sched;
    bg_level_cfg_s cfg;
    bg_level_e     level;

    ftl_ut_trace_begin();
    memset(&sched, 0, sizeof(sched));
    bg_scheduler_init(&sched);

    munit_assert_uint32(sched.task_count, ==, (DWORD)BG_TASK_COUNT);
    munit_assert_uint32(sched.credits, ==, 0U);

    /* Token bucket：每 interval 补 grant_units，饱和到 grant_units × CAP。 */
    memset(&cfg, 0, sizeof(cfg));
    cfg.grant_units = 4U;
    cfg.task_mask   = 0U;
    sched.cfg       = &cfg;

    bg_scheduler_refill(&sched);
    munit_assert_uint32(sched.credits, ==, 4U);

    for (DWORD i = 0U; i < (BG_TOKEN_CAP_TICKS + 2U); i++)
    {
        bg_scheduler_refill(&sched);
    }
    munit_assert_uint32(sched.credits, ==, (4U * BG_TOKEN_CAP_TICKS));

    level = bg_scheduler_level_select(ftl_ut_live());

    munit_assert_uint32((DWORD)level, <, (DWORD)BG_LEVEL_NUM);

    ftl_ut_trace_detail("bg: task_count 0->%hhu; credits 0->4->%lu; level_select(live)=%d; BG_TASK_COUNT=%u", sched.task_count, sched.credits, (int)level, (unsigned)BG_TASK_COUNT);
    ftl_ut_trace_summary("bg: task_count 0->%hhu credits=%lu level=%d", sched.task_count, sched.credits, (int)level);

    return MUNIT_OK;
}

/* op_ctx_capacity — request 表的容量与同 id 覆盖语义。
 *
 * 覆盖：填满 FTL_OP_REQ_TABLE_SIZE 后计数不再增长；同 cmd_id 的 add 是覆盖而不是
 *       追加；全部 remove 后归零。
 * 判据：同 id 追加会让表提前耗尽，后续完成回调无处登记。纯 RAM。 */

static MunitResult test_op_ctx_capacity(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    int dummy = 0;

    ftl_ut_trace_begin();

    munit_assert_uint32(ftl_op_req_count(), ==, 0U);

    for (WORD i = 0U; i < FTL_OP_REQ_TABLE_SIZE; i++)
    {
        munit_assert_int(ftl_op_req_add(i, FTL_OP_HOST_IO, &dummy), ==, FTL_OK);
    }
    munit_assert_uint32(ftl_op_req_count(), ==, (DWORD)FTL_OP_REQ_TABLE_SIZE);

    /* 同 cmd_id 覆盖而不是追加：容量仍为 64。 */
    munit_assert_int(ftl_op_req_add(0U, FTL_OP_HOST_IO, &dummy), ==, FTL_OK);
    munit_assert_uint32(ftl_op_req_count(), ==, (DWORD)FTL_OP_REQ_TABLE_SIZE);

    for (WORD i = 0U; i < FTL_OP_REQ_TABLE_SIZE; i++)
    {
        ftl_op_req_remove(i);
    }
    munit_assert_uint32(ftl_op_req_count(), ==, 0U);

    ftl_ut_trace_detail("op request: count 0->%u (fill); same-id add(0) -> count=%u; remove all -> count=%hu", (unsigned)FTL_OP_REQ_TABLE_SIZE, (unsigned)FTL_OP_REQ_TABLE_SIZE, ftl_op_req_count());
    ftl_ut_trace_summary("op request: count 0->%u (fill) -> %u (same-id) -> 0 (remove)", (unsigned)FTL_OP_REQ_TABLE_SIZE, (unsigned)FTL_OP_REQ_TABLE_SIZE);

    return MUNIT_OK;
}

/* op_route — 完成路由表的 set/get。
 *
 * 覆盖：(ch,ce,lun) 路由与 region 路由都取回同一实例；测试结束恢复原表。
 * 判据：路由错一条，完成会派发到错误实例（L2P/journal 记账串实例）。
 * 备注：恢复原表是必须的——completion 表是全局的。纯 RAM。 */

static MunitResult test_op_route(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    struct FTL_CONTEXT_S *route[FTL_DIE_MAX];
    ftl_ctx_s            *live  = ftl_ut_live();
    ftl_ctx_s            *saved = ftl_op_route_get(0U, 0U, 0U);

    ftl_ut_trace_begin();

    munit_assert_not_null(saved);

    memset(route, 0, sizeof(route));
    route[0] = live;
    ftl_op_route_set(route);

    munit_assert_ptr_equal(ftl_op_route_get(0U, 0U, 0U), live);
    munit_assert_ptr_equal(ftl_op_route_get_region(0U), live);

    memset(route, 0, sizeof(route));
    route[0] = saved;
    ftl_op_route_set(route);

    ftl_ut_trace_detail("op route: (0,0,0) saved=%p -> live=%p; region0=%p; restored=%p", (void *)saved, (void *)live, (void *)ftl_op_route_get_region(0U), (void *)ftl_op_route_get(0U, 0U, 0U));
    ftl_ut_trace_summary("op route: (0,0,0) %p -> %p; region0 -> %p", (void *)saved, (void *)live, (void *)ftl_op_route_get_region(0U));

    return MUNIT_OK;
}

/* bg_level_select — 负载档位的判据。
 *
 * 覆盖：pending 槽 0→IDLE、8→NORMAL、17→BUSY；释放后回 IDLE；存在 used group 时
 *       保守升 BUSY（给前台让路）。
 * 判据：档位决定后台预算，判错会让 GC/CP 在 host 忙时抢资源。纯 RAM。 */

static MunitResult test_bg_level_select(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *f = ftl_ut_live();
    pend_req_s *slots[FTL_PENDING_MAX];

    ftl_ut_trace_begin();

    munit_assert_int(bg_scheduler_level_select(f), ==, BG_LEVEL_IDLE);

    for (WORD i = 0U; i < 8U; i++)
    {
        slots[i] = pend_alloc(&f->pending, (WORD)i);
        munit_assert_not_null(slots[i]);
    }
    munit_assert_int(bg_scheduler_level_select(f), ==, BG_LEVEL_NORMAL);

    for (WORD i = 8U; i < 17U; i++)
    {
        slots[i] = pend_alloc(&f->pending, (WORD)i);
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

/* wbuf_density — 参数化 n=1/2/4/8 的位图与数据区一致性。
 *
 * 覆盖：一次 append n 个 CW 后 occupied == (1<<n)-1、组在飞只 +1、每个 CW 读回字节一致。
 * 判据：occupied 与 data[] 的索引必须同源（H31）；位图对而数据错会静默写错 CW。
 * 备注：纯 RAM。 */

static MunitResult test_wbuf_density(const MunitParameter params[], void *data)
{
    (void)data;

    BYTE        src[FTL_LBA_PER_PAIR * FTL_IU_BYTES];
    BYTE        out[FTL_IU_BYTES];
    wb_handle_s h;
    pend_ctx_s  table;
    BYTE        gi = 0U;
    const BYTE  n  = (BYTE)(munit_parameters_get(params, "n")[0] - '0');

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

    wb_trigger_s tr;

    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_APPEND;
    tr.lba  = 0U;
    tr.data = src;
    tr.n    = n;
    tr.gi   = gi;

    munit_assert_int(wb_handle(&h, &tr, NULL), ==, FTL_OK);
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

/* group_failure_aggregation — 组内子操作失败的状态聚合。
 *
 * 覆盖：第一个子操作 IO_ERROR 后 agg_status 立即粘滞；第二个子操作 OK 不能清零；
 *       组只完成一次，status_out/complete_flag 反映失败。
 * 判据：把部分失败报成成功会让 host 认为写入已持久化。纯 RAM。 */

static MunitResult test_group_failure_aggregation(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    pend_ctx_s            table;
    BYTE                  gi     = 0U;
    volatile BYTE         flag   = 0U;
    volatile ssd_status_e status = SSD_STATUS_OK;

    memset(&table, 0, sizeof(table));
    pend_init(&table);
    munit_assert_true(pend_group_alloc(&table, &gi));

    pend_group_s *g = &table.groups[gi];

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
    pend_group_complete_subop_multi(&table, ftl_group_bit(gi), SSD_STATUS_IO_ERROR);
    munit_assert_int(g->agg_status, ==, SSD_STATUS_IO_ERROR);
    munit_assert_uint32(s_group_done, ==, 0U);

    /* 第二个成功也不能把错误清掉；组只完成一次。 */
    pend_group_complete_subop_multi(&table, ftl_group_bit(gi), SSD_STATUS_OK);
    munit_assert_uint32(s_group_done, ==, 1U);
    munit_assert_uint32(flag, ==, 1U);
    munit_assert_int(status, ==, SSD_STATUS_IO_ERROR);
    munit_assert_uint32(pend_group_get_count(&table), ==, 0U);

    ftl_ut_trace_detail("group failure: sub1=IO_ERROR -> agg=%d; sub2=OK -> agg=%d; done=%lu flag=%hu status=%d groups=%hu", (int)SSD_STATUS_IO_ERROR, (int)g->agg_status, s_group_done, flag, (int)status, pend_group_get_count(&table));
    ftl_ut_trace_summary("group: IO_ERROR sticky across OK subop; done once, status=IO_ERROR");

    return MUNIT_OK;
}

/* confirm_park_timeout — 写确认待入账的超时判据与结算原语（RISK-HOST-05 的纯 RAM 部分）。
 *
 * 覆盖：pend_confirm_is_expired 在 deadline 前为 false、到点（==）为 true；
 *       pend_confirm_settle 释放槽、按 IO_ERROR 结算组一次；待入账计数随结算归零。
 * 判据：待入账项若永不超时，host 写组永久 pending ⇒ 后续 I/O 拿不到组槽（全系统 AGAIN）；
 *       若过早超时，正在异步加载的 region 会被误判失败（静默丢写）。纯 RAM。 */
static MunitResult test_confirm_park_timeout(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    pend_ctx_s            table;
    BYTE                  gi     = 0U;
    volatile BYTE         flag   = 0U;
    volatile ssd_status_e status = SSD_STATUS_OK;

    memset(&table, 0, sizeof(table));
    pend_init(&table);
    munit_assert_true(pend_group_alloc(&table, &gi));

    pend_group_s *g      = &table.groups[gi];
    pend_req_s   *p      = pend_alloc(&table, 0U);
    const WORD    idx    = (WORD)(p - table.slots);
    const DWORD   owners = ftl_group_bit(gi);

    g->op            = PENDING_OP_WRITE;
    g->total         = (DWORD)FTL_LBA_PER_PAIR;
    g->next          = (DWORD)FTL_LBA_PER_PAIR;
    g->status_out    = &status;
    g->complete_flag = &flag;
    g->outstanding   = 1U;

    p->lba    = FTL_UT_LBA_BASE;
    p->n_cw   = FTL_LBA_PER_PAIR;
    p->owners = owners;
    pend_validate(p); /* owners/n_cw 的 [BOUND] 不变量（pend_alloc 契约） */
    pend_confirm_park(&table, idx, owners);
    munit_assert_true(pend_op_is_write_confirm(p->op));
    munit_assert_uint32(pend_confirm_get_count(&table), ==, 1U);

    /* 待入账时刻显式给定：超时判据只吃 park_ms，不依赖板端开机时钟。 */
    p->park_ms = 1000U;

    /* 超时判据是纯谓词：1099 未到、1100 到期（== deadline）。 */
    munit_assert_false(pend_confirm_is_expired(p, 1099U)); /* 差 1 ms */
    munit_assert_true(pend_confirm_is_expired(p, 1100U));  /* == FTL_GROUP_DRAIN_TIMEOUT_MS */

    /* 未到期：槽/组/通知都不动（判据成立才谈得上结算）。 */
    munit_assert_true(p->used);
    munit_assert_true(g->used);
    munit_assert_uint32(flag, ==, 0U);
    munit_assert_uint32(pend_slot_get_count(&table), ==, 1U);

    /* 到期结算一次：释放槽 + 组按 IO_ERROR 完成。 */
    pend_confirm_settle(&table, idx, SSD_STATUS_IO_ERROR);

    munit_assert_false(p->used);
    munit_assert_false(g->used);
    munit_assert_uint32(flag, ==, 1U);
    munit_assert_int(status, ==, SSD_STATUS_IO_ERROR);
    munit_assert_uint32(pend_confirm_get_count(&table), ==, 0U);

    ftl_ut_trace_detail("confirm park: t=1099 expired=0; t=1100 settle -> flag=%hhu status=%d",
                        flag,
                        (int)status);
    ftl_ut_trace_summary("confirm park: predicate flips at deadline; settle releases once with IO_ERROR");

    return MUNIT_OK;
}

/* confirm_is_parked — 待入账判别只看 WRITE_CONFIRM，不看 used。
 *
 * 覆盖：同表内一个普通 used 槽 + 一个待入账槽时，is_parked 对前者 false、后者 true；
 *       结算后计数归零，普通槽不受影响。
 * 判据：若判别用 used 而不是 op，会把在飞写当待入账结算 ⇒ 组被提前完成、
 *       命令随后再结算一次（重复完成）。纯 RAM。 */
static MunitResult test_confirm_is_parked(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    pend_ctx_s            table;
    BYTE                  gi     = 0U;
    volatile BYTE         flag   = 0U;
    volatile ssd_status_e status = SSD_STATUS_OK;

    memset(&table, 0, sizeof(table));
    pend_init(&table);
    munit_assert_true(pend_group_alloc(&table, &gi));

    pend_group_s *g      = &table.groups[gi];
    const DWORD   owners = ftl_group_bit(gi);

    g->op            = PENDING_OP_WRITE;
    g->total         = (DWORD)FTL_LBA_PER_PAIR;
    g->next          = (DWORD)FTL_LBA_PER_PAIR;
    g->status_out    = &status;
    g->complete_flag = &flag;
    g->outstanding   = 1U;

    /* 槽 A：普通在飞写（used 但 op 不是 WRITE_CONFIRM）。 */
    pend_req_s *a     = pend_alloc(&table, 0U);
    const WORD  idx_a = (WORD)(a - table.slots);

    a->lba    = FTL_UT_LBA_BASE;
    a->n_cw   = FTL_LBA_PER_PAIR;
    a->owners = owners;
    a->op     = PENDING_OP_WRITE;
    pend_validate(a);

    /* 槽 B：待入账。 */
    pend_req_s *b     = pend_alloc(&table, 1U);
    const WORD  idx_b = (WORD)(b - table.slots);

    b->lba    = FTL_UT_LBA_BASE;
    b->n_cw   = FTL_LBA_PER_PAIR;
    b->owners = owners;
    pend_validate(b);
    pend_confirm_park(&table, idx_b, owners);

    munit_assert_uint32(pend_confirm_get_count(&table), ==, 1U);
    munit_assert_false(pend_confirm_is_parked(&table, idx_a)); /* used 但不是待入账 */
    munit_assert_true(pend_confirm_is_parked(&table, idx_b));

    pend_confirm_settle(&table, idx_b, SSD_STATUS_OK);

    munit_assert_true(a->used); /* 普通写槽不参与待入账结算 */
    munit_assert_false(b->used);
    munit_assert_uint32(flag, ==, 1U);
    munit_assert_int(status, ==, SSD_STATUS_OK);
    munit_assert_uint32(pend_confirm_get_count(&table), ==, 0U);
    munit_assert_false(pend_confirm_is_parked(&table, idx_a)); /* 负例：used ≠ 待入账 */
    munit_assert_false(pend_confirm_is_parked(&table, idx_b));

    ftl_ut_trace_detail("confirm scan: a_parked=%d b_parked=%d; after settle a_used=%d flag=%hhu status=%d",
                        0,
                        0,
                        a->used,
                        flag,
                        (int)status);
    ftl_ut_trace_summary("confirm scan: is_parked ignores used-non-parked; settle touches only parked");

    return MUNIT_OK;
}

/* wbuf_drop_reset_owners — DROP/RESET 的贡献组结算语义。
 *
 * 覆盖：DROP 一个 CW 只清位；页仍有数据时 owners/在飞数不变；清空最后一个 CW 时
 *       按 OK 结算贡献组一次。RESET 丢弃整页并按 IO_ERROR 结算。
 * 判据：未编程数据被当成功上报就是静默丢写；结算漏一次则组永不完成。纯 RAM。 */

static MunitResult test_wbuf_drop_reset_owners(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    wb_handle_s  h;
    pend_ctx_s   table;
    BYTE         gi = 0U;
    BYTE         src[2U * FTL_IU_BYTES];
    wb_trigger_s tr;

    memset(&h, 0, sizeof(h));
    memset(&table, 0, sizeof(table));
    pend_init(&table);
    munit_assert_true(pend_group_alloc(&table, &gi));
    wb_init(&h, ftl_ut_ram_alloc(), &table, NULL);

    volatile BYTE         flag   = 0U;
    volatile ssd_status_e status = SSD_STATUS_OK;
    pend_group_s         *g      = &table.groups[gi];

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
    munit_assert_int(wb_handle(&h, &tr, NULL), ==, FTL_OK);
    munit_assert_uint32(h.occupied, ==, 0x03U);
    munit_assert_uint32(g->outstanding, ==, 1U);

    /* DROP 一个 CW：页仍有数据，owners/在飞数不变。 */
    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_DROP;
    tr.lba  = 0U;
    munit_assert_int(wb_handle(&h, &tr, NULL), ==, FTL_OK);
    munit_assert_uint32(h.occupied, ==, 0x02U);
    munit_assert_uint32(g->outstanding, ==, 1U);

    /* DROP 最后一个 CW：页清空 ⇒ 贡献组结算一次（OK），组完成。 */
    tr.lba = 1U;
    munit_assert_int(wb_handle(&h, &tr, NULL), ==, FTL_OK);
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
    pend_group_s *g2 = &table.groups[gi2];

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
    munit_assert_int(wb_handle(&h, &tr, NULL), ==, FTL_OK);

    memset(&tr, 0, sizeof(tr));
    tr.kind = WBUF_TRIGGER_RESET;
    munit_assert_int(wb_handle(&h, &tr, NULL), ==, FTL_OK);
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

/* host_submit_invalid_args — host 入口的非法参数拒绝。
 *
 * 覆盖：count=0、buffer=NULL、lba=FTL_LBA_MAX、ftl=NULL 的读写都返回 FTL_ERROR；
 *       且必须在分配任何槽/组/ctx 之前拒绝。
 * 判据：先建组再校验会留下半建组，后续请求被永久占用一个组槽。纯 RAM。 */

static MunitResult test_host_submit_invalid_args(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    ftl_ctx_s   *ftl = ftl_ut_live();
    BYTE         buf[FTL_IU_BYTES];
    ftl_io_req_s req;

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
    munit_assert_uint32(ftl_op_req_count(), ==, 0U);

    ftl_ut_trace_detail("submit invalid: count=0, buffer=NULL, lba=FTL_LBA_MAX+1, ftl=NULL -> all ERROR; slots %hu->%hu groups %hu->%hu req=0", slots0, pend_slot_get_count(&ftl->pending), groups0, pend_group_get_count(&ftl->pending));
    ftl_ut_trace_summary("host submit: invalid args rejected before resource allocation");

    return MUNIT_OK;
}

/* ══ 子套件 /ftl/pure/host/：pending / 组 / wbuf / op ctx / bg 调度（纯 RAM）══ */

MunitTest g_ftl_pure_host_tests[] = {
    {                (char *)"host_idle",                 test_host_idle, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {            (char *)"pending_slots",             test_pending_slots, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {         (char *)"group_exhaustion",          test_group_exhaustion, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {      (char *)"group_complete_once",       test_group_complete_once, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {(char *)"group_failure_aggregation", test_group_failure_aggregation, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {     (char *)"confirm_park_timeout",      test_confirm_park_timeout, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
    {        (char *)"confirm_is_parked",         test_confirm_is_parked, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,            NULL},
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

/* 同 mem_fill_free：GC/WL 用例用它把 Free Pool 补到目标水位。 */
static void io_fill_free(alc_s *a, WORD n)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD i = 0U; i < n; i++)
        {
            alc_free_push(a, ftl_gblock(p, i));
        }
    }
}

/* policy_entry — GC/WL 策略入口的空盘基线。
 *
 * 覆盖：活实例上 gc_should_trigger 只返回 watermark/emergency 位，wl_should_trigger
 *       返回合法布尔值，L2P load_inflight=0。
 * 判据：先钉住入口无副作用；策略求值不应制造 I/O 或推进账本。纯 RAM。 */

static MunitResult test_policy_entry(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

    ftl_ut_trace_begin();

    const DWORD triggers = gc_should_trigger(&ftl->alloc);
    const BOOL  wl       = wl_should_trigger(&ftl->alloc, ftl->total_writes);

    munit_assert_uint32(triggers & ~(GC_TRIGGER_WATERMARK | GC_TRIGGER_EMERGENCY), ==, 0U);
    munit_assert_uint32(ftl->cache->load_inflight, ==, 0U);

    ftl_ut_trace_detail("policy: free=%hu gc=0x%02lX wl=%d load_inflight=%hhu", ftl->alloc.free_count, triggers, (int)wl, ftl->cache->load_inflight);
    ftl_ut_trace_summary("policy: free=%hu gc=0x%02lX wl=%d", ftl->alloc.free_count, triggers, (int)wl);

    return MUNIT_OK;
}

/* gc_trigger — GC 三级水位的触发判据。
 *
 * 覆盖：空池必命中 EMERGENCY；补到 FREE_POOL_TARGET 以上不触发；跌破 TARGET 但高于
 *       紧急线时只命中 WATERMARK；**总水位达标但单 plane 低于 FREE_POOL_PLANE_LOW**
 *       时仍命中 WATERMARK（RISK-ALLOC-09）。
 * 判据：水位是 GC 唯一的入口闸门；空池不触发会让写路径永久 FTL_AGAIN。纯 RAM。 */

static MunitResult test_gc_trigger(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

    munit_assert_uint32(gc_should_trigger(NULL), ==, 0U);

    /* 空池：紧急水位必命中。 */
    DWORD trig = gc_should_trigger(a);

    munit_assert_uint32((trig & GC_TRIGGER_EMERGENCY), !=, 0U);

    const DWORD trig_empty = trig; /* 证据：空池的首个判定 */

    /* 补到 TARGET 以上：不再触发。 */
    io_fill_free(a, 32U);
    munit_assert_uint32(a->free_count, >=, (DWORD)FREE_POOL_TARGET);
    munit_assert_uint32(gc_should_trigger(a), ==, 0U);

    /* per-plane 水位（RISK-ALLOC-09）：**总水位仍在 TARGET 之上**，但把 plane1 抽到
     * 低于单 plane 水位 ⇒ 必须触发。只判 total 的实现会在这里返回 0，接着成对分配
     * 在低水位无声失败 —— 这条是把「两本账」与「一本总账」区分开的负例。 */
    for (WORD i = 0U; i < (WORD)(32U - (FREE_POOL_PLANE_LOW - 1U)); i++)
    {
        alc_free_remove(a, ftl_gblock(1U, i));
    }
    munit_assert_uint32(a->free_count, >=, (DWORD)FREE_POOL_TARGET);
    munit_assert_uint32(alc_free_get_count(a, 1U), ==, (DWORD)(FREE_POOL_PLANE_LOW - 1U));
    munit_assert_uint32(alc_free_min_plane_count(a), ==, (DWORD)(FREE_POOL_PLANE_LOW - 1U));

    const DWORD trig_plane_low = gc_should_trigger(a);

    munit_assert_uint32((trig_plane_low & GC_TRIGGER_WATERMARK), !=, 0U);

    /* 掉到 TARGET 以下（仍高于紧急线）：只命中 WATERMARK。 */
    while (a->free_count >= (WORD)FREE_POOL_TARGET)
    {
        alc_free_remove(a, a->free_blocks[0]);
    }
    trig = gc_should_trigger(a);
    munit_assert_uint32((trig & GC_TRIGGER_WATERMARK), !=, 0U);

    ftl_ut_trace_detail("gc trigger: free 0 -> emergency=%d; free 64 -> 0; plane1=%hu fpmin=%hu -> watermark=%d; free %hu (<%u) -> watermark=%d trig=0x%02lX", (int)((trig_empty & GC_TRIGGER_EMERGENCY) != 0U), alc_free_get_count(a, 1U), alc_free_min_plane_count(a), (int)((trig_plane_low & GC_TRIGGER_WATERMARK) != 0U), a->free_count, (unsigned)FREE_POOL_TARGET, (int)((trig & GC_TRIGGER_WATERMARK) != 0U), trig);
    ftl_ut_trace_summary("gc trigger: free 0 -> emergency; 64 -> 0; plane1=%hu (<%u) -> watermark; %hu -> watermark", alc_free_get_count(a, 1U), (unsigned)FREE_POOL_PLANE_LOW, a->free_count);

    return MUNIT_OK;
}

/* gc_select_victim — GC victim 选择（参数 mode=off/idle/emergency）。
 *
 * 覆盖：① 两个 CLOSED SB 的有效 CW 分别为 10 与 248：三种 benefit 模式下都必须选
 *       有效数据更少的那一个，并回报 best_valid_pct/scanned/eligible；
 *       ② 单 plane 空闲失衡时（RISK-ALLOC-09）优先「成对条目 + 饥饿 plane 的单条目」
 *       —— 有效页最少的富-plane 单条目不得被优先；
 *       ③ 偏好组为空时必须回退到普通贪心，不得报无候选。
 * 判据：选错 victim 会把写放大推到极高，且回收后 plane 失衡会让成对分配失败。纯 RAM。 */

static MunitResult test_gc_select_victim(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    WORD            blocks_a[PLANE_NUM];
    WORD            blocks_b[PLANE_NUM];
    gc_select_out_s out;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

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

    /* ── ② 饥饿 plane 偏好（RISK-ALLOC-09）：单 plane 条目在富 plane 上时不得优先 ──
     * free 池：plane0 = 4、plane1 = 2 ⇒ plane1 饥饿（< FREE_POOL_PLANE_LOW）。
     * 再放一个**有效页最少**的单 plane CLOSED 条目（只在 plane0，block 6）：
     * 朴素贪心会选它（cost 最低），但回收它只补富 plane ⇒ 必须选成对条目 a。 */
    for (WORD i = 2U; i < 6U; i++)
    {
        alc_free_push(a, ftl_gblock(0U, i));
    }
    for (WORD i = 2U; i < 4U; i++)
    {
        alc_free_push(a, ftl_gblock(1U, i));
    }

    WORD blocks_c[PLANE_NUM];

    blocks_c[0] = 6U;
    blocks_c[1] = FTL_BLOCK_NO_PLANE;
    munit_assert_uint32(alc_super_closed_add(a, blocks_c, FTL_TEMP_HOT), !=, FTL_BLOCK_NO_SB);
    ftl_block_set_valid(&a->blocks[ftl_gblock(0U, 6U)], 4U);

    munit_assert_uint32(alc_free_get_count(a, 1U), ==, 2U);
    munit_assert_uint32(alc_free_min_plane_count(a), ==, 2U);

    memset(&out, 0, sizeof(out));
    munit_assert_int(gc_select_victim(a, mode, &out), ==, GC_SELECT_OK);
    munit_assert_not_null(out.victim);
    munit_assert_uint32(out.victim->plane_blocks[0], ==, 0U);
    munit_assert_uint32(out.victim->plane_blocks[1], !=, (DWORD)FTL_BLOCK_NO_PLANE);

    const WORD victim_pref = out.victim->plane_blocks[0];

    /* ── ③ 偏好组为空 ⇒ 回退到富 plane 的单条目（不得报 NO_CANDIDATE/无收益）──
     * 只有单 plane 条目、且都不含饥饿 plane：偏好组空，必须是「有候选就回收」，
     * 否则低水位时 GC 反过来拒绝干活，成对分配永久等不到块。 */
    ftl_ut_ram_init();

    a = ftl_ut_ram_alloc();

    WORD blocks_d[PLANE_NUM];

    blocks_d[0] = 6U;
    blocks_d[1] = FTL_BLOCK_NO_PLANE;
    munit_assert_uint32(alc_super_closed_add(a, blocks_d, FTL_TEMP_HOT), !=, FTL_BLOCK_NO_SB);
    ftl_block_set_valid(&a->blocks[ftl_gblock(0U, 6U)], 4U);

    for (WORD i = 2U; i < 6U; i++)
    {
        alc_free_push(a, ftl_gblock(0U, i));
    }
    for (WORD i = 2U; i < 4U; i++)
    {
        alc_free_push(a, ftl_gblock(1U, i));
    }

    memset(&out, 0, sizeof(out));
    munit_assert_int(gc_select_victim(a, mode, &out), ==, GC_SELECT_OK);
    munit_assert_not_null(out.victim);
    munit_assert_uint32(out.victim->plane_blocks[0], ==, 6U);

    const WORD victim_fallback = out.victim->plane_blocks[0];

    ftl_ut_trace_detail("gc victim[%s]: valid a=10 b=%u -> best=%hu%% scanned=%hu eligible=%hu; starved-plane preference picked plane0-block %hu (single-plane block 6 skipped); fallback picked %hu", mode_s, (unsigned)(FTL_CW_PER_BLOCK - 8U), out.best_valid_pct, out.scanned, out.eligible, victim_pref, victim_fallback);
    ftl_ut_trace_summary("gc victim[%s]: valid 10 vs %u -> best=%hu%% scanned=%hu; starved-plane preference + fallback OK", mode_s, (unsigned)(FTL_CW_PER_BLOCK - 8U), out.best_valid_pct, out.scanned);

    return MUNIT_OK;
}

/* wl_trigger_and_victim — WL 启动门与 victim。
 *
 * 覆盖：写入数低于 WL_MIN_WRITES 不触发；空闲块擦除次数高出 used 块 WL_THRESHOLD
 *       后才触发；victim 取擦除次数最高的空闲块（UBI 判据）。
 * 判据：差值门槛防抖，否则 idle 下会无限搬迁（v0.91 修复的形态）。纯 RAM。 */

static MunitResult test_wl_trigger_and_victim(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    WORD        blocks[PLANE_NUM];
    wl_victim_s victim;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

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

/* rr_health — 读恢复的 ECC 健康状态机。
 *
 * 覆盖：HEALTHY→WARNING（连续上升达阈值）→CRITICAL（高水位）→DYING（RR_ECC_MAX）；
 *       之后遇到低 ECC 也不回落。
 * 判据：健康度必须单调——回落会让濒死块重新被当健康块使用。纯 RAM。 */

static MunitResult test_rr_health(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    rr_ecc_track_s track;

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

/* rr_predictive_retire — 预测性退役。
 *
 * 覆盖：块 ECC 达 RR_ECC_MAX 后 rr_predictive_scan 命中该块；rr_retire_block_at 置
 *       RETIRED 并清 read_count/健康计数。
 * 判据：退役块不得再被选块路径取到；清计数是为了让退役记录本身不参与评分。纯 RAM。 */

static MunitResult test_rr_predictive_retire(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    alc_s     *a = ftl_ut_ram_alloc();
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

/* wl_victim_threshold — WL victim 的有效数据门槛。
 *
 * 覆盖：有效 10/256 CW（约 4%）时拒绝搬迁；有效 200/256（约 78%）时接受。
 * 判据：搬高有效块几乎不回收空间却翻倍写放大；门槛是 WL 与 GC 的分工边界。纯 RAM。 */

static MunitResult test_wl_victim_threshold(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    WORD        blocks[PLANE_NUM];
    wl_victim_s victim;

    ftl_ut_trace_begin();

    alc_s *a = ftl_ut_ram_alloc();

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

/* gc_decode_cw_lba — 从 FC desc 的 meta 解 CW 级 LBA。
 *
 * 覆盖：同页基址 → GC_DECODE_OK；全 FTL_LBA_INVALID → EMPTY；某 CW 落在别的页基址
 *       → CORRUPT；只有部分 CW 写过（空洞）→ OK 且空洞保持 INVALID。
 * 判据：GC 按这个结果决定搬哪些 CW；把空洞当有效会搬运垃圾，把 CORRUPT 当 OK
 *       会按错误页基址重建映射。备注：需要 FC 命令槽，但不写 NAND。 */

static MunitResult test_gc_decode_cw_lba(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    fc_cmd_s *cmd = fc_command_alloc();

    munit_assert_not_null(cmd);

    const DWORD pba  = pba_make(0U, 0U, 0U, 5U, 0U, 1U, 0U);
    const DWORD page = 4000U; /* CW_NUM 对齐 */

    cmd->paa = pba_to_paa(pba);

    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        munit_assert_int(fc_command_configure_descriptor(cmd->cmd_id, 0U, c, pba_to_paa(pba)), ==, FC_STATUS_OK);
    }

    DWORD cw[CW_NUM];
    DWORD base = 0U;

    /* OK：四个 CW 同页基址。 */
    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        BYTE *meta = fc_buffer_get_meta(cmd->cmd_id, c);
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
        BYTE *meta = fc_buffer_get_meta(cmd->cmd_id, c);
        DWORD v    = FTL_LBA_INVALID;

        memcpy(meta, &v, sizeof(v));
    }
    munit_assert_int(gc_decode_cw_lba(cmd, pba, cw, &base), ==, GC_DECODE_EMPTY);

    /* CORRUPT：CW1 落在另一页基址。 */
    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        BYTE *meta = fc_buffer_get_meta(cmd->cmd_id, c);
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
        BYTE *meta = fc_buffer_get_meta(cmd->cmd_id, c);
        DWORD v    = (c == 2U) ? (page + 2U) : FTL_LBA_INVALID;

        memcpy(meta, &v, sizeof(v));
    }
    munit_assert_int(gc_decode_cw_lba(cmd, pba, cw, &base), ==, GC_DECODE_OK);
    munit_assert_uint32(base, ==, page);
    munit_assert_uint32(cw[0], ==, FTL_LBA_INVALID);
    munit_assert_uint32(cw[2], ==, page + 2U);

    fc_command_release(cmd->cmd_id);

    ftl_ut_trace_detail("gc decode: pba=0x%08lX page=%lu; OK base=page cw=page+0..3; EMPTY all INVALID; CORRUPT cw1=page+8; HOLE base=%lu cw0=0x%08lX cw2=%lu", pba, page, base, cw[0], cw[2]);
    ftl_ut_trace_summary("gc decode: OK base=%lu; EMPTY/CORRUPT/HOLE ok", base);

    return MUNIT_OK;
}

/* l2p_resident_states — L2P 行状态的三态语义。
 *
 * 覆盖：CLEAN 可读写；write 后转 DIRTY；FLUSHING 可读但写返 FTL_AGAIN；LOADING 读
 *       写都返 FTL_AGAIN；批量掩码读写同源。
 * 判据：LOADING 返回 FTL_AGAIN（「还不知道」）绝不能当成未映射——那会给 host 填假零。
 * 备注：测试直接改 cache 行，结束前恢复行与脏位图，避免污染后续用例。纯 RAM。 */

static MunitResult test_l2p_resident_states(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    ftl_ctx_s   *f      = ftl_ut_live();
    l2p_cache_s *c      = f->cache;
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

    l2p_cache_line_s *line = &c->lines[idx];

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

static l2p_owner_s s_fake_owner;

static const l2p_owner_s *l2p_fake_owner_fn(void *ctx, DWORD global_region)
{
    (void)ctx;
    (void)global_region;
    return &s_fake_owner;
}

/* l2p_miss_unmapped_evict — L2P miss / 合法未映射 / 写回读。
 *
 * 覆盖：miss 首次读起异步加载并返 FTL_AGAIN；重试后为 CLEAN 且条目 NAND_PBA_INVALID
 *       （合法未映射）；写入后读回新值。
 * 判据：未映射是合法结果，加载中是暂态；两者混用会读到假数据。
 * 备注：纯 RAM 不做 NAND 写回——若发现 DIRTY/FLUSHING/LOADING 行直接 SKIP。 */

static MunitResult test_l2p_miss_unmapped_evict(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    ftl_ctx_s   *f = ftl_ut_live();
    l2p_cache_s *c = f->cache;

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

    s_fake_owner.die          = 0U;
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

/* l2p_wal_gate — L2P 区域写回必须等 Journal 落盘（WAL 次序，RISK-L2P-09）。
 *
 * 覆盖：dirty 行的 `dirty_seq` 高于 Journal 的 `persisted_seq` ⇒ `l2p_flush_kick`
 *       退让（FTL_AGAIN、`wal_defer_count` +1、不提交任何写）；水位追上后闸门放行。
 * 判据：区域页先于 Journal 落盘 ⇒ 掉电后「映射在、valid_count 缺」→ 计数漂移 →
 *       GC 按 valid_cw==0 误回收活块（v1.08 已在板上复现并修复）。
 * 备注：纯 RAM、**不制造 I/O** —— 放行侧用 flush_inflight 饱和挡住闸门之后的路径，
 *       结果只可能是 FTL_AGAIN，且 `wal_defer_count` 不再增长（可区分是哪一处退让）。 */

static MunitResult test_l2p_wal_gate(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    ftl_ctx_s   *f = ftl_ut_live();
    l2p_cache_s *c = f->cache;

    l2p_jrn_s journal;

    memset(&journal, 0, sizeof(journal));
    jrn_init(&journal, f->topo, ftl_ut_ram_alloc());

    /* 伪造 owner：region → 本用例的 Journal（只读水位，不做目录/介质访问）。 */
    l2p_owner_fn_t old_fn  = c->owner_of_region;
    void          *old_ctx = c->owner_ctx;

    s_fake_owner.die          = 0U;
    s_fake_owner.metadata_dir = ftl_ut_ram_md();
    s_fake_owner.alloc        = ftl_ut_ram_alloc();
    s_fake_owner.topo         = f->topo;
    s_fake_owner.journal      = &journal;
    c->owner_of_region        = l2p_fake_owner_fn;
    c->owner_ctx              = NULL;

    /* 借第 0 行做实验，结束后原样还原（活实例的行属于开机加载的 region，不能留脏）。 */
    l2p_cache_line_s *line = &c->lines[0];
    l2p_cache_line_s  saved;
    const BYTE        saved_inflight = c->flush_inflight;

    memcpy(&saved, line, sizeof(saved));

    line->state        = L2P_CACHE_DIRTY;
    line->dirty_seq    = 100U;
    line->flush_issued = 0U;
    line->flush_done   = 0U;
    line->flush_fail   = false;

    journal.handle.persisted_seq = 99U;

    /* 放行侧也必须停在这里：inflight 饱和 ⇒ 闸门之后的路径同样只返回 AGAIN，
     * 但不会让 wal_defer_count 增长（这一条把「哪一处退让」区分开）。 */
    c->flush_inflight = L2P_FLUSH_MAX_INFLIGHT;

    const DWORD defer0 = c->wal_defer_count;

    munit_assert_int(l2p_flush_kick(c, 0), ==, FTL_AGAIN);
    munit_assert_uint32(c->wal_defer_count, ==, defer0 + 1U);

    journal.handle.persisted_seq = line->dirty_seq;

    munit_assert_int(l2p_flush_kick(c, 0), ==, FTL_AGAIN);
    munit_assert_uint32(c->wal_defer_count, ==, defer0 + 1U); /* 闸门已放行：退让来自 inflight */

    *line                = saved;
    c->flush_inflight    = saved_inflight;
    c->owner_of_region   = old_fn;
    c->owner_ctx         = old_ctx;
    s_fake_owner.journal = NULL; /* 它指向本函数的栈上 journal：不清会留下悬垂指针 */

    ftl_ut_trace_detail("wal gate: dirty_seq=100 persisted=99 -> AGAIN wd=%lu; persisted=100 -> AGAIN(no defer) wd=%lu", defer0 + 1U, c->wal_defer_count);
    ftl_ut_trace_summary("wal gate: persisted<dirty defers (+%lu); persisted>=dirty passes", 1UL);

    return MUNIT_OK;
}

/* ══ 子套件 /ftl/pure/io/：GC / WL / RR / L2P（纯 RAM）══ */

MunitTest g_ftl_pure_io_tests[] = {
    {           (char *)"policy_entry",            test_policy_entry, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {             (char *)"gc_trigger",              test_gc_trigger, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {       (char *)"gc_select_victim",        test_gc_select_victim, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, s_gc_mode_params},
    {       (char *)"gc_decode_cw_lba",        test_gc_decode_cw_lba, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {  (char *)"wl_trigger_and_victim",   test_wl_trigger_and_victim, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {    (char *)"l2p_resident_states",     test_l2p_resident_states, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {(char *)"l2p_miss_unmapped_evict", test_l2p_miss_unmapped_evict, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {           (char *)"l2p_wal_gate",            test_l2p_wal_gate, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {    (char *)"wl_victim_threshold",     test_wl_victim_threshold, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {              (char *)"rr_health",               test_rr_health, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {   (char *)"rr_predictive_retire",    test_rr_predictive_retire, ftl_ut_pure_setup, ftl_ut_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,             NULL},
    {                             NULL,                         NULL,              NULL,                 NULL,             MUNIT_TEST_OPTION_NONE,             NULL},
};
