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
ftl_status_t allocator_init(ftl_allocator_t *alloc, const ftl_init_params_t *params)
{
    if ((alloc == NULL) || (params == NULL))
    {
        return FTL_ERROR;
    }

    alloc->ch           = params->topo.ch;
    alloc->ce           = params->topo.ce;
    alloc->lun          = params->topo.lun;
    alloc->total_blocks = params->free_count;

    /* Free Pool */
    alloc->free_blocks     = params->free_blocks;
    alloc->free_count      = 0U;
    alloc->pre_cond_count  = 0U;
    alloc->emergency_count = 0U;

    /* Open Super Block（暂无效，首次 alloc 时分配） */
    memset(&alloc->hot_sb, 0, sizeof(alloc->hot_sb));
    memset(&alloc->cold_sb, 0, sizeof(alloc->cold_sb));
    alloc->hot_sb.temperature  = FTL_TEMP_HOT;
    alloc->cold_sb.temperature = FTL_TEMP_COLD;

    /* Closed 列表 */
    alloc->closed_sb_list  = params->closed_sb_list;
    alloc->closed_sb_count = 0U;

    /* Block State Table（每 Block 状态统一承载；由调用者提供存储） */
    alloc->blocks           = params->blocks;
    alloc->bad_count        = 0U;
    alloc->spare_pool_count = 0U;

    /* 状态机初始：未坏块默认 FREE（坏块由 bbt_init 置 BAD） */
    if (alloc->blocks != NULL)
    {
        uint16_t i;
        for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
        {
            ftl_block_t *b = &alloc->blocks[i];
            if (!ftl_block_is_bad(b))
            {
                ftl_block_state_set(b, FTL_BLOCK_STATE_FREE);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            }
        }
    }

    return FTL_OK;
}

/*
 * 内部工具：从 Free Pool 移除一个 gblock（swap-remove，O(1)）
 * @return 1 移除成功，0 未找到
 */
static uint8_t allocator_pool_remove(ftl_allocator_t *alloc, uint16_t gblock)
{
    uint16_t i;

    for (i = 0U; i < alloc->free_count; i++)
    {
        if (alloc->free_blocks[i] == gblock)
        {
            alloc->free_blocks[i] = alloc->free_blocks[--alloc->free_count];
            return 1U;
        }
    }
    return 0U;
}

/*
 * 内部工具：从 Free Pool 选一个 Super Block 的 block 号（磨损均衡）
 *
 * 行业标准：Super Block 的 plane0/plane1 用同一 block 号（NAND 行地址 plane
 * 只差 BA6 位，block 必须同号），Multi-Plane 命令一次写双 plane 同号页。
 * 遍历 block 号 b in 0..BLOCKS_PER_PLANE-1，两 plane 的 gblock 都必须
 * 在 Free Pool（state & FREE）且非坏；寿命取双 plane 平均擦除次数（Super
 * Block 整体寿命 = 两 plane 共同），选平均最小。
 *
 * 若 free_count 为 0 或无可配对 block 则返回 UINT16_MAX
 */
static uint16_t allocator_select_block(ftl_allocator_t *alloc)
{
    uint16_t b;
    uint16_t best_block = UINT16_MAX;
    uint32_t min_erase  = UINT32_MAX;

    if ((alloc == NULL) || (alloc->blocks == NULL) || (alloc->free_count == 0U))
    {
        return UINT16_MAX;
    }

    for (b = 0U; b < BLOCKS_PER_PLANE; b++)
    {
        uint16_t    g0 = ftl_gblock(0U, b);
        uint16_t    g1 = ftl_gblock(1U, b);
        ftl_block_t *blk0 = &alloc->blocks[g0];
        ftl_block_t *blk1 = &alloc->blocks[g1];
        uint32_t    sum;

        /* 双 plane 都必须空闲且非坏（坏块不参与配对） */
        if ((!ftl_block_state_is(blk0, FTL_BLOCK_STATE_FREE)) ||
            (!ftl_block_state_is(blk1, FTL_BLOCK_STATE_FREE)) ||
            ftl_block_is_bad(blk0) || ftl_block_is_bad(blk1))
        {
            continue;
        }

        /* 寿命取双 plane 平均擦除次数；比较用和（均值单调同序） */
        sum = (uint32_t)blk0->erase_count + (uint32_t)blk1->erase_count;
        if (sum < min_erase)
        {
            min_erase  = sum;
            best_block = b;
        }
    }

    return best_block;
}

/*
 * 内部工具：从 Open 转移到 Closed 列表
 *
 * 注意：sb_id 语义统一为「Closed 列表索引」——块在 Open SB 时 sb_id=UINT16_MAX（占位），
 * 关闭后指向 closed_sb_list 中的条目索引；GC 回收从列表移除时恢复 UINT16_MAX。
 */
static void allocator_close_sb(ftl_allocator_t *alloc, super_block_t *sb)
{
    uint8_t plane;
    uint16_t closed_idx = alloc->closed_sb_count;

    /* 写满关闭：两物理块置 CLOSED（清 OPEN），记录反向索引 = closed 列表索引 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])];
        ftl_block_state_clr(b, FTL_BLOCK_STATE_OPEN);
        ftl_block_state_set(b, FTL_BLOCK_STATE_CLOSED);
        ftl_block_set_sb(b, closed_idx);
    }

    if (alloc->closed_sb_count < SUPER_BLOCK_LIST_MAX)
    {
        alloc->closed_sb_list[alloc->closed_sb_count++] = *sb;
    }
    sb->valid = 0U;
}

/*
 * allocator_remove_closed_sb - GC 回收后从 Closed 列表移除条目（swap-remove，O(1)）
 *
 * 不移除会让 closed_sb_count 只增不减 → 列表爆炸 + 已回收 SB 被再次选为 victim
 * （其 block 已 FREE、VPC=0，select_victim 恒命中）→ 无限 GC 空转。
 *
 * @param alloc      Allocator 上下文
 * @param closed_idx 要移除的 closed 列表索引
 */
void allocator_remove_closed_sb(ftl_allocator_t *alloc, uint16_t closed_idx)
{
    uint8_t plane;

    if ((alloc == NULL) || (closed_idx >= alloc->closed_sb_count))
    {
        return;
    }

    /* 被移除条目的两 block 恢复"未归属"（已由 allocator_free_block 置 FREE + 清 sb_id，
     * 这里兜底再清一次索引，避免残留） */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        ftl_block_t *b = &alloc->blocks[ftl_gblock(plane,
                                                   alloc->closed_sb_list[closed_idx].plane_blocks[plane])];
        if (ftl_block_sb_id(b) == closed_idx)
        {
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        }
    }

    /* swap-remove：末位条目搬到被移除位，并修正其两 block 的 sb_id */
    alloc->closed_sb_count--;
    if (closed_idx != alloc->closed_sb_count)
    {
        super_block_t *moved = &alloc->closed_sb_list[closed_idx];
        *moved              = alloc->closed_sb_list[alloc->closed_sb_count];

        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, moved->plane_blocks[plane])];
            if (ftl_block_sb_id(b) == alloc->closed_sb_count)
            {
                ftl_block_set_sb(b, closed_idx);
            }
        }
    }
}

ftl_status_t allocator_close_open_sb(ftl_allocator_t *alloc, uint8_t temperature)
{
    super_block_t *sb;

    if (alloc == NULL)
    {
        return FTL_ERROR;
    }

    sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;
    if ((sb == NULL) || (sb->valid == 0U))
    {
        return FTL_ERROR; /* 无对应 Open SB */
    }

    /* 关闭：置 CLOSED + 进 closed_sb_list + valid=0（行业坏块替换流程 ①） */
    allocator_close_sb(alloc, sb);
    return FTL_OK;
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

        /* 行业标准：Super Block 双 plane 同号 block（Multi-Plane 命令要求） */
        block0 = allocator_select_block(alloc);
        if (block0 == UINT16_MAX)
        {
            /*
             * Free Pool 无可配对 block（耗尽 / 单 plane 坏块残留）。
             *
             * 返回 NAND_PBA_INVALID，调用者（ftl_submit_write）需触发紧急 GC
             * 回收 Closed Super Block 后重试。
             * pre_cond_count 和 emergency_count 已反映当前 free pool 状态。
             */
            return NAND_PBA_INVALID;
        }
        block1 = block0; /* 双 plane 同号：plane 由 gblock 位域区分 */

        /* 从 Free Pool 移除双 plane 两个 gblock（swap-remove） */
        (void)allocator_pool_remove(alloc, ftl_gblock(0U, block0));
        (void)allocator_pool_remove(alloc, ftl_gblock(1U, block1));

        sb->plane_blocks[0] = block0;
        sb->plane_blocks[1] = block1;
        sb->next_page       = 0U;
        sb->temperature     = temperature;
        sb->valid           = 1U;

        /* 绑定真实拓扑（Phase V: 多 LUN 时 Super Block 必须知道所属 ch/ce/lun，
         * super_block_page_addr 由此展开地址——此前 sb->topo 恒为零） */
        sb->topo.ch  = alloc->ch;
        sb->topo.ce  = alloc->ce;
        sb->topo.lun = alloc->lun;

        /* 状态机：两物理块置 OPEN（清 FREE/CLOSED）+ 记录温度流 + 反向索引 sb_id
         * （OPEN 时 sb_id 占位 NO_SB，关闭后由 allocator_close_sb 写 closed 索引） */
        {
            uint8_t plane;
            for (plane = 0U; plane < PLANE_NUM; plane++)
            {
                uint16_t    gblock = ftl_gblock(plane, (plane == 0U) ? block0 : block1);
                ftl_block_t *b     = &alloc->blocks[gblock];

                ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE | FTL_BLOCK_STATE_CLOSED);
                ftl_block_state_set(b, FTL_BLOCK_STATE_OPEN);
                ftl_block_set_temp(b, temperature);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            }
        }

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

void allocator_free_block(ftl_allocator_t *alloc, uint16_t gblock)
{
    ftl_block_t *b;

    if (alloc == NULL)
    {
        return;
    }

    /*
     * Block 擦除由调用者（gc_erase_and_free）确保在调用本函数前已完成。
     * 本函数只归还 Block 编号（全局 Block 号）到 Free Pool，并同步 pre_cond_count。
     */
    if (alloc->free_count < FTL_BLOCK_TOTAL)
    {
        alloc->free_blocks[alloc->free_count++] = gblock;
    }

    /* 状态机：置 FREE（清 OPEN/CLOSED/TEMP），清反向索引 */
    b = &alloc->blocks[gblock];
    ftl_block_state_clr(b, FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED |
                           FTL_BLOCK_TEMP_MASK);
    ftl_block_state_set(b, FTL_BLOCK_STATE_FREE);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    /* 所有 Free Pool 中的 Block 都已擦除 → pre_cond_count = free_count */
    alloc->pre_cond_count = alloc->free_count;
}

/* 说明：allocator_mark_bad 已删除——坏块标记统一走 bbt_mark_bad（BBT 单数据源）。 */

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