#include "ftl/checkpoint.h"

#include "common/boot_policy.h"
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
 * 池几何由 ftl_config.h 推导；sequence 与写游标是**本池自己的**（与 journal /
 * Anchor / BBT 的 sequence 互不相干），开机由 checkpoint_find_latest 恢复。
 *
 * ⚠ file-scope static：多实例（多 LUN）下所有实例共享同一份游标/序号。
 *   与 bbt.c / wear.c 同一局限，登记在案（见 plan §八「不做多实例」）。 */
static uint32_t g_cp_sequence     = 0U;
static uint16_t g_cp_write_idx    = 0U;
static uint8_t  g_cp_cursor_valid = 0U;

static void checkpoint_ring_init(metadata_ring_t *ring)
{
    ring->first_block      = CHECKPOINT_BLOCK0;
    ring->block_count      = CHECKPOINT_POOL_BLOCKS;
    ring->pages_per_record = 1U;
    ring->plane            = 0U;
    ring->module           = FC_COMMAND_MODULE_CHECKPOINT;
}

/* 整条校验（一条 record = 1 页）：magic + CRC */
static int checkpoint_header_valid(const l2p_checkpoint_header_t *hdr)
{
    if (hdr->magic != CHECKPOINT_MAGIC)
    {
        return 0;
    }

    return (boot_crc32(hdr, CHECKPOINT_CRC_LEN) == hdr->crc) ? 1 : 0;
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

    if (g_cp_cursor_valid == 0U)
    {
        uint16_t newest = UINT16_MAX;

        if (metadata_ring_locate(&ring, &probe_ctx.topo, checkpoint_record_probe, &probe_ctx, &newest, &g_cp_sequence, &g_cp_write_idx) != FTL_OK)
        {
            return FTL_ERROR;
        }
        g_cp_cursor_valid = 1U;
    }

    /* ⑤ 完成 Header 剩余字段（sequence 在定位之后取，池内已有序号时才能续上）*/
    cp.magic            = CHECKPOINT_MAGIC;
    cp.sequence         = ++g_cp_sequence;               /* 本池自己的单调序号 */
    cp.timestamp        = checkpoint_now_ms(ftl);        /* 平台 time_us 换算 ms 快照 */
    cp.journal_sequence = ftl->journal.sequence_counter; /* 截断到此 seq */
    cp.crc              = boot_crc32(&cp, CHECKPOINT_CRC_LEN);

    /* ⑥ 追加写：池内下一槽（块满则先擦池内下一块）*/
    if (metadata_ring_prepare_write(&ring, &probe_ctx.topo, g_cp_write_idx, &addr) != FTL_OK)
    {
        return FTL_ERROR;
    }

    /* 无论成败都推进游标：已写过的槽（可能半编程）不可复用。
     * 失败时不写 journal_truncate，脏数据仍由下个 checkpoint 重试。 */
    if (ftl_sync_write_page(addr, &cp, sizeof(cp), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_CHECKPOINT)) != 0)
    {
        g_cp_write_idx = (uint16_t)((g_cp_write_idx + 1U) % metadata_ring_total_records(&ring));
        return FTL_ERROR;
    }

    g_cp_write_idx = (uint16_t)((g_cp_write_idx + 1U) % metadata_ring_total_records(&ring));

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

ftl_status_t checkpoint_find_latest(l2p_checkpoint_header_t *cp, ftl_topology_t topo)
{
    checkpoint_probe_ctx_t probe_ctx;
    uint16_t               newest = UINT16_MAX;
    nand_addr_t            addr;

    if (cp == NULL)
    {
        return FTL_ERROR;
    }

    /* 复位本池状态：找不到有效 Header 时保持 0/无效（后续 create 从池首开始） */
    g_cp_sequence     = 0U;
    g_cp_write_idx    = 0U;
    g_cp_cursor_valid = 0U;

    checkpoint_ring_init(&probe_ctx.ring);
    probe_ctx.topo = topo;

    if (metadata_ring_locate(&probe_ctx.ring, &probe_ctx.topo, checkpoint_record_probe, &probe_ctx, &newest, &g_cp_sequence, &g_cp_write_idx) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (newest == UINT16_MAX)
    {
        g_cp_sequence  = 0U;
        g_cp_write_idx = 0U;
        return FTL_ERROR; /* 池内无有效 record */
    }

    g_cp_cursor_valid = 1U;

    /* 重读胜者（locate 只返回序号，Header 本体要再读一次）*/
    addr = metadata_ring_addr(&probe_ctx.ring, &probe_ctx.topo, newest, 0U);
    if (ftl_sync_read_page(addr, cp, sizeof(*cp), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_CHECKPOINT)) != 0)
    {
        return FTL_ERROR;
    }
    if ((checkpoint_header_valid(cp) == 0) || (cp->sequence != g_cp_sequence))
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

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

int checkpoint_is_emergency(const ftl_context_t *ftl)
{
    if (ftl == NULL)
    {
        return FTL_OK;
    }

    return (int)ftl->journal.emergency;
}

void checkpoint_journal_watermark(l2p_journal_t *journal)
{
    uint32_t used;

    if (journal == NULL)
    {
        return;
    }

    /*
     * 计算 Journal 使用率（双块环形全局口径）：
     *   used% = (current_block × PAGES_PER_BLOCK + current_page) × 100
     *           + current_entry × 100 / JOURNAL_ENTRIES_PER_PAGE
     *           ──────────────────────────────────────────────────────
     *           (JOURNAL_BLOCK_COUNT × PAGES_PER_BLOCK)
     *
     * 分子必须是「全局页偏移」= current_block×64 + current_page（双块环形），
     * 而非单块内 current_page（0..63）。旧实现两处都错：
     *   ① 分母用 PAGES_PER_BLOCK=64（单块）→ 85%/95% 在 page≈55/61 就触发，
     *      紧急水位离写满只剩 3 页≈384 条
     *   ② 若只改分母 128 而分子仍用 current_page → used 最大 63×100/128≈49%，
     *      85%/95% 永远不触发（更糟）
     * 现分子分母都按双块 128 页口径：85%（JOURNAL_FULL_THRESHOLD）在全局
     * 页≈108（留 19 页）、95%（JOURNAL_CRITICAL）在全局页≈122（留 6 页≈768 条），
     * 与 §7.5.2 推导对齐。
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