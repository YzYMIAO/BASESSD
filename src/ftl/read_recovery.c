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
 *   2. 恢复路径     rr_kick / rr_step（static）/ ftl_op_read_recovery（异步 job 状态机）
 *   3. 判定扫描     rr_check_predictive / rr_get_health / rr_predictive_scan
 *   4. 退役         rr_retire_block
 *
 * ⚠ 层内接口（rr_record_ecc / rr_kick / rr_predictive_scan / rr_retire_block）
 *   形参由调用者保证非空 ⇒ ASSERT（判据 2：调用者保证的前置条件）。
 */

#include "ftl/read_recovery.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "fc/fc_command.h"
#include "ftl/bbt.h"              /* bbt_mark_bad */
#include "ftl/ftl.h"              /* ftl_context_t（含 pending[] / cache / rr_page_scratch） */
#include "ftl/ftl_fc_interface.h" /* ftl_async_read_page / ftl_async_set_features / ftl_cmd_read_data */
#include "ftl/ftl_op.h"           /* ftl_op_submit / FTL_OP_READ_RECOVERY */
#include "nand/onfi_1_0.h"

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
void rr_record_ecc(rr_ecc_track_t *track, uint8_t ecc_bits)
{
    /* 层内接口：调用者一律传 &ftl_block_t.rr（BST 常驻，恒非 NULL） */
    ASSERT(track != NULL);

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

    /* 健康状态升级 */
    if (track->rising_count >= RR_ECC_RISING_THRESHOLD)
    {
        track->health = RR_BLOCK_WARNING;
    }
    if (ecc_bits >= RR_ECC_HIGH_WATER)
    {
        track->health = RR_BLOCK_CRITICAL;
    }
    if (ecc_bits >= RR_ECC_MAX)
    {
        track->health = RR_BLOCK_DYING;
    }
}

/* ── 2. 恢复路径（异步 job 状态机）──
 *
 * 三级管线的**分步**实现：原 rr_recover_read 把三级串成一次阻塞调用，本版把它拆成
 * rr_step 的 stage 序列，每步只提交一条命令就返回。逐条对照原实现：
 *
 *   Stage 1（读电压重试，attempt = 1..RR_MAX_RETRY）
 *     SET_FEATURES(attempt) → 重试读 → 失败则 SET_FEATURES(0) → 下一 attempt
 *     · SET_FEATURES 失败（提交失败 / NAND_FAIL）⇒ **直接**下一 attempt，**不**恢复模式
 *       （原 rr_exec_retry 在 Step 1 就 return -1，根本走不到 Step 3）
 *     · 重试读失败 ⇒ 先恢复模式 0 再下一 attempt（原 Step 3 无条件执行，含最后一次 attempt）
 *     · 重试读成功 ⇒ 本 plane 恢复成功（**不**恢复模式 0 —— 原实现的既有行为，保持不变）
 *   Stage 2（邻居页辅助，neighbor_i = 0..RR_NEIGHBOR_ATTEMPTS-1）
 *     · 邻居选择：page∓1、块内回绕（与原 rr_neighbor_assist 同）
 *     · 邻居读成功 ⇒ 再试目标页；成功 ⇒ 本 plane 恢复成功
 *     · 邻居读失败 / 再试失败 ⇒ 下一个邻居；都用完 ⇒ 本 plane 失败
 *   Stage 3（显式兜底）
 *     · 本 plane 失败 ⇒ 各 CW 清零 + bbt_mark_bad（XOR/RAID 恢复属后续阶段，不做）
 *
 * ⚠ 本文件的每一条命令都走 ftl_async_* + ftl_op_submit（**不同步等待**）——
 *   服务函数里再跑完成泵就是嵌套泵（见 ftl_op.h）。
 * ⚠ 阶段 5 起 rr_select_plane 里的 l2p_cache_read 也不再同步加载：region 未命中时
 *   它起异步加载并返回 FTL_AGAIN，rr_step 原地等下轮（`stall_rounds` 清零，
 *   加载在飞不算提交卡死）。本文件至此**零同步等待**。
 */

/* 提交一条 SET_FEATURES（读重试电压模式）。背压 ⇒ FTL_AGAIN，job 原地等下轮。 */
static ftl_status_t rr_submit_set_features(ftl_context_t *ftl, uint8_t mode)
{
    fc_command_t *cmd;

    if (ftl_async_set_features(&cmd, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, RR_FEATURE_READ_RETRY, mode) != FTL_OK)
    {
        return FTL_AGAIN; /* 命令池空 */
    }

    /* waiting 先于提交置位、提交失败再回滚：反序（提交成功后才置位）在"完成早于置位"
     * 的路径上会丢掉一次等待状态——本仓单线程顺序主循环下不会发生，但先置位零成本。 */
    ftl->rr.waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_READ_RECOVERY, &ftl->rr) != FTL_OK)
    {
        ftl->rr.waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    /* ⚠ 卡死计数在**提交成功之后**才清零（不是准备成功时）：`ftl_op_submit` 自身失败
     *   （完成表满 / SQ 满）也是一轮"提交不出去"，若在此前清零则计数永远停在 1、
     *   中止判据 RR_SUBMIT_STALL_MAX 永不触发。 */
    ftl->rr.stall_rounds = 0U;

    return FTL_OK;
}

/* 提交一条页读（目标页或邻居页）。数据在完成时由服务函数拷进 ftl->rr_page_scratch
 * ——**读**不像写那样在 prepare 阶段搬数据（见 ftl_async_read_page 的注释）。 */
static ftl_status_t rr_submit_read(ftl_context_t *ftl, nand_pba_t pba, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd;

    if (ftl_async_read_page(&cmd, pba, cfg) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    ftl->rr.waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_READ_RECOVERY, &ftl->rr) != FTL_OK)
    {
        ftl->rr.waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    /* ⚠ 卡死计数在**提交成功之后**才清零（不是准备成功时）：`ftl_op_submit` 自身失败
     *   （完成表满 / SQ 满）也是一轮"提交不出去"，若在此前清零则计数永远停在 1、
     *   中止判据 RR_SUBMIT_STALL_MAX 永不触发。 */
    ftl->rr.stall_rounds = 0U;

    return FTL_OK;
}

/* 记账本 attempt 的重试次数，再决定去向：还有 attempt ⇒ 再切模式；用满 ⇒ 进 Stage 2。 */
static void rr_next_attempt(ftl_context_t *ftl)
{
    ftl->rr.retry_count = ftl->rr.attempt;

    if (ftl->rr.attempt < RR_MAX_RETRY)
    {
        ftl->rr.attempt++;
        ftl->rr.stage = (uint8_t)RR_STAGE_SF_SET;
    }
    else
    {
        ftl->rr.neighbor_i = 0U;
        ftl->rr.stage      = (uint8_t)RR_STAGE_NB_READ;
    }
}

/* 下一个邻居；已用满 ⇒ 本 plane 失败，去结算。 */
static void rr_next_neighbor(ftl_context_t *ftl)
{
    if (((uint32_t)ftl->rr.neighbor_i + 1U) < RR_NEIGHBOR_ATTEMPTS)
    {
        ftl->rr.neighbor_i++;
        ftl->rr.stage = (uint8_t)RR_STAGE_NB_READ;
    }
    else
    {
        ftl->rr.stage = (uint8_t)RR_STAGE_PLANE_END;
    }
}

/* --------------------------------------------------------------------
 * rr_select_plane
 *   功能：为当前 plane 找恢复目标页——该 plane 第一个被本子操作覆盖的 LBA 的映射。
 *   参数：ftl —— 实例。
 *   返回：1 找到（page / page_lba / first 已填）；0 本子操作不覆盖本 plane（或该 LBA
 *         未映射）⇒ 调用者跳过本 plane；-1 该 LBA 的 region **尚未就绪**（本函数已起
 *         异步加载）⇒ 调用者原地等待、下轮重来。
 *   实现要点：与原 ftl_recover_read_subop 的选页循环逐条一致——只看**第一个** plane
 *             匹配的 k（命中即 break，无论映射是否有效）。目标页取自 L2P 而非失败
 *             命令的 desc 链：读失败不改 L2P，二者同一。
 *   ⚠ 三态（阶段 5）：必须把"还不知道"（-1）与"未映射"（0）分清——把前者当后者会让
 *     RR 直接判"本 plane 无需恢复"，host 读到的是零填而非真数据。函数可重入（每轮从
 *     头扫，先清 page/page_lba/first）。
 * -------------------------------------------------------------------- */
static int rr_select_plane(ftl_context_t *ftl)
{
    const ftl_pending_req_t *pending = &ftl->pending[ftl->rr.pending_idx];
    uint8_t                  k;

    ftl->rr.page     = NAND_PBA_INVALID;
    ftl->rr.page_lba = 0U;
    ftl->rr.first    = 0U;

    for (k = 0U; k < pending->n_cw; k++)
    {
        const lba_t l = (lba_t)(pending->lba + (lba_t)k);

        if (ftl_lba_plane(l) == ftl->rr.plane)
        {
            nand_pba_t cur;

            if (l2p_cache_read(&ftl->cache, l, &cur) == FTL_AGAIN)
            {
                return -1; /* region 在加载：下轮再来 */
            }

            if (cur != NAND_PBA_INVALID)
            {
                ftl->rr.page     = pba_page(cur);
                ftl->rr.page_lba = ftl_lba_page(l);
                ftl->rr.first    = k;
            }
            break;
        }
    }

    return (ftl->rr.page != NAND_PBA_INVALID) ? 1 : 0;
}

/* --------------------------------------------------------------------
 * rr_settle_plane
 *   功能：结算当前 plane——恢复成功按 CW 偏移拷回 caller 缓冲，失败则清零 + 标坏块。
 *   参数：ftl —— 实例。
 *   返回：无。
 *   实现要点：恢复读是**整页**读、从 CW0 起连续拷出（落 rr_page_scratch），而子操作
 *             可能只覆盖页内部分 CW ⇒ 必须按各 CW 的真实偏移（ftl_lba_cw）搬回，
 *             否则整段错位（与原实现逐条一致）。
 * -------------------------------------------------------------------- */
static void rr_settle_plane(ftl_context_t *ftl)
{
    ftl_pending_req_t *pending = &ftl->pending[ftl->rr.pending_idx];
    uint8_t            k;

    for (k = 0U; k < pending->n_cw; k++)
    {
        const lba_t l = (lba_t)(pending->lba + (lba_t)k);
        uint8_t    *dst;

        if (ftl_lba_plane(l) != ftl->rr.plane)
        {
            continue;
        }
        dst = (uint8_t *)pending->buffer + ((size_t)k * FTL_IU_BYTES);

        if (ftl->rr.recovered != 0U)
        {
            memcpy(dst, ftl->rr_page_scratch + ((size_t)ftl_lba_cw(l) * FTL_IU_BYTES), FTL_IU_BYTES);
        }
        else
        {
            memset(dst, 0, FTL_IU_BYTES);
        }
    }

    if (ftl->rr.recovered == 0U)
    {
        /* Stage 1（读重试）+ Stage 2（邻居辅助）全失败 ⇒ 页面已损坏提升为块级不可靠，
         * 显式标记坏块（RR Stage 3 XOR/RAID 恢复属后续阶段，当前不做）。 */
        LOGE("[FTL] read unrecoverable lba=%lu gblock=%u retry=%u\r\n",
             (unsigned long)(pending->lba + (lba_t)ftl->rr.first),
             (unsigned)ftl_pba_gblock(ftl->rr.page),
             (unsigned)ftl->rr.retry_count);
        (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(ftl->rr.page));
    }
}

/* 收尾：释放被托管的 pending 槽 + 让 host 读组结算。
 * owners 必须在释放**之前**取走——组完成通知可能重入并复用本槽。 */
static void rr_finish(ftl_context_t *ftl)
{
    const uint32_t owners = ftl->pending[ftl->rr.pending_idx].owners;

    pending_release_index(ftl, ftl->rr.pending_idx);

    /* ⚠ 一律 IO_ERROR：沿用原实现语义——原代码在 if/else 之后**无条件**写 IO_ERROR，
     *   覆盖了 recovered==0 分支的 OK，即"recovery 成功仍报错"。语义存疑、属既有缺陷；
     *   本次只搬位置，未顺手改。 */
    pending_group_complete_subop_multi(ftl, owners, SSD_STATUS_IO_ERROR);

    ftl->rr.stage   = (uint8_t)RR_STAGE_IDLE;
    ftl->rr.waiting = 0U;
}

/* 卡死兜底：提交侧长期无进展（命令池泄漏），按恢复失败收尾。
 * ⚠ **不**标坏块——卡死是控制器侧资源问题，与介质无关，标坏会白丢一个好块。 */
static void rr_abort(ftl_context_t *ftl)
{
    ftl_pending_req_t *pending = &ftl->pending[ftl->rr.pending_idx];
    uint8_t            k;

    LOGE("[RR] submit stalled, abort lba=%lu\r\n", (unsigned long)pending->lba);

    for (k = 0U; k < pending->n_cw; k++)
    {
        memset((uint8_t *)pending->buffer + ((size_t)k * FTL_IU_BYTES), 0, FTL_IU_BYTES);
    }

    ftl->rr.stage = (uint8_t)RR_STAGE_FINISH;
}

/* --------------------------------------------------------------------
 * rr_step
 *   功能：推进状态机——跑纯计算 stage 直到提交出一条命令、或整个 job 收尾。
 *   参数：ftl —— 实例。
 *   返回：无。
 *   实现要点：**只在没有命令在飞时调用**（waiting == 0）。提交 stage 提交成功即
 *             return（stage 原地不动，等完成服务函数改写）；提交失败（背压）同样
 *             return，stage 不变 ⇒ 下轮 rr_kick 再试同一 stage（幂等）。
 * -------------------------------------------------------------------- */
static void rr_step(ftl_context_t *ftl)
{
    for (;;)
    {
        switch ((rr_stage_t)ftl->rr.stage)
        {
            case RR_STAGE_IDLE:
                /* 领走一个被托管的子操作；没有就空闲退出。 */
                if (pending_find_op(ftl, PENDING_OP_READ_RECOVERY, &ftl->rr.pending_idx) == 0)
                {
                    return;
                }
                ftl->rr.plane        = 0U;
                ftl->rr.recovered    = 0U;
                ftl->rr.stall_rounds = 0U; /* 新 job：卡死计数从头算（不复用上一 job 的） */
                ftl->rr.stage        = (uint8_t)RR_STAGE_PLANE;
                break;

            case RR_STAGE_PLANE:
            {
                int sel;

                if (ftl->rr.plane >= PLANE_NUM)
                {
                    ftl->rr.stage = (uint8_t)RR_STAGE_FINISH;
                    break;
                }

                sel = rr_select_plane(ftl);

                if (sel < 0)
                {
                    /* L2P region 在加载（阶段 5）：原地等，**不计**提交卡死——
                     * 加载在飞是有确定完成时刻的进展。rr_kick 每轮会 +1，这里清零。 */
                    ftl->rr.stall_rounds = 0U;
                    return;
                }
                if (sel == 0)
                {
                    ftl->rr.plane++; /* 本子操作不覆盖本 plane ⇒ 跳过 */
                    break;
                }
                ftl->rr.attempt     = 1U;
                ftl->rr.neighbor_i  = 0U;
                ftl->rr.recovered   = 0U;
                ftl->rr.retry_count = 0U;
                ftl->rr.stage       = (uint8_t)RR_STAGE_SF_SET;
                break;
            }

            case RR_STAGE_SF_SET:
                (void)rr_submit_set_features(ftl, ftl->rr.attempt);
                return;

            case RR_STAGE_READ:
                (void)rr_submit_read(ftl, ftl->rr.page, FTL_PAGE_CFG_DATA(ftl->rr.page_lba));
                return;

            case RR_STAGE_SF_RESTORE:
                (void)rr_submit_set_features(ftl, RR_FEATURE_READ_RETRY_MODE);
                return;

            case RR_STAGE_NB_READ:
            {
                /* 邻居选择与 pba_make 逐字沿用原 rr_neighbor_assist（块内回绕）。 */
                const uint32_t page     = pba_to_page(ftl->rr.page);
                const uint32_t block    = pba_to_block(ftl->rr.page);
                const uint32_t plane    = pba_to_plane(ftl->rr.page);
                const uint32_t neighbor = (ftl->rr.neighbor_i == 0U)
                                              ? ((page > 0U) ? (page - 1U) : (PAGES_PER_BLOCK - 1U))
                                              : ((page < (PAGES_PER_BLOCK - 1U)) ? (page + 1U) : 0U);

                /* lun 恒 0（LUN_NUM = 1）。 */
                ftl->rr.neighbor = pba_make(ftl->topo.ch, ftl->topo.ce, 0U, block, plane, neighbor, 0U);

                /* 邻居页的 LBA **本就未知**（辅助读只要"这一页读得出来"这个事实）⇒
                 * FTL_PAGE_CFG_DATA_ANY 让命令 META = FC_META_UNKNOWN，跳过页内 META 比对。
                 * ⚠ 用 FTL_PAGE_CFG_DATA(0) 会把"LBA 就是 0"当事实下传 ⇒ 恒置 MATA_ERROR。 */
                (void)rr_submit_read(ftl, ftl->rr.neighbor, FTL_PAGE_CFG_DATA_ANY());
                return;
            }

            case RR_STAGE_NB_RETRY:
                (void)rr_submit_read(ftl, ftl->rr.page, FTL_PAGE_CFG_DATA(ftl->rr.page_lba));
                return;

            case RR_STAGE_PLANE_END:
                rr_settle_plane(ftl);
                ftl->rr.plane++;
                ftl->rr.stage = (uint8_t)RR_STAGE_PLANE;
                break;

            case RR_STAGE_FINISH:
                rr_finish(ftl);
                break; /* 回到 IDLE，继续看还有没有下一个托管项 */

            default:
                ASSERT(0); /* 状态机无此态 */
                return;
        }
    }
}

/* --------------------------------------------------------------------
 * rr_kick
 *   功能：Read Recovery job 的唯一驱动入口（每轮 ftl_service 调一次）。
 *   参数：ftl —— 实例。
 *   返回：无。
 *   实现要点：分三支——空闲 ⇒ rr_step 领活；命令在飞 ⇒ 直接返回；未在飞又推进不下去
 *             （提交侧背压）⇒ 累计 stall_rounds，超 RR_SUBMIT_STALL_MAX 判卡死收尾。
 *             幂等且廉价：空闲且无托管项时只是一次 32 项线性扫描。
 * -------------------------------------------------------------------- */
void rr_kick(ftl_context_t *ftl)
{
    ASSERT(ftl != NULL);

    if (ftl->rr.waiting != 0U)
    {
        return; /* 命令在飞：NAND 层自身超时保证它**必有**完成（超时置 FC_CW_STATUS_TIMEOUT） */
    }

    if (ftl->rr.stage == (uint8_t)RR_STAGE_IDLE)
    {
        rr_step(ftl); /* 无 job：看有没有新托管的子操作 */
        return;
    }

    /* 有 job 却没在飞 = 上轮提交失败。连续无进展超限 ⇒ 判卡死收尾。 */
    if (ftl->rr.stall_rounds >= RR_SUBMIT_STALL_MAX)
    {
        rr_abort(ftl);
    }
    else
    {
        ftl->rr.stall_rounds++;
    }

    rr_step(ftl);
}

/* --------------------------------------------------------------------
 * ftl_op_read_recovery
 *   功能：Read Recovery 命令的完成服务函数（g_op_service[FTL_OP_READ_RECOVERY]）。
 *   参数：ftl —— 命令归属实例（ftl_op_dispatch 按命令自身 ch/ce/lun 解析后传入）；
 *         completion —— FC 完成状态；ctx —— &ftl->rr。
 *   返回：无。
 *   实现要点：按 stage 判完成结果 → 改 stage → release 命令 → rr_step 续跑。
 *             ⚠ 成功判据逐条对齐原同步实现：SET_FEATURES 只认 NAND_FAIL；读认
 *               FC_CW_STATUS_ERROR_MASK 全集（同 ftl_sync_read_page）。
 *             ⚠ 本函数**不得**同步等待（见 ftl_op.h）。
 * -------------------------------------------------------------------- */
void ftl_op_read_recovery(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    uint16_t             cmd_id = (uint16_t)completion->command_id;
    fc_command_t        *cmd    = fc_command_id_to_command(cmd_id);
    ftl_read_recovery_t *rr;

    ASSERT(cmd != NULL);

    /* 开机早期 g_route 尚未 set_route ⇒ ftl 可为 NULL（正常状态）：只 release。 */
    if (ftl == NULL)
    {
        (void)fc_command_release(cmd);
        return;
    }

    rr = (ftl_read_recovery_t *)ctx;
    ASSERT(rr == &ftl->rr);

    /* 旁表项缺失（ctx 为 NULL）或本 job 已不在飞 ⇒ 孤儿/迟到的完成，只 release。
     * 正常路径不该出现：每条 RR 命令提交时都登记了 &ftl->rr，且只被 take 一次。 */
    if ((rr != &ftl->rr) || (rr->waiting == 0U))
    {
        (void)fc_command_release(cmd);
        return;
    }

    rr->waiting = 0U;

    switch ((rr_stage_t)rr->stage)
    {
        case RR_STAGE_SF_SET:
            if ((completion->status & FC_CW_STATUS_NAND_FAIL) != 0U)
            {
                rr_next_attempt(ftl); /* 切模式失败：**不**恢复模式，直接下一 attempt */
            }
            else
            {
                rr->stage = (uint8_t)RR_STAGE_READ;
            }
            break;

        case RR_STAGE_READ:
            rr->retry_count = rr->attempt;
            if ((completion->status & FC_CW_STATUS_ERROR_MASK) == 0U)
            {
                /* 重试成功：拷出数据（**release 之前**），本 plane 完成。
                 * ⚠ 不恢复模式 0 —— 原 rr_exec_retry 的成功分支亦然，保持不变。 */
                (void)ftl_cmd_read_data(cmd, ftl->rr_page_scratch, FTL_READ_USER_SIZE);
                rr->recovered = 1U;
                rr->stage     = (uint8_t)RR_STAGE_PLANE_END;
            }
            else
            {
                /* 失败 ⇒ 先恢复默认读模式（原 Step 3 无条件执行，含最后一次 attempt）。 */
                rr->stage = (uint8_t)RR_STAGE_SF_RESTORE;
            }
            break;

        case RR_STAGE_SF_RESTORE:
            /* 原实现只判提交/等待成败、**不**看 NAND_FAIL，且无论结果都返回 -1
             * ⇒ 一律进下一 attempt（或 Stage 2）。 */
            rr_next_attempt(ftl);
            break;

        case RR_STAGE_NB_READ:
            if ((completion->status & FC_CW_STATUS_ERROR_MASK) == 0U)
            {
                /* 邻居读得出来 ⇒ 块本身没问题，可能是目标页局部 bit 错：再试目标页。
                 * 邻居数据本身不用（原实现读进局部 neighbor_buf 后即弃）⇒ 拷进
                 * rr_page_scratch 也无妨，它随后会被 NB_RETRY 的结果覆盖。 */
                (void)ftl_cmd_read_data(cmd, ftl->rr_page_scratch, FTL_READ_USER_SIZE);
                rr->stage = (uint8_t)RR_STAGE_NB_RETRY;
            }
            else
            {
                rr_next_neighbor(ftl);
            }
            break;

        case RR_STAGE_NB_RETRY:
            if ((completion->status & FC_CW_STATUS_ERROR_MASK) == 0U)
            {
                (void)ftl_cmd_read_data(cmd, ftl->rr_page_scratch, FTL_READ_USER_SIZE);
                rr->recovered = 1U;
                rr->stage     = (uint8_t)RR_STAGE_PLANE_END;
            }
            else
            {
                rr_next_neighbor(ftl);
            }
            break;

        default:
            /* 纯计算 stage 不该有命令在飞；到此即状态机与簿记不一致。 */
            ASSERT(0);
            break;
    }

    (void)fc_command_release(cmd);

    rr_step(ftl);
}
/* ── 3. 判定（块数组 / 全表扫描）── */

/* rr_check_predictive — 在块数组 [0,max_blocks) 里找第一个 health ≥ CRITICAL 的块，返回其下标 */
int rr_check_predictive(const ftl_block_t *blocks, uint16_t max_blocks, uint16_t *gblock)
{
    uint16_t i;

    if ((blocks == NULL) || (gblock == NULL))
    {
        return 0;
    }

    for (i = 0U; i < max_blocks; i++)
    {
        const rr_ecc_track_t *track = &blocks[i].rr;

        if (track->health >= RR_BLOCK_CRITICAL)
        {
            *gblock = i;
            return 1;
        }
    }

    return 0;
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
 *   返回：1 找到；0 未找到。
 *   实现要点：只针对 CLOSED 块（Open 还在写、Free 无数据、Bad/Reserved 已退役）；
 *             命中条件 = health ≥ CRITICAL **或** read_count ≥ RR_READ_DISTURB_THRESHOLD
 *             （慢死第一信号）。调用者 mig_begin_predict 用 sb_id 反查 Closed 条目。
 * -------------------------------------------------------------------- */
int rr_predictive_scan(const ftl_allocator_t *alloc, uint16_t *gblock)
{
    uint16_t i;

    /* 层内接口：唯一调用者 mig_begin_predict 传 &ftl->alloc 与局部 gblock */
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);
    ASSERT(gblock != NULL);

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &alloc->blocks[i];

        /* 慢死预测只针对 Closed 块：Open 还在写、Free 没数据、Bad/Reserved 已退役 */
        if (!ftl_block_state_is(b, FTL_BLOCK_STATE_CLOSED))
        {
            continue;
        }

        /* B5：ECC 趋势（连续上升 ≥RISING / 单次 ≥HIGH_WATER）或 read disturb
         * 高水位（read_count 超阈值，慢死第一信号）都触发搬移。 */
        if ((b->rr.health >= RR_BLOCK_CRITICAL) ||
            (b->read_count >= RR_READ_DISTURB_THRESHOLD))
        {
            *gblock = i;
            return 1;
        }
    }

    return 0;
}

/* ── 4. 退役 ── */

/* --------------------------------------------------------------------
 * rr_retire_block
 *   功能：慢死块退役——置 RESERVED（不再分配新数据），不标坏（BBT 不删、仍可读），健康复位。
 *   参数：b —— 目标块（恒非 NULL）。
 *   返回：无。
 *   实现要点：read_count 归零 = read disturb 重计（块仍活着，只是不再写）。
 *             ⚠ 归零后需调用者补 wear_mark_dirty（本函数只拿 ftl_block_t*，拿不到 gblock）。
 * -------------------------------------------------------------------- */
void rr_retire_block(ftl_block_t *b)
{
    /* 层内接口：唯一调用者 ftl_op_migrate（MIG_ERASE 完成分支）传 &ftl->alloc.blocks[gblock] */
    ASSERT(b != NULL);

    /* 退役：不再分配新数据（RESERVED = Spare Pool 语义），不标坏（BBT 不删、仍可读） */
    ftl_block_state_clr(b, FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED | FTL_BLOCK_STATE_FREE | FTL_BLOCK_TEMP_MASK);
    ftl_block_state_set(b, FTL_BLOCK_STATE_RESERVED);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    /* 健康复位：块仍活着，ECC 趋势重计（read_count 归零，read disturb 重计） */
    b->rr.health       = (uint8_t)RR_BLOCK_HEALTHY;
    b->rr.rising_count = 0U;
    b->rr.last_ecc     = 0U;
    b->read_count      = 0U;
}
