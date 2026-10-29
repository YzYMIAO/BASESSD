/*
 * read_recovery.c — 读恢复（CRC 重读一次 + 慢死预测 + 块退役）
 *
 * 职责：
 *   读失败时：一次正常读；只有 CRC_ERROR 允许再读一次；其余直接失败并块退役
 *   （本器件无 read-retry table，见头文件说明）。
 *   以及 ECC 趋势/read disturb 驱动的「慢死预测」——提前搬空将坏块并退役。
 *
 * 与 migrate.c 的分工：
 *   本文件只做**判定与记录**（rr_record_ecc / rr_predictive_scan / rr_retire_block）；
 *   搬移动作由 migrate.c 的搬移 job 执行（mig_predict_begin 调本文件的扫描，
 *   MIG_ERASE 完成后调本文件的退役）。
 *
 * 块健康状态机（rr_block_health_e，单调升级）：
 *   HEALTHY → WARNING（ECC 连续上升 ≥RR_ECC_RISING_THRESHOLD）
 *           → CRITICAL（单次 ≥RR_ECC_HIGH_WATER 或趋势到顶）→ DYING（单次 ≥RR_ECC_MAX）
 *
 * 文件结构：
 *   1. 记录         rr_record_ecc / rr_record_block_ecc
 *   2. 恢复 job     rr_read_submit / rr_select_plane / rr_settle_plane /
 *                   rr_job_finish / rr_job_abort / rr_job_step（static）
 *   3. typed handle rr_init / rr_trigger / rr_status_fill / rr_handle
 *   4. 完成服务     rr_read_complete（static）/ ftl_read_recovery_callback
 *   5. 判定扫描     rr_get_health / rr_predictive_scan
 *   6. 退役         rr_retire_block / rr_retire_block_at
 *
 * ⚠ 层内接口（rr_record_ecc / rr_handle / rr_predictive_scan / rr_retire_block）
 *   形参由调用者保证非空；实例 handle 由 ftl_contract_audit 在 boot 期证明，
 *   运行期入口不再逐次复检。
 */

#include "read_recovery.h"

#include "assertion.h"
#include "bbt.h" /* bbt_mark_bad */
#include "fc.h"
#include "fc_command.h"
#include "ftl.h"              /* ftl_ctx_t（含 pending[] / cache / rr_page_scratch） */
#include "ftl_fc_interface.h" /* ftl_async_read_page / ftl_cmd_read_data */
#include "ftl_job.h"          /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_READ_RECOVERY */
#include "log.h"
#include "onfi_1_0.h"

#include <string.h>

/* ── 文件级静态数据 ── */

// 不可恢复读的限流日志：坏块上的 LBA 会被 host（文件系统元数据）反复读，
// 逐次 LOGE 会写满 UART 并拖慢校验。5 s 一行 + suppressed；已 BAD 的块不再重复标坏。
static DWORD s_rr_unrec_log_ms;
static DWORD s_rr_unrec_suppressed;

/* ── 1. 记录 / 判定 ── */

// 实现要点：连续上升计数（本次 > 上次则 +1，否则清零）；三级水位各自升级 health，
// 只升不降（恢复由 rr_retire_block 复位）。
void rr_record_ecc(rr_ecc_track_t *track, BYTE ecc_bits)
{
    // 层内接口：调用者一律传 &bst_block_t.rr（BST 常驻，恒非 NULL）

    // 趋势：与上一次读数比较（last_ecc 即上次值），连续上升计数
    if (ecc_bits > track->last_ecc)
    {
        track->rising_count++;
    }
    else
    {
        track->rising_count = 0U; /* 不上升就清空连续计数 */
    }
    track->last_ecc = ecc_bits;

    // 健康状态**单调升级**：只取 max，绝不回落。
    // ⚠ 逐条直接赋值会让 CRITICAL/DYING 块在后续几次低 ECC 连续上升时被打回
    // WARNING（例如 1→2→3→4 触发 rising 门）——那会掩盖慢死信号。
    rr_block_health_e level = track->health;

    if ((track->rising_count >= RR_ECC_RISING_THRESHOLD) && (level < RR_BLOCK_WARNING))
    {
        level = RR_BLOCK_WARNING;
    }
    if ((ecc_bits >= RR_ECC_HIGH_WATER) && (level < RR_BLOCK_CRITICAL))
    {
        level = RR_BLOCK_CRITICAL;
    }
    if ((ecc_bits >= RR_ECC_MAX) && (level < RR_BLOCK_DYING))
    {
        level = RR_BLOCK_DYING;
    }
    track->health = level;
}

void rr_record_block_ecc(alc_t *alloc, WORD gblock, BYTE ecc_bits)
{
    // 前置条件：唯一调用者 migrate.c 传实例 alloc 与 ftl_gblock() 算出的块号；
    // gblock 的界内由 BST 唯一索引入口保证，此处只保留越界断言（H31）。
    ASSERT(gblock < FTL_BLOCK_TOTAL);

    bst_block_t *blk = &alloc->blocks[gblock];

    rr_record_ecc(&blk->rr, ecc_bits);
    blk->read_count++;
}

/* ── 2. 恢复路径（异步 job 状态机）──
 *
 * 三级管线的**分步**实现：rr_job_step 的 stage 序列每步只提交一条命令就返回。
 *
 *   读一次；**只有 CRC_ERROR** 允许再读一次；UNC/NAND/META 等直接失败。
 *   本器件 `0x89` 是保留地址（无读电压重试表），邻居页辅助也无法恢复目标页
 *   ⇒ 两者都**不实现**，失败即判坏。
 *   本 plane 失败 ⇒ 各 CW 清零 + bbt_mark_bad（XOR/RAID 恢复属后续阶段，不做）。
 *
 * ⚠ 本文件的每一条命令都走 ftl_async_* + ftl_op_submit（**不同步等待**）——
 *   服务函数里再跑完成泵就是嵌套泵（见 ftl_op.h）。
 * ⚠ rr_select_plane 里的 l2p_read 不同步加载：region 未命中时
 *   它起异步加载并返回 FTL_AGAIN，rr_job_step 原地等下轮（`stall_ms` 清零，
 *   加载在飞不算提交卡死）。本文件至此**零同步等待**。
 */

/**
 * @brief 提交一条页读（目标页或邻居页）。数据在完成时由服务函数拷进 rr->page_scratch
 * ——**读**不像写那样在 prepare 阶段搬数据（见 ftl_async_read_page 的注释）。
 */
static ftl_status_e rr_read_submit(rr_handle_t *rr, DWORD pba, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd;

    const ftl_status_e st = ftl_async_read_page(&cmd, pba, cfg);
    if (st != FTL_OK)
    {
        return st;
    }

    rr->waiting = true;

    if (ftl_op_submit(cmd, FTL_OP_READ_RECOVERY, rr) != FTL_OK)
    {
        rr->waiting = false;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    // ⚠ 卡死计数在**提交成功之后**才清零（不是准备成功时）：`ftl_op_submit` 自身失败
    // （完成表满 / SQ 满）也是一轮"提交不出去"，若在此前清零则计数永远停在 1、
    // submit deadline 永不触发。
    rr->stall_ms = 0U;

    return FTL_OK;
}

/**
 * @brief 为当前 plane 找恢复目标页——该 plane 第一个被本子操作覆盖的 LBA 的映射。
 *
 * @param rr 实例。
 *
 * @return RR_PLANE_READY 找到（page / page_lba / first 已填）；
 *                        RR_PLANE_SKIP 本子操作不覆盖本 plane（或该 LBA 未映射）⇒ 调用者跳过本 plane；
 *                        RR_PLANE_WAIT 该 LBA 的 region **尚未就绪**（本函数已起异步加载）⇒ 调用者原地等待、下轮重来。
 *
 * @note 实现要点：与原 ftl_recover_read_subop 的选页循环逐条一致——只看**第一个** plane
 * 匹配的 k（命中即 break，无论映射是否有效）。目标页取自 L2P 而非失败
 * 命令的 desc 链：读失败不改 L2P，二者同一。
 * ⚠ 三态：必须把"还不知道"（RR_PLANE_WAIT）与"未映射"（RR_PLANE_SKIP）分清——把前者当后者会让
 * RR 直接判"本 plane 无需恢复"，host 读到的是零填而非真数据。函数可重入（每轮从
 * 头扫，先清 page/page_lba/first）。
 */
static rr_plane_step_e rr_select_plane(rr_handle_t *rr)
{
    rr->page     = NAND_PBA_INVALID;
    rr->page_lba = 0U;
    rr->first    = 0U;

    for (BYTE k = 0U; k < rr->owner.n_cw; k++)
    {
        const DWORD l = (rr->owner.lba + k);

        if (ftl_lba_plane(l) == rr->plane)
        {
            DWORD cur;

            if (l2p_read(rr->cache, l, &cur) == FTL_AGAIN)
            {
                return RR_PLANE_WAIT; /* region 在加载：下轮再来 */
            }

            if (cur != NAND_PBA_INVALID)
            {
                rr->page     = pba_page(cur);
                rr->page_lba = ftl_lba_page(l);
                rr->first    = k;
            }
            break;
        }
    }

    return (rr->page != NAND_PBA_INVALID) ? RR_PLANE_READY : RR_PLANE_SKIP;
}

static bool rr_unrec_log_is_due(DWORD *out_suppressed)
{
    const DWORD now_ms = ftl_job_now_ms();

    bool due = (s_rr_unrec_suppressed >= 64U);

    if ((!due) && (now_ms != 0U))
    {
        due = (s_rr_unrec_log_ms == 0U) || ((now_ms - s_rr_unrec_log_ms) >= FTL_JOB_LOG_SLOW_INTERVAL_MS);
    }

    if (!due)
    {
        s_rr_unrec_suppressed++;
        return false;
    }

    *out_suppressed       = s_rr_unrec_suppressed;
    s_rr_unrec_suppressed = 0U;
    s_rr_unrec_log_ms     = now_ms;
    return true;
}

/**
 * @brief 结算当前 plane——恢复成功按 CW 偏移拷回 caller 缓冲，失败则清零 + 标坏块。
 *
 * @param rr 实例。
 *
 * @note 实现要点：恢复读是**整页**读、从 CW0 起连续拷出（落 rr_page_scratch），而子操作
 * 可能只覆盖页内部分 CW ⇒ 必须按各 CW 的真实偏移（ftl_lba_cw）搬回，
 * 否则整段错位。
 */
static void rr_settle_plane(rr_handle_t *rr)
{
    for (BYTE k = 0U; k < rr->owner.n_cw; k++)
    {
        const DWORD l = (rr->owner.lba + k);

        if (ftl_lba_plane(l) != rr->plane)
        {
            continue;
        }
        BYTE *dst = rr->owner.upper_buf + (k * FTL_IU_BYTES);

        if (rr->recovered)
        {
            memcpy(dst, rr->page_scratch + (ftl_lba_cw(l) * FTL_IU_BYTES), FTL_IU_BYTES);
        }
        else
        {
            memset(dst, 0, FTL_IU_BYTES);
        }
    }

    if (!rr->recovered)
    {
        rr->all_ok = false; /* 任一 plane 失败 ⇒ 整个托管子操作按 IO_ERROR 结算 */
        rr->planes_failed++;

        const DWORD lba      = rr->owner.lba + rr->first;
        const DWORD fail_pba = rr->page; /* 目标页基址（cw=0） */
        const DWORD fail_paa = pba_to_paa(fail_pba);
        const WORD  gblock   = ftl_pba_gblock(fail_pba);

        bst_block_t *const blk         = &rr->alloc->blocks[gblock];
        const bool         already_bad = ftl_block_is_bad(blk);

        // 读（含 CRC 重读）失败 ⇒ 页面已损坏提升为块级不可靠。
        // XOR/RAID 恢复未实现（CONFIG_FC_RAID=0）⇒ 块退役。
        // 已 BAD 的块不再重复 bbt_mark_bad（幂等但会重复记账/日志）：
        // · 已退役块被 host 反复重读是**预期降级**（数据已丢）⇒ 每 gblock 只打一次，
        // 其余只计数；
        // · 好块首次不可纠 ⇒ 完整现场（PBA/PAA/plane/block/page/cw + 解码状态）⇒ 限流。
        char stbuf[48];

        (void)fc_cw_status_str(rr->last_status, stbuf, sizeof(stbuf));

        if (already_bad)
        {
            rr->already_bad_reads++;

            const DWORD word = (DWORD)gblock / 32U;
            const DWORD bit  = 1UL << ((DWORD)gblock % 32U);

            if ((rr->already_bad_logged[word] & bit) == 0U)
            {
                rr->already_bad_logged[word] |= bit;
                LOGW(LOG_TAG_RR "retired lba=%lu g=%hu st=%s (data lost, count=%lu)\r\n", lba, gblock, stbuf, rr->already_bad_reads);
            }
        }
        else
        {
            DWORD suppressed = 0U;

            if (rr_unrec_log_is_due(&suppressed))
            {
                LOGE(LOG_TAG_RR "unc lba=%lu pba=0x%08lx paa=0x%08lx g=%hu pl=%hhu bk=%hu pg=%hhu cw=%hhu retry=%hhu st=%s supp=%lu\r\n", lba, fail_pba, fail_paa, gblock, pba_to_plane(fail_pba), pba_to_block(fail_pba), pba_to_page(fail_pba), pba_to_cw(fail_pba), rr->retry_count, stbuf, suppressed);
            }

            // 首次不可纠：dump 与失败 PBA/LBA 相关的最近写轨迹（判二次编程/写后读坏）。
            alc_trace_dump(rr->alloc, lba, fail_pba);

            (void)bbt_mark_bad(rr->alloc, gblock);
        }
    }
    else
    {
        rr->planes_recovered++;
    }
}

/**
 * @brief 收尾：释放被托管的 pending 槽 + 让 host 读组结算。
 * owners 必须在释放**之前**取走——组完成通知可能重入并复用本槽。
 */
static void rr_job_finish(rr_handle_t *rr)
{
    const ssd_status_e status = (rr->all_ok) ? SSD_STATUS_OK : SSD_STATUS_IO_ERROR;

    // all_ok 是**所有被覆盖 plane 的聚合**：任一 plane 不可恢复或提交侧 abort
    // 都按 IO_ERROR 结算；全部恢复成功才报 OK（数据已由 rr_settle_plane 拷回）。
    // owners 已保存在 rr->owner（槽释放不影响本副本）；先释放槽再结算组。
    // 不新增逐次 done 行：读失败现场由 [RR  ] unc（限流）承担，恢复过程由
    // [RR  ] job 心跳（卡住时才出现）承担。每次恢复都打 done 在坏介质下会刷屏。

    pend_release_index(rr->pending, rr->owner.pending_idx);
    pend_group_complete_subop_multi(rr->pending, rr->owner.owners, status);

    rr->has_owner = false;
    rr->stage     = RR_STAGE_IDLE;
    rr->waiting   = false;
}

/**
 * @brief 卡死兜底：提交侧长期无进展（命令池泄漏），按恢复失败收尾。
 * ⚠ **不**标坏块——卡死是控制器侧资源问题，与介质无关，标坏会白丢一个好块。
 */
static void rr_job_abort(rr_handle_t *rr)
{
    LOGE(LOG_TAG_RR "submit stalled, abort lba=%lu\r\n", rr->owner.lba);

    for (BYTE k = 0U; k < rr->owner.n_cw; k++)
    {
        memset(rr->owner.upper_buf + (k * FTL_IU_BYTES), 0, FTL_IU_BYTES);
    }

    rr->all_ok = false;
    rr->error  = FTL_TIMEOUT;
    rr->stage  = RR_STAGE_FINISH;
}

/**
 * @brief 推进状态机——跑纯计算 stage 直到提交出一条命令、或整个 job 收尾。
 *
 * @param rr 实例。
 *
 * @note 实现要点：**只在没有命令在飞时调用**（waiting == 0）。提交 stage 提交成功即
 * return（stage 原地不动，等完成服务函数改写）；提交失败（背压）同样
 * return，stage 不变 ⇒ 下轮 rr_handle 再试同一 stage（幂等）。
 */
static void rr_job_step(rr_handle_t *rr)
{
    for (;;)
    {
        switch (rr->stage)
        {
            case RR_STAGE_IDLE:
            {
                // 优先用 rr_trigger 登记的 owner；否则向 pending owner API 领取一个
                // 被托管的读子操作。都没有就空闲退出。
                if (!rr->has_owner)
                {
                    pend_op_view_t view;

                    if (!pend_view_op(rr->pending, PENDING_OP_READ_RECOVERY, &view))
                    {
                        return;
                    }

                    rr->owner.owners      = view.owners;
                    rr->owner.pending_idx = view.index;
                    rr->owner.upper_buf   = (BYTE *)view.buffer;
                    rr->owner.lba         = view.lba;
                    rr->owner.n_cw        = view.n_cw;
                    rr->has_owner         = true;
                }

                rr->plane            = 0U;
                rr->recovered        = false;
                rr->planes_recovered = 0U;
                rr->planes_failed    = 0U;
                rr->error            = FTL_OK;
                rr->all_ok           = true; /* 聚合结果：任一 plane 失败才清零 */
                rr->stall_ms         = 0U;   /* 新 job：卡死计数从头算（不复用上一 job 的） */
                rr->start_ms         = ftl_job_now_ms();
                rr->log_ms           = rr->start_ms;
                rr->stage            = RR_STAGE_PLANE;
                break;
            }

            case RR_STAGE_PLANE:
            {
                rr_plane_step_e sel;

                if (rr->plane >= PLANE_NUM)
                {
                    rr->stage = RR_STAGE_FINISH;
                    break;
                }

                sel = rr_select_plane(rr);

                if (sel == RR_PLANE_WAIT)
                {
                    // L2P region 在加载：原地等，**不计**提交卡死——
                    // 加载在飞是有确定完成时刻的进展。rr_handle 每轮会 +1，这里清零。
                    rr->stall_ms = 0U;
                    return;
                }
                if (sel == RR_PLANE_SKIP)
                {
                    rr->plane++; /* 本子操作不覆盖本 plane ⇒ 跳过 */
                    break;
                }

                // 目标页所在块已是 BAD：重试/邻居只能白读 NAND。直接按失败结算
                // ——数据已不可恢复，早失败早返回（否则每次 host 读要白跑 8 次重试，
                // 校验速度被拖到写速度以下）。
                const WORD gblock = ftl_pba_gblock(rr->page);

                if (ftl_block_is_bad(&rr->alloc->blocks[gblock]))
                {
                    rr->stage = RR_STAGE_PLANE_END;
                    break;
                }

                rr->attempt     = 1U;
                rr->recovered   = false;
                rr->retry_count = 0U;
                rr->stage       = RR_STAGE_READ;
                break;
            }

            case RR_STAGE_READ:
            {
                const ftl_page_cfg_t *cfg = FTL_PAGE_CFG_DATA(rr->page_lba);
                const ftl_status_e    st  = rr_read_submit(rr, rr->page, cfg);

                // FTL_ERROR = 地址/建链硬失败 ⇒ 立即 abort；FTL_AGAIN = 背压 ⇒
                // stage 不变，下轮 rr_handle 重试。
                if (st == FTL_ERROR)
                {
                    rr_job_abort(rr);
                }
                return;
            }

            case RR_STAGE_PLANE_END:
                rr_settle_plane(rr);
                rr->plane++;
                rr->stage = RR_STAGE_PLANE;
                break;

            case RR_STAGE_FINISH:
                rr_job_finish(rr);
                break; /* 回到 IDLE，继续看还有没有下一个托管项 */

            default:
                ASSERT(0); /* 状态机无此态 */
                return;
        }
    }
}

/* ── typed handle（端口装配 / 触发 / 上报） ── */

void rr_init(rr_handle_t *rr, l2p_cache_t *cache, alc_t *alloc, BYTE *page_scratch, pend_table_t *pending)
{
    // 前置条件：ftl_init 调本函数前已由 contract_check_handles 证明各容器非空
    // （cache / alloc / rr_page_scratch / pending 均为实例内嵌对象的地址）⇒ 不重复检查。
    memset(rr, 0, sizeof(*rr));
    rr->cache        = cache;
    rr->alloc        = alloc;
    rr->topo         = alc_topo(alloc);
    rr->page_scratch = page_scratch;
    rr->pending      = pending;
    rr->error        = FTL_OK;
}

void rr_trigger(rr_handle_t *rr, const rr_owner_t *owner)
{
    // 单飞行：busy 时不覆盖当前 owner；被托管的 pending 项仍在表里，
    // 当前 job 结束后由 pending 端口的 take 领走（行为与旧扫描一致）。
    if ((rr->stage == RR_STAGE_IDLE) && (!rr->has_owner))
    {
        rr->owner     = *owner;
        rr->has_owner = true;
    }
}

static void rr_status_fill(const rr_handle_t *rr, rr_status_t *out)
{
    out->base.critical  = true; /* host 读在等结果，不推进即死等 */
    out->base.wants_run = (rr->stage != RR_STAGE_IDLE) || (rr->has_owner);
    out->base.progress  = rr->retry_count;
    out->base.next_ms   = 0U;
    out->base.error     = rr->error;

    if (rr->waiting)
    {
        out->base.state = FTL_MODULE_WAITING;
    }
    else if (rr->stage != RR_STAGE_IDLE)
    {
        out->base.state = (rr->error != FTL_OK) ? FTL_MODULE_FAILED : FTL_MODULE_RUNNING;
    }
    else if (rr->has_owner)
    {
        out->base.state = FTL_MODULE_RUNNING;
    }
    else
    {
        out->base.state = FTL_MODULE_IDLE;
    }

    out->stage            = rr->stage;
    out->waiting          = rr->waiting;
    out->all_ok           = rr->all_ok;
    out->retries          = rr->retry_count;
    out->planes_recovered = rr->planes_recovered;
    out->planes_failed    = rr->planes_failed;
}

void rr_handle(rr_handle_t *rr, const bg_info_t *info, rr_status_t *out)
{
    // info 当前不参与恢复决策；保留形参以统一 typed 入口。
    (void)info;

    // 心跳（见 ftl_job.h）：恢复在飞期间每满 1s 打一行 stage/age。
    // ⚠ 判「在飞」用 stage 而非 waiting——提交侧背压时 stage 不变但无命令在飞，
    // 那段同样属于「job 卡住」，正是心跳要暴露的。
    if (rr->stage == RR_STAGE_IDLE)
    {
        rr->log_ms   = 0U;
        rr->start_ms = 0U;
    }
    else if (ftl_job_heartbeat(&rr->log_ms, &rr->start_ms))
    {
        LOGI(LOG_TAG_RR "job stage=%d plane=%hhu attempt=%hhu retry=%hhu stall=%lums age=%lums\r\n", rr->stage, rr->plane, rr->attempt, rr->retry_count, rr->stall_ms, ftl_job_age_ms(&rr->start_ms));
    }

    if (!rr->waiting)
    {
        if (rr->stage == RR_STAGE_IDLE)
        {
            rr_job_step(rr); /* 无 job：看有没有新托管的子操作 */
        }
        else
        {
            // 有 job 却没在飞 = 上轮提交失败。连续无进展超过 submit deadline ⇒ 判卡死收尾。
            const DWORD now_ms = ftl_job_now_ms();

            if (now_ms != 0U)
            {
                if (rr->stall_ms == 0U)
                {
                    rr->stall_ms = now_ms;
                }
                else if ((now_ms - rr->stall_ms) >= FTL_SUBMIT_TIMEOUT_MS)
                {
                    rr->error = FTL_TIMEOUT;
                    rr_job_abort(rr);
                }
            }

            rr_job_step(rr);
        }
    }

    rr_status_fill(rr, out);
}

/* ── RR 各 stage 的完成处理（ftl_read_recovery_callback 按 rr->stage 分派）──
 *
 * 共同约定：命令的 release 与 rr_job_step 续跑**不**在这里做（见 dispatcher）——
 * 但 READ 分支必须在此把数据拷进 rr_page_scratch，因为 release 之后 desc 链失效。
 *
 * 读重试策略（SLC、本器件无 read-retry table）：
 *   · 无错误 ⇒ 成功；
 *   · **只有 CRC_ERROR** ⇒ 允许**一次**重读（总线/瞬态；CRC 能区分出来）；
 *   · UNC / NAND_FAIL / TIMEOUT / MATA_ERROR ⇒ 不重试，直接按失败结算。
 *     `0x89` 读电压重试在本器件是保留地址（空操作），邻居页辅助也无法恢复目标页，
 *     二者都**不实现**，失败即判坏。
 */
static void rr_read_complete(rr_handle_t *rr, fc_command_t *cmd, const fc_command_status_t *completion)
{
    rr->retry_count = rr->attempt;
    rr->last_status = completion->status; /* 收尾日志解码用 */

    if (!fc_cw_status_has_error(completion->status))
    {
        (void)ftl_cmd_read_data(cmd, rr->page_scratch, FTL_READ_USER_SIZE);
        rr->recovered = true;
        rr->stage     = RR_STAGE_PLANE_END;
        return;
    }

    const DWORD err = completion->status & FC_CW_STATUS_ERROR_MASK;

    if (((err & ~FC_CW_STATUS_CRC_ERROR) == 0U) && (rr->attempt < 2U))
    {
        rr->attempt++;
        rr->stage = RR_STAGE_READ; /* CRC 错：再读一次 */
    }
    else
    {
        rr->stage = RR_STAGE_PLANE_END; /* 数据不可恢复：交给结算（清零 + 标坏） */
    }
}

// 实现要点：按 stage 分派到 rr_complete_* 判完成结果并改 stage，然后 release
// 命令 → rr_job_step 续跑。
// ⚠ 成功判据：FC_CW_STATUS_ERROR_MASK 全集（同 ftl_sync_read_page）。
// ⚠ 本函数**不得**同步等待（见 ftl_op.h）。
void ftl_read_recovery_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    // 形参由完成派发签名固定；release 下 ASSERT 裁空 ⇒ 显式丢弃（H30）。
    (void)ftl;

    WORD          cmd_id = completion->command_id;
    fc_command_t *cmd    = fc_command_id_to_command(cmd_id);

    // ftl 恒非空（路由表早于逐实例 boot 设好，见 ftl_op.h）；NULL = 编程错误 ⇒
    // ASSERT 而非 if-guard：release 下解引用留现场，比静默吞完成可定位。

    rr_handle_t *rr = (rr_handle_t *)ctx;
    ASSERT(rr == &ftl->read_recovery_handle);

    // 旁表项缺失（ctx 为 NULL）或本 job 已不在飞 ⇒ 孤儿/迟到的完成，只 release。
    // 正常路径不该出现：每条 RR 命令提交时都登记了 &ftl->read_recovery_handle，
    // 且只被 take 一次。
    if ((rr == NULL) || (!rr->waiting))
    {
        (void)fc_command_release(cmd);
        return;
    }

    rr->waiting = false;

    switch (rr->stage)
    {
        case RR_STAGE_READ:
            rr_read_complete(rr, cmd, completion);
            break;

        default:
            // 纯计算 stage 不该有命令在飞；到此即状态机与簿记不一致。
            ASSERT(0);
            break;
    }

    (void)fc_command_release(cmd);

    rr_job_step(rr);
}
/* ── 3. 判定（块数组 / 全表扫描）── */

// rr_get_health — 取块健康状态（track 为 NULL 返回 HEALTHY）
rr_block_health_e rr_get_health(const rr_ecc_track_t *track)
{
    if (track == NULL)
    {
        return RR_BLOCK_HEALTHY;
    }

    return track->health;
}

// 实现要点：只针对 CLOSED 块（Open 还在写、Free 无数据、BAD/RETIRED 已出局）；
// 命中条件 = health ≥ CRITICAL **或** read_count ≥ RR_READ_DISTURB_THRESHOLD
// （慢死第一信号）。调用者 mig_predict_begin 用 sb_id 反查 Closed 条目。
bool rr_predictive_scan(const alc_t *alloc, WORD *gblock)
{
    // 层内接口：唯一调用者 mig_predict_begin 传 rr->alloc 与局部 gblock

    for (WORD i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const bst_block_t *b = &alloc->blocks[i];

        // 慢死预测只针对 Closed 块：Open 还在写、Free 没数据、Retired/Bad 已出局
        if (ftl_block_state(b) != FTL_BLOCK_STATE_CLOSED)
        {
            continue;
        }

        // ECC 趋势（连续上升 ≥RISING / 单次 ≥HIGH_WATER）或 read disturb
        // 高水位（read_count 超阈值，慢死第一信号）都触发搬移。
        if ((b->rr.health >= RR_BLOCK_CRITICAL) || (b->read_count >= RR_READ_DISTURB_THRESHOLD))
        {
            // 候选 SB 的另一个 plane 若已 BAD，migrate 读它必失败并 abort，
            // 而巡检每轮仍返回同一块 ⇒ 预测路径永久卡死。跳过该 SB。
            const WORD sb_id = ftl_block_sb_id(b);

            if (sb_id >= alloc->closed_sb_count)
            {
                continue; /* 状态与闭链表不一致（不应发生）：不拿它冒险 */
            }

            const super_block_t *sb       = alc_super_closed_get_at(alloc, sb_id);
            bool                 bad_peer = false;

            for (BYTE p = 0U; p < PLANE_NUM; p++)
            {
                if (!super_block_has_plane(sb, p))
                {
                    continue;
                }
                const WORD peer_gblock = ftl_gblock(p, sb->plane_blocks[p]);

                if (ftl_block_is_bad(&alloc->blocks[peer_gblock]))
                {
                    bad_peer = true;
                    break;
                }
            }
            if (bad_peer)
            {
                continue;
            }

            *gblock = i;
            return true;
        }
    }

    return false;
}

/* ── 4. 退役 ── */

// 实现要点：read_count 归零 = read disturb 重计（块仍活着，只是不再写）。
// 计数随下一次 Checkpoint 的磨损快照持久化（不再有独立的 dirty 标记）。
void rr_retire_block(bst_block_t *b)
{
    // 层内接口：唯一调用者 ftl_migrate_callback（MIG_ERASE 完成分支）传 rr->alloc.blocks[gblock]

    // 退役：不再分配新数据（RETIRED 态），不标坏（BBT 不删、仍可读）。
    // ⚠ **调用者负责先把该块驱逐出 Free Pool**（本函数只拿 bst_block_t*，拿不到 gblock）——
    // 只置 RETIRED 挡不住分配：alc_free_find_block 的判据是
    // Free Pool **数组**（只跳过 is_bad），留在池里的退役块照样会被选中。
    // 由 migrate.c 的 MIG_ERASE RETIRE 分支调 alc_free_remove 完成。
    ftl_block_set_state(b, FTL_BLOCK_STATE_RETIRED);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    // 健康复位：块仍活着，ECC 趋势重计（read_count 归零，read disturb 重计）
    b->rr.health       = RR_BLOCK_HEALTHY;
    b->rr.rising_count = 0U;
    b->rr.last_ecc     = 0U;
    b->read_count      = 0U;
}

void rr_retire_block_at(alc_t *alloc, WORD gblock)
{
    // 前置条件：唯一调用者 migrate.c 传实例 alloc 与 ftl_gblock() 算出的块号。
    ASSERT(gblock < FTL_BLOCK_TOTAL);

    rr_retire_block(&alloc->blocks[gblock]);
}
