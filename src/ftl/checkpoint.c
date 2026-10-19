/*
 * checkpoint.c — L2P 全量快照（Checkpoint 池环形轮转 + 触发/水位）
 *
 * 职责：
 *   把 L2P 缓存里的脏 region 固化进 NAND L2P 日志块、flush Metadata Directory（Anchor）、
 *   再写一条 Checkpoint Header 记录「此刻 L2P 已落盘到哪」，最后截断 Journal；
 *   开机由 checkpoint_find_latest 在池内挑最新一条有效记录恢复游标/序号。
 *
 * 与 journal 的分工：
 *   Journal    —— L2P 的**增量**日志（每笔映射一条，掉电回放的最小单位）；
 *   Checkpoint —— L2P 的**全量**快照（把 journal 已写条目固化进 NAND L2P 表并截断 journal）。
 *   两者用各自的 sequence：Journal.sequence_counter 是全局单调；本池 ftl->cp.sequence 独立。
 *
 * 池几何（由 ftl/metadata.h 推导，见 checkpoint_ring_init）：
 *   CHECKPOINT_POOL_BLOCKS 个块、一条 record = 21 页、P0+P1 镜像；
 *   写游标 ftl->cp.write_idx 在 [0, 总记录数) 环形推进，块满由 metadata_ring_prepare_addr
 *   判定（同步站点当场擦；异步 job 提交异步擦，见 CP_HDR_ERASE）。
 *
 * 掉电安全顺序（不可倒，见下方"异步 job"的长注释）：
 *   先 flush region → 再 flush Anchor → 最后写 Header 声称「已落盘」。
 *   Header 是"承诺书"：它一旦落盘，boot 就信 bitmap 里置位的 region 在 NAND 上，
 *   故任何被 Header 声称的落盘必须**先于** Header 完成。
 *
 * 文件结构：
 *   1. 内部辅助        checkpoint_header_valid / checkpoint_record_probe
 *                      （单调毫秒 / 心跳基准见 ftl_job.h）
 *   2. 异步 job        cp_step（static）/ checkpoint_begin / checkpoint_kick / ftl_op_checkpoint
 *   3. 创建/恢复       checkpoint_find_latest / checkpoint_write_initial
 *   4. 触发/水位       checkpoint_check_trigger / checkpoint_is_emergency / checkpoint_journal_watermark
 *
 * 游标/序号是 **per-instance**（序 4：收进 ftl_context_t.cp，见 l2p_cp_state_t）——
 * 多实例（多 LUN）下每实例一份，互不干扰。
 */

#include "checkpoint.h"

#include "assertion.h"
#include "bbt.h"         /* bbt_medium_has_ftl_data（CP 缺失时的空片判定） */
#include "boot_policy.h" /* boot_crc32（块元数据快照跨页校验和） */
#include "fc.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* 心跳 + 单调毫秒（ftl_job_now_ms / ftl_job_heartbeat）*/
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_CHECKPOINT */
#include "log.h"
#include "metadata.h"
#include "onfi_1_0.h"

#include <string.h>

/* ── 内部辅助 ── */

/* ── Checkpoint 池：环形轮转（一条 record = 21 页、P0+P1 镜像）──
 *
 * 池几何由 ftl/metadata.h 推导；sequence 与写游标是**本池自己的**（与 journal /
 * Anchor / BBT 的 sequence 互不相干），开机由 checkpoint_find_latest 恢复。
 *
 * 游标/序号是 per-instance 的 ftl->cp（l2p_cp_state_t，序 4）——原为 file-scope
 * static，多实例下互相覆盖。 */

/* checkpoint_ring_init — 按 metadata.h 常量填充本池环形几何（块范围/记录页数/plane/module） */
static void checkpoint_ring_init(metadata_ring_t *ring)
{
    ring->first_block      = CHECKPOINT_BLOCK0;
    ring->block_count      = CHECKPOINT_POOL_BLOCKS;
    ring->pages_per_record = CHECKPOINT_RECORD_PAGES; /* Header(1) + 块元数据(4) + 磨损(16) */
    ring->plane_mask       = METADATA_MIRROR_PLANE_MASK;
}

/* Header 校验（record 的 commit 页）：magic + 布局版本。
 * 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证（ADR v0.62）。 */
static bool checkpoint_header_valid(const l2p_checkpoint_header_t *hdr)
{
    if (hdr->magic != CHECKPOINT_MAGIC)
    {
        return false;
    }
    if (hdr->layout_version != FTL_MEDIA_LAYOUT_VERSION)
    {
        return false; /* 旧格式介质的 Checkpoint ⇒ 作废（触发开机自动格式化）*/
    }

    return true;
}

typedef struct
{
    ftl_topology_t  topo;
    metadata_ring_t ring;
    bool            any_version; /* true = 只校验 magic（格式化判据探针），不校验 layout_version */
} checkpoint_probe_ctx_t;

/* Checkpoint record 探针（见 metadata_ring_probe_fn）。
 * full 与廉价探针等价（一条 record 只有一页），故忽略。 */
static ftl_status_t checkpoint_record_probe(void *ctx, uint16_t record_idx, bool full, uint32_t *out_sequence)
{
    checkpoint_probe_ctx_t *c = (checkpoint_probe_ctx_t *)ctx;

    (void)full;

    /* 镜像池：任一 plane 的副本有效即可。 */
    const uint8_t plane_count = metadata_ring_plane_count(c->ring.plane_mask);

    for (uint8_t pi = 0U; pi < plane_count; pi++)
    {
        const uint8_t           plane = metadata_ring_plane_at(c->ring.plane_mask, pi);
        nand_pba_t              addr  = metadata_ring_addr_plane(&c->ring, &c->topo, record_idx, 0U, plane);
        l2p_checkpoint_header_t hdr;

        if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((hdr.magic != CHECKPOINT_MAGIC) || (hdr.sequence == 0U))
        {
            continue; /* sequence 从 1 起；0 = 未写 */
        }
        if ((!c->any_version) && (hdr.layout_version != FTL_MEDIA_LAYOUT_VERSION))
        {
            continue; /* 正常定位只认当前版本；格式化判据探针放宽到任意版本 */
        }

        *out_sequence = hdr.sequence;
        return FTL_OK;
    }

    return FTL_ERROR;
}

/* ── 创建 / 恢复 ── */

/* ══════════════════════════════════════════════════════════════════
 * Checkpoint 异步 job（v0.75 阶段 6）
 *
 * checkpoint_create 原是**一整段同步流水线**：脏区落盘 → Anchor → Header → 截断，
 * 每一步都在调用者（ftl_service_*）的栈上阻塞等 NAND 回来。单次 CP 可阻塞数十毫秒
 * （脏 region 写回 + 8 页 Anchor + Header + 预擦），期间 host I/O 的完成泵停摆。
 *
 * 现在同一份**顺序契约**由 phase 承载：每个阶段提交一条命令就返回，完成由
 * g_op_service[FTL_OP_CHECKPOINT]（ftl_op_checkpoint）按 phase 收尾，下一轮
 * ftl_service 调 checkpoint_kick 续跑。
 *
 * ⚠ **顺序即掉电安全契约**（与旧实现逐条对应，不可倒）：
 *     ① 快照 region 脏标记 + journal 水位（**必须在任何 flush 之前**：flush 成功会清位）
 *     ② flush 脏 region（任一失败 ⇒ 中止，不写 Header）
 *     ③ flush Anchor（**必须早于 Header**）
 *     ④ 写 Header（**无论成败都推进池游标**：半编程槽不可复用）
 *     ⑤ 截断 Journal（预擦环形最旧块）+ 复位水位标志
 *
 * ⚠ **快照点前移（与旧同步实现的刻意差异）**：旧实现取 `journal.sequence_counter`
 *   是在 Anchor 之后（步骤⑤），异步 CP 跨多轮，若照旧取"CP 末尾"的水位，则在
 *   CP_ANCHOR 期间完成的 host 写会同时满足：① region 脏位在 RAM 里置了但没进本次
 *   快照（不会被 flush）；② 其 journal 条目 seq ≤ 末尾水位（boot 视为"已进 L2P 表"
 *   而跳过回放）⇒ **两边都不认，静默丢映射**。
 *   故水位与脏位图**一起在 job 起点取定**：此后任何 host 写都有 seq > 快照水位，
 *   boot 必然回放它。代价是 journal_truncate 的水位判据更容易不满足（更少预擦）——
 *   预擦只是磨损优化，跳过它只是让 journal_flush_page 自己擦（每块每圈仍只擦一次）。
 *
 * ⚠ **服务函数不推进状态机**（与 migrate 的 ftl_op_migrate 不同）：本 job 的
 *   CP_ANCHOR_PREP / CP_HDR_PREP 里有同步原语（游标未建立时要 metadata_ring_locate
 *   读池定位），在完成泵里跑就是嵌套泵（l2p_sync_pump 有 ASSERT 挡）。故
 *   ftl_op_checkpoint 只记结果 + release + 改 phase，推进一律留给下一轮 ftl_service。
 * ══════════════════════════════════════════════════════════════════ */

/* 中止本次 CP：记失败相位、转 CP_DONE（**不**写 Header / **不**截断 journal）。 */
static void cp_abort(cp_job_t *j)
{
    j->failed     = 1U;
    j->fail_phase = j->phase;
    j->phase      = (uint8_t)CP_DONE;
}

/* 累计一次「提交不出去」；超限即中止 job（与 MIG/RR 同族）。返回 true = 已中止。 */
static bool cp_stall(cp_job_t *j)
{
    if (j->stall_rounds >= CP_SUBMIT_STALL_MAX)
    {
        LOGW("[CP] stall %u rounds — abort phase=%u\r\n",
             (unsigned)j->stall_rounds,
             (unsigned)j->phase);
        cp_abort(j);
        return true;
    }
    j->stall_rounds++;
    return false;
}

/* 提交一条异步写（Anchor 分页 / Checkpoint Header 共用）。同步镜像到 P0+P1：
 * 同一页数据用 multi-plane 一次写两个 plane；data 在本函数内已拷进 FC buffer
 * ⇒ 调用者的栈缓冲随即可复用。 */
static ftl_status_t cp_submit_write(cp_job_t *j, nand_pba_t addr, const void *data, uint32_t length)
{
    fc_command_t *cmd;

    if (ftl_async_write_page_mirror(&cmd, addr, data, length, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN; /* 命令池空 / 地址非法 */
    }

    j->waiting = 1U; /* 先置位、提交失败再回滚（同 mig_submit_*） */

    if (ftl_op_submit(cmd, FTL_OP_CHECKPOINT, j) != FTL_OK)
    {
        j->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    /* ⚠ 卡死计数在**提交成功之后**才清零（见 mig_submit_read 的同款注释）。 */
    j->stall_rounds = 0U;
    return FTL_OK;
}

/* 提交一条异步擦除（Anchor 换块 / Checkpoint 换块 / Journal 预擦共用）。
 * 固定元数据池是 P0+P1 镜像 ⇒ 换块必须 multi-plane 擦，否则副本 plane 不会清。 */
static ftl_status_t cp_submit_erase(cp_job_t *j, nand_pba_t addr)
{
    fc_command_t *cmd;

    if (ftl_async_erase_block_multi_plane(&cmd, addr) != FTL_OK)
    {
        return FTL_AGAIN;
    }

    j->waiting = 1U;

    if (ftl_op_submit(cmd, FTL_OP_CHECKPOINT, j) != FTL_OK)
    {
        j->waiting = 0U;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    j->stall_rounds = 0U;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * cp_flush_regions
 *   功能：按**快照**脏位图把每个脏 region 落盘（阶段②）。返回 CP_FLUSH_DONE = 全部已持久；
 *         CP_FLUSH_PENDING = 仍有在飞（下轮再来）；CP_FLUSH_FAILED = 硬失败（调用者中止，不写 Header）。
 *   实现要点：**不按 state == DIRTY 过滤**——异步化后脏行可能已被后台 l2p_flush_kick
 *             起了写回（state = FLUSHING）而尚未完成，它同样属于"本周期改过、必须已持久"，
 *             漏掉它就会让 Header 承诺一个还没落盘的映射。FLUSHING 行**等**它（不重复起）。
 *             CLEAN 行 = 已落盘（commit 已清脏位，见 l2p_cache.c 的 ftl_op_l2p）。
 *             ⚠ 判据是**快照位图**而非实时位图：实时位图会被本次 flush 的 commit 清掉
 *             （那样循环就永远看不到自己该等的行）。
 * -------------------------------------------------------------------- */
static cp_flush_step_t cp_flush_regions(ftl_context_t *ftl, cp_job_t *j)
{
    const uint32_t dim     = ftl_topo_dim(ftl->topo);
    int            pending = 0;

    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &ftl->cache->lines[i];

        if (line->state == L2P_CACHE_INVALID)
        {
            continue; /* 空行（region_id 无意义，脏位查询无意义） */
        }
        if (!l2p_region_owned_by(line->region_id, dim))
        {
            continue; /* 别的 LUN 的行：由它自己的 Checkpoint 负责 */
        }

        const uint32_t local_region = l2p_global_to_local_region(line->region_id);
        ASSERT(local_region < L2P_REGIONS_PER_INSTANCE);

        uint32_t word = j->region_bitmap[local_region / 32U];
        uint32_t mask = (1UL << (local_region & 31U));
        if ((word & mask) == 0U)
        {
            continue; /* 本次 CP 起点时不脏（此后新脏的由 journal 兜底，见文件头） */
        }

        if (line->state == L2P_CACHE_CLEAN)
        {
            continue; /* 已落盘：本次 CP 期间由我们（或后台）写回成功，commit 已清脏位 */
        }
        if (line->state == L2P_CACHE_FLUSHING)
        {
            pending = 1; /* 已提交、未落盘：等它 */
            continue;
        }
        if (line->state == L2P_CACHE_DIRTY)
        {
            const ftl_status_t st = l2p_cache_flush_kick(ftl->cache, (int)i);

            if (st == FTL_ERROR)
            {
                return CP_FLUSH_FAILED; /* 无依赖 / 建链失败：中止（同旧 checkpoint_flush_dirty_regions） */
            }
            /* ⚠ **FTL_AGAIN 是背压不是失败**（阶段 8 起 l2p_cache_flush_kick 会把
             *   "取不到槽"报成 AGAIN：Free Pool 空 或 目录整理 job 在飞）。一次正常的
             *   后台压缩不该把 Checkpoint 打回去——下轮重试即可。 */
            pending = 1; /* OK = 已提交；AGAIN = 背压，都还没落盘 */
            continue;
        }

        /* 只剩 LOADING：脏位已置却 entries 尚未有效 —— 不变量被破坏（脏位只由
         * l2p_cache_write 在行可写时置位，而 LOADING 行不可写；commit 又只在
         * FLUSHING 行上清位）。**中止**而非放过：此刻既没在写回、也没有有效数据，
         * 若当作"已持久"写进 Header，boot 会按 bitmap 预载这一区 ⇒ **静默丢映射**。
         * 中止的代价只是本次 CP 不做（journal 与脏位全保留，下轮重来），方向安全。 */
        LOGW("[CP] region=%lu dirty but state=%u (LOADING) — abort\r\n",
             (unsigned long)line->region_id,
             (unsigned)line->state);
        return CP_FLUSH_FAILED;
    }

    if (pending == 0)
    {
        j->stall_rounds = 0U;
        return CP_FLUSH_DONE;
    }

    /* 有在飞写回 = 有进展（**不**计卡死轮数）：闸门满 / 提交失败都会让本轮起不动新行，
     * 但 flush_inflight > 0 说明完成迟早会来。共享 credit 池是全局的，别的 LUN 的在飞
     * 写回同样代表系统在推进。
     * ⚠ 同理，**目录整理 job 在飞也算有进展**（阶段 8）：它独占当前日志块的空槽，期间的
     *   脏 region 一律收到 FTL_AGAIN。一次正常的压缩要走上百轮命令，若照常累计就会撞
     *   CP_SUBMIT_STALL_MAX 把 Checkpoint 打回去。 */
    if ((ftl->cache->flush_inflight > 0U) || metadata_dir_busy(ftl->metadata_dir))
    {
        j->stall_rounds = 0U;
        return CP_FLUSH_PENDING;
    }

    return cp_stall(j) ? CP_FLUSH_FAILED : CP_FLUSH_PENDING;
}

/* --------------------------------------------------------------------
 * cp_step
 *   功能：状态机主体——一次只提交**一条**命令就返回；纯计算阶段连续推进到下一个
 *         提交阶段或终态。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 *   实现要点：阶段序列见 checkpoint.h 的 cp_phase_t。提交失败（背压）⇒ 原地返回，
 *             由 checkpoint_kick 下轮重试同一阶段（幂等）；连续 CP_SUBMIT_STALL_MAX
 *             轮无进展 ⇒ 中止（failed=1，走 CP_DONE 且**不**写 Header / 不截断）。
 * -------------------------------------------------------------------- */
static void cp_step(ftl_context_t *ftl)
{
    cp_job_t *j = ftl->cp_job;

    for (;;)
    {
        if (j->waiting != 0U)
        {
            return; /* 有命令在飞：等完成服务函数 */
        }

        switch ((cp_phase_t)j->phase)
        {
            case CP_FLUSH_REGIONS:
            {
                const cp_flush_step_t r = cp_flush_regions(ftl, j);

                if (r == CP_FLUSH_FAILED)
                {
                    cp_abort(j);
                    break;
                }
                if (r == CP_FLUSH_PENDING)
                {
                    return; /* 仍有脏行在飞 */
                }
                j->phase = (uint8_t)CP_ANCHOR_PREP;
                break;
            }

            case CP_ANCHOR_PREP:
            {
                /* ③ Anchor：定位游标 + 取槽（游标未建立时会**同步**扫池定位——
                 *    这是本 job 唯一可能瞬时阻塞的点，且只在冷启动后首次 CP 出现）。 */
                bool             need_erase = false;
                md_anchor_step_t r          = metadata_dir_anchor_prepare(ftl->metadata_dir,
                                                                 &j->anchor_ring,
                                                                 &j->anchor_base,
                                                                 &j->anchor_idx,
                                                                 &j->anchor_seq,
                                                                 &need_erase);

                if (r == MD_ANCHOR_FAILED)
                {
                    cp_abort(j);
                    break;
                }
                if (r == MD_ANCHOR_CLEAN)
                {
                    /* Directory 无变更：整段跳过（Anchor 每写一条都消耗池内槽位，
                     * 空转会让池无谓轮转）。旧实现是 metadata_dir_flush_to_anchor 的
                     * dirty==0 早退，语义相同。 */
                    j->anchor_skip = 1U;
                    j->phase       = (uint8_t)CP_HDR_PREP;
                    break;
                }

                j->anchor_need_erase = need_erase;
                j->anchor_chunk      = 0U;
                j->anchor_ok         = 1U;
                j->phase             = need_erase ? (uint8_t)CP_ANCHOR_ERASE : (uint8_t)CP_ANCHOR_WRITE;
                break;
            }

            case CP_ANCHOR_ERASE:
                if (cp_submit_erase(j, j->anchor_base) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;

            case CP_ANCHOR_WRITE:
            {
                if (j->anchor_chunk >= ANCHOR_PAGE_COUNT)
                {
                    /* 整条 record 写完：推进游标（无论成败），成功才清 dirty。
                     * 失败 ⇒ 整条残缺（locate 会整条丢弃）⇒ 中止，不写 Header。 */
                    metadata_dir_anchor_commit(ftl->metadata_dir, &j->anchor_ring, j->anchor_idx, (j->anchor_ok != 0U));

                    if (j->anchor_ok == 0U)
                    {
                        LOGW("[META] anchor write incomplete: idx=%u seq=%lu (chunk 写失败)\r\n",
                             (unsigned)j->anchor_idx,
                             (unsigned long)j->anchor_seq);
                        cp_abort(j);
                        break;
                    }
                    j->phase = (uint8_t)CP_DONE;
                    break;
                }

                nand_pba_t             aaddr = pba_with_page(j->anchor_base, pba_to_page(j->anchor_base) + j->anchor_chunk);
                metadata_anchor_page_t page; /* 2KB 栈缓冲：按页组好后写入 */

                metadata_dir_anchor_page(ftl->metadata_dir, j->anchor_seq, j->anchor_chunk, &page);

                if (cp_submit_write(j, aaddr, &page, (uint32_t)sizeof(page)) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;
            }

            case CP_HDR_PREP:
            {
                /* ④ Header：定位写游标（游标未建立时才扫池——开机 checkpoint_find_latest
                 *    已建立时零代价）。⚠ 这里可能同步读池（同 CP_ANCHOR_PREP）。 */
                checkpoint_ring_init(&j->hdr_ring);

                if (ftl->cp.cursor_valid == 0U)
                {
                    uint16_t               newest = UINT16_MAX;
                    checkpoint_probe_ctx_t probe_ctx;

                    probe_ctx.topo        = ftl->topo;
                    probe_ctx.ring        = j->hdr_ring;
                    probe_ctx.any_version = false;

                    if (metadata_ring_locate(&j->hdr_ring, checkpoint_record_probe, &probe_ctx, &newest, &ftl->cp.sequence, &ftl->cp.write_idx) != FTL_OK)
                    {
                        cp_abort(j);
                        break;
                    }
                    ftl->cp.cursor_valid = 1U;
                    /* 刚定位到的游标同样可能指向掉电残留的半编程槽 ⇒ 首次写入也跳一槽。 */
                    ftl->cp.advance_next = 1U;
                }

                j->hdr_idx = ftl->cp.write_idx;

                bool need_erase = false;
                if (metadata_ring_prepare_addr(&j->hdr_ring, &ftl->topo, j->hdr_idx, &j->hdr_addr, &need_erase) != FTL_OK)
                {
                    cp_abort(j);
                    break;
                }

                /* 本 session 首次 CP：跳过一槽。掉电可能留下「快照页已写、Header 未写」
                 * 的半编程槽，locate 会把它当下一可写槽。跳过后 re-prepare：
                 *   · 同块内 ⇒ 下一槽的 21 页从未被写，安全；
                 *   · 跨块   ⇒ need_erase=1，先擦目标块，同样安全。
                 * 只在 need_erase==0 时跳（块边界本来就会擦，不需要额外浪费）。 */
                if ((ftl->cp.advance_next != 0U) && (need_erase == 0))
                {
                    j->hdr_idx = (uint16_t)((j->hdr_idx + 1U) % metadata_ring_total_records(&j->hdr_ring));
                    if (metadata_ring_prepare_addr(&j->hdr_ring, &ftl->topo, j->hdr_idx, &j->hdr_addr, &need_erase) != FTL_OK)
                    {
                        cp_abort(j);
                        break;
                    }
                }
                ftl->cp.advance_next = 0U;

                /* sequence 在最终槽位确定后取（池内已有序号时才能续上），与旧序一致。 */
                j->hdr_seq = ++ftl->cp.sequence;

                j->block_page     = 0U;
                j->block_ok       = 1U;
                j->hdr_need_erase = (uint8_t)need_erase;
                j->phase          = (need_erase != 0) ? (uint8_t)CP_HDR_ERASE : (uint8_t)CP_BLOCK_WRITE;
                break;
            }

            case CP_HDR_ERASE:
                if (cp_submit_erase(j, j->hdr_addr) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;

            case CP_BLOCK_WRITE:
            {
                if (j->block_page >= CP_BLOCK_PAGES)
                {
                    if (j->block_ok == 0U)
                    {
                        /* 半编程槽：游标推进一槽，永不复用本条 record 的剩余页。 */
                        ftl->cp.write_idx = (uint16_t)((j->hdr_idx + 1U) %
                                                       metadata_ring_total_records(&j->hdr_ring));
                        cp_abort(j);
                        break;
                    }
                    j->wear_page = 0U;
                    j->phase     = (uint8_t)CP_WEAR_WRITE;
                    break;
                }

                nand_pba_t a = metadata_ring_addr(&j->hdr_ring, &ftl->topo, j->hdr_idx, (uint8_t)(1U + j->block_page));

                if (cp_submit_write(j, a, &j->block_words[j->block_page * CP_BLOCK_WORDS_PER_PAGE], PAGE_MAIN_SIZE) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;
            }

            case CP_WEAR_WRITE:
            {
                if (j->wear_page >= FTL_CHECKPOINT_WEAR_PAGES)
                {
                    if (j->block_ok == 0U)
                    {
                        ftl->cp.write_idx = (uint16_t)((j->hdr_idx + 1U) %
                                                       metadata_ring_total_records(&j->hdr_ring));
                        cp_abort(j);
                        break;
                    }
                    j->phase = (uint8_t)CP_HDR_WRITE;
                    break;
                }

                /* 磨损快照在**写到哪页时组哪页**（软状态；CP 期间的计数变化允许漏到下一 CP）。 */
                checkpoint_wear_page_build(&ftl->alloc, (uint16_t)j->wear_page, j->wear_page_buf);
                j->wear_page_crc[j->wear_page] = boot_crc32(j->wear_page_buf, PAGE_MAIN_SIZE);

                nand_pba_t a = metadata_ring_addr(&j->hdr_ring,
                                                  &ftl->topo,
                                                  j->hdr_idx,
                                                  (uint8_t)(1U + CP_BLOCK_PAGES + j->wear_page));

                if (cp_submit_write(j, a, j->wear_page_buf, PAGE_MAIN_SIZE) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;
            }

            case CP_HDR_WRITE:
            {
                l2p_checkpoint_header_t cp; /* 2KB 栈缓冲：恰好一页主区 */

                memset(&cp, 0, sizeof(cp));
                memcpy(cp.region_bitmap, j->region_bitmap, sizeof(cp.region_bitmap));

                /* region_count = 非零 word 数（无消费方，仅信息量）。快照是**本 die 的
                 * 局部 2048-bit 位图**，与 on-media 头的 64 word 尺寸一致。 */
                uint32_t region_words = 0U;

                for (uint8_t i = 0U; i < (uint8_t)L2P_LOCAL_DIRTY_WORDS; i++)
                {
                    if (cp.region_bitmap[i] != 0U)
                    {
                        region_words++;
                    }
                }
                cp.region_count = region_words;

                cp.magic            = CHECKPOINT_MAGIC;
                cp.sequence         = j->hdr_seq; /* 本池自己的单调序号 */
                cp.timestamp        = ftl_job_now_ms();
                cp.journal_sequence = j->journal_seq; /* 截断到此 seq（job 起点快照） */
                cp.layout_version   = FTL_MEDIA_LAYOUT_VERSION;
                cp.block_word_count = FTL_BLOCK_TOTAL;
                cp.block_word_bytes = CP_BLOCK_WORD_BYTES;
                cp.block_page_count = CP_BLOCK_PAGES;
                cp.block_crc32      = boot_crc32(j->block_words, sizeof(j->block_words));
                cp.wear_entry_count = FTL_BLOCK_TOTAL;
                cp.wear_entry_bytes = FTL_CHECKPOINT_WEAR_ENTRY_BYTES;
                cp.wear_page_count  = FTL_CHECKPOINT_WEAR_PAGES;
                memcpy(cp.wear_page_crc, j->wear_page_crc, sizeof(cp.wear_page_crc));
                cp.record_pages = CHECKPOINT_RECORD_PAGES;

                if (cp_submit_write(j, j->hdr_addr, &cp, (uint32_t)sizeof(cp)) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;
            }

            case CP_TRUNC_PREP:
            {
                /* ⑤ 截断 Journal：预擦环形最旧块（纯 RAM 判定 + 算址，不动介质）。
                 *   ⚠ 无论 plan 返回 JRN_TRUNC_ALREADY（已预擦过）还是 JRN_TRUNC_NO_TRUNCATE
                 *   （水位不满足），本 CP 都**已
                 *   完成**它承诺的事（region 已落盘、Anchor/Header 已写）⇒ 照旧复位水位
                 *   标志并收尾。这与旧 checkpoint_create 第 ⑧ 步的**无条件复位**逐字对应
                 *   （旧函数末尾无条件清 emergency/checkpoint_needed，与 truncate 内部
                 *   是否真的擦了无关）。 */
                nand_pba_t addr;
                if (journal_truncate_plan(ftl->journal, j->journal_seq, &addr, &j->trunc_next) !=
                    JRN_TRUNC_NEEDS_ERASE)
                {
                    journal_truncate_commit(ftl->journal, JOURNAL_BLOCK_NONE);
                    j->phase = (uint8_t)CP_DONE;
                    break;
                }

                j->trunc_addr = addr;
                j->phase      = (uint8_t)CP_TRUNC_ERASE;
                break;
            }

            case CP_TRUNC_ERASE:
                if (cp_submit_erase(j, j->trunc_addr) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;

            case CP_DONE:
            {
                if (j->failed != 0U)
                {
                    LOGW("[CP] abort at phase=%u (不写 Header / 不截断，脏区与 journal 保留)\r\n",
                         (unsigned)j->fail_phase);
                }
                else
                {
                    /* 观测口：一条 CP 走完一行（含 journal 水位）。 */
                    LOGD("[CP] done seq=%lu jrn_seq=%lu anchor=%s\r\n",
                         (unsigned long)j->hdr_seq,
                         (unsigned long)j->journal_seq,
                         (j->anchor_skip != 0U) ? "skip" : "written");
                }

                j->active       = 0U;
                j->phase        = (uint8_t)CP_IDLE;
                j->waiting      = 0U;
                j->failed       = 0U;
                j->stall_rounds = 0U;
                return;
            }

            case CP_IDLE:
            default:
                /* 不该发生：active 置位而 phase 非法 */
                ASSERT(j->active == 0U);
                return;
        }
    }
}

/* ── 对外接口（job）── */

ftl_status_t checkpoint_begin(ftl_context_t *ftl)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传实例指针。 */
    ASSERT(ftl != NULL);

    if (ftl->cp_job->active != 0U)
    {
        return FTL_AGAIN; /* 单飞行：上一次还没跑完 */
    }

    cp_job_t *j = ftl->cp_job;
    memset(j, 0, sizeof(*j));

    /* ① 起点快照（**必须早于任何 flush**，见文件头"快照点前移"的长注释）：
     *    flush 成功会清 region 脏位 ⇒ 快照必须先行；journal 水位同理。
     *    共享 cache 的脏位图是全局的；本 job 只需要本 die 的局部 2048-bit 快照。 */
    l2p_cache_snapshot_owner_bitmap(ftl->cache, ftl_topo_dim(ftl->topo), j->region_bitmap);
    j->journal_seq = ftl->journal->sequence_counter;

    /* 块元数据快照与 region_bitmap / journal_seq 在**同一个协作点**取定：
     * 单线程 superloop 里本函数不会被完成回调打断 ⇒ 三者是同一个 cut。
     * ⚠ 不要把 块元数据快照挪到 flush 之后：CP 期间完成的写会同时改 BST 与 journal，
     *   水位与快照会对不上。 */
    checkpoint_block_snapshot(&ftl->alloc, j->block_words);

    /* CP 之后的同 LBA append 不得合并进水位之前的条目——否则 sequence 还是旧的、
     * new_pba 却是新的，delta 回放会漏记（见 journal.c 的 merge_floor）。 */
    ftl->journal->merge_floor = j->journal_seq;

    j->active   = 1U;
    j->phase    = (uint8_t)CP_FLUSH_REGIONS;
    j->start_ms = ftl_job_now_ms();
    j->log_ms   = j->start_ms;

    cp_step(ftl); /* 立刻推进第一步（脏区落盘可当场起异步写回） */
    return FTL_OK;
}

void checkpoint_kick(ftl_context_t *ftl)
{
    if (ftl->cp_job->active == 0U)
    {
        return;
    }

    cp_job_t *j = ftl->cp_job;

    /* 心跳（见 ftl_job.h）：正常 CP 远快于 1s ⇒ 平时静默；只有卡在某 phase 才持续可见。
     * 判据 = 两次心跳之间 phase 不动 ⇒ 卡死（异步后"卡在哪一步"否则完全不可见）。
     * ⚠ 起点在 checkpoint_begin 显式记（CP 的"job 起点"是快照时刻，不是首次心跳时刻）。 */
    if (ftl_job_heartbeat(&j->log_ms, &j->start_ms))
    {
        LOGI("[CP] job phase=%u wait=%u fail=%u age=%lums\r\n",
             (unsigned)j->phase,
             (unsigned)j->waiting,
             (unsigned)j->failed,
             (unsigned long)ftl_job_age_ms(&j->start_ms));
    }

    cp_step(ftl);
}

/* ── Checkpoint job 的完成服务（g_op_service[FTL_OP_CHECKPOINT]）── */

/* --------------------------------------------------------------------
 * ftl_op_checkpoint
 *   功能：Checkpoint 命令的完成服务函数——按 job.phase 记完成结果、改 phase、release。
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `cp_job_t *`（= ftl->cp_job）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：**本函数不推进状态机**（见文件头：阶段里有同步原语，在完成泵里跑就是
 *             嵌套泵）——只把 phase 改到"下一步该干什么"，由下一轮 checkpoint_kick 续跑。
 *             ⚠ Header 的池游标**无论成败都推进**（半编程槽不可复用）；失败时**不**截断
 *             journal、**不**复位水位标志（与旧实现的提前 return 逐条对应）。
 * -------------------------------------------------------------------- */
void ftl_op_checkpoint(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    cp_job_t *j = (cp_job_t *)ctx;

    ASSERT(completion != NULL);
    ASSERT(j != NULL);

    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    /* 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出的归属实例，
     * 二者必须一致——不一致说明派发路由错了。 */
    ASSERT(ftl != NULL);
    ASSERT(ftl->cp_job == j);

    int failed = ((completion->status & FC_CW_STATUS_ERROR_MASK) != 0U) ? 1 : 0;

    switch ((cp_phase_t)j->phase)
    {
        case CP_ANCHOR_ERASE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;
            if (failed != 0)
            {
                /* 换块擦失败 ⇒ 不写、**不推进游标**（与旧序一致：metadata_ring_prepare_write
                 * 擦失败即 return FTL_ERROR），dirty 保留 ⇒ 下次在新槽重试整条。 */
                cp_abort(j);
                return;
            }
            j->phase = (uint8_t)CP_ANCHOR_WRITE;
            return;

        case CP_ANCHOR_WRITE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;
            if (failed != 0)
            {
                j->anchor_ok = 0U; /* 本条 record 残缺 ⇒ 整条作废（不取半条） */
            }
            j->anchor_chunk = (uint8_t)(j->anchor_chunk + 1U); /* 无论成败都往后写下一页 */
            return;

        case CP_HDR_ERASE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;
            if (failed != 0)
            {
                /* 换块擦失败 ⇒ 不写 Header、**不推进游标**（同旧序），中止本次 CP。 */
                cp_abort(j);
                return;
            }
            j->phase = (uint8_t)CP_BLOCK_WRITE;
            return;

        case CP_BLOCK_WRITE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* 快照页写失败 ⇒ 整条 record 作废，不写 Header（Header 是 commit）。
                 * 游标推进一槽：本条记录的剩余页可能半编程，永不复用。 */
                j->block_ok       = 0U;
                ftl->cp.write_idx = (uint16_t)((j->hdr_idx + 1U) %
                                               metadata_ring_total_records(&j->hdr_ring));
                cp_abort(j);
                return;
            }
            j->block_page = (uint8_t)(j->block_page + 1U);
            return;

        case CP_WEAR_WRITE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* 磨损页写失败 ⇒ 同块元数据页：整条 record 作废（不写 Header），游标推进一槽。 */
                j->block_ok       = 0U;
                ftl->cp.write_idx = (uint16_t)((j->hdr_idx + 1U) %
                                               metadata_ring_total_records(&j->hdr_ring));
                cp_abort(j);
                return;
            }
            j->wear_page = (uint8_t)(j->wear_page + 1U);
            return;

        case CP_HDR_WRITE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            /* 无论成败都推进游标：已写过的槽（可能半编程）不可复用。 */
            ftl->cp.write_idx = (uint16_t)((j->hdr_idx + 1U) % metadata_ring_total_records(&j->hdr_ring));

            if (failed != 0)
            {
                /* 失败时不写 journal_truncate、不复位水位标志：脏数据仍由下个 checkpoint
                 * 重试（与旧实现的提前 return FTL_ERROR 逐条对应）。 */
                cp_abort(j);
                return;
            }
            ftl->cp.last_journal_sequence = j->journal_seq; /* T3 空转门 / 增量判据 */
            /* Header 是 commit 页：此时快照的脏区都已落盘。若本次 CP 起点覆盖了
             * Journal 的持久性失败点，可清除 pending（见 journal_clear_write_failure）。 */
            journal_clear_write_failure(ftl->journal, j->journal_seq);
            j->phase = (uint8_t)CP_TRUNC_PREP;
            return;

        case CP_TRUNC_ERASE:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* ⚠ **与旧实现逐条对应**：journal_truncate 当年就是 `(void)ftl_sync_erase_block(...)`
                 * 丢弃结果、照样记账 pre_erased_block（本次异步化**不顺手改语义**）。
                 * 留一行日志把这条路径变可观测——它意味着 journal_flush_page 回切到该块时
                 * 会跳过擦除（写进未擦块）。 */
                LOGW("[CP] journal pre-erase failed blk=%u — 仍记账（沿用旧语义）\r\n",
                     (unsigned)j->trunc_next);
            }

            /* commit 记账 + 复位水位标志（设计 §7.5.4：本次 CP 已把 seq ≤ 水位 的条目
             * 落盘并截断 ⇒ 清零。不复位会让 T1 永久命中——每圈必 CP）。 */
            journal_truncate_commit(ftl->journal, j->trunc_next);
            j->phase = (uint8_t)CP_DONE;
            return;

        case CP_IDLE:
        case CP_FLUSH_REGIONS:
        case CP_ANCHOR_PREP:
        case CP_HDR_PREP:
        case CP_TRUNC_PREP:
        case CP_DONE:
        default:
            /* 不该发生：一条命令对应一个提交阶段（ANCHOR_ERASE / ANCHOR_WRITE /
             * HDR_ERASE / HDR_WRITE / TRUNC_ERASE）。 */
            LOGE("[CP] unexpected completion phase=%u\r\n", (unsigned)j->phase);
            (void)fc_command_release(cmd);
            j->waiting = 0U;
            cp_abort(j);
            return;
    }
}

/* --------------------------------------------------------------------
 * checkpoint_find_latest
 *   功能：开机扫 Checkpoint 池，挑 sequence 最大的有效记录，重读其 Header 并恢复游标/序号。
 *   参数：ftl —— FTL 实例（池路由取 ftl->topo，游标/序号写回 ftl->cp）；
 *         cp —— 出参，收最新 Header 本体。
 *   返回：FTL_OK 找到并读回一致；FTL_ERROR 池内无有效记录 / 重读失败 / 校验不符。
 *   实现要点：进入即复位 ftl->cp（找不到时保持 0/无效 ⇒ 后续 create 从池首开始）；
 *             metadata_ring_locate 只回序号，Header 本体要按该序号**再读一次**，
 *             并复核 sequence 与 locate 报的一致（防止 locate 后池被改动）。
 * -------------------------------------------------------------------- */
ftl_status_t checkpoint_find_latest(struct ftl_context *ftl, l2p_checkpoint_header_t *cp, uint16_t *out_record_idx)
{
    /* 复位本实例池状态：找不到有效 Header 时保持 0/无效（后续 create 从池首开始） */
    ftl->cp.sequence              = 0U;
    ftl->cp.last_journal_sequence = 0U;
    ftl->cp.write_idx             = 0U;
    ftl->cp.cursor_valid          = 0U;
    ftl->cp.advance_next          = 0U;

    checkpoint_probe_ctx_t probe_ctx;
    checkpoint_ring_init(&probe_ctx.ring);
    probe_ctx.topo        = ftl->topo;
    probe_ctx.any_version = false;

    uint16_t newest = UINT16_MAX;
    if (metadata_ring_locate(&probe_ctx.ring, checkpoint_record_probe, &probe_ctx, &newest, &ftl->cp.sequence, &ftl->cp.write_idx) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (newest == UINT16_MAX)
    {
        ftl->cp.sequence  = 0U;
        ftl->cp.write_idx = 0U;
        return FTL_ERROR; /* 池内无有效 record */
    }

    ftl->cp.cursor_valid = 1U;

    /* 重读胜者（locate 只返回序号，Header 本体要再读一次）。
     * 镜像池：逐个 plane 试；sequence 必须等于 locate 选中的 record sequence。 */
    bool found = false;
    for (uint8_t pi = 0U; pi < metadata_ring_plane_count(probe_ctx.ring.plane_mask); pi++)
    {
        const uint8_t plane = metadata_ring_plane_at(probe_ctx.ring.plane_mask, pi);
        nand_pba_t    addr  = metadata_ring_addr_plane(&probe_ctx.ring, &probe_ctx.topo, newest, 0U, plane);

        if (ftl_sync_read_page(addr, cp, sizeof(*cp), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((!checkpoint_header_valid(cp)) || (cp->sequence != ftl->cp.sequence))
        {
            continue;
        }
        found = true;
        break;
    }
    if (!found)
    {
        return FTL_ERROR;
    }

    ftl->cp.last_journal_sequence = cp->journal_sequence;
    /* 本 session 首次 CP 先跳一槽：避开掉电残留的「快照已写 / Header 未写」半编程槽。 */
    ftl->cp.advance_next          = 1U;

    if (out_record_idx != NULL)
    {
        *out_record_idx = newest;
    }

    return FTL_OK;
}

cp_media_status_t checkpoint_probe_media_status(struct ftl_context *ftl, uint32_t *out_layout_version)
{
    ASSERT(ftl != NULL);

    checkpoint_probe_ctx_t ctx;
    checkpoint_ring_init(&ctx.ring);
    ctx.topo        = ftl->topo;
    ctx.any_version = true; /* 只校验 magic：版本不符也要能定位出来 */

    uint16_t newest    = UINT16_MAX;
    uint32_t seq       = 0U;
    uint16_t write_idx = 0U;

    if (metadata_ring_locate(&ctx.ring, checkpoint_record_probe, &ctx, &newest, &seq, &write_idx) != FTL_OK)
    {
        return CP_MEDIA_IO_ERROR; /* 池几何/配置错误：不格式化 */
    }

    if (newest != UINT16_MAX)
    {
        const uint8_t plane_count = metadata_ring_plane_count(ctx.ring.plane_mask);

        for (uint8_t pi = 0U; pi < plane_count; pi++)
        {
            const uint8_t           plane = metadata_ring_plane_at(ctx.ring.plane_mask, pi);
            nand_pba_t              addr  = metadata_ring_addr_plane(&ctx.ring, &ctx.topo, newest, 0U, plane);
            l2p_checkpoint_header_t hdr;

            if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != FTL_OK)
            {
                continue;
            }
            if ((hdr.magic != CHECKPOINT_MAGIC) || (hdr.sequence == 0U))
            {
                continue;
            }

            if (out_layout_version != NULL)
            {
                *out_layout_version = hdr.layout_version;
            }
            return (hdr.layout_version == FTL_MEDIA_LAYOUT_VERSION) ? CP_MEDIA_CURRENT
                                                                    : CP_MEDIA_VERSION_MISMATCH;
        }

        return CP_MEDIA_IO_ERROR; /* locate 看到了 record，重读却读不通 */
    }

    /* BBT 最新 record 带 FORMAT_IN_PROGRESS ⇒ 上次格式化在擦除中途掉电：
     * 继续格式化（BBT 坏块图已在擦用户块之前落盘）。 */
    if (ftl->alloc.bbt.format_in_progress)
    {
        return CP_MEDIA_NO_RECORD;
    }

    /* 没有可定位的 record：raw 读 CP 池 slot 0 的 page 0（两 plane）区分三种情形：
     *   · 命令走不通        ⇒ IO_ERROR（总线/超时，不格式化）
     *   · 读到非 FF 内容    ⇒ 有介质内容但没有有效 CP（损坏，不格式化）
     *   · 读到全 FF         ⇒ 再看固定元数据区是否已写过：空片 ⇒ NO_RECORD；
     *                         其它元数据已写 ⇒ IO_ERROR（CP 缺失，不自动擦片）
     * ⚠ 不要只用 bbt_medium_has_ftl_data 判空：它对“空片”和“总线读死”都返回 false。 */
    bool cmd_ok = false;
    bool non_ff = false;

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        nand_pba_t addr = metadata_ring_addr_plane(&ctx.ring, &ctx.topo, 0U, 0U, plane);
        uint8_t    raw[PAGE_SIZE];

        (void)memset(raw, 0xFF, sizeof(raw));
        if (ftl_sync_read_page_raw(addr, raw) != FTL_OK)
        {
            continue;
        }
        cmd_ok = true;

        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            const uint8_t *p = raw + ((uint32_t)cw * FC_BUFFER_POOL_CW_SIZE);

            for (uint8_t j = 0U; j < 16U; j++)
            {
                if (p[j] != 0xFFU)
                {
                    non_ff = true;
                    break;
                }
            }
            if (non_ff)
            {
                break;
            }
        }
    }

    if (!cmd_ok)
    {
        return CP_MEDIA_IO_ERROR;
    }
    if (non_ff)
    {
        return CP_MEDIA_IO_ERROR;
    }

    return bbt_medium_has_ftl_data(&ftl->alloc) ? CP_MEDIA_IO_ERROR : CP_MEDIA_NO_RECORD;
}

/* --------------------------------------------------------------------
 * checkpoint_block_snapshot
 *   功能：把当前 BST（state/valid_count/temp）与 OP/Spare 池归属编码成块元数据快照。
 *   参数：alloc —— 分配器（前置条件）；words —— 输出（FTL_BLOCK_TOTAL 个 16-bit word）。
 *   实现要点：在 Checkpoint job 起点调用，与 region_bitmap / journal_sequence 同一
 *             协作点取定（单线程 superloop 不会被完成回调打断）。
 *             池归属不在 BST state 里（OP/Spare 都是 NONE）⇒ 从两个数组补写。
 * -------------------------------------------------------------------- */
void checkpoint_block_snapshot(const ftl_allocator_t *alloc, uint16_t words[FTL_BLOCK_TOTAL])
{
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);
    ASSERT(words != NULL);

    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const ftl_block_t *b = &alloc->blocks[g];

        words[g] = checkpoint_block_word_pack(b->valid_count,
                                              (uint8_t)ftl_block_state(b),
                                              b->temp,
                                              CP_BLOCK_POOL_NONE);
    }

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        for (uint16_t i = 0U; i < alloc->op_count[p]; i++)
        {
            const uint16_t g = alloc->op_blocks[p][i];

            if (!ftl_block_is_bad(&alloc->blocks[g]))
            {
                words[g] = (uint16_t)((words[g] & ~((uint16_t)CP_BLOCK_WORD_POOL_MASK << CP_BLOCK_WORD_POOL_SHIFT)) |
                                      ((uint16_t)CP_BLOCK_POOL_OP << CP_BLOCK_WORD_POOL_SHIFT));
            }
        }
        for (uint16_t i = 0U; i < alloc->spare_count[p]; i++)
        {
            const uint16_t g = alloc->spare_blocks[p][i];

            if (!ftl_block_is_bad(&alloc->blocks[g]))
            {
                words[g] = (uint16_t)((words[g] & ~((uint16_t)CP_BLOCK_WORD_POOL_MASK << CP_BLOCK_WORD_POOL_SHIFT)) |
                                      ((uint16_t)CP_BLOCK_POOL_SPARE << CP_BLOCK_WORD_POOL_SHIFT));
            }
        }
    }
}

/* --------------------------------------------------------------------
 * checkpoint_wear_page_build
 *   功能：把第 page_index 页磨损快照从 BST 组进 page（256 条 × 8 B）。
 *   参数：alloc —— 分配器；page_index —— 0..FTL_CHECKPOINT_WEAR_PAGES-1；
 *         page —— 输出（PAGE_MAIN_SIZE）。
 *   实现要点：条目按 gblock 稠密排列（下标即块号）；rising_count 截到 4 bit，
 *             health 截到 2 bit。软状态：调用时从 BST 现取，CP 期间的计数变化
 *             允许漏到下一个 CP。
 * -------------------------------------------------------------------- */
void checkpoint_wear_page_build(const ftl_allocator_t *alloc, uint16_t page_index, uint8_t page[PAGE_MAIN_SIZE])
{
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);
    ASSERT(page != NULL);
    ASSERT(page_index < FTL_CHECKPOINT_WEAR_PAGES);

    cp_wear_entry_t *entries = (cp_wear_entry_t *)page;

    for (uint16_t i = 0U; i < FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE; i++)
    {
        const uint32_t g = ((uint32_t)page_index * FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE) + i;

        if (g < FTL_BLOCK_TOTAL)
        {
            const ftl_block_t *b      = &alloc->blocks[g];
            uint8_t            rising = (b->rr.rising_count > CP_WEAR_RISING_MASK) ? CP_WEAR_RISING_MASK : b->rr.rising_count;

            entries[i].erase_count = b->erase_count;
            entries[i].read_count  = b->read_count;
            entries[i].last_ecc    = b->rr.last_ecc;
            entries[i].flags       = (uint8_t)((rising & CP_WEAR_RISING_MASK) |
                                         ((b->rr.health & CP_WEAR_HEALTH_MASK) << CP_WEAR_HEALTH_SHIFT));
        }
        else
        {
            entries[i].erase_count = 0U;
            entries[i].read_count  = 0U;
            entries[i].last_ecc    = 0U;
            entries[i].flags       = 0U;
        }
    }
}

/* --------------------------------------------------------------------
 * checkpoint_load_wear_table
 *   功能：读取并校验 Checkpoint 记录里的磨损快照，回填 BST。
 *   返回：true = 已加载并回填；false = 无快照 / 字段不符 / 读失败 / CRC 不符。
 *   实现要点：逐页从镜像的任一 plane 读；每页自带 CRC（Header 的 wear_page_crc[]）。
 *             失败只表示软状态不可用，**不得**触发格式化。
 * -------------------------------------------------------------------- */
bool checkpoint_load_wear_table(struct ftl_context            *ftl,
                                const l2p_checkpoint_header_t *cp,
                                uint16_t                       record_idx)
{
    ASSERT(ftl != NULL);
    ASSERT(cp != NULL);

    if (cp->wear_page_count == 0U)
    {
        return false; /* initial CP / 旧记录：无磨损快照 */
    }
    if ((cp->wear_entry_count != FTL_BLOCK_TOTAL) ||
        (cp->wear_entry_bytes != FTL_CHECKPOINT_WEAR_ENTRY_BYTES) ||
        (cp->wear_page_count != FTL_CHECKPOINT_WEAR_PAGES))
    {
        LOGW("[WEAR] snapshot fields inconsistent (entries=%lu bytes=%lu pages=%lu)\r\n",
             (unsigned long)cp->wear_entry_count,
             (unsigned long)cp->wear_entry_bytes,
             (unsigned long)cp->wear_page_count);
        return false;
    }

    metadata_ring_t ring;
    checkpoint_ring_init(&ring);

    uint8_t *page = ftl->cp_job->wear_page_buf; /* 开机时 CP job 未在跑，可复用 */

    for (uint8_t p = 0U; p < FTL_CHECKPOINT_WEAR_PAGES; p++)
    {
        bool loaded = false;

        for (uint8_t pi = 0U; pi < metadata_ring_plane_count(ring.plane_mask); pi++)
        {
            const uint8_t plane = metadata_ring_plane_at(ring.plane_mask, pi);
            nand_pba_t    addr  = metadata_ring_addr_plane(&ring,
                                                       &ftl->topo,
                                                       record_idx,
                                                       (uint8_t)(1U + CP_BLOCK_PAGES + p),
                                                       plane);

            if (ftl_sync_read_page(addr, page, PAGE_MAIN_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
            {
                continue;
            }
            if (boot_crc32(page, PAGE_MAIN_SIZE) != cp->wear_page_crc[p])
            {
                LOGW("[WEAR] page %u crc mismatch (plane=%u rec=%u)\r\n",
                     (unsigned)p,
                     (unsigned)plane,
                     (unsigned)record_idx);
                continue;
            }

            const cp_wear_entry_t *entries = (const cp_wear_entry_t *)page;

            for (uint16_t i = 0U; i < FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE; i++)
            {
                const uint32_t g = ((uint32_t)p * FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE) + i;

                if (g >= FTL_BLOCK_TOTAL)
                {
                    break;
                }

                ftl_block_t *b = &ftl->alloc.blocks[g];

                b->erase_count     = entries[i].erase_count;
                b->read_count      = entries[i].read_count;
                b->rr.last_ecc     = entries[i].last_ecc;
                b->rr.rising_count = (uint8_t)(entries[i].flags & CP_WEAR_RISING_MASK);
                b->rr.health       = (uint8_t)((entries[i].flags >> CP_WEAR_HEALTH_SHIFT) & CP_WEAR_HEALTH_MASK);
            }

            loaded = true;
            break;
        }

        if (!loaded)
        {
            LOGW("[WEAR] snapshot page %u unavailable (rec=%u)\r\n", (unsigned)p, (unsigned)record_idx);
            return false;
        }
    }

    return true;
}

/* --------------------------------------------------------------------
 * checkpoint_load_block_table
 *   功能：读取并校验一条 Checkpoint 记录的块元数据快照（Header 之后的 4 页）。
 *   参数：ftl —— 实例；cp —— 最近 Header；record_idx —— 该 Header 的池内槽位；
 *         block_words —— 输出（FTL_BLOCK_TOTAL 个 16-bit word）。
 *   返回：true = 加载且 CRC 通过；false = 无快照 / 字段不符 / 读失败 / CRC 不符。
 *   实现要点：block_page_count == 0 是 initial CP 的显式「无快照」标记，不是错误；
 *             快照损坏**不得**触发格式化，也不得回退到更旧的 Checkpoint——
 *             旧快照属于旧 L2P 快照，回退会丢 journal 已截断的映射。
 * -------------------------------------------------------------------- */
bool checkpoint_load_block_table(struct ftl_context            *ftl,
                                 const l2p_checkpoint_header_t *cp,
                                 uint16_t                       record_idx,
                                 uint16_t                       block_words[FTL_BLOCK_TOTAL])
{
    ASSERT(ftl != NULL);
    ASSERT(cp != NULL);
    ASSERT(block_words != NULL);

    if (cp->block_page_count == 0U)
    {
        return false; /* initial CP / 旧记录：无快照 */
    }
    if ((cp->block_word_count != FTL_BLOCK_TOTAL) ||
        (cp->block_word_bytes != CP_BLOCK_WORD_BYTES) ||
        (cp->block_page_count != CP_BLOCK_PAGES) ||
        (cp->record_pages != CHECKPOINT_RECORD_PAGES))
    {
        LOGW("[BLK] header fields inconsistent (words=%lu bytes=%lu pages=%lu rec=%lu)\r\n",
             (unsigned long)cp->block_word_count,
             (unsigned long)cp->block_word_bytes,
             (unsigned long)cp->block_page_count,
             (unsigned long)cp->record_pages);
        return false;
    }

    metadata_ring_t ring;
    checkpoint_ring_init(&ring);

    /* 镜像池：整条快照从**同一个 plane** 读（不在 plane 间拼页）；该副本读失败或
     * CRC 不符时换另一个 plane 重读整条。 */
    for (uint8_t pi = 0U; pi < metadata_ring_plane_count(ring.plane_mask); pi++)
    {
        const uint8_t plane = metadata_ring_plane_at(ring.plane_mask, pi);

        uint8_t fail_page = 0U;
        bool    ok        = true;

        for (uint8_t p = 0U; p < CP_BLOCK_PAGES; p++)
        {
            nand_pba_t addr = metadata_ring_addr_plane(&ring, &ftl->topo, record_idx, (uint8_t)(1U + p), plane);

            if (ftl_sync_read_page(addr,
                                   &block_words[(uint32_t)p * CP_BLOCK_WORDS_PER_PAGE],
                                   PAGE_MAIN_SIZE,
                                   FTL_PAGE_CFG_META()) != FTL_OK)
            {
                fail_page = p;
                ok        = false;
                break;
            }
        }

        if (!ok)
        {
            LOGW("[BLK] read failed plane=%u page=%u rec=%u\r\n",
                 (unsigned)plane,
                 (unsigned)fail_page,
                 (unsigned)record_idx);
            continue;
        }

        if (boot_crc32(block_words, sizeof(uint16_t) * FTL_BLOCK_TOTAL) != cp->block_crc32)
        {
            LOGW("[BLK] crc mismatch plane=%u rec=%u seq=%lu\r\n",
                 (unsigned)plane,
                 (unsigned)record_idx,
                 (unsigned long)cp->sequence);
            continue;
        }

        LOGI("[BLK] load plane=%u rec=%u seq=%lu pages=%u crc=ok\r\n",
             (unsigned)plane,
             (unsigned)record_idx,
             (unsigned long)cp->sequence,
             (unsigned)CP_BLOCK_PAGES);
        return true;
    }

    return false;
}

/* --------------------------------------------------------------------
 * checkpoint_write_initial
 *   功能：在 Checkpoint 池首槽写一条「空快照」标记（全量格式化后调用）。
 *   参数：topo —— 目标芯片拓扑。
 *   返回：FTL_OK / FTL_ERROR（取槽或写失败）。
 *   实现要点：目的是让下次开机的版本判据（checkpoint_find_latest 成功 ⇒ 版本匹配）
 *             立即命中 ⇒ 自动格式化只发生一次，不会因"开机写了元数据但还没写过
 *             checkpoint"而在下次开机再格式化一遍。sequence 从 1 起（0 = 未写）。
 * -------------------------------------------------------------------- */
ftl_status_t checkpoint_write_initial(ftl_topology_t topo)
{
    l2p_checkpoint_header_t cp;

    memset(&cp, 0, sizeof(cp));
    cp.magic            = CHECKPOINT_MAGIC;
    cp.sequence         = 1U;
    cp.layout_version   = FTL_MEDIA_LAYOUT_VERSION;
    cp.journal_sequence = 0U; /* 空快照：无 region、journal 从 0 起 */
    /* initial CP 不带块元数据快照：首次开机走 F2 → carve，下一次 CP 才写完整块元数据。 */
    cp.block_word_count = 0U;
    cp.block_word_bytes = 0U;
    cp.block_page_count = 0U;
    cp.block_crc32      = 0U;
    cp.wear_entry_count = 0U;
    cp.wear_entry_bytes = 0U;
    cp.wear_page_count  = 0U;
    cp.record_pages     = 1U;

    metadata_ring_t ring;
    checkpoint_ring_init(&ring);

    nand_pba_t addr;
    if (metadata_ring_prepare_write(&ring, &topo, 0U, &addr) != FTL_OK)
    {
        return FTL_ERROR;
    }
    /* 开机/格式化：同步镜像写到 P0+P1（没有服务泵可等）。 */
    if (ftl_sync_write_page_mirror(addr, &cp, sizeof(cp), FTL_PAGE_CFG_META()) != 0)
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

/* ── 触发 / 水位 ── */

/* --------------------------------------------------------------------
 * checkpoint_check_trigger
 *   功能：判定是否该做 Checkpoint（三路触发，任一命中即 true）。
 *   参数：ftl —— FTL 实例；elapsed_ms —— 距上次 CP 的毫秒数（调用者算）。
 *   返回：true 触发；false 不触发（含 ftl==NULL）。
 *   实现要点：T1 Journal 水位（checkpoint_needed，由 checkpoint_journal_watermark 置）；
 *             T2 写入量（total_writes 整除 CP_WRITE_INTERVAL，且 >0 防开机即命中）；
 *             T3 定时（elapsed_ms ≥ CP_TIME_INTERVAL_MS）。
 * -------------------------------------------------------------------- */
bool checkpoint_check_trigger(const ftl_context_t *ftl, uint32_t elapsed_ms)
{
    (void)elapsed_ms;

    if (ftl == NULL)
    {
        return false;
    }

    /* T1: Journal 水位线 */
    if (ftl->journal->checkpoint_needed)
    {
        return true;
    }

    /* T2: 写入量触发 */
    if ((ftl->total_writes > 0U) &&
        ((ftl->total_writes % CP_WRITE_INTERVAL) == 0U))
    {
        return true;
    }

    /* T3: 定时触发（由调用者检查 elapsed_ms）。
     * ⚠ 21 页/CP 的块元数据快照让本条 record 的磨损约为旧 1 页/CP 的 21× ⇒ 轻载下加最小
     *   journal 增量门：定时拍到了但自上次 CP 没有足够新条目 ⇒ 不写。 */
    if (elapsed_ms >= CP_TIME_INTERVAL_MS)
    {
        const uint32_t seq  = ftl->journal->sequence_counter;
        const uint32_t last = ftl->cp.last_journal_sequence;

        if ((seq >= last) && ((seq - last) >= CP_T3_MIN_JOURNAL_DELTA))
        {
            return true;
        }
    }

    return false;
}

/* checkpoint_is_emergency — Journal ≥95% 紧急水位（journal.emergency）置位则 true，否则 false */
bool checkpoint_is_emergency(const ftl_context_t *ftl)
{
    if (ftl == NULL)
    {
        return false;
    }

    return (ftl->journal->emergency != 0U);
}

/* --------------------------------------------------------------------
 * checkpoint_journal_watermark
 *   功能：按 Journal 当前写位置算出使用率，置位 checkpoint_needed(≥85%) / emergency(≥95%)。
 *   参数：journal —— 待评估的 Journal（前置条件，ASSERT）。
 *   返回：无。
 *   实现要点：分子必须是**全局页偏移**（current_block×PAGES_PER_BLOCK + current_page，
 *             全环口径），分母为 JOURNAL_BLOCK_COUNT×PAGES_PER_BLOCK。
 *             旧实现单块口径导致水位过早/永不触发（详见函数内长注释）。
 * -------------------------------------------------------------------- */
void checkpoint_journal_watermark(l2p_journal_t *journal)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 ftl->journal。 */
    ASSERT(journal != NULL);

    /*
     * 计算 Journal 使用率（全环全局口径）：
     *   used% = (current_block × PAGES_PER_BLOCK + current_page) × 100
     *           + current_entry × 100 / JOURNAL_ENTRIES_PER_PAGE
     *           ──────────────────────────────────────────────────────
     *           (JOURNAL_BLOCK_COUNT × PAGES_PER_BLOCK)
     *
     * 分子必须是「全局页偏移」= current_block×PAGES_PER_BLOCK + current_page，
     * 而非单块内 current_page（0..63）。旧实现两处都错：
     *   ① 分母用 PAGES_PER_BLOCK=64（单块）→ 85%/95% 在 page≈55/61 就触发，
     *      紧急水位离写满只剩 3 页≈384 条
     *   ② 若只改分母而分子仍用 current_page → used 最大 63×100/256≈24%，
     *      85%/95% 永远不触发（更糟）
     * 现分子分母都按全环口径（JOURNAL_BLOCK_COUNT = CW_NUM = 4 块 = 256 页）：
     * 85%（JOURNAL_FULL_THRESHOLD）在全局页 218（留 38 页）、95%（JOURNAL_CRITICAL）
     * 在全局页 244（留 12 页），与 §7.5.2 推导对齐。
     */
    uint32_t used = ((uint32_t)journal->current_block * PAGES_PER_BLOCK +
                     (uint32_t)journal->current_page) *
                        100U / ((uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK) +
                    ((uint32_t)journal->current_entry * 100U / JOURNAL_ENTRIES_PER_PAGE / ((uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK));

    if (used >= JOURNAL_CRITICAL)
    {
        journal->emergency = 1U; /* ≥95% 紧急 */
    }
    else if (used >= JOURNAL_FULL_THRESHOLD)
    {
        journal->checkpoint_needed = 1U; /* ≥85% 需 Checkpoint */
    }
}
