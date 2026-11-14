/*
 * test_fc_negative.c — /fc-nand/negative/：FC 边界的负例（release UT 生效）
 *
 * 这些守卫用 `ASSERT_REJECT`：debug 下打印并停机、release 下返回
 * `NAND_RESULT_INVALID_ARGUMENT`。debug UT 无法「预期停机」，所以用例在 `!NDEBUG`
 * 下返回 `MUNIT_SKIP`；要验证拒绝语义，跑 `make tests BUILD_TYPE=release UT=FC`
 * 的 `release_ut.elf`。
 *
 * 注释约定同 test_ftl_media.c：覆盖 / 判据 / 备注，不套 S5 契约（P5 偏离）。
 */

#include "fc_command.h"
#include "fc_nand_dispatch.h"
#include "fc_nand_engine.h"
#include "fc_test_common.h"
#include "ftl_fc_interface.h"
#include "munit.h"
#include "nand_device.h"
#include "onfi_1_0.h"

#include <string.h>

/* dispatch_enqueue_guards — 提交入口的非法参数必须被拒绝（DP-10 / DP-25）。
 *
 * 覆盖：NULL 请求、count=0、保留/未实现 opcode。
 * 判据：release 下全部返回 `NAND_RESULT_INVALID_ARGUMENT`，不入队、不崩溃。
 * 备注：debug 下 ASSERT_REJECT 会停机 ⇒ `MUNIT_SKIP`。 */

static MunitResult test_dispatch_enqueue_guards(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP; /* debug：ASSERT_REJECT 停机，无法用 munit 断言 */
#else
    fc_nand_dispatch_req_s req;

    memset(&req, 0, sizeof(req));
    req.operation = (BYTE)OP_COUNT; /* 保留/未实现的 op → req/completion 表项为空 */

    munit_assert_int(fc_nand_dispatch_submit(NULL), ==, NAND_RESULT_INVALID_ARGUMENT);
    munit_assert_int(fc_nand_dispatch_submit_chain(&req, 0U), ==, NAND_RESULT_INVALID_ARGUMENT);
    munit_assert_int(fc_nand_dispatch_submit(&req), ==, NAND_RESULT_INVALID_ARGUMENT);

    return MUNIT_OK;
#endif
}

/* nand_start_unbound_txn — 零初始化 txn 不得解引用空 request/completion（DP-23）。
 *
 * 覆盖：不手工绑定 `engine.request/completion` 直接 `fc_nand_start`。
 * 判据：release 下返回 `NAND_RESULT_INVALID_ARGUMENT`（backend 未绑定），不崩。
 * 备注：`fc_nand_start` 已统一绑定 request/completion；本用例防回归到「调用者必须
 *       预绑定」。debug 下 ASSERT_REJECT 停机 ⇒ `MUNIT_SKIP`。 */

static MunitResult test_nand_start_unbound_txn(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    fc_nand_txn_s  txn;
    nand_cmd_req_s request;

    memset(&txn, 0, sizeof(txn));
    memset(&request, 0, sizeof(request));

    munit_assert_int(fc_nand_start(&txn, (BYTE)OP_RESET, &request), ==, NAND_RESULT_INVALID_ARGUMENT);

    return MUNIT_OK;
#endif
}

/* stats_consistency — 资源统计必须与三池实际占用一致（POOL-05）。
 *
 * 覆盖：分配 1 条命令后 `command_used` +1，释放后回到基线。
 * 判据：差分严格相等；不依赖全池扫描的绝对值。
 * 备注：debug 档 `fc_get_resource_stats` 内部已做全池计数校验，本用例做外部对拍。 */

static MunitResult test_stats_consistency(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_resource_stats_s before;
    fc_resource_stats_s after;

    fc_get_resource_stats(&before);

    fc_cmd_s *cmd = fc_command_alloc();

    munit_assert_ptr_not_null(cmd);

    fc_get_resource_stats(&after);
    munit_assert_uint16(after.command_used, ==, (WORD)(before.command_used + 1U));
    munit_assert_uint16(after.descriptor_used, ==, before.descriptor_used);
    munit_assert_uint16(after.buffer_used, ==, before.buffer_used);

    fc_command_release(cmd->cmd_id);

    fc_get_resource_stats(&after);
    munit_assert_uint16(after.command_used, ==, before.command_used);

    return MUNIT_OK;
}

/* batch_arg_guards — 批提交入口的非法参数必须整批拒绝（SCH-12 / SCH-08 / POOL-01）。
 *
 * 覆盖：NULL 数组、n=0、ID 越界、批内重复、链混优先级。
 * 判据：release 下全部返回 `FC_STATUS_INVALID_ARGUMENT`，被拒命令仍处于
 *       ALLOCATED（可正常释放），队列占用不变。
 * 备注：debug 下 ASSERT_REJECT 停机 ⇒ `MUNIT_SKIP`。 */

static MunitResult test_batch_arg_guards(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    fc_cmd_s   *a = fc_command_alloc();
    fc_cmd_s   *b = fc_command_alloc();
    fc_cmd_id_t ids[2];

    munit_assert_ptr_not_null(a);
    munit_assert_ptr_not_null(b);

    fc_resource_stats_s base;
    fc_resource_stats_s now;

    fc_get_resource_stats(&base);

    ids[0] = a->cmd_id;
    ids[1] = a->cmd_id;

    munit_assert_int(fc_submit_batch(NULL, 1U), ==, FC_STATUS_INVALID_ARGUMENT);
    munit_assert_int(fc_submit_batch(ids, 0U), ==, FC_STATUS_INVALID_ARGUMENT);
    munit_assert_int(fc_submit_batch(ids, 2U), ==, FC_STATUS_INVALID_ARGUMENT); /* 批内重复 */

    ids[0] = FC_CMD_ID_INVALID; /* 0xFFFF ≥ 池深 */
    ids[1] = b->cmd_id;
    munit_assert_int(fc_submit_batch(ids, 2U), ==, FC_STATUS_INVALID_ARGUMENT);

    /* 链混优先级：a 标 MORE + 低优先级，b 高优先级 ⇒ 会被拆成 SQ/HSQ 两段。 */
    ids[0]           = a->cmd_id;
    a->config.chain  = FC_CHAIN_MORE;
    b->high_priority = 1U;
    munit_assert_int(fc_submit_batch(ids, 2U), ==, FC_STATUS_INVALID_ARGUMENT);

    fc_get_resource_stats(&now);
    munit_assert_uint16(now.sq_used, ==, base.sq_used);
    munit_assert_uint16(now.hsq_used, ==, base.hsq_used);

    fc_command_release(a->cmd_id);
    fc_command_release(b->cmd_id);

    return MUNIT_OK;
#endif
}

/* iface_release_guards — 接口层 release 守卫：单 plane cache-op（IF-18）。
 *
 * 覆盖：`ftl_async_write_pair_cmd` 的「单 plane cache-op」release 拒绝分支。
 * 判据：返回 `FTL_ERROR`，`out_cmd` 不被写出（命令已回收）。
 * 备注：debug 下 ASSERT 先停机 ⇒ `MUNIT_SKIP`。
 *       ⚠ **跨 die pair 分支在本板不可构造**：`LUN_NUM == CH_NUM == CE_NUM == 1`，
 *       PBA 的 lun/ch/ce 位宽为 0，换 LUN 编码不出不同地址（归 K 类硬件缺口）。
 *       `sync_submit_wait` 超时上界分支也未覆盖（需要真实提交后等待的命令）。 */

static MunitResult test_iface_release_guards(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    static BYTE    pair_buf[CW_TOTAL_MAX * FTL_IU_BYTES];
    ftl_page_cfg_s cfg;
    fc_cmd_s      *out = NULL;
    DWORD          pba[PLANE_NUM];

    memset(&pba, 0, sizeof(pba));
    cfg.crc    = FC_CRC_DISABLE;
    cfg.random = FC_RANDOM_DISABLE;
    cfg.ldpc   = FC_ECC_MODE_NORMAL;
    cfg.lba    = FTL_LBA_UNKNOWN;

    pba[0] = pba_make(0U, 0U, 0U, 0U, 0U, 0U, 0U);
    pba[1] = pba_make(0U, 0U, 0U, 0U, 1U, 0U, 0U);

    /* 单 plane cache-op：opcode 按两 plane 定尺，位图只覆盖 plane0 ⇒ 拒绝。 */
    munit_assert_int(ftl_async_write_pair_cmd(&out, pba, pair_buf, ftl_cw_plane_mask(0U), (BYTE)OP_TWO_PLANE_PROGRAM_CACHE, &cfg), ==, FTL_ERROR);
    munit_assert_ptr_null(out);

    return MUNIT_OK;
#endif
}

/* in_flight_release_guard — 在飞命令不得被释放（POOL-01）。
 *
 * 覆盖：提交后立刻 `fc_command_release`。
 * 判据：release 拒绝，`command_used` 保持不变（命令仍在队列，buffer 不被提前归还）。
 * 备注：debug 下 ASSERT_REJECT_VOID 停机 ⇒ `MUNIT_SKIP`。收尾用 `fc_init` 丢弃排队
 *       命令（纯软件重置，不碰 NAND），避免污染后续用例。 */

static MunitResult test_in_flight_release_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    fc_resource_stats_s before;
    fc_resource_stats_s after;

    fc_cmd_s *cmd = fc_command_alloc();

    munit_assert_ptr_not_null(cmd);

    cmd->config.operation = (BYTE)OP_RESET;
    cmd->config.chain     = FC_CHAIN_END;

    fc_get_resource_stats(&before);
    munit_assert_int(fc_submit(cmd->cmd_id), ==, FC_STATUS_OK);

    fc_command_release(cmd->cmd_id); /* 在飞：必须被拒绝 */

    fc_get_resource_stats(&after);
    munit_assert_uint16(after.command_used, ==, before.command_used);

    fc_init(); /* 丢弃排队命令，恢复干净池 */

    return MUNIT_OK;
#endif
}

/* data_ptr_single_plane_only — 多 plane 命令的数据指针查询必须拒绝（SCH-08）。
 *
 * 覆盖：对 multi-plane 命令调 `fc_get_cmd_data_ptr(cmd_id, cw)`。
 * 判据：release 返回 NULL；若少了守卫，cw0 会命中 plane0 的 buffer 返回非空。
 * 备注：debug 下 ASSERT 停机 ⇒ `MUNIT_SKIP`。 */

static MunitResult test_data_ptr_single_plane_only(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    nand_addr_s  addr;
    fc_cmd_cfg_s cfg;

    memset(&addr, 0, sizeof(addr));
    memset(&cfg, 0, sizeof(cfg));
    cfg.operation = (BYTE)OP_PROGRAM_PAGE_MULTI_PLANE;

    fc_cmd_s *cmd = test_cmd_build(&addr, test_plane_cw_bitmap(TEST_PLANE_BM_ALL), 0U, &cfg);

    munit_assert_uint8(cmd->config.multi_plane, ==, PLANE_NUM);
    munit_assert_ptr_null(fc_get_cmd_data_ptr(cmd->cmd_id, 0U));

    fc_command_release(cmd->cmd_id);

    return MUNIT_OK;
#endif
}

/* cw_write_plane1_only — 只覆盖 plane1 的 CW 位图必须落在 plane1（IF-01）。
 *
 * 覆盖：`ftl_cmd_setup_ppa_pair_cw` 用 plane1 位图建链。
 * 判据：cmd->paa 与链上每条 desc 都在 plane1、CW 覆盖 0..3；数据指针按 CW 可查。
 * 备注：plane 盲位图（`1U<<cw`）会把 plane1 的 CW0 算成 plane0 ⇒ 链落错 plane 或
 *       越段。本用例是 GC 迁移写（同一入口）的链级判据。 */

static MunitResult test_cw_write_plane1_only(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_cmd_s *cmd = fc_command_alloc();

    munit_assert_ptr_not_null(cmd);

    DWORD pba[PLANE_NUM];

    memset(pba, 0, sizeof(pba));
    pba[1] = pba_make(0U, 0U, 0U, 0U, 1U, 0U, 0U);

    cmd->config.operation   = (BYTE)OP_NORMAL_PROGRAM_PAGE;
    cmd->config.multi_plane = 0U;

    munit_assert_int(ftl_cmd_setup_ppa_pair_cw(cmd, pba, ftl_cw_plane_mask(1U)), ==, FTL_OK);
    munit_assert_uint8(paa_to_plane(cmd->paa), ==, 1U);

    BYTE cw_seen = 0U;

    for (fc_desc_id_t did = (fc_desc_id_t)cmd->desc_id; did != FC_DESC_ID_INVALID; did = (fc_desc_id_t)fc_descriptor_id_to_descriptor(did)->next_id)
    {
        const fc_desc_s *d = fc_descriptor_id_to_descriptor(did);

        munit_assert_uint8(paa_to_plane(d->paa), ==, 1U);
        cw_seen |= (BYTE)(1U << paa_to_cw(d->paa));
    }

    munit_assert_uint8(cw_seen, ==, (BYTE)((1U << CW_NUM) - 1U));

    for (BYTE cw = 0U; cw < CW_NUM; cw++)
    {
        munit_assert_ptr_not_null(fc_get_cmd_data_ptr(cmd->cmd_id, cw));
    }

    fc_command_release(cmd->cmd_id);

    return MUNIT_OK;
}

/* nand_timing_bounds — timing 查询的越界/空指针必须拒绝（NANDP-13）。
 *
 * 覆盖：`nand_get_timing_mode` 的 mode=NULL、ch/ce 越界。
 * 判据：release 返回 `NAND_RESULT_INVALID_ARGUMENT`；不进入器件回调。
 * 备注：debug 下 ASSERT_REJECT 停机 ⇒ `MUNIT_SKIP`。`mt29f_set_timing` 是器件内部
 *       static 回调，不在对外契约内。 */

static MunitResult test_nand_timing_bounds(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    BYTE mode = 0U;

    munit_assert_int(nand_get_timing_mode(0U, 0U, NULL), ==, NAND_RESULT_INVALID_ARGUMENT);
    munit_assert_int(nand_get_timing_mode((BYTE)CH_NUM, 0U, &mode), ==, NAND_RESULT_INVALID_ARGUMENT);
    munit_assert_int(nand_get_timing_mode(0U, (BYTE)CE_NUM, &mode), ==, NAND_RESULT_INVALID_ARGUMENT);

    return MUNIT_OK;
#endif
}

/* batch_cross_target_reject — 链成员跨 target 必须整批拒绝（SCH-01）。
 *
 * 覆盖：`fc_submit_batch` 的 ch/ce/lun 一致性预检。
 * 判据：release 返回 `FC_STATUS_INVALID_ARGUMENT`，两条命令仍可释放。
 * 备注：本板 `CH_NUM = CE_NUM = LUN_NUM = 1`，真实跨 target 不可构造；这里把
 *       第二条命令的 ch 置为越界值模拟「不同 target」，只验证比较逻辑。
 *       debug 下 ASSERT_REJECT 停机 ⇒ `MUNIT_SKIP`。 */

static MunitResult test_batch_cross_target_reject(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    fc_cmd_s   *a = fc_command_alloc();
    fc_cmd_s   *b = fc_command_alloc();
    fc_cmd_id_t ids[2];

    munit_assert_ptr_not_null(a);
    munit_assert_ptr_not_null(b);

    a->config.operation = (BYTE)OP_RESET;
    a->config.chain     = FC_CHAIN_END;
    b->config.operation = (BYTE)OP_RESET;
    b->config.chain     = FC_CHAIN_END;
    b->ch               = (BYTE)CH_NUM; /* 单 target 板：越界值模拟不同 target */

    ids[0] = a->cmd_id;
    ids[1] = b->cmd_id;

    munit_assert_int(fc_submit_batch(ids, 2U), ==, FC_STATUS_INVALID_ARGUMENT);

    fc_command_release(a->cmd_id);
    fc_command_release(b->cmd_id);

    return MUNIT_OK;
#endif
}

/* ══ 子套件 /fc-nand/negative/：FC 边界负例（release UT 生效）══ */

MunitTest g_fc_negative_tests[] = {
    {   (char *)"dispatch_enqueue_guards",    test_dispatch_enqueue_guards, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"nand_start_unbound_txn",     test_nand_start_unbound_txn, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"stats_consistency",          test_stats_consistency, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"batch_arg_guards",           test_batch_arg_guards, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { (char *)"batch_cross_target_reject",  test_batch_cross_target_reject, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"iface_release_guards",       test_iface_release_guards, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"in_flight_release_guard",    test_in_flight_release_guard, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"data_ptr_single_plane_only", test_data_ptr_single_plane_only, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"cw_write_plane1_only",       test_cw_write_plane1_only, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        (char *)"nand_timing_bounds",         test_nand_timing_bounds, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                                NULL,                            NULL,       NULL,           NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
