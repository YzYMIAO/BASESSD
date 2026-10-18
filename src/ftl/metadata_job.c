/*
 * metadata_job.c — 元数据目录整理 job：日志块回收 + 抗碎片压缩
 *
 * 与 metadata.c 的分工：
 *   metadata.c     —— Directory 结构、Anchor 落盘/加载、region 追加/登记（同步原语）；
 *   metadata_job.c —— 两件"要碰 NAND 的整理活"的异步状态机：
 *                     ① 回收：待回收位图（reclaim_pending）里的块 ⇒ 擦 + 归还 Free Pool；
 *                     ② 压缩：挑活 region 最少的 victim，搬进当前块，落 Anchor 后擦除。
 *   共享的内部原语（slot_now / job_reset / 待回收位图）在 include/ftl/metadata_job.h。
 *
 * 设计约束（与 metadata.h 的 md_job_t 长注释配套）：
 *   · **一次只提交一条命令**：提交阶段设 waiting 就返回；纯 RAM 阶段连续推进到下一个
 *     提交阶段或终态。推进一律由 metadata_dir_kick 驱动（服务函数不推进，见下）。
 *   · **服务函数不推进状态机**（同 ftl_op_checkpoint）：CP_ANCHOR_PREP 要调
 *     metadata_dir_anchor_prepare，而它在游标未建立时会**同步**扫池定位
 *     （metadata_ring_locate → ftl_sync_read_page）——在完成泵里跑就是嵌套泵
 *     （l2p_sync_pump 首行有 ASSERT 挡）。故 ftl_op_metadata 只记结果 + release + 改
 *     phase，下一轮 kick 接着走。
 *   · **提交失败（背压）保留现场**：压缩搬移的槽位取到后即消耗（写指针已推进），
 *     故 `job.slot` 在提交失败时必须留着，下轮重试**同一个**槽——绝不能重取
 *     （重取会白烧一页）；待回收位在擦除在飞/失败期间也保持置位。
 */

#include "metadata_job.h"

#include "assertion.h"
#include "fc.h"
#include "ftl.h" /* ftl_context_t（服务函数要从实例取 &ftl->alloc） */
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_METADATA */
#include "log.h"
#include "onfi_1_0.h"

/* 连续"提交不出去"的轮数上限（同 CP/MIG/RR_SUBMIT_STALL_MAX）。 */
#define MD_SUBMIT_STALL_MAX (256U)

/* md_block_addr — gblock → 该块第 0 页的 PBA（ch/ce/lun 取本 Directory 拓扑）。
 * ⚠ 单一转换漏斗（C9）：block/plane 的字段序只在这里出现一次，调用点不手写 pba_make。 */
static nand_pba_t md_block_addr(const metadata_dir_cache_t *dir, uint16_t gblock)
{
    return pba_make(dir->topo.ch,
                    dir->topo.ce,
                    dir->topo.lun,
                    (uint32_t)ftl_gblock_block(gblock),
                    (uint32_t)ftl_gblock_plane(gblock),
                    0U,
                    0U);
}

/* --------------------------------------------------------------------
 * metadata_dir_compact_victim
 *   功能：选压缩 victim —— 活 region 数**最少**（但 > 0）的非当前日志块。
 *   参数：dir —— Directory；alloc —— 分配器（读 BST 判 LOG 身份）。
 *   返回：victim 的 gblock；无候选返回 UINT16_MAX。
 *   实现要点：零活块**不在此列**——它们已登记在 reclaim_pending，由 RECLAIM 阶段
 *             直接擦除归还，无需搬移。平手取块号最小者：结果确定（跨复位可复现），
 *             且先分配的块通常收的是更早、更可能变冷的数据。
 * -------------------------------------------------------------------- */
static uint16_t metadata_dir_compact_victim(metadata_dir_cache_t *dir,
                                            ftl_allocator_t      *alloc)
{
    uint16_t best      = UINT16_MAX;
    uint16_t best_live = UINT16_MAX;

    /* 死守卫（§十附判据 2）：本文件内部调用者都持 dir/alloc。 */

    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const uint16_t live = dir->l2p_live[g];

        if (live == 0U)
        {
            continue; /* 零活块：reclaim 的活，直接擦，不必搬 */
        }
        if ((dir->cur_block_valid != 0U) && (g == dir->cur_metadata_block))
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

/* 中止本次整理：清请求与游标，回 IDLE。
 * ⚠ **victim 不擦**（已登记的 region 已指向新页、未搬的仍指向 victim ⇒ 两边都有效），
 *   下次换块时重来即可——与旧同步实现的"失败不回滚"逐条一致。
 * ⚠ 若 victim 已被搬空（live == 0），登记进待回收位图保持旧 sweep 的覆盖范围。
 *   "abort 后 Anchor 未落盘就擦 victim"是**既有**风险（旧 sweep 同样会擦），本轮不改。 */
static void md_job_abort(metadata_dir_cache_t *dir)
{
    md_job_t *j = &dir->job;

    LOGW("[META] md job abort phase=%u victim=%u moved=%u (victim 不擦)\r\n",
         (unsigned)j->phase,
         (unsigned)j->victim,
         (unsigned)j->moved);

    if ((j->victim != UINT16_MAX) && (dir->l2p_live[j->victim] == 0U))
    {
        (void)md_reclaim_mark(dir, j->victim);
    }

    md_job_reset(dir);
}

/* 累计一次「提交不出去」；超限即中止 job（同 CP/MIG 同族）。返回 true = 已中止。 */
static bool md_stall(metadata_dir_cache_t *dir)
{
    md_job_t *j = &dir->job;

    if (j->stall_rounds >= MD_SUBMIT_STALL_MAX)
    {
        md_job_abort(dir);
        return true;
    }
    j->stall_rounds++;
    return false;
}

/* 提交一条异步写（搬移页 / Anchor 分页共用）。data 在 ftl_async_write_page 内已拷进
 * FC buffer ⇒ 调用者的缓冲随即可复用。 */
static ftl_status_t md_submit_write(metadata_dir_cache_t *dir, nand_pba_t addr, const void *data, uint32_t length)
{
    fc_command_t *cmd;

    if (ftl_async_write_page(&cmd, addr, data, length, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN; /* 命令池空 / 地址非法 */
    }

    md_job_t *j = &dir->job;

    j->waiting = 1U; /* 先置位、提交失败再回滚（同 cp_submit_write） */

    if (ftl_op_submit(cmd, FTL_OP_METADATA, j) != FTL_OK)
    {
        j->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    j->stall_rounds = 0U; /* ⚠ 提交**成功之后**才清零（同 mig/cp） */
    return FTL_OK;
}

/* 提交一条**镜像**异步写（固定元数据池：同一页用 multi-plane 写两个 plane）。
 * 只用于 Anchor/CP/BBT 的元数据页；L2P region 搬移仍走单 plane 的 md_submit_write。 */
static ftl_status_t md_submit_write_mirror(metadata_dir_cache_t *dir, nand_pba_t addr, const void *data, uint32_t length)
{
    fc_command_t *cmd;

    if (ftl_async_write_page_mirror(&cmd, addr, data, length, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    md_job_t *j = &dir->job;

    j->waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_METADATA, j) != FTL_OK)
    {
        j->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    j->stall_rounds = 0U;
    return FTL_OK;
}

/* 提交一条异步擦除（日志块回收 / Anchor 换块 / victim 擦除共用）。 */
static ftl_status_t md_submit_erase(metadata_dir_cache_t *dir, nand_pba_t addr)
{
    fc_command_t *cmd;

    if (ftl_async_erase_block(&cmd, addr) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    md_job_t *j = &dir->job;

    j->waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_METADATA, j) != FTL_OK)
    {
        j->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    j->stall_rounds = 0U;
    return FTL_OK;
}

/* 提交一条**镜像**异步擦除（固定元数据池的换块：两个 plane 一起擦）。 */
static ftl_status_t md_submit_erase_multi(metadata_dir_cache_t *dir, nand_pba_t addr)
{
    fc_command_t *cmd;

    if (ftl_async_erase_block_multi_plane(&cmd, addr) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    md_job_t *j = &dir->job;

    j->waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_METADATA, j) != FTL_OK)
    {
        j->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    j->stall_rounds = 0U;
    return FTL_OK;
}

/* 提交一条异步读（搬移 victim 的 region 页 → job.cp_buf）。
 * ⚠ 读的数据**留在 desc 链上** ⇒ 目标缓冲必须活到完成，ftl_cmd_read_data 必须早于
 *   release —— 二者都在 ftl_op_metadata 里，缓冲就是 job.cp_buf（job 常驻 ⇒ 稳定）。 */
static ftl_status_t md_submit_read(metadata_dir_cache_t *dir, nand_pba_t addr)
{
    fc_command_t *cmd;

    if (ftl_async_read_page(&cmd, addr, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    md_job_t *j = &dir->job;

    j->waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_METADATA, j) != FTL_OK)
    {
        j->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    j->stall_rounds = 0U;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * md_compact_begin
 *   功能：选 victim、核空槽预算、把 job 推进到 CP_READ（纯 RAM，不碰介质）。
 *   参数：dir / alloc。
 *   返回：无（起不动就留在 IDLE）。
 *   实现要点：**无论成败都先清 compact_wanted**——它由 append_slot 置位而 append_slot
 *             又对 `phase != IDLE` 让路，不清就永远起不来（死锁）。
 *             空槽预算在此**重算**（不在 append_slot 置位时算）：置位到本函数之间可能
 *             又发放过若干槽位（此时 phase 还是 IDLE），按当下 cur_metadata_page 核才准。
 * -------------------------------------------------------------------- */
static void md_compact_begin(metadata_dir_cache_t *dir, ftl_allocator_t *alloc)
{
    md_job_t *j = &dir->job;

    j->compact_wanted = 0U;

    /* alloc 是死守卫（本文件内部调用者持 alloc）；cur_block_valid 是运行时状态，保留。 */
    if (dir->cur_block_valid == 0U)
    {
        return; /* 无当前写块：无处可搬 */
    }

    uint16_t victim = metadata_dir_compact_victim(dir, alloc);
    if (victim == UINT16_MAX)
    {
        return; /* 无候选：全池零活（reclaim 已管）或只剩当前块 */
    }

    /* 空槽预算：victim 的全部活 region 必须一次装进当前块（l2p_live 的定义就是
     * "最新位置落在该块的 region 数" ⇒ 预算一次核得准）。装不下 ⇒ 留到换块后再来。 */
    if (dir->l2p_live[victim] > (uint16_t)(PAGES_PER_BLOCK - dir->cur_metadata_page))
    {
        return;
    }

    j->victim       = victim;
    j->region_id    = 0U;
    j->cur_region   = 0U;
    j->moved        = 0U;
    j->slot         = NAND_PBA_INVALID;
    j->anchor_ok    = 0U;
    j->stall_rounds = 0U;
    j->phase        = (uint8_t)MD_JOB_CP_READ;

    LOGD("[META] compact start victim=%u live=%u cur_page=%u\r\n",
         (unsigned)victim,
         (unsigned)dir->l2p_live[victim],
         (unsigned)dir->cur_metadata_page);
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
static md_compact_step_t md_compact_read(metadata_dir_cache_t *dir)
{
    md_job_t *j = &dir->job;

    while (j->region_id < METADATA_DIR_ENTRIES)
    {
        const uint16_t rid = j->region_id;

        j->region_id = (uint16_t)(rid + 1U); /* 游标先走：本轮不命中也不回头 */

        nand_pba_t old = metadata_dir_lookup(dir, METADATA_TYPE_L2P_REGION, rid);
        if (old == NAND_PBA_INVALID)
        {
            continue;
        }
        if (ftl_pba_gblock(old) != j->victim)
        {
            continue; /* 该 region 的最新位置不在 victim 上 */
        }

        j->cur_region = rid;

        if (md_submit_read(dir, old) != FTL_OK)
        {
            j->region_id = rid; /* 退回：下轮重试同一 region */
            if (md_stall(dir))
            {
                return MD_COMPACT_FAILED;
            }
            return MD_COMPACT_BACKPRESSURE;
        }
        return MD_COMPACT_SUBMITTED;
    }

    return MD_COMPACT_SCANNED; /* 扫描完毕 */
}

/* --------------------------------------------------------------------
 * md_compact_write
 *   功能：把 job.cp_buf 写进当前块的新槽（槽只取一次，提交失败重试同一槽）。
 *   返回：同 md_compact_read。
 * -------------------------------------------------------------------- */
static md_compact_step_t md_compact_write(metadata_dir_cache_t *dir)
{
    md_job_t *j = &dir->job;

    if (j->slot == NAND_PBA_INVALID)
    {
        j->slot = metadata_dir_slot_now(dir);
        if (j->slot == NAND_PBA_INVALID)
        {
            /* 当前块无空槽 ⇒ 空槽预算失准（不该发生：预算一次核得准且期间无人发放槽位） */
            LOGW("[META] compact: 当前块无空槽 (page=%u) — 中止\r\n",
                 (unsigned)dir->cur_metadata_page);
            return MD_COMPACT_FAILED;
        }
    }

    if (md_submit_write(dir, j->slot, j->cp_buf, (uint32_t)L2P_REGION_SIZE) != FTL_OK)
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
 * metadata_dir_kick
 *   功能：推进整理 job 一步（每轮服务循环调一次）。见 metadata.h 的同名注释。
 *   参数：dir / alloc。
 *   返回：无。
 *   实现要点：`for (;;)` 里每个分支要么设 waiting 后 return（提交阶段），要么推进 phase
 *             后 break（纯 RAM 阶段）。**不允许既不推进也不返回**——那会死循环。
 * -------------------------------------------------------------------- */
void metadata_dir_kick(metadata_dir_cache_t *dir, ftl_allocator_t *alloc)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir 与 &ftl->alloc。 */
    ASSERT(dir != NULL);
    ASSERT(alloc != NULL);

    /* 心跳（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/age。
     * 默认档（INFO）不打印；需要现场观测时 `log level debug`。 */
    if (dir->job.phase == (uint8_t)MD_JOB_IDLE)
    {
        dir->job.log_ms   = 0U;
        dir->job.start_ms = 0U;
    }
    else if (ftl_job_heartbeat(&dir->job.log_ms, &dir->job.start_ms))
    {
        LOGD("[MD] job phase=%u victim=%u moved=%u pending=%u stall=%u age=%lums\r\n",
             (unsigned)dir->job.phase,
             (unsigned)dir->job.victim,
             (unsigned)dir->job.moved,
             (unsigned)dir->reclaim_pending_count,
             (unsigned)dir->job.stall_rounds,
             (unsigned long)ftl_job_age_ms(&dir->job.start_ms));
    }

    for (;;)
    {
        md_job_t *j = &dir->job;

        if (j->waiting != 0U)
        {
            return; /* 有命令在飞：等完成服务函数 */
        }

        switch ((md_job_phase_t)j->phase)
        {
            case MD_JOB_IDLE:
            {
                if (dir->reclaim_pending_count != 0U)
                {
                    j->phase = (uint8_t)MD_JOB_RECLAIM;
                    break;
                }
                if (j->compact_wanted == 0U)
                {
                    return; /* 无事可做 */
                }
                md_compact_begin(dir, alloc);
                if (j->phase == (uint8_t)MD_JOB_IDLE)
                {
                    return; /* 起不动（无候选 / 空槽不够）——compact_wanted 已被清 */
                }
                break;
            }

            case MD_JOB_RECLAIM:
            {
                uint16_t g = 0U;

                if (!md_reclaim_take(dir, &g))
                {
                    /* 计数非 0 但位图全空：计数与位图不一致（不该发生）。
                     * 清计数回 IDLE，避免每轮空转。 */
                    ASSERT(dir->reclaim_pending_count == 0U);
                    dir->reclaim_pending_count = 0U;
                    j->phase                   = (uint8_t)MD_JOB_IDLE;
                    break;
                }

                /* 消费前**重查**（登记到消费之间可能隔了很多轮，块号可能已被重新分配）：
                 *   ① 活计数非 0 ⇒ 该块又被引用了；
                 *   ② 成了当前写块 ⇒ 正在被追加；
                 *   ③ 不再带 LOG ⇒ 已被别处归还/复用。
                 * 任一命中即放弃本次（清位），**绝不擦活块**。 */
                if ((dir->l2p_live[g] != 0U) ||
                    ((dir->cur_block_valid != 0U) && (g == dir->cur_metadata_block)) ||
                    (ftl_block_state(&alloc->blocks[g]) != FTL_BLOCK_STATE_LOG))
                {
                    md_reclaim_clear(dir, g);
                    j->phase = (uint8_t)MD_JOB_IDLE;
                    break;
                }

                if (md_submit_erase(dir, md_block_addr(dir, g)) != FTL_OK)
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
                const md_compact_step_t r = md_compact_read(dir);

                if (r == MD_COMPACT_FAILED)
                {
                    md_job_abort(dir);
                    break;
                }
                if (r != MD_COMPACT_SCANNED)
                {
                    return; /* 已提交 / 背压 */
                }
                j->phase = (uint8_t)MD_JOB_CP_ANCHOR_PREP;
                break;
            }

            case MD_JOB_CP_WRITE:
            {
                /* md_compact_write 只返回 MD_COMPACT_SUBMITTED / MD_COMPACT_BACKPRESSURE /
                 * MD_COMPACT_FAILED，**不会**返回 MD_COMPACT_SCANNED（"扫完"）
                 * ——写一条就回去读下一条。 */
                const md_compact_step_t r = md_compact_write(dir);

                if (r == MD_COMPACT_FAILED)
                {
                    md_job_abort(dir);
                    break;
                }
                return;
            }

            case MD_JOB_CP_ANCHOR_PREP:
            {
                /* ③ Anchor：定位游标 + 取槽。游标未建立时会**同步**扫池定位——本 job
                 *   唯一可能瞬时阻塞的点，且只在冷启动后首次整理出现。 */
                int              need_erase = 0;
                md_anchor_step_t r          = metadata_dir_anchor_prepare(dir,
                                                                 &j->anchor_ring,
                                                                 &j->anchor_base,
                                                                 &j->anchor_idx,
                                                                 &j->anchor_seq,
                                                                 &need_erase);

                if (r == MD_ANCHOR_FAILED)
                {
                    md_job_abort(dir);
                    break;
                }
                if (r == MD_ANCHOR_CLEAN)
                {
                    /* Directory 无变更（本次没搬成任何 region）：跳过 Anchor 直接擦 victim。 */
                    j->phase = (uint8_t)MD_JOB_CP_RECLAIM;
                    break;
                }

                j->anchor_need_erase = (uint8_t)need_erase;
                j->anchor_chunk      = 0U;
                j->anchor_ok         = 1U;
                j->phase             = (need_erase != 0) ? (uint8_t)MD_JOB_CP_ANCHOR_ERASE : (uint8_t)MD_JOB_CP_ANCHOR_WRITE;
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
                    metadata_dir_anchor_commit(dir, &j->anchor_ring, j->anchor_idx, (int)j->anchor_ok);

                    if (j->anchor_ok == 0U)
                    {
                        LOGW("[META] anchor write incomplete: idx=%u seq=%lu (chunk 写失败)\r\n",
                             (unsigned)j->anchor_idx,
                             (unsigned long)j->anchor_seq);
                        md_job_abort(dir);
                        break;
                    }
                    j->phase = (uint8_t)MD_JOB_CP_RECLAIM;
                    break;
                }

                nand_pba_t aaddr = pba_with_page(j->anchor_base, (uint32_t)pba_to_page(j->anchor_base) + j->anchor_chunk);

                metadata_anchor_page_t page; /* 2KB 栈缓冲：按页组好后写入 */
                metadata_dir_anchor_page(dir, j->anchor_seq, j->anchor_chunk, &page);

                if (md_submit_write_mirror(dir, aaddr, &page, (uint32_t)sizeof(page)) != FTL_OK)
                {
                    (void)md_stall(dir);
                }
                return;
            }

            case MD_JOB_CP_RECLAIM:
            {
                const uint16_t v = j->victim;

                /* ④ 擦 victim —— 只在它**确实已无引用**时（③ Anchor 已落盘）。 */
                if (v == UINT16_MAX)
                {
                    j->phase = (uint8_t)MD_JOB_DONE;
                    break;
                }
                if ((dir->l2p_live[v] != 0U) ||
                    ((dir->cur_block_valid != 0U) && (v == dir->cur_metadata_block)) ||
                    (ftl_block_state(&alloc->blocks[v]) != FTL_BLOCK_STATE_LOG))
                {
                    /* 扫描期间又有 region 落回 victim（在飞的写回完成）⇒ 不能擦，留给下次。
                     * 这与旧实现末尾的 `l2p_live[victim] == 0` 判据同一件事。 */
                    LOGD("[META] compact: victim %u 仍有 %u 活 region — 不擦\r\n",
                         (unsigned)v,
                         (unsigned)dir->l2p_live[v]);
                    j->phase = (uint8_t)MD_JOB_DONE;
                    break;
                }

                if (md_submit_erase(dir, md_block_addr(dir, v)) != FTL_OK)
                {
                    (void)md_stall(dir);
                }
                return;
            }

            case MD_JOB_DONE:
                LOGD("[META] compact done victim=%u moved=%u\r\n",
                     (unsigned)j->victim,
                     (unsigned)j->moved);
                md_job_reset(dir);
                break; /* 回 IDLE：继续循环（可能还有待回收的块）*/

            default:
                /* 不该发生：phase 越界 ⇒ 清回 IDLE，否则 append_slot 永久让路。 */
                LOGE("[META] md job bad phase=%u — reset\r\n", (unsigned)j->phase);
                md_job_reset(dir);
                return;
        }
    }
}

/* ── 整理 job 的完成服务（g_op_service[FTL_OP_METADATA]）── */

/* --------------------------------------------------------------------
 * ftl_op_metadata
 *   功能：日志块整理命令的完成服务函数——按 job.phase 记结果、改 phase、release。
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `md_job_t *`（= ftl->metadata_dir->job）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：**本函数不推进状态机**（见本文件开头的说明）：只把 phase 改到"下一步该
 *             干什么"，由下一轮 metadata_dir_kick 续跑。
 *             ⚠ CP_READ 的 ftl_cmd_read_data **必须早于** release（数据在 desc 链上）。
 * -------------------------------------------------------------------- */
void ftl_op_metadata(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    md_job_t *j = (md_job_t *)ctx;

    ASSERT(completion != NULL);
    ASSERT(j != NULL);

    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    /* 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出的归属实例，
     * 二者必须一致——不一致说明派发路由错了。 */
    ASSERT(ftl != NULL);
    ASSERT(&ftl->metadata_dir->job == j);

    metadata_dir_cache_t *dir    = ftl->metadata_dir;
    int                   failed = ((completion->status & FC_CW_STATUS_ERROR_MASK) != 0U) ? 1 : 0;

    switch ((md_job_phase_t)j->phase)
    {
        case MD_JOB_RECLAIM:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* 擦失败：位保持置位 ⇒ 下一轮 kick 重新取到它并重试。
                 * （旧实现依赖 append_slot 换块时的同步 sweep 兜底；该 sweep 已删除。） */
                LOGW("[META] reclaim erase failed blk=%u — 位保留，下轮重试\r\n",
                     (unsigned)j->reclaim_gblock);
            }
            else
            {
                md_reclaim_clear(dir, j->reclaim_gblock);
                /* 元数据日志块不经 migrate 擦除路径，磨损计数在这里补记。 */
                allocator_note_erase(&ftl->alloc, j->reclaim_gblock, ftl->total_writes);
                allocator_free_block(&ftl->alloc, j->reclaim_gblock); /* 已擦除 ⇒ 归还 Free Pool */
            }

            j->reclaim_gblock = UINT16_MAX;
            j->phase          = (uint8_t)MD_JOB_IDLE;
            return;

        case MD_JOB_CP_READ:
            if (failed == 0)
            {
                /* 数据在 desc 链上 ⇒ 必须早于 release 拷出 */
                (void)ftl_cmd_read_data(cmd, j->cp_buf, (uint32_t)L2P_REGION_SIZE);
            }
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* 读不回：该 region 仍指向 victim 旧页，victim 不擦 ⇒ 无损失。 */
                md_job_abort(dir);
                return;
            }
            j->phase = (uint8_t)MD_JOB_CP_WRITE;
            return;

        case MD_JOB_CP_WRITE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* 槽已消耗（可能半编程）⇒ 不登记；region 仍指向 victim 旧页 ⇒ 无损失。 */
                md_job_abort(dir);
                return;
            }

            metadata_dir_commit_region(dir, &ftl->alloc, j->cur_region, j->slot);
            j->slot  = NAND_PBA_INVALID;
            j->moved = (uint16_t)(j->moved + 1U);
            j->phase = (uint8_t)MD_JOB_CP_READ;
            return;

        case MD_JOB_CP_ANCHOR_ERASE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* 换块擦失败 ⇒ 不写、**不推进游标**（同 metadata_ring_prepare_write 的
                 * 擦失败即返回错误），dirty 保留 ⇒ 下次在新槽重试整条。 */
                md_job_abort(dir);
                return;
            }
            j->phase = (uint8_t)MD_JOB_CP_ANCHOR_WRITE;
            return;

        case MD_JOB_CP_ANCHOR_WRITE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                j->anchor_ok = 0U; /* 本条 record 残缺 ⇒ 整条作废（不取半条）*/
            }
            j->anchor_chunk = (uint8_t)(j->anchor_chunk + 1U); /* 无论成败都往后写下一页 */
            return;

        case MD_JOB_CP_RECLAIM:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* 擦失败：victim 已无引用（Anchor 已落盘）⇒ 登记给回收阶段重试。 */
                LOGW("[META] compact: victim erase failed blk=%u — 登记重试\r\n",
                     (unsigned)j->victim);
                (void)md_reclaim_mark(dir, j->victim);
            }
            else
            {
                /* 元数据日志块不经 migrate 擦除路径，磨损计数在这里补记。 */
                allocator_note_erase(&ftl->alloc, j->victim, ftl->total_writes);
                allocator_free_block(&ftl->alloc, j->victim); /* 已擦除 ⇒ 归还 Free Pool */
            }

            j->phase = (uint8_t)MD_JOB_DONE;
            return;

        case MD_JOB_IDLE:
        case MD_JOB_CP_ANCHOR_PREP:
        case MD_JOB_DONE:
        default:
            /* 不该发生：一条命令对应一个提交阶段（RECLAIM / CP_READ / CP_WRITE /
             * CP_ANCHOR_ERASE / CP_ANCHOR_WRITE / CP_RECLAIM）。 */
            LOGE("[META] md job unexpected completion phase=%u\r\n", (unsigned)j->phase);
            (void)fc_command_release(cmd);
            j->waiting = 0U;
            md_job_abort(dir);
            return;
    }
}
