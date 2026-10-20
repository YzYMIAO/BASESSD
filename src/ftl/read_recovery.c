/*
 * read_recovery.c — 读恢复（多级读重试 + 慢死预测 + 块退役）
 *
 * 职责：
 *   读失败时的三级恢复（读电压重试 → 邻近页辅助 → 显式兜底），
 *   以及 ECC 趋势/read disturb 驱动的「慢死预测」——提前搬空将坏块并退役。
 *
 * 与 migrate.c 的分工：
 *   本文件只做**判定与记录**（rr_record_ecc / rr_predictive_scan / rr_retire_block）；
 *   搬移动作由 migrate.c 的搬移 job 执行（mig_begin_predict 调本文件的扫描，
 *   MIG_ERASE 完成后调本文件的退役）。
 *
 * 块健康状态机（rr_block_health_t，单调升级）：
 *   HEALTHY → WARNING（ECC 连续上升 ≥RR_ECC_RISING_THRESHOLD）
 *           → CRITICAL（单次 ≥RR_ECC_HIGH_WATER 或趋势到顶）→ DYING（单次 ≥RR_ECC_MAX）
 *
 * 文件结构：
 *   1. 记录/判定   rr_record_ecc
 *   2. 恢复路径     rr_handle / rr_step（static）/ ftl_read_recovery_callback（异步 job 状态机）
 *   3. 判定扫描     rr_check_predictive / rr_get_health / rr_predictive_scan
 *   4. 退役         rr_retire_block
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
#include "ftl.h"              /* ftl_context_t（含 pending[] / cache / rr_page_scratch） */
#include "ftl_fc_interface.h" /* ftl_async_read_page / ftl_async_set_features / ftl_cmd_read_data */
#include "ftl_job.h"          /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_READ_RECOVERY */
#include "log.h"
#include "onfi_1_0.h"

#include <string.h>

/* ── 1. 记录 / 判定 ── */

/* --------------------------------------------------------------------
 * rr_record_ecc
 *   功能：记录一次读的 ECC 纠正 bit 数，更新趋势与健康状态（单调升级）。
 *   参数：track —— 块的 rr 跟踪器（恒非 NULL）；ecc_bits —— 本次读最大纠正 bit 数。
 *   返回：无。
 *   实现要点：连续上升计数（本次 > 上次则 +1，否则清零）；三级水位各自升级 health，
 *             只升不降（恢复由 rr_retire_block 复位）。
 * -------------------------------------------------------------------- */
void rr_record_ecc(rr_ecc_track_t *track, BYTE ecc_bits)
{
    /* 层内接口：调用者一律传 &ftl_block_t.rr（BST 常驻，恒非 NULL） */

    /* 趋势：与上一次读数比较（last_ecc 即上次值），连续上升计数 */
    if (ecc_bits > track->last_ecc)
    {
        track->rising_count++;
    }
    else
    {
        track->rising_count = 0U; /* 不上升就清空连续计数 */
    }
    track->last_ecc = ecc_bits;

    /* 健康状态**单调升级**：只取 max，绝不回落。
     * ⚠ 逐条直接赋值会让 CRITICAL/DYING 块在后续几次低 ECC 连续上升时被打回
     *   WARNING（例如 1→2→3→4 触发 rising 门）——那会掩盖慢死信号。 */
    BYTE level = track->health;

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

/* ── 2. 恢复路径（异步 job 状态机）──
 *
 * 三级管线的**分步**实现：rr_step 的 stage 序列每步只提交一条命令就返回。
 *
 *   Stage 1（读电压重试，attempt = 1..RR_MAX_RETRY）
 *     SET_FEATURES(attempt) → 重试读 → 失败则 SET_FEATURES(0) → 下一 attempt
 *     · SET_FEATURES 失败（提交失败 / NAND_FAIL）⇒ **直接**下一 attempt，**不**恢复模式。
 *     · 重试读失败 ⇒ 先恢复模式 0 再下一 attempt（含最后一次 attempt，无例外）。
 *     · 重试读成功 ⇒ 本 plane 恢复成功（**不**恢复模式 0——成功后紧接着结算/收尾）。
 *   Stage 2（邻居页辅助，neighbor_i = 0..RR_NEIGHBOR_ATTEMPTS-1）
 *     · 邻居选择：page∓1、块内回绕（与原 rr_neighbor_assist 同）
 *     · 邻居读成功 ⇒ 再试目标页；成功 ⇒ 本 plane 恢复成功
 *     · 邻居读失败 / 再试失败 ⇒ 下一个邻居；都用完 ⇒ 本 plane 失败
 *   Stage 3（显式兜底）
 *     · 本 plane 失败 ⇒ 各 CW 清零 + bbt_mark_bad（XOR/RAID 恢复属后续阶段，不做）
 *
 * ⚠ 本文件的每一条命令都走 ftl_async_* + ftl_op_submit（**不同步等待**）——
 *   服务函数里再跑完成泵就是嵌套泵（见 ftl_op.h）。
 * ⚠ rr_select_plane 里的 l2p_cache_read 不同步加载：region 未命中时
 *   它起异步加载并返回 FTL_AGAIN，rr_step 原地等下轮（`stall_ms` 清零，
 *   加载在飞不算提交卡死）。本文件至此**零同步等待**。
 */

/* 提交一条 SET_FEATURES（读重试电压模式）。背压 ⇒ FTL_AGAIN，job 原地等下轮。 */
static ftl_status_t rr_submit_set_features(read_recovery_handle_t *rr, BYTE mode)
{
    fc_command_t *cmd;

    const ftl_status_t st = ftl_async_set_features(&cmd, rr->port.topo.ch, rr->port.topo.ce, rr->port.topo.lun, RR_FEATURE_READ_RETRY, mode);
    if (st != FTL_OK)
    {
        return st; /* FTL_AGAIN 命令池空 / FTL_ERROR 建链失败 */
    }

    /* waiting 先于提交置位、提交失败再回滚：反序（提交成功后才置位）在"完成早于置位"
     * 的路径上会丢掉一次等待状态——本仓单线程顺序主循环下不会发生，但先置位零成本。 */
    rr->waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_READ_RECOVERY, rr) != FTL_OK)
    {
        rr->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    /* ⚠ 卡死计数在**提交成功之后**才清零（不是准备成功时）：`ftl_op_submit` 自身失败
     *   （完成表满 / SQ 满）也是一轮"提交不出去"，若在此前清零则计数永远停在 1、
     *   submit deadline 永不触发。 */
    rr->stall_ms = 0U;

    return FTL_OK;
}

/* 提交一条页读（目标页或邻居页）。数据在完成时由服务函数拷进 rr->port.page_scratch
 * ——**读**不像写那样在 prepare 阶段搬数据（见 ftl_async_read_page 的注释）。 */
static ftl_status_t rr_submit_read(read_recovery_handle_t *rr, DWORD pba, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd;

    const ftl_status_t st = ftl_async_read_page(&cmd, pba, cfg);
    if (st != FTL_OK)
    {
        return st;
    }

    rr->waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_READ_RECOVERY, rr) != FTL_OK)
    {
        rr->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    /* ⚠ 卡死计数在**提交成功之后**才清零（不是准备成功时）：`ftl_op_submit` 自身失败
     *   （完成表满 / SQ 满）也是一轮"提交不出去"，若在此前清零则计数永远停在 1、
     *   submit deadline 永不触发。 */
    rr->stall_ms = 0U;

    return FTL_OK;
}

/* 记账本 attempt 的重试次数，再决定去向：还有 attempt ⇒ 再切模式；用满 ⇒ 进 Stage 2。 */
static void rr_next_attempt(read_recovery_handle_t *rr)
{
    rr->retry_count = rr->attempt;

    if (rr->attempt < RR_MAX_RETRY)
    {
        rr->attempt++;
        rr->stage = RR_STAGE_SF_SET;
    }
    else
    {
        rr->neighbor_i = 0U;
        rr->stage      = RR_STAGE_NB_READ;
    }
}

/* 下一个邻居；已用满 ⇒ 本 plane 失败，去结算。 */
static void rr_next_neighbor(read_recovery_handle_t *rr)
{
    if ((rr->neighbor_i + 1U) < RR_NEIGHBOR_ATTEMPTS)
    {
        rr->neighbor_i++;
        rr->stage = RR_STAGE_NB_READ;
    }
    else
    {
        rr->stage = RR_STAGE_PLANE_END;
    }
}

/* --------------------------------------------------------------------
 * rr_select_plane
 *   功能：为当前 plane 找恢复目标页——该 plane 第一个被本子操作覆盖的 LBA 的映射。
 *   参数：ftl —— 实例。
 *   返回：RR_PLANE_READY 找到（page / page_lba / first 已填）；RR_PLANE_SKIP 本子操作
 *         不覆盖本 plane（或该 LBA 未映射）⇒ 调用者跳过本 plane；RR_PLANE_WAIT 该 LBA
 *         的 region **尚未就绪**（本函数已起异步加载）⇒ 调用者原地等待、下轮重来。
 *   实现要点：与原 ftl_recover_read_subop 的选页循环逐条一致——只看**第一个** plane
 *             匹配的 k（命中即 break，无论映射是否有效）。目标页取自 L2P 而非失败
 *             命令的 desc 链：读失败不改 L2P，二者同一。
 *   ⚠ 三态：必须把"还不知道"（RR_PLANE_WAIT）与"未映射"（RR_PLANE_SKIP）分清——把前者当后者会让
 *     RR 直接判"本 plane 无需恢复"，host 读到的是零填而非真数据。函数可重入（每轮从
 *     头扫，先清 page/page_lba/first）。
 * -------------------------------------------------------------------- */
static rr_plane_step_t rr_select_plane(read_recovery_handle_t *rr)
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

            if (l2p_cache_read(rr->port.cache, l, &cur) == FTL_AGAIN)
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

/* --------------------------------------------------------------------
 * rr_settle_plane
 *   功能：结算当前 plane——恢复成功按 CW 偏移拷回 caller 缓冲，失败则清零 + 标坏块。
 *   参数：ftl —— 实例。
 *   返回：无。
 *   实现要点：恢复读是**整页**读、从 CW0 起连续拷出（落 rr_page_scratch），而子操作
 *             可能只覆盖页内部分 CW ⇒ 必须按各 CW 的真实偏移（ftl_lba_cw）搬回，
 *             否则整段错位。
 * -------------------------------------------------------------------- */
static void rr_settle_plane(read_recovery_handle_t *rr)
{
    for (BYTE k = 0U; k < rr->owner.n_cw; k++)
    {
        const DWORD l = (rr->owner.lba + k);

        if (ftl_lba_plane(l) != rr->plane)
        {
            continue;
        }
        BYTE *dst = rr->owner.upper_buf + (k * FTL_IU_BYTES);

        if (rr->recovered != 0U)
        {
            memcpy(dst, rr->port.page_scratch + (ftl_lba_cw(l) * FTL_IU_BYTES), FTL_IU_BYTES);
        }
        else
        {
            memset(dst, 0, FTL_IU_BYTES);
        }
    }

    if (rr->recovered == 0U)
    {
        rr->all_ok = 0U; /* 任一 plane 失败 ⇒ 整个托管子操作按 IO_ERROR 结算 */
        rr->planes_failed++;

        const DWORD lba    = rr->owner.lba + rr->first;
        const WORD  gblock = ftl_pba_gblock(rr->page);

        /* Stage 1（读重试）+ Stage 2（邻居辅助）全失败 ⇒ 页面已损坏提升为块级不可靠，
         * 显式标记坏块（RR Stage 3 XOR/RAID 恢复属后续阶段，当前不做）。 */
        LOGE("[FTL] read unrecoverable lba=%lu gblock=%u retry=%u\r\n", lba, (unsigned)gblock, (unsigned)rr->retry_count);
        (void)bbt_mark_bad(rr->port.alloc, gblock);
    }
    else
    {
        rr->planes_recovered++;
    }
}

/* 收尾：释放被托管的 pending 槽 + 让 host 读组结算。
 * owners 必须在释放**之前**取走——组完成通知可能重入并复用本槽。 */
static void rr_finish(read_recovery_handle_t *rr)
{
    const ssd_status_t status =
        (rr->all_ok != 0U) ? SSD_STATUS_OK : SSD_STATUS_IO_ERROR;

    /* all_ok 是**所有被覆盖 plane 的聚合**：任一 plane 不可恢复或提交侧 abort
     * 都按 IO_ERROR 结算；全部恢复成功才报 OK（数据已由 rr_settle_plane 拷回）。
     * 释放 pending 槽与组结算都走端口（RR 不直接摸 pending 表）。 */
    const rr_pending_port_t *pending = &rr->port.pending;

    pending->complete(pending->ctx, rr->owner.pending_idx, rr->owner.owners, status);

    rr->has_owner = 0U;
    rr->stage     = RR_STAGE_IDLE;
    rr->waiting   = 0U;
}

/* 卡死兜底：提交侧长期无进展（命令池泄漏），按恢复失败收尾。
 * ⚠ **不**标坏块——卡死是控制器侧资源问题，与介质无关，标坏会白丢一个好块。 */
static void rr_abort(read_recovery_handle_t *rr)
{
    LOGE("[RR] submit stalled, abort lba=%lu\r\n", rr->owner.lba);

    for (BYTE k = 0U; k < rr->owner.n_cw; k++)
    {
        memset(rr->owner.upper_buf + (k * FTL_IU_BYTES), 0, FTL_IU_BYTES);
    }

    rr->all_ok = 0U;
    rr->error  = (int32_t)FTL_TIMEOUT;
    rr->stage  = RR_STAGE_FINISH;
}

/* 提交结果处理：FTL_ERROR = 地址/建链硬失败 ⇒ 立即 abort；
 * FTL_AGAIN = 背压 ⇒ stage 不变，下轮 rr_handle 重试。 */
static void rr_submit_result(read_recovery_handle_t *rr, ftl_status_t st)
{
    if (st == FTL_ERROR)
    {
        rr_abort(rr);
    }
}

/* 邻居页号：neighbor 0 = 前一页、1 = 后一页，块内回绕。 */
static DWORD rr_neighbor_page(DWORD page, BYTE neighbor_i)
{
    if (neighbor_i == 0U)
    {
        return (page > 0U) ? (page - 1U) : (PAGES_PER_BLOCK - 1U);
    }

    return (page < (PAGES_PER_BLOCK - 1U)) ? (page + 1U) : 0U;
}

/* --------------------------------------------------------------------
 * rr_step
 *   功能：推进状态机——跑纯计算 stage 直到提交出一条命令、或整个 job 收尾。
 *   参数：ftl —— 实例。
 *   返回：无。
 *   实现要点：**只在没有命令在飞时调用**（waiting == 0）。提交 stage 提交成功即
 *             return（stage 原地不动，等完成服务函数改写）；提交失败（背压）同样
 *             return，stage 不变 ⇒ 下轮 rr_handle 再试同一 stage（幂等）。
 * -------------------------------------------------------------------- */
static void rr_step(read_recovery_handle_t *rr)
{
    for (;;)
    {
        switch ((rr_stage_t)rr->stage)
        {
            case RR_STAGE_IDLE:
            {
                /* 优先用 rr_trigger 登记的 owner；否则问 pending 端口要一个
                 * 被托管的读子操作。都没有就空闲退出。 */
                if (rr->has_owner == 0U)
                {
                    const rr_pending_port_t *pending = &rr->port.pending;
                    rr_owner_t               owner;

                    if ((pending->take == NULL) || (!pending->take(pending->ctx, &owner)))
                    {
                        return;
                    }
                    rr->owner     = owner;
                    rr->has_owner = 1U;
                }

                rr->plane            = 0U;
                rr->recovered        = 0U;
                rr->planes_recovered = 0U;
                rr->planes_failed    = 0U;
                rr->error            = (int32_t)FTL_OK;
                rr->all_ok           = 1U; /* 聚合结果：任一 plane 失败才清零 */
                rr->stall_ms         = 0U; /* 新 job：卡死计数从头算（不复用上一 job 的） */
                rr->stage            = RR_STAGE_PLANE;
                break;
            }

            case RR_STAGE_PLANE:
            {
                rr_plane_step_t sel;

                if (rr->plane >= PLANE_NUM)
                {
                    rr->stage = RR_STAGE_FINISH;
                    break;
                }

                sel = rr_select_plane(rr);

                if (sel == RR_PLANE_WAIT)
                {
                    /* L2P region 在加载：原地等，**不计**提交卡死——
                     * 加载在飞是有确定完成时刻的进展。rr_handle 每轮会 +1，这里清零。 */
                    rr->stall_ms = 0U;
                    return;
                }
                if (sel == RR_PLANE_SKIP)
                {
                    rr->plane++; /* 本子操作不覆盖本 plane ⇒ 跳过 */
                    break;
                }
                rr->attempt     = 1U;
                rr->neighbor_i  = 0U;
                rr->recovered   = 0U;
                rr->retry_count = 0U;
                rr->stage       = RR_STAGE_SF_SET;
                break;
            }

            case RR_STAGE_SF_SET:
            {
                const ftl_status_t st = rr_submit_set_features(rr, rr->attempt);

                rr_submit_result(rr, st);
                return;
            }

            case RR_STAGE_READ:
            {
                const ftl_page_cfg_t *cfg = FTL_PAGE_CFG_DATA(rr->page_lba);
                const ftl_status_t    st  = rr_submit_read(rr, rr->page, cfg);

                rr_submit_result(rr, st);
                return;
            }

            case RR_STAGE_SF_RESTORE:
            {
                const ftl_status_t st = rr_submit_set_features(rr, RR_FEATURE_READ_RETRY_MODE);

                rr_submit_result(rr, st);
                return;
            }

            case RR_STAGE_NB_READ:
            {
                /* 邻居选择沿用原 rr_neighbor_assist（块内回绕）。 */
                const DWORD page     = pba_to_page(rr->page);
                const DWORD neighbor = rr_neighbor_page(page, rr->neighbor_i);

                /* 只换 page 字段：rr.page 的 ch/ce/lun/block/plane 原样保留
                 * （LUN 随 PBA 携带，不假设恒 0）。 */
                rr->neighbor = pba_with_page(rr->page, neighbor);

                /* 邻居页的 LBA **本就未知**（辅助读只要"这一页读得出来"这个事实）⇒
                 * FTL_PAGE_CFG_DATA_ANY 让命令 META = FC_META_UNKNOWN，跳过页内 META 比对。
                 * ⚠ 用 FTL_PAGE_CFG_DATA(0) 会把"LBA 就是 0"当事实下传 ⇒ 恒置 MATA_ERROR。 */
                const ftl_page_cfg_t *cfg = FTL_PAGE_CFG_DATA_ANY();
                const ftl_status_t    st  = rr_submit_read(rr, rr->neighbor, cfg);

                rr_submit_result(rr, st);
                return;
            }

            case RR_STAGE_NB_RETRY:
            {
                const ftl_page_cfg_t *cfg = FTL_PAGE_CFG_DATA(rr->page_lba);
                const ftl_status_t    st  = rr_submit_read(rr, rr->page, cfg);

                rr_submit_result(rr, st);
                return;
            }

            case RR_STAGE_PLANE_END:
                rr_settle_plane(rr);
                rr->plane++;
                rr->stage = RR_STAGE_PLANE;
                break;

            case RR_STAGE_FINISH:
                rr_finish(rr);
                break; /* 回到 IDLE，继续看还有没有下一个托管项 */

            default:
                ASSERT(0); /* 状态机无此态 */
                return;
        }
    }
}

/* --------------------------------------------------------------------
 * rr_service_step
 *   功能：Read Recovery job 的推进主体（typed 入口的内部实现）。
 *   实现要点：分三支——空闲 ⇒ rr_step 领活；命令在飞 ⇒ 直接返回；未在飞又推进不下去
 *             （提交侧背压）⇒ 累计无进展时长，超 FTL_SUBMIT_TIMEOUT_MS 判卡死收尾。
 *             幂等且廉价：空闲且无托管项时只问一次 pending 端口。
 * -------------------------------------------------------------------- */
static void rr_service_step(read_recovery_handle_t *rr)
{
    /* 心跳（见 ftl_job.h）：恢复在飞期间每满 1s 打一行 stage/age。
     * ⚠ 判「在飞」用 stage 而非 waiting——提交侧背压时 stage 不变但无命令在飞，
     *   那段同样属于「job 卡住」，正是心跳要暴露的。 */
    if (rr->stage == RR_STAGE_IDLE)
    {
        rr->log_ms   = 0U;
        rr->start_ms = 0U;
    }
    else if (ftl_job_heartbeat(&rr->log_ms, &rr->start_ms))
    {
        LOGI("[RR] job stage=%u plane=%u attempt=%u retry=%u stall=%lums age=%lums\r\n",
             (unsigned)rr->stage,
             (unsigned)rr->plane,
             (unsigned)rr->attempt,
             (unsigned)rr->retry_count,
             rr->stall_ms,
             ftl_job_age_ms(&rr->start_ms));
    }

    if (rr->waiting != 0U)
    {
        return; /* 命令在飞：NAND 层自身超时保证它**必有**完成（超时置 FC_CW_STATUS_TIMEOUT） */
    }

    if (rr->stage == RR_STAGE_IDLE)
    {
        rr_step(rr); /* 无 job：看有没有新托管的子操作 */
        return;
    }

    /* 有 job 却没在飞 = 上轮提交失败。连续无进展超过 submit deadline ⇒ 判卡死收尾。 */
    {
        const DWORD now_ms = ftl_job_now_ms();

        if (now_ms != 0U)
        {
            if (rr->stall_ms == 0U)
            {
                rr->stall_ms = now_ms;
            }
            else if ((now_ms - rr->stall_ms) >= FTL_SUBMIT_TIMEOUT_MS)
            {
                rr->error = (int32_t)FTL_TIMEOUT;
                rr_abort(rr);
            }
        }
    }

    rr_step(rr);
}

/* ── typed handle（端口装配 / 触发 / 上报） ── */

void rr_init(read_recovery_handle_t *rr, const rr_port_t *port)
{
    memset(rr, 0, sizeof(*rr));
    rr->port  = *port;
    rr->error = (int32_t)FTL_OK;
}

void rr_trigger(read_recovery_handle_t *rr, const rr_owner_t *owner)
{
    /* 单飞行：busy 时不覆盖当前 owner；被托管的 pending 项仍在表里，
     * 当前 job 结束后由 pending 端口的 take 领走（行为与旧扫描一致）。 */
    if ((rr->stage == RR_STAGE_IDLE) && (rr->has_owner == 0U))
    {
        rr->owner     = *owner;
        rr->has_owner = 1U;
    }
}

static void rr_status_fill(const read_recovery_handle_t *rr, rr_status_t *out)
{
    out->base.critical  = true; /* host 读在等结果，不推进即死等 */
    out->base.wants_run = (rr->stage != RR_STAGE_IDLE) || (rr->has_owner != 0U);
    out->base.progress  = rr->retry_count;
    out->base.next_ms   = 0U;
    out->base.error     = rr->error;

    if (rr->waiting != 0U)
    {
        out->base.state = FTL_MODULE_WAITING;
    }
    else if (rr->stage != RR_STAGE_IDLE)
    {
        out->base.state = (rr->error != (int32_t)FTL_OK) ? FTL_MODULE_FAILED : FTL_MODULE_RUNNING;
    }
    else if (rr->has_owner != 0U)
    {
        out->base.state = FTL_MODULE_RUNNING;
    }
    else
    {
        out->base.state = FTL_MODULE_IDLE;
    }

    out->stage            = rr->stage;
    out->waiting          = (rr->waiting != 0U);
    out->all_ok           = (rr->all_ok != 0U);
    out->retries          = rr->retry_count;
    out->planes_recovered = rr->planes_recovered;
    out->planes_failed    = rr->planes_failed;
    out->desc_count       = 0U; /* compact 重下发机制：不保留失败命令的 desc */
}

void rr_handle(read_recovery_handle_t *rr, const ftl_bg_info_t *info, rr_status_t *out)
{
    /* info 当前不参与恢复决策；保留形参以统一 typed 入口。 */
    (void)info;

    rr_service_step(rr);
    rr_status_fill(rr, out);
}

/* ── RR 各 stage 的完成处理（ftl_read_recovery_callback 按 rr->stage 分派）──
 *
 * 共同约定：命令的 release 与 rr_step 续跑**不**在这里做（见 dispatcher）——
 * 但需要 cmd 数据的分支（READ / NB_*）必须在此把数据拷进 rr_page_scratch，
 * 因为 release 之后 desc 链失效。 */

/* 切读重试模式（SET_FEATURES）完成。⚠ 判据只认 NAND_FAIL，不是全集：
 * 模式命令不读数据，无 UNC/CRC 可言；用全集会把无关位置 1 当失败。 */
static void rr_complete_sf_set(read_recovery_handle_t *rr, const fc_command_status_t *completion)
{
    if ((completion->status & FC_CW_STATUS_NAND_FAIL) != 0U)
    {
        rr_next_attempt(rr); /* 切模式失败：**不**恢复模式，直接下一 attempt */
    }
    else
    {
        rr->stage = RR_STAGE_READ;
    }
}

/* 重试读完成。成功 ⇒ 拷出数据、本 plane 完成。
 * ⚠ 不恢复模式 0 —— 原 rr_exec_retry 的成功分支亦然，保持不变。
 * 失败 ⇒ 先恢复默认读模式（原 Step 3 无条件执行，含最后一次 attempt）。 */
static void rr_complete_read(read_recovery_handle_t *rr, fc_command_t *cmd, const fc_command_status_t *completion)
{
    rr->retry_count = rr->attempt;

    if (!fc_cw_status_has_error(completion->status))
    {
        (void)ftl_cmd_read_data(cmd, rr->port.page_scratch, FTL_READ_USER_SIZE);
        rr->recovered = 1U;
        rr->stage     = RR_STAGE_PLANE_END;
    }
    else
    {
        rr->stage = RR_STAGE_SF_RESTORE;
    }
}

/* 模式恢复完成：只管把读电压切回默认，**不看** NAND_FAIL，一律进入下一
 * attempt（或 Stage 2）。 */
static void rr_complete_sf_restore(read_recovery_handle_t *rr)
{
    rr_next_attempt(rr);
}

/* 邻居读完成。得出来 ⇒ 块本身没问题，可能是目标页局部 bit 错：再试目标页。
 * 邻居数据本身不用 ⇒ 拷进 rr_page_scratch 也无妨，它随后会被 NB_RETRY 的结果覆盖。 */
static void rr_complete_nb_read(read_recovery_handle_t *rr, fc_command_t *cmd, const fc_command_status_t *completion)
{
    if (!fc_cw_status_has_error(completion->status))
    {
        (void)ftl_cmd_read_data(cmd, rr->port.page_scratch, FTL_READ_USER_SIZE);
        rr->stage = RR_STAGE_NB_RETRY;
    }
    else
    {
        rr_next_neighbor(rr);
    }
}

/* 邻居重试读完成：成功 ⇒ 拷出数据并收尾；失败 ⇒ 换下一个邻居。 */
static void rr_complete_nb_retry(read_recovery_handle_t *rr, fc_command_t *cmd, const fc_command_status_t *completion)
{
    if (!fc_cw_status_has_error(completion->status))
    {
        (void)ftl_cmd_read_data(cmd, rr->port.page_scratch, FTL_READ_USER_SIZE);
        rr->recovered = 1U;
        rr->stage     = RR_STAGE_PLANE_END;
    }
    else
    {
        rr_next_neighbor(rr);
    }
}

/* --------------------------------------------------------------------
 * ftl_read_recovery_callback
 *   功能：Read Recovery 命令的完成服务函数（g_op_callbacks[FTL_OP_READ_RECOVERY]）。
 *   参数：ftl —— 命令归属实例（ftl_op_dispatch 按命令自身 ch/ce/lun 解析后传入）；
 *         completion —— FC 完成状态；ctx —— rr。
 *   返回：无。
 *   实现要点：按 stage 分派到 rr_complete_* 判完成结果并改 stage，然后 release
 *             命令 → rr_step 续跑。
 *             ⚠ 成功判据：SET_FEATURES 只认 NAND_FAIL；读认
 *               FC_CW_STATUS_ERROR_MASK 全集（同 ftl_sync_read_page）。
 *             ⚠ 本函数**不得**同步等待（见 ftl_op.h）。
 * -------------------------------------------------------------------- */
void ftl_read_recovery_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    /* 形参由完成派发签名固定；release 下 ASSERT 裁空 ⇒ 显式丢弃（H30）。 */
    (void)ftl;

    WORD          cmd_id = completion->command_id;
    fc_command_t *cmd    = fc_command_id_to_command(cmd_id);

    /* ftl 恒非空（路由表早于逐实例 boot 设好，见 ftl_op.h）；NULL = 编程错误 ⇒
     * ASSERT 而非 if-guard：release 下解引用留现场，比静默吞完成可定位。 */

    read_recovery_handle_t *rr = (read_recovery_handle_t *)ctx;
    ASSERT(rr == &ftl->read_recovery_handle);

    /* 旁表项缺失（ctx 为 NULL）或本 job 已不在飞 ⇒ 孤儿/迟到的完成，只 release。
     * 正常路径不该出现：每条 RR 命令提交时都登记了 &ftl->read_recovery_handle，
     * 且只被 take 一次。 */
    if ((rr == NULL) || (rr->waiting == 0U))
    {
        (void)fc_command_release(cmd);
        return;
    }

    rr->waiting = 0U;

    switch ((rr_stage_t)rr->stage)
    {
        case RR_STAGE_SF_SET:
            rr_complete_sf_set(rr, completion);
            break;

        case RR_STAGE_READ:
            rr_complete_read(rr, cmd, completion);
            break;

        case RR_STAGE_SF_RESTORE:
            rr_complete_sf_restore(rr);
            break;

        case RR_STAGE_NB_READ:
            rr_complete_nb_read(rr, cmd, completion);
            break;

        case RR_STAGE_NB_RETRY:
            rr_complete_nb_retry(rr, cmd, completion);
            break;

        default:
            /* 纯计算 stage 不该有命令在飞；到此即状态机与簿记不一致。 */
            ASSERT(0);
            break;
    }

    (void)fc_command_release(cmd);

    rr_step(rr);
}
/* ── 3. 判定（块数组 / 全表扫描）── */

/* rr_check_predictive — 在块数组 [0,max_blocks) 里找第一个 health ≥ CRITICAL 的块，
 * 命中块号经 *gblock 出参给出（返回值只说"有没有命中"）。 */
bool rr_check_predictive(const ftl_block_t *blocks, WORD max_blocks, WORD *gblock)
{
    if ((blocks == NULL) || (gblock == NULL))
    {
        return false;
    }

    for (WORD i = 0U; i < max_blocks; i++)
    {
        const rr_ecc_track_t *track = &blocks[i].rr;

        if (track->health >= RR_BLOCK_CRITICAL)
        {
            *gblock = i;
            return true;
        }
    }

    return false;
}

/* rr_get_health — 取块健康状态（track 为 NULL 返回 HEALTHY） */
rr_block_health_t rr_get_health(const rr_ecc_track_t *track)
{
    if (track == NULL)
    {
        return RR_BLOCK_HEALTHY;
    }

    return (rr_block_health_t)track->health;
}

/* --------------------------------------------------------------------
 * rr_predictive_scan
 *   功能：全表扫 CLOSED 块，找第一个需要慢死搬移的块（ECC 趋势恶化或 read disturb 高）。
 *   参数：alloc —— 分配器（blocks 恒非 NULL）；gblock —— 出参块号。
 *   返回：true 找到；false 未找到。
 *   实现要点：只针对 CLOSED 块（Open 还在写、Free 无数据、BAD/RETIRED 已出局）；
 *             命中条件 = health ≥ CRITICAL **或** read_count ≥ RR_READ_DISTURB_THRESHOLD
 *             （慢死第一信号）。调用者 mig_begin_predict 用 sb_id 反查 Closed 条目。
 * -------------------------------------------------------------------- */
bool rr_predictive_scan(const ftl_allocator_t *alloc, WORD *gblock)
{
    /* 层内接口：唯一调用者 mig_begin_predict 传 rr->port.alloc 与局部 gblock */

    for (WORD i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &alloc->blocks[i];

        /* 慢死预测只针对 Closed 块：Open 还在写、Free 没数据、Retired/Bad 已出局 */
        if (ftl_block_state(b) != FTL_BLOCK_STATE_CLOSED)
        {
            continue;
        }

        /* ECC 趋势（连续上升 ≥RISING / 单次 ≥HIGH_WATER）或 read disturb
         * 高水位（read_count 超阈值，慢死第一信号）都触发搬移。 */
        if ((b->rr.health >= RR_BLOCK_CRITICAL) ||
            (b->read_count >= RR_READ_DISTURB_THRESHOLD))
        {
            /* 候选 SB 的另一个 plane 若已 BAD，migrate 读它必失败并 abort，
             * 而巡检每轮仍返回同一块 ⇒ 预测路径永久卡死。跳过该 SB。 */
            const WORD sb_id = ftl_block_sb_id(b);

            if (sb_id >= alloc->closed_sb_count)
            {
                continue; /* 状态与闭链表不一致（不应发生）：不拿它冒险 */
            }

            const super_block_t *sb       = allocator_closed_at(alloc, sb_id);
            bool                 bad_peer = false;

            for (BYTE p = 0U; p < PLANE_NUM; p++)
            {
                if (!super_block_has_plane(sb, p))
                {
                    continue;
                }
                if (ftl_block_is_bad(&alloc->blocks[ftl_gblock(p, sb->plane_blocks[p])]))
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

/* --------------------------------------------------------------------
 * rr_retire_block
 *   功能：慢死块退役——置 RETIRED（不再分配新数据），不标坏（BBT 不删、仍可读），健康复位。
 *   参数：b —— 目标块（恒非 NULL）。
 *   返回：无。
 *   实现要点：read_count 归零 = read disturb 重计（块仍活着，只是不再写）。
 *             计数随下一次 Checkpoint 的磨损快照持久化（不再有独立的 dirty 标记）。
 * -------------------------------------------------------------------- */
void rr_retire_block(ftl_block_t *b)
{
    /* 层内接口：唯一调用者 ftl_migrate_callback（MIG_ERASE 完成分支）传 rr->port.alloc.blocks[gblock] */

    /* 退役：不再分配新数据（RETIRED 态），不标坏（BBT 不删、仍可读）。
     * ⚠ **调用者负责先把该块驱逐出 Free Pool**（本函数只拿 ftl_block_t*，拿不到 gblock）——
     *   只置 RETIRED 挡不住分配：allocator_select_plane_block_from_pool 的判据是
     *   Free Pool **数组**（只跳过 is_bad），留在池里的退役块照样会被选中。
     *   由 migrate.c 的 MIG_ERASE RETIRE 分支调 allocator_remove_from_free 完成。 */
    ftl_block_set_state(b, FTL_BLOCK_STATE_RETIRED);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    /* 健康复位：块仍活着，ECC 趋势重计（read_count 归零，read disturb 重计） */
    b->rr.health       = RR_BLOCK_HEALTHY;
    b->rr.rising_count = 0U;
    b->rr.last_ecc     = 0U;
    b->read_count      = 0U;
}
