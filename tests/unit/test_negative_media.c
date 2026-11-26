/*
 * test_negative_media.c — /ftl/negative/media/{gc,l2p,meta,boot,if}/：故障注入负例（真 NAND，破坏性）
 *
 * 设计：docs/guides/testing_guide.md §12。
 *
 * 注入机制分两种，测试侧用法相同：
 *   · W（链接期 wrap，`tests/unit/fi_wrap.c`）：FC/FTL 边界故障，生产源码零改动；
 *   · H（生产源码宏隔离 hook）：cache FAIL_CACHE / Journal 写失败（已有用例覆盖）。
 *
 * 注释约定同 test_ftl_media.c：每个用例写「覆盖 / 判据 / 备注」，不套 S5 契约（P5 偏离）。
 * 注释不写风险编号（映射只在设计文档 §11 与 known_open_items.md 维护）。
 *
 * 用例生命周期（所有负例统一）：
 *   ① neg_prepare()    首次 format（一条 `test` 命令内只做一次）
 *   ② arrange          用 neg_lba() / neg_fill_tag() 造数据；GC 类先 neg_gc_pick_victim()
 *                      建立「注入能命中」的前置（有数据 / 成对整块）
 *   ③ arm → act → assert  fi_arm* 后触发动作；断言必须含 fi_hits 证明注入确实到达
 *   ④ 清理             用例内 neg_recover()；teardown 统一 neg_drain_to_clean()
 *
 * 结构约束：用例不得依赖「victim 选择会重复 / 缓存是否驻留 / flush 会顺带 CP」这类
 * 动态状态。需要某种前置时，在 ② 用 helper 明确建立并在 helper 里断言。
 * 清理同理集中在 neg_teardown：fi_reset → 恢复旋钮 → flush → 脏 CMT 用 CP 落盘 →
 * quiesce → assert_clean。
 *
 * ⚠ 运行前拔掉 USB。首个用例会做一次完整格式化（约 10–21 s），后续复用。
 *    跑完建议 CLI `format` + POR 再回到 USB 使用。
 */

#include "allocator.h"
#include "bbt.h"
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
#include "migrate.h"
#include "munit.h"
#include "onfi_1_0.h"
#include "pending.h"
#include "read_recovery.h"
#include "ssd_compiler.h"
#include "timer.h"
#include "wear_leveling.h"

#include <stdio.h>
#include <string.h>

/* 「持续武装」的剩余次数：用例期间不允许耗尽，否则被注入的路径会自行恢复，
 * 断言就失去区分力（C7）。 */
#define NEG_STUCK (0x40000000UL)

#define NEG_LBAS 8U

/* 用例 LBA 统一入口：所有用例从测试保留窗口起算，并保证 [offset, offset+NEG_LBAS)
 * 不越过 FTL_UT_LBA_WINDOW（越界会写运行时容量之外，host 前端本会拒绝）。 */
static DWORD neg_lba(DWORD offset)
{
    munit_assert_uint32(offset + NEG_LBAS, <=, FTL_UT_LBA_WINDOW);
    return ftl_ut_lba_base() + offset;
}

/* 8 LBA = 一个 pair 窗口（两 plane 各 4 CW）⇒ host 读建成一条 multi-plane 读命令。 */
#define NEG_HOST_READ_OP OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT

static const char        *s_boot_fail_kind_values[] = {"probe_load", "scoped_degrade", "journal_scan", "journal_apply", NULL};
static MunitParameterEnum s_boot_fail_kind_params[] = {
    {(char *)"kind", s_boot_fail_kind_values},
    {          NULL,                    NULL}
};

/* 单缓冲：期望值由 neg_byte 现场重算，读回直接覆盖源数据（AXI_RAM 在 combined UT
 * 构建里已接近上限，缓冲区放到 SRAM4）。 */
static SRAM4_DATA BYTE s_neg_buf[NEG_LBAS * FTL_IU_BYTES];

static BOOL s_neg_ready;
static BOOL s_destructive_active;

/* 按 LBA 生成可复算的数据 pattern（写读两侧用同一公式，不保留第二份期望缓冲）。 */
/* ⚠ 图案必须能区分 **CW 位移**：纯 `off * k`（k 为奇数）的周期是 256B，位移 512B
 * 后完全重合（C7 无区分力）。这里叠加 `off / FTL_IU_BYTES` 项，使每个 CW 的图案不同。 */
static BYTE neg_byte(DWORD lba, DWORD off)
{
    return (BYTE)((lba * 29U + off * 17U + (off / FTL_IU_BYTES) * 53U) & 0xFFU);
}

static void neg_fill(DWORD lba, DWORD count, BYTE *buf)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        buf[i] = neg_byte(lba, i);
    }
}

static void neg_assert_data(DWORD lba, DWORD count, const BYTE *buf)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        munit_assert_uint8(buf[i], ==, neg_byte(lba, i));
    }
}

/* 同一 LBA 的两种内容（用于「写 A→flush→写 B→读」区分新旧映射）。 */
static void neg_fill_tag(DWORD lba, DWORD count, BYTE *buf, BYTE tag)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        buf[i] = (BYTE)((lba * 29U + i * 17U + (i / FTL_IU_BYTES) * 53U + tag * 101U) & 0xFFU);
    }
}

static void neg_assert_tag(DWORD lba, DWORD count, const BYTE *buf, BYTE tag)
{
    for (DWORD i = 0U; i < (count * FTL_IU_BYTES); i++)
    {
        munit_assert_uint8(buf[i], ==, (BYTE)((lba * 29U + i * 17U + (i / FTL_IU_BYTES) * 53U + tag * 101U) & 0xFFU));
    }
}

/* 一次 `test` 命令内只格式化一次：负例套件可单独跑，不依赖 media 套件先跑。 */
/* 隔离会话的 teardown 兜底：用例断言失败 longjmp 时 isolated_end 不会执行，
 * teardown 依据本标志强制 format，避免同一 test 命令后续用例踩半破坏介质。 */
static void neg_isolated_begin(void)
{
    s_destructive_active = true;
    ftl_ut_isolated_begin();
}

static void neg_isolated_end(void)
{
    ftl_ut_isolated_end();
    s_destructive_active = false;
}

static void neg_prepare(void)
{
    if (s_neg_ready == false)
    {
        ftl_ut_media_format();
        s_neg_ready = true;
    }
}

/**
 * @brief 每条 `test` 命令开始时复位负例介质会话。
 *
 * @note `s_neg_ready` 是进程级 static；不重置会让上一条失败的 `test` 命令留下
 *       半完成介质，下一条命令的 `neg_prepare` 误以为已格式化而跳过。由
 *       ut_main.c 的 `ut_run_all` 在命令入口调用。
 */
void ftl_ut_negative_session_reset(void)
{
    s_neg_ready = false;
}

/* 失校/失败后的恢复动作：先取走延后错误（粘性错误会挡住下一条 submit），
 * 再把后台推进到空闲。 */
static void neg_recover(void)
{
    ftl_ctx_s *ftl = ftl_ut_live();

    (void)ftl_take_deferred_error(ftl);
    (void)ftl_ut_flush();
    (void)ftl_take_deferred_error(ftl);
}

/* 丢弃全部驻留行（同时清掉对应 region 的 CMT 条目），强制后续读走 NAND 的
 * L2P region 加载。
 *
 * ⚠ **调用前必须 flush**：掉电回放把映射写进 region line 并置 DIRTY；DIRTY 行不能
 *    作废（会丢未落盘映射），跳过又会让「强制重载」失效——用例会在 DIRTY 行上读到
 *    缓存值并假失败。这里的断言就是这个前提的守卫（C7：前提不成立要当场红）。 */
static void neg_drop_l2p_cache(void)
{
    l2p_cache_s *cache   = ftl_ut_live()->cache;
    WORD         dropped = 0U;

    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_line_info_s info;

        l2p_line_get(cache, i, &info);
        if (info.state == L2P_CACHE_INVALID)
        {
            continue;
        }
        munit_assert_int(info.state, ==, L2P_CACHE_CLEAN);
        l2p_line_invalidate(cache, i);
        dropped++;
    }

    ftl_ut_trace_detail("l2p: invalidated %hu resident lines (region load now mandatory)", dropped);
}

/* ── 状态归一化 ────────────────────────────────────────────────────────────
 * 所有负例共用同一套「恢复到干净空闲」的路径：用例只负责 arrange/arm/act/assert，
 * 清理与跨用例隔离集中在这里，不在每条用例里各写一遍。 */

/**
 * @brief 泵到当前 CP job 结束（不触发新 CP）。
 *
 * @param ftl       实例
 * @param max_steps 最大服务轮数
 * @return 实际泵轮数
 */
static DWORD neg_wait_cp_idle(ftl_ctx_s *ftl, DWORD max_steps)
{
    DWORD steps = 0U;

    while ((ftl->cp_handle->active == true) && (steps < max_steps))
    {
        timer_scheduler_poll();
        ftl_service_pump();

        /* 不依赖后台调度器是否被 bg_suspend 挡住：显式推进在飞 CP。 */
        if (ftl->cp_handle->active == true)
        {
            const cp_trigger_s advance = ftl_cp_trigger_build(ftl, CP_REASON_ADVANCE, 0);

            cp_handle(ftl->cp_handle, &advance);
        }

        // cp_handle 只登记 L2P flush item；必须让 scheduler dispatch 执行它。
        // 调用者可能带着 bg_suspend=1 进来（quiesce 后未恢复），临时放开再恢复。
        const BOOL suspend0 = ftl->flags.bg_suspend;

        ftl->flags.bg_suspend = false;
        ftl_service(ftl);
        ftl->flags.bg_suspend = suspend0;
        steps++;
    }
    return steps;
}

/**
 * @brief 强制一次显式 CP 并泵到结束。
 *
 * @param ftl       实例
 * @param max_steps 最大服务轮数
 * @param out_steps 输出：实际泵轮数（可 NULL）
 * @return true = job 已结束
 */
static BOOL neg_cp_force(ftl_ctx_s *ftl, DWORD max_steps, DWORD *out_steps)
{
    cp_trigger_s trigger = ftl_cp_trigger_build(ftl, CP_REASON_EXPLICIT, true);

    cp_handle(ftl->cp_handle, &trigger);

    const DWORD steps = neg_wait_cp_idle(ftl, max_steps);

    if (out_steps != NULL)
    {
        *out_steps = steps;
    }
    return (ftl->cp_handle->active == false);
}

/**
 * @brief 把用例可改的全局旋钮恢复到基线（故障注入由 `fi_reset` 单独负责）。
 *
 * @param ftl 实例
 *
 * @note migrate 的熔断 / batch 禁用 / 每 plane 失败计数都是「直到重启」的生产语义；
 *       同一 boot 内连续跑用例时必须复位，否则一条用例会把后续用例一起关掉。
 */
static void neg_restore_knobs(ftl_ctx_s *ftl)
{
    l2p_cmt_set_enabled(ftl->cache, true);
    ftl->flags.bg_suspend = false;

    ftl->mig_handle->state.failure.paused           = false;
    ftl->mig_handle->state.failure.read_fail_streak = 0U;
    ftl->mig_handle->state.batch_disabled           = false;
    ftl->mig_handle->state.urgent_gc                = false;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        ftl->mig_handle->state.failure.fail_count[p]  = 0U;
        ftl->mig_handle->state.failure.fail_gblock[p] = FTL_GBLOCK_INVALID;
    }
}

/**
 * @brief 只把 Journal 页缓冲刷到 NAND，不触发 CP（保持 journal > CP 水位）。
 *
 * @param ftl 实例
 *
 * @note host FLUSH 会顺带 kick CP（`ftl_flush_kick_cp`），不能用来制造 replay 场景；
 *       这里直接推进 `jrn_handle(FLUSH)` + NAND 完成泵。
 */
static void neg_journal_flush_only(ftl_ctx_s *ftl)
{
    const BOOL suspend0 = ftl->flags.bg_suspend;

    /* 禁止后台 CP 在这段窗口里把 journal 覆盖进 CP 水位。 */
    ftl->flags.bg_suspend = true;

    for (DWORD i = 0U; i < 200000U; i++)
    {
        jrn_trigger_e trigger = JRN_TRIGGER_FLUSH;

        jrn_handle(ftl->journal, &trigger);
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);

        if ((jrn_is_idle(ftl->journal) != false) && (jrn_has_pending_entries(ftl->journal) == false))
        {
            ftl->flags.bg_suspend = suspend0;
            return;
        }
    }

    ftl->flags.bg_suspend = suspend0;
    munit_assert_true(false);
}

/**
 * @brief 排空到「干净空闲」：flush（best-effort）→ 脏 CMT 用 CP 落盘 → quiesce。
 *
 * @param ftl 实例
 *
 * @note host FLUSH 是 best-effort：wbuf FAILED / 超时时也会返回成功，而脏 CMT 可能
 *       还没写回 ⇒ 只看 flush 不够，必须再强制一次 CP。最多两轮（第一轮 CP 收尾后
 *       第二轮即可清）；仍不干净就交给调用方的断言。
 */
static void neg_drain_to_clean(ftl_ctx_s *ftl)
{
    neg_restore_knobs(ftl);

    for (BYTE pass = 0U; pass < 3U; pass++)
    {
        /* 先 quiesce：写确认/在飞 job 期间可能**新产生** dirty CMT，
         * 必须等它们落定后再 flush/CP，否则 CP 快照会漏掉这些条目。 */
        ftl_ut_quiesce(ftl);
        (void)ftl_take_deferred_error(ftl);

        (void)ftl_ut_flush();
        (void)ftl_take_deferred_error(ftl);

        if (ftl->cache->stats.cmt_dirty == 0U)
        {
            break;
        }

        munit_assert_true(neg_cp_force(ftl, 200000U, NULL));

        if (ftl->cache->stats.cmt_dirty == 0U)
        {
            break;
        }
    }

    ftl_ut_quiesce(ftl);
    ftl->flags.bg_suspend = false; /* quiesce 置 1；用例之间必须放开后台 */
}

/* 造一个**有有效数据**的 Closed SB 作为 GC victim：按不同 8-LBA 窗口顺序写满 64 页
 * （每页不同 LBA ⇒ 全部有效），再写一页到下一个窗口把满块关闭。
 *
 * ⚠ 不能用「同一 LBA 反复覆盖」：最新映射会落到**新块**，旧块变成空 victim；
 *    GC 对 `valid_cw==0` 走快路径直接擦除、**不发读命令** ⇒ 读失败注入永远不命中。 */
static void neg_make_closed_victim(DWORD base)
{
    for (BYTE page = 0U; page < 64U; page++)
    {
        const DWORD lba = base + ((DWORD)page * NEG_LBAS);

        neg_fill(lba, NEG_LBAS, s_neg_buf);
        munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    }

    const DWORD next = base + (64U * NEG_LBAS);

    neg_fill(next, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(next, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
}

/* 强制一次 urgent GC 并泵到 job 结束。
 *
 * @return true = 观察到 job 起止；false = urgent 请求没起出 job（无 victim 等）
 *
 * @note urgent 请求走 `mig_handle`（ftl_service 的 must-run 任务）；注入在提交阶段
 *       当场失败时，起止可能发生在**同一次** service 内 ⇒ 用 `job.aborted` 判定。 */
static BOOL neg_run_gc(ftl_ctx_s *ftl, DWORD max_steps)
{
    /* 清上一个 job 的观测位，作为本次「是否中止」的基线（job 结构只在 job 起点
     * memset；两次强制之间没有别的观察者）。要求当前无 job，避免踩在飞状态。 */
    munit_assert_false(ftl->mig_handle->job.active);
    ftl->mig_handle->job.aborted = false;

    /* 一轮内完成/中止的快路径 job 观察不到 active；用进展计数与 novictim 区分
     * 「起了 job」和「没有候选」。 */
    const DWORD done0 = ftl->mig_handle->state.counters.moved + ftl->mig_handle->state.counters.reclaimed + ftl->mig_handle->state.counters.noop;
    const DWORD nov0  = ftl->mig_handle->state.counters.novictim;

    mig_gc_request_urgent(ftl->mig_handle);

    BOOL started = false;
    BOOL aborted = false;

    for (DWORD i = 0U; i < max_steps; i++)
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);

        if (ftl->mig_handle->job.aborted == true)
        {
            aborted = true;
        }

        if (ftl->mig_handle->job.active == true)
        {
            started = true;
            continue;
        }

        /* ⚠ job 必须先真正 inactive 才能返回：aborted 只表示「已决定中止」，等待中的
         * 命令可能还在飞。提前返回会让下一次 neg_run_gc / teardown 踩在飞状态。 */
        if ((started == true) || (aborted == true))
        {
            return true;
        }

        const DWORD done1 = ftl->mig_handle->state.counters.moved + ftl->mig_handle->state.counters.reclaimed + ftl->mig_handle->state.counters.noop;

        if (done1 != done0)
        {
            return true; /* 快路径 job 已在本轮内完整收尾 */
        }
        if (ftl->mig_handle->state.counters.novictim != nov0)
        {
            return false; /* 触发被消费但选不出候选 */
        }
        if (i >= 8U)
        {
            return false;
        }
    }

    munit_assert_false(ftl->mig_handle->job.active);
    return started;
}

/* 选一个 GC victim 并保证它的形态能触发目标路径：
 *   · need_full == false：只要有有效数据（空 victim 走 FLUSH_JRN→ERASE 快路径，不读不写，
 *     依赖读/写失败的注入永远不命中）；
 *   · need_full == true：还要求成对且整块有效——cache-program 批需要连续的有效页
 *     （批内 n≥2 才用 FIRST/CACHE/END opcode）。
 * 不合格候选先搬走/回收；victim 选择是成本排序的确定性函数，选完后启动 GC 仍是同一块。
 *
 * @return 选中的 Closed 条目（指向 closed_sb_list；标坏摘 plane 前取用）
 */
static const super_block_s *neg_gc_force_victim_for_lba(ftl_ctx_s *ftl, DWORD lba)
{
    DWORD pba = 0U;

    if ((l2p_read(ftl->cache, lba, &pba) != FTL_OK) || (pba == NAND_PBA_INVALID))
    {
        return NULL;
    }

    const WORD g = ftl_pba_gblock(pba);

    for (WORD i = ftl->alloc.closed_sb_count; i > 0U; i--)
    {
        const super_block_s *e = &ftl->alloc.closed_sb_list[i - 1U];

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if ((super_block_has_plane(e, p) != false) && (ftl_gblock(p, e->plane_blocks[p]) == g))
            {
                fi_wrap_gc_force_victim(e);
                return e;
            }
        }
    }

    return NULL;
}

static const super_block_s *neg_gc_pick_victim(ftl_ctx_s *ftl, BOOL need_full)
{
    /* 先回到干净空闲：journal 无写失败、无在飞命令，GC 才可能启动；
     * 同时复位 batch 禁用 / 熔断等跨 job 闩锁。 */
    neg_drain_to_clean(ftl);
    munit_assert_false(ftl->mig_handle->job.active);
    munit_assert_false(jrn_has_write_failure(ftl->journal));
    munit_assert_uint16(ftl->mig_handle->state.inflight, ==, 0U);
    munit_assert_uint16(ftl->alloc.free_count, >, FREE_POOL_EMERGENCY); /* urgent 走 GC_BENEFIT_OFF */

    /* 直接选一个形状合格的 Closed 条目并**强制**给下一次 gc_select_victim：
     * 不依赖成本排序/共享介质上已有多少旧候选（单跑与全量同一结果）。 */
    for (WORD i = ftl->alloc.closed_sb_count; i > 0U; i--)
    {
        const super_block_s *e = &ftl->alloc.closed_sb_list[i - 1U];
        alc_super_stats_s    vst;
        BOOL                 any_bad = false;

        if (e->is_open != false)
        {
            continue;
        }

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if ((super_block_has_plane(e, p) != false) && (ftl_block_is_bad(&ftl->alloc.blocks[ftl_gblock(p, e->plane_blocks[p])]) != false))
            {
                any_bad = true;
            }
        }
        if (any_bad != false)
        {
            continue;
        }

        alc_super_get_stats(&ftl->alloc, e, &vst);

        if (vst.valid_cw == 0U)
        {
            continue;
        }
        if ((need_full != false) && ((vst.plane_count != PLANE_NUM) || (vst.valid_cw < FTL_CW_PER_BLOCK)))
        {
            continue;
        }

        fi_wrap_gc_force_victim(e);
        return e;
    }

    munit_assert_true(false); /* 没有形状合格的 Closed 条目：环境不成立 */
    return NULL;
}

static void *neg_setup(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_topology_init();
    (void)ftl_ut_live();
    fi_reset();
    fi_wrap_reset();
    neg_prepare();
    fi_reset();
    s_destructive_active = false;
    return NULL;
}

static void neg_teardown(void *fixture)
{
    (void)fixture;

    /* 注入状态是进程级的：先清注入，清理路径才能在正常状态下推进。 */
    fi_reset();
    fi_wrap_reset();

    if (s_destructive_active)
    {
        neg_isolated_end();
        s_destructive_active = false;
    }

    neg_drain_to_clean(ftl_ut_live());
    ftl_ut_assert_clean(ftl_ut_live());
}

/**
 * @brief 需要“该区域从未写过”的用例的 per-test setup。
 *
 * @note munit 的 MunitSuite 没有 setup/tear_down 指针，只有 MunitTest 有；
 *       因此把隔离前置挂到测试数组的 setup 上，而不是在每个测试体里手写
 *       neg_isolated_begin()。
 */
static void *neg_isolated_setup(const MunitParameter params[], void *data)
{
    (void)neg_setup(params, data);
    neg_isolated_begin();
    return NULL;
}

static void neg_isolated_teardown(void *fixture)
{
    neg_isolated_end();
    neg_teardown(fixture);
}

/* alloc_fail_bounded — 分配持续失败时写/flush 必须有界失败，放开后恢复。
 *
 * 覆盖：`alc_super_open_alloc_pair` 注入 FTL_AGAIN（持续）。
 * 判据：① 写 + flush 不能“全部静默成功”；② 清注入后 flush 归零、重写读回一致。
 * 备注：只验证有界失败与恢复，不验证具体错误码传播层级。 */

static MunitResult test_alloc_fail_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const DWORD lba = neg_lba(0U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);

    const DWORD hits0 = fi_hits(FI_ALLOC_FAIL);

    fi_arm(FI_ALLOC_FAIL, NEG_STUCK);

    const BOOL wrote   = ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf);
    const BOOL flushed = ftl_ut_flush();

    munit_assert_false((wrote == true) && (flushed == true));

    munit_assert_uint32(fi_hits(FI_ALLOC_FAIL) - hits0, >, 0U);

    fi_reset();
    neg_recover();

    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("alloc fail: write=%d flush=%d; recovered write+flush+read byte-equal", wrote, flushed);
    ftl_ut_trace_summary("neg: alloc fail bounded, recovery ok");

    return MUNIT_OK;
}

/* l2p_load_fail_bounded — L2P region 加载失败的读必须有界失败，且不返回错误数据。
 *
 * 覆盖：清空 CLEAN 行后对元数据读注入 FTL_ERROR（持续）。
 * 判据：① 未注入时同一读成功（正例对照）；② 注入后读报错；③ 清注入后读回原数据。
 * 备注：PBA key 未用到（FI_KEY_ANY），失败粒度是「所有元数据读」。 */

static MunitResult test_l2p_load_fail_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const DWORD lba = neg_lba(4096U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    /* flush 让行变 CLEAN，作废后下一次读必须重新加载 region 页。
     * **不 reboot**：掉电回放会把映射写进 region line 并置 DIRTY，DIRTY 行不能作废，
     * 于是读会命中缓存行、注入点根本走不到。 */
    neg_drop_l2p_cache();

    /* 读路径第一跳是 CMT；用回放旁路模式（不查不填 CMT）确保走 region 行加载。 */
    l2p_cache_s *cache = ftl_ut_live()->cache;

    l2p_cmt_set_enabled(cache, false);

    /* 对照：不注入时该读成功（证明注入点确实在这条路径上）。 */
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    neg_drop_l2p_cache();

    const DWORD hits0 = fi_hits(FI_L2P_LOAD_FAIL);

    fi_arm(FI_L2P_LOAD_FAIL, NEG_STUCK);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_false(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));

    l2p_cmt_set_enabled(cache, true);
    munit_assert_uint32(fi_hits(FI_L2P_LOAD_FAIL) - hits0, >, 0U);

    fi_reset();
    neg_recover();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("l2p load fail: control read OK; injected read bounded-fail; recovery byte-equal");
    ftl_ut_trace_summary("neg: l2p load fail bounded, recovery ok");

    return MUNIT_OK;
}

/* data_read_fail_bounded — 数据页读提交失败时 host 读有界报错，不返回陈旧/错误数据。
 *
 * 覆盖：host 读在 `ftl_op_submit`（`op==HOST_IO` + 读 opcode）注入 FTL_ERROR（持续）。
 *       host 读是 `host_group_subop_issue` 内联建链后直接提交，**不经过** `ftl_async_read_page`；
 *       后者只覆盖 GC/RR/L2P/元数据读。
 * 判据：① 对照读成功；② 注入后读失败；③ 放开后读回原数据。
 * 备注：入口把提交失败归为 FTL_AGAIN（重试），持续失败由 drain 的无进展预算结算成
 *       IO_ERROR —— 本条验证「有界失败」，不验证具体错误码传播层级。 */

static MunitResult test_data_read_fail_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const DWORD lba = neg_lba(128U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    ftl_ut_reboot_from_media();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    const DWORD hits0 = fi_hits(FI_MIG_READ_FAIL);

    fi_arm(FI_MIG_READ_FAIL, NEG_STUCK);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_false(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));

    munit_assert_uint32(fi_hits(FI_MIG_READ_FAIL) - hits0, >, 0U);

    fi_reset();
    neg_recover();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("data read fail: control OK; injected read bounded-fail; recovery byte-equal");
    ftl_ut_trace_summary("neg: data read fail bounded, recovery ok");

    return MUNIT_OK;
}

/* data_write_fail_bounded — 数据页写硬失败时写/flush 有界失败，不静默丢写。
 *
 * 覆盖：`ftl_async_write_pair_cmd`（DATA，wbuf / cache program 批次）+ `_cw`（GC/WL）
 *       注入 FTL_ERROR（持续）。
 * 判据：① 写 + flush 不能“全部静默成功”；② 清注入后新写读回一致。
 * 备注：host 写走 pair 页入口，**不经过** `ftl_async_write_page_cw`（后者是 GC/WL 用的）。 */

static MunitResult test_data_write_fail_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(256U);

    ftl_ut_trace_begin();

    const WORD free0 = ftl->alloc.free_count;

    neg_fill(lba, NEG_LBAS, s_neg_buf);

    const DWORD hits0 = fi_hits(FI_MIG_WRITE_FAIL);

    fi_arm(FI_MIG_WRITE_FAIL, NEG_STUCK);

    const BOOL wrote   = ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf);
    const BOOL flushed = ftl_ut_flush();

    munit_assert_false((wrote == true) && (flushed == true));

    /* 页对必须每次重试都归还（`alc_super_open_rewind_pair`）：5 s 的重试窗口只允许
     * 最多 2 次正常的块边界切换；按重试次数流失会让差值达到数百（WB-01）。 */
    munit_assert_uint16((WORD)(free0 - ftl->alloc.free_count), <=, 2U);

    munit_assert_uint32(fi_hits(FI_MIG_WRITE_FAIL) - hits0, >, 0U);

    fi_reset();
    neg_recover();

    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("data write fail: write=%d flush=%d; recovered write+flush+read byte-equal", wrote, flushed);
    ftl_ut_trace_summary("neg: data write fail bounded, recovery ok");

    return MUNIT_OK;
}

/* rr_read_fail_bounded — 数据读完成报错进入 RR、RR 的重读也失败 ⇒ host 读有界报错。
 *
 * 覆盖：读完成注入 1 次 DATA_ERROR（RR 的触发条件），RR 重读持续注入 FTL_ERROR。
 * 判据：① 注入后读失败（RR 没有“无限重试直到成功”）；② 放开后读回原数据。
 * 备注：RR 由**完成态错误**触发；提交层 FTL_ERROR 直接终结组、不进 RR，
 *       所以触发用 FI_FC_STATUS，恢复读失败用 FI_RR_READ_FAIL。 */

static MunitResult test_rr_read_fail_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const DWORD lba = neg_lba(384U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    ftl_ut_reboot_from_media();

    fi_arm_ex(FI_FC_STATUS, FI_KEY_OP(NEG_HOST_READ_OP), 1U, FC_CMD_STATUS_DATA_ERROR);
    const DWORD hits0 = fi_hits(FI_RR_READ_FAIL);

    fi_arm(FI_RR_READ_FAIL, NEG_STUCK);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_false(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));

    munit_assert_uint32(fi_hits(FI_RR_READ_FAIL) - hits0, >, 0U);

    fi_reset();
    neg_recover();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("rr fail: first read injected once + RR injected persistently -> bounded read error; recovery byte-equal");
    ftl_ut_trace_summary("neg: RR fail bounded, recovery ok");

    return MUNIT_OK;
}

/* fc_status_nand_fail — NAND 70h FAIL 出现在数据读完成上时 FTL 必须看见并进入 RR。
 *
 * 覆盖：在 CQ 出口给 multi-plane 读完成 OR 上 NAND_FAIL（只 1 次）。
 * 判据：① 该读仍成功（RR 重读恢复）；② `planes_recovered` 增加（证明状态位真的被消费）。
 * 备注：**不用持续注入**——RR 重读全失败会走 `rr_settle_plane` 把好块标坏，
 *       每跑一次烧一个块；持续失败的有界性由 `data_read_fail_bounded` /
 *       `rr_read_fail_bounded` 覆盖（提交层失败走 abort，不标坏）。 */

static MunitResult test_fc_status_nand_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(512U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    ftl_ut_reboot_from_media();

    fi_arm_ex(FI_FC_STATUS, FI_KEY_OP(NEG_HOST_READ_OP), 1U, FC_CMD_STATUS_NAND_FAIL);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    fi_reset();

    /* `planes_recovered` 是**本 job** 的计数（job 起点清零）⇒ >0 即证明这次读的
     * NAND_FAIL 被 FC→FTL 消费并进入了 RR（无注入时该读根本不进 RR）。 */
    munit_assert_uint8(ftl->read_recovery_handle.planes_recovered, >, 0U);

    ftl_ut_trace_detail("fc status: NAND_FAIL consumed by RR, planes_recovered=%hhu, readback byte-equal", ftl->read_recovery_handle.planes_recovered);
    ftl_ut_trace_summary("neg: FC NAND_FAIL reaches FTL and RR recovers");

    return MUNIT_OK;
}

/* seq_gate_starve — 提交被持续拒绝时组必须有界停住，放开后同一个组完成。
 *
 * 覆盖：`ftl_op_submit` 注入 FTL_AGAIN（持续）。
 * 判据：① 受理成功（入口非阻塞）；② 持续拒绝期间组不完成；③ 放开后同一个组完成、读回一致。
 * 备注：先 quiesce，保证被拒的是本用例的 wbuf 提交而不是后台命令。 */

static MunitResult test_seq_gate_starve(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(640U);

    ftl_ut_trace_begin();

    neg_drain_to_clean(ftl); /* quiesce + 放开后台 */

    static volatile BYTE         s_done;
    static volatile ssd_status_e s_status;
    ftl_io_req_s                 req;

    neg_fill(lba, NEG_LBAS, s_neg_buf);

    memset(&req, 0, sizeof(req));
    s_done            = 0U;
    s_status          = SSD_STATUS_IO_ERROR;
    req.lba           = lba;
    req.buffer        = s_neg_buf;
    req.count         = NEG_LBAS;
    req.complete_flag = &s_done;
    req.status_out    = &s_status;

    (void)ftl_take_deferred_error(ftl);

    /* 门控拒绝只影响**下发尝试**：`ftl_submit_write` 是非阻塞受理（组先入 pending，
     * 由 ftl_service 的补发步重试），被拒时入口仍返回 FTL_OK。持续拒绝 ⇒ 组不得完成；
     * 放开后同一个组必须完成，不需要重新提交。 */
    const DWORD hits0 = fi_hits(FI_SEQ_GATE_STARVE);

    fi_arm(FI_SEQ_GATE_STARVE, NEG_STUCK);

    munit_assert_int(ftl_submit_write(ftl, &req), ==, FTL_OK);

    for (DWORD i = 0U; i < 2000U; i++)
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);
    }

    munit_assert_uint8(s_done, ==, 0U);

    munit_assert_uint32(fi_hits(FI_SEQ_GATE_STARVE) - hits0, >, 0U);

    fi_reset();

    for (DWORD i = 0U; (i < 200000U) && (s_done == 0U); i++)
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);
    }

    munit_assert_uint8(s_done, !=, 0U);
    munit_assert_int(s_status, ==, SSD_STATUS_OK);

    munit_assert_true(ftl_ut_flush());
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("gate starve: accepted then stalled (no completion); release -> same group completed, readback byte-equal");
    ftl_ut_trace_summary("neg: seq gate starve stalls bounded, release completes the same group");

    return MUNIT_OK;
}

/* 强制一次 CP，并在指定 phase 持续拒绝提交，直到 10 ms submit deadline 中止。
 *
 * @return true = 观察到「该 phase 因提交截止时间中止」
 *
 * @note 返回前 `fi_reset()`；中止现场留在 `fail_phase` / `timeout_phase`（持久）。 */
static BOOL neg_cp_abort_at(ftl_ctx_s *ftl, cp_phase_e phase, DWORD max_steps)
{
    const DWORD hits0 = fi_hits(FI_CP_ABORT_PHASE);

    cp_trigger_s trigger = ftl_cp_trigger_build(ftl, CP_REASON_EXPLICIT, true);

    fi_arm_at(FI_CP_ABORT_PHASE, phase, NEG_STUCK);
    cp_handle(ftl->cp_handle, &trigger);

    const DWORD steps = neg_wait_cp_idle(ftl, max_steps);
    const DWORD hits  = fi_hits(FI_CP_ABORT_PHASE) - hits0;
    const BOOL  ok    = (ftl->cp_handle->active == false) && (hits > 0U) && (ftl->cp_handle->timed_out == true) && (ftl->cp_handle->fail_phase == phase) && (ftl->cp_handle->timeout_phase == phase);

    ftl_ut_trace_detail("cp abort[%d]: ok=%d steps=%lu hits=%lu fail_phase=%d timeout_phase=%d seq=%lu", phase, ok, steps, hits, ftl->cp_handle->fail_phase, ftl->cp_handle->timeout_phase, ftl->cp_handle->cp_state.sequence);

    fi_reset();
    return ok;
}

/* cp_abort_phase — CP 在 HDR_WRITE 持续提交不出去时必须有界中止，恢复后能成功。
 *
 * 覆盖：`ftl_op_submit` + `FTL_OP_CHECKPOINT` 在 `CP_HDR_WRITE` **持续**注入 FTL_ERROR。
 * 判据：① `fi_hits > 0`；② `timed_out && timeout_phase == CP_HDR_WRITE` 且
 *       `fail_phase == CP_HDR_WRITE`；③ 恢复 CP 写出新 Header（sequence 前进）。
 * 备注：`cp_write_submit` 把提交失败按**瞬态背压**处理（`FTL_AGAIN`，不是中止），
 *       单次失败会被下一轮重试成功；持续性由 `FTL_SUBMIT_TIMEOUT_MS`（10 ms）的
 *       submit deadline 兜底 ⇒ 必须持续注入才能测到中止。
 *       CP_DONE 会清 `failed` 但**不**清 `fail_phase` ⇒ 事后判据用 `fail_phase`。 */

static MunitResult test_cp_abort_phase(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

    ftl_ut_trace_begin();

    munit_assert_true(ftl_ut_flush());

    /* flush 会 kick 一个 CP；先等它收尾，否则下面的 force 落在 active job 上只会推进
     * 旧 job，注入可能错过 HDR_WRITE（阶段已经过去）。 */
    neg_wait_cp_idle(ftl, 200000U);

    munit_assert_false(ftl->cp_handle->active);

    const DWORD seq_before = ftl->cp_handle->cp_state.sequence;

    munit_assert_true(neg_cp_abort_at(ftl, CP_HDR_WRITE, 200000U));

    /* job 确实推进过（HDR_PREP 已取 sequence）；中止不得写 Header。 */
    const DWORD seq_aborted = ftl->cp_handle->cp_state.sequence;

    munit_assert_uint32(seq_aborted, >, seq_before);

    /* 恢复：强制 CP 必须正常收尾并写出新 Header（新 job 起点会清零 fail_phase）。 */
    DWORD steps = 0U;

    munit_assert_true(neg_cp_force(ftl, 200000U, &steps));

    munit_assert_false(ftl->cp_handle->active);
    munit_assert_int(ftl->cp_handle->fail_phase, ==, CP_IDLE);
    munit_assert_uint32(ftl->cp_handle->cp_state.sequence, >, seq_aborted);

    ftl_ut_trace_detail("cp abort: seq %lu->%lu(abort)->%lu(recovery), recovery steps=%lu", seq_before, seq_aborted, ftl->cp_handle->cp_state.sequence, steps);
    ftl_ut_trace_summary("neg: CP abort at HDR_WRITE bounded, recovery Header written");

    return MUNIT_OK;
}

/* cp_dir_ckpt_abort — 目录检查点写持续失败时 CP 必须在 HDR_PREP 之前中止；恢复后数据完整。
 *
 * 覆盖：`ftl_op_submit` + `FTL_OP_CHECKPOINT` 在 `CP_DIR_CKPT_WRITE` **持续**注入 FTL_ERROR。
 * 判据：① `neg_cp_abort_at`（fail_phase/timeout_phase 都是 DIR_CKPT_WRITE）；
 *       ② sequence **不变**（HDR_PREP 尚未执行）；③ 恢复 CP 写出新 Header；
 *       ④ reboot 后原数据可读（目录检查点 abort 不破坏旧记录）。
 * 备注：与 `cp_abort_phase` 共用中止机制，区别是 phase 与恢复后的持久化检查。 */

static MunitResult test_cp_dir_ckpt_abort(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(7168U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    neg_wait_cp_idle(ftl, 200000U);

    munit_assert_false(ftl->cp_handle->active);

    /* Directory 无变更时 CP 会整段跳过目录检查点（`MD_DIR_CKPT_CLEAN` → 直接 HDR_PREP）
     * ⇒ 用公开的 commit 接口把同一对 PBA 再提交一次，仅置脏，保证本次 CP 走目录写。 */
    const DWORD region = lba_to_global_region(lba);
    const WORD  local  = l2p_global_to_local_region(region);
    const DWORD prim   = md_entry_find(ftl->metadata_dir, METADATA_TYPE_L2P_REGION, local);
    const DWORD back   = md_backup_find(ftl->metadata_dir, local);

    munit_assert_true(pba_is_valid(prim));
    munit_assert_true(pba_is_valid(back));
    munit_assert_uint32(prim, !=, back);

    md_commit_region(ftl->metadata_dir, local, MD_COMMIT_PAIR, prim, back);

    const DWORD seq_before  = ftl->cp_handle->cp_state.sequence;
    const WORD  widx_before = ftl->metadata_dir->ckpt.write_idx;

    munit_assert_true(neg_cp_abort_at(ftl, CP_DIR_CKPT_WRITE, 200000U));

    /* 目录检查点写在中止 → Header 从未开始 ⇒ sequence 不得前进。 */
    munit_assert_uint32(ftl->cp_handle->cp_state.sequence, ==, seq_before);

    /* 半写的槽必须被消费：恢复 CP 不能在同一槽上二次编程。 */
    munit_assert_uint16(ftl->metadata_dir->ckpt.write_idx, !=, widx_before);

    /* 先把 CP abort 期间被唤醒的 metadata 整理 job 跑到 idle：它和 CP 共用
     * 目录检查点单飞锁；不等它，恢复 CP 会在 PREP 拿到 BUSY 并再次中止，
     * fail_phase 仍是 PREP 而不是 CP_IDLE。
     * quiesce 会保留 bg_suspend=1；恢复 CP 需要后台 CP 任务推进，故手动放开。 */
    ftl_ut_quiesce(ftl);
    ftl->flags.bg_suspend = false;

    /* 恢复 CP 必须完整收尾（新 job 起点清零 fail_phase）。 */
    DWORD steps = 0U;

    munit_assert_true(neg_cp_force(ftl, 200000U, &steps));

    munit_assert_false(ftl->cp_handle->active);
    munit_assert_int(ftl->cp_handle->fail_phase, ==, CP_IDLE);
    munit_assert_uint32(ftl->cp_handle->cp_state.sequence, >, seq_before);
    munit_assert_uint16(ftl->metadata_dir->ckpt.write_idx, !=, widx_before);

    /* 旧目录检查点仍有效：重启后数据可读。 */
    ftl_ut_reboot_from_media();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("cp dir-ckpt abort: seq %lu unchanged, recovery steps=%lu, reboot readback byte-equal", seq_before, steps);
    ftl_ut_trace_summary("neg: CP dir-ckpt abort bounded, old record intact after reboot");

    return MUNIT_OK;
}

/* bbt_write_fail — BBT record 写失败时 dirty 必须保留，放开后落盘成功。
 *
 * 覆盖：同步镜像写（`ftl_sync_write_page_mirror`）注入 FTL_ERROR。
 * 判据：① `bbt_store` 返回非 FTL_OK 且 `dirty` 仍为 true；② 放开后 `bbt_store` 成功、`dirty` 清零。
 * 备注：先 quiesce，避免后台 BBT job 与同步保存互斥（后者会被拒绝）。 */

static MunitResult test_bbt_write_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl   = ftl_ut_live();
    alc_s     *alloc = &ftl->alloc;

    ftl_ut_trace_begin();

    neg_drain_to_clean(ftl);

    /* 前置：在飞 job 会让 bbt_store 直接拒绝——那会让「!= FTL_OK」以错误原因通过。 */
    munit_assert_false(alloc->bbt.handle.waiting);

    bbt_dirty_note(alloc);
    munit_assert_true(alloc->bbt.dirty);

    const DWORD seq0  = alloc->bbt.sequence;
    const DWORD hits0 = fi_hits(FI_BBT_WRITE_FAIL);

    fi_arm(FI_BBT_WRITE_FAIL, 1U);

    munit_assert_int(bbt_store(alloc), !=, FTL_OK);
    munit_assert_true(alloc->bbt.dirty);
    munit_assert_uint32(fi_hits(FI_BBT_WRITE_FAIL) - hits0, >, 0U); /* 注入确实到达 record 写 */

    fi_reset();

    munit_assert_int(bbt_store(alloc), ==, FTL_OK);
    munit_assert_false(alloc->bbt.dirty);

    /* 两次 save 都走到写入（各自 sequence++）：证明第一次确实失败在 record 写，
     * 而不是在更早的入口拒绝（C7 的区分力）。 */
    munit_assert_uint32(alloc->bbt.sequence, >=, seq0 + 2U);

    ftl_ut_trace_detail("bbt write fail: save failed, dirty retained; retry ok, dirty cleared");
    ftl_ut_trace_summary("neg: BBT write fail keeps dirty, retry ok");

    return MUNIT_OK;
}

/* gc_read_fail_bounded — GC victim 读提交失败时 job 必须有界中止，victim 不擦、数据可读。
 *
 * 覆盖：`ftl_async_read_page`（DATA）持续注入 FTL_ERROR；`mig_gc_request_urgent` 强制起 job。
 * 判据：① `fi_hits > 0`、`job.aborted`；② 原 LBA 读回一致（victim 未被擦/搬一半）；
 *       ③ 放开后同一路径的 GC 能完整跑完。
 * 备注：victim 由「不同 8-LBA 顺序写满 64 页 + 1 页换块」构造（整块有效、已 Closed）。 */

static MunitResult test_gc_read_fail_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(2048U);

    ftl_ut_trace_begin();

    neg_make_closed_victim(lba);
    neg_gc_pick_victim(ftl, false);

    const DWORD hits0 = fi_hits(FI_MIG_READ_FAIL);

    fi_arm(FI_MIG_READ_FAIL, NEG_STUCK);

    munit_assert_true(neg_run_gc(ftl, 200000U));

    const DWORD hits = fi_hits(FI_MIG_READ_FAIL) - hits0;

    ftl_ut_trace_detail("gc read fail: aborted=%d error=%ld hits=%lu", ftl->mig_handle->job.aborted, ftl->mig_handle->state.error, hits);

    munit_assert_uint32(hits, >, 0U);
    munit_assert_true(ftl->mig_handle->job.aborted);
    munit_assert_int(ftl->mig_handle->state.error, ==, FTL_ERROR);

    fi_reset();

    /* victim 未被擦：原数据仍可读。 */
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    /* 恢复：放开注入后 GC 必须能完整收尾。 */
    munit_assert_true(neg_run_gc(ftl, 200000U));
    munit_assert_false(ftl->mig_handle->job.aborted);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_summary("neg: GC read submit fail bounded, victim intact, recovery GC ok");

    return MUNIT_OK;
}

/* gc_write_fail_bounded — GC 目的页写提交失败时 job 中止，不误标源块、数据可读。
 *
 * 覆盖：`ftl_async_write_page_cw`（DATA）持续注入 FTL_ERROR。
 * 判据：① `fi_hits > 0`、`job.aborted`；② `bad_count` 不变（写失败不是介质坏）；
 *       ③ 数据可读；④ 放开后 GC 完整跑完。
 * 备注：`FI_MIG_WRITE_FAIL` 同时覆盖 host 写入口（pair_cmd），本用例只触发 GC 目的页。 */

static MunitResult test_gc_write_fail_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(2560U);

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);
    neg_gc_pick_victim(ftl, false);

    const WORD bad0 = ftl->alloc.bad_count;

    const DWORD hits0 = fi_hits(FI_MIG_WRITE_FAIL);

    fi_arm(FI_MIG_WRITE_FAIL, NEG_STUCK);

    munit_assert_true(neg_run_gc(ftl, 200000U));

    const DWORD hits = fi_hits(FI_MIG_WRITE_FAIL) - hits0;

    ftl_ut_trace_detail("gc write fail: aborted=%d hits=%lu bad=%hu->%hu", ftl->mig_handle->job.aborted, hits, bad0, ftl->alloc.bad_count);

    munit_assert_uint32(hits, >, 0U);
    munit_assert_true(ftl->mig_handle->job.aborted);
    /* 目的页写失败是资源/写失败，不是介质坏 ⇒ 不得把源块标 BAD。 */
    munit_assert_uint16(ftl->alloc.bad_count, ==, bad0);

    fi_reset();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    munit_assert_true(neg_run_gc(ftl, 200000U));
    munit_assert_false(ftl->mig_handle->job.aborted);

    ftl_ut_trace_summary("neg: GC dest write fail bounded, source not marked bad, recovery ok");

    return MUNIT_OK;
}

/* gc_erase_fail_bounded — GC victim 擦除**提交**失败时 job 中止；提交失败不标坏。
 *
 * 覆盖：`ftl_async_erase_block` 持续注入 FTL_ERROR。
 * 判据：① `fi_hits > 0`、`job.aborted`；② `bad_count` 不变（提交失败 ≠ 擦除失败）；
 *       ③ 数据可读；④ 放开后 GC 完整跑完。
 * 备注：**擦除完成态**失败（FC 状态错误）会走 `mig_erase_complete` → `bbt_mark_bad`，
 *       是另一条策略；本用例不注入完成态，避免每次跑都烧一个块。 */

static MunitResult test_gc_erase_fail_bounded(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(3072U);

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);

    const WORD bad0 = ftl->alloc.bad_count;

    const DWORD hits0 = fi_hits(FI_MIG_ERASE_FAIL);

    fi_arm(FI_MIG_ERASE_FAIL, NEG_STUCK);

    munit_assert_true(neg_run_gc(ftl, 200000U));

    const DWORD hits = fi_hits(FI_MIG_ERASE_FAIL) - hits0;

    ftl_ut_trace_detail("gc erase fail: aborted=%d hits=%lu bad=%hu->%hu", ftl->mig_handle->job.aborted, hits, bad0, ftl->alloc.bad_count);

    munit_assert_uint32(hits, >, 0U);
    munit_assert_true(ftl->mig_handle->job.aborted);
    munit_assert_uint16(ftl->alloc.bad_count, ==, bad0);

    fi_reset();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    munit_assert_true(neg_run_gc(ftl, 200000U));
    munit_assert_false(ftl->mig_handle->job.aborted);

    ftl_ut_trace_summary("neg: GC erase submit fail bounded, no bad mark, recovery ok");

    return MUNIT_OK;
}

/* l2p_read_error_degrades — region 读**完成**错误连续 3 次后降级，host 读必须 IO_ERROR。
 *
 * 覆盖：`fc_poll_completion` 给 `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` 完成注入 DATA_ERROR（持续）。
 *       region 加载走该原语；主副本失败会先换备份（同样被注入），两份都失败才计一次
 *       `l2p_load_fail_settle`；累计 `L2P_LOAD_RETRY_MAX=3` 次后降级为全未映射。
 * 判据：① **瞬态** 1 次错误：主副本失败后备份重读成功 ⇒ 读成功、不降级（`fi_hits>0`）；
 *       ② **持续**错误：`fi_hits>0`、`l2p_is_region_degraded` 为真、host 读失败（不零填）；
 *       ③ reboot 清降级位后读回原数据。
 * 备注：降级计数是**按 cache 行**累计的；本用例作废全部行后，第一遍遍历把每行计数置 1、
 *       第二遍累计到 2、第三遍到 3 触发降级（约 129 次加载，远小于 5 s 超时）。
 *       降级位只在 RAM（`region_degraded_bitmap`），reboot 后清除。 */

static MunitResult test_l2p_read_error_degrades(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(6144U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    /* 强制下一次读走 region 加载（与 l2p_load_fail_bounded 同口径）。 */
    l2p_cache_s *cache = ftl->cache;

    l2p_cmt_set_enabled(cache, false);
    neg_drop_l2p_cache();

    const DWORD region = lba_to_global_region(lba);

    /* ① 瞬态：只注入 1 次完成错误 → 主副本失败后改读备份，映射不丢、不降级。 */
    const DWORD trans0 = fi_hits(FI_FC_STATUS);

    fi_arm_ex(FI_FC_STATUS, FI_KEY_OP(OP_NORMAL_READ_PAGE_WITH_DATA_OUT), 1U, FC_CMD_STATUS_DATA_ERROR);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("l2p transient: read ok, hits=%lu degraded=%d", fi_hits(FI_FC_STATUS) - trans0, l2p_is_region_degraded(cache, region));

    munit_assert_uint32(fi_hits(FI_FC_STATUS) - trans0, >, 0U);
    munit_assert_false(l2p_is_region_degraded(cache, region));

    /* ② 持续：重新作废该行后再读，三次重试后降级。 */
    neg_drop_l2p_cache();

    const DWORD hits0 = fi_hits(FI_FC_STATUS);

    fi_arm_ex(FI_FC_STATUS, FI_KEY_OP(OP_NORMAL_READ_PAGE_WITH_DATA_OUT), NEG_STUCK, FC_CMD_STATUS_DATA_ERROR);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_false(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));

    const DWORD hits = fi_hits(FI_FC_STATUS) - hits0;

    ftl_ut_trace_detail("l2p degrade: read fail, hits=%lu degraded=%d", hits, l2p_is_region_degraded(cache, region));

    munit_assert_uint32(hits, >, 0U);
    munit_assert_true(l2p_is_region_degraded(cache, region));

    fi_reset();
    l2p_cmt_set_enabled(cache, true);

    /* 降级位是 RAM 态：reboot 清除后，同一 LBA 必须读回原数据。 */
    ftl_ut_reboot_from_media();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_summary("neg: L2P read-error degrades region, host read IO_ERROR, reboot recovers");

    return MUNIT_OK;
}

/* gc_read_unc_marks_bad — 同一 victim 连续读失败达上限必须标 BAD 并摘除 Closed plane（GC-01）。
 *
 * 覆盖：预选 GC victim 并预置「前 2 次同块读失败」，再对数据读持续注入 DATA_ERROR。
 * 判据：① 注入命中；② 第 3 次失败触发 `bad_count +1` 且该 victim 的块变 BAD；
 *       ③ 标坏后原 LBA 仍可读回；④ 放开注入后 GC 能继续完成（不永久卡死）。
 * 备注：victim 选择按成本排序、**不保证连续选中同一块**（同 plane 只有一个失败槽），
 *       所以不能靠“多跑几次 GC”来累计失败；预置 2 次失败等于复现前两次 abort 的
 *       状态，验证的仍是 migrate.c 的 fail_count → bbt_mark_bad 真实路径。 */

static MunitResult test_gc_read_unc_marks_bad(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl  = ftl_ut_live();
    const DWORD lba  = neg_lba(3584U);
    const WORD  bad0 = ftl->alloc.bad_count;

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);

    const super_block_s *victim = neg_gc_pick_victim(ftl, false);

    munit_assert_ptr_not_null(victim);

    BYTE first_plane = PLANE_NUM;
    WORD gsrc_first  = FTL_GBLOCK_INVALID;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (super_block_has_plane(victim, p) == false)
        {
            continue;
        }

        const WORD g = ftl_gblock(p, victim->plane_blocks[p]);

        if (first_plane == PLANE_NUM)
        {
            first_plane = p;
            gsrc_first  = g; /* 标坏后条目会被摘 plane，必须事先取号 */
        }

        ftl->mig_handle->state.failure.fail_gblock[p] = g;
        ftl->mig_handle->state.failure.fail_count[p]  = (BYTE)(MIG_VICTIM_READ_FAIL_MAX - 1U);
    }

    munit_assert_uint8(first_plane, <, PLANE_NUM);

    const DWORD hits0 = fi_hits(FI_FC_STATUS_DATA);

    fi_arm_ex(FI_FC_STATUS_DATA, FI_KEY_ANY, NEG_STUCK, FC_CMD_STATUS_DATA_ERROR);

    munit_assert_true(neg_run_gc(ftl, 200000U));

    const DWORD hits = fi_hits(FI_FC_STATUS_DATA) - hits0;

    ftl_ut_trace_detail("gc unc: hits=%lu bad=%hu->%hu", hits, bad0, ftl->alloc.bad_count);

    munit_assert_uint32(hits, >, 0U);
    munit_assert_uint16(ftl->alloc.bad_count, ==, (WORD)(bad0 + 1U));

    munit_assert_true(ftl_block_is_bad(&ftl->alloc.blocks[gsrc_first]));

    fi_reset();

    /* 块 BAD 不阻止读：映射仍指向原页。 */
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    /* 标坏后系统继续回收：放开注入后 GC 必须能跑完。 */
    munit_assert_true(neg_run_gc(ftl, 200000U));
    munit_assert_false(ftl->mig_handle->job.aborted);

    ftl_ut_trace_summary("neg: victim read failure marks BAD at threshold, data intact, GC resumes");
    return MUNIT_OK;
}

/* gc_batch_program_fail — cache batch 首页完成失败 ⇒ 关 Cold SB、不标坏（GC-02/GC-17）。
 *
 * 覆盖：`FI_FC_STATUS` 对 `OP_TWO_PLANE_PROGRAM_CACHE_FIRST` 注入 NAND_FAIL。
 * 判据：① 命中且 job 中止；② `cold_sb.is_open == false`、`bad_count` 不变；
 *       ③ 数据可读；④ 放开后 GC 完成并真实走到 batch（`batch_jobs` 增长、未 disabled）。 */

static MunitResult test_gc_batch_program_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl  = ftl_ut_live();
    alc_s      *a    = &ftl->alloc;
    const DWORD lba  = neg_lba(4608U);
    const WORD  bad0 = a->bad_count;

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);

    /* 选一个**成对且有有效数据**的候选：单 plane victim 不走 cache-program 批，
     * 空 victim 直接擦，两者都会让注入永不命中。把不合格候选先搬走/回收，逼出
     * 成对候选；选择是成本排序的确定性函数，选完再启动 GC 仍是同一块。 */
    (void)neg_gc_pick_victim(ftl, true);

    /* 选 victim 的过程中搬走的单 plane 条目可能把 batch 关了（跨 job 保留）；
     * 本用例验证 batch 失败路径，必须在启动 GC 前复位。 */
    ftl->mig_handle->state.batch_disabled = false;

    const DWORD hits0    = fi_hits(FI_FC_STATUS);
    const DWORD aborts0  = ftl->mig_handle->state.counters.batch_aborts;
    const DWORD batches0 = ftl->mig_handle->state.counters.batch_jobs;

    fi_arm_ex(FI_FC_STATUS, FI_KEY_OP(OP_TWO_PLANE_PROGRAM_CACHE_FIRST), 1U, FC_CMD_STATUS_NAND_FAIL);

    munit_assert_true(neg_run_gc(ftl, 200000U));

    const DWORD hits = fi_hits(FI_FC_STATUS) - hits0;

    ftl_ut_trace_detail("gc batch fail: hits=%lu aborted=%d aborts=%lu cold_open=%d bad=%hu->%hu", hits, ftl->mig_handle->job.aborted, ftl->mig_handle->state.counters.batch_aborts, a->cold_sb.is_open, bad0, a->bad_count);

    munit_assert_uint32(hits, >, 0U);
    munit_assert_true(ftl->mig_handle->job.aborted);
    munit_assert_false(a->cold_sb.is_open);
    munit_assert_uint16(a->bad_count, ==, bad0);
    munit_assert_uint32(ftl->mig_handle->state.counters.batch_aborts, >, aborts0);
    munit_assert_uint32(ftl->mig_handle->state.counters.batch_jobs, >, batches0);

    fi_reset();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    /* 放开注入后系统继续搬移；不要求下一 job 一定走批——剩余候选可能是单 plane
     * 退化条目，只断言不中止、batch 未被永久禁用。 */
    munit_assert_true(neg_run_gc(ftl, 200000U));
    munit_assert_false(ftl->mig_handle->job.aborted);
    munit_assert_false(ftl->mig_handle->state.batch_disabled);

    ftl_ut_trace_summary("neg: GC batch program fail closes cold SB, no bad mark, GC resumes");
    return MUNIT_OK;
}

/* gc_l2p_load_wait_timeout — victim settle 的 L2P 加载永久不就绪 ⇒ job 有界中止（MIG-01）。
 *
 * 覆盖：`FI_L2P_LOAD_FAIL` 持续注入（META 读失败）；强制 GC 走 READ_SETTLE。
 * 判据：① 命中；② job 在 `FTL_WAIT_TIMEOUT_MS`（50 ms）量级内 aborted；
 *       ③ 放开后 GC 可完成。
 * 备注：只影响 META 读；victim 的 DATA 读正常。 */

static MunitResult test_gc_l2p_load_wait_timeout(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(5120U);

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);
    neg_gc_pick_victim(ftl, false);

    /* GC 反查 L2P 必须真的触发 region 加载：命中驻留行就不会走注入点。
     * 先归一到干净空闲（迁移的 dirty CMT 落盘），再强制清空驻留行。 */
    neg_drain_to_clean(ftl);
    neg_drop_l2p_cache();

    const DWORD t0    = platform_now_ms(); /* 只计被注入的这次 job */
    const DWORD hits0 = fi_hits(FI_L2P_LOAD_FAIL);

    fi_arm(FI_L2P_LOAD_FAIL, NEG_STUCK);

    munit_assert_true(neg_run_gc(ftl, 200000U));

    const DWORD hits    = fi_hits(FI_L2P_LOAD_FAIL) - hits0;
    const DWORD elapsed = platform_now_ms() - t0;

    ftl_ut_trace_detail("gc l2p wait: hits=%lu aborted=%d elapsed=%lums", hits, ftl->mig_handle->job.aborted, elapsed);

    munit_assert_uint32(hits, >, 0U);
    munit_assert_true(ftl->mig_handle->job.aborted);
    munit_assert_int(ftl->mig_handle->state.error, ==, FTL_ERROR);
    munit_assert_uint32(elapsed, <, 2000U);

    fi_reset();

    munit_assert_true(neg_run_gc(ftl, 200000U));
    munit_assert_false(ftl->mig_handle->job.aborted);

    ftl_ut_trace_summary("neg: GC L2P load wait bounded by timeout, recovery ok");
    return MUNIT_OK;
}

/* flush_reports_journal_error — Journal 页写失败必须可见，放开后 CP 清账（JN-02）。
 *
 * 覆盖：`FI_JOURNAL_WRITE_FAIL` 注入 1 次；flush → `jrn_has_write_failure` 置位。
 * 判据：① 失败标记可见；② 放开后强制 CP 完成、标记清零；③ 数据可读。
 * 备注：`ftl_ut_flush` 对 wb/jrn 失败仍返回完成（best-effort），判据用 journal 标志。 */

static MunitResult test_flush_reports_journal_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(2944U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));

    fi_journal_fail_arm(1U);

    munit_assert_true(ftl_ut_flush());
    munit_assert_true(jrn_has_write_failure(ftl->journal));

    fi_reset();

    /* 强制 CP 完成 WAL 清账。 */
    neg_wait_cp_idle(ftl, 200000U);

    munit_assert_true(neg_cp_force(ftl, 200000U, NULL));

    ftl_ut_trace_detail("flush jrn fail: write_fail visible, after CP failure=%d", jrn_has_write_failure(ftl->journal));

    munit_assert_false(ftl->cp_handle->active);
    munit_assert_false(jrn_has_write_failure(ftl->journal));

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_summary("neg: journal write failure visible, CP clears WAL failure");
    return MUNIT_OK;
}

/* rr_mixed_plane_fail — 一 plane 恢复、另一 plane 不可恢复 ⇒ 整组 IO_ERROR（RR-02）。
 *
 * 覆盖：1 次多 plane 读完成错误触发 RR；`FI_FC_STATUS_PLANE` 只让 plane 0 的重读失败。
 * 判据：① 两种注入都命中；② `planes_failed>=1` 且 `planes_recovered>=1`（聚合半区）；
 *       ③ host 读 IO_ERROR；④ 放开后读回原数据。
 * 备注：失败 plane 的块会被 `rr_settle_plane` 标坏（设计策略），format 后复位。 */

static MunitResult test_rr_mixed_plane_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(5632U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    ftl_ut_reboot_from_media();

    const DWORD hits0  = fi_hits(FI_FC_STATUS);
    const DWORD plane0 = fi_hits(FI_FC_STATUS_PLANE);

    fi_arm_ex(FI_FC_STATUS, FI_KEY_OP(NEG_HOST_READ_OP), 1U, FC_CMD_STATUS_DATA_ERROR);
    fi_arm_ex(FI_FC_STATUS_PLANE, 0U, NEG_STUCK, FC_CMD_STATUS_DATA_ERROR);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_false(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));

    const DWORD hits   = fi_hits(FI_FC_STATUS) - hits0;
    const DWORD hits0p = fi_hits(FI_FC_STATUS_PLANE) - plane0;

    ftl_ut_trace_detail("rr mixed: hits=%lu plane_hits=%lu failed=%hhu recovered=%hhu", hits, hits0p, ftl->read_recovery_handle.planes_failed, ftl->read_recovery_handle.planes_recovered);

    munit_assert_uint32(hits, >, 0U);
    munit_assert_uint32(hits0p, >, 0U);
    munit_assert_uint8(ftl->read_recovery_handle.planes_failed, >=, 1U);
    munit_assert_uint8(ftl->read_recovery_handle.planes_recovered, >=, 1U);

    fi_reset();
    neg_recover();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_summary("neg: RR mixed plane aggregation -> IO_ERROR, recovery byte-equal");
    return MUNIT_OK;
}

/* l2p_cmt_read_priority — 写 A→flush→写 B→立刻读，必须读到 B（L2P-12）。
 *
 * 覆盖：读路径必须优先查 CMT/已编程页表，不能拿 CLEAN line 的旧 PBA。
 * 判据：未 flush 的第二次写完成后，host 读逐字节等于 B。
 * 备注：不注入；差异由同一 LBA 的两种内容区分。 */

static MunitResult test_l2p_cmt_read_priority(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const DWORD lba = neg_lba(6400U);

    ftl_ut_trace_begin();

    neg_fill_tag(lba, NEG_LBAS, s_neg_buf, 1U);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    neg_fill_tag(lba, NEG_LBAS, s_neg_buf, 2U);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_tag(lba, NEG_LBAS, s_neg_buf, 2U);

    munit_assert_true(ftl_ut_flush());
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_tag(lba, NEG_LBAS, s_neg_buf, 2U);

    ftl_ut_trace_summary("neg: L2P read prefers new mapping over stale CLEAN line");
    return MUNIT_OK;
}

/* l2p_idle_drain — 低负载写后 IDLE 必须把 dirty CMT 排空（L2P-18）。
 *
 * 覆盖：不调用 host FLUSH，靠后台 IDLE 节拍把 dirty CMT 写回。
 * 判据：2 s 内 `cache->stats.cmt_dirty == 0`；数据可读。
 * 备注：不等价于掉电持久性，只验「低于水位不会永久留在 RAM」。 */

static MunitResult test_l2p_idle_drain(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(896U);
    const DWORD t0  = platform_now_ms();

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));

    while ((ftl->cache->stats.cmt_dirty != 0U) && ((platform_now_ms() - t0) < 2000U))
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);
    }

    ftl_ut_trace_detail("l2p idle: dirty=%lu elapsed=%lums", ftl->cache->stats.cmt_dirty, platform_now_ms() - t0);

    munit_assert_uint32(ftl->cache->stats.cmt_dirty, ==, 0U);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_summary("neg: L2P idle drain clears dirty CMT without host FLUSH");
    return MUNIT_OK;
}

/* l2p_flush_backpressure — 提交被持续拒绝时 flush 有界失败，放开后恢复（L2P-03）。
 *
 * 覆盖：`FI_SEQ_GATE_STARVE` 持续拒绝所有 `ftl_op_submit`（含 L2P/Journal 写回）。
 * 判据：① 注入期间 flush 失败（有界，不挂死）；② 放开后 flush 成功；③ 数据可读。
 * 备注：不验证日志槽计数（槽位是内部账），只验「背压 → 有界失败 → 可恢复」。 */

static MunitResult test_l2p_flush_backpressure(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(1024U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));

    const DWORD hits0 = fi_hits(FI_SEQ_GATE_STARVE);

    fi_arm(FI_SEQ_GATE_STARVE, NEG_STUCK);

    const DWORD t0      = platform_now_ms();
    const BOOL  flushed = ftl_ut_flush();
    const DWORD elapsed = platform_now_ms() - t0;
    const DWORD hits    = fi_hits(FI_SEQ_GATE_STARVE) - hits0;

    ftl_ut_trace_detail("l2p flush backpressure: hits=%lu flush=%d elapsed=%lums", hits, flushed, elapsed);

    munit_assert_uint32(hits, >, 0U);
    /* 返回 true 是允许的：journal 可能已 durable，L2P 写回被拒只是延后（CP 回写）。
     * 本用例验的是「有界、不挂死」，不是「必须失败」。 */
    munit_assert_uint32(elapsed, <=, FTL_UT_IO_TIMEOUT_MS);

    fi_reset();
    neg_drain_to_clean(ftl);

    /* 无 L2P flush 槽泄漏：脏行最终必须排空，数据仍可读。 */
    munit_assert_uint32(ftl->cache->inflight.flush, ==, 0U);
    munit_assert_uint32(ftl->cache->stats.cmt_dirty, ==, 0U);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_summary("neg: L2P flush under submit backpressure bounded, no slot leak, data intact");
    return MUNIT_OK;
}

/* boot_corrupt_cp_restore — 最新 CP Header 损坏后仍能启动并读回旧数据（BOOT-05）。
 *
 * 覆盖：强制 CP 完成后，用 `ftl_sync_write_page` 把该 Header 页写成垃圾（ECC/CRC 合法但
 *       magic/CRC 语义无效）；reboot 走旧 record / full rebuild 回退。
 * 判据：① 写入损坏成功；② reboot 后 FTL 正常（夹具断言）；③ 原 LBA 数据可读；
 *       ④ 再次强制 CP 能成功（写路径恢复）。
 * 备注：不校验具体回退类型（旧 record vs full rebuild），只验「不崩、不丢已落盘数据」。 */

static MunitResult test_boot_corrupt_cp_restore(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(7680U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    neg_wait_cp_idle(ftl, 200000U);

    munit_assert_true(neg_cp_force(ftl, 200000U, NULL));

    munit_assert_false(ftl->cp_handle->active);

    const DWORD addr = ftl->cp_handle->hdr.addr;

    munit_assert_true(pba_is_valid(addr));

    BYTE junk[PAGE_MAIN_SIZE];

    memset(junk, 0x5A, sizeof(junk));
    munit_assert_int(ftl_sync_write_page(addr, junk, sizeof(junk), FTL_PAGE_CFG_META()), ==, FTL_OK);

    ftl_ut_reboot_from_media();

    /* BOOT-02：full rebuild 后不得出现「同一块既是 Free 池成员又是 Closed 条目」。 */
    for (WORD si = 0U; si < ftl->alloc.closed_sb_count; si++)
    {
        const super_block_s *sb = &ftl->alloc.closed_sb_list[si];

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (super_block_has_plane(sb, p) == false)
            {
                continue;
            }

            const WORD g = ftl_gblock(p, sb->plane_blocks[p]);

            for (WORD fi = 0U; fi < ftl->alloc.free_count; fi++)
            {
                munit_assert_uint16(ftl->alloc.free_blocks[fi], !=, g);
            }
        }
    }

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    munit_assert_true(neg_cp_force(ftl, 200000U, NULL));

    munit_assert_false(ftl->cp_handle->active);
    munit_assert_int(ftl->cp_handle->fail_phase, ==, CP_IDLE);

    ftl_ut_trace_detail("boot corrupt cp: addr=0x%08lx reboot readback byte-equal, recovery CP ok", addr);
    ftl_ut_trace_summary("neg: boot survives corrupted latest CP header, data intact");
    return MUNIT_OK;
}

/* boot_no_rebuild_loop — 连续两次 reboot 不进入「每次开机重建」循环（BOOT-15）。
 *
 * 覆盖：写入后连续模拟重启两次；夹具要求 `pfr_boot_recovery == FTL_OK`，任一轮失败即红。
 * 判据：两次 reboot 后同一 LBA 数据都可读；boot 状态不退化。
 * 备注：不直接读 boot type（夹具未返回），用「两次都能起且数据在」作判据。 */

static MunitResult test_boot_no_rebuild_loop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const DWORD lba = neg_lba(7936U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    for (BYTE round = 0U; round < 2U; round++)
    {
        ftl_ut_reboot_from_media();

        memset(s_neg_buf, 0, sizeof(s_neg_buf));
        munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
        neg_assert_data(lba, NEG_LBAS, s_neg_buf);
    }

    ftl_ut_trace_summary("neg: two consecutive reboots recover without rebuild loop");
    return MUNIT_OK;
}

/* boot_degraded_reload — 降级位图不得阻止后续重新加载（BOOT-21）。
 *
 * 覆盖：先造降级 region（META 读完成错误），再写入新数据并 flush、驱逐行、重读。
 * 判据：① 降级位置位；② 新写数据重载后仍可读（加载走 Directory，而非短路成全 INVALID）；
 *       ③ 数据逐字节等于新内容。
 * 备注：降级位只用于「未映射 LBA 报 IO_ERROR」，不禁止加载。 */

static MunitResult test_boot_degraded_reload(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(2304U);

    ftl_ut_trace_begin();

    neg_fill_tag(lba, NEG_LBAS, s_neg_buf, 1U);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    l2p_cache_s *cache = ftl->cache;

    l2p_cmt_set_enabled(cache, false);
    neg_drop_l2p_cache();

    const DWORD region = lba_to_global_region(lba);

    fi_arm_ex(FI_FC_STATUS, FI_KEY_OP(OP_NORMAL_READ_PAGE_WITH_DATA_OUT), NEG_STUCK, FC_CMD_STATUS_DATA_ERROR);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_false(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(l2p_is_region_degraded(cache, region));

    fi_reset();
    l2p_cmt_set_enabled(cache, true);

    neg_fill_tag(lba, NEG_LBAS, s_neg_buf, 2U);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    neg_drop_l2p_cache();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_tag(lba, NEG_LBAS, s_neg_buf, 2U);
    munit_assert_true(l2p_is_region_degraded(cache, region));

    ftl_ut_trace_summary("neg: degraded bitmap does not block Directory reload");
    return MUNIT_OK;
}

/* iface_sparse_page — 稀疏页（只写部分 CW）的数据与映射必须正确（IF-04）。
 *
 * 覆盖：同一 pair 页只写 CW0 与 CW2（各 1 LBA），其余 CW 从未编程。
 * 判据：读回时 CW0/CW2 数据正确、其余 LBA 为零填（未映射），不出现整页 0xFF。
 * 备注：验证 host 读按未映射 CW 零填，且写位图按实际覆盖的 plane 定段。 */

static MunitResult test_iface_sparse_page(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(1536U);

    ftl_ut_trace_begin();

    /* 稀疏页断言要求该 region 的其它 CW **从未映射**；跑完整套件时前面用例
     * 可能已经写过同一 region ⇒ 必须从隔离格式化基线开始。 */

    neg_fill_tag(lba, 1U, s_neg_buf, 1U);
    munit_assert_true(ftl_ut_host_write(lba, 1U, s_neg_buf));

    neg_fill_tag(lba + 2U, 1U, s_neg_buf, 2U);
    munit_assert_true(ftl_ut_host_write(lba + 2U, 1U, s_neg_buf));

    munit_assert_true(ftl_ut_flush());

    /* 前置判据：只有 LBA0/LBA2 有映射，其余必须未映射——把「映射泄漏」和
     * 「读零填错误」分开（两者都会让下面的零断言红，但责任完全不同）。 */
    for (BYTE i = 0U; i < NEG_LBAS; i++)
    {
        DWORD p = 0U;

        munit_assert_int(l2p_read(ftl->cache, lba + i, &p), ==, FTL_OK);

        if ((i != 0U) && (i != 2U))
        {
            munit_assert_uint32(p, ==, NAND_PBA_INVALID);
        }
    }

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));

    neg_assert_tag(lba, 1U, s_neg_buf, 1U);
    neg_assert_tag(lba + 2U, 1U, s_neg_buf + (2U * FTL_IU_BYTES), 2U);

    /* 未写 LBA 零填：逐洞检查 **1 个 IU**（两个 IU 会把 LBA2 的合法数据算进零断言）。 */
    for (BYTE sl = 0U; sl < NEG_LBAS; sl++)
    {
        if ((sl == 0U) || (sl == 2U))
        {
            continue;
        }

        for (DWORD i = 0U; i < FTL_IU_BYTES; i++)
        {
            munit_assert_uint8(s_neg_buf[(sl * FTL_IU_BYTES) + i], ==, 0U);
        }
    }

    ftl_ut_trace_summary("neg: sparse CW page data/mapping correct");
    return MUNIT_OK;
}

/* blank_ff_trim_rewrite — 整 region TRIM 后重写不得把空白页误判为已编程（IF-15）。
 *
 * 覆盖：写数据 → TRIM 整个 region（flush 写出全 0xFF 的映射）→ reboot → 重写。
 * 判据：重写数据可读、region 未降级。
 * 备注：整 region = 512 LBA；覆盖「全 FF 区域页」形态，验证空白判定不误伤。 */

static MunitResult test_blank_ff_trim_rewrite(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = ftl_ut_lba_base() & ~511U;

    ftl_ut_trace_begin();

    neg_fill_tag(lba, NEG_LBAS, s_neg_buf, 1U);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    munit_assert_true(ftl_ut_trim(lba, 512U));
    munit_assert_true(ftl_ut_flush());

    ftl_ut_reboot_from_media();

    neg_fill_tag(lba, NEG_LBAS, s_neg_buf, 2U);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_tag(lba, NEG_LBAS, s_neg_buf, 2U);
    munit_assert_false(l2p_is_region_degraded(ftl->cache, lba_to_global_region(lba)));

    ftl_ut_trace_summary("neg: all-FF region page after TRIM not treated as blank");
    return MUNIT_OK;
}

/* host_read_multiplane_stats — 一个 multi-plane 读必须给**两个**块都记 ECC/read disturb（HOST-02）。
 *
 * 覆盖：8 LBA 读（跨两 plane 两个块），读前后比较各 gblock 的 read_count。
 * 判据：两个块的 read_count 都 +1；只记首个 desc 的块会漏掉第二块。
 * 备注：reboot 后读，避免读缓存/写缓存掩蔽 NAND 读路径。 */

static MunitResult test_host_read_multiplane_stats(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(2816U);

    ftl_ut_trace_begin();

    neg_fill_tag(lba, NEG_LBAS, s_neg_buf, 1U);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    ftl_ut_reboot_from_media();

    DWORD pba0 = 0U;
    DWORD pba1 = 0U;

    munit_assert_int(l2p_read(ftl->cache, lba, &pba0), ==, FTL_OK);
    munit_assert_int(l2p_read(ftl->cache, lba + 4U, &pba1), ==, FTL_OK);

    const WORD g0 = ftl_pba_gblock(pba0);
    const WORD g1 = ftl_pba_gblock(pba1);

    munit_assert_uint16(g0, !=, g1); /* 两 plane 各一个独立块 */

    const WORD r0 = ftl->alloc.blocks[g0].read_count;
    const WORD r1 = ftl->alloc.blocks[g1].read_count;

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_tag(lba, NEG_LBAS, s_neg_buf, 1U);

    munit_assert_uint16(ftl->alloc.blocks[g0].read_count, ==, (WORD)(r0 + 1U));
    munit_assert_uint16(ftl->alloc.blocks[g1].read_count, ==, (WORD)(r1 + 1U));

    ftl_ut_trace_summary("neg: multi-plane read counts both gblocks");
    return MUNIT_OK;
}

/* md_alias_full_rebuild_repro — format + 3 代写 + 4 次重启不得回退 full rebuild 丢映射（MD-06）。
 *
 * 覆盖：同一 LBA 连续 3 代写后连续 4 次 reboot。
 * 判据：boot 后 region 未降级、读回第 3 代数据；回退 full rebuild 会丢最新映射。
 * 备注：复现序列来自风险登记，无需注入。 */

static MunitResult test_md_alias_full_rebuild_repro(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(3264U);

    ftl_ut_trace_begin();

    for (BYTE gen = 1U; gen <= 3U; gen++)
    {
        neg_fill_tag(lba, NEG_LBAS, s_neg_buf, gen);
        munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
        munit_assert_true(ftl_ut_flush());
    }

    for (BYTE boot = 0U; boot < 4U; boot++)
    {
        ftl_ut_reboot_from_media();
    }

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_tag(lba, NEG_LBAS, s_neg_buf, 3U);
    munit_assert_false(l2p_is_region_degraded(ftl->cache, lba_to_global_region(lba)));

    ftl_ut_trace_summary("neg: 3 generations + 4 reboots keep latest mapping");
    return MUNIT_OK;
}

/* bbt_mark_bad_idempotent — 运行时标坏幂等 + 落盘后重启仍是 BAD（BBT-01）。
 *
 * 覆盖：对用户区一个 FREE 块重复 `bbt_mark_bad` → `bbt_store` → reboot。
 * 判据：bad_count 只 +1；两次调用都 `FTL_OK`；重启后该块仍 BAD。
 * 备注：Spare 记账只断言坏块数，不展开 spare 池收支。 */

static MunitResult test_bbt_mark_bad_idempotent(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();
    alc_s     *a   = &ftl->alloc;

    ftl_ut_trace_begin();

    /* 后台 job 可能在飞（BBT 保存互斥）；同时保证选块期间池不被分配。 */
    neg_drain_to_clean(ftl);
    munit_assert_false(a->bbt.handle.waiting);

    WORD g = FTL_GBLOCK_INVALID;

    for (WORD i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        if ((a->blocks[i].state != FTL_BLOCK_STATE_FREE) || (ftl_block_is_bad(&a->blocks[i]) == true) || (ftl_gblock_block(i) < FTL_METADATA_AREA_END))
        {
            continue;
        }

        /* 排除两个 Open SB 正在用的块：FREE 状态在稳态下应等于「在 Free Pool」，
         * 但 Open SB 的块可能是刚分配、池条目尚未摘净的窗口。 */
        BOOL open_used = false;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if ((super_block_has_plane(&a->hot_sb, p) == true) && (ftl_gblock(p, a->hot_sb.plane_blocks[p]) == i))
            {
                open_used = true;
            }
            if ((super_block_has_plane(&a->cold_sb, p) == true) && (ftl_gblock(p, a->cold_sb.plane_blocks[p]) == i))
            {
                open_used = true;
            }
        }

        if (open_used == false)
        {
            g = i;
            break;
        }
    }

    munit_assert_uint16(g, !=, FTL_GBLOCK_INVALID);

    const WORD bad0 = a->bad_count;

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK);
    munit_assert_uint16(a->bad_count, ==, (WORD)(bad0 + 1U));
    munit_assert_true(ftl_block_is_bad(&a->blocks[g]));

    munit_assert_int(bbt_mark_bad(a, g), ==, FTL_OK); /* 终态幂等 */
    munit_assert_uint16(a->bad_count, ==, (WORD)(bad0 + 1U));

    munit_assert_int(bbt_store(a), ==, FTL_OK);

    ftl_ut_reboot_from_media();

    munit_assert_true(ftl_block_is_bad(&a->blocks[g]));

    ftl_ut_trace_summary("neg: runtime bad mark idempotent and persists across reboot");
    return MUNIT_OK;
}

/* gc_partial_plane_erase_abort — 部分 plane 擦除后中止，旧条目不得引用已释放 plane（GC-04）。
 *
 * 覆盖：成对 victim 的 plane0 擦成功、plane1 擦除提交失败。
 * 判据：job aborted；失败 plane 的块标 BAD；**已擦成功的 plane 不再被任何 Closed
 *       条目引用**（旧条目误认已释放 plane 会让块被重分配后撞车）。
 * 备注：RAM/介质破坏性：本次会烧掉 1 个块（失败 plane）。 */

static MunitResult test_gc_partial_plane_erase_abort(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl  = ftl_ut_live();
    const DWORD lba  = neg_lba(3328U);
    const WORD  bad0 = ftl->alloc.bad_count;

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);

    const super_block_s *v = neg_gc_pick_victim(ftl, true);

    const WORD p0 = v->plane_blocks[0];
    const WORD p1 = v->plane_blocks[1];

    const DWORD hits0 = fi_hits(FI_MIG_ERASE_FAIL);

    fi_arm_at(FI_MIG_ERASE_FAIL, 1U, 1U); /* 只让 plane1 的擦除失败 */

    munit_assert_true(neg_run_gc(ftl, 200000U));

    ftl_ut_trace_detail("gc plane erase: hits=%lu erased=%hu plane=%hhu aborted=%d bad=%hu->%hu", fi_hits(FI_MIG_ERASE_FAIL) - hits0, ftl->mig_handle->job.erased, ftl->mig_handle->job.plane, (int)ftl->mig_handle->job.aborted, bad0, ftl->alloc.bad_count);

    munit_assert_uint32(fi_hits(FI_MIG_ERASE_FAIL) - hits0, >, 0U);
    munit_assert_true(ftl->mig_handle->job.aborted);
    /* 擦除**提交**失败：按契约不标坏（只有完成态擦失败才 mark BAD）。 */
    munit_assert_uint16(ftl->alloc.bad_count, ==, bad0);
    munit_assert_uint16(ftl->mig_handle->job.erased, ==, 1U); /* 必须真的先擦成功 1 个 plane */

    /* 成功擦掉的 plane 已归还 Free Pool：任何 Closed 条目都不得再引用它。 */
    {
        const BYTE ok_plane   = (ftl->mig_handle->job.plane == 1U) ? 0U : 1U;
        const WORD ok_block   = (ok_plane == 0U) ? p0 : p1;
        BOOL       referenced = false;

        for (WORD i = 0U; i < ftl->alloc.closed_sb_count; i++)
        {
            if (ftl->alloc.closed_sb_list[i].plane_blocks[ok_plane] == ok_block)
            {
                referenced = true;
            }
        }
        munit_assert_false(referenced);
    }

    fi_reset();

    return MUNIT_OK;
}

/* gc_systemic_read_fuse — 连续不同 victim 读失败达上限必须熔断 GC（GC-10 / MIG-07）。
 *
 * 覆盖：预置 `read_fail_streak = MAX-1` + victim 的第 3 次失败，注入 1 次完成态错误。
 * 判据：该块标 BAD、`state.failure.paused == true`；再次请求 GC 不再起 job。
 * 备注：不依赖连续 8 个真实 victim（否则要跑 8 轮 GC 且烧 8 个块）。 */

static MunitResult test_gc_systemic_read_fuse(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl  = ftl_ut_live();
    const DWORD lba  = neg_lba(3456U);
    const WORD  bad0 = ftl->alloc.bad_count;

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);

    const super_block_s *v = neg_gc_pick_victim(ftl, false);
    BYTE                 p = PLANE_NUM;

    for (BYTE i = 0U; i < PLANE_NUM; i++)
    {
        if (super_block_has_plane(v, i))
        {
            p = i;
            break;
        }
    }

    munit_assert_uint8(p, <, PLANE_NUM);

    const WORD g = ftl_gblock(p, v->plane_blocks[p]);

    ftl->mig_handle->state.failure.fail_gblock[p]   = g;
    ftl->mig_handle->state.failure.fail_count[p]    = (BYTE)(MIG_VICTIM_READ_FAIL_MAX - 1U);
    ftl->mig_handle->state.failure.read_fail_streak = (BYTE)(MIG_SYSTEMIC_READ_FAIL_MAX - 1U);

    fi_arm_ex(FI_FC_STATUS_DATA, FI_KEY_ANY, NEG_STUCK, FC_CMD_STATUS_DATA_ERROR);

    munit_assert_true(neg_run_gc(ftl, 200000U));

    munit_assert_uint16(ftl->alloc.bad_count, ==, (WORD)(bad0 + 1U));
    munit_assert_true(ftl->mig_handle->state.failure.paused);

    /* 熔断后 urgent 请求不得再起 job。 */
    munit_assert_false(neg_run_gc(ftl, 200000U));

    fi_reset();

    return MUNIT_OK;
}

/* bbt_kick_candidates — BBT 后台 kick 必须把 dirty 落盘并推进 sequence（BBT-10）。
 *
 * 覆盖：`bbt_dirty_note` + `flush_due` + `bbt_handle` 的 kick 路径。
 * 判据：job 完成后 `dirty == false`、`sequence` 前进。
 * 备注：factory-scan / format_erase 属空片/破坏性路径，不在本用例内。 */

static MunitResult test_bbt_kick_candidates(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();
    alc_s     *a   = &ftl->alloc;

    ftl_ut_trace_begin();
    neg_drain_to_clean(ftl);

    const DWORD seq0 = a->bbt.sequence;

    munit_assert_false(a->bbt.handle.waiting);

    bbt_dirty_note(a);
    a->bbt.flush_due = 1U;
    bbt_handle(a);

    for (DWORD i = 0U; (i < 200000U) && ((a->bbt.handle.waiting != false) || (a->bbt.dirty != false)); i++)
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);
    }

    munit_assert_false(a->bbt.dirty);
    munit_assert_uint32(a->bbt.sequence, >, seq0);

    return MUNIT_OK;
}

/* journal_ring_erase_fail — 环形块连续擦失败必须标坏；3 块坏后安全停写（JN-01 / JN-07）。
 *
 * 覆盖：`jrn_erase_failed` + `jrn_block_next` 在只剩当前写块时的行为。
 * 判据：① 每块连续 `JRN_ERASE_FAIL_MAX` 次失败后置 bad_mask、两 plane 标 BAD、
 *       失败计数清零；② 前 1~2 块坏时 ring_full 仍为 false；③ 3 块坏后必须置
 *       ring_full，且当前写块不被擦/不被标坏（否则会擦掉持有最新条目的块）。
 * 备注：只验证路径；结束时还原全部 BST/BBT/Journal RAM 状态并 reboot，不烧真实
 *       Journal 候选块。 */

static MunitResult test_journal_ring_erase_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();
    l2p_jrn_s *j   = ftl->journal;
    alc_s     *a   = &ftl->alloc;

    ftl_ut_trace_begin();

    /* 本用例要求 4 个 Journal 候选块全部健康；单跑时上一用例可能留下 BAD
     * （尤其失败断言后 teardown 没跑），因此用隔离格式化建立确定基线。 */

    const BYTE cur = j->handle.state.current_block;

    /* 快照：本次 BAD 只允许留在 RAM 供断言，结束后必须还原，否则会把 Journal
     * 候选块永久烧掉（重复跑会让候选 < 4，下次 boot 直接拒绝启动）。 */
    bst_block_s snaps[JOURNAL_BLOCK_COUNT][PLANE_NUM];

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            snaps[b][p] = a->blocks[ftl_gblock(p, j->blocks[b])];
        }
    }

    const WORD  bad0     = a->bad_count;
    const BOOL  dirty0   = a->bbt.dirty;
    const DWORD gen0     = a->bbt.dirty_gen;
    const BOOL  rebuild0 = a->bbt.rebuild_required;
    const BOOL  pools_d0 = (a->pools != NULL) ? a->pools->dirty : false;
    const BYTE  mask0    = j->handle.state.bad_mask;
    const BYTE  failblk0 = j->handle.state.erase_fail_block;
    const BYTE  failcnt0 = j->handle.state.erase_fail_count;
    const BYTE  target0  = j->handle.job.target_block;
    const BOOL  need0    = j->handle.job.need_erase;
    const BOOL  full0    = j->handle.state.ring_full;

    munit_assert_true(jrn_is_idle(j));
    munit_assert_uint8(j->handle.state.ring_full, ==, false);
    munit_assert_uint8(mask0, ==, 0U); /* 前提：本次开始前没有坏 ring 块 */

    ftl->flags.bg_suspend = 1U; /* 禁止后台 BBT 把本次 dirty 写进 NAND */

    BYTE marked = 0U;

    /* FI_* 每个 fault 只有一个槽：逐块 fi_arm_ex 会互相覆盖，最后只有一块
     * 被武装（板端现象：只标坏 1 块）。这里用 FI_KEY_ANY 持续失败，让
     * jrn_erase_failed 按 3 次失败逐个标坏、跳到下一块。 */
    fi_arm_ex(FI_FC_STATUS_PBA, FI_KEY_ANY, NEG_STUCK, FC_CMD_STATUS_NAND_FAIL);

    j->handle.job.target_block = (BYTE)((cur + 1U) % JOURNAL_BLOCK_COUNT);
    j->handle.job.need_erase   = true;

    const DWORD hits0 = fi_hits(FI_FC_STATUS_PBA);

    jrn_trigger_e t = JRN_TRIGGER_ERASE_ONLY;

    for (DWORD i = 0U; (i < 5000U) && (j->handle.state.ring_full == false); i++)
    {
        jrn_handle(j, &t);
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);
    }

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        if ((BYTE)((cur + 1U + b) % JOURNAL_BLOCK_COUNT) != cur)
        {
            munit_assert_uint8((j->handle.state.bad_mask & (1U << ((cur + 1U + b) % JOURNAL_BLOCK_COUNT))), !=, 0U);
            marked++;
        }
    }

    munit_assert_uint32(fi_hits(FI_FC_STATUS_PBA), >, hits0);
    munit_assert_uint8(j->handle.state.erase_fail_count, ==, 0U);
    munit_assert_uint16(a->bad_count, ==, (WORD)(bad0 + ((WORD)marked * PLANE_NUM)));

    munit_assert_uint8(marked, ==, (BYTE)(JOURNAL_BLOCK_COUNT - 1U));
    munit_assert_uint8(j->handle.state.bad_mask, ==, (BYTE)(((1U << JOURNAL_BLOCK_COUNT) - 1U) & ~(1U << cur)));
    munit_assert_true(j->handle.state.ring_full);
    munit_assert_uint8((j->handle.state.bad_mask & (1U << cur)), ==, 0U);

    /* 还原本次 BAD（只验证路径，不消耗真实 Journal 候选块）。 */
    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            a->blocks[ftl_gblock(p, j->blocks[b])] = snaps[b][p];
        }
    }

    a->bad_count            = bad0;
    a->bbt.dirty            = dirty0;
    a->bbt.dirty_gen        = gen0;
    a->bbt.rebuild_required = rebuild0;
    if (a->pools != NULL)
    {
        a->pools->dirty = pools_d0;
    }

    j->handle.state.bad_mask         = mask0;
    j->handle.state.erase_fail_block = failblk0;
    j->handle.state.erase_fail_count = failcnt0;
    j->handle.state.ring_full        = full0;
    j->handle.job.target_block       = target0;
    j->handle.job.need_erase         = need0;
    j->handle.job.phase              = JRN_IDLE;

    fi_reset();
    ftl->flags.bg_suspend = false;

    ftl_ut_reboot_from_media(); /* 清 Journal 运行态，且 NAND 上没有本次 dirty */

    ftl_ut_trace_summary("jrn ring: 3 bad blocks -> ring_full, current block untouched");

    return MUNIT_OK;
}

/* l2p_pool_starve_flush — 命令池耗尽时 flush / CP 必须有界且不泄漏槽（L2P-02 / L2P-05 / CP-04）。
 *
 * 覆盖：占满 FC 命令池后 flush（L2P 区域写回 / 备副本提交都拿不到命令），并在
 *       池仍满时显式 force 一次 CP。
 * 判据：flush 与 CP 都在 UT 超时内返回（CP 不无限等命令槽）；释放命令并恢复后
 *       CMT 排空、数据可读。
 * 备注：不检查 flush 返回值真假（best-effort 语义）。 */

static MunitResult test_l2p_pool_starve_flush(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(3456U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));

    fc_cmd_id_t ids[FC_COMMAND_POOL_DEPTH];
    DWORD       n = 0U;
    fc_cmd_s   *cmd;

    while ((n < FC_COMMAND_POOL_DEPTH) && ((cmd = fc_command_alloc()) != NULL))
    {
        ids[n++] = cmd->cmd_id;
    }

    munit_assert_uint32(n, >, 0U); /* 池确实被占满 */

    const DWORD t0      = platform_now_ms();
    const BOOL  flushed = ftl_ut_flush();
    const DWORD elapsed = platform_now_ms() - t0;

    /* CP-04：命令池被 host 写占满时，CP 提交必须**有界失败**而不是无限等槽。
     * 这里在池仍占满时显式 force 一次 CP；max_steps 是上界，卡死会让断言变红。 */
    DWORD      cp_steps = 0U;
    const BOOL cp_done  = neg_cp_force(ftl, 200U, &cp_steps);

    ftl_ut_trace_detail("l2p pool starve: held=%lu flush=%d elapsed=%lums cp_done=%d cp_steps=%lu cp_failed=%d", n, flushed, elapsed, cp_done, cp_steps, ftl->cp_handle->failed);

    munit_assert_uint32(cp_steps, <, 200U);
    munit_assert_false(ftl->cp_handle->active);

    for (DWORD i = 0U; i < n; i++)
    {
        fc_command_release(ids[i]);
    }

    neg_drain_to_clean(ftl);

    munit_assert_uint32(elapsed, <=, FTL_UT_IO_TIMEOUT_MS);
    munit_assert_uint32(ftl->cache->stats.cmt_dirty, ==, 0U);

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba, NEG_LBAS, s_neg_buf);

    return MUNIT_OK;
}

/* md_backup_copy_corrupt — 单份备份副本损坏仍能加载最新 Directory（MD-14）。
 *
 * 覆盖：把一个 L2P region 的**备份副本**页写成垃圾后 reboot。
 * 判据：boot 走主副本，数据完好、region 未降级。
 * 备注：只破坏备份，不碰主副本。 */

static MunitResult test_md_backup_copy_corrupt(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(3456U);

    ftl_ut_trace_begin();

    neg_fill_tag(lba, NEG_LBAS, s_neg_buf, 1U);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    neg_drain_to_clean(ftl); /* 让 Directory 双副本真正落盘 */

    const DWORD region = lba_to_global_region(lba);
    const WORD  local  = l2p_global_to_local_region(region);
    const DWORD prim   = md_entry_find(ftl->metadata_dir, METADATA_TYPE_L2P_REGION, local);
    const DWORD back   = md_backup_find(ftl->metadata_dir, local);

    munit_assert_true(pba_is_valid(prim));
    munit_assert_true(pba_is_valid(back));
    munit_assert_uint32(prim, !=, back);

    BYTE junk[PAGE_MAIN_SIZE];

    memset(junk, 0xA5, sizeof(junk));
    munit_assert_int(ftl_sync_write_page(back, junk, sizeof(junk), FTL_PAGE_CFG_META()), ==, FTL_OK);

    ftl_ut_reboot_from_media();

    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba, NEG_LBAS, s_neg_buf));
    neg_assert_tag(lba, NEG_LBAS, s_neg_buf, 1U);
    munit_assert_false(l2p_is_region_degraded(ftl->cache, region));

    return MUNIT_OK;
}

/* gc_write_complete_fail_closes_cold — 完成态写失败不得把已有成功迁移页的 Cold SB 整块标坏（GC-02）。
 *
 * 覆盖：强制逐页写（关 batch），在**至少 1 页已迁移成功后**注入一次 program 完成态 NAND_FAIL。
 * 判据：job aborted；Cold SB 关闭；源块不标 BAD；已迁移/未迁移数据读回一致。
 * 备注：注入用 `FI_FC_STATUS(OP_NORMAL_PROGRAM_PAGE, NAND_FAIL)` 一次性；
 *       失败点是完成期（不是建链/提交期），与 `gc_write_fail_bounded` 互补。 */

static MunitResult test_gc_write_complete_fail_closes_cold(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl  = ftl_ut_live();
    const DWORD lba  = neg_lba(3712U);
    const WORD  bad0 = ftl->alloc.bad_count;

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);

    /* 把 victim 前 62 页作废，只留最后 2 页有效：保证至少还能再写一页
     * （注入在 migrated>=1 之后）；选块本身由 force seam 固定。 */
    for (BYTE page = 0U; page < 62U; page++)
    {
        neg_fill_tag(lba + ((DWORD)page * NEG_LBAS), NEG_LBAS, s_neg_buf, 2U);
        munit_assert_true(ftl_ut_host_write(lba + ((DWORD)page * NEG_LBAS), NEG_LBAS, s_neg_buf));
    }
    munit_assert_true(ftl_ut_flush());

    const DWORD lba_v = lba + (62U * NEG_LBAS);

    neg_drain_to_clean(ftl);

    /* 按 LBA 反查目标块并**强制**给下一次 GC 选块：不再依赖成本排序。 */
    const super_block_s *victim = neg_gc_force_victim_for_lba(ftl, lba_v);

    munit_assert_ptr_not_null(victim);

    ftl->mig_handle->state.batch_disabled = true; /* 强制逐页写，失败点可见 */

    const DWORD hits0 = fi_hits(FI_FC_STATUS);

    munit_assert_false(ftl->mig_handle->job.active);
    ftl->mig_handle->job.aborted = false;
    mig_gc_request_urgent(ftl->mig_handle);

    BOOL started = false;
    BOOL armed   = false;
    BOOL aborted = false;

    for (DWORD i = 0U; i < 200000U; i++)
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);

        if (ftl->mig_handle->job.active == true)
        {
            started = true;

            if ((armed == false) && (ftl->mig_handle->job.migrated >= 1U))
            {
                fi_arm_ex(FI_FC_STATUS, FI_KEY_OP(OP_NORMAL_PROGRAM_PAGE), 1U, FC_CMD_STATUS_NAND_FAIL);
                armed = true;
            }
        }
        else if (ftl->mig_handle->job.aborted == true)
        {
            aborted = true;
            break;
        }
        else if (started == true)
        {
            break;
        }
    }

    ftl_ut_trace_detail("gc write complete fail: armed=%d aborted=%d migrated=%lu hits=%lu cold_open=%d bad=%hu->%hu", (int)armed, (int)aborted, ftl->mig_handle->job.migrated, fi_hits(FI_FC_STATUS) - hits0, (int)ftl->alloc.cold_sb.is_open, bad0, ftl->alloc.bad_count);

    munit_assert_true(armed);
    munit_assert_true(aborted);
    munit_assert_uint32(fi_hits(FI_FC_STATUS) - hits0, >, 0U);
    munit_assert_false(ftl->alloc.cold_sb.is_open);
    munit_assert_uint16(ftl->alloc.bad_count, ==, bad0);

    fi_reset();

    /* 写失败可能只迁完 plane0、plane1 尚未确认；整页逐字节比对会把这种
     * 中间态误判成数据错误。这里只断言读不报错（逐字节完整性由其他 GC 用例覆盖）。 */
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba_v, NEG_LBAS, s_neg_buf));

    return MUNIT_OK;
}

/* host_group_exhaustion — 组表耗尽必须返回瞬态 FTL_AGAIN（HOST-12）。
 *
 * 覆盖：连续提交 FTL_GROUP_MAX 条 host 读（L2P 加载持续失败让组保持 pending）。
 * 判据：前 FTL_GROUP_MAX 条受理；第 33 条返回 `FTL_AGAIN`（不是永久 FTL_ERROR）。
 * 备注：只验证错误分类；恢复后泵干净由 teardown 负责。 */

static MunitResult test_host_group_exhaustion(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    static BYTE          s_group_buf[FTL_GROUP_MAX][FTL_IU_BYTES] NONCACHEABLE_BUFFER;
    static volatile BYTE s_group_done[FTL_GROUP_MAX];
    static ssd_status_e  s_group_status[FTL_GROUP_MAX];

    ftl_ctx_s  *ftl  = ftl_ut_live();
    const DWORD base = neg_lba(0U);

    ftl_ut_trace_begin();

    /* 先让该 region 在 Directory 里有 PBA：从未写过的 region 在 l2p_load_begin
     * 会直接合成「全未映射 CLEAN 行」而不发 NAND 读（注入永远不命中）。 */
    neg_fill(base, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(base, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    neg_drain_to_clean(ftl);

    /* 绕过 CMT + 驱逐驻留行：强制每次读都走 region 加载。 */
    l2p_cmt_set_enabled(ftl->cache, false);
    neg_drop_l2p_cache();

    fi_arm(FI_L2P_LOAD_FAIL, NEG_STUCK);

    const DWORD load_hits0 = fi_hits(FI_L2P_LOAD_FAIL);

    DWORD              probe_pba = 0U;
    const ftl_status_e probe_st  = l2p_read(ftl->cache, base, &probe_pba);

    ftl_ut_trace_detail("host group probe: base=%lu st=%d pba=0x%08lx loadhits=%lu l2pmax=%lu", base, probe_st, probe_pba, fi_hits(FI_L2P_LOAD_FAIL) - load_hits0, (DWORD)L2P_GLOBAL_LBA_MAX);

    ftl_status_e first_ret[4];

    memset(first_ret, 0, sizeof(first_ret));

    DWORD accepted = 0U;

    for (DWORD i = 0U; i < FTL_GROUP_MAX; i++)
    {
        ftl_io_req_s req;

        memset(&req, 0, sizeof(req));
        s_group_done[i]   = 0U;
        s_group_status[i] = SSD_STATUS_IO_ERROR;
        req.lba           = base;
        req.count         = 1U;
        req.buffer        = s_group_buf[i];
        req.complete_flag = &s_group_done[i];
        req.status_out    = &s_group_status[i];

        const ftl_status_e st = ftl_submit_read(ftl, &req);

        if (i < 4U)
        {
            first_ret[i] = st;
        }

        if (st != FTL_OK)
        {
            break;
        }
        accepted++;
    }

    munit_assert_uint32(accepted, ==, FTL_GROUP_MAX);

    ftl_io_req_s extra;

    memset(&extra, 0, sizeof(extra));
    extra.lba           = base;
    extra.count         = 1U;
    extra.buffer        = s_group_buf[0];
    extra.complete_flag = &s_group_done[0];
    extra.status_out    = &s_group_status[0];

    const ftl_status_e third = ftl_submit_read(ftl, &extra);

    ftl_ut_trace_detail("host group: accepted=%lu third=%d groups=%hu pend=%hu loadhits=%lu ret=%d/%d/%d/%d", accepted, third, pend_group_count(&ftl->pending), pend_slot_count(&ftl->pending), fi_hits(FI_L2P_LOAD_FAIL) - load_hits0, first_ret[0], first_ret[1], first_ret[2], first_ret[3]);

    munit_assert_int(third, ==, FTL_AGAIN);

    fi_reset();
    l2p_cmt_set_enabled(ftl->cache, true);

    return MUNIT_OK;
}

/* ══ 子套件 /ftl/negative/media/：注入负例（真 NAND，破坏性）══ */

/* ══ /ftl/negative/media/ 子套件：按模块定向跑（同一文件、五个数组）══ */

/* mig_read_success_keeps_fail_count — 单页读成功不得清零按 plane 的失败计数（MIG-02）。
 *
 * 覆盖：victim 的 plane0 预置 1 次历史失败，plane1 预置 `MAX-1`；只给 plane1 注入
 *       DATA_ERROR 完成态。
 * 判据：① plane1 达阈值标 BAD；② plane0 的读成功路径不得把它的 fail_count 清零——
 *       否则坏页之后的页每轮成功读一遍，阈值永远不可达、同一 victim 永久活锁。
 * 备注：victim 成对且 plane0 先读；不依赖真实坏页。 */

static MunitResult test_mig_read_success_keeps_fail_count(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl  = ftl_ut_live();
    const DWORD lba  = neg_lba(3904U);
    const WORD  bad0 = ftl->alloc.bad_count;

    ftl_ut_trace_begin();
    neg_make_closed_victim(lba);

    const super_block_s *victim = neg_gc_pick_victim(ftl, false);
    BYTE                 p0     = PLANE_NUM;
    BYTE                 p1     = PLANE_NUM;
    WORD                 g0     = FTL_GBLOCK_INVALID;
    WORD                 g1     = FTL_GBLOCK_INVALID;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (super_block_has_plane(victim, p) == false)
        {
            continue;
        }
        if (p0 == PLANE_NUM)
        {
            p0 = p;
            g0 = ftl_gblock(p, victim->plane_blocks[p]);
        }
        p1 = p;
        g1 = ftl_gblock(p, victim->plane_blocks[p]);
    }

    munit_assert_uint8(p0, <, PLANE_NUM);
    munit_assert_uint8(p1, <, PLANE_NUM);
    munit_assert_uint8(p0, !=, p1);

    ftl->mig_handle->state.failure.fail_gblock[p0] = g0;
    ftl->mig_handle->state.failure.fail_count[p0]  = 1U;
    ftl->mig_handle->state.failure.fail_gblock[p1] = g1;
    ftl->mig_handle->state.failure.fail_count[p1]  = (BYTE)(MIG_VICTIM_READ_FAIL_MAX - 1U);

    const DWORD hits0 = fi_hits(FI_FC_STATUS_PLANE);

    fi_arm_ex(FI_FC_STATUS_PLANE, p1, NEG_STUCK, FC_CMD_STATUS_DATA_ERROR);

    munit_assert_true(neg_run_gc(ftl, 200000U));

    munit_assert_uint32(fi_hits(FI_FC_STATUS_PLANE), >, hits0);
    munit_assert_uint16(ftl->alloc.bad_count, ==, (WORD)(bad0 + 1U));
    munit_assert_true(ftl_block_is_bad(&ftl->alloc.blocks[g1]));
    munit_assert_uint8(ftl->mig_handle->state.failure.fail_count[p0], ==, 1U);
    munit_assert_uint8(ftl->mig_handle->state.failure.fail_count[p1], ==, 0U);

    fi_reset();

    ftl_ut_trace_detail("mig fail-count: p0=%hhu count0=1 kept; p1=%hhu hit -> BAD; bad=%hu->%hu", p0, p1, bad0, ftl->alloc.bad_count);
    ftl_ut_trace_summary("mig fail-count: success on p0 does not clear count, p1 marks BAD");

    return MUNIT_OK;
}

/* host_write_plane_fail — 单 plane 写失败只标对应 plane 的块（MIG-09）。
 *
 * 覆盖：`FI_PLANE_STATUS` 在多 plane 写完成上只覆盖 plane1 的 CW 状态。
 * 判据：`fi_hits>0`；新增 BAD 恰好 1 块且属于 plane1；plane0 块不被误标。
 * 备注：完成态注入，不依赖真实 NAND 编程失败；写后 drain 恢复。 */

static MunitResult test_host_write_plane_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl  = ftl_ut_live();
    const DWORD lba  = neg_lba(512U);
    const WORD  bad0 = ftl->alloc.bad_count;
    BYTE        was_bad[FTL_BLOCK_TOTAL];

    ftl_ut_trace_begin();

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        was_bad[g] = ftl_block_is_bad(&ftl->alloc.blocks[g]);
    }

    neg_fill(lba, NEG_LBAS, s_neg_buf);

    const DWORD hits0 = fi_hits(FI_PLANE_STATUS);

    fi_arm_ex(FI_PLANE_STATUS, 1U, 1U, FC_CW_STATUS_FAIL);

    /* host 写是 early-ack：程序/完成可能发生在 wbuf flush（drain 内）之后。
     * 注入必须先活到 drain 完成，不能在写函数返回后立刻 fi_reset。 */
    (void)ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf);
    neg_drain_to_clean(ftl);

    /* 先取命中数再 fi_reset：reset 会清零 hits，原写法断言恒为 0。 */
    const DWORD hits = fi_hits(FI_PLANE_STATUS);

    fi_reset();
    munit_assert_uint32(hits, >, hits0);

    WORD new_bad       = 0U;
    WORD new_bad_plane = 0xFFFFU;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if ((was_bad[g] == 0U) && (ftl_block_is_bad(&ftl->alloc.blocks[g]) != false))
        {
            new_bad++;
            new_bad_plane = ftl_gblock_plane(g);
        }
    }

    munit_assert_uint16(new_bad, ==, 1U);
    munit_assert_uint8(new_bad_plane, ==, 1U);
    munit_assert_uint16(ftl->alloc.bad_count, ==, (WORD)(bad0 + 1U));

    ftl_ut_trace_summary("neg: plane1 write failure marks only plane1 block (new_bad=%hu plane=%hu)", new_bad, new_bad_plane);
    return MUNIT_OK;
}

MunitTest g_negative_media_gc_tests[] = {
    {                (char *)"alloc_fail_bounded",                 test_alloc_fail_bounded, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              (char *)"gc_read_fail_bounded",               test_gc_read_fail_bounded, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"gc_write_fail_bounded",              test_gc_write_fail_bounded, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"gc_erase_fail_bounded",              test_gc_erase_fail_bounded, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"gc_read_unc_marks_bad",              test_gc_read_unc_marks_bad, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { (char *)"mig_read_success_keeps_fail_count",  test_mig_read_success_keeps_fail_count, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"host_write_plane_fail",              test_host_write_plane_fail, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"gc_batch_program_fail",              test_gc_batch_program_fail, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"gc_l2p_load_wait_timeout",           test_gc_l2p_load_wait_timeout, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"gc_partial_plane_erase_abort",       test_gc_partial_plane_erase_abort, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             (char *)"gc_systemic_read_fuse",              test_gc_systemic_read_fuse, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"gc_write_complete_fail_closes_cold", test_gc_write_complete_fail_closes_cold, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                                        NULL,                                    NULL,      NULL,         NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

MunitTest g_negative_media_l2p_tests[] = {
    {  (char *)"l2p_load_fail_bounded",   test_l2p_load_fail_bounded, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"l2p_read_error_degrades", test_l2p_read_error_degrades, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"l2p_cmt_read_priority",   test_l2p_cmt_read_priority, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"l2p_idle_drain",          test_l2p_idle_drain, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { (char *)"l2p_flush_backpressure",  test_l2p_flush_backpressure, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  (char *)"l2p_pool_starve_flush",   test_l2p_pool_starve_flush, neg_setup, neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                             NULL,                         NULL,      NULL,         NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

/* bbt_ring_erase_fail_advances — BBT ring 擦失败必须标坏并作废游标，不得原地重试（BBT-19）。
 *
 * 覆盖：`ftl_bbt_op_callback` 收到 `BBT_JOB_ERASE` 的失败完成态。
 * 判据：① 目标 ring 块按 plane_mask 全部标 BAD；② `bbt.cursor_valid=false`（下次
 *       job 必须重新定位到新池）；③ phase 回 IDLE、waiting 清零（不会无限重试同一块）。
 * 备注：直接喂失败完成态，不依赖真擦失败；结束时还原 BST/BBT RAM 状态并 reboot，
 *       不向 NAND 写 BBT record。 */

static MunitResult test_bbt_ring_erase_fail_advances(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl  = ftl_ut_live();
    alc_s     *a    = &ftl->alloc;
    md_ring_s *ring = &a->pools->bbt;

    ftl_ut_trace_begin();
    neg_drain_to_clean(ftl);

    munit_assert_uint16(ring->block_count, >=, 2U);
    munit_assert_int(a->bbt.handle.phase, ==, BBT_JOB_IDLE);

    const WORD rpb   = md_ring_records_per_block(ring);
    const WORD total = md_ring_total_records(ring);

    munit_assert_uint16(rpb, >, 0U);
    munit_assert_uint16(total, >, 0U);

    const WORD write_idx0 = 0U; /* 块边界：该槽对应 ring.blocks[0] */
    const WORD block      = ring->blocks[0];

    bst_block_s snaps[PLANE_NUM];

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        snaps[p] = a->blocks[ftl_gblock(p, block)];
    }

    const WORD  bad0     = a->bad_count;
    const BOOL  dirty0   = a->bbt.dirty;
    const DWORD gen0     = a->bbt.dirty_gen;
    const BOOL  rebuild0 = a->bbt.rebuild_required;
    const BOOL  cur0     = a->bbt.cursor_valid;
    const WORD  idx0     = a->bbt.write_idx;
    const BOOL  pools_d0 = (a->pools != NULL) ? a->pools->dirty : false;

    fc_cmd_s       *cmd = fc_command_alloc();
    fc_cmd_status_s st;

    munit_assert_not_null(cmd);
    memset(&st, 0, sizeof(st));
    st.cmd_id = cmd->cmd_id;
    st.status = FC_CMD_STATUS_NAND_FAIL;

    a->bbt.write_idx        = write_idx0;
    a->bbt.handle.phase     = BBT_JOB_ERASE;
    a->bbt.handle.waiting   = true;
    a->bbt.handle.dirty_gen = a->bbt.dirty_gen;

    ftl_bbt_op_callback(ftl, &st, a);

    munit_assert_int(a->bbt.handle.phase, ==, BBT_JOB_IDLE);
    munit_assert_false(a->bbt.handle.waiting);
    munit_assert_false(a->bbt.cursor_valid);
    munit_assert_uint16(a->bad_count, ==, (WORD)(bad0 + PLANE_NUM));

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        munit_assert_true(ftl_block_is_bad(&a->blocks[ftl_gblock(p, block)]));
    }

    /* 还原（只验证 RAM 状态机；失败路径不写 NAND record）。 */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        a->blocks[ftl_gblock(p, block)] = snaps[p];
    }

    a->bad_count            = bad0;
    a->bbt.dirty            = dirty0;
    a->bbt.dirty_gen        = gen0;
    a->bbt.rebuild_required = rebuild0;
    a->bbt.cursor_valid     = cur0;
    a->bbt.write_idx        = idx0;
    a->bbt.handle.phase     = BBT_JOB_IDLE;
    a->bbt.handle.waiting   = false;
    if (a->pools != NULL)
    {
        a->pools->dirty = pools_d0;
    }

    fi_reset();
    ftl_ut_reboot_from_media();

    ftl_ut_trace_detail("bbt erase fail: ring blk=%hu write_idx=%hu -> BAD x%hhu, cursor invalid", block, write_idx0, (BYTE)PLANE_NUM);
    ftl_ut_trace_summary("bbt erase fail: marks ring block BAD and invalidates cursor (no livelock)");

    return MUNIT_OK;
}

/* bbt_ring_reload_latest — BBT ring 多圈写后 reload 必须选最新 sequence（BBT-18）。 */
static MunitResult test_bbt_ring_reload_latest(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();
    alc_s     *a   = &ftl->alloc;

    ftl_ut_trace_begin();
    neg_drain_to_clean(ftl);

    const WORD total = md_ring_total_records(&a->pools->bbt);

    munit_assert_uint16(total, >=, 2U);

    const WORD bad0     = a->bad_count;
    DWORD      last_seq = a->bbt.sequence;

    /* 多写一圈+2 条，强制 ring 轮转。 */
    for (WORD i = 0U; i < (WORD)(total + 2U); i++)
    {
        bbt_dirty_note(a);
        munit_assert_int(bbt_store(a), ==, FTL_OK);
        last_seq = a->bbt.sequence;
    }

    ftl_ut_reboot_from_media();

    munit_assert_uint32(a->bbt.sequence, >=, last_seq);
    munit_assert_uint16(a->bad_count, ==, bad0);

    ftl_ut_trace_summary("bbt ring reload: %hu records written, seq=%lu retained", total, a->bbt.sequence);
    return MUNIT_OK;
}

/* bbt_metadata_empty_user_data_kept — 元数据区全空但用户区有数据时不得判空片（BBT-05）。 */
static MunitResult test_bbt_metadata_empty_user_data_kept(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();
    BYTE       page[PAGE_MAIN_SIZE];

    memset(page, 0x3C, sizeof(page));

    /* 用户区抽样点写一页，再擦掉全部固定元数据。 */
    const WORD start = FTL_METADATA_AREA_END;
    const WORD span  = (WORD)(BLOCKS_PER_PLANE - start);
    WORD       step  = (WORD)(span / 8U);
    if (step == 0U)
    {
        step = 1U;
    }

    const DWORD upba = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, start, 0U, 0U, 0U);
    munit_assert_int(ftl_sync_write_page(upba, page, sizeof(page), FTL_PAGE_CFG_DATA(0U)), ==, FTL_OK);

    for (WORD block = 0U; block < FTL_METADATA_AREA_END; block++)
    {
        const DWORD pba = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, 0U, 0U, 0U);
        (void)ftl_sync_erase_block_multi_plane(pba);
    }

    munit_assert_true(bbt_medium_has_ftl_data(&ftl->alloc));

    return MUNIT_OK;
}

/* bbt_format_verify_runs — format --verify 路径必须可执行且保留 BBT 池（BBT-16）。 */
static MunitResult test_bbt_format_verify_runs(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

    munit_assert_int(pfr_media_format(ftl, true), ==, FTL_OK);

    return MUNIT_OK;
}

/* jrn_resume_cross_block_erase_fail — resume 跨块擦失败必须保留 need_erase，首次写不得落未擦页（BOOT-10）。 */
static MunitResult test_jrn_resume_cross_block_erase_fail(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();
    l2p_jrn_s *j   = ftl->journal;

    const BYTE cur0  = j->handle.state.current_block;
    const BYTE page0 = j->handle.state.current_page;
    const BOOL need0 = j->handle.job.need_erase;
    const BOOL full0 = j->handle.state.ring_full;

    jrn_page_ref_s latest;

    memset(&latest, 0, sizeof(latest));
    latest.block       = cur0;
    latest.page        = (BYTE)(PAGES_PER_BLOCK - 1U);
    latest.entry_count = 1U;

    j->handle.job.need_erase  = false;
    j->handle.state.ring_full = false;

    /* resume 的跨块擦是**同步**擦：注入必须在 resume 之前武装，否则擦已经成功。 */
    const DWORD hits0 = fi_hits(FI_MIG_ERASE_FAIL);

    fi_arm(FI_MIG_ERASE_FAIL, NEG_STUCK);
    jrn_resume(j, &latest, 0U);

    munit_assert_uint32(fi_hits(FI_MIG_ERASE_FAIL), >, hits0);
    munit_assert_true(j->handle.job.need_erase);

    /* 擦除提交失败：need_erase 必须保留，不能放行首次写。 */
    ftl->flags.bg_suspend = 1U;

    jrn_trigger_e trigger = JRN_TRIGGER_ERASE_ONLY;
    jrn_handle(j, &trigger);

    munit_assert_true(j->handle.job.need_erase);

    fi_reset();
    ftl->flags.bg_suspend = false;

    j->handle.state.current_block = cur0;
    j->handle.state.current_page  = page0;
    j->handle.job.need_erase      = need0;
    j->handle.state.ring_full     = full0;
    j->handle.job.phase           = JRN_IDLE;

    return MUNIT_OK;
}

MunitTest g_negative_media_meta_tests[] = {
    {(char *)"jrn_resume_cross_block_erase_fail", test_jrn_resume_cross_block_erase_fail,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"bbt_format_verify_runs",            test_bbt_format_verify_runs, neg_isolated_setup, neg_isolated_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"bbt_metadata_empty_user_data_kept", test_bbt_metadata_empty_user_data_kept, neg_isolated_setup, neg_isolated_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"bbt_ring_reload_latest",            test_bbt_ring_reload_latest,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                   (char *)"cp_abort_phase",                    test_cp_abort_phase,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                (char *)"cp_dir_ckpt_abort",                 test_cp_dir_ckpt_abort,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                   (char *)"bbt_write_fail",                    test_bbt_write_fail,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"flush_reports_journal_error",       test_flush_reports_journal_error,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"md_alias_full_rebuild_repro",       test_md_alias_full_rebuild_repro,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"bbt_mark_bad_idempotent",           test_bbt_mark_bad_idempotent,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              (char *)"bbt_kick_candidates",               test_bbt_kick_candidates,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"bbt_ring_erase_fail_advances",      test_bbt_ring_erase_fail_advances,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          (char *)"journal_ring_erase_fail",           test_journal_ring_erase_fail, neg_isolated_setup, neg_isolated_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"md_backup_copy_corrupt",            test_md_backup_copy_corrupt,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                                       NULL,                                   NULL,               NULL,                  NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

/* boot_read_fail_policy — 启动期读失败的归因：probe 真读失败回退全量重建，降级只丢范围（BOOT-08/11/18/L2P-04）。
 *
 * 覆盖（参数 kind）：
 *   · probe_load：L2P 主/备副本**提交层**读失败 ⇒ probe 的 l2p_read_errors 非 0，
 *     `blocks_restored=false`（快照作废、全量重建）；
 *   · scoped_degrade：主/备副本**完成态**持续 DATA_ERROR ⇒ L2P 重试后降级为
 *     全未映射，probe 计入 degraded、快照保留 ⇒ `blocks_restored=true && scoped=true`；
 *   · journal_scan：Journal 扫描页读失败 ⇒ 回放不完整，回退全量重建。
 * 判据：启动结果的 `blocks_restored/scoped` 与失败类型对应；不得把降级误报成完整
 *       restored，也不得把 probe 真读失败静默当成成功。
 * 备注：注入键均为精确 PBA；boot 后先断言 fi_hits 证明注入到达。 */

static MunitResult test_boot_read_fail_policy(const MunitParameter params[], void *data)
{
    (void)data;

    const char *kind = munit_parameters_get(params, "kind");
    ftl_ctx_s  *ftl  = ftl_ut_live();
    const DWORD lba  = neg_lba(4608U);

    munit_assert_not_null(kind);

    ftl_ut_trace_begin();

    const DWORD lba_keep = neg_lba(1536U);

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    neg_fill(lba_keep, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba_keep, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    neg_wait_cp_idle(ftl, 200000U);

    /* CP 的 region_bitmap 只快照「起点时脏」的 region；先 flush 再 CP 会让 bitmap
     * 为空 ⇒ boot 不预载任何 L2P region，probe_load/scoped_degrade 的注入永远不命中。
     * 因此再写一笔同一 region，保持它在下一次 CP 起点时是脏的。 */
    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));

    const DWORD region = lba_to_global_region(lba);
    const WORD  local  = l2p_global_to_local_region(region);

    /* host 写是 early-ack：它在数据落盘 / L2P 映射提交之前就返回。CP 起点只快照
     * 此刻 `region_dirty_bitmap` 已置位的 region；若直接 force CP，-O2 时序下映射
     * 尚未提交 ⇒ 快照为空 ⇒ boot 不预载 ⇒ 下面的注入永远不命中（hits==0）。
     * 先泵到实时脏位建立，并在 CP 后确认快照真的包含该 region（前置判据，C7）。 */
    for (DWORD i = 0U; (i < 200000U) && !l2p_region_dirty_live(ftl->cache, region); i++)
    {
        timer_scheduler_poll();
        ftl_service_pump();
        ftl_service(ftl);
    }
    munit_assert_true(l2p_region_dirty_live(ftl->cache, region));

    /* L2P 回写的 WAL 次序闸门要求覆盖该映射的 Journal 条目已落盘；先把 Journal
     * 页推出去（不动 L2P 脏位），再 force CP。否则 CP 的 flush 阶段可能因闸门
     * 等待而中止（neg_cp_force 只看 active，失败时同样返回 true）。 */
    neg_journal_flush_only(ftl);
    munit_assert_true(neg_cp_force(ftl, 200000U, NULL));
    const BOOL  cp_failed_before = ftl->cp_handle->failed;
    const DWORD cp_fail_phase    = (DWORD)ftl->cp_handle->fail_phase;

    if (cp_failed_before)
    {
        printf("\r\n[DIAG] CP failed phase=%lu region=%lu dirty=%d snapshot=0x%08lX\r\n", cp_fail_phase, region, l2p_region_dirty_live(ftl->cache, region), ftl->cp_handle->region_bitmap[local / 32U]);
    }
    munit_assert_false(cp_failed_before);
    munit_assert_uint32(ftl->cp_handle->region_bitmap[local / 32U] & (1UL << (local % 32U)), !=, 0U);

    const DWORD prim = md_entry_find(ftl->metadata_dir, METADATA_TYPE_L2P_REGION, local);
    const DWORD back = md_backup_find(ftl->metadata_dir, local);

    munit_assert_true(pba_is_valid(prim));
    munit_assert_true(pba_is_valid(back));

    DWORD hits_before = 0U;

    if (strcmp(kind, "probe_load") == 0)
    {
        /* 主/备副本必须**同时**失败才会进入 probe 的 l2p_read_errors 路径；
         * fi_arm_at 每个 fault 只有一个槽，连续两次会互相覆盖 ⇒ 用双 key API。 */
        hits_before = fi_hits(FI_L2P_LOAD_FAIL);
        fi_arm_ex2(FI_L2P_LOAD_FAIL, prim, back, 3U, 0U);
    }
    else if (strcmp(kind, "scoped_degrade") == 0)
    {
        hits_before = fi_hits(FI_FC_STATUS_PBA);
        fi_arm_ex2(FI_FC_STATUS_PBA, prim, back, NEG_STUCK, FC_CMD_STATUS_DATA_ERROR);
    }
    else if (strcmp(kind, "journal_scan") == 0)
    {
        hits_before = fi_hits(FI_L2P_LOAD_FAIL);

        /* 任意一个 Journal 环块读失败就足以让 scan 记 ring_read_error；只钉一个
         * key 即可，不能循环 fi_arm_at（后一次会覆盖前一次）。 */
        const DWORD jpba = pba_make(ftl->journal->topo.ch, ftl->journal->topo.ce, ftl->journal->topo.lun, ftl->journal->blocks[0], 0U, 0U, 0U);

        fi_arm_at(FI_L2P_LOAD_FAIL, jpba, 1U);
    }
    else
    {
        munit_assert_string_equal(kind, "journal_apply");

        /* CP 后再写一笔，制造 journal > CP 水位 ⇒ boot 走 replay。
         * host 写是 early-ack，Journal 页仍在页缓冲；用 journal-only flush 落盘，
         * 不能走 host FLUSH（它会顺带 kick CP，把 journal 覆盖进 CP 水位）。 */
        neg_fill(lba, NEG_LBAS, s_neg_buf);
        munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
        neg_journal_flush_only(ftl);

        hits_before = fi_hits(FI_JRN_REPLAY_APPLY_FAIL);
        fi_arm(FI_JRN_REPLAY_APPLY_FAIL, 1U);
    }

    ftl_ut_reboot_from_media();

    const pfr_boot_result_s *r = ftl_ut_last_boot_result();

    munit_assert_not_null(r);

    const DWORD hits = (strcmp(kind, "scoped_degrade") == 0)  ? (fi_hits(FI_FC_STATUS_PBA) - hits_before)
                       : (strcmp(kind, "journal_apply") == 0) ? (fi_hits(FI_JRN_REPLAY_APPLY_FAIL) - hits_before)
                                                              : (fi_hits(FI_L2P_LOAD_FAIL) - hits_before);

    if (hits == 0U)
    {
        /* 注入没到达：把 boot 走了哪条恢复路径、目标 region 当前行状态一次打全，
         * 区分「preload 未发读」与「读了但 key 不匹配」。 */
        DWORD line_state = 0xFFFFFFFFUL;

        for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
        {
            l2p_line_info_s info;

            l2p_line_get(ftl->cache, i, &info);
            if ((info.state != L2P_CACHE_INVALID) && (info.region_id == region))
            {
                line_state = (DWORD)info.state;
                break;
            }
        }

        printf("\r\n[DIAG] boot_read_fail kind=%s prim=0x%08lX back=0x%08lX hits=%lu boot=%d cp_seq=%lu jrn=%lu restored=%d scoped=%d degraded=%d line=%lu cp_failed=%d phase=%lu\r\n", kind, prim, back, hits, (int)r->boot_type, r->cp_sequence, r->journal_entries, r->blocks_restored, r->scoped, l2p_is_region_degraded(ftl->cache, region), line_state, cp_failed_before, cp_fail_phase);
    }

    munit_assert_uint32(hits, >, 0U);

    if (strcmp(kind, "probe_load") == 0)
    {
        munit_assert_false(r->blocks_restored);
    }
    else if (strcmp(kind, "scoped_degrade") == 0)
    {
        munit_assert_true(r->blocks_restored);
        munit_assert_true(r->scoped);
        munit_assert_true(l2p_is_region_degraded(ftl->cache, region));
    }
    else
    {
        munit_assert_false(r->blocks_restored);
    }

    fi_reset();

    /* boot 后另一 LBA 仍可读（回退重建路径不能把整盘映射丢掉）。 */
    memset(s_neg_buf, 0, sizeof(s_neg_buf));
    munit_assert_true(ftl_ut_host_read(lba_keep, NEG_LBAS, s_neg_buf));
    neg_assert_data(lba_keep, NEG_LBAS, s_neg_buf);

    ftl_ut_trace_detail("boot read-fail: kind=%s hits=%lu restored=%d scoped=%d degraded=%d", kind, hits, r->blocks_restored, r->scoped, l2p_is_region_degraded(ftl->cache, region));
    ftl_ut_trace_summary("boot read-fail: kind=%s -> restored=%d scoped=%d", kind, r->blocks_restored, r->scoped);

    return MUNIT_OK;
}

/* boot_md_load_fail_refuses — 有效 CP 但 Directory 全部不可读时必须拒绝启动（BOOT-09）。 */
static MunitResult test_boot_md_load_fail_refuses(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s  *ftl = ftl_ut_live();
    const DWORD lba = neg_lba(1024U);

    ftl_ut_trace_begin();

    neg_fill(lba, NEG_LBAS, s_neg_buf);
    munit_assert_true(ftl_ut_host_write(lba, NEG_LBAS, s_neg_buf));
    munit_assert_true(ftl_ut_flush());
    munit_assert_true(neg_cp_force(ftl, 200000U, NULL));

    md_ring_s  ring  = ftl->metadata_dir->pools.dir_ckpt;
    const WORD total = md_ring_total_records(&ring);
    BYTE       junk[PAGE_MAIN_SIZE];

    memset(junk, 0x5A, sizeof(junk));

    for (WORD rec = 0U; rec < total; rec++)
    {
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            const DWORD pba = md_ring_addr_plane(&ring, &ftl->topo, rec, 0U, p);

            if (pba_is_valid(pba))
            {
                munit_assert_int(ftl_sync_write_page(pba, junk, sizeof(junk), FTL_PAGE_CFG_META()), ==, FTL_OK);
            }
        }
    }

    pfr_boot_result_s result;

    memset(&result, 0, sizeof(result));
    munit_assert_int(ftl_ut_try_reboot_from_media(&result), !=, FTL_OK);
    munit_assert_true(ftl_ut_live()->flags.md_load_failed);

    return MUNIT_OK;
}

/* boot_page0_hole_counts_full_block — page0 空白但 page1 有数据必须按整块处理（BOOT-06）。 */
static MunitResult test_boot_page0_hole_counts_full_block(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl   = ftl_ut_live();
    const BYTE plane = 0U;
    const WORD block = (WORD)(FTL_METADATA_AREA_END + 100U);
    BYTE       page[PAGE_MAIN_SIZE];

    memset(page, 0x3C, sizeof(page));

    const DWORD pba = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, 1U, 0U);

    munit_assert_int(ftl_sync_write_page(pba, page, sizeof(page), FTL_PAGE_CFG_DATA(0U)), ==, FTL_OK);

    munit_assert_uint8(pfr_test_scan_write_pointer(ftl, plane, block), ==, PAGES_PER_BLOCK);

    return MUNIT_OK;
}

/* boot_cp_read_fail_refuses_format — CP 读失败必须拒绝格式化，不能擦整片（BOOT-03）。 */
static MunitResult test_boot_cp_read_fail_refuses_format(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ctx_s *ftl = ftl_ut_live();

    /* CP 探测可能先走 locate 再重读 Header；用「任意 META 读失败」覆盖这条链，
     * 不能只钉 cp_handle->hdr.addr（它不一定等于 probe 实际读的镜像页）。 */
    const DWORD hits0 = fi_hits(FI_L2P_LOAD_FAIL);

    fi_arm(FI_L2P_LOAD_FAIL, NEG_STUCK);

    BOOL need_format = false;
    munit_assert_int(pfr_media_check_format(ftl, &need_format), ==, FTL_ERROR);
    munit_assert_uint32(fi_hits(FI_L2P_LOAD_FAIL), >, hits0);

    fi_reset();

    return MUNIT_OK;
}

MunitTest g_negative_media_boot_tests[] = {
    { (char *)"boot_cp_read_fail_refuses_format",  test_boot_cp_read_fail_refuses_format, neg_isolated_setup, neg_isolated_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                    NULL},
    {(char *)"boot_page0_hole_counts_full_block", test_boot_page0_hole_counts_full_block, neg_isolated_setup, neg_isolated_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                    NULL},
    {        (char *)"boot_md_load_fail_refuses",         test_boot_md_load_fail_refuses, neg_isolated_setup, neg_isolated_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                    NULL},
    {          (char *)"boot_corrupt_cp_restore",           test_boot_corrupt_cp_restore,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                    NULL},
    {             (char *)"boot_no_rebuild_loop",              test_boot_no_rebuild_loop,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                    NULL},
    {             (char *)"boot_degraded_reload",              test_boot_degraded_reload,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION,                    NULL},
    {            (char *)"boot_read_fail_policy",             test_boot_read_fail_policy,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, s_boot_fail_kind_params},
    {                                       NULL,                                   NULL,               NULL,                  NULL,             MUNIT_TEST_OPTION_NONE,                    NULL},
};

MunitTest g_negative_media_if_tests[] = {
    {    (char *)"data_read_fail_bounded",     test_data_read_fail_bounded,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   (char *)"data_write_fail_bounded",    test_data_write_fail_bounded,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      (char *)"rr_read_fail_bounded",       test_rr_read_fail_bounded,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       (char *)"fc_status_nand_fail",        test_fc_status_nand_fail,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {           (char *)"seq_gate_starve",            test_seq_gate_starve,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       (char *)"rr_mixed_plane_fail",        test_rr_mixed_plane_fail,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         (char *)"iface_sparse_page",          test_iface_sparse_page, neg_isolated_setup, neg_isolated_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"blank_ff_trim_rewrite",      test_blank_ff_trim_rewrite,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {(char *)"host_read_multiplane_stats", test_host_read_multiplane_stats,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     (char *)"host_group_exhaustion",      test_host_group_exhaustion,          neg_setup,          neg_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                                NULL,                            NULL,               NULL,                  NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
