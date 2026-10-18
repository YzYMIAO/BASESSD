/*
 * allocator.c — 块分配器实现
 *
 * 只做账本，不碰介质：块的擦除由调用者在 allocator_free_block 之前完成。
 * 不变量见 allocator.h。
 *
 * 文件结构：
 *   1. 初始化
 *   2. Free Pool 进出闸门
 *   3. 内部工具（OP 补池 / 选块 / Open SB / Closed 提交）
 *   4. 分配与 SB 生命周期
 *   5. 只读统计
 */

#include "allocator.h"

#include "assertion.h"
#include "log.h" /* LOGE：release 运行期拒绝要留日志 */

#include <string.h>

/* ── 1. 初始化 ── */

/* 实现要点：只绑指针与清计数；不置 FREE（FREE ⟺ 在池，池由恢复路径回填）。
 * tables 的 state/temp/valid 依赖 BSS 清零，调用者不得传入脏存储。 */
void allocator_init(ftl_allocator_t *alloc, ftl_topology_t topo, ftl_tables_t *tables)
{
    ASSERT(alloc != NULL);
    ASSERT(tables != NULL);

    alloc->ch           = topo.ch;
    alloc->ce           = topo.ce;
    alloc->lun          = topo.lun;
    alloc->total_blocks = 0U; /* 恢复路径回填 */

    alloc->free_blocks = tables->free_blocks;
    alloc->free_count  = 0U;

    alloc->op_blocks    = tables->op_blocks;
    alloc->spare_blocks = tables->spare_blocks;
    memset(alloc->op_count, 0, sizeof(alloc->op_count));
    memset(alloc->spare_count, 0, sizeof(alloc->spare_count));

    memset(&alloc->hot_sb, 0, sizeof(alloc->hot_sb));
    memset(&alloc->cold_sb, 0, sizeof(alloc->cold_sb));
    alloc->hot_sb.temperature  = FTL_TEMP_HOT;
    alloc->cold_sb.temperature = FTL_TEMP_COLD;

    alloc->closed_sb_list  = tables->closed_sb_list;
    alloc->closed_sb_count = 0U;

    alloc->blocks    = tables->blocks;
    alloc->bad_count = 0U;

    memset(&alloc->bbt, 0, sizeof(alloc->bbt));

    /* 反向索引：未坏块置 NO_SB；坏块由 bbt_init 置 BAD，不在此动。 */
    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        if (!ftl_block_is_bad(&alloc->blocks[i]))
        {
            ftl_block_set_sb(&alloc->blocks[i], FTL_BLOCK_NO_SB);
        }
    }
}

/* ── 2. Free Pool 进出闸门 ── */

/* 实现要点：调用者保证块已擦除。入池前拒绝越界、BAD、重复归还、池满；
 * debug ASSERT + release 拒绝（后果是静默漏账或坏块复活）。 */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t gblock)
{
    ASSERT(alloc != NULL);

    ASSERT(gblock < FTL_BLOCK_TOTAL);
    if (gblock >= FTL_BLOCK_TOTAL)
    {
        LOGE("[FTL] free_block: gblock %u out of range\r\n", (unsigned)gblock);
        return;
    }

    /* BAD 是终态，入池会把它改回 FREE。 */
    ASSERT(!ftl_block_is_bad(&alloc->blocks[gblock]));
    if (ftl_block_is_bad(&alloc->blocks[gblock]))
    {
        LOGE("[FTL] free_block: gblock %u is bad, dropped\r\n", (unsigned)gblock);
        return;
    }

    /* state == FREE ⟺ 在池 ⇒ 已 FREE 即重复归还。 */
    ASSERT(ftl_block_state(&alloc->blocks[gblock]) != FTL_BLOCK_STATE_FREE);
    if (ftl_block_state(&alloc->blocks[gblock]) == FTL_BLOCK_STATE_FREE)
    {
        LOGE("[FTL] free_block: gblock %u double free, dropped\r\n", (unsigned)gblock);
        return;
    }

    /* 池满只是重复条目积累后的晚期症状；这里兜住前两道检查漏掉的坏账。 */
    ASSERT(alloc->free_count < FTL_BLOCK_TOTAL);
    if (alloc->free_count >= FTL_BLOCK_TOTAL)
    {
        LOGE("[FTL] free_block: free pool full, gblock %u dropped\r\n", (unsigned)gblock);
        return;
    }

    alloc->free_blocks[alloc->free_count++] = gblock;

    ftl_block_t *b = &alloc->blocks[gblock];

    ftl_block_set_state(b, FTL_BLOCK_STATE_FREE);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

/* 实现要点：线性查找 + swap-remove（查找 O(free_count)，交换 O(1)）。
 * 不在池内是文档化 no-op；出池只校验 state == FREE，不赋新态。 */
void allocator_remove_from_free(ftl_allocator_t *alloc, uint16_t gblock)
{
    ASSERT(alloc != NULL);
    ASSERT(gblock < FTL_BLOCK_TOTAL);

    for (uint16_t i = 0U; i < alloc->free_count; i++)
    {
        if (alloc->free_blocks[i] == gblock)
        {
            ASSERT(ftl_block_state(&alloc->blocks[gblock]) == FTL_BLOCK_STATE_FREE);
            alloc->free_blocks[i] = alloc->free_blocks[--alloc->free_count];
            return;
        }
    }
}

/* 实现要点：与 migrate 擦除完成同口径；供元数据日志块等不经 migrate 的擦除路径调用。 */
void allocator_note_erase(ftl_allocator_t *alloc, uint16_t gblock, uint32_t now)
{
    ASSERT(alloc != NULL);
    ASSERT(gblock < FTL_BLOCK_TOTAL);
    ASSERT(!ftl_block_is_bad(&alloc->blocks[gblock]));

    ftl_block_t *b = &alloc->blocks[gblock];

    b->erase_count++;
    b->last_pe_ts = now;
    ftl_block_clear_valid(b);
}

/* 实现要点：按 plane 追加到 op_blocks；容量由 FTL_OP_BLOCKS_PER_PLANE 限定。 */
bool allocator_op_try_push(ftl_allocator_t *alloc, uint8_t plane, uint16_t gblock)
{
    ASSERT(plane < PLANE_NUM);

    if (alloc->op_count[plane] >= FTL_OP_BLOCKS_PER_PLANE)
    {
        return false;
    }

    alloc->op_blocks[plane][alloc->op_count[plane]] = gblock;
    alloc->op_count[plane]++;
    return true;
}

/* 实现要点：按 plane 追加到 spare_blocks；容量由 FTL_SPARE_POOL_PER_PLANE 限定。 */
bool allocator_spare_try_push(ftl_allocator_t *alloc, uint8_t plane, uint16_t gblock)
{
    ASSERT(plane < PLANE_NUM);

    if (alloc->spare_count[plane] >= FTL_SPARE_POOL_PER_PLANE)
    {
        return false;
    }

    alloc->spare_blocks[plane][alloc->spare_count[plane]] = gblock;
    alloc->spare_count[plane]++;
    return true;
}

/* 实现要点：LIFO 弹出；调用者保证 out_gblock 非空。 */
bool allocator_spare_try_pop(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    ASSERT(plane < PLANE_NUM);
    ASSERT(out_gblock != NULL);

    if (alloc->spare_count[plane] == 0U)
    {
        return false;
    }

    alloc->spare_count[plane]--;
    *out_gblock = alloc->spare_blocks[plane][alloc->spare_count[plane]];
    return true;
}

/* 实现要点：遍历 PLANE_NUM 个 plane 求和。 */
uint16_t allocator_op_total(const ftl_allocator_t *alloc)
{
    uint16_t n = 0U;

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        n = (uint16_t)(n + alloc->op_count[p]);
    }
    return n;
}

/* 实现要点：遍历 PLANE_NUM 个 plane 求和。 */
uint16_t allocator_spare_total(const ftl_allocator_t *alloc)
{
    uint16_t n = 0U;

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        n = (uint16_t)(n + alloc->spare_count[p]);
    }
    return n;
}

/* ── 3. 内部工具 ── */

/*
 * allocator_op_try_pop - 从指定 plane 的 OP 池弹一个块
 * 功能：OP 出池；仅供 allocator.c 内部使用。
 * 参数：alloc — 分配器；plane — 目标 plane；out_gblock — 出参。
 * 返回：true 取到；false 池空。
 * 实现要点：LIFO 弹出；容量由 op_count 维护。
 */
static bool allocator_op_try_pop(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    ASSERT(plane < PLANE_NUM);
    ASSERT(out_gblock != NULL);

    if (alloc->op_count[plane] == 0U)
    {
        return false;
    }

    alloc->op_count[plane]--;
    *out_gblock = alloc->op_blocks[plane][alloc->op_count[plane]];
    return true;
}

/*
 * allocator_pull_from_op - 从指定 plane 的 OP 池取一块并归还 Free Pool
 * 功能：OP 是 GC 缓冲池，取出的块直接进入 Free Pool。
 * 参数：alloc — 分配器；plane — 目标 plane；out_gblock — 出参。
 * 返回：true 取到；false 该 plane OP 池已空。
 * 实现要点：跳过运行时被标坏的块；坏块弹出即丢弃，不回 Free Pool。
 */
static bool allocator_pull_from_op(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    uint16_t gblock;

    ASSERT(out_gblock != NULL);

    while (allocator_op_try_pop(alloc, plane, &gblock))
    {
        ASSERT(gblock < FTL_BLOCK_TOTAL);

        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            continue;
        }

        allocator_free_block(alloc, gblock);
        *out_gblock = gblock;
        return true;
    }
    return false;
}

/*
 * allocator_select_plane_block_from_pool - 从 Free Pool 选该 plane 擦除最少的块
 * 功能：按 plane 选块；返回 gblock，池空返回 FTL_GBLOCK_INVALID。
 * 参数：alloc — 分配器；plane — 目标 plane。
 * 返回：gblock 或 FTL_GBLOCK_INVALID。
 * 实现要点：判据是 Free Pool 数组成员关系，不是 BST 的 FREE 位；
 *           选中 ⇒ allocator_remove_from_free 必然成功。
 */
static uint16_t allocator_select_plane_block_from_pool(const ftl_allocator_t *alloc, uint8_t plane)
{
    uint16_t best      = FTL_GBLOCK_INVALID;
    uint32_t min_erase = UINT32_MAX;

    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);
    ASSERT(plane < PLANE_NUM);

    for (uint16_t i = 0U; i < alloc->free_count; i++)
    {
        uint16_t gblock = alloc->free_blocks[i];

        ASSERT(gblock < FTL_BLOCK_TOTAL);

        if (ftl_gblock_plane(gblock) != plane)
        {
            continue;
        }

        const ftl_block_t *blk = &alloc->blocks[gblock];

        if (ftl_block_is_bad(blk))
        {
            continue;
        }
        if (blk->erase_count < min_erase)
        {
            min_erase = blk->erase_count;
            best      = gblock;
        }
    }

    return best;
}

/*
 * allocator_open_sb - 为 Open SB 分配各 plane 的物理块
 * 功能：各 plane 独立选块；Free Pool 空时从该 plane 的 OP 池补。
 * 参数：alloc — 分配器；sb — 目标槽；temperature — 温度流。
 * 返回：true 成功；false 某 plane 无块。
 * 实现要点：选块阶段无副作用；全部就位后才出池并置 OPEN，失败无需回滚。
 */
static bool allocator_open_sb(ftl_allocator_t *alloc, super_block_t *sb, uint8_t temperature)
{
    uint16_t gblock[PLANE_NUM];

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        gblock[plane] = allocator_select_plane_block_from_pool(alloc, plane);
        if (gblock[plane] == FTL_GBLOCK_INVALID)
        {
            if (!allocator_pull_from_op(alloc, plane, &gblock[plane]))
            {
                return false;
            }
        }
    }

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        /* 先出池（校验 state == FREE），再由本转移赋 OPEN。 */
        allocator_remove_from_free(alloc, gblock[plane]);
        sb->plane_blocks[plane] = ftl_gblock_block(gblock[plane]);

        ftl_block_t *b = &alloc->blocks[gblock[plane]];

        ftl_block_set_state(b, FTL_BLOCK_STATE_OPEN);
        ftl_block_set_temp(b, temperature);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
    }

    sb->next_page   = 0U;
    sb->temperature = temperature;
    sb->is_open     = true;

    return true;
}

/*
 * allocator_closed_sb_commit - 把一个 SB 追加进 closed_sb_list
 * 功能：逐 plane 置 CLOSED / 温度 / sb_id；返回列表索引。
 * 参数：alloc — 分配器；sb — 源条目。
 * 返回：新条目在 closed_sb_list 的索引。
 * 实现要点：BAD 是终态——该 plane 在列表副本里改记哨兵，状态不动；
 *           容量 = FTL_BLOCK_TOTAL，满在结构上不可能。
 */
static uint16_t allocator_closed_sb_commit(ftl_allocator_t *alloc, const super_block_t *sb)
{
    ASSERT(alloc != NULL);
    ASSERT(sb != NULL);
    ASSERT(alloc->closed_sb_count < SUPER_BLOCK_LIST_MAX);

    /* release 兜底：结构上不应满；真满时拒绝写入，避免越界。 */
    if (alloc->closed_sb_count >= SUPER_BLOCK_LIST_MAX)
    {
        LOGE("[FTL] closed_sb_list full, entry dropped\r\n");
        return FTL_BLOCK_NO_SB;
    }

    const uint16_t closed_idx = alloc->closed_sb_count;
    super_block_t  entry      = *sb;

    entry.is_open = false;

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (!super_block_has_plane(&entry, plane))
        {
            continue;
        }

        /* release 兜底：非法块号按哨兵处理，避免索引越界。 */
        ASSERT(entry.plane_blocks[plane] < BLOCKS_PER_PLANE);
        if (entry.plane_blocks[plane] >= BLOCKS_PER_PLANE)
        {
            LOGE("[FTL] closed_sb: plane%u block %u out of range\r\n",
                 (unsigned)plane,
                 (unsigned)entry.plane_blocks[plane]);
            entry.plane_blocks[plane] = FTL_BLOCK_NO_PLANE;
            continue;
        }

        ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, entry.plane_blocks[plane])];

        if (ftl_block_is_bad(b))
        {
            entry.plane_blocks[plane] = FTL_BLOCK_NO_PLANE; /* 坏块不进入 Closed SB */
            continue;
        }

        ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
        ftl_block_set_temp(b, entry.temperature);
        ftl_block_set_sb(b, closed_idx);
    }

    alloc->closed_sb_list[alloc->closed_sb_count++] = entry;
    return closed_idx;
}

/*
 * allocator_close_sb - Open → Closed
 * 功能：置 is_open=false 并追加进 closed_sb_list。
 * 参数：alloc — 分配器；sb — Open SB。
 * 返回：无。
 * 实现要点：is_open 必须在入列表前清掉，否则 GC/WL 会永久跳过该条目。
 */
static void allocator_close_sb(ftl_allocator_t *alloc, super_block_t *sb)
{
    ASSERT(alloc != NULL);
    ASSERT(sb != NULL);

    sb->is_open = false;
    (void)allocator_closed_sb_commit(alloc, sb);
}

/* ── 4. 分配与 SB 生命周期 ── */

/* 实现要点：SB 未开、写满或含已标坏块时先换新 SB；各 plane 取同一页。 */
ftl_status_t allocator_alloc_pair(ftl_allocator_t *alloc, uint8_t temperature, nand_pba_t out[PLANE_NUM])
{
    ASSERT(alloc != NULL);
    ASSERT(out != NULL);
    ASSERT(temperature < FTL_TEMP_NUM);

    super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    /* 读恢复可能把 Open SB 内的块标 BAD；BAD 终态不能再写。 */
    if (sb->is_open)
    {
        for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (!super_block_has_plane(sb, plane))
            {
                continue;
            }
            if (ftl_block_is_bad(&alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])]))
            {
                allocator_close_sb(alloc, sb);
                break;
            }
        }
    }

    if (!sb->is_open || (sb->next_page >= PAGES_PER_BLOCK))
    {
        if (sb->is_open)
        {
            allocator_close_sb(alloc, sb);
        }
        if (!allocator_open_sb(alloc, sb, temperature))
        {
            return FTL_AGAIN;
        }
    }

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        out[plane] = super_block_page_pba(allocator_topo(alloc), sb, plane, sb->next_page);
    }

    sb->next_page++;
    return FTL_OK;
}

/* 实现要点：从 Free Pool 取该 plane 擦除最少的块，出池并置 LOG。 */
ftl_status_t allocator_alloc_log_block(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    ASSERT(alloc != NULL);
    ASSERT(plane < PLANE_NUM);
    ASSERT(out_gblock != NULL);

    uint16_t gblock = allocator_select_plane_block_from_pool(alloc, plane);

    if (gblock == FTL_GBLOCK_INVALID)
    {
        return FTL_AGAIN; /* 该 plane Free Pool 空；不向 OP 池借 */
    }

    allocator_remove_from_free(alloc, gblock);

    ftl_block_t *b = &alloc->blocks[gblock];

    ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    *out_gblock = gblock;
    return FTL_OK;
}

/* 实现要点：已关闭的槽再关一次是合法 no-op ⇒ 返回 FTL_ERROR，不转 ASSERT。 */
ftl_status_t allocator_close_open_sb(ftl_allocator_t *alloc, uint8_t temperature)
{
    ASSERT(alloc != NULL);
    ASSERT(temperature < FTL_TEMP_NUM);

    super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if (!sb->is_open)
    {
        return FTL_ERROR;
    }

    allocator_close_sb(alloc, sb);
    return FTL_OK;
}

/* 实现要点：重建按单个物理块建条目，另一 plane 记哨兵；next_page 记满。 */
uint16_t allocator_add_closed_sb(ftl_allocator_t *alloc,
                                 const uint16_t   plane_blocks[PLANE_NUM],
                                 uint8_t          temperature)
{
    ASSERT(alloc != NULL);
    ASSERT(plane_blocks != NULL);
    ASSERT(temperature < FTL_TEMP_NUM);

    super_block_t sb;

    memset(&sb, 0, sizeof(sb));
    sb.next_page   = (uint8_t)PAGES_PER_BLOCK;
    sb.temperature = temperature;
    sb.is_open     = false;

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        sb.plane_blocks[plane] = plane_blocks[plane];
    }

    return allocator_closed_sb_commit(alloc, &sb);
}

/* 实现要点：swap-remove；末位条目搬家后修正其各 plane 的 sb_id。
 * 首循环兜住“free/retire 被拒绝、sb_id 未清”的路径。 */
void allocator_remove_closed_sb(ftl_allocator_t *alloc, uint16_t closed_idx)
{
    ASSERT(alloc != NULL);
    ASSERT(closed_idx < alloc->closed_sb_count);

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (alloc->closed_sb_list[closed_idx].plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue;
        }

        ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, alloc->closed_sb_list[closed_idx].plane_blocks[plane])];

        if (ftl_block_sb_id(b) == closed_idx)
        {
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        }
    }

    alloc->closed_sb_count--;
    if (closed_idx != alloc->closed_sb_count)
    {
        super_block_t *moved = &alloc->closed_sb_list[closed_idx];
        *moved               = alloc->closed_sb_list[alloc->closed_sb_count];

        for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (moved->plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
            {
                continue;
            }

            ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, moved->plane_blocks[plane])];

            if (ftl_block_sb_id(b) == alloc->closed_sb_count)
            {
                ftl_block_set_sb(b, closed_idx);
            }
        }
    }
}

/* ── 5. 只读统计 ── */

/* 参与磨损统计的块：非 BAD 且已归属（state != NONE）。
 * NONE 的 erase_count 不随擦除累加，计入会把 erase_min 永久钉在 0。 */
static bool allocator_block_is_wear_counted(const ftl_block_t *b)
{
    ASSERT(b != NULL);
    return !ftl_block_is_bad(b) && (ftl_block_state(b) != FTL_BLOCK_STATE_NONE);
}

/* 实现要点：跳过哨兵与 BAD plane；valid_count 单位是 CW。 */
void allocator_sb_stats(const ftl_allocator_t *alloc, const super_block_t *sb, allocator_sb_stats_t *out)
{
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);
    ASSERT(sb != NULL);
    ASSERT(out != NULL);

    uint16_t valid     = 0U;
    uint32_t erase_max = 0U;
    uint32_t erase_sum = 0U;
    uint32_t pe_min    = UINT32_MAX;
    uint32_t read_sum  = 0U;
    uint8_t  n         = 0U;

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (!super_block_has_plane(sb, plane))
        {
            continue;
        }

        const ftl_block_t *blk = &alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])];

        if (ftl_block_is_bad(blk))
        {
            continue;
        }

        valid = (uint16_t)(valid + blk->valid_count);
        erase_sum += blk->erase_count;
        if (blk->erase_count > erase_max)
        {
            erase_max = blk->erase_count;
        }
        if (blk->last_pe_ts < pe_min)
        {
            pe_min = blk->last_pe_ts;
        }
        read_sum += blk->read_count;
        n++;
    }

    out->valid_cw    = valid;
    out->plane_count = n;
    out->erase_max   = erase_max;
    out->erase_sum   = erase_sum;
    out->pe_min      = pe_min;
    out->read_sum    = read_sum;
}

/* 实现要点：一次扫描；跳过 BAD 与 NONE（固定元数据 / OP / Spare / 未初始化）。 */
void allocator_erase_stats(const ftl_allocator_t *alloc, allocator_erase_stats_t *out)
{
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);
    ASSERT(out != NULL);

    uint32_t min_ec = UINT32_MAX;
    uint32_t max_ec = 0U;
    uint32_t sum    = 0U;
    uint32_t max_t  = 0U;
    uint16_t n      = 0U;

    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *blk = &alloc->blocks[i];

        if (!allocator_block_is_wear_counted(blk))
        {
            continue;
        }
        if (blk->erase_count < min_ec)
        {
            min_ec = blk->erase_count;
        }
        if (blk->erase_count > max_ec)
        {
            max_ec = blk->erase_count;
        }
        sum += blk->erase_count;
        if (blk->last_pe_ts > max_t)
        {
            max_t = blk->last_pe_ts;
        }
        n++;
    }

    out->erase_min  = (n == 0U) ? 0U : min_ec;
    out->erase_max  = max_ec;
    out->erase_sum  = sum;
    out->max_pe_ts  = max_t;
    out->good_count = n;
}

/* 实现要点：口径与 allocator_erase_stats 相同。 */
uint16_t allocator_count_blocks_below(const ftl_allocator_t *alloc, uint32_t threshold)
{
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);

    uint16_t count = 0U;

    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *blk = &alloc->blocks[i];

        if (!allocator_block_is_wear_counted(blk))
        {
            continue;
        }
        if (blk->erase_count < threshold)
        {
            count++;
        }
    }

    return count;
}

/* 实现要点：比较全部 plane——不同 SB 的哨兵 plane 可能相同，只比一个会误中。 */
bool allocator_find_closed_sb(const ftl_allocator_t *alloc, const super_block_t *sb, uint16_t *out_idx)
{
    ASSERT(alloc != NULL);
    ASSERT(sb != NULL);
    ASSERT(out_idx != NULL);

    for (uint16_t i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_t *e    = &alloc->closed_sb_list[i];
        bool                 same = true;

        for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (e->plane_blocks[plane] != sb->plane_blocks[plane])
            {
                same = false;
                break;
            }
        }
        if (same)
        {
            *out_idx = i;
            return true;
        }
    }

    return false;
}
