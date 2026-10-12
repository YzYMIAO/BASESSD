/*
 * allocator.c — 块分配器（Free Pool + Open/Closed Super Block + 磨损选块）
 *
 * 职责：
 *   管理每个 FTL 实例的块账本：Free Pool（已擦除块）、Open Super Block（热/冷两条写入流）、
 *   Closed Super Block 列表（GC 的候选集）、OP/Spare 保留池；并提供选块（按 plane 挑擦除最少）、
 *   分配一对 plane 页、归还块、保留块（RESERVED，元数据日志块专用）。
 *
 * 与 allocator.h 的分工：
 *   allocator.h —— 结构（ftl_allocator_t / super_block_t）+ API + 水位宏；
 *   本文件     —— 上述结构的全部算法实现。
 *
 * Free Pool 模型（P1-3）：
 *   它是**一个已擦除池**（先擦后归还 ⇒ 池内无未擦除块）。所谓"三级"是建在 free_count 上的
 *   **水位**（≥32 / 8<x<32 / ≤8），**不是三个分区**——见 allocator.h 文件头。
 *
 * Super Block 状态机（见 ftl_block_t 状态位）：
 *   FREE → OPEN（allocator_open_sb）→ CLOSED（allocator_close_sb）→ FREE（allocator_free_block）
 *   RESERVED 是旁路状态：独占块（元数据日志块）不参与 Open/Closed 流。
 *
 * 文件结构：
 *   1. 初始化        allocator_init
 *   2. 内部工具      allocator_pool_remove / pull_from_op / select_plane_block_from_pool /
 *                    open_sb / close_sb
 *   3. 关闭/回收     allocator_close_open_sb / allocator_remove_closed_sb
 *   4. 分配          allocator_alloc_pair / allocator_alloc / allocator_alloc_reserved_block
 *   5. 归还          allocator_free_block / allocator_free_reserved_block / allocator_remove_from_free
 *   6. 重建入口      allocator_add_closed_sb
 *
 * ⚠ 本文件只做**账本**，不做介质操作：块的擦除由调用者（gc_erase_and_free 等）在归还前完成。
 */

#include "ftl/allocator.h"

#include "common/assertion.h"

#include <string.h>

/*
 * allocator_init 初始化 Allocator
 *
 *   - 初始化分配器、温度参数、free pool、closed 列表
 *   - 第一个 Super Block 分配需延迟到首次 allocator_alloc()
 *
 * Free Pool 是**一个已擦除池**（先擦后归还 ⇒ 池内无未擦除块），
 * 「三级」是建在 free_count 上的**水位**，不是三个分区——见 allocator.h 文件头。
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
    alloc->free_blocks = params->free_blocks;
    alloc->free_count  = 0U;

    /* OP / Spare Pool（容量不动） */
    alloc->op_blocks    = params->op_blocks;
    alloc->op_count     = params->op_count;
    alloc->spare_blocks = params->spare_blocks;
    alloc->spare_count  = params->spare_count;

    /* Open Super Block（暂无效，首次 alloc 时分配） */
    memset(&alloc->hot_sb, 0, sizeof(alloc->hot_sb));
    memset(&alloc->cold_sb, 0, sizeof(alloc->cold_sb));
    alloc->hot_sb.temperature  = FTL_TEMP_HOT;
    alloc->cold_sb.temperature = FTL_TEMP_COLD;

    /* Closed 列表 */
    alloc->closed_sb_list  = params->closed_sb_list;
    alloc->closed_sb_count = 0U;

    /* Block State Table（每 Block 状态统一承载；由调用者提供存储）
     * + 独立的逐页有效位图表（主流弹性项，见 bst.h 文件头） */
    alloc->blocks    = params->blocks;
    alloc->valid_map = params->valid_map;
    alloc->bad_count = 0U;

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

/* ── 内部工具 ── */

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
 * 从 OP 池取一个 Block 归还 Free Pool（容量不动语义：OP 是 GC 缓冲池）。
 * 分配器 free pool 耗尽时由 allocator_alloc 回退调用。
 * @return 1 成功取块，0 OP 池空
 */
static uint8_t allocator_pull_from_op(ftl_allocator_t *alloc)
{
    if ((alloc == NULL) || (alloc->op_blocks == NULL) || (alloc->op_count == 0U))
    {
        return 0U;
    }

    alloc->op_count--;
    allocator_free_block(alloc, alloc->op_blocks[alloc->op_count]);

    return 1U;
}

/*
 * 内部工具：从 **Free Pool 数组**里选该 plane 擦除最少的一块（磨损均衡）。
 *
 * ⚠ **唯一合法的选块入口**。判据是**数组**，不是 BST 的 FREE 位——两者账本会不一致：
 * 固定元数据块（per-plane 0..62）在 allocator_init 里被无条件置 FREE，却**不进**
 * Free Pool（main.c 回填与 ftl_rebuild_closed_sb_list 都跳过它们）。若按 FREE 位选块，
 * 新片上一律选中 block 0 = **L2P region-0 哨兵页**（设计上"永不写入"），用户数据把它
 * 覆盖掉 ⇒ 重启后 region 0 读回垃圾 PBA（实测 0x43d555d5，见 ADR）。
 * 以数组为准还保证「选中 ⇒ allocator_pool_remove 必然成功」，不会出现
 * 「选中 → 移除失败 → 重选同一块」的死循环。
 *
 * Policy B：两个 plane **各自独立选块**，不再要求 block 同号——同号是旧策略的取舍，
 * 不是 NAND 约束（multi-plane 行地址每个 plane 各带自己的 block 位）。
 *
 * 返回 gblock；该 plane 无可选块返回 UINT16_MAX。
 */
static uint16_t allocator_select_plane_block_from_pool(const ftl_allocator_t *alloc, uint8_t plane)
{
    uint16_t i;
    uint16_t best      = UINT16_MAX;
    uint32_t min_erase = UINT32_MAX;

    if ((alloc == NULL) || (alloc->blocks == NULL) || (plane >= PLANE_NUM))
    {
        return UINT16_MAX;
    }

    for (i = 0U; i < alloc->free_count; i++)
    {
        uint16_t           gblock = alloc->free_blocks[i];
        const ftl_block_t *blk    = &alloc->blocks[gblock];

        if ((uint8_t)(gblock / BLOCKS_PER_PLANE) != plane)
        {
            continue; /* 不属于目标 plane */
        }
        if (ftl_block_is_bad(blk))
        {
            continue; /* 坏块不该在 Free Pool，兜底再判一次 */
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
 * 内部工具：为 Open SB 槽位分配一对物理块（两 plane 各自独立选块）。
 *
 * 每个 plane 独立取块；某 plane 拿不到时空闲池耗尽 → 从 OP 池补（容量不动语义：
 * OP 是 GC 缓冲池，擦除即用），最多补 PLANE_NUM 次再重试该 plane。
 * 全部就位后：两 gblock 从 Free Pool 移除、置 OPEN、绑拓扑。
 *
 * 返回 1 成功，0 失败（调用者触发紧急 GC）。
 */
static uint8_t allocator_open_sb(ftl_allocator_t *alloc, super_block_t *sb, uint8_t temperature)
{
    uint16_t gblock[PLANE_NUM];
    uint8_t  plane;

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint8_t tries;

        gblock[plane] = allocator_select_plane_block_from_pool(alloc, plane);
        for (tries = 0U; (gblock[plane] == UINT16_MAX) && (tries < PLANE_NUM); tries++)
        {
            if (allocator_pull_from_op(alloc) == 0U)
            {
                break; /* OP 池也空 */
            }
            gblock[plane] = allocator_select_plane_block_from_pool(alloc, plane);
        }
        if (gblock[plane] == UINT16_MAX)
        {
            /* 该 plane 真无空间；已选的 plane 还没移出 Free Pool，无需回滚 */
            return 0U;
        }
    }

    /* 从 Free Pool 移除两 plane 的 gblock（swap-remove），并写入 SB
     * （SB 存 **plane 内块号**；gblock 是跨 plane 全局编号，需取模还原） */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        (void)allocator_pool_remove(alloc, gblock[plane]);
        sb->plane_blocks[plane] = (uint16_t)(gblock[plane] % BLOCKS_PER_PLANE);
    }
    sb->next_page   = 0U;
    sb->temperature = temperature;
    sb->valid       = 1U;

    /* 绑定真实拓扑（Phase V: 多 LUN 时 Super Block 必须知道所属 ch/ce/lun，
     * super_block_page_addr 由此展开地址——此前 sb->topo 恒为零） */
    sb->topo.ch  = alloc->ch;
    sb->topo.ce  = alloc->ce;
    sb->topo.lun = alloc->lun;

    /* 状态机：两物理块置 OPEN（清 FREE/CLOSED——块被 Open SB 占用，从 Free Pool 取走）
     * + 记录温度流 + 反向索引 sb_id
     * （OPEN 时 sb_id 占位 NO_SB，关闭后由 allocator_close_sb 写 closed 索引） */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        ftl_block_t *b = &alloc->blocks[gblock[plane]];

        ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE | FTL_BLOCK_STATE_CLOSED);
        ftl_block_state_set(b, FTL_BLOCK_STATE_OPEN);
        ftl_block_set_temp(b, temperature);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
    }

    return 1U;
}

/*
 * 内部工具：从 Open 转移到 Closed 列表
 *
 * 注意：sb_id 语义统一为「Closed 列表索引」——块在 Open SB 时 sb_id=UINT16_MAX（占位），
 * 关闭后指向 closed_sb_list 中的条目索引；GC 回收从列表移除时恢复 UINT16_MAX。
 */
static void allocator_close_sb(ftl_allocator_t *alloc, super_block_t *sb)
{
    uint8_t  plane;
    uint16_t closed_idx = alloc->closed_sb_count;

    /* 写满关闭：两物理块置 CLOSED（清 OPEN），记录反向索引 = closed 列表索引 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        ftl_block_t *b;

        if (sb->plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue; /* 退化单 plane 条目（掉电重建产物） */
        }
        b = &alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])];
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

/* ── 关闭 / 回收 ── */

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
        ftl_block_t *b;

        if (alloc->closed_sb_list[closed_idx].plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue; /* 退化单 plane 条目 */
        }
        b = &alloc->blocks[ftl_gblock(plane, alloc->closed_sb_list[closed_idx].plane_blocks[plane])];
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
        *moved               = alloc->closed_sb_list[alloc->closed_sb_count];

        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            ftl_block_t *b;

            if (moved->plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
            {
                continue; /* 退化单 plane 条目 */
            }
            b = &alloc->blocks[ftl_gblock(plane, moved->plane_blocks[plane])];
            if (ftl_block_sb_id(b) == alloc->closed_sb_count)
            {
                ftl_block_set_sb(b, closed_idx);
            }
        }
    }
}

/* --------------------------------------------------------------------
 * allocator_close_open_sb
 *   功能：主动关闭指定温度的 Open SB（写满前的显式收尾）。
 *   参数：alloc —— 分配器；temperature —— FTL_TEMP_HOT / FTL_TEMP_COLD 选槽。
 *   返回：FTL_OK 成功；FTL_ERROR 参数非法 / 对应 Open SB 无效。
 *   实现要点：置 CLOSED + 进 closed_sb_list + valid=0（行业坏块替换流程 ①）。
 * -------------------------------------------------------------------- */
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

/* ── 分配 ── */

/* --------------------------------------------------------------------
 * allocator_alloc_pair
 *   功能：分配一对 plane 页——推进 Open SB 写指针，返回两 plane 同页 PBA。
 *   参数：alloc —— 分配器；temperature —— 热/冷流；out[PLANE_NUM] —— 出参两 plane PBA。
 *   返回：out[0]（plane 0 的 PBA）；NAND_PBA_INVALID 参数非法 / 无块可分配（调用者触发紧急 GC）。
 *   实现要点：当前 SB 无效或写满 ⇒ 先关闭旧 SB（若有效）再 allocator_open_sb 取新对；
 *             两 plane 取同一页号（multi-plane 一次烧两 plane 同页）；最后推进写指针。
 * -------------------------------------------------------------------- */
nand_pba_t allocator_alloc_pair(ftl_allocator_t *alloc, uint8_t temperature, nand_pba_t out[PLANE_NUM])
{
    super_block_t *sb;
    uint8_t        plane;

    if ((alloc == NULL) || (out == NULL))
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
        if (allocator_open_sb(alloc, sb, temperature) == 0U)
        {
            return NAND_PBA_INVALID; /* Free/OP 池都拿不出某个 plane 的块 → 调用者触发紧急 GC */
        }
    }

    /* 两 plane 同一页：multi-plane 写一次烧两个 plane 的同页 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        out[plane] = super_block_page_pba(alloc->ch, alloc->ce, sb, plane, sb->next_page);
    }

    /* 推进写指针 */
    sb->next_page++;

    return out[0];
}

/* allocator_alloc — 单 plane 便捷：分配一对后返回 plane 0 那一半（GC / WL / metadata 单 plane 写路径） */
nand_pba_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature)
{
    nand_pba_t pair[PLANE_NUM];

    if (allocator_alloc_pair(alloc, temperature, pair) == NAND_PBA_INVALID)
    {
        return NAND_PBA_INVALID;
    }

    /* 单 plane 便捷：返回 plane 0 的那一半（GC / WL / metadata 的单 plane 写路径） */
    return pair[0];
}

/* --------------------------------------------------------------------
 * allocator_free_block
 *   功能：把 gblock 归还 Free Pool，并置 FREE、清 OPEN/CLOSED/TEMP/RESERVED、清反向索引。
 *   参数：alloc —— 分配器；gblock —— 全局块号。
 *   返回：无。
 *   实现要点：**调用者必须先擦除该块**（本函数只还编号）。RESERVED 必须在清除之列，
 *             否则独占块归还后仍带 RESERVED，被再取走时状态叠加。Free Pool 是单一
 *             已擦除池，无派生计数需同步（三级水位直接建在 free_count 上，P1-3）。
 * -------------------------------------------------------------------- */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t gblock)
{
    ftl_block_t *b;

    if (alloc == NULL)
    {
        return;
    }

    /*
     * Block 擦除由调用者（gc_erase_and_free）确保在调用本函数前已完成。
     * 本函数只归还 Block 编号（全局 Block 号）到 Free Pool。
     */
    if (alloc->free_count < FTL_BLOCK_TOTAL)
    {
        alloc->free_blocks[alloc->free_count++] = gblock;
    }

    /* 状态机：置 FREE（本入口的契约是「调用者已擦除」⇒ 进池即已擦除态。GC 擦除 /
     * OP 补池 / Spare 补池 / rebuild 拉回全走本入口）。清 OPEN/CLOSED/TEMP/RESERVED、
     * 清反向索引。
     * ⚠ RESERVED 必须在清除之列：否则独占块（元数据日志块）归还后仍带 RESERVED，
     *   被 allocator_alloc_reserved_block 再次取走时状态叠加，且「已归还」语义为假。
     * 无派生计数需要同步：Free Pool 是**一个**已擦除池，三级水位直接建在 free_count 上（P1-3）。 */
    b = &alloc->blocks[gblock];
    ftl_block_state_clr(b, FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED | FTL_BLOCK_TEMP_MASK);
    ftl_block_state_clr(b, FTL_BLOCK_STATE_RESERVED);
    ftl_block_state_set(b, FTL_BLOCK_STATE_FREE);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

/* --------------------------------------------------------------------
 * allocator_alloc_reserved_block
 *   功能：从指定 plane 的 Free Pool 取一块并置为 RESERVED（独占，元数据日志块用）。
 *   参数：alloc —— 分配器；plane —— 目标 plane；out_gblock —— 出参 gblock。
 *   返回：FTL_OK；FTL_ERROR 参数非法 / 该 plane Free Pool 空。
 *   实现要点：独占三件套——① 从 Free Pool 数组拿掉 ② 置 RESERVED ③ 清 FREE。
 *             只置 RESERVED 挡不住分配（选块以数组为准），必须先从数组拿掉；
 *             **不向 OP 池借**（OP 是 GC 的紧急缓冲）。
 * -------------------------------------------------------------------- */
ftl_status_t allocator_alloc_reserved_block(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    uint16_t     gblock;
    ftl_block_t *b;

    if ((alloc == NULL) || (alloc->blocks == NULL) || (out_gblock == NULL) || (plane >= PLANE_NUM))
    {
        return FTL_ERROR;
    }

    gblock = allocator_select_plane_block_from_pool(alloc, plane);
    if (gblock == UINT16_MAX)
    {
        return FTL_ERROR; /* 该 plane 的 Free Pool 空（不向 OP 池借：OP 是 GC 的紧急缓冲） */
    }

    /* 独占三件套：① 从 Free Pool 拿掉 ② 置 RESERVED ③ 清 FREE
     * （只置 RESERVED 挡不住分配——选块以 Free Pool 数组为准，不先从数组拿掉
     *   就会被下一次 allocator_select_plane_block_from_pool 再选中） */
    (void)allocator_pool_remove(alloc, gblock);

    b = &alloc->blocks[gblock];
    ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE | FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED | FTL_BLOCK_TEMP_MASK);
    ftl_block_state_set(b, FTL_BLOCK_STATE_RESERVED);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    *out_gblock = gblock;

    return FTL_OK;
}

/* allocator_free_reserved_block — 归还独占块；与 allocator_free_block 同一入口语义（调用者保证已擦除） */
void allocator_free_reserved_block(ftl_allocator_t *alloc, uint16_t gblock)
{
    if ((alloc == NULL) || (alloc->blocks == NULL) || (gblock >= FTL_BLOCK_TOTAL))
    {
        return;
    }

    /* 与 allocator_free_block 同一入口语义（置 FREE、清 RESERVED、清反向索引）。
     * 调用者保证块已擦除——与 allocator_free_block 的契约一致。 */
    allocator_free_block(alloc, gblock);
}

/* 说明：allocator_mark_bad 已删除——坏块标记统一走 bbt_mark_bad（BBT 单数据源）。 */

/* ── 重建入口 ── */

/* --------------------------------------------------------------------
 * allocator_add_closed_sb
 *   功能：开机重建时把一个 Closed SB 条目装进 closed_sb_list（并设块状态/温度/反向索引）。
 *   参数：alloc —— 分配器；plane_blocks[PLANE_NUM] —— 两 plane 内块号（可含 FTL_BLOCK_NO_PLANE）；
 *         temperature —— 温度流。
 *   返回：新条目在列表中的索引；UINT16_MAX 参数非法。
 *   实现要点：列表满在重建阶段**不应发生**（好块数远小于 1024）⇒ ASSERT（块账本已破坏）；
 *             条目 next_page=64（=满）、valid=0（Closed，gc_select_victim 依赖）。
 * -------------------------------------------------------------------- */
uint16_t allocator_add_closed_sb(ftl_allocator_t *alloc,
                                 const uint16_t   plane_blocks[PLANE_NUM],
                                 uint8_t          temperature)
{
    super_block_t sb;
    uint8_t       plane;

    if ((alloc == NULL) || (plane_blocks == NULL))
    {
        return UINT16_MAX;
    }

    /* 列表满在重建阶段不应发生（好块数远小于 1024）——满了说明块账本已破坏 */
    ASSERT(alloc->closed_sb_count < SUPER_BLOCK_LIST_MAX);

    /* 组装条目（next_page=64=满；valid=0=Closed，gc_select_victim 依赖） */
    memset(&sb, 0, sizeof(sb));
    sb.topo.ch         = alloc->ch;
    sb.topo.ce         = alloc->ce;
    sb.topo.lun        = alloc->lun;
    sb.plane_blocks[0] = plane_blocks[0];
    sb.plane_blocks[1] = plane_blocks[1];
    sb.next_page       = (uint8_t)PAGES_PER_BLOCK;
    sb.temperature     = temperature;
    sb.valid           = 0U;

    /* 双 Plane 置 CLOSED（清 FREE/OPEN）+ 温度 + 反向索引 = closed 列表索引。
     * plane_blocks[plane] 允许为 FTL_BLOCK_NO_PLANE（重建产生的退化单 plane 条目）。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        ftl_block_t *b;

        if (plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue;
        }
        b = &alloc->blocks[ftl_gblock(plane, plane_blocks[plane])];
        ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE | FTL_BLOCK_STATE_OPEN);
        ftl_block_state_set(b, FTL_BLOCK_STATE_CLOSED);
        ftl_block_set_temp(b, temperature);
        ftl_block_set_sb(b, alloc->closed_sb_count);
    }

    alloc->closed_sb_list[alloc->closed_sb_count++] = sb;

    return (uint16_t)(alloc->closed_sb_count - 1U);
}

/* allocator_remove_from_free — 仅从 Free Pool 数组移除（不动状态位；rebuild 把块"认领"出池时用） */
void allocator_remove_from_free(ftl_allocator_t *alloc, uint16_t gblock)
{
    if (alloc == NULL)
    {
        return;
    }

    (void)allocator_pool_remove(alloc, gblock);
}

/* 说明：allocator_needs_gc 已删除（P1-3）——零调用者，且判定与 gc_check_triggers()
 * 的 T1（水位）/ T4（紧急）重复。GC 触发的**单一数据源** = ftl/gc.h 的 GC_TRIGGER_*。 */
