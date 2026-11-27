/*
 * ftl_ut_common.c — FTL 板端 UT 共享夹具
 *
 * §1 证据输出（[EV] 行，与 FC UT 同格式）
 * §2 活实例
 * §3 伪时钟
 * §4 用例结束不变量
 */

#include "ftl_ut_common.h"

#include "checkpoint.h"
#include "cli.h" /* g_ctrl */
#include "fault_inject.h"
#include "fc_interface.h"
#include "fc_nand_dispatch.h"
#include "fc_nand_engine.h"
#include "ftl_op.h"
#include "journal.h"
#include "metadata_job.h"
#include "migrate.h"
#include "pending.h"
#include "platform.h"
#include "power_failure_recovery.h"
#include "ssd_compiler.h"
#include "timer.h"
#include "wbuf.h"

#include <stdio.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 * §1 证据输出
 * ══════════════════════════════════════════════════════════════════════ */

/* munit 打印用例前缀时不带换行；第一条证据行需要先给它收尾。 */
static BOOL s_ev_line_started = 0;
static BOOL s_ev_suppressed   = 0;

void ftl_ut_trace_begin(void)
{
    /* 只复位状态；第一条可见证据由 ftl_ut_trace_emit 负责换行。
     * 不在这里提前写 '\n'：否则无证据的用例也会把 munit 的 [ OK ] 顶到下一行。 */
    s_ev_line_started = 0;
}

void ftl_ut_trace_suppress(BOOL suppressed)
{
    s_ev_suppressed = suppressed;
}

static void ftl_ut_trace_emit(BOOL force, MunitLogLevel level, const char *tag, const char *format, va_list ap)
{
    if (s_ev_suppressed || ((force == 0) && !munit_log_visible(level)))
    {
        return;
    }

    if (!s_ev_line_started)
    {
        (void)fputc('\n', MUNIT_OUTPUT_FILE);
        s_ev_line_started = 1;
    }

    (void)fputs(tag, MUNIT_OUTPUT_FILE);
    (void)vfprintf(MUNIT_OUTPUT_FILE, format, ap);
    (void)fputc('\n', MUNIT_OUTPUT_FILE);
    (void)fflush(MUNIT_OUTPUT_FILE);
}

void ftl_ut_trace_summary(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    ftl_ut_trace_emit(0, MUNIT_LOG_INFO, "[EV] ", format, ap);
    va_end(ap);
}

void ftl_ut_trace_detail(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    ftl_ut_trace_emit(0, MUNIT_LOG_DEBUG, "[EV]   ", format, ap);
    va_end(ap);
}

void ftl_ut_result(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    ftl_ut_trace_emit(1, MUNIT_LOG_INFO, "[EV] ", format, ap);
    va_end(ap);
}

/* ══════════════════════════════════════════════════════════════════════
 * §2 活实例
 * ══════════════════════════════════════════════════════════════════════ */

ftl_ctx_s *ftl_ut_live(void)
{
    ftl_ctx_s *ftl = g_ctrl.ftl[0];

    munit_assert_not_null(ftl);
    return ftl;
}

void ftl_ut_topology_init(void)
{
    static BOOL s_topology_ready = 0;

    if (s_topology_ready)
    {
        return;
    }

    /* 用上电扫描结果重建，而不是依赖主 boot 留下的表；与生产建表同一入口。 */
    munit_assert_true(ftl_topology_build(g_nand_targets));
    munit_assert_uint32(DIE_MAX, >, 0U);
    s_topology_ready = 1;
}

/* ══════════════════════════════════════════════════════════════════════
 * §2b 测试自有 RAM 表（结构体与生产同型，只填小状态）
 * ══════════════════════════════════════════════════════════════════════ */

static bst_block_s           s_ut_blocks[FTL_BLOCK_TOTAL] SRAM12_DATA;
static WORD                  s_ut_free[FTL_BLOCK_TOTAL] SRAM12_DATA;
static alc_op_block_arr_t    s_ut_op[PLANE_NUM] SRAM12_DATA;
static alc_spare_block_arr_t s_ut_spare[PLANE_NUM] SRAM12_DATA;
static super_block_s         s_ut_closed[SUPER_BLOCK_LIST_MAX] SRAM12_DATA;

static ftl_table_refs_s s_ut_refs;
static alc_s            s_ut_alloc;
/* md_dir_cache_s 约 33 KB；AXI_RAM 已被生产 FTL 的 cache/CMT 占满，
 * 测试自有副本放 AXI_NOCACHE（无 DMA 访问，只是借空间）。 */
static md_dir_cache_s s_ut_md NONCACHEABLE_BUFFER;
static DWORD                  s_ut_total_writes;

void ftl_ut_ram_init(void)
{
    ftl_ctx_s *live = ftl_ut_live();

    memset(s_ut_blocks, 0, sizeof(s_ut_blocks));
    memset(s_ut_free, 0, sizeof(s_ut_free));
    memset(s_ut_op, 0, sizeof(s_ut_op));
    memset(s_ut_spare, 0, sizeof(s_ut_spare));
    memset(s_ut_closed, 0, sizeof(s_ut_closed));
    memset(&s_ut_md, 0, sizeof(s_ut_md));

    s_ut_refs.blocks         = s_ut_blocks;
    s_ut_refs.free_blocks    = s_ut_free;
    s_ut_refs.op_blocks      = s_ut_op;
    s_ut_refs.spare_blocks   = s_ut_spare;
    s_ut_refs.closed_sb_list = s_ut_closed;

    s_ut_total_writes = 0U;
    alc_init(&s_ut_alloc, live->topo, &s_ut_refs);

    s_ut_md.topo = live->topo;
    md_init(&s_ut_md, &s_ut_alloc, &s_ut_total_writes);

    s_ut_alloc.md_dir = &s_ut_md;
    s_ut_alloc.pools  = &s_ut_md.pools;
}

alc_s *ftl_ut_ram_alloc(void)
{
    return &s_ut_alloc;
}

md_dir_cache_s *ftl_ut_ram_md(void)
{
    return &s_ut_md;
}

/* ══════════════════════════════════════════════════════════════════════
 * §3 伪时钟
 * ══════════════════════════════════════════════════════════════════════ */

static QWORD                     s_ut_us;
static const ssd_platform_ops_s *s_saved_ops;

static QWORD ftl_ut_time_us(void)
{
    return s_ut_us;
}

static DWORD ftl_ut_time_ms(void)
{
    return (DWORD)(s_ut_us / 1000U);
}

void ftl_ut_clock_install(void)
{
    static const ssd_platform_ops_s ops = {.time_us = ftl_ut_time_us, .time_ms = ftl_ut_time_ms};

    s_saved_ops = ssd_platform_get();
    munit_assert_not_null(s_saved_ops);

    s_ut_us = (s_saved_ops->time_us != NULL) ? s_saved_ops->time_us() : 0U;
    munit_assert_int(ssd_platform_register(&ops), ==, 0);
}

void ftl_ut_clock_restore(void)
{
    if (s_saved_ops != NULL)
    {
        (void)ssd_platform_register(s_saved_ops);
        s_saved_ops = NULL;
    }
}

void ftl_ut_clock_set_ms(DWORD ms)
{
    s_ut_us = (QWORD)ms * 1000U;
}

void ftl_ut_clock_advance_ms(DWORD ms)
{
    s_ut_us += (QWORD)ms * 1000U;
}

/* ══════════════════════════════════════════════════════════════════════
 * §3b 真 NAND 媒体夹具（破坏性）
 * ══════════════════════════════════════════════════════════════════════ */

/* 运行期容量尾窗起点：host_lba_max 已扣实际坏块，不能再用编译期 FTL_LBA_MAX
 * （否则尾窗会落到主机可见容量之外）。 */
DWORD ftl_ut_lba_base(void)
{
    const DWORD max = ssd_controller_host_lba_max(&g_ctrl);

    munit_assert_uint32(max, >, FTL_UT_LBA_WINDOW);
    return (DWORD)((max - FTL_UT_LBA_WINDOW) & ~(FTL_LBA_PER_PAIR - 1U));
}

static volatile BYTE         s_io_done;
static volatile ssd_status_e s_io_status;

/* 让在飞命令收敛：停掉后台新任务，泵到 FC 池/CQ/ctx 全空。
 * ⚠ 必须在 ftl_init 前做：否则旧 CP/Journal 的迟到完成会在新实例上触发
 *    `h->waiting != 0` 断言（checkpoint.c）。
 * ⚠ 返回时 `bg_suspend` 仍为 1：调用者按需恢复。 */
/**
 * @brief 把「已到期但还没启动」的后台 CP 在 fixture 里落定
 *
 * quiesce 只排空**已在飞** job；boot/journal 回放可能留下 journal > CP 的到期状态。
 * 不在这里落定，任何长泵 `timer_scheduler_poll + ftl_service` 的用例都会在测试内部
 * 把 CP 拉起来 → metadata 日志轮转 → reclaim 擦除命令在飞，污染「FC 池回到基线」类判据。
 * 用非 force 的 TIMER 触发走 T1/T2/T3：真到期才起；随后的 quiesce 把 CP 及其
 * 派生出的 md reclaim 一并推进到空闲。
 *
 * @param ftl 活实例
 */
void ftl_ut_settle_background(ftl_ctx_s *ftl)
{
    ftl_ut_quiesce(ftl);

    const cp_trigger_s due = ftl_cp_trigger_build(ftl, CP_REASON_TIMER, false);

    cp_handle(ftl->cp_handle, &due);
    ftl_ut_quiesce(ftl);
}

void ftl_ut_quiesce(ftl_ctx_s *ftl)
{
    const DWORD t0 = platform_now_ms();

    ftl->flags.bg_suspend = 1U;

    for (;;)
    {
        fc_resource_stats_s rs;

        ftl_service_pump();
        /* bg_suspend 只阻止**新**后台任务；这里仍要让已起 job 走完 CPU 阶段，
         * 否则 media teardown 的 assert_clean 会在 CP/MIG 仍 active 时失败。 */
        ftl_service(ftl);

        if (ftl->cp_handle->active != 0)
        {
            const cp_trigger_s trigger = ftl_cp_trigger_build(ftl, CP_REASON_ADVANCE, 0);

            cp_handle(ftl->cp_handle, &trigger);
        }
        if (ftl->mig_handle->job.active != 0)
        {
            mig_handle(ftl->mig_handle, BG_LEVEL_IDLE);
        }
        md_handle(ftl->metadata_dir);
        jrn_handle(ftl->journal, NULL);

        // bg_suspend 只阻止**新**后台任务；本轮的 dispatch 要执行已登记的 flush item
        // （CP 现在只登记 item，不再直接 kick）。dispatch 同时会跑后台策略；临时把
        // credit 清零，保证 quiesce 期间不提交预算任务（must-run 在静止态是幂等空操作）。
        const DWORD credits_saved = ftl->sched.bg.credits;

        ftl->sched.bg.credits = 0U;
        ftl->flags.bg_suspend = 0U;
        ftl_sched_dispatch(ftl);
        ftl->flags.bg_suspend = 1U;
        ftl->sched.bg.credits = credits_saved;

        ftl_service_pump();

        memset(&rs, 0, sizeof(rs));
        fc_resource_stats_get(&rs);

        const BOOL jobs_idle = (ftl->cp_handle->active == 0) && (ftl->mig_handle->job.active == 0) && md_handle_is_idle(ftl->metadata_dir) && jrn_is_idle(ftl->journal) && wb_is_idle(ftl->wbuf) && (ftl->cache_stream.seq.write.count == 0U) && (ftl->cache_stream.seq.write_inflight == 0U) && (ftl->cache->inflight.load == 0U) && (ftl->cache->inflight.flush == 0U) && (ftl->sched.items != NULL) &&
                               (ftl->sched.host_items == 0U) && (ftl->sched.flush_items == 0U) && (ftl->sched.prefetch_items == 0U) && (ftl->sched.credit[FTL_WORK_CLASS_L2P_LOAD] == 0U) && (ftl->sched.credit[FTL_WORK_CLASS_L2P_FLUSH] == 0U);

        if ((rs.sq_used == 0U) && (rs.hsq_used == 0U) && (rs.cq_used == 0U) && (rs.command_used == 0U) && (rs.descriptor_used == 0U) && (rs.buffer_used == 0U) && (ftl_op_req_count() == 0U) && jobs_idle)
        {
            return;
        }

        munit_assert_uint32(platform_now_ms() - t0, <, FTL_UT_IO_TIMEOUT_MS);
    }
}

/* 重新绑定 FTL。ftl_init 会 memset 整个上下文 ⇒ 先把 storage 指针拷出来。 */
static pfr_boot_result_s s_last_boot_result;

const pfr_boot_result_s *ftl_ut_last_boot_result(void)
{
    return &s_last_boot_result;
}

ftl_status_e ftl_ut_try_reboot_from_media(pfr_boot_result_s *out)
{
    ftl_ctx_s *ftl = ftl_ut_live();

    ftl_ut_quiesce(ftl);

    ftl_ut_trace_detail("reboot: quiesced (req=%hu cp_waiting=%d cp_phase=%d)", ftl_op_req_count(), ftl->cp_handle->waiting, ftl->cp_handle->phase);

    /* 模拟 POR：FC 池/CQ 与派发旁表都回到开机状态，杜绝旧命令的迟到完成。 */
    fc_init();
    fc_nand_engine_set_armed(1U); /* fc_init 会复位置位；与 FC UT 的 test_setup 同做法 */
    ftl_op_init();

    static struct FTL_CONTEXT_S *s_route[FTL_DIE_MAX];

    memset(s_route, 0, sizeof(s_route));
    s_route[0] = ftl;
    ftl_op_route_set(s_route);

    const ftl_topo_s       topo  = ftl->topo;
    l2p_cache_s           *cache = ftl->cache;
    const ftl_table_refs_s refs  = ftl->table_refs;
    md_dir_cache_s        *md    = ftl->metadata_dir;
    l2p_jrn_s             *jrn   = ftl->journal;
    cp_handle_s           *cp    = ftl->cp_handle;
    mig_handle_s          *mig   = ftl->mig_handle;
    wb_handle_s           *wb    = ftl->wbuf;
    BYTE                  *rr    = ftl->rr_page_scratch;

    const l2p_owner_fn_t owner_fn  = cache->owner_of_region;
    void                *owner_ctx = cache->owner_ctx;

    ftl_storage_s st;

    memset(&st, 0, sizeof(st));
    st.table_refs      = &refs;
    st.metadata_dir    = md;
    st.journal         = jrn;
    st.cp_handle       = cp;
    st.migrate         = mig;
    st.wbuf            = wb;
    st.rr_page_scratch = rr;

    munit_assert_int(ftl_init(ftl, topo, cache, &st), ==, FTL_OK);
    l2p_init(cache, topo.ch, topo.ce, owner_fn, owner_ctx);

    ftl_ut_trace_detail("reboot: ftl_init ok (cp_waiting=%d cp_phase=%d)", ftl->cp_handle->waiting, ftl->cp_handle->phase);

    pfr_boot_result_s result;

    memset(&result, 0, sizeof(result));
    const ftl_status_e boot_status = pfr_boot_recovery(ftl, &result);

    s_last_boot_result = result;
    if (out != NULL)
    {
        *out = result;
    }

    ftl_ut_trace_detail("reboot: status=%ld type=%d cp_seq=%lu jrn=%lu restored=%d scoped=%d", (long)boot_status, result.boot_type, result.cp_sequence, result.journal_entries, result.blocks_restored, result.scoped);
    return boot_status;
}

void ftl_ut_reboot_from_media(void)
{
    pfr_boot_result_s result;

    munit_assert_int(ftl_ut_try_reboot_from_media(&result), ==, FTL_OK);
}

void ftl_ut_isolated_begin(void)
{
    ftl_ut_media_format();
    fi_reset();
}

void ftl_ut_isolated_end(void)
{
    fi_reset();
    ftl_ut_media_format();
}

void ftl_ut_media_format(void)
{
    ftl_ctx_s   *ftl = ftl_ut_live();
    wb_trigger_s trigger;
    wb_status_s  status;

    ftl_ut_quiesce(ftl);

    memset(&trigger, 0, sizeof(trigger));
    memset(&status, 0, sizeof(status));
    trigger.kind = WBUF_TRIGGER_RESET;

    ftl->flags.bg_suspend = 1U;
    (void)wb_handle(ftl->wbuf, &trigger, &status);

    munit_assert_int(pfr_media_format(ftl, 0), ==, FTL_OK);

    ftl_ut_reboot_from_media();

    ftl_ut_trace_summary("media: formatted, free=%hu op=%hu", ftl->alloc.free_count, ftl->alloc.op_count[0]);
}

static BOOL ftl_ut_io_wait(DWORD timeout_ms)
{
    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD t0  = platform_now_ms();

    while (s_io_done == 0U)
    {
        /* 与生产主循环一致：timer 表补 sched bg credit，CP/Journal/L2P 后台
         * 任务才能推进。漏掉它时长时间的写序列会因 credit 耗尽而停摆。 */
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);

        if ((platform_now_ms() - t0) > timeout_ms)
        {
            return 0;
        }
    }
    return s_io_status == SSD_STATUS_OK;
}

/* host I/O 失败时把「为什么」打出来：只给一个 0 无法区分
 * submit 拒绝 / 等待超时 / 组报错 / deferred error。默认只在 debug 档可见。 */
static void ftl_ut_trace_io_fail(const char *op, DWORD lba, DWORD count, int submit_st, BOOL waited)
{
    ftl_ctx_s *f = ftl_ut_live();

    ftl_ut_trace_detail("host %s FAIL lba=%lu count=%lu submit=%d wait=%d done=%hhu status=%d deferred=%d free=%hu jrn=%lu/%lu wr=%hhu/%hhu flush=%hhu/%hhu",
                        op,
                        lba,
                        count,
                        submit_st,
                        waited,
                        s_io_done,
                        s_io_status,
                        f->flags.deferred_error,
                        f->alloc.free_count,
                        jrn_sequence_get(f->journal),
                        jrn_persisted_seq(f->journal),
                        f->cache_stream.seq.write.count,
                        f->cache_stream.seq.write_inflight,
                        f->cache->inflight.flush,
                        f->cache->inflight.load);
}

BOOL ftl_ut_host_write(DWORD lba, DWORD count, const void *buffer)
{
    ftl_ctx_s   *ftl = ftl_ut_live();
    ftl_io_req_s req;

    memset(&req, 0, sizeof(req));
    s_io_done   = 0U;
    s_io_status = SSD_STATUS_IO_ERROR;

    req.lba           = lba;
    req.buffer        = (void *)buffer;
    req.count         = count;
    req.complete_flag = &s_io_done;
    req.status_out    = &s_io_status;

    const ftl_status_e submit = ftl_submit_write(ftl, &req);

    if (submit != FTL_OK)
    {
        ftl_ut_trace_io_fail("write", lba, count, submit, 0);
        return 0;
    }

    const BOOL ok = ftl_ut_io_wait(FTL_UT_IO_TIMEOUT_MS);

    if (ok == 0)
    {
        ftl_ut_trace_io_fail("write", lba, count, submit, 1);
    }
    return ok;
}

BOOL ftl_ut_host_read(DWORD lba, DWORD count, void *buffer)
{
    ftl_ctx_s   *ftl = ftl_ut_live();
    ftl_io_req_s req;

    memset(&req, 0, sizeof(req));
    s_io_done   = 0U;
    s_io_status = SSD_STATUS_IO_ERROR;

    req.lba           = lba;
    req.buffer        = buffer;
    req.count         = count;
    req.complete_flag = &s_io_done;
    req.status_out    = &s_io_status;

    const ftl_status_e submit = ftl_submit_read(ftl, &req);

    if (submit != FTL_OK)
    {
        ftl_ut_trace_io_fail("read", lba, count, submit, 0);
        return 0;
    }

    const BOOL ok = ftl_ut_io_wait(FTL_UT_IO_TIMEOUT_MS);

    if (ok == 0)
    {
        ftl_ut_trace_io_fail("read", lba, count, submit, 1);
    }
    return ok;
}

BOOL ftl_ut_flush(void)
{
    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD t0  = platform_now_ms();

    ftl_flush_begin(ftl);

    while (!ftl_flush_step(ftl))
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);

        if ((platform_now_ms() - t0) > FTL_UT_IO_TIMEOUT_MS)
        {
            return 0;
        }
    }
    return 1;
}

BOOL ftl_ut_trim(DWORD lba, DWORD count)
{
    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD t0  = platform_now_ms();

    if (ftl_trim_begin(ftl, lba, count) != FTL_OK)
    {
        return 0;
    }

    for (;;)
    {
        const ftl_status_e st = ftl_trim_step(ftl);

        if (st == FTL_OK)
        {
            return 1;
        }
        if (st != FTL_AGAIN)
        {
            return 0;
        }

        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);

        if ((platform_now_ms() - t0) > FTL_UT_IO_TIMEOUT_MS)
        {
            return 0;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * §3c pure 套件资源钩子
 * ══════════════════════════════════════════════════════════════════════ */

/* pure 用例共用 live FTL 的 L2P cache，但套件默认「行是空的」：需要空行构造
 * 三态/CMT 用例。上一条 `test` 命令（尤其 /bench/speed/ftl_rw 写 3×25 MiB）
 * 会留下 64 个 CLEAN 常驻行，跨命令跑 `test` 时就会撞 `idx < 64`。
 * 只作废 CLEAN 行：这一步纯 RAM，不碰介质、不弄脏 Directory。DIRTY 行代表
 * 未落盘映射，不得在这里 flush/作废——flush 会更新 Directory，把「目录脏」
 * 这个后台 CP 触发点泄漏给后面的 negative/pure 用例（`ftl_service` 一旦被泵
 * 就会起 metadata 检查点 → reclaim 命令在飞 → `command_used` 判据假红）。 */
static void ftl_ut_pure_l2p_lines_clear(ftl_ctx_s *ftl)
{
    l2p_cache_s *cache = ftl->cache;

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_line_info_s info;

        l2p_line_get(cache, i, &info);
        if (info.state == L2P_CACHE_CLEAN)
        {
            l2p_line_invalidate(cache, i);
        }
    }
}

static BOOL s_pure_bg_saved;

void *ftl_ut_pure_setup(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_topology_init();

    ftl_ctx_s *ftl = ftl_ut_live();

    s_pure_bg_saved = ftl->flags.bg_suspend;

    /* 与 negative/pure 同口径：先排空在飞 job 并落定 boot 遗留的到期 CP；
     * 随后整个用例期间冻结后台——pure 用例不应被调度器按当时水位自行起的
     * CP/压缩/reclaim 干扰，否则结果取决于上一条命令/上次上电留下的状态。 */
    ftl_ut_settle_background(ftl);
    ftl->flags.bg_suspend = true;

    ftl_ut_ram_init();
    ftl_ut_pure_l2p_lines_clear(ftl);
    fi_reset(); /* 注入状态是进程级的：漏一次复位会静默污染后续用例 */
    return NULL;
}

void ftl_ut_pure_teardown(void *fixture)
{
    (void)fixture;

    ftl_ctx_s *ftl = ftl_ut_live();

    ftl->flags.bg_suspend = s_pure_bg_saved;
    ftl_ut_assert_clean(ftl);
}

/* ══════════════════════════════════════════════════════════════════════
 * §4 用例结束不变量
 * ══════════════════════════════════════════════════════════════════════ */

void ftl_ut_assert_clean(ftl_ctx_s *ftl)
{
    fc_resource_stats_s rs;

    munit_assert_not_null(ftl);
    munit_assert_not_null(ftl->cache);
    munit_assert_not_null(ftl->journal);
    munit_assert_not_null(ftl->metadata_dir);
    munit_assert_not_null(ftl->cp_handle);
    munit_assert_not_null(ftl->mig_handle);
    munit_assert_not_null(ftl->wbuf);

    munit_assert_uint16(pend_slot_count(&ftl->pending), ==, 0U);
    munit_assert_uint16(pend_group_count(&ftl->pending), ==, 0U);

    /* ctx 残留 = 有能力登记却没被派发的命令留在队列里。这里把「谁卡住」一并打出来
     * （只读快照；munit_errorf 会带出这行），否则只有一句 count!=0 无法定位。 */
    if (ftl_op_req_count() != 0U)
    {
        fc_debug_state_s dbg;

        memset(&dbg, 0, sizeof(dbg));
        fc_debug_state_get(&dbg);
        munit_errorf("ftl teardown leaked op req=%hu | sq=%hu hsq=%hu owner=%hhu lun=%hhu/%hhu sched_sq=%hu bound=%hhu | sq_head=%hu st=%hhu chain=%hhu op=%hhu | hsq_head=%hu chain=%hhu op=%hhu | fill=%lu/%lu",
                     ftl_op_req_count(),
                     dbg.sq_used,
                     dbg.hsq_used,
                     dbg.chain_owner,
                     dbg.lun_used,
                     dbg.lun_phase,
                     dbg.sched_sq_count,
                     dbg.sched_sq_bound,
                     dbg.sq_head_id,
                     dbg.sq_head_state,
                     dbg.sq_head_chain,
                     dbg.sq_head_op,
                     dbg.hsq_head_id,
                     dbg.hsq_head_chain,
                     dbg.hsq_head_op,
                     dbg.fill_calls,
                     dbg.fill_pops);
    }

    munit_assert_uint16(ftl_op_req_count(), ==, 0U);

    memset(&rs, 0, sizeof(rs));
    fc_resource_stats_get(&rs);
    munit_assert_uint32(rs.sq_used, ==, 0U);
    munit_assert_uint32(rs.hsq_used, ==, 0U);
    munit_assert_uint32(rs.cq_used, ==, 0U);
    munit_assert_uint32(rs.command_used, ==, 0U);
    munit_assert_uint32(rs.descriptor_used, ==, 0U);
    munit_assert_uint32(rs.buffer_used, ==, 0U);

    munit_assert_false(ftl->mig_handle->job.active);
    munit_assert_false(ftl->mig_handle->job.waiting);
    munit_assert_false(ftl->cp_handle->active);
    munit_assert_false(ftl->cp_handle->waiting);
    munit_assert_true(md_handle_is_idle(ftl->metadata_dir));
    munit_assert_true(jrn_is_idle(ftl->journal));
    munit_assert_true(wb_is_idle(ftl->wbuf));
    munit_assert_uint32(ftl->cache->inflight.load, ==, 0U);
    munit_assert_uint32(ftl->cache->inflight.flush, ==, 0U);
    /* scheduler 账本同样必须归零：item 泄漏/credit 未还是静默容量流失。 */
    munit_assert_uint32(ftl->sched.host_items, ==, 0U);
    munit_assert_uint32(ftl->sched.flush_items, ==, 0U);
    munit_assert_uint32(ftl->sched.prefetch_items, ==, 0U);
    munit_assert_uint32(ftl->sched.credit[FTL_WORK_CLASS_L2P_LOAD], ==, 0U);
    munit_assert_uint32(ftl->sched.credit[FTL_WORK_CLASS_L2P_FLUSH], ==, 0U);
    /* 写序列的页表 / 在飞成员 / dirty CMT 是跨用例污染的主要来源：teardown 必须
     * 证明它们已排空，否则下一用例可能读到旧页或被序列门控拒绝。 */
    munit_assert_uint32(ftl->cache_stream.seq.write.count, ==, 0U);
    munit_assert_uint32(ftl->cache_stream.seq.write_inflight, ==, 0U);
    munit_assert_uint32(ftl->cache->stats.cmt_dirty, ==, 0U);
}
