/* F2 掉电重建 closed_sb_list 相关的纯 RAM 单元测试
 *
 * 覆盖：allocator_add_closed_sb（簿记 + sb_id 契约）、
 *      allocator_remove_from_free / allocator_pool_remove、
 *      allocator_remove_closed_sb swap-remove 后 sb_id 修正、
 *      ftl_is_metadata_block 命中/不命中。
 *
 * 纯 RAM 测试：构造 ftl_allocator_t + 手工 ftl_block_t blocks[]，
 * 不触碰 NAND I/O（gc_read_page_with_lba / l2p_cache_read 不在此测）。
 */

#include "munit.h"

#include "ftl/allocator.h"
#include "ftl/metadata.h"
#include "ftl/checkpoint.h"
#include "ftl/bbt.h"
#include "ftl/journal.h"
#include "nand/nand_config.h"

#include <string.h>

/* 被测静态：allocator_pool_remove 通过 allocator_remove_from_free 间接覆盖 */

/**************************************************************************
 *  工具：构造 allocator 上下文
 **************************************************************************/

typedef struct
{
    ftl_block_t     blocks[FTL_BLOCK_TOTAL];
    uint16_t        free_blocks[FTL_BLOCK_TOTAL];
    super_block_t   closed_sb_list[SUPER_BLOCK_LIST_MAX];
    ftl_allocator_t alloc;
    metadata_dir_cache_t md_dir;
} rebuild_fixture_t;

static void fixture_init(rebuild_fixture_t *fx)
{
    ftl_init_params_t params;

    memset(fx, 0, sizeof(*fx));
    /* 全块非坏，全 FREE（allocator_init 会再置一遍，这里只需清坏位） */
    memset(fx->blocks, 0, sizeof(fx->blocks));

    params.topo           = (ftl_topology_t){ .ch = 0, .ce = 0, .lun = 0 };
    params.blocks         = fx->blocks;
    params.free_blocks    = fx->free_blocks;
    params.free_count     = 0;
    params.closed_sb_list = fx->closed_sb_list;

    munit_assert_int(allocator_init(&fx->alloc, &params), ==, FTL_OK);

    /* md_dir 固定 L2P region 映射（默认 init：region r → block r/64，0..31） */
    fx->md_dir.topo = params.topo;
    metadata_dir_init(&fx->md_dir);
}

/* 往 free pool 塞 gblock（模拟 main.c 回填非坏块） */
static void push_free(rebuild_fixture_t *fx, uint16_t gblock)
{
    allocator_free_block(&fx->alloc, gblock);
}

/**************************************************************************
 *  test_allocator_add_closed_sb_*
 **************************************************************************/

static MunitResult test_add_closed_sb_basic(const MunitParameter params[], void *data)
{
    (void)params;
    rebuild_fixture_t *fx = (rebuild_fixture_t *)data;
    const uint16_t     pair[PLANE_NUM] = { 100U, 100U }; /* 同号双 plane */
    uint16_t           idx;
    ftl_block_t       *b0;
    ftl_block_t       *b1;

    idx = allocator_add_closed_sb(&fx->alloc, pair, FTL_TEMP_HOT);
    munit_assert_int(idx, ==, 0);

    /* 列表条目字段 */
    munit_assert_int(fx->alloc.closed_sb_count, ==, 1);
    munit_assert_int(fx->closed_sb_list[0].plane_blocks[0], ==, 100);
    munit_assert_int(fx->closed_sb_list[0].plane_blocks[1], ==, 100);
    munit_assert_int(fx->closed_sb_list[0].next_page, ==, PAGES_PER_BLOCK);
    munit_assert_int(fx->closed_sb_list[0].temperature, ==, FTL_TEMP_HOT);
    munit_assert_int(fx->closed_sb_list[0].valid, ==, 0);
    munit_assert_int(fx->closed_sb_list[0].topo.ch, ==, 0);
    munit_assert_int(fx->closed_sb_list[0].topo.ce, ==, 0);
    munit_assert_int(fx->closed_sb_list[0].topo.lun, ==, 0);

    /* 双 plane 块状态：CLOSED + 清 FREE/OPEN + 温度 + sb_id=索引 */
    b0 = &fx->blocks[ftl_gblock(0U, 100U)];
    b1 = &fx->blocks[ftl_gblock(1U, 100U)];
    munit_assert_int(ftl_block_state_is(b0, FTL_BLOCK_STATE_CLOSED), !=, 0);
    munit_assert_int(ftl_block_state_is(b0, FTL_BLOCK_STATE_FREE), ==, 0);
    munit_assert_int(ftl_block_state_is(b0, FTL_BLOCK_STATE_OPEN), ==, 0);
    munit_assert_int(ftl_block_get_temp(b0), ==, FTL_TEMP_HOT);
    munit_assert_int(ftl_block_sb_id(b0), ==, 0);
    munit_assert_int(ftl_block_state_is(b1, FTL_BLOCK_STATE_CLOSED), !=, 0);
    munit_assert_int(ftl_block_sb_id(b1), ==, 0);

    return MUNIT_OK;
}

static MunitResult test_add_closed_sb_full(const MunitParameter params[], void *data)
{
    (void)params;
    rebuild_fixture_t *fx = (rebuild_fixture_t *)data;
    const uint16_t     pair[PLANE_NUM] = { 5U, 5U };
    uint16_t           i;

    /* 塞满 SUPER_BLOCK_LIST_MAX 条（1024 个不同 block，双 plane 各 1024 块足够） */
    for (i = 0U; i < SUPER_BLOCK_LIST_MAX; i++)
    {
        uint16_t b = (uint16_t)(2U * i + 1U); /* 奇数 block 号，避免与已用冲突 */
        uint16_t idx;
        if (b >= BLOCKS_PER_PLANE)
        {
            break; /* 防御：fixture 块有限，不至于走满 */
        }
        idx = allocator_add_closed_sb(&fx->alloc, (const uint16_t[PLANE_NUM]){ b, b }, FTL_TEMP_COLD);
        munit_assert_int(idx, ==, i);
    }

    munit_assert_int(fx->alloc.closed_sb_count, ==, SUPER_BLOCK_LIST_MAX);

    /* 满后返回 UINT16_MAX 且不写入 */
    {
        uint16_t idx = allocator_add_closed_sb(&fx->alloc, pair, FTL_TEMP_HOT);
        munit_assert_int(idx, ==, UINT16_MAX);
        munit_assert_int(fx->alloc.closed_sb_count, ==, SUPER_BLOCK_LIST_MAX);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_allocator_remove_from_free / swap-remove 一致性
 **************************************************************************/

static MunitResult test_remove_from_free(const MunitParameter params[], void *data)
{
    (void)params;
    rebuild_fixture_t *fx = (rebuild_fixture_t *)data;
    uint16_t           i;

    /* 塞 3 个块进 free pool */
    push_free(fx, ftl_gblock(0U, 10U));
    push_free(fx, ftl_gblock(1U, 10U));
    push_free(fx, ftl_gblock(0U, 20U));
    munit_assert_int(fx->alloc.free_count, ==, 3);

    /* 移除一个（swap-remove） */
    allocator_remove_from_free(&fx->alloc, ftl_gblock(0U, 10U));
    munit_assert_int(fx->alloc.free_count, ==, 2);

    /* 剩余两个都在 */
    {
        uint16_t found = 0U;
        for (i = 0U; i < fx->alloc.free_count; i++)
        {
            if ((fx->alloc.free_blocks[i] == ftl_gblock(1U, 10U)) ||
                (fx->alloc.free_blocks[i] == ftl_gblock(0U, 20U)))
            {
                found++;
            }
        }
        munit_assert_int(found, ==, 2);
    }

    /* 移除不存在的块：无害 */
    allocator_remove_from_free(&fx->alloc, ftl_gblock(0U, 999U));
    munit_assert_int(fx->alloc.free_count, ==, 2);

    return MUNIT_OK;
}

static MunitResult test_remove_closed_sb_sbid_fix(const MunitParameter params[], void *data)
{
    (void)params;
    rebuild_fixture_t *fx = (rebuild_fixture_t *)data;
    uint16_t           idx_a;
    uint16_t           idx_b;

    /* 两个 closed SB：block 50 与 60 */
    idx_a = allocator_add_closed_sb(&fx->alloc, (const uint16_t[PLANE_NUM]){ 50U, 50U }, FTL_TEMP_HOT);
    idx_b = allocator_add_closed_sb(&fx->alloc, (const uint16_t[PLANE_NUM]){ 60U, 60U }, FTL_TEMP_COLD);
    munit_assert_int(idx_a, ==, 0);
    munit_assert_int(idx_b, ==, 1);
    munit_assert_int(ftl_block_sb_id(&fx->blocks[ftl_gblock(0U, 60U)]), ==, 1);

    /* 移除索引 0（block 50）→ swap-remove：索引 1（block 60）搬到索引 0 并修正 sb_id */
    allocator_remove_closed_sb(&fx->alloc, 0U);
    munit_assert_int(fx->alloc.closed_sb_count, ==, 1);

    /* 剩下的是 block 60，且其 sb_id 从 1 修正为 0 */
    munit_assert_int(fx->closed_sb_list[0].plane_blocks[0], ==, 60);
    munit_assert_int(ftl_block_sb_id(&fx->blocks[ftl_gblock(0U, 60U)]), ==, 0);
    munit_assert_int(ftl_block_sb_id(&fx->blocks[ftl_gblock(1U, 60U)]), ==, 0);

    /* 被移除的 block 50 的 sb_id 已复位 NO_SB */
    munit_assert_int(ftl_block_sb_id(&fx->blocks[ftl_gblock(0U, 50U)]), ==, FTL_BLOCK_NO_SB);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_ftl_is_metadata_block
 **************************************************************************/

static MunitResult test_is_metadata_block(const MunitParameter params[], void *data)
{
    (void)params;
    rebuild_fixture_t *fx = (rebuild_fixture_t *)data;

    /* 固定 L2P 表区（block < 32） */
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 0U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 31U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 1U), ==, 1);

    /* Anchor 33/34 */
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 33U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 34U), ==, 1);

    /* BBT 36/39、Checkpoint 37/38、Journal 40/41 */
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 36U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 39U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 37U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 38U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 40U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 41U), ==, 1);

    /* 32（固定表区之外第一个，非 Anchor） */
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 32U), ==, 0);

    /* 普通用户块不命中 */
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 35U), ==, 0);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 42U), ==, 0);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 100U), ==, 0);
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 2047U), ==, 0);

    return MUNIT_OK;
}

/* 动态 region 块：往 md_dir 追加一个 >= 32 的 L2P region 位置 */
static MunitResult test_is_metadata_block_dynamic(const MunitParameter params[], void *data)
{
    (void)params;
    rebuild_fixture_t *fx = (rebuild_fixture_t *)data;

    /* 未登记时，block 200 不是元数据 */
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 200U), ==, 0);

    /* 登记 region 5 到 block 200（page/plane 随便） */
    metadata_dir_update(&fx->md_dir, METADATA_TYPE_L2P_REGION, 5U,
                        paa_make(0U, 0U, 200U, 0U, 0U));
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 200U), ==, 1);

    /* 同 block 其他 L2P region 也命中 */
    metadata_dir_update(&fx->md_dir, METADATA_TYPE_L2P_REGION, 6U,
                        paa_make(0U, 0U, 201U, 0U, 0U));
    munit_assert_int(ftl_is_metadata_block(&fx->md_dir, 201U), ==, 1);

    /* NULL md_dir：固定区间仍命中，动态不适用 */
    munit_assert_int(ftl_is_metadata_block(NULL, 40U), ==, 1);
    munit_assert_int(ftl_is_metadata_block(NULL, 200U), ==, 0);

    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 **************************************************************************/

static void *rebuild_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;
    rebuild_fixture_t *fx = (rebuild_fixture_t *)malloc(sizeof(*fx));
    if (fx == NULL)
    {
        return NULL;
    }
    fixture_init(fx);
    return fx;
}

static void rebuild_teardown(void *fixture)
{
    free(fixture);
}

MunitTest g_rebuild_tests[] = {
    {   "add_closed_sb_basic", test_add_closed_sb_basic, rebuild_setup, rebuild_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "add_closed_sb_full",  test_add_closed_sb_full, rebuild_setup, rebuild_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      "remove_from_free",   test_remove_from_free, rebuild_setup, rebuild_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  "remove_closed_sbid_fix", test_remove_closed_sb_sbid_fix, rebuild_setup, rebuild_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "is_metadata_block", test_is_metadata_block, rebuild_setup, rebuild_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"is_metadata_block_dyn", test_is_metadata_block_dynamic, rebuild_setup, rebuild_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                       NULL,                     NULL,         NULL,             NULL,                        MUNIT_TEST_OPTION_NONE, NULL},
};
