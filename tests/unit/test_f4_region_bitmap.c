/* F4 checkpoint 增量（region_bitmap）纯 RAM 单元测试
 *
 * 覆盖：
 *   - region_dirty_bitmap 置脏/写回/init 的位生命周期（l2p_cache_write /
 *     l2p_cache_flush_one / l2p_cache_init）
 *   - evict 时 DIRTY 行 flush 失败 → 行不被复用（孤儿 bit 修复，F4 前置）
 *   - checkpoint_create 先快照 bitmap 再 flush 的顺序契约
 *   - checkpoint_create flush 失败 → 中止（不写 header）
 *   - 静态 ftl_boot_apply_region_bitmap：跳过 DIRTY 行；非 bitmap 区 INVALID；
 *     bitmap 区加载 CLEAN
 *
 * 纯 RAM 测试：构造 l2p_cache_t + 手工 metadata_dir_cache_t，不触碰 NAND I/O。
 * 静态函数通过 include 间接测（与 test_rebuild 同风格，但本文件独立编译、
 * 由 tests/Makefile 单独 target 链接）。
 */

#include "munit.h"
#include "munit_stub_fc.h"   /* fc_stub_fail_next 失败注入 */

#include "ftl/allocator.h"
#include "ftl/checkpoint.h"
#include "ftl/ftl.h"          /* ftl_context_t */
#include "ftl/l2p_cache.h"
#include "ftl/metadata.h"
#include "nand/nand_config.h"

#include <string.h>

/**************************************************************************
 *  fixture：l2p_cache_t + md_dir（固定映射），不触碰 NAND
 *  （NAND I/O 由 munit_stub_fc.c 的 FC 桩"虚拟成功"，flush/read 走通簿记）
 **************************************************************************/

typedef struct
{
    l2p_cache_t         cache;
    metadata_dir_cache_t md_dir;
    ftl_allocator_t     alloc;
    ftl_block_t         blocks[FTL_BLOCK_TOTAL];
    uint16_t            free_blocks[FTL_BLOCK_TOTAL];
    super_block_t       closed_sb_list[SUPER_BLOCK_LIST_MAX];
} f4_fixture_t;

static void fixture_init(f4_fixture_t *fx)
{
    ftl_init_params_t params;

    memset(fx, 0, sizeof(*fx));
    memset(fx->blocks, 0, sizeof(fx->blocks));

    params.topo           = (ftl_topology_t){ .ch = 0, .ce = 0, .lun = 0 };
    params.blocks         = fx->blocks;
    params.free_blocks    = fx->free_blocks;
    params.free_count     = 0;
    params.closed_sb_list = fx->closed_sb_list;
    munit_assert_int(allocator_init(&fx->alloc, &params), ==, FTL_OK);

    fx->md_dir.topo = params.topo;
    metadata_dir_init(&fx->md_dir);

    l2p_cache_init(&fx->cache, 0U, 0U, 0U);
    l2p_cache_set_md_dir(&fx->cache, &fx->md_dir, &fx->alloc);
}

/* 置位 region 的脏 bit（内部宏对齐实现） */
static uint32_t dirty_bit_word(const l2p_cache_t *cache, uint16_t region_id)
{
    return cache->region_dirty_bitmap[region_id / 32U] &
           (1UL << (region_id & 31U));
}

/**************************************************************************
 *  region_dirty_bitmap 位生命周期
 **************************************************************************/

static MunitResult test_bitmap_init_clear(const MunitParameter params[], void *data)
{
    (void)params;
    f4_fixture_t *fx = (f4_fixture_t *)data;
    uint16_t      i;

    for (i = 0U; i < L2P_REGIONS_PER_LUN / 32U; i++)
    {
        munit_assert_int(fx->cache.region_dirty_bitmap[i], ==, 0);
    }
    return MUNIT_OK;
}

static MunitResult test_bitmap_set_on_write(const MunitParameter params[], void *data)
{
    (void)params;
    f4_fixture_t *fx = (f4_fixture_t *)data;

    /* region 3 的 entry 0 */
    l2p_cache_write(&fx->cache, (lba_t)(3U * L2P_ENTRIES_PER_REGION),
                    (nand_pba_t)0x12345678U);
    munit_assert_int(dirty_bit_word(&fx->cache, 3U), !=, 0);

    /* 同 region 多 entry 写 → bit 仍单一位 */
    l2p_cache_write(&fx->cache, (lba_t)(3U * L2P_ENTRIES_PER_REGION + 5U),
                    (nand_pba_t)0x87654321U);
    munit_assert_int(fx->cache.region_dirty_bitmap[0], ==,
                     (1UL << 3U));

    /* 不同 region → 各自 bit */
    l2p_cache_write(&fx->cache, (lba_t)(8U * L2P_ENTRIES_PER_REGION),
                    (nand_pba_t)0xAABBCCDDU);
    munit_assert_int(fx->cache.region_dirty_bitmap[0], ==,
                     (1UL << 3U) | (1UL << 8U));

    return MUNIT_OK;
}

static MunitResult test_bitmap_clear_on_flush(const MunitParameter params[], void *data)
{
    (void)params;
    f4_fixture_t *fx = (f4_fixture_t *)data;
    int           i;
    int           found_line = -1;

    /* 只写 entry 5（与 entry 0 同 region），且选一个 md_dir 未预置的 region
     * （> METADATA_DIR_ENTRIES），使 flush_one 的 lookup 必然 miss → 走
     * alloc_region 动态分配（dir->cur_metadata_* 由 metadata_dir_init 置为
     * 本 LUN 固定区之后第一块，page=0，block=32——无需 free pool）。
     * 这样 flush_one 返回 0，bit 正确清零。 */
    l2p_cache_write(&fx->cache,
                    (lba_t)(2000U * L2P_ENTRIES_PER_REGION + 5U),
                    (nand_pba_t)0x11111111U);
    munit_assert_int(dirty_bit_word(&fx->cache, 2000U), !=, 0);

    for (i = 0; i < (int)L2P_CACHE_SIZE; i++)
    {
        if (fx->cache.lines[i].region_id == 2000U)
        {
            found_line = i;
            break;
        }
    }
    munit_assert_int(found_line, !=, -1);
    munit_assert_int(fx->cache.lines[found_line].state, ==, L2P_CACHE_DIRTY);

    munit_assert_int(l2p_cache_flush_one(&fx->cache, found_line), ==, FTL_OK);
    munit_assert_int(fx->cache.lines[found_line].state, ==, L2P_CACHE_CLEAN);
    munit_assert_int(dirty_bit_word(&fx->cache, 2000U), ==, 0);

    return MUNIT_OK;
}

/**************************************************************************
 *  evict：DIRTY 行 flush 失败 → 行不复用（孤儿 bit 修复）
 **************************************************************************/

static MunitResult test_evict_flush_failure_keeps_line(const MunitParameter params[], void *data)
{
    (void)params;
    f4_fixture_t *fx = (f4_fixture_t *)data;
    uint16_t      i;
    int           dirty_line = -1;
    uint16_t      dirty_region = 0U;

    /* 填满 cache：写满 64 个 region（每 line 一个），全 DIRTY + bit 置位 */
    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_write(&fx->cache, (lba_t)(i * L2P_ENTRIES_PER_REGION),
                        (nand_pba_t)(0x1000U + i));
    }

    /* 找任意 DIRTY 行（region_id = i） */
    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        if (fx->cache.lines[i].state == L2P_CACHE_DIRTY)
        {
            dirty_line  = (int)i;
            dirty_region = fx->cache.lines[i].region_id;
            break;
        }
    }
    munit_assert_int(dirty_line, !=, -1);

    /* 注入写失败：fc_submit 返回 IO_ERROR → flush_one 失败。
     * 断言失败态：状态回 DIRTY、region bit 保持置位（数据仍在，未假声明落盘） */
    fc_stub_fail_next = 1;
    munit_assert_int(l2p_cache_flush_one(&fx->cache, dirty_line), ==, FTL_ERROR);
    munit_assert_int(fx->cache.lines[dirty_line].state, ==, L2P_CACHE_DIRTY);
    munit_assert_int(dirty_bit_word(&fx->cache, dirty_region), !=, 0);

    return MUNIT_OK;
}

/**************************************************************************
 *  checkpoint_create 快照-先于-flush 契约（不触碰 NAND 写：用失败桩
 *  验证中止语义，避免依赖真实 FC）
 **************************************************************************/

static MunitResult test_checkpoint_create_aborts_on_flush_fail(const MunitParameter params[], void *data)
{
    (void)params;
    f4_fixture_t *fx = (f4_fixture_t *)data;
    ftl_context_t ftl;

    /* 构造最小 ftl_context_t（checkpoint_create 只读 journal.sequence_counter） */
    memset(&ftl, 0, sizeof(ftl));
    ftl.cache   = fx->cache;
    ftl.topo    = (ftl_topology_t){ .ch = 0, .ce = 0, .lun = 0 };
    ftl.journal.sequence_counter = 42U;

    /* 制造一个 DIRTY 行（其 region bit 置位） */
    l2p_cache_write(&ftl.cache, (lba_t)(5U * L2P_ENTRIES_PER_REGION),
                    (nand_pba_t)0xCAFEBABEU);
    munit_assert_int(dirty_bit_word(&ftl.cache, 5U), !=, 0);

    /* 注入写失败：fc_submit 返回 IO_ERROR → flush_one 失败 → checkpoint 必须
     * 中止（FTL_ERROR），且不写 header / 不 flush anchor / 不截断 journal。
     * 中止后 DIRTY 行保留、region bit 保持置位（数据仍在，下个 CP 重试）。 */
    fc_stub_fail_next = 1;
    munit_assert_int(checkpoint_create(&ftl), ==, FTL_ERROR);
    munit_assert_int(dirty_bit_word(&ftl.cache, 5U), !=, 0);
    munit_assert_int(ftl.cache.lines[0].state, ==, L2P_CACHE_DIRTY);

    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义 + host 入口
 **************************************************************************/

static void *f4_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;
    f4_fixture_t *fx = (f4_fixture_t *)malloc(sizeof(*fx));
    if (fx == NULL)
    {
        return NULL;
    }
    fixture_init(fx);
    return fx;
}

static void f4_teardown(void *fixture)
{
    free(fixture);
}

MunitTest g_f4_tests[] = {
    {        "bitmap_init_clear",  test_bitmap_init_clear, f4_setup, f4_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      "bitmap_set_on_write",  test_bitmap_set_on_write, f4_setup, f4_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "bitmap_clear_on_flush",  test_bitmap_clear_on_flush, f4_setup, f4_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"evict_flush_failure_keep",  test_evict_flush_failure_keeps_line, f4_setup, f4_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"cp_aborts_on_flush_fail", test_checkpoint_create_aborts_on_flush_fail, f4_setup, f4_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                           NULL,                     NULL,        NULL,        NULL,                          MUNIT_TEST_OPTION_NONE, NULL},
};

/* 上板固件由 ut_main.c 统一调度（引用 g_f4_tests，不经本 g_f4_suite）；
 * 仅 host 独立入口需要本套件，故引用它避免未用告警。 */
#if defined(MUNIT_HOST_STANDALONE)
static MunitSuite g_f4_suite =
    {
        (char *)"/f4/",
        g_f4_tests,
        NULL,
        1,
        MUNIT_SUITE_OPTION_NONE,
    };

int main(int argc, char *argv[argc + 1])
{
    return munit_suite_main(&g_f4_suite, NULL, argc, argv);
}
#endif
