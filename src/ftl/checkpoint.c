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
 *   写游标 ftl->cp.write_idx 在 [0, 总记录数) 环形推进，块满由 metadata_ring_prepare_write 擦下一块。
 *
 * 掉电安全顺序（不可倒，见 checkpoint_create 步骤③/⑦）：
 *   先 flush region → 再 flush Anchor → 最后写 Header 声称「已落盘」。
 *   Header 是"承诺书"：它一旦落盘，boot 就信 bitmap 里置位的 region 在 NAND 上，
 *   故任何被 Header 声称的落盘必须**先于** Header 完成。
 *
 * 文件结构：
 *   1. 内部辅助        checkpoint_now_ms / checkpoint_header_valid / checkpoint_record_probe
 *   2. 脏区落盘        checkpoint_flush_dirty_regions（static）
 *   3. 创建/恢复       checkpoint_create / checkpoint_find_latest
 *   4. 触发/水位       checkpoint_check_trigger / checkpoint_is_emergency / checkpoint_journal_watermark
 *
 * 游标/序号是 **per-instance**（序 4：收进 ftl_context_t.cp，见 l2p_cp_state_t）——
 * 多实例（多 LUN）下每实例一份，互不干扰。
 */

#include "ftl/checkpoint.h"

#include "fc/fc.h"
#include "ftl/ftl.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/metadata.h"
#include "nand/onfi_1_0.h"
#include "platform/platform.h"

#include <string.h>

/* ── 内部辅助 ── */

/*
 * checkpoint_now_ms - 当前单调毫秒（经平台抽象 time_us 换算）。
 * 平台未注册时返回 0，checkpoint 头部时间戳退化为 0（行为同接线前）。
 */
static uint32_t checkpoint_now_ms(const ftl_context_t *ftl)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();

    (void)ftl;
    if ((platform == NULL) || (platform->time_us == NULL))
    {
        return 0U;
    }
    return (uint32_t)(platform->time_us() / 1000U);
}

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
    ring->module           = FC_COMMAND_MODULE_CHECKPOINT;
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
    nand_addr_t             addr;

    (void)full;

    addr = metadata_ring_addr(&c->ring, &c->topo, record_idx, 0U);

    if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_CHECKPOINT)) != 0)
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

/* ── 脏区落盘 ── */

/*
 * checkpoint_flush_dirty_regions - 按 region 脏标记只 flush 脏区
 *
 * 覆盖：cache 中所有 state==DIRTY 且其 region 的脏标记置位的行
 *（等价于"本轮周期内改过的 region"；与 flush_all 对 resident 行的
 * 覆盖相同，语义上先快照 bitmap 再 flush）。
 *
 * 任一 flush 失败立即返回 FTL_ERROR——调用方必须中止 checkpoint：
 * 该 region 的 NAND 页仍旧（脏标记保持置位），若继续写 header 声称其
 * 已落盘，boot 会按 bitmap 预载旧页 → 数据一致性破坏。
 */
static ftl_status_t checkpoint_flush_dirty_regions(l2p_cache_t *cache)
{
    uint16_t i;

    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &cache->lines[i];
        uint32_t          word;
        uint32_t          mask;

        if (line->state != L2P_CACHE_DIRTY)
        {
            continue;
        }

        /* 只有 region 脏标记置位的行才需要写回（其余在本次周期内未改） */
        word = cache->region_dirty_bitmap[line->region_id / 32U];
        mask = (1UL << (line->region_id & 31U));
        if ((word & mask) == 0U)
        {
            continue;
        }

        if (l2p_cache_flush_one(cache, (int)i) != FTL_OK)
        {
            return FTL_ERROR;
        }
    }

    return FTL_OK;
}

/* ── 创建 / 恢复 ── */

/* --------------------------------------------------------------------
 * checkpoint_create
 *   功能：执行一次完整 Checkpoint（脏区落盘 → Anchor → Header → 截断 Journal）。
 *   参数：ftl —— FTL 实例。
 *   返回：FTL_OK 成功；FTL_ERROR 参数非法 / 脏区落盘失败 / Anchor 落盘失败 /
 *         池定位失败 / 写 Header 失败。
 *   实现要点：八个步骤的**顺序即掉电安全契约**（圈码见正文）——
 *     ① 先快照 region 脏标记进 Header（flush 会清位，快照必须在前）；
 *     ② flush 脏区；③ flush Anchor（必须早于 Header）；⑤ 定位后才取 sequence；
 *     ⑥ 追加写并**无论成败都推进游标**（半编程槽不可复用）；⑦ 截断 Journal；
 *     ⑧ 复位 Journal 水位标志（否则 T1 永久命中）。
 *     任一前置步骤失败即 return，不写 Header——保持"承诺与事实一致"。
 * -------------------------------------------------------------------- */
ftl_status_t checkpoint_create(ftl_context_t *ftl)
{
    l2p_checkpoint_header_t cp;
    metadata_ring_t         ring;
    checkpoint_probe_ctx_t  probe_ctx;
    nand_addr_t             addr;
    uint8_t                 i;
    uint32_t                region_words = 0U;

    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    /* ① 快照 region 脏标记 → 构造 Header（先快照后 flush：flush 会清位） */
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.region_bitmap, ftl->cache.region_dirty_bitmap, sizeof(cp.region_bitmap));

    /* region_count = 置位 word 数（无消费方，仅信息量；多 LUN 时高位 word 恒 0） */
    for (i = 0U; i < 64U; i++)
    {
        if (cp.region_bitmap[i] != 0U)
        {
            region_words++;
        }
    }
    cp.region_count = region_words;

    /* ② 按 bitmap 只 flush 脏 region → NAND L2P 日志块。
     *    flush 失败必须中止（见 checkpoint_flush_dirty_regions 注释），
     *    不写 header / 不 flush anchor / 不截断 journal，保 DIRTY 行与
     *    journal 供下个 checkpoint 重试（95% 紧急水位仍兜底强制）。 */
    if (checkpoint_flush_dirty_regions(&ftl->cache) != FTL_OK)
    {
        return FTL_ERROR;
    }

    /* ③ 先 flush Metadata Directory（Anchor）——必须早于 Header：
     *    header 声称「bitmap 置位的 region 已落盘」，若 Directory 落后于 header，
     *    掉电后 boot 按 bitmap 预载会读到 Directory 里不存在的 region。 */
    if (metadata_dir_flush_to_anchor(&ftl->metadata_dir) != FTL_OK)
    {
        return FTL_ERROR; /* Anchor 未落盘 ⇒ 不能写 header 声称已落盘 */
    }

    /* ④ 定位写游标（游标未建立时才扫池：开机 checkpoint_find_latest 已建立时零代价）*/
    checkpoint_ring_init(&ring);
    probe_ctx.topo = ftl->topo;
    probe_ctx.ring = ring;

    if (ftl->cp.cursor_valid == 0U)
    {
        uint16_t newest = UINT16_MAX;

        if (metadata_ring_locate(&ring, &probe_ctx.topo, checkpoint_record_probe, &probe_ctx, &newest, &ftl->cp.sequence, &ftl->cp.write_idx) != FTL_OK)
        {
            return FTL_ERROR;
        }
        ftl->cp.cursor_valid = 1U;
    }

    /* ⑤ 完成 Header 剩余字段（sequence 在定位之后取，池内已有序号时才能续上）*/
    cp.magic            = CHECKPOINT_MAGIC;
    cp.sequence         = ++ftl->cp.sequence;            /* 本池自己的单调序号 */
    cp.timestamp        = checkpoint_now_ms(ftl);        /* 平台 time_us 换算 ms 快照 */
    cp.journal_sequence = ftl->journal.sequence_counter; /* 截断到此 seq */
    cp.layout_version   = FTL_MEDIA_LAYOUT_VERSION;      /* 介质布局版本（版本不符 ⇒ 该记录作废）*/

    /* ⑥ 追加写：池内下一槽（块满则先擦池内下一块）*/
    if (metadata_ring_prepare_write(&ring, &probe_ctx.topo, ftl->cp.write_idx, &addr) != FTL_OK)
    {
        return FTL_ERROR;
    }

    /* 无论成败都推进游标：已写过的槽（可能半编程）不可复用。
     * 失败时不写 journal_truncate，脏数据仍由下个 checkpoint 重试。 */
    if (ftl_sync_write_page(addr, &cp, sizeof(cp), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_CHECKPOINT)) != 0)
    {
        ftl->cp.write_idx = (uint16_t)((ftl->cp.write_idx + 1U) % metadata_ring_total_records(&ring));
        return FTL_ERROR;
    }

    ftl->cp.write_idx = (uint16_t)((ftl->cp.write_idx + 1U) % metadata_ring_total_records(&ring));

    /* ⑦ 截断 Journal（间接入口：纯 RAM 测试构建可用弱桩替换 journal_truncate） */
    journal_truncate(&ftl->journal, cp.journal_sequence);

    /* ⑧ 复位 Journal 水位标志（设计 §7.5.4：checkpoint 完成后 j->emergency /
     *    checkpoint_needed 置 false——本次 CP 已把 sequence ≤ cp_seq 的条目 flush 进
     *    NAND L2P 表并截断 journal，水位已清零。不复位会导致 T1 永久命中（每圈必 CP）
     *    且 emergency 置位后 BUSY 模式每圈强制 CP（ftl_service_busy）。 */
    ftl->journal.emergency         = 0U;
    ftl->journal.checkpoint_needed = 0U;

    return FTL_OK;
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
    nand_addr_t            addr;

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
    if (ftl_sync_read_page(addr, cp, sizeof(*cp), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_CHECKPOINT)) != 0)
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
    nand_addr_t             addr;

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
    if (ftl_sync_write_page(addr, &cp, sizeof(cp), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_CHECKPOINT)) != 0)
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
