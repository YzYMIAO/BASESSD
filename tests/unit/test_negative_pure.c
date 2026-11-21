/*
 * test_negative_pure.c — /ftl/negative/pure/：不碰介质的负例（RAM 夹具 / 纯函数）
 *
 * 设计：docs/guides/testing_guide.md §12。
 *
 * 与 /ftl/negative/media/ 的分界：
 *   · pure：只用 RAM allocator 夹具、纯函数、或启动后即闲置的 live 实例表；
 *     不 format、不读写 NAND，可单独跑、秒级完成。
 *   · media：真 NAND 破坏性负例，需 format + 统一 drain。
 *
 * 用例名是稳定 ID（回溯/文档/覆盖账直接引用），不随套件重排改名。
 * 注释不写风险编号（映射只在设计文档 §11 与 known_open_items.md 维护）。
 */

#include "allocator.h"
#include "bbt.h"
#include "bg_policy.h"
#include "bst.h"
#include "checkpoint.h"
#include "fault_inject.h"
#include "fi_wrap.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"
#include "ftl_ut_common.h"
#include "gc.h"
#include "l2p_cache.h"
#include "metadata_job.h"
#include "migrate.h"
#include "munit.h"
#include "onfi_1_0.h"
#include "pending.h"
#include "read_recovery.h"
#include "ssd_compiler.h"
#include "timer.h"
#include "wear_leveling.h"

#include <string.h>

/* 纯用例不需要介质，只需一个可用的 live 实例（RAM 夹具与表守卫使用）。 */
static BOOL s_neg_pure_bg_saved;

static void *neg_pure_setup(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_topology_init();

    ftl_ctx_s *ftl = ftl_ut_live();

    s_neg_pure_bg_saved = ftl->flags.bg_suspend;

    /* pure 用例复用的是 live 实例的 cache/journal/metadata_dir；先排空上一条
     * media 用例可能留下的后台 job，再清掉 reclaim/compact 触发位，
     * 否则 l2p_sync_pump 里的 md_handle 会自行提交目录检查点命令，污染
     * 「FC 池不变」这类判据。 */
    /* 排空在飞 job，并把 boot/journal 遗留的「CP 已到期」在 setup 里落定：
     * 否则长时间泵 timer/ftl_service 的用例会在测试内部把 CP 拉起来
     * （→ metadata 日志轮转 → reclaim 命令在飞），污染「FC 池回到基线」判据。 */
    ftl_ut_settle_background(ftl);

    /* 整个用例期间冻结后台：pure 用例是 RAM 级验证，后台任务在这里既不是被测
     * 对象，也没有确定性。不冻结时，timer credit 会在测试内部按当时的水位
     * （log 区 free、journal 增量）起 CP/压缩/reclaim，结果取决于上一条命令/
     * 上一次上电留下的状态 —— 这正是单测通过、全量随机失败的根因。
     * 需要后台的用例自己直接调 md_handle/cp_handle 等模块入口，不经调度器。 */
    ftl->flags.bg_suspend = true;
    md_reclaim_reset(ftl->metadata_dir);
    md_handle_reset(ftl->metadata_dir);

    ftl_ut_ram_init(); /* RAM 分配器/Directory 是跨用例全局：每条用例先归零 */
    fi_reset();
    fi_wrap_reset();
    return NULL;
}

static void neg_pure_teardown(void *fixture)
{
    (void)fixture;

    fi_reset();
    fi_wrap_reset();
    ftl_ut_live()->flags.bg_suspend = s_neg_pure_bg_saved;
    ftl_ut_assert_clean(ftl_ut_live());
}

/* journal_seq_serial_compare — 32-bit 序号回绕处必须按串行号语义比较（JN-09）。
 *
 * 覆盖：`jrn_seq_after` / `jrn_seq_ge` 在回绕边界的真值。
 * 判据：活值间距 < 2^31（RFC 1982）；裸 `>` 会在 0xFFFFFFFF→0 处判反。
 * 备注：纯函数，不碰介质/实例。 */

static MunitResult test_journal_seq_serial_compare(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    munit_assert_true(jrn_seq_after(1U, 0U));
    munit_assert_true(jrn_seq_after(0U, 0xFFFFFFFFU)); /* 回绕后 0 比 max 新 */
    munit_assert_true(jrn_seq_after(0x10U, 0xFFFFFFF0U));
    munit_assert_false(jrn_seq_after(0xFFFFFFF0U, 0x10U));
    munit_assert_true(jrn_seq_ge(5U, 5U));
    munit_assert_false(jrn_seq_ge(4U, 5U));
    munit_assert_false(jrn_seq_ge(0xFFFFFFF0U, 0x10U));

    ftl_ut_trace_summary("neg: journal serial compare handles 32-bit wrap");
    return MUNIT_OK;
}

/* rr_ecc_monotonic — ECC 健康状态只升级不回落（RR-03）。
 *
 * 覆盖：`rr_record_ecc` 的 rising 门与 health=max。
 * 判据：连续上升触发 WARNING；单次高 ECC 触发 CRITICAL；之后再喂低 ECC 上升序列，
 *       不得把 CRITICAL 打回 WARNING。 */

static MunitResult test_rr_ecc_monotonic(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    rr_ecc_track_s t;

    ftl_ut_trace_begin();
    memset(&t, 0, sizeof(t));

    for (BYTE i = 0U; i < RR_ECC_RISING_THRESHOLD; i++)
    {
        rr_record_ecc(&t, (BYTE)(i + 1U));
    }
    munit_assert_int(t.health, >=, RR_BLOCK_WARNING);

    rr_record_ecc(&t, RR_ECC_HIGH_WATER);
    munit_assert_int(t.health, ==, RR_BLOCK_CRITICAL);

    for (BYTE i = 1U; i <= RR_ECC_RISING_THRESHOLD; i++)
    {
        rr_record_ecc(&t, i);
    }
    munit_assert_int(t.health, ==, RR_BLOCK_CRITICAL);

    ftl_ut_trace_summary("neg: RR ECC health monotonic up (no downgrade)");
    return MUNIT_OK;
}

/* iface_pair_cmd_guards — 空位图建链必须拒绝，不得越界读 pba[PLANE_NUM]（IF-18）。
 *
 * 覆盖：`ftl_cmd_setup_ppa_pair_cw(cmd, pba, 0)`。
 * 判据：返回 `FTL_ERROR`；命令槽位随后可正常释放。
 * 备注：这是 release 运行期拒绝（非 ASSERT）；debug 同样返回。 */

static MunitResult test_iface_pair_cmd_guards(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_trace_begin();

    fc_cmd_s *cmd = fc_command_alloc();

    munit_assert_ptr_not_null(cmd);

    DWORD pba[PLANE_NUM];

    memset(pba, 0, sizeof(pba));

    munit_assert_int(ftl_cmd_setup_ppa_pair_cw(cmd, pba, 0U), ==, FTL_ERROR);

    fc_command_release(cmd->cmd_id);

    ftl_ut_trace_summary("neg: empty cw bitmap rejected before first_plane");
    return MUNIT_OK;
}

/* op_req_table_guard — 完成派发 request 表的非法 id / 重复登记（CR-04）。
 *
 * 覆盖：`ftl_op_req_add` 越界 cmd_id、同一 id 重复登记。
 * 判据：越界返回 `FTL_ERROR`（不写表）；重复登记覆盖旧项、计数不增（不漏账）。
 * 备注：debug 下越界 ASSERT 先停机 ⇒ `MUNIT_SKIP`。要求表起始为空（quiesce 后）。 */

static MunitResult test_op_req_table_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    BYTE dummy = 0U;

    ftl_ut_trace_begin();

    const WORD base = ftl_op_req_count();

    munit_assert_uint16(base, ==, 0U); /* 前提：系统 idle，id 0 未被占用 */

    munit_assert_int(ftl_op_req_add((WORD)FTL_OP_REQ_TABLE_SIZE, FTL_OP_HOST_IO, &dummy), ==, FTL_ERROR);

    munit_assert_int(ftl_op_req_add(0U, FTL_OP_HOST_IO, &dummy), ==, FTL_OK);
    munit_assert_int(ftl_op_req_add(0U, FTL_OP_HOST_IO, &dummy), ==, FTL_OK);
    munit_assert_uint16(ftl_op_req_count(), ==, 1U);

    ftl_op_req_remove(0U);
    munit_assert_uint16(ftl_op_req_count(), ==, 0U);
    ftl_op_req_remove(0U); /* 幂等 */

    ftl_ut_trace_summary("neg: op request table rejects OOB id, overwrite counts once");
    return MUNIT_OK;
#endif
}

/* pend_table_guard — pending 表越界 id / 释放后按 id 查询（CR-05）。
 *
 * 覆盖：`pend_find` / `pend_alloc` 越界 cmd_id；分配→查找→摘除→再查找。
 * 判据：越界返回 NULL 且不越界写；摘除后按 id 查不到已释放槽；重复摘除幂等。
 * 备注：`pend_bind` 双绑定在 release 无运行期拒绝（仅 debug ASSERT），不在此断言。 */

static MunitResult test_pend_table_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    pend_ctx_s *table = &ftl_ut_live()->pending;

    ftl_ut_trace_begin();

    munit_assert_ptr_null(pend_find(table, (WORD)FC_COMMAND_POOL_DEPTH));
    munit_assert_ptr_null(pend_alloc(table, (WORD)FC_COMMAND_POOL_DEPTH));

    pend_req_s *req = pend_alloc(table, 0U);

    munit_assert_ptr_not_null(req);
    munit_assert_ptr_equal(pend_find(table, 0U), req);

    pend_remove(table, 0U);
    munit_assert_ptr_null(pend_find(table, 0U));
    pend_remove(table, 0U); /* 幂等 */

    ftl_ut_trace_summary("neg: pending table rejects OOB id, released slot unreachable");
    return MUNIT_OK;
#endif
}

/* alloc_rewind_underflow — next_page == 0 时 rewind 不得下溢成 63（ALLOC-13）。
 *
 * 覆盖：未分配页对的 SB 上调 `alc_super_open_rewind_pair`。
 * 判据：`next_page` 保持 0（不递减回绕）。
 * 备注：debug 下入口 ASSERT 停机 ⇒ `MUNIT_SKIP`。 */

static MunitResult test_alloc_rewind_underflow(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    a->hot_sb.next_page = 0U;
    a->hot_sb.is_open   = true;

    alc_super_open_rewind_pair(a, FTL_TEMP_HOT);

    munit_assert_uint8(a->hot_sb.next_page, ==, 0U);

    return MUNIT_OK;
#endif
}

/* alloc_spare_skip_bad — Spare 池里的 BAD 块必须跳过（ALLOC-16）。
 *
 * 覆盖：两个 Spare，尾部（先弹出）标 BAD。
 * 判据：pop 返回前一个好块，`spare_count` 递减到 0；不把 BAD 块当替换块交出去。
 * 备注：RAM 分配器夹具，不动真介质。 */

static MunitResult test_alloc_spare_skip_bad(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    /* Spare 池成员由 spare_count 持有，state 必须是 NONE；FREE 只表示「在 Free Pool」。 */
    a->blocks[0].state = FTL_BLOCK_STATE_NONE;
    a->blocks[1].state = FTL_BLOCK_STATE_NONE;

    a->spare_blocks[0][0] = 0U;
    a->spare_blocks[0][1] = 1U;
    a->spare_count[0]     = 2U;

    ftl_block_set_bad(&a->blocks[1]); /* 栈顶先弹：必须是坏块 */

    WORD out = FTL_GBLOCK_INVALID;

    munit_assert_true(alc_spare_pop(a, 0U, &out));
    munit_assert_uint16(out, ==, 0U);
    munit_assert_uint16(a->spare_count[0], ==, 0U);

    ftl_ut_trace_summary("neg: spare pop skips runtime-bad block");
    return MUNIT_OK;
}

/* wl_erase_resets_read_count — 擦除必须清零 read disturb 计数（WL-02）。
 *
 * 覆盖：置 read_count 后调 `alc_bst_note_erase`。
 * 判据：read_count 归零、erase_count +1、last_pe_ts 更新；趋势字段保留。
 * 备注：RAM 分配器夹具。 */

static MunitResult test_wl_erase_resets_read_count(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    /* 本用例只验证擦除记账，不把块放进 Free Pool：state 用 NONE，避免制造
     * 「FREE ⟺ 在池」的反向不一致。 */
    a->blocks[5].state      = FTL_BLOCK_STATE_NONE;
    a->blocks[5].read_count = 1234U;
    a->blocks[5].rr.health  = 7U;

    const DWORD e0 = a->blocks[5].erase_count;

    alc_bst_note_erase(a, 5U, 4242U);

    munit_assert_uint16(a->blocks[5].read_count, ==, 0U);
    munit_assert_uint32(a->blocks[5].erase_count, ==, e0 + 1U);
    munit_assert_uint32(a->blocks[5].last_pe_ts, ==, 4242U);
    munit_assert_uint8(a->blocks[5].rr.health, ==, 7U);

    ftl_ut_trace_summary("neg: erase resets read disturb, keeps ECC trend");
    return MUNIT_OK;
}

/* wl_skip_bad_plane — 含 BAD plane 的 Closed SB 不得被静态 WL 选中（WL-03）。
 *
 * 覆盖：RAM 分配器里造一个 plane1 块变坏的 Closed 条目，调 `wl_select_victim`。
 * 判据：对照（两块都健康）选中；标坏 plane1 后返回 false。选中会 migrate 读失败并
 *       每轮重选，WL 永久卡死。
 * 备注：RAM 夹具，不动真介质。 */

static MunitResult test_wl_skip_bad_plane(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    memset(&a->closed_sb_list[0], 0, sizeof(a->closed_sb_list[0]));
    a->closed_sb_list[0].is_open   = false;
    a->closed_sb_list[0].next_page = (BYTE)PAGES_PER_BLOCK;
    a->closed_sb_count             = 1U;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        a->closed_sb_list[0].plane_blocks[p] = 0U;

        const WORD g = ftl_gblock(p, 0U);

        a->blocks[g].state       = FTL_BLOCK_STATE_CLOSED;
        a->blocks[g].valid_count = (WORD)FTL_CW_PER_BLOCK;
    }

    wl_victim_s victim;

    memset(&victim, 0, sizeof(victim));
    munit_assert_true(wl_select_victim(a, &victim)); /* 对照：健康条目可选 */

    ftl_block_set_bad(&a->blocks[ftl_gblock(1U, 0U)]);

    memset(&victim, 0, sizeof(victim));
    munit_assert_false(wl_select_victim(a, &victim));

    ftl_ut_trace_summary("neg: WL victim selection skips SB with BAD plane");
    return MUNIT_OK;
}

/* alloc_closed_list_full — Closed 列表满时关闭 SB 必须失败且保持 live（ALLOC-08）。
 *
 * 覆盖：`alc_super_close` 在 `closed_sb_count == SUPER_BLOCK_LIST_MAX` 时。
 * 判据：返回 `FTL_ERROR`；`is_open` 不变、计数不增（不丢活 SB）。
 * 备注：`alc_super_close_live` 入口只有 debug ASSERT，release 才走运行期拒绝 ⇒
 *       `MUNIT_SKIP`。RAM 分配器夹具。 */

static MunitResult test_alloc_closed_list_full(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    a->closed_sb_count        = SUPER_BLOCK_LIST_MAX;
    a->hot_sb.is_open         = true;
    a->hot_sb.plane_blocks[0] = 0U;
    a->hot_sb.plane_blocks[1] = 0U;

    munit_assert_int(alc_super_close(a, FTL_TEMP_HOT), ==, FTL_ERROR);
    munit_assert_true(a->hot_sb.is_open);
    munit_assert_uint16(a->closed_sb_count, ==, SUPER_BLOCK_LIST_MAX);

    return MUNIT_OK;
#endif
}

/* alloc_closed_plane_bad_drop — 运行期标坏要同步摘除 Closed 条目的 plane（ALLOC-14）。
 *
 * 覆盖：`bbt_mark_bad` 对 Closed SB 的一个 plane 块。
 * 判据：该 plane 从条目摘除（`FTL_BLOCK_NO_PLANE`）、条目仍在；另一 plane 不受影响。
 * 备注：RAM 分配器夹具，不动真介质。 */

static MunitResult test_alloc_closed_plane_bad_drop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    memset(&a->closed_sb_list[0], 0, sizeof(a->closed_sb_list[0]));
    a->closed_sb_list[0].is_open         = false;
    a->closed_sb_list[0].plane_blocks[0] = 2U;
    a->closed_sb_list[0].plane_blocks[1] = 3U;
    a->closed_sb_count                   = 1U;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        const WORD g = ftl_gblock(p, a->closed_sb_list[0].plane_blocks[p]);

        a->blocks[g].state = FTL_BLOCK_STATE_CLOSED;
        ftl_block_set_sb(&a->blocks[g], 0U);
    }

    munit_assert_int(bbt_mark_bad(a, ftl_gblock(0U, 2U)), ==, FTL_OK);

    munit_assert_uint16(a->closed_sb_list[0].plane_blocks[0], ==, FTL_BLOCK_NO_PLANE);
    munit_assert_uint16(a->closed_sb_list[0].plane_blocks[1], ==, 3U);

    return MUNIT_OK;
}

/* alloc_close_bad_pair_no_entry — 全 plane BAD 的 SB 关闭不得产生零 plane 条目（ALLOC-15）。
 *
 * 覆盖：两个 plane 块都 BAD 时 `alc_super_close`。
 * 判据：`closed_sb_count` 不增（不产生永远选不中的死条目）；SB 仍算关闭。
 * 备注：RAM 分配器夹具。 */

static MunitResult test_alloc_close_bad_pair_no_entry(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    a->hot_sb.is_open         = true;
    a->hot_sb.plane_blocks[0] = 4U;
    a->hot_sb.plane_blocks[1] = 5U;

    ftl_block_set_bad(&a->blocks[ftl_gblock(0U, 4U)]);
    ftl_block_set_bad(&a->blocks[ftl_gblock(1U, 5U)]);

    const WORD before = a->closed_sb_count;

    munit_assert_int(alc_super_close(a, FTL_TEMP_HOT), ==, FTL_OK);
    munit_assert_uint16(a->closed_sb_count, ==, before);
    munit_assert_false(a->hot_sb.is_open);

    return MUNIT_OK;
}

/* wl_destination_prepare_fail — closed 列表满时 WL 目的准备失败不得退化为普通分配（WL-11）。
 *
 * 覆盖：`alc_super_prepare_wl_destination` 在列表满时。
 * 判据：返回 false；live cold SB 保持打开（调用者必须放弃本次 WL）。
 * 备注：入口只有 debug ASSERT，release 才走运行期拒绝 ⇒ `MUNIT_SKIP`。 */

static MunitResult test_wl_destination_prepare_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    a->closed_sb_count         = SUPER_BLOCK_LIST_MAX;
    a->cold_sb.is_open         = true;
    a->cold_sb.next_page       = 1U; /* 非空 ⇒ 走 close_live，列表满才会失败 */
    a->cold_sb.plane_blocks[0] = 0U;
    a->cold_sb.plane_blocks[1] = 0U;

    munit_assert_false(alc_super_prepare_wl_destination(a));
    munit_assert_true(a->cold_sb.is_open);

    return MUNIT_OK;
#endif
}

/* wl_snapshot_entry_trigger — 快照恢复的单 plane 条目也要能触发静态 WL（WL-10）。
 *
 * 覆盖：`wl_ec_spread_get` / `wl_should_trigger` 的分母按条目自身容量。
 * 判据：free 高擦除块 − 最冷 used SB ≥ WL_THRESHOLD 时触发。
 * 备注：RAM 分配器夹具；用于钉住「分母用整 SB 容量」的回归。 */

static MunitResult test_wl_snapshot_entry_trigger(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    alc_free_push(a, ftl_gblock(0U, 5U));
    alc_free_push(a, ftl_gblock(1U, 5U));
    a->blocks[ftl_gblock(0U, 5U)].erase_count = 500U;
    a->blocks[ftl_gblock(1U, 5U)].erase_count = 500U;

    memset(&a->closed_sb_list[0], 0, sizeof(a->closed_sb_list[0]));
    a->closed_sb_list[0].is_open         = false;
    a->closed_sb_list[0].next_page       = (BYTE)PAGES_PER_BLOCK;
    a->closed_sb_list[0].plane_blocks[0] = 6U;
    a->closed_sb_list[0].plane_blocks[1] = FTL_BLOCK_NO_PLANE;
    a->closed_sb_count                   = 1U;

    const WORD g = ftl_gblock(0U, 6U);

    a->blocks[g].state       = FTL_BLOCK_STATE_CLOSED;
    a->blocks[g].erase_count = 100U;
    a->blocks[g].valid_count = (WORD)FTL_CW_PER_BLOCK;

    DWORD max_free = 0U;
    DWORD min_used = 0U;

    munit_assert_true(wl_ec_spread_get(a, WL_MIN_WRITES, &max_free, &min_used));
    munit_assert_uint32((max_free - min_used), >=, WL_THRESHOLD);
    munit_assert_true(wl_should_trigger(a, WL_MIN_WRITES));

    return MUNIT_OK;
}

/* rr_predictive_skip_bad_plane — 预测巡检必须跳过含 BAD plane 的 SB（RR-04）。
 *
 * 覆盖：两个 Closed 条目都达到预测阈值，第一个含 BAD plane。
 * 判据：返回第二个条目，不被坏 plane 卡死。
 * 备注：RAM 分配器夹具。 */

static MunitResult test_rr_predictive_skip_bad_plane(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_ram_init();

    alc_s *a = ftl_ut_ram_alloc();

    memset(a->closed_sb_list, 0, 2U * sizeof(a->closed_sb_list[0]));

    a->closed_sb_list[0].is_open         = false;
    a->closed_sb_list[0].plane_blocks[0] = 10U;
    a->closed_sb_list[0].plane_blocks[1] = 10U;
    a->closed_sb_list[1].is_open         = false;
    a->closed_sb_list[1].plane_blocks[0] = 11U;
    a->closed_sb_list[1].plane_blocks[1] = 11U;
    a->closed_sb_count                   = 2U;

    for (BYTE s = 0U; s < 2U; s++)
    {
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            const WORD g = ftl_gblock(p, a->closed_sb_list[s].plane_blocks[p]);

            a->blocks[g].state       = FTL_BLOCK_STATE_CLOSED;
            a->blocks[g].valid_count = 1U;
            a->blocks[g].rr.health   = RR_BLOCK_CRITICAL;
            a->blocks[g].rr.last_ecc = 8U;
            ftl_block_set_sb(&a->blocks[g], s);
        }
    }

    ftl_block_set_bad(&a->blocks[ftl_gblock(1U, 10U)]);

    WORD gblock = FTL_GBLOCK_INVALID;

    munit_assert_true(rr_predictive_scan(a, &gblock));
    munit_assert_uint16(gblock, ==, ftl_gblock(0U, 11U));

    return MUNIT_OK;
}

/* mig_block_sub_valid_no_wrap — BAD / 已零计数块做减计数必须不下溢（MIG-03）。
 *
 * 覆盖：`ftl_block_sub_valid_if_live`。
 * 判据：BAD 或 count==0 时不变；正常块 −1；0 处不回绕成 0xFFFF。
 * 备注：纯函数，不碰介质。 */

static MunitResult test_mig_block_sub_valid_no_wrap(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    bst_block_s b;

    memset(&b, 0, sizeof(b));

    b.state       = FTL_BLOCK_STATE_CLOSED;
    b.valid_count = 0U;
    ftl_block_sub_valid_if_live(&b);
    munit_assert_uint16(b.valid_count, ==, 0U);

    b.state       = FTL_BLOCK_STATE_BAD;
    b.valid_count = 5U;
    ftl_block_sub_valid_if_live(&b);
    munit_assert_uint16(b.valid_count, ==, 5U);

    b.state       = FTL_BLOCK_STATE_CLOSED;
    b.valid_count = 3U;
    ftl_block_sub_valid_if_live(&b);
    munit_assert_uint16(b.valid_count, ==, 2U);

    return MUNIT_OK;
}

/* ══ 子套件 /ftl/negative/pure/：RAM/纯函数负例（不碰介质）══ */

/* read_column_fail_seam — D2 seam 证明：列读注入必须命中并返回 FTL_ERROR（BBT-02）。
 *
 * 覆盖：`FI_READ_COLUMN_FAIL` 在 `ftl_sync_read_column` 的 PBA 键。
 * 判据：`fi_hits>0` 且返回 FTL_ERROR；不依赖真 NAND 失败。
 * 备注：只验证 seam 可达，BBT 工厂扫描的降级判据另由 media 用例覆盖。 */

static MunitResult test_read_column_fail_seam(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const DWORD pba = pba_make(0U, 0U, 0U, 0U, 0U, 0U, 0U);
    BYTE        out[4];
    const DWORD hits0 = fi_hits(FI_READ_COLUMN_FAIL);

    memset(out, 0, sizeof(out));
    fi_arm_at(FI_READ_COLUMN_FAIL, pba, 1U);

    munit_assert_int(ftl_sync_read_column(pba, 2048U, out), ==, FTL_ERROR);
    munit_assert_uint32(fi_hits(FI_READ_COLUMN_FAIL), >, hits0);

    ftl_ut_trace_summary("d2 seam: read column fail hit pba=0x%08lX", pba);

    return MUNIT_OK;
}

/* cp_duplicate_completion_guard — 无在飞命令时重复/迟到完成必须被拒绝并释放（CP-02）。
 *
 * 覆盖：`ftl_checkpoint_callback` 在 `waiting==false` 的入口守卫。
 * 判据：不按当前 phase 再推进 chunk/游标；完成携带的命令被 release（池回到基线）。
 * 备注：debug 下入口 ASSERT 会停机 ⇒ `MUNIT_SKIP`；release 验证运行期拒绝。 */

static MunitResult test_cp_duplicate_completion_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    ftl_ctx_s   *ftl = ftl_ut_live();
    cp_handle_s *h   = ftl->cp_handle;

    fc_resource_stats_s before;

    fc_get_resource_stats(&before);

    const cp_phase_e phase0 = h->phase;

    munit_assert_false(h->waiting);

    fc_cmd_s *cmd = fc_command_alloc();

    munit_assert_not_null(cmd);

    fc_cmd_status_s st;

    memset(&st, 0, sizeof(st));
    st.cmd_id = cmd->cmd_id;

    ftl_checkpoint_callback(ftl, &st, h);

    fc_resource_stats_s after;

    fc_get_resource_stats(&after);

    munit_assert_int(h->phase, ==, phase0);
    munit_assert_uint16(after.command_used, ==, before.command_used);

    return MUNIT_OK;
#endif
}

/* sync_timeout_late_completion — 同步等待超时后迟到完成不得解引用 NULL ctx / 泄漏（IF-02/17）。
 *
 * 覆盖：`FI_COMPLETION_HOLD` 扣住读完成 → `ftl_sync_read_page` 超时返回 FTL_ERROR →
 *       `fi_wrap_release_held()` 放行迟到完成，`ftl_op` 的 missing-request 分支必须
 *       release 命令而不是崩/二次释放。
 * 判据：超时返回错误；放行并泵后 FC command 池回到基线；`fi_wrap_held_count()==0`。 */

static MunitResult test_sync_timeout_late_completion(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD pba = pba_make(0U, 0U, 0U, 0U, 0U, 0U, 0U);
    BYTE        buf[FTL_IU_BYTES];

    fc_resource_stats_s before;

    fc_get_resource_stats(&before);
    memset(buf, 0, sizeof(buf));

    fi_arm_at(FI_COMPLETION_HOLD, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, 1U);

    const ftl_status_e st = ftl_sync_read_page(pba, buf, sizeof(buf), FTL_PAGE_CFG_META());

    munit_assert_int(st, ==, FTL_ERROR);
    munit_assert_uint32(fi_hits(FI_COMPLETION_HOLD), >, 0U);
    munit_assert_uint32(fi_wrap_held_count(), ==, 1U);

    fi_wrap_release_held();

    for (DWORD i = 0U; i < 2000U; i++)
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);
    }

    /* 本轮泵会给后台发 credit，可能在接近结束时起 metadata 检查点/reclaim；
     * 判据要证的是「迟到完成释放了那条读命令」，不是「后台永远不干活」。
     * 先让后台稳定到空闲再比池基线：真正没被释放的命令不会因 quiesce 消失。 */
    const BOOL saved_suspend = ftl->flags.bg_suspend;

    ftl_ut_quiesce(ftl);
    ftl->flags.bg_suspend = saved_suspend;

    fc_resource_stats_s after;

    fc_get_resource_stats(&after);

    munit_assert_uint32(fi_wrap_held_count(), ==, 0U);
    munit_assert_uint16(after.command_used, ==, before.command_used);

    return MUNIT_OK;
}

/* cache_pad_early_exit_releases — pad 回调早退（序列已死）必须释放命令（CACHE-31）。
 *
 * 覆盖：`ftl_cache_pad_callback` 在 `kind != WRITE || closing == false` 的早退分支。
 * 判据：完成携带的命令被 release，FC command 池回到基线；不改 allocator 账本。 */

static MunitResult test_cache_pad_early_exit_releases(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s          *ftl = ftl_ut_live();
    fc_resource_stats_s before;

    fc_get_resource_stats(&before);

    ftl->cache_stream.seq.kind    = FTL_CACHE_SEQ_NONE;
    ftl->cache_stream.seq.closing = false;

    fc_cmd_s *cmd = fc_command_alloc();

    munit_assert_not_null(cmd);

    fc_cmd_status_s st;

    memset(&st, 0, sizeof(st));
    st.cmd_id = cmd->cmd_id;

    ftl_cache_pad_callback(ftl, &st, NULL);

    fc_resource_stats_s after;

    fc_get_resource_stats(&after);

    munit_assert_uint16(after.command_used, ==, before.command_used);

    return MUNIT_OK;
}

/* jrn_handle_busy_guard — job 在飞时 jrn_handle 不得同步刷页/二次编程（JN-04）。 */
static MunitResult test_jrn_handle_busy_guard(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_jrn_s journal;

    ftl_ut_ram_init(); /* RAM 分配器是跨用例全局：先归零再绑定 journal */

    memset(&journal, 0, sizeof(journal));
    jrn_init(&journal, ftl_ut_live()->topo, ftl_ut_ram_alloc());

    journal.handle.job.phase = JRN_WRITE;

    const jrn_phase_e phase0 = journal.handle.job.phase;

    jrn_handle(&journal, NULL);

    munit_assert_int(journal.handle.job.phase, ==, phase0);

    return MUNIT_OK;
}

/* mig_inflight_blocks_new_job — 已有未完成命令时不得启动新搬移 job（MIG-10）。
 *
 * 覆盖：`mig_handle`/`mig_gc_begin` 对 `state.inflight != 0` 的拒绝。
 * 判据：inflight=1 时 urgent GC 请求不产生 active job；恢复 inflight=0 后状态不被污染。 */

static MunitResult test_mig_inflight_blocks_new_job(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s    *ftl = ftl_ut_live();
    mig_handle_s *m   = ftl->mig_handle;

    munit_assert_not_null(m);

    const DWORD inflight0 = m->state.inflight;
    const BOOL  active0   = m->job.active;
    const BOOL  urgent0   = m->state.urgent_gc;
    const BOOL  paused0   = m->state.failure.paused;

    m->job.active           = false;
    m->state.failure.paused = false;
    m->state.urgent_gc      = false;
    m->state.inflight       = 1U;

    mig_gc_request_urgent(m);
    mig_handle(m, BG_LEVEL_IDLE);

    munit_assert_false(m->job.active);
    munit_assert_uint32(m->state.inflight, ==, 1U);

    m->state.inflight       = inflight0;
    m->job.active           = active0;
    m->state.urgent_gc      = urgent0;
    m->state.failure.paused = paused0;

    return MUNIT_OK;
}

/* l2p_orphan_dirty_bit — 孤儿实时脏位必须自愈并返回错误（L2P-19）。 */
static MunitResult test_l2p_orphan_dirty_bit(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_cache_s *cache  = ftl_ut_live()->cache;
    const DWORD  region = 0U;

    munit_assert_not_null(cache);

    /* 确保该 region 没有驻留行/CMT，只剩孤儿位。 */
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_line_info_s info;

        l2p_line_get(cache, i, &info);
        if ((info.state != L2P_CACHE_INVALID) && (info.region_id == region))
        {
            l2p_line_invalidate(cache, i);
        }
    }

    cache->region_dirty_bitmap[region >> 5U] |= (1UL << (region & 31U));
    cache->cmt_dirty_region_bitmap[region >> 5U] &= ~(1UL << (region & 31U));

    munit_assert_int(l2p_region_flush_kick(cache, region), ==, FTL_ERROR);
    munit_assert_false(l2p_cmt_region_dirty(cache, region));

    cache->region_dirty_bitmap[region >> 5U] &= ~(1UL << (region & 31U));

    return MUNIT_OK;
}

/* l2p_cmt_dirty_double_release — dirty 计数在 0 处不得静默吞账（L2P-22）。
 *
 * 覆盖：`l2p_cmt_dirty_release` 唯一释放闸门；debug 入口 ASSERT 会停机 ⇒ SKIP。
 * 判据：release 两次后两份计数都为 0，不出现负值/静默分叉。 */
static MunitResult test_l2p_cmt_dirty_double_release(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

#if !defined(NDEBUG)
    return MUNIT_SKIP;
#else
    l2p_cache_s *cache = ftl_ut_live()->cache;
    const DWORD  lba   = 0U;
    const BYTE   die   = lba_2_die(lba);

    cache->cmt_dirty_owner[die] = 1U;
    cache->stats.cmt_dirty      = 1U;

    l2p_test_cmt_dirty_release(cache, lba);
    munit_assert_uint32(cache->cmt_dirty_owner[die], ==, 0U);
    munit_assert_uint32(cache->stats.cmt_dirty, ==, 0U);

    l2p_test_cmt_dirty_release(cache, lba); /* 第二次：release 下 LOGE + 夹取，不回绕 */
    munit_assert_uint32(cache->cmt_dirty_owner[die], ==, 0U);
    munit_assert_uint32(cache->stats.cmt_dirty, ==, 0U);

    return MUNIT_OK;
#endif
}

/* l2p_invalid_dir_pba — Directory 的坏 PBA 必须拒绝，不进入 FC 路由（L2P-21）。 */
static MunitResult test_l2p_invalid_dir_pba(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s      *ftl    = ftl_ut_live();
    l2p_cache_s    *cache  = ftl->cache;
    md_dir_cache_s *d      = ftl->metadata_dir;
    const DWORD     lba    = 0U;
    const DWORD     region = lba_to_global_region(lba);
    const WORD      local  = l2p_global_to_local_region(region);
    const DWORD     saved  = md_entry_find(d, METADATA_TYPE_L2P_REGION, local);

    /* 让本次加载没有驻留行可命中；CMT 也旁路。 */
    l2p_cmt_set_enabled(cache, false);

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_line_info_s info;

        l2p_line_get(cache, i, &info);
        if (info.state != L2P_CACHE_INVALID)
        {
            l2p_line_invalidate(cache, i);
        }
    }

    /* 0xFFFFFFFF 是 NAND_PBA_INVALID（合法「未映射」），不是损坏。用一个非 INVALID
     * 但字段越界的 PBA 才能命中 l2p_load_begin 的 pba_is_valid 拒绝分支。
     * ⚠ 直接改 RAM 条目，不走 md_entry_update：后者会置 ckpt.dirty，l2p sync pump
     * 里的 md_handle 会顺带提交目录检查点命令，令下面的“FC 池不变”判据假红。 */
    munit_assert_uint16(local, <, d->count);
    munit_assert_uint8(d->entries[local].type, ==, METADATA_TYPE_L2P_REGION);
    munit_assert_uint16(d->entries[local].id, ==, local);
    d->entries[local].pba = 0xFFFFFFFEU;

    fc_resource_stats_s before;

    fc_get_resource_stats(&before);

    DWORD out = 0U;

    /* 损坏位置按加载失败重试/降级：最终给全 INVALID 的 CLEAN 行（host 路径据
     * region_degraded 报 IO_ERROR），但不提交任何 FC 读命令。 */
    munit_assert_int(l2p_read_sync(cache, lba, &out), ==, FTL_OK);
    munit_assert_uint32(out, ==, NAND_PBA_INVALID);
    munit_assert_true(l2p_is_region_degraded(cache, region));

    fc_resource_stats_s after;

    fc_get_resource_stats(&after);
    munit_assert_uint32(after.command_used, ==, before.command_used);
    munit_assert_uint32(after.descriptor_used, ==, before.descriptor_used);
    munit_assert_uint32(after.buffer_used, ==, before.buffer_used);
    munit_assert_uint16(ftl_op_req_count(), ==, 0U);

    /* 清掉本用例制造的降级位/降级行：它们是 cache 级 RAM 状态，会污染后续用例。 */
    cache->region_degraded_bitmap[region >> 5U] &= ~(1UL << (region & 31U));
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_line_info_s info;

        l2p_line_get(cache, i, &info);
        if ((info.state != L2P_CACHE_INVALID) && (info.region_id == region))
        {
            l2p_line_invalidate(cache, i);
        }
    }

    d->entries[local].pba = saved; /* 直接还原，避免再次置 ckpt.dirty */
    l2p_cmt_set_enabled(cache, true);

    return MUNIT_OK;
}

/* bg_busy_cfg_single_source — BUSY 档的调度配置必须发布到唯一 cfg（BG-06）。 */
static MunitResult test_bg_busy_cfg_single_source(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    pend_ctx_s *t   = &ftl->pending;
    BYTE        gi  = 0U;

    munit_assert_true(pend_group_alloc(t, &gi));

    /* 只验证选档 + cfg 发布；不跑任务表，避免这里启一个搬移 job 污染 teardown。 */
    bg_policy_test_publish(ftl);

    munit_assert_int(ftl->bg.level, ==, BG_LEVEL_BUSY);
    munit_assert_not_null(ftl->bg.cfg);
    munit_assert_uint32(ftl->bg.cfg->grant_units, ==, 1U);
    munit_assert_uint32(ftl->bg.cfg->task_mask, !=, 0U);

    memset(&t->groups[gi], 0, sizeof(t->groups[gi]));

    return MUNIT_OK;
}

/* bbt_metadata_pool_rebuild — 元数据池块 BAD 后候选池必须剔除并重建游标（BBT-12/15）。 */
static MunitResult test_bbt_metadata_pool_rebuild(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_dir_cache_s *d = ftl_ut_ram_md();
    alc_s          *a = d->alloc;

    munit_assert_not_null(d);
    munit_assert_not_null(a);
    munit_assert_true(md_pool_build_all(a));

    md_ring_s *ring  = &d->pools.bbt;
    const WORD block = ring->blocks[0];
    const WORD g     = ftl_gblock(0U, block);

    a->bbt.cursor_valid = true;

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);
    /* 标坏只置 pool dirty；游标由池重建单点作废，不能在 mark_bad 里各自清。 */
    munit_assert_true(d->pools.dirty);

    munit_assert_true(md_pool_build_all(a));
    munit_assert_false(a->bbt.cursor_valid);

    BOOL still_listed = false;

    for (WORD i = 0U; i < ring->block_count; i++)
    {
        if (ring->blocks[i] == block)
        {
            still_listed = true;
        }
    }

    munit_assert_false(still_listed);
    /* BBT ring 块属于固定元数据区：池重建即可自愈，不要求 full metadata rebuild
     * （rebuild_required 只给 Journal / L2P 日志块的丢失）。 */

    return MUNIT_OK;
}

/* l2p_fail_page_undo_precise — 失败页撤销只清该页 LBA，保留同 region 其他 dirty（L2P-15）。 */
static MunitResult test_l2p_fail_page_undo_precise(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_cache_s *cache  = ftl_ut_live()->cache;
    const DWORD  region = lba_to_global_region(0U);
    const DWORD  lba_a  = (region * L2P_ENTRIES_PER_REGION);
    const DWORD  lba_b  = lba_a + FTL_LBA_PER_PAIR;
    const BYTE   die    = lba_2_die(lba_a);

    l2p_cmt_entry_s *a = &cache->cmt[0][0];
    l2p_cmt_entry_s *b = &cache->cmt[0][1];

    memset(a, 0, sizeof(*a));
    memset(b, 0, sizeof(*b));
    a->lba       = lba_a;
    a->pba       = 0x1000U;
    a->state     = L2P_CMT_DIRTY;
    a->dirty_seq = 1U;
    b->lba       = lba_b;
    b->pba       = 0x2000U;
    b->state     = L2P_CMT_DIRTY;
    b->dirty_seq = 2U;

    const DWORD owner0 = cache->cmt_dirty_owner[die];
    const DWORD dirty0 = cache->stats.cmt_dirty;

    cache->cmt_dirty_owner[die] = 2U;
    cache->stats.cmt_dirty      = 2U;

    l2p_note_region_lost(cache, region, lba_a, 0xFFU);

    munit_assert_uint32(a->lba, ==, FTL_LBA_INVALID);
    munit_assert_uint32(b->lba, ==, lba_b);
    munit_assert_uint8(b->state, ==, L2P_CMT_DIRTY);
    munit_assert_uint32(cache->cmt_dirty_owner[die], ==, 1U);
    munit_assert_uint32(cache->stats.cmt_dirty, ==, 1U);

    /* 还原 live cache 的 RAM 状态。 */
    memset(a, 0, sizeof(*a));
    memset(b, 0, sizeof(*b));
    cache->cmt_dirty_owner[die] = owner0;
    cache->stats.cmt_dirty      = dirty0;
    cache->region_degraded_bitmap[region >> 5U] &= ~(1UL << (region & 31U));

    return MUNIT_OK;
}

/* l2p_replay_bypass_cmt — CMT 关闭时映射只写 line，不产生 dirty CMT（L2P-14）。 */
static MunitResult test_l2p_replay_bypass_cmt(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_cache_s *cache  = ftl_ut_live()->cache;
    const DWORD  region = 1U;
    const DWORD  lba    = region * L2P_ENTRIES_PER_REGION;
    DWORD        pba[1] = {0x1000U};

    WORD idx = L2P_CACHE_SIZE;

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        if (cache->tags[i].state == L2P_CACHE_INVALID)
        {
            idx = i;
            break;
        }
    }

    munit_assert_uint16(idx, <, L2P_CACHE_SIZE);

    const BOOL  enabled0 = cache->cmt_enabled;
    const DWORD dirty0   = cache->stats.cmt_dirty;
    const BYTE  state0   = cache->tags[idx].state;
    const DWORD rid0     = cache->tags[idx].region_id;

    cache->tags[idx].region_id    = region;
    cache->tags[idx].state        = L2P_CACHE_CLEAN;
    cache->tags[idx].access_count = 0U;
    memset(cache->lines[idx].entries, 0, sizeof(cache->lines[idx].entries));

    l2p_cmt_set_enabled(cache, false);
    munit_assert_int(l2p_entries_write(cache, lba, 0x01U, pba), ==, FTL_OK);

    munit_assert_uint8(cache->tags[idx].state, ==, L2P_CACHE_DIRTY);
    munit_assert_true(l2p_region_dirty_live(cache, region));
    munit_assert_uint32(cache->stats.cmt_dirty, ==, dirty0);

    /* 还原 live cache。 */
    cache->tags[idx].state        = state0;
    cache->tags[idx].region_id    = rid0;
    cache->tags[idx].access_count = 0U;
    cache->region_dirty_bitmap[region >> 5U] &= ~(1UL << (region & 31U));
    l2p_cmt_set_enabled(cache, enabled0);

    return MUNIT_OK;
}

/* l2p_cmt_capacity_precheck — CMT set 全 DIRTY 时 ready 预判必须拒绝整批（L2P-16）。 */
static MunitResult test_l2p_cmt_capacity_precheck(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_cache_s    *cache = ftl_ut_live()->cache;
    l2p_cmt_entry_s saved[L2P_CMT_WAYS];

    for (BYTE w = 0U; w < L2P_CMT_WAYS; w++)
    {
        saved[w] = cache->cmt[0][w];
        memset(&cache->cmt[0][w], 0, sizeof(cache->cmt[0][w]));
        cache->cmt[0][w].lba   = 1024UL * (DWORD)(w + 1U); /* set 0 */
        cache->cmt[0][w].pba   = 0x1000U + w;
        cache->cmt[0][w].state = L2P_CMT_DIRTY;
        cache->cmt[0][w].stamp = w;
    }

    munit_assert_false(l2p_test_cmt_room(cache, 0U));

    cache->cmt[0][0].lba = FTL_LBA_INVALID;
    munit_assert_true(l2p_test_cmt_room(cache, 0U));

    for (BYTE w = 0U; w < L2P_CMT_WAYS; w++)
    {
        cache->cmt[0][w] = saved[w];
    }

    return MUNIT_OK;
}

/* md_victim_skips_reserved — 压缩 victim 选择必须跳过在飞槽位块（MD-19）。 */
static MunitResult test_md_victim_skips_reserved(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_dir_cache_s *d  = ftl_ut_ram_md();
    alc_s          *a  = d->alloc;
    const WORD      g1 = ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 10U));
    const WORD      g2 = ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 11U));

    memset(d->slot_resv, 0xFF, sizeof(d->slot_resv)); /* 全 INVALID */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        d->cur.valid[p] = false;
    }

    a->blocks[g1].state = FTL_BLOCK_STATE_LOG;
    a->blocks[g2].state = FTL_BLOCK_STATE_LOG;
    d->l2p_live[g1]     = 2U;
    d->l2p_live[g2]     = 1U;

    munit_assert_uint16(md_test_compact_victim_find(d, a), ==, g2);

    /* 给 g2 挂一个在飞快照：选择必须退回 g1。 */
    d->slot_resv[0] = ftl_gblock_plane(g2) == 0U ? pba_make(0U, 0U, 0U, ftl_gblock_block(g2), 0U, 0U, 0U)
                                                 : pba_make(0U, 0U, 0U, ftl_gblock_block(g2), 1U, 0U, 0U);
    munit_assert_uint16(md_test_compact_victim_find(d, a), ==, g1);

    memset(d->slot_resv, 0xFF, sizeof(d->slot_resv));
    d->l2p_live[g1]     = 0U;
    d->l2p_live[g2]     = 0U;
    a->blocks[g1].state = FTL_BLOCK_STATE_NONE;
    a->blocks[g2].state = FTL_BLOCK_STATE_NONE;

    return MUNIT_OK;
}

/* md_compact_superseded_no_commit — 压缩 read→commit 间副本已移动必须丢弃新槽（MD-20）。 */
static MunitResult test_md_compact_superseded_no_commit(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s      *live     = ftl_ut_live();
    md_dir_cache_s *d        = ftl_ut_ram_md();
    md_dir_cache_s *saved_md = live->metadata_dir;

    live->metadata_dir = d;

    d->entries[0].type = METADATA_TYPE_L2P_REGION;
    d->entries[0].id   = 0U;
    d->entries[0].pba  = 0x2000U;
    d->count           = 1U;

    d->handle.phase      = MD_JOB_CP_WRITE;
    d->handle.waiting    = true;
    d->handle.cur_region = 0U;
    d->handle.cur_copy   = MD_COPY_PRIMARY;
    d->handle.cur_source = 0x1000U;
    d->handle.slot       = 0x3000U;
    d->handle.moved      = 0U;

    fc_cmd_s       *cmd = fc_command_alloc();
    fc_cmd_status_s st;

    munit_assert_not_null(cmd);
    memset(&st, 0, sizeof(st));
    st.cmd_id = cmd->cmd_id;

    ftl_metadata_callback(live, &st, d);

    munit_assert_uint32(d->entries[0].pba, ==, 0x2000U); /* 未被旧页覆盖 */
    munit_assert_int(d->handle.phase, ==, MD_JOB_CP_READ);
    munit_assert_uint16(d->handle.slot, ==, UINT16_MAX);
    munit_assert_uint16(d->handle.moved, ==, 0U);

    live->metadata_dir = saved_md;

    return MUNIT_OK;
}

/* md_dir_ckpt_dirty_rewrite — 写入期间 Directory 换代会保留 dirty，不得擦 victim（MD-18）。
 *
 * 覆盖：`md_dir_ckpt_commit` 的 covered_gen 判据。写入开始时快照 covered_gen=0，
 *       写完时 dirty_gen 已推进到 1 ⇒ commit 必须保留 dirty，下一轮重新写目录检查点。
 * 判据：commit 后 `ckpt.dirty` 仍为 true；victim 回收计数不变。
 * 备注：直接测 commit 契约，不驱动 md_handle 后续的异步写（RAM 夹具没有有效元数据池）。 */
static MunitResult test_md_dir_ckpt_dirty_rewrite(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_dir_cache_s *d = ftl_ut_ram_md();

    d->ckpt.dirty       = true;
    d->ckpt.dirty_gen   = 1U; /* 写入期间 Directory 又变了 */
    d->ckpt.covered_gen = 0U; /* 本 record 覆盖的是旧代 */
    d->ckpt.busy        = true;

    d->handle.dir_ckpt.ok                    = true;
    d->handle.dir_ckpt.idx                   = 0U;
    d->handle.dir_ckpt.chunk                 = DIR_CKPT_PAGE_COUNT;
    d->handle.dir_ckpt.ring.block_count      = 1U;
    d->handle.dir_ckpt.ring.pages_per_record = DIR_CKPT_PAGE_COUNT;
    d->reclaim.count                         = 0U;

    md_dir_ckpt_commit(d, &d->handle.dir_ckpt);

    munit_assert_true(d->ckpt.dirty);
    munit_assert_uint16(d->reclaim.count, ==, 0U);

    d->ckpt.dirty = false;

    return MUNIT_OK;
}

/* md_compact_reserved_budget — 压缩启动必须给在飞写预留槽位（MD-02）。 */
static MunitResult test_md_compact_reserved_budget(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    md_dir_cache_s *d      = ftl_ut_ram_md();
    alc_s          *a      = d->alloc;
    const WORD      victim = ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 12U));
    const WORD      curblk = ftl_gblock(0U, (WORD)(L2P_LOG_BLOCK0 + 13U));

    memset(d->slot_resv, 0xFF, sizeof(d->slot_resv));
    d->l2p_live[victim]     = 2U;
    a->blocks[victim].state = FTL_BLOCK_STATE_LOG;
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        d->cur.valid[p] = false;
        d->cur.page[p]  = 0U;
    }
    d->cur.valid[0] = true;
    d->cur.block[0] = curblk;
    d->cur.page[0]  = (BYTE)(PAGES_PER_BLOCK - 1U); /* 只剩 1 槽 */

    munit_assert_int(md_test_compact_begin(d, a), ==, MD_COMPACT_NO_SLOTS);

    d->cur.page[0] = 0U; /* 64 槽：够 live(2)+reserved(2) */
    munit_assert_int(md_test_compact_begin(d, a), ==, MD_COMPACT_STARTED);
    munit_assert_int(d->handle.phase, ==, MD_JOB_CP_READ);

    d->cur.valid[0]         = false;
    d->l2p_live[victim]     = 0U;
    a->blocks[victim].state = FTL_BLOCK_STATE_NONE;
    d->handle.phase         = MD_JOB_IDLE;

    return MUNIT_OK;
}

/* cache_release_sees_inflight — 写序列在飞成员未完成时不得干净清除（CACHE-36）。 */
static MunitResult test_cache_release_sees_inflight(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s       *ftl = ftl_ut_live();
    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    const BYTE kind0 = (BYTE)seq->kind;
    const BYTE gen0  = seq->gen;

    seq->kind        = FTL_CACHE_SEQ_WRITE;
    seq->gen         = 0U;
    seq->wr_inflight = 1U;
    seq->wr.count    = 0U;

    ftl_cache_seq_release(ftl);

    munit_assert_int(seq->kind, ==, FTL_CACHE_SEQ_NONE);
    munit_assert_uint8(seq->gen, ==, 1U);         /* abort 升 gen，迟到完成被丢弃 */
    munit_assert_uint8(seq->wr_inflight, ==, 1U); /* release 本身不改在飞计数 */

    seq->kind        = (ftl_cache_seq_kind_e)kind0;
    seq->gen         = gen0;
    seq->wr_inflight = 0U;

    return MUNIT_OK;
}

/* cache_pad_skips_proven — pad 收尾不得把已证明未确认页判失败（CACHE-30）。 */
static MunitResult test_cache_pad_skips_proven(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s       *ftl = ftl_ut_live();
    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    const BYTE kind0    = (BYTE)seq->kind;
    const BOOL closing0 = seq->closing;

    seq->kind              = FTL_CACHE_SEQ_WRITE;
    seq->closing           = true;
    seq->pad_verdict_valid = false;
    seq->wr.count          = 1U;
    seq->wr.head           = 0U;
    seq->wr.page[0].proven = true;
    seq->pad_pba[0]        = pba_make(0U, 0U, 0U, 1U, 0U, 1U, 0U); /* page1，避开 page0 屏障 */

    fc_cmd_s       *cmd = fc_command_alloc();
    fc_cmd_status_s st;

    munit_assert_not_null(cmd);
    memset(&st, 0, sizeof(st));
    st.cmd_id = cmd->cmd_id;

    ftl_cache_pad_callback(ftl, &st, NULL);

    munit_assert_true(seq->wr.page[0].proven);
    munit_assert_int(seq->kind, ==, FTL_CACHE_SEQ_NONE);
    munit_assert_false(seq->closing);

    seq->kind              = (ftl_cache_seq_kind_e)kind0;
    seq->closing           = closing0;
    seq->wr.count          = 0U;
    seq->wr.page[0].proven = false;

    return MUNIT_OK;
}

/* l2p_thrash_does_not_gate_flush — thrashing 不得挡住 CMT 必需写回（L2P-17）。 */
static MunitResult test_l2p_thrash_does_not_gate_flush(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_cache_s *cache = ftl_ut_live()->cache;
    const BYTE   die   = 0U;

    const BOOL  enabled0  = cache->cmt_enabled;
    const DWORD owner0    = cache->cmt_dirty_owner[die];
    const DWORD dirty0    = cache->stats.cmt_dirty;
    const BYTE  inflight0 = cache->inflight.flush;
    const BOOL  thrash0   = cache->thrash.thrashing;

    cache->cmt_enabled          = true;
    cache->cmt_dirty_owner[die] = 1U;
    cache->stats.cmt_dirty      = (L2P_CMT_SIZE * 3U) / 4U; /* L2P_CMT_WB_WATERMARK */
    cache->inflight.flush       = 0U;
    cache->thrash.thrashing     = true;

    munit_assert_true(l2p_test_owner_flush_ready(cache, die, false));

    cache->thrash.thrashing = false;
    munit_assert_true(l2p_test_owner_flush_ready(cache, die, false));

    cache->cmt_enabled          = enabled0;
    cache->cmt_dirty_owner[die] = owner0;
    cache->stats.cmt_dirty      = dirty0;
    cache->inflight.flush       = inflight0;
    cache->thrash.thrashing     = thrash0;

    return MUNIT_OK;
}

/* bbt_spare_refill_from_free — CP 快照并集 BAD 后 Spare 必须从 Free 补回（BBT-07）。 */
static MunitResult test_bbt_spare_refill_from_free(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_ram_init();
    alc_s *a = ftl_ut_ram_alloc();

    /* 清空 Spare，Free 放足量好块。 */
    a->spare_count[0] = 0U;
    for (WORD i = 0U; i < (WORD)(FTL_SPARE_POOL_PER_PLANE + 4U); i++)
    {
        alc_free_push(a, i);
    }

    munit_assert_true(alc_spare_refill_from_free(a, 0U));
    munit_assert_uint16(alc_spare_get_count(a, 0U), ==, FTL_SPARE_POOL_PER_PLANE);

    /* Free 不足时只补到实际数量，不越界/不虚报。 */
    a->spare_count[0] = 0U;
    for (WORD i = 0U; i < (WORD)(FTL_SPARE_POOL_PER_PLANE + 4U); i++)
    {
        alc_free_remove(a, i);
        a->blocks[i].state = FTL_BLOCK_STATE_NONE;
    }
    for (WORD i = 0U; i < 3U; i++)
    {
        alc_free_push(a, i);
    }

    munit_assert_true(alc_spare_refill_from_free(a, 0U));
    munit_assert_uint16(alc_spare_get_count(a, 0U), ==, 3U);

    return MUNIT_OK;
}

/* gc_empty_victim_journal_gate — 空 victim 也必须先过 Journal 闸门，不能直接擦（GC-18）。 */
static MunitResult test_gc_empty_victim_journal_gate(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s    *ftl = ftl_ut_live();
    mig_handle_s  m;
    DWORD         writes = 0U;
    super_block_s sb;

    /* RAM 分配器是跨用例复用的全局对象；不重置会把上一条用例留下的
     * valid_count 带进来，使「空 victim」前提在 debug 断言处直接失败。 */
    ftl_ut_ram_init();

    memset(&m, 0, sizeof(m));
    memset(&sb, 0, sizeof(sb));
    mig_init(&m, ftl_ut_ram_alloc(), ftl->journal, ftl->cache, &writes);

    sb.plane_blocks[0] = (WORD)(L2P_LOG_BLOCK0 + 20U);
    sb.next_page       = 0U;

    /* Journal 忙要在 job 起点之前置位：mig_job_start 末尾会立即 step 一次，
     * 空闲 Journal 会让 FLUSH_JRN 一步过掉，测试就看不到闸门。 */
    const jrn_phase_e phase0       = ftl->journal->handle.job.phase;
    ftl->journal->handle.job.phase = JRN_WRITE;

    mig_test_job_start(&m, &sb, MIG_KIND_GC, "test", 0U);
    munit_assert_int(m.job.phase, ==, MIG_FLUSH_JRN);

    mig_handle(&m, BG_LEVEL_IDLE);

    munit_assert_int(m.job.phase, ==, MIG_FLUSH_JRN);

    ftl->journal->handle.job.phase = phase0;

    return MUNIT_OK;
}

/* rr_gate_ignores_hosted_read — RR 托管读不得让序列永久 draining（RR-06）。 */
static MunitResult test_rr_gate_ignores_hosted_read(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s       *ftl   = ftl_ut_live();
    ftl_cache_seq_s *seq   = &ftl->cache_stream.seq;
    pend_ctx_s      *table = &ftl->pending;
    BYTE             gi    = 0U;

    munit_assert_true(pend_group_alloc(table, &gi));

    pend_req_s *req = pend_alloc(table, 0U);

    munit_assert_not_null(req);
    req->owners = ftl_group_bit(gi);
    req->op     = PENDING_OP_READ_RECOVERY;

    const BYTE kind0 = (BYTE)seq->kind;
    const BYTE gen0  = seq->gen;
    const WORD gi0   = seq->owner_gi;

    seq->kind     = FTL_CACHE_SEQ_READ;
    seq->gen      = 0U;
    seq->owner_gi = gi;

    ftl_cache_seq_release(ftl);

    munit_assert_false(seq->draining);

    seq->kind     = (ftl_cache_seq_kind_e)kind0;
    seq->gen      = gen0;
    seq->owner_gi = gi0;

    pend_release_index(table, (WORD)(req - table->slots));
    memset(&table->groups[gi], 0, sizeof(table->groups[gi])); /* 测试结束归还组槽 */

    return MUNIT_OK;
}

/* cache_pad_no_free_aborts — 盘满时 padding END 必须放弃序列而不是死锁（CACHE-15）。 */
static MunitResult test_cache_pad_no_free_aborts(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s       *ftl = ftl_ut_live();
    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    const BYTE kind0   = (BYTE)seq->kind;
    const BYTE gen0    = seq->gen;
    const BOOL active0 = seq->active;
    const BOOL clos0   = seq->closing;

    seq->kind        = FTL_CACHE_SEQ_WRITE;
    seq->gen         = 0U;
    seq->active      = true;
    seq->closing     = false;
    seq->wr_inflight = 1U;

    fi_arm(FI_ALLOC_FAIL, 1U);

    munit_assert_int(ftl_cache_test_pad_submit(ftl), ==, FTL_ERROR);
    munit_assert_int(seq->kind, ==, FTL_CACHE_SEQ_NONE);
    munit_assert_uint8(seq->gen, ==, 1U);

    fi_reset();
    seq->kind        = (ftl_cache_seq_kind_e)kind0;
    seq->gen         = gen0;
    seq->active      = active0;
    seq->closing     = clos0;
    seq->wr_inflight = 0U;

    return MUNIT_OK;
}

/* journal_truncate_commit_none — 本次未擦时 commit(NONE) 不得覆写 pre_erased（CP-01）。 */
static MunitResult test_journal_truncate_commit_none(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2p_jrn_s *j    = ftl_ut_live()->journal;
    const BYTE pre0 = j->handle.state.pre_erased_block;

    j->handle.state.pre_erased_block  = 2U;
    j->handle.state.emergency         = true;
    j->handle.state.checkpoint_needed = true;

    jrn_truncate_commit(j, JRN_BLOCK_NONE);

    munit_assert_uint8(j->handle.state.pre_erased_block, ==, 2U);
    munit_assert_false(j->handle.state.emergency);
    munit_assert_false(j->handle.state.checkpoint_needed);

    j->handle.state.pre_erased_block = pre0;

    return MUNIT_OK;
}

MunitTest g_negative_pure_tests[] = {
    {   (char *)"journal_truncate_commit_none",    test_journal_truncate_commit_none, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       (char *)"cache_pad_no_free_aborts",        test_cache_pad_no_free_aborts, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"gc_empty_victim_journal_gate",    test_gc_empty_victim_journal_gate, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"rr_gate_ignores_hosted_read",     test_rr_gate_ignores_hosted_read, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"bbt_spare_refill_from_free",      test_bbt_spare_refill_from_free, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { (char *)"l2p_thrash_does_not_gate_flush",  test_l2p_thrash_does_not_gate_flush, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"cache_pad_skips_proven",          test_cache_pad_skips_proven, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"cache_release_sees_inflight",     test_cache_release_sees_inflight, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"md_compact_reserved_budget",      test_md_compact_reserved_budget, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"md_dir_ckpt_dirty_rewrite",       test_md_dir_ckpt_dirty_rewrite, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"md_compact_superseded_no_commit", test_md_compact_superseded_no_commit, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       (char *)"md_victim_skips_reserved",        test_md_victim_skips_reserved, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"l2p_cmt_capacity_precheck",       test_l2p_cmt_capacity_precheck, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"l2p_replay_bypass_cmt",           test_l2p_replay_bypass_cmt, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"l2p_fail_page_undo_precise",      test_l2p_fail_page_undo_precise, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"bbt_metadata_pool_rebuild",       test_bbt_metadata_pool_rebuild, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"bg_busy_cfg_single_source",       test_bg_busy_cfg_single_source, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"l2p_cmt_dirty_double_release",    test_l2p_cmt_dirty_double_release, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {            (char *)"l2p_invalid_dir_pba",             test_l2p_invalid_dir_pba, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"l2p_orphan_dirty_bit",            test_l2p_orphan_dirty_bit, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"mig_inflight_blocks_new_job",     test_mig_inflight_blocks_new_job, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"jrn_handle_busy_guard",           test_jrn_handle_busy_guard, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"sync_timeout_late_completion",    test_sync_timeout_late_completion, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"cache_pad_early_exit_releases",   test_cache_pad_early_exit_releases, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"journal_seq_serial_compare",      test_journal_seq_serial_compare, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {               (char *)"rr_ecc_monotonic",                test_rr_ecc_monotonic, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"iface_pair_cmd_guards",           test_iface_pair_cmd_guards, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"read_column_fail_seam",           test_read_column_fail_seam, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"cp_duplicate_completion_guard",   test_cp_duplicate_completion_guard, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"op_req_table_guard",              test_op_req_table_guard, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {               (char *)"pend_table_guard",                test_pend_table_guard, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"alloc_rewind_underflow",          test_alloc_rewind_underflow, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"alloc_spare_skip_bad",            test_alloc_spare_skip_bad, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"wl_erase_resets_read_count",      test_wl_erase_resets_read_count, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              (char *)"wl_skip_bad_plane",               test_wl_skip_bad_plane, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"alloc_closed_list_full",          test_alloc_closed_list_full, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"alloc_closed_plane_bad_drop",     test_alloc_closed_plane_bad_drop, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"alloc_close_bad_pair_no_entry",   test_alloc_close_bad_pair_no_entry, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"wl_destination_prepare_fail",     test_wl_destination_prepare_fail, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"wl_snapshot_entry_trigger",       test_wl_snapshot_entry_trigger, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"rr_predictive_skip_bad_plane",    test_rr_predictive_skip_bad_plane, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    (char *)"mig_block_sub_valid_no_wrap",     test_mig_block_sub_valid_no_wrap, neg_pure_setup, neg_pure_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                                     NULL,                                 NULL,           NULL,              NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
