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
#include "munit.h"
#include "platform.h"

#include <string.h>

#define HIL_IO_LBAS       (4U)
#define HIL_LBA_WINDOW    (8U)
#define HIL_IO_TIMEOUT_MS (5000U)

static BYTE       s_hil_wr[HIL_IO_LBAS * FTL_IU_BYTES];
static BYTE       s_hil_rd[HIL_IO_LBAS * FTL_IU_BYTES];
static const BYTE s_hil_zero[HIL_IO_LBAS * FTL_IU_BYTES];
static BYTE       s_hil_edge_buf[HIL_IO_LBAS * FTL_IU_BYTES];

/* 拓扑不符用例的假实例：只用于 ssd_controller_init 的 topo 校验，不碰硬件。 */
static ftl_ctx_t s_hil_bad_ftl;

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
static bool hil_wait(const ssd_request_t *req, DWORD timeout_ms)
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

        if ((platform_now_ms() - t0) > timeout_ms)
        {
            return false;
        }
    }
    return true;
}

/* 排空 FTL/FC 在飞资源；bg_suspend 恢复原值（与 /ftl/media/ 同口径）。 */
static void hil_quiesce(void)
{
    ftl_ctx_t  *ftl   = g_ctrl.ftl[0];
    const bool  saved = ftl->flags.bg_suspend;
    const DWORD t0    = platform_now_ms();

    ftl->flags.bg_suspend = 1U;

    for (;;)
    {
        fc_resource_stats_t rs;

        ftl_service_pump();
        for (BYTE die = 0U; die < DIE_MAX; die++)
        {
            ftl_service(g_ctrl.ftl[die]);
        }
        ssd_controller_service(&g_ctrl);

        memset(&rs, 0, sizeof(rs));
        (void)fc_get_resource_stats(&rs);

        if ((rs.sq_used == 0U) && (rs.hsq_used == 0U) && (rs.cq_used == 0U) && (rs.command_used == 0U) && (rs.descriptor_used == 0U) && (rs.buffer_used == 0U) && (ftl_op_req_count() == 0U))
        {
            break;
        }
        munit_assert_uint32(platform_now_ms() - t0, <, HIL_IO_TIMEOUT_MS);
    }

    ftl->flags.bg_suspend = saved;
}

static void *hil_setup(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    munit_assert_int(g_ctrl.state, ==, SSD_CONTROLLER_READY);
    munit_assert_not_null(g_ctrl.ftl[0]);
    return NULL;
}

static void hil_teardown(void *fixture)
{
    (void)fixture;
    hil_quiesce();
}

static void hil_req_init(ssd_request_t *req, ssd_req_opcode_e opcode, QWORD lba, DWORD count, void *buffer, size_t buffer_size)
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

static MunitResult test_hil_state_machine(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_controller_t c;

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

    return MUNIT_OK;
}

static MunitResult test_hil_init_ready(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_controller_t c;

    memset(&c, 0, sizeof(c));
    munit_assert_int(ssd_controller_init(&c, g_ctrl.ftl), ==, SSD_STATUS_OK);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_READY);
    munit_assert_uint32(c.host_lba_max, ==, (DWORD)FTL_LBA_MAX);

    return MUNIT_OK;
}

static MunitResult test_hil_init_missing_slot(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_controller_t c;
    ftl_ctx_t       *route[FTL_DIE_MAX];

    munit_assert_uint32(DIE_MAX, >, 0U);

    memset(&c, 0, sizeof(c));
    memset(route, 0, sizeof(route));

    munit_assert_int(ssd_controller_init(&c, route), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_FAILED);

    return MUNIT_OK;
}

static MunitResult test_hil_init_topo_mismatch(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_controller_t c;
    ftl_ctx_t       *route[FTL_DIE_MAX];

    munit_assert_uint32(DIE_MAX, >, 0U);

    memset(&s_hil_bad_ftl, 0, sizeof(s_hil_bad_ftl));
    s_hil_bad_ftl.topo.ch = 0xFFU; /* 与 die_2_ch(0) 必然不符 */

    memset(&c, 0, sizeof(c));
    memset(route, 0, sizeof(route));
    route[0] = &s_hil_bad_ftl;

    munit_assert_int(ssd_controller_init(&c, route), ==, SSD_STATUS_INVALID_ARGUMENT);
    munit_assert_int(c.state, ==, SSD_CONTROLLER_FAILED);

    return MUNIT_OK;
}

static MunitResult test_hil_host_queue(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_host_queue_t  q;
    ssd_host_entry_t *entries[SSD_HOST_QUEUE_DEPTH];

    ssd_host_queue_init(&q);
    munit_assert_uint8(ssd_host_queue_used(&q), ==, 0U);
    munit_assert_true(ssd_host_queue_owner_idle(&q, SSD_HOST_OWNER_USB));
    munit_assert_true(ssd_host_queue_owner_idle(&q, SSD_HOST_OWNER_CLI));

    for (BYTE i = 0U; i < SSD_HOST_QUEUE_DEPTH; i++)
    {
        entries[i] = ssd_host_queue_alloc(&q, SSD_HOST_OWNER_USB);
        munit_assert_not_null(entries[i]);
        munit_assert_uint8(entries[i]->tag, ==, i);
    }
    munit_assert_uint8(ssd_host_queue_used(&q), ==, SSD_HOST_QUEUE_DEPTH);
    munit_assert_false(ssd_host_queue_owner_idle(&q, SSD_HOST_OWNER_USB));
    munit_assert_true(ssd_host_queue_owner_idle(&q, SSD_HOST_OWNER_CLI));
    munit_assert_ptr_null(ssd_host_queue_alloc(&q, SSD_HOST_OWNER_CLI));

    ssd_host_queue_free(&q, entries[0]);
    munit_assert_uint8(ssd_host_queue_used(&q), ==, (SSD_HOST_QUEUE_DEPTH - 1U));
    munit_assert_not_null(ssd_host_queue_alloc(&q, SSD_HOST_OWNER_CLI));

    return MUNIT_OK;
}

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

    return MUNIT_OK;
}

static MunitResult test_hil_submit_not_ready(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_controller_t c;
    ssd_request_t    req;
    BYTE             buf[FTL_IU_BYTES];

    memset(&c, 0, sizeof(c));
    hil_req_init(&req, SSD_REQUEST_READ, 0U, 1U, buf, sizeof(buf));

    munit_assert_int(ssd_controller_submit(&c, &req), ==, SSD_STATUS_INVALID_ARGUMENT);

    ssd_controller_state_transition(&c, SSD_CONTROLLER_INITIALIZING);
    ssd_controller_state_transition(&c, SSD_CONTROLLER_FAILED);
    munit_assert_int(ssd_controller_submit(&c, &req), ==, SSD_STATUS_INVALID_ARGUMENT);

    return MUNIT_OK;
}

static MunitResult test_hil_submit_edges(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_request_t req;
    BYTE         *buf = s_hil_edge_buf;

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

    return MUNIT_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 真实数据面（破坏性）
 * ══════════════════════════════════════════════════════════════════ */

static MunitResult test_hil_write_read_roundtrip(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_request_t req;
    const DWORD   lba = hil_lba_base();

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

    return MUNIT_OK;
}

static MunitResult test_hil_flush_job(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_request_t req;

    hil_req_init(&req, SSD_REQUEST_FLUSH, 0U, 0U, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req), ==, SSD_STATUS_OK);
    munit_assert_true(hil_wait(&req, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req.completion_status, ==, SSD_STATUS_OK);
    munit_assert_ptr_null(g_ctrl.flush_req);

    return MUNIT_OK;
}

static MunitResult test_hil_flush_busy(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_request_t req1;
    ssd_request_t req2;

    hil_req_init(&req1, SSD_REQUEST_FLUSH, 0U, 0U, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req1), ==, SSD_STATUS_OK);

    /* 第一条还没泵完：FLUSH 单飞行必须拒绝第二条 */
    hil_req_init(&req2, SSD_REQUEST_FLUSH, 0U, 0U, NULL, 0U);
    munit_assert_int(ssd_controller_submit(&g_ctrl, &req2), ==, SSD_STATUS_BUSY);
    munit_assert_uint8(req2.complete_flag, ==, 1U);
    munit_assert_int(req2.completion_status, ==, SSD_STATUS_BUSY);

    munit_assert_true(hil_wait(&req1, HIL_IO_TIMEOUT_MS));
    munit_assert_int(req1.completion_status, ==, SSD_STATUS_OK);

    return MUNIT_OK;
}

static MunitResult test_hil_trim_job(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ssd_request_t req;
    const DWORD   lba = hil_lba_base();

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
    {(char *)"write_read_roundtrip", test_hil_write_read_roundtrip, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"flush_job",            test_hil_flush_job, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"flush_busy",           test_hil_flush_busy, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {            (char *)"trim_job",             test_hil_trim_job, hil_setup, hil_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                          NULL,                          NULL,      NULL,         NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
