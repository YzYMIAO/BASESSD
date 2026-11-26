/*
 * ftl_flush.c — 异步 FLUSH / TRIM job
 *
 * 职责：
 *   · FLUSH：汇总 wbuf / L2P / Journal / 写确认四类在飞数据，全部落定后回执；
 *     无进展超时按失败语义结算（未确认写页 ⇒ deferred_error）；
 *   · TRIM：逐 LBA 解除映射，Journal 先于 L2P；每步限额，无进展超时中止；
 *   · CP 触发快照构造（flush / sched 后台策略共用）。
 *
 * 关键机制：
 *   · 完成是轮询式：flush_step / trim_step 由 service 每轮推进，begin 提交即返回；
 *   · flush 完成判据包含早回执页表（seq.write.count + write_inflight）与活跃写序列；
 *   · TRIM 游标只在成功/跳过时前进，资源未就绪原地等待（deadline 兜底）。
 *
 * 与其他模块：
 *   · 上游：HIL controller 调 begin/step/abort；ftl_service 调 step；
 *   · 下游：wbuf（flush/drop）、l2p_cache（owner flush / 读写）、journal（reserve/append/
 *     水位）、checkpoint（触发快照）、pending（写确认屏障）。
 */

#include "ftl_flush.h"

#include "assertion.h"
#include "ftl.h"
#include "journal.h"
#include "l2p_cache.h"
#include "log.h"
#include "metadata.h"
#include "pending.h"
#include "timer.h"
#include "wbuf.h"

/* 文件级宏 / 变量 */
#define FTL_TRIM_STEP_LBAS (8U)

#define FTL_TRIM_TIMEOUT_MS (1000U) /* 无进展上限：Journal ring_full / L2P 加载卡死时让 TRIM 失败 */

static log_ratelimit_s s_flush_log; /* flush 正常耗时（5 s） */

static log_ratelimit_s s_flush_err_log; /* flush 失败 / 超时（5 s） */

/* ── cp ── */

/**
 * @brief 汇总本轮 CP 触发信息
 *
 * @param ftl    实例
 * @param reason 触发原因
 * @param force  显式请求
 * @return 填好的 trigger
 */
cp_trigger_s ftl_cp_trigger_build(const ftl_ctx_s *ftl, cp_reason_e reason, BOOL force)
{
    cp_trigger_s trigger;

    trigger.now_ms            = platform_now_ms();
    trigger.total_writes      = ftl->stats.total_writes;
    trigger.journal_seq       = jrn_sequence_get(ftl->journal);
    trigger.journal_watermark = jrn_watermark_get(ftl->journal);
    trigger.emergency         = jrn_is_emergency(ftl->journal);
    trigger.force             = force;
    trigger.reason            = reason;
    return trigger;
}

/* ── flush ── */

/**
 * @brief host flush 也是"该落一次 checkpoint"的信号：没有 CP 时，掉电后只能拿初始 CP
 * （无块表）做全量重建。异步启动，限流 5s（半个 T3），避免 host 频繁 flush 写放大。
 *
 * @param ftl 实例（触发参数取自本实例的 CP / journal 状态）
 */
static void ftl_flush_kick_cp(ftl_ctx_s *ftl)
{
    if (cp_flush_kick_is_due(ftl->cp_handle) == false)
    {
        return;
    }

    const cp_trigger_s trigger = ftl_cp_trigger_build(ftl, CP_REASON_EXPLICIT, true);

    cp_handle(ftl->cp_handle, &trigger);
}

/**
 * @brief 取消 flush：只清 job 标志
 *
 * 在飞的 wbuf / L2P / journal 操作各自完成，不破坏不变量。
 *
 * @param ftl 实例
 */
void ftl_flush_abort(ftl_ctx_s *ftl)
{
    // 只摘 job 标志：wbuf / L2P / journal 的在飞操作有各自的服务路径，
    // 继续完成不会破坏不变量。下一次 begin 以新的起点重新开始。
    ftl->flush.active = false;
}

/**
 * @brief 启动异步 FLUSH job（提交即返回）
 *
 * 完成由 ftl_flush_step 推进，墙钟上限见 FTL_FLUSH_TIMEOUT_MS。
 *
 * @param ftl 实例
 */
void ftl_flush_begin(ftl_ctx_s *ftl)
{
    if (ftl->flush.active)
    {
        return; /* 幂等：已有在飞的 flush */
    }

    ftl->flush.active           = true;
    ftl->flush.start_ms         = platform_now_ms();
    ftl->flush.last_progress_ms = ftl->flush.start_ms;
    ftl->flush.progress         = 0U;
    ftl_seq_reset(ftl);

    // 序列必须先退出，flush 的 L2P/journal 写才不会被门控。
    ftl_cache_seq_yield(ftl);

    // 先把页打包缓冲里尚未编程的写编程掉：它们还没进 L2P/journal，"落盘"不成立。
    // 遇瞬态背压会保留缓冲，后续 step 的 MAYBE_FLUSH_IDLE 会重试。
    wb_trigger_s trigger;

    memset(&trigger, 0, sizeof(trigger));
    trigger.kind = WBUF_TRIGGER_FLUSH;
    (void)wb_handle(ftl->wbuf, &trigger, NULL);
}

/**
 * @brief 推进 FLUSH 一轮
 *
 * 推进 wbuf / L2P / journal；四类在飞数据全部落定（或失败/超时）才返回 true。
 *
 * @param ftl 实例
 * @return true 已完成；false 仍需下一轮
 */
BOOL ftl_flush_step(ftl_ctx_s *ftl)
{
    if (ftl->flush.active == false)
    {
        return true; /* 没有在飞的 flush：对调用者而言就是"已完成" */
    }

    const BYTE die = topo_2_die(ftl->topo);

    // ① wbuf：把打开的窗口推下去，并取当前状态。
    wb_trigger_s wb_trigger;

    memset(&wb_trigger, 0, sizeof(wb_trigger));
    wb_trigger.kind = WBUF_TRIGGER_MAYBE_FLUSH_IDLE;

    wb_status_s wb_status;
    (void)wb_handle(ftl->wbuf, &wb_trigger, &wb_status);

    // ② L2P：继续 kick 本 owner 的脏行（受共享 credit 池门控）。
    l2p_owner_flush_kick(ftl->cache, die, L2P_CACHE_SIZE, true);

    // ③ Journal：推进异步刷页 job。
    const jrn_trigger_e jrn_trigger = JRN_TRIGGER_FLUSH;
    jrn_handle(ftl->journal, &jrn_trigger);

    // ④ 完成判据：四类在飞数据全部落定。待入账中的写确认也必须先完成——它对应的
    // L2P 脏行还没出现，否则 flush 会「提前完成」。
    const BOOL confirm_pending    = pend_find_op(&ftl->pending, PENDING_OP_WRITE_CONFIRM, NULL);
    // 早回执后 cache 页的等待对象变了：组早已完成，flush 要等**已编程页表**排空
    // （每页都已被后继/收尾证明、且已确认进 L2P），否则 SYNCHRONIZE CACHE 会在
    // 「未证明」窗口里提前回执（屏障语义）。
    // 页在**完成时**才进页表 ⇒ 屏障必须同时看「在飞成员」：控制器可能已对写段
    // 提前回执（BOT 串行下 SYNCHRONIZE CACHE 紧接着到来），此时命令还在 NAND 里编程。
    const BOOL cache_wb_pending   = (ftl->cache_stream.seq.write.count != 0U) || (ftl->cache_stream.seq.write_inflight != 0U);
    const BOOL cache_write_active = (ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE);
    const BOOL wb_done            = (wb_status.state == WBUF_STATE_IDLE) && (wb_status.occupied == 0U);
    const BOOL l2p_done           = l2p_flush_owner_idle(ftl->cache, die);
    const BOOL jrn_done           = jrn_is_idle(ftl->journal) && (jrn_has_pending_entries(ftl->journal) == false);
    const BOOL jrn_fail           = jrn_has_write_failure(ftl->journal);
    const BOOL fail               = jrn_fail;

    // 无进展上限：wbuf / L2P / journal / 写确认任一项在推进就续期；连续 1 s 没有任何
    // 推进才判超时（满盘 flush 的正常时长可以超过 1 s，见 FTL_FLUSH_TIMEOUT_MS）。
    const DWORD now_ms = platform_now_ms();
    DWORD       sig    = ((DWORD)ftl->cache_stream.seq.write.count) |
                ((DWORD)ftl->cache_stream.seq.write_inflight << 4U) |
                ((DWORD)wb_status.occupied << 8U) |
                ((DWORD)wb_status.state << 12U) |
                ((DWORD)(l2p_done ? 1U : 0U) << 16U) |
                ((DWORD)(jrn_done ? 1U : 0U) << 17U) |
                ((DWORD)(confirm_pending ? 1U : 0U) << 18U) |
                ((DWORD)(cache_write_active ? 1U : 0U) << 19U);

    sig ^= (jrn_sequence_get(ftl->journal) * 2654435761U);
    sig ^= ((DWORD)jrn_watermark_get(ftl->journal) << 7U);

    if (sig != ftl->flush.progress)
    {
        ftl->flush.progress         = sig;
        ftl->flush.last_progress_ms = now_ms;
    }

    const DWORD flush_ms = now_ms - ftl->flush.start_ms;
    const DWORD idle_ms  = (ftl->flush.last_progress_ms != 0U) ? (now_ms - ftl->flush.last_progress_ms) : 0U;
    const BOOL  timeout  = (ftl->flush.last_progress_ms != 0U) && (idle_ms >= FTL_FLUSH_TIMEOUT_MS);

    if (((wb_done && l2p_done && jrn_done && (confirm_pending == false) && (cache_wb_pending == false) && (cache_write_active == false)) || fail || timeout) == false)
    {
        return false;
    }

    ftl->flush.active = false;

    if (fail)
    {
        // best-effort：journal 页写/擦持续失败。CP 成功后
        // jrn_write_failure_clear 清 journal 标志。
        LOGW_RL(&s_flush_err_log, LOG_TAG_FTL "flush: journal write failure pending — complete best-effort");
    }
    else if (timeout)
    {
        const BOOL wr_pending = (ftl->cache_stream.seq.write.count != 0U) || (ftl->cache_stream.seq.write_inflight != 0U);

        // 已回执的写映射未确认时，不能让 host 把这次 FLUSH 当成成功（H14）：置 deferred_error，由下一条命令如实上报 IO_ERROR。
        if (wr_pending == true)
        {
            ftl->flags.deferred_error = true;
        }

        // dirty CMT / 未确认写页 / 其它：三选一打印，共用限流器，不刷屏。
        if (ftl->cache->cmt_dirty_owner[die] != 0U)
        {
            LOGE_RL(&s_flush_err_log, LOG_TAG_FTL "flush stalled %lums (no progress %lums) with %lu dirty CMT entr(ies) — mappings not persisted", flush_ms, idle_ms, ftl->cache->cmt_dirty_owner[die]);
        }
        else if (wr_pending == true)
        {
            LOGE_RL(&s_flush_err_log, LOG_TAG_FTL "flush stalled %lums (no progress %lums) with %hhu unconfirmed write page(s) — deferred IO_ERROR", flush_ms, idle_ms, ftl->cache_stream.seq.write.count);
        }
        else
        {
            LOGW_RL(&s_flush_err_log, LOG_TAG_FTL "flush stalled %lums (no progress %lums) (wb=%d l2p=%d jrn=%d) — best-effort complete", flush_ms, idle_ms, wb_done, l2p_done, jrn_done);
        }
    }

    ftl_flush_kick_cp(ftl);

    // 耗时日志限流 5 s（单线程 superloop；多实例共享一个时间戳即可）。host 若
    // 频繁 fsync，逐次打印会刷屏。
    LOGW_RATE((ftl->flush.start_ms != 0U) && (flush_ms >= FTL_FLUSH_SLOW_LOG_MS) && (timeout == false) && log_ratelimit_due(&s_flush_log), s_flush_log.suppressed, LOG_TAG_FTL "flush took %lums (async job)", flush_ms);

    return true;
}

/**
 * @brief 初始化 flush/trim 的日志限流对象（NOLOAD，显式复位）。
 */
void ftl_flush_trim_init(void)
{
    log_ratelimit_init(&s_flush_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_flush_err_log, LOG_RATE_SLOW_INTERVAL_MS);
}

/* ── trim ── */

/**
 * @brief 本轮没有进展时的等待记账：超过上限 ⇒ 中止 TRIM（避免 CLI/USB 永久 BUSY）。
 * 有进展（processed > 0）就清等待起点；平台时钟缺失时退化为无超时（同 FLUSH）。
 *
 * @param ftl 实例（trim_deadline 由本函数更新）
 * @param processed 本轮已处理的 LBA 数（0 = 无进展）
 * @return FTL_AGAIN 继续等待
 *         FTL_ERROR 等待超时，已中止 TRIM
 */
static ftl_status_e ftl_trim_wait(ftl_ctx_s *ftl, BYTE processed)
{
    if (processed != 0U)
    {
        timer_deadline_clear(&ftl->trim.deadline);
        jrn_watermark_update(ftl->journal);
        return FTL_AGAIN;
    }

    const DWORD now_ms = platform_now_ms();

    if (now_ms != 0U)
    {
        if (ftl->trim.deadline.armed == false)
        {
            timer_deadline_arm(&ftl->trim.deadline, now_ms, FTL_TRIM_TIMEOUT_MS);
        }
        else if (timer_deadline_expired(&ftl->trim.deadline, now_ms))
        {
            LOGE(LOG_TAG_FTL "trim: wait timeout lba=%lu\r\n", ftl->trim.lba);
            ftl->trim.active = false;
            timer_deadline_clear(&ftl->trim.deadline);
            return FTL_ERROR;
        }
    }

    return FTL_AGAIN;
}

/**
 * @brief 启动异步 TRIM job
 *
 * 每次 step 至多 FTL_TRIM_STEP_LBAS 条，避免长 TRIM 占满主循环。
 *
 * @param ftl   实例
 * @param lba   起始 LBA
 * @param count LBA 数
 * @return FTL_OK；FTL_AGAIN 已有 job；FTL_ERROR 参数非法
 */
ftl_status_e ftl_trim_begin(ftl_ctx_s *ftl, DWORD lba, DWORD count)
{
    ASSERT(ftl != NULL);

    if (ftl->trim.active)
    {
        return FTL_AGAIN; /* 单飞行：已有 TRIM job */
    }
    if (count == 0U)
    {
        return FTL_ERROR;
    }
    if (((QWORD)lba + count) > (QWORD)FTL_LBA_MAX)
    {
        return FTL_ERROR;
    }

    ftl->trim.lba       = lba;
    ftl->trim.end       = lba + count;
    ftl->trim.start_lba = lba;
    ftl->trim.start_ms  = platform_now_ms();
    ftl->trim.active    = true;
    timer_deadline_clear(&ftl->trim.deadline);
    ftl_seq_reset(ftl);

    // 与 flush 同口径：TRIM 要写 Journal/L2P，序列必须先退出。
    ftl_cache_seq_yield(ftl);

    LOGI(LOG_TAG_TRIM "start lba=%lu n=%lu\r\n", lba, count);
    return FTL_OK;
}

/**
 * @brief 推进 TRIM 一轮
 *
 * 最多处理 FTL_TRIM_STEP_LBAS 条；未完成返回 FTL_AGAIN，完成/映射错误返回终态。
 *
 * @param ftl 实例
 * @return FTL_OK 完成；FTL_AGAIN 继续；FTL_ERROR 失败
 */
ftl_status_e ftl_trim_step(ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    if (ftl->trim.active == false)
    {
        return FTL_OK;
    }

    BYTE processed = 0U;

    while ((ftl->trim.lba < ftl->trim.end) && (processed < FTL_TRIM_STEP_LBAS))
    {
        const DWORD l = ftl->trim.lba;

        // 该 LBA 还在页打包写缓冲里（未编程）：只清缓冲槽，不碰 L2P/journal——它从未
        // 落盘，无映射变更需记。缓冲的内部不变量（清空 ⇒ 结算 owners）由 wb_handle(DROP)
        // 收口。
        // ⚠ 这里不清旧 L2P 映射：当前依赖 Controller 串行化（TRIM 时该 LBA 没有未完成
        // 写）。若将来并发 TRIM，drop 后旧映射仍可读，需另行处理。
        if (wb_has_lba(ftl->wbuf, l))
        {
            wb_trigger_s trigger;
            memset(&trigger, 0, sizeof(trigger));
            trigger.kind = WBUF_TRIGGER_DROP;
            trigger.lba  = l;
            (void)wb_handle(ftl->wbuf, &trigger, NULL);

            ftl->trim.lba++;
            processed++;
            continue;
        }

        if (l2p_is_ready(ftl->cache, l) == false)
        {
            return ftl_trim_wait(ftl, processed); /* 已起异步加载：游标不动 */
        }
        if (jrn_reserve(ftl->journal, 1U) == false)
        {
            return ftl_trim_wait(ftl, processed); /* 已起异步刷页：游标不动 */
        }

        DWORD old_pba;

        const ftl_status_e rst = l2p_read(ftl->cache, l, &old_pba);
        if (rst == FTL_AGAIN)
        {
            return ftl_trim_wait(ftl, processed);
        }
        if (rst != FTL_OK)
        {
            LOGE(LOG_TAG_FTL "trim: l2p read lba=%lu failed\r\n", l);
            ftl->trim.active = false;
            return FTL_ERROR;
        }

        // Journal 先于 L2P（同 host 写确认）；预留已保证 append 不触发页满分支。
        (void)jrn_append(ftl->journal, l, old_pba, NAND_PBA_INVALID);

        const ftl_status_e wst = l2p_write(ftl->cache, l, NAND_PBA_INVALID);

        ASSERT(wst == FTL_OK);
        (void)wst;

        // 有效计数在两步之后减：它只是 RAM 统计，不进 Journal。
        if (old_pba != NAND_PBA_INVALID)
        {
            const WORD gblock = ftl_pba_gblock(old_pba);

            // 同写确认路径：BAD / count 已清时跳过，避免断言或 `WORD` 回绕。
            ftl_dangling_check(ftl, l, old_pba, FTL_GBLOCK_INVALID);
            ftl_block_sub_valid_if_live(&ftl->alloc.blocks[gblock]);
        }

        ftl->trim.lba++;
        processed++;
    }

    if (processed != 0U)
    {
        timer_deadline_clear(&ftl->trim.deadline);
        jrn_watermark_update(ftl->journal);
    }

    if (ftl->trim.lba >= ftl->trim.end)
    {
        const DWORD n       = ftl->trim.end - ftl->trim.start_lba;
        const DWORD elapsed = (ftl->trim.start_ms != 0U) ? timer_age_ms(&ftl->trim.start_ms) : 0U;

        LOGI(LOG_TAG_TRIM "done n=%lu elapsed=%lums\r\n", n, elapsed);
        ftl->trim.active    = false;
        ftl->trim.start_lba = 0U;
        ftl->trim.start_ms  = 0U;
        timer_deadline_clear(&ftl->trim.deadline);
        return FTL_OK;
    }

    return FTL_AGAIN;
}
