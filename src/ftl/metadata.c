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
 *   metadata.h —— 结构（metadata_ring_t / metadata_anchor_page_t / metadata_dir_cache_t）+
 *                 固定元数据区布局（池尺寸 / 块偏移 / 表字节 / 分区表声明）；
 *   本文件     —— 上述结构的全部算法实现 + 分区表本体（g_ftl_partitions[]，单一事实来源）。
 *
 * L2P region 的位置只有一层来源（见 metadata.h「固定元数据区布局」）：
 *   Directory 条目 = `NAND_PBA_INVALID` ⇒ region 从未写过（未映射，读取不碰 NAND）；
 *   非 INVALID ⇒ region 页在日志块上的真实 PBA（从 Free Pool 取块、追加写）。
 *
 * 文件结构（═══ 编号大区沿用既有编号，未重排）：
 *   一、元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 *   二、Anchor 池（Metadata Directory 的 record 池）
 *   三、Directory 查询 / 更新
 *   四、L2P 日志块：追加槽 / 登记（元数据写模型 ①②③④）
 *   五、Anchor 落盘 / 加载
 *   六、元数据块判定
 *
 * ⚠ 同步 / 异步的分界：
 *   · **异步**：日志块的回收与压缩（`md_job_t` + g_op_service[FTL_OP_METADATA]，
 *      状态机在 metadata_job.c）。待回收块由 commit_region / append_slot 登记进
 *      `reclaim_pending` 位图，运行期没有任何同步擦除。
 *   · **同步（刻意）**：开机加载 / 格式化（metadata_dir_load / init）、Anchor 池定位
 *      （metadata_ring_locate）——前者在 superloop 之前跑（无泵可等），后者只在
 *      冷启动后首次整理出现。
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
#include "metadata_internal.h" /* slot_now / job_reset / 待回收位图（与 metadata_job.c 共用） */
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
 *         FTL_ERROR ring 几何非法（block_count < 2 —— 单块池无环形语义；判据 9 运行期拒绝）。
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
                                  metadata_ring_probe_fn probe,
                                  void                  *ctx,
                                  uint16_t              *out_newest_idx,
                                  uint32_t              *out_sequence,
                                  uint16_t              *out_write_idx)
{
    /* 前置条件（判据 2 / §二十六「子模块公开入口」）：五个指针均由调用者自持。
     * ⚠ 形参 `topo` 已删（H5）：本函数体内从未用它，唯一"用途"是原 NULL 守卫。 */
    ASSERT(ring != NULL);
    ASSERT(probe != NULL);
    ASSERT(out_newest_idx != NULL);
    ASSERT(out_sequence != NULL);
    ASSERT(out_write_idx != NULL);
    /* ring 自身几何是**配置**（构造错误 ⇒ 寻址全错）⇒ 保留运行期拒绝（判据 9）。 */
    if ((ring->pages_per_record == 0U) || (ring->block_count < 2U))
    {
        return FTL_ERROR;
    }

    uint16_t rpb   = metadata_ring_records_per_block(ring);
    uint16_t total = metadata_ring_total_records(ring);

    /* 池空时下面各出口都不写 *out_sequence ⇒ 先给确定值（调用者据此续序号）*/
    *out_sequence   = 0U;
    *out_newest_idx = UINT16_MAX;

    /* ① 逐块廉价探针该块**末条** record：末条有效 = 该块写满 */
    uint16_t cursor_block  = UINT16_MAX;
    uint16_t best_full_idx = UINT16_MAX;
    uint32_t best_full_seq = 0U;

    for (uint16_t b = 0U; b < ring->block_count; b++)
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
    int ring_full = 0;

    if (cursor_block == UINT16_MAX)
    {
        cursor_block = (uint16_t)(((best_full_idx / rpb) + 1U) % ring->block_count);
        ring_full    = 1;
    }

    /* ③ 游标块内降序找最后一条有效 record（池转满时游标块本身就是满的，不参与）*/
    uint16_t cursor_last_idx = UINT16_MAX;
    uint32_t cursor_last_seq = 0U;

    if (ring_full == 0)
    {
        for (uint16_t b = 0U; b < rpb; b++)
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
    uint16_t cand_idx[2];
    uint32_t cand_seq[2];
    uint8_t  cand_n = 0U;

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

    for (uint8_t i = 0U; i < cand_n; i++)
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

            for (uint16_t k = 0U; k < total; k++)
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
 *   返回：FTL_OK；FTL_ERROR write_idx 越界（ring/topo/出参为前置条件，非法即 ASSERT）。
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
    /* 前置条件（判据 2）：四个指针均由调用者自持。 */
    ASSERT(ring != NULL);
    ASSERT(topo != NULL);
    ASSERT(out_addr != NULL);
    ASSERT(out_need_erase != NULL);
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
 *   返回：FTL_OK；FTL_ERROR write_idx 越界 / 换块时擦除失败。
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
static bool anchor_page_valid(const metadata_anchor_page_t *page)
{
    if (page->magic != ANCHOR_MAGIC)
    {
        return false;
    }
    if (page->chunk_count != ANCHOR_PAGE_COUNT)
    {
        return false;
    }
    if (page->layout_version != FTL_MEDIA_LAYOUT_VERSION)
    {
        return false; /* 旧格式介质的 Anchor ⇒ 作废（触发开机自动格式化）*/
    }

    return true;
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

    uint32_t seq = 0U;

    for (uint8_t chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
    {
        nand_pba_t addr = metadata_ring_addr(c->ring, &c->dir->topo, record_idx, chunk);

        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
        {
            return FTL_ERROR;
        }
        if ((!anchor_page_valid(&page)) || (page.chunk_index != chunk))
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
 * metadata_dir_fill_fixed - 为全部 region 登记"未映射"（见 metadata.h「固定元数据区布局」）
 *
 * 每个实例的 L2P_REGIONS_PER_INSTANCE 个 region **全部登记为 NAND_PBA_INVALID**；
 * 某个 region 刷新到日志块后由 metadata_dir_commit_region 就地覆盖成真实 PBA。
 * "全部预登记"同时保证 Directory 容量（= region 数）按构造不会溢出 ——
 * metadata_dir_update 的"满则丢弃"分支对 L2P region 不可达。
 *
 * 共用于 metadata_dir_init（首次格式化）与 metadata_dir_load（基础层）：
 * Anchor 损坏时该层即"全部未映射"，不需要任何闪存读。
 */
static void metadata_dir_fill_fixed(metadata_dir_cache_t *dir)
{
    dir->count        = 0U;
    dir->have_dynamic = 0U;

    for (uint16_t i = 0U; i < L2P_REGIONS_PER_INSTANCE; i++)
    {
        dir->entries[dir->count].type = METADATA_TYPE_L2P_REGION;
        dir->entries[dir->count].id   = i;
        dir->entries[dir->count].pba  = NAND_PBA_INVALID;
        dir->count++;
    }
}

/* 由 entries 重建 l2p_live / have_dynamic（日志块 = block >= FTL_METADATA_BLOCK_END）*/
static void metadata_dir_rebuild_live(metadata_dir_cache_t *dir)
{
    memset(dir->l2p_live, 0, sizeof(dir->l2p_live));
    dir->have_dynamic = 0U;

    for (uint16_t i = 0U; i < dir->count; i++)
    {
        if (dir->entries[i].type != METADATA_TYPE_L2P_REGION)
        {
            continue;
        }
        /* ⚠ 必须先挡 INVALID：0xFFFFFFFF 的 block 字段解出 0x7FF、plane 解出 1，
         *   会误算成一个合法 gblock（4095）并记进 l2p_live —— 编译期完全无声。 */
        if (dir->entries[i].pba == NAND_PBA_INVALID)
        {
            continue; /* 未映射，不在任何日志块上 */
        }

        uint16_t block = (uint16_t)pba_to_block(dir->entries[i].pba);
        if (block < FTL_METADATA_BLOCK_END)
        {
            continue; /* 非日志块（正常路径不会出现：L2P region 只落日志块） */
        }

        uint16_t g = ftl_gblock((uint8_t)pba_to_plane(dir->entries[i].pba), block);
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
    uint32_t live_blocks = 0U;

    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
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
 * metadata_dir_init
 *   功能：首次格式化——全部 region 置"未映射"、清 Anchor 序号/脏标记、游标全部置无效。
 *   参数：dir —— 待初始化 Directory（前置条件，非法即 ASSERT）。
 *   返回：无。
 *   实现要点：游标置无效是"绝不复用可能已编程的旧页"的保证——首次追加才从 Free
 *             Pool 新取块。
 * -------------------------------------------------------------------- */
void metadata_dir_init(metadata_dir_cache_t *dir)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir。 */
    ASSERT(dir != NULL);

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

    /* 待回收位图 / 后台整理 job 归零（RAM 态，不跨重启；
     * 不清 job ⇒ append_slot 永久让路）。 */
    md_reclaim_reset(dir);
    md_job_reset(dir);
}

/* metadata_dir_lookup — 线性查 (type,id) 的登记位置；未找到返回 NAND_PBA_INVALID */
// 问题 除了日志块还有其他类型需要查找吗？
nand_pba_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
                               uint8_t                     type,
                               uint16_t                    id)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir。 */
    ASSERT(dir != NULL);

    for (uint16_t i = 0U; i < dir->count; i++)
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
    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir。 */
    ASSERT(dir != NULL);

    /* 查找现有 entry（覆盖旧位置） */
    for (uint16_t i = 0U; i < dir->count; i++)
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
     *   region ≥128 的刷新位置被**静默丢弃**（重启回落到"未映射" = 更新永久丢失）。
     *
     * ⚠ 只有"登记第 METADATA_DIR_ENTRIES+1 种元数据"才会走到这里。届时**不要**在此
     *   静默返回：调用者以为位置已记录，实际没有。必须先扩容量或实现淘汰。
     */
}

/* ══════════════════════════════════════════════════════════════════
 * 四、L2P 日志块：追加槽 / 登记 / 回收（元数据写模型 ①②③④）
 * ══════════════════════════════════════════════════════════════════ */

/* md_compacting — 压缩 job 是否在飞（CP_* 阶段）。
 * 等价于旧 `dir->compacting` 标志的读法：commit_region 据此**抑制逐次立即回收**
 * （搬移 victim 的最后一个活 region 时它会归零，此时 Anchor 还没指向新位置
 * ⇒ 擦了就是掉电丢映射，见 metadata.h 的 ③④ 契约）。 */
static bool md_compacting(const metadata_dir_cache_t *dir)
{
    return (dir->job.phase >= (uint8_t)MD_JOB_CP_READ) && (dir->job.phase <= (uint8_t)MD_JOB_CP_RECLAIM);
}

/* --------------------------------------------------------------------
 * metadata_dir_append_slot
 *   功能：返回 L2P 日志块的"下一个空槽"物理位置，并推进写指针。
 *   参数：dir —— Directory；alloc —— 分配器（取新块）。
 *   返回：nand_pba_t 槽位置；NAND_PBA_INVALID 两 plane Free Pool 皆空 / 无当前写块。
 *   实现要点：当前块无效或写满 ⇒ 从 Free Pool 取新块（plane 轮转，本 plane 空则换另一
 *             plane；**两 plane 都空则返回 INVALID**——等后台 job 擦完待回收块后重试）；
 *             取到位置后**立即推进** cur_metadata_page（写失败页可能半编程不可复用）。
 *             旧当前块若已无活 region ⇒ 置进 `reclaim_pending` 位图；换块时还会扫一遍
 *             全池**只登记**零活 LOG 块（不擦）。运行期擦除全部由后台 job 异步做，
 *             本函数里没有任何同步 tERASE。
 *             ⚠ 换块后若 Free Pool 吃紧，只**登记压缩请求**（job.compact_wanted）并返回
 *             NAND_PBA_INVALID——新块整块空槽，是唯一能一次搬空任意 victim 的时机。
 * -------------------------------------------------------------------- */
nand_pba_t metadata_dir_append_slot(metadata_dir_cache_t *dir, ftl_allocator_t *alloc)
{
    uint16_t plane;
    uint16_t gblock;
    uint16_t old_cur = UINT16_MAX;
    bool     had_cur = false;

    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir 与 &ftl->alloc。 */
    ASSERT(dir != NULL);
    ASSERT(alloc != NULL);

    /* 整理 job 在飞 ⇒ 一律让路：压缩搬移按**发放那一刻**的 cur_metadata_page 核定
     * 空槽预算，期间再发放会打乱它（详见 metadata.h 的 md_job_t 长注释）。
     * 调用者（l2p_cache_flush_kick）据此返回 FTL_AGAIN，下轮重试。 */
    if (dir->job.phase != (uint8_t)MD_JOB_IDLE)
    {
        return NAND_PBA_INVALID;
    }

    if ((dir->cur_block_valid == 0U) || (dir->cur_metadata_page >= PAGES_PER_BLOCK))
    {
        /* 当前块已写满且无活 region：它不可能再被追加，先从"当前写块"身份上摘下来，
         * 否则 mark_all 会把它永远排除；Free Pool 空时就再也换不了块（死等）。 */
        if ((dir->cur_block_valid != 0U) && (dir->cur_metadata_page >= PAGES_PER_BLOCK) &&
            (dir->l2p_live[dir->cur_metadata_block] == 0U))
        {
            (void)md_reclaim_mark(dir, dir->cur_metadata_block);
            dir->cur_block_valid = 0U;
        }

        /* 换块时机扫一遍全池：只**登记**零活 LOG 块，绝不在这里擦。
         * 擦除由后台 job 异步做——同步 tERASE ~3 ms 会击穿服务预算。
         * 扫描保持旧 sweep 的覆盖范围（开机快照 / journal 回放 / 压缩中止留下的零活块）。 */
        md_reclaim_mark_all(dir, alloc);

        /* plane 轮转：Policy B 下两 plane 各自独立选块，拿不到"同号双 plane 都空闲"
         * 的保证，故不跨 plane 配对；日志页落在哪个 plane 对读取无影响（位置由 pba 携带）*/
        plane = dir->cur_metadata_plane;
        // 问题   这里plane没有泛化呀，而且取日志块不能精准定位吗，需要这样多次定位吗
        if (allocator_alloc_log_block(alloc, plane, &gblock) != FTL_OK)
        {
            /* 本 plane 的 Free Pool 空 ⇒ 换另一 plane 再试（两池是独立的，一个空不代表全空）*/
            plane = (uint8_t)((plane + 1U) % PLANE_NUM);
            if (allocator_alloc_log_block(alloc, plane, &gblock) != FTL_OK)
            {
                return NAND_PBA_INVALID; /* 两 plane 都空：本仓没有可回退的固定位置 */
            }
        }

        /* ⚠ 临时诊断探针（`log level debug` 才打印）：日志块从哪来。定位完即删。 */
        LOGD("[META] log-block: plane=%u gblock=%u\r\n", (unsigned)plane, (unsigned)gblock);

        if (dir->cur_block_valid != 0U)
        {
            old_cur = dir->cur_metadata_block;
            had_cur = true;
        }

        dir->cur_metadata_block = gblock;
        dir->cur_metadata_page  = 0U;
        // 问题 这个游标不能是bool类型吗
        dir->cur_block_valid    = 1U;
        dir->cur_metadata_plane = (uint8_t)((plane + 1U) % PLANE_NUM);

        /* 旧当前块可能已被搬空（压缩只搬活 region，零活块不会成为 victim）：
         * 换走后它再也不会被追加，登记给后台回收。 */
        if (had_cur && (dir->l2p_live[old_cur] == 0U))
        {
            (void)md_reclaim_mark(dir, old_cur);
        }

        /* 抗碎片（P0-3）：刚换到新日志块 ⇒ 块内有整 PAGES_PER_BLOCK 个空槽，正好
         * 够搬空任意一个 victim（一块最多 PAGES_PER_BLOCK 个 region）。零活块已登记
         * 在待回收位图（见上），这里只管"多活"块。
         * ⚠ 只在 Free Pool 吃紧时请求（FREE_POOL_TARGET 是 GC 的水位①）——稳态下每
         *   PAGES_PER_BLOCK 次 region 写最多压缩一块，既不抖也不制造写放大；
         *   池子宽裕时压缩纯属白写 NAND。
         * ⚠ 只**登记请求**，本槽本次失败：调用者收到 NAND_PBA_INVALID 会返回 FTL_AGAIN
         *   下轮重试，那时新块仍在、cur_metadata_page == 0 ⇒ 直接取到槽 0（若 job 已起，
         *   则继续让路直到压缩走完）。 */
        if (allocator_free_count(alloc) <= FREE_POOL_TARGET)
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
 *   返回：无（越界 no-op）。
 *   实现要点：只对**日志块**位置（block ≥ FTL_METADATA_BLOCK_END）增减活计数——
 *             INVALID（未映射）不计。旧块归零且非当前块 ⇒ 置进 `reclaim_pending` 位图，
 *             由后台 job 异步擦除；位图不会像单槽那样被后来的块覆盖。
 *   ⚠ **本函数不擦 NAND**（阶段 8 起的刻意变更）：它运行在完成派发里
 *     （`ftl_op_l2p` 的 FLUSHING 分支），就地擦块 = 在完成泵里嵌套泵。
 * -------------------------------------------------------------------- */
void metadata_dir_commit_region(metadata_dir_cache_t *dir,
                                ftl_allocator_t      *alloc,
                                uint16_t              region_id,
                                nand_pba_t            new_pba)
{
    // 问题 这个形参没有就删掉呀
    (void)alloc; /* 回收已改由后台 job 做（擦除地址由 md_block_addr 从 dir->topo 算出）*/

    /* 前置条件（判据 2）：dir 恒 ftl->metadata_dir。region_id / new_pba 是**数据**判断
     * （越界 no-op 是既有契约）⇒ 保留运行期分支。 */
    ASSERT(dir != NULL);
    // 问题 下面的if判断这个有意义吗，存在这个场景吗
    if ((region_id >= METADATA_DIR_ENTRIES) || (new_pba == NAND_PBA_INVALID))
    {
        return;
    }

    uint16_t old_g = UINT16_MAX;

    /* 旧位置的活计数 -1（仅当日志块）*/

    nand_pba_t old = metadata_dir_lookup(dir, METADATA_TYPE_L2P_REGION, region_id);
    // 问题 这个判断条件是不是冗余呀
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

    /* 旧块活计数归零且非当前块 ⇒ 登记进待回收位图，由后台 job 异步擦除。
     * ⚠ 压缩期间**抑制**本登记：搬移 victim 的最后一个 region 时它会归零，此时 Anchor
     *   还没指向新位置 ⇒ 擦了就会掉电丢映射（③④ 契约）。压缩期间归零的块由 job 自己在
     *   CP_RECLAIM 里擦。 */
    // 问题 这里的判断是不是太多了能优化吗
    if ((old_g != UINT16_MAX) && (dir->l2p_live[old_g] == 0U) && (!md_compacting(dir)) &&
        ((dir->cur_block_valid == 0U) || (old_g != dir->cur_metadata_block)))
    {
        (void)md_reclaim_mark(dir, old_g);
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
 * 返回：MD_ANCHOR_READY = 需要写（出参全部有效）；MD_ANCHOR_CLEAN = 无需写（dir 不脏）；
 *       MD_ANCHOR_FAILED = 失败（定位失败 / 池定位失败 / 槽位越界）。
 * 副作用：游标未建立时定位并置 anchor_cursor_valid；**anchor_sequence 自增**（与旧
 *        同步实现同序：序号先取，再取槽）。
 * ⚠ 不动介质：换块擦除由调用者做（同步站点即刻擦；异步 job 提交异步擦命令）。
 */
md_anchor_step_t metadata_dir_anchor_prepare(metadata_dir_cache_t *dir,
                                             metadata_ring_t      *out_ring,
                                             nand_pba_t           *out_base,
                                             uint16_t             *out_idx,
                                             uint32_t             *out_seq,
                                             int                  *out_need_erase)
{
    /* 前置条件（判据 2）：六个指针均由调用者自持。 */
    ASSERT(dir != NULL);
    ASSERT(out_ring != NULL);
    ASSERT(out_base != NULL);
    ASSERT(out_idx != NULL);
    ASSERT(out_seq != NULL);
    ASSERT(out_need_erase != NULL);
    if (dir->dirty == 0U)
    {
        return MD_ANCHOR_CLEAN; /* 无变更早退：Anchor 每写一条都消耗池内槽位，空转会让池无谓轮转 */
    }

    anchor_ring_init(out_ring);

    /* 游标未建立（冷启动后首次 flush）⇒ 先定位池内最新有效 record 的下一槽 */
    if (dir->anchor_cursor_valid == 0U)
    {
        anchor_probe_ctx_t ctx;
        uint16_t           newest = UINT16_MAX;

        ctx.dir  = dir;
        ctx.ring = out_ring;

        if (metadata_ring_locate(out_ring, anchor_record_probe, &ctx, &newest, &dir->anchor_sequence, &dir->anchor_write_idx) != FTL_OK)
        {
            return MD_ANCHOR_FAILED;
        }
        dir->anchor_cursor_valid = 1U;
    }

    *out_idx = dir->anchor_write_idx;
    *out_seq = ++dir->anchor_sequence;

    if (metadata_ring_prepare_addr(out_ring, &dir->topo, *out_idx, out_base, out_need_erase) != FTL_OK)
    {
        return MD_ANCHOR_FAILED;
    }

    return MD_ANCHOR_READY;
}

/*
 * metadata_dir_anchor_page - 组第 chunk 页 Anchor 数据（纯 RAM，不碰 NAND）
 *
 * padding 归零 ⇒ 介质内容确定；本页有效条目数 = 全局有效范围 [0,count) 与本页区间的交。
 */
void metadata_dir_anchor_page(const metadata_dir_cache_t *dir, uint32_t seq, uint8_t chunk, metadata_anchor_page_t *page)
{
    uint16_t base_entry = (uint16_t)(chunk * METADATA_DIR_ENTRIES_PER_PAGE);

    /* 前置条件（判据 2）：两个指针均由调用者自持。 */
    ASSERT(dir != NULL);
    ASSERT(page != NULL);

    uint16_t n;

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

    for (uint16_t k = 0U; k < METADATA_DIR_ENTRIES_PER_PAGE; k++)
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
    /* 前置条件（判据 2）：两个指针均由调用者自持。 */
    ASSERT(dir != NULL);
    ASSERT(ring != NULL);

    dir->anchor_write_idx = (uint16_t)((idx + 1U) % metadata_ring_total_records(ring));

    if (ok != 0)
    {
        dir->dirty        = 0U;
        dir->change_count = 0U;
    }
}

/* --------------------------------------------------------------------
 * metadata_dir_load
 *   功能：开机加载 Directory——先铺"全部未映射"基础层，再定位并读回最新有效 Anchor 覆盖 entries。
 *   参数：dir —— Directory（含 topo）。
 *   返回：FTL_OK 成功；FTL_ERROR 池内无有效 Anchor（调用者转 metadata_dir_init）。
 *   实现要点：先 fill_fixed 铺基础层（Anchor 缺页/损坏时该区间回落"未映射"，不出现空洞）；
 *             定位失败或无有效记录 ⇒ 清游标返回错误；读胜者逐页校验，坏页保留基础层；
 *             **游标不跨重启恢复**（首次追加从 Free Pool 新取块，绝不复用旧页）。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_dir_load(metadata_dir_cache_t *dir)
{
    metadata_anchor_page_t page; /* 2KB 栈缓冲（按页读）*/

    /* 前置条件（判据 2）：调用者一律传 ftl->metadata_dir。 */
    ASSERT(dir != NULL);

    /* 基础层：全部 region = NAND_PBA_INVALID（未映射）。
     * Anchor 缺页/损坏时该区间回落"未映射"，不会出现"未登记"的空洞。 */
    metadata_dir_fill_fixed(dir);
    metadata_dir_rebuild_live(dir); /* 失败路径也要给出确定的活计数（开机日志用）*/

    metadata_ring_t    ring;
    anchor_probe_ctx_t ctx;
    uint16_t           newest = UINT16_MAX;

    anchor_ring_init(&ring);
    ctx.dir  = dir;
    ctx.ring = &ring;

    {
        const ftl_status_t loc = metadata_ring_locate(&ring,
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
    for (uint8_t chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
    {
        uint16_t   base_entry = (uint16_t)(chunk * METADATA_DIR_ENTRIES_PER_PAGE);
        nand_pba_t addr       = metadata_ring_addr(&ring, &dir->topo, newest, chunk);

        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((!anchor_page_valid(&page)) || (page.sequence != dir->anchor_sequence) || (page.chunk_index != chunk))
        {
            continue; /* 该页不可信：保留基础层的"未映射" */
        }

        uint16_t n = (uint16_t)((page.entry_count < METADATA_DIR_ENTRIES_PER_PAGE) ? page.entry_count : METADATA_DIR_ENTRIES_PER_PAGE);
        for (uint16_t k = 0U; (k < n) && ((uint16_t)(base_entry + k) < METADATA_DIR_ENTRIES); k++)
        {
            dir->entries[base_entry + k] = page.entries[k];
        }
    }

    dir->dirty        = 0U;
    dir->change_count = 0U;

    /* 由装配后的 entries 重建活计数；**游标不跨重启恢复**：首次追加时从 Free Pool
     * 新取一块（绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒ 由 l2p_live
     * 保住，待其 region 全部改写后由后台整理 job 异步擦除归还。 */
    metadata_dir_rebuild_live(dir);
    dir->cur_metadata_block = 0U;
    dir->cur_metadata_page  = 0U;
    dir->cur_block_valid    = 0U;
    dir->cur_metadata_plane = 0U;
    md_reclaim_reset(dir); /* 待回收位图不跨重启（同 job：全是 RAM 态）*/
    md_job_reset(dir);     /* 整理 job 不跨重启（同游标：phase / 游标 / 请求位全是 RAM 态）*/

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
    {    "Anchor",     ANCHOR_BLOCK0,     ANCHOR_POOL_BLOCKS,         ANCHOR_RECORDS_PER_BLOCK, FTL_PART_FIXED,     FTL_ANCHOR_TABLE_BYTES},
    {"Checkpoint", CHECKPOINT_BLOCK0, CHECKPOINT_POOL_BLOCKS, FTL_CHECKPOINT_RECORDS_PER_BLOCK, FTL_PART_FIXED, FTL_CHECKPOINT_TABLE_BYTES},
    {       "BBT",         BBT_BLOCK,        BBT_POOL_BLOCKS,                  PAGES_PER_BLOCK, FTL_PART_FIXED,        FTL_BBT_TABLE_BYTES},
    {   "Journal",    JOURNAL_BLOCK0,    JOURNAL_BLOCK_COUNT,                               1U, FTL_PART_FIXED,    FTL_JOURNAL_TABLE_BYTES},
    {      "Wear",        WEAR_BLOCK,            WEAR_BLOCKS,                      WEAR_COPIES, FTL_PART_FIXED,       FTL_WEAR_TABLE_BYTES},
};

/* 分区数（表长）。sizeof 派生 ⇒ 加/删分区项时自动跟随，无需手改。 */
const uint32_t g_ftl_partition_count = (uint32_t)(sizeof(g_ftl_partitions) / sizeof(g_ftl_partitions[0]));

/* --------------------------------------------------------------------
 * ftl_is_metadata_block
 *   功能：判定 (plane, per-plane 块号 block) 是否属于元数据（固定分区 / 当前日志块 /
 *         已登记日志块）。
 *   参数：md_dir —— Directory（可 NULL：只判固定分区）；plane —— 物理 plane 号；
 *         block —— per-plane 块号。
 *   返回：true 是元数据块；false 不是。
 *   实现要点：三分支——
 *     ① 固定分区表遍历（各 (ch,ce,lun) 用同一份块号，分片模型 ⇒ **不看 plane**）；
 *     ② 当前日志块（刚分配还没 commit，扫 entries 扫不到，必须显式排除）；
 *     ③ 已登记日志块（用 have_dynamic 先挡掉"无动态块"常见情形，避免每次开机
 *        回填数千次调用都全表线性扫）。
 *   ②③ 是**从用户区动态取的**块 ⇒ 必须带 plane 比较；只比块号会连带掩掉另一 plane
 *   的同号块（既出不了 free pool、又被 F2 重建跳过，白丢一个可用块）。
 * -------------------------------------------------------------------- */
bool ftl_is_metadata_block(const metadata_dir_cache_t *md_dir, uint8_t plane, uint16_t block)
{
    /* ── 固定元数据块：遍历分区表（本文件上方 g_ftl_partitions[]，声明见 metadata.h）──
     * 每项覆盖 [first_block, first_block + block_count)。
     * **各 (ch,ce,lun) 用同一份块号**（分片模型：每 die 一套完整元数据），
     * 分区不随维度错开。block 为 per-plane 号（调用侧取模）。 */
    for (uint32_t p = 0U; p < g_ftl_partition_count; p++)
    {
        const ftl_partition_t *part = &g_ftl_partitions[p];

        if (part->kind != FTL_PART_FIXED)
        {
            continue;
        }

        if ((block >= part->first_block) && (block < (uint16_t)(part->first_block + part->block_count)))
        {
            return true;
        }
    }

    /* 前置条件（判据 2）：调用者（main.c / power_failure_recovery.c）恒传 ftl->metadata_dir。 */
    ASSERT(md_dir != NULL);

    /* ── 当前日志块：可能尚无任何 region 指向它（刚分配、还没 commit），
     *    扫 entries 扫不到 ⇒ 必须显式排除，否则会被 Free Pool 回填当用户块收走。
     *    cur_metadata_block 是 gblock ⇒ 用 ftl_gblock_* 解码出 plane 一起比。── */
    if ((md_dir->cur_block_valid != 0U) && (plane == ftl_gblock_plane(md_dir->cur_metadata_block)) &&
        (block == ftl_gblock_block(md_dir->cur_metadata_block)))
    {
        return true;
    }

    /* ── 日志块：Metadata Directory 登记的 L2P region 物理位置 ──
     * L2P region 只落动态日志块（从 Free Pool 取块，块号 >= FTL_METADATA_BLOCK_END；
     * 未映射的 region 条目是 INVALID，不指向任何块）。这些块写满 64 页，
     * 逐页判空会误判为满 SB，必须排除。
     * ⚠ entries 有 2048 条而本函数在开机回填里按块调用数千次 ⇒ 用 have_dynamic
     *   先挡掉"根本没有动态块"的常见情形（全表线性扫会明显拖慢开机）。 */
    if (md_dir->have_dynamic != 0U)
    {
        for (uint16_t i = 0U; i < md_dir->count; i++)
        {
            /* ⚠ 必须跳过 INVALID：0xFFFFFFFF 解出 plane=1 / block=2047，会把最后
             *   一个用户块误判成元数据块（既出不了 Free Pool、又被 F2 重建跳过）。 */
            if ((md_dir->entries[i].type == METADATA_TYPE_L2P_REGION) &&
                (md_dir->entries[i].pba != NAND_PBA_INVALID) &&
                ((uint8_t)pba_to_plane(md_dir->entries[i].pba) == plane) &&
                (pba_to_block(md_dir->entries[i].pba) == block))
            {
                return true;
            }
        }
    }

    return false;
}
