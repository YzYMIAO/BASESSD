#include "ftl/journal.h"

#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── Journal Block 页地址辅助 ── */
static nand_addr_t journal_page_addr(const l2p_journal_t *journal, uint8_t b, uint8_t page)
{
    nand_addr_t addr;

    addr.ch    = journal->topo.ch;
    addr.ce    = journal->topo.ce;
    addr.lun   = journal->topo.lun;
    addr.block = (b == 0U) ? JOURNAL_BLOCK0 : JOURNAL_BLOCK1;
    addr.plane = (uint8_t)(page & 1U); /* 偶页 P0, 奇页 P1（双 Plane 交错） */
    addr.page  = page;
    addr.cw    = 0U; /* 0 = 整页 */

    return addr;
}

static nand_addr_t journal_block_addr(const l2p_journal_t *journal, uint8_t b)
{
    nand_addr_t addr;

    addr.ch    = journal->topo.ch;
    addr.ce    = journal->topo.ce;
    addr.lun   = journal->topo.lun;
    addr.block = (b == 0U) ? JOURNAL_BLOCK0 : JOURNAL_BLOCK1;
    addr.plane = 0U;
    addr.page  = 0U;
    addr.cw    = 0U;

    return addr;
}

void journal_init(l2p_journal_t *journal, ftl_topology_t topo)
{
    if (journal == NULL)
    {
        return;
    }

    journal->topo = topo;

    journal->block_pba[0] = pba_from_addr(&(nand_addr_t){.ch = topo.ch, .ce = topo.ce, .block = JOURNAL_BLOCK0});
    journal->block_pba[1] = pba_from_addr(&(nand_addr_t){.ch = topo.ch, .ce = topo.ce, .block = JOURNAL_BLOCK1});

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
    nand_addr_t waddr;
    uint8_t     target_page;
    uint32_t    hdr_page_seq;
    uint16_t    hdr_entry_count;
    uint16_t    hdr_block_seq;

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

    /* 构造当前写入页的地址 */
    target_page = journal->current_page;
    waddr       = journal_page_addr(journal, (uint8_t)journal->current_block, target_page);

    /*
     * 同步写 Journal 页到 NAND。
     * CRC/Random DISABLED：Journal 有自身四层保护（ECC/CRC/entry_count/sequence）。
     */
    (void)ftl_sync_write_page(waddr, journal->page_buf, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));

    /* 推进指针 */
    journal->current_page++;
    if (journal->current_page >= PAGES_PER_BLOCK)
    {
        /* 当前 Block 写满，切到另一个 Block */
        journal->current_block = (journal->current_block == 0U) ? 1U : 0U;
        journal->current_page  = 0U;

        /* 擦除目标 Block（已存在的旧数据）*/
        (void)ftl_sync_erase_block(journal_block_addr(journal, (uint8_t)journal->current_block));
    }

    journal->current_entry = 0U;
    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));
}

ftl_status_t journal_replay(l2p_journal_t *journal, l2p_cache_t *cache, uint32_t cp_sequence)
{
    uint8_t  b, page;
    uint32_t max_sequence = 0U;
    uint8_t  last_block   = 0U;
    uint8_t  last_page    = 0U;
    uint8_t  last_entry   = 0U;

    /* F10：已发现的"需回放页"（page_seq > cp_sequence）记录表。
     * 双块 × 每块 64 页 ≤ 128 页；每页 4×4B=16B → 512B 栈上数组。
     * 按 (page_seq, entry_index) 递增序应用依赖先收集后排序。 */
    typedef struct
    {
        uint32_t page_seq;
        uint8_t  block;
        uint8_t  page;
        uint8_t  entry_count;
    } replay_page_t;
    replay_page_t replay_pages[JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK];
    uint8_t       replay_count = 0U;

    if ((journal == NULL) || (cache == NULL))
    {
        return FTL_ERROR;
    }

    /* 第一遍：扫描两个 Journal Block，收集需回放页（page_seq > cp_sequence） */
    for (b = 0; b < JOURNAL_BLOCK_COUNT; b++)
    {
        for (page = 0; page < PAGES_PER_BLOCK; page++)
        {
            uint8_t     page_data[JOURNAL_PAGE_SIZE];
            uint16_t    count;
            uint32_t    page_seq;
            nand_addr_t raddr;
            int         ret;

            /* 读 Journal 页 */
            raddr = journal_page_addr(journal, b, (uint8_t)page);

            memset(page_data, 0xFF, sizeof(page_data));
            ret = ftl_sync_read_page(raddr, page_data, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));
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

            /* 跟踪最大 page_seq（用于更新写指针） */
            if (page_seq >= max_sequence)
            {
                max_sequence = page_seq;
                last_block   = b;
                last_page    = page;
                last_entry   = (uint8_t)(count - 1U);
            }

            /* F10：只收集 post-checkpoint 页（≤ cp_sequence 的条目已在
             * checkpoint 时 flush 进 NAND L2P 表，无需回放） */
            if (page_seq > cp_sequence)
            {
                if (replay_count <
                    (uint8_t)(JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK))
                {
                    replay_pages[replay_count].page_seq    = page_seq;
                    replay_pages[replay_count].block       = b;
                    replay_pages[replay_count].page        = page;
                    replay_pages[replay_count].entry_count = (uint8_t)count;
                    replay_count++;
                }
            }
        }
    }

    /* 第二遍：按 (page_seq, entry_index) 递增序应用条目。
     * 插入排序（replay_count ≤ 128）：per-LBA last-write-wins——
     * 同 LBA 的旧条目（含 GC/WL 搬移记录）被后写覆盖，不指向已擦除块。 */
    {
        uint8_t i;
        uint8_t n;

        for (i = 1U; i < replay_count; i++)
        {
            replay_page_t key = replay_pages[i];

            n = i;
            while ((n > 0U) && (replay_pages[n - 1U].page_seq > key.page_seq))
            {
                replay_pages[n] = replay_pages[n - 1U];
                n--;
            }
            replay_pages[n] = key;
        }

        for (i = 0U; i < replay_count; i++)
        {
            uint8_t     page_data[JOURNAL_PAGE_SIZE];
            nand_addr_t raddr;
            int         ret;

            raddr = journal_page_addr(journal, replay_pages[i].block, replay_pages[i].page);

            memset(page_data, 0xFF, sizeof(page_data));
            ret = ftl_sync_read_page(raddr, page_data, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));
            if (ret != 0)
            {
                continue; /* 页已失效，跳过（应用顺序其余页不受影响） */
            }

            {
                uint16_t k;
                for (k = 0U; k < replay_pages[i].entry_count; k++)
                {
                    uint32_t entry_lba;
                    uint32_t entry_new_pba;
                    uint16_t offset = JOURNAL_HEADER_SIZE +
                                      sizeof(l2p_journal_entry_t) * k;
                    memcpy(&entry_lba, page_data + offset + 0, 4);
                    memcpy(&entry_new_pba, page_data + offset + 8, 4);
                    l2p_cache_write(cache, (lba_t)entry_lba, (nand_pba_t)entry_new_pba);
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

    (void)ftl_sync_erase_block(journal_block_addr(journal, b));

    /* 截断后水位清零：对方 Block 已擦除，当前 Block 继续写入直到回切。
     * 复位 watermark 标志（设计 §7.5.4）——truncate 即水位已清，
     * 双保险兜底 checkpoint_create 的复位（后者在 journal_truncate 之后）。 */
    journal->emergency         = 0U;
    journal->checkpoint_needed = 0U;

    /* 如果当前 Block 的 current_page 已推进，对方 Block 已擦除，
     * 当前 Block 继续写入，直到写满后回切到已擦除的对方 Block。 */
}