/*
 * wear.c — 磨损计数持久化（Wear 表：页式追加日志）
 *
 * 职责：
 *   把每个块的读/擦计数（read_count / erase_count）持久化到固定块 WEAR_BLOCK 上的
 *   **页式追加日志**——常态增量写脏块、写满则 compact 成全量快照；开机逐页回放恢复计数。
 *
 * 与 wear.h / wear_leveling.c 的分工：
 *   wear.h            —— wear_page_t / wear_entry_t 格式 + 宏；
 *   本文件            —— 持久化（日志追加/compact/回放）+ 脏位图；
 *   wear_leveling.c   —— 用这些计数做静态磨损均衡（搬移决策），不碰持久化。
 *
 * 介质模型（P0-1）：
 *   块 WEAR_BLOCK（plane 0）上按页 0..PAGES_PER_BLOCK-1 **只追加**；页序 == sequence 序。
 *   一条记录 = 一页 = 头（magic+sequence+entry_count）+ 最多 WEAR_ENTRY_MAX 条
 *   (block, read_count, erase_count)。回放**绝对值 + 最新胜**（后写覆盖先写）。
 *
 * 文件结构：
 *   1. 脏位图            wear_dirty_set / wear_dirty_get / wear_dirty_count / wear_mark_dirty
 *   2. 页地址 / 写页     wear_page_addr / wear_write_page
 *   3. 日志写             wear_write_range / wear_compact
 *   4. 开机回放 / 落盘   wear_init / wear_flush
 *
 * 游标/序号/脏位图是 **per-instance**（序 4：收进 ftl_allocator_t.wear，见 wear_state_t）——
 * 多实例（多 LUN）下每实例一份，互不干扰（原为 file-scope static）。
 */

#include "ftl/wear.h"

#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "platform/platform.h"

#include <string.h>

/*
 * Wear 内部状态：**per-instance**，落在 ftl_allocator_t.wear（wear_state_t，序 4）——
 * sequence / next_page / 节拍基准 / 脏位图。原先是 file-scope static ⇒ 多实例
 * （多 LUN）下所有实例共享一份，落盘互相踩。
 */

/* ── 1. 脏位图（512 B，按 gblock 置位；越界忽略）── */

static void wear_dirty_set(ftl_allocator_t *alloc, uint16_t gblock)
{
    alloc->wear.dirty[gblock >> 3] |= (uint8_t)(1U << (gblock & 7U));
}

static int wear_dirty_get(const ftl_allocator_t *alloc, uint16_t gblock)
{
    return (int)((alloc->wear.dirty[gblock >> 3] >> (gblock & 7U)) & 1U);
}

/* wear_dirty_count — 置位块数（= 本次 flush 需写的条目数，popcount 逐字节累加） */
static uint32_t wear_dirty_count(const ftl_allocator_t *alloc)
{
    uint32_t n = 0U;
    uint16_t i;

    for (i = 0U; i < (uint16_t)sizeof(alloc->wear.dirty); i++)
    {
        n += (uint32_t)__builtin_popcount((unsigned)alloc->wear.dirty[i]);
    }

    return n;
}

/* wear_mark_dirty — 标记某块的计数已变（越界忽略）；计数变更点调用，flush 时只写脏块 */
void wear_mark_dirty(ftl_allocator_t *alloc, uint16_t gblock)
{
    if ((alloc != NULL) && (gblock < FTL_BLOCK_TOTAL))
    {
        wear_dirty_set(alloc, gblock);
    }
}

/* ── 2. 页地址 / 写页 ── */

/* ── 固定位置页地址（WEAR_BLOCK，plane 0，页 0..PAGES_PER_BLOCK-1）── */
static nand_pba_t wear_page_addr(uint8_t ch, uint8_t ce, uint8_t lun, uint16_t page)
{
    return pba_make(ch, ce, lun, WEAR_BLOCK, 0U, page, 0U);
}

/* ── 写一页日志并推进 next_page ──
 * 失败**不**推进：下次 flush 从同一页重试（已写成功的页仍有效，
 * 绝对值 + 最新胜 ⇒ 重试重复写同一块是幂等的）。 */
static ftl_status_t wear_write_page(ftl_allocator_t *alloc, wear_page_t *page, uint32_t count)
{
    nand_pba_t addr;

    page->entry_count = count;

    addr = wear_page_addr(alloc->ch, alloc->ce, alloc->lun, alloc->wear.next_page);
    if (ftl_sync_write_page(addr, page, sizeof(*page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_WEAR_LEVELING)) != 0)
    {
        return FTL_ERROR;
    }

    alloc->wear.next_page++;
    return FTL_OK;
}

/* ── 3. 日志写 ── */

/*
 * wear_write_range - 把计数流式写成日志页
 *
 *   dirty_only != 0：只写脏块（增量落盘，常态 1–4 页）
 *   dirty_only == 0：写全部块（compact 后的全量快照，25 页）
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
        if ((dirty_only != 0) && (wear_dirty_get(alloc, gblock) == 0))
        {
            continue;
        }

        if (n == 0U)
        {
            memset(&page, 0, sizeof(page)); /* padding 归零 ⇒ 介质内容确定 */
            page.magic    = WEAR_MAGIC;
            page.sequence = ++alloc->wear.sequence;
        }

        page.entries[n].block        = gblock;
        page.entries[n].read_count   = alloc->blocks[gblock].read_count;
        page.entries[n].erase_count  = alloc->blocks[gblock].erase_count;
        page.entries[n].last_ecc     = alloc->blocks[gblock].rr.last_ecc;
        page.entries[n].rising_count = alloc->blocks[gblock].rr.rising_count;
        page.entries[n].health       = alloc->blocks[gblock].rr.health;
        page.entries[n]._reserved    = 0U;
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
    nand_pba_t addr = wear_page_addr(alloc->ch, alloc->ce, alloc->lun, 0U);

    if (ftl_sync_erase_block(addr) != 0)
    {
        return FTL_ERROR;
    }

    alloc->wear.next_page = 0U;

    if (wear_write_range(alloc, 0) != FTL_OK)
    {
        /* 快照写残：擦除已抹掉旧内容 ⇒ **全部**块都成了"未持久化"（不只是原来脏的那些）。
         * 全标脏让下次 flush 重写全量；否则快照尾部未写到的块会被静默丢掉
         * （它们在 RAM 里是对的，只是没落盘，而旧内容已被擦除）。 */
        memset(alloc->wear.dirty, 0xFF, sizeof(alloc->wear.dirty));
        return FTL_ERROR;
    }

    return FTL_OK;
}

/* ── 4. 开机回放 / 落盘 ── */

/* --------------------------------------------------------------------
 * wear_init
 *   功能：开机逐页回放 WEAR_BLOCK 日志，恢复各块读/擦计数；并打印观测口。
 *   参数：alloc —— 分配器（承载计数）。
 *   返回：FTL_OK 成功（含冷启动无有效页）；FTL_ERROR 参数非法。
 *   实现要点：页序 == sequence 序（只追加），故**第一个无效页即日志末尾**，回放到此为止；
 *             无有效页 = 冷启动（计数保持 ftl_init 的零值），**不 brick 开机**（非致命）。
 * -------------------------------------------------------------------- */
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

    memset(alloc->wear.dirty, 0, sizeof(alloc->wear.dirty));
    alloc->wear.sequence          = 0U;
    alloc->wear.next_page         = 0U;
    alloc->wear.last_flush_writes = 0U;
    alloc->wear.last_flush_ms     = 0U;

    /* 逐页回放：页序 == sequence 序（块内只追加；compact 擦块后从头写全量快照），
     * 故**第一个无效页即日志末尾**，回放到此为止。
     * 非致命：无有效页 = 冷启动（计数保持 ftl_init 的 memset 零值），不 brick 开机。 */
    for (p = 0U; p < PAGES_PER_BLOCK; p++)
    {
        nand_pba_t addr = wear_page_addr(alloc->ch, alloc->ce, alloc->lun, p);
        uint32_t   k;

        memset(&page, 0, sizeof(page));
        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_WEAR_LEVELING)) != 0)
        {
            break;
        }
        if ((page.magic != WEAR_MAGIC) || (page.entry_count > WEAR_ENTRY_MAX))
        {
            break;
        }

        for (k = 0U; k < page.entry_count; k++)
        {
            const wear_entry_t *e = &page.entries[k];

            if (e->block >= FTL_BLOCK_TOTAL)
            {
                continue;
            }
            alloc->blocks[e->block].erase_count = e->erase_count;
            alloc->blocks[e->block].read_count  = e->read_count;

            /* rr 健康（v0.65，序 8）：慢死预测的输入。health 取值越界即当 HEALTHY
             * （防御坏页——枚举只有 0..3，越界值会让 rr_predictive_scan 的比较行为
             *  未定义；钳到 HEALTHY 等于"这块重头数"，安全侧）。 */
            alloc->blocks[e->block].rr.last_ecc     = e->last_ecc;
            alloc->blocks[e->block].rr.rising_count = e->rising_count;
            alloc->blocks[e->block].rr.health =
                (e->health <= (uint8_t)RR_BLOCK_DYING) ? e->health : (uint8_t)RR_BLOCK_HEALTHY;
        }

        alloc->wear.sequence = page.sequence;
        valid_pages++;
    }

    alloc->wear.next_page = valid_pages;

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
         (unsigned)alloc->wear.sequence,
         (unsigned)valid_pages,
         (unsigned)max_ec,
         (unsigned)min_ec);

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * wear_flush
 *   功能：把脏块计数落盘——按节拍门决定是否落；增量追加，页不够则 compact 全量快照。
 *   参数：alloc —— 分配器；total_writes —— 当前累计写次数（节拍判据之一）。
 *   返回：FTL_OK 成功（含无脏 / 节拍未到早退）；FTL_ERROR 参数非法 / 写或 compact 失败（脏位保留）。
 *   实现要点：节拍 = 定时（WEAR_FLUSH_INTERVAL_MS）**或** 写入增量（WEAR_FLUSH_ERASE_DELTA，
 *             擦除活跃度的代理），任一满足即落盘；剩余页装不下脏条目 ⇒ compact（擦块+全量）；
 *             成功后清脏位图并更新节拍基准。
 * -------------------------------------------------------------------- */
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

    dirty_count = wear_dirty_count(alloc);
    if (dirty_count == 0U)
    {
        return FTL_OK; /* 无变更：空转 */
    }

    /* 节拍：定时 **或** 写入增量（擦除活跃度的代理），任一满足即落盘 */
    platform = ssd_platform_get();
    now_ms   = ((platform != NULL) && (platform->time_us != NULL)) ? (uint32_t)(platform->time_us() / 1000U) : 0U;

    if (((now_ms - alloc->wear.last_flush_ms) < WEAR_FLUSH_INTERVAL_MS) &&
        ((total_writes - alloc->wear.last_flush_writes) < WEAR_FLUSH_ERASE_DELTA))
    {
        return FTL_OK;
    }

    /* 剩余页装不下本次脏条目 ⇒ compact（擦块 + 全量快照）；
     * 否则增量追加。两者都把全部脏块写进日志。 */
    pages_needed = (dirty_count + WEAR_ENTRY_MAX - 1U) / WEAR_ENTRY_MAX;

    if (((uint32_t)alloc->wear.next_page + pages_needed) > PAGES_PER_BLOCK)
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

    memset(alloc->wear.dirty, 0, sizeof(alloc->wear.dirty));
    alloc->wear.last_flush_writes = total_writes;
    alloc->wear.last_flush_ms     = now_ms;

    return FTL_OK;
}
