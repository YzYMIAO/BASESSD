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
 *   CHECKPOINT_POOL_BLOCKS 个块、一条 record = 1 页、单 plane；
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

#include "ftl/checkpoint.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/ftl_job.h" /* 心跳 + 单调毫秒（ftl_job_now_ms / ftl_job_heartbeat）*/
#include "ftl/ftl_op.h"  /* ftl_op_submit / FTL_OP_CHECKPOINT */
#include "ftl/metadata.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 内部辅助 ── */

/* ── Checkpoint 池：环形轮转（一条 record = 1 页）──
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
    ring->pages_per_record = 1U;
    ring->plane            = 0U;
}

/* 整条校验（一条 record = 1 页）：magic + 布局版本。
 * 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证（ADR v0.62）。 */
static int checkpoint_header_valid(const l2p_checkpoint_header_t *hdr)
{
    if (hdr->magic != CHECKPOINT_MAGIC)
    {
        return 0;
    }
    if (hdr->layout_version != FTL_MEDIA_LAYOUT_VERSION)
    {
        return 0; /* 旧格式介质的 Checkpoint ⇒ 作废（触发开机自动格式化）*/
    }

    return 1;
}

typedef struct
{
    ftl_topology_t  topo;
    metadata_ring_t ring;
} checkpoint_probe_ctx_t;

/* Checkpoint record 探针（见 metadata_ring_probe_fn）。
 * full 与廉价探针等价（一条 record 只有一页），故忽略。 */
static ftl_status_t checkpoint_record_probe(void *ctx, uint16_t record_idx, int full, uint32_t *out_sequence)
{
    checkpoint_probe_ctx_t *c = (checkpoint_probe_ctx_t *)ctx;
    l2p_checkpoint_header_t hdr;
    nand_pba_t              addr;

    (void)full;

    addr = metadata_ring_addr(&c->ring, &c->topo, record_idx, 0U);

    if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != 0)
    {
        return FTL_ERROR;
    }
    if ((checkpoint_header_valid(&hdr) == 0) || (hdr.sequence == 0U))
    {
        return FTL_ERROR; /* sequence 从 1 起；0 = 未写 */
    }

    *out_sequence = hdr.sequence;
    return FTL_OK;
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

/* 累计一次「提交不出去」；超限即中止 job（与 MIG/RR 同族）。返回 1 = 已中止。 */
static int cp_stall(cp_job_t *j)
{
    if (j->stall_rounds >= CP_SUBMIT_STALL_MAX)
    {
        LOGW("[CP] stall %u rounds — abort phase=%u\r\n",
             (unsigned)j->stall_rounds,
             (unsigned)j->phase);
        cp_abort(j);
        return 1;
    }
    j->stall_rounds++;
    return 0;
}

/* 提交一条异步写（Anchor 分页 / Checkpoint Header 共用）。data 在本函数内已被
 * ftl_async_write_page 拷进 FC buffer ⇒ 调用者的栈缓冲随即可复用。 */
static ftl_status_t cp_submit_write(cp_job_t *j, nand_pba_t addr, const void *data, uint32_t length)
{
    fc_command_t *cmd;

    if (ftl_async_write_page(&cmd, addr, data, length, FTL_PAGE_CFG_META()) != FTL_OK)
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

/* 提交一条异步擦除（Anchor 换块 / Checkpoint 换块 / Journal 预擦共用）。 */
static ftl_status_t cp_submit_erase(cp_job_t *j, nand_pba_t addr)
{
    fc_command_t *cmd;

    if (ftl_async_erase_block(&cmd, addr) != FTL_OK)
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
 *   功能：按**快照**脏位图把每个脏 region 落盘（阶段②）。返回 1 = 全部已持久；
 *         0 = 仍有在飞（下轮再来）；-1 = 硬失败（调用者中止，不写 Header）。
 *   实现要点：**不按 state == DIRTY 过滤**——异步化后脏行可能已被后台 l2p_flush_kick
 *             起了写回（state = FLUSHING）而尚未完成，它同样属于"本周期改过、必须已持久"，
 *             漏掉它就会让 Header 承诺一个还没落盘的映射。FLUSHING 行**等**它（不重复起）。
 *             CLEAN 行 = 已落盘（commit 已清脏位，见 l2p_cache.c 的 ftl_op_l2p）。
 *             ⚠ 判据是**快照位图**而非实时位图：实时位图会被本次 flush 的 commit 清掉
 *             （那样循环就永远看不到自己该等的行）。
 * -------------------------------------------------------------------- */
static int cp_flush_regions(ftl_context_t *ftl, cp_job_t *j)
{
    uint16_t i;
    int      pending = 0;

    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &ftl->cache.lines[i];
        uint32_t          word;
        uint32_t          mask;

        if (line->state == L2P_CACHE_INVALID)
        {
            continue; /* 空行（region_id 无意义，脏位查询无意义） */
        }

        word = j->region_bitmap[line->region_id / 32U];
        mask = (1UL << (line->region_id & 31U));
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
            const ftl_status_t st = l2p_cache_flush_kick(&ftl->cache, (int)i);

            if (st == FTL_ERROR)
            {
                return -1; /* 无依赖 / 建链失败：中止（同旧 checkpoint_flush_dirty_regions） */
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
        LOGW("[CP] region=%u dirty but state=%u (LOADING) — abort\r\n",
             (unsigned)line->region_id,
             (unsigned)line->state);
        return -1;
    }

    if (pending == 0)
    {
        j->stall_rounds = 0U;
        return 1;
    }

    /* 有在飞写回 = 有进展（**不**计卡死轮数）：闸门满 / 提交失败都会让本轮起不动新行，
     * 但 flush_inflight > 0 说明完成迟早会来。
     * ⚠ 同理，**目录整理 job 在飞也算有进展**（阶段 8）：它独占当前日志块的空槽，期间的
     *   脏 region 一律收到 FTL_AGAIN。一次正常的压缩要走上百轮命令，若照常累计就会撞
     *   CP_SUBMIT_STALL_MAX 把 Checkpoint 打回去。 */
    if ((ftl->cache.flush_inflight > 0U) || (metadata_dir_busy(&ftl->metadata_dir) != 0))
    {
        j->stall_rounds = 0U;
        return 0;
    }

    return cp_stall(j) ? -1 : 0;
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
    cp_job_t *j = &ftl->cp_job;

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
                const int r = cp_flush_regions(ftl, j);

                if (r < 0)
                {
                    cp_abort(j);
                    break;
                }
                if (r == 0)
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
                int need_erase = 0;
                int r          = metadata_dir_anchor_prepare(&ftl->metadata_dir,
                                                    &j->anchor_ring,
                                                    &j->anchor_base,
                                                    &j->anchor_idx,
                                                    &j->anchor_seq,
                                                    &need_erase);

                if (r < 0)
                {
                    cp_abort(j);
                    break;
                }
                if (r == 0)
                {
                    /* Directory 无变更：整段跳过（Anchor 每写一条都消耗池内槽位，
                     * 空转会让池无谓轮转）。旧实现是 metadata_dir_flush_to_anchor 的
                     * dirty==0 早退，语义相同。 */
                    j->anchor_skip = 1U;
                    j->phase       = (uint8_t)CP_HDR_PREP;
                    break;
                }

                j->anchor_need_erase = (uint8_t)need_erase;
                j->anchor_chunk      = 0U;
                j->anchor_ok         = 1U;
                j->phase             = (need_erase != 0) ? (uint8_t)CP_ANCHOR_ERASE : (uint8_t)CP_ANCHOR_WRITE;
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
                metadata_anchor_page_t page; /* 2KB 栈缓冲：按页组好后写入 */
                nand_pba_t             aaddr;

                if (j->anchor_chunk >= ANCHOR_PAGE_COUNT)
                {
                    /* 整条 record 写完：推进游标（无论成败），成功才清 dirty。
                     * 失败 ⇒ 整条残缺（locate 会整条丢弃）⇒ 中止，不写 Header。 */
                    metadata_dir_anchor_commit(&ftl->metadata_dir, &j->anchor_ring, j->anchor_idx, (int)j->anchor_ok);

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

                aaddr = pba_with_page(j->anchor_base, (uint32_t)pba_to_page(j->anchor_base) + j->anchor_chunk);
                metadata_dir_anchor_page(&ftl->metadata_dir, j->anchor_seq, j->anchor_chunk, &page);

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
                checkpoint_probe_ctx_t probe_ctx;
                int                    need_erase = 0;

                checkpoint_ring_init(&j->hdr_ring);

                if (ftl->cp.cursor_valid == 0U)
                {
                    uint16_t newest = UINT16_MAX;

                    probe_ctx.topo = ftl->topo;
                    probe_ctx.ring = j->hdr_ring;

                    if (metadata_ring_locate(&j->hdr_ring, &probe_ctx.topo, checkpoint_record_probe, &probe_ctx, &newest, &ftl->cp.sequence, &ftl->cp.write_idx) != FTL_OK)
                    {
                        cp_abort(j);
                        break;
                    }
                    ftl->cp.cursor_valid = 1U;
                }

                /* sequence 在定位之后取（池内已有序号时才能续上），与旧序一致。 */
                j->hdr_seq = ++ftl->cp.sequence;
                j->hdr_idx = ftl->cp.write_idx;

                if (metadata_ring_prepare_addr(&j->hdr_ring, &ftl->topo, j->hdr_idx, &j->hdr_addr, &need_erase) != FTL_OK)
                {
                    cp_abort(j);
                    break;
                }

                j->hdr_need_erase = (uint8_t)need_erase;
                j->phase          = (need_erase != 0) ? (uint8_t)CP_HDR_ERASE : (uint8_t)CP_HDR_WRITE;
                break;
            }

            case CP_HDR_ERASE:
                if (cp_submit_erase(j, j->hdr_addr) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;

            case CP_HDR_WRITE:
            {
                l2p_checkpoint_header_t cp; /* 2KB 栈缓冲：恰好一页主区 */
                uint8_t                 i;
                uint32_t                region_words = 0U;

                memset(&cp, 0, sizeof(cp));
                memcpy(cp.region_bitmap, j->region_bitmap, sizeof(cp.region_bitmap));

                /* region_count = 置位 word 数（无消费方，仅信息量；多 LUN 时高位 word 恒 0） */
                for (i = 0U; i < 64U; i++)
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

                if (cp_submit_write(j, j->hdr_addr, &cp, (uint32_t)sizeof(cp)) != FTL_OK)
                {
                    (void)cp_stall(j);
                    return;
                }
                return;
            }

            case CP_TRUNC_PREP:
            {
                nand_pba_t addr;

                /* ⑤ 截断 Journal：预擦环形最旧块（纯 RAM 判定 + 算址，不动介质）。
                 *   ⚠ 无论 plan 返回 0（已预擦过）还是 -1（水位不满足），本 CP 都**已
                 *   完成**它承诺的事（region 已落盘、Anchor/Header 已写）⇒ 照旧复位水位
                 *   标志并收尾。这与旧 checkpoint_create 第 ⑧ 步的**无条件复位**逐字对应
                 *   （旧函数末尾无条件清 emergency/checkpoint_needed，与 truncate 内部
                 *   是否真的擦了无关）。 */
                if (journal_truncate_plan(&ftl->journal, j->journal_seq, &addr, &j->trunc_next) <= 0)
                {
                    journal_truncate_commit(&ftl->journal, JOURNAL_BLOCK_NONE);
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
    cp_job_t *j;

    if (ftl == NULL)
    {
        return FTL_ERROR;
    }
    if (ftl->cp_job.active != 0U)
    {
        return FTL_AGAIN; /* 单飞行：上一次还没跑完 */
    }

    j = &ftl->cp_job;
    memset(j, 0, sizeof(*j));

    /* ① 起点快照（**必须早于任何 flush**，见文件头"快照点前移"的长注释）：
     *    flush 成功会清 region 脏位 ⇒ 快照必须先行；journal 水位同理。 */
    memcpy(j->region_bitmap, ftl->cache.region_dirty_bitmap, sizeof(j->region_bitmap));
    j->journal_seq = ftl->journal.sequence_counter;

    j->active   = 1U;
    j->phase    = (uint8_t)CP_FLUSH_REGIONS;
    j->start_ms = ftl_job_now_ms();
    j->log_ms   = j->start_ms;

    cp_step(ftl); /* 立刻推进第一步（脏区落盘可当场起异步写回） */
    return FTL_OK;
}

void checkpoint_kick(ftl_context_t *ftl)
{
    cp_job_t *j;

    if ((ftl == NULL) || (ftl->cp_job.active == 0U))
    {
        return;
    }

    j = &ftl->cp_job;

    /* 心跳（见 ftl_job.h）：正常 CP 远快于 1s ⇒ 平时静默；只有卡在某 phase 才持续可见。
     * 判据 = 两次心跳之间 phase 不动 ⇒ 卡死（异步后"卡在哪一步"否则完全不可见）。
     * ⚠ 起点在 checkpoint_begin 显式记（CP 的"job 起点"是快照时刻，不是首次心跳时刻）。 */
    if (ftl_job_heartbeat(&j->log_ms, &j->start_ms) != 0)
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
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `cp_job_t *`（= &ftl->cp_job）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：**本函数不推进状态机**（见文件头：阶段里有同步原语，在完成泵里跑就是
 *             嵌套泵）——只把 phase 改到"下一步该干什么"，由下一轮 checkpoint_kick 续跑。
 *             ⚠ Header 的池游标**无论成败都推进**（半编程槽不可复用）；失败时**不**截断
 *             journal、**不**复位水位标志（与旧实现的提前 return 逐条对应）。
 * -------------------------------------------------------------------- */
void ftl_op_checkpoint(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    cp_job_t     *j = (cp_job_t *)ctx;
    fc_command_t *cmd;
    int           failed;

    ASSERT(completion != NULL);
    ASSERT(j != NULL);

    cmd = fc_command_id_to_command((uint16_t)completion->command_id);
    ASSERT(cmd != NULL);

    /* 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出的归属实例，
     * 二者必须一致——不一致说明派发路由错了。 */
    ASSERT(ftl != NULL);
    ASSERT(&ftl->cp_job == j);

    failed = ((completion->status & FC_CW_STATUS_ERROR_MASK) != 0U) ? 1 : 0;

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
            j->phase = (uint8_t)CP_HDR_WRITE;
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
            journal_truncate_commit(&ftl->journal, j->trunc_next);
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
 *   返回：FTL_OK 找到并读回一致；FTL_ERROR 参数非法 / 池内无有效记录 / 重读失败 / 校验不符。
 *   实现要点：进入即复位 ftl->cp（找不到时保持 0/无效 ⇒ 后续 create 从池首开始）；
 *             metadata_ring_locate 只回序号，Header 本体要按该序号**再读一次**，
 *             并复核 sequence 与 locate 报的一致（防止 locate 后池被改动）。
 * -------------------------------------------------------------------- */
ftl_status_t checkpoint_find_latest(struct ftl_context *ftl, l2p_checkpoint_header_t *cp)
{
    checkpoint_probe_ctx_t probe_ctx;
    uint16_t               newest = UINT16_MAX;
    nand_pba_t             addr;

    if ((ftl == NULL) || (cp == NULL))
    {
        return FTL_ERROR;
    }

    /* 复位本实例池状态：找不到有效 Header 时保持 0/无效（后续 create 从池首开始） */
    ftl->cp.sequence     = 0U;
    ftl->cp.write_idx    = 0U;
    ftl->cp.cursor_valid = 0U;

    checkpoint_ring_init(&probe_ctx.ring);
    probe_ctx.topo = ftl->topo;

    if (metadata_ring_locate(&probe_ctx.ring, &probe_ctx.topo, checkpoint_record_probe, &probe_ctx, &newest, &ftl->cp.sequence, &ftl->cp.write_idx) != FTL_OK)
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

    /* 重读胜者（locate 只返回序号，Header 本体要再读一次）*/
    addr = metadata_ring_addr(&probe_ctx.ring, &probe_ctx.topo, newest, 0U);
    if (ftl_sync_read_page(addr, cp, sizeof(*cp), FTL_PAGE_CFG_META()) != 0)
    {
        return FTL_ERROR;
    }
    if ((checkpoint_header_valid(cp) == 0) || (cp->sequence != ftl->cp.sequence))
    {
        return FTL_ERROR;
    }

    return FTL_OK;
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
    metadata_ring_t         ring;
    nand_pba_t              addr;

    memset(&cp, 0, sizeof(cp));
    cp.magic            = CHECKPOINT_MAGIC;
    cp.sequence         = 1U;
    cp.layout_version   = FTL_MEDIA_LAYOUT_VERSION;
    cp.journal_sequence = 0U; /* 空快照：无 region、journal 从 0 起 */

    checkpoint_ring_init(&ring);
    if (metadata_ring_prepare_write(&ring, &topo, 0U, &addr) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (ftl_sync_write_page(addr, &cp, sizeof(cp), FTL_PAGE_CFG_META()) != 0)
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

/* ── 触发 / 水位 ── */

/* --------------------------------------------------------------------
 * checkpoint_check_trigger
 *   功能：判定是否该做 Checkpoint（三路触发，任一命中即 1）。
 *   参数：ftl —— FTL 实例；elapsed_ms —— 距上次 CP 的毫秒数（调用者算）。
 *   返回：1 触发；FTL_OK(0) 不触发（含 ftl==NULL）。
 *   实现要点：T1 Journal 水位（checkpoint_needed，由 checkpoint_journal_watermark 置）；
 *             T2 写入量（total_writes 整除 CP_WRITE_INTERVAL，且 >0 防开机即命中）；
 *             T3 定时（elapsed_ms ≥ CP_TIME_INTERVAL_MS）。
 * -------------------------------------------------------------------- */
int checkpoint_check_trigger(const ftl_context_t *ftl, uint32_t elapsed_ms)
{
    (void)elapsed_ms;

    if (ftl == NULL)
    {
        return FTL_OK;
    }

    /* T1: Journal 水位线 */
    if (ftl->journal.checkpoint_needed)
    {
        return 1;
    }

    /* T2: 写入量触发 */
    if ((ftl->total_writes > 0U) &&
        ((ftl->total_writes % CP_WRITE_INTERVAL) == 0U))
    {
        return 1;
    }

    /* T3: 定时触发（由调用者检查 elapsed_ms）*/
    if (elapsed_ms >= CP_TIME_INTERVAL_MS)
    {
        return 1;
    }

    return FTL_OK;
}

/* checkpoint_is_emergency — Journal ≥95% 紧急水位（journal.emergency）置位则 1，否则 FTL_OK(0) */
int checkpoint_is_emergency(const ftl_context_t *ftl)
{
    if (ftl == NULL)
    {
        return FTL_OK;
    }

    return (int)ftl->journal.emergency;
}

/* --------------------------------------------------------------------
 * checkpoint_journal_watermark
 *   功能：按 Journal 当前写位置算出使用率，置位 checkpoint_needed(≥85%) / emergency(≥95%)。
 *   参数：journal —— 待评估的 Journal（NULL 直接返回）。
 *   返回：无。
 *   实现要点：分子必须是**全局页偏移**（current_block×PAGES_PER_BLOCK + current_page，
 *             全环口径），分母为 JOURNAL_BLOCK_COUNT×PAGES_PER_BLOCK。
 *             旧实现单块口径导致水位过早/永不触发（详见函数内长注释）。
 * -------------------------------------------------------------------- */
void checkpoint_journal_watermark(l2p_journal_t *journal)
{
    uint32_t used;

    if (journal == NULL)
    {
        return;
    }

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
    used = ((uint32_t)journal->current_block * PAGES_PER_BLOCK +
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
