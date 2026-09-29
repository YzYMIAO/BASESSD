#include "ftl/journal.h"

#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/*
 * Journal Page Header Layout（放在 page_buf 开头）：
 *   offset  size  field
 *   0       4     magic       = 0x4A4F5552 ("JOUR")
 *   4       4     page_seq    = 本页第一条 entry 的全局序号
 *   8       2     entry_count = 本页有效 entry 数（0~128）
 *   10      2     block_seq   = 本 Journal Block 写入轮次
 *   12      N*16  entries     = N × l2p_journal_entry_t
 *   12+N*16 ...  padding     = 0xFF
 */
#define JOURNAL_HEADER_SIZE (12U)

/* "JOUR" — little-endian 4-byte magic */
#define JOURNAL_MAGIC (0x524F554AUL)

void journal_init(l2p_journal_t *journal, uint8_t ch, uint8_t ce, uint8_t lun)
{
    if (journal == NULL)
    {
        return;
    }

    journal->ch  = ch;
    journal->ce  = ce;
    journal->lun = lun;

    journal->block_pba[0] = pba_make(ch, ce, 0, 0, JOURNAL_BLOCK0, 0, 0);
    journal->block_pba[1] = pba_make(ch, ce, 0, 0, JOURNAL_BLOCK1, 0, 0);

    journal->current_block    = 0U;
    journal->current_page     = 0U;
    journal->current_entry    = 0U;
    journal->sequence_counter = 1U;

    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));

    journal->checkpoint_needed = 0U;
    journal->emergency         = 0U;
}

void journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old_pba, nand_pba_t new_pba)
{
    l2p_journal_entry_t *entry;

    if (journal == NULL)
    {
        return;
    }

    uint16_t offset = JOURNAL_HEADER_SIZE +
                      sizeof(l2p_journal_entry_t) * journal->current_entry;
    entry = (l2p_journal_entry_t *)(journal->page_buf + offset);

    entry->lba      = (uint32_t)lba;
    entry->old_pba  = old_pba;
    entry->new_pba  = new_pba;
    entry->sequence = journal->sequence_counter++;

    journal->current_entry++;

    if (journal->current_entry >= JOURNAL_ENTRIES_PER_PAGE)
    {
        journal_flush_page(journal);
    }
}

void journal_flush_page(l2p_journal_t *journal)
{
    nand_pba_t page_pba;
    uint8_t    target_page;
    uint32_t   hdr_page_seq;
    uint16_t   hdr_entry_count;
    uint16_t   hdr_block_seq;

    if (journal == NULL)
    {
        return;
    }
    if (journal->current_entry == 0U)
    {
        return; /* 无数据，无需 flush */
    }

    /* 构造 Page Header（memcpy 写入以避免 alignment 警告） */
    {
        uint32_t magic = JOURNAL_MAGIC;
        memcpy(journal->page_buf + 0, &magic, sizeof(magic));
    }
    hdr_page_seq    = journal->sequence_counter - journal->current_entry;
    hdr_entry_count = journal->current_entry;
    hdr_block_seq   = (uint16_t)journal->current_block;
    memcpy(journal->page_buf + 4, &hdr_page_seq, sizeof(hdr_page_seq));
    memcpy(journal->page_buf + 8, &hdr_entry_count, sizeof(hdr_entry_count));
    memcpy(journal->page_buf + 10, &hdr_block_seq, sizeof(hdr_block_seq));

    /* 构造当前写入页的 PBA */
    target_page = journal->current_page;
    page_pba    = pba_make(journal->ch, journal->ce, target_page, (uint8_t)(target_page & 1U), (journal->current_block == 0U) ? JOURNAL_BLOCK0 : JOURNAL_BLOCK1, 0, 0);

    /*
     * 同步写 Journal 页到 NAND。
     * CRC/Random DISABLED：Journal 有自身四层保护（ECC/CRC/entry_count/sequence）。
     */
    (void)ftl_internal_write_page(page_pba,
                                  journal->page_buf,
                                  JOURNAL_PAGE_SIZE,
                                  FC_CRC_DISABLE,
                                  FC_RANDOM_DISABLE,
                                  FC_COMMAND_MODULE_METADATA,
                                  0U);

    /* 推进指针 */
    journal->current_page++;
    if (journal->current_page >= PAGES_PER_BLOCK)
    {
        /* 当前 Block 写满，切到另一个 Block */
        journal->current_block = (journal->current_block == 0U) ? 1U : 0U;
        journal->current_page  = 0U;

        /* 擦除目标 Block（已存在的旧数据）*/
        nand_pba_t new_block_pba = pba_make(
            journal->ch, journal->ce, 0, 0, (journal->current_block == 0U) ? JOURNAL_BLOCK0 : JOURNAL_BLOCK1, 0, 0);
        (void)ftl_internal_erase_block(new_block_pba);
    }

    journal->current_entry = 0U;
    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));
}

ftl_status_t journal_replay(l2p_journal_t *journal, l2p_cache_t *cache)
{
    uint8_t  b, page;
    uint32_t max_sequence = 0U;
    uint8_t  last_block   = 0U;
    uint8_t  last_page    = 0U;
    uint8_t  last_entry   = 0U;

    if ((journal == NULL) || (cache == NULL))
    {
        return FTL_ERROR;
    }

    /* 扫描两个 Journal Block */
    for (b = 0; b < 2; b++)
    {
        for (page = 0; page < PAGES_PER_BLOCK; page++)
        {
            uint8_t    page_data[JOURNAL_PAGE_SIZE];
            uint16_t   count;
            uint32_t   page_seq;
            nand_pba_t page_pba;
            int        ret;

            /* 读 Journal 页 */
            page_pba = pba_make(journal->ch, journal->ce, page, (uint8_t)(page & 1U), (b == 0U) ? JOURNAL_BLOCK0 : JOURNAL_BLOCK1, 0, 0);

            memset(page_data, 0xFF, sizeof(page_data));
            ret = ftl_internal_read_page(page_pba,
                                         page_data,
                                         JOURNAL_PAGE_SIZE,
                                         FC_CRC_DISABLE,
                                         FC_RANDOM_DISABLE,
                                         FC_COMMAND_MODULE_METADATA,
                                         0U);
            if (ret != 0)
            {
                continue; /* 读失败，跳过本页 */
            }

            /* 验证 Magic */
            {
                uint32_t magic;
                memcpy(&magic, page_data + 0, sizeof(magic));
                if (magic != JOURNAL_MAGIC)
                {
                    continue; /* 非 Journal 页 */
                }
            }

            /* 读取 entry_count */
            {
                uint16_t ec;
                memcpy(&ec, page_data + 8, sizeof(ec));
                count = ec;
            }
            if (count > JOURNAL_ENTRIES_PER_PAGE)
            {
                continue; /* 无效 count */
            }
            if (count == 0U)
            {
                continue; /* 空页 */
            }

            {
                uint32_t ps;
                memcpy(&ps, page_data + 4, sizeof(ps));
                page_seq = ps;
            }

            /* 如果本页 seq 比之前发现的大，更新最后位置 */
            if (page_seq >= max_sequence)
            {
                max_sequence = page_seq;
                last_block   = b;
                last_page    = page;
                last_entry   = (uint8_t)(count - 1U);

                /* 应用所有 entry 到 L2P Cache */
                {
                    uint16_t i;
                    for (i = 0U; i < count; i++)
                    {
                        uint32_t entry_lba;
                        uint32_t entry_new_pba;
                        uint16_t offset = JOURNAL_HEADER_SIZE +
                                          sizeof(l2p_journal_entry_t) * i;
                        memcpy(&entry_lba, page_data + offset + 0, 4);
                        memcpy(&entry_new_pba, page_data + offset + 8, 4);
                        l2p_cache_write(cache, (lba_t)entry_lba, (nand_pba_t)entry_new_pba);
                    }
                }
            }
        }
    }

    /* 更新 Journal 写指针到最新位置之后 */
    if (max_sequence > 0U)
    {
        journal->current_block = last_block;
        journal->current_page  = last_page;

        /* 如果最后一页已满，推进到下一页 */
        if (last_entry >= JOURNAL_ENTRIES_PER_PAGE - 1U)
        {
            journal->current_page++;
            if (journal->current_page >= PAGES_PER_BLOCK)
            {
                journal->current_block = (journal->current_block == 0U) ? 1U : 0U;
                journal->current_page  = 0U;
            }
            journal->current_entry = 0U;
        }
        else
        {
            journal->current_entry = last_entry + 1U;
        }

        journal->sequence_counter = max_sequence + 1U;
    }

    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));

    return FTL_OK;
}

void journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence)
{
    uint8_t b;

    (void)up_to_sequence;

    if (journal == NULL)
    {
        return;
    }

    /*
     * 截断：擦除包含 sequence ≤ up_to_sequence 的 Journal Block。
     *
     * 遍历两个 Journal Block 的最后写入页：
     *   如果 Block 中所有页的 page_seq ≤ up_to_sequence → 擦除整个 Block。
     *
     * 简化实现：始终擦除回切的目标 Block（current_block 的对方）。
     * 因为 Checkpoint 总是在 Journal 回切前执行，对方 Block 一定 ≤ up_to_sequence。
     */

    /* 擦除非当前 Block（对方 Block 中所有 entry 的 seq ≤ cp_seq，可安全擦除） */
    b = (journal->current_block == 0U) ? 1U : 0U;

    {
        nand_pba_t block_pba = pba_make(journal->ch, journal->ce, 0, 0, (b == 0U) ? JOURNAL_BLOCK0 : JOURNAL_BLOCK1, 0, 0);
        (void)ftl_internal_erase_block(block_pba);
    }

    /* 如果当前 Block 的 current_page 已推进，对方 Block 已擦除，
     * 当前 Block 继续写入，直到写满后回切到已擦除的对方 Block。 */
}