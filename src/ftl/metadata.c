/*
 * metadata.c — 元数据落盘基建：record 池环形轮转 + Anchor 池 + L2P 日志块
 *
 * 职责：
 *   提供 FTL 全部持久化元数据（Anchor / Checkpoint / BBT / L2P region）共用的
 *   落盘原语：环形 record 池的寻址/定位/换块擦除；Anchor（Metadata Directory 快照）
 *   的组页/落盘/加载；L2P 日志块的追加槽/登记/回收；以及"这块是不是元数据块"的判定。
 *
 * 与 metadata.h 的分工：
 *   metadata.h —— 结构（metadata_ring_t / metadata_anchor_page_t / metadata_dir_cache_t）+
 *                 固定元数据区布局（池尺寸 / 块偏移 / 表字节 / 分区表声明 / 打包公式）；
 *   本文件     —— 上述结构的全部算法实现 + 分区表本体（g_ftl_partitions[]，单一事实来源）。
 *
 * 元数据两层（见 metadata.h「固定元数据区布局」）：
 *   固定层（哨兵）：L2P region 的初始位置，**永不写入**，读回全 0xFF = 未映射；
 *   日志层（动态）：region 刷新后从 Free Pool 取块、追加写，位置由 Directory 记账。
 *
 * 文件结构（═══ 编号大区沿用既有编号，未重排）：
 *   一、元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 *   二、Anchor 池（Metadata Directory 的 record 池）
 *   三、Directory 查询 / 更新
 *   四、L2P 日志块：追加槽 / 登记 / 回收（元数据写模型 ①②③④）+ 后台整理 job
 *   五、Anchor 落盘 / 加载
 *   六、元数据块判定
 *
 * ⚠ 同步 / 异步的分界（v0.75 阶段 8 起）：
 *   · **异步**：日志块的回收与压缩（`md_job_t` + g_op_service[FTL_OP_METADATA]）——
 *      它们原来嵌在 `l2p_cache_flush_kick → metadata_dir_append_slot / ftl_op_l2p →
 *      metadata_dir_commit_region` 这条**完成回调链**上，就地擦块就是在完成泵里嵌套泵。
 *    · **同步（残留）**：开机加载 / 格式化（metadata_dir_load / init）、
 *      Anchor 池定位（metadata_ring_locate）、append_slot 换块前的全池 sweep
 *      （metadata_dir_reclaim）。前者在 superloop 之前跑（无泵可等），后两者是
 *      后台路径且有界。
 * ⚠ 无断言——失败一律返回 FTL_ERROR / NAND_PBA_INVALID（服务函数按错误位判失败）。
 */

#include "ftl/metadata.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/checkpoint.h"
#include "ftl/ftl.h" /* ftl_context_t（服务函数要从实例取 &ftl->alloc） */
#include "ftl/ftl_fc_interface.h"
#include "ftl/ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl/ftl_op.h"  /* ftl_op_submit / FTL_OP_METADATA */
#include "ftl/journal.h"
#include "nand/onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
 * 一、元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 * ══════════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * metadata_ring_addr
 *   功能：把「池内第 record_idx 条 record 的第 page_in_record 页」换算成物理地址。
 *   参数：ring —— 池几何；topo —— ch/ce/lun；record_idx —— 池内记录序号；
 *         page_in_record —— record 内页偏移（Anchor 一条 = 8 页）。
 *   返回：nand_pba_t（plane 取 ring->plane，cw 恒 0）。
 *   实现要点：先按 records_per_block 整除得块偏移、取余得块内记录号，
 *             再乘 pages_per_record 得块内页号。地址公式的唯一出口。
 * -------------------------------------------------------------------- */
nand_pba_t metadata_ring_addr(const metadata_ring_t *ring,
                              const ftl_topology_t  *topo,
                              uint16_t               record_idx,
                              uint8_t                page_in_record)
{
    uint16_t rpb = metadata_ring_records_per_block(ring);

    return pba_make(topo->ch,
                    topo->ce,
                    topo->lun,
                    (uint32_t)(ring->first_block + (record_idx / rpb)),
                    ring->plane,
                    (uint32_t)(((record_idx % rpb) * ring->pages_per_record) + page_in_record),
                    0U);
}

/* --------------------------------------------------------------------
 * metadata_ring_locate
 *   功能：扫池，定位最新有效 record，并算出「下一个该写的槽」。
 *   参数：ring/topo —— 池与拓扑；probe —— 记录探针（廉价/整条两档）；
 *         ctx —— 探针上下文；out_newest_idx/out_sequence/out_write_idx —— 三个出参。
 *   返回：FTL_OK（池空时 out_newest_idx=UINT16_MAX、out_sequence=0 也是 OK）；
 *         FTL_ERROR 参数非法（含 ring->block_count < 2 —— 单块池无环形语义）。
 *   实现要点（六步，见正文圈码）：
 *     ① 逐块探**末条**，末条有效=该块写满 → 记录全池"最新满块"；
 *        第一个未写满的块 = 游标块；
 *     ② 全满 ⇒ 池转满一圈，游标=最新满块之后，ring_full=1；
 *     ③ 游标块内降序找最后一条有效 record；
 *     ④ 写点 = 该末条的下一条（跨块处理）；
 *     ⑤ 快路径：只在「最新满块」+「游标块末条」两个候选里整条校验（正常路径）；
 *     ⑥ 慢路径：快路径全失败（局部损坏）才全池扫描取次新，整条校验，逐轮降 limit。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_ring_locate(const metadata_ring_t *ring,
                                  const ftl_topology_t  *topo,
                                  metadata_ring_probe_fn probe,
                                  void                  *ctx,
                                  uint16_t              *out_newest_idx,
                                  uint32_t              *out_sequence,
                                  uint16_t              *out_write_idx)
{
    uint16_t rpb;
    uint16_t total;
    uint16_t b;
    uint16_t cursor_block    = UINT16_MAX;
    uint16_t best_full_idx   = UINT16_MAX;
    uint32_t best_full_seq   = 0U;
    uint16_t cursor_last_idx = UINT16_MAX;
    uint32_t cursor_last_seq = 0U;
    uint16_t cand_idx[2];
    uint32_t cand_seq[2];
    uint8_t  cand_n = 0U;
    uint8_t  i;
    int      ring_full = 0;

    if ((ring == NULL) || (topo == NULL) || (probe == NULL) || (out_newest_idx == NULL) ||
        (out_sequence == NULL) || (out_write_idx == NULL) || (ring->pages_per_record == 0U) ||
        (ring->block_count < 2U))
    {
        return FTL_ERROR;
    }

    rpb   = metadata_ring_records_per_block(ring);
    total = metadata_ring_total_records(ring);

    /* 池空时下面各出口都不写 *out_sequence ⇒ 先给确定值（调用者据此续序号）*/
    *out_sequence   = 0U;
    *out_newest_idx = UINT16_MAX;

    /* ① 逐块廉价探针该块**末条** record：末条有效 = 该块写满 */
    for (b = 0U; b < ring->block_count; b++)
    {
        uint32_t seq = 0U;
        uint16_t idx = (uint16_t)((b * rpb) + (rpb - 1U));

        if (probe(ctx, idx, 0, &seq) == FTL_OK)
        {
            if ((best_full_idx == UINT16_MAX) || (seq > best_full_seq))
            {
                best_full_idx = idx;
                best_full_seq = seq;
            }
        }
        else if (cursor_block == UINT16_MAX)
        {
            cursor_block = b; /* 第一个未写满的块 = 游标块 */
        }
    }

    /* ② 所有块都写满 ⇒ 池转满一圈：游标 = 最新块之后，且该块整体擦掉重写 */
    if (cursor_block == UINT16_MAX)
    {
        cursor_block = (uint16_t)(((best_full_idx / rpb) + 1U) % ring->block_count);
        ring_full    = 1;
    }

    /* ③ 游标块内降序找最后一条有效 record（池转满时游标块本身就是满的，不参与）*/
    if (ring_full == 0)
    {
        for (b = 0U; b < rpb; b++)
        {
            uint16_t idx = (uint16_t)((cursor_block * rpb) + (rpb - 1U - b));
            uint32_t seq = 0U;

            if (probe(ctx, idx, 0, &seq) == FTL_OK)
            {
                cursor_last_idx = idx;
                cursor_last_seq = seq;
                break;
            }
        }
    }

    /* ④ 写点 = 游标块末条的下一条（末条是块内最后一条时跨到下一块槽 0）；
     *    游标块内无有效 record ⇒ 写点 = 游标块的槽 0 */
    if (cursor_last_idx == UINT16_MAX)
    {
        *out_write_idx = (uint16_t)(cursor_block * rpb);
    }
    else if ((uint16_t)(cursor_last_idx % rpb) == (uint16_t)(rpb - 1U))
    {
        *out_write_idx = (uint16_t)((cursor_last_idx + 1U) % total);
    }
    else
    {
        *out_write_idx = (uint16_t)(cursor_last_idx + 1U);
    }

    /* ⑤ 快路径候选：全部块末条中 sequence 最大者 + 游标块末条（按 seq 降序整条校验）*/
    if (best_full_idx != UINT16_MAX)
    {
        cand_idx[cand_n] = best_full_idx;
        cand_seq[cand_n] = best_full_seq;
        cand_n++;
    }
    if ((cursor_last_idx != UINT16_MAX) && (cursor_last_idx != best_full_idx))
    {
        cand_idx[cand_n] = cursor_last_idx;
        cand_seq[cand_n] = cursor_last_seq;
        cand_n++;
    }
    if ((cand_n == 2U) && (cand_seq[1] > cand_seq[0]))
    {
        uint16_t ti = cand_idx[0];
        uint32_t ts = cand_seq[0];

        cand_idx[0] = cand_idx[1];
        cand_seq[0] = cand_seq[1];
        cand_idx[1] = ti;
        cand_seq[1] = ts;
    }

    for (i = 0U; i < cand_n; i++)
    {
        uint32_t seq = 0U;

        if (probe(ctx, cand_idx[i], 1, &seq) == FTL_OK)
        {
            *out_newest_idx = cand_idx[i];
            *out_sequence   = seq;
            return FTL_OK;
        }
    }

    /* ⑥ 慢路径（快路径候选整条校验全失败，例如局部损坏）：全池廉价探针取 sequence
     *    最大者，整条校验；失败则排除该 sequence 继续找次新。仅在损坏时付出全池代价。 */
    {
        uint32_t limit = UINT32_MAX;

        for (;;)
        {
            uint16_t ci = UINT16_MAX;
            uint32_t cs = 0U;
            uint16_t k;

            for (k = 0U; k < total; k++)
            {
                uint32_t seq = 0U;

                if (probe(ctx, k, 0, &seq) != FTL_OK)
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
                break; /* 池内已无更多候选 */
            }
            {
                /* 整条校验用独立出参：探针契约是「失败时不写 *out_sequence」，
                 * 但不能把 cs（下一步的排除界）押在这个隐含约定上。 */
                uint32_t full_seq = 0U;

                if (probe(ctx, ci, 1, &full_seq) == FTL_OK)
                {
                    *out_newest_idx = ci;
                    *out_sequence   = full_seq;
                    break;
                }
            }
            limit = cs;
        }
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * metadata_ring_prepare_addr
 *   功能：为写入池内第 write_idx 条 record 算地址，并判定是否需先擦目标块。
 *   参数：ring/topo —— 池与拓扑；write_idx —— 目标槽；out_addr —— 出参物理地址；
 *         out_need_erase —— 出参，非 0 = 写之前必须先擦 *out_addr 所在块。
 *   返回：FTL_OK；FTL_ERROR 参数非法 / write_idx 越界。
 *   实现要点：仅当 write_idx 落在块首（% rpb == 0）才需擦块。池内轮转 ⇒ 该块持全池
 *             最旧 record，且每条 record 是完整快照 ⇒ 擦掉不丢信息；块内槽 0 恒走本
 *             分支（新取/刚擦的块再擦一次是幂等无害代价）。
 *             **不动介质**：擦除由调用者做（同步站点即刻擦；异步 job 提交异步擦）。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_ring_prepare_addr(const metadata_ring_t *ring,
                                        const ftl_topology_t  *topo,
                                        uint16_t               write_idx,
                                        nand_pba_t            *out_addr,
                                        int                   *out_need_erase)
{
    if ((ring == NULL) || (topo == NULL) || (out_addr == NULL) || (out_need_erase == NULL))
    {
        return FTL_ERROR;
    }
    if (write_idx >= metadata_ring_total_records(ring))
    {
        return FTL_ERROR;
    }

    *out_addr       = metadata_ring_addr(ring, topo, write_idx, 0U);
    *out_need_erase = ((uint16_t)(write_idx % metadata_ring_records_per_block(ring)) == 0U) ? 1 : 0;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * metadata_ring_prepare_write
 *   功能：为写入池内第 write_idx 条 record 做前置——算地址，必要时先擦目标块。
 *   参数：ring/topo —— 池与拓扑；write_idx —— 目标槽；out_addr —— 出参物理地址。
 *   返回：FTL_OK；FTL_ERROR 参数非法 / write_idx 越界 / 换块时擦除失败。
 *   实现要点：= metadata_ring_prepare_addr + 同步擦除。**同步**原语。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_ring_prepare_write(const metadata_ring_t *ring,
                                         const ftl_topology_t  *topo,
                                         uint16_t               write_idx,
                                         nand_pba_t            *out_addr)
{
    nand_pba_t addr;
    int        need_erase = 0;

    if (metadata_ring_prepare_addr(ring, topo, write_idx, &addr, &need_erase) != FTL_OK)
    {
        return FTL_ERROR;
    }

    if (need_erase != 0)
    {
        /* 换块：先擦目标块（池内轮转 ⇒ 该块持全池最旧 record，擦掉不丢信息） */
        if (ftl_sync_erase_block(addr) != 0)
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

/*
 * anchor_page_valid - 校验一页 Anchor：magic + chunk_count + 布局版本。
 * 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证（ADR v0.62）。
 */
static int anchor_page_valid(const metadata_anchor_page_t *page)
{
    if (page->magic != ANCHOR_MAGIC)
    {
        return 0;
    }
    if (page->chunk_count != ANCHOR_PAGE_COUNT)
    {
        return 0;
    }
    if (page->layout_version != FTL_MEDIA_LAYOUT_VERSION)
    {
        return 0; /* 旧格式介质的 Anchor ⇒ 作废（触发开机自动格式化）*/
    }

    return 1;
}

/* Anchor 池几何（per-plane 块号 ANCHOR_BLOCK0 起 ANCHOR_POOL_BLOCKS 块，plane 0）*/
static void anchor_ring_init(metadata_ring_t *ring)
{
    ring->first_block      = ANCHOR_BLOCK0;
    ring->block_count      = ANCHOR_POOL_BLOCKS;
    ring->pages_per_record = ANCHOR_PAGE_COUNT;
    ring->plane            = 0U;
}

typedef struct
{
    const metadata_dir_cache_t *dir;
    const metadata_ring_t      *ring;
} anchor_probe_ctx_t;

/*
 * anchor_record_probe - Anchor record 探针（见 metadata_ring_probe_fn）
 *
 * full == 0：只读 chunk 0 取 sequence（1 次页读）
 * full != 0：8 页全读，逐页校验 magic/chunk_count + chunk_index 序 + sequence 一致
 */
static ftl_status_t anchor_record_probe(void *ctx, uint16_t record_idx, int full, uint32_t *out_sequence)
{
    anchor_probe_ctx_t    *c = (anchor_probe_ctx_t *)ctx;
    metadata_anchor_page_t page;
    uint32_t               seq = 0U;
    uint8_t                chunk;

    for (chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
    {
        nand_pba_t addr = metadata_ring_addr(c->ring, &c->dir->topo, record_idx, chunk);

        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
        {
            return FTL_ERROR;
        }
        if ((anchor_page_valid(&page) == 0) || (page.chunk_index != chunk))
        {
            return FTL_ERROR;
        }

        if (chunk == 0U)
        {
            seq = page.sequence;
            if (seq == 0U)
            {
                return FTL_ERROR; /* sequence 从 1 起；0 = 未写 */
            }
            if (full == 0)
            {
                break; /* 廉价探针：只读第 0 页 */
            }
        }
        else if (page.sequence != seq)
        {
            return FTL_ERROR; /* 整条校验：各页 sequence 必须一致 */
        }
    }

    *out_sequence = seq;
    return FTL_OK;
}

/*
 * metadata_dir_fill_fixed - 建立全部 region 的固定位置（哨兵层，见 metadata.h「固定元数据区布局」）
 *
 * 每个实例的 L2P_REGIONS_PER_INSTANCE 个 region **全部登记**；某个 region 刷新到
 * 日志块后由 metadata_dir_commit_region 就地覆盖。固定位置**永不写入**（页保持
 * 擦除态 ⇒ 读回全 0xFF ⇒ 未映射），它只是「尚未搬到日志块」的表示。
 *
 * 共用于 metadata_dir_init（首次格式化）与 metadata_dir_load（基础层）。
 */
static void metadata_dir_fill_fixed(metadata_dir_cache_t *dir)
{
    uint16_t i;
    uint8_t  plane;
    uint16_t block;
    uint8_t  page;

    dir->count        = 0U;
    dir->have_dynamic = 0U;

    for (i = 0U; i < L2P_REGIONS_PER_INSTANCE; i++)
    {
        ftl_l2p_region_placement(i, &block, &page, &plane);

        dir->entries[dir->count].type = METADATA_TYPE_L2P_REGION;
        dir->entries[dir->count].id   = i;
        dir->entries[dir->count].pba  = pba_make(dir->topo.ch, dir->topo.ce, dir->topo.lun, block, plane, page, 0U);
        dir->count++;
    }
}

/* 由 entries 重建 l2p_live / have_dynamic（日志块 = block >= FTL_METADATA_BLOCK_END）*/
static void metadata_dir_rebuild_live(metadata_dir_cache_t *dir)
{
    uint16_t i;

    memset(dir->l2p_live, 0, sizeof(dir->l2p_live));
    dir->have_dynamic = 0U;

    for (i = 0U; i < dir->count; i++)
    {
        uint16_t block;
        uint16_t g;

        if (dir->entries[i].type != METADATA_TYPE_L2P_REGION)
        {
            continue;
        }

        block = (uint16_t)pba_to_block(dir->entries[i].pba);
        if (block < FTL_METADATA_BLOCK_END)
        {
            continue; /* 固定层（哨兵）不是日志块 */
        }

        g = ftl_gblock((uint8_t)pba_to_plane(dir->entries[i].pba), block);
        if (g < FTL_BLOCK_TOTAL)
        {
            dir->l2p_live[g]++;
        }
        dir->have_dynamic = 1U;
    }
}

/* 开机观测口（判据②「走动态分配」/ 判据③「Anchor 真的落盘」的唯一观测口）*/
static void metadata_dir_boot_log(const metadata_dir_cache_t *dir)
{
    uint16_t g;
    uint32_t live_blocks = 0U;

    for (g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] != 0U)
        {
            live_blocks++;
        }
    }

    if (dir->cur_block_valid != 0U)
    {
        LOGI("[META] anchor_seq=%u dyn=%u cur_blk=%u live_blks=%u\r\n",
             (unsigned)dir->anchor_sequence,
             (unsigned)dir->have_dynamic,
             (unsigned)dir->cur_metadata_block,
             (unsigned)live_blocks);
    }
    else
    {
        LOGI("[META] anchor_seq=%u dyn=%u cur_blk=- live_blks=%u\r\n",
             (unsigned)dir->anchor_sequence,
             (unsigned)dir->have_dynamic,
             (unsigned)live_blocks);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 三、Directory 查询 / 更新
 * ══════════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * md_job_reset
 *   功能：把后台整理 job（v0.75 阶段 8）清回 IDLE——冷启动 / 开机加载后必须调。
 *   参数：dir —— Directory（NULL 直接返回）。
 *   返回：无。
 *   实现要点：**哨兵值必须显式写**（memset 只给 0）：victim / reclaim_target /
 *             reclaim_gblock 的"无"是 UINT16_MAX，slot 的"未取"是 NAND_PBA_INVALID。
 *             ⚠ 不清的后果与旧的 compacting 标志同类：残留 phase ≠ IDLE ⇒
 *             metadata_dir_append_slot 永久让路（以为整理在跑）⇒ L2P 写不出第二块。
 * -------------------------------------------------------------------- */
static void md_job_reset(metadata_dir_cache_t *dir)
{
    if (dir == NULL)
    {
        return;
    }

    memset(&dir->job, 0, sizeof(dir->job));
    dir->job.victim         = UINT16_MAX;
    dir->job.reclaim_target = UINT16_MAX;
    dir->job.reclaim_gblock = UINT16_MAX;
    dir->job.slot           = NAND_PBA_INVALID;
}

/* --------------------------------------------------------------------
 * metadata_dir_init
 *   功能：首次格式化——建立固定哨兵层、清 Anchor 序号/脏标记、游标全部置无效。
 *   参数：dir —— 待初始化 Directory（NULL 直接返回）。
 *   返回：无。
 *   实现要点：游标置无效是"绝不复用可能已编程的旧页"的保证——首次追加才从 Free
 *             Pool 新取块。
 * -------------------------------------------------------------------- */
void metadata_dir_init(metadata_dir_cache_t *dir)
{
    if (dir == NULL)
    {
        return;
    }

    dir->anchor_sequence = 0U;
    dir->dirty           = 0U;
    dir->change_count    = 0U;

    metadata_dir_fill_fixed(dir);
    metadata_dir_rebuild_live(dir);

    /* 游标全部置无效：首次追加才从 Free Pool 取块（绝不复用可能已编程的旧页）*/
    dir->cur_metadata_block  = 0U;
    dir->cur_metadata_page   = 0U;
    dir->cur_block_valid     = 0U;
    dir->cur_metadata_plane  = 0U;
    dir->anchor_write_idx    = 0U;
    dir->anchor_cursor_valid = 0U;

    /* 后台整理 job 归零（见 md_job_reset：不清 ⇒ append_slot 永久让路）。 */
    md_job_reset(dir);
}

/* metadata_dir_lookup — 线性查 (type,id) 的登记位置；未找到返回 NAND_PBA_INVALID */
nand_pba_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
                               uint8_t                     type,
                               uint16_t                    id)
{
    uint16_t i;

    if (dir == NULL)
    {
        return NAND_PBA_INVALID;
    }

    for (i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            return dir->entries[i].pba;
        }
    }

    return NAND_PBA_INVALID;
}

/* --------------------------------------------------------------------
 * metadata_dir_update
 *   功能：更新 (type,id) 的登记位置；已有则覆盖，无则尾部追加；置 dirty 并计数。
 *   参数：dir —— Directory；type/id —— 元数据标识；new_pba —— 新位置。
 *   返回：无。
 *   实现要点：容量满时**不追加**（静默返回）。对本项目的 L2P_REGION 不可达
 *             （容量 == region 数，全部在 fill_fixed 已登记）——见函数内长注释。
 * -------------------------------------------------------------------- */
void metadata_dir_update(metadata_dir_cache_t *dir,
                         uint8_t               type,
                         uint16_t              id,
                         nand_pba_t            new_pba)
{
    uint16_t i;

    if (dir == NULL)
    {
        return;
    }

    /* 查找现有 entry（覆盖旧位置） */
    for (i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            dir->entries[i].pba = new_pba;
            dir->dirty          = 1U;
            dir->change_count++;
            return;
        }
    }

    /* 新增 entry（往 entries 数组尾部追加）*/
    if (dir->count < METADATA_DIR_ENTRIES)
    {
        dir->entries[dir->count].type = type;
        dir->entries[dir->count].id   = id;
        dir->entries[dir->count].pba  = new_pba;
        dir->count++;
        dir->dirty = 1U;
        dir->change_count++;
    }
    /*
     * 满：不追加。
     *
     * ⚠ 本分支对 METADATA_TYPE_L2P_REGION **不可达**：METADATA_DIR_ENTRIES 已等于
     *   L2P_REGIONS_PER_INSTANCE，且每个 region 在 metadata_dir_fill_fixed 就登记完毕，
     *   刷新后走的是上面的"覆盖"分支。旧版容量 128 < 2048 时本分支可达，结果是
     *   region ≥128 的刷新位置被**静默丢弃**（重启回落到固定位置 = 更新永久丢失）。
     *
     * ⚠ 只有"登记第 METADATA_DIR_ENTRIES+1 种元数据"才会走到这里。届时**不要**在此
     *   静默返回：调用者以为位置已记录，实际没有。必须先扩容量或实现淘汰。
     */
}

/* ══════════════════════════════════════════════════════════════════
 * 四、L2P 日志块：追加槽 / 登记 / 回收（元数据写模型 ①②③④）
 * ══════════════════════════════════════════════════════════════════ */

/* ── 日志块整理的共用判据（同步 sweep 与异步 job 共用）── */

/* md_block_addr — gblock → 该块第 0 页的 PBA（ch/ce/lun 取本 Directory 拓扑）。 */
static nand_pba_t md_block_addr(const metadata_dir_cache_t *dir, uint16_t gblock)
{
    return pba_make(dir->topo.ch,
                    dir->topo.ce,
                    dir->topo.lun,
                    (uint32_t)(gblock % BLOCKS_PER_PLANE),
                    (uint32_t)(gblock / BLOCKS_PER_PLANE),
                    0U,
                    0U);
}

/* md_block_is_reserved — 该 gblock 是否**确实是本池独占的日志块**（BST 带 RESERVED 位）。
 * 只认 RESERVED：否则会误擦"恰好活计数为 0 的用户块"。
 * ⚠ 重启后 RESERVED 位为 0（RAM 态）⇒ 开机不做回收；开机时"无引用却带旧内容"的
 *   日志块由 ftl_rebuild_closed_sb_list 的全盘扫描拉出 Free Pool 后交 GC 擦除。 */
static int md_block_is_reserved(const ftl_allocator_t *alloc, uint16_t gblock)
{
    if ((alloc == NULL) || (alloc->blocks == NULL) || (gblock >= FTL_BLOCK_TOTAL))
    {
        return 0;
    }
    return (ftl_block_state_is(&alloc->blocks[gblock], FTL_BLOCK_STATE_RESERVED) != 0) ? 1 : 0;
}

/* md_compacting — 压缩 job 是否在飞（CP_* 阶段）。
 * 等价于旧 `dir->compacting` 标志的读法：commit_region 据此**抑制逐次立即回收**
 * （搬移 victim 的最后一个活 region 时它会归零，此时 Anchor 还没指向新位置
 * ⇒ 擦了就是掉电丢映射，见 metadata.h 的 ③④ 契约）。 */
static int md_compacting(const metadata_dir_cache_t *dir)
{
    return ((dir->job.phase >= (uint8_t)MD_JOB_CP_READ) &&
            (dir->job.phase <= (uint8_t)MD_JOB_CP_RECLAIM))
               ? 1
               : 0;
}

/* --------------------------------------------------------------------
 * metadata_dir_reclaim_block
 *   功能：擦除一个**已无引用**的日志块并归还 Free Pool（**同步**）。
 *   参数：dir / alloc / gblock。
 *   返回：FTL_OK（含"不该回收"）/ FTL_ERROR（参数非法 / 擦失败）。
 *   实现要点：只回收本池独占的日志块（RESERVED 位，见 md_block_is_reserved）。
 *             ⚠ **同步残留**：只被 metadata_dir_reclaim 的 sweep 调用，而 sweep 只在
 *             append_slot 换块分支里跑（后台路径、通常一个块都不擦）。异步回收走
 *             job 的 MD_JOB_RECLAIM 阶段（同一组判据 + md_submit_erase）。
 * -------------------------------------------------------------------- */
static ftl_status_t metadata_dir_reclaim_block(metadata_dir_cache_t *dir,
                                               ftl_allocator_t      *alloc,
                                               uint16_t              gblock)
{
    if ((dir == NULL) || (alloc == NULL) || (alloc->blocks == NULL) || (gblock >= FTL_BLOCK_TOTAL))
    {
        return FTL_ERROR;
    }

    if (md_block_is_reserved(alloc, gblock) == 0)
    {
        return FTL_OK; /* 不是本池的日志块：不动它 */
    }

    if (ftl_sync_erase_block(md_block_addr(dir, gblock)) != 0)
    {
        return FTL_ERROR; /* 擦失败：留 RESERVED + 活计数 0，下次再试 */
    }

    allocator_free_reserved_block(alloc, gblock);
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * metadata_dir_reclaim
 *   功能：全池扫一遍，回收所有"活计数 0 且非当前写块"的日志块。
 *   参数：dir —— Directory；alloc —— 分配器（判定 RESERVED 位 / 归还块）。
 *   返回：FTL_OK（逐块失败被忽略——留待下次重试）；FTL_ERROR 参数非法。
 *   实现要点：当前写块（cur_metadata_block）**绝不**回收——它正在被追加。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_dir_reclaim(metadata_dir_cache_t *dir, ftl_allocator_t *alloc)
{
    uint16_t g;

    if ((dir == NULL) || (alloc == NULL))
    {
        return FTL_ERROR;
    }

    for (g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] != 0U)
        {
            continue;
        }
        if ((dir->cur_block_valid != 0U) && (g == dir->cur_metadata_block))
        {
            continue; /* 当前写入块不可回收 */
        }
        (void)metadata_dir_reclaim_block(dir, alloc, g);
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * metadata_dir_compact_victim
 *   功能：选压缩 victim —— 活 region 数**最少**（但 > 0）的非当前日志块。
 *   参数：dir —— Directory；alloc —— 分配器（读 BST 判 RESERVED 身份）。
 *   返回：victim 的 gblock；无候选返回 UINT16_MAX。
 *   实现要点：零活块**不在此列**——它们由 metadata_dir_reclaim 直接擦除归还，
 *             无需搬移。平手取块号最小者：结果确定（跨复位可复现），且先分配的
 *             块通常收的是更早、更可能变冷的数据。
 * -------------------------------------------------------------------- */
static uint16_t metadata_dir_compact_victim(metadata_dir_cache_t *dir,
                                            ftl_allocator_t      *alloc)
{
    uint16_t best      = UINT16_MAX;
    uint16_t best_live = UINT16_MAX;
    uint16_t g;

    if ((dir == NULL) || (alloc == NULL) || (alloc->blocks == NULL))
    {
        return UINT16_MAX;
    }

    for (g = 0U; g < FTL_BLOCK_TOTAL; g++)
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
        if (ftl_block_state_is(&alloc->blocks[g], FTL_BLOCK_STATE_RESERVED) == 0)
        {
            continue; /* 只认本池独占的日志块（与 reclaim_block 同一判据）*/
        }
        if (live < best_live)
        {
            best_live = live;
            best      = g;
        }
    }

    return best;
}

/* --------------------------------------------------------------------
 * metadata_dir_slot_now
 *   功能：取当前日志块的**下一个空槽**并推进写指针（纯 RAM，不做任何块管理）。
 *   参数：dir —— Directory。
 *   返回：槽位 PBA；无当前块 / 块已满 ⇒ NAND_PBA_INVALID。
 *   实现要点：取到位置后**立即推进** cur_metadata_page——写失败时该页可能已半编程，
 *             不可复用（槽被跳过，块内浪费一页）。与 metadata_dir_append_slot 的尾部
 *             逐字一致；抽出来是给压缩 job 用的（它**不能**走 append_slot：那里有
 *             "job 在飞就让路"的判据，job 自己调就会自锁）。
 * -------------------------------------------------------------------- */
static nand_pba_t metadata_dir_slot_now(metadata_dir_cache_t *dir)
{
    uint16_t plane;
    uint16_t block;
    uint8_t  page;

    if ((dir == NULL) || (dir->cur_block_valid == 0U) || (dir->cur_metadata_page >= PAGES_PER_BLOCK))
    {
        return NAND_PBA_INVALID;
    }

    plane = ftl_gblock_plane(dir->cur_metadata_block);
    block = ftl_gblock_block(dir->cur_metadata_block);
    page  = dir->cur_metadata_page;

    /* 立即推进：写失败时该页可能已半编程，不可复用（槽被跳过，块内浪费一页）*/
    dir->cur_metadata_page++;

    return pba_make(dir->topo.ch, dir->topo.ce, dir->topo.lun, block, plane, page, 0U);
}

/* --------------------------------------------------------------------
 * metadata_dir_append_slot
 *   功能：返回 L2P 日志块的"下一个空槽"物理位置，并推进写指针。
 *   参数：dir —— Directory；alloc —— 分配器（取新块）。
 *   返回：nand_pba_t 槽位置；NAND_PBA_INVALID 参数非法 / 两 plane Free Pool 皆空。
 *   实现要点：当前块无效或写满 ⇒ 先回收无活 region 的日志块，再取新块（plane 轮转，
 *             本 plane 空则换另一 plane；**两 plane 都空绝不回退固定位置**）；
 *             取到位置后**立即推进** cur_metadata_page（写失败页可能半编程不可复用）。
 *             ⚠ 换块后若 Free Pool 吃紧，只**登记压缩请求**（job.compact_wanted）并返回
 *             NAND_PBA_INVALID——新块整块空槽，是唯一能一次搬空任意 victim 的时机，
 *             但压缩本身交给后台 job（阶段 8：旧实现就地同步跑完，可阻塞服务循环数十毫秒）。
 * -------------------------------------------------------------------- */
nand_pba_t metadata_dir_append_slot(metadata_dir_cache_t *dir, ftl_allocator_t *alloc)
{
    uint16_t plane;
    uint16_t gblock;

    if ((dir == NULL) || (alloc == NULL))
    {
        return NAND_PBA_INVALID;
    }

    /* 整理 job 在飞 ⇒ 一律让路（空槽预算 + ③④ 顺序，见函数头与 metadata.h）：
     *   · 压缩搬移按**发放那一刻**的 cur_metadata_page 核定空槽预算，期间再发放会打乱它；
     *   · 更要紧的是本函数换块分支会调 metadata_dir_reclaim 全池 sweep，而 victim 的
     *     活计数此刻可能刚好归零 ⇒ 会在 Anchor 落盘之前把 victim 擦掉。
     * 调用者（l2p_cache_flush_kick）据此返回 FTL_AGAIN，下轮重试。 */
    if (dir->job.phase != (uint8_t)MD_JOB_IDLE)
    {
        return NAND_PBA_INVALID;
    }

    if ((dir->cur_block_valid == 0U) || (dir->cur_metadata_page >= PAGES_PER_BLOCK))
    {
        /* 先回收无活 region 的日志块（否则稳态每填满一块就永久占一块），再取新块 */
        (void)metadata_dir_reclaim(dir, alloc);

        /* plane 轮转：Policy B 下两 plane 各自独立选块，拿不到"同号双 plane 都空闲"
         * 的保证，故不跨 plane 配对；日志页落在哪个 plane 对读取无影响（位置由 pba 携带）*/
        plane = dir->cur_metadata_plane;
        if (allocator_alloc_reserved_block(alloc, plane, &gblock) != FTL_OK)
        {
            /* 本 plane 的 Free Pool 空 ⇒ 换另一 plane 再试（两池是独立的，一个空不代表全空）*/
            plane = (uint8_t)((plane + 1U) % PLANE_NUM);
            if (allocator_alloc_reserved_block(alloc, plane, &gblock) != FTL_OK)
            {
                return NAND_PBA_INVALID; /* 两 plane 都空：**绝不**回退到固定位置 */
            }
        }

        /* ⚠ 临时诊断探针（`log level debug` 才打印）：日志块从哪来。定位完即删。 */
        LOGD("[META] log-block: plane=%u gblock=%u\r\n", (unsigned)plane, (unsigned)gblock);

        dir->cur_metadata_block = gblock;
        dir->cur_metadata_page  = 0U;
        dir->cur_block_valid    = 1U;
        dir->cur_metadata_plane = (uint8_t)((plane + 1U) % PLANE_NUM);

        /* 抗碎片（P0-3）：刚换到新日志块 ⇒ 块内有整 PAGES_PER_BLOCK 个空槽，正好
         * 够搬空任意一个 victim（一块最多 PAGES_PER_BLOCK 个 region）。零活块已由上面
         * 的 reclaim 直接擦除归还，这里只管"多活"块。
         * ⚠ 只在 Free Pool 吃紧时请求（FREE_POOL_TARGET 是 GC 的水位①）——稳态下每
         *   PAGES_PER_BLOCK 次 region 写最多压缩一块，既不抖也不制造写放大；
         *   池子宽裕时压缩纯属白写 NAND。
         * ⚠ 只**登记请求**，本槽本次失败：调用者收到 NAND_PBA_INVALID 会返回 FTL_AGAIN
         *   下轮重试，那时新块仍在、cur_metadata_page == 0 ⇒ 直接取到槽 0（若 job 已起，
         *   则继续让路直到压缩走完）。 */
        if (alloc->free_count <= FREE_POOL_TARGET)
        {
            dir->job.compact_wanted = 1U;
            return NAND_PBA_INVALID;
        }
    }

    return metadata_dir_slot_now(dir);
}

/* --------------------------------------------------------------------
 * metadata_dir_commit_region
 *   功能：登记 region 的新位置——旧位置活计数 -1、新位置 +1、更新 entries；旧块归零即登记回收。
 *   参数：dir —— Directory；alloc —— 分配器（保留形参：回收已改由 job 做）；region_id；
 *         new_pba —— 新位置。
 *   返回：无（参数非法直接返回）。
 *   实现要点：只对**日志块**位置（block ≥ FTL_METADATA_BLOCK_END）增减活计数——固定
 *             哨兵层不计。旧块归零且非当前块 ⇒ 登记进 `job.reclaim_target`，把
 *             "已无引用却带旧内容"的窗口压到最短（否则要等下次 append_slot 换块才回收）。
 *   ⚠ **本函数不擦 NAND**（阶段 8 起的刻意变更）：它运行在完成派发里
 *     （`ftl_op_l2p` 的 FLUSHING 分支），就地擦块 = 在完成泵里嵌套泵。
 * -------------------------------------------------------------------- */
void metadata_dir_commit_region(metadata_dir_cache_t *dir,
                                ftl_allocator_t      *alloc,
                                uint16_t              region_id,
                                nand_pba_t            new_pba)
{
    nand_pba_t old;
    uint16_t   old_g = UINT16_MAX;

    (void)alloc; /* 回收已改由后台 job 做（擦除地址由 md_block_addr 从 dir->topo 算出）*/

    if ((dir == NULL) || (region_id >= METADATA_DIR_ENTRIES) || (new_pba == NAND_PBA_INVALID))
    {
        return;
    }

    /* 旧位置的活计数 -1（仅当日志块）*/
    old = metadata_dir_lookup(dir, METADATA_TYPE_L2P_REGION, region_id);
    if ((old != NAND_PBA_INVALID) && (pba_to_block(old) >= FTL_METADATA_BLOCK_END))
    {
        uint16_t g = ftl_pba_gblock(old);

        if ((g < FTL_BLOCK_TOTAL) && (dir->l2p_live[g] > 0U))
        {
            dir->l2p_live[g]--;
            old_g = g;
        }
    }

    /* 新位置的活计数 +1 */
    {
        uint16_t g = ftl_pba_gblock(new_pba);

        if (g < FTL_BLOCK_TOTAL)
        {
            dir->l2p_live[g]++;
        }
    }

    metadata_dir_update(dir, METADATA_TYPE_L2P_REGION, region_id, new_pba);
    dir->have_dynamic = 1U;

    /* 旧块活计数归零且非当前块 ⇒ 登记回收，把「已无引用却仍带旧内容」的窗口压到最短。
     * ⚠ 压缩期间**抑制**本登记：搬移 victim 的最后一个 region 时它会归零，此时 Anchor
     *   还没指向新位置 ⇒ 擦了就会掉电丢映射（③④ 契约）。压缩期间归零的块由 job 自己在
     *   CP_RECLAIM 里擦。 */
    if ((old_g != UINT16_MAX) && (dir->l2p_live[old_g] == 0U) && (md_compacting(dir) == 0) &&
        ((dir->cur_block_valid == 0U) || (old_g != dir->cur_metadata_block)))
    {
        /* ⚠ 只留**一个**待回收槽位：同一轮里若有多个块先后归零，早的会被覆盖。
         *   这不是泄漏——被覆盖的块仍是"活计数 0 + RESERVED"，append_slot 换块前的
         *   metadata_dir_reclaim 全池 sweep 会补上（它本来就是既有兜底）。 */
        dir->job.reclaim_target = old_g;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 四·b、日志块整理 job（v0.75 阶段 8）—— 回收 + 抗碎片压缩
 *
 * 设计 / 掉电安全顺序 / 失败语义见 metadata.h 的 md_job_t 长注释。此处只补实现约定：
 *
 *   · **一次只提交一条命令**：提交阶段设 waiting 就返回；纯 RAM 阶段连续推进到下一个
 *     提交阶段或终态。推进一律由 metadata_dir_kick 驱动（服务函数不推进，见下）。
 *   · **服务函数不推进状态机**（同 ftl_op_checkpoint）：CP_ANCHOR_PREP 要调
 *     metadata_dir_anchor_prepare，而它在游标未建立时会**同步**扫池定位
 *     （metadata_ring_locate → ftl_sync_read_page）——在完成泵里跑就是嵌套泵
 *     （l2p_sync_pump 首行有 ASSERT 挡）。故 ftl_op_metadata 只记结果 + release + 改
 *     phase，下一轮 kick 接着走。
 *   · **提交失败（背压）保留现场**：压缩搬移的槽位取到后即消耗（写指针已推进），
 *     故 `job.slot` 在提交失败时必须留着，下轮重试**同一个**槽——绝不能重取
 *     （重取会白烧一页）。
 * ══════════════════════════════════════════════════════════════════ */

/* 连续"提交不出去"的轮数上限（同 CP/MIG/RR_SUBMIT_STALL_MAX）。 */
#define MD_SUBMIT_STALL_MAX (256U)

/* 中止本次整理：清请求与游标，回 IDLE。
 * ⚠ **victim 不擦**（已登记的 region 已指向新页、未搬的仍指向 victim ⇒ 两边都有效），
 *   下次换块时重来即可——与旧同步实现的"失败不回滚"逐条一致。 */
static void md_job_abort(metadata_dir_cache_t *dir)
{
    md_job_t *j = &dir->job;

    LOGW("[META] md job abort phase=%u victim=%u moved=%u (victim 不擦)\r\n",
         (unsigned)j->phase,
         (unsigned)j->victim,
         (unsigned)j->moved);

    md_job_reset(dir);
}

/* 累计一次「提交不出去」；超限即中止 job（同 CP/MIG 同族）。返回 1 = 已中止。 */
static int md_stall(metadata_dir_cache_t *dir)
{
    md_job_t *j = &dir->job;

    if (j->stall_rounds >= MD_SUBMIT_STALL_MAX)
    {
        md_job_abort(dir);
        return 1;
    }
    j->stall_rounds++;
    return 0;
}

/* 提交一条异步写（搬移页 / Anchor 分页共用）。data 在 ftl_async_write_page 内已拷进
 * FC buffer ⇒ 调用者的缓冲随即可复用。 */
static ftl_status_t md_submit_write(metadata_dir_cache_t *dir, nand_pba_t addr, const void *data, uint32_t length)
{
    md_job_t     *j = &dir->job;
    fc_command_t *cmd;

    if (ftl_async_write_page(&cmd, addr, data, length, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN; /* 命令池空 / 地址非法 */
    }

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

/* 提交一条异步擦除（日志块回收 / Anchor 换块 / victim 擦除共用）。 */
static ftl_status_t md_submit_erase(metadata_dir_cache_t *dir, nand_pba_t addr)
{
    md_job_t     *j = &dir->job;
    fc_command_t *cmd;

    if (ftl_async_erase_block(&cmd, addr) != FTL_OK)
    {
        return FTL_AGAIN;
    }

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
    md_job_t     *j = &dir->job;
    fc_command_t *cmd;

    if (ftl_async_read_page(&cmd, addr, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_AGAIN;
    }

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
    uint16_t  victim;

    j->compact_wanted = 0U;

    if ((dir->cur_block_valid == 0U) || (alloc == NULL))
    {
        return; /* 无当前写块：无处可搬 */
    }

    victim = metadata_dir_compact_victim(dir, alloc);
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
 *   返回：2 = 已扫完（未提交任何命令）；1 = 已提交（等完成）；0 = 背压未提交（下轮重试）；
 *         -1 = 硬失败（调用者中止）。
 *   实现要点：游标先走后命中，提交失败再把游标退回来（重试同一 region）。
 *             搬移走的是**裸页拷贝**（region 页无头，就是 L2P_REGION_SIZE 字节的 entries
 *             数组，与 l2p_cache 的读写口径逐字节一致）——不经过 l2p_cache，故不触发
 *             "写→evict→flush→又要块"的递归。
 * -------------------------------------------------------------------- */
static int md_compact_read(metadata_dir_cache_t *dir)
{
    md_job_t *j = &dir->job;

    while (j->region_id < METADATA_DIR_ENTRIES)
    {
        const uint16_t rid = j->region_id;
        nand_pba_t     old;

        j->region_id = (uint16_t)(rid + 1U); /* 游标先走：本轮不命中也不回头 */

        old = metadata_dir_lookup(dir, METADATA_TYPE_L2P_REGION, rid);
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
            if (md_stall(dir) != 0)
            {
                return -1;
            }
            return 0;
        }
        return 1;
    }

    return 2; /* 扫描完毕 */
}

/* --------------------------------------------------------------------
 * md_compact_write
 *   功能：把 job.cp_buf 写进当前块的新槽（槽只取一次，提交失败重试同一槽）。
 *   返回：同 md_compact_read。
 * -------------------------------------------------------------------- */
static int md_compact_write(metadata_dir_cache_t *dir)
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
            return -1;
        }
    }

    if (md_submit_write(dir, j->slot, j->cp_buf, (uint32_t)L2P_REGION_SIZE) != FTL_OK)
    {
        if (md_stall(dir) != 0)
        {
            return -1;
        }
        return 0; /* 背压：**保留 slot**，下轮重试同一槽（重取会白烧一页）*/
    }

    return 1;
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
    if ((dir == NULL) || (alloc == NULL))
    {
        return;
    }

    /* 心跳（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/age */
    if (dir->job.phase == (uint8_t)MD_JOB_IDLE)
    {
        ftl_job_heartbeat_reset(&dir->job.log_ms, &dir->job.start_ms);
    }
    else if (ftl_job_heartbeat(&dir->job.log_ms, &dir->job.start_ms) != 0)
    {
        LOGI("[MD] job phase=%u victim=%u moved=%u reclaim=%u stall=%u age=%lums\r\n",
             (unsigned)dir->job.phase,
             (unsigned)dir->job.victim,
             (unsigned)dir->job.moved,
             (unsigned)dir->job.reclaim_target,
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
                if (j->reclaim_target != UINT16_MAX)
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
                const uint16_t g = j->reclaim_target;

                if (g == UINT16_MAX)
                {
                    j->phase = (uint8_t)MD_JOB_IDLE;
                    break;
                }

                /* 消费前**重查**（登记到消费之间可能隔了很多轮，块号可能已被重新分配）：
                 *   ① 活计数非 0 ⇒ 该块又被引用了；
                 *   ② 成了当前写块 ⇒ 正在被追加；
                 *   ③ 不再带 RESERVED ⇒ 已被别处归还/复用。
                 * 任一命中即放弃本次（块留给下次），**绝不擦活块**。 */
                if ((dir->l2p_live[g] != 0U) ||
                    ((dir->cur_block_valid != 0U) && (g == dir->cur_metadata_block)) ||
                    (md_block_is_reserved(alloc, g) == 0))
                {
                    j->reclaim_target = UINT16_MAX;
                    j->phase          = (uint8_t)MD_JOB_IDLE;
                    break;
                }

                if (md_submit_erase(dir, md_block_addr(dir, g)) != FTL_OK)
                {
                    (void)md_stall(dir); /* 背压：保留 target，下轮重试 */
                    return;
                }

                j->reclaim_gblock = g; /* 提交**成功**才认领 */
                j->reclaim_target = UINT16_MAX;
                return;
            }

            case MD_JOB_CP_READ:
            {
                const int r = md_compact_read(dir);

                if (r < 0)
                {
                    md_job_abort(dir);
                    break;
                }
                if (r != 2)
                {
                    return; /* 已提交 / 背压 */
                }
                j->phase = (uint8_t)MD_JOB_CP_ANCHOR_PREP;
                break;
            }

            case MD_JOB_CP_WRITE:
            {
                /* md_compact_write 只返回 1（已提交）/ 0（背压）/ -1（失败），
                 * **不会**返回 2（"扫完"）——写一条就回去读下一条。 */
                const int r = md_compact_write(dir);

                if (r < 0)
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
                int need_erase = 0;
                int r          = metadata_dir_anchor_prepare(dir,
                                                    &j->anchor_ring,
                                                    &j->anchor_base,
                                                    &j->anchor_idx,
                                                    &j->anchor_seq,
                                                    &need_erase);

                if (r < 0)
                {
                    md_job_abort(dir);
                    break;
                }
                if (r == 0)
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
                if (md_submit_erase(dir, j->anchor_base) != FTL_OK)
                {
                    (void)md_stall(dir);
                }
                return;

            case MD_JOB_CP_ANCHOR_WRITE:
            {
                metadata_anchor_page_t page; /* 2KB 栈缓冲：按页组好后写入 */
                nand_pba_t             aaddr;

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

                aaddr = pba_with_page(j->anchor_base, (uint32_t)pba_to_page(j->anchor_base) + j->anchor_chunk);
                metadata_dir_anchor_page(dir, j->anchor_seq, j->anchor_chunk, &page);

                if (md_submit_write(dir, aaddr, &page, (uint32_t)sizeof(page)) != FTL_OK)
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
                    (md_block_is_reserved(alloc, v) == 0))
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
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `md_job_t *`（= &ftl->metadata_dir.job）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：**本函数不推进状态机**（见本大区开头的说明）：只把 phase 改到"下一步该
 *             干什么"，由下一轮 metadata_dir_kick 续跑。
 *             ⚠ CP_READ 的 ftl_cmd_read_data **必须早于** release（数据在 desc 链上）。
 * -------------------------------------------------------------------- */
void ftl_op_metadata(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    md_job_t             *j = (md_job_t *)ctx;
    metadata_dir_cache_t *dir;
    fc_command_t         *cmd;
    int                   failed;

    ASSERT(completion != NULL);
    ASSERT(j != NULL);

    cmd = fc_command_id_to_command((uint16_t)completion->command_id);
    ASSERT(cmd != NULL);

    /* 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出的归属实例，
     * 二者必须一致——不一致说明派发路由错了。 */
    ASSERT(ftl != NULL);
    ASSERT(&ftl->metadata_dir.job == j);

    dir    = &ftl->metadata_dir;
    failed = ((completion->status & FC_CW_STATUS_ERROR_MASK) != 0U) ? 1 : 0;

    switch ((md_job_phase_t)j->phase)
    {
        case MD_JOB_RECLAIM:
            (void)fc_command_release(cmd);
            j->waiting = 0U;

            if (failed != 0)
            {
                /* 擦失败：留 RESERVED + 活计数 0 ⇒ append_slot 换块前的全池 sweep
                 * （或下一次登记）会再来一次。与旧同步实现逐条一致。 */
                LOGW("[META] reclaim erase failed blk=%u — 留 RESERVED 下次再试\r\n",
                     (unsigned)j->reclaim_gblock);
            }
            else
            {
                allocator_free_reserved_block(&ftl->alloc, j->reclaim_gblock);
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
                /* 擦失败：留 RESERVED + 活计数 0 ⇒ 下次换块时重来（同旧实现的
                 * `(void)metadata_dir_reclaim_block(...)` 忽略返回值，但这里留痕）。 */
                LOGW("[META] compact: victim erase failed blk=%u — 留 RESERVED\r\n",
                     (unsigned)j->victim);
            }
            else
            {
                allocator_free_reserved_block(&ftl->alloc, j->victim);
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

/* ══════════════════════════════════════════════════════════════════
 * 五、Anchor 落盘 / 加载
 * ══════════════════════════════════════════════════════════════════ */

/* ── Anchor 落盘的三段式（唯一形态）──
 *
 * 一条 Anchor record = ANCHOR_PAGE_COUNT 页。**所有**落盘站点（Checkpoint job 的
 * CP_ANCHOR_*、日志块整理 job 的 MD_JOB_CP_ANCHOR_*）都拆成
 * prepare →（可选换块擦）→ 逐页写 → commit 四步，每步之间可以回到服务循环。
 * 三段式把「Directory 格式」留在本文件，「状态机」留给调用者。
 * （阶段 8 删掉了同步一次性版本 metadata_dir_flush_to_anchor——它的两个调用者
 *  都已 job 化。）
 */

/*
 * metadata_dir_anchor_prepare - Anchor 落盘第一步：定位游标 + 取槽 + 算是否需换块擦
 *
 * 返回：1 = 需要写（出参全部有效）；0 = 无需写（dir 不脏）；-1 = 失败（参数非法 /
 *       池定位失败 / 槽位越界）。
 * 副作用：游标未建立时定位并置 anchor_cursor_valid；**anchor_sequence 自增**（与旧
 *        同步实现同序：序号先取，再取槽）。
 * ⚠ 不动介质：换块擦除由调用者做（同步站点即刻擦；异步 job 提交异步擦命令）。
 */
int metadata_dir_anchor_prepare(metadata_dir_cache_t *dir,
                                metadata_ring_t      *out_ring,
                                nand_pba_t           *out_base,
                                uint16_t             *out_idx,
                                uint32_t             *out_seq,
                                int                  *out_need_erase)
{
    if ((dir == NULL) || (out_ring == NULL) || (out_base == NULL) || (out_idx == NULL) || (out_seq == NULL) || (out_need_erase == NULL))
    {
        return -1;
    }
    if (dir->dirty == 0U)
    {
        return 0; /* 无变更早退：Anchor 每写一条都消耗池内槽位，空转会让池无谓轮转 */
    }

    anchor_ring_init(out_ring);

    /* 游标未建立（冷启动后首次 flush）⇒ 先定位池内最新有效 record 的下一槽 */
    if (dir->anchor_cursor_valid == 0U)
    {
        anchor_probe_ctx_t ctx;
        uint16_t           newest = UINT16_MAX;

        ctx.dir  = dir;
        ctx.ring = out_ring;

        if (metadata_ring_locate(out_ring, &dir->topo, anchor_record_probe, &ctx, &newest, &dir->anchor_sequence, &dir->anchor_write_idx) != FTL_OK)
        {
            return -1;
        }
        dir->anchor_cursor_valid = 1U;
    }

    *out_idx = dir->anchor_write_idx;
    *out_seq = ++dir->anchor_sequence;

    if (metadata_ring_prepare_addr(out_ring, &dir->topo, *out_idx, out_base, out_need_erase) != FTL_OK)
    {
        return -1;
    }

    return 1;
}

/*
 * metadata_dir_anchor_page - 组第 chunk 页 Anchor 数据（纯 RAM，不碰 NAND）
 *
 * padding 归零 ⇒ 介质内容确定；本页有效条目数 = 全局有效范围 [0,count) 与本页区间的交。
 */
void metadata_dir_anchor_page(const metadata_dir_cache_t *dir, uint32_t seq, uint8_t chunk, metadata_anchor_page_t *page)
{
    uint16_t base_entry = (uint16_t)(chunk * METADATA_DIR_ENTRIES_PER_PAGE);
    uint16_t n;
    uint16_t k;

    if ((dir == NULL) || (page == NULL))
    {
        return;
    }

    if (base_entry >= dir->count)
    {
        n = 0U;
    }
    else
    {
        n = (uint16_t)(dir->count - base_entry);
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

    for (k = 0U; k < METADATA_DIR_ENTRIES_PER_PAGE; k++)
    {
        if (k < n)
        {
            page->entries[k] = dir->entries[base_entry + k];
        }
        else
        {
            page->entries[k].type = 0U;
            page->entries[k].id   = 0U;
            page->entries[k].pba  = NAND_PBA_INVALID;
        }
    }
}

/*
 * metadata_dir_anchor_commit - Anchor 落盘收尾：推进游标（无论成败），成功才清 dirty
 *
 * ⚠ **无论成败都推进游标**：已写过的槽（可能半编程）不可复用。ok == 0 时保留 dirty
 *   ⇒ 下次在新槽重试整条（整条残缺的 record 会被 locate 丢弃，不取半条）。
 */
void metadata_dir_anchor_commit(metadata_dir_cache_t *dir, const metadata_ring_t *ring, uint16_t idx, int ok)
{
    if ((dir == NULL) || (ring == NULL))
    {
        return;
    }

    dir->anchor_write_idx = (uint16_t)((idx + 1U) % metadata_ring_total_records(ring));

    if (ok != 0)
    {
        dir->dirty        = 0U;
        dir->change_count = 0U;
    }
}

/* --------------------------------------------------------------------
 * metadata_dir_load
 *   功能：开机加载 Directory——先铺固定哨兵层，再定位并读回最新有效 Anchor 覆盖 entries。
 *   参数：dir —— Directory（含 topo）。
 *   返回：FTL_OK 成功；FTL_ERROR 参数非法 / 池内无有效 Anchor（调用者转 metadata_dir_init）。
 *   实现要点：先 fill_fixed 铺基础层（Anchor 缺页/损坏时该区间回落固定位置，不出现空洞）；
 *             定位失败或无有效记录 ⇒ 清游标返回错误；读胜者逐页校验，坏页保留基础层；
 *             **游标不跨重启恢复**（首次追加从 Free Pool 新取块，绝不复用旧页）。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_dir_load(metadata_dir_cache_t *dir)
{
    metadata_anchor_page_t page; /* 2KB 栈缓冲（按页读）*/
    metadata_ring_t        ring;
    anchor_probe_ctx_t     ctx;
    uint16_t               newest = UINT16_MAX;
    uint8_t                chunk;

    if (dir == NULL)
    {
        return FTL_ERROR;
    }

    /* 基础层：全部 region 的固定位置（哨兵层，永不写入 ⇒ 读回即"未映射"）。
     * Anchor 缺页/损坏时该区间回落到固定位置，不会出现"未登记"的空洞。 */
    metadata_dir_fill_fixed(dir);
    metadata_dir_rebuild_live(dir); /* 失败路径也要给出确定的活计数（开机日志用）*/

    anchor_ring_init(&ring);
    ctx.dir  = dir;
    ctx.ring = &ring;

    {
        const ftl_status_t loc = metadata_ring_locate(&ring,
                                                      &dir->topo,
                                                      anchor_record_probe,
                                                      &ctx,
                                                      &newest,
                                                      &dir->anchor_sequence,
                                                      &dir->anchor_write_idx);

        /* 观测口（**INFO**，随开机必打印）：Anchor 读侧结论。
         * loc=0 且 newest=65535 ⇒ 池内一条有效 record 都没有（写没落盘 / 读不回来？）。
         * loc=0 且 newest<N ⇒ 找到了，看 seq 是否与写侧对得上。
         * ⚠ 必须 INFO：本函数在 ftl_init 里跑，早于 CLI ⇒ `log level debug` 是开机后
         *   才敲的，LOGD 在此处永远看不到（曾因此两轮上板定位不到 Anchor 读失败）。*/
        LOGI("[META] anchor load: loc=%d newest=%u seq=%lu\r\n",
             (int)loc,
             (unsigned)newest,
             (unsigned long)dir->anchor_sequence);

        if ((loc != FTL_OK) || (newest == UINT16_MAX))
        {
            dir->anchor_sequence     = 0U;
            dir->anchor_write_idx    = 0U;
            dir->anchor_cursor_valid = 0U;
            dir->dirty               = 0U;
            dir->change_count        = 0U;
            metadata_dir_boot_log(dir);
            return FTL_ERROR; /* 无有效 Anchor：调用者转 metadata_dir_init */
        }
    }

    dir->anchor_cursor_valid = 1U;

    /* 读胜者的全部页，逐页校验后覆盖对应区间 */
    for (chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
    {
        uint16_t   base_entry = (uint16_t)(chunk * METADATA_DIR_ENTRIES_PER_PAGE);
        nand_pba_t addr       = metadata_ring_addr(&ring, &dir->topo, newest, chunk);
        uint16_t   n;
        uint16_t   k;

        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((anchor_page_valid(&page) == 0) || (page.sequence != dir->anchor_sequence) || (page.chunk_index != chunk))
        {
            continue; /* 该页不可信：保留基础层的固定位置 */
        }

        n = (uint16_t)((page.entry_count < METADATA_DIR_ENTRIES_PER_PAGE) ? page.entry_count : METADATA_DIR_ENTRIES_PER_PAGE);
        for (k = 0U; (k < n) && ((uint16_t)(base_entry + k) < METADATA_DIR_ENTRIES); k++)
        {
            dir->entries[base_entry + k] = page.entries[k];
        }
    }

    dir->dirty        = 0U;
    dir->change_count = 0U;

    /* 由装配后的 entries 重建活计数；**游标不跨重启恢复**：首次追加时从 Free Pool
     * 新取一块（绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒ 由 l2p_live
     * 保住，待其 region 全部改写后由 metadata_dir_reclaim 归还。 */
    metadata_dir_rebuild_live(dir);
    dir->cur_metadata_block = 0U;
    dir->cur_metadata_page  = 0U;
    dir->cur_block_valid    = 0U;
    dir->cur_metadata_plane = 0U;
    md_job_reset(dir); /* 整理 job 不跨重启（同游标：phase / 游标 / 请求位全是 RAM 态）*/

    metadata_dir_boot_log(dir);

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 六、元数据块判定
 * ══════════════════════════════════════════════════════════════════ */

/*
 * 固定元数据分区表 —— 单一事实来源（宏声明在 ftl/metadata.h）
 *
 * 区间为 [first_block, first_block + block_count)；**各 (ch,ce,lun) 用同一份块号**
 * （分片模型：每个 die 一套完整元数据，分区不随维度错开）。
 *
 * Anchor / Checkpoint / BBT 都是**多块轮转池**（P0-2 元数据写模型统一）：
 * 池内每块容纳 `records_per_block` 条 record，块满换池内下一块（先擦后写）。
 *
 * 每项完全由 metadata.h 的宏派生——块号 / 块数 / 每块 record 数 / 表字节均无手写数字。
 * 本表原先住在 src/ftl/ftl_config.c；布局归 metadata 之后随之下沉到本文件。
 */
const ftl_partition_t g_ftl_partitions[] = {
    { "L2P Table",    L2P_BASE_BLOCK,             L2P_BLOCKS,                       1U, FTL_PART_FIXED,        FTL_L2P_TABLE_BYTES},
    {    "Anchor",     ANCHOR_BLOCK0,     ANCHOR_POOL_BLOCKS, ANCHOR_RECORDS_PER_BLOCK, FTL_PART_FIXED,     FTL_ANCHOR_TABLE_BYTES},
    {"Checkpoint", CHECKPOINT_BLOCK0, CHECKPOINT_POOL_BLOCKS,          PAGES_PER_BLOCK, FTL_PART_FIXED, FTL_CHECKPOINT_TABLE_BYTES},
    {       "BBT",         BBT_BLOCK,        BBT_POOL_BLOCKS,          PAGES_PER_BLOCK, FTL_PART_FIXED,        FTL_BBT_TABLE_BYTES},
    {   "Journal",    JOURNAL_BLOCK0,    JOURNAL_BLOCK_COUNT,                       1U, FTL_PART_FIXED,    FTL_JOURNAL_TABLE_BYTES},
    {      "Wear",        WEAR_BLOCK,            WEAR_BLOCKS,              WEAR_COPIES, FTL_PART_FIXED,       FTL_WEAR_TABLE_BYTES},
};

/* 分区数（表长）。sizeof 派生 ⇒ 加/删分区项时自动跟随，无需手改。 */
const uint32_t g_ftl_partition_count = (uint32_t)(sizeof(g_ftl_partitions) / sizeof(g_ftl_partitions[0]));

/* --------------------------------------------------------------------
 * ftl_is_metadata_block
 *   功能：判定 (plane, per-plane 块号 block) 是否属于元数据（固定分区 / 当前日志块 /
 *         已登记日志块）。
 *   参数：md_dir —— Directory（可 NULL：只判固定分区）；plane —— 物理 plane 号；
 *         block —— per-plane 块号。
 *   返回：1 是元数据块；0 不是。
 *   实现要点：三分支——
 *     ① 固定分区表遍历（各 (ch,ce,lun) 用同一份块号，分片模型 ⇒ **不看 plane**）；
 *     ② 当前日志块（刚分配还没 commit，扫 entries 扫不到，必须显式排除）；
 *     ③ 已登记日志块（用 have_dynamic 先挡掉"无动态块"常见情形，避免每次开机
 *        回填数千次调用都全表线性扫）。
 *   ②③ 是**从用户区动态取的**块 ⇒ 必须带 plane 比较；只比块号会连带掩掉另一 plane
 *   的同号块（既出不了 free pool、又被 F2 重建跳过，白丢一个可用块）。
 * -------------------------------------------------------------------- */
int ftl_is_metadata_block(const metadata_dir_cache_t *md_dir, uint8_t plane, uint16_t block)
{
    uint32_t p;
    uint16_t i;

    /* ── 固定元数据块：遍历分区表（本文件上方 g_ftl_partitions[]，声明见 metadata.h）──
     * 每项覆盖 [first_block, first_block + block_count)。
     * **各 (ch,ce,lun) 用同一份块号**（分片模型：每 die 一套完整元数据），
     * 分区不随维度错开。block 为 per-plane 号（调用侧取模）。 */
    for (p = 0U; p < g_ftl_partition_count; p++)
    {
        const ftl_partition_t *part = &g_ftl_partitions[p];

        if (part->kind != FTL_PART_FIXED)
        {
            continue;
        }

        if ((block >= part->first_block) && (block < (uint16_t)(part->first_block + part->block_count)))
        {
            return 1;
        }
    }

    if (md_dir == NULL)
    {
        return 0;
    }

    /* ── 当前日志块：可能尚无任何 region 指向它（刚分配、还没 commit），
     *    扫 entries 扫不到 ⇒ 必须显式排除，否则会被 Free Pool 回填当用户块收走。
     *    cur_metadata_block 是 gblock ⇒ 用 ftl_gblock_* 解码出 plane 一起比。── */
    if ((md_dir->cur_block_valid != 0U) && (plane == ftl_gblock_plane(md_dir->cur_metadata_block)) &&
        (block == ftl_gblock_block(md_dir->cur_metadata_block)))
    {
        return 1;
    }

    /* ── 日志块：Metadata Directory 登记的 L2P region 物理位置 ──
     * L2P region 首写落在固定表区（上面已短路），之后从 Free Pool 动态取块
     * （flushed region，块号 >= FTL_METADATA_BLOCK_END）。这些块写满 64 页，
     * 逐页判空会误判为满 SB，必须排除。
     * ⚠ entries 有 2048 条而本函数在开机回填里按块调用数千次 ⇒ 用 have_dynamic
     *   先挡掉"根本没有动态块"的常见情形（全表线性扫会明显拖慢开机）。 */
    if (md_dir->have_dynamic != 0U)
    {
        for (i = 0U; i < md_dir->count; i++)
        {
            if ((md_dir->entries[i].type == METADATA_TYPE_L2P_REGION) &&
                ((uint8_t)pba_to_plane(md_dir->entries[i].pba) == plane) &&
                (pba_to_block(md_dir->entries[i].pba) == block))
            {
                return 1;
            }
        }
    }

    return 0;
}
