/*
 * test_ftl.c — FTL 层 L0 纯 RAM 单元测试（8 套件）
 *
 * 设计文档：docs/design/ftl_ut_munit.md（§4 用例清单、§5 随机化、§8 不做清单）。
 *
 * 定位：**只测 FTL 层自身的纯 RAM 逻辑**——分配器 / 水位 / GC-WL 打分 / ECC 趋势 /
 * BBT / 元数据目录 / pending+完成表 / 完成组。不碰 NAND、不建 desc 链、不推
 * ftl_service（那些属 HIL 级，见 docs/design/hil_ut_design.md）。
 *
 * 夹具：**复用活实例** `g_ctrl.ftl[0][0][0]`（boot 已建），不另开大数组——
 * ftl_context_t ≈ 287KB，AXI_RAM 已放 g_ftl/g_blocks，再开一份装不下
 * （见 docs/design/debug_sbrk_heap_stack_20261004.md §5）。
 *
 * ⚠ 本套件**破坏性**：每个用例开头 ftl_fixture_reset() 会 memset 掉活实例的表
 *   （BST / Free Pool / pending / groups / metadata_dir / journal / wbuf / cp / rr）。
 *   跑完 UT 后该实例不可继续当生产实例用；要恢复只能复位（或 `bbt --erase` 后重启）。
 *   这是 §2.2 的既定代价：UT 是 RAM 内自造状态，与介质内容无关。
 *
 * ⚠ 用例名/套件前缀：munit 只做**首尾相接**、不补分隔符 ⇒ 子套件前缀必须以 '/' 结尾。
 *
 * 与 §4 的已知偏差（实现按**实测代码行为**写，文档为准待订正，见交付报告）：
 *   ① allocator_add_closed_sb 满列表不可测（实现是 ASSERT + 无条件写，非返回码）；
 *   ② checkpoint_journal_watermark **只置不撤**（撤位在 checkpoint_create ⑧，需 NAND）
 *      ⇒ emergency_set_reset 改为钉住「置位 + 回落不清」的实测语义；
 *   ③ rr_ecc_track_t 是 3B、**无环形缓冲** ⇒ record_ecc_ring_depth8 换成
 *      record_ecc_health_monotonic（健康度只升不降）；
 *   ④ bbt_mark_bad 的 bad_count++ 是**无条件**的 ⇒ mark_bad_idempotent 只钉位级幂等，
 *      不钉 bad_count；
 *   ⑤ completion_table_init() 会清掉固件开机注册的 default handler（ftl.c 的 static
 *      ftl_handle_completion，测试无法重装）⇒ ct 用例用**相对计数**、不调 init。
 */

#include "controller/cli.h" /* g_ctrl（活实例路由表，extern） */
#include "ftl/allocator.h"
#include "ftl/bbt.h"
#include "ftl/bst.h"
#include "ftl/checkpoint.h"
#include "ftl/completion_table.h"
#include "ftl/gc.h"
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"
#include "ftl/metadata.h"
#include "ftl/pending.h"
#include "ftl/read_recovery.h"
#include "ftl/wear_leveling.h"
#include "munit.h"
#include "nand/nand_config.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 * §1  夹具
 * ══════════════════════════════════════════════════════════════════════ */

/* 活实例指针。boot 后 g_ctrl.ftl[0][0][0] 恒非 NULL（CLI `test` 是同步命令，
 * 执行期无并发泵，独占该实例）。 */
static ftl_context_t *ftl_fixture(void)
{
    ftl_context_t *F = g_ctrl.ftl[0][0][0];

    munit_assert_not_null(F);
    return F;
}

/*
 * ftl_fixture_reset — 把活实例复位到「开机初始、无介质内容」的 RAM 状态。
 *
 * 范围 = §2.2 表：只清会被用例写的子结构；**F->topo 保持不动**（reset 不该改路由）。
 * 顺序有依赖：先 memset 再 init（init 只建固定表/游标，不 memset 整表）。
 */
static void ftl_fixture_reset(void)
{
    ftl_context_t *F = ftl_fixture();

    memset(&F->tables, 0, sizeof(F->tables));
    memset(&F->metadata_dir, 0, sizeof(F->metadata_dir));
    memset(F->pending, 0, sizeof(F->pending));
    memset(F->groups, 0, sizeof(F->groups));
    memset(&F->wbuf, 0, sizeof(F->wbuf));
    memset(&F->cp, 0, sizeof(F->cp));
    memset(&F->rr, 0, sizeof(F->rr));
    memset(F->rr_page_scratch, 0, sizeof(F->rr_page_scratch));

    F->cp_timer_start  = 0U;
    F->total_writes    = 0U;
    F->rr_last_scan_ms = 0U;
    F->bg_suspend      = 0U;

    /* 分配器（绑表指针 + 全块置 FREE/无归属；**不回填 Free Pool**，见 §2 行 5） */
    (void)allocator_init(&F->alloc, F->topo, &F->tables);

    /* 元数据目录：fill_fixed 用 dir->topo.lun 展开固定区地址 ⇒ 必须先绑 topo */
    F->metadata_dir.topo = F->topo;
    metadata_dir_init(&F->metadata_dir);

    /* L2P cache（不碰 md_dir / alloc 绑定，见 l2p_cache_init 头注） */
    l2p_cache_init(&F->cache, F->topo.ch, F->topo.ce, F->topo.lun);

    /* Journal（复位游标 + block_pba 展开 + page_buf 0xFF） */
    journal_init(&F->journal, F->topo);
}

/* setup 一律返回 NULL：夹具是**活实例**，不是 malloc 的私有块（§2.2）。
 * 返回 NULL 是合法契约（测试体用 ftl_fixture() 自己取指针）。 */
static void *ftl_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    ftl_fixture_reset();
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 * §2  辅助
 * ══════════════════════════════════════════════════════════════════════ */

/* 随机数加盐：munit 每个用例开头用 runner.seed 重新播种（munit.c），不加盐则
 * 各用例的首抽样相同（§5.1）。FNV-1a(tag) 异或进去 ⇒ 每个 tag 一条独立流。 */
static uint32_t ftl_rand(const char *tag)
{
    uint32_t h = 2166136261UL; /* FNV-1a offset basis */

    while (*tag != '\0')
    {
        h ^= (uint8_t)*tag++;
        h *= 16777619UL;
    }

    return munit_rand_uint32() ^ h;
}

/* [lo, hi] 闭区间随机 */
static uint32_t ftl_rand_range(const char *tag, uint32_t lo, uint32_t hi)
{
    return lo + (ftl_rand(tag) % ((hi - lo) + 1U));
}

/*
 * ftl_fixture_backfill — 把 [first, first+count) 内的**好块**回填进 Free Pool。
 *
 * 复刻 src/main.c 开机回填循环：跳过坏块 + 跳过固定元数据块（取模到 per-plane 号
 * 后判）。allocator_init 只标状态不入池（§2 行 5），要测分配必须先回填。
 *
 * 返回实际入池块数（= 跳过坏块/元数据块之后的净增数）。
 */
static uint16_t ftl_fixture_backfill(ftl_allocator_t *alloc, uint16_t first, uint16_t count)
{
    uint16_t g;
    uint16_t added = 0U;

    for (g = first; (g < (uint16_t)(first + count)) && (g < FTL_BLOCK_TOTAL); g++)
    {
        if (ftl_block_is_bad(&alloc->blocks[g]))
        {
            continue;
        }
        if (ftl_is_metadata_block(NULL, (uint16_t)(g % BLOCKS_PER_PLANE)) != 0)
        {
            continue;
        }
        allocator_free_block(alloc, g);
        added++;
    }

    return added;
}

/*
 * ftl_ut_put_closed_sb — 直接在 closed_sb_list[idx] 造一条 Closed 条目。
 *
 * 不走 allocator_add_closed_sb 是为了让 GC/WL 用例能**逐字段**摆弄（valid_count /
 * erase_count / read_count 由调用者自己写 BST）。会按需把 closed_sb_count 顶到 idx+1。
 */
static void ftl_ut_put_closed_sb(ftl_allocator_t *alloc,
                                 uint16_t         idx,
                                 uint16_t         blk0,
                                 uint16_t         blk1,
                                 uint8_t          temperature)
{
    super_block_t *sb = &alloc->closed_sb_list[idx];

    memset(sb, 0, sizeof(*sb));
    sb->topo.ch         = alloc->ch;
    sb->topo.ce         = alloc->ce;
    sb->topo.lun        = alloc->lun;
    sb->plane_blocks[0] = blk0;
    sb->plane_blocks[1] = blk1;
    sb->next_page       = (uint8_t)PAGES_PER_BLOCK; /* 满 */
    sb->temperature     = temperature;
    sb->valid           = 0U; /* 0 = Closed（gc/wl 的入选前提） */

    if ((uint16_t)(idx + 1U) > alloc->closed_sb_count)
    {
        alloc->closed_sb_count = (uint16_t)(idx + 1U);
    }
}

/* 造一个 nand_paa_t（只填 PAA 关心的字段；ch/ce 不入 PAA）。 */
static nand_paa_t ftl_ut_paa(uint8_t lun, uint16_t block, uint8_t plane, uint8_t page)
{
    nand_addr_t addr = {.ch = 0U, .ce = 0U, .lun = lun, .block = block, .plane = plane, .page = page, .cw = 0U};

    return paa_from_addr(&addr);
}

/* 完成表 handler 桩（只为验证 lookup 取回的是同一个函数指针）。 */
static void ftl_ut_completion_stub(const fc_command_status_t *completion, void *ctx)
{
    (void)completion;
    (void)ctx;
}

/* 组完成回调计数桩（group_complete_once 用）。 */
static uint16_t g_ut_cb_count;

static void ftl_ut_group_cb(void *ctx)
{
    (void)ctx;
    g_ut_cb_count++;
}

/* 可重入回调桩（group_reentrant_callback 用）：回调里再申请一个组槽。 */
static ftl_context_t *g_ut_reent_ftl;
static int            g_ut_reent_new_gi;
static uint16_t       g_ut_reent_calls;

static void ftl_ut_reentrant_cb(void *ctx)
{
    (void)ctx;
    g_ut_reent_calls++;
    g_ut_reent_new_gi = pending_group_alloc(g_ut_reent_ftl);
}

/* ══════════════════════════════════════════════════════════════════════
 * §3  套件 1 — Allocator（Free Pool / Open SB / Closed SB / 反向索引）
 * ══════════════════════════════════════════════════════════════════════ */

/* 复位后：三个池计数为 0、无 Open/Closed SB、全块 FREE 且无归属。 */
static MunitResult test_alloc_init_zeroes_pool(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a = &ftl_fixture()->alloc;
    uint16_t         g;

    (void)params;
    (void)fixture;

    munit_assert_uint16(a->free_count, ==, 0U);
    munit_assert_uint16(a->op_count, ==, 0U);
    munit_assert_uint16(a->spare_count, ==, 0U);
    munit_assert_uint16(a->closed_sb_count, ==, 0U);
    munit_assert_uint16(a->bad_count, ==, 0U);
    munit_assert_uint16(a->total_blocks, ==, 0U);
    munit_assert_uint8(a->hot_sb.valid, ==, 0U);
    munit_assert_uint8(a->cold_sb.valid, ==, 0U);

    for (g = 0U; g < (uint16_t)FTL_BLOCK_TOTAL; g++)
    {
        munit_assert_true(ftl_block_state_is(&a->blocks[g], FTL_BLOCK_STATE_FREE));
        munit_assert_uint16(ftl_block_sb_id(&a->blocks[g]), ==, FTL_BLOCK_NO_SB);
    }

    return MUNIT_OK;
}

/* 回填按序入池；入池块状态仍是 FREE 且无归属（回填只还编号，不改状态机）。 */
static MunitResult test_alloc_backfill_fills_pool(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a = &ftl_fixture()->alloc;
    uint16_t         n;
    uint16_t         i;

    (void)params;
    (void)fixture;

    n = ftl_fixture_backfill(a, 200U, 100U);

    munit_assert_uint16(n, ==, 100U);
    munit_assert_uint16(a->free_count, ==, 100U);

    for (i = 0U; i < 100U; i++)
    {
        uint16_t gb = a->free_blocks[i];

        munit_assert_uint16(gb, ==, (uint16_t)(200U + i));
        munit_assert_true(ftl_block_state_is(&a->blocks[gb], FTL_BLOCK_STATE_FREE));
        munit_assert_uint16(ftl_block_sb_id(&a->blocks[gb]), ==, FTL_BLOCK_NO_SB);
    }

    return MUNIT_OK;
}

/* allocator_free_block 清 OPEN/CLOSED/RESERVED/TEMP 与反向索引，并置 FREE。 */
static MunitResult test_alloc_free_block_clears_state(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a = &ftl_fixture()->alloc;
    uint16_t         g = 300U;
    ftl_block_t     *b = &a->blocks[g];

    (void)params;
    (void)fixture;

    ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE);
    ftl_block_state_set(b,
                        FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED | FTL_BLOCK_STATE_RESERVED |
                            FTL_BLOCK_STATE_TEMP_HOT);
    ftl_block_set_sb(b, 7U);

    allocator_free_block(a, g);

    munit_assert_true(ftl_block_state_is(b, FTL_BLOCK_STATE_FREE));
    munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_OPEN));
    munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_CLOSED));
    munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_RESERVED));
    munit_assert_uint8(ftl_block_get_temp(b), ==, 0U);
    munit_assert_uint16(ftl_block_sb_id(b), ==, FTL_BLOCK_NO_SB);
    munit_assert_uint16(a->free_count, ==, 1U);
    munit_assert_uint16(a->free_blocks[0], ==, g);

    return MUNIT_OK;
}

/* alloc_pair 首次开 SB → 写页 0 → 关闭后进 Closed 列表、两 plane 块置 CLOSED、
 * 反向索引 = 列表下标、next_page = 1。 */
static MunitResult test_alloc_close_open_sb(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a = &ftl_fixture()->alloc;
    nand_pba_t       out[PLANE_NUM];
    uint8_t          p;

    (void)params;
    (void)fixture;

    munit_assert_uint16(ftl_fixture_backfill(a, 200U, 32U), ==, 32U);
    munit_assert_uint16(ftl_fixture_backfill(a, (uint16_t)(BLOCKS_PER_PLANE + 200U), 32U), ==, 32U);

    munit_assert_uint32(allocator_alloc_pair(a, FTL_TEMP_HOT, out), !=, (uint32_t)NAND_PBA_INVALID);
    munit_assert_uint8(a->hot_sb.valid, ==, 1U);
    munit_assert_uint8(a->hot_sb.next_page, ==, 1U);

    munit_assert_int(allocator_close_open_sb(a, FTL_TEMP_HOT), ==, FTL_OK);

    munit_assert_uint16(a->closed_sb_count, ==, 1U);
    munit_assert_uint8(a->hot_sb.valid, ==, 0U); /* 关闭后 Open 槽失效 */
    munit_assert_uint8(a->closed_sb_list[0].valid, ==, 0U);
    munit_assert_uint8(a->closed_sb_list[0].next_page, ==, 1U);
    munit_assert_uint8(a->closed_sb_list[0].temperature, ==, FTL_TEMP_HOT);

    for (p = 0U; p < PLANE_NUM; p++)
    {
        uint16_t gb = ftl_gblock(p, a->closed_sb_list[0].plane_blocks[p]);

        munit_assert_true(ftl_block_state_is(&a->blocks[gb], FTL_BLOCK_STATE_CLOSED));
        munit_assert_false(ftl_block_state_is(&a->blocks[gb], FTL_BLOCK_STATE_OPEN));
        munit_assert_false(ftl_block_state_is(&a->blocks[gb], FTL_BLOCK_STATE_FREE));
        munit_assert_uint16(ftl_block_sb_id(&a->blocks[gb]), ==, 0U);
    }

    /* 再关一次：Open 槽已失效 ⇒ 拒绝 */
    munit_assert_int(allocator_close_open_sb(a, FTL_TEMP_HOT), ==, FTL_ERROR);

    return MUNIT_OK;
}

/* add_closed_sb：条目字段（next_page=64/valid=0/温度/拓扑）+ 两 plane 块置 CLOSED + 反向索引。 */
static MunitResult test_alloc_add_closed_sb(const MunitParameter params[], void *fixture)
{
    /* ⚠ plane_blocks[] 存的是 **plane 内块号**（< BLOCKS_PER_PLANE=2048），不是 gblock：
     * allocator_open_sb 写它时取 `gblock % BLOCKS_PER_PLANE`，下游一律 ftl_gblock() 还原。
     * 填 gblock（如 2100）会让 ftl_gblock(1,2100)=4148 ≥ FTL_BLOCK_TOTAL ⇒ 越界踩 free_blocks。 */
    ftl_allocator_t *a             = &ftl_fixture()->alloc;
    uint16_t         pb[PLANE_NUM] = {100U, 152U};
    uint16_t         idx;
    uint8_t          p;

    (void)params;
    (void)fixture;

    idx = allocator_add_closed_sb(a, pb, FTL_TEMP_COLD);

    munit_assert_uint16(idx, ==, 0U);
    munit_assert_uint16(a->closed_sb_count, ==, 1U);
    munit_assert_uint16(a->closed_sb_list[0].plane_blocks[0], ==, 100U);
    munit_assert_uint16(a->closed_sb_list[0].plane_blocks[1], ==, 152U);
    munit_assert_uint8(a->closed_sb_list[0].next_page, ==, PAGES_PER_BLOCK);
    munit_assert_uint8(a->closed_sb_list[0].temperature, ==, FTL_TEMP_COLD);
    munit_assert_uint8(a->closed_sb_list[0].valid, ==, 0U);
    munit_assert_uint8(a->closed_sb_list[0].topo.ch, ==, a->ch);
    munit_assert_uint8(a->closed_sb_list[0].topo.ce, ==, a->ce);
    munit_assert_uint8(a->closed_sb_list[0].topo.lun, ==, a->lun);

    for (p = 0U; p < PLANE_NUM; p++)
    {
        uint16_t gb = ftl_gblock(p, pb[p]);

        munit_assert_true(ftl_block_state_is(&a->blocks[gb], FTL_BLOCK_STATE_CLOSED));
        munit_assert_false(ftl_block_state_is(&a->blocks[gb], FTL_BLOCK_STATE_FREE));
        munit_assert_false(ftl_block_state_is(&a->blocks[gb], FTL_BLOCK_STATE_OPEN));
        munit_assert_uint8(ftl_block_get_temp(&a->blocks[gb]), ==, FTL_TEMP_COLD);
        munit_assert_uint16(ftl_block_sb_id(&a->blocks[gb]), ==, 0U);
    }

    /* NULL 参数：返回哨兵、不写 */
    munit_assert_uint16(allocator_add_closed_sb(a, NULL, FTL_TEMP_COLD), ==, UINT16_MAX);
    munit_assert_uint16(a->closed_sb_count, ==, 1U);

    return MUNIT_OK;
}

/* swap-remove：末位搬到被移除位并修正其两 block 的 sb_id；被移除者的 block 复位。 */
static MunitResult test_alloc_remove_closed_sb_swap_remove(const MunitParameter params[], void *fixture)
{
    /* 同 add_closed_sb：plane_blocks[] 是 plane 内块号（< 2048），不是 gblock。 */
    ftl_allocator_t *a               = &ftl_fixture()->alloc;
    uint16_t         pb_a[PLANE_NUM] = {100U, 150U};
    uint16_t         pb_b[PLANE_NUM] = {101U, 151U};
    uint16_t         pb_c[PLANE_NUM] = {102U, 152U};

    (void)params;
    (void)fixture;

    (void)allocator_add_closed_sb(a, pb_a, FTL_TEMP_COLD); /* idx 0 */
    (void)allocator_add_closed_sb(a, pb_b, FTL_TEMP_COLD); /* idx 1 */
    (void)allocator_add_closed_sb(a, pb_c, FTL_TEMP_COLD); /* idx 2 */
    munit_assert_uint16(a->closed_sb_count, ==, 3U);

    allocator_remove_closed_sb(a, 0U);

    munit_assert_uint16(a->closed_sb_count, ==, 2U);

    /* C 从 idx 2 搬到 idx 0 */
    munit_assert_uint16(a->closed_sb_list[0].plane_blocks[0], ==, 102U);
    munit_assert_uint16(a->closed_sb_list[0].plane_blocks[1], ==, 152U);
    munit_assert_uint16(ftl_block_sb_id(&a->blocks[ftl_gblock(0U, 102U)]), ==, 0U);
    munit_assert_uint16(ftl_block_sb_id(&a->blocks[ftl_gblock(1U, 152U)]), ==, 0U);

    /* A 的 block 恢复未归属 */
    munit_assert_uint16(ftl_block_sb_id(&a->blocks[ftl_gblock(0U, 100U)]), ==, FTL_BLOCK_NO_SB);
    munit_assert_uint16(ftl_block_sb_id(&a->blocks[ftl_gblock(1U, 150U)]), ==, FTL_BLOCK_NO_SB);

    /* B 留在 idx 1，未受影响 */
    munit_assert_uint16(a->closed_sb_list[1].plane_blocks[0], ==, 101U);
    munit_assert_uint16(ftl_block_sb_id(&a->blocks[ftl_gblock(0U, 101U)]), ==, 1U);

    /* 越界下标：no-op（不崩、不改计数） */
    allocator_remove_closed_sb(a, 5U);
    munit_assert_uint16(a->closed_sb_count, ==, 2U);

    return MUNIT_OK;
}

/* remove_from_free 命中即出池；未入池的块调用是 no-op。 */
static MunitResult test_alloc_remove_from_free(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a = &ftl_fixture()->alloc;
    uint16_t         i;

    (void)params;
    (void)fixture;

    munit_assert_uint16(ftl_fixture_backfill(a, 200U, 3U), ==, 3U);
    munit_assert_uint16(a->free_count, ==, 3U);

    allocator_remove_from_free(a, 201U);

    munit_assert_uint16(a->free_count, ==, 2U);
    for (i = 0U; i < a->free_count; i++)
    {
        munit_assert_uint16(a->free_blocks[i], !=, 201U);
    }

    /* 不在池内（含越界）⇒ 计数不变 */
    allocator_remove_from_free(a, 999U);
    munit_assert_uint16(a->free_count, ==, 2U);

    return MUNIT_OK;
}

/* 独占块：从 Free Pool 取（置 RESERVED + 清 FREE + 清归属）；归还走 free_block 语义；
 * 空 plane 拒绝（不向 OP 池借）。 */
static MunitResult test_alloc_reserved_alloc_free(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a  = &ftl_fixture()->alloc;
    uint16_t         g0 = 0U;
    uint16_t         g1 = 0U;

    (void)params;
    (void)fixture;

    munit_assert_uint16(ftl_fixture_backfill(a, 200U, 4U), ==, 4U);

    munit_assert_int(allocator_alloc_reserved_block(a, 0U, &g0), ==, FTL_OK);
    munit_assert_uint16(g0, ==, 200U);
    munit_assert_uint16(a->free_count, ==, 3U);
    munit_assert_true(ftl_block_state_is(&a->blocks[g0], FTL_BLOCK_STATE_RESERVED));
    munit_assert_false(ftl_block_state_is(&a->blocks[g0], FTL_BLOCK_STATE_FREE));
    munit_assert_uint16(ftl_block_sb_id(&a->blocks[g0]), ==, FTL_BLOCK_NO_SB);

    /* 已取走的块不会再被选中：取走 200 时 allocator_pool_remove 是 swap-remove
     * （free_blocks[i] = free_blocks[--free_count]）⇒ 池变 [203,201,202]，首个最小擦除
     * 块 = 203（不是 201）。*/
    munit_assert_int(allocator_alloc_reserved_block(a, 0U, &g1), ==, FTL_OK);
    munit_assert_uint16(g1, ==, 203U);

    /* plane1 池空 ⇒ 拒绝（allocator_alloc_reserved_block 不向 OP 池借） */
    munit_assert_int(allocator_alloc_reserved_block(a, 1U, &g1), ==, FTL_ERROR);

    /* 归还：RESERVED 清、FREE 置、归属复位 */
    allocator_free_reserved_block(a, g0);
    munit_assert_true(ftl_block_state_is(&a->blocks[g0], FTL_BLOCK_STATE_FREE));
    munit_assert_false(ftl_block_state_is(&a->blocks[g0], FTL_BLOCK_STATE_RESERVED));
    munit_assert_uint16(ftl_block_sb_id(&a->blocks[g0]), ==, FTL_BLOCK_NO_SB);

    /* 越界 / NULL 拒绝 */
    munit_assert_int(allocator_alloc_reserved_block(a, PLANE_NUM, &g1), ==, FTL_ERROR);
    munit_assert_int(allocator_alloc_reserved_block(a, 0U, NULL), ==, FTL_ERROR);

    return MUNIT_OK;
}

/* 开 SB 只从 Free Pool 拿（回填跳过固定元数据块）⇒ 分配出的块恒 ≥ FTL_METADATA_BLOCK_END；
 * 固定区 0..62 仍在 BST 里是 FREE 但**不入池**、不被消耗。 */
static MunitResult test_alloc_open_sb_never_picks_metadata(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a = &ftl_fixture()->alloc;
    nand_pba_t       out[PLANE_NUM];
    uint16_t         b;
    uint8_t          p;

    (void)params;
    (void)fixture;

    munit_assert_uint16(ftl_fixture_backfill(a, 100U, 16U), ==, 16U);
    munit_assert_uint16(ftl_fixture_backfill(a, (uint16_t)(BLOCKS_PER_PLANE + 100U), 16U), ==, 16U);

    for (b = 0U; b < (uint16_t)FTL_METADATA_BLOCK_END; b++)
    {
        munit_assert_true(ftl_block_state_is(&a->blocks[b], FTL_BLOCK_STATE_FREE));
        munit_assert_uint16(ftl_block_sb_id(&a->blocks[b]), ==, FTL_BLOCK_NO_SB);
    }

    munit_assert_uint32(allocator_alloc_pair(a, FTL_TEMP_HOT, out), !=, (uint32_t)NAND_PBA_INVALID);

    for (p = 0U; p < PLANE_NUM; p++)
    {
        uint16_t blk = (uint16_t)pba_to_block(out[p]);

        munit_assert_uint8((uint8_t)pba_to_plane(out[p]), ==, p);
        munit_assert_true(blk >= (uint16_t)FTL_METADATA_BLOCK_END);
        munit_assert_true(ftl_block_state_is(&a->blocks[ftl_gblock(p, blk)], FTL_BLOCK_STATE_OPEN));
    }

    /* 元数据块一个都没被吃掉 */
    for (b = 0U; b < (uint16_t)FTL_METADATA_BLOCK_END; b++)
    {
        munit_assert_true(ftl_block_state_is(&a->blocks[b], FTL_BLOCK_STATE_FREE));
    }

    return MUNIT_OK;
}

/* 逐 plane 独立选块：选中的是**该 plane 池内擦除次数最小**的块（随机擦除分布）。 */
static MunitResult test_alloc_open_sb_min_erase_count(const MunitParameter params[], void *fixture)
{
    const char      *tag = "alloc/min_erase";
    ftl_allocator_t *a   = &ftl_fixture()->alloc;
    nand_pba_t       out[PLANE_NUM];
    uint32_t         min0 = UINT32_MAX;
    uint32_t         min1 = UINT32_MAX;
    uint16_t         i;

    (void)params;
    (void)fixture;

    munit_assert_uint16(ftl_fixture_backfill(a, 200U, 16U), ==, 16U);
    munit_assert_uint16(ftl_fixture_backfill(a, (uint16_t)(BLOCKS_PER_PLANE + 200U), 16U), ==, 16U);

    for (i = 0U; i < 16U; i++)
    {
        uint32_t e0 = ftl_rand_range(tag, 1U, 4000U);
        uint32_t e1 = ftl_rand_range(tag, 1U, 4000U);

        a->blocks[(uint16_t)(200U + i)].erase_count                    = e0;
        a->blocks[(uint16_t)(BLOCKS_PER_PLANE + 200U + i)].erase_count = e1;

        if (e0 < min0)
        {
            min0 = e0;
        }
        if (e1 < min1)
        {
            min1 = e1;
        }
    }

    munit_assert_uint32(allocator_alloc_pair(a, FTL_TEMP_HOT, out), !=, (uint32_t)NAND_PBA_INVALID);

    munit_assert_uint32(a->blocks[ftl_pba_gblock(out[0])].erase_count, ==, min0);
    munit_assert_uint32(a->blocks[ftl_pba_gblock(out[1])].erase_count, ==, min1);

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §4  套件 2 — Journal 水位 + Checkpoint 触发
 * ══════════════════════════════════════════════════════════════════════ */

/* used < 85% ⇒ 两位都不置。used = (global_page*100)/(JOURNAL_BLOCK_COUNT*64) = gpage*100/256。 */
static MunitResult test_wm_below_85(const MunitParameter params[], void *fixture)
{
    l2p_journal_t *j = &ftl_fixture()->journal;

    (void)params;
    (void)fixture;

    /* 全局页 217/256 = 84%（85% 门限的第一页是 218） */
    j->current_block = 3U;
    j->current_page  = 25U;
    j->current_entry = 0U;

    checkpoint_journal_watermark(j);

    munit_assert_uint8(j->checkpoint_needed, ==, 0U);
    munit_assert_uint8(j->emergency, ==, 0U);

    return MUNIT_OK;
}

/* 85% ≤ used < 95% ⇒ checkpoint_needed（**不含** emergency）。 */
static MunitResult test_wm_at_85(const MunitParameter params[], void *fixture)
{
    const char    *tag   = "wm/at85";
    l2p_journal_t *j     = &ftl_fixture()->journal;
    uint32_t       gpage = ftl_rand_range(tag, 218U, 243U); /* 218→85% … 243→94% */

    (void)params;
    (void)fixture;

    j->current_block = (uint8_t)(gpage / PAGES_PER_BLOCK);
    j->current_page  = (uint8_t)(gpage % PAGES_PER_BLOCK);
    j->current_entry = (uint8_t)ftl_rand_range(tag, 0U, (uint32_t)JOURNAL_ENTRIES_PER_PAGE - 1U);

    checkpoint_journal_watermark(j);

    munit_assert_uint8(j->checkpoint_needed, ==, 1U);
    munit_assert_uint8(j->emergency, ==, 0U);

    return MUNIT_OK;
}

/* used ≥ 95% ⇒ emergency（else-if 分支，不另置 checkpoint_needed）。 */
static MunitResult test_wm_at_95_emergency(const MunitParameter params[], void *fixture)
{
    const char    *tag   = "wm/at95";
    l2p_journal_t *j     = &ftl_fixture()->journal;
    uint32_t       gpage = ftl_rand_range(tag, 244U, 255U); /* 244→95% … 255→99% */

    (void)params;
    (void)fixture;

    j->current_block = (uint8_t)(gpage / PAGES_PER_BLOCK);
    j->current_page  = (uint8_t)(gpage % PAGES_PER_BLOCK);
    j->current_entry = 0U;

    checkpoint_journal_watermark(j);

    munit_assert_uint8(j->emergency, ==, 1U);

    return MUNIT_OK;
}

/*
 * 水位**只置不撤**：checkpoint_journal_watermark 没有清位路径（清位在
 * checkpoint_create ⑧ 之后，需真 NAND ⇒ 属 L2）。此处钉住该语义，防止未来
 * 有人在纯函数里加"回落清位"而悄悄改变 checkpoint 触发时机。
 */
static MunitResult test_wm_emergency_set_reset(const MunitParameter params[], void *fixture)
{
    l2p_journal_t *j = &ftl_fixture()->journal;

    (void)params;
    (void)fixture;

    j->current_block = 3U;
    j->current_page  = 52U; /* 全局页 244/256 = 95% */
    j->current_entry = 0U;

    checkpoint_journal_watermark(j);
    munit_assert_uint8(j->emergency, ==, 1U);

    /* 回落到 0% 再求值：不清位（撤位在 checkpoint_create ⑧） */
    j->current_block = 0U;
    j->current_page  = 0U;
    checkpoint_journal_watermark(j);
    munit_assert_uint8(j->emergency, ==, 1U);

    return MUNIT_OK;
}

/* 三条触发：T1 水位位 / T2 total_writes 整除 CP_WRITE_INTERVAL / T3 超时。 */
static MunitResult test_ckpt_trigger_T1_T2_T3(const MunitParameter params[], void *fixture)
{
    ftl_context_t *F = ftl_fixture();

    (void)params;
    (void)fixture;

    /* 复位态：三条都不命中 */
    munit_assert_int(checkpoint_check_trigger(F, 0U), ==, 0);
    munit_assert_int(checkpoint_check_trigger(F, CP_TIME_INTERVAL_MS - 1U), ==, 0);

    /* T1 */
    F->journal.checkpoint_needed = 1U;
    munit_assert_int(checkpoint_check_trigger(F, 0U), ==, 1);
    F->journal.checkpoint_needed = 0U;

    /* T2：> 0 且整除（0 不触发——防开机即命中） */
    F->total_writes = (uint32_t)CP_WRITE_INTERVAL;
    munit_assert_int(checkpoint_check_trigger(F, 0U), ==, 1);
    F->total_writes = (uint32_t)CP_WRITE_INTERVAL + 1U;
    munit_assert_int(checkpoint_check_trigger(F, 0U), ==, 0);
    F->total_writes = 0U;
    munit_assert_int(checkpoint_check_trigger(F, 0U), ==, 0);

    /* T3 */
    munit_assert_int(checkpoint_check_trigger(F, CP_TIME_INTERVAL_MS), ==, 1);

    /* NULL 不崩 */
    munit_assert_int(checkpoint_check_trigger(NULL, CP_TIME_INTERVAL_MS), ==, 0);

    return MUNIT_OK;
}

/* is_emergency 直读 journal.emergency，与水位函数一致。 */
static MunitResult test_ckpt_is_emergency(const MunitParameter params[], void *fixture)
{
    ftl_context_t *F = ftl_fixture();

    (void)params;
    (void)fixture;

    munit_assert_int(checkpoint_is_emergency(F), ==, 0);

    F->journal.emergency = 1U;
    munit_assert_int(checkpoint_is_emergency(F), ==, 1);

    F->journal.emergency = 0U;
    munit_assert_int(checkpoint_is_emergency(F), ==, 0);

    /* 与水位函数一致：≥95%（全局页 244/256）⇒ 紧急 */
    F->journal.current_block = 3U;
    F->journal.current_page  = 52U;
    checkpoint_journal_watermark(&F->journal);
    munit_assert_int(checkpoint_is_emergency(F), ==, 1);

    munit_assert_int(checkpoint_is_emergency(NULL), ==, 0);

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §5  套件 3 — GC victim 打分 + GC/WL 触发
 * ══════════════════════════════════════════════════════════════════════ */

/* cost 的 vr 项 ∝ 有效 CW ⇒ 随机有效数下 argmin 有效数的 SB 胜出。 */
static MunitResult test_gc_victim_lowest_vpc(const MunitParameter params[], void *fixture)
{
    const char      *tag              = "gc/victim";
    ftl_allocator_t *a                = &ftl_fixture()->alloc;
    uint16_t         pb[3][PLANE_NUM] = {
        {10U, 2010U},
        {20U, 2020U},
        {30U, 2030U}
    };
    uint16_t       valid[3];
    uint16_t       arg    = 0U;
    uint16_t       best   = UINT16_MAX;
    super_block_t *victim = NULL;
    uint8_t        i;

    (void)params;
    (void)fixture;

    for (i = 0U; i < 3U; i++)
    {
        valid[i] = (uint16_t)ftl_rand_range(tag, 1U, 500U);
        ftl_ut_put_closed_sb(a, i, pb[i][0], pb[i][1], FTL_TEMP_COLD);
        a->blocks[ftl_gblock(0U, pb[i][0])].valid_count = valid[i];
        a->blocks[ftl_gblock(1U, pb[i][1])].valid_count = valid[i];

        if (valid[i] < best)
        {
            best = valid[i];
            arg  = i;
        }
    }

    munit_assert_int(gc_select_victim(a, &victim), ==, 1);
    munit_assert_not_null(victim);
    munit_assert_uint16(victim->plane_blocks[0], ==, pb[arg][0]);
    munit_assert_uint16(victim->plane_blocks[1], ==, pb[arg][1]);

    return MUNIT_OK;
}

/* valid != 0（Open）的条目一律跳过；全 Open ⇒ 无 victim。 */
static MunitResult test_gc_victim_skips_open(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a      = &ftl_fixture()->alloc;
    super_block_t   *victim = NULL;

    (void)params;
    (void)fixture;

    /* 条目 0：Open 且有效数最低——必须被跳过 */
    ftl_ut_put_closed_sb(a, 0U, 10U, 2010U, FTL_TEMP_COLD);
    a->closed_sb_list[0].valid                   = 1U;
    a->blocks[ftl_gblock(0U, 10U)].valid_count   = 0U;
    a->blocks[ftl_gblock(1U, 2010U)].valid_count = 0U;

    /* 条目 1：Closed，有效数高 */
    ftl_ut_put_closed_sb(a, 1U, 20U, 2020U, FTL_TEMP_COLD);
    a->blocks[ftl_gblock(0U, 20U)].valid_count   = 50U;
    a->blocks[ftl_gblock(1U, 2020U)].valid_count = 50U;

    munit_assert_int(gc_select_victim(a, &victim), ==, 1);
    munit_assert_not_null(victim);
    munit_assert_uint16(victim->plane_blocks[0], ==, 20U);

    /* 连条目 1 也变 Open ⇒ 无 victim */
    a->closed_sb_list[1].valid = 1U;
    victim                     = NULL;
    munit_assert_int(gc_select_victim(a, &victim), ==, 0);
    munit_assert_null(victim);

    return MUNIT_OK;
}

/* T1/T4 建在 free_count；T5 建在全表擦除比（max≥64 地板 + min>0 + 比>10）。 */
static MunitResult test_gc_trigger_T1_T4_T5(const MunitParameter params[], void *fixture)
{
    const char      *tag = "gc/trigger";
    ftl_allocator_t *a   = &ftl_fixture()->alloc;
    uint16_t         g;
    uint32_t         t1_lo = (uint32_t)FREE_POOL_EMERGENCY + 1U;
    uint32_t         t1_hi = (uint32_t)FREE_POOL_TARGET - 1U;

    (void)params;
    (void)fixture;

    /* T1 only：8 < free_count < 32 */
    a->free_count = (uint16_t)ftl_rand_range(tag, t1_lo, t1_hi);
    munit_assert_uint32(gc_check_triggers(a), ==, (uint32_t)GC_TRIGGER_WATERMARK);

    /* T1 + T4：free_count ≤ 8（T1 同源共触发） */
    a->free_count = (uint16_t)ftl_rand_range(tag, 0U, (uint32_t)FREE_POOL_EMERGENCY);
    munit_assert_uint32(gc_check_triggers(a), ==, (uint32_t)(GC_TRIGGER_WATERMARK | GC_TRIGGER_EMERGENCY));

    /* 水位充足 ⇒ T1/T4 都不命中 */
    a->free_count = (uint16_t)(FREE_POOL_TARGET + 1U);
    munit_assert_uint32(gc_check_triggers(a), ==, 0U);

    /* T5：全表 erase=1，随机一块 640（≥64 地板；640/1 > 10） */
    for (g = 0U; g < (uint16_t)FTL_BLOCK_TOTAL; g++)
    {
        a->blocks[g].erase_count = 1U;
    }
    a->blocks[(uint16_t)ftl_rand_range(tag, 0U, (uint32_t)FTL_BLOCK_TOTAL - 1U)].erase_count = 640U;
    munit_assert_uint32(gc_check_triggers(a), ==, (uint32_t)GC_TRIGGER_WEAR);

    /* min_erase == 0 ⇒ 比值无意义，T5 不触发 */
    a->blocks[0].erase_count = 0U;
    munit_assert_uint32(gc_check_triggers(a), ==, 0U);

    /* max_erase < 地板（64）⇒ 不触发。先把上一段的 640 清掉，否则 max 仍是 640 */
    for (g = 0U; g < (uint16_t)FTL_BLOCK_TOTAL; g++)
    {
        a->blocks[g].erase_count = 1U;
    }
    a->blocks[(uint16_t)ftl_rand_range(tag, 0U, (uint32_t)FTL_BLOCK_TOTAL - 1U)].erase_count = 63U;
    munit_assert_uint32(gc_check_triggers(a), ==, 0U);

    munit_assert_uint32(gc_check_triggers(NULL), ==, 0U);

    return MUNIT_OK;
}

/* WL 条件 1（max-min 差值）单独命中：差值恰 5000、其余两条自检为假。 */
static MunitResult test_wl_trigger_delta(const MunitParameter params[], void *fixture)
{
    ftl_context_t   *F = ftl_fixture();
    ftl_allocator_t *a = &F->alloc;
    uint16_t         g;

    (void)params;
    (void)fixture;

    F->total_writes = (uint32_t)WL_MIN_INTERVAL; /* 过最小间隔门 */
    a->total_blocks = (uint16_t)FTL_BLOCK_TOTAL;

    for (g = 0U; g < (uint16_t)FTL_BLOCK_TOTAL; g++)
    {
        a->blocks[g].erase_count = 1667U;
    }
    a->blocks[0].erase_count = 0U;
    a->blocks[1].erase_count = 5000U;

    munit_assert_uint32(wl_max_erase(F), ==, 5000U);
    munit_assert_uint32(wl_min_erase(F), ==, 0U);
    munit_assert_uint32(wl_avg_erase(F), ==, 1667U);

    /* 隔离自检：条件 2 / 条件 3 必须为假 */
    munit_assert_true(wl_max_erase(F) < (wl_avg_erase(F) * (uint32_t)WL_MAX_AVG_RATIO));
    munit_assert_true(wl_count_blocks_below(F, wl_min_erase(F) + (uint32_t)WL_LOW_ZONE_WIDTH) <=
                      ((uint32_t)a->total_blocks * (uint32_t)WL_LOW_BLOCK_PCT / 100U));

    munit_assert_int(wl_should_trigger(F), ==, 1);

    /* 间隔门：写入量不足 ⇒ 一律不触发 */
    F->total_writes = (uint32_t)WL_MIN_INTERVAL - 1U;
    munit_assert_int(wl_should_trigger(F), ==, 0);

    return MUNIT_OK;
}

/* WL 条件 2（max ≥ avg*3）单独命中。 */
static MunitResult test_wl_trigger_ratio(const MunitParameter params[], void *fixture)
{
    ftl_context_t   *F = ftl_fixture();
    ftl_allocator_t *a = &F->alloc;
    uint16_t         g;

    (void)params;
    (void)fixture;

    F->total_writes = (uint32_t)WL_MIN_INTERVAL;
    a->total_blocks = (uint16_t)FTL_BLOCK_TOTAL;

    /* 204×1000 + 1×5900 + 3891×2001 ⇒ sum 7,995,791 ⇒ avg 1952 */
    for (g = 0U; g < (uint16_t)FTL_BLOCK_TOTAL; g++)
    {
        a->blocks[g].erase_count = 2001U;
    }
    for (g = 0U; g < 204U; g++)
    {
        a->blocks[g].erase_count = 1000U;
    }
    a->blocks[204].erase_count = 5900U;

    munit_assert_uint32(wl_max_erase(F), ==, 5900U);
    munit_assert_uint32(wl_min_erase(F), ==, 1000U);
    munit_assert_uint32(wl_avg_erase(F), ==, 1952U);

    /* 隔离自检：条件 1（4900 < 5000）与条件 3（低区 204 块，不 > 204）必须为假 */
    munit_assert_true((wl_max_erase(F) - wl_min_erase(F)) < (uint32_t)WL_DELTA_THRESHOLD);
    munit_assert_true(wl_count_blocks_below(F, wl_min_erase(F) + (uint32_t)WL_LOW_ZONE_WIDTH) <=
                      ((uint32_t)a->total_blocks * (uint32_t)WL_LOW_BLOCK_PCT / 100U));

    munit_assert_int(wl_should_trigger(F), ==, 1);

    return MUNIT_OK;
}

/* WL 条件 3（低擦除区块占比）单独命中：全表 0 ⇒ max=avg=0、条件 1/2 皆假。 */
static MunitResult test_wl_trigger_low_zone(const MunitParameter params[], void *fixture)
{
    ftl_context_t   *F = ftl_fixture();
    ftl_allocator_t *a = &F->alloc;
    uint16_t         g;

    (void)params;
    (void)fixture;

    F->total_writes = (uint32_t)WL_MIN_INTERVAL;
    a->total_blocks = (uint16_t)FTL_BLOCK_TOTAL;

    for (g = 0U; g < (uint16_t)FTL_BLOCK_TOTAL; g++)
    {
        a->blocks[g].erase_count = 0U;
    }

    munit_assert_uint32(wl_max_erase(F), ==, 0U);
    munit_assert_uint32(wl_avg_erase(F), ==, 0U); /* avg==0 ⇒ 条件 2 短路 */

    munit_assert_true(wl_count_blocks_below(F, (uint32_t)WL_LOW_ZONE_WIDTH) >
                      ((uint32_t)a->total_blocks * (uint32_t)WL_LOW_BLOCK_PCT / 100U));
    munit_assert_int(wl_should_trigger(F), ==, 1);

    return MUNIT_OK;
}

/* WL victim：有效 CW ≥ 门限（70%）者中取最大；随机基线 + 严格递增保证唯一 argmax。 */
static MunitResult test_wl_victim_high_valid_low_erase(const MunitParameter params[], void *fixture)
{
    const char      *tag              = "wl/victim";
    ftl_context_t   *F                = ftl_fixture();
    ftl_allocator_t *a                = &F->alloc;
    uint16_t         pb[3][PLANE_NUM] = {
        {10U, 2010U},
        {20U, 2020U},
        {30U, 2030U}
    };
    uint32_t    base = ftl_rand_range(tag, 360U, 420U);
    wl_victim_t victim;

    (void)params;
    (void)fixture;

    for (uint8_t i = 0U; i < 3U; i++)
    {
        uint16_t v = (uint16_t)(base + (uint32_t)(i * 40U)); /* 360..500，均 ≥ 358 门限 */

        ftl_ut_put_closed_sb(a, i, pb[i][0], pb[i][1], FTL_TEMP_COLD);
        a->blocks[ftl_gblock(0U, pb[i][0])].valid_count = v;
        a->blocks[ftl_gblock(1U, pb[i][1])].valid_count = 0U; /* 合计 = v */
    }

    munit_assert_int(wl_select_victim(F, &victim), ==, 1);
    munit_assert_uint16(victim.plane_blocks[0], ==, 30U);
    munit_assert_uint16(victim.plane_blocks[1], ==, 2030U);
    munit_assert_uint16(victim.valid_cw, ==, (uint16_t)(base + 80U));

    /* 全部低于有效门限（358）⇒ 无 victim（须把三条都压低，否则高者仍入选） */
    for (uint8_t i = 0U; i < 3U; i++)
    {
        a->blocks[ftl_gblock(0U, pb[i][0])].valid_count = 357U;
    }
    munit_assert_int(wl_select_victim(F, &victim), ==, 0);

    return MUNIT_OK;
}

/* WL victim：有效数相同时 read_count 高者胜（read disturb 优先搬）。 */
static MunitResult test_wl_victim_read_count_weight(const MunitParameter params[], void *fixture)
{
    ftl_context_t   *F = ftl_fixture();
    ftl_allocator_t *a = &F->alloc;
    wl_victim_t      victim;

    (void)params;
    (void)fixture;

    ftl_ut_put_closed_sb(a, 0U, 10U, 2010U, FTL_TEMP_COLD);
    ftl_ut_put_closed_sb(a, 1U, 20U, 2020U, FTL_TEMP_COLD);

    a->blocks[ftl_gblock(0U, 10U)].valid_count = 400U;
    a->blocks[ftl_gblock(0U, 10U)].read_count  = 10U;
    a->blocks[ftl_gblock(0U, 20U)].valid_count = 400U;
    a->blocks[ftl_gblock(0U, 20U)].read_count  = 50U;

    munit_assert_int(wl_select_victim(F, &victim), ==, 1);
    munit_assert_uint16(victim.plane_blocks[0], ==, 20U);
    munit_assert_uint16(victim.valid_cw, ==, 400U);
    munit_assert_uint32(victim.read_count, ==, 50U);

    /* 两者都低于门限（358）⇒ 无 victim */
    a->blocks[ftl_gblock(0U, 10U)].valid_count = 357U;
    a->blocks[ftl_gblock(0U, 20U)].valid_count = 357U;
    munit_assert_int(wl_select_victim(F, &victim), ==, 0);

    return MUNIT_OK;
}

/* 磨损统计：全表 7，块 0 = 100、块 1 = 3 ⇒ max=100 / min=3 / avg=7（整除截断）。 */
static MunitResult test_wl_max_min_avg_erase(const MunitParameter params[], void *fixture)
{
    ftl_context_t   *F = ftl_fixture();
    ftl_allocator_t *a = &F->alloc;
    uint16_t         g;

    (void)params;
    (void)fixture;

    for (g = 0U; g < (uint16_t)FTL_BLOCK_TOTAL; g++)
    {
        a->blocks[g].erase_count = 7U;
    }
    a->blocks[0].erase_count = 100U;
    a->blocks[1].erase_count = 3U;

    munit_assert_uint32(wl_max_erase(F), ==, 100U);
    munit_assert_uint32(wl_min_erase(F), ==, 3U);
    munit_assert_uint32(wl_avg_erase(F), ==, 7U);

    return MUNIT_OK;
}

/* 低区计数是**严格小于**阈值（边界值本身不计入）。 */
static MunitResult test_wl_count_blocks_below(const MunitParameter params[], void *fixture)
{
    ftl_context_t   *F = ftl_fixture();
    ftl_allocator_t *a = &F->alloc;
    uint16_t         g;

    (void)params;
    (void)fixture;

    for (g = 0U; g < (uint16_t)FTL_BLOCK_TOTAL; g++)
    {
        a->blocks[g].erase_count = 5U;
    }
    a->blocks[0].erase_count = 50U; /* 唯一高块 */

    munit_assert_uint32(wl_count_blocks_below(F, 5U), ==, 0U); /* 5 不小于 5 */
    munit_assert_uint32(wl_count_blocks_below(F, 6U), ==, 4095U);
    munit_assert_uint32(wl_count_blocks_below(F, 10U), ==, 4095U);
    munit_assert_uint32(wl_count_blocks_below(F, 51U), ==, 4096U);

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §6  套件 4 — Read Recovery（ECC 趋势 / 健康度 / 慢死扫描 / 退役）
 * ══════════════════════════════════════════════════════════════════════ */

/* 连续上升 ≥4 次 → WARNING；回落清零；单次 ≥6 → CRITICAL；单次 ≥15 → DYING。 */
static MunitResult test_rr_record_ecc_rising(const MunitParameter params[], void *fixture)
{
    rr_ecc_track_t t;

    (void)params;
    (void)fixture;

    memset(&t, 0, sizeof(t));

    rr_record_ecc(&t, 1U);
    rr_record_ecc(&t, 2U);
    rr_record_ecc(&t, 3U);
    rr_record_ecc(&t, 4U);
    munit_assert_uint8(t.rising_count, ==, 4U);
    munit_assert_uint8(t.health, ==, RR_BLOCK_WARNING);
    munit_assert_uint8(t.last_ecc, ==, 4U);

    /* 不上升 ⇒ 连续计数清零 */
    rr_record_ecc(&t, 1U);
    munit_assert_uint8(t.rising_count, ==, 0U);

    /* 单次 ≥ RR_ECC_HIGH_WATER ⇒ CRITICAL */
    memset(&t, 0, sizeof(t));
    rr_record_ecc(&t, (uint8_t)RR_ECC_HIGH_WATER);
    munit_assert_uint8(t.health, ==, RR_BLOCK_CRITICAL);

    /* 单次 ≥ RR_ECC_MAX ⇒ DYING */
    memset(&t, 0, sizeof(t));
    rr_record_ecc(&t, (uint8_t)RR_ECC_MAX);
    munit_assert_uint8(t.health, ==, RR_BLOCK_DYING);

    return MUNIT_OK;
}

/*
 * 健康度**只升不降**：一旦 DYING，后续低 ECC 读数不降级（趋势项只累计）。
 * （§4.4 原列的 record_ecc_ring_depth8 已换成本条——rr_ecc_track_t 是 3B、
 *   无环形缓冲，见文件头偏差 ③。）
 */
static MunitResult test_rr_record_ecc_health_monotonic(const MunitParameter params[], void *fixture)
{
    rr_ecc_track_t t;

    (void)params;
    (void)fixture;

    memset(&t, 0, sizeof(t));

    rr_record_ecc(&t, (uint8_t)RR_ECC_MAX);
    munit_assert_uint8(t.health, ==, RR_BLOCK_DYING);

    rr_record_ecc(&t, 0U);
    munit_assert_uint8(t.health, ==, RR_BLOCK_DYING);
    munit_assert_uint8(t.rising_count, ==, 0U);
    munit_assert_uint8(t.last_ecc, ==, 0U);

    return MUNIT_OK;
}

/* rr_check_predictive 只认 health ≥ CRITICAL，返回首个命中块号；NULL 保护。 */
static MunitResult test_rr_check_predictive_critical(const MunitParameter params[], void *fixture)
{
    ftl_block_t blocks[4];
    uint16_t    gb = UINT16_MAX;

    (void)params;
    (void)fixture;

    memset(blocks, 0, sizeof(blocks));

    blocks[0].rr.health = RR_BLOCK_WARNING;
    munit_assert_int(rr_check_predictive(blocks, 4U, &gb), ==, 0);

    blocks[2].rr.health = RR_BLOCK_CRITICAL;
    munit_assert_int(rr_check_predictive(blocks, 4U, &gb), ==, 1);
    munit_assert_uint16(gb, ==, 2U);

    munit_assert_int(rr_check_predictive(NULL, 4U, &gb), ==, 0);
    munit_assert_int(rr_check_predictive(blocks, 4U, NULL), ==, 0);

    return MUNIT_OK;
}

/* get_health 直读 track.health；NULL ⇒ HEALTHY。 */
static MunitResult test_rr_get_health(const MunitParameter params[], void *fixture)
{
    rr_ecc_track_t t;

    (void)params;
    (void)fixture;

    memset(&t, 0, sizeof(t));

    munit_assert_int(rr_get_health(NULL), ==, RR_BLOCK_HEALTHY);
    munit_assert_int(rr_get_health(&t), ==, RR_BLOCK_HEALTHY);

    t.health = RR_BLOCK_CRITICAL;
    munit_assert_int(rr_get_health(&t), ==, RR_BLOCK_CRITICAL);

    return MUNIT_OK;
}

/* 慢死扫描只认 CLOSED 块；两条入选条件：health ≥ CRITICAL 或 read_count ≥ 阈值。 */
static MunitResult test_rr_predictive_scan_read_disturb(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a  = &ftl_fixture()->alloc;
    ftl_block_t     *b  = &a->blocks[500U];
    uint16_t         gb = UINT16_MAX;

    (void)params;
    (void)fixture;

    /* 复位态：全 FREE ⇒ 扫不到 */
    munit_assert_int(rr_predictive_scan(a, &gb), ==, 0);

    ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE);
    ftl_block_state_set(b, FTL_BLOCK_STATE_CLOSED);

    /* read_count = 阈值-1 ⇒ 不命中 */
    b->read_count = (uint16_t)(RR_READ_DISTURB_THRESHOLD - 1U);
    munit_assert_int(rr_predictive_scan(a, &gb), ==, 0);

    /* 到阈值 ⇒ 命中 */
    b->read_count = (uint16_t)RR_READ_DISTURB_THRESHOLD;
    munit_assert_int(rr_predictive_scan(a, &gb), ==, 1);
    munit_assert_uint16(gb, ==, 500U);

    /* 非 CLOSED 的块即使 read_count 超高也不选 */
    ftl_block_state_clr(b, FTL_BLOCK_STATE_CLOSED);
    b->read_count = 5000U;
    munit_assert_int(rr_predictive_scan(a, &gb), ==, 0);

    /* CLOSED + health = CRITICAL ⇒ 命中 */
    ftl_block_state_set(b, FTL_BLOCK_STATE_CLOSED);
    b->read_count = 0U;
    b->rr.health  = RR_BLOCK_CRITICAL;
    munit_assert_int(rr_predictive_scan(a, &gb), ==, 1);

    return MUNIT_OK;
}

/* 退役：置 RESERVED、清 OPEN/CLOSED/FREE/TEMP、归属复位、健康度与计数全清。 */
static MunitResult test_rr_retire_block(const MunitParameter params[], void *fixture)
{
    ftl_block_t *b  = &ftl_fixture()->alloc.blocks[600U];
    uint16_t     gb = UINT16_MAX;

    (void)params;
    (void)fixture;

    ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE);
    ftl_block_state_set(b, FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED | FTL_BLOCK_STATE_TEMP_HOT);
    ftl_block_set_sb(b, 5U);
    b->rr.health       = RR_BLOCK_CRITICAL;
    b->rr.rising_count = 7U;
    b->rr.last_ecc     = 9U;
    b->read_count      = (uint16_t)RR_READ_DISTURB_THRESHOLD;

    rr_retire_block(b);

    munit_assert_true(ftl_block_state_is(b, FTL_BLOCK_STATE_RESERVED));
    munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_OPEN));
    munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_CLOSED));
    munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_FREE));
    munit_assert_uint8(ftl_block_get_temp(b), ==, 0U);
    munit_assert_uint16(ftl_block_sb_id(b), ==, FTL_BLOCK_NO_SB);
    munit_assert_uint8(b->rr.health, ==, RR_BLOCK_HEALTHY);
    munit_assert_uint8(b->rr.rising_count, ==, 0U);
    munit_assert_uint8(b->rr.last_ecc, ==, 0U);
    munit_assert_uint16(b->read_count, ==, 0U);

    /* 退役后不再被慢死扫描选中（RESERVED 不是 CLOSED） */
    munit_assert_int(rr_predictive_scan(&ftl_fixture()->alloc, &gb), ==, 0);

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §7  套件 5 — BBT（标坏 / Spare 补池 / 位图原语）
 * ══════════════════════════════════════════════════════════════════════ */

/* 标坏：置 BAD、清有效计数与 OPEN/CLOSED/FREE、归属复位、dirty 置位、bad_count++；
 * Spare 空 ⇒ free_count 不变。 */
static MunitResult test_bbt_mark_bad_sets_bits(const MunitParameter params[], void *fixture)
{
    const char      *tag = "bbt/mark_bad";
    ftl_allocator_t *a   = &ftl_fixture()->alloc;
    uint16_t         n   = (uint16_t)ftl_rand_range(tag, 1U, 8U);
    uint16_t         blocks[8];
    uint16_t         free_before = a->free_count;
    uint16_t         i;

    (void)params;
    (void)fixture;

    munit_assert_uint16(a->bad_count, ==, 0U);

    /* 随机取 n 个互不相同的块号（避开固定元数据区，便于观察状态位） */
    for (i = 0U; i < n; i++)
    {
        uint16_t g;
        int      dup;
        uint16_t k;

        do
        {
            g   = (uint16_t)ftl_rand_range(tag, (uint32_t)FTL_METADATA_BLOCK_END, (uint32_t)FTL_BLOCK_TOTAL - 1U);
            dup = 0;
            for (k = 0U; k < i; k++)
            {
                if (blocks[k] == g)
                {
                    dup = 1;
                }
            }
        } while (dup != 0);

        blocks[i] = g;

        /* 先给点残留状态，验证标坏会把它们清掉 */
        a->blocks[g].valid_count = 123U;
        a->blocks[g].read_count  = 7U;
        ftl_block_state_set(&a->blocks[g], FTL_BLOCK_STATE_CLOSED | FTL_BLOCK_STATE_TEMP_COLD);
        ftl_block_set_sb(&a->blocks[g], 3U);

        munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);
    }

    for (i = 0U; i < n; i++)
    {
        ftl_block_t *b = &a->blocks[blocks[i]];

        munit_assert_true(ftl_block_is_bad(b));
        munit_assert_uint16(b->valid_count, ==, 0U);
        munit_assert_uint16(ftl_block_sb_id(b), ==, FTL_BLOCK_NO_SB);
        munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_OPEN));
        munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_CLOSED));
        munit_assert_false(ftl_block_state_is(b, FTL_BLOCK_STATE_FREE));
        munit_assert_uint8(ftl_block_get_temp(b), ==, 0U);
    }

    munit_assert_uint16(a->bad_count, ==, n);
    munit_assert_uint16(a->free_count, ==, free_before); /* Spare 空 ⇒ 不补池 */
    munit_assert_uint8(a->bbt.dirty, ==, 1U);

    /* 越界 / NULL 拒绝 */
    munit_assert_int(bbt_mark_bad(a, (uint16_t)FTL_BLOCK_TOTAL), ==, FTL_ERROR);
    munit_assert_int(bbt_mark_bad(NULL, 0U), ==, FTL_ERROR);

    return MUNIT_OK;
}

/*
 * 重复标坏：位级幂等（BAD 仍置、valid 归零、无归属）。
 * ⚠ 已知偏差 ④：bbt_mark_bad 的 bad_count++ 是**无条件**的（src/ftl/bbt.c 的
 *   "alloc->bad_count++"），重复标坏会重复计数。bad_count 只用于 CLI 显示与
 *   bbt_save 持久化，不影响分配决策 ⇒ 此处**不钉**该值。
 */
static MunitResult test_bbt_mark_bad_idempotent(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a = &ftl_fixture()->alloc;
    uint16_t         g = 700U;

    (void)params;
    (void)fixture;

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);
    munit_assert_uint16(a->bad_count, ==, 1U);

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);

    munit_assert_true(ftl_block_is_bad(&a->blocks[g]));
    munit_assert_uint16(a->blocks[g].valid_count, ==, 0U);
    munit_assert_uint16(ftl_block_sb_id(&a->blocks[g]), ==, FTL_BLOCK_NO_SB);
    munit_assert_false(ftl_block_state_is(&a->blocks[g], FTL_BLOCK_STATE_FREE));

    return MUNIT_OK;
}

/* Spare 补池：LIFO 取块、置 FREE、计数递减；耗尽返回 FTL_ERROR 且不动计数。 */
static MunitResult test_bbt_spare_pool_refill(const MunitParameter params[], void *fixture)
{
    ftl_allocator_t *a = &ftl_fixture()->alloc;

    (void)params;
    (void)fixture;

    a->spare_blocks[0] = 300U;
    a->spare_blocks[1] = 301U;
    a->spare_count     = 2U;

    munit_assert_int(bbt_spare_pool_refill(a), ==, FTL_OK);
    munit_assert_uint16(a->spare_count, ==, 1U);
    munit_assert_uint16(a->free_count, ==, 1U);
    munit_assert_uint16(a->free_blocks[0], ==, 301U); /* LIFO：取末位 */
    munit_assert_true(ftl_block_state_is(&a->blocks[301U], FTL_BLOCK_STATE_FREE));

    munit_assert_int(bbt_spare_pool_refill(a), ==, FTL_OK);
    munit_assert_uint16(a->spare_count, ==, 0U);
    munit_assert_uint16(a->free_count, ==, 2U);
    munit_assert_uint16(a->free_blocks[1], ==, 300U);

    /* 耗尽 */
    munit_assert_int(bbt_spare_pool_refill(a), ==, FTL_ERROR);
    munit_assert_uint16(a->free_count, ==, 2U);

    munit_assert_int(bbt_spare_pool_refill(NULL), ==, FTL_ERROR);

    return MUNIT_OK;
}

/* 位图原语：置位/查询按 gblock 位序，越界位（4095）落在位图末字节，NULL 保护。 */
static MunitResult test_bbt_is_bad_set_bad_inline(const MunitParameter params[], void *fixture)
{
    uint8_t map[512];

    (void)params;
    (void)fixture;

    memset(map, 0, sizeof(map));

    munit_assert_int(bbt_is_bad(NULL, 5U), ==, 0);
    munit_assert_int(bbt_is_bad(map, 5U), ==, 0);

    bbt_set_bad(map, 5U);
    munit_assert_int(bbt_is_bad(map, 5U), ==, 1);
    munit_assert_int(bbt_is_bad(map, 4U), ==, 0);
    munit_assert_int(bbt_is_bad(map, 6U), ==, 0);

    /* 最高块号：落在 map[511] 的 bit7 */
    bbt_set_bad(map, (uint16_t)FTL_BLOCK_TOTAL - 1U);
    munit_assert_int(bbt_is_bad(map, (uint16_t)FTL_BLOCK_TOTAL - 1U), ==, 1);
    munit_assert_int(bbt_is_bad(map, (uint16_t)FTL_BLOCK_TOTAL - 2U), ==, 0);

    /* NULL 是 no-op（不崩） */
    bbt_set_bad(NULL, 1U);
    munit_assert_int(bbt_is_bad(map, 1U), ==, 0);

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §8  套件 6 — Metadata Directory（固定表 / 动态日志块 / ring 地址）
 * ══════════════════════════════════════════════════════════════════════ */

/* init 后：2048 条 L2P region 全登记在固定区，游标无效、dirty/have_dynamic 归零。 */
static MunitResult test_meta_dir_init_entries(const MunitParameter params[], void *fixture)
{
    metadata_dir_cache_t *d = &ftl_fixture()->metadata_dir;
    uint16_t              i;

    (void)params;
    (void)fixture;

    munit_assert_uint16(d->count, ==, (uint16_t)L2P_REGIONS_PER_INSTANCE);
    munit_assert_uint8(d->dirty, ==, 0U);
    munit_assert_uint8(d->have_dynamic, ==, 0U);
    munit_assert_uint8(d->cur_block_valid, ==, 0U);
    munit_assert_uint8(d->anchor_cursor_valid, ==, 0U);
    munit_assert_uint8(d->compacting, ==, 0U);
    munit_assert_uint32(d->change_count, ==, 0U);

    for (i = 0U; i < d->count; i++)
    {
        munit_assert_uint8(d->entries[i].type, ==, METADATA_TYPE_L2P_REGION);
        munit_assert_uint16(d->entries[i].id, ==, i);
        munit_assert_uint32(d->entries[i].paa, !=, (uint32_t)NAND_PBA_INVALID);
        munit_assert_true(paa_to_block(d->entries[i].paa) < (uint32_t)FTL_METADATA_BLOCK_END);
    }

    return MUNIT_OK;
}

/* update 覆盖既有条目（2048 条已预登记 ⇒ 永远走覆盖分支）；lookup 取回新位置。 */
static MunitResult test_meta_dir_update_lookup_roundtrip(const MunitParameter params[], void *fixture)
{
    const char           *tag   = "meta/roundtrip";
    metadata_dir_cache_t *d     = &ftl_fixture()->metadata_dir;
    uint16_t              r     = (uint16_t)ftl_rand_range(tag, 0U, (uint32_t)L2P_REGIONS_PER_INSTANCE - 1U);
    uint16_t              block = (uint16_t)ftl_rand_range(tag, (uint32_t)FTL_METADATA_BLOCK_END, (uint32_t)BLOCKS_PER_PLANE - 1U);
    nand_paa_t            paa   = ftl_ut_paa(ftl_fixture()->topo.lun,
                                block,
                                (uint8_t)ftl_rand_range(tag, 0U, (uint32_t)PLANE_NUM - 1U),
                                (uint8_t)ftl_rand_range(tag, 0U, (uint32_t)PAGES_PER_BLOCK - 1U));

    (void)params;
    (void)fixture;

    metadata_dir_update(d, METADATA_TYPE_L2P_REGION, r, paa);

    munit_assert_uint32(metadata_dir_lookup(d, METADATA_TYPE_L2P_REGION, r), ==, paa);
    munit_assert_uint8(d->dirty, ==, 1U);
    munit_assert_uint32(d->change_count, ==, 1U);
    munit_assert_uint16(d->count, ==, (uint16_t)L2P_REGIONS_PER_INSTANCE); /* 覆盖而非追加 */

    /* 其他 region 不受影响 */
    munit_assert_uint32(metadata_dir_lookup(d, METADATA_TYPE_L2P_REGION, (uint16_t)((r + 1U) % L2P_REGIONS_PER_INSTANCE)),
                        !=,
                        paa);

    return MUNIT_OK;
}

/* 查不到的 type / 越界 id / NULL 目录一律返回哨兵。 */
static MunitResult test_meta_dir_lookup_missing_unknown(const MunitParameter params[], void *fixture)
{
    metadata_dir_cache_t *d = &ftl_fixture()->metadata_dir;

    (void)params;
    (void)fixture;

    /* 未登记的 type（只有 L2P_REGION 被 fill_fixed 登记） */
    munit_assert_uint32(metadata_dir_lookup(d, METADATA_TYPE_CHECKPOINT, 0U), ==, (uint32_t)NAND_PBA_INVALID);
    munit_assert_uint32(metadata_dir_lookup(d, METADATA_TYPE_BBT, 0U), ==, (uint32_t)NAND_PBA_INVALID);

    /* 越界 id */
    munit_assert_uint32(metadata_dir_lookup(d, METADATA_TYPE_L2P_REGION, (uint16_t)L2P_REGIONS_PER_INSTANCE),
                        ==,
                        (uint32_t)NAND_PBA_INVALID);

    /* NULL */
    munit_assert_uint32(metadata_dir_lookup(NULL, METADATA_TYPE_L2P_REGION, 0U), ==, (uint32_t)NAND_PBA_INVALID);

    return MUNIT_OK;
}

/* 固定分区表：块 0..62 恒为元数据块（md_dir 为 NULL 也成立），63 及以后不是。 */
static MunitResult test_meta_is_metadata_block(const MunitParameter params[], void *fixture)
{
    metadata_dir_cache_t *d = &ftl_fixture()->metadata_dir;
    uint16_t              b;

    (void)params;
    (void)fixture;

    for (b = 0U; b < (uint16_t)FTL_METADATA_BLOCK_END; b++)
    {
        munit_assert_int(ftl_is_metadata_block(d, b), ==, 1);
        munit_assert_int(ftl_is_metadata_block(NULL, b), ==, 1); /* 固定层不依赖 md_dir */
    }

    munit_assert_int(ftl_is_metadata_block(d, (uint16_t)FTL_METADATA_BLOCK_END), ==, 0);
    munit_assert_int(ftl_is_metadata_block(NULL, (uint16_t)FTL_METADATA_BLOCK_END), ==, 0);
    munit_assert_int(ftl_is_metadata_block(d, (uint16_t)(BLOCKS_PER_PLANE - 1U)), ==, 0);

    return MUNIT_OK;
}

/* commit_region 把 region 指到动态块 ⇒ have_dynamic 置位、l2p_live 记账、该块被判为元数据块。 */
static MunitResult test_meta_is_metadata_block_dynamic(const MunitParameter params[], void *fixture)
{
    ftl_context_t        *F   = ftl_fixture();
    metadata_dir_cache_t *d   = &F->metadata_dir;
    uint16_t              r   = 7U;
    nand_paa_t            paa = ftl_ut_paa(F->topo.lun, 100U, 0U, 0U);

    (void)params;
    (void)fixture;

    munit_assert_int(ftl_is_metadata_block(d, 100U), ==, 0);

    /* 传 alloc = NULL 是安全的：旧位置是固定层（block < 63）⇒ 不走 reclaim */
    metadata_dir_commit_region(d, NULL, r, paa);

    munit_assert_uint32(metadata_dir_lookup(d, METADATA_TYPE_L2P_REGION, r), ==, paa);
    munit_assert_uint8(d->have_dynamic, ==, 1U);
    munit_assert_uint16(d->l2p_live[ftl_gblock(0U, 100U)], ==, 1U);
    munit_assert_int(ftl_is_metadata_block(d, 100U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(d, 101U), ==, 0);

    /* md_dir == NULL：固定层仍命中，动态层无从得知 ⇒ 0 */
    munit_assert_int(ftl_is_metadata_block(NULL, 100U), ==, 0);
    munit_assert_int(ftl_is_metadata_block(NULL, 10U), ==, 1);

    /* 非法入参（region 越界 / paa 哨兵）不动表 */
    metadata_dir_commit_region(d, NULL, (uint16_t)METADATA_DIR_ENTRIES, paa);
    metadata_dir_commit_region(d, NULL, r, (nand_paa_t)NAND_PBA_INVALID);
    munit_assert_uint32(d->change_count, ==, 1U);

    return MUNIT_OK;
}

/* ring 地址几何：Checkpoint 池（1 页/record）与 Anchor 池（8 页/record）的槽位换算。 */
static MunitResult test_meta_ring_addr_geometry(const MunitParameter params[], void *fixture)
{
    ftl_context_t  *F = ftl_fixture();
    metadata_ring_t ring;
    nand_addr_t     a;

    (void)params;
    (void)fixture;

    /* ── Checkpoint 池：块 48..55，1 页/record ⇒ 每块 64 条、共 512 条 ── */
    memset(&ring, 0, sizeof(ring));
    ring.first_block      = (uint16_t)CHECKPOINT_BLOCK0;
    ring.block_count      = (uint16_t)CHECKPOINT_POOL_BLOCKS;
    ring.pages_per_record = 1U;
    ring.plane            = 0U;

    munit_assert_uint16(metadata_ring_records_per_block(&ring), ==, 64U);
    munit_assert_uint16(metadata_ring_total_records(&ring), ==, 512U);

    a = metadata_ring_addr(&ring, &F->topo, 0U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)CHECKPOINT_BLOCK0);
    munit_assert_uint8(a.page, ==, 0U);
    munit_assert_uint8(a.plane, ==, 0U);
    munit_assert_uint8(a.cw, ==, 0U);
    munit_assert_uint8(a.ch, ==, F->topo.ch);
    munit_assert_uint8(a.ce, ==, F->topo.ce);
    munit_assert_uint8(a.lun, ==, F->topo.lun);

    a = metadata_ring_addr(&ring, &F->topo, 63U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)CHECKPOINT_BLOCK0);
    munit_assert_uint8(a.page, ==, 63U);

    /* ── Anchor 池：块 16..47，8 页/record ⇒ 每块 8 条、共 256 条 ── */
    memset(&ring, 0, sizeof(ring));
    ring.first_block      = (uint16_t)ANCHOR_BLOCK0;
    ring.block_count      = (uint16_t)ANCHOR_POOL_BLOCKS;
    ring.pages_per_record = (uint8_t)ANCHOR_PAGE_COUNT;
    ring.plane            = 0U;

    munit_assert_uint16(metadata_ring_records_per_block(&ring), ==, (uint16_t)ANCHOR_RECORDS_PER_BLOCK);
    munit_assert_uint16(metadata_ring_total_records(&ring), ==, (uint16_t)(ANCHOR_POOL_BLOCKS * ANCHOR_RECORDS_PER_BLOCK));

    a = metadata_ring_addr(&ring, &F->topo, 0U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)ANCHOR_BLOCK0);
    munit_assert_uint8(a.page, ==, 0U);

    a = metadata_ring_addr(&ring, &F->topo, 0U, 7U);
    munit_assert_uint16(a.block, ==, (uint16_t)ANCHOR_BLOCK0);
    munit_assert_uint8(a.page, ==, 7U);

    /* record 内跨页 */
    a = metadata_ring_addr(&ring, &F->topo, 1U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)ANCHOR_BLOCK0);
    munit_assert_uint8(a.page, ==, 8U);

    /* 换块 */
    a = metadata_ring_addr(&ring, &F->topo, (uint16_t)ANCHOR_RECORDS_PER_BLOCK, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)(ANCHOR_BLOCK0 + 1));
    munit_assert_uint8(a.page, ==, 0U);

    /* 池末槽 */
    a = metadata_ring_addr(&ring, &F->topo, (uint16_t)(ANCHOR_POOL_BLOCKS * ANCHOR_RECORDS_PER_BLOCK - 1U), (uint8_t)(ANCHOR_PAGE_COUNT - 1U));
    munit_assert_uint16(a.block, ==, (uint16_t)(ANCHOR_BLOCK0 + ANCHOR_POOL_BLOCKS - 1U));
    munit_assert_uint8(a.page, ==, (uint8_t)(PAGES_PER_BLOCK - 1U));

    return MUNIT_OK;
}

/* ring 地址回绕：record 边界（rpb 的整数倍）恰是下一块的页 0，且环序单调不跳块。 */
static MunitResult test_meta_ring_addr_wrap(const MunitParameter params[], void *fixture)
{
    ftl_context_t  *F = ftl_fixture();
    metadata_ring_t ring;
    nand_addr_t     a;

    (void)params;
    (void)fixture;

    memset(&ring, 0, sizeof(ring));
    ring.first_block      = (uint16_t)CHECKPOINT_BLOCK0;
    ring.block_count      = (uint16_t)CHECKPOINT_POOL_BLOCKS;
    ring.pages_per_record = 1U;
    ring.plane            = 0U;

    /* 块内末槽 → 下一块首页 */
    a = metadata_ring_addr(&ring, &F->topo, 63U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)CHECKPOINT_BLOCK0);
    munit_assert_uint8(a.page, ==, 63U);

    a = metadata_ring_addr(&ring, &F->topo, 64U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)(CHECKPOINT_BLOCK0 + 1));
    munit_assert_uint8(a.page, ==, 0U);

    a = metadata_ring_addr(&ring, &F->topo, 127U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)(CHECKPOINT_BLOCK0 + 1));
    munit_assert_uint8(a.page, ==, 63U);

    a = metadata_ring_addr(&ring, &F->topo, 128U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)(CHECKPOINT_BLOCK0 + 2));
    munit_assert_uint8(a.page, ==, 0U);

    /* 池末槽（512 条 × 8 块 = 末块末页），不越出池 */
    a = metadata_ring_addr(&ring, &F->topo, 511U, 0U);
    munit_assert_uint16(a.block, ==, (uint16_t)(CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS - 1U));
    munit_assert_uint8(a.page, ==, (uint8_t)(PAGES_PER_BLOCK - 1U));

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §9  套件 7 — Pending 表 + 完成表
 * ══════════════════════════════════════════════════════════════════════ */

/* 增/查/删：字段逐项落表、随机增删序下计数单调一致、重复删幂等。 */
static MunitResult test_pending_add_lookup_remove(const MunitParameter params[], void *fixture)
{
    const char    *tag  = "pending/add";
    ftl_context_t *F    = ftl_fixture();
    uint16_t       base = (uint16_t)ftl_rand_range(tag, 1U, 50000U);
    uint16_t       cmds[8];
    uint8_t        dummy = 0U;
    uint8_t        n     = (uint8_t)ftl_rand_range(tag, 1U, 8U);
    uint16_t       remaining;
    uint8_t        i;

    (void)params;
    (void)fixture;

    munit_assert_uint16(pending_count(F), ==, 0U);

    for (i = 0U; i < n; i++)
    {
        cmds[i] = (uint16_t)(base + i);

        munit_assert_int(pending_add(F,
                                     cmds[i],
                                     (uint32_t)(1U << i),
                                     (lba_t)(i * 8U),
                                     &dummy,
                                     (uint8_t)(1U + (i % 8U)),
                                     (nand_pba_t)(0x1000U + i),
                                     (nand_pba_t)NAND_PBA_INVALID,
                                     (uint8_t)(i & 1U),
                                     PENDING_OP_WRITE,
                                     (uint8_t)(0x01U << i)),
                         ==,
                         FTL_OK);
    }

    munit_assert_uint16(pending_count(F), ==, n);

    for (i = 0U; i < n; i++)
    {
        ftl_pending_req_t *p = pending_lookup(F, cmds[i]);

        munit_assert_not_null(p);
        munit_assert_uint16(p->command_id, ==, cmds[i]);
        munit_assert_uint32(p->owners, ==, (uint32_t)(1U << i));
        munit_assert_uint8(p->op, ==, (uint8_t)PENDING_OP_WRITE);
        munit_assert_uint8(p->n_cw, ==, (uint8_t)(1U + (i % 8U)));
        munit_assert_uint8(p->cw_bitmap, ==, (uint8_t)(0x01U << i));
        munit_assert_uint32(p->lba, ==, (uint32_t)(i * 8U));
        munit_assert_uint32(p->new_pba, ==, (uint32_t)(0x1000U + i));
        munit_assert_uint32(p->new_pba1, ==, (uint32_t)NAND_PBA_INVALID);
        munit_assert_uint8(p->cache_intermediate, ==, (uint8_t)(i & 1U));
        munit_assert_ptr(p->buffer, ==, &dummy);
    }

    /* 随机序移除（用 0 标记已移除；base ≥ 1 ⇒ 0 不可能是合法 cmd_id） */
    remaining = n;
    while (remaining > 0U)
    {
        uint8_t pick = (uint8_t)ftl_rand_range(tag, 0U, (uint32_t)(n - 1U));

        if (cmds[pick] == 0U)
        {
            continue;
        }

        pending_remove(F, cmds[pick]);
        munit_assert_null(pending_lookup(F, cmds[pick]));
        cmds[pick] = 0U;
        remaining--;
        munit_assert_uint16(pending_count(F), ==, remaining);
    }

    /* 重复移除无害 */
    pending_remove(F, base);
    munit_assert_uint16(pending_count(F), ==, 0U);

    return MUNIT_OK;
}

/* 表满 32 拒绝；释放一个后可再入。 */
static MunitResult test_pending_full_32_rejects(const MunitParameter params[], void *fixture)
{
    ftl_context_t *F = ftl_fixture();
    uint16_t       i;

    (void)params;
    (void)fixture;

    for (i = 0U; i < (uint16_t)FTL_PENDING_MAX; i++)
    {
        munit_assert_int(pending_add(F,
                                     (uint16_t)(i + 1U),
                                     1U,
                                     (lba_t)i,
                                     NULL,
                                     1U,
                                     (nand_pba_t)NAND_PBA_INVALID,
                                     (nand_pba_t)NAND_PBA_INVALID,
                                     0U,
                                     PENDING_OP_READ,
                                     0U),
                         ==,
                         FTL_OK);
    }

    munit_assert_uint16(pending_count(F), ==, (uint16_t)FTL_PENDING_MAX);

    /* 满 ⇒ 拒绝，计数不变 */
    munit_assert_int(pending_add(F, 999U, 1U, 0U, NULL, 1U, (nand_pba_t)NAND_PBA_INVALID, (nand_pba_t)NAND_PBA_INVALID, 0U, PENDING_OP_READ, 0U),
                     ==,
                     FTL_ERROR);
    munit_assert_uint16(pending_count(F), ==, (uint16_t)FTL_PENDING_MAX);

    /* 释放一个 ⇒ 再入成功 */
    pending_remove(F, 1U);
    munit_assert_uint16(pending_count(F), ==, (uint16_t)FTL_PENDING_MAX - 1U);
    munit_assert_int(pending_add(F, 999U, 1U, 0U, NULL, 1U, (nand_pba_t)NAND_PBA_INVALID, (nand_pba_t)NAND_PBA_INVALID, 0U, PENDING_OP_READ, 0U),
                     ==,
                     FTL_OK);
    munit_assert_uint16(pending_count(F), ==, (uint16_t)FTL_PENDING_MAX);

    return MUNIT_OK;
}

/* pending_count 与增删同步；移除后 lookup 落空。 */
static MunitResult test_pending_count(const MunitParameter params[], void *fixture)
{
    ftl_context_t *F = ftl_fixture();
    uint16_t       i;

    (void)params;
    (void)fixture;

    munit_assert_uint16(pending_count(F), ==, 0U);

    for (i = 0U; i < 5U; i++)
    {
        munit_assert_int(pending_add(F, i, (uint32_t)(1U << i), (lba_t)i, NULL, 1U, (nand_pba_t)NAND_PBA_INVALID, (nand_pba_t)NAND_PBA_INVALID, 0U, PENDING_OP_READ, 0U),
                         ==,
                         FTL_OK);
    }
    munit_assert_uint16(pending_count(F), ==, 5U);

    pending_remove(F, 1U);
    pending_remove(F, 3U);
    munit_assert_uint16(pending_count(F), ==, 3U);
    munit_assert_null(pending_lookup(F, 1U));

    pending_remove(F, 0U);
    pending_remove(F, 2U);
    pending_remove(F, 4U);
    munit_assert_uint16(pending_count(F), ==, 0U);

    return MUNIT_OK;
}

/*
 * 完成表：add/lookup/remove/count。
 *
 * ⚠ 偏差 ⑤：**不调 completion_table_init()** —— 它会连同固件开机注册的 default
 *   handler（ftl.c 的 static ftl_handle_completion）一起清掉，测试无法重装 ⇒
 *   后续所有 NAND 完成回调都会静默丢失。改用**相对计数**：先取基线，只增删自己
 *   的条目，收尾恢复基线。
 */
static MunitResult test_ct_add_remove_lookup_count(const MunitParameter params[], void *fixture)
{
    uint16_t ids[3] = {0x7FF0U, 0x7FF1U, 0x7FF2U};
    uint16_t base_count;
    uint16_t i;

    (void)params;
    (void)fixture;

    base_count = completion_table_count();

    /* 未注册的 id 落空（0x7FFE 远离命令池实际 ID） */
    munit_assert_null(completion_table_lookup(0x7FFEU));

    for (i = 0U; i < 3U; i++)
    {
        munit_assert_int(completion_table_add(ids[i], ftl_ut_completion_stub, &ids[i]), ==, FTL_OK);
    }
    munit_assert_uint16(completion_table_count(), ==, (uint16_t)(base_count + 3U));

    for (i = 0U; i < 3U; i++)
    {
        completion_entry_t *e = completion_table_lookup(ids[i]);

        munit_assert_not_null(e);
        munit_assert_uint16(e->cmd_id, ==, ids[i]);
        munit_assert_true(e->handler == ftl_ut_completion_stub);
        munit_assert_ptr(e->ctx, ==, &ids[i]);
    }

    /* handler == NULL 拒绝 */
    munit_assert_int(completion_table_add(0x7FF3U, NULL, NULL), ==, FTL_ERROR);
    munit_assert_uint16(completion_table_count(), ==, (uint16_t)(base_count + 3U));

    /* 移除 + 幂等 */
    completion_table_remove(ids[1]);
    munit_assert_uint16(completion_table_count(), ==, (uint16_t)(base_count + 2U));
    munit_assert_null(completion_table_lookup(ids[1]));

    completion_table_remove(ids[1]);
    munit_assert_uint16(completion_table_count(), ==, (uint16_t)(base_count + 2U));

    /* 收尾：恢复基线 */
    completion_table_remove(ids[0]);
    completion_table_remove(ids[2]);
    munit_assert_uint16(completion_table_count(), ==, base_count);

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §10  套件 8 — 完成组（分配 / 门控 / 粘性状态 / 恰一次通知 / 重入 / 多组）
 * ══════════════════════════════════════════════════════════════════════ */

/* 槽位分配：顺序取空槽、释放后可复用、满 32 拒绝。 */
static MunitResult test_group_alloc_free(const MunitParameter params[], void *fixture)
{
    ftl_context_t *F = ftl_fixture();
    uint16_t       i;

    (void)params;
    (void)fixture;

    munit_assert_uint16(pending_group_count(F), ==, 0U);

    munit_assert_int(pending_group_alloc(F), ==, 0);
    munit_assert_int(pending_group_alloc(F), ==, 1);
    munit_assert_int(pending_group_alloc(F), ==, 2);
    munit_assert_uint16(pending_group_count(F), ==, 3U);

    pending_group_free(F, 1U);
    munit_assert_uint16(pending_group_count(F), ==, 2U);

    /* 空槽复用 */
    munit_assert_int(pending_group_alloc(F), ==, 1);
    munit_assert_uint16(pending_group_count(F), ==, 3U);

    /* 填满 ⇒ 再申请失败 */
    for (i = 0U; i < (uint16_t)FTL_GROUP_MAX; i++)
    {
        if (F->groups[i].used == 0U)
        {
            (void)pending_group_alloc(F);
        }
    }
    munit_assert_uint16(pending_group_count(F), ==, (uint16_t)FTL_GROUP_MAX);
    munit_assert_int(pending_group_alloc(F), ==, FTL_ERROR);
    munit_assert_uint16(pending_group_count(F), ==, (uint16_t)FTL_GROUP_MAX);

    return MUNIT_OK;
}

/* 完成门控：next < total 或 outstanding != 0 时都不通知；两者归零才发一次。 */
static MunitResult test_group_maybe_complete_gating(const MunitParameter params[], void *fixture)
{
    ftl_context_t  *F  = ftl_fixture();
    int             gi = pending_group_alloc(F);
    ftl_io_group_t *g;
    uint8_t         flag = 0U;
    ssd_status_t    st   = SSD_STATUS_IO_ERROR;

    (void)params;
    (void)fixture;

    munit_assert_int(gi, >=, 0);
    g = &F->groups[gi];

    g->complete_flag = &flag;
    g->status_out    = &st;
    g->total         = 2U;
    g->next          = 1U; /* 未全部下发 */
    g->outstanding   = 0U;
    g->agg_status    = SSD_STATUS_OK;

    pending_group_maybe_complete(F, (uint16_t)gi);
    munit_assert_uint8(flag, ==, 0U);
    munit_assert_int(st, ==, SSD_STATUS_IO_ERROR); /* 未写回 */

    g->next        = 2U;
    g->outstanding = 1U; /* 仍有在飞 */
    pending_group_maybe_complete(F, (uint16_t)gi);
    munit_assert_uint8(flag, ==, 0U);

    g->outstanding = 0U;
    pending_group_maybe_complete(F, (uint16_t)gi);
    munit_assert_uint8(flag, ==, 1U);
    munit_assert_int(st, ==, SSD_STATUS_OK);
    munit_assert_uint8(F->groups[gi].used, ==, 0U); /* 通知后释放 */

    return MUNIT_OK;
}

/* 粘性聚合状态：任一子操作非 OK ⇒ 组整体非 OK，后续成功不覆盖。 */
static MunitResult test_group_agg_status_sticky(const MunitParameter params[], void *fixture)
{
    ftl_context_t  *F  = ftl_fixture();
    int             gi = pending_group_alloc(F);
    ftl_io_group_t *g;
    uint8_t         flag = 0U;
    ssd_status_t    st   = SSD_STATUS_OK;

    (void)params;
    (void)fixture;

    munit_assert_int(gi, >=, 0);
    g = &F->groups[gi];

    g->complete_flag = &flag;
    g->status_out    = &st;
    g->total         = 3U;
    g->next          = 3U;
    g->outstanding   = 3U;
    g->agg_status    = SSD_STATUS_OK;

    pending_group_complete_subop(F, (uint16_t)gi, SSD_STATUS_IO_ERROR);
    munit_assert_uint16(g->outstanding, ==, 2U);
    munit_assert_int(g->agg_status, ==, SSD_STATUS_IO_ERROR);
    munit_assert_uint8(flag, ==, 0U);

    /* 后续成功不覆盖首个错误 */
    pending_group_complete_subop(F, (uint16_t)gi, SSD_STATUS_OK);
    munit_assert_uint16(g->outstanding, ==, 1U);
    munit_assert_int(g->agg_status, ==, SSD_STATUS_IO_ERROR);

    pending_group_complete_subop(F, (uint16_t)gi, SSD_STATUS_OK);
    munit_assert_uint8(flag, ==, 1U);
    munit_assert_int(st, ==, SSD_STATUS_IO_ERROR);

    return MUNIT_OK;
}

/* 恰一次：完成通知只发一次（on_complete 计数），且槽位在通知后才释放。 */
static MunitResult test_group_complete_once(const MunitParameter params[], void *fixture)
{
    ftl_context_t  *F  = ftl_fixture();
    int             gi = pending_group_alloc(F);
    ftl_io_group_t *g;
    uint8_t         flag = 0U;
    ssd_status_t    st   = SSD_STATUS_IO_ERROR;

    (void)params;
    (void)fixture;

    munit_assert_int(gi, >=, 0);
    g = &F->groups[gi];

    g_ut_cb_count = 0U;

    g->complete_flag = &flag;
    g->status_out    = &st;
    g->on_complete   = ftl_ut_group_cb;
    g->total         = 1U;
    g->next          = 1U;
    g->outstanding   = 1U;
    g->agg_status    = SSD_STATUS_OK;

    pending_group_complete_subop(F, (uint16_t)gi, SSD_STATUS_OK);

    munit_assert_uint8(flag, ==, 1U);
    munit_assert_int(st, ==, SSD_STATUS_OK);
    munit_assert_uint16(g_ut_cb_count, ==, 1U);
    munit_assert_uint8(F->groups[gi].used, ==, 0U);

    /* 槽位已释放 ⇒ 再调不再通知 */
    pending_group_maybe_complete(F, (uint16_t)gi);
    munit_assert_uint16(g_ut_cb_count, ==, 1U);
    munit_assert_uint8(flag, ==, 1U);

    return MUNIT_OK;
}

/* 重入：回调里申请新组槽，必须拿不到**正在释放**的那一槽（used=0 在通知之后）。 */
static MunitResult test_group_reentrant_callback(const MunitParameter params[], void *fixture)
{
    ftl_context_t  *F  = ftl_fixture();
    int             gi = pending_group_alloc(F);
    ftl_io_group_t *g;
    uint8_t         flag = 0U;
    ssd_status_t    st   = SSD_STATUS_IO_ERROR;

    (void)params;
    (void)fixture;

    munit_assert_int(gi, >=, 0);
    munit_assert_int(gi, ==, 0); /* 复位态下必是首槽，便于断言新槽号 */
    g = &F->groups[gi];

    g_ut_reent_ftl    = F;
    g_ut_reent_new_gi = -1;
    g_ut_reent_calls  = 0U;

    g->complete_flag = &flag;
    g->status_out    = &st;
    g->on_complete   = ftl_ut_reentrant_cb;
    g->total         = 1U;
    g->next          = 1U;
    g->outstanding   = 1U;
    g->agg_status    = SSD_STATUS_OK;

    pending_group_maybe_complete(F, (uint16_t)gi);

    munit_assert_uint16(g_ut_reent_calls, ==, 1U);
    munit_assert_int(g_ut_reent_new_gi, ==, 1);    /* 取到下一个空槽，而非槽 0 */
    munit_assert_uint8(F->groups[0].used, ==, 0U); /* 槽 0 在回调后已释放 */
    munit_assert_uint8(F->groups[1].used, ==, 1U); /* 回调申请的槽仍占用 */

    return MUNIT_OK;
}

/* 多组完成：位掩码内各减一次、掩码外不动；非 OK 状态逐组粘住。 */
static MunitResult test_group_complete_subop_multi(const MunitParameter params[], void *fixture)
{
    const char    *tag    = "group/multi";
    ftl_context_t *F      = ftl_fixture();
    uint32_t       owners = ftl_rand(tag) & 0x000000FFU;
    uint8_t        flags[8];
    ssd_status_t   stats[8];
    uint16_t       n_groups = 0U;
    uint8_t        i;

    (void)params;
    (void)fixture;

    if (owners == 0U)
    {
        owners = 0x01U; /* 掩码至少一位 */
    }

    /* 轮次 1：全部成功 */
    memset(flags, 0, sizeof(flags));
    memset(stats, 0, sizeof(stats));

    for (i = 0U; i < 8U; i++)
    {
        if ((owners & (uint32_t)(1U << i)) != 0U)
        {
            ftl_io_group_t *g = &F->groups[i];

            g->used          = 1U;
            g->total         = 1U;
            g->next          = 1U;
            g->outstanding   = 1U;
            g->agg_status    = SSD_STATUS_OK;
            g->complete_flag = &flags[i];
            g->status_out    = &stats[i];
            n_groups++;
        }
    }

    munit_assert_uint16(pending_group_count(F), ==, n_groups);

    pending_group_complete_subop_multi(F, owners, SSD_STATUS_OK);

    for (i = 0U; i < 8U; i++)
    {
        if ((owners & (uint32_t)(1U << i)) != 0U)
        {
            munit_assert_uint8(flags[i], ==, 1U);
            munit_assert_int(stats[i], ==, SSD_STATUS_OK);
        }
        else
        {
            munit_assert_uint8(flags[i], ==, 0U); /* 掩码外：不动 */
        }
        munit_assert_uint8(F->groups[i].used, ==, 0U);
    }
    munit_assert_uint16(pending_group_count(F), ==, 0U);

    /* 轮次 2：非 OK 状态逐组粘住 */
    memset(flags, 0, sizeof(flags));

    for (i = 0U; i < 8U; i++)
    {
        if ((owners & (uint32_t)(1U << i)) != 0U)
        {
            ftl_io_group_t *g = &F->groups[i];

            stats[i]         = SSD_STATUS_OK;
            g->used          = 1U;
            g->total         = 1U;
            g->next          = 1U;
            g->outstanding   = 1U;
            g->agg_status    = SSD_STATUS_OK;
            g->complete_flag = &flags[i];
            g->status_out    = &stats[i];
        }
    }

    pending_group_complete_subop_multi(F, owners, SSD_STATUS_IO_ERROR);

    for (i = 0U; i < 8U; i++)
    {
        if ((owners & (uint32_t)(1U << i)) != 0U)
        {
            munit_assert_uint8(flags[i], ==, 1U);
            munit_assert_int(stats[i], ==, SSD_STATUS_IO_ERROR);
            munit_assert_uint8(F->groups[i].used, ==, 0U);
        }
    }
    munit_assert_uint16(pending_group_count(F), ==, 0U);

    /* owners = 0：无组被通知（不崩） */
    pending_group_complete_subop_multi(F, 0U, SSD_STATUS_OK);
    munit_assert_uint16(pending_group_count(F), ==, 0U);

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * §11  套件数组
 *
 * 随机化用例（§5.1）用 MUNIT_TEST_OPTION_NONE：--iterations N 会真正跑 N 遍
 * （SINGLE_ITERATION 会把迭代压成 1，随机化就没意义了）。
 * ══════════════════════════════════════════════════════════════════════ */

#define FTL_UT_NONE MUNIT_TEST_OPTION_NONE
#define FTL_UT_ONCE MUNIT_TEST_OPTION_SINGLE_ITERATION

MunitTest g_ftl_allocator_tests[] = {
    {            (char *)"init_zeroes_pool",             test_alloc_init_zeroes_pool, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {         (char *)"backfill_fills_pool",          test_alloc_backfill_fills_pool, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {     (char *)"free_block_clears_state",      test_alloc_free_block_clears_state, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {               (char *)"close_open_sb",                test_alloc_close_open_sb, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {               (char *)"add_closed_sb",                test_alloc_add_closed_sb, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"remove_closed_sb_swap_remove", test_alloc_remove_closed_sb_swap_remove, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {            (char *)"remove_from_free",             test_alloc_remove_from_free, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {         (char *)"reserved_alloc_free",          test_alloc_reserved_alloc_free, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"open_sb_never_picks_metadata", test_alloc_open_sb_never_picks_metadata, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {     (char *)"open_sb_min_erase_count",      test_alloc_open_sb_min_erase_count, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    {                                  NULL,                                    NULL,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_ftl_watermark_tests[] = {
    {        (char *)"journal_wm_below_85",            test_wm_below_85, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {           (char *)"journal_wm_at_85",               test_wm_at_85, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    { (char *)"journal_wm_at_95_emergency",     test_wm_at_95_emergency, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    {        (char *)"emergency_set_reset", test_wm_emergency_set_reset, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"checkpoint_trigger_T1_T2_T3",  test_ckpt_trigger_T1_T2_T3, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {    (char *)"checkpoint_is_emergency",      test_ckpt_is_emergency, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {                                 NULL,                        NULL,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_ftl_gc_wl_tests[] = {
    {          (char *)"gc_victim_lowest_vpc",           test_gc_victim_lowest_vpc, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    {          (char *)"gc_victim_skips_open",           test_gc_victim_skips_open, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {           (char *)"gc_trigger_T1_T4_T5",            test_gc_trigger_T1_T4_T5, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    {              (char *)"wl_trigger_delta",               test_wl_trigger_delta, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {              (char *)"wl_trigger_ratio",               test_wl_trigger_ratio, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {           (char *)"wl_trigger_low_zone",            test_wl_trigger_low_zone, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"wl_victim_high_valid_low_erase", test_wl_victim_high_valid_low_erase, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    {   (char *)"wl_victim_read_count_weight",    test_wl_victim_read_count_weight, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {          (char *)"wl_max_min_avg_erase",           test_wl_max_min_avg_erase, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {         (char *)"wl_count_blocks_below",          test_wl_count_blocks_below, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {                                    NULL,                                NULL,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_ftl_rr_tests[] = {
    {           (char *)"record_ecc_rising",            test_rr_record_ecc_rising, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    { (char *)"record_ecc_health_monotonic",  test_rr_record_ecc_health_monotonic, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {   (char *)"check_predictive_critical",    test_rr_check_predictive_critical, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {                  (char *)"get_health",                   test_rr_get_health, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"predictive_scan_read_disturb", test_rr_predictive_scan_read_disturb, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {                (char *)"retire_block",                 test_rr_retire_block, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {                                  NULL,                                 NULL,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_ftl_bbt_tests[] = {
    {   (char *)"mark_bad_sets_bits",    test_bbt_mark_bad_sets_bits, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    {  (char *)"mark_bad_idempotent",   test_bbt_mark_bad_idempotent, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {    (char *)"spare_pool_refill",     test_bbt_spare_pool_refill, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"is_bad_set_bad_inline", test_bbt_is_bad_set_bad_inline, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {                           NULL,                           NULL,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_ftl_metadata_tests[] = {
    {           (char *)"dir_init_entries",            test_meta_dir_init_entries, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"dir_update_lookup_roundtrip", test_meta_dir_update_lookup_roundtrip, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    { (char *)"dir_lookup_missing_unknown",  test_meta_dir_lookup_missing_unknown, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {          (char *)"is_metadata_block",           test_meta_is_metadata_block, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {  (char *)"is_metadata_block_dynamic",   test_meta_is_metadata_block_dynamic, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {         (char *)"ring_addr_geometry",          test_meta_ring_addr_geometry, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {             (char *)"ring_addr_wrap",              test_meta_ring_addr_wrap, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {                                 NULL,                                  NULL,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_ftl_pending_ct_tests[] = {
    { (char *)"pending_add_lookup_remove",  test_pending_add_lookup_remove, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    {   (char *)"pending_full_32_rejects",    test_pending_full_32_rejects, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {             (char *)"pending_count",              test_pending_count, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"ct_add_remove_lookup_count", test_ct_add_remove_lookup_count, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {                                NULL,                            NULL,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_ftl_group_tests[] = {
    {           (char *)"group_alloc_free",            test_group_alloc_free, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {(char *)"group_maybe_complete_gating", test_group_maybe_complete_gating, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {    (char *)"group_agg_status_sticky",     test_group_agg_status_sticky, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {        (char *)"group_complete_once",         test_group_complete_once, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    {   (char *)"group_reentrant_callback",    test_group_reentrant_callback, ftl_setup, NULL,            FTL_UT_ONCE, NULL},
    { (char *)"group_complete_subop_multi",  test_group_complete_subop_multi, ftl_setup, NULL,            FTL_UT_NONE, NULL},
    {                                 NULL,                             NULL,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};
