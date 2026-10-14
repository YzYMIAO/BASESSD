/*
 * metadata.c — 元数据落盘基建：record 池环形轮转 + Anchor 池 + L2P 日志块
 *
 * 职责：
 *   提供 FTL 全部持久化元数据（Anchor / Checkpoint / BBT / L2P region）共用的
 *   落盘原语：环形 record 池的寻址/定位/换块擦除；Anchor（Metadata Directory 快照）
 *   的组页/落盘/加载；L2P 日志块的追加槽/登记/回收；以及"这块是不是元数据块"的判定。
 *
 * 与 metadata.h 的分工：
 *   metadata.h —— 结构（metadata_ring_t / metadata_anchor_page_t / metadata_dir_cache_t）+ 几何宏；
 *   本文件     —— 上述结构的全部算法实现。
 *
 * 元数据两层（见 ftl_config.h §六）：
 *   固定层（哨兵）：L2P region 的初始位置，**永不写入**，读回全 0xFF = 未映射；
 *   日志层（动态）：region 刷新后从 Free Pool 取块、追加写，位置由 Directory 记账。
 *
 * 文件结构（═══ 编号大区沿用既有编号，未重排）：
 *   一、元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 *   二、Anchor 池（Metadata Directory 的 record 池）
 *   三、Directory 查询 / 更新
 *   四、L2P 日志块：追加槽 / 登记 / 回收（元数据写模型 ①②③④）
 *   五、Anchor 落盘 / 加载
 *   六、元数据块判定
 *
 * ⚠ 全部为纯 RAM 操作 + 同步 NAND 调用；无断言——失败一律返回 FTL_ERROR。
 */

#include "ftl/metadata.h"

#include "common/log.h"
#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/checkpoint.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"
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
 *   返回：nand_addr_t（plane 取 ring->plane，cw 恒 0）。
 *   实现要点：先按 records_per_block 整除得块偏移、取余得块内记录号，
 *             再乘 pages_per_record 得块内页号。地址公式的唯一出口。
 * -------------------------------------------------------------------- */
nand_addr_t metadata_ring_addr(const metadata_ring_t *ring,
                               const ftl_topology_t  *topo,
                               uint16_t               record_idx,
                               uint8_t                page_in_record)
{
    uint16_t    rpb = metadata_ring_records_per_block(ring);
    nand_addr_t addr;

    addr.ch    = topo->ch;
    addr.ce    = topo->ce;
    addr.lun   = topo->lun;
    addr.plane = ring->plane;
    addr.block = (uint16_t)(ring->first_block + (record_idx / rpb));
    addr.page  = (uint8_t)(((record_idx % rpb) * ring->pages_per_record) + page_in_record);
    addr.cw    = 0U;

    return addr;
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
 * metadata_ring_prepare_write
 *   功能：为写入池内第 write_idx 条 record 做前置——算地址，必要时先擦目标块。
 *   参数：ring/topo —— 池与拓扑；write_idx —— 目标槽；out_addr —— 出参物理地址。
 *   返回：FTL_OK；FTL_ERROR 参数非法 / write_idx 越界 / 换块时擦除失败。
 *   实现要点：仅当 write_idx 落在块首（% rpb == 0）才擦块。池内轮转 ⇒ 该块持全池
 *             最旧 record，且每条 record 是完整快照 ⇒ 擦掉不丢信息；块内槽 0 恒走本
 *             分支（新取/刚擦的块再擦一次是幂等无害代价）。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_ring_prepare_write(const metadata_ring_t *ring,
                                         const ftl_topology_t  *topo,
                                         uint16_t               write_idx,
                                         nand_addr_t           *out_addr)
{
    uint16_t rpb;

    if ((ring == NULL) || (topo == NULL) || (out_addr == NULL))
    {
        return FTL_ERROR;
    }
    if (write_idx >= metadata_ring_total_records(ring))
    {
        return FTL_ERROR;
    }

    *out_addr = metadata_ring_addr(ring, topo, write_idx, 0U);

    rpb = metadata_ring_records_per_block(ring);
    if ((uint16_t)(write_idx % rpb) == 0U)
    {
        /* 换块：先擦目标块。池内轮转 ⇒ 该块持的是全池最旧的 record，
         * 且每条 record 都是完整快照 ⇒ 擦掉不丢信息。
         * （块内槽 0 恒走本分支：新取/刚擦的块再擦一次是无害的幂等代价。）*/
        if (ftl_sync_erase_block(*out_addr) != 0)
        {
            return FTL_ERROR;
        }
    }

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
    ring->module           = FC_COMMAND_MODULE_METADATA;
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
        nand_addr_t addr = metadata_ring_addr(c->ring, &c->dir->topo, record_idx, chunk);

        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) != 0)
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
 * metadata_dir_fill_fixed - 建立全部 region 的固定位置（哨兵层，见 ftl_config.h §六）
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
        dir->entries[dir->count].paa  = paa_from_addr(&(nand_addr_t){.lun = dir->topo.lun, .block = block, .plane = plane, .page = page});
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

        block = (uint16_t)paa_to_block(dir->entries[i].paa);
        if (block < FTL_METADATA_BLOCK_END)
        {
            continue; /* 固定层（哨兵）不是日志块 */
        }

        g = ftl_gblock((uint8_t)paa_to_plane(dir->entries[i].paa), block);
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

    /* 压缩标志归零：init 不清 ⇒ 冷启动后若残留 1，append_slot 的换块分支会一直
     * 失败让路（以为压缩进行中）⇒ L2P 永远写不出第二块。 */
    dir->compacting = 0U;
}

/* metadata_dir_lookup — 线性查 (type,id) 的登记位置；未找到返回 NAND_PBA_INVALID */
nand_paa_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
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
            return dir->entries[i].paa;
        }
    }

    return NAND_PBA_INVALID;
}

/* --------------------------------------------------------------------
 * metadata_dir_update
 *   功能：更新 (type,id) 的登记位置；已有则覆盖，无则尾部追加；置 dirty 并计数。
 *   参数：dir —— Directory；type/id —— 元数据标识；new_paa —— 新位置。
 *   返回：无。
 *   实现要点：容量满时**不追加**（静默返回）。对本项目的 L2P_REGION 不可达
 *             （容量 == region 数，全部在 fill_fixed 已登记）——见函数内长注释。
 * -------------------------------------------------------------------- */
void metadata_dir_update(metadata_dir_cache_t *dir,
                         uint8_t               type,
                         uint16_t              id,
                         nand_paa_t            new_paa)
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
            dir->entries[i].paa = new_paa;
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
        dir->entries[dir->count].paa  = new_paa;
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

static ftl_status_t metadata_dir_reclaim_block(metadata_dir_cache_t *dir,
                                               ftl_allocator_t      *alloc,
                                               uint16_t              gblock)
{
    nand_addr_t addr;

    if ((dir == NULL) || (alloc == NULL) || (alloc->blocks == NULL) || (gblock >= FTL_BLOCK_TOTAL))
    {
        return FTL_ERROR;
    }

    /* 只回收本池独占的日志块（RESERVED 位）——而非恰好活计数为 0 的用户块。
     * ⚠ 重启后 RESERVED 位为 0（RAM 态）⇒ 开机不做本回收；开机时"无引用却带旧内容"
     *   的日志块由 ftl_rebuild_closed_sb_list 的全盘扫描拉出 Free Pool 后交 GC 擦除。 */
    if (ftl_block_state_is(&alloc->blocks[gblock], FTL_BLOCK_STATE_RESERVED) == 0)
    {
        return FTL_OK;
    }

    addr.ch    = dir->topo.ch;
    addr.ce    = dir->topo.ce;
    addr.lun   = dir->topo.lun;
    addr.plane = (uint8_t)(gblock / BLOCKS_PER_PLANE);
    addr.block = (uint16_t)(gblock % BLOCKS_PER_PLANE);
    addr.page  = 0U;
    addr.cw    = 0U;

    if (ftl_sync_erase_block(addr) != 0)
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
 * metadata_dir_compact
 *   功能：搬空一个「多活」日志块后擦除归还（抗碎片，P0-3）。
 *   参数：dir —— Directory；alloc —— 分配器。
 *   返回：FTL_OK = 确实压缩并归还一块；FTL_ERROR = 未做（无候选 / 空槽不够 /
 *         读失败 / 写失败 / Anchor 落盘失败）。
 *   实现要点：见 metadata.h 的同名注释。此处补充三点：
 *             ① victim 的活 region 数 == 遍历中命中 victim 的 region 数（l2p_live
 *                的定义就是"最新位置落在该块的 region 数"）⇒ 空槽预算一次核得准；
 *             ② 中途失败**不回滚**：已登记的 region 已指向新页，未搬的仍指向 victim
 *                （victim 不擦）⇒ 两边都有效，下次重来即可；
 *             ③ 搬移走的是**裸页拷贝**（region 页无头，就是 L2P_REGION_SIZE 字节的
 *                entries 数组，与 l2p_cache 的读写口径逐字节一致）——不经过 l2p_cache，
 *                故不触发"写→evict→flush→又要块"的递归。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_dir_compact(metadata_dir_cache_t *dir, ftl_allocator_t *alloc)
{
    uint16_t     victim;
    uint16_t     region_id;
    ftl_status_t st = FTL_OK;

    if ((dir == NULL) || (alloc == NULL))
    {
        return FTL_ERROR;
    }

    if ((dir->compacting != 0U) || (dir->cur_block_valid == 0U))
    {
        return FTL_ERROR; /* 防重入 / 无当前写块（无处可搬）*/
    }

    victim = metadata_dir_compact_victim(dir, alloc);
    if (victim == UINT16_MAX)
    {
        return FTL_ERROR; /* 无候选：全池零活（reclaim 已管）或只剩当前块 */
    }

    /* 空槽预算：victim 的全部活 region 必须一次装进当前块（见实现要点 ①）*/
    if (dir->l2p_live[victim] > (uint16_t)(PAGES_PER_BLOCK - dir->cur_metadata_page))
    {
        return FTL_ERROR; /* 当前块装不下 ⇒ 不冒险，留到换块后再来 */
    }

    dir->compacting = 1U;

    for (region_id = 0U; region_id < METADATA_DIR_ENTRIES; region_id++)
    {
        nand_paa_t  old;
        nand_paa_t  slot;
        nand_addr_t src;
        uint8_t     buf[L2P_REGION_SIZE]; /* 2KB 栈缓冲：region 页就是裸 entries 数组 */

        old = metadata_dir_lookup(dir, METADATA_TYPE_L2P_REGION, region_id);
        if (old == NAND_PBA_INVALID)
        {
            continue;
        }
        if (ftl_gblock((uint8_t)paa_to_plane(old), (uint16_t)paa_to_block(old)) != victim)
        {
            continue; /* 该 region 的最新位置不在 victim 上 */
        }

        src = addr_from_paa(dir->topo.ch, dir->topo.ce, old);
        if (ftl_sync_read_page(src, buf, sizeof(buf), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) != 0)
        {
            st = FTL_ERROR; /* 读不回：该 region 仍指向 victim 旧页，victim 不擦 ⇒ 无损失 */
            break;
        }

        slot = metadata_dir_append_slot(dir, alloc);
        if (slot == NAND_PBA_INVALID)
        {
            st = FTL_ERROR;
            break;
        }

        if (ftl_sync_write_page(addr_from_paa(dir->topo.ch, dir->topo.ce, slot), buf, sizeof(buf), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) != 0)
        {
            st = FTL_ERROR; /* 槽已消耗（可能半编程），不登记 ⇒ region 仍指向 victim */
            break;
        }

        metadata_dir_commit_region(dir, alloc, region_id, slot);
    }

    if ((st == FTL_OK) && (dir->l2p_live[victim] == 0U))
    {
        /* ③ Directory 的新位置**先落盘**，④ 才擦 victim（顺序不可倒，见 metadata.h）*/
        st = metadata_dir_flush_to_anchor(dir);
        if (st == FTL_OK)
        {
            (void)metadata_dir_reclaim_block(dir, alloc, victim); /* ④ 擦 + ⑤ 归还 */
        }
    }

    dir->compacting = 0U;
    return st;
}

/* --------------------------------------------------------------------
 * metadata_dir_append_slot
 *   功能：返回 L2P 日志块的"下一个空槽"物理位置，并推进写指针。
 *   参数：dir —— Directory；alloc —— 分配器（取新块）。
 *   返回：nand_paa_t 槽位置；NAND_PBA_INVALID 参数非法 / 两 plane Free Pool 皆空。
 *   实现要点：当前块无效或写满 ⇒ 先回收无活 region 的日志块，再取新块（plane 轮转，
 *             本 plane 空则换另一 plane；**两 plane 都空绝不回退固定位置**）；
 *             取到位置后**立即推进** cur_metadata_page（写失败页可能半编程不可复用）。
 *             ⚠ 换块后若 Free Pool 吃紧，顺带做一次抗碎片压缩（P0-3，见 metadata_dir_compact）
 *             ——新块整块空槽，是唯一能一次搬空任意 victim 的时机。
 * -------------------------------------------------------------------- */
nand_paa_t metadata_dir_append_slot(metadata_dir_cache_t *dir, ftl_allocator_t *alloc)
{
    uint16_t plane;
    uint16_t block;
    uint8_t  page;

    if ((dir == NULL) || (alloc == NULL))
    {
        return NAND_PBA_INVALID;
    }

    if ((dir->cur_block_valid == 0U) || (dir->cur_metadata_page >= PAGES_PER_BLOCK))
    {
        uint16_t gblock;

        /* 压缩进行中**不得换块**：搬移的 region 都往当前块写，此时换块说明空槽预算
         * 算错了；更要紧的是本分支会调 reclaim，而 victim 的活计数可能刚好归零
         * ⇒ 会在 Anchor 落盘之前把 victim 擦掉（见 metadata_dir_compact 的 ③④）。
         * 直接失败让路：压缩方收到 NAND_PBA_INVALID 即中止，victim 不擦，无损失。*/
        if (dir->compacting != 0U)
        {
            return NAND_PBA_INVALID;
        }

        /* 先回收无活 region 的日志块（否则稳态每填满一块就永久占一块），再取新块 */
        (void)metadata_dir_reclaim(dir, alloc);

        /* plane 轮转：Policy B 下两 plane 各自独立选块，拿不到"同号双 plane 都空闲"
         * 的保证，故不跨 plane 配对；日志页落在哪个 plane 对读取无影响（位置由 paa 携带）*/
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
         * ⚠ 只在 Free Pool 吃紧时做（FREE_POOL_TARGET 是 GC 的水位①）——稳态下每
         *   PAGES_PER_BLOCK 次 region 写最多压缩一块，既不抖也不制造写放大；
         *   池子宽裕时压缩纯属白写 NAND。 */
        if (alloc->free_count <= FREE_POOL_TARGET)
        {
            (void)metadata_dir_compact(dir, alloc);
        }
    }

    plane = ftl_gblock_plane(dir->cur_metadata_block);
    block = ftl_gblock_block(dir->cur_metadata_block);
    page  = dir->cur_metadata_page;

    /* 立即推进：写失败时该页可能已半编程，不可复用（槽被跳过，块内浪费一页）*/
    dir->cur_metadata_page++;

    return paa_from_addr(&(nand_addr_t){.lun = dir->topo.lun, .block = block, .plane = plane, .page = page});
}

/* --------------------------------------------------------------------
 * metadata_dir_commit_region
 *   功能：登记 region 的新位置——旧位置活计数 -1、新位置 +1、更新 entries；旧块归零即回收。
 *   参数：dir —— Directory；alloc —— 分配器；region_id —— region 号；new_paa —— 新位置。
 *   返回：无（参数非法直接返回）。
 *   实现要点：只对**日志块**位置（block ≥ FTL_METADATA_BLOCK_END）增减活计数——固定
 *             哨兵层不计。旧块归零且非当前块 ⇒ **立即**回收，把"已无引用却带旧内容"
 *             的窗口压到最短（否则要等下次 append_slot 换块才回收，期间掉电会被当空白块复用）。
 * -------------------------------------------------------------------- */
void metadata_dir_commit_region(metadata_dir_cache_t *dir,
                                ftl_allocator_t      *alloc,
                                uint16_t              region_id,
                                nand_paa_t            new_paa)
{
    nand_paa_t old;
    uint16_t   old_g = UINT16_MAX;

    if ((dir == NULL) || (region_id >= METADATA_DIR_ENTRIES) || (new_paa == NAND_PBA_INVALID))
    {
        return;
    }

    /* 旧位置的活计数 -1（仅当日志块）*/
    old = metadata_dir_lookup(dir, METADATA_TYPE_L2P_REGION, region_id);
    if ((old != NAND_PBA_INVALID) && (paa_to_block(old) >= FTL_METADATA_BLOCK_END))
    {
        uint16_t g = ftl_gblock((uint8_t)paa_to_plane(old), (uint16_t)paa_to_block(old));

        if ((g < FTL_BLOCK_TOTAL) && (dir->l2p_live[g] > 0U))
        {
            dir->l2p_live[g]--;
            old_g = g;
        }
    }

    /* 新位置的活计数 +1 */
    {
        uint16_t g = ftl_gblock((uint8_t)paa_to_plane(new_paa), (uint16_t)paa_to_block(new_paa));

        if (g < FTL_BLOCK_TOTAL)
        {
            dir->l2p_live[g]++;
        }
    }

    metadata_dir_update(dir, METADATA_TYPE_L2P_REGION, region_id, new_paa);
    dir->have_dynamic = 1U;

    /* 旧块活计数归零且非当前块 ⇒ 立即回收，把「已无引用却仍带旧内容」的窗口压到最短
     * （否则要等下一次 append_slot 换块时才回收，期间掉电会让该块被当空白块复用）。
     * ⚠ 压缩期间**抑制**本回收：搬移 victim 的最后一个 region 时它会归零，此时 Anchor
     *   还没指向新位置 ⇒ 擦了就会掉电丢映射（见 metadata_dir_compact 的 ③④）。*/
    if ((old_g != UINT16_MAX) && (dir->l2p_live[old_g] == 0U) && (dir->compacting == 0U) &&
        ((dir->cur_block_valid == 0U) || (old_g != dir->cur_metadata_block)))
    {
        (void)metadata_dir_reclaim_block(dir, alloc, old_g);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 五、Anchor 落盘 / 加载
 * ══════════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * metadata_dir_flush_to_anchor
 *   功能：把整份 Directory 快照成一条 Anchor record（ANCHOR_PAGE_COUNT 页）追加写进池。
 *   参数：dir —— Directory。
 *   返回：FTL_OK 成功（含 dirty==0 早退）；FTL_ERROR 参数非法 / 池定位失败 / prepare
 *         失败 / 任一分页写失败（整条作废）。
 *   实现要点：游标未建立时先定位池内最新 record 的下一槽；逐页组包（padding 归零）；
 *             **无论成败都推进游标**（半编程槽不可复用）；任一分页
 *             写失败 ⇒ 整条残缺 ⇒ 返回错误且**保留 dirty**（下次在新槽重试整条）。
 * -------------------------------------------------------------------- */
ftl_status_t metadata_dir_flush_to_anchor(metadata_dir_cache_t *dir)
{
    metadata_anchor_page_t page; /* 2KB 栈缓冲：**按页**组好后写入，勿放整份 Directory */
    metadata_ring_t        ring;
    nand_addr_t            base;
    uint16_t               idx;
    uint16_t               total;
    uint32_t               seq;
    uint8_t                chunk;
    int                    ok = 1;

    if ((dir == NULL) || (!dir->dirty))
    {
        return FTL_OK; /* 无变更早退：Anchor 每写一条都消耗池内槽位，空转会让池无谓轮转 */
    }

    anchor_ring_init(&ring);
    total = metadata_ring_total_records(&ring);

    /* 游标未建立（冷启动后首次 flush）⇒ 先定位池内最新有效 record 的下一槽 */
    if (dir->anchor_cursor_valid == 0U)
    {
        anchor_probe_ctx_t ctx;
        uint16_t           newest = UINT16_MAX;

        ctx.dir  = dir;
        ctx.ring = &ring;

        if (metadata_ring_locate(&ring, &dir->topo, anchor_record_probe, &ctx, &newest, &dir->anchor_sequence, &dir->anchor_write_idx) != FTL_OK)
        {
            return FTL_ERROR;
        }
        dir->anchor_cursor_valid = 1U;
    }

    idx = dir->anchor_write_idx;
    seq = ++dir->anchor_sequence;

    if (metadata_ring_prepare_write(&ring, &dir->topo, idx, &base) != FTL_OK)
    {
        return FTL_ERROR;
    }

    /* ⚠ 临时诊断探针（`log level debug` 才打印）：Anchor 写点。定位完即删。 */
    LOGD("[META] anchor write: idx=%u seq=%lu blk=%u pl=%u pg=%u\r\n",
         (unsigned)idx,
         (unsigned long)seq,
         (unsigned)base.block,
         (unsigned)base.plane,
         (unsigned)base.page);

    for (chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
    {
        uint16_t    base_entry = (uint16_t)(chunk * METADATA_DIR_ENTRIES_PER_PAGE);
        uint16_t    n;
        uint16_t    k;
        nand_addr_t aaddr = base;

        aaddr.page = (uint8_t)(base.page + chunk);

        /* 本页有效条目数 = 全局有效范围 [0,count) 与本页区间 [base,base+PER_PAGE) 的交 */
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

        memset(&page, 0, sizeof(page)); /* padding 归零 ⇒ 介质内容确定 */
        page.magic          = ANCHOR_MAGIC;
        page.sequence       = seq;
        page.chunk_index    = chunk;
        page.chunk_count    = ANCHOR_PAGE_COUNT;
        page.entry_count    = n;
        page.layout_version = FTL_MEDIA_LAYOUT_VERSION;

        for (k = 0U; k < METADATA_DIR_ENTRIES_PER_PAGE; k++)
        {
            if (k < n)
            {
                page.entries[k] = dir->entries[base_entry + k];
            }
            else
            {
                page.entries[k].type = 0U;
                page.entries[k].id   = 0U;
                page.entries[k].paa  = NAND_PBA_INVALID;
            }
        }

        if (ftl_sync_write_page(aaddr, &page, sizeof(page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) != 0)
        {
            ok = 0; /* 本条 record 残缺 ⇒ 下次 locate 会整条丢弃它（不取半条）*/
        }
    }

    /* 无论成败都推进游标：已写过的槽（可能半编程）不可复用 */
    dir->anchor_write_idx = (uint16_t)((idx + 1U) % total);

    if (ok == 0)
    {
        /* 观测口：Anchor 只写了一半 ⇒ 整条作废（下次 locate 会整条丢弃）。
         * 这是"开机 anchor_seq 恒 0"的**另一个**可能来源，必须留痕。 */
        LOGW("[META] anchor write incomplete: idx=%u seq=%lu (chunk 写失败)\r\n",
             (unsigned)idx,
             (unsigned long)seq);
        return FTL_ERROR; /* dirty 保留 ⇒ 下次在新槽重试整条 */
    }

    dir->dirty        = 0U;
    dir->change_count = 0U;

    return FTL_OK;
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
        uint16_t    base_entry = (uint16_t)(chunk * METADATA_DIR_ENTRIES_PER_PAGE);
        nand_addr_t addr       = metadata_ring_addr(&ring, &dir->topo, newest, chunk);
        uint16_t    n;
        uint16_t    k;

        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) != 0)
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
    dir->compacting         = 0U; /* 压缩不跨重启（同游标：RAM 态，冷启动恒 0）*/

    metadata_dir_boot_log(dir);

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 六、元数据块判定
 * ══════════════════════════════════════════════════════════════════ */

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

    /* ── 固定元数据块：遍历分区表（ftl/ftl_config.h §五）──
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
                ((uint8_t)paa_to_plane(md_dir->entries[i].paa) == plane) &&
                (paa_to_block(md_dir->entries[i].paa) == block))
            {
                return 1;
            }
        }
    }

    return 0;
}
