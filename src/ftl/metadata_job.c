/*
 * metadata_job.c — 元数据目录整理 job：日志块回收 + 抗碎片压缩
 *
 * 与 metadata.c 的分工：
 *   metadata.c     —— Directory 结构、Anchor 落盘/加载、region 追加/登记（同步原语）；
 *   metadata_job.c —— 两件"要碰 NAND 的整理活"的异步状态机：
 *                     ① 回收：待回收位图（reclaim_pending）里的块 ⇒ 擦 + 归还固定 L2P 日志区；
 *                     ② 压缩：挑活 region 最少的 victim，搬进当前块，落 Anchor 后擦除。
 *   共享的内部原语（位图 / slot_now / job_reset）也定义在本文件，声明在
 *   include/ftl/metadata_job.h。
 *
 * 设计约束（与 metadata.h 的 md_handle_t 长注释配套）：
 *   · **一次只提交一条命令**：提交阶段设 waiting 就返回；纯 RAM 阶段连续推进到下一个
 *     提交阶段或终态。推进一律由 md_handle 驱动（服务函数不推进，见下）。
 *   · **服务函数不推进状态机**（同 ftl_checkpoint_callback）：CP_ANCHOR_PREP 要调
 *     md_anchor_prepare，而它在游标未建立时会**同步**扫池定位
 *     （md_ring_locate → ftl_sync_read_page）——在完成泵里跑就是嵌套泵
 *     （l2p_sync_pump 首行有 ASSERT 挡）。故 ftl_metadata_callback 只记结果 + release + 改
 *     phase，下一轮 kick 接着走。
 *   · **提交失败（背压）保留现场**：压缩搬移的槽位取到后即消耗（写指针已推进），
 *     故 `job.slot` 在提交失败时必须留着，下轮重试**同一个**槽——绝不能重取
 *     （重取会白烧一页）；待回收位在擦除在飞/失败期间也保持置位。
 */

#include "metadata_job.h"

#include "assertion.h"
#include "bbt.h" /* bbt_mark_bad（持续擦除失败标坏，RISK-MD-01） */
#include "fc.h"
#include "ftl.h" /* ftl_ctx_t（完成回调签名） */
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_METADATA */
#include "log.h"
#include "onfi_1_0.h"

#include <string.h>

/* 提交 deadline：连续提交不出去超过 FTL_SUBMIT_TIMEOUT_MS 即中止 job（同 CP/MIG/RR）。 */

/* 同一块连续擦除失败多少次后标坏（RISK-MD-01）。
 * 3 次覆盖瞬态擦除失败（忙/超时）；持续失败只能来自物理坏块，继续重试没有意义：
 * 保留回收位会让该块永久占着轮转队列、每次后台都白付一次擦除命令。 */
#define MD_RECLAIM_FAIL_MAX (3U)

/* 压缩空槽预算为「扫描期间可能落回 victim 的前台 region 写」预留的槽位数。
 * 一条前台 flush 现在**双副本占两个槽**（RISK-BOOT-18 L2）⇒ 预留 2；见 md_compact_begin。 */
#define MD_COMPACT_RESERVED_SLOTS (L2P_REGION_COPIES)

/* md_block_page_pba — gblock + 页号 → PBA（ch/ce/lun 取本 Directory 拓扑）。
 * ⚠ 单一转换漏斗（C9）：block/plane 的字段序只在这里出现一次，调用点不手写 pba_make。 */
static DWORD md_block_page_pba(const md_dir_cache_t *dir,
                               WORD                  gblock,
                               BYTE                  page)
{
    return pba_make(dir->topo.ch,
                    dir->topo.ce,
                    dir->topo.lun,
                    ftl_gblock_block(gblock),
                    ftl_gblock_plane(gblock),
                    page,
                    0U);
}

/* ── 待回收位图 / 写指针 / job 状态（H42：改状态的闸门定义在 .c）── */

bool md_reclaim_mark(md_dir_cache_t *dir, WORD g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return false;
    }

    DWORD *word = &dir->reclaim_pending[g >> 5U];
    DWORD  bit  = 1UL << (g & 31U);

    if ((*word & bit) != 0U)
    {
        return false;
    }

    *word |= bit;
    dir->reclaim_pending_count++;
    return true;
}

bool md_reclaim_take(md_dir_cache_t *dir, WORD *out)
{
    /* 从上次取走的位置继续扫描并环绕：擦除持续失败的块保留位，但不再永久占住
     * 扫描起点，后续块仍能被回收。 */
    for (WORD i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const WORD g = ((dir->reclaim_scan + i) % FTL_BLOCK_TOTAL);

        if ((dir->reclaim_pending[g >> 5U] & (1UL << (g & 31U))) != 0U)
        {
            *out              = g;
            dir->reclaim_scan = ((g + 1U) % FTL_BLOCK_TOTAL);
            return true;
        }
    }

    return false;
}

void md_reclaim_clear(md_dir_cache_t *dir, WORD g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return;
    }

    DWORD *word = &dir->reclaim_pending[g >> 5U];
    DWORD  bit  = 1UL << (g & 31U);

    if ((*word & bit) != 0U)
    {
        *word &= ~bit;
        if (dir->reclaim_pending_count > 0U)
        {
            dir->reclaim_pending_count--;
        }
    }
}

void md_reclaim_reset(md_dir_cache_t *dir)
{
    memset(dir->reclaim_pending, 0, sizeof(dir->reclaim_pending));
    dir->reclaim_pending_count = 0U;
    dir->reclaim_scan          = 0U;
}

void md_reclaim_mark_all(md_dir_cache_t *dir, const alc_t *alloc)
{
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (!md_log_block_reclaimable(dir, alloc, g))
        {
            continue;
        }
        (void)md_reclaim_mark(dir, g);
    }
}

DWORD md_take_slot(md_dir_cache_t *dir)
{
    if ((!dir->cur_block_valid) || (dir->cur_metadata_page >= PAGES_PER_BLOCK))
    {
        return NAND_PBA_INVALID;
    }

    const BYTE page = dir->cur_metadata_page;

    dir->cur_metadata_page++;

    return md_block_page_pba(dir, dir->cur_metadata_block, page);
}

void md_handle_reset(md_dir_cache_t *dir)
{
    /* reclaimed_count 是**单调进度计数**（跨 job 保留）；其余 job 状态清零。
     * error 也保留到下一次起 job 时再清（status 要能看到最近一次失败）。 */
    const DWORD   reclaimed  = dir->handle.reclaimed_count;
    const int32_t last_error = dir->handle.error;

    memset(&dir->handle, 0, sizeof(dir->handle));
    dir->handle.victim              = UINT16_MAX;
    dir->handle.reclaim_gblock      = UINT16_MAX;
    dir->handle.reclaim_fail_gblock = UINT16_MAX;
    dir->handle.slot                = NAND_PBA_INVALID;
    dir->handle.reclaimed_count     = reclaimed;
    dir->handle.error               = last_error;
}

/* --------------------------------------------------------------------
 * md_compact_victim
 *   功能：选压缩 victim —— 活 region 数**最少**（但 > 0）的非当前日志块。
 *   参数：dir —— Directory；alloc —— 分配器（读 BST 判 LOG 身份）。
 *   返回：victim 的 gblock；无候选返回 UINT16_MAX。
 *   实现要点：零活块**不在此列**——它们已登记在 reclaim_pending，由 RECLAIM 阶段
 *             直接擦除归还，无需搬移。平手取块号最小者：结果确定（跨复位可复现），
 *             且先分配的块通常收的是更早、更可能变冷的数据。
 * -------------------------------------------------------------------- */
static WORD md_compact_victim(md_dir_cache_t *dir,
                              alc_t          *alloc)
{
    WORD best      = UINT16_MAX;
    WORD best_live = UINT16_MAX;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const WORD live = dir->l2p_live[g];

        if (live == 0U)
        {
            continue; /* 零活块：reclaim 的活，直接擦，不必搬 */
        }
        if (dir->cur_block_valid && (g == dir->cur_metadata_block))
        {
            continue; /* 当前写块正在被追加，不搬 */
        }
        if (ftl_block_state(&alloc->blocks[g]) != FTL_BLOCK_STATE_LOG)
        {
            continue; /* 只认本池独占的日志块（否则会误搬恰好活计数 > 0 的用户块）*/
        }
        if (live < best_live)
        {
            best_live = live;
            best      = g;
        }
    }

    return best;
}

/* 中止日志限流：真异常（读页失败 / Anchor 写坏 / 账本分叉）会每轮 GC 重复，
 * 逐次 WRN 会刷屏；5 s 一行 + 被抑制条数（时钟缺失时 64 次一行）。 */
static DWORD s_abort_log_ms;
static DWORD s_abort_log_n;

static bool md_abort_log_due(DWORD *out_suppressed)
{
    const DWORD now_ms = ftl_job_now_ms();

    bool due = (s_abort_log_n >= 64U);

    if ((!due) && (now_ms != 0U))
    {
        due = (s_abort_log_ms == 0U) || ((now_ms - s_abort_log_ms) >= FTL_JOB_LOG_SLOW_INTERVAL_MS);
    }

    if (!due)
    {
        s_abort_log_n++;
        return false;
    }

    *out_suppressed = s_abort_log_n;
    s_abort_log_n   = 0U;
    s_abort_log_ms  = now_ms;
    return true;
}

/* 中止本次整理：清请求与游标，回 IDLE。
 * ⚠ **victim 不擦**（已登记的 region 已指向新页、未搬的仍指向 victim ⇒ 两边都有效），
 *   下次换块时重来即可——失败不回滚。
 * ⚠ 若 victim 已被搬空（live == 0），登记进待回收位图保持覆盖范围。 */
static void md_handle_abort(md_dir_cache_t *dir)
{
    md_handle_t *j = &dir->handle;

    if (j->timed_out != 0U)
    {
        /* 提交超时 = 资源 starvation 的瞬态中止：victim 不擦、下轮重来，无数据风险。
         * 每轮打 WRN 会被当成故障并刷屏 ⇒ 只在 debug 档可见。 */
        LOGD(LOG_TAG_META "md job abort (transient timeout) phase=%u victim=%u moved=%u\r\n",
             (unsigned)j->timeout_phase,
             (unsigned)j->victim,
             (unsigned)j->moved);
    }
    else
    {
        DWORD suppressed = 0U;

        if (md_abort_log_due(&suppressed))
        {
            LOGE(LOG_TAG_META "md job abort phase=%u victim=%u moved=%u (victim 不擦, suppressed=%lu)\r\n",
                 (unsigned)j->phase,
                 (unsigned)j->victim,
                 (unsigned)j->moved,
                 suppressed);
        }
    }

    if ((j->victim != UINT16_MAX) && (dir->l2p_live[j->victim] == 0U))
    {
        (void)md_reclaim_mark(dir, j->victim);
    }

    md_handle_reset(dir);
    dir->handle.error = (int32_t)FTL_ERROR; /* status.error：最近一次中止的归因 */
}

/* 提交不出去超过 FTL_SUBMIT_TIMEOUT_MS ⇒ 中止 job（同 CP/MIG/RR 同族）。
 * 返回 true = 已中止。时钟缺失时 deadline 不生效。 */
static bool md_stall(md_dir_cache_t *dir)
{
    md_handle_t *j      = &dir->handle;
    const DWORD  now_ms = ftl_job_now_ms();

    if (now_ms == 0U)
    {
        return false;
    }
    if (j->stall_ms == 0U)
    {
        j->stall_ms = now_ms;
        return false;
    }
    if ((now_ms - j->stall_ms) >= FTL_SUBMIT_TIMEOUT_MS)
    {
        LOGW(LOG_TAG_META "md job submit stalled %lums — abort\r\n", (now_ms - j->stall_ms));
        j->timed_out     = 1U;
        j->timeout_phase = j->phase;
        md_handle_abort(dir);
        return true;
    }
    return false;
}

/* 提交一条已建好链的异步命令（所有 md_submit_* 的唯一漏斗）。
 * 先把 waiting 置位、提交失败再回滚；stall_ms 只在提交成功后清零。 */
static ftl_status_e md_submit(md_dir_cache_t *dir, fc_command_t *cmd)
{
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    md_handle_t *j = &dir->handle;

    j->waiting = true;

    /* ctx = Directory（模块上下文），完成服务据此取 handle 与端口。 */
    if (ftl_op_submit(cmd, FTL_OP_METADATA, dir) != FTL_OK)
    {
        j->waiting = false;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    j->stall_ms = 0U;
    return FTL_OK;
}

/* 提交一条异步写（搬移页 / Anchor 分页共用）。data 在 ftl_async_write_page 内已拷进
 * FC buffer ⇒ 调用者的缓冲随即可复用。 */
static ftl_status_e md_submit_write(md_dir_cache_t *dir, DWORD addr, const void *data, DWORD length)
{
    fc_command_t *cmd = NULL;

    if (ftl_async_write_page(&cmd, addr, data, length, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN; /* 命令池空 / 地址非法 */
    }

    return md_submit(dir, cmd);
}

/* 提交一条**镜像**异步写（固定元数据池：同一页用 multi-plane 写全部 plane）。
 * 只用于 Anchor/CP/BBT 的元数据页；L2P region 搬移仍走单 plane 的 md_submit_write。 */
static ftl_status_e md_submit_write_mirror(md_dir_cache_t *dir, DWORD addr, const void *data, DWORD length)
{
    fc_command_t *cmd = NULL;

    if (ftl_async_write_page_mirror(&cmd, addr, data, length, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    return md_submit(dir, cmd);
}

/* 提交一条异步擦除（日志块回收 / Anchor 换块 / victim 擦除共用）。 */
static ftl_status_e md_submit_erase(md_dir_cache_t *dir, DWORD addr)
{
    fc_command_t *cmd = NULL;

    if (ftl_async_erase_block(&cmd, addr) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    return md_submit(dir, cmd);
}

/* 提交一条**镜像**异步擦除（固定元数据池的换块：所有 plane 一起擦）。 */
static ftl_status_e md_submit_erase_multi(md_dir_cache_t *dir, DWORD addr)
{
    fc_command_t *cmd = NULL;

    if (ftl_async_erase_block_multi_plane(&cmd, addr) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    return md_submit(dir, cmd);
}

/* 提交一条异步读（搬移 victim 的 region 页 → job.cp_buf）。
 * ⚠ 读的数据**留在 desc 链上** ⇒ 目标缓冲必须活到完成，ftl_cmd_read_data 必须早于
 *   release —— 二者都在 ftl_metadata_callback 里，缓冲就是 job.cp_buf（job 常驻 ⇒ 稳定）。 */
static ftl_status_e md_submit_read(md_dir_cache_t *dir, DWORD addr)
{
    fc_command_t *cmd = NULL;

    if (ftl_async_read_page(&cmd, addr, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    return md_submit(dir, cmd);
}

/* --------------------------------------------------------------------
 * md_compact_begin
 *   功能：选 victim、核空槽预算、把 job 推进到 CP_READ（纯 RAM，不碰介质）。
 *   参数：dir / alloc。
 *   返回：md_compact_start_e（起不动时 phase 留在 IDLE，由调用者决定是否换块）。
 *   实现要点：**无论成败都先清 compact_wanted**——它由 append_slot 置位而 append_slot
 *             又对 `phase != IDLE` 让路，不清就永远起不来（死锁）。
 *             空槽预算在此**重算**（不在 append_slot 置位时算）：置位到本函数之间可能
 *             又发放过若干槽位（此时 phase 还是 IDLE），按当下 cur_metadata_page 核才准。
 *   ⚠ 已知良性竞态：job 启动前已发出、尚未 commit 的前台 region 写可能在扫描期间落回
 *     victim ⇒ 实际需搬数超过本预算 ⇒ md_compact_write 取不到槽 ⇒ 中止。已登记 region
 *     指向新页、victim 不擦，无数据损失，只是本次压缩重来。
 * -------------------------------------------------------------------- */
static md_compact_start_e md_compact_begin(md_dir_cache_t *dir, alc_t *alloc)
{
    md_handle_t *j = &dir->handle;

    j->compact_wanted = false;

    if (!dir->cur_block_valid)
    {
        return MD_COMPACT_NO_CURRENT; /* 无当前写块：无处可搬 */
    }

    WORD victim = md_compact_victim(dir, alloc);

    if (victim == UINT16_MAX)
    {
        return MD_COMPACT_NO_VICTIM; /* 全池零活（reclaim 已管）或只剩当前块 */
    }

    /* 空槽预算：victim 的全部活 region 必须一次装进当前块（l2p_live 的定义就是
     * "最新位置落在该块的 region 数"）。装不下 ⇒ 留到换块后再来。
     * ⚠ 预留 MD_COMPACT_RESERVED_SLOTS 个槽位给「job 启动前已发出、扫描期间才 commit
     *     的前台 region 写」：双副本下一次 flush 占两槽，故预留 = L2P_REGION_COPIES。
     *     不预留时，在飞写落回 victim 会让实际需搬数超过预算 ⇒ slot_now 取不到槽 ⇒
     *     本次压缩中止（良性但白做）。预留槽把该竞态从「中止」降为「容量少两页」。
     *     代价：victim 活 region 数等于剩余槽位时本轮到不了它，换块后再来。 */
    const WORD free_slots = (PAGES_PER_BLOCK - dir->cur_metadata_page);

    if (dir->l2p_live[victim] + MD_COMPACT_RESERVED_SLOTS > free_slots)
    {
        return MD_COMPACT_NO_SLOTS;
    }

    j->victim     = victim;
    j->region_id  = 0U;
    j->cur_region = 0U;
    j->moved      = 0U;
    j->slot       = NAND_PBA_INVALID;
    j->anchor_ok  = false;
    j->stall_ms   = 0U;
    j->phase      = MD_JOB_CP_READ;

    LOGD(LOG_TAG_META "compact start victim=%u live=%u cur_page=%u\r\n",
         (unsigned)victim,
         (unsigned)dir->l2p_live[victim],
         (unsigned)dir->cur_metadata_page);

    return MD_COMPACT_STARTED;
}

/* --------------------------------------------------------------------
 * md_compact_read
 *   功能：从 job.region_id 起找 victim 上**下一个**仍有引用的 region，发起异步读。
 *   返回：MD_COMPACT_SCANNED = 已扫完（未提交任何命令）；MD_COMPACT_SUBMITTED = 已提交（等完成）；
 *         MD_COMPACT_BACKPRESSURE = 背压未提交（下轮重试）；MD_COMPACT_FAILED = 硬失败（调用者中止）。
 *   实现要点：游标先走后命中，提交失败再把游标退回来（重试同一 region）。
 *             搬移走的是**裸页拷贝**（region 页无头，就是 L2P_REGION_SIZE 字节的 entries
 *             数组，与 l2p_cache 的读写口径逐字节一致）——不经过 l2p_cache，故不触发
 *             "写→evict→flush→又要块"的递归。
 * -------------------------------------------------------------------- */
static md_compact_step_e md_compact_read(md_dir_cache_t *dir)
{
    md_handle_t *j = &dir->handle;

    while (j->region_id < METADATA_DIR_ENTRIES)
    {
        const WORD  rid     = j->region_id;
        const DWORD primary = md_find_entry(dir, METADATA_TYPE_L2P_REGION, rid);
        const DWORD backup  = md_find_backup(dir, rid);
        DWORD       old     = NAND_PBA_INVALID;
        BYTE        copy    = 0U;

        /* 副本感知：victim 上可能只有主、只有备、或两份都在。
         * 两份都在时先搬主，下一次重扫同一 rid 再搬备（游标不前进）。 */
        if ((primary != NAND_PBA_INVALID) && (ftl_pba_gblock(primary) == j->victim))
        {
            old  = primary;
            copy = 0U;
        }
        else if ((backup != NAND_PBA_INVALID) && (ftl_pba_gblock(backup) == j->victim))
        {
            old  = backup;
            copy = 1U;
        }

        if (old == NAND_PBA_INVALID)
        {
            j->region_id = (rid + 1U); /* 两份都不在本 victim：推进 */
            continue;
        }

        j->cur_region = rid;
        j->cur_copy   = copy;

        if (md_submit_read(dir, old) != FTL_OK)
        {
            /* 游标不前进：下轮重试同一 region 的同一副本 */
            if (md_stall(dir))
            {
                return MD_COMPACT_FAILED;
            }
            return MD_COMPACT_BACKPRESSURE;
        }
        return MD_COMPACT_SUBMITTED;
    }

    /* 扫完仍找不到任何指向 victim 的 region，但账本声称 live>0 ⇒ 账本与 Directory
     * 分叉。⚠ 绝不能就这样返回 SCANNED：caller（md_handle_step 的 `for(;;)`）会再次
     * 选中同一个 victim（它仍是最小 live>0）⇒ **主循环永不返回**、USB 静默掉线。
     * Directory 是权威：清零这条陈旧计数并以 LOGE 暴露（release 也拒绝空转，H14）。 */
    if (dir->l2p_live[j->victim] != 0U)
    {
        LOGE(LOG_TAG_META "compact: victim %u live=%u but no directory entry — clear stale count\r\n",
             (unsigned)j->victim,
             (unsigned)dir->l2p_live[j->victim]);
        dir->l2p_live[j->victim] = 0U;
    }

    return MD_COMPACT_SCANNED; /* 扫描完毕 */
}

/* --------------------------------------------------------------------
 * md_compact_write
 *   功能：把 job.cp_buf 写进当前块的新槽（槽只取一次，提交失败重试同一槽）。
 *   返回：同 md_compact_read。
 * -------------------------------------------------------------------- */
static md_compact_step_e md_compact_write(md_dir_cache_t *dir)
{
    md_handle_t *j = &dir->handle;

    if (j->slot == NAND_PBA_INVALID)
    {
        j->slot = md_take_slot(dir);
        if (j->slot == NAND_PBA_INVALID)
        {
            /* 当前块无空槽 ⇒ 预算失准。最可能来源是 md_compact_begin 注释里的
             * "启动前已在飞的前台写落回 victim"：中止是安全的（victim 不擦）。 */
            LOGW(LOG_TAG_META "compact: 当前块无空槽 (page=%u) — 中止\r\n", (unsigned)dir->cur_metadata_page);
            return MD_COMPACT_FAILED;
        }
    }

    if (md_submit_write(dir, j->slot, j->cp_buf, L2P_REGION_SIZE) != FTL_OK)
    {
        if (md_stall(dir))
        {
            return MD_COMPACT_FAILED;
        }
        return MD_COMPACT_BACKPRESSURE; /* 背压：**保留 slot**，下轮重试同一槽（重取会白烧一页）*/
    }

    return MD_COMPACT_SUBMITTED;
}

/* --------------------------------------------------------------------
 * md_compact_next
 *   功能：当前 victim 搬空后，继续挑下一个能装进当前块剩余空槽的稀疏 victim。
 *   返回：true = 已切到下一个 victim（phase=CP_READ）；false = 没有可装下的 victim。
 *   实现要点：victim 选择仍是“活 region 最少且 >0”；因为取的是最小值，若它也装不下，
 *             更大的更装不下 ⇒ 直接结束本轮压缩，交给 Anchor/回收。
 *             一次新生块可退多个稀疏块（每个 victim 约 1–2 个活 region），这是把
 *             live_log_blocks 从上千块收敛到理想值的关键（RISK-MD-07）。
 * -------------------------------------------------------------------- */
static bool md_compact_next(md_dir_cache_t *dir, alc_t *alloc)
{
    md_handle_t *j = &dir->handle;

    const WORD free_slots = (PAGES_PER_BLOCK - dir->cur_metadata_page);
    const WORD victim     = md_compact_victim(dir, alloc);

    if (victim == UINT16_MAX)
    {
        return false;
    }
    if ((dir->l2p_live[victim] + MD_COMPACT_RESERVED_SLOTS) > free_slots)
    {
        return false;
    }

    j->victim     = victim;
    j->region_id  = 0U;
    j->cur_region = 0U;
    j->slot       = NAND_PBA_INVALID;
    j->phase      = MD_JOB_CP_READ;
    return true;
}

/* --------------------------------------------------------------------
 * md_handle
 *   功能：推进整理 job 一步（每轮服务循环调一次）。见 metadata.h 的同名注释。
 *   参数：dir / alloc。
 *   返回：无。
 *   实现要点：`for (;;)` 里每个分支要么设 waiting 后 return（提交阶段），要么推进 phase
 *             后 break（纯 RAM 阶段）。**不允许既不推进也不返回**——那会死循环。
 * -------------------------------------------------------------------- */
static void md_handle_step(md_dir_cache_t *dir)
{
    /* 前置条件（判据 2）：调用者一律传 dir；allocator 走已绑定端口。 */

    alc_t *alloc = dir->alloc;

    /* 心跳（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/age。
     * 默认档（INFO）不打印；需要现场观测时 `log level debug`。 */
    if (dir->handle.phase == MD_JOB_IDLE)
    {
        dir->handle.log_ms   = 0U;
        dir->handle.start_ms = 0U;
    }
    else if (ftl_job_heartbeat(&dir->handle.log_ms, &dir->handle.start_ms))
    {
        LOGD(LOG_TAG_MD "job phase=%u victim=%u moved=%u pending=%u stall=%lums age=%lums\r\n",
             (unsigned)dir->handle.phase,
             (unsigned)dir->handle.victim,
             (unsigned)dir->handle.moved,
             (unsigned)dir->reclaim_pending_count,
             dir->handle.stall_ms,
             ftl_job_age_ms(&dir->handle.start_ms));
    }

    for (;;)
    {
        md_handle_t *j = &dir->handle;

        if (j->waiting)
        {
            return; /* 有命令在飞：等完成服务函数 */
        }

        switch ((md_handle_phase_e)j->phase)
        {
            case MD_JOB_IDLE:
            {
                j->error = (int32_t)FTL_OK; /* 新 job 起点：清上一次的失败归因 */

                if (dir->reclaim_pending_count != 0U)
                {
                    /* ③→④ 契约（同压缩路径）：普通 L2P flush 在活计数归零时就登记了
                     * 回收候选，但新位置只在 RAM Directory 里。擦除前必须先把当前
                     * Directory 写成 Anchor；否则掉电后 on-media Anchor 仍指向该块。 */
                    if (dir->dirty)
                    {
                        j->anchor_for_reclaim = true;
                        j->victim             = UINT16_MAX;
                        j->phase              = MD_JOB_CP_ANCHOR_PREP;
                        break;
                    }
                    j->phase = MD_JOB_RECLAIM;
                    break;
                }
                /* free 水位是压缩的**主判据**：空闲日志块低于水位就压，不等 live
                 * 涨到接近区上限（那时压缩已无目标块）。每轮扫 64 个块，开销可忽略。 */
                if (!j->compact_wanted && (md_log_area_free_count(alloc) <= L2P_LOG_FREE_WATERMARK))
                {
                    j->compact_wanted = true;
                }
                if (!j->compact_wanted)
                {
                    return; /* 无事可做 */
                }

                const md_compact_start_e started = md_compact_begin(dir, alloc);

                if (started != MD_COMPACT_STARTED)
                {
                    /* 空槽预算取的是**活 region 最少**的 victim：连它都装不下，其它
                     * victim 只会更装不下，原地重试永远不会成功。日志区见底时强制
                     * 换块——新块的整块空槽是压缩唯一的 destination。
                     * ⚠ 换块原本只由 append_slot 触发：host 写一停就没有追加、不会
                     *   换块，压缩永远缺目标槽 ⇒ 日志槽永不放行、脏行无法驱逐
                     *   （板端现场：live=64、job IDLE、`no_evictable`）。 */
                    const bool no_slot  = (started == MD_COMPACT_NO_SLOTS);
                    const bool no_cur   = (started == MD_COMPACT_NO_CURRENT);
                    const bool at_floor = (md_log_area_free_count(alloc) <= L2P_LOG_FREE_WATERMARK);

                    if ((no_slot || no_cur) && at_floor)
                    {
                        (void)md_log_rotate(dir, alloc);
                    }
                    return;
                }
                break;
            }

            case MD_JOB_RECLAIM:
            {
                /* Anchor 落后于 Directory（登记之后、擦除之前又有 commit）⇒ 不能擦；
                 * 回 IDLE，下一轮先写 Anchor（上面的 dirty 分支）。 */
                if (dir->dirty)
                {
                    j->phase = MD_JOB_IDLE;
                    break;
                }

                WORD g = 0U;

                if (!md_reclaim_take(dir, &g))
                {
                    /* 计数非 0 但位图全空：计数与位图不一致（不该发生）。
                     * 清计数回 IDLE，避免每轮空转。 */
                    ASSERT(dir->reclaim_pending_count == 0U);
                    dir->reclaim_pending_count = 0U;
                    j->phase                   = MD_JOB_IDLE;
                    break;
                }

                /* 消费前**重查**（登记到消费之间可能隔了很多轮，块号可能已被重新分配）：
                 *   ① 活计数非 0 ⇒ 该块又被引用了；
                 *   ② 成了当前写块 ⇒ 正在被追加；
                 *   ③ 不再带 LOG ⇒ 已被别处归还/复用。
                 * 任一命中即放弃本次（清位），**绝不擦活块**。 */
                if (!md_log_block_reclaimable(dir, alloc, g))
                {
                    md_reclaim_clear(dir, g);
                    j->phase = MD_JOB_IDLE;
                    break;
                }

                const DWORD addr = md_block_page_pba(dir, g, 0U);

                if (md_submit_erase(dir, addr) != FTL_OK)
                {
                    /* 背压：位保持置位，下轮重试（md_stall 可能已 abort，位仍保留）。 */
                    (void)md_stall(dir);
                    return;
                }

                j->reclaim_gblock = g; /* 提交**成功**才认领；位在完成成功时才清 */
                return;
            }

            case MD_JOB_CP_READ:
            {
                const md_compact_step_e r = md_compact_read(dir);

                if (r == MD_COMPACT_FAILED)
                {
                    md_handle_abort(dir);
                    break;
                }
                if (r != MD_COMPACT_SCANNED)
                {
                    return; /* 已提交 / 背压 */
                }
                /* 本 victim 已搬空。当前块还有空槽、且还有能装下的稀疏 victim ⇒ 继续搬
                 * 下一个（一次新生块退多个块）。旧实现“一个 rotation 只压一个”会让
                 * 碎片稳态停在 live≈N，日志区永远收不回来（RISK-MD-07）。 */
                if (md_compact_next(dir, alloc))
                {
                    break; /* phase 已置 CP_READ，循环继续 */
                }
                j->phase = MD_JOB_CP_ANCHOR_PREP;
                break;
            }

            case MD_JOB_CP_WRITE:
            {
                /* md_compact_write 只返回 MD_COMPACT_SUBMITTED / MD_COMPACT_BACKPRESSURE /
                 * MD_COMPACT_FAILED，**不会**返回 MD_COMPACT_SCANNED（"扫完"）
                 * ——写一条就回去读下一条。 */
                const md_compact_step_e r = md_compact_write(dir);

                if (r == MD_COMPACT_FAILED)
                {
                    md_handle_abort(dir);
                    break;
                }
                return;
            }

            case MD_JOB_CP_ANCHOR_PREP:
            {
                /* ③ Anchor：定位游标 + 取槽。游标未建立时会**同步**扫池定位——本 job
                 *   唯一可能瞬时阻塞的点，且只在冷启动后首次整理出现。 */
                bool             need_erase = false;
                md_anchor_step_e r          = md_anchor_prepare(dir,
                                                       &j->anchor_ring,
                                                       &j->anchor_base,
                                                       &j->anchor_idx,
                                                       &j->anchor_seq,
                                                       &need_erase);

                if (r == MD_ANCHOR_FAILED)
                {
                    md_handle_abort(dir);
                    break;
                }
                if (r == MD_ANCHOR_CLEAN)
                {
                    /* Directory 已与 on-media Anchor 一致：跳过写入，直接进入下一步。
                     * 普通回收路径回 RECLAIM；压缩路径回 CP_RECLAIM 擦 victim。 */
                    const bool for_reclaim = j->anchor_for_reclaim;

                    j->anchor_for_reclaim = false;
                    j->phase              = for_reclaim ? MD_JOB_RECLAIM : MD_JOB_CP_RECLAIM;
                    break;
                }

                j->anchor_need_erase = need_erase;
                j->anchor_chunk      = 0U;
                j->anchor_ok         = true;
                j->phase             = need_erase ? MD_JOB_CP_ANCHOR_ERASE : MD_JOB_CP_ANCHOR_WRITE;
                break;
            }

            case MD_JOB_CP_ANCHOR_ERASE:
                if (md_submit_erase_multi(dir, j->anchor_base) != FTL_OK)
                {
                    (void)md_stall(dir);
                }
                return;

            case MD_JOB_CP_ANCHOR_WRITE:
            {
                if (j->anchor_chunk >= ANCHOR_PAGE_COUNT)
                {
                    /* 整条 record 写完：推进游标（无论成败），成功才清 dirty。
                     * 失败 ⇒ 整条残缺（locate 会整条丢弃）⇒ 中止，不擦 victim。 */
                    md_anchor_commit(dir, &j->anchor_ring, j->anchor_idx, j->anchor_ok);

                    if (!j->anchor_ok)
                    {
                        LOGW(LOG_TAG_META "anchor write incomplete: idx=%u seq=%lu (chunk 写失败)\r\n", (unsigned)j->anchor_idx, j->anchor_seq);
                        md_handle_abort(dir);
                        break;
                    }

                    /* 只有 Anchor 确实覆盖了当前 Directory 时才允许登记新的回收候选：
                     * 否则 on-media Anchor 仍引用「看起来已死」的块，擦了就是掉电丢映射。 */
                    if (!dir->dirty)
                    {
                        md_reclaim_mark_all(dir, alloc);
                    }

                    if (j->anchor_for_reclaim)
                    {
                        j->anchor_for_reclaim = false;
                        /* 写入期间又有 commit ⇒ 回 IDLE 再写一条 Anchor，不能擦。 */
                        j->phase              = dir->dirty ? MD_JOB_IDLE : MD_JOB_RECLAIM;
                        break;
                    }

                    j->phase = MD_JOB_CP_RECLAIM;
                    break;
                }

                const DWORD anchor_page = pba_to_page(j->anchor_base) + j->anchor_chunk;
                DWORD       aaddr       = pba_with_page(j->anchor_base, anchor_page);

                md_anchor_page_t page; /* 2KB 栈缓冲：按页组好后写入 */

                md_anchor_page(dir, j->anchor_seq, j->anchor_chunk, &page);

                const DWORD page_size = sizeof(page);

                if (md_submit_write_mirror(dir, aaddr, &page, page_size) != FTL_OK)
                {
                    (void)md_stall(dir);
                }
                return;
            }

            case MD_JOB_CP_RECLAIM:
            {
                const WORD v = j->victim;

                /* ④ 擦 victim —— 只在它**确实已无引用**时（③ Anchor 已落盘）。 */
                if (v == UINT16_MAX)
                {
                    j->phase = MD_JOB_DONE;
                    break;
                }
                if (!md_log_block_reclaimable(dir, alloc, v))
                {
                    /* 扫描期间又有 region 落回 victim（在飞的写回完成）⇒ 不能擦，留给下次。 */
                    LOGD(LOG_TAG_META "compact: victim %u 仍有 %u 活 region — 不擦\r\n", (unsigned)v, (unsigned)dir->l2p_live[v]);
                    j->phase = MD_JOB_DONE;
                    break;
                }

                const DWORD addr = md_block_page_pba(dir, v, 0U);

                if (md_submit_erase(dir, addr) != FTL_OK)
                {
                    (void)md_stall(dir);
                }
                return;
            }

            case MD_JOB_DONE:
                LOGD(LOG_TAG_META "compact done victim=%u moved=%u\r\n", (unsigned)j->victim, (unsigned)j->moved);
                md_handle_reset(dir);
                break; /* 回 IDLE：继续循环（可能还有待回收的块）*/

            default:
                /* 不该发生：phase 越界 ⇒ 清回 IDLE，否则 append_slot 永久让路。 */
                LOGE(LOG_TAG_META "md job bad phase=%u — reset\r\n", (unsigned)j->phase);
                md_handle_reset(dir);
                return;
        }
    }
}

/* ── 模块 handle（typed 入口） ── */

static void md_status_fill(const md_dir_cache_t *dir, md_status_t *out)
{
    const md_handle_t *j = &dir->handle;

    out->base.critical  = false; /* 由 must-run 适配器保证 liveness */
    out->base.wants_run = (j->phase != MD_JOB_IDLE) || dir->reclaim_pending_count != 0U ||
                          j->compact_wanted;
    out->base.progress = j->reclaimed_count;
    out->base.next_ms  = 0U; /* 事件驱动，无周期 */
    out->base.error    = j->error;

    if (j->waiting)
    {
        out->base.state = FTL_MODULE_WAITING;
    }
    else if (j->phase != MD_JOB_IDLE)
    {
        out->base.state = FTL_MODULE_RUNNING;
    }
    else if (out->base.wants_run)
    {
        out->base.state = FTL_MODULE_RUNNING;
    }
    else
    {
        out->base.state = FTL_MODULE_IDLE;
    }

    out->phase           = j->phase;
    out->victim          = j->victim;
    out->moved           = j->moved;
    out->reclaimed       = j->reclaimed_count;
    out->compact_wanted  = j->compact_wanted;
    out->anchor_ok       = j->anchor_ok;
    out->reclaim_pending = (dir->reclaim_pending_count != 0U);
    out->timed_out       = (j->timed_out != 0U);
    out->timeout_phase   = j->timeout_phase;
}

void md_handle(md_dir_cache_t *dir, const bg_info_t *info, md_status_t *out)
{
    /* info 当前不参与目录整理决策（事件驱动）；保留形参以统一 typed 入口。 */
    (void)info;

    md_handle_step(dir);
    md_status_fill(dir, out);
}

/* ── 整理 job 的完成服务（g_op_callbacks[FTL_OP_METADATA]）── */

/* 每个提交阶段的完成处理（ftl_metadata_callback 按 phase 分派）。
 * ⚠ 与 cp_complete_* 不同，**不能**把 release 提到派发处统一做：CP_READ 必须先
 *   ftl_cmd_read_data 再 release（数据在 desc 链上）。 */

/* 擦除成功后的统一收尾：元数据日志块不经 migrate 擦除路径，磨损计数在这里补记，
 * 且**先记账再归还**。固定区块回固定区（NONE）；用户区借来的块回 Free Pool。 */
static void md_note_block_erased(md_dir_cache_t *dir, WORD gblock)
{
    const DWORD total_writes = *dir->total_writes;

    alc_bst_note_erase(dir->alloc, gblock, total_writes);
    md_log_release(dir, gblock);
}

/* 待回收块擦完成：失败 ⇒ 位保持置位（下一轮 kick 重新取到它并重试，轮转游标保证
 * 它不会永久挡住后续块）；成功 ⇒ 清位 + 归还。 */
static void md_complete_reclaim(md_dir_cache_t *dir, md_handle_t *j, fc_command_t *cmd, bool failed)
{
    (void)fc_command_release(cmd);
    j->waiting = false;

    if (failed)
    {
        /* 同一块连续失败计数；达上限即标坏并清回收位（RISK-MD-01）。
         * 否则该块会被无限重试——数据安全但每次后台白付一条擦除命令，
         * 且坏块永远不会进入 BBT。 */
        if (j->reclaim_fail_gblock != j->reclaim_gblock)
        {
            j->reclaim_fail_gblock = j->reclaim_gblock;
            j->reclaim_fail_count  = 0U;
        }
        j->reclaim_fail_count++;

        if (j->reclaim_fail_count >= MD_RECLAIM_FAIL_MAX)
        {
            LOGE(LOG_TAG_META "reclaim erase failed blk=%u x%u — mark BAD\r\n", (unsigned)j->reclaim_gblock, (unsigned)j->reclaim_fail_count);
            (void)bbt_mark_bad(dir->alloc, j->reclaim_gblock);
            md_reclaim_clear(dir, j->reclaim_gblock);
            j->reclaim_fail_count  = 0U;
            j->reclaim_fail_gblock = UINT16_MAX;
        }
        else
        {
            LOGW(LOG_TAG_META "reclaim erase failed blk=%u (%u/%u) — 位保留，下轮重试\r\n",
                 (unsigned)j->reclaim_gblock,
                 (unsigned)j->reclaim_fail_count,
                 MD_RECLAIM_FAIL_MAX);
        }
    }
    else
    {
        j->reclaim_fail_count  = 0U;
        j->reclaim_fail_gblock = UINT16_MAX;
        md_reclaim_clear(dir, j->reclaim_gblock);
        md_note_block_erased(dir, j->reclaim_gblock);
        j->reclaimed_count++; /* 单调进度：区分「在回收」与「卡死」 */
    }

    j->reclaim_gblock = UINT16_MAX;
    j->phase          = MD_JOB_IDLE;
}

/* 整理 CP_READ 完成：成功把 region 整页拷进 job.cp_buf（**必须早于 release**）；
 * 读不回 ⇒ 中止——该 region 仍指向 victim 旧页，victim 不擦 ⇒ 无损失。 */
static void md_complete_cp_read(md_dir_cache_t *dir, md_handle_t *j, fc_command_t *cmd, bool failed)
{
    if (!failed)
    {
        (void)ftl_cmd_read_data(cmd, j->cp_buf, L2P_REGION_SIZE);
    }
    (void)fc_command_release(cmd);
    j->waiting = false;

    if (failed)
    {
        md_handle_abort(dir);
        return;
    }
    j->phase = MD_JOB_CP_WRITE;
}

/* 整理 CP_WRITE 完成：成功登记新位置；失败 ⇒ 槽已消耗（可能半编程）⇒ **不登记**，
 * region 仍指向 victim 旧页 ⇒ 无损失。 */
static void md_complete_cp_write(md_dir_cache_t *dir, md_handle_t *j, fc_command_t *cmd, bool failed)
{
    (void)fc_command_release(cmd);
    j->waiting = false;

    if (failed)
    {
        md_handle_abort(dir);
        return;
    }

    md_commit_region_copy(dir, j->cur_region, j->cur_copy, j->slot);
    j->slot  = NAND_PBA_INVALID;
    j->moved = (j->moved + 1U);
    j->phase = MD_JOB_CP_READ;
}

/* Anchor 换块擦完成：失败 ⇒ 不写、**不推进游标**，dirty 保留 ⇒ 下次在新槽重试整条。 */
static void md_complete_anchor_erase(md_dir_cache_t *dir, md_handle_t *j, fc_command_t *cmd, bool failed)
{
    (void)fc_command_release(cmd);
    j->waiting = false;

    if (failed)
    {
        md_handle_abort(dir);
        return;
    }
    j->phase = MD_JOB_CP_ANCHOR_WRITE;
}

/* Anchor 一页写完：无论成败都往后写下一页；失败 ⇒ 本条 record 残缺 ⇒ 整条作废。 */
static void md_complete_anchor_write(md_handle_t *j, fc_command_t *cmd, bool failed)
{
    (void)fc_command_release(cmd);
    j->waiting = false;

    if (failed)
    {
        j->anchor_ok = false; /* 整条作废（不取半条）*/
    }
    j->anchor_chunk = (j->anchor_chunk + 1U); /* 无论成败都往后写下一页 */
}

/* Victim 擦完成：失败 ⇒ victim 已无引用（Anchor 已落盘）⇒ 登记给回收阶段重试。 */
static void md_complete_cp_reclaim(md_dir_cache_t *dir, md_handle_t *j, fc_command_t *cmd, bool failed)
{
    (void)fc_command_release(cmd);
    j->waiting = false;

    if (failed)
    {
        LOGW(LOG_TAG_META "compact: victim erase failed blk=%u — 登记重试\r\n", (unsigned)j->victim);
        (void)md_reclaim_mark(dir, j->victim);
    }
    else
    {
        md_note_block_erased(dir, j->victim);
    }

    j->phase = MD_JOB_DONE;
}

/* --------------------------------------------------------------------
 * ftl_metadata_callback
 *   功能：日志块整理命令的完成服务函数——按 job.phase 分派到 md_complete_* 记结果。
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `md_dir_cache_t *`（模块上下文）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：**本函数不推进状态机**（见本文件开头的说明）：只把 phase 改到"下一步该
 *             干什么"，由下一轮 md_handle 续跑。
 * -------------------------------------------------------------------- */
void ftl_metadata_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    /* 形参由完成派发签名固定；release 下 ASSERT 裁空 ⇒ 显式丢弃（H30）。 */
    (void)ftl;
    md_dir_cache_t *dir = (md_dir_cache_t *)ctx;
    md_handle_t    *j   = (dir != NULL) ? &dir->handle : NULL;

    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);

    /* 实例由 ctx 反推；ftl 形参是 dispatch 解析出的归属实例，二者必须一致
     * ——不一致说明派发路由错了。 */
    ASSERT(ftl->metadata_dir == dir);

    const bool failed = fc_cw_status_has_error(completion->status);

    switch ((md_handle_phase_e)j->phase)
    {
        case MD_JOB_RECLAIM:
            md_complete_reclaim(dir, j, cmd, failed);
            return;

        case MD_JOB_CP_READ:
            md_complete_cp_read(dir, j, cmd, failed);
            return;

        case MD_JOB_CP_WRITE:
            md_complete_cp_write(dir, j, cmd, failed);
            return;

        case MD_JOB_CP_ANCHOR_ERASE:
            md_complete_anchor_erase(dir, j, cmd, failed);
            return;

        case MD_JOB_CP_ANCHOR_WRITE:
            md_complete_anchor_write(j, cmd, failed);
            return;

        case MD_JOB_CP_RECLAIM:
            md_complete_cp_reclaim(dir, j, cmd, failed);
            return;

        case MD_JOB_IDLE:
        case MD_JOB_CP_ANCHOR_PREP:
        case MD_JOB_DONE:
        default:
            /* 不该发生：一条命令对应一个提交阶段（RECLAIM / CP_READ / CP_WRITE /
             * CP_ANCHOR_ERASE / CP_ANCHOR_WRITE / CP_RECLAIM）。 */
            LOGE(LOG_TAG_META "md job unexpected completion phase=%u\r\n", (unsigned)j->phase);
            (void)fc_command_release(cmd);
            j->waiting = false;
            md_handle_abort(dir);
            return;
    }
}
