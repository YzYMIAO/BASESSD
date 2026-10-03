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

/* Checkpoint CRC 覆盖的字节数（从开头到 crc 字段） */
#define CHECKPOINT_CRC_SIZE   (sizeof(uint32_t) * 6)                        /* magic+seq+ts+rc+bitmap[64]+js = 6 fields before crc */
#define CHECKPOINT_CRC_OFFSET (CHECKPOINT_CRC_SIZE + sizeof(uint32_t) * 64) /* + region_bitmap[64] */
/* = 24 + 256 = 280 bytes covered by CRC */

/* ── 内部同步写页（原 ftl_internal_write_page 内联化，固定地址直填）── */
static ftl_status_t sync_write_page(nand_addr_t addr, const void *data, uint32_t length,
                                    uint8_t module)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = 0U;

    if (addr_to_cmd(cmd, &addr) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U))
    {
        (void)ftl_cmd_write_data(cmd, data, length);
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (completion.status & FC_CW_STATUS_NAND_FAIL)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* ── 内部同步读页（原 ftl_internal_read_page 内联化）── */
static ftl_status_t sync_read_page(nand_addr_t addr, void *buffer, uint32_t length,
                                   uint8_t module)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = 0U;

    if (addr_to_cmd(cmd, &addr) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (completion.status & FC_CW_STATUS_NAND_FAIL)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if ((buffer != NULL) && (length > 0U))
    {
        (void)ftl_cmd_read_data(cmd, buffer, length);
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* ── Checkpoint 固定位置页地址（Block0/Block1 各 3 页）── */
static nand_addr_t checkpoint_page_addr(uint8_t ch, uint8_t ce, uint8_t lun, uint8_t i)
{
    nand_addr_t addr;

    addr.ch     = ch;
    addr.ce     = ce;
    addr.lun    = lun;
    addr.plane  = (uint8_t)((i & 1U) & 1U); /* 页号奇偶 → P1/P0（双 Plane 交错） */
    addr.page   = (uint8_t)((i < 3U) ? i : (i - 3U));
    addr.block  = (uint8_t)((i < 3U) ? CHECKPOINT_BLOCK0 : CHECKPOINT_BLOCK1);
    addr.cw     = 0U;

    return addr;
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
    uint8_t                 i;
    int                     ret_total = 0;
    uint32_t                region_words = 0U;

    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    /* ① 快照 region 脏标记 → 构造 Header（先快照后 flush：flush 会清位） */
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.region_bitmap, ftl->cache.region_dirty_bitmap,
           sizeof(cp.region_bitmap));

    /* region_count = 置位 word 数（无消费方，仅信息量；多 LUN 时高位 word 恒 0） */
    for (i = 0U; i < 64U; i++)
    {
        if (cp.region_bitmap[i] != 0U)
        {
            region_words++;
        }
    }
    cp.region_count = region_words;

    /* ② 按 bitmap 只 flush 脏 region → NAND L2P Table。
     *    flush 失败必须中止（见 checkpoint_flush_dirty_regions 注释），
     *    不写 header / 不 flush anchor / 不截断 journal，保 DIRTY 行与
     *    journal 供下个 checkpoint 重试（95% 紧急水位仍兜底强制）。 */
    if (checkpoint_flush_dirty_regions(&ftl->cache) != FTL_OK)
    {
        return FTL_ERROR;
    }

    /* ③ 完成 Header 剩余字段 */
    cp.magic            = CHECKPOINT_MAGIC;
    cp.sequence         = ftl->journal.sequence_counter; /* 用 journal 最新 sequence */
    cp.timestamp        = checkpoint_now_ms(ftl);         /* 平台 time_us 换算 ms 快照 */
    cp.journal_sequence = ftl->journal.sequence_counter; /* 截断到此 seq */

    /* CRC */
    cp.crc = 0U;
    cp.crc = boot_crc32(&cp, sizeof(cp) - sizeof(cp.padding) - 4); /* 不含 padding 和 crc 自身 */

    /* ④ 写 6 页冗余 */
    for (i = 0U; i < CHECKPOINT_TOTAL_PAGES; i++)
    {
        nand_addr_t pba;
        int         ret;

        pba = checkpoint_page_addr(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, i);

        ret = sync_write_page(pba, &cp, sizeof(cp), FC_COMMAND_MODULE_CHECKPOINT);
        if (ret != 0)
        {
            ret_total = -1; /* 单份失败不影响整体，有冗余 */
        }
    }

    /* ④ flush Metadata Directory（保证 Directory 最新） */
    metadata_dir_flush_to_anchor(&ftl->metadata_dir);

    /* ⑤ 截断 Journal（间接入口：纯 RAM 测试构建可用弱桩替换 journal_truncate） */
    journal_truncate(&ftl->journal, cp.journal_sequence);

    /* ⑥ 复位 Journal 水位标志（设计 §7.5.4：checkpoint 完成后 j->emergency /
     *    checkpoint_needed 置 false——本次 CP 已把 sequence ≤ cp_seq 的条目 flush 进
     *    NAND L2P 表并截断 journal，水位已清零。不复位会导致 T1 永久命中（每圈必 CP）
     *    且 emergency 置位后 BUSY 模式每圈强制 CP（ftl_service_busy）。 */
    ftl->journal.emergency         = 0U;
    ftl->journal.checkpoint_needed = 0U;

    return ret_total;
}

ftl_status_t checkpoint_find_latest(l2p_checkpoint_header_t *cp, ftl_topology_t topo)
{
    l2p_checkpoint_header_t  candidates[6];
    l2p_checkpoint_header_t *best = NULL;
    uint8_t                  i;
    int                      found = 0;

    if (cp == NULL)
    {
        return FTL_ERROR;
    }

    memset(candidates, 0, sizeof(candidates));

    for (i = 0U; i < CHECKPOINT_TOTAL_PAGES; i++)
    {
        nand_addr_t pba;
        int         ret;

        pba = checkpoint_page_addr(topo.ch, topo.ce, topo.lun, i);

        ret = sync_read_page(pba, &candidates[i], sizeof(candidates[i]), FC_COMMAND_MODULE_CHECKPOINT);
        if (ret != 0)
        {
            continue;
        }

        /* 验证 Magic */
        if (candidates[i].magic != CHECKPOINT_MAGIC)
        {
            continue;
        }

        /* 验证 CRC */
        {
            uint32_t saved_crc = candidates[i].crc;
            candidates[i].crc  = 0U;
            if (boot_crc32(&candidates[i], sizeof(candidates[i]) - sizeof(candidates[i].padding) - 4) != saved_crc)
            {
                candidates[i].crc = saved_crc;
                continue;
            }
            candidates[i].crc = saved_crc;
        }

        /* 选 sequence 最大的 */
        if ((best == NULL) || (candidates[i].sequence > best->sequence))
        {
            best  = &candidates[i];
            found = 1;
        }
    }

    if (!found)
    {
        return FTL_ERROR;
    }

    *cp = *best;
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
            (uint32_t)journal->current_page) * 100U
           / ((uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK)
           + ((uint32_t)journal->current_entry * 100U / JOURNAL_ENTRIES_PER_PAGE
              / ((uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK));

    if (used >= JOURNAL_CRITICAL)
    {
        journal->emergency = 1U; /* ≥95% 紧急 */
    }
    else if (used >= JOURNAL_FULL_THRESHOLD)
    {
        journal->checkpoint_needed = 1U; /* ≥85% 需 Checkpoint */
    }
}