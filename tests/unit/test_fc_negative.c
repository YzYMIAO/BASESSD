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

#include "delay.h"
#include "fault_inject.h"
#include "fc_command.h"
#include "fc_crc_engine.h"
#include "fc_nand_dispatch.h"
#include "fc_nand_engine.h"
#include "fc_test_common.h"
#include "ftl_config.h"
#include "ftl_fc_interface.h"
#include "munit.h"
#include "nand_device.h"
#include "nand_protocol.h"
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

/* short_chain_read_short — 短链读必须返回错误，不能把未初始化缓冲交给上层（HOST-03 / IF-03）。
 *
 * 覆盖：`ftl_cmd_read_data` 在 desc 链只覆盖 1 个 CW、请求 4 KiB 时。
 * 判据：返回 `FTL_ERROR`（链比请求短）。
 * 备注：FC 夹具建 1-CW 读命令，不提交；release/debug 均可运行。 */

static MunitResult test_short_chain_read_short(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    static BYTE  buf[PAGE_MAIN_SIZE];
    nand_addr_s  addr;
    fc_cmd_cfg_s cfg;

    memset(&addr, 0, sizeof(addr));
    memset(&cfg, 0, sizeof(cfg));
    cfg.operation = (BYTE)OP_NORMAL_READ_PAGE_WITH_DATA_OUT;

    fc_cmd_s *cmd = test_cmd_build(&addr, 1U << 0, FC_META_UNKNOWN, &cfg);

    munit_assert_int(ftl_cmd_read_data(cmd, buf, PAGE_MAIN_SIZE), ==, FTL_ERROR);

    fc_command_release(cmd->cmd_id);

    return MUNIT_OK;
}

/* ══ 子套件 /fc-nand/negative/：FC 边界负例（release UT 生效）══ */

/* cache_read_completion_all_cws — cache 读失败必须标记全部选中 CW（DP-12）。
 *
 * 覆盖：OP_READ_PAGE_CACHE_SEQ/RANDOM/LAST 在 completion 策略表中的映射。
 * 判据：多 CW 失败时，选中的每个 flat CW 都是 FC_CW_STATUS_FAIL；未选中槽保持
 *       UNUSED。若映射回 completion_per_cw，只有最低位 CW 会被标记。
 * 备注：只调用 completion 策略表（测试 seam），不提交 NAND。 */

static MunitResult test_cache_read_completion_all_cws(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    static const BYTE ops[] = {OP_READ_PAGE_CACHE_SEQ, OP_READ_PAGE_CACHE_RANDOM, OP_READ_PAGE_CACHE_LAST};

    /* 选 plane1 的 CW0 与 CW2：不连续、非最低位，能区分 per_cw 与 all_cws。 */
    const BYTE  plane  = 1U;
    const DWORD bitmap = ftl_cw_bit(plane, 0U) | ftl_cw_bit(plane, 2U);
    const BYTE  cw0    = fc_cw_flat(plane, 0U);
    const BYTE  cw2    = fc_cw_flat(plane, 2U);

    BYTE status[CW_TOTAL_MAX];

    test_trace_begin();

    for (BYTE i = 0U; i < (BYTE)(sizeof(ops) / sizeof(ops[0])); i++)
    {
        memset(status, 0xEE, sizeof(status));
        fc_nand_dispatch_test_completion(ops[i], bitmap, status);

        munit_assert_uint8(status[cw0], ==, FC_CW_STATUS_FAIL);
        munit_assert_uint8(status[cw2], ==, FC_CW_STATUS_FAIL);

        for (BYTE n = 0U; n < CW_TOTAL_MAX; n++)
        {
            if ((n != cw0) && (n != cw2))
            {
                munit_assert_uint8(status[n], ==, FC_CW_STATUS_UNUSED);
            }
        }
    }

    test_trace_summary("fc negative: cache read fail marks all selected CWs (op=%hhu..%hhu)", ops[0], ops[2]);

    return MUNIT_OK;
}

/* uop_jump_operand_guard — JUMP 回跳 operand > index 必须被拒绝（DP-17）。
 *
 * 覆盖：`fc_uop_engine_step` 的 JUMP_IF_NOT_READY release 守卫。
 * 判据：release 返回 `NAND_RESULT_INVALID_ARGUMENT`，不发生 WORD 下溢/越界读 UOP 表；
 *       debug 下 ASSERT_REJECT 停机 ⇒ `MUNIT_SKIP`。
 * 备注：纯内存 UOP/状态，不碰 NAND。 */

static MunitResult test_uop_jump_operand_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    static nand_uop_handler_fn_t handlers[NAND_UOP_COUNT];

    nand_phy_backend_s backend;
    nand_cmd_req_s     request;
    nand_cmd_comp_s    completion;
    nand_op_timing_s   timing;
    nand_uop_s         uops[] = {
        {NAND_UOP_JUMP_IF_NOT_READY, 1U},
        {              NAND_UOP_END, 0U}
    };
    fc_uop_engine_state_s state;

    memset(&backend, 0, sizeof(backend));
    backend.handlers = handlers;
    memset(&request, 0, sizeof(request));
    memset(&completion, 0, sizeof(completion));
    memset(&timing, 0, sizeof(timing));
    timing.busy_timeout_us = UINT16_MAX;
    completion.nand_status = 0U; /* 无 READY 位 ⇒ 进入 JUMP 回跳分支 */

    memset(&state, 0, sizeof(state));
    state.backend      = &backend;
    state.uops         = uops;
    state.timing       = &timing;
    state.request      = &request;
    state.completion   = &completion;
    state.uop_index    = 0U;
    state.active       = 1U;
    state.jump.started = 1U; /* 跳过时基锁存，直接测 operand 守卫 */

    munit_assert_int(fc_uop_engine_step(&state), ==, NAND_RESULT_INVALID_ARGUMENT);

    return MUNIT_OK;
#endif
}

/* data_path_encode_fail — 编码失败必须让写命令失败，不得提交未编码数据（DP-02）。 */
static MunitResult test_data_path_encode_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_cmd_cfg_s conf;
    nand_addr_s  addr;

    memset(&conf, 0, sizeof(conf));
    conf.operation = OP_NORMAL_PROGRAM_PAGE;
    memset(&addr, 0, sizeof(addr));

    fc_cmd_s *cmd = test_cmd_build(&addr, 0x01U, 0U, &conf);

    test_memset_read_buffer(cmd);
    fi_arm(FI_DATA_PATH_ENCODE_FAIL, 1U);

    const fc_cmd_status_s completion = test_submit_wait(cmd);

    munit_assert_uint32(fi_hits(FI_DATA_PATH_ENCODE_FAIL), >, 0U);
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, !=, 0U);

    fc_command_release(cmd->cmd_id);
    return MUNIT_OK;
}

/* data_path_read_fail — 读路径解码失败必须上报错误，不能返回陈旧/未校验数据（DP-01）。 */
static MunitResult test_data_path_read_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_cmd_cfg_s conf;
    nand_addr_s  addr;

    memset(&conf, 0, sizeof(conf));
    conf.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    memset(&addr, 0, sizeof(addr));

    fc_cmd_s *cmd = test_cmd_build(&addr, 0x01U, 0U, &conf);

    test_memset_read_buffer(cmd);
    fi_arm(FI_DATA_PATH_READ_FAIL, 1U);

    const fc_cmd_status_s completion = test_submit_wait(cmd);

    munit_assert_uint32(fi_hits(FI_DATA_PATH_READ_FAIL), >, 0U);
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, !=, 0U);

    fc_command_release(cmd->cmd_id);
    return MUNIT_OK;
}

/* clock_now_fail — 时基失败 seam 可达（DP-11）。 */
static MunitResult test_clock_now_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    DWORD       us    = 0U;
    const DWORD hits0 = fi_hits(FI_CLOCK_NOW_FAIL);

    fi_arm(FI_CLOCK_NOW_FAIL, 1U);

    munit_assert_false(delay_time_now_us(&us));
    munit_assert_uint32(fi_hits(FI_CLOCK_NOW_FAIL), >, hits0);

    return MUNIT_OK;
}

/* uop_data_range_guard — DATA_IN/OUT operand 越界必须被 release 拒绝（NANDP-02）。
 *
 * 覆盖：自定义 UOP 表 + 真实 NAND backend，DATA_OUT operand > 页缓冲。
 * 判据：release 返回 `NAND_RESULT_INVALID_ARGUMENT`，不越界读写；debug 下停机 ⇒ SKIP。 */
static MunitResult test_uop_data_range_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    const nand_phy_backend_s *backend = NULL;
    BYTE                      buf[8]  = {0};

    munit_assert_int(nand_target_get(0U, 0U, &backend), ==, NAND_RESULT_OK);
    munit_assert_not_null(backend);

    nand_cmd_req_s   request;
    nand_cmd_comp_s  completion;
    nand_op_timing_s timing;
    nand_uop_s       uops[] = {
        {NAND_UOP_DATA_OUT, PAGE_BUFFER_SIZE + 1U},
        {     NAND_UOP_END,                    0U}
    };
    fc_uop_engine_state_s state;

    memset(&request, 0, sizeof(request));
    request.buffer      = buf;
    request.data_length = (WORD)sizeof(buf);
    memset(&completion, 0, sizeof(completion));
    memset(&timing, 0, sizeof(timing));
    timing.busy_timeout_us = UINT16_MAX;

    memset(&state, 0, sizeof(state));
    state.backend    = backend;
    state.uops       = uops;
    state.timing     = &timing;
    state.request    = &request;
    state.completion = &completion;
    state.active     = 1U;

    munit_assert_int(fc_uop_engine_step(&state), ==, NAND_RESULT_INVALID_ARGUMENT);

    return MUNIT_OK;
#endif
}

/* uop_zero_timing_guard — timing 字段为 0 必须被 release 拒绝（NANDP-10）。
 *
 * 覆盖：自定义 UOP 表 `DELAY_TIMING(tadl)` + timing.tadl_us=0。
 * 判据：release 返回 `NAND_RESULT_INVALID_ARGUMENT`，不静默 delay(0)；debug 停机 ⇒ SKIP。 */
static MunitResult test_uop_zero_timing_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    const nand_phy_backend_s *backend = NULL;

    munit_assert_int(nand_target_get(0U, 0U, &backend), ==, NAND_RESULT_OK);
    munit_assert_not_null(backend);

    nand_cmd_req_s   request;
    nand_cmd_comp_s  completion;
    nand_op_timing_s timing;
    nand_uop_s       uops[] = {
        {NAND_UOP_DELAY_TIMING, NAND_TIMING_TADL},
        {         NAND_UOP_END,               0U}
    };
    fc_uop_engine_state_s state;

    memset(&request, 0, sizeof(request));
    memset(&completion, 0, sizeof(completion));
    memset(&timing, 0, sizeof(timing)); /* 所有字段为 0 */
    timing.busy_timeout_us = UINT16_MAX;

    memset(&state, 0, sizeof(state));
    state.backend    = backend;
    state.uops       = uops;
    state.timing     = &timing;
    state.request    = &request;
    state.completion = &completion;
    state.active     = 1U;

    munit_assert_int(fc_uop_engine_step(&state), ==, NAND_RESULT_INVALID_ARGUMENT);

    return MUNIT_OK;
#endif
}

/* batch_encode_fail — 批提交中 encode 失败必须整批失败、不留半 ACTIVE（DP-07）。 */
static MunitResult test_batch_encode_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_cmd_cfg_s conf;
    nand_addr_s  addr;

    memset(&conf, 0, sizeof(conf));
    conf.operation = OP_NORMAL_PROGRAM_PAGE;

    fc_cmd_id_t ids[2];

    for (BYTE i = 0U; i < 2U; i++)
    {
        fc_cmd_s *cmd;

        memset(&addr, 0, sizeof(addr));
        addr.page = i;
        cmd       = test_cmd_build(&addr, 0x01U, 0U, &conf);
        test_memset_read_buffer(cmd);
        ids[i] = cmd->cmd_id;
    }

    fi_arm(FI_DATA_PATH_ENCODE_FAIL, 2U);

    munit_assert_int(fc_submit_batch(ids, 2U), ==, FC_STATUS_OK);

    BYTE got = 0U;

    for (DWORD i = 0U; (i < 2000U) && (got < 2U); i++)
    {
        fc_cmd_status_s c;
        WORD            n = 0U;

        (void)fc_service();
        if (fc_poll_completions(&c, 1U, &n) == FC_STATUS_OK && n != 0U)
        {
            munit_assert_uint32(c.status & FC_CMD_STATUS_ERROR_MASK, !=, 0U);
            got++;
        }
    }

    munit_assert_uint8(got, ==, 2U);

    for (BYTE i = 0U; i < 2U; i++)
    {
        fc_command_release(ids[i]);
    }

    return MUNIT_OK;
}

/* crc_compute_fail_seam — CRC 计算失败 seam 可达（DP-28）。 */
static MunitResult test_crc_compute_fail_seam(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    BYTE buf[FC_CRC_USER_SIZE];

    memset(buf, 0x5A, sizeof(buf));

    const DWORD hits0 = fi_hits(FI_CRC_COMPUTE_FAIL);

    fi_arm(FI_CRC_COMPUTE_FAIL, 1U);

    munit_assert_uint32(fc_crc_compute(buf), ==, 0U);
    munit_assert_uint32(fi_hits(FI_CRC_COMPUTE_FAIL), >, hits0);
    munit_assert_uint8(fc_crc_verify(buf, 0x12345678U), ==, FC_CRC_RESULT_ERROR);

    return MUNIT_OK;
}

/* onfi_read_fail_seam — 参数页读失败 seam 可达（TOP-03）。 */
static MunitResult test_onfi_read_fail_seam(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    BYTE            page[NAND_ONFI_PARAM_PAGE_SIZE];
    nand_cmd_comp_s completion;
    const DWORD     hits0 = fi_hits(FI_ONFI_READ_FAIL);

    memset(page, 0, sizeof(page));
    memset(&completion, 0, sizeof(completion));
    fi_arm(FI_ONFI_READ_FAIL, 1U);

    munit_assert_int(fc_nand_test_read_parameter_page(0U, 0U, page, &completion), ==, NAND_RESULT_OPERATION_FAILED);
    munit_assert_uint32(fi_hits(FI_ONFI_READ_FAIL), >, hits0);

    return MUNIT_OK;
}

/* desc_cycle_release_bounded — 成环 desc 链释放必须有界截断（POOL-03 / DP-04）。 */
static MunitResult test_desc_cycle_release_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    /* 成环检测在 debug 走 ASSERT_MSG 停机；release 才走有界返回。 */
    return MUNIT_SKIP;
#else
    fc_cmd_cfg_s conf;
    nand_addr_s  addr;

    memset(&conf, 0, sizeof(conf));
    conf.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    memset(&addr, 0, sizeof(addr));

    fc_cmd_s *cmd = test_cmd_build(&addr, 0x03U, 0U, &conf);

    fc_desc_id_t d0 = (fc_desc_id_t)cmd->desc_id;
    fc_desc_s   *a  = fc_descriptor_id_to_descriptor(d0);

    munit_assert_not_null(a);

    fc_desc_id_t d1 = (fc_desc_id_t)a->next_id;
    fc_desc_s   *b  = fc_descriptor_id_to_descriptor(d1);

    munit_assert_not_null(b);

    /* 成环：d1 -> d0。release 必须有界截断，不能死循环。 */
    b->next_id = (DWORD)d0;

    fc_command_release(cmd->cmd_id);

    return MUNIT_OK;
#endif
}

/* invalid_buffer_rejected — desc 指向 INVALID buffer 必须拒绝（DP-03）。 */
static MunitResult test_invalid_buffer_rejected(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_cmd_cfg_s conf;
    nand_addr_s  addr;

    memset(&conf, 0, sizeof(conf));
    conf.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    memset(&addr, 0, sizeof(addr));

    fc_cmd_s  *cmd = test_cmd_build(&addr, 0x01U, 0U, &conf);
    fc_desc_s *d   = fc_descriptor_id_to_descriptor((fc_desc_id_t)cmd->desc_id);

    munit_assert_not_null(d);

    const DWORD saved = d->buffer_id;

    d->buffer_id = FC_BUF_ID_INVALID;

    const fc_cmd_status_s completion = test_submit_wait(cmd);

    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, !=, 0U);

    d->buffer_id = saved; /* 归还时恢复，避免 buffer 泄漏 */
    fc_command_release(cmd->cmd_id);

    return MUNIT_OK;
}

/* nonactive_completion_rejected — 非 ACTIVE 命令的完成必须拒绝，不写 CQ（SCH-11）。 */
static MunitResult test_nonactive_completion_rejected(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    fc_resource_stats_s before;

    fc_get_resource_stats(&before);

    munit_assert_int(fc_test_complete_nonactive(), ==, FC_STATUS_IO_ERROR);

    fc_resource_stats_s after;

    fc_get_resource_stats(&after);
    munit_assert_uint16(after.command_used, ==, before.command_used);

    return MUNIT_OK;
#endif
}

MunitTest g_fc_negative_tests[] = {
    {(char *)"nonactive_completion_rejected", test_nonactive_completion_rejected, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"desc_cycle_release_bounded",    test_desc_cycle_release_bounded, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"invalid_buffer_rejected",       test_invalid_buffer_rejected, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        (char *)"crc_compute_fail_seam",         test_crc_compute_fail_seam, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"onfi_read_fail_seam",           test_onfi_read_fail_seam, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {            (char *)"batch_encode_fail",             test_batch_encode_fail, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"dispatch_enqueue_guards",       test_dispatch_enqueue_guards, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       (char *)"nand_start_unbound_txn",        test_nand_start_unbound_txn, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {            (char *)"stats_consistency",             test_stats_consistency, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"batch_arg_guards",              test_batch_arg_guards, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"batch_cross_target_reject",     test_batch_cross_target_reject, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"iface_release_guards",          test_iface_release_guards, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"in_flight_release_guard",       test_in_flight_release_guard, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"data_ptr_single_plane_only",    test_data_ptr_single_plane_only, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"cw_write_plane1_only",          test_cw_write_plane1_only, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"cache_read_completion_all_cws", test_cache_read_completion_all_cws, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       (char *)"uop_jump_operand_guard",        test_uop_jump_operand_guard, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        (char *)"data_path_encode_fail",         test_data_path_encode_fail, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"data_path_read_fail",           test_data_path_read_fail, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {               (char *)"clock_now_fail",                test_clock_now_fail, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"uop_data_range_guard",          test_uop_data_range_guard, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {        (char *)"uop_zero_timing_guard",         test_uop_zero_timing_guard, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"nand_timing_bounds",            test_nand_timing_bounds, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       (char *)"short_chain_read_short",        test_short_chain_read_short, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                                   NULL,                               NULL,       NULL,           NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
