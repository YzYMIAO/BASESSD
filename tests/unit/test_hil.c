/*
 * test_hil.c — /hil/：HIL 板端 UT（真 NAND/FTL，无 mock）
 *
 * 覆盖 HIL 能被固件直接驱动的部分：
 *   · controller 状态机 / init（缺槽、拓扑不符）/ LBA 范围 / submit 边界
 *   · 经 controller 的真实 READ / WRITE / FLUSH / TRIM（破坏性）
 *
 * ⚠ 运行前拔掉 USB。本套件**不 format**：写读/flush/trim 固定用容量尾部的一个
 *   8-LBA 窗口（不碰文件系统常用区）；重复运行只是覆盖同一窗口。
 * ⚠ 单 die 无法构造多 run 拆分 / 跨实例部分失败（RISK-CTRL-01）；USB 槽状态机
 *   （flush 槽 / read BUSY / 0x91 / 相同写匹配）由 PC 主机侧矩阵覆盖，
 *   见 docs/design/known_risks.md RISK-USB-09..13。
 * ⚠ 本套件不注入、不 mock：所有路径走生产 FTL/NAND；只复用主 boot 建好的实例。
 */

#include "test_hil.h"

#include "cli.h" /* g_ctrl（main.c 定义） */
#include "controller.h"
#include "fc_interface.h" /* fc_get_resource_stats（teardown 排空判据） */
#include "ftl.h"
#include "ftl_op.h" /* ftl_op_req_count */
#include "metadata_job.h"
#include "munit.h"
#include "platform.h"
#include "ssd_compiler.h"
#include "timer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HIL_IO_LBAS       (4U)
#define HIL_LBA_WINDOW    (8U)
#define HIL_IO_TIMEOUT_MS (5000U)

static BYTE       s_hil_wr[HIL_IO_LBAS * FTL_IU_BYTES];
static BYTE       s_hil_rd[HIL_IO_LBAS * FTL_IU_BYTES];
static const BYTE s_hil_zero[HIL_IO_LBAS * FTL_IU_BYTES];
static BYTE       s_hil_edge_buf[HIL_IO_LBAS * FTL_IU_BYTES];

/* 拓扑不符用例的假实例：只用于 ssd_controller_init 的 topo 校验，不碰硬件。
 * 放 AXI_NOCACHE：全 UT 镜像（FC+FTL+HIL）下 AXI_RAM 会被测试夹具占满；
 * 该结构约 9 KB 且 CPU-only，不需要 D-Cache。 */
static ftl_ctx_s s_hil_bad_ftl NONCACHEABLE_BUFFER;

/* HIL 证据输出：与 /ftl/、/fc-nand/ 的 `[EV]` 同形，复用 munit 的 --log-visible。
 * HIL 构建不链接 fc/ftl 的测试夹具，故这里保留最小本地实现。默认档（error）
 * 一个字节都不输出，报告格式不变。 */
static BOOL s_hil_trace_line_started;

static void hil_trace_begin(void)
{
    /* 只复位状态；第一条可见证据负责给 munit 的前缀行收尾。 */
    s_hil_trace_line_started = 0U;
}

static void hil_trace_emit(MunitLogLevel level, const char *tag, const char *format, va_list ap)
{
    if (!munit_log_visible(level))
    {
        return;
    }

    if (!s_hil_trace_line_started)
    {
        (void)fputc('\n', MUNIT_OUTPUT_FILE);
        s_hil_trace_line_started = 1U;
    }

    (void)fputs(tag, MUNIT_OUTPUT_FILE);
    (void)vfprintf(MUNIT_OUTPUT_FILE, format, ap);
    (void)fputc('\n', MUNIT_OUTPUT_FILE);
    (void)fflush(MUNIT_OUTPUT_FILE);
}

MUNIT_PRINTF(1, 2)
static void hil_trace_summary(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    hil_trace_emit(MUNIT_LOG_INFO, "[EV] ", format, ap);
    va_end(ap);
}

MUNIT_PRINTF(1, 2)
static void hil_trace_detail(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    hil_trace_emit(MUNIT_LOG_DEBUG, "[EV]   ", format, ap);
    va_end(ap);
}

/* ══════════════════════════════════════════════════════════════════
 * 夹具
 * ══════════════════════════════════════════════════════════════════ */

/* 容量尾部、8 对齐的 LBA 窗口；避开文件系统常用区。 */
static DWORD hil_lba_base(void)
{
    const DWORD max = ssd_controller_host_lba_max(&g_ctrl);

    munit_assert_uint32(max, >, HIL_LBA_WINDOW);
    return ((max - HIL_LBA_WINDOW) & ~(HIL_LBA_WINDOW - 1U));
}

/* 泵主循环的 HIL 相关部分，直到 request 完成或超时。 */
static BOOL hil_wait(const ssd_req_s *req, DWORD timeout_ms)
{
    const DWORD t0 = platform_now_ms();

    while (ssd_controller_is_complete(req) == 0U)
    {
        ftl_service_pump();

        for (BYTE die = 0U; die < DIE_MAX; die++)
        {
            ftl_service(g_ctrl.ftl[die]);
        }
        ssd_controller_service(&g_ctrl);

        /* 与生产主循环一致：timer 表补 bg_scheduler credit，CP/Journal/WL 才能推进；
         * 漏掉它时长写/flush 等待会因 credit 耗尽而超时。 */
        timer_scheduler_poll();

        if ((platform_now_ms() - t0) > timeout_ms)
        {
            return false;
        }
    }
    return true;
}

/* FTL 侧是否已无在飞 job。FC 池/op 计数归零不等于 FTL 空闲：CP/MIG/Journal/
 * wbuf/cache 仍可能持有在飞状态，必须一起判。 */
static BOOL hil_ftl_idle(const ftl_ctx_s *ftl)
{
    return (ftl->cp_handle->active == 0U) && (ftl->cp_handle->waiting == 0U) && (ftl->mig_handle->job.active == 0U) && (ftl->mig_handle->job.waiting == 0U) && md_handle_is_idle(ftl->metadata_dir) && jrn_is_idle(ftl->journal) && wb_is_idle(ftl->wbuf) && (ftl->cache_stream.seq.wr.count == 0U) && (ftl->cache_stream.seq.wr_inflight == 0U) && (ftl->cache->inflight.load == 0U) && (ftl->cache->inflight.flush == 0U);
}

/* 排空 FTL/FC 在飞资源；bg_suspend 恢复原值（与 /ftl/media/ 同口径）。
 * 超时先恢复 bg_suspend 再让 munit 失败：断言 longjmp 会跳过恢复，污染后续用例。 */
static void hil_quiesce(void)
{
    ftl_ctx_s  *ftl       = g_ctrl.ftl[0];
    const BOOL  saved     = ftl->flags.bg_suspend;
    const DWORD t0        = platform_now_ms();
    BOOL        timed_out = false;

    ftl->flags.bg_suspend = 1U;

    for (;;)
    {
        fc_resource_stats_s rs;

        ftl_service_pump();

        for (BYTE die = 0U; die < DIE_MAX; die++)
        {
            ftl_service(g_ctrl.ftl[die]);
        }

        /* 与 ftl_ut_quiesce 同口径：主动推进 CP/MIG/metadata/Journal，不能只等
         * ftl_service；否则 bg_suspend=1 时这些 job 可能永远不归零。 */
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

        ssd_controller_service(&g_ctrl);
        ftl_service_pump();

        /* 与 hil_wait 同口径：teardown 也要让 timer 表推进后台任务，否则 CP/Journal
         * 的在飞 job 不会因池计数归零而结束。 */
        timer_scheduler_poll();

        memset(&rs, 0, sizeof(rs));
        (void)fc_get_resource_stats(&rs);

        if ((rs.sq_used == 0U) && (rs.hsq_used == 0U) && (rs.cq_used == 0U) && (rs.command_used == 0U) && (rs.descriptor_used == 0U) && (rs.buffer_used == 0U) && (ftl_op_req_count() == 0U) && (g_ctrl.flush_req == NULL) && (g_ctrl.trim_req == NULL) && hil_ftl_idle(ftl))
        {
            break;
        }
        if ((platform_now_ms() - t0) > HIL_IO_TIMEOUT_MS)
        {
            timed_out = true;
            break;
        }
    }

    if (timed_out)
    {
        hil_trace_detail("quiesce timeout: cp=%d/%d mig=%d/%d md=%d jrn=%d wb=%d wr=%hhu/%hhu load=%hhu flush=%hhu op=%hu req=%d/%d",
                         (int)ftl->cp_handle->active,
                         (int)ftl->cp_handle->waiting,
                         (int)ftl->mig_handle->job.active,
                         (int)ftl->mig_handle->job.waiting,
                         (int)md_handle_is_idle(ftl->metadata_dir),
                         (int)jrn_is_idle(ftl->journal),
                         (int)wb_is_idle(ftl->wbuf),
                         ftl->cache_stream.seq.wr.count,
                         ftl->cache_stream.seq.wr_inflight,
                         ftl->cache->inflight.load,
                         ftl->cache->inflight.flush,
                         ftl_op_req_count(),
                         (g_ctrl.flush_req != NULL),
                         (g_ctrl.trim_req != NULL));
    }

    ftl->flags.bg_suspend = saved;
    munit_assert_false(timed_out);
}

static void *hil_setup(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    munit_assert_int(g_ctrl.state, ==, SSD_CONTROLLER_READY);
    munit_assert_not_null(g_ctrl.ftl[0]);
    hil_trace_begin();
    return NULL;
}

static void hil_teardown(void *fixture)
{
    (void)fixture;
    hil_quiesce();
}

static void hil_req_init(ssd_req_s *req, ssd_req_opcode_e opcode, QWORD lba, DWORD count, void *buffer, size_t buffer_size)
{
    memset(req, 0, sizeof(*req));
    req->opcode      = opcode;
    req->start_lba   = lba;
    req->block_count = count;
    req->buffer      = buffer;
    req->buffer_size = buffer_size;
}

/* ══════════════════════════════════════════════════════════════════
 * Controller 状态 / 初始化 / 校验（不碰 NAND）
 * ══════════════════════════════════════════════════════════════════ */

/* state_machine — 状态机合法/非法转换与 NULL 守卫。
 *
 * 覆盖：RESET→INITIALIZING→READY→FAILED；RESET→READY 与 FAILED→READY 被忽略；
 *       state_transition(NULL, …) 不崩。
 * 判据：非法转换必须保持现态、FAILED 为终态；否则失效设备可能被重新放行。 */

static MunitResult test_hil_state_machine(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_ctl_s c;

    memset(&c, 0, sizeof(c));

    /* RESET 只接受 INITIALIZING：直接 READY 被忽略 */
    ssd_controller_state_transition(&c, SSD_CONTROLLER_READY);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_RESET);

    ssd_controller_state_transition(&c, SSD_CONTROLLER_INITIALIZING);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_INITIALIZING);

    ssd_controller_state_transition(&c, SSD_CONTROLLER_READY);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_READY);

    ssd_controller_state_transition(&c, SSD_CONTROLLER_FAILED);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_FAILED);

    /* FAILED 是终态 */
    ssd_controller_state_transition(&c, SSD_CONTROLLER_READY);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_FAILED);

    /* NULL 守卫：不得解引用，也不得改变任何状态。 */
    ssd_controller_state_transition(NULL, SSD_CONTROLLER_READY);

    hil_trace_detail("state: RESET->READY ignored; INITIALIZING->READY; READY->FAILED; FAILED->READY ignored; NULL no-op");
    hil_trace_summary("hil state: legal transitions applied; illegal/FAILED ignored");

    return MUNIT_OK;
}

/* init_ready — 正常 init 的 READY 与容量基线。
 *
 * 覆盖：合法 FTL 槽表 → READY；host_lba_max 初值 = 编译期 FTL_LBA_MAX。
 * 判据：init 必须绑定全部 present die 并给出 host 可见上限；否则 submit 路由/容量口径错。 */

static MunitResult test_hil_init_ready(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_ctl_s c;

    memset(&c, 0, sizeof(c));
    munit_assert_int(ssd_controller_init(&c, g_ctrl.ftl), ==, SSD_STATUS_OK);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_READY);
    munit_assert_uint32(c.host_lba_max, ==, (DWORD)FTL_LBA_MAX);

    hil_trace_detail("init: state=%d host_lba_max=%lu (compile-time FTL_LBA_MAX)", c.state, c.host_lba_max);
    hil_trace_summary("hil init: READY, lba_max=%lu", c.host_lba_max);

    return MUNIT_OK;
}

/* init_missing_slot — 缺失 FTL 槽必须失败。
 *
 * 覆盖：route[0]=NULL → INVALID_ARGUMENT 且 state=FAILED。
 * 判据：把缺失 die 当可用会路由到 NULL 实例（UAF/崩溃）。 */

static MunitResult test_hil_init_missing_slot(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_ctl_s  c;
    ftl_ctx_s *route[FTL_DIE_MAX];

    munit_assert_uint32(DIE_MAX, >, 0U);

    memset(&c, 0, sizeof(c));
    memset(route, 0, sizeof(route));

    munit_assert_int(ssd_controller_init(&c, route), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_FAILED);

    hil_trace_detail("init missing slot: route[0]=NULL -> state=%d", c.state);
    hil_trace_summary("hil init: missing FTL slot rejected, state=FAILED");

    return MUNIT_OK;
}

/* init_topo_mismatch — 槽位拓扑与 die 分解不符必须失败。
 *
 * 覆盖：slot.topo.ch=0xFF → INVALID_ARGUMENT 且 state=FAILED。
 * 判据：拓扑不符会让 LBA 路由到错误 chip；必须拒绝而非继续 READY。 */

static MunitResult test_hil_init_topo_mismatch(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_ctl_s  c;
    ftl_ctx_s *route[FTL_DIE_MAX];

    munit_assert_uint32(DIE_MAX, >, 0U);

    memset(&s_hil_bad_ftl, 0, sizeof(s_hil_bad_ftl));
    s_hil_bad_ftl.topo.ch = 0xFFU; /* 与 die_2_ch(0) 必然不符 */

    memset(&c, 0, sizeof(c));
    memset(route, 0, sizeof(route));
    route[0] = &s_hil_bad_ftl;

    munit_assert_int(ssd_controller_init(&c, route), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_FAILED);

    hil_trace_detail("init topo mismatch: slot ch=0xFF vs die_2_ch(0) -> state=%d", c.state);
    hil_trace_summary("hil init: topo mismatch rejected, state=FAILED");

    return MUNIT_OK;
}

/* host_queue — host tag 表分配/释放/owner。
 *
 * 覆盖：填满 8 entry（tag=下标）→ alloc 返回 NULL；释放 tag0 → CLI 可再分配。
 * 判据：tag 必须唯一且 free 后 entry 归零；否则 USB/CLI 会复用旧请求。 */

static MunitResult test_hil_host_queue(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_host_queue_s  q;
    ssd_host_entry_s *entries[SSD_HOST_QUEUE_DEPTH];

    ssd_host_queue_init(&q);

    for (BYTE i = 0U; i < SSD_HOST_QUEUE_DEPTH; i++)
    {
        entries[i] = ssd_host_queue_alloc(&q, SSD_HOST_OWNER_USB);
        munit_assert_not_null(entries[i]);
        munit_assert_uint8(entries[i]->tag, ==, i);
        munit_assert_uint8(entries[i]->owner, ==, SSD_HOST_OWNER_USB);
    }
    munit_assert_uint32(q.used, ==, SSD_HOST_QUEUE_DEPTH);
    munit_assert_ptr_null(ssd_host_queue_alloc(&q, SSD_HOST_OWNER_CLI));

    ssd_host_queue_free(&q, entries[0]);
    ssd_host_entry_s *reused = ssd_host_queue_alloc(&q, SSD_HOST_OWNER_CLI);

    munit_assert_not_null(reused);
    munit_assert_uint8(reused->tag, ==, 0U);
    munit_assert_uint8(reused->owner, ==, SSD_HOST_OWNER_CLI);
    munit_assert_uint32(q.used, ==, SSD_HOST_QUEUE_DEPTH);

    /* free 会 memset entry；owner/tag 必须在 free 前取快照。 */
    const BYTE reused_tag   = reused->tag;
    const BYTE reused_owner = reused->owner;

    for (BYTE i = 0U; i < SSD_HOST_QUEUE_DEPTH; i++)
    {
        ssd_host_queue_free(&q, entries[i]);
    }
    munit_assert_uint32(q.used, ==, 0U);
    munit_assert_uint8(entries[1]->in_use, ==, 0U);

    hil_trace_detail("host queue: depth=%u full; tag0 freed -> CLI alloc tag=%hhu owner=%hhu; free all -> used=%hhu", SSD_HOST_QUEUE_DEPTH, reused_tag, reused_owner, q.used);
    hil_trace_summary("hil host queue: 8 tags, full reject, free/realloc/free-all ok");

    return MUNIT_OK;
}

/* lba_range_ok — host LBA 半开区间边界。
 *
 * 覆盖：count=0、lba=max、[max-1,2) 拒绝；[0,1)、[max-1,1) 接受。
 * 判据：越界请求必须由 HIL 入口拒绝，不能依赖前端。 */

static MunitResult test_hil_lba_range_ok(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const DWORD max = ssd_controller_host_lba_max(&g_ctrl);

    munit_assert_uint32(max, >, 0U);
    munit_assert_false(ssd_controller_lba_range_ok(&g_ctrl, (QWORD)0U, 0U));
    munit_assert_false(ssd_controller_lba_range_ok(&g_ctrl, (QWORD)max, 1U));

    if (max >= 2U)
    {
        munit_assert_true(ssd_controller_lba_range_ok(&g_ctrl, (QWORD)0U, 1U));
        munit_assert_true(ssd_controller_lba_range_ok(&g_ctrl, (QWORD)(max - 1U), 1U));
        munit_assert_false(ssd_controller_lba_range_ok(&g_ctrl, (QWORD)(max - 1U), 2U));
    }

    hil_trace_detail("lba range: max=%lu; count=0 / lba=max / [max-1,2) rejected; [0,1) and [max-1,1) accepted", max);
    hil_trace_summary("hil lba range: max=%lu, half-open interval checked at both ends", max);

    return MUNIT_OK;
}

/* submit_not_ready — 非 READY 状态与 NULL 入参拒绝提交。
 *
 * 覆盖：RESET / FAILED 下 submit READ，以及 controller=NULL / request=NULL →
 *       INVALID_ARGUMENT。
 * 判据：未就绪控制器或空入参不能进入拆分/分配；否则会在 FTL 内部才崩。 */

static MunitResult test_hil_submit_not_ready(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_ctl_s c;
    ssd_req_s req;
    BYTE      buf[FTL_IU_BYTES];

    memset(&c, 0, sizeof(c));
    hil_req_init(&req, SSD_REQUEST_READ, 0U, 1U, buf, sizeof(buf));

    munit_assert_int(ssd_controller_submit(&c, &req), ==, SSD_STATUS_INVALID_ARGUMENT);

    ssd_controller_state_transition(&c, SSD_CONTROLLER_INITIALIZING);
    ssd_controller_state_transition(&c, SSD_CONTROLLER_FAILED);
    munit_assert_int(ssd_controller_submit(&c, &req), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_int(ssd_controller_submit(NULL, &req), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_int(ssd_controller_submit(&g_ctrl, NULL), ==, SSD_STATUS_INVALID_ARGUMENT);

    hil_trace_detail("submit not ready: RESET/FAILED -> INVALID_ARGUMENT; NULL controller/request -> INVALID_ARGUMENT");
    hil_trace_summary("hil submit: non-READY / NULL inputs rejected");

    return MUNIT_OK;
}

/* submit_edges — HIL 入口非法参数分类。
 *
 * 覆盖：buffer 不足、count=0、越界 LBA → INVALID_ARGUMENT；未知 opcode → NOT_SUPPORTED。
 * 判据：必须在分配/拆分前拒绝并置 complete_flag，避免调用者无限等待。 */

static MunitResult test_hil_submit_edges(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_req_s req;
    BYTE     *buf = s_hil_edge_buf;

    /* buffer_size 不足：在 split 之前拒绝 */
    hil_req_init(&req, SSD_REQUEST_WRITE, 0U, HIL_IO_LBAS, buf, sizeof(s_hil_edge_buf) - 1U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_uint8(req.complete_flag, ==, 1U);
    munit_assert_int(req.completion_status, ==, SSD_STATUS_INVALID_ARGUMENT);

    /* 0 长度 */
    hil_req_init(&req, SSD_REQUEST_READ, 0U, 0U, buf, sizeof(s_hil_edge_buf));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_uint8(req.complete_flag, ==, 1U);

    /* 越界 LBA：HIL 入口统一拒绝（不再依赖前端） */
    hil_req_init(&req, SSD_REQUEST_READ, (QWORD)ssd_controller_host_lba_max(&g_ctrl), 1U, buf, sizeof(s_hil_edge_buf));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_uint8(req.complete_flag, ==, 1U);

    /* 未知 opcode */
    hil_req_init(&req, (ssd_req_opcode_e)0x7FU, 0U, 1U, buf, sizeof(s_hil_edge_buf));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_NOT_SUPPORTED);
    munit_assert_uint8(req.complete_flag, ==, 1U);
    munit_assert_int(req.completion_status, ==, SSD_STATUS_NOT_SUPPORTED);

    hil_trace_detail("submit edges: short buffer/zero count/oob lba -> INVALID_ARGUMENT; opcode=0x7F -> NOT_SUPPORTED; complete_flag=1");
    hil_trace_summary("hil submit: 4 invalid-input classes rejected at HIL entry");

    return MUNIT_OK;
}

/* is_complete — 完成轮询的 NULL 与 flag 语义。
 *
 * 覆盖：request=NULL → 1（视为已完成）；complete_flag=0 → 0；置 1 → 1。
 * 判据：NULL 若返回 0，调用者会永久等待一个不存在的请求；flag 必须直读。 */

static MunitResult test_hil_is_complete(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_req_s req;

    memset(&req, 0, sizeof(req));
    munit_assert_uint8(ssd_controller_is_complete(NULL), ==, 1U);
    munit_assert_uint8(ssd_controller_is_complete(&req), ==, 0U);

    req.complete_flag = 1U;
    munit_assert_uint8(ssd_controller_is_complete(&req), ==, 1U);

    hil_trace_detail("is_complete: NULL -> 1; flag 0 -> 0; flag 1 -> 1");
    hil_trace_summary("hil complete: NULL treated done, flag read directly");

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 真实数据面（破坏性）
 * ══════════════════════════════════════════════════════════════════ */

/* write_read_roundtrip — 经 controller 的真实写读闭环。
 *
 * 覆盖：尾窗 4 LBA 写 → 不 flush 立即读回逐字节一致。
 * 判据：早回执/读覆盖路径必须返回新数据；sub_remaining 归零证明组完成一次。 */

static MunitResult test_hil_write_read_roundtrip(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_req_s   req;
    const DWORD lba = hil_lba_base();

    for (DWORD i = 0U; i < (DWORD)sizeof(s_hil_wr); i++)
    {
        s_hil_wr[i] = (BYTE)((i * 13U) + 0x5AU);
    }

    hil_req_init(&req, SSD_REQUEST_WRITE, (QWORD)lba, HIL_IO_LBAS, s_hil_wr, sizeof(s_hil_wr));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);
    munit_assert_uint32(req.sub_remaining, ==, 0U);

    memset(s_hil_rd, 0, sizeof(s_hil_rd));
    hil_req_init(&req, SSD_REQUEST_READ, (QWORD)lba, HIL_IO_LBAS, s_hil_rd, sizeof(s_hil_rd));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);
    munit_assert_memory_equal(sizeof(s_hil_wr), s_hil_wr, s_hil_rd);

    hil_trace_detail("write/read: lba=%lu count=%u bytes=%lu sub_remaining=%lu status=%d; memory equal", lba, HIL_IO_LBAS, (DWORD)sizeof(s_hil_wr), req.sub_remaining, req.completion_status);
    hil_trace_summary("hil io: write->read byte-equal lba=%lu count=%u", lba, HIL_IO_LBAS);

    return MUNIT_OK;
}

/* write_flush_readback — 写→FLUSH→读的持久化闭环。
 *
 * 覆盖：尾窗 4 LBA 写 → FLUSH 完成并清 flush_req → 读回逐字节一致。
 * 判据：FLUSH 必须把 cache/L2P 的写落盘；只写不 flush 的读回成功不能证明 flush 语义。 */

static MunitResult test_hil_write_flush_readback(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_req_s   req;
    const DWORD lba = hil_lba_base();

    for (DWORD i = 0U; i < (DWORD)sizeof(s_hil_wr); i++)
    {
        s_hil_wr[i] = (BYTE)((i * 29U) + 0xA5U);
    }

    hil_req_init(&req, SSD_REQUEST_WRITE, (QWORD)lba, HIL_IO_LBAS, s_hil_wr, sizeof(s_hil_wr));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);

    hil_req_init(&req, SSD_REQUEST_FLUSH, 0U, 0U, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);
    munit_assert_ptr_null(g_ctrl.flush_req);

    memset(s_hil_rd, 0, sizeof(s_hil_rd));
    hil_req_init(&req, SSD_REQUEST_READ, (QWORD)lba, HIL_IO_LBAS, s_hil_rd, sizeof(s_hil_rd));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);
    munit_assert_memory_equal(sizeof(s_hil_wr), s_hil_wr, s_hil_rd);

    hil_trace_detail("write/flush/read: lba=%lu count=%u bytes=%lu; flush_req cleared; memory equal", lba, HIL_IO_LBAS, (DWORD)sizeof(s_hil_wr));
    hil_trace_summary("hil flush persist: lba=%lu count=%u byte-equal", lba, HIL_IO_LBAS);

    return MUNIT_OK;
}

/* flush_job — 异步 FLUSH 完成与回执清理。
 *
 * 覆盖：submit FLUSH → 泵到 complete_flag=1 → status=OK，flush_req 清空。
 * 判据：FLUSH 必须在主循环异步完成，不能在 host 回调里自旋。 */

static MunitResult test_hil_flush_job(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_req_s req;

    hil_req_init(&req, SSD_REQUEST_FLUSH, 0U, 0U, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);
    munit_assert_ptr_null(g_ctrl.flush_req);

    hil_trace_detail("flush: completion_status=%d, controller flush_req cleared", req.completion_status);
    hil_trace_summary("hil flush: async job completed and cleared");

    return MUNIT_OK;
}

/* flush_busy — FLUSH 单飞行背压。
 *
 * 覆盖：第一条在飞时第二条 submit → BUSY，complete_flag=1；第一条最终 OK。
 * 判据：重复 FLUSH 不能覆盖在飞请求，否则完成回执写错对象。 */

static MunitResult test_hil_flush_busy(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_req_s req1;
    ssd_req_s req2;

    hil_req_init(&req1, SSD_REQUEST_FLUSH, 0U, 0U, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req1), ==, SSD_STATUS_OK);

    /* 第一条还没泵完：FLUSH 单飞行必须拒绝第二条 */
    hil_req_init(&req2, SSD_REQUEST_FLUSH, 0U, 0U, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req2), ==, SSD_STATUS_BUSY);
    munit_assert_uint8(req2.complete_flag, ==, 1U);
    munit_assert_int(req2.completion_status, ==, SSD_STATUS_BUSY);

    munit_assert_true(hil_wait(&req1, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req1.completion_status, ==, SSD_STATUS_OK);

    hil_trace_detail("flush busy: req1 accepted; req2 rejected BUSY complete_flag=1; req1 finished OK");
    hil_trace_summary("hil flush: single-flight backpressure held");

    return MUNIT_OK;
}

/* trim_job — 经 controller 的真实 TRIM。
 *
 * 覆盖：写 4 LBA → TRIM → trim_req 清空，回读全零。
 * 判据：TRIM 必须让旧映射不可见；否则已删数据复活。 */

static MunitResult test_hil_trim_job(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_req_s   req;
    const DWORD lba = hil_lba_base();

    for (DWORD i = 0U; i < (DWORD)sizeof(s_hil_wr); i++)
    {
        s_hil_wr[i] = (BYTE)((i * 7U) + 0x3CU);
    }

    hil_req_init(&req, SSD_REQUEST_WRITE, (QWORD)lba, HIL_IO_LBAS, s_hil_wr, sizeof(s_hil_wr));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);

    hil_req_init(&req, SSD_REQUEST_TRIM, (QWORD)lba, HIL_IO_LBAS, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);
    munit_assert_ptr_null(g_ctrl.trim_req);

    memset(s_hil_rd, 0xFF, sizeof(s_hil_rd));
    hil_req_init(&req, SSD_REQUEST_READ, (QWORD)lba, HIL_IO_LBAS, s_hil_rd, sizeof(s_hil_rd));
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);
    munit_assert_memory_equal(sizeof(s_hil_zero), s_hil_zero, s_hil_rd);

    hil_trace_detail("trim: lba=%lu count=%u status=%d trim_req cleared; readback all zero", lba, HIL_IO_LBAS, req.completion_status);
    hil_trace_summary("hil trim: lba=%lu count=%u readback zero", lba, HIL_IO_LBAS);

    return MUNIT_OK;
}

/* trim_busy — TRIM 单飞行背压。
 *
 * 覆盖：第一条 TRIM 在飞时第二条 submit → BUSY，complete_flag=1；第一条最终 OK。
 * 判据：TRIM 的 trim_lba/trim_started 是单实例状态；重复提交必须拒绝，不能覆盖范围。 */

static MunitResult test_hil_trim_busy(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_req_s   req1;
    ssd_req_s   req2;
    const DWORD lba = hil_lba_base();

    hil_req_init(&req1, SSD_REQUEST_TRIM, (QWORD)lba, HIL_IO_LBAS, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req1), ==, SSD_STATUS_OK);

    hil_req_init(&req2, SSD_REQUEST_TRIM, (QWORD)(lba + HIL_IO_LBAS), HIL_IO_LBAS, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req2), ==, SSD_STATUS_BUSY);
    munit_assert_uint8(req2.complete_flag, ==, 1U);
    munit_assert_int(req2.completion_status, ==, SSD_STATUS_BUSY);

    munit_assert_true(hil_wait(&req1, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req1.completion_status, ==, SSD_STATUS_OK);
    munit_assert_ptr_null(g_ctrl.trim_req);

    hil_trace_detail("trim busy: req1 accepted; req2 rejected BUSY complete_flag=1; req1 finished OK");
    hil_trace_summary("hil trim: single-flight backpressure held");

    return MUNIT_OK;
}

/* ══ 子套件 /hil/ ══ */

MunitTest g_hil_tests[] = {
    {       (char *)"state_machine",        test_hil_state_machine, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"init_ready",           test_hil_init_ready, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"init_missing_slot",    test_hil_init_missing_slot, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"init_topo_mismatch",   test_hil_init_topo_mismatch, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"host_queue",           test_hil_host_queue, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        (char *)"lba_range_ok",         test_hil_lba_range_ok, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"submit_not_ready",     test_hil_submit_not_ready, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        (char *)"submit_edges",         test_hil_submit_edges, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"is_complete",          test_hil_is_complete, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"write_read_roundtrip", test_hil_write_read_roundtrip, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"write_flush_readback", test_hil_write_flush_readback, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"flush_job",            test_hil_flush_job, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"flush_busy",           test_hil_flush_busy, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {            (char *)"trim_job",             test_hil_trim_job, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"trim_busy",            test_hil_trim_busy, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                          NULL,                          NULL,      NULL,         NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
