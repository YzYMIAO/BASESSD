/*
 * test_ftl_media.c — /ftl/media/：真 NAND 路径（破坏性）
 *
 * ⚠ 本文件的用例会在板端执行 format / 写 NAND；运行前拔掉 USB，跑完 format + POR。
 *    当前为脚手架：先验证活实例与结束不变量，真 NAND 用例按阶段 P4/P5 落地。
 */

#include "bbt.h"
#include "checkpoint.h"
#include "ftl_fc_interface.h"
#include "ftl_job.h"
#include "ftl_ut_common.h"
#include "migrate.h"
#include "munit.h"
#include "power_failure_recovery.h"
#include "wear_leveling.h"

#include <stdio.h>
#include <string.h>

#define FTL_UT_MEDIA_LBAS  16U
#define FTL_UT_MATRIX_LBAS 64U

static BYTE        s_wr_buf[FTL_UT_MEDIA_LBAS * FTL_IU_BYTES];
static BYTE        s_rd_buf[FTL_UT_MEDIA_LBAS * FTL_IU_BYTES];
static BYTE        s_zero_buf[FTL_UT_MEDIA_LBAS * FTL_IU_BYTES];
static cp_header_t s_cp_hdr;
static BYTE        s_mx_wr[FTL_UT_MATRIX_LBAS * FTL_IU_BYTES];
static BYTE        s_mx_rd[FTL_UT_MATRIX_LBAS * FTL_IU_BYTES];

/* 参数矩阵：写长度 × 起始偏移（覆盖 odd tail / 跨 pair / 跨 region）。 */
static const char        *s_io_count_values[] = {"1", "7", "8", "9", "64", NULL};
static const char        *s_io_off_values[]   = {"0", "3", "7", NULL};
static MunitParameterEnum s_io_params[]       = {
    {(char *)"count", s_io_count_values},
    {  (char *)"off",   s_io_off_values},
    {           NULL,              NULL},
};

static DWORD media_atou(const char *s)
{
    DWORD v = 0U;

    while ((*s >= '0') && (*s <= '9'))
    {
        v = (v * 10U) + (DWORD)(*s - '0');
        s++;
    }
    return v;
}

static bool       s_media_ready;
static ftl_ctx_t *s_media_ftl;

static MunitResult test_media_live(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_t *ftl = ftl_ut_live();

    ftl_ut_trace_begin();
    ftl_ut_assert_clean(ftl);

    ftl_ut_trace_detail("media live: free=%hu op0=%hu spare0=%hu bad=%hu", ftl->alloc.free_count, ftl->alloc.op_count[0], ftl->alloc.spare_count[0], ftl->alloc.bad_count);
    ftl_ut_trace_summary("media: live clean free=%hu op0=%hu spare0=%hu bad=%hu", ftl->alloc.free_count, ftl->alloc.op_count[0], ftl->alloc.spare_count[0], ftl->alloc.bad_count);

    return MUNIT_OK;
}

static void *media_setup(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    (void)ftl_ut_live();
    return NULL;
}

static void media_teardown(void *fixture)
{
    (void)fixture;
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

    ftl_ut_trace_detail("roundtrip: lba=%lu count=%lu bytes=%lu; write->flush->read byte-equal; free=%hu op0=%hu", FTL_UT_LBA_BASE, count, count * FTL_IU_BYTES, ftl->alloc.free_count, ftl->alloc.op_count[0]);
    ftl_ut_trace_summary("media: roundtrip lba=%lu count=%lu (%lu B) byte-equal free=%hu", FTL_UT_LBA_BASE, count, count * FTL_IU_BYTES, ftl->alloc.free_count);

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

    ftl_ut_trace_detail("cross-region: lba=%lu..%lu count=%lu bytes=%lu spans region boundary; byte-equal free=%hu", lba, lba + count - 1U, count, count * FTL_IU_BYTES, ftl->alloc.free_count);
    ftl_ut_trace_summary("media: cross-region lba=%lu count=%lu byte-equal free=%hu", lba, count, ftl->alloc.free_count);

    return MUNIT_OK;
}

static MunitResult test_host_io_matrix(const MunitParameter params[], void *data)
{
    (void)data;

    const DWORD count = media_atou(munit_parameters_get(params, "count"));
    const DWORD off   = media_atou(munit_parameters_get(params, "off"));
    const DWORD lba   = FTL_UT_LBA_BASE + off;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        s_mx_wr[i] = (BYTE)((lba * 17U + i * 13U) & 0xFFU);
    }
    memset(s_mx_rd, 0, sizeof(s_mx_rd));

    munit_assert_true(ftl_ut_host_write(lba, count, s_mx_wr));
    munit_assert_true(ftl_ut_flush());
    munit_assert_true(ftl_ut_host_read(lba, count, s_mx_rd));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_mx_wr, s_mx_rd);

    ftl_ut_trace_detail("matrix: off=%lu lba=%lu count=%lu bytes=%lu; byte-equal", off, lba, count, count * FTL_IU_BYTES);
    ftl_ut_trace_summary("media matrix: off=%lu count=%lu lba=%lu byte-equal", off, count, lba);

    return MUNIT_OK;
}

static MunitResult test_host_io_random(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    for (BYTE round = 0U; round < 8U; round++)
    {
        const DWORD count = (DWORD)munit_rand_int_range(1, (int)FTL_UT_MATRIX_LBAS + 1);
        const DWORD off   = (DWORD)munit_rand_int_range(0, (int)(8192U - count));
        const DWORD lba   = FTL_UT_LBA_BASE + off;

        for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
        {
            s_mx_wr[i] = (BYTE)((round * 37U + i * 11U) & 0xFFU);
        }
        memset(s_mx_rd, 0, sizeof(s_mx_rd));

        munit_assert_true(ftl_ut_host_write(lba, count, s_mx_wr));
        munit_assert_true(ftl_ut_flush());
        munit_assert_true(ftl_ut_host_read(lba, count, s_mx_rd));
        munit_assert_memory_equal(count * FTL_IU_BYTES, s_mx_wr, s_mx_rd);

        ftl_ut_trace_detail("random io: round=%hhu lba=%lu count=%lu", round, lba, count);
    }

    ftl_ut_trace_detail("random io: 8 seeded rounds, each write->flush->read byte-equal");
    ftl_ut_trace_summary("media random io: 8 seeded rounds byte-equal");

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
    const WORD  free0 = ftl_ut_live()->alloc.free_count;

    media_fill_wr(lba, count);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));

    munit_assert_true(ftl_ut_host_write(lba, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());

    ftl_ut_reboot_from_media();

    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    ftl_ut_trace_detail("flush+reboot: lba=%lu count=%lu bytes=%lu; free %hu->%hu; byte-equal after simulated reboot", lba, count, count * FTL_IU_BYTES, free0, ftl_ut_live()->alloc.free_count);
    ftl_ut_trace_summary("media: flush+reboot lba=%lu count=%lu free %hu->%hu byte-equal", lba, count, free0, ftl_ut_live()->alloc.free_count);

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
    /* 起点序号必须在写入/trim 之前取：flush/trim 自身可能已触发一次 CP，
     * 等到请求前再取会把「已经推进」当成「没有推进」。 */
    const DWORD cp0   = ftl_ut_live()->cp_handle->cp_state.sequence;
    DWORD       cp1   = 0U;

    media_fill_wr(lba, count);
    munit_assert_true(ftl_ut_host_write(lba, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());

    munit_assert_true(ftl_ut_trim(lba, count));
    /* TRIM 的失效走 Journal；持久化需要一次 FLUSH（host 的 SYNCHRONIZE CACHE 语义）。 */
    munit_assert_true(ftl_ut_flush());

    /* 再落一次 Checkpoint：Journal-only 的 TRIM 持久化存在已知缺口（RISK-HOST-11），
     * 本用例锚定“CP 落盘后的 TRIM 不复活”这一明确持久化点。 */
    {
        ftl_ctx_t  *f  = ftl_ut_live();
        const DWORD t0 = ftl_job_now_ms();

        jrn_checkpoint_request(f->journal);

        /* 等 IDLE 且「至少有一次 CP 晚于用例起点 cp0」：flush/trim 可能已经自然落过一次 CP，
         * 此时再 request 可能无事可做（序号不前进），所以基准必须取用例起点而不是 request 前。 */
        while (((f->cp_handle->cp_state.sequence <= cp0) || (f->cp_handle->phase != CP_IDLE)) && ((ftl_job_now_ms() - t0) < FTL_UT_IO_TIMEOUT_MS))
        {
            ftl_service_pump();
            ftl_service(f);
        }
        munit_assert_uint32(f->cp_handle->cp_state.sequence, >, cp0);
        munit_assert_int(f->cp_handle->phase, ==, CP_IDLE);

        const DWORD target = f->cp_handle->cp_state.sequence;
        WORD        idx    = 0U;

        memset(&s_cp_hdr, 0, sizeof(s_cp_hdr));
        munit_assert_int(cp_pool_find_latest(f->cp_handle, &s_cp_hdr, &idx), ==, FTL_OK);
        cp1 = s_cp_hdr.sequence;
        munit_assert_uint32(cp1, ==, target);
    }

    memset(s_zero_buf, 0, sizeof(s_zero_buf));
    memset(s_rd_buf, 0xFF, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_zero_buf, s_rd_buf);

    ftl_ut_reboot_from_media();

    memset(s_rd_buf, 0xFF, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_zero_buf, s_rd_buf);

    ftl_ut_trace_detail("trim: lba=%lu count=%lu; cp %lu->%lu (on-media header %lu); read zeros before and after simulated reboot; post-reboot cp=%lu", lba, count, cp0, cp1, s_cp_hdr.sequence, ftl_ut_live()->cp_handle->cp_state.sequence);
    ftl_ut_trace_summary("media: trim+reboot lba=%lu count=%lu cp %lu->%lu no resurrection", lba, count, cp0, cp1);

    return MUNIT_OK;
}

static MunitResult test_preload_region_zero_and_high(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    const DWORD count  = 8U;
    const DWORD lba_hi = FTL_UT_LBA_BASE + 64U;

    media_fill_wr(0U, count);
    munit_assert_true(ftl_ut_host_write(0U, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());

    media_fill_wr(lba_hi, count);
    munit_assert_true(ftl_ut_host_write(lba_hi, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());

    ftl_ut_reboot_from_media();

    /* 预载 region 0 与高位 region：两处都不能别名到对方。 */
    media_fill_wr(0U, count);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(0U, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    media_fill_wr(lba_hi, count);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(lba_hi, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    ftl_ut_trace_detail("preload: lba0 count=%lu; lba_hi=%lu count=%lu; both byte-equal after reboot", count, lba_hi, count);
    ftl_ut_trace_summary("media: preload lba0/lba_hi=%lu count=%lu no alias", lba_hi, count);

    return MUNIT_OK;
}

static MunitResult test_journal_barrier_cp_progress(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_t  *ftl   = ftl_ut_live();
    const DWORD lba   = FTL_UT_LBA_BASE + 96U;
    const DWORD count = 8U;
    const DWORD cp0   = ftl->cp_handle->cp_state.sequence; /* 基准取写入前 */

    media_fill_wr(lba, count);
    munit_assert_true(ftl_ut_host_write(lba, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());

    munit_assert_false(jrn_has_pending_entries(ftl->journal));

    const DWORD t0 = ftl_job_now_ms();

    jrn_checkpoint_request(ftl->journal);

    /* 等「序号推进 + job 回到 IDLE」：序号在 Header prep 时就会 ++，只等序号会在 Header
     * 尚未落盘时提前退出。⚠ 下面 find_latest 会把 cp_state.sequence 覆盖为它找到的旧记录
     * 的序号，所以「先比较、后调用」，且比较基准取调用前的 cp_target（否则断言恒真）。 */
    while (((ftl->cp_handle->cp_state.sequence <= cp0) || (ftl->cp_handle->phase != CP_IDLE)) && ((ftl_job_now_ms() - t0) < FTL_UT_IO_TIMEOUT_MS))
    {
        ftl_service_pump();
        ftl_service(ftl);
    }

    munit_assert_uint32(ftl->cp_handle->cp_state.sequence, >, cp0);
    munit_assert_int(ftl->cp_handle->phase, ==, CP_IDLE);

    const DWORD cp_target = ftl->cp_handle->cp_state.sequence;
    WORD        idx       = 0U;

    memset(&s_cp_hdr, 0, sizeof(s_cp_hdr));
    munit_assert_int(cp_pool_find_latest(ftl->cp_handle, &s_cp_hdr, &idx), ==, FTL_OK);
    munit_assert_uint32(s_cp_hdr.sequence, ==, cp_target);

    ftl_ut_reboot_from_media();

    media_fill_wr(lba, count);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    ftl_ut_trace_detail("journal barrier: lba=%lu count=%lu; pending after flush=false; cp %lu->%lu; on-media header seq=%lu; post-reboot cp=%lu; read byte-equal", lba, count, cp0, cp_target, s_cp_hdr.sequence, ftl->cp_handle->cp_state.sequence);
    ftl_ut_trace_summary("media: journal barrier lba=%lu count=%lu cp %lu->%lu + reboot byte-equal", lba, count, cp0, cp_target);

    return MUNIT_OK;
}

static MunitResult test_gc_progress(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_t  *ftl   = ftl_ut_live();
    const DWORD lba   = FTL_UT_LBA_BASE + 128U;
    const DWORD count = 8U;

    media_fill_wr(lba, count);
    munit_assert_true(ftl_ut_host_write(lba, count, s_wr_buf));
    munit_assert_true(ftl_ut_flush());

    /* 关闭仍打开的 SB，保证 closed 列表里有带有效 CW 的 victim。 */
    if (ftl->alloc.hot_sb.is_open)
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }
    if (ftl->alloc.cold_sb.is_open)
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_COLD);
    }

    const DWORD moved0 = ftl->mig_handle->moved_count;
    const DWORD rec0   = ftl->mig_handle->reclaimed_count;
    const DWORD t0     = ftl_job_now_ms();

    mig_gc_request_urgent(ftl->mig_handle);

    while (((ftl->mig_handle->moved_count == moved0) && (ftl->mig_handle->reclaimed_count == rec0)) &&
           ((ftl_job_now_ms() - t0) < (FTL_UT_IO_TIMEOUT_MS * 2U)))
    {
        ftl_service_pump();
        ftl_service(ftl);
    }

    munit_assert_true((ftl->mig_handle->moved_count > moved0) || (ftl->mig_handle->reclaimed_count > rec0));

    media_fill_wr(lba, count);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_rd_buf));
    munit_assert_memory_equal(count * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    ftl_ut_trace_detail("gc: lba=%lu count=%lu; moved %lu->%lu reclaimed %lu->%lu; read byte-equal", lba, count, moved0, ftl->mig_handle->moved_count, rec0, ftl->mig_handle->reclaimed_count);
    ftl_ut_trace_summary("media: urgent GC moved %lu->%lu CW reclaimed %lu->%lu", moved0, ftl->mig_handle->moved_count, rec0, ftl->mig_handle->reclaimed_count);

    return MUNIT_OK;
}

static MunitResult test_wl_progress(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_t  *ftl       = ftl_ut_live();
    const DWORD base      = FTL_UT_LBA_BASE;
    const DWORD per_write = 8U;
    const DWORD pages     = (DWORD)PAGES_PER_BLOCK;

    /* 写满一个 SB（64 页 × 8 LBA = 512 LBA），保证 WL victim 的有效 CW ≥70%。 */
    for (DWORD page = 0U; page < pages; page++)
    {
        const DWORD lba = base + (page * per_write);

        media_fill_wr(lba, per_write);
        munit_assert_true(ftl_ut_host_write(lba, per_write, s_wr_buf));
    }
    munit_assert_true(ftl_ut_flush());

    alc_t *a = &ftl->alloc;

    if (a->hot_sb.is_open)
    {
        (void)alc_super_close(a, FTL_TEMP_HOT);
    }
    if (a->cold_sb.is_open)
    {
        (void)alc_super_close(a, FTL_TEMP_COLD);
    }

    /* 造 UBI 判据：used SB 低擦除、空闲块高擦除。 */
    for (WORD i = 0U; i < a->closed_sb_count; i++)
    {
        const super_block_t *sb = alc_super_closed_get_at(a, i);

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (super_block_has_plane(sb, p))
            {
                a->blocks[ftl_gblock(p, sb->plane_blocks[p])].erase_count = 100U;
            }
        }
    }
    for (WORD i = 0U; i < a->free_count; i++)
    {
        const WORD g = a->free_blocks[i];

        if (a->blocks[g].erase_count < 400U)
        {
            a->blocks[g].erase_count = 400U;
        }
    }

    ftl->total_writes = WL_MIN_WRITES;

    mig_handle_t *m = ftl->mig_handle;

    m->wl_eval_last_ms = 0U;
    m->wl_last_writes  = ftl->total_writes;

    bg_info_t info;

    memset(&info, 0, sizeof(info));
    info.now_ms     = ftl_job_now_ms() + 1U;
    info.load_level = BG_LEVEL_IDLE;

    const DWORD  moved0 = m->moved_count;
    const DWORD  t0     = ftl_job_now_ms();
    mig_status_t st;

    memset(&st, 0, sizeof(st));
    mig_handle(m, &info, &st);

    munit_assert_true(m->active);
    munit_assert_int(m->kind, ==, MIG_KIND_WL);

    while ((m->moved_count == moved0) && ((ftl_job_now_ms() - t0) < (FTL_UT_IO_TIMEOUT_MS * 3U)))
    {
        mig_handle(m, &info, &st);
        ftl_service_pump();
    }

    munit_assert_uint32(m->moved_count, >, moved0);

    /* WL 迁移后数据仍可读（按源坐标校验）。 */
    const DWORD check_lba = base;

    media_fill_wr(check_lba, per_write);
    memset(s_rd_buf, 0, sizeof(s_rd_buf));
    munit_assert_true(ftl_ut_host_read(check_lba, per_write, s_rd_buf));
    munit_assert_memory_equal(per_write * FTL_IU_BYTES, s_wr_buf, s_rd_buf);

    ftl_ut_trace_detail("wl: writes=%lu sb=%lu pages x %lu LBA; erase used=100 free=400; moved %lu->%lu; lba=%lu read byte-equal", (DWORD)WL_MIN_WRITES, pages, per_write, moved0, m->moved_count, check_lba);
    ftl_ut_trace_summary("media: WL moved %lu->%lu CW (delta %lu)", moved0, m->moved_count, m->moved_count - moved0);

    return MUNIT_OK;
}

static MunitResult test_cp_probe_clean_media(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_t *ftl     = ftl_ut_live();
    DWORD      version = 0U;

    const cp_media_status_e st = cp_media_status_probe(ftl->cp_handle, &version);

    munit_assert_int(st, ==, CP_MEDIA_CURRENT);
    munit_assert_uint32(version, ==, (DWORD)FTL_MEDIA_LAYOUT_VERSION);

    bool need_format = true;

    munit_assert_int(pfr_media_check_format(ftl, &need_format), ==, FTL_OK);
    munit_assert_false(need_format);

    ftl_ut_trace_detail("cp probe: status=%d version=%lu (layout=%lu); need_format true->%d", (int)st, version, (DWORD)FTL_MEDIA_LAYOUT_VERSION, (int)need_format);
    ftl_ut_trace_summary("media: cp probe status=%d version=%lu no format needed", (int)st, version);

    return MUNIT_OK;
}

/* BBT 池清空：多圈 ring 的 save→reload 会选到旧 record（RISK-BBT-18）。
 * 要验证“本次 save 的坏块/标志是否持久化”的用例必须先隔离这个变量。 */
static void media_erase_bbt_pool(alc_t *a)
{
    const md_ring_t *ring = &a->pools->bbt;
    const ftl_topo_t topo = alc_topo(a);

    for (WORD i = 0U; i < ring->block_count; i++)
    {
        const DWORD pba = pba_make(topo.ch, topo.ce, topo.lun, ring->blocks[i], 0U, 0U, 0U);

        munit_assert_int(ftl_sync_erase_block_multi_plane(pba), ==, FTL_OK);
    }
    memset(&a->bbt, 0, sizeof(a->bbt));
}

static MunitResult test_bbt_persist(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_t *ftl = ftl_ut_live();
    alc_t     *a   = &ftl->alloc;

    /* format 会刻意保留旧 BBT record；多圈 ring 会让 locate 选到旧 record
     * （RISK-BBT-18），与本用例要测的“save→load 持久化”无关 ⇒ 先清池。 */
    media_erase_bbt_pool(a);

    munit_assert_uint32(a->free_count, >, 0U);

    WORD g = FTL_GBLOCK_INVALID;

    for (WORD i = 0U; i < a->free_count; i++)
    {
        if (!ftl_block_is_bad(&a->blocks[a->free_blocks[i]]))
        {
            g = a->free_blocks[i];
            break;
        }
    }
    munit_assert_uint32(g, !=, (DWORD)FTL_GBLOCK_INVALID);

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);
    munit_assert_true(ftl_block_is_bad(&a->blocks[g]));

    ftl_ut_trace_detail("bbt before save g=%hu bad_count=%hu dirty=%d seq=%lu idx=%hu", g, a->bad_count, (int)a->bbt.dirty, a->bbt.sequence, a->bbt.write_idx);

    munit_assert_int(bbt_save(a), ==, FTL_OK);

    ftl_ut_trace_detail("bbt after save dirty=%d seq=%lu idx=%hu", (int)a->bbt.dirty, a->bbt.sequence, a->bbt.write_idx);

    munit_assert_uint32(a->bbt.dirty, ==, 0U);

    const DWORD saved_seq = a->bbt.sequence;

    /* ② 走生产启动路径重新加载 BBT（模拟重启后 bbt_init），坏块必须还在。 */
    ftl_ut_reboot_from_media();
    bbt_init(&ftl_ut_live()->alloc);

    munit_assert_true(ftl_block_is_bad(&ftl_ut_live()->alloc.blocks[g]));

    ftl_ut_trace_detail("bbt persist: marked g=%hu; saved seq=%lu; reloaded bad_count=%hu is_bad=%d", g, saved_seq, ftl_ut_live()->alloc.bad_count, (int)ftl_block_is_bad(&ftl_ut_live()->alloc.blocks[g]));
    ftl_ut_trace_summary("media: BBT persisted g=%hu seq=%lu bad_count=%hu", g, saved_seq, ftl_ut_live()->alloc.bad_count);

    return MUNIT_OK;
}

/* L1/L2 真 NAND 用例按 design 文档 §4.5 逐步加入；子套件在 ut_main.c 注册。 */
static MunitResult test_format_in_progress_resume(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_t       *ftl     = ftl_ut_live();
    alc_t           *a       = &ftl->alloc;
    const ftl_topo_t topo    = alc_topo(a);
    DWORD            version = 0U;

    /* ① 模拟两阶段格式化在「BBT 已落盘、initial CP 尚未写」时掉电：
     *    写一条带 FORMAT_IN_PROGRESS 的 BBT record，并用**更大的 sequence** 让它
     *    在真实开机 bootstrap 扫描中胜出（旧 record 可能留在已移出候选池的 spare 上，
     *    RISK-BBT-18 / BOOT-22）。不擦 BBT 载体：失败路径上擦 spare 会丢坏块图。
     *    ⚠ bbt_save 在 dirty==0 时直接返回 OK（不写盘），必须同时置 dirty。 */
    a->bbt.sequence           = 0xFFFFFF00U; /* 新 record 必须是最新（见上） */
    a->bbt.cursor_valid       = 0U;          /* 让 save 重新定位 write_idx */
    a->bbt.format_in_progress = true;
    a->bbt.dirty              = 1U;
    munit_assert_int(bbt_save(a), ==, FTL_OK);

    /* ② 擦掉 Checkpoint 池，制造“无有效 CP record”。 */
    for (WORD block = 0U; block < CHECKPOINT_POOL_BLOCKS; block++)
    {
        const WORD  pb  = (WORD)(CHECKPOINT_BLOCK0 + block);
        const DWORD pba = pba_make(topo.ch, topo.ce, topo.lun, pb, 0U, 0U, 0U);

        munit_assert_int(ftl_sync_erase_block_multi_plane(pba), ==, FTL_OK);
    }

    /* ③ 启动判据：带 FORMAT_IN_PROGRESS 必须“继续格式化”，不能误判成 IO_ERROR。 */
    munit_assert_int(cp_media_status_probe(ftl->cp_handle, &version), ==, CP_MEDIA_NO_RECORD);

    bool need_format = false;

    munit_assert_int(pfr_media_check_format(ftl, &need_format), ==, FTL_OK);
    munit_assert_true(need_format);

    /* ④ 模拟重启：bbt_init 必须从最新 record 恢复标志。 */
    ftl_ut_reboot_from_media();

    ftl = ftl_ut_live();
    munit_assert_true(alc_bbt_is_format_in_progress(&ftl->alloc));
    munit_assert_int(cp_media_status_probe(ftl->cp_handle, &version), ==, CP_MEDIA_NO_RECORD);

    /* ⑤ 恢复介质：完整格式化的提交点会写 initial CP 并清标志。 */
    ftl_ut_media_format();

    ftl = ftl_ut_live();
    munit_assert_false(alc_bbt_is_format_in_progress(&ftl->alloc));
    munit_assert_int(cp_media_status_probe(ftl->cp_handle, &version), ==, CP_MEDIA_CURRENT);

    ftl_ut_trace_detail("format-in-progress: flag set + CP pool erased -> probe=NO_RECORD need_format=1; reboot flag=1 probe=NO_RECORD; after format flag=0 probe=CURRENT version=%lu", version);
    ftl_ut_trace_summary("media: format-in-progress -> NO_RECORD (continue format) and flag cleared after commit");

    return MUNIT_OK;
}

static MunitResult test_cp_corrupt_refuse_format(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_t       *ftl     = ftl_ut_live();
    alc_t           *a       = &ftl->alloc;
    const ftl_topo_t topo    = alc_topo(a);
    DWORD            version = 0U;

    /* ① 擦掉 CP 池，确保没有可定位的 Checkpoint record。 */
    for (WORD block = 0U; block < CHECKPOINT_POOL_BLOCKS; block++)
    {
        const WORD  pb  = (WORD)(CHECKPOINT_BLOCK0 + block);
        const DWORD pba = pba_make(topo.ch, topo.ce, topo.lun, pb, 0U, 0U, 0U);

        munit_assert_int(ftl_sync_erase_block_multi_plane(pba), ==, FTL_OK);
    }

    /* ② 在 CP 环 slot0/page0 的 plane0 写一页非 FF 内容。
     *    探针（cp_media_status_probe）在没有有效 record 时会裸读 slot0/page0：
     *    只要读到非 FF，就判“有介质内容但没有有效 CP” ⇒ IO_ERROR，拒绝格式化。 */
    const md_ring_t *ring  = &a->pools->cp;
    const BYTE       plane = md_ring_plane_at(ring->plane_mask, 0U);
    const DWORD      addr  = md_ring_addr_plane(ring, &topo, 0U, 0U, plane);
    BYTE             page[FTL_IU_BYTES];

    memset(page, 0x00, sizeof(page));
    munit_assert_int(ftl_sync_write_page(addr, page, sizeof(page), FTL_PAGE_CFG_META()), ==, FTL_OK);

    /* ③ 判据：无有效 record + 非 FF 内容 ⇒ CP_MEDIA_IO_ERROR（不是 NO_RECORD）。 */
    munit_assert_int(cp_media_status_probe(ftl->cp_handle, &version), ==, CP_MEDIA_IO_ERROR);

    /* ④ 启动决策：读坏必须拒绝格式化，不能把整片擦掉（RISK-BOOT-03）。 */
    bool need_format = true;

    munit_assert_int(pfr_media_check_format(ftl, &need_format), ==, FTL_ERROR);
    munit_assert_false(need_format);

    /* ⑤ 恢复介质：完整格式化重建 CP 池。 */
    ftl_ut_media_format();

    ftl = ftl_ut_live();
    munit_assert_int(cp_media_status_probe(ftl->cp_handle, &version), ==, CP_MEDIA_CURRENT);

    ftl_ut_trace_detail("cp corrupt: CP pool erased + slot0/page0 non-FF -> probe=IO_ERROR need_format=0; after format probe=CURRENT version=%lu", version);
    ftl_ut_trace_summary("media: corrupt CP -> IO_ERROR and format refused (no whole-chip erase)");

    return MUNIT_OK;
}

MunitTest g_ftl_media_e2e_tests[] = {
    {                  (char *)"media_live",                   test_media_live, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {        (char *)"format_and_roundtrip",         test_format_and_roundtrip, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {      (char *)"cross_region_roundtrip",       test_cross_region_roundtrip, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {       (char *)"flush_reboot_readback",        test_flush_reboot_readback, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {        (char *)"trim_no_resurrection",         test_trim_no_resurrection, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {(char *)"preload_region_zero_and_high", test_preload_region_zero_and_high, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    { (char *)"journal_barrier_cp_progress",  test_journal_barrier_cp_progress, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {                 (char *)"gc_progress",                  test_gc_progress, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {                 (char *)"wl_progress",                  test_wl_progress, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {        (char *)"cp_probe_clean_media",         test_cp_probe_clean_media, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {              (char *)"host_io_matrix",               test_host_io_matrix, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, s_io_params},
    {              (char *)"host_io_random",               test_host_io_random, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {                 (char *)"bbt_persist",                  test_bbt_persist, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {    (char *)"cp_corrupt_refuse_format",     test_cp_corrupt_refuse_format, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {   (char *)"format_in_progress_resume",    test_format_in_progress_resume, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,        NULL},
    {                                  NULL,                              NULL,        NULL,           NULL,             MUNIT_TEST_OPTION_NONE,        NULL},
};
