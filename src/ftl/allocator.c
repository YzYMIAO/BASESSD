#include "ftl/allocator.h"

#include <string.h>

/*
 * allocator_init 初始化 Allocator
 *
 *   - 初始化分配器、温度参数、free pool、closed 列表
 *   - 第一个 Super Block 分配需延迟到首次 allocator_alloc()
 *   - pre_cond_count 跟踪当前 Free Pool 中可用的 Block 数，
 *     所有归还到 Free Pool 的 Block 都已擦除（调用者保证），
 *     所以 pre_cond_count 始终等于 free_count。
 */
ftl_status_t allocator_init(ftl_allocator_t *alloc,
                            uint8_t          ch,
                            uint8_t          ce,
                            uint16_t        *free_blocks,
                            uint16_t         free_block_count,
                            uint16_t        *bad_blocks,
                            uint32_t        *erase_counts,
                            super_block_t   *closed_sb_list)
{
    if (alloc == NULL)
    {
        return FTL_ERROR;
    }

    alloc->ch           = ch;
    alloc->ce           = ce;
    alloc->total_blocks = free_block_count;

    /* Free Pool */
    alloc->free_blocks     = free_blocks;
    alloc->free_count      = 0U;
    alloc->pre_cond_count  = 0U;
    alloc->emergency_count = 0U;

    /* Open Super Block（暂无效，首次 alloc 时分配） */
    memset(&alloc->hot_sb, 0, sizeof(alloc->hot_sb));
    memset(&alloc->cold_sb, 0, sizeof(alloc->cold_sb));
    alloc->hot_sb.temperature  = FTL_TEMP_HOT;
    alloc->cold_sb.temperature = FTL_TEMP_COLD;

    /* Closed 列表 */
    alloc->closed_sb_list  = closed_sb_list;
    alloc->closed_sb_count = 0U;

    /* 磨损信息 */
    alloc->erase_counts = erase_counts;

    /* 坏块 */
    alloc->bad_blocks       = bad_blocks;
    alloc->bad_count        = 0U;
    alloc->spare_pool_count = 0U;

    return FTL_OK;
}

/*
 * 内部工具：从 Free Pool 选择一个 Block（磨损均衡：选擦除次数最少的）
 * 若 free_count 为 0 则返回 UINT16_MAX
 */
static uint16_t allocator_select_block(ftl_allocator_t *alloc)
{
    uint16_t best_idx   = 0U;
    uint16_t best_block = UINT16_MAX;
    uint32_t min_erase  = UINT32_MAX;
    uint16_t i;

    if (alloc->free_count == 0U)
    {
        return UINT16_MAX;
    }

    for (i = 0U; i < alloc->free_count; i++)
    {
        uint16_t block = alloc->free_blocks[i];
        uint32_t ec    = alloc->erase_counts[block];

        if (ec < min_erase)
        {
            min_erase  = ec;
            best_idx   = i;
            best_block = block;
        }
    }

    /* 从 Free Pool 移除选中 Block */
    alloc->free_blocks[best_idx] = alloc->free_blocks[--alloc->free_count];
    return best_block;
}

/*
 * 内部工具：从 Open 转移到 Closed 列表
 */
static void allocator_close_sb(ftl_allocator_t *alloc, super_block_t *sb)
{
    if (alloc->closed_sb_count < SUPER_BLOCK_LIST_MAX)
    {
        alloc->closed_sb_list[alloc->closed_sb_count++] = *sb;
    }
    sb->valid = 0U;
}

nand_pba_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature)
{
    super_block_t *sb;
    uint16_t       block0;
    uint16_t       block1;
    nand_pba_t     pba;

    if (alloc == NULL)
    {
        return NAND_PBA_INVALID;
    }

    sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    /* 当前 Super Block 无效或已写满 → 关闭并分配新 Super Block */
    if ((sb->valid == 0U) || (sb->next_page >= PAGES_PER_BLOCK))
    {
        if (sb->valid != 0U)
        {
            allocator_close_sb(alloc, sb);
        }

        block0 = allocator_select_block(alloc);
        block1 = allocator_select_block(alloc);

        if ((block0 == UINT16_MAX) || (block1 == UINT16_MAX))
        {
            /*
             * Free Pool 耗尽。
             *
             * 返回 NAND_PBA_INVALID，调用者（ftl_submit_write）需触发紧急 GC
             * 回收 Closed Super Block 后重试。
             * pre_cond_count 和 emergency_count 已反映当前 free pool 状态。
             */
            return NAND_PBA_INVALID;
        }

        sb->plane_blocks[0] = block0;
        sb->plane_blocks[1] = block1;
        sb->next_page       = 0U;
        sb->temperature     = temperature;
        sb->valid           = 1U;

        /* 同步 pre_cond_count：已从 Free Pool 取走 2 个 Block */
        if (alloc->pre_cond_count > 2U)
        {
            alloc->pre_cond_count -= 2U;
        }
        else
        {
            alloc->pre_cond_count = 0U;
        }
    }

    /* 返回当前写入页的 PBA（Plane 0） */
    pba = super_block_page_pba(alloc->ch, alloc->ce, sb, 0U, sb->next_page);

    /* 推进写指针 */
    sb->next_page++;

    return pba;
}

void allocator_free_block(ftl_allocator_t *alloc, uint16_t block)
{
    if (alloc == NULL)
    {
        return;
    }

    /*
     * Block 擦除由调用者（gc_erase_and_free）确保在调用本函数前已完成。
     * 本函数只归还 Block 编号到 Free Pool，并同步 pre_cond_count。
     */
    if (alloc->free_count < BLOCKS_PER_PLANE * PLANE_NUM)
    {
        alloc->free_blocks[alloc->free_count++] = block;
    }

    /* 所有 Free Pool 中的 Block 都已擦除 → pre_cond_count = free_count */
    alloc->pre_cond_count = alloc->free_count;
}

void allocator_mark_bad(ftl_allocator_t *alloc, uint16_t block)
{
    if (alloc == NULL)
    {
        return;
    }

    /*
     * SRAM 中记录坏块编号，allocator_select_block 在遍历 Free Pool 时
     * 不再涉及此 Block。NAND BBT 的持久化写入由 bbt 模块（bad_block_management.c）
     * 负责，当前为桩。
     */
    if (alloc->bad_count < BLOCKS_PER_PLANE * PLANE_NUM)
    {
        alloc->bad_blocks[alloc->bad_count++] = block;
    }
}

int allocator_needs_gc(const ftl_allocator_t *alloc)
{
    if (alloc == NULL)
    {
        return FTL_OK;
    }

    /* T1: 水位线 — Pre-conditioned 低于目标水位 */
    if (alloc->pre_cond_count < FREE_POOL_TARGET)
    {
        return 1;
    }

    /* T4: 紧急 — Free Pool 空 */
    if (alloc->free_count == 0U)
    {
        return 1;
    }

    return FTL_OK;
}