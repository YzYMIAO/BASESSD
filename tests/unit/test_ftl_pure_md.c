/*
 * test_ftl_pure_md.c — L0 /ftl/pure/md/：metadata / checkpoint / journal
 *
 * 只碰 RAM：Directory/ring 纯逻辑与编码器；不读介质。
 */

#include "ftl_ut_common.h"
#include "journal.h"
#include "metadata.h"
#include "metadata_job.h"
#include "munit.h"

#include <string.h>

static MunitResult test_metadata_idle(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t *ftl = ftl_ut_live();

    ftl_ut_trace_begin();

    munit_assert_not_null(ftl->metadata_dir);
    munit_assert_true(md_handle_is_idle(ftl->metadata_dir));

    ftl_ut_trace_summary("metadata: idle, live_blocks=%u", md_live_block_count_get(ftl->metadata_dir));

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

    ftl_ut_trace_summary("ring: records/block=%hu total=%hu", md_ring_records_per_block(&ring), md_ring_total_records(&ring));

    return MUNIT_OK;
}

static MunitResult test_directory_commit(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();

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

    ftl_ut_trace_summary("directory: primary/backup commit ok, dirty_gen=%lu", dir->dirty_gen);

    return MUNIT_OK;
}

static MunitResult test_journal_append(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_jrn_t journal;

    ftl_ut_trace_begin();
    ftl_ut_ram_init();
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

    ftl_ut_trace_summary("journal: seq=%lu watermark=%hhu pending=%d", jrn_sequence_get(&journal), jrn_watermark_get(&journal), (int)jrn_has_pending_entries(&journal));

    return MUNIT_OK;
}

MunitTest g_ftl_pure_md_tests[] = {
    {   (char *)"metadata_idle",    test_metadata_idle, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"ring_geometry",    test_ring_geometry, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"directory_commit", test_directory_commit, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"journal_append",   test_journal_append, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                      NULL,                  NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
