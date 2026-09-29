#include "ftl/checkpoint.h"

#include "common/boot_policy.h"
#include "fc/fc.h"
#include "ftl/ftl.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/metadata.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* Checkpoint CRC 覆盖的字节数（从开头到 crc 字段） */
#define CHECKPOINT_CRC_SIZE   (sizeof(uint32_t) * 6)                        /* magic+seq+ts+rc+bitmap[64]+js = 6 fields before crc */
#define CHECKPOINT_CRC_OFFSET (CHECKPOINT_CRC_SIZE + sizeof(uint32_t) * 64) /* + region_bitmap[64] */
/* = 24 + 256 = 280 bytes covered by CRC */

ftl_status_t checkpoint_create(ftl_context_t *ftl)
{
    l2p_checkpoint_header_t cp;
    uint8_t                 i;
    int                     ret_total = 0;

    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    /* ① flush 所有 dirty cache → NAND L2P Table */
    l2p_cache_flush_all(&ftl->cache);

    /* ② 构造 Header */
    memset(&cp, 0, sizeof(cp));
    cp.magic            = CHECKPOINT_MAGIC;
    cp.sequence         = ftl->journal.sequence_counter; /* 用 journal 最新 sequence */
    cp.timestamp        = 0U;                            /* timer_get_ms 暂不可用，填 0 */
    cp.region_count     = L2P_REGIONS_TOTAL;             /* 全量 checkpoint */
    cp.journal_sequence = ftl->journal.sequence_counter; /* 截断到此 seq */

    /* region_bitmap: 全 1（全量） */
    for (i = 0U; i < 64U; i++)
    {
        cp.region_bitmap[i] = 0xFFFFFFFFU;
    }

    /* CRC */
    cp.crc = 0U;
    cp.crc = boot_crc32(&cp, sizeof(cp) - sizeof(cp.padding) - 4); /* 不含 padding 和 crc 自身 */

    /* ③ 写 6 页冗余 */
    for (i = 0U; i < CHECKPOINT_TOTAL_PAGES; i++)
    {
        nand_pba_t pba;
        uint8_t    block;
        uint8_t    page;
        int        ret;

        if (i < 3U)
        {
            block = CHECKPOINT_BLOCK0;
            page  = i;
        }
        else
        {
            block = CHECKPOINT_BLOCK1;
            page  = (uint8_t)(i - 3U);
        }

        pba = pba_make(ftl->ch, ftl->ce, page, (uint8_t)(page & 1U), block, 0, 0);

        ret = ftl_internal_write_page(pba, &cp, sizeof(cp), FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_COMMAND_MODULE_CHECKPOINT, 0U);
        if (ret != 0)
        {
            ret_total = -1; /* 单份失败不影响整体，有冗余 */
        }
    }

    /* ④ flush Metadata Directory（保证 Directory 最新） */
    metadata_dir_flush_to_anchor(&ftl->metadata_dir);

    /* ⑤ 截断 Journal */
    journal_truncate(&ftl->journal, cp.journal_sequence);

    return ret_total;
}

ftl_status_t checkpoint_find_latest(l2p_checkpoint_header_t *cp)
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
        nand_pba_t pba;
        uint8_t    block;
        uint8_t    page;
        int        ret;

        if (i < 3U)
        {
            block = CHECKPOINT_BLOCK0;
            page  = i;
        }
        else
        {
            block = CHECKPOINT_BLOCK1;
            page  = (uint8_t)(i - 3U);
        }

        pba = pba_make(0, 0, page, (uint8_t)(page & 1U), block, 0, 0);

        ret = ftl_internal_read_page(pba, &candidates[i], sizeof(candidates[i]), FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_COMMAND_MODULE_CHECKPOINT, 0U);
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
     * 计算 Journal 使用率：
     *   used% = (current_page × 100 + current_entry × 100 / JOURNAL_ENTRIES_PER_PAGE)
     *           / PAGES_PER_BLOCK
     */
    used = ((uint32_t)journal->current_page * 100U +
            (uint32_t)journal->current_entry * 100U / JOURNAL_ENTRIES_PER_PAGE) /
           PAGES_PER_BLOCK;

    if (used >= JOURNAL_CRITICAL)
    {
        journal->emergency = 1U; /* ≥95% 紧急 */
    }
    else if (used >= JOURNAL_FULL_THRESHOLD)
    {
        journal->checkpoint_needed = 1U; /* ≥85% 需 Checkpoint */
    }
}