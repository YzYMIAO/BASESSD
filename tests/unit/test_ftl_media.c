/*
 * test_ftl_media.c — /ftl/media/：真 NAND 路径（破坏性）
 *
 * ⚠ 本文件的用例会在板端执行 format / 写 NAND；运行前拔掉 USB，跑完 format + POR。
 *    当前为脚手架：先验证活实例与结束不变量，真 NAND 用例按阶段 P4/P5 落地。
 */

#include "ftl_ut_common.h"
#include "munit.h"

#include <string.h>

#define FTL_UT_MEDIA_LBAS 16U

static BYTE       s_wr_buf[FTL_UT_MEDIA_LBAS * FTL_IU_BYTES];
static BYTE       s_rd_buf[FTL_UT_MEDIA_LBAS * FTL_IU_BYTES];
static BYTE       s_zero_buf[FTL_UT_MEDIA_LBAS * FTL_IU_BYTES];
static bool       s_media_ready;
static ftl_ctx_t *s_media_ftl;

static MunitResult test_media_live(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t *ftl = ftl_ut_live();

    ftl_ut_trace_begin();
    ftl_ut_assert_clean(ftl);

    ftl_ut_trace_summary("media: live instance clean (free=%u op=%u spare=%u)", ftl->alloc.free_count, ftl->alloc.op_count[0], ftl->alloc.spare_count[0]);

    return MUNIT_OK;
}

static void media_prepare(ftl_ctx_t *ftl)
{
    if (!s_media_ready)
    {
        ftl_ut_media_format();
        s_media_ready = true;
        s_media_ftl   = ftl;
    }
    else
    {
        munit_assert_ptr_equal(s_media_ftl, ftl);
    }
}

static void media_fill_wr(DWORD lba, DWORD count)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        s_wr_buf[i] = (BYTE)((lba * 31U + i * 7U) & 0xFFU);
    }
}

static MunitResult test_format_and_roundtrip(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t *ftl = ftl_ut_live();

    ftl_ut_trace_begin();
    media_prepare(ftl);

    const DWORD count = 8U;

    media_fill_wr(FTL_UT_LBA_BASE, count);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));

    munit_assert_true(ftl_ut_host_write(FTL_UT_LBA_BASE, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());
    munit_assert_true(ftl_ut_host_read(FTL_UT_LBA_BASE, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    ftl_ut_trace_summary("media: roundtrip lba=%lu count=%lu free=%hu", FTL_UT_LBA_BASE, count, ftl->alloc.free_count);

    return MUNIT_OK;
}

static MunitResult test_cross_region_roundtrip(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t *ftl = ftl_ut_live();

    ftl_ut_trace_begin();
    media_prepare(ftl);

    /* 跨一个 L2P region（512 LBA）边界。 */
    const DWORD lba   = FTL_UT_LBA_BASE + 504U;
    const DWORD count = 16U;

    media_fill_wr(lba, count);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));

    munit_assert_true(ftl_ut_host_write(lba, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());
    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    ftl_ut_trace_summary("media: cross-region ok lba=%lu count=%lu", lba, count);

    return MUNIT_OK;
}

static MunitResult test_flush_reboot_readback(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    const DWORD lba   = FTL_UT_LBA_BASE + 16U;
    const DWORD count = 8U;

    media_fill_wr(lba, count);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));

    munit_assert_true(ftl_ut_host_write(lba, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());

    ftl_ut_reboot_from_media();

    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    ftl_ut_trace_summary("media: flush+reboot readback ok lba=%lu", lba);

    return MUNIT_OK;
}

static MunitResult test_trim_no_resurrection(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    const DWORD lba   = FTL_UT_LBA_BASE + 32U;
    const DWORD count = 8U;

    media_fill_wr(lba, count);
    munit_assert_true(ftl_ut_host_write(lba, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());

    munit_assert_true(ftl_ut_trim(lba, count));
    /* TRIM 的失效走 Journal；持久化需要一次 FLUSH（host 的 SYNCHRONIZE CACHE 语义）。 */
    munit_assert_true(ftl_ut_flush());

    memset(s_zero_buf, 0, sizeof(s_zero_buf));
    memset(s_rd_buf, 0xFF, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_zero_buf, s_rd_buf);

    ftl_ut_reboot_from_media();

    memset(s_rd_buf, 0xFF, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_zero_buf, s_rd_buf);

    ftl_ut_trace_summary("media: trim+reboot no resurrection lba=%lu", lba);

    return MUNIT_OK;
}

/* L1/L2 真 NAND 用例按 design 文档 §4.5 逐步加入；子套件在 ut_main.c 注册。 */
MunitTest g_ftl_media_e2e_tests[] = {
    {            (char *)"media_live",             test_media_live, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"format_and_roundtrip",   test_format_and_roundtrip, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"cross_region_roundtrip", test_cross_region_roundtrip, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { (char *)"flush_reboot_readback",  test_flush_reboot_readback, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"trim_no_resurrection",   test_trim_no_resurrection, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                            NULL,                        NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
