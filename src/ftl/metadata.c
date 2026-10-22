/*
 * metadata.c — 元数据落盘基建：record 池环形轮转 + Anchor 池 + L2P 日志块
 *
 * 职责：
 *   提供 FTL 全部持久化元数据（Anchor / Checkpoint / BBT / L2P region）共用的
 *   落盘原语：环形 record 池的寻址/定位/换块擦除；Anchor（Metadata Directory 快照）
 *   的组页/落盘/加载；L2P 日志块的追加槽/登记；以及"这块是不是元数据块"的判定。
 *   日志块的**回收与压缩**状态机在 metadata_job.c（本文件只负责登记待回收位图）。
 *
 * 与 metadata.h 的分工：
 *   metadata.h —— 结构（md_ring_t / md_anchor_page_t / md_dir_cache_t）+
 *                 固定元数据区布局（池尺寸 / 块偏移 / 表字节 / 分区表声明）；
 *   本文件     —— 上述结构的全部算法实现 + 分区表本体（单一事实来源）。
 *
 * L2P region 的位置只有一层来源（见 metadata.h「固定元数据区布局」）：
 *   Directory 条目 = `NAND_PBA_INVALID` ⇒ region 从未写过（未映射，读取不碰 NAND）；
 *   非 INVALID ⇒ region 页在日志块上的真实 PBA（从固定 L2P 日志区取块、追加写）。
 *
 * 文件结构：
 *   一、元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 *   二、Anchor 池（Metadata Directory 的 record 池）
 *   三、Directory 基础层 / 查询 / 更新
 *   四、L2P 日志块：追加槽 / 登记
 *   五、Anchor 落盘 / 加载
 *   六、元数据块判定
 *
 * ⚠ 同步 / 异步的分界：
 *   · **异步**：日志块的回收与压缩（`md_handle_t` + g_op_callbacks[FTL_OP_METADATA]，
 *      状态机在 metadata_job.c）。待回收块由 commit_region / append_slot 登记进
 *      `reclaim_pending` 位图，运行期没有任何同步擦除。
 *   · **同步（刻意）**：开机加载 / 格式化（md_load / init）、Anchor 池定位
 *      （md_ring_locate）——前者在 superloop 之前跑（无泵可等），后者只在
 *      冷启动后首次整理出现。
 *
 * 断言口径：init/bind 入口与 release 关键边界用 ASSERT（判据 2）；运行期模块入口的
 * 实例句柄/出参由调用链保证，不再逐次复检；ring 几何非法、槽位越界等
 * 配置错误在 release 也必须拒绝（判据 9），因此保留 FTL_ERROR 返回。
 */

#include "metadata.h"

#include "assertion.h"
#include "bbt.h"
#include "checkpoint.h"
#include "fc.h"
#include "ftl.h" /* ftl_ctx_t（服务函数要从实例取 &ftl->alloc） */
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_METADATA */
#include "journal.h"
#include "log.h"
#include "metadata_job.h" /* slot_now / job_reset / 待回收位图（与 metadata_job.c 共用） */
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
 * 一、元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 * ══════════════════════════════════════════════════════════════════ */

/* ring 几何自检：任何一条不满足都会让地址公式除零或产生越界槽位。
 * 这是**配置错误** ⇒ 调用者在 release 也必须拿到 FTL_ERROR（判据 9）。
 * 不要求 PAGES_PER_BLOCK % pages_per_record == 0：Checkpoint 的 21 页/record
 * 刻意向下取整，块尾剩余页不用。 */
static bool md_ring_geom_valid(const md_ring_t *ring)
{
    const BYTE valid_plane_bits = ((1U << PLANE_NUM) - 1U);

    if ((ring->block_count < 2U) || (ring->pages_per_record == 0U) ||
        (ring->pages_per_record > PAGES_PER_BLOCK))
    {
        return false;
    }
    if ((ring->plane_mask == 0U) || ((ring->plane_mask & ~valid_plane_bits) != 0U))
    {
        return false; /* 必须至少用一个 plane，且不得指向不存在的 plane */
    }

    const WORD rpb = (PAGES_PER_BLOCK / ring->pages_per_record);

    if ((ring->block_count * rpb) > UINT16_MAX)
    {
        return false; /* 槽位总数必须装进 WORD（写游标 / record 下标都是 16 位）*/
    }
    if ((ring->block_count > MD_RING_MAX_BLOCKS) ||
        (ring->nominal_count > MD_RING_MAX_BLOCKS))
    {
        return false; /* 候选表容量 */
    }
    for (WORD i = 0U; i < ring->block_count; i++)
    {
        if (ring->blocks[i] >= BLOCKS_PER_PLANE)
        {
            return false; /* 候选块号越出本 plane */
        }
    }

    return true;
}

/* --------------------------------------------------------------------
 * md_ring_addr_plane / md_ring_addr
 *   功能：把「池内第 record_idx 条 record 的第 page_in_record 页」换算成物理地址。
 *   参数：ring —— 池几何；topo —— ch/ce/lun；record_idx —— 池内记录序号；
 *         page_in_record —— record 内页偏移（Anchor 一条 = 8 页）；plane —— 目标 plane。
 *   返回：DWORD（cw 恒 0）；几何非法时返回 NAND_PBA_INVALID（release 拒绝）。
 *   实现要点：先按 records_per_block 整除得**候选下标**、取余得块内记录号，
 *             再查 `ring->blocks[]` 得到物理块号，乘 pages_per_record 得块内页号。
 *             地址公式的唯一出口；候选池让同一 record_idx 在不同 BAD 集下落到
 *             不同物理块，但 **sequence 扫描定位**不依赖固定块号。
 *             `md_ring_addr` 是"取 plane_mask 里第一个 plane"的薄入口，
 *             镜像池的读回退/写镜像用 `_plane` 版本。
 * -------------------------------------------------------------------- */
DWORD md_ring_addr_plane(const md_ring_t  *ring,
                         const ftl_topo_t *topo,
                         WORD              record_idx,
                         BYTE              page_in_record,
                         BYTE              plane)
{
    if (!md_ring_geom_valid(ring))
    {
        return NAND_PBA_INVALID; /* 配置错误：不产生"看起来合法"的错误地址 */
    }

    WORD rpb = md_ring_records_per_block(ring);

    if (record_idx >= (ring->block_count * rpb))
    {
        return NAND_PBA_INVALID; /* 槽位越界：release 也必须拒绝（判据 9）*/
    }

    return pba_make(topo->ch,
                    topo->ce,
                    topo->lun,
                    ring->blocks[record_idx / rpb],
                    plane,
                    (((record_idx % rpb) * ring->pages_per_record) + page_in_record),
                    0U);
}

/* ══════════════════════════════════════════════════════════════════
 * 固定元数据分区的候选块池（BBT + shared spare）
 * ══════════════════════════════════════════════════════════════════ */

/* 物理块在 plane_mask 的所有 plane 上都非 BAD。
 * 镜象/多 plane 擦写要求**同一物理块号**在每个 plane 上都可用，否则
 * mirror_write_prepare / multi-plane erase 会写到坏 plane 的同号块。 */
static bool md_block_good_on_mask(const alc_t *alloc, WORD block, BYTE plane_mask)
{
    const BYTE n = md_ring_plane_count(plane_mask);

    for (BYTE i = 0U; i < n; i++)
    {
        const BYTE plane = md_ring_plane_at(plane_mask, i);
        const WORD g     = ftl_gblock(plane, block);

        if (ftl_block_is_bad(&alloc->blocks[g]))
        {
            return false;
        }
    }
    return true;
}

/* 收名义区间的好块；不足 min_blocks 时从 shared spare 按升序补（受 spare_budget
 * 与全局 spare_used 双重限制）。**只补到硬下界，不强行补满名义数**：CP 少一块
 * 仍可轮转，把 spare 留给更紧的 BBT/Journal。 */
static WORD md_pool_collect(md_ring_t   *ring,
                            const alc_t *alloc,
                            WORD         first,
                            WORD         nominal,
                            BYTE         pages_per_record,
                            BYTE         plane_mask,
                            WORD         min_blocks,
                            WORD         spare_budget,
                            bool         spare_used[MD_SPARE_BLOCKS_PER_PLANE])
{
    ring->first_block      = first;
    ring->nominal_count    = nominal;
    ring->block_count      = 0U;
    ring->pages_per_record = pages_per_record;
    ring->plane_mask       = plane_mask;

    const WORD end = (WORD)(first + nominal);

    for (WORD b = first; (b < end) && (ring->block_count < MD_RING_MAX_BLOCKS); b++)
    {
        if (md_block_good_on_mask(alloc, b, plane_mask))
        {
            ring->blocks[ring->block_count++] = b;
        }
    }

    WORD used = 0U;

    for (WORD s = MD_SPARE_BLOCK0;
         (s < (MD_SPARE_BLOCK0 + MD_SPARE_BLOCKS_PER_PLANE)) && (used < spare_budget) &&
         (ring->block_count < min_blocks) && (ring->block_count < MD_RING_MAX_BLOCKS);
         s++)
    {
        const WORD si = (WORD)(s - MD_SPARE_BLOCK0);

        if (spare_used[si] || !md_block_good_on_mask(alloc, s, plane_mask))
        {
            continue;
        }
        ring->blocks[ring->block_count++] = s;
        spare_used[si]                    = true;
        used++;
    }

    return ring->block_count;
}

void md_pool_build_bbt_bootstrap(md_ring_t *ring)
{
    /* 未知 BAD 集：名义 BBT 块 + 全部 shared spare。BBT record 自带 magic/sequence，
     * 坏块/空块读失败由 md_ring_locate 的探针自然忽略。 */
    ring->first_block      = BBT_BLOCK;
    ring->nominal_count    = BBT_POOL_BLOCKS;
    ring->block_count      = 0U;
    ring->pages_per_record = 1U;
    ring->plane_mask       = METADATA_MIRROR_PLANE_MASK;

    for (WORD b = BBT_BLOCK; b < (BBT_BLOCK + BBT_POOL_BLOCKS); b++)
    {
        ring->blocks[ring->block_count++] = b;
    }
    for (WORD s = MD_SPARE_BLOCK0; s < (MD_SPARE_BLOCK0 + MD_SPARE_BLOCKS_PER_PLANE); s++)
    {
        ring->blocks[ring->block_count++] = s;
    }
}

bool md_pool_build_all(alc_t *alloc)
{
    if ((alloc == NULL) || (alloc->pools == NULL))
    {
        return false; /* 绑定缺失是配置错误：release 也必须拒绝 */
    }

    md_pools_t *p = alloc->pools;

    /* 先在局部构建，全部成功才整体替换：失败时保留上一份可用候选池，
     * 避免「BBT/CP 已换、Journal 失败」的半更新状态被在飞模块读到。 */
    md_pools_t next;

    (void)memset(&next, 0, sizeof(next));

    /* 分配顺序固定：先紧后松。BBT/Journal 的名义池最小，优先拿 spare。 */
    bool spare_used[MD_SPARE_BLOCKS_PER_PLANE];

    (void)memset(spare_used, 0, sizeof(spare_used));

    if (md_pool_collect(&next.bbt,
                        alloc,
                        BBT_BLOCK,
                        BBT_POOL_BLOCKS,
                        1U,
                        METADATA_MIRROR_PLANE_MASK,
                        2U,
                        2U,
                        spare_used) < 2U)
    {
        LOGE(LOG_TAG_POOL "BBT candidates < 2 — metadata area unusable\r\n");
        return false;
    }

    if (md_pool_collect(&next.cp,
                        alloc,
                        CHECKPOINT_BLOCK0,
                        CHECKPOINT_POOL_BLOCKS,
                        FTL_CHECKPOINT_RECORD_PAGES,
                        METADATA_MIRROR_PLANE_MASK,
                        2U,
                        1U,
                        spare_used) < 2U)
    {
        LOGE(LOG_TAG_POOL "Checkpoint candidates < 2\r\n");
        return false;
    }

    if (md_pool_collect(&next.jrn,
                        alloc,
                        JOURNAL_BLOCK0,
                        JOURNAL_BLOCK_COUNT,
                        1U,
                        METADATA_MIRROR_PLANE_MASK,
                        JOURNAL_BLOCK_COUNT,
                        2U,
                        spare_used) < JOURNAL_BLOCK_COUNT)
    {
        LOGE(LOG_TAG_POOL "Journal candidates < %u\r\n", (unsigned)JOURNAL_BLOCK_COUNT);
        return false;
    }

    if (md_pool_collect(&next.anchor,
                        alloc,
                        ANCHOR_BLOCK0,
                        ANCHOR_POOL_BLOCKS,
                        ANCHOR_PAGE_COUNT,
                        METADATA_MIRROR_PLANE_MASK,
                        2U,
                        0U,
                        spare_used) < 2U)
    {
        LOGE(LOG_TAG_POOL "Anchor candidates < 2\r\n");
        return false;
    }

    next.dirty = false;
    *p         = next;

    /* 候选表可能变了 ⇒ BBT 的 record 下标到物理块的映射也可能变。
     * 下一次 bbt_save 必须在新池上重新定位（不能沿用旧 write_idx）。 */
    alloc->bbt.cursor_valid = false;

    return true;
}

DWORD md_ring_addr(const md_ring_t  *ring,
                   const ftl_topo_t *topo,
                   WORD              record_idx,
                   BYTE              page_in_record)
{
    return md_ring_addr_plane(ring, topo, record_idx, page_in_record, md_ring_plane_at(ring->plane_mask, 0U));
}

/* 一次池扫描的中间结果（①~③ 步）。 */
typedef struct
{
    WORD  cursor_block;  /* 第一个未写满的块；全满时为"最新满块之后" */
    WORD  best_full_idx; /* 全部满块末条中 sequence 最大者（UINT16_MAX = 无）*/
    DWORD best_full_seq;
    WORD  cursor_last_idx; /* 游标块内最后一条有效 record（UINT16_MAX = 无）*/
    DWORD cursor_last_seq;
    bool  ring_full; /* 池已写满一圈 */
} ring_scan_t;

/* ①~③：逐块探末条 → 找游标块 → 游标块内找末条。 */
static void ring_scan(const md_ring_t       *ring,
                      metadata_ring_probe_fn probe,
                      void                  *ctx,
                      WORD                   rpb,
                      ring_scan_t           *s)
{
    s->cursor_block    = UINT16_MAX;
    s->best_full_idx   = UINT16_MAX;
    s->best_full_seq   = 0U;
    s->cursor_last_idx = UINT16_MAX;
    s->cursor_last_seq = 0U;
    s->ring_full       = false;

    /* ① 逐块廉价探针该块**末条** record：末条有效 = 该块写满 */
    for (WORD b = 0U; b < ring->block_count; b++)
    {
        DWORD seq = 0U;
        WORD  idx = ((b * rpb) + (rpb - 1U));

        if (probe(ctx, idx, false, &seq) == FTL_OK)
        {
            if ((s->best_full_idx == UINT16_MAX) || (seq > s->best_full_seq))
            {
                s->best_full_idx = idx;
                s->best_full_seq = seq;
            }
        }
        else if (s->cursor_block == UINT16_MAX)
        {
            s->cursor_block = b; /* 第一个未写满的块 = 游标块 */
        }
    }

    /* ② 所有块都写满 ⇒ 池转满一圈：游标 = 最新块之后，且该块整体擦掉重写 */
    if (s->cursor_block == UINT16_MAX)
    {
        s->cursor_block = (((s->best_full_idx / rpb) + 1U) % ring->block_count);
        s->ring_full    = true;
    }

    /* ③ 游标块内降序找最后一条有效 record（池转满时游标块本身就是满的，不参与）*/
    if (!s->ring_full)
    {
        for (WORD b = 0U; b < rpb; b++)
        {
            WORD  idx = ((s->cursor_block * rpb) + (rpb - 1U - b));
            DWORD seq = 0U;

            if (probe(ctx, idx, false, &seq) == FTL_OK)
            {
                s->cursor_last_idx = idx;
                s->cursor_last_seq = seq;
                break;
            }
        }
    }
}

/* ⑥ 慢路径：快路径候选整条校验全失败（局部损坏）⇒ 全池廉价探针取 sequence 最大者，
 *    整条校验；失败则排除该 sequence 继续找次新。仅在损坏时付出全池代价。 */
static bool ring_locate_fallback(metadata_ring_probe_fn probe,
                                 void                  *ctx,
                                 WORD                   total,
                                 WORD                  *out_newest_idx,
                                 DWORD                 *out_sequence)
{
    DWORD limit = UINT32_MAX;

    for (;;)
    {
        WORD  ci = UINT16_MAX;
        DWORD cs = 0U;

        for (WORD k = 0U; k < total; k++)
        {
            DWORD seq = 0U;

            if (probe(ctx, k, false, &seq) != FTL_OK)
            {
                continue;
            }
            if (seq >= limit)
            {
                continue; /* 已排除（含同 sequence 的重复份）*/
            }
            if ((ci == UINT16_MAX) || (seq > cs))
            {
                ci = k;
                cs = seq;
            }
        }

        if ((ci == UINT16_MAX) || (cs == 0U))
        {
            return false; /* 池内已无更多候选 */
        }

        /* 整条校验用独立出参：探针契约是「失败时不写 *out_sequence」，
         * 不能把 cs（下一步的排除界）押在这个隐含约定上。 */
        DWORD full_seq = 0U;

        if (probe(ctx, ci, true, &full_seq) == FTL_OK)
        {
            *out_newest_idx = ci;
            *out_sequence   = full_seq;
            return true;
        }
        limit = cs;
    }
}

/* --------------------------------------------------------------------
 * md_ring_locate
 *   功能：扫池，定位最新有效 record，并算出「下一个该写的槽」。
 *   参数：ring/topo —— 池与拓扑；probe —— 记录探针（廉价/整条两档）；
 *         ctx —— 探针上下文；out_newest_idx/out_sequence/out_write_idx —— 三个出参。
 *   返回：FTL_OK（池空时 out_newest_idx=UINT16_MAX、out_sequence=0 也是 OK）；
 *         FTL_ERROR ring 几何非法（判据 9 运行期拒绝）。
 *   实现要点（六步）：
 *     ①②③ 由 ring_scan 完成：找游标块与最新满块；
 *     ④ 写点 = 游标块末条的下一条（跨块处理）；
 *     ⑤ 快路径：只在「最新满块」+「游标块末条」两个候选里整条校验（正常路径）；
 *     ⑥ 慢路径：快路径全失败（局部损坏）才全池扫描取次新，整条校验，逐轮降 limit。
 * -------------------------------------------------------------------- */
ftl_status_e md_ring_locate(const md_ring_t       *ring,
                            metadata_ring_probe_fn probe,
                            void                  *ctx,
                            WORD                  *out_newest_idx,
                            DWORD                 *out_sequence,
                            WORD                  *out_write_idx)
{
    /* 前置条件（判据 2 / §二十六「子模块公开入口」）：指针均由调用者自持。 */
    /* ring 自身几何是**配置**（构造错误 ⇒ 寻址全错）⇒ 保留运行期拒绝（判据 9）。 */
    if (!md_ring_geom_valid(ring))
    {
        return FTL_ERROR;
    }

    WORD rpb   = md_ring_records_per_block(ring);
    WORD total = md_ring_total_records(ring);

    /* 池空时下面各出口都不写 *out_sequence ⇒ 先给确定值（调用者据此续序号）*/
    *out_sequence   = 0U;
    *out_newest_idx = UINT16_MAX;

    ring_scan_t scan;

    ring_scan(ring, probe, ctx, rpb, &scan);

    /* ④ 写点 = 游标块末条的下一条（末条是块内最后一条时跨到下一块槽 0）；
     *    游标块内无有效 record ⇒ 写点 = 游标块的槽 0 */
    if (scan.cursor_last_idx == UINT16_MAX)
    {
        *out_write_idx = (scan.cursor_block * rpb);
    }
    else if ((scan.cursor_last_idx % rpb) == (rpb - 1U))
    {
        *out_write_idx = ((scan.cursor_last_idx + 1U) % total);
    }
    else
    {
        *out_write_idx = (scan.cursor_last_idx + 1U);
    }

    /* ⑤ 快路径候选：全部块末条中 sequence 最大者 + 游标块末条（按 seq 降序整条校验）*/
    WORD  cand_idx[2];
    DWORD cand_seq[2];
    BYTE  cand_n = 0U;

    if (scan.best_full_idx != UINT16_MAX)
    {
        cand_idx[cand_n] = scan.best_full_idx;
        cand_seq[cand_n] = scan.best_full_seq;
        cand_n++;
    }
    if ((scan.cursor_last_idx != UINT16_MAX) && (scan.cursor_last_idx != scan.best_full_idx))
    {
        cand_idx[cand_n] = scan.cursor_last_idx;
        cand_seq[cand_n] = scan.cursor_last_seq;
        cand_n++;
    }
    if ((cand_n == 2U) && (cand_seq[1] > cand_seq[0]))
    {
        WORD  ti = cand_idx[0];
        DWORD ts = cand_seq[0];

        cand_idx[0] = cand_idx[1];
        cand_seq[0] = cand_seq[1];
        cand_idx[1] = ti;
        cand_seq[1] = ts;
    }

    for (BYTE i = 0U; i < cand_n; i++)
    {
        DWORD seq = 0U;

        if (probe(ctx, cand_idx[i], true, &seq) == FTL_OK)
        {
            *out_newest_idx = cand_idx[i];
            *out_sequence   = seq;
            return FTL_OK;
        }
    }

    /* ⑥ 局部损坏时的全池回退。找不到也返回 FTL_OK（池内无有效 record）。 */
    (void)ring_locate_fallback(probe, ctx, total, out_newest_idx, out_sequence);

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * md_ring_prepare_addr
 *   功能：为写入池内第 write_idx 条 record 算地址，并判定是否需先擦目标块。
 *   参数：ring/topo —— 池与拓扑；write_idx —— 目标槽；out_addr —— 出参物理地址；
 *         out_need_erase —— 出参，true = 写之前必须先擦 *out_addr 所在块。
 *   返回：FTL_OK；FTL_ERROR 几何非法 / write_idx 越界（ring/topo/出参为前置条件）。
 *   实现要点：仅当 write_idx 落在块首（% rpb == 0）才需擦块。池内轮转 ⇒ 该块持全池
 *             最旧 record，且每条 record 是完整快照 ⇒ 擦掉不丢信息；块内槽 0 恒走本
 *             分支（新取/刚擦的块再擦一次是幂等无害代价）。
 *             **不动介质**：擦除由调用者做（同步站点即刻擦；异步 job 提交异步擦）。
 * -------------------------------------------------------------------- */
ftl_status_e md_ring_prepare_addr(const md_ring_t  *ring,
                                  const ftl_topo_t *topo,
                                  WORD              write_idx,
                                  DWORD            *out_addr,
                                  bool             *out_need_erase)
{
    /* 前置条件（判据 2）：四个指针均由调用者自持。 */

    if (!md_ring_geom_valid(ring))
    {
        return FTL_ERROR;
    }
    if (write_idx >= md_ring_total_records(ring))
    {
        return FTL_ERROR;
    }

    *out_addr       = md_ring_addr(ring, topo, write_idx, 0U);
    *out_need_erase = ((write_idx % md_ring_records_per_block(ring)) == 0U);
    if (*out_addr == NAND_PBA_INVALID)
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * md_ring_prepare_write
 *   功能：为写入池内第 write_idx 条 record 做前置——算地址，必要时先擦目标块。
 *   参数：ring/topo —— 池与拓扑；write_idx —— 目标槽；out_addr —— 出参物理地址。
 *   返回：FTL_OK；FTL_ERROR write_idx 越界 / 换块时擦除失败。
 *   实现要点：= md_ring_prepare_addr + 同步擦除。**同步原语，仅开机/格式化用**
 *             （运行期 job 走 md_ring_prepare_addr + 异步擦除）。
 *             镜像池（plane_mask 含多个 plane）用 multi-plane 擦，所有副本一起擦。
 * -------------------------------------------------------------------- */
ftl_status_e md_ring_prepare_write(const md_ring_t  *ring,
                                   const ftl_topo_t *topo,
                                   WORD              write_idx,
                                   DWORD            *out_addr)
{
    DWORD addr;
    bool  need_erase = false;

    if (md_ring_prepare_addr(ring, topo, write_idx, &addr, &need_erase) != FTL_OK)
    {
        return FTL_ERROR;
    }

    if (need_erase)
    {
        /* 换块：先擦目标块（池内轮转 ⇒ 该块持全池最旧 record，擦掉不丢信息） */
        const int st = (md_ring_plane_count(ring->plane_mask) > 1U) ? ftl_sync_erase_block_multi_plane(addr)
                                                                    : ftl_sync_erase_block(addr);
        if (st != 0)
        {
            return FTL_ERROR;
        }
    }

    *out_addr = addr;
    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 二、Anchor 池（Metadata Directory 的 record 池）
 * ══════════════════════════════════════════════════════════════════ */

/* 页头字段校验：magic + chunk_count + 布局版本。 */
static bool anchor_header_fields_valid(DWORD magic, WORD chunk_count, DWORD layout_version)
{
    return (magic == ANCHOR_MAGIC) && (chunk_count == ANCHOR_PAGE_COUNT) &&
           (layout_version == FTL_MEDIA_LAYOUT_VERSION);
}

/* Anchor 池几何：优先用 md_pool_build_all() 生成的候选池（跳过 BAD）；
 * 池尚未建立时退回名义区间（开机早期/测试），BAD 由后续 rebuild 修正。 */
static void anchor_ring_init(const md_dir_cache_t *dir, md_ring_t *ring)
{
    if ((dir->alloc != NULL) && (dir->alloc->pools != NULL))
    {
        *ring = dir->alloc->pools->anchor;
        return;
    }

    ring->first_block      = ANCHOR_BLOCK0;
    ring->nominal_count    = ANCHOR_POOL_BLOCKS;
    ring->block_count      = ANCHOR_POOL_BLOCKS;
    ring->pages_per_record = ANCHOR_PAGE_COUNT;
    ring->plane_mask       = METADATA_MIRROR_PLANE_MASK;
    for (WORD i = 0U; i < ANCHOR_POOL_BLOCKS; i++)
    {
        ring->blocks[i] = (WORD)(ANCHOR_BLOCK0 + i);
    }
}

typedef struct
{
    const md_dir_cache_t *dir;
    const md_ring_t      *ring;
} anchor_probe_ctx_t;

/* 廉价探针：只读 chunk 0 的 20 字节页头取 sequence。
 * FC 读整页并校验 per-CW CRC + ECC，`length` 只限制拷出字节数。 */
static ftl_status_e anchor_record_probe_cheap(const anchor_probe_ctx_t *c,
                                              WORD                      record_idx,
                                              DWORD                    *out_sequence)
{
    const BYTE plane_count = md_ring_plane_count(c->ring->plane_mask);

    for (BYTE pi = 0U; pi < plane_count; pi++)
    {
        const BYTE         plane = md_ring_plane_at(c->ring->plane_mask, pi);
        md_anchor_header_t hdr;
        DWORD              addr = md_ring_addr_plane(c->ring, &c->dir->topo, record_idx, 0U, plane);

        if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((!anchor_header_fields_valid(hdr.magic, hdr.chunk_count, hdr.layout_version)) ||
            (hdr.chunk_index != 0U) || (hdr.sequence == 0U))
        {
            continue;
        }

        *out_sequence = hdr.sequence;
        return FTL_OK;
    }

    return FTL_ERROR;
}

/* 整条校验：全部页有效、chunk 序完整、各页 sequence 一致；不跨 plane 拼半条。 */
static ftl_status_e anchor_record_probe_full(const anchor_probe_ctx_t *c,
                                             WORD                      record_idx,
                                             DWORD                    *out_sequence)
{
    md_anchor_page_t page;
    const BYTE       plane_count = md_ring_plane_count(c->ring->plane_mask);

    for (BYTE pi = 0U; pi < plane_count; pi++)
    {
        const BYTE plane      = md_ring_plane_at(c->ring->plane_mask, pi);
        DWORD      seq        = 0U;
        bool       primary_ok = true;

        for (BYTE chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
        {
            const bool backup = (chunk >= ANCHOR_DIR_PRIMARY_PAGES);
            DWORD      addr   = md_ring_addr_plane(c->ring, &c->dir->topo, record_idx, chunk, plane);

            /* 备份半是 best-effort：单页坏/旧代只让对应 region 暂无双副本，
             * 不能让整条最新 Directory 作废（否则一次备份页失败就回退到旧快照）。 */
            if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
            {
                if (backup)
                {
                    continue;
                }
                primary_ok = false;
                break;
            }
            const bool header_ok = anchor_header_fields_valid(page.magic, page.chunk_count, page.layout_version);

            if ((!header_ok) || (page.chunk_index != chunk))
            {
                if (backup)
                {
                    continue;
                }
                primary_ok = false;
                break;
            }

            if (chunk == 0U)
            {
                seq = page.sequence;
                if (seq == 0U)
                {
                    primary_ok = false; /* sequence 从 1 起；0 = 未写 */
                    break;
                }
            }
            else if (page.sequence != seq)
            {
                if (backup)
                {
                    continue;
                }                   /* 旧代备份页：忽略 */
                primary_ok = false; /* 主半必须同 sequence */
                break;
            }
        }

        if (primary_ok)
        {
            *out_sequence = seq;
            return FTL_OK;
        }
    }

    return FTL_ERROR;
}

/* 探针入口：按 full 分派到廉价/整条两档。 */
static ftl_status_e anchor_record_probe(void *ctx, WORD record_idx, bool full, DWORD *out_sequence)
{
    const anchor_probe_ctx_t *c = (const anchor_probe_ctx_t *)ctx;

    return full ? anchor_record_probe_full(c, record_idx, out_sequence)
                : anchor_record_probe_cheap(c, record_idx, out_sequence);
}

/* ══════════════════════════════════════════════════════════════════
 * 三、Directory 基础层 / 查询 / 更新
 * ══════════════════════════════════════════════════════════════════ */

/*
 * md_fill_fixed - 为全部 region 登记"未映射"（见 metadata.h「固定元数据区布局」）
 *
 * 每个实例的 L2P_REGIONS_PER_INSTANCE 个 region **全部登记为 NAND_PBA_INVALID**；
 * 某个 region 刷新到日志块后由 md_commit_region 就地覆盖成真实 PBA。
 * "全部预登记"同时保证 Directory 容量（= region 数）按构造不会溢出 ——
 * md_update 的"满则丢弃"分支对 L2P region 不可达。
 *
 * 共用于 md_init（首次格式化）与 md_load（基础层）：
 * Anchor 损坏时该层即"全部未映射"，不需要任何闪存读。
 */
static void md_fill_fixed(md_dir_cache_t *dir)
{
    dir->count = 0U;

    for (WORD i = 0U; i < L2P_REGIONS_PER_INSTANCE; i++)
    {
        dir->entries[dir->count].type = METADATA_TYPE_L2P_REGION;
        dir->entries[dir->count].id   = i;
        dir->entries[dir->count].pba  = NAND_PBA_INVALID;
        dir->backup_pba[i]            = NAND_PBA_INVALID;
        dir->count++;
    }
}

/* 把一个 region 副本的 PBA 计入 l2p_live（主/备共用同一套校验）。
 * `*warned` 保证整轮只警告一次；越界/固定非日志区一律跳过。 */
static void md_live_add_copy(md_dir_cache_t *dir, DWORD pba, WORD entry_idx, bool *warned)
{
    if (pba == NAND_PBA_INVALID)
    {
        return;
    }

    const BYTE plane = pba_to_plane(pba);
    const WORD block = pba_to_block(pba);

    if ((plane >= PLANE_NUM) || (block >= BLOCKS_PER_PLANE))
    {
        if (!*warned)
        {
            LOGW(LOG_TAG_META "directory entry %u: plane=%u block=%u out of range — skipped\r\n",
                 (unsigned)entry_idx,
                 (unsigned)plane,
                 (unsigned)block);
            *warned = true;
        }
        return;
    }

    /* 只排除**非日志的**固定元数据区（0..L2P_LOG_BLOCK0−1）。 */
    if (block < L2P_LOG_BLOCK0)
    {
        return;
    }

    const WORD g = ftl_gblock(plane, block);

    if (g < FTL_BLOCK_TOTAL)
    {
        dir->l2p_live[g]++;
    }
    else if (!*warned)
    {
        LOGW(LOG_TAG_META "directory entry %u: gblock %u out of range — skipped\r\n", (unsigned)entry_idx, (unsigned)g);
        *warned = true;
    }
}

/* 由 entries 重建 l2p_live（日志块 = 非固定元数据块）。 */
static void md_rebuild_live(md_dir_cache_t *dir)
{
    memset(dir->l2p_live, 0, sizeof(dir->l2p_live));
    dir->log_borrow_count = 0U;

    bool warned = false;

    for (WORD i = 0U; i < dir->count; i++)
    {
        if (dir->entries[i].type != METADATA_TYPE_L2P_REGION)
        {
            continue;
        }

        /* 主副本 + 备副本都计入 live：回收判据是“所有占用者都走了”。
         * ⚠ INVALID/越界/固定非日志区的校验在 helper 里统一（介质输入不可信，H29）。 */
        const WORD rid = dir->entries[i].id;

        md_live_add_copy(dir, dir->entries[i].pba, i, &warned);
        if (rid < METADATA_DIR_ENTRIES)
        {
            md_live_add_copy(dir, dir->backup_pba[rid], i, &warned);
        }
    }

    /* 借用计数按**块**计，不按 entry 计（一个借来的日志块可承载多个 region）。 */
    dir->log_borrow_count = 0U;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] == 0U)
        {
            continue;
        }

        const BYTE plane = ftl_gblock_plane(g);
        const WORD block = ftl_gblock_block(g);

        if (!ftl_is_metadata_block(NULL, plane, block))
        {
            dir->log_borrow_count++;
        }
    }
}

/* 开机观测口（判据②「走动态分配」/ 判据③「Anchor 真的落盘」的唯一观测口）*/
static void md_boot_log(const md_dir_cache_t *dir)
{
    DWORD live_blocks = 0U;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] != 0U)
        {
            live_blocks++;
        }
    }

    const unsigned dyn = (live_blocks != 0U) ? 1U : 0U;

    if (dir->cur_block_valid)
    {
        LOGI(LOG_TAG_META "anchor_seq=%lu dyn=%u cur_blk=%u live_blks=%lu\r\n",
             dir->anchor_sequence,
             dyn,
             (unsigned)dir->cur_metadata_block,
             live_blocks);
    }
    else
    {
        LOGI(LOG_TAG_META "anchor_seq=%lu dyn=%u cur_blk=- live_blks=%lu\r\n", dir->anchor_sequence, dyn, live_blocks);
    }
}

/* --------------------------------------------------------------------
 * md_init
 *   功能：首次格式化——全部 region 置"未映射"、清 Anchor 序号/脏标记、游标全部置无效。
 *   参数：dir —— 待初始化 Directory（前置条件：调用者传 ftl->metadata_dir）。
 *   返回：无。
 *   实现要点：游标置无效是"绝不复用可能已编程的旧页"的保证——首次追加才从 Free
 *             Pool 新取块。
 * -------------------------------------------------------------------- */
void md_init(md_dir_cache_t *dir, alc_t *alloc, const DWORD *total_writes)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir。 */
    ASSERT(alloc != NULL);
    ASSERT(total_writes != NULL);

    dir->alloc        = alloc;
    dir->total_writes = total_writes;

    dir->anchor_sequence = 0U;
    dir->dirty           = false;
    dir->dirty_gen       = 0U;
    dir->anchor_gen      = 0U;

    md_fill_fixed(dir);
    md_rebuild_live(dir);

    /* 游标全部置无效：首次追加才从固定 L2P 日志区取块（绝不复用可能已编程的旧页）*/
    dir->cur_metadata_block  = 0U;
    dir->cur_metadata_page   = 0U;
    dir->cur_block_valid     = false;
    dir->cur_metadata_plane  = 0U;
    dir->anchor_write_idx    = 0U;
    dir->anchor_cursor_valid = false;

    /* 待回收位图 / 后台整理 job 归零（RAM 态，不跨重启；
     * 不清 job ⇒ append_slot 永久让路）。 */
    md_reclaim_reset(dir);
    md_handle_reset(dir);
}

/* 查 (type,id) 的下标；未找到返回 dir->count（调用者用它判 < count）。
 *
 * ⚠ 必须先走**稠密快路径**：Directory 的 L2P_REGION 条目由 `md_fill_fixed` 按
 * `entries[id].id == id` 稠密预登记（Anchor 也按同一顺序读写），查找是 O(1)。
 * 只做线性扫描时，`md_compact_read` 对每个 region_id 各查一次 ⇒ O(N²)
 * （N = 2048，约 400 万次比较 ≈ 100 ms），元数据压缩期间主循环被拖住、USB
 * 事件得不到服务（表现为设备消失）。慢路径保留给非稠密/历史格式。 */
static WORD md_find_index(const md_dir_cache_t *dir, BYTE type, WORD id)
{
    if ((id < dir->count) && (dir->entries[id].type == type) && (dir->entries[id].id == id))
    {
        return id;
    }

    for (WORD i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            return i;
        }
    }

    return dir->count;
}

/* md_find_entry — 线性查 (type,id) 的登记位置；未找到返回 NAND_PBA_INVALID。
 * Directory 当前只登记 L2P_REGION；其余元数据是固定位置，不经过本函数。 */
DWORD md_find_entry(const md_dir_cache_t *dir,
                    BYTE                  type,
                    WORD                  id)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir。 */

    const WORD i = md_find_index(dir, type, id);

    return (i < dir->count) ? dir->entries[i].pba : NAND_PBA_INVALID;
}

WORD md_get_live_count(const md_dir_cache_t *dir, WORD gblock)
{
    if (gblock >= FTL_BLOCK_TOTAL)
    {
        return 0U; /* 来源可能是介质/快照（H29）：越界按“无引用”处理 */
    }
    return dir->l2p_live[gblock];
}

/* --------------------------------------------------------------------
 * md_update
 *   功能：更新 (type,id) 的登记位置；已有则覆盖，无则尾部追加；置 dirty。
 *   参数：dir —— Directory；type/id —— 元数据标识；new_pba —— 新位置。
 *   返回：无。
 *   实现要点：容量满时**不追加**并 LOGE。对本项目的 L2P_REGION 不可达
 *             （容量 == region 数，全部在 fill_fixed 已登记）。
 * -------------------------------------------------------------------- */
void md_update(md_dir_cache_t *dir,
               BYTE            type,
               WORD            id,
               DWORD           new_pba)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir。 */

    /* 查找现有 entry（覆盖旧位置） */
    const WORD i = md_find_index(dir, type, id);

    if (i < dir->count)
    {
        dir->entries[i].pba = new_pba;
        dir->dirty          = true;
        dir->dirty_gen++;
        return;
    }

    /* 新增 entry（往 entries 数组尾部追加）*/
    if (dir->count < METADATA_DIR_ENTRIES)
    {
        dir->entries[dir->count].type = type;
        dir->entries[dir->count].id   = id;
        dir->entries[dir->count].pba  = new_pba;
        dir->count++;
        dir->dirty = true;
        dir->dirty_gen++;
        return;
    }

    /* 满：不追加。L2P_REGION 按构造不可达；新增元数据类型前必须先扩容量。
     * 静默丢弃会让调用者以为位置已记录 ⇒ 至少留下错误日志。 */
    LOGE(LOG_TAG_META "directory full: type=%u id=%u dropped\r\n", (unsigned)type, (unsigned)id);
}

/* ══════════════════════════════════════════════════════════════════
 * 四、L2P 日志块：追加槽 / 登记
 *（回收 / 压缩在 metadata_job.c；本区只维护写指针与 Directory 登记）
 * ══════════════════════════════════════════════════════════════════ */

/* ── 固定 L2P 日志区：分配 / 归还 / 开机恢复（RISK-MD-07 / RISK-CAP-02）── */

WORD md_log_alloc(md_dir_cache_t *dir, alc_t *alloc, BYTE plane)
{
    ASSERT(dir != NULL);
    ASSERT(alloc != NULL);
    ASSERT(plane < PLANE_NUM);

    for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
    {
        const WORD   g = ftl_gblock(plane, block);
        bst_block_t *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }
        /* 只有“已擦净、空闲”的固定日志块可分配（NONE 是本区的 free 标记）。 */
        if (ftl_block_state(b) != FTL_BLOCK_STATE_NONE)
        {
            continue;
        }
        if (dir->cur_block_valid && (g == dir->cur_metadata_block))
        {
            continue;
        }

        ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        return g;
    }

    /* 固定区无空闲块：从 **OP 池**有界借用动态日志块（L2P_LOG_BORROW_MAX）。
     * 不借则压缩没有目标块、CP 永远拿不到槽（RISK-MD-08）。借出的块由 l2p_live /
     * ftl_is_metadata_block 动态识别，擦空后由 md_log_release 回 OP 池。 */
    if (dir->log_borrow_count < L2P_LOG_BORROW_MAX)
    {
        const WORD borrowed = alc_alloc_log_block(alloc, plane);

        if (borrowed != FTL_GBLOCK_INVALID)
        {
            dir->log_borrow_count++;
            return borrowed;
        }
    }

    return FTL_GBLOCK_INVALID;
}

void md_log_release(md_dir_cache_t *dir, WORD gblock)
{
    ASSERT(dir != NULL);
    ASSERT(gblock < FTL_BLOCK_TOTAL);

    alc_t *alloc = dir->alloc;

    bst_block_t *b = &alloc->blocks[gblock];

    /* 固定日志区内的块 ⇒ 留在固定区（NONE）；借来的块 ⇒ 回 OP 池。 */
    const bool borrowed = !ftl_is_l2p_log_block(ftl_gblock_block(gblock));

    if (borrowed && (dir->log_borrow_count > 0U))
    {
        dir->log_borrow_count--;
    }

    /* BAD 是终态：擦除失败后不得“释放”回可用池。 */
    if (ftl_block_is_bad(b))
    {
        return;
    }

    if (borrowed)
    {
        /* 借来的动态日志块回 OP 池（与 alc_alloc_log_block 成对）；OP 满则回 Free Pool。 */
        ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
        ftl_block_clear_valid(b);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

        const BYTE plane = ftl_gblock_plane(gblock);

        if (!alc_op_try_push(alloc, plane, gblock))
        {
            alc_free_block(alloc, gblock);
        }
        return;
    }

    ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
    ftl_block_clear_valid(b);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

/* 固定 L2P 日志区的**空闲块数**（state == NONE 且非 BAD）。
 * 压缩触发的主判据：按 free 水位压，而不是等 live 涨到接近区上限——
 * live 接近上限时压缩已经没有目标块，只能死锁（RISK-MD-08）。 */
WORD md_log_area_free_count(const alc_t *alloc)
{
    ASSERT(alloc != NULL);

    WORD n = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            const WORD         g = ftl_gblock(plane, block);
            const bst_block_t *b = &alloc->blocks[g];

            if (ftl_block_is_bad(b))
            {
                continue;
            }
            if (ftl_block_state(b) == FTL_BLOCK_STATE_NONE)
            {
                n++;
            }
        }
    }

    return n;
}

WORD md_get_live_block_count(const md_dir_cache_t *dir)
{
    ASSERT(dir != NULL);

    WORD n = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            if (dir->l2p_live[ftl_gblock(plane, block)] > 0U)
            {
                n++;
            }
        }
    }

    return n;
}

/* 判固定日志块的整块是否已擦净：逐页读 META 判空。
 * ⚠ 不能只看 page 0：日志是顺序追加、写失败页只消耗槽位不关闭块 ⇒ 可能
 *   “page 0 空、后面有数据”（与 RISK-BOOT-14 的数据块假设不同）。把这种块当
 *   空闲分配会在未擦页上编程 = 静默损坏。首块非空即提前返回。 */
static bool md_log_block_erased(const alc_t *alloc, BYTE plane, WORD block)
{
    BYTE raw[PAGE_SIZE];

    for (BYTE page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        const DWORD pba = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, page, 0U);

        if (!ftl_page_is_blank_raw(pba, raw))
        {
            return false;
        }
    }

    return true;
}

void md_log_area_restore(md_dir_cache_t *dir, alc_t *alloc)
{
    ASSERT(dir != NULL);
    ASSERT(alloc != NULL);

    WORD n_live    = 0U;
    WORD n_free    = 0U;
    WORD n_reclaim = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            const WORD   g = ftl_gblock(plane, block);
            bst_block_t *b = &alloc->blocks[g];

            if (ftl_block_is_bad(b))
            {
                continue;
            }

            const bool cur  = dir->cur_block_valid && (g == dir->cur_metadata_block);
            const bool live = (dir->l2p_live[g] > 0U);

            if (live || cur)
            {
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                n_live++;
                continue;
            }

            /* 非活块：Directory 不引用它。整块逐页判空 ⇒ 已是擦净的空闲块；否则残留
             * 旧数据，登记回收由后台擦除，完成后再 md_log_release 置 NONE。 */
            if (md_log_block_erased(alloc, plane, block))
            {
                md_log_release(dir, g);
                n_free++;
            }
            else
            {
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                (void)md_reclaim_mark(dir, g);
                n_reclaim++;
            }
        }
    }

    /* ── 动态借用块对账（用户区、非固定区）──
     * ① live>0 但 state 不是 LOG（CP 快照早于借用）：补置 LOG，防止被池化/重建误用；
     * ② live==0 且 state==LOG（借用块已搬空、掉电前没来得及擦/归还）：登记回收，
     *    擦除后 md_log_release 回 OP；在此之前占用借用配额，避免无限借。
     * 没有这一遍，② 会成为“有借不还”的孤儿块（不在任何池、也无回收登记）。 */
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const BYTE   plane = ftl_gblock_plane(g);
        const WORD   block = ftl_gblock_block(g);
        bst_block_t *b     = &alloc->blocks[g];

        if (ftl_is_metadata_block(NULL, plane, block) || ftl_block_is_bad(b))
        {
            continue;
        }

        if (dir->l2p_live[g] > 0U)
        {
            if (ftl_block_state(b) != FTL_BLOCK_STATE_LOG)
            {
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            }
        }
        else if (ftl_block_state(b) == FTL_BLOCK_STATE_LOG)
        {
            if (md_reclaim_mark(dir, g) && (dir->log_borrow_count < UINT16_MAX))
            {
                dir->log_borrow_count++;
            }
        }
    }

    LOGI(LOG_TAG_META "L2P log area: live=%u free=%u reclaim=%u borrow=%u (total=%u/plane)\r\n",
         (unsigned)n_live,
         (unsigned)n_free,
         (unsigned)n_reclaim,
         (unsigned)dir->log_borrow_count,
         (unsigned)L2P_LOG_BLOCKS_PER_PLANE);

    /* 固定区没有任何空闲块：运行期可以从 **OP 池**有界借用（L2P_LOG_BORROW_MAX）
     * 打破“压缩没有目标块”的死锁；只有 OP 池也空时才真正需要格式化。 */
    if (n_free == 0U)
    {
        LOGW(LOG_TAG_META "L2P log area exhausted (free=0, reclaim=%u) — dynamic borrow up to %u blocks\r\n",
             (unsigned)n_reclaim,
             (unsigned)L2P_LOG_BORROW_MAX);
    }
}

/*
 * md_log_rotate - 换一个新的当前写块（追加路径写满 / 压缩缺目标槽时调用）。
 *
 * 返回：true = 已换到新块（cur_metadata_page == 0，可发放槽位）；
 *       false = 没换（固定日志区无空闲块，或活块数超水位请求了压缩）。
 * 实现要点：
 *   · 旧当前块已无活 region ⇒ 登记待回收；扫描全池只登记、不擦。
 *   · plane 从 cur_metadata_plane 起逐个尝试（H39：不写死两 plane），只从**固定
 *     L2P 日志区**取块（md_log_alloc），不碰用户 Free Pool。
 *   · 活日志块数超过 L2P_LOG_COMPACT_TRIGGER 时只登记 `job.compact_wanted` 并返回
 *     false——新块整块空槽，正好把多个稀疏 victim 一次搬空。
 *   · 压缩 job 也调用本函数：当前块空槽装不下 victim 时，新块的整块空槽是压缩唯一
 *     的 destination；换块原本只由追加触发，host 写一停就会永久缺目标槽。
 */
bool md_log_rotate(md_dir_cache_t *dir, alc_t *alloc)
{
    /* 当前块已写满且无活 region：先从"当前写块"身份上摘下来，否则 mark_all 会把它
     * 永远排除；日志区没有空闲块时就再也换不了块（死等）。 */
    if (dir->cur_block_valid && (dir->cur_metadata_page >= PAGES_PER_BLOCK) &&
        (dir->l2p_live[dir->cur_metadata_block] == 0U))
    {
        (void)md_reclaim_mark(dir, dir->cur_metadata_block);
        dir->cur_block_valid = false;
    }

    /* 换块时机扫一遍全池：只**登记**零活 LOG 块，绝不在这里擦。
     * 擦除由后台 job 异步做——同步 tERASE ~3 ms 会击穿服务预算。 */
    md_reclaim_mark_all(dir, alloc);

    /* 只剩最后 1 个空闲块：先登记压缩再把它用作当前块——新块有整块空槽，
     * 压缩才有目标可搬。free 水位判据见 L2P_LOG_FREE_WATERMARK。 */
    if (md_log_area_free_count(alloc) <= 1U)
    {
        dir->handle.compact_wanted = true;
    }

    const BYTE start     = dir->cur_metadata_plane;
    BYTE       plane     = start;
    WORD       gblock    = 0U;
    bool       allocated = false;

    for (BYTE attempt = 0U; attempt < PLANE_NUM; attempt++)
    {
        const BYTE p = ((start + attempt) % PLANE_NUM);

        const WORD g = md_log_alloc(dir, alloc, p);

        if (g != FTL_GBLOCK_INVALID)
        {
            gblock    = g;
            plane     = p;
            allocated = true;
            break;
        }
    }

    if (!allocated)
    {
        /* 固定日志区无空闲块：所有块都是活/当前/待擦/BAD。**必须在这里请求压缩**——
         * 只返回 false 而不置 compact_wanted 时，后台整理 job 无事可做，append_slot_pair
         * 永远返回 INVALID，CP 在 CP_FLUSH_REGIONS 每 10~25ms abort 一次、写不成 Header
         * ⇒ 掉电后每次开机全量重建（实测 `free=3241 cmd=0 buf=0` 仍 stall）。
         * 压缩/回收 job 会腾块；这里不能回退到用户 Free Pool。 */
        dir->handle.compact_wanted = true;
        LOGD(LOG_TAG_META "log area exhausted: live=%u reclaim=%u cur_page=%u — request compact\r\n",
             (unsigned)md_get_live_block_count(dir),
             (unsigned)dir->reclaim_pending_count,
             (unsigned)dir->cur_metadata_page);
        return false;
    }

    const WORD old_cur = dir->cur_metadata_block;
    const bool had_cur = dir->cur_block_valid;

    dir->cur_metadata_block = gblock;
    dir->cur_metadata_page  = 0U;
    dir->cur_block_valid    = true;
    dir->cur_metadata_plane = ((plane + 1U) % PLANE_NUM);

    /* 旧当前块可能已被搬空（压缩只搬活 region，零活块不会成为 victim）：
     * 换走后它再也不会被追加，登记给后台回收。 */
    if (had_cur && (dir->l2p_live[old_cur] == 0U))
    {
        (void)md_reclaim_mark(dir, old_cur);
    }

    /* 抗碎片：刚换到新日志块 ⇒ 块内有整 PAGES_PER_BLOCK 个空槽，正好够把多个稀疏
     * victim 一次搬空。触发判据是**元数据空间水位**（live 日志块数），不是用户
     * Free Pool——旧实现只在 free≤32 时压，运行期大部分时间不压，日志碎片从理想
     * 32 块一路涨到上千块，把用户容量吃光（RISK-MD-07 / RISK-CAP-02）。
     * 只**登记请求**，本次不发放槽位：调用者收到 NAND_PBA_INVALID 返回 FTL_AGAIN，
     * 下轮重试；若 job 已起则继续让路直到压缩走完。 */
    if (md_get_live_block_count(dir) > L2P_LOG_COMPACT_TRIGGER)
    {
        dir->handle.compact_wanted = true;
        return false;
    }

    return true;
}

/* --------------------------------------------------------------------
 * md_append_slot_pair
 *   功能：一次预留主/备两个连续槽（L2P region 写点），并推进写指针。
 *   参数：dir —— Directory；alloc —— 分配器（取新块）；out —— 两个槽位置。
 *   返回：true = out[0]/out[1] 已发放；false = job 在飞 / 换块失败（未消耗槽）。
 *   实现要点：当前块无效或剩余页不足 2 ⇒ **先换块再取**（避免烧掉半对）；取到位置
 *             后**立即推进** cur_metadata_page（写失败页可能半编程不可复用）。
 *             运行期擦除全部由后台 job 异步做。
 * -------------------------------------------------------------------- */
bool md_append_slot_pair(md_dir_cache_t *dir, alc_t *alloc, DWORD out[L2P_REGION_COPIES])
{
    /* 前置条件（判据 2）：调用者传 ftl->metadata_dir / &ftl->alloc / 栈上数组。 */
    ASSERT(dir != NULL);
    ASSERT(alloc != NULL);
    ASSERT(out != NULL);

    if (!md_is_handle_idle(dir))
    {
        return false; /* 整理 job 在飞：背压语义（见 md_log_rotate 的 job 让路说明） */
    }

    if (dir->cur_block_valid && ftl_block_is_bad(&alloc->blocks[dir->cur_metadata_block]))
    {
        dir->cur_block_valid = false;
    }

    /* 剩余页不足两份 ⇒ **先换块再取**：否则第一个槽消耗后第二个换块失败，
     * 第一个槽就白烧了（RISK-BOOT-18）。 */
    if ((!dir->cur_block_valid) ||
        ((WORD)dir->cur_metadata_page + L2P_REGION_COPIES > PAGES_PER_BLOCK))
    {
        if (!md_log_rotate(dir, alloc))
        {
            return false;
        }
    }

    out[0] = md_take_slot(dir);
    out[1] = md_take_slot(dir);

    return (out[0] != NAND_PBA_INVALID) && (out[1] != NAND_PBA_INVALID);
}

/* --------------------------------------------------------------------
 * md_commit_region
 *   功能：登记 region 的新位置——旧位置活计数 -1、新位置 +1、更新 entries；旧块归零即登记回收。
 *   参数：dir —— Directory；region_id；new_pba —— 新位置。
 *   返回：无（越界/INVALID 在 release 下 no-op）。
 *   实现要点：只对**动态日志块**位置（非固定元数据块）增减活计数——INVALID（未映射）
 *             不计。旧块归零且非当前块 ⇒ 置进 `reclaim_pending` 位图，由后台 job
 *             异步擦除；位图不会像单槽那样被后来的块覆盖。
 *   ⚠ **本函数不擦 NAND**：它运行在完成派发里（`ftl_l2p_callback` 的 FLUSHING 分支），
 *     就地擦块 = 在完成泵里嵌套泵。
 * -------------------------------------------------------------------- */
/* 单个 PBA 的账本迁移：旧位置 -1、新位置 +1、旧块归零则登记回收。
 * 主/备副本共用（RISK-BOOT-18 的 L2 双副本）。 */
static void md_commit_one_copy(md_dir_cache_t *dir, DWORD old, DWORD new_pba)
{
    WORD old_g = UINT16_MAX;

    /* 旧位置必须真的可能是 L2P region 槽位：只排除非日志固定区（0..L2P_LOG_BLOCK0−1）。
     * Anchor 条目来自介质（H29），损坏条目可能指向 Anchor/CP/BBT/Journal ⇒ 跳过。
     * ⚠ **不能**用 `ftl_is_metadata_block(NULL, ...)`：L2P 日志区是固定分区，会被一起
     *   排除 ⇒ 旧位置的活计数永不递减、`l2p_live` 只增不减（RISK-MD-13）。 */
    if ((old != NAND_PBA_INVALID) && (pba_to_plane(old) < PLANE_NUM) &&
        (pba_to_block(old) >= L2P_LOG_BLOCK0))
    {
        WORD g = ftl_pba_gblock(old);

        if ((g < FTL_BLOCK_TOTAL) && (dir->l2p_live[g] > 0U))
        {
            dir->l2p_live[g]--;
            old_g = g;
        }
    }

    /* 新位置的活计数 +1 */
    {
        WORD g = ftl_pba_gblock(new_pba);

        if (g < FTL_BLOCK_TOTAL)
        {
            dir->l2p_live[g]++;
        }
    }

    /* 旧块活计数归零且非当前块 ⇒ 登记进待回收位图，由后台 job 异步擦除。
     * ⚠ 压缩期间**抑制**本登记：搬移 victim 的最后一个 region 时它会归零，此时 Anchor
     *   还没指向新位置 ⇒ 擦了就会掉电丢映射（③④ 契约）。压缩期间归零的块由 job 自己
     *   在 CP_RECLAIM 里擦。 */
    if ((old_g != UINT16_MAX) && (!md_is_compacting(dir)) && md_log_block_idle(dir, old_g))
    {
        (void)md_reclaim_mark(dir, old_g);
    }
}

/* 更新备份 PBA 并置 Directory 脏。 */
static void md_update_backup(md_dir_cache_t *dir, WORD region_id, DWORD pba)
{
    if (region_id >= METADATA_DIR_ENTRIES)
    {
        return;
    }
    dir->backup_pba[region_id] = pba;
    dir->dirty                 = true;
    dir->dirty_gen++;
}

/* 读某个 region 的备份 PBA（INVALID = 无副本）。 */
DWORD md_find_backup(const md_dir_cache_t *dir, WORD region_id)
{
    return (region_id < METADATA_DIR_ENTRIES) ? dir->backup_pba[region_id] : NAND_PBA_INVALID;
}

/* 更新**单个副本**的位置（copy 0 = 主，1 = 备）。压缩搬移按副本粒度调用。 */
void md_commit_region_copy(md_dir_cache_t *dir,
                           WORD            region_id,
                           BYTE            copy,
                           DWORD           new_pba)
{
    ASSERT(region_id < METADATA_DIR_ENTRIES);
    ASSERT(new_pba != NAND_PBA_INVALID);
    ASSERT(copy < L2P_REGION_COPIES);

    if ((region_id >= METADATA_DIR_ENTRIES) || (new_pba == NAND_PBA_INVALID) || (copy >= L2P_REGION_COPIES))
    {
        return;
    }

    if (copy == 0U)
    {
        const DWORD old = md_find_entry(dir, METADATA_TYPE_L2P_REGION, region_id);

        md_commit_one_copy(dir, old, new_pba);
        md_update(dir, METADATA_TYPE_L2P_REGION, region_id, new_pba);
    }
    else
    {
        const DWORD old = md_find_backup(dir, region_id);

        md_commit_one_copy(dir, old, new_pba);
        md_update_backup(dir, region_id, new_pba);
    }
}

/* 双副本原子提交：主/备都在**同一次 flush** 写成功后才调用。 */
void md_commit_region_pair(md_dir_cache_t *dir,
                           WORD            region_id,
                           DWORD           new_primary,
                           DWORD           new_backup)
{
    ASSERT(new_primary != new_backup);

    md_commit_region_copy(dir, region_id, 0U, new_primary);
    md_commit_region_copy(dir, region_id, 1U, new_backup);
}

/* 主副本提交（保持旧签名：压缩/兼容路径用单副本）。 */
void md_commit_region(md_dir_cache_t *dir,
                      WORD            region_id,
                      DWORD           new_pba)
{
    md_commit_region_copy(dir, region_id, 0U, new_pba);
}

/* ══════════════════════════════════════════════════════════════════
 * 五、Anchor 落盘 / 加载
 * ══════════════════════════════════════════════════════════════════ */

/* ── Anchor 落盘的三段式（唯一形态）──
 *
 * 一条 Anchor record = ANCHOR_PAGE_COUNT 页。**所有**落盘站点（Checkpoint job、
 * 日志块整理 job）都拆成 prepare →（可选换块擦）→ 逐页写 → commit 四步，每步之间
 * 可以回到服务循环。三段式把「Directory 格式」留在本文件，「状态机」留给调用者。
 */

/*
 * md_anchor_prepare - Anchor 落盘第一步：定位游标 + 取槽 + 算是否需换块擦
 *
 * 返回：MD_ANCHOR_READY = 需要写（出参全部有效）；MD_ANCHOR_CLEAN = 无需写（dir 不脏）；
 *       MD_ANCHOR_FAILED = 失败（定位失败 / 池定位失败 / 槽位越界）。
 * 副作用：游标未建立时定位并置 anchor_cursor_valid；**anchor_sequence 自增**
 *        （序号先取，再取槽）。
 * ⚠ 不动介质：换块擦除由调用者做（同步站点即刻擦；异步 job 提交异步擦命令）。
 */
md_anchor_step_e md_anchor_prepare(md_dir_cache_t *dir,
                                   md_ring_t      *out_ring,
                                   DWORD          *out_base,
                                   WORD           *out_idx,
                                   DWORD          *out_seq,
                                   bool           *out_need_erase)
{
    /* 前置条件（判据 2）：六个指针均由调用者自持。 */

    if (!dir->dirty)
    {
        return MD_ANCHOR_CLEAN; /* 无变更早退：Anchor 每写一条都消耗池内槽位，空转会让池无谓轮转 */
    }

    /* 记录本次 Anchor 覆盖的变更代数：写入期间若有新的 commit，commit 时不得把
     * dirty 清掉（否则 on-media Anchor 落后于 RAM Directory，旧块可能被误回收）。 */
    dir->anchor_gen = dir->dirty_gen;

    anchor_ring_init(dir, out_ring);

    /* 游标未建立（冷启动后首次 flush）⇒ 先定位池内最新有效 record 的下一槽 */
    if (!dir->anchor_cursor_valid)
    {
        anchor_probe_ctx_t ctx;
        WORD               newest = UINT16_MAX;

        ctx.dir  = dir;
        ctx.ring = out_ring;

        if (md_ring_locate(out_ring, anchor_record_probe, &ctx, &newest, &dir->anchor_sequence, &dir->anchor_write_idx) != FTL_OK)
        {
            return MD_ANCHOR_FAILED;
        }
        dir->anchor_cursor_valid = true;
    }

    *out_idx = dir->anchor_write_idx;
    *out_seq = ++dir->anchor_sequence;

    if (md_ring_prepare_addr(out_ring, &dir->topo, *out_idx, out_base, out_need_erase) != FTL_OK)
    {
        return MD_ANCHOR_FAILED;
    }

    return MD_ANCHOR_READY;
}

/*
 * md_anchor_page - 组第 chunk 页 Anchor 数据（纯 RAM，不碰 NAND）
 *
 * padding 归零 ⇒ 介质内容确定；本页有效条目数 = 全局有效范围 [0,count) 与本页区间的交。
 */
void md_anchor_page(const md_dir_cache_t *dir, DWORD seq, BYTE chunk, md_anchor_page_t *page)
{
    /* 前置条件（判据 2）：两个指针均由调用者自持。 */

    /* chunk 0..PRIMARY-1 承载主 PBA；PRIMARY..COUNT-1 承载 backup_pba[]（同一 id 区间）。 */
    const bool backup      = (chunk >= ANCHOR_DIR_PRIMARY_PAGES);
    const BYTE local_chunk = backup ? (BYTE)(chunk - ANCHOR_DIR_PRIMARY_PAGES) : chunk;

    WORD base_entry = (local_chunk * METADATA_DIR_ENTRIES_PER_PAGE);
    WORD n;

    if (base_entry >= dir->count)
    {
        n = 0U;
    }
    else
    {
        n = (dir->count - base_entry);
        if (n > METADATA_DIR_ENTRIES_PER_PAGE)
        {
            n = METADATA_DIR_ENTRIES_PER_PAGE;
        }
    }

    memset(page, 0, sizeof(*page));
    page->magic          = ANCHOR_MAGIC;
    page->sequence       = seq;
    page->chunk_index    = chunk;
    page->chunk_count    = ANCHOR_PAGE_COUNT;
    page->entry_count    = n;
    page->layout_version = FTL_MEDIA_LAYOUT_VERSION;

    for (WORD k = 0U; k < METADATA_DIR_ENTRIES_PER_PAGE; k++)
    {
        if (k < n)
        {
            page->entries[k] = dir->entries[base_entry + k];
            if (backup)
            {
                /* 备份半：type/id 沿用主条目，pba 换成本 region 的第二副本位置。 */
                page->entries[k].pba = dir->backup_pba[base_entry + k];
            }
        }
        else
        {
            /* ⚠ memset 只给 0，不能替代这里的 NAND_PBA_INVALID 哨兵：
             *   尾部条目的 pba 必须是 0xFFFFFFFF，否则会被当成"指向 block 0"的映射。 */
            page->entries[k].type = 0U;
            page->entries[k].id   = 0U;
            page->entries[k].pba  = NAND_PBA_INVALID;
        }
    }
}

/*
 * md_anchor_commit - Anchor 落盘收尾：推进游标（无论成败），成功且未过期才清 dirty
 *
 * ⚠ **无论成败都推进游标**：已写过的槽（可能半编程）不可复用。ok == false 或
 *   `dirty_gen` 在写入期间又前进（期间有新的 commit）时保留 dirty ⇒ 下次在新槽重试整条。
 */
void md_anchor_commit(md_dir_cache_t *dir, const md_ring_t *ring, WORD idx, bool ok)
{
    /* 前置条件（判据 2）：两个指针均由调用者自持。 */

    dir->anchor_write_idx = ((idx + 1U) % md_ring_total_records(ring));

    if (ok && (dir->dirty_gen == dir->anchor_gen))
    {
        dir->dirty = false;
    }
}

/* 读胜者的全部页，逐页校验后覆盖对应区间。
 * 镜像池：每页先读首选 plane；该页不可信时回退到另一个 plane（sequence 必须仍等于
 * 被选中的 record sequence，绝不跨 record 拼页）。 */
static void md_load_assemble(md_dir_cache_t *dir, const md_ring_t *ring, WORD newest)
{
    md_anchor_page_t page;
    const BYTE       plane_count = md_ring_plane_count(ring->plane_mask);

    for (BYTE chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
    {
        const bool backup      = (chunk >= ANCHOR_DIR_PRIMARY_PAGES);
        const BYTE local_chunk = backup ? (BYTE)(chunk - ANCHOR_DIR_PRIMARY_PAGES) : chunk;
        const WORD base_entry  = (WORD)(local_chunk * METADATA_DIR_ENTRIES_PER_PAGE);

        for (BYTE pi = 0U; pi < plane_count; pi++)
        {
            const BYTE plane = md_ring_plane_at(ring->plane_mask, pi);
            DWORD      addr  = md_ring_addr_plane(ring, &dir->topo, newest, chunk, plane);

            if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
            {
                continue;
            }
            const bool header_ok = anchor_header_fields_valid(page.magic, page.chunk_count, page.layout_version);

            if ((!header_ok) || (page.sequence != dir->anchor_sequence) || (page.chunk_index != chunk))
            {
                continue; /* 该副本不可信：试另一个 plane；都失败则保留基础层的"未映射" */
            }

            WORD n = ((page.entry_count < METADATA_DIR_ENTRIES_PER_PAGE) ? page.entry_count : METADATA_DIR_ENTRIES_PER_PAGE);

            for (WORD k = 0U; (k < n) && ((base_entry + k) < METADATA_DIR_ENTRIES); k++)
            {
                if (!backup)
                {
                    dir->entries[base_entry + k] = page.entries[k];
                }
                else if ((page.entries[k].type == METADATA_TYPE_L2P_REGION) &&
                         (page.entries[k].id == (base_entry + k)))
                {
                    /* 备份半 best-effort：缺页/损坏的页保留基础层的 INVALID（单副本运行）。 */
                    dir->backup_pba[base_entry + k] = page.entries[k].pba;
                }
                else
                {
                    /* 非本 region 的条目：忽略（介质输入不可信，H29）。 */
                }
            }
            break; /* 本页已从某个 plane 取到有效副本 */
        }
    }
}

/* --------------------------------------------------------------------
 * md_load
 *   功能：开机加载 Directory——先铺"全部未映射"基础层，再定位并读回最新有效 Anchor 覆盖 entries。
 *   参数：dir —— Directory（含 topo）。
 *   返回：FTL_OK 成功；FTL_ERROR 池内无有效 Anchor（调用者转 md_init）。
 *   实现要点：先 fill_fixed 铺基础层（Anchor 缺页/损坏时该区间回落"未映射"，不出现空洞）；
 *             定位失败或无有效记录 ⇒ 清游标返回错误；读胜者逐页校验，坏页保留基础层；
 *             **日志块游标不跨重启恢复**；Anchor 游标由 locate 结果恢复。
 * -------------------------------------------------------------------- */
ftl_status_e md_load(md_dir_cache_t *dir, alc_t *alloc, const DWORD *total_writes)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir。 */
    ASSERT(alloc != NULL);
    ASSERT(total_writes != NULL);

    dir->alloc        = alloc;
    dir->total_writes = total_writes;

    /* 基础层：全部 region = NAND_PBA_INVALID（未映射）。
     * Anchor 缺页/损坏时该区间回落"未映射"，不会出现"未登记"的空洞。 */
    md_fill_fixed(dir);
    md_rebuild_live(dir); /* 失败路径也要给出确定的活计数（开机日志用）*/

    md_ring_t          ring;
    anchor_probe_ctx_t ctx;
    WORD               newest = UINT16_MAX;

    anchor_ring_init(dir, &ring);
    ctx.dir  = dir;
    ctx.ring = &ring;

    {
        const ftl_status_e loc = md_ring_locate(&ring,
                                                anchor_record_probe,
                                                &ctx,
                                                &newest,
                                                &dir->anchor_sequence,
                                                &dir->anchor_write_idx);

        /* 观测口（**INFO**，随开机必打印）：Anchor 读侧结论。
         * loc=0 且 newest=65535 ⇒ 池内一条有效 record 都没有（写没落盘 / 读不回来？）。
         * loc=0 且 newest<N ⇒ 找到了，看 seq 是否与写侧对得上。
         * ⚠ 必须 INFO：本函数在 ftl_init 里跑，早于 CLI ⇒ `log level debug` 是开机后
         *   才敲的，LOGD 在此处永远看不到。*/
        LOGI(LOG_TAG_META "anchor load: loc=%d newest=%u seq=%lu\r\n", (int)loc, (unsigned)newest, dir->anchor_sequence);

        if ((loc != FTL_OK) || (newest == UINT16_MAX))
        {
            dir->anchor_sequence     = 0U;
            dir->anchor_write_idx    = 0U;
            dir->anchor_cursor_valid = false;
            dir->dirty               = false;
            dir->dirty_gen           = 0U;
            dir->anchor_gen          = 0U;
            md_boot_log(dir);
            return FTL_ERROR; /* 无有效 Anchor：调用者转 md_init */
        }
    }

    dir->anchor_cursor_valid = true;

    md_load_assemble(dir, &ring, newest);

    dir->dirty      = false;
    dir->dirty_gen  = 0U;
    dir->anchor_gen = 0U;

    /* 由装配后的 entries 重建活计数；**日志块游标不跨重启恢复**：首次追加时从**固定
     * L2P 日志区**新取一块（绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒
     * 由 l2p_live 保住，待其 region 全部改写后由后台整理 job 异步擦除归还。 */
    md_rebuild_live(dir);
    dir->cur_metadata_block = 0U;
    dir->cur_metadata_page  = 0U;
    dir->cur_block_valid    = false;
    dir->cur_metadata_plane = 0U;
    md_reclaim_reset(dir); /* 待回收位图不跨重启（同 job：全是 RAM 态）*/
    md_handle_reset(dir);  /* 整理 job 不跨重启（同游标：phase / 游标 / 请求位全是 RAM 态）*/

    md_boot_log(dir);

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 六、元数据块判定
 * ══════════════════════════════════════════════════════════════════ */

/*
 * 固定元数据分区表 —— 单一事实来源（类型声明在 metadata.h）
 *
 * 区间为 [first_block, first_block + block_count)；**各 (ch,ce,lun) 用同一份块号**
 * （分片模型：每个 die 一套完整元数据，分区不随维度错开）。
 *
 * Anchor / Checkpoint / BBT 都是**多块轮转池**：池内每块容纳 `records_per_block`
 * 条 record，块满换池内下一块（先擦后写）。
 *
 * 每项完全由 metadata.h 的宏派生——块号 / 块数 / 每块 record 数 / 表字节均无手写数字。
 */
static const md_partition_t g_ftl_partitions[] = {
  /* name, first_block, block_count, plane_mask, redundancy, table_bytes */
    {    "Anchor",     ANCHOR_BLOCK0,       ANCHOR_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK,         ANCHOR_RECORDS_PER_BLOCK,                       FTL_ANCHOR_TABLE_BYTES},
    {"Checkpoint", CHECKPOINT_BLOCK0,   CHECKPOINT_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK, FTL_CHECKPOINT_RECORDS_PER_BLOCK,                   FTL_CHECKPOINT_TABLE_BYTES},
    {       "BBT",         BBT_BLOCK,          BBT_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK,                  PAGES_PER_BLOCK,                          FTL_BBT_TABLE_BYTES},
    {   "Journal",    JOURNAL_BLOCK0,      JOURNAL_BLOCK_COUNT, METADATA_MIRROR_PLANE_MASK,                               1U,                      FTL_JOURNAL_TABLE_BYTES},
 /* L2P region 日志区：固定预留（RISK-MD-07/CAP-02）。redundancy 填 1（不是 record 池）；
  * table_bytes = region 数 × 每个 region 的 2 KB。 */
    {   "L2P log",    L2P_LOG_BLOCK0, L2P_LOG_BLOCKS_PER_PLANE, METADATA_MIRROR_PLANE_MASK,                               1U, (L2P_REGIONS_PER_INSTANCE * L2P_REGION_SIZE)},
};

/* 分区数（表长）。sizeof 派生 ⇒ 加/删分区项时自动跟随。 */
static const DWORD g_ftl_partition_count = (sizeof(g_ftl_partitions) / sizeof(g_ftl_partitions[0]));

/* --------------------------------------------------------------------
 * ftl_is_metadata_block
 *   功能：判定 (plane, per-plane 块号 block) 是否属于元数据（固定分区 / 当前日志块 /
 *         已登记日志块）。
 *   参数：md_dir —— Directory（可 NULL：只判固定分区）；plane —— 物理 plane 号；
 *         block —— per-plane 块号。
 *   返回：true 是元数据块；false 不是。
 *   实现要点：三分支——
 *     ① 固定保留区：`block < FTL_METADATA_AREA_END`（含 shared spare；名义 BAD 块也保留）；
 *     ② 当前日志块（刚分配还没 commit，Directory 扫不到，必须显式排除）；
 *     ③ 已登记日志块：用 `l2p_live[g] != 0` 做 O(1) 判定（该账本的定义就是
 *        "最新位置落在 g 的 region 数"）。debug 档保留全表扫描做差分自检。
 *   ②③ 是**从用户区动态取的**块 ⇒ 必须带 plane 比较；只比块号会连带掩掉另一 plane
 *   的同号块（既出不了 free pool、又被 full metadata rebuild 跳过，白丢一个可用块）。
 * -------------------------------------------------------------------- */
bool ftl_is_metadata_block(const md_dir_cache_t *md_dir, BYTE plane, WORD block)
{
    /* ── 固定元数据块：v11 起 0..FTL_METADATA_AREA_END-1 **全部保留** ──
     * 包含 shared metadata spare；名义块即使 BAD 也不回归数据池（否则 BBT 记录的
     * 坏块会被 carve 当用户块再分出去）。这比逐分区区间判断更简单，且与候选池
     * 的物理块集合一致（C9：一个分类坐标）。 */
    if (block < FTL_METADATA_AREA_END)
    {
        return true;
    }

    /* md_dir == NULL：只判固定分区（开机 carve / recovery 的"固定块"判据用）。
     * 这不是错误——调用者明确只要固定分区视图。 */
    if (md_dir == NULL)
    {
        return false;
    }

    /* ── 当前日志块：可能尚无任何 region 指向它（刚分配、还没 commit），
     *    Directory 扫不到 ⇒ 必须显式排除，否则会被 Free Pool 回填当用户块收走。 ── */
    if (md_dir->cur_block_valid && (plane == ftl_gblock_plane(md_dir->cur_metadata_block)) &&
        (block == ftl_gblock_block(md_dir->cur_metadata_block)))
    {
        return true;
    }

    /* ── 已登记日志块：l2p_live 是 Directory 条目的账本，O(1) 判定 ──
     * block 可能来自损坏介质：先挡掉越界值，避免 ftl_gblock 截断后误判。 */
    const bool in_range = (block < BLOCKS_PER_PLANE);
    const WORD g        = in_range ? ftl_gblock(plane, block) : FTL_BLOCK_TOTAL;
    const bool fast     = in_range && (g < FTL_BLOCK_TOTAL) && (md_dir->l2p_live[g] != 0U);

#ifndef NDEBUG
    /* 差分自检：证明 O(1) 账本判据与 Directory 逐条扫描完全等价。release 编译期消失。
     * 扫描使用与 md_rebuild_live 相同的过滤条件（挡越界 / 固定块），
     * 避免损坏介质上的条目让调试版误报。 */
    bool slow = false;

    for (WORD i = 0U; (!slow) && (i < md_dir->count); i++)
    {
        if (md_dir->entries[i].type != METADATA_TYPE_L2P_REGION)
        {
            continue;
        }

        /* 主/备两份都参与差分（副本只占备份位置时，只扫主会让 fast != slow）。 */
        const WORD rid = md_dir->entries[i].id;

        for (BYTE copy = 0U; (!slow) && (copy < L2P_REGION_COPIES); copy++)
        {
            const DWORD pba = (copy == 0U)
                                  ? md_dir->entries[i].pba
                                  : ((rid < METADATA_DIR_ENTRIES) ? md_dir->backup_pba[rid] : NAND_PBA_INVALID);

            if (pba == NAND_PBA_INVALID)
            {
                continue;
            }

            const BYTE ep = pba_to_plane(pba);
            const WORD eb = pba_to_block(pba);

            if ((ep >= PLANE_NUM) || (eb >= BLOCKS_PER_PLANE) ||
                ftl_is_metadata_block(NULL, ep, eb))
            {
                continue;
            }
            if ((ep == plane) && (eb == block))
            {
                slow = true;
            }
        }
    }
    ASSERT(fast == slow);
#endif

    return fast;
}

/* --------------------------------------------------------------------
 * md_pool_print - 打印固定元数据分区的候选块池（CLI `pool` / 负例验证）
 *
 *   输出每个分区的 nominal / 实际候选数，以及因 BAD 被 shared spare 顶替的块。
 *   只读 RAM，不碰介质。
 * -------------------------------------------------------------------- */
void md_pool_print(const md_dir_cache_t *dir)
{
    if ((dir == NULL) || (dir->alloc == NULL) || (dir->alloc->pools == NULL))
    {
        LOGW(LOG_TAG_POOL "not initialized\r\n");
        return;
    }

    for (DWORD p = 0U; p < g_ftl_partition_count; p++)
    {
        const md_partition_t *part = &g_ftl_partitions[p];

        if (part->first_block == L2P_LOG_BLOCK0)
        {
            DWORD good = 0U;

            for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
            {
                for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
                {
                    if (!ftl_block_is_bad(&dir->alloc->blocks[ftl_gblock(plane, block)]))
                    {
                        good++;
                    }
                }
            }

            LOGI(LOG_TAG_POOL "%-10s range=%u..%u good(plane-blocks)=%lu\r\n",
                 part->name,
                 (unsigned)L2P_LOG_BLOCK0,
                 (unsigned)(L2P_LOG_AREA_END - 1U),
                 good);
            continue;
        }

        const md_ring_t *ring = NULL;

        if (part->first_block == ANCHOR_BLOCK0)
        {
            ring = &dir->pools.anchor;
        }
        else if (part->first_block == CHECKPOINT_BLOCK0)
        {
            ring = &dir->pools.cp;
        }
        else if (part->first_block == BBT_BLOCK)
        {
            ring = &dir->pools.bbt;
        }
        else if (part->first_block == JOURNAL_BLOCK0)
        {
            ring = &dir->pools.jrn;
        }

        if (ring == NULL)
        {
            continue;
        }

        LOGI(LOG_TAG_POOL "%-10s nominal=%u candidates=%u\r\n",
             part->name,
             (unsigned)ring->nominal_count,
             (unsigned)ring->block_count);

        for (WORD i = 0U; i < ring->block_count; i++)
        {
            const WORD b = ring->blocks[i];

            if ((b < ring->first_block) || (b >= (ring->first_block + ring->nominal_count)))
            {
                LOGI(LOG_TAG_POOL "%-10s remapped -> spare block=%u (candidate_idx=%u)\r\n",
                     part->name,
                     (unsigned)b,
                     (unsigned)i);
            }
        }
    }
}

/* 候选池重建日志节流：第 1 次立即打印，此后每 N 次一条（元数据坏块风暴不刷屏）。
 * N=16：重建本身较贵且罕见，16 条足以覆盖一次故障风暴。 */
#define MD_POOL_REBUILT_LOG_EVERY (16U)
static DWORD s_pool_rebuilt_logged;

void md_pool_service(struct ftl_context *ftl)
{
    /* 前置条件（判据 2）：唯一调用者 ftl_service 传 &g_ftl[...]。 */
    alc_t          *alloc = &ftl->alloc;
    md_dir_cache_t *dir   = ftl->metadata_dir;

    if ((alloc->pools == NULL) || !alloc->pools->dirty || ftl->bg_suspend)
    {
        return; /* bg_suspend：bbt --erase / format 期间 RAM 状态已不可信，不重建 */
    }

    /* 等元数据写路径空闲：换游标可能让在飞命令的落点与新候选表不一致。
     * 单线程 superloop ⇒ 这里不会与完成回调并发；只是在下一轮再试。 */
    if (ftl->cp_handle->active || ftl->flush_active || ftl->mig_handle->active ||
        (ftl->journal->handle.phase != JRN_IDLE) || alloc->bbt.handle.waiting ||
        (dir->handle.phase != MD_JOB_IDLE) || dir->handle.waiting)
    {
        return;
    }

    /* 旧 Journal 候选表快照：只有它真的变了才复位环形游标（否则不应丢弃缓冲页）。 */
    WORD old_jrn[JOURNAL_BLOCK_COUNT];

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        old_jrn[b] = ftl->journal->blocks[b];
    }

    if (!md_pool_build_all(alloc))
    {
        /* 元数据区已无足够好块：停止重试（避免每轮刷屏），下次开机由 BBT/rebuild 兜底。 */
        alloc->pools->dirty         = false;
        alloc->bbt.rebuild_required = true;
        bbt_note_dirty(alloc);
        LOGE(LOG_TAG_POOL "rebuild failed — metadata area exhausted, rebuild required\r\n");
        return;
    }

    /* 游标全部作废：下一次提交/定位会按新候选表重新扫池。 */
    dir->anchor_cursor_valid              = false;
    alloc->bbt.cursor_valid               = false;
    ftl->cp_handle->cp_state.cursor_valid = false;

    /* Journal 候选表变了 ⇒ 环形下标到物理块的映射已变。不能把游标简单归零
     * （新下标可能指向未擦块；直接写 = 二次编程）。按新候选表重新扫环、接回
     * 最新好页（jrn_scan_latest + jrn_resume），坏块上的条目由 REBUILD_REQUIRED
     * 在下次开机 full rebuild 恢复。 */
    bool jrn_changed = false;

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        if (old_jrn[b] != alloc->pools->jrn.blocks[b])
        {
            jrn_changed = true;
        }
        ftl->journal->blocks[b] = alloc->pools->jrn.blocks[b];
    }

    if (jrn_changed)
    {
        jrn_page_ref_t latest;
        bool           read_error = false;

        /* 旧下标的失败记录先清掉，否则 jrn_resume 会把新候选误判成坏块。 */
        ftl->journal->handle.bad_mask         = 0U;
        ftl->journal->handle.pre_erased_block = JOURNAL_BLOCK_NONE;
        ftl->journal->handle.erase_fail_count = 0U;
        ftl->journal->handle.erase_fail_block = JOURNAL_BLOCK_NONE;
        ftl->journal->handle.ring_full        = false; /* 新候选池可能已补回 spare */
        (void)memset(ftl->journal->handle.page_buf, 0xFF, sizeof(ftl->journal->handle.page_buf));

        (void)jrn_scan_latest(ftl->journal, &latest, &read_error);

        /* floor 取当前序号，保证 resume 不回退；merge_floor 取最近 CP 水位，
         * 阻止跨 CP 的旧条目就地合并。 */
        jrn_resume(ftl->journal, &latest, ftl->journal->handle.sequence_counter);
        ftl->journal->handle.merge_floor = ftl->cp_handle->cp_state.last_journal_sequence;

        /* 环里扫不到有效页时，jrn_resume 会把写点放到第一个可用块的 page0——
         * 该块在运行期不保证已擦（与开机 fresh 的假设不同）。挂起一次换块擦除，
         * 避免对未擦页二次编程。 */
        if (latest.page_seq == 0U)
        {
            ftl->journal->handle.target_block = ftl->journal->handle.current_block;
            ftl->journal->handle.need_erase   = 1U;
        }
    }

    /* 当前 L2P 日志块若已 BAD ⇒ 下次 append 重新分配。 */
    if (dir->cur_block_valid && ftl_block_is_bad(&alloc->blocks[dir->cur_metadata_block]))
    {
        dir->cur_block_valid = false;
    }

    /* 触发一次 Anchor 提交 + Checkpoint：把 RAM 里的权威状态重新落到新候选池。 */
    dir->dirty = true;
    jrn_request_checkpoint(ftl->journal);
    bbt_note_dirty(alloc);

    if ((s_pool_rebuilt_logged++ % MD_POOL_REBUILT_LOG_EVERY) == 0U)
    {
        LOGW(LOG_TAG_POOL "rebuilt after metadata BAD (jrn_changed=%u rebuild_required=%u count=%lu)\r\n",
             (unsigned)(jrn_changed ? 1U : 0U),
             (unsigned)(alloc->bbt.rebuild_required ? 1U : 0U),
             s_pool_rebuilt_logged);
    }
}
