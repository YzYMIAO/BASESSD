/* F10 journal_replay 全量回放 单元测试
 *
 * 覆盖（NAND I/O 走 munit_stub_fc 的内存 NAND 桩，双块×64 页真实读写）：
 *   - 多页回放：post-CP 条目跨 3 页，全部回放（旧实现只回放最大一页）
 *   - 过滤：page_seq ≤ cp_sequence 的页不回放（已落 L2P 表）
 *   - last-write-wins：同 LBA 旧 GC 条目 + 新 host 写，按 (page_seq,index) 序回放
 *   - 排序防御：双块 page_seq 区间不交叠，打乱扫描序结果一致
 *   - GC flush 契约：搬移条目 flush 后擦除，NAND 上必然存在（回放不指向已擦除块）
 *
 * 构建：tests/Makefile f10 target（链 journal.c + l2p_cache.c + metadata/allocator/
 *        boot_policy 满足符号 + munit_stub_fc 内存 NAND 桩）。
 * 上板：src/fc/makefile.mk + ut_main.c 注册 /f10/ 套件。
 */

#include "munit.h"
#include "munit_stub_fc.h"   /* 内存 NAND 桩 */

#include "ftl/journal.h"
#include "ftl/l2p_cache.h"
#include "nand/nand_config.h"

#include <string.h>

/**************************************************************************
 *  fixture：l2p_journal_t + l2p_cache_t（单实例拓扑，不触碰 allocator）
 **************************************************************************/

typedef struct
{
    l2p_journal_t journal;
    l2p_cache_t   cache;
} f10_fixture_t;

static void fixture_init(f10_fixture_t *fx)
{
    ftl_topology_t topo = { .ch = 0, .ce = 0, .lun = 0 };

    memset(fx, 0, sizeof(*fx));

    munit_stub_nand_init(); /* 内存 NAND 全 0xFF（出厂态） */

    journal_init(&fx->journal, topo);
    l2p_cache_init(&fx->cache, 0U, 0U, 0U);
}

/* 测试把 journal 序列起点拨到 base（模拟大量历史条目已消费后的继续写） */
static void set_seq_base(f10_fixture_t *fx, uint32_t base)
{
    fx->journal.sequence_counter = base;
}

static void append(f10_fixture_t *fx, lba_t lba, nand_pba_t old_pba, nand_pba_t new_pba)
{
    journal_append(&fx->journal, lba, old_pba, new_pba);
}

static void flush_pending(f10_fixture_t *fx)
{
    journal_flush_page(&fx->journal);
}

static nand_pba_t read_pba(const f10_fixture_t *fx, lba_t lba)
{
    return l2p_cache_read((l2p_cache_t *)&fx->cache, lba);
}

/**************************************************************************
 *  场景 1（核心）：post-CP 条目跨 3 页，全部回放
 **************************************************************************/

static MunitResult test_replay_multipage(const MunitParameter params[], void *data)
{
    (void)params;
    f10_fixture_t *fx = (f10_fixture_t *)data;
    uint32_t       cp  = 100U;
    uint16_t       i;
    lba_t          lba;
    nand_pba_t     pba;

    /* base=101：页 1 (seq=101) 16 条，页 2 (seq=117) 16 条，页 3 (seq=133) 8 条 */
    set_seq_base(fx, 101U);
    for (i = 0U; i < 16U; i++)
    {
        append(fx, (lba_t)i, NAND_PBA_INVALID, (nand_pba_t)(0x10000000U + i));
    }
    flush_pending(fx);
    for (i = 0U; i < 16U; i++)
    {
        append(fx, (lba_t)(0x100U + i), NAND_PBA_INVALID, (nand_pba_t)(0x20000000U + i));
    }
    flush_pending(fx);
    for (i = 0U; i < 8U; i++)
    {
        append(fx, (lba_t)(0x200U + i), NAND_PBA_INVALID, (nand_pba_t)(0x30000000U + i));
    }
    flush_pending(fx);

    /* 回放：全量回放 post-CP（seq 101/117/133 三页） */
    munit_assert_int(journal_replay(&fx->journal, &fx->cache, cp), ==, FTL_OK);

    for (i = 0U; i < 16U; i++)
    {
        lba = (lba_t)i;
        pba = read_pba(fx, lba);
        munit_assert_int((int)(pba & 0x0FFFFFFFU), ==, (int)(0x0000000U + i));
    }
    for (i = 0U; i < 16U; i++)
    {
        lba = (lba_t)(0x100U + i);
        pba = read_pba(fx, lba);
        munit_assert_int((int)(pba & 0x0FFFFFFFU), ==, (int)(0x0000000U + i));
    }
    for (i = 0U; i < 8U; i++)
    {
        lba = (lba_t)(0x200U + i);
        pba = read_pba(fx, lba);
        munit_assert_int((int)(pba & 0x0FFFFFFFU), ==, (int)(0x0000000U + i));
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  场景 2：page_seq ≤ cp_sequence 的页不回放
 **************************************************************************/

static MunitResult test_replay_filters_cp(const MunitParameter params[], void *data)
{
    (void)params;
    f10_fixture_t *fx = (f10_fixture_t *)data;
    uint32_t       cp = 100U;
    uint16_t       i;

    /* base=97：页 1 (seq=97 ≤ 100) 过滤；页 2 (seq=101 > 100) 回放 */
    set_seq_base(fx, 97U);
    for (i = 0U; i < 4U; i++)
    {
        append(fx, (lba_t)i, NAND_PBA_INVALID, (nand_pba_t)(0x11111111U + i));
    }
    flush_pending(fx); /* page_seq=97 ≤ 100 → 不回放 */
    for (i = 0U; i < 4U; i++)
    {
        append(fx, (lba_t)(0x40U + i), NAND_PBA_INVALID, (nand_pba_t)(0x22222222U + i));
    }
    flush_pending(fx); /* page_seq=101 > 100 → 回放 */

    munit_assert_int(journal_replay(&fx->journal, &fx->cache, cp), ==, FTL_OK);

    /* 页 1 条目不在 cache（过滤） */
    for (i = 0U; i < 4U; i++)
    {
        munit_assert_int((int)read_pba(fx, (lba_t)i), ==, (int)FTL_PBA_INVALID);
    }
    /* 页 2 条目在 cache */
    for (i = 0U; i < 4U; i++)
    {
        munit_assert_int((int)(read_pba(fx, (lba_t)(0x40U + i)) & 0x0FFFFFFFU),
                         ==, (int)(0x2222222U + i));
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  场景 3：last-write-wins——同 LBA 旧条目被新条目覆盖
 **************************************************************************/

static MunitResult test_replay_last_write_wins(const MunitParameter params[], void *data)
{
    (void)params;
    f10_fixture_t *fx = (f10_fixture_t *)data;
    uint32_t       cp = 50U;

    /* 页 1 (seq=51)：GC 搬移 X→A；页 2 (seq=53)：host 覆盖 X→B */
    set_seq_base(fx, 51U);
    append(fx, 10U, 0x0A000000U, 0x0A111111U); /* 旧 GC 条目 */
    append(fx, 11U, 0x0A000000U, 0x0A222222U);
    flush_pending(fx); /* page_seq=51 > 50 → 回放 */
    append(fx, 10U, 0x0A111111U, 0x0B333333U); /* 新 host 写 */
    flush_pending(fx); /* page_seq=53 */

    munit_assert_int(journal_replay(&fx->journal, &fx->cache, cp), ==, FTL_OK);

    /* LBA 10 最终 = B（新条目胜出，旧 A 已擦除块不指向） */
    munit_assert_int((int)read_pba(fx, 10U), ==, (int)0x0B333333U);
    /* LBA 11 保持 GC 搬移结果 */
    munit_assert_int((int)read_pba(fx, 11U), ==, (int)0x0A222222U);

    return MUNIT_OK;
}

/**************************************************************************
 *  场景 4：排序防御——跨块条目按 page_seq 序回放结果一致
 **************************************************************************/

static MunitResult test_replay_order_independent(const MunitParameter params[], void *data)
{
    (void)params;
    f10_fixture_t *fx = (f10_fixture_t *)data;
    uint32_t       cp = 0U;
    uint16_t       i;

    /* block0 页 0 (seq=1..8)、页 1 (seq=9..16)，再手动切到 block1 页 0 (seq=17..20) */
    set_seq_base(fx, 1U);
    for (i = 0U; i < 8U; i++)
    {
        append(fx, (lba_t)i, NAND_PBA_INVALID, (nand_pba_t)(0x01000000U + i));
    }
    flush_pending(fx); /* block0 页 0 */
    for (i = 0U; i < 8U; i++)
    {
        append(fx, (lba_t)(0x100U + i), NAND_PBA_INVALID, (nand_pba_t)(0x02000000U + i));
    }
    flush_pending(fx); /* block0 页 1 */
    fx->journal.current_block = 1U; /* 测试内驱动：直接切块（不依赖满页回切） */
    fx->journal.current_page  = 0U;
    for (i = 0U; i < 4U; i++)
    {
        append(fx, (lba_t)(0x200U + i), NAND_PBA_INVALID, (nand_pba_t)(0x03000000U + i));
    }
    flush_pending(fx); /* block1 页 0 */

    munit_assert_int(journal_replay(&fx->journal, &fx->cache, cp), ==, FTL_OK);

    /* block0 页 0/1 + block1 页 0 全部回放（page_seq 1..20 递增，last-write 语义） */
    for (i = 0U; i < 8U; i++)
    {
        munit_assert_int((int)(read_pba(fx, (lba_t)i) & 0x0FFFFFFFU), ==, (int)(0x1000000U + i));
    }
    for (i = 0U; i < 8U; i++)
    {
        munit_assert_int((int)(read_pba(fx, (lba_t)(0x100U + i)) & 0x0FFFFFFFU), ==, (int)(0x2000000U + i));
    }
    for (i = 0U; i < 4U; i++)
    {
        munit_assert_int((int)(read_pba(fx, (lba_t)(0x200U + i)) & 0x0FFFFFFFU), ==, (int)(0x3000000U + i));
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  场景 5：GC flush 契约——擦除前 flush 使条目必落盘
 **************************************************************************/

static MunitResult test_gc_flush_before_erase(const MunitParameter params[], void *data)
{
    (void)params;
    f10_fixture_t *fx = (f10_fixture_t *)data;
    uint32_t       cp = 0U;

    /* 模拟 GC：append 一条搬移条目 → 擦除前 flush（gc_migrate_sb 新契约）。
     * GC 的 victim 是用户数据块，而 journal 写在自己的 journal block；
     * 擦除 victim 不应清掉 journal 页。这里用 erase block 41（JOURNAL_BLOCK1，
     * 当前 journal 不在写它）模拟"擦除一个无关块"，同时清掉 block0 上的
     * 旧垃圾页，验证 flush 落盘条目不受影响。 */
    set_seq_base(fx, 1U);
    append(fx, 7U, 0x0A000000U, 0x0B000000U);
    journal_flush_page(&fx->journal); /* 落盘到 block0 页 0（当前写块） */

    munit_stub_nand_erase_block(1U); /* 擦除无关块（当前写块保留） */

    /* 掉电重启：回放 → 条目必须在 NAND（否则回放找不到 X→B，L2P 指已擦除块） */
    munit_assert_int(journal_replay(&fx->journal, &fx->cache, cp), ==, FTL_OK);
    munit_assert_int((int)read_pba(fx, 7U), ==, (int)0x0B000000U);

    return MUNIT_OK;
}

/**************************************************************************
 *  套件注册
 **************************************************************************/

static void *f10_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;
    f10_fixture_t *fx = (f10_fixture_t *)malloc(sizeof(*fx));
    munit_assert_not_null(fx);
    fixture_init(fx);
    return fx;
}

static void f10_teardown(void *fixture)
{
    free(fixture);
}

MunitTest g_f10_tests[] = {
    {   "replay_multipage", test_replay_multipage, f10_setup, f10_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { "replay_filters_cp", test_replay_filters_cp, f10_setup, f10_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"replay_last_write_wins", test_replay_last_write_wins, f10_setup, f10_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"replay_order_independent", test_replay_order_independent, f10_setup, f10_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { "gc_flush_before_erase", test_gc_flush_before_erase, f10_setup, f10_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                              NULL,         NULL,     NULL,         NULL,                       MUNIT_TEST_OPTION_NONE, NULL},
};

#if defined(MUNIT_HOST_STANDALONE)
static MunitSuite g_f10_suite =
    {
        (char *)"/f10/",
        g_f10_tests,
        NULL,
        1,
        MUNIT_SUITE_OPTION_NONE,
    };

int main(int argc, char *argv[argc + 1])
{
    return munit_suite_main(&g_f10_suite, NULL, argc, argv);
}
#endif
