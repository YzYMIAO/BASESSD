/*
 * journal.c — L2P 增量日志（环形块池 + 页追加 + 掉电回放 + 截断）
 *
 * 职责：
 *   把每次 L2P 更新（lba, old_pba, new_pba）以「页追加」写进 Journal 环形块池，
 *   供掉电后回放恢复；与 Checkpoint 协同做日志回收（truncate）。
 *
 * 与 journal.h 的分工：
 *   journal.h —— 页头格式（JOURNAL_HEADER_SIZE / MAGIC）+ 条目结构 + 环形状态
 *                （l2p_journal_t）；
 *   本文件     —— 追加 / 刷页 / 回放 / 截断的实现。
 *
 * 写模型（P1-6）：
 *   · 追加进 RAM 页缓冲（page_buf）；页满（JOURNAL_ENTRIES_PER_PAGE）或显式 flush
 *     时同步写一页到 NAND；
 *   · 块满换环形下一块——**先擦后写**，journal_flush_page 是唯一的环形块擦除点；
 *   · 同 LBA 就地合并（不消耗 sequence）。
 *
 * 掉电保护：Journal 页 Random **关闭**、CRC **开启**（v0.62 起由下层 FC 提供 per-CW CRC +
 *   ECC），外加页头 magic + entry_count 合法性 + page_seq 单调三层结构判据（设计 §7.6）。
 *
 * ⚠ **本文件整个不转断言**（assertion_guidelines §六 辨析 C）：replay 读的是 NAND 上的
 *   页，页头可能因掉电撕裂而非法——那是掉电的**预期情形**，必须返回错误/跳过，不能断言。
 *
 * 文件结构：
 *   1. 页地址辅助     journal_page_addr / journal_block_addr（static）
 *   2. 初始化         journal_init
 *   3. 追加与刷页     journal_append / journal_flush_page
 *   4. 回放与写指针恢复 journal_scan_latest / journal_resume / journal_replay
 *   5. 截断           journal_truncate
 */

#include "ftl/journal.h"

#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════
 * 1. 页地址辅助
 * ══════════════════════════════════════════════════════════ */

/* ── Journal Block 页地址辅助 ── */
/* journal_page_addr — 第 b 块第 page 页的 NAND 地址（plane 由页号奇偶决定，见下） */
static nand_pba_t journal_page_addr(const l2p_journal_t *journal, uint8_t b, uint8_t page)
{
    /* cw = 0 = 整页；plane 由页号奇偶决定：偶页 P0、奇页 P1（双 Plane 交错） */
    return pba_make(journal->topo.ch, journal->topo.ce, journal->topo.lun, (uint32_t)(JOURNAL_BLOCK0 + b), (uint32_t)(page & 1U), page, 0U);
}

/* journal_block_addr — 第 b 块的首页地址（擦除用；plane/page 归零，cw=0） */
static nand_pba_t journal_block_addr(const l2p_journal_t *journal, uint8_t b)
{
    return pba_make(journal->topo.ch, journal->topo.ce, journal->topo.lun, (uint32_t)(JOURNAL_BLOCK0 + b), 0U, 0U, 0U);
}

/* ══════════════════════════════════════════════════════════
 * 2. 初始化
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * journal_init
 *   功能：初始化 Journal 环形块池——记录拓扑、预铸各块 PBA、复位写指针与序号。
 *   参数：journal —— 待初始化对象；topo —— 目标 NAND 芯片路由。
 *   返回：无（journal == NULL 直接返回）。
 *   实现要点：sequence_counter 从 1 起（0 保留作「无序号」）；page_buf 填 0xFF 表示
 *             空页（与 NAND 擦除态一致）。
 * -------------------------------------------------------------------- */
void journal_init(l2p_journal_t *journal, ftl_topology_t topo)
{
    uint8_t b;

    if (journal == NULL)
    {
        return;
    }

    journal->topo = topo;

    for (b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        journal->block_pba[b] = pba_make(topo.ch, topo.ce, topo.lun, (uint32_t)(JOURNAL_BLOCK0 + b), 0U, 0U, 0U);
    }

    journal->current_block    = 0U;
    journal->current_page     = 0U;
    journal->current_entry    = 0U;
    journal->sequence_counter = 1U;
    journal->pre_erased_block = JOURNAL_BLOCK_NONE;

    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));

    journal->checkpoint_needed = 0U;
    journal->emergency         = 0U;
}

/* ══════════════════════════════════════════════════════════
 * 3. 追加与刷页
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * journal_append
 *   功能：向当前页缓冲追加一条 L2P 更新条目（同 LBA 就地合并）。
 *   参数：journal —— Journal；lba —— 逻辑地址；old_pba/new_pba —— 更新前后物理地址。
 *   返回：无。
 *   实现要点：页满（current_entry 达 JOURNAL_ENTRIES_PER_PAGE）自动 flush 一页。
 * -------------------------------------------------------------------- */
void journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old_pba, nand_pba_t new_pba)
{
    l2p_journal_entry_t *entry;
    uint16_t             i;

    if (journal == NULL)
    {
        return;
    }

    /*
     * P1-6 更新合并：当前页缓冲里已有同一 LBA 的条目 ⇒ 就地更新，不新增条目。
     * 回放按页序 + 页内下标应用（per-LBA last-write-wins），页内同 LBA 只保留最新
     * new_pba 即正确；old_pba 仅首条保留（回放不读 old_pba）。
     * ⚠ **不**消耗 sequence、**不**推进 current_entry：hdr_page_seq 由
     * `sequence_counter − current_entry` 反推本页首条序号，二者必须同步推进，
     * 否则页序错乱会导致回放顺序错误。
     */
    for (i = 0U; i < journal->current_entry; i++)
    {
        entry = (l2p_journal_entry_t *)(journal->page_buf + JOURNAL_HEADER_SIZE +
                                        sizeof(l2p_journal_entry_t) * i);
        if (entry->lba == (uint32_t)lba)
        {
            entry->new_pba = new_pba; /* 保留首条 old_pba，更新到最新 new_pba */
            return;
        }
    }

    entry = (l2p_journal_entry_t *)(journal->page_buf + JOURNAL_HEADER_SIZE +
                                    sizeof(l2p_journal_entry_t) * journal->current_entry);

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

/* --------------------------------------------------------------------
 * journal_flush_page
 *   功能：把当前页缓冲连同页头同步写进 NAND 当前页，并推进写指针。
 *   参数：journal —— Journal。
 *   返回：无（空页直接返回）。
 *   实现要点：
 *     · 页头 hdr_page_seq 由 `sequence_counter − current_entry` **反推**本页首条序号
 *       （与 journal_append 的就地合并约定耦合，二者必须同步）。
 *     · 页满换块：本函数是**唯一**的环形块擦除点；若该块已被 journal_truncate
 *       预擦除（pre_erased_block），**跳过擦除**（否则同块每圈擦两次，+25% 磨损）。
 *     · 写后清空页缓冲（0xFF）与 current_entry，准备下一页。
 * -------------------------------------------------------------------- */
void journal_flush_page(l2p_journal_t *journal)
{
    nand_pba_t waddr;
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

    /* 构造当前写入页的地址 */
    target_page = journal->current_page;
    waddr       = journal_page_addr(journal, (uint8_t)journal->current_block, target_page);

    /*
     * 同步写 Journal 页到 NAND。
     * Random DISABLED / CRC ENABLED（v0.62）：Journal 有 FC per-CW CRC + ECC，
     * 外加自身结构判据（magic/entry_count/sequence）。
     */
    (void)ftl_sync_write_page(waddr, journal->page_buf, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));

    /* 推进指针 */
    journal->current_page++;
    if (journal->current_page >= PAGES_PER_BLOCK)
    {
        /* 当前 Block 写满，切到下一 Block（环形，共 JOURNAL_BLOCK_COUNT 块）*/
        journal->current_block = (uint8_t)((journal->current_block + 1U) % JOURNAL_BLOCK_COUNT);
        journal->current_page  = 0U;

        /* 回切擦除（本函数是**唯一**的环形块擦除点）。
         * P1-6：若该块已被 journal_truncate 预擦除（水位截断），跳过——否则同一块每圈
         * 被擦两次，把 §14.1 认定的 Journal 寿命瓶颈又放大 ~25%。 */
        if (journal->pre_erased_block == journal->current_block)
        {
            journal->pre_erased_block = JOURNAL_BLOCK_NONE; /* 记账消费掉：本块即将被写 */
        }
        else
        {
            (void)ftl_sync_erase_block(journal_block_addr(journal, (uint8_t)journal->current_block));
        }
    }

    journal->current_entry = 0U;
    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));
}

/* ══════════════════════════════════════════════════════════
 * 4. 回放与写指针恢复
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * journal_scan_ring
 *   功能：扫一遍环形块池——找**最新页**（最大 page_seq），并按需收集需回放页。
 *   参数：journal —— Journal；cp_sequence —— 回放水位；
 *         out_pages / capacity —— 收集数组与其容量（out_pages == NULL = 只找最新页）；
 *         out_count —— 出参收集条数（可为 NULL）；out_latest —— 出参最新页（可为 NULL）。
 *   返回：无。
 *   实现要点：只读 NAND，**不改 journal 状态**（怎么用由调用方决定）。
 *             需回放判据是**整页**的：`page_seq + entry_count > cp_sequence`。
 *             旧判据只看页首页序号（`page_seq > cp_sequence`），跨水位的那一页会被
 *             整页跳过——它页尾的新条目（> cp_sequence）随页面一起丢。
 *             页头 magic / entry_count 校验失败即跳过（掉电撕裂的预期情形，见文件头 ⚠）。
 * -------------------------------------------------------------------- */
static void journal_scan_ring(const l2p_journal_t *journal,
                              uint32_t             cp_sequence,
                              journal_page_ref_t  *out_pages,
                              uint16_t             capacity,
                              uint16_t            *out_count,
                              journal_page_ref_t  *out_latest)
{
    uint16_t count = 0U;
    uint8_t  b, page;

    if (out_latest != NULL)
    {
        out_latest->page_seq    = 0U;
        out_latest->block       = 0U;
        out_latest->page        = 0U;
        out_latest->entry_count = 0U;
    }

    for (b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        for (page = 0U; page < PAGES_PER_BLOCK; page++)
        {
            uint8_t    page_data[JOURNAL_PAGE_SIZE];
            uint32_t   page_seq;
            uint16_t   entry_count;
            nand_pba_t raddr;

            raddr = journal_page_addr(journal, b, (uint8_t)page);

            memset(page_data, 0xFF, sizeof(page_data));
            if (ftl_sync_read_page(raddr, page_data, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) !=
                0)
            {
                continue; /* 读失败，跳过本页 */
            }

            {
                uint32_t magic;
                memcpy(&magic, page_data + 0, sizeof(magic));
                if (magic != JOURNAL_MAGIC)
                {
                    continue; /* 非 Journal 页 */
                }
            }

            memcpy(&page_seq, page_data + 4, sizeof(page_seq));
            memcpy(&entry_count, page_data + 8, sizeof(entry_count));
            if ((entry_count == 0U) || (entry_count > JOURNAL_ENTRIES_PER_PAGE))
            {
                continue; /* 空页 / 非法 count */
            }

            /* 最新页（用于恢复写指针与序号）：取 page_seq 最大者 */
            if ((out_latest != NULL) && (page_seq >= out_latest->page_seq))
            {
                out_latest->page_seq    = page_seq;
                out_latest->block       = b;
                out_latest->page        = (uint8_t)page;
                out_latest->entry_count = (uint8_t)entry_count;
            }

            /* 需回放页：本页**最后一条**的序号已过水位 ⇒ 页内至少一条没进 L2P 表 */
            if ((out_pages != NULL) && (count < capacity) && ((page_seq + (uint32_t)entry_count) > cp_sequence))
            {
                out_pages[count].page_seq    = page_seq;
                out_pages[count].block       = b;
                out_pages[count].page        = (uint8_t)page;
                out_pages[count].entry_count = (uint8_t)entry_count;
                count++;
            }
        }
    }

    if (out_count != NULL)
    {
        *out_count = count;
    }
}

/* --------------------------------------------------------------------
 * journal_scan_latest
 *   功能：只读扫环取最新页（journal_scan_ring 的「只找最新页」外壳）。
 *   参数：journal —— Journal；out —— 出参（环空时全置 0，可为 NULL）。
 *   返回：1 = 环非空；0 = 环空。
 * -------------------------------------------------------------------- */
int journal_scan_latest(const l2p_journal_t *journal, journal_page_ref_t *out)
{
    if (journal == NULL)
    {
        return 0;
    }

    journal_scan_ring(journal, 0U, NULL, 0U, NULL, out);

    return ((out != NULL) && (out->page_seq > 0U)) ? 1 : 0;
}

/* --------------------------------------------------------------------
 * journal_resume
 *   功能：恢复 Journal 的 RAM 写态（写指针 + 序号），**不回放条目**。
 *   参数：journal —— Journal；latest —— 最新页（NULL 或 page_seq==0 视为环空）；
 *         floor_sequence —— 序号下限（Checkpoint 的 journal_sequence）。
 *   返回：无。
 *   实现要点：① 落点是最新页**之后一页**——最新页已编程，续填会在页满时对同一页
 *                二次编程（NAND 只能 1→0 ⇒ 页头 entry_count 被 AND 成错值）；
 *             ② 跨块时先擦下一块（那里还留着上一圈的旧页）；**不记账
 *                pre_erased_block**——那块马上会被写满，下一圈回切时仍需按常规擦一次；
 *             ③ 序号取 max(最新页序号 + 1, floor)，0 抬到 1（0 保留作「无序号」，
 *                与 journal_init 同约定；否则 journal_truncate 的水位校验会被 0 挡掉）。
 * -------------------------------------------------------------------- */
void journal_resume(l2p_journal_t *journal, const journal_page_ref_t *latest, uint32_t floor_sequence)
{
    uint32_t next = floor_sequence;

    if (journal == NULL)
    {
        return;
    }

    if ((latest != NULL) && (latest->page_seq > 0U))
    {
        uint8_t next_block = latest->block;
        uint8_t next_page  = (uint8_t)(latest->page + 1U);

        /* ⚠ 必须加 **entry_count**，不是 +1：本页占了 page_seq .. page_seq+entry_count-1
         *   共 entry_count 个序号（journal_append 逐条 `sequence_counter++`，页头
         *   `hdr_page_seq = sequence_counter − current_entry` 反推）。若只 +1，序号会
         *   退回本页内部 ⇒ 下一页的 page_seq 落进本页的序号区间，回放过滤
         *   `page_seq + entry_count > cp_sequence` 会把已进 L2P 的页**重复回放**。 */
        if ((latest->page_seq + (uint32_t)latest->entry_count) > next)
        {
            next = latest->page_seq + (uint32_t)latest->entry_count;
        }

        if (next_page >= PAGES_PER_BLOCK)
        {
            next_block = (uint8_t)((next_block + 1U) % JOURNAL_BLOCK_COUNT);
            next_page  = 0U;
            (void)ftl_sync_erase_block(journal_block_addr(journal, next_block));
        }

        journal->current_block = next_block;
        journal->current_page  = next_page;
    }
    else
    {
        /* 环空：写指针留在 journal_init 的 (block0, page0) */
        journal->current_block = 0U;
        journal->current_page  = 0U;
    }

    if (next == 0U)
    {
        next = 1U;
    }

    journal->current_entry    = 0U;
    journal->sequence_counter = next;

    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));
}

/* --------------------------------------------------------------------
 * journal_replay
 *   功能：开机回放——扫 Journal 块池，把**未进 L2P 表**的页按
 *         (page_seq, entry_index) 递增序应用到 L2P cache，末尾恢复写指针。
 *   参数：journal —— Journal；cache —— 目标 L2P cache；cp_sequence —— checkpoint
 *         截断水位（≤ 该序号的条目已进 NAND L2P 表，无需回放）。
 *   返回：FTL_OK 成功；FTL_ERROR 入参为 NULL。
 *   实现要点：
 *     · 第一遍（journal_scan_ring）收集「需回放页」进栈上数组 replay_pages[]
 *       （JOURNAL_BLOCK_COUNT 块 × PAGES_PER_BLOCK 页，共 256 项 8B = 2KB；
 *       ⚠ 256 > 255 ⇒ 计数/下标必须 uint16_t，uint8_t 会死循环），
 *       同时取到最新页用于恢复写指针。
 *     · 第二遍插入排序（≤256 项）后按序应用 ⇒ per-LBA last-write-wins，同 LBA 的旧
 *       条目（含 GC/WL 搬移记录）被后写覆盖，不指向已擦除块。
 *     · 末尾 journal_resume 把写指针落到最新页之后并接续序号。
 *     · out_entries 在**应用循环里**逐页累加 entry_count（读失败的页不计）——
 *       不能用 journal 的 RAM 写指针反推：journal_resume 会清 current_entry。
 * -------------------------------------------------------------------- */
ftl_status_t journal_replay(l2p_journal_t *journal,
                            l2p_cache_t   *cache,
                            uint32_t       cp_sequence,
                            uint32_t      *out_entries)
{
    /* JOURNAL_BLOCK_COUNT 块 × 每块 PAGES_PER_BLOCK 页 = 256 页；每项 8B → 2KB 栈上数组。 */
    journal_page_ref_t replay_pages[JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK];
    journal_page_ref_t latest;
    uint16_t           replay_count = 0U;
    uint32_t           applied      = 0U;

    if ((journal == NULL) || (cache == NULL))
    {
        return FTL_ERROR;
    }

    /* 第一遍：扫环——收集需回放页 + 取最新页 */
    journal_scan_ring(journal,
                      cp_sequence,
                      replay_pages,
                      (uint16_t)(JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK),
                      &replay_count,
                      &latest);

    /* 第二遍：按 (page_seq, entry_index) 递增序应用条目。
     * 插入排序（replay_count ≤ 256）：per-LBA last-write-wins——
     * 同 LBA 的旧条目（含 GC/WL 搬移记录）被后写覆盖，不指向已擦除块。 */
    {
        uint16_t i;
        uint16_t n;

        for (i = 1U; i < replay_count; i++)
        {
            journal_page_ref_t key = replay_pages[i];

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
            uint8_t    page_data[JOURNAL_PAGE_SIZE];
            nand_pba_t raddr;
            int        ret;

            raddr = journal_page_addr(journal, replay_pages[i].block, replay_pages[i].page);

            memset(page_data, 0xFF, sizeof(page_data));
            ret = ftl_sync_read_page(raddr, page_data, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));
            if (ret != 0)
            {
                continue; /* 页已失效，跳过（应用顺序其余页不受影响） */
            }

            applied += (uint32_t)replay_pages[i].entry_count;

            {
                uint16_t k;
                for (k = 0U; k < replay_pages[i].entry_count; k++)
                {
                    uint32_t entry_lba;
                    uint32_t entry_new_pba;
                    uint16_t offset = JOURNAL_HEADER_SIZE + sizeof(l2p_journal_entry_t) * k;
                    memcpy(&entry_lba, page_data + offset + 0, 4);
                    memcpy(&entry_new_pba, page_data + offset + 8, 4);
                    l2p_cache_write(cache, (lba_t)entry_lba, (nand_pba_t)entry_new_pba);
                }
            }
        }
    }

    /* 恢复写指针与序号（落到最新页之后；floor 兜住「环空 / 序号回退」） */
    journal_resume(journal, &latest, cp_sequence);

    if (out_entries != NULL)
    {
        *out_entries = applied;
    }

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════
 * 5. 截断
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * journal_truncate
 *   功能：丢弃 ≤ up_to_sequence 的条目（Checkpoint 已把它们写进 NAND L2P 表）。
 *   参数：journal —— Journal；up_to_sequence —— 截断水位（调用者传 sequence_counter）。
 *   返回：无。
 *   实现要点：
 *     ① 水位校验：若水位不足以覆盖全部已写条目，**不截断**（最旧块可能仍持有回放
 *        需要的条目，擦掉即丢数据）；sequence_counter == 0 一并挡掉避免下溢。
 *     ② 预擦除环形最旧块（= 环形下一块）并记账 pre_erased_block，使 journal_flush_page
 *        回切到它时跳过擦除——保证同一块每圈只擦一次（旧实现两处各擦一次 = 双倍磨损）。
 *     ③ 复位水位标志（emergency / checkpoint_needed）。
 * -------------------------------------------------------------------- */
void journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence)
{
    uint8_t next;

    if (journal == NULL)
    {
        return;
    }

    /*
     * ① 水位校验（P1-6：up_to_sequence 不再是死形参）。
     * 调用者是 checkpoint_create，传的正是 sequence_counter（下一个待分配序号）⇒
     * 全部已写条目 seq < up_to_sequence。若水位**不足以**覆盖全部已写条目，说明有
     * 未 checkpoint 的条目，而环形里最旧的一块可能仍持有回放需要的条目 ⇒ **不截断**。
     * （sequence_counter == 0 理论上不会出现；一并挡掉，避免 -1 下溢误判。）
     */
    if ((journal->sequence_counter == 0U) || ((up_to_sequence + 1U) < journal->sequence_counter))
    {
        return;
    }

    /*
     * ② 预擦除环形最旧块（= 环形下一块）并记账。
     * 该块整块的条目都 ≤ up_to_sequence（checkpoint 已把它们写进 NAND L2P 表），
     * 擦掉即完成截断。记账 pre_erased_block 让 journal_flush_page 回切到它时跳过擦除
     * ——旧实现在此处与 flush_page 各擦一次 = **同一块每圈擦两次**，是 Journal 磨损
     * （§14.1 的寿命瓶颈）中凭空多出的 ~25%。重复调用（两次 checkpoint 之间无写入）
     * 时 next == pre_erased_block ⇒ 跳过重擦。
     */
    next = (uint8_t)((journal->current_block + 1U) % JOURNAL_BLOCK_COUNT);
    if (next != journal->pre_erased_block)
    {
        (void)ftl_sync_erase_block(journal_block_addr(journal, next));
        journal->pre_erased_block = next;
    }

    /* ③ 复位水位标志（设计 §7.5.4）：截断即水位已清。与 checkpoint_create 第 ⑧ 步
     * 双保险（后者覆盖 truncate 被弱桩替换的纯 RAM 测试构建）。 */
    journal->emergency         = 0U;
    journal->checkpoint_needed = 0U;
}
