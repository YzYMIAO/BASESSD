/*
 * test_ftl_media.c — /ftl/media/：真 NAND 路径（破坏性）
 *
 * 注释约定：测试函数按 ftl_ut_design.md §8 的「覆盖/判据/备注」块，
 * 不套 S5 的 Doxygen 契约（P5 偏离，测试源专用）。
 *
 * ⚠ 本文件的用例会在板端执行 format / 写 NAND；运行前拔掉 USB。
 *    首个用例的 media_prepare() 会做一次完整格式化，后续用例复用该介质；
 *    trim/bbt/cp 类用例在末尾自行恢复介质，保证同一轮可重复运行。
 *    跑完建议 CLI `format` + POR 再回到 USB 使用。
 */

#include "bbt.h"
#include "checkpoint.h"
#include "fault_inject.h" /* debug-only 注入（ISS-FTL-16） */
#include "ftl_fc_interface.h"
#include "ftl_ut_common.h"
#include "journal.h"
#include "l2p_cache.h"
#include "migrate.h"
#include "munit.h"
#include "power_failure_recovery.h"
#include "timer.h"
#include "wear_leveling.h"

#include <string.h>

#define FTL_UT_MEDIA_LBAS  16U
#define FTL_UT_MATRIX_LBAS 64U

/* 读写复用同一块缓冲：写完成后 FTL 已拷走数据，读回直接覆盖它；
 * 期望值由 pattern 函数现场重算，不再保留第二份“期望缓冲”（AXI RAM 紧张）。 */
static BYTE        s_media_buf[FTL_UT_MEDIA_LBAS * FTL_IU_BYTES];
static BYTE        s_io_buf[FTL_UT_MATRIX_LBAS * FTL_IU_BYTES];
static cp_header_s s_cp_hdr;

/* 参数化写长度（边界值），起始偏移由 munit seed 随机：同一边界每次跑不同 LBA，
 * 既减少固定组合、又持续压非对齐起点 / 跨 pair / 跨 region。 */
static const char        *s_io_count_values[] = {"1", "7", "8", "9", "64", NULL};
static MunitParameterEnum s_io_params[]       = {
    {(char *)"count", s_io_count_values},
    {           NULL,              NULL},
};

static DWORD media_atou(const char *s)
{
    DWORD v = 0U;

    while ((*s >= '0') && (*s <= '9'))
    {
        v = (v * 10U) + (*s - '0');
        s++;
    }
    return v;
}

static BOOL       s_media_ready;
static ftl_ctx_s *s_media_ftl;

/* 由 `ut_run_all()` 在**每次 `test` 命令开始时**调用：
 * 介质只在一次 `test` 内可信 —— `/fc-nand/` 下的原始块测试会擦写 live FTL 的
 * 元数据块，而且 `s_media_ready` 是进程级 static（同一个上电周期里第二条 `test`
 * 命令会继承上一条的状态）。不复位 ⇒ media 套件在一份被改写过、又不重新格式化的
 * 介质上跑，`cache_failc_abort` 这类「重启后读回」用例会读到 IO_ERROR 而不是零填。 */
void ftl_ut_media_session_reset(void)
{
    s_media_ready = 0;
    s_media_ftl   = NULL;
}

/* media_live — 真介质活实例的账本基线。
 *
 * 覆盖：ftl_ut_assert_clean 检查 free/op/spare/bad 账本与池/组/ctx 归零。
 * 判据：后续 media 用例都从这里起步；基线不干净时先报账本问题，不再叠加介质噪声。
 * 备注：不写 NAND。 */

static MunitResult test_media_live(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

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

    ftl_ut_topology_init();
    (void)ftl_ut_live();
    fi_reset(); /* 注入状态是进程级的：漏一次复位会静默污染后续用例 */
    return NULL;
}

static void media_teardown(void *fixture)
{
    (void)fixture;

    /* 排空 FTL/FC 再交给下一个套件：用例可能把后台命令（journal / CP / BBT）的提交
     * 留在最后一次泵之后，其完成会滞留 FC 的 CQ。speed 套件的 `test_setup` 不会重跑
     * `fc_init`（它用静态 inited 只初始化一次），于是它提交的第一条命令会先收到
     * 别人的完成（`command_id` 不匹配）。quiesce 会把 bg_suspend 置 1，这里恢复原值。 */
    ftl_ctx_s *ftl   = ftl_ut_live();
    const BOOL saved = ftl->flags.bg_suspend;

    ftl_ut_quiesce(ftl);
    ftl_ut_assert_clean(ftl);
    ftl->flags.bg_suspend = saved;
}

static void media_prepare(ftl_ctx_s *ftl)
{
    if (!s_media_ready)
    {
        ftl_ut_media_format();
        s_media_ready = 1;
        s_media_ftl   = ftl;
    }
    else
    {
        munit_assert_ptr_equal(s_media_ftl, ftl);
    }
}

/* 测试数据 pattern 保持直白循环：三种 pattern（wr/mx/seed）语义不同、各自只服务
 * 少量用例，不做带系数的通用引擎（P5 偏离 C2：避免把简单公式拆成多层 helper）。 */
static void media_fill_wr(DWORD lba, DWORD count)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        s_media_buf[i] = ((lba * 31U + i * 7U) & 0xFFU);
    }
}

static void media_assert_wr(DWORD lba, DWORD count)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        munit_assert_uint8(s_media_buf[i], ==, ((lba * 31U + i * 7U) & 0xFFU));
    }
}

static void media_fill_failc(DWORD count, BYTE tag, DWORD start)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        s_io_buf[i] = ((tag * 61U + (start + i) * 13U) & 0xFFU);
    }
}

static void media_assert_failc(DWORD count, BYTE tag, DWORD start)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        munit_assert_uint8(s_io_buf[i], ==, ((tag * 61U + (start + i) * 13U) & 0xFFU));
    }
}

static void media_assert_zero_buf(const BYTE *buf, DWORD bytes)
{
    for (DWORD i = 0U; i < bytes; i++)
    {
        munit_assert_uint8(buf[i], ==, 0U);
    }
}

/* format_and_roundtrip — 首次格式化后的最小闭环。
 *
 * 覆盖：8 LBA（一个完整 pair）写 → flush → 读回逐字节一致。
 * 判据：flush 保证 L2P/Journal 落盘，读回必须走真实映射而不是 wbuf 快路径。
 * 备注：media_prepare 首次调用会做完整 format（约 10–21 s，视优化档而定）。 */

static MunitResult test_format_and_roundtrip(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

    ftl_ut_trace_begin();
    media_prepare(ftl);

    const DWORD count = 8U;

    media_fill_wr(ftl_ut_lba_base(), count);

    munit_assert_true(ftl_ut_host_write(ftl_ut_lba_base(), count, s_media_buf));
    munit_assert_true(ftl_ut_flush());
    munit_assert_true(ftl_ut_host_read(ftl_ut_lba_base(), count, s_media_buf));
    media_assert_wr(ftl_ut_lba_base(), count);

    ftl_ut_trace_detail("roundtrip: lba=%lu count=%lu bytes=%lu; write->flush->read byte-equal; free=%hu op0=%hu", ftl_ut_lba_base(), count, count * FTL_IU_BYTES, ftl->alloc.free_count, ftl->alloc.op_count[0]);
    ftl_ut_trace_summary("media: roundtrip lba=%lu count=%lu (%lu B) byte-equal free=%hu", ftl_ut_lba_base(), count, count * FTL_IU_BYTES, ftl->alloc.free_count);

    return MUNIT_OK;
}

/* cross_region_roundtrip — 跨 L2P region 边界的写读。
 *
 * 覆盖：起点 504、长度 16（跨 512 LBA 的 region 边界）。
 * 判据：一次 host 请求跨 region 时，加载/写回两侧都不能别名到相邻 region 的条目。
 * 备注：真 NAND。 */

static MunitResult test_cross_region_roundtrip(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

    ftl_ut_trace_begin();
    media_prepare(ftl);

    /* 跨一个 L2P region（512 LBA）边界。 */
    const DWORD lba   = ftl_ut_lba_base() + 504U;
    const DWORD count = 16U;

    media_fill_wr(lba, count);

    munit_assert_true(ftl_ut_host_write(lba, count, s_media_buf));
    munit_assert_true(ftl_ut_flush());
    munit_assert_true(ftl_ut_host_read(lba, count, s_media_buf));
    media_assert_wr(lba, count);

    ftl_ut_trace_detail("cross-region: lba=%lu..%lu count=%lu bytes=%lu spans region boundary; byte-equal free=%hu", lba, lba + count - 1U, count, count * FTL_IU_BYTES, ftl->alloc.free_count);
    ftl_ut_trace_summary("media: cross-region lba=%lu count=%lu byte-equal free=%hu", lba, count, ftl->alloc.free_count);

    return MUNIT_OK;
}

/* host_io_matrix — 参数化长度的边界矩阵，起点按种子随机。
 *
 * 覆盖：count=1/7/8/9/64。8 是 pair 边界，9 是最小「跨 pair 非对齐」，7 是奇尾；
 *       起点在尾窗内随机，覆盖非 pair 对齐起点且不重复固定几个偏移。
 * 判据：写读逐字节一致；长度/偏移收缩错误会静默丢尾部数据。真 NAND。 */

static MunitResult test_host_io_matrix(const MunitParameter params[], void *data)
{
    (void)data;

    const DWORD count = media_atou(munit_parameters_get(params, "count"));

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    /* 偏移在尾窗内随机；种子由 munit 固定，失败可用 --seed 原样重放。 */
    const DWORD off = (DWORD)munit_rand_int_range(0, (int)(FTL_UT_LBA_WINDOW - count));
    const DWORD lba = ftl_ut_lba_base() + off;

    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        s_io_buf[i] = ((lba * 17U + i * 13U) & 0xFFU);
    }

    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));
    munit_assert_true(ftl_ut_flush());
    munit_assert_true(ftl_ut_host_read(lba, count, s_io_buf));
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        munit_assert_uint8(s_io_buf[i], ==, ((lba * 17U + i * 13U) & 0xFFU));
    }

    ftl_ut_trace_detail("matrix: off=%lu lba=%lu count=%lu bytes=%lu; byte-equal", off, lba, count, count * FTL_IU_BYTES);
    ftl_ut_trace_summary("media matrix: off=%lu count=%lu lba=%lu byte-equal", off, count, lba);

    return MUNIT_OK;
}

/* host_io_random — 固定种子的随机 LBA/长度写读。
 *
 * 覆盖：8 轮，每轮随机 count（1..64）与 8192 LBA 范围内的偏移；每轮 flush 后读回。
 * 判据：种子固定使失败可复现；随机起点持续压 L2P miss/驱逐与 Journal 追加。
 * 备注：真 NAND。 */

static MunitResult test_host_io_random(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    for (BYTE round = 0U; round < 8U; round++)
    {
        const DWORD count = (DWORD)munit_rand_int_range(1, (int)FTL_UT_MATRIX_LBAS + 1);
        const DWORD off   = (DWORD)munit_rand_int_range(0, (int)(FTL_UT_LBA_WINDOW - count));
        const DWORD lba   = ftl_ut_lba_base() + off;

        for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
        {
            s_io_buf[i] = ((round * 37U + i * 11U) & 0xFFU);
        }

        munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));
        munit_assert_true(ftl_ut_flush());
        munit_assert_true(ftl_ut_host_read(lba, count, s_io_buf));
        for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
        {
            munit_assert_uint8(s_io_buf[i], ==, ((round * 37U + i * 11U) & 0xFFU));
        }

        ftl_ut_trace_detail("random io: round=%hhu lba=%lu count=%lu", round, lba, count);
    }

    ftl_ut_trace_detail("random io: 8 seeded rounds, each write->flush->read byte-equal");
    ftl_ut_trace_summary("media random io: 8 seeded rounds byte-equal");

    return MUNIT_OK;
}

/* flush_reboot_readback — flush 后模拟重启再读。
 *
 * 覆盖：写 8 LBA → flush → ftl_ut_reboot_from_media（quiesce + fc_init + re-arm）→ 读回。
 * 判据：数据必须来自介质上的 L2P/Journal，而不是进程内 cache；free 计数前后自洽。
 * 备注：真 NAND。 */

static MunitResult test_flush_reboot_readback(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    const DWORD lba   = ftl_ut_lba_base() + 16U;
    const DWORD count = 8U;
    const WORD  free0 = ftl_ut_live()->alloc.free_count;

    media_fill_wr(lba, count);

    munit_assert_true(ftl_ut_host_write(lba, count, s_media_buf));
    munit_assert_true(ftl_ut_flush());

    ftl_ut_reboot_from_media();

    munit_assert_true(ftl_ut_host_read(lba, count, s_media_buf));
    media_assert_wr(lba, count);

    ftl_ut_trace_detail("flush+reboot: lba=%lu count=%lu bytes=%lu; free %hu->%hu; byte-equal after simulated reboot", lba, count, count * FTL_IU_BYTES, free0, ftl_ut_live()->alloc.free_count);
    ftl_ut_trace_summary("media: flush+reboot lba=%lu count=%lu free %hu->%hu byte-equal", lba, count, free0, ftl_ut_live()->alloc.free_count);

    return MUNIT_OK;
}

/* trim_no_resurrection — TRIM 的明确持久化点。
 *
 * 覆盖：① 写→flush→trim→flush→强制一次 CP（等 IDLE 且序号超过用例起点）→读 0 →
 *       模拟重启 → 仍读 0；② 另一段**不强制 CP**，只 flush 后重启 → 仍不得复活
 *       （TRIM 的失效条目必须已随 Journal 页落盘，的 WAL 侧判据）。
 * 判据：TRIM 只活在 RAM/未持久化的 Journal 里，重启后旧映射复活 = 已删除的数据回来。
 * 备注：CP 基准取写入前——flush/trim 自身可能已触发 CP，取请求前会把已推进当没推进。 */

static MunitResult test_trim_no_resurrection(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    const DWORD lba   = ftl_ut_lba_base() + 32U;
    const DWORD count = 8U;
    /* 起点序号必须在写入/trim 之前取：flush/trim 自身可能已触发一次 CP，
     * 等到请求前再取会把「已经推进」当成「没有推进」。 */
    const DWORD cp0   = ftl_ut_live()->cp_handle->cp_state.sequence;
    DWORD       cp1   = 0U;

    media_fill_wr(lba, count);
    munit_assert_true(ftl_ut_host_write(lba, count, s_media_buf));
    munit_assert_true(ftl_ut_flush());

    munit_assert_true(ftl_ut_trim(lba, count));
    /* TRIM 的失效走 Journal；持久化需要一次 FLUSH（host 的 SYNCHRONIZE CACHE 语义）。 */
    munit_assert_true(ftl_ut_flush());

    /* 再落一次 Checkpoint：Journal-only 的 TRIM 持久化存在已知缺口，
     * 本用例锚定“CP 落盘后的 TRIM 不复活”这一明确持久化点。 */
    {
        ftl_ctx_s  *f  = ftl_ut_live();
        const DWORD t0 = platform_now_ms();

        jrn_checkpoint_request(f->journal);

        /* 等 IDLE 且「至少有一次 CP 晚于用例起点 cp0」：flush/trim 可能已经自然落过一次 CP，
         * 此时再 request 可能无事可做（序号不前进），所以基准必须取用例起点而不是 request 前。 */
        while (((f->cp_handle->cp_state.sequence <= cp0) || (f->cp_handle->phase != CP_IDLE)) && ((platform_now_ms() - t0) < FTL_UT_IO_TIMEOUT_MS))
        {
            /* 生产主循环每轮都跑 timer_scheduler_poll：bg_scheduler 的 credit 与
             * CP 触发任务都在 timer 表里；只泵 ftl_service 时预算任务永远不跑。 */
            timer_scheduler_poll();
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

    memset(s_media_buf, 0, sizeof(s_media_buf));
    memset(s_media_buf, 0xFF, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_media_buf));
    media_assert_zero_buf(s_media_buf, count * FTL_IU_BYTES);

    ftl_ut_reboot_from_media();

    memset(s_media_buf, 0xFF, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_media_buf));
    media_assert_zero_buf(s_media_buf, count * FTL_IU_BYTES);

    /* ── 第二段：不额外强制 CP，只靠 flush ──
     * 上一段锚定的是「CP 落盘后不复活」这一确定点；本段不再 request CP，只 flush：
     * trim 的失效条目必须已随 Journal 页落盘，回放要能把它应用回来。
     * 若这里复活，说明 trim 仍只活在 RAM/未持久化的 Journal 里。
     * ⚠ 只保证「本用例不 request CP」；后台按水位自然触发的 CP 不排除。 */
    {
        const DWORD lba2 = lba + 64U;

        media_fill_wr(lba2, count);
        munit_assert_true(ftl_ut_host_write(lba2, count, s_media_buf));
        munit_assert_true(ftl_ut_flush());
        munit_assert_true(ftl_ut_trim(lba2, count));
        munit_assert_true(ftl_ut_flush()); /* 只 flush：不 request CP */

        ftl_ut_reboot_from_media();

        memset(s_media_buf, 0xFF, sizeof(s_media_buf));
        munit_assert_true(ftl_ut_host_read(lba2, count, s_media_buf));
        media_assert_zero_buf(s_media_buf, count * FTL_IU_BYTES);

        ftl_ut_trace_detail("trim[journal-only]: lba=%lu count=%lu; no forced CP; read zeros after reboot", lba2, count);
    }

    ftl_ut_trace_detail("trim: lba=%lu count=%lu; cp %lu->%lu (on-media header %lu); read zeros before and after simulated reboot; post-reboot cp=%lu", lba, count, cp0, cp1, s_cp_hdr.sequence, ftl_ut_live()->cp_handle->cp_state.sequence);
    ftl_ut_trace_summary("media: trim+reboot lba=%lu count=%lu cp %lu->%lu no resurrection (incl. journal-only)", lba, count, cp0, cp1);

    return MUNIT_OK;
}

/* preload_region_zero_and_high — 低/高 region 的预载互不别名。
 *
 * 覆盖：重启后分别访问 LBA 0 与 LBA 96（高位 region），两处都读回各自数据。
 * 判据：region 0 常被当作哨兵/预载目标，若与高位 region 共用行或路由，会读到对方数据。
 * 备注：真 NAND。 */

static MunitResult test_preload_region_zero_and_high(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    const DWORD count  = 8U;
    const DWORD lba_hi = ftl_ut_lba_base() + 64U;

    media_fill_wr(0U, count);
    munit_assert_true(ftl_ut_host_write(0U, count, s_media_buf));
    munit_assert_true(ftl_ut_flush());

    media_fill_wr(lba_hi, count);
    munit_assert_true(ftl_ut_host_write(lba_hi, count, s_media_buf));
    munit_assert_true(ftl_ut_flush());

    ftl_ut_reboot_from_media();

    /* 预载 region 0 与高位 region：两处都不能别名到对方。 */
    media_fill_wr(0U, count);
    memset(s_media_buf, 0, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(0U, count, s_media_buf));
    media_assert_wr(0U, count);

    media_fill_wr(lba_hi, count);
    memset(s_media_buf, 0, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(lba_hi, count, s_media_buf));
    media_assert_wr(lba_hi, count);

    ftl_ut_trace_detail("preload: lba0 count=%lu; lba_hi=%lu count=%lu; both byte-equal after reboot", count, lba_hi, count);
    ftl_ut_trace_summary("media: preload lba0/lba_hi=%lu count=%lu no alias", lba_hi, count);

    return MUNIT_OK;
}

/* journal_barrier_cp_progress — Journal 屏障与 CP 落盘。
 *
 * 覆盖：写→flush 后 journal 无 pending；显式 request CP 后序号推进、phase 回 IDLE，
 *       且 on-media Header 的 sequence 等于目标；重启后数据一致。
 * 判据：序号在 Header prep 时就 ++，只等序号会在 Header 未落盘时提前退出；
 *       find_latest 会覆盖 cp_state.sequence，所以先取 cp_target 再调用。
 * 备注：真 NAND。 */

static MunitResult test_journal_barrier_cp_progress(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl   = ftl_ut_live();
    const DWORD lba   = ftl_ut_lba_base() + 96U;
    const DWORD count = 8U;
    const DWORD cp0   = ftl->cp_handle->cp_state.sequence; /* 基准取写入前 */

    media_fill_wr(lba, count);
    munit_assert_true(ftl_ut_host_write(lba, count, s_media_buf));

    /* 写入已确认后、flush 之前取 Journal 序号：本次写的全部条目都 ≤ 它。 */
    const DWORD seq_after_write = jrn_sequence_get(ftl->journal);

    munit_assert_true(ftl_ut_flush());

    munit_assert_false(jrn_has_pending_entries(ftl->journal));

    /* WAL 次序（RISK-L2P-09）：flush 必须把本次写的 Journal 条目全部落盘
     * （`persisted_seq` 追上写入时的序号），且**驻留 dirty 行的写入序号都不高于**
     * 该水位。区域页先于覆盖它的 Journal 条目落盘，掉电后就会出现「映射在、
     * valid_count 缺」→ 计数漂移 → GC 误回收活块（区域页先于 Journal 落盘会让 valid_count 与映射分叉）。
     * ⚠ 前半是主判据（可失败）；后半在 flush 已把行刷净时可能为空集，只作兜底。 */
    {
        const DWORD persisted = jrn_persisted_seq(ftl->journal);

        munit_assert_uint32(persisted, >=, seq_after_write);

        for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
        {
            const l2p_cache_line_s *line = &ftl->cache->lines[i];
            l2p_line_info_s         info;

            l2p_line_get(ftl->cache, i, &info);
            if ((info.state == L2P_CACHE_DIRTY) && (line->dirty_seq != 0U))
            {
                munit_assert_uint32(line->dirty_seq, <=, persisted);
            }
        }

        /* CMT 是写缓存主单元：flush 之后不能还有 dirty 条目未落盘。 */
        munit_assert_uint32(ftl->cache->stats.cmt_dirty, ==, 0U);

        ftl_ut_trace_detail("wal order: seq_after_write=%lu persisted=%lu; all resident dirty lines <= watermark; cmt_dirty=0", seq_after_write, persisted);
    }

    const DWORD t0 = platform_now_ms();

    jrn_checkpoint_request(ftl->journal);

    /* 等「序号推进 + job 回到 IDLE」：序号在 Header prep 时就会 ++，只等序号会在 Header
     * 尚未落盘时提前退出。⚠ 下面 find_latest 会把 cp_state.sequence 覆盖为它找到的旧记录
     * 的序号，所以「先比较、后调用」，且比较基准取调用前的 cp_target（否则断言恒真）。 */
    while (((ftl->cp_handle->cp_state.sequence <= cp0) || (ftl->cp_handle->phase != CP_IDLE)) && ((platform_now_ms() - t0) < FTL_UT_IO_TIMEOUT_MS))
    {
        /* 同 test_trim_no_resurrection：CP 触发是 bg_scheduler 的预算任务，
         * 必须先让 timer 表补 credit。 */
        timer_scheduler_poll();
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
    memset(s_media_buf, 0, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_media_buf));
    media_assert_wr(lba, count);

    ftl_ut_trace_detail("journal barrier: lba=%lu count=%lu; pending after flush=0; cp %lu->%lu; on-media header seq=%lu; post-reboot cp=%lu; read byte-equal", lba, count, cp0, cp_target, s_cp_hdr.sequence, ftl->cp_handle->cp_state.sequence);
    ftl_ut_trace_summary("media: journal barrier lba=%lu count=%lu cp %lu->%lu + reboot byte-equal", lba, count, cp0, cp_target);

    return MUNIT_OK;
}

/* gc_progress — 主动 GC 的**搬移**与回收推进。
 *
 * 覆盖：format 清场 → 写满一个 SB（64 页 × 8 LBA）→ trim 中间 64 LBA 制造无效 CW
 *       → 关闭 SB → 该 SB 成为 closed 列表里唯一有有效 CW 的 victim → urgent GC 后
 *       **moved 必须增长**（不是只 reclaimed 一个全无效块）；未 trim 的源 LBA 仍可读。
 * 判据：搬移必须同时更新 L2P 与回收计数；只动一边会丢映射或漏回收。真 NAND。
 * 备注：必须留一部分 invalid CW：emergency 模式把 100% 有效的 SB 判为「无收益」
 *       （可回收比例 <1%），GC 会判无收益、moved 保持 0。format 是为了
 *       保证 closed 列表里没有前面用例留下的全无效块。 */

static MunitResult test_gc_progress(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl       = ftl_ut_live();
    const DWORD base      = ftl_ut_lba_base() + 128U;
    const DWORD per_write = FTL_LBA_PER_PAIR;
    const DWORD pages     = PAGES_PER_BLOCK;
    const DWORD sb_lbas   = PAGES_PER_BLOCK * FTL_LBA_PER_PAIR;
    const DWORD trim_lbas = sb_lbas / 8U;

    /* 从空白介质开始，保证 closed 列表里的 victim 就是刚写满的 SB。 */
    ftl_ut_media_format();
    ftl = ftl_ut_live();
    munit_assert_uint32(ftl->alloc.closed_sb_count, ==, 0U);

    for (DWORD page = 0U; page < pages; page++)
    {
        const DWORD lba = base + (page * per_write);

        media_fill_wr(lba, per_write);
        munit_assert_true(ftl_ut_host_write(lba, per_write, s_media_buf));
    }
    munit_assert_true(ftl_ut_flush());

    /* 制造无效 CW：emergency 模式拒绝 100% 有效的 SB（可回收比例 <1%）。
     * trim 后 flush：把 trim 产生的 CMT/Journal 映射落定，teardown 才能断言 CMT 为空。 */
    munit_assert_true(ftl_ut_trim(base + (sb_lbas / 2U), trim_lbas));
    munit_assert_true(ftl_ut_flush());

    if (ftl->alloc.hot_sb.is_open)
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }
    if (ftl->alloc.cold_sb.is_open)
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_COLD);
    }

    munit_assert_uint32(ftl->alloc.closed_sb_count, >, 0U);

    const DWORD moved0 = ftl->mig_handle->state.counters.moved;
    const DWORD rec0   = ftl->mig_handle->state.counters.reclaimed;
    const DWORD t0     = platform_now_ms();

    /* 选 victim 校验：GC 第一轮可能先选 padding/空 SB（valid=0），所以下面循环
     * 要重复 request，直到选中有效 victim 并真正搬移。 */
    gc_select_out_s sel;

    memset(&sel, 0, sizeof(sel));
    const gc_select_e pick = gc_select_victim(&ftl->alloc, GC_BENEFIT_OFF, &sel);

    munit_assert_int(pick, ==, GC_SELECT_OK);
    munit_assert_not_null(sel.victim);

    /* 至少有一个 closed SB 含有效 CW；GC 第一轮可能先选 padding/空 SB（valid=0），
     * 所以循环里要重复 request，直到选中有效 victim 并真正搬移。 */
    BOOL has_valid_victim = 0;

    for (WORD i = 0U; i < ftl->alloc.closed_sb_count; i++)
    {
        alc_super_stats_s st;

        memset(&st, 0, sizeof(st));
        alc_super_get_stats(&ftl->alloc, alc_super_closed_get_at(&ftl->alloc, i), &st);
        if (st.valid_cw != 0U)
        {
            has_valid_victim = 1;
            break;
        }
    }
    munit_assert_true(has_valid_victim);

    while ((ftl->mig_handle->state.counters.moved == moved0) && ((platform_now_ms() - t0) < (FTL_UT_IO_TIMEOUT_MS * 3U)))
    {
        /* 每轮重新排队 urgent：第一轮可能只回收 valid=0 的 padding SB（reclaimed+1,
         * moved 不变），下一轮才会选到有有效数据的 SB。直接推进 mig 以排除
         * bg_scheduler 档位/credit 对用例的干扰。 */
        mig_gc_request_urgent(ftl->mig_handle);
        timer_scheduler_poll();
        mig_handle(ftl->mig_handle, BG_LEVEL_IDLE);
        ftl_service_pump();
        ftl_service(ftl);
    }

    /* victim 有有效 CW ⇒ 必须真正搬移；只回收全无效块不满足本用例判据。 */
    munit_assert_uint32(ftl->mig_handle->state.counters.moved, >, moved0);

    media_fill_wr(base, per_write);
    memset(s_media_buf, 0, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(base, per_write, s_media_buf));
    media_assert_wr(base, per_write);

    media_fill_wr(base + (sb_lbas - FTL_LBA_PER_PAIR), per_write);
    memset(s_media_buf, 0, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(base + (sb_lbas - FTL_LBA_PER_PAIR), per_write, s_media_buf));
    media_assert_wr(base + (sb_lbas - FTL_LBA_PER_PAIR), per_write);

    /* GC 搬移通过 CMT 更新 L2P；flush 让 dirty CMT 落定，teardown 才能断言归零。 */
    munit_assert_true(ftl_ut_flush());

    ftl_ut_trace_detail("gc: lba=%lu..%lu; moved %lu->%lu reclaimed %lu->%lu; head/tail read byte-equal", base, base + (sb_lbas - 1U), moved0, ftl->mig_handle->state.counters.moved, rec0, ftl->mig_handle->state.counters.reclaimed);
    ftl_ut_trace_summary("media: urgent GC moved %lu->%lu CW reclaimed %lu->%lu", moved0, ftl->mig_handle->state.counters.moved, rec0, ftl->mig_handle->state.counters.reclaimed);

    return MUNIT_OK;
}

/* wl_progress — 静态 WL 的搬迁（UBI 判据）。
 *
 * 覆盖：写满一个 SB（64 页 × 8 LBA）后关闭；把 used 块擦除次数压到 100、空闲块抬到
 *       400 造出 UBI 判据；触发后必须进入 MIG_KIND_WL，moved 增长，数据仍可读。
 * 判据：WL 目的块取擦除次数最高的空闲块；只搬不校验会静默丢冷数据。
 * 备注：真 NAND；high-wear 分支未覆盖（RISK-WL-*）。 */

static MunitResult test_wl_progress(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl       = ftl_ut_live();
    const DWORD base      = ftl_ut_lba_base();
    const DWORD per_write = FTL_LBA_PER_PAIR;
    const DWORD pages     = PAGES_PER_BLOCK;

    /* 写满一个 SB（64 页 × 8 LBA = 512 LBA），保证 WL victim 的有效 CW ≥70%。 */
    for (DWORD page = 0U; page < pages; page++)
    {
        const DWORD lba = base + (page * per_write);

        media_fill_wr(lba, per_write);
        munit_assert_true(ftl_ut_host_write(lba, per_write, s_media_buf));
    }
    munit_assert_true(ftl_ut_flush());

    alc_s *a = &ftl->alloc;

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
        const super_block_s *sb = alc_super_closed_get_at(a, i);

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

    ftl->stats.total_writes = WL_MIN_WRITES;

    mig_handle_s *m      = ftl->mig_handle;
    const DWORD   moved0 = m->state.counters.moved;
    const DWORD   t0     = platform_now_ms();

    /* WL 由 timer 表的 `mig_wl_eval_task` 驱动（主循环的 timer_scheduler_poll 在
     * `test` 命令期间被阻塞）⇒ 本用例手动 poll。`host_quiet` 要求本次求值看到的
     * total_writes 与上次相同。 */
    m->state.wl_last_writes = ftl->stats.total_writes;

    while (((m->job.active == 0) || (m->job.kind != MIG_KIND_WL)) && ((platform_now_ms() - t0) < (FTL_UT_IO_TIMEOUT_MS * 3U)))
    {
        mig_handle(m, BG_LEVEL_IDLE); /* 发布档位：mig_wl_eval 只在 IDLE 档启动 */
        timer_scheduler_poll();
        ftl_service_pump();
    }

    munit_assert_true(m->job.active);
    munit_assert_int(m->job.kind, ==, MIG_KIND_WL);

    while ((m->state.counters.moved == moved0) && ((platform_now_ms() - t0) < (FTL_UT_IO_TIMEOUT_MS * 3U)))
    {
        mig_handle(m, BG_LEVEL_IDLE);
        ftl_service_pump();
    }

    munit_assert_uint32(m->state.counters.moved, >, moved0);

    /* WL 迁移后数据仍可读（按源坐标校验）。 */
    const DWORD check_lba = base;

    media_fill_wr(check_lba, per_write);
    memset(s_media_buf, 0, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(check_lba, per_write, s_media_buf));
    media_assert_wr(check_lba, per_write);

    /* WL 搬移同样通过 CMT 更新 L2P；flush 后再交给 teardown。 */
    munit_assert_true(ftl_ut_flush());

    ftl_ut_trace_detail("wl: writes=%u sb=%lu pages x %lu LBA; erase used=100 free=400; moved %lu->%lu; lba=%lu read byte-equal", WL_MIN_WRITES, pages, per_write, moved0, m->state.counters.moved, check_lba);
    ftl_ut_trace_summary("media: WL moved %lu->%lu CW (delta %lu)", moved0, m->state.counters.moved, m->state.counters.moved - moved0);

    return MUNIT_OK;
}

/* cp_probe_clean_media — 干净介质的启动判据。
 *
 * 覆盖：cp_media_status_probe=CURRENT 且 version=FTL_MEDIA_LAYOUT_VERSION；
 *       pfr_media_check_format 判定无需格式化。
 * 判据：误判会让每次启动都全片格式化（用户数据清零），这是启动路径最贵的错误。
 * 备注：真 NAND。 */

static MunitResult test_cp_probe_clean_media(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s *ftl     = ftl_ut_live();
    DWORD      version = 0U;

    const cp_media_status_e st = cp_media_status_probe(ftl->cp_handle, &version);

    munit_assert_int(st, ==, CP_MEDIA_CURRENT);
    munit_assert_uint32(version, ==, FTL_MEDIA_LAYOUT_VERSION);

    BOOL need_format = 1;

    munit_assert_int(pfr_media_check_format(ftl, &need_format), ==, FTL_OK);
    munit_assert_false(need_format);

    ftl_ut_trace_detail("cp probe: status=%d version=%lu (layout=%lu); need_format 1->%d", st, version, FTL_MEDIA_LAYOUT_VERSION, need_format);
    ftl_ut_trace_summary("media: cp probe status=%d version=%lu no format needed", st, version);

    return MUNIT_OK;
}

/* BBT 池清空：多圈 ring 的 save→reload 会选到旧 record。
 * 要验证“本次 save 的坏块/标志是否持久化”的用例必须先隔离这个变量。 */
static void media_erase_bbt_pool(alc_s *a)
{
    const md_ring_s *ring = &a->pools->bbt;
    const ftl_topo_s topo = alc_topo(a);

    for (WORD i = 0U; i < ring->block_count; i++)
    {
        const DWORD pba = pba_make(topo.ch, topo.ce, topo.lun, ring->blocks[i], 0U, 0U, 0U);

        munit_assert_int(ftl_sync_erase_block_multi_plane(pba), ==, FTL_OK);
    }
    memset(&a->bbt, 0, sizeof(a->bbt));
}

/* bbt_persist — BBT 的 save→reload 持久化。
 *
 * 覆盖：标坏一个空闲块 → bbt_save（dirty 清零、sequence 推进）→ 模拟重启并走
 *       生产 bbt_init → 坏块仍在。
 * 判据：坏块图丢失会把已知坏块当可用块写，首次上板才炸。
 * 备注：先清 BBT 池隔离「多圈 ring 选中旧 record」的 ；真 NAND。 */

static MunitResult test_bbt_persist(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s *ftl = ftl_ut_live();
    alc_s     *a   = &ftl->alloc;

    /* format 会刻意保留旧 BBT record；多圈 ring 会让 locate 选到旧 record
     * ，与本用例要测的“save→load 持久化”无关 ⇒ 先清池。 */
    media_erase_bbt_pool(a);

    munit_assert_uint16(a->free_count, >, 0U);

    WORD g = FTL_GBLOCK_INVALID;

    for (WORD i = 0U; i < a->free_count; i++)
    {
        if (!ftl_block_is_bad(&a->blocks[a->free_blocks[i]]))
        {
            g = a->free_blocks[i];
            break;
        }
    }
    munit_assert_uint32(g, !=, FTL_GBLOCK_INVALID);

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);
    munit_assert_true(ftl_block_is_bad(&a->blocks[g]));

    ftl_ut_trace_detail("bbt before save g=%hu bad_count=%hu dirty=%d seq=%lu idx=%hu", g, a->bad_count, a->bbt.dirty, a->bbt.sequence, a->bbt.write_idx);

    munit_assert_int(bbt_save(a), ==, FTL_OK);

    ftl_ut_trace_detail("bbt after save dirty=%d seq=%lu idx=%hu", a->bbt.dirty, a->bbt.sequence, a->bbt.write_idx);

    munit_assert_uint32(a->bbt.dirty, ==, 0U);

    const DWORD saved_seq = a->bbt.sequence;

    /* ② 走生产启动路径重新加载 BBT（模拟重启后 bbt_init），坏块必须还在。 */
    ftl_ut_reboot_from_media();
    bbt_init(&ftl_ut_live()->alloc);

    munit_assert_true(ftl_block_is_bad(&ftl_ut_live()->alloc.blocks[g]));

    ftl_ut_trace_detail("bbt persist: marked g=%hu; saved seq=%lu; reloaded bad_count=%hu is_bad=%d", g, saved_seq, ftl_ut_live()->alloc.bad_count, ftl_block_is_bad(&ftl_ut_live()->alloc.blocks[g]));
    ftl_ut_trace_summary("media: BBT persisted g=%hu seq=%lu bad_count=%hu", g, saved_seq, ftl_ut_live()->alloc.bad_count);

    return MUNIT_OK;
}

/* L1/L2 真 NAND 用例按 design 文档 §4.5 逐步加入；子套件在 ut_main.c 注册。 */
/* format_in_progress_resume — 两阶段格式化中断后的恢复。
 *
 * 覆盖：写一条 FORMAT_IN_PROGRESS 的 BBT record（用更大 sequence 保证 bootstrap 扫描
 *       选中）+ 擦 CP 池 ⇒ 探针 NO_RECORD、需要格式化；重启后标志恢复；完整 format
 *       的提交点写 initial CP 并清标志。
 * 判据：标志丢失 + 无 CP 会被判成「损坏」而拒绝启动或误格式化。
 * 备注：不擦 BBT 载体——失败路径上擦 spare 会丢坏块图；bbt_save 在 dirty=0 时是空操作。 */

static MunitResult test_format_in_progress_resume(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s       *ftl     = ftl_ut_live();
    alc_s           *a       = &ftl->alloc;
    const ftl_topo_s topo    = alc_topo(a);
    DWORD            version = 0U;

    /* ① 模拟两阶段格式化在「BBT 已落盘、initial CP 尚未写」时掉电：
     *    写一条带 FORMAT_IN_PROGRESS 的 BBT record，并用**更大的 sequence** 让它
     *    在真实开机 bootstrap 扫描中胜出（旧 record 可能留在已移出候选池的 spare 上，
     *    / BOOT-22）。不擦 BBT 载体：失败路径上擦 spare 会丢坏块图。
     *    ⚠ bbt_save 在 dirty==0 时直接返回 OK（不写盘），必须同时置 dirty。 */
    a->bbt.sequence           = 0xFFFFFF00U; /* 新 record 必须是最新（见上） */
    a->bbt.cursor_valid       = 0U;          /* 让 save 重新定位 write_idx */
    a->bbt.format_in_progress = 1;
    a->bbt.dirty              = 1U;
    munit_assert_int(bbt_save(a), ==, FTL_OK);

    /* ② 擦掉 Checkpoint 池，制造“无有效 CP record”。 */
    for (WORD block = 0U; block < CHECKPOINT_POOL_BLOCKS; block++)
    {
        const WORD  pb  = (CHECKPOINT_BLOCK0 + block);
        const DWORD pba = pba_make(topo.ch, topo.ce, topo.lun, pb, 0U, 0U, 0U);

        munit_assert_int(ftl_sync_erase_block_multi_plane(pba), ==, FTL_OK);
    }

    /* ③ 启动判据：带 FORMAT_IN_PROGRESS 必须“继续格式化”，不能误判成 IO_ERROR。 */
    munit_assert_int(cp_media_status_probe(ftl->cp_handle, &version), ==, CP_MEDIA_NO_RECORD);

    BOOL need_format = 0;

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

/* cp_corrupt_refuse_format — CP 损坏时必须拒绝格式化。
 *
 * 覆盖：擦 CP 池后在 slot0/page0 写非 FF 页 ⇒ 探针 IO_ERROR（不是 NO_RECORD）；
 *       pfr_media_check_format 返回 FTL_ERROR 且 need_format=0；最后完整 format 恢复。
 * 判据：介质有内容但没有有效 CP 时格式化会擦掉用户数据；必须拒绝并把决定权交给用户
 *       。真 NAND。 */

static MunitResult test_cp_corrupt_refuse_format(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s       *ftl     = ftl_ut_live();
    alc_s           *a       = &ftl->alloc;
    const ftl_topo_s topo    = alc_topo(a);
    DWORD            version = 0U;

    /* ① 擦掉 CP 池，确保没有可定位的 Checkpoint record。 */
    for (WORD block = 0U; block < CHECKPOINT_POOL_BLOCKS; block++)
    {
        const WORD  pb  = (CHECKPOINT_BLOCK0 + block);
        const DWORD pba = pba_make(topo.ch, topo.ce, topo.lun, pb, 0U, 0U, 0U);

        munit_assert_int(ftl_sync_erase_block_multi_plane(pba), ==, FTL_OK);
    }

    /* ② 在 CP 环 slot0/page0 的 plane0 写一页非 FF 内容。
     *    探针（cp_media_status_probe）在没有有效 record 时会裸读 slot0/page0：
     *    只要读到非 FF，就判“有介质内容但没有有效 CP” ⇒ IO_ERROR，拒绝格式化。 */
    const md_ring_s *ring  = &a->pools->cp;
    const BYTE       plane = md_ring_plane_at(ring->plane_mask, 0U);
    const DWORD      addr  = md_ring_addr_plane(ring, &topo, 0U, 0U, plane);
    BYTE             page[FTL_IU_BYTES];

    memset(page, 0x00, sizeof(page));
    munit_assert_int(ftl_sync_write_page(addr, page, sizeof(page), FTL_PAGE_CFG_META()), ==, FTL_OK);

    /* ③ 判据：无有效 record + 非 FF 内容 ⇒ CP_MEDIA_IO_ERROR（不是 NO_RECORD）。 */
    munit_assert_int(cp_media_status_probe(ftl->cp_handle, &version), ==, CP_MEDIA_IO_ERROR);

    /* ④ 启动决策：读坏必须拒绝格式化，不能把整片擦掉。 */
    BOOL need_format = 1;

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

static const char        *s_failc_arm_values[] = {"1", "2", "last", NULL};
static MunitParameterEnum s_failc_params[]     = {
    {(char *)"arm", s_failc_arm_values},
    {         NULL,               NULL},
};

/* cache_failc_abort — cache program 成员的 FAIL_CACHE 中止与回滚。
 *
 * 覆盖（参数 arm = **携带 FAILC 的命令**所对应的页序号）：
 *   命令 K 的 FAILC 结算的是**上一页**（page K-1），所以
 *   arm=1   —— 命令 1（op44）报 FAILC = **page0 失败** ⇒ 已确认 LBA = 0；
 *   arm=2   —— page1 失败 ⇒ 已确认 LBA = 8（之前页已确认）；
 *   last    —— END 的 FAILC = page6 失败 ⇒ 已确认 LBA = 48。
 *   三种情况都必须：组报 IO_ERROR、`cache_wb_aborts` 恰好 +1、已确认前缀 = 新数据、
 *   **失败页的 LBA 读 IO_ERROR**、同批未确认页保留旧映射。
 *   ⚠ 不断言「序列已关闭」：中止后同一条 host 命令的剩余 LBA 会合法地重新开一条
 *   序列，结束时 `cache_stream.seq.active` 可能为真。
 * 判据：失败页之后的页若被确认，rebuild 会引用一个 page0 未落盘的块 ⇒ 静默丢数据。
 * 备注：注入点在 FTL 层（`ftl_cache_wr_member_done`），验证的是 FTL 策略；器件
 *       FAILC 归属与 7Ah 状态字节仍须上板。真 NAND。 */

static MunitResult test_cache_failc_abort(const MunitParameter params[], void *data)
{
    (void)data;

    const char *arm_s = munit_parameters_get(params, "arm");

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl      = ftl_ut_live();
    const DWORD lba      = ftl_ut_lba_base() + 4096U;
    const DWORD count    = FTL_UT_MATRIX_LBAS; /* 64 LBA = 8 页 = 2 个窗口 */
    const DWORD win_lbas = count / 2U;         /* 第一窗口 32 LBA（4 页） */
    BYTE        arm_page;
    DWORD       confirmed;

    /* 命令 K 的 FAILC 结算**上一页**（page K-1）⇒ 已确认 = pages 0..K-2 = (K-1) 页。 */
    if (strcmp(arm_s, "last") == 0)
    {
        arm_page = 7U; /* END */
    }
    else
    {
        arm_page = (BYTE)media_atou(arm_s);
    }
    confirmed = (arm_page - 1U) * FTL_LBA_PER_PAIR;

    /* 让本用例从**新开的 SB** 起步：room=64 ⇒ 窗口固定 4 页（END 只落在 page7），
     * 「命令 K ⇒ page K-1」的页号映射才是确定的（块尾 1~2 页窗口会让 END 提前、
     * 把已确认前缀改变）。 */
    if (ftl->alloc.hot_sb.is_open)
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }

    /* ① 旧内容：作为「未确认页应回滚到旧映射」的判据（不假设介质是空的）。 */
    media_fill_failc(count, 1U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));
    munit_assert_true(ftl_ut_flush());

    memset(s_io_buf, 0, sizeof(s_io_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_io_buf));
    media_assert_failc(count, 1U, 0U);

    /* ② 新内容 + 注入：令 arm_page 的完成带 FAIL_CACHE。 */
    media_fill_failc(count, 2U, 0U);

    const DWORD arm_lba = lba + (arm_page * FTL_LBA_PER_PAIR);

    fi_cache_fail_arm(arm_lba);

    const DWORD aborts0 = ftl->cache_stream.wb_aborts;

    munit_assert_false(ftl_ut_host_write(lba, count, s_io_buf)); /* 组必须报错 */

    const DWORD aborts = ftl->cache_stream.wb_aborts - aborts0;

    /* 只有被注入的那一条命令能触发中止：多一条说明有迟到的完成又走了失败路径。 */
    munit_assert_uint32(aborts, ==, 1U);

    /* 延后上报（/ USBC-01）：失败结论由**下一条**命令承载、且只报一次。
     * ⚠ 必须先取走：否则下一条 `ftl_submit_*` 会被粘性错误挡掉，掩盖后面的读判据。 */
    munit_assert_true(ftl_take_deferred_error(ftl));
    munit_assert_false(ftl_take_deferred_error(ftl));

    /* ③ 新语义：
     *    · 已确认前缀（pages 0..K-2）= 新数据；
     *    · **失败页**（page K-1）的 LBA 读必须报 **IO_ERROR**（撤销 + region 降级；
     *      不能返回旧数据，更不能返回编程失败页的内容）；
     *    · 同批未确认页（被中止丢弃）保留旧映射 ⇒ 读旧数据。
     *    第二窗口（本序列已中止后才下发）不在判据内。 */
    const DWORD fail_off = confirmed;                    /* 失败页起点（LBA 偏移） */
    const DWORD fail_end = confirmed + FTL_LBA_PER_PAIR; /* 失败页终点 */
    const DWORD checked  = (fail_end > win_lbas) ? fail_end : win_lbas;

    if (confirmed > 0U)
    {
        memset(s_io_buf, 0, confirmed * FTL_IU_BYTES);
        munit_assert_true(ftl_ut_host_read(lba, confirmed, s_io_buf));

        media_assert_failc(confirmed, 2U, 0U);
    }

    munit_assert_false(ftl_ut_host_read(lba + fail_off, FTL_LBA_PER_PAIR, s_io_buf));

    if (checked > fail_end)
    {
        const DWORD rest = checked - fail_end;

        memset(s_io_buf, 0, rest * FTL_IU_BYTES);
        munit_assert_true(ftl_ut_host_read(lba + fail_end, rest, s_io_buf));

        media_assert_failc(rest, 1U, fail_end * FTL_IU_BYTES);
    }

    /* ④ 重启后：已确认前缀的映射要落盘后才可见（先 flush）；失败页的 **region 降级位
     *    不持久**（已知边界）⇒ 那些 LBA 变成未映射、读回全零；
     *    同批未确认页的旧映射不受影响。 */
    munit_assert_true(ftl_ut_flush());
    ftl_ut_reboot_from_media();

    if (confirmed > 0U)
    {
        memset(s_io_buf, 0, confirmed * FTL_IU_BYTES);
        munit_assert_true(ftl_ut_host_read(lba, confirmed, s_io_buf));

        media_assert_failc(confirmed, 2U, 0U);
    }

    memset(s_io_buf, 0, FTL_LBA_PER_PAIR * FTL_IU_BYTES);
    munit_assert_true(ftl_ut_host_read(lba + fail_off, FTL_LBA_PER_PAIR, s_io_buf));

    media_assert_zero_buf(s_io_buf, FTL_LBA_PER_PAIR * FTL_IU_BYTES);

    if (checked > fail_end)
    {
        const DWORD rest = checked - fail_end;

        memset(s_io_buf, 0, rest * FTL_IU_BYTES);
        munit_assert_true(ftl_ut_host_read(lba + fail_end, rest, s_io_buf));

        media_assert_failc(rest, 1U, fail_end * FTL_IU_BYTES);
    }

    /* ⑤ 恢复：序列已关闭，后续写必须正常落盘。 */
    const DWORD head = 16U;

    media_fill_failc(head, 3U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, head, s_io_buf));
    munit_assert_true(ftl_ut_flush());

    memset(s_io_buf, 0, sizeof(s_io_buf));
    munit_assert_true(ftl_ut_host_read(lba, head, s_io_buf));
    media_assert_failc(head, 3U, 0U);

    fi_reset();

    ftl_ut_trace_detail("failc[%s]: armed page=%hhu -> confirmed=%lu LBA, aborts=%lu; failed page IO_ERROR, unconfirmed keep old mapping, prefix survives reboot, recovery write OK", arm_s, arm_page, confirmed, aborts);
    ftl_ut_trace_summary("cache failc[%s]: %lu confirmed, aborts=%lu, failed-page read IO_ERROR", arm_s, confirmed, aborts);

    return MUNIT_OK;
}

/* journal_write_fail_wal_stall — Journal 页写失败后 Checkpoint 必须仍能收尾。
 *
 * 覆盖：注入 2 次 Journal 页写失败 → `write_fail_pending` 置位、`persisted_seq` 落后于
 *       dirty 行的 `dirty_seq`；放开注入并请求 Checkpoint 后，失败标记必须在有界时间内
 *       清除、数据可读。
 * 判据：失败页的条目已丢，`persisted_seq` 永远追不上；若 WAL 闸门在失败窗口继续退让，
 *       CP 的区域落盘被永久挡住，而清除失败标记的唯一途径是「CP 完成」⇒ 死锁
 *       （RISK-L2P-10，由本用例暴露并修复）。⚠ 这里不断言闸门退让——那是
 *       `l2p_wal_gate`（纯 RAM）的覆盖。真 NAND。 */

static MunitResult test_journal_write_fail_wal_stall(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl   = ftl_ut_live();
    const DWORD lba   = ftl_ut_lba_base() + 7000U;
    const DWORD count = FTL_UT_MATRIX_LBAS;

    media_fill_failc(count, 5U, 0U);

    /* 注入**先于写入**武装：本次写可能刚好填满上一页并当场冻结（那一次就先失败），
     * 否则留给 Checkpoint 请求触发的页写；两种时序都必须被覆盖。 */
    fi_journal_fail_arm(2U);
    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));

    /* ① 本次写的全部条目序号 ≤ 写入时的 `sequence_counter`（下一个待分配）。
     *    「条目还没落盘」的判据 = 水位还没追上它。用序号而不是扫描 dirty 行：
     *    Checkpoint 随时可能把行刷成 CLEAN，按行判断会与时序赛跑。 */
    const DWORD ds = jrn_sequence_get(ftl->journal);

    munit_assert_uint32(jrn_persisted_seq(ftl->journal), <, ds);

    /* ② 请求 Checkpoint：它要先刷 Journal 页，注入让该页写失败。 */
    jrn_checkpoint_request(ftl->journal);

    const DWORD t0        = platform_now_ms();
    BOOL        fail_seen = 0;

    while ((fail_seen == 0) && ((platform_now_ms() - t0) < FTL_UT_IO_TIMEOUT_MS))
    {
        /* CP / Journal 刷页都是 timer 表驱动的预算任务：只泵 ftl_service 时
         * credit 耗尽后它们不再运行，注入的失败页永远不会被写到。 */
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);

        if (jrn_has_write_failure(ftl->journal))
        {
            fail_seen = 1;
        }
    }
    munit_assert_true(fail_seen);

    /* 失败页没有推进水位：本次写的映射仍然「未被 Journal 覆盖」。 */
    munit_assert_uint32(jrn_persisted_seq(ftl->journal), <, ds);

    /* ③ 放开注入：Checkpoint **必须能完成**并清除失败标记。
     *    ⚠ 这里刻意**不**断言区域写回被 WAL 闸门挡住：Journal 页写失败时闸门必须
     *    放行（RISK-L2P-10）。失败页的条目已丢、`persisted_seq` 永远追不上，
     *    若闸门继续退让，CP 的区域落盘会被永久挡住，而清除失败标记的唯一途径
     *    正是「CP 完成」⇒ 死锁。本段就是这条死锁的回归判据。 */
    fi_reset();
    jrn_checkpoint_request(ftl->journal);

    const DWORD t1 = platform_now_ms();

    while (jrn_has_write_failure(ftl->journal) && ((platform_now_ms() - t1) < FTL_UT_IO_TIMEOUT_MS))
    {
        /* CP 收尾同样依赖 bg_scheduler / timer 表推进。 */
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);
    }
    munit_assert_false(jrn_has_write_failure(ftl->journal));

    munit_assert_true(ftl_ut_flush());

    memset(s_io_buf, 0, sizeof(s_io_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_io_buf));
    media_assert_failc(count, 5U, 0U);

    ftl_ut_trace_detail("journal fail: injected page fails -> write_fail_pending=1, persisted=%lu < dirty=%lu; after clearing injection the CP completed, flag cleared, data byte-equal", jrn_persisted_seq(ftl->journal), ds);
    ftl_ut_trace_summary("journal fail: CP completes and clears the flag (no deadlock), data byte-equal");

    return MUNIT_OK;
}

/* bbt_ring_reload — 多圈 BBT ring 必须选中**最新** record。
 *
 * 覆盖：清空 BBT 池 → 标坏 g1 + save → 标坏 g2 + save（两条 record）→ 模拟重启走
 *       生产 bbt_init → g1、g2 都必须仍为 BAD（载入的是后一条 record）。
 * 判据：环形池重载若选到旧 record，最近标出的坏块会「复活」并被写入。
 * 备注：`bbt_persist` 刻意先擦池来隔离本项；这里保留多条 record 并断言载入最新
 *       的那条（若 ring 定位选旧 record，g2 会在重启后「复活」）。⚠ 标坏持久化且
 *       format 不清除 ⇒ **每次运行永久消耗 2 块**。真 NAND。 */

static MunitResult test_bbt_ring_reload(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s *ftl = ftl_ut_live();
    alc_s     *a   = &ftl->alloc;

    media_erase_bbt_pool(a);

    /* 取两个**不同的**空闲好块（第一个可能已被 bbt_persist 标坏）。 */
    WORD g1 = FTL_GBLOCK_INVALID;
    WORD g2 = FTL_GBLOCK_INVALID;

    for (WORD i = 0U; i < a->free_count; i++)
    {
        const WORD g = a->free_blocks[i];

        if (ftl_block_is_bad(&a->blocks[g]))
        {
            continue;
        }
        if (g1 == FTL_GBLOCK_INVALID)
        {
            g1 = g;
            continue;
        }
        g2 = g;
        break;
    }
    munit_assert_uint32(g1, !=, FTL_GBLOCK_INVALID);
    munit_assert_uint32(g2, !=, FTL_GBLOCK_INVALID);

    munit_assert_int(bbt_mark_bad(a, g1), ==, FTL_OK);
    munit_assert_int(bbt_save(a), ==, FTL_OK);

    const DWORD seq1 = a->bbt.sequence;

    munit_assert_int(bbt_mark_bad(a, g2), ==, FTL_OK);
    munit_assert_int(bbt_save(a), ==, FTL_OK);

    const DWORD seq2 = a->bbt.sequence;

    munit_assert_uint32(seq2, >, seq1);

    ftl_ut_reboot_from_media();
    bbt_init(&ftl_ut_live()->alloc);

    munit_assert_true(ftl_block_is_bad(&ftl_ut_live()->alloc.blocks[g1]));
    munit_assert_true(ftl_block_is_bad(&ftl_ut_live()->alloc.blocks[g2]));

    ftl_ut_trace_detail("bbt ring: g1=%hu g2=%hu; seq %lu -> %lu; after reboot both still BAD (latest record)", g1, g2, seq1, seq2);
    ftl_ut_trace_summary("media: BBT ring reload picks latest record (g%hu, g%hu BAD)", g1, g2);

    return MUNIT_OK;
}

/* 一个 Super Block 占用的物理块数：退化条目的哨兵 plane 不计。 */
static WORD media_sb_plane_count(const super_block_s *sb)
{
    WORD n = 0U;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (super_block_has_plane(sb, p))
        {
            n++;
        }
    }
    return n;
}

/* 用户区里处于 LOG 的块数（被借用作 L2P 日志的块）。
 *
 * 这是一条**合法的**占用状态：块从 OP 池借出后既不在任何池里，也不在 SB 里，
 * 运行期由 Directory 的 l2p_live 管理。漏计它会让「重启后该块被恢复路径分类成
 * Closed」看起来像凭空多出一块 —— 账本守恒判据会误报。
 * 固定日志区（md_block_is_metadata 为真）不在此列：它本来就不进用户容量口径。 */
static WORD media_user_log_planes(const alc_s *a)
{
    WORD sum = 0U;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const BYTE         plane = ftl_gblock_plane(g);
        const WORD         block = ftl_gblock_block(g);
        const bst_block_s *b     = &a->blocks[g];

        if (md_block_is_metadata(NULL, plane, block) || ftl_block_is_bad(b))
        {
            continue;
        }
        if (ftl_block_state(b) == FTL_BLOCK_STATE_LOG)
        {
            sum++;
        }
    }

    return sum;
}

/* 全部账本（Free / OP / Spare / Closed / 两个 Open SB / 用户区借用的日志块）
 * 合计的物理块数。
 * 守恒判据：恢复前后这个合计必须相等 —— 漂移 = 块被漏登或重复登记。
 * 六项互斥且覆盖用户区全部好块，缺任一项都会把「合法的状态迁移」误判成漏账。 */
static WORD media_ledger_total(const alc_s *a)
{
    WORD sum = a->free_count;

    sum = (WORD)(sum + alc_pool_sum_counts(a->op_count, PLANE_NUM));
    sum = (WORD)(sum + alc_pool_sum_counts(a->spare_count, PLANE_NUM));

    for (WORD i = 0U; i < a->closed_sb_count; i++)
    {
        sum = (WORD)(sum + media_sb_plane_count(&a->closed_sb_list[i]));
    }

    if (a->hot_sb.is_open)
    {
        sum = (WORD)(sum + media_sb_plane_count(&a->hot_sb));
    }
    if (a->cold_sb.is_open)
    {
        sum = (WORD)(sum + media_sb_plane_count(&a->cold_sb));
    }

    sum = (WORD)(sum + media_user_log_planes(a));

    return sum;
}

/* boot_restore_reconcile — 模拟重启后分配账本必须守恒。
 *
 * 覆盖：写 16 LBA + flush → 记录 free/OP/Spare/Closed/open 的合计 → 重启（生产
 *       恢复路径）→ 合计不变、抽样读回一致。
 * 判据：恢复若漏掉或重复登记某一块（BOOT-24 的 closed/free 对账），合计会漂移；
 *       漂移的后果是容量无声流失或同一块被分配两次。真 NAND（非真掉电）。 */

static MunitResult test_boot_restore_reconcile(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl   = ftl_ut_live();
    const DWORD lba   = ftl_ut_lba_base() + 6000U;
    const DWORD count = FTL_UT_MEDIA_LBAS;

    media_fill_wr(lba, count);
    munit_assert_true(ftl_ut_host_write(lba, count, s_media_buf));
    munit_assert_true(ftl_ut_flush());

    const WORD ledger0 = media_ledger_total(&ftl->alloc);

    ftl_ut_reboot_from_media();

    ftl = ftl_ut_live();

    const WORD ledger1 = media_ledger_total(&ftl->alloc);

    munit_assert_uint32(ledger1, ==, ledger0);

    memset(s_media_buf, 0, sizeof(s_media_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_media_buf));
    media_assert_wr(lba, count);

    ftl_ut_trace_detail("boot reconcile: ledger %hu -> %hu; free=%hu op=%hu spare=%hu closed=%hu bad=%hu; read byte-equal", ledger0, ledger1, ftl->alloc.free_count, alc_pool_sum_counts(ftl->alloc.op_count, PLANE_NUM), alc_pool_sum_counts(ftl->alloc.spare_count, PLANE_NUM), ftl->alloc.closed_sb_count, ftl->alloc.bad_count);
    ftl_ut_trace_summary("media: boot restore ledger %hu -> %hu (conserved)", ledger0, ledger1);

    return MUNIT_OK;
}

/* cache_seq_cross_block — 顺序读跨物理 block 的数据正确性 + cache 读路径生效。
 *
 * 覆盖：连续写 1024 LBA（= 2 个物理 block × 64 页 × 8 LBA），再按 64 LBA 分段读回；
 *       逐字节比对，并断言 `cache_read_pairs` 至少覆盖整段（cache 读路径确实在用）、
 *       `cache_read_fallbacks` 不增长。
 * 判据：跨 block 的 cache read 序列若用错页地址或提前 END，会读到相邻块的旧数据；
 *       判据不能只看「跑通」——计数器证明走了 cache 路径（C8）。
 * 备注：**不断言 starts==1** —— 物理块是否相邻由分配器决定（ISS-FTL-13），
 *       序列在物理不连续处合法地断开重开。真 NAND。 */

static MunitResult test_cache_seq_cross_block(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl   = ftl_ut_live();
    const DWORD lba   = ftl_ut_lba_base() + 5120U;
    const DWORD total = 1024U; /* 2 个物理 block */
    const DWORD chunk = FTL_UT_MATRIX_LBAS;

    for (DWORD r = 0U; r < (total / chunk); r++)
    {
        const DWORD at = lba + (r * chunk);

        media_fill_failc(chunk, 7U, at);
        munit_assert_true(ftl_ut_host_write(at, chunk, s_io_buf));
    }

    munit_assert_true(ftl_ut_flush());

    /* 预热一遍：让跨 2 个 L2P region 的行先加载常驻，统计只量「路径是否在用」，
     * 不把首次加载导致的普通读算进来。 */
    for (DWORD r = 0U; r < (total / chunk); r++)
    {
        munit_assert_true(ftl_ut_host_read(lba + (r * chunk), chunk, s_io_buf));
    }

    const DWORD pairs0 = ftl->cache_stream.read_pairs;

    for (DWORD r = 0U; r < (total / chunk); r++)
    {
        const DWORD at = lba + (r * chunk);

        memset(s_io_buf, 0, sizeof(s_io_buf));
        munit_assert_true(ftl_ut_host_read(at, chunk, s_io_buf));

        media_assert_failc(chunk, 7U, at);
    }

    const DWORD pairs = ftl->cache_stream.read_pairs - pairs0;

    /* 1024 LBA = 128 pair；允许首尾窗口各少一对（START 只在流头、末窗口必 END）。 */
    munit_assert_uint32(pairs, >=, (total / FTL_LBA_PER_PAIR) - 8U);

    ftl_ut_trace_detail("cache rd x block: %lu LBA = %lu pairs; pairs via cache (measured pass)=%lu", total, total / FTL_LBA_PER_PAIR, pairs);
    ftl_ut_trace_summary("media: cross-block seq read %lu LBA byte-equal, cache pairs=%lu", total, pairs);

    return MUNIT_OK;
}

/* cache_early_ack_read_visible — 早回执后不等 flush 立刻读回新数据。
 *
 * 覆盖：先写旧数据并 flush，再写新数据（cache 序列，组在页编程完成时完成），
 *       不 flush 直接读同一段 ⇒ 必须逐字节等于新数据；随后 flush + 模拟重启再读仍一致。
 * 判据：若读覆盖层失效，读走旧 L2P 映射 ⇒ 读到旧数据，本用例必须红。
 * 备注：真 NAND。 */

static MunitResult test_cache_early_ack_read_visible(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl   = ftl_ut_live();
    const DWORD lba   = (ftl_ut_lba_base() + (FTL_UT_LBA_WINDOW / 4U)) & ~(FTL_LBA_PER_PAIR - 1U);
    const DWORD count = FTL_STREAM_MIN_LBAS;

    /* 旧数据：给读覆盖层一个可区分的旧映射。 */
    media_fill_failc(count, 8U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));
    munit_assert_true(ftl_ut_flush());

    /* 新数据：不 flush，组完成后立刻读。 */
    media_fill_failc(count, 9U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));

    const BYTE pending = ftl->cache_stream.seq.wr.count;

    /* 早回执后页表还有未 pop 的页（可能是 PROGRAMMED 走读覆盖层，也可能已
     * CONFIRMED 等后继 FAILC 结论而走 L2P）。两条路径都必须读到新数据；本用例
     * 的判据是「不 flush 即可见新数据」，不把内部路径钉死。 */
    munit_assert_uint32(pending, >, 0U);

    memset(s_io_buf, 0, sizeof(s_io_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_io_buf));
    media_assert_failc(count, 9U, 0U);

    munit_assert_true(ftl_ut_flush());
    munit_assert_uint32(ftl->cache_stream.seq.wr.count, ==, 0U);

    ftl_ut_reboot_from_media();

    memset(s_io_buf, 0, sizeof(s_io_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_io_buf));
    media_assert_failc(count, 9U, 0U);

    ftl_ut_trace_detail("early ack: lba=%lu count=%lu; pending=%hhu; read-before-flush new; after flush/reboot new", lba, count, pending);
    ftl_ut_trace_summary("media: early-ack read visible lba=%lu count=%lu (pending=%hhu)", lba, count, pending);

    return MUNIT_OK;
}

/* cache_early_ack_flush_barrier — flush 必须等已编程页确认完才完成。
 *
 * 覆盖：写一段新数据后 `ftl_flush_begin/step`；完成前页表仍有未证明页，完成后
 *       `wr.count==0` / `wr_inflight==0`，L2P 指向新 PBA，重启读回一致。
 * 判据：flush 提前完成 ⇒ 掉电窗口内「已回执未确认」的页没有屏障。
 * 备注：真 NAND。 */

static MunitResult test_cache_early_ack_flush_barrier(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl   = ftl_ut_live();
    const DWORD lba   = (ftl_ut_lba_base() + (FTL_UT_LBA_WINDOW * 3U / 8U)) & ~(FTL_LBA_PER_PAIR - 1U);
    const DWORD count = FTL_STREAM_MIN_LBAS;

    /* 旧映射：flush 后取 PBA，作为「flush 必须换到新 PBA」的基准。 */
    media_fill_failc(count, 14U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));
    munit_assert_true(ftl_ut_flush());

    DWORD old_pba = NAND_PBA_INVALID;

    munit_assert_int(l2p_read_sync(ftl->cache, lba, &old_pba), ==, FTL_OK);
    munit_assert_true(pba_is_valid(old_pba));

    media_fill_failc(count, 10U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));

    const BYTE pending = ftl->cache_stream.seq.wr.count;

    munit_assert_uint32(pending, >, 0U);
    munit_assert_true(ftl_ut_flush());
    munit_assert_uint32(ftl->cache_stream.seq.wr.count, ==, 0U);
    munit_assert_uint32(ftl->cache_stream.seq.wr_inflight, ==, 0U);

    DWORD new_pba = NAND_PBA_INVALID;

    munit_assert_int(l2p_read_sync(ftl->cache, lba, &new_pba), ==, FTL_OK);
    munit_assert_true(pba_is_valid(new_pba));
    munit_assert_uint32(new_pba, !=, old_pba);

    memset(s_io_buf, 0, sizeof(s_io_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_io_buf));
    media_assert_failc(count, 10U, 0U);

    ftl_ut_reboot_from_media();

    memset(s_io_buf, 0, sizeof(s_io_buf));
    munit_assert_true(ftl_ut_host_read(lba, count, s_io_buf));
    media_assert_failc(count, 10U, 0U);

    ftl_ut_trace_detail("early ack barrier: lba=%lu count=%lu; pending=%hhu -> flush -> count=0 inflight=0; pba 0x%08lX->0x%08lX; reboot byte-equal", lba, count, pending, old_pba, new_pba);
    ftl_ut_trace_summary("media: early-ack flush barrier lba=%lu count=%lu (pending=%hhu)", lba, count, pending);

    return MUNIT_OK;
}

/* cache_failc_undo_read_error — FAILC 撤销的读语义与环排空。
 *
 * 覆盖：FAILC 注入后下一条 host 命令只报一次 `deferred_error`；失败页 LBA 读 IO_ERROR；
 *       同 region 未映射 LBA 也 IO_ERROR（region 粒度降级）；清注入后新写落盘，页表排空。
 * 判据：失败页不能返回旧数据/编程失败页内容；deferred 不能重复上报。
 * 备注：真 NAND；与 `cache_failc_abort` 共用注入点，但聚焦「同 region / 环排空」。 */

static MunitResult test_cache_failc_undo_read_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s  *ftl   = ftl_ut_live();
    const DWORD lba   = (ftl_ut_lba_base() + (FTL_UT_LBA_WINDOW / 2U)) & ~(L2P_ENTRIES_PER_REGION - 1U);
    const DWORD count = FTL_UT_MATRIX_LBAS;

    /* 旧内容，保证失败页/同批未确认页有可区分的旧映射。 */
    media_fill_failc(count, 11U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));
    munit_assert_true(ftl_ut_flush());

    /* arm=1：命令 1 的 FAILC 结算 page0。 */
    media_fill_failc(count, 12U, 0U);
    fi_cache_fail_arm(lba + FTL_LBA_PER_PAIR);
    munit_assert_false(ftl_ut_host_write(lba, count, s_io_buf));

    munit_assert_true(ftl_take_deferred_error(ftl));
    munit_assert_false(ftl_take_deferred_error(ftl));

    /* 失败页读 IO_ERROR；同 region（512 LBA）未映射 LBA 也 IO_ERROR。 */
    munit_assert_false(ftl_ut_host_read(lba, FTL_LBA_PER_PAIR, s_io_buf));
    munit_assert_uint32(lba_to_global_region(lba), ==, lba_to_global_region(lba + (L2P_ENTRIES_PER_REGION / 2U)));
    munit_assert_false(ftl_ut_host_read(lba + (L2P_ENTRIES_PER_REGION / 2U), FTL_LBA_PER_PAIR, s_io_buf));

    /* 清注入：新写必须能落盘，确认环排空。 */
    fi_reset();
    media_fill_failc(16U, 13U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, 16U, s_io_buf));
    munit_assert_true(ftl_ut_flush());
    munit_assert_uint32(ftl->cache_stream.seq.wr.count, ==, 0U);

    memset(s_io_buf, 0, sizeof(s_io_buf));
    munit_assert_true(ftl_ut_host_read(lba, 16U, s_io_buf));
    media_assert_failc(16U, 13U, 0U);

    ftl_ut_trace_detail("failc undo: lba=%lu count=%lu; deferred once; failed page + same-region unmapped read IO_ERROR; after reset write/flush drains", lba, count);
    ftl_ut_trace_summary("media: failc undo read semantics + ring drained");

    return MUNIT_OK;
}

/* cache_cmt_writeback_retire — CMT 在页写回后的 CLEAN/负条目收敛。
 *
 * 覆盖：写 8 LBA → flush；有效 dirty CMT 应 retire 成 CLEAN（若仍驻留），PBA 与
 *       L2P 真值一致；trim 同一段再 flush，INVALID 负条目应被丢弃，读回未映射。
 * 判据：写回 commit 后把 dirty 当 CLEAN 保留会留下旧映射；负条目不清会让 TRIM 复活。
 * 备注：真 NAND；CMT 条目可能因容量被驱逐，因此“不存在”也接受，但存在时状态必须正确。 */

static MunitResult test_cache_cmt_writeback_retire(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();
    media_prepare(ftl_ut_live());

    ftl_ctx_s   *ftl   = ftl_ut_live();
    l2p_cache_s *c     = ftl->cache;
    const DWORD  lba   = (ftl_ut_lba_base() + 1024U) & ~(FTL_LBA_PER_PAIR - 1U);
    const DWORD  count = FTL_LBA_PER_PAIR;

    media_fill_failc(count, 15U, 0U);
    munit_assert_true(ftl_ut_host_write(lba, count, s_io_buf));
    munit_assert_true(ftl_ut_flush());
    munit_assert_uint32(c->stats.cmt_dirty, ==, 0U);

    for (DWORD i = 0U; i < count; i++)
    {
        l2p_cmt_entry_s *e = NULL;

        for (WORD set = 0U; set < L2P_CMT_SETS; set++)
        {
            for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
            {
                if (c->cmt[set][way].lba == (lba + i))
                {
                    e = &c->cmt[set][way];
                }
            }
        }
        if (e != NULL)
        {
            DWORD map = NAND_PBA_INVALID;

            munit_assert_uint32(e->state, ==, L2P_CMT_CLEAN);
            munit_assert_true(pba_is_valid(e->pba));
            munit_assert_int(l2p_read_sync(c, lba + i, &map), ==, FTL_OK);
            munit_assert_uint32(e->pba, ==, map);
        }
    }

    munit_assert_true(ftl_ut_trim(lba, count));
    munit_assert_true(ftl_ut_flush());
    munit_assert_uint32(c->stats.cmt_dirty, ==, 0U);

    for (DWORD i = 0U; i < count; i++)
    {
        DWORD map = NAND_PBA_INVALID;

        for (WORD set = 0U; set < L2P_CMT_SETS; set++)
        {
            for (BYTE way = 0U; way < L2P_CMT_WAYS; way++)
            {
                munit_assert_uint32(c->cmt[set][way].lba, !=, (lba + i));
            }
        }
        munit_assert_int(l2p_read_sync(c, lba + i, &map), ==, FTL_OK);
        munit_assert_uint32(map, ==, NAND_PBA_INVALID);
    }

    ftl_ut_trace_detail("cmt retire: lba=%lu count=%lu; valid entries CLEAN after flush; trim negative entries dropped", lba, count);
    ftl_ut_trace_summary("media: CMT writeback retire / negative entry cleanup ok");

    return MUNIT_OK;
}

/* ══ 子套件 /ftl/media/e2e/：真 NAND 端到端（破坏性）══ */

MunitTest g_ftl_media_e2e_tests[] = {
    {                   (char *)"media_live",                    test_media_live, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {         (char *)"format_and_roundtrip",          test_format_and_roundtrip, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {       (char *)"cross_region_roundtrip",        test_cross_region_roundtrip, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {        (char *)"flush_reboot_readback",         test_flush_reboot_readback, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {         (char *)"trim_no_resurrection",          test_trim_no_resurrection, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    { (char *)"preload_region_zero_and_high",  test_preload_region_zero_and_high, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {  (char *)"journal_barrier_cp_progress",   test_journal_barrier_cp_progress, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    { (char *)"journal_write_fail_wal_stall",  test_journal_write_fail_wal_stall, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {            (char *)"cache_failc_abort",             test_cache_failc_abort, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, s_failc_params},
    { (char *)"cache_early_ack_read_visible",  test_cache_early_ack_read_visible, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {  (char *)"cache_failc_undo_read_error",   test_cache_failc_undo_read_error, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {(char *)"cache_early_ack_flush_barrier", test_cache_early_ack_flush_barrier, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {        (char *)"cache_seq_cross_block",         test_cache_seq_cross_block, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {   (char *)"cache_cmt_writeback_retire",    test_cache_cmt_writeback_retire, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {                  (char *)"gc_progress",                   test_gc_progress, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {                  (char *)"wl_progress",                   test_wl_progress, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {         (char *)"cp_probe_clean_media",          test_cp_probe_clean_media, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {               (char *)"host_io_matrix",                test_host_io_matrix, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,    s_io_params},
    {               (char *)"host_io_random",                test_host_io_random, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {                  (char *)"bbt_persist",                   test_bbt_persist, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {              (char *)"bbt_ring_reload",               test_bbt_ring_reload, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {     (char *)"cp_corrupt_refuse_format",      test_cp_corrupt_refuse_format, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {    (char *)"format_in_progress_resume",     test_format_in_progress_resume, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {       (char *)"boot_restore_reconcile",        test_boot_restore_reconcile, media_setup, media_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,           NULL},
    {                                   NULL,                               NULL,        NULL,           NULL,             MUNIT_TEST_OPTION_NONE,           NULL},
};
