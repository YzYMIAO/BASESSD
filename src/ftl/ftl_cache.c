/*
 * ftl_cache.c — cache program 写序列 / 已编程页表 / padding END
 *
 * 职责：
 *   · 早回执：cache 成员完成即结算组（页在完成时进「已编程页表」）；
 *   · 页表覆盖层：lookup / failed 拦截 / 逐页确认（Journal→L2P→BST）；
 *   · 序列收尾：abort / release / drop / padding END；
 *   · verdict：FAIL_CACHE 按提交序前一页落结论。
 *
 * 关键机制：
 *   · 页表按提交序确认；FAILC 结论按 LBA 绑定（页未入表先暂存，入表时消费）；
 *   · 序列代号（gen）区分中止后的迟到完成；
 *   · 失败页撤销映射时 Journal 先于 L2P，并过 dangling 守卫。
 *
 * 与其他模块：
 *   · 上游：host_io 完成入口（member_finish）、cache_batch（序列状态）、
 *     ftl_service（confirm_step）；
 *   · 下游：pending（子操作结算）、journal / l2p_cache / allocator（撤销与有效计数）、
 *     fc_interface（pad 命令）。
 */

#include "ftl_cache.h"

#include "assertion.h"
#include "fc_interface.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"
#include "log.h"
#include "migrate.h"
#include "pending.h"
#include "ssd_compiler.h"
#include "timer.h"
#if defined(FC_NAND_TEST_ENABLE)
#    include "fault_inject.h" /* 测试镜像专属注入点；生产构建整段裁掉（宏隔离） */
#endif
#include <string.h>

/* 文件级变量 */
static log_ratelimit_s s_cache_wb_fail_log; /* cache program 批次失败（5 s） */

// padding 页镜像：整页零。页不被任何 L2P 引用（META = UNKNOWN），只用于给
// 已经提前回执的最后一页提供 FAIL_CACHE 结算点。ftl.o 的 .bss 随整文件规则落
// DTCM（NOLOAD）⇒ 启动不清零，由 ftl_global_init 显式 memset；只读使用。
// `.bss` 归属 ftl_cache_write.o，linker 的 KEEP 按对象名匹配。
static BYTE s_cache_pad_page[FTL_LBA_PER_PAIR * FTL_IU_BYTES];

/* ── write ── */

/**
 * @brief 取走「下一条命令结论的落点」页，并清游标。
 *
 * @param ftl 实例。
 *
 * @return 待证页；NULL = 本命令没有上一页（新序列首条 / 上一页是 END 页）。
 */
static void ftl_cache_write_verdict_clear(ftl_ctx_s *ftl)
{
    ftl_cache_write_s *write = &ftl->cache_stream.seq.write;

    for (BYTE i = 0U; i < FTL_CACHE_SEQ_RING; i++)
    {
        write->verdict[i].valid = false;
    }
    write->verdict_next = 0U;
}

/**
 * @brief 该页所属的 hot SB 若仍打开则关闭它。
 *
 * @param ftl  实例。
 * @param pba0 plane0 页 PBA。
 * @param pba1 plane1 页 PBA。
 *
 * @note 先查再关：失败页可能属于一个早已关闭/换掉的块，无条件 close 会误关无关块。
 */
static void ftl_cache_write_close_sb_if_open(ftl_ctx_s *ftl, DWORD pba0, DWORD pba1)
{
    const BOOL in_sb = ((alc_super_open_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba0) != ALC_SUPER_PBA_NOT_OPEN) ||
                        (alc_super_open_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba1) != ALC_SUPER_PBA_NOT_OPEN));

    if (in_sb == true)
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }
}

static void ftl_cache_write_verdict_store(ftl_ctx_s *ftl, DWORD lba, BOOL ok)
{
    ftl_cache_write_s   *write = &ftl->cache_stream.seq.write;
    ftl_cache_verdict_s *slot  = NULL;

    for (BYTE i = 0U; i < FTL_CACHE_SEQ_RING; i++)
    {
        if ((write->verdict[i].valid == true) && (write->verdict[i].lba == lba))
        {
            slot = &write->verdict[i];
            break;
        }
    }

    if (slot == NULL)
    {
        for (BYTE i = 0U; i < FTL_CACHE_SEQ_RING; i++)
        {
            const BYTE idx = (BYTE)((write->verdict_next + i) % FTL_CACHE_SEQ_RING);

            if (write->verdict[idx].valid == false)
            {
                slot                = &write->verdict[idx];
                write->verdict_next = (BYTE)((idx + 1U) % FTL_CACHE_SEQ_RING);
                break;
            }
        }
    }

    if (slot == NULL)
    {
        /* 表满不应发生（在飞页 ≤ 环深）：覆盖轮转位。 */
        slot                = &write->verdict[write->verdict_next];
        write->verdict_next = (BYTE)((write->verdict_next + 1U) % FTL_CACHE_SEQ_RING);
    }

    slot->lba   = lba;
    slot->ok    = ok;
    slot->valid = true;
}

static BOOL ftl_cache_write_verdict_take(ftl_ctx_s *ftl, DWORD lba, BOOL *out_ok)
{
    ftl_cache_write_s *write = &ftl->cache_stream.seq.write;

    for (BYTE i = 0U; i < FTL_CACHE_SEQ_RING; i++)
    {
        if ((write->verdict[i].valid == true) && (write->verdict[i].lba == lba))
        {
            write->verdict[i].valid = false;
            *out_ok                 = write->verdict[i].ok;
            return true;
        }
    }

    return false;
}

/**
 * @brief 弹出队头（已证明且已确认 / 已撤销）。
 *
 * @param ftl 实例。
 *
 * @note 只动 head/count，不搬元素——页表里的绝对下标（含结论游标）因此不会因
 *       弹出而漂移。
 */
static void ftl_cache_write_ring_pop(ftl_ctx_s *ftl)
{
    ftl_cache_write_s *write = &ftl->cache_stream.seq.write;

    if (write->count == 0U)
    {
        return;
    }

    write->head = (BYTE)((write->head + 1U) % FTL_CACHE_SEQ_RING);
    write->count--;
}

/**
 * @brief 页表里是否还有「等不到结论」的页（未证明且未判失败）。
 *
 * @param ftl 实例。
 *
 * @return true = 存在未证明页（序列释放时必须 abort）。
 */
static BOOL ftl_cache_write_has_unproven(const ftl_ctx_s *ftl)
{
    const ftl_cache_write_s *write = &ftl->cache_stream.seq.write;

    for (BYTE k = 0U; k < write->count; k++)
    {
        const ftl_cache_page_s *pg = &write->page[(BYTE)((write->head + k) % FTL_CACHE_SEQ_RING)];

        if ((pg->proven == false) && (pg->state != FTL_CACHE_PAGE_FAILED))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 在已编程页表里查**覆盖该 LBA 的最新**一页。
 *
 * @param ftl 实例。
 * @param lba 全局 host LBA。
 *
 * @return 命中的页；NULL = 页表里没有覆盖该 LBA 的页。
 *
 * @note 读覆盖层与失败拦截共用这一条扫描规则（C2）：从**最新**往回查，第一个覆盖
 *       该 LBA 的环项就定论 —— 后写必须盖住先写，绝不回退到更早的项（否则会把
 *       旧数据复活）。
 */
static const ftl_cache_page_s *ftl_cache_write_find(const ftl_ctx_s *ftl, DWORD lba)
{
    const ftl_cache_write_s *write = &ftl->cache_stream.seq.write;
    const DWORD              flat  = ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba));

    // 同一条扫描规则（读到它的两处：读覆盖层 / 失败拦截）：从**最新**往回查，
    // 第一个覆盖该 LBA 的环项就定论（后写必须盖住先写，绝不回退到更早的项）。
    for (BYTE k = write->count; k > 0U; k--)
    {
        const ftl_cache_page_s *pg = &write->page[(BYTE)((write->head + k - 1U) % FTL_CACHE_SEQ_RING)];

        if ((lba < pg->lba) || (lba >= (pg->lba + FTL_LBA_PER_PAIR)))
        {
            continue;
        }
        if (bit_mask_any(pg->cw_bitmap, flat) == false)
        {
            continue;
        }

        return pg;
    }

    return NULL;
}

/**
 * @brief 把一页标记为失败：关 hot SB、region 立即降级、置粘性 deferred error。
 *
 * @param ftl 实例。
 * @param pg  失败页。
 * @param why 失败来源（日志用：FAIL_CACHE / abort / pad …）。
 *
 * @note 撤销（L2P 作废）交给 confirm_step：它可能要等 Journal 容量，不能在这里同步做。
 *       region 降级**立即**置位，让撤销完成前的读也报 IO_ERROR。
 *       `cache_wb_aborts` 按**命令级中止**计一次（member_done 的错误分支），这里不计，
 *       否则一条 FAILC 会同时经「结论落点失败」与「本命令中止」记两笔。
 */
static void ftl_cache_write_mark_failed(ftl_ctx_s *ftl, ftl_cache_page_s *pg, const char *why)
{
    pg->state  = FTL_CACHE_PAGE_FAILED;
    pg->proven = false;

    ftl_cache_write_close_sb_if_open(ftl, pg->pba0, pg->pba1);
    ftl->flags.deferred_error = true;

    // region 降级**立即**置位（不等撤销）：FAILC 之后该页的 L2P 映射还在，读会指到
    // 编程失败的页；降级位让「撤销完成前」的读也报 IO_ERROR，而不是返回可疑页内容。
    l2p_note_region_lost(ftl->cache, lba_to_global_region(pg->lba), pg->lba, pg->cw_bitmap);

    LOGE_RL(&s_cache_wb_fail_log, LOG_TAG_FTL "cache-write failed lba=%lu (%s) — close hot SB", pg->lba, why);
}

/**
 * @brief 把一页登记进「已编程页表」，并把结论游标指向它。
 *
 * @param ftl 实例。
 * @param pg  页描述（pba / bitmap / 是否已由自身 STATUS 证明）。
 *
 * @return true = 入表；false = 环满（结构性错误，调用者断言）。
 */
static BOOL ftl_cache_write_ring_push(ftl_ctx_s *ftl, const ftl_cache_page_s *pg)
{
    ftl_cache_write_s *write = &ftl->cache_stream.seq.write;

    if (write->count >= FTL_CACHE_SEQ_RING)
    {
        return false;
    }

    const BYTE idx = (BYTE)((write->head + write->count) % FTL_CACHE_SEQ_RING);

    write->page[idx] = *pg;
    write->count++;

    // 结论按 LBA 绑定（见 ftl.h）：本页若已有暂存结论，立即消费。
    BOOL verdict_ok = false;

    if (ftl_cache_write_verdict_take(ftl, pg->lba, &verdict_ok) == true)
    {
        if (verdict_ok == true)
        {
            write->page[idx].proven = true;
        }
        else
        {
            ftl_cache_write_mark_failed(ftl, &write->page[idx], "FAIL_CACHE (deferred)");
        }
    }
    return true;
}

/* 按 LBA 落 FAIL_CACHE 结论；页未入表则暂存。返回 true = 失败结论。 */
static BOOL ftl_cache_write_verdict_apply(ftl_ctx_s *ftl, DWORD lba, BOOL ok)
{
    ftl_cache_write_s *write = &ftl->cache_stream.seq.write;

    for (BYTE k = write->count; k > 0U; k--)
    {
        ftl_cache_page_s *pg = &write->page[(BYTE)((write->head + k - 1U) % FTL_CACHE_SEQ_RING)];

        if ((lba < pg->lba) || (lba >= (pg->lba + FTL_LBA_PER_PAIR)))
        {
            continue;
        }
        if (bit_mask_any(pg->cw_bitmap, ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba))) == false)
        {
            continue;
        }

        if ((pg->proven == false) && (pg->state != FTL_CACHE_PAGE_FAILED))
        {
            if (ok == true)
            {
                pg->proven = true;
            }
            else
            {
                ftl_cache_write_mark_failed(ftl, pg, "FAIL_CACHE");
            }
        }
        return (ok == false);
    }

    ftl_cache_write_verdict_store(ftl, lba, ok);
    return (ok == false);
}

/**
 * @brief 撤销一页失败写：把该页覆盖的 LBA 从 L2P 里作废（Journal 先行，与 TRIM 同源）。
 *
 * @param ftl 实例。
 * @param pg  已判失败的页。
 *
 * @return FTL_OK    已作废
 *         FTL_AGAIN region 加载 / Journal 容量未就绪（下轮重试）
 *         FTL_ERROR L2P 读失败（LBA 越界 ⇒ 编程错误，按丢弃处理）
 *
 * @note region 的 degraded 标记在判失败时就置好了（mark_failed），这里只作废映射。
 */
static ftl_status_e ftl_cache_write_invalidate_page(ftl_ctx_s *ftl, const ftl_cache_page_s *pg)
{
    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        const DWORD l = (pg->lba + bit);

        // flat 位图：bit = plane*CW_NUM + cw（页对覆盖 2 plane × 4 CW）。走 helper 而不是
        // 手写 `1U << bit`——同一个坐标在页表里只有这一套（C9/H15）。
        if (bit_mask_any(pg->cw_bitmap, ftl_cw_bit(ftl_lba_plane(l), ftl_lba_cw(l))) == false)
        {
            continue;
        }

        if (l2p_is_ready(ftl->cache, l) == false)
        {
            return FTL_AGAIN; /* 已起异步加载：下轮再来 */
        }
        if (jrn_reserve(ftl->journal, 1U) == false)
        {
            return FTL_AGAIN; /* 已起异步刷页 */
        }

        DWORD old = NAND_PBA_INVALID;

        const ftl_status_e rst = l2p_read(ftl->cache, l, &old);

        if (rst == FTL_AGAIN)
        {
            return FTL_AGAIN;
        }
        if (rst != FTL_OK)
        {
            return FTL_ERROR;
        }
        if (old == NAND_PBA_INVALID)
        {
            continue; /* 本来就未映射 */
        }

        (void)jrn_append(ftl->journal, l, old, NAND_PBA_INVALID);

        const ftl_status_e wst = l2p_write(ftl->cache, l, NAND_PBA_INVALID);

        ASSERT(wst == FTL_OK);
        (void)wst;

        // 解除映射也要过守卫：count 已为 0 时运行期静默夹住，但 journal 已记账
        // ⇒ 下次开机回放 `pfr_valid_remove` 判下溢、整份块元数据快照作废。
        ftl_dangling_check(ftl, l, old, FTL_GBLOCK_INVALID);
        ftl_block_sub_valid_if_live(&ftl->alloc.blocks[ftl_pba_gblock(old)]);
    }

    return FTL_OK;
}

/**
 * @brief 中止写序列：未证明页一律按 IO_ERROR 结算、关闭 hot SB、清序列状态。
 *
 * @param ftl 实例。
 *
 * @note 只处理仍在本序列页表里的页：**器件已证明**的页保留（数据有效，交给确认路径），
 *       已判失败的页不重复记账。在飞成员（尚未进页表的命令）由完成入口的序列代号
 *       守卫丢弃（失败序列其后页一律不确认）。代号在这里递增，
 *       是本函数与迟到完成的唯一分界。
 */
static void ftl_cache_write_abort(ftl_ctx_s *ftl)
{
    ftl_cache_write_s *write = &ftl->cache_stream.seq.write;

    for (BYTE k = 0U; k < write->count; k++)
    {
        ftl_cache_page_s *pg = &write->page[(BYTE)((write->head + k) % FTL_CACHE_SEQ_RING)];

        if (pg->proven == true)
        {
            continue; /* 器件已证明该页编程成功：数据有效，交给 confirm_step 正常确认并弹出
                       * （早回执下它可能还没进 L2P，但不是「失败」） */
        }
        if (pg->state == FTL_CACHE_PAGE_FAILED)
        {
            continue; /* 已判失败（如 FAILC 落点）：等 confirm_step 撤销，不重复记账 */
        }

        ftl_cache_write_mark_failed(ftl, pg, "abort");
    }

    ftl_cache_write_verdict_clear(ftl);
    ftl->cache_stream.seq.kind            = FTL_CACHE_SEQ_NONE;
    ftl->cache_stream.seq.gen             = (BYTE)(ftl->cache_stream.seq.gen + 1U); /* 迟到完成 ⇒ 代号不符 */
    ftl->cache_stream.seq.active          = false;
    ftl->cache_stream.seq.break_requested = false;
    ftl->cache_stream.seq.idle_ms         = 0U;
    ftl->cache_stream.seq.run_pairs       = 0U;
    ftl->cache_stream.seq.closing         = false;
    ftl->cache_stream.seq.close_requested = false;
    ftl->cache_stream.seq.own_page0       = false;
}

/**
 * @brief 读覆盖层：该 LBA 是否有「已编程、尚未进 L2P」的页。
 *
 * @param ftl     实例。
 * @param lba     全局 host LBA。
 * @param out_pba 命中时写入该 LBA 所在 plane 的**页基址 PBA**（调用者再按 cw 取）。
 *
 * @return true = 命中（读必须走 out_pba，不能读 L2P）。
 *
 * @note PROGRAMMED 用它；CONFIRMED（L2P 才是真值）或 FAILED（映射已作废）都返回
 *       false —— 都不回退到更早的环项。
 */
BOOL ftl_cache_write_lookup(const ftl_ctx_s *ftl, DWORD lba, DWORD *out_pba)
{
    const ftl_cache_page_s *pg = ftl_cache_write_find(ftl, lba);

    if ((pg == NULL) || (pg->state != FTL_CACHE_PAGE_PROGRAMMED))
    {
        return false; /* CONFIRMED：L2P 真值；FAILED：映射已作废 —— 都不回退 */
    }

    const BYTE  plane    = ftl_lba_plane(lba);
    const DWORD page_pba = (plane == 0U) ? pg->pba0 : pg->pba1;

    *out_pba = pba_with_cw(page_pba, ftl_lba_cw(lba));
    return true;
}

/**
 * @brief 该 LBA 是否落在**已判失败、尚未撤销**的 cache 页上。
 *
 * @param ftl 实例。
 * @param lba 全局 host LBA。
 *
 * @return true = 读该 LBA 必须报 IO_ERROR（不能读 L2P 里的可疑 PBA）。
 *
 * @note 用 ftl_cache_write_find 的同一条扫描规则（从新到旧、第一个覆盖项定论）。
 *       失败页在撤销完成前仍留在环里，而它的 L2P 映射可能还指向编程失败的页
 *       ⇒ 这一层必须在 l2p_read 之前拦下。
 */
BOOL ftl_cache_write_lba_is_failed(const ftl_ctx_s *ftl, DWORD lba)
{
    const ftl_cache_page_s *pg = ftl_cache_write_find(ftl, lba);

    return (pg != NULL) && (pg->state == FTL_CACHE_PAGE_FAILED);
}

/**
 * @brief 推进「已编程页表」：按序确认 / 撤销 / 弹出已证明的页。
 *
 * @param ftl 实例。
 *
 * @note 每轮 ftl_service 调用一次。顺序 = 提交顺序（Journal 章节也因此有序）。
 *       遇到确认/撤销的瞬态背压（region 加载、Journal 容量）就地停下，下轮继续。
 */
void ftl_cache_write_confirm_step(ftl_ctx_s *ftl)
{
    ftl_cache_write_s *write = &ftl->cache_stream.seq.write;

    while (write->count > 0U)
    {
        ftl_cache_page_s *pg = &write->page[write->head];

        if (pg->state == FTL_CACHE_PAGE_FAILED)
        {
            const ftl_status_e st = ftl_cache_write_invalidate_page(ftl, pg);

            if (st == FTL_AGAIN)
            {
                return;
            }
            // 硬错误只可能是 L2P 读的 LBA 越界（编程错误）：本页已判失败、region 已
            // 降级，丢弃该项避免永久卡住队头（flush 屏障会一直等它）。
            LOGE_IF(st != FTL_OK, LOG_TAG_FTL "cache-write undo failed lba=%lu — drop entry\r\n", pg->lba);
            ftl_cache_write_ring_pop(ftl);
            continue;
        }

        if (pg->state == FTL_CACHE_PAGE_PROGRAMMED)
        {
            ftl_page_desc_s d;

            d.lba       = pg->lba;
            d.new_pba   = pg->pba0;
            d.new_pba1  = pg->pba1;
            d.cw_bitmap = pg->cw_bitmap;
            d.seq_page  = true;

            if (ftl_confirm_write(ftl, &d, NULL) != FTL_OK)
            {
                return; /* FTL_AGAIN：背压，下轮再来 */
            }
            pg->state = FTL_CACHE_PAGE_CONFIRMED;
        }

        if (pg->proven == false)
        {
            // 未来结论只可能来自后继命令的 FAILC 或 padding END；都没有时判失败并撤销，
            // 否则页表排不空（flush 只能无进展超时）。`write_inflight` 独立于 kind：序列已被
            // 释放（kind 清空）时，在飞成员的 FAILC 仍可能证明本页。
            const ftl_cache_seq_s *seq             = &ftl->cache_stream.seq;
            const BOOL             may_get_verdict = (seq->write_inflight != 0U) ||
                                         ((seq->kind == FTL_CACHE_SEQ_WRITE) &&
                                          ((seq->active == true) || (seq->closing == true) || (seq->close_requested == true)));

            if (may_get_verdict == true)
            {
                return; /* 等后继命令的 FAIL 结论；确认已完成，读走 L2P */
            }

            ftl_cache_write_mark_failed(ftl, pg, "no verdict");
            continue; /* 下一轮按 FAILED 撤销并弹出 */
        }

        ftl_cache_write_ring_pop(ftl);
    }
}

/**
 * @brief cache program 成员的完成入口（早回执）。
 *
 * 命令 i 完成时：
 *  1. 代号不符（序列已中止 / 已被新序列取代）⇒ 迟到完成：只按 IO_ERROR 结算组，不碰页表；
 *  2. 状态带 FAIL_CACHE ⇒ **结论游标页**（= page i-1）判失败；否则该页被证明成功；
 *  3. 状态带 NAND 级错误 **或** FAIL_CACHE ⇒ 序列中止：本页不入表、按 IO_ERROR 结算，
 *     未证明页一律不确认，器件已证明的页保留；
 *  4. 否则本页已编程 ⇒ 入表 + **立即结算子操作**（组完成 ⇒ host 早回执）。
 *
 * @note 找不到 pending 的迟到完成由调用者兜底（本函数只处理仍登记在序列里的页）。
 */
void ftl_cache_write_member_finish(ftl_ctx_s *ftl, pend_req_s *pending, const fc_cmd_status_s *completion)
{
    const BYTE  idx      = (BYTE)(pending - ftl->pending.slots);
    const DWORD page_lba = pending->lba;
    const DWORD owners   = pending->owners;

    // 在飞成员数：flush 屏障与「开新序列」都等它归零（页在完成时才进页表，
    // 只看 write.count 会漏掉仍在编程的页）。所有出口都必须走到这里一次。
    ASSERT(ftl->cache_stream.seq.write_inflight > 0U);
    ftl->cache_stream.seq.write_inflight--;

#if defined(FC_NAND_TEST_ENABLE)
    // 测试镜像专属注入：在本条 cache program 命令的完成上伪造 FAIL_CACHE。
    // 器件语义 = 第 N 条命令的 70h 状态报**上一页**的结论（op44/op48 映射，op49 首页
    // 不映射）⇒ 注入到某页的完成 = 判它的**前一页**失败。
    // 生产构建整段裁掉：连 completion 拷贝都不存在（宏隔离，H14）。
    fc_cmd_status_s completion_used = *completion;

    if (fi_cache_fail_take(page_lba))
    {
        completion_used.status |= FC_CMD_STATUS_FAIL_CACHE; /* 常量落在 8 位字段内（H48 ②） */
    }
    completion = &completion_used;
#endif

    // 迟到完成：所属写序列已中止 / 已被新序列取代（代号不符）⇒ 本页不确认映射
    // （失败序列其后页一律不确认，否则 rebuild 的 page0 捷径会被击穿）。
    if (pending->cache_gen != ftl->cache_stream.seq.gen)
    {
        LOGW_RL(&s_cache_wb_fail_log, LOG_TAG_FTL "cache-write late completion lba=%lu gen=%hhu stale — drop page", page_lba, pending->cache_gen);

        pend_release_index(&ftl->pending, idx);
        pend_group_subop_finish_multi(&ftl->pending, owners, SSD_STATUS_IO_ERROR);
        return;
    }

    const DWORD status     = completion->status;
    const BOOL  is_end     = (pending->cache_role == PEND_CACHE_ROLE_END);
    const BOOL  fail_cache = ((status & FC_CMD_STATUS_FAIL_CACHE) != 0U);
    const BOOL  own_fail   = ((status & (FC_CMD_STATUS_NAND_FAIL | FC_CMD_STATUS_TIMEOUT | FC_CMD_STATUS_DATA_ERROR)) != 0U);

    // 1) 本命令的 FAILC 按**提交序前一页**的 LBA 落结论（建链时记在 pending 槽里；
    //    页未入表则暂存，入表时消费 ⇒ 完成顺序反转不影响正确性）。
    BOOL verdict_failed = false;

    if (pending->verdict_valid == true)
    {
        verdict_failed = ftl_cache_write_verdict_apply(ftl, pending->verdict_lba, (fail_cache == false));
    }

    // 2) 序列中止：① 本命令自身编程失败；② 本命令的 FAILC 判了前一页失败。
    //    为什么 FAILC 也要中止：器件级 FAIL_CACHE 表示**缓存程序管线**里上一页没编程
    //    成功，后续页还在同一条可疑管线上；依赖「任一错误 ⇒ 其后页一律
    //    不确认」（rebuild 的「page0 空白 ⇒ 整块空白」捷径），继续写下去会击穿它。
    //    本页不入表、按 IO_ERROR 结算组；控制器已提交即回执，失败由 deferred error
    //    在下一条命令上报。
    //    `verdict_valid == false` 说明本命令是第一页（first，前无本序列的页）：器件对
    //    op49 不映射 FAILC，此时该位只能是上一序列的残留 ⇒ 忽略。
    if ((own_fail == true) || (verdict_failed == true))
    {
        char stbuf[48];
        (void)fc_cmd_status_str(status, stbuf, sizeof(stbuf));
        ftl->cache_stream.wb_aborts++;

        LOGE_RL(&s_cache_wb_fail_log, LOG_TAG_FTL "cache-write cmd failed lba=%lu st=%s", page_lba, stbuf);

        ftl_cache_write_close_sb_if_open(ftl, pending->new_pba, pending->new_pba1);
        ftl->flags.deferred_error = true;

        // 顺序：先中止序列，再结算组。组完成会回调 `on_complete`，回调可以重入
        // `ftl_submit_*`（`pend_group_maybe_finish` 的既有契约）；若结算在前，
        // 回调可能已经开出一条新序列，随后的 abort 会把**新序列**一起中止。
        ftl_cache_write_abort(ftl); /* 其余页：已回执的撤销、未证明的丢弃 */

        pend_release_index(&ftl->pending, idx);
        pend_group_subop_finish_multi(&ftl->pending, owners, SSD_STATUS_IO_ERROR);
        return;
    }

    // 3) 本页已编程 ⇒ 入表 + **早回执**。
    //    page0 屏障记账（cache 路径自己打开的块）：成功编程即记，与确认解耦。
    if (pba_to_page(pending->new_pba) == 0U)
    {
        alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba);
        alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba1);
    }

    {
        ftl_cache_page_s pg;

        pg.lba       = page_lba;
        pg.pba0      = pending->new_pba;
        pg.pba1      = pending->new_pba1;
        pg.cw_bitmap = pending->cw_bitmap;
        pg.state     = FTL_CACHE_PAGE_PROGRAMMED;
        pg.proven    = is_end; /* END 的 FAIL 位就是它自己的结论，不会有后继命令 */

        const BOOL wr_ring_pushed = ftl_cache_write_ring_push(ftl, &pg);

        if (wr_ring_pushed == false)
        {
            // 环满属结构性错误：环深 16 ≫ 在飞窗口（≤4 页/窗口 + 前窗尾页）。
            // release 下不能只靠 ASSERT：按 OK 结算会让这页永不确认，
            // host 已看到成功的写静默丢失。按失败中止序列 + 本组报 IO_ERROR（host 重试）。
            LOGE_RL(&s_cache_wb_fail_log, LOG_TAG_FTL "cache-write page table full lba=%lu — abort sequence", page_lba);
            ASSERT_MSG(wr_ring_pushed, "cache-write page table full lba=%lu", page_lba);
            ftl_cache_write_abort(ftl);
            pend_release_index(&ftl->pending, idx);
            pend_group_subop_finish_multi(&ftl->pending, owners, SSD_STATUS_IO_ERROR);
            return;
        }
    }

#if SSD_STATS_PERSIST_ENABLE
    ftl_op_lifetime_note_host_io(ftl, pending, true);
#endif
    pend_release_index(&ftl->pending, idx);
    pend_group_subop_finish_multi(&ftl->pending, owners, SSD_STATUS_OK);
}

/**
 * @brief 初始化本模块的 NOLOAD 状态与日志限流。
 *
 * s_cache_pad_page 在 NOLOAD DTCM：上电/软复位都不清零，必须显式 memset；
 * s_cache_wb_fail_log 的 interval 同样要在初始化时装好。
 */
void ftl_cache_write_init(void)
{
    // padding 页内容不映射任何 LBA，但必须是确定的零页；NOLOAD 段的随机残留
    // （可能是上一次运行的主机数据）不应被写进介质。
    memset(s_cache_pad_page, 0, sizeof(s_cache_pad_page));
    log_ratelimit_init(&s_cache_wb_fail_log, LOG_RATE_SLOW_INTERVAL_MS);
}

/* ── pad ── */

/**
 * @brief 为打开中的写序列提交一条 padding END（48h）。
 *
 * @param ftl 实例
 * @return FTL_OK    已提交（seq->closing = true，门控继续关到完成）
 *         FTL_AGAIN 页对 / 命令资源不足（已登记紧急 GC，下轮重试）
 *         FTL_ERROR 建链失败
 *
 * @note 为什么需要它：设备序列没有"无数据 END"。写序列在 host 命令边界保持打开
 *       （提前回执模型），当下一段 LBA 不连续 / host 空闲 / flush 到来时，必须
 *       用一个**数据页**把最后已提交页的 FAIL_CACHE 结算出来。本仓用一个不映射的
 *       零页承担这个角色（每段最多一次，≈0.4% 额外页写）。
 */
static SSD_CODE_FAST ftl_status_e ftl_cache_pad_submit(ftl_ctx_s *ftl)
{
    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    if ((seq->active != true) || (seq->kind != FTL_CACHE_SEQ_WRITE) || (seq->closing == true))
    {
        return FTL_OK;
    }

    DWORD pba[PLANE_NUM];

    if (alc_super_open_alloc_pair(&ftl->alloc, ALC_SUPER_ALLOC_HOT, pba) != FTL_OK)
    {
        mig_gc_request_urgent(ftl->mig_handle);

        // ⚠ 无空闲页 ⇒ 收尾页也提交不了。若继续等（序列门控 + GC 需要 Journal 容量），
        // 会与 GC 互相等待（livelock）：GC 的 jrn_reserve 被序列门控拒绝，而序列又
        // 需要 GC 腾出的页才能收尾。**放弃本序列**换活性：未确认页按 IO_ERROR 结算
        // （host 已提前回执 ⇒ deferred error），门控放开后 GC 可以刷 Journal 页并回收。
        ftl_cache_write_abort(ftl);
        return FTL_ERROR;
    }

    fc_cmd_s          *cmd = NULL;
    const ftl_status_e st  = ftl_async_write_pair_cmd(&cmd, pba, s_cache_pad_page, FTL_CACHE_FULL_CW, OP_TWO_PLANE_PROGRAM_CACHE_END, FTL_PAGE_CFG_DATA_ANY());

    if (st != FTL_OK)
    {
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return st;
    }

    cmd->config.chain = FC_CHAIN_END;

    // 序列所有者通道：本命令就是该 LUN 序列的收尾，门控必须旁路。
    const BOOL owner_prev     = seq->owner_submit;
    seq->owner_submit         = true;
    const ftl_status_e sub_st = ftl_op_submit(cmd, FTL_OP_CACHE_PAD, NULL);
    seq->owner_submit         = owner_prev;

    if (sub_st != FTL_OK)
    {
        fc_command_release(cmd->cmd_id);
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return sub_st;
    }

    // 记账用**真实**两 plane 地址：Policy B 下它们可能是不同的物理块，不能用
    // pba_with_plane() 从 plane0 推导（否则 plane1 的 page0 屏障会漏记，见回调）。
    seq->pad_pba[0] = pba[0];
    seq->pad_pba[1] = pba[1];

    // pad 的 FAIL_CACHE 指向序列最后一页（提交时确定，按 LBA 绑定）。
    seq->pad_verdict_valid = (seq->write.count != 0U);
    seq->pad_verdict_lba   = (seq->next_lba >= FTL_LBA_PER_PAIR) ? (seq->next_lba - FTL_LBA_PER_PAIR) : 0U;

    seq->closing         = true;
    seq->close_requested = false;
    seq->break_requested = false;
    return FTL_OK;
}

/**
 * @brief padding 收尾页完成服务
 *
 * 按 FAILC 判定上一页成败（收尾页自身不映射数据），并补 page0 屏障记账；
 * 随后关闭序列、放开 L2P 门控。
 *
 * @param ftl        实例
 * @param completion FC 完成状态
 * @param ctx        恒 NULL（序列状态在 ftl->cache_stream.seq）
 */
void SSD_CODE_FAST ftl_cache_pad_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    // ctx 恒 NULL（见 ftl_op.h 契约）；序列状态在 ftl->cache_stream.seq。
    (void)ctx;

    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    // 本服务函数拥有命令，下面**任何**退出路径都必须释放（H1）。必须先取再判：
    // 序列已死时的早退若直接 return，会把 command / desc 链 / buffer 一起漏掉
    // （⇒ 反复触发耗尽 FC 池）。
    fc_cmd_s *cmd = fc_command_lookup(completion->cmd_id);

    // 收尾页在飞期间序列被中止（盘满 abort / 失败 abort）⇒ 这条回调属于已死的序列：
    // 不能再动 allocator 的 page0/SB 记账（那会误关新块的 SB）。
    if ((seq->kind != FTL_CACHE_SEQ_WRITE) || (seq->closing == false))
    {
        if (cmd != NULL)
        {
            fc_command_release(cmd->cmd_id);
        }
        return;
    }

    // 收尾页自身的结果（NAND_FAIL）不映射任何数据 ⇒ 只取 FAIL_CACHE 判定**上一页**；
    // 超时意味着上一页结论未知，按失败处理（保守）。
    const BOOL prev_ok = ((completion->status & (FC_CMD_STATUS_FAIL_CACHE | FC_CMD_STATUS_TIMEOUT)) == 0U);

    // padding 页落在新块 page0 时承担 page0 异步写屏障：成功必须记账，否则
    // allocator 的 `alc_super_open_room` 永远返回 0（后续窗口全被拒）。
    // 失败（含上一页 FAILC）⇒ 关 hot SB：与 cache 成员失败同口径，绝不继续写可疑块。
    if ((cmd != NULL) && (fc_cmd_status_is_success(completion->status) == true))
    {
        if (pba_to_page(seq->pad_pba[0]) == 0U)
        {
            // ⚠ 逐 plane 用 submit 时记下的**真实**地址：Policy B 下两 plane 的块可能
            // 不同，用 pba_with_plane(pad_pba[0], 1) 推导会把 plane1 的 page0 屏障
            // 漏记（实测：`p0=01` ⇒ wbuf 永久 AGAIN）。
            for (BYTE p = 0U; p < PLANE_NUM; p++)
            {
                alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, seq->pad_pba[p]);
            }
        }
    }
    else
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }

    // pad 的 FAIL_CACHE 落给序列最后一页（提交时已绑定 LBA）；超时按失败处理。
    if (seq->pad_verdict_valid == true)
    {
        (void)ftl_cache_write_verdict_apply(ftl, seq->pad_verdict_lba, prev_ok);
        seq->pad_verdict_valid = false;
    }

    if (seq->closing == true)
    {
        // 收尾后不得留下无结论页：pad 只覆盖一页，其余未证明页（含 head）一律
        // 判失败，否则页表无法排空、flush 只能以无进展超时结束。
        // ⚠ **只判未证明页**（与 ftl_cache_write_abort 同口径）：已编程成功、只是在等
        // 确认的页要留给 confirm_step，判失败会撤销有效映射。
        for (BYTE k = 0U; k < seq->write.count; k++)
        {
            ftl_cache_page_s *pg = &seq->write.page[(BYTE)((seq->write.head + k) % FTL_CACHE_SEQ_RING)];

            if ((pg->proven == false) && (pg->state != FTL_CACHE_PAGE_FAILED))
            {
                ftl_cache_write_mark_failed(ftl, pg, "pad: stray page");
            }
        }

        ftl_cache_write_verdict_clear(ftl);

        seq->kind            = FTL_CACHE_SEQ_NONE;
        seq->active          = false;
        seq->closing         = false;
        seq->close_requested = false;
        seq->break_requested = false;
        seq->own_page0       = false;
        seq->draining        = false;
        seq->idle_ms         = 0U;

        ftl->cache_stream.wb_pad_closes++;
    }

    // 本服务函数拥有命令。
    if (cmd != NULL)
    {
        fc_command_release(cmd->cmd_id);
    }
}

/* ── seq ── */

/**
 * @brief 只清 RAM：关闭 cache 序列状态（不发任何设备命令）。
 *
 * @param ftl 实例。
 *
 * @note 设备侧未收 LAST 的缓存序列由随后的新命令终止（待上板确认）。
 *       本函数与 idle 守卫不在每 I/O 热路径上，放内部 Flash，不占 ITCM。
 */
void SSD_CODE_FAST ftl_cache_seq_release(ftl_ctx_s *ftl)
{
    // 判据必须含 `write_inflight`：页在完成时才入表（早回执），组级失败可能正好落在窗口
    // 命令全在飞、页表为空时。此时干净清除（不升 gen）会让迟到完成把未证明页推进死
    // 序列；abort 升 gen 后迟到完成被丢弃、并按 IO_ERROR 结算组。
    if ((ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE) &&
        ((ftl->cache_stream.seq.write_inflight != 0U) || (ftl_cache_write_has_unproven(ftl) == true)))
    {
        ftl_cache_write_abort(ftl); /* 未证明/在飞页：按失败结算 + 升 gen + 关 SB + 清状态 */
        return;
    }

    // 序列被放弃时要分清「命令还在飞」与「已经排空」：前者必须继续关着门控，
    // 否则别的组会在 31h/3Fh 管线半途插命令；后者才真正放开。
    // 判据只数**序列自己拥有**的读子操作：读失败托管给 RR 的那一项已脱钩
    // （PENDING_OP_READ_RECOVERY），不占设备读管线。把它算成在飞会让 draining
    // 永远关着门，而 RR 的恢复读又被同一道门挡住 ⇒ 组永不完成，形成死锁。
    const WORD gi        = ftl->cache_stream.seq.owner_gi;
    const BOOL in_flight = (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_READ) && (gi < FTL_GROUP_MAX) && (ftl->pending.groups[gi].used == true) &&
                           (pend_group_has_op(&ftl->pending, gi, PENDING_OP_READ) == true);

    ftl->cache_stream.seq.kind     = FTL_CACHE_SEQ_NONE;
    ftl->cache_stream.seq.active   = false;
    ftl->cache_stream.seq.draining = in_flight;
    ftl_cache_write_verdict_clear(ftl);
    ftl->cache_stream.seq.break_requested = false;
    ftl->cache_stream.seq.idle_ms         = 0U;
    ftl->cache_stream.seq.run_pairs       = 0U;
    ftl->cache_stream.seq.closing         = false;
    ftl->cache_stream.seq.close_requested = false;
    ftl->cache_stream.seq.own_page0       = false;
}

/**
 * @brief 让出设备序列：写序列请求 padding 收尾，读序列直接放弃（不发 LAST）。
 *
 * @note 需要写 Journal/L2P 的前台控制路径（flush / trim）在入口调用：序列不退出，
 *       这些元数据提交会被门控。写序列不能 abort（提前回执页未确认），走 padding END。
 */
void SSD_CODE_FAST ftl_cache_seq_yield(ftl_ctx_s *ftl)
{
    if (ftl->cache_stream.seq.active == false)
    {
        return;
    }

    if (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE)
    {
        ftl->cache_stream.seq.close_requested = true;
    }
    else
    {
        ftl_cache_seq_release(ftl);
    }
}

/**
 * @brief 组被弃/失败时关闭它持有的 cache 序列。
 *
 * @note 不关闭会让序列门控永久挡住该 LUN 的所有提交（门控只认 owner_submit）。
 */
void SSD_CODE_FAST ftl_cache_seq_close(ftl_ctx_s *ftl, WORD gi)
{
    if ((ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.owner_gi == gi))
    {
        ftl_cache_seq_release(ftl);
    }
}

void SSD_CODE_FAST ftl_cache_seq_drop(ftl_ctx_s *ftl)
{
    if ((ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE) &&
        ((ftl->cache_stream.seq.write_inflight != 0U) || (ftl_cache_write_has_unproven(ftl) == true)))
    {
        ftl_cache_write_abort(ftl); /* 未证明/在飞页按失败结算（升 gen 丢弃迟到完成），下面再清门控 */
    }

    ftl->cache_stream.seq.kind            = FTL_CACHE_SEQ_NONE;
    ftl->cache_stream.seq.active          = false;
    ftl->cache_stream.seq.draining        = false;
    ftl->cache_stream.seq.owner_submit    = false;
    ftl->cache_stream.seq.break_requested = false;
    ftl->cache_stream.seq.close_requested = false;
    ftl->cache_stream.seq.closing         = false;
    ftl->cache_stream.seq.own_page0       = false;
    ftl->cache_stream.seq.idle_ms         = 0U;
    ftl->cache_stream.seq.yield_pending   = false;
    ftl->cache_stream.seq.yield_ms        = 0U;
    ftl_cache_write_verdict_clear(ftl);
}

/**
 * @brief 序列有序收尾入口（host 补发之后每轮调用）。
 *
 * 读序列：直接放弃（不发 LAST）；写序列：提交 padding END（不能 abort——未确认页
 * 已经对 host 提前回执）。资源不足时保持关闭请求，下轮重试。
 *
 * @param ftl 实例
 */
SSD_CODE_FAST void ftl_cache_seq_close_step(ftl_ctx_s *ftl)
{
    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    if ((seq->active != true) || (seq->owner_submit == true) || (seq->closing == true))
    {
        return;
    }
    if ((seq->close_requested == false) && (seq->break_requested == false))
    {
        return;
    }

    if (seq->kind == FTL_CACHE_SEQ_READ)
    {
        ftl_cache_seq_release(ftl);
        return;
    }

    // ⚠ 必须等所有数据成员完成并进入页表（`write_inflight == 0`）再提交 padding END：
    //   · pad 的 FAIL_CACHE 是对**器件管线里上一条命令**的结论，必须对准真正的最后一页；
    //   · 若此时仍有数据命令在飞，pad 会先收尾（active=false），迟到的成员完成再把页
    //     推进死序列——该页没有后继 FAIL_CACHE 可证明，永久卡住队头 ⇒ flush 超时。
    if (seq->write_inflight != 0U)
    {
        return;
    }

    (void)ftl_cache_pad_submit(ftl);
}

/* ── test ── */

#if defined(FC_NAND_TEST_ENABLE)
ftl_status_e ftl_cache_test_pad_submit(ftl_ctx_s *ftl)
{
    return ftl_cache_pad_submit(ftl);
}
#endif
