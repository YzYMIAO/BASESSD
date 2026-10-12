#include "ftl/wear.h"

#include "common/boot_policy.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "platform/platform.h"

#include <string.h>

/*
 * Wear 内部状态（全局单例）。
 *
 * ⚠ 与 bbt.c 同一局限：file-scope static ⇒ **单实例**。per-instance 惯例是放
 *   ftl_context_t（见 checkpoint.c），但当前 FTL 只跑 g_ftl[0][0][0]，提维度时
 *   与 bbt / completion_table 一并搬（已登记的阻断点）。
 */
static uint32_t g_wear_sequence;                    /* 最新已写页的 sequence */
static uint16_t g_wear_next_page;                   /* 下一个可写页（= 已回放的有效页数）*/
static uint32_t g_wear_last_flush_writes;           /* 上次落盘时的 total_writes */
static uint32_t g_wear_last_flush_ms;               /* 上次落盘时刻（ms）*/
static uint8_t  g_wear_dirty[FTL_BLOCK_TOTAL / 8U]; /* 脏位图：哪些块的计数变了 */

/* ── 脏位图（512 B，按 gblock 置位；越界忽略）── */

static void wear_dirty_set(uint16_t gblock)
{
    g_wear_dirty[gblock >> 3] |= (uint8_t)(1U << (gblock & 7U));
}

static int wear_dirty_get(uint16_t gblock)
{
    return (int)((g_wear_dirty[gblock >> 3] >> (gblock & 7U)) & 1U);
}

static uint32_t wear_dirty_count(void)
{
    uint32_t n = 0U;
    uint16_t i;

    for (i = 0U; i < (uint16_t)sizeof(g_wear_dirty); i++)
    {
        n += (uint32_t)__builtin_popcount((unsigned)g_wear_dirty[i]);
    }

    return n;
}

void wear_mark_dirty(uint16_t gblock)
{
    if (gblock < FTL_BLOCK_TOTAL)
    {
        wear_dirty_set(gblock);
    }
}

/* ── 固定位置页地址（WEAR_BLOCK，plane 0，页 0..PAGES_PER_BLOCK-1）── */
static nand_addr_t wear_page_addr(uint8_t ch, uint8_t ce, uint8_t lun, uint16_t page)
{
    nand_addr_t addr;

    addr.ch    = ch;
    addr.ce    = ce;
    addr.lun   = lun;
    addr.plane = 0U;
    addr.page  = (uint8_t)page;
    addr.block = (uint16_t)WEAR_BLOCK;
    addr.cw    = 0U;

    return addr;
}

/* ── 写一页日志并推进 next_page ──
 * 失败**不**推进：下次 flush 从同一页重试（已写成功的页仍有效，
 * 绝对值 + 最新胜 ⇒ 重试重复写同一块是幂等的）。 */
static ftl_status_t wear_write_page(ftl_allocator_t *alloc, wear_page_t *page, uint32_t count)
{
    nand_addr_t addr;

    page->entry_count = count;
    page->crc         = 0U;
    page->crc         = boot_crc32(page, sizeof(*page) - sizeof(uint32_t));

    addr = wear_page_addr(alloc->ch, alloc->ce, alloc->lun, g_wear_next_page);
    if (ftl_sync_write_page(addr, page, sizeof(*page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_WEAR_LEVELING)) != 0)
    {
        return FTL_ERROR;
    }

    g_wear_next_page++;
    return FTL_OK;
}

/*
 * wear_write_range - 把计数流式写成日志页
 *
 *   dirty_only != 0：只写脏块（增量落盘，常态 1–4 页）
 *   dirty_only == 0：写全部块（compact 后的全量快照，17 页）
 *
 * 按 gblock 升序流式填页（2 KB 栈缓冲），页满即写；中途失败立即返回，
 * 脏位保留待重试。
 */
static ftl_status_t wear_write_range(ftl_allocator_t *alloc, int dirty_only)
{
    wear_page_t page;
    uint32_t    n = 0U; /* 当前页已填条目数；0 = 该页尚未开始 */
    uint16_t    gblock;

    for (gblock = 0U; gblock < FTL_BLOCK_TOTAL; gblock++)
    {
        if ((dirty_only != 0) && (wear_dirty_get(gblock) == 0))
        {
            continue;
        }

        if (n == 0U)
        {
            memset(&page, 0, sizeof(page)); /* padding 归零 ⇒ 介质内容确定 */
            page.magic    = WEAR_MAGIC;
            page.sequence = ++g_wear_sequence;
        }

        page.entries[n].block       = gblock;
        page.entries[n].read_count  = alloc->blocks[gblock].read_count;
        page.entries[n].erase_count = alloc->blocks[gblock].erase_count;
        n++;

        if (n == WEAR_ENTRY_MAX)
        {
            if (wear_write_page(alloc, &page, n) != FTL_OK)
            {
                return FTL_ERROR;
            }
            n = 0U;
        }
    }

    if (n > 0U)
    {
        if (wear_write_page(alloc, &page, n) != FTL_OK)
        {
            return FTL_ERROR;
        }
    }

    return FTL_OK;
}

/* 块满：擦掉整块后从头写全量快照（erase 失败则 next_page 不动，下次再试）*/
static ftl_status_t wear_compact(ftl_allocator_t *alloc)
{
    nand_addr_t addr = wear_page_addr(alloc->ch, alloc->ce, alloc->lun, 0U);

    if (ftl_sync_erase_block(addr) != 0)
    {
        return FTL_ERROR;
    }

    g_wear_next_page = 0U;

    if (wear_write_range(alloc, 0) != FTL_OK)
    {
        /* 快照写残：擦除已抹掉旧内容 ⇒ **全部**块都成了"未持久化"（不只是原来脏的那些）。
         * 全标脏让下次 flush 重写全量；否则快照尾部未写到的块会被静默丢掉
         * （它们在 RAM 里是对的，只是没落盘，而旧内容已被擦除）。 */
        memset(g_wear_dirty, 0xFF, sizeof(g_wear_dirty));
        return FTL_ERROR;
    }

    return FTL_OK;
}

ftl_status_t wear_init(ftl_allocator_t *alloc)
{
    wear_page_t page;
    uint16_t    p;
    uint16_t    valid_pages = 0U;
    uint32_t    max_ec      = 0U;
    uint32_t    min_ec      = 0U;
    int         have_min    = 0;
    uint16_t    gblock;

    if ((alloc == NULL) || (alloc->blocks == NULL))
    {
        return FTL_ERROR;
    }

    memset(g_wear_dirty, 0, sizeof(g_wear_dirty));
    g_wear_sequence          = 0U;
    g_wear_next_page         = 0U;
    g_wear_last_flush_writes = 0U;
    g_wear_last_flush_ms     = 0U;

    /* 逐页回放：页序 == sequence 序（块内只追加；compact 擦块后从头写全量快照），
     * 故**第一个无效页即日志末尾**，回放到此为止。
     * 非致命：无有效页 = 冷启动（计数保持 ftl_init 的 memset 零值），不 brick 开机。 */
    for (p = 0U; p < PAGES_PER_BLOCK; p++)
    {
        nand_addr_t addr = wear_page_addr(alloc->ch, alloc->ce, alloc->lun, p);
        uint32_t    saved_crc;
        uint32_t    k;

        memset(&page, 0, sizeof(page));
        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_WEAR_LEVELING)) != 0)
        {
            break;
        }
        if ((page.magic != WEAR_MAGIC) || (page.entry_count > WEAR_ENTRY_MAX))
        {
            break;
        }

        saved_crc = page.crc;
        page.crc  = 0U;
        if (boot_crc32(&page, sizeof(page) - sizeof(uint32_t)) != saved_crc)
        {
            break;
        }
        page.crc = saved_crc;

        for (k = 0U; k < page.entry_count; k++)
        {
            const wear_entry_t *e = &page.entries[k];

            if (e->block >= FTL_BLOCK_TOTAL)
            {
                continue;
            }
            alloc->blocks[e->block].erase_count = e->erase_count;
            alloc->blocks[e->block].read_count  = e->read_count;
        }

        g_wear_sequence = page.sequence;
        valid_pages++;
    }

    g_wear_next_page = valid_pages;

    /* 开机可观测（判据「重启后计数一致」的观测口）：跳过坏块统计 max/min */
    for (gblock = 0U; gblock < FTL_BLOCK_TOTAL; gblock++)
    {
        uint32_t ec = alloc->blocks[gblock].erase_count;

        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            continue;
        }
        if (ec > max_ec)
        {
            max_ec = ec;
        }
        if ((have_min == 0) || (ec < min_ec))
        {
            min_ec   = ec;
            have_min = 1;
        }
    }

    LOGI("[WEAR] seq=%u pages=%u max_ec=%u min_ec=%u\r\n",
         (unsigned)g_wear_sequence,
         (unsigned)valid_pages,
         (unsigned)max_ec,
         (unsigned)min_ec);

    return FTL_OK;
}

ftl_status_t wear_flush(ftl_allocator_t *alloc, uint32_t total_writes)
{
    const ssd_platform_ops_t *platform;
    uint32_t                  now_ms;
    uint32_t                  dirty_count;
    uint32_t                  pages_needed;

    if ((alloc == NULL) || (alloc->blocks == NULL))
    {
        return FTL_ERROR;
    }

    dirty_count = wear_dirty_count();
    if (dirty_count == 0U)
    {
        return FTL_OK; /* 无变更：空转 */
    }

    /* 节拍：定时 **或** 写入增量（擦除活跃度的代理），任一满足即落盘 */
    platform = ssd_platform_get();
    now_ms   = ((platform != NULL) && (platform->time_us != NULL)) ? (uint32_t)(platform->time_us() / 1000U) : 0U;

    if (((now_ms - g_wear_last_flush_ms) < WEAR_FLUSH_INTERVAL_MS) &&
        ((total_writes - g_wear_last_flush_writes) < WEAR_FLUSH_ERASE_DELTA))
    {
        return FTL_OK;
    }

    /* 剩余页装不下本次脏条目 ⇒ compact（擦块 + 全量快照）；
     * 否则增量追加。两者都把全部脏块写进日志。 */
    pages_needed = (dirty_count + WEAR_ENTRY_MAX - 1U) / WEAR_ENTRY_MAX;

    if (((uint32_t)g_wear_next_page + pages_needed) > PAGES_PER_BLOCK)
    {
        if (wear_compact(alloc) != FTL_OK)
        {
            return FTL_ERROR; /* 脏位保留，下次重试 */
        }
    }
    else if (wear_write_range(alloc, 1) != FTL_OK)
    {
        return FTL_ERROR;
    }

    memset(g_wear_dirty, 0, sizeof(g_wear_dirty));
    g_wear_last_flush_writes = total_writes;
    g_wear_last_flush_ms     = now_ms;

    return FTL_OK;
}
