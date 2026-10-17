/*
 * allocator.c — 块分配器（Free Pool + Open/Closed Super Block + 磨损选块）
 *
 * 职责：
 *   管理每个 FTL 实例的块账本：Free Pool（已擦除块）、Open Super Block（热/冷两条写入流）、
 *   Closed Super Block 列表（GC 的候选集）、OP/Spare 保留池；并提供选块（按 plane 挑擦除最少）、
 *   分配一对 plane 页、归还块、保留块（LOG 态，元数据日志块专用）。
 *
 * 与 allocator.h 的分工：
 *   allocator.h —— 结构（ftl_allocator_t / super_block_t）+ API + 水位宏；
 *   本文件     —— 上述结构的全部算法实现。
 *
 * Free Pool 模型：
 *   它是**一个已擦除池**（先擦后归还 ⇒ 池内无未擦除块）。所谓"三级"是建在 free_count 上的
 *   **水位**（≥32 / 8<x<32 / ≤8），**不是三个分区**——见 allocator.h 文件头。
 *
 * Super Block 状态机（见 ftl_block_state_t）：
 *   FREE → OPEN（allocator_open_sb）→ CLOSED（allocator_close_sb）→ FREE（allocator_free_block）
 *   LOG / RETIRED 是旁路态：独占块（元数据日志块）与退役块不参与 Open/Closed 流。
 *
 * 文件结构：
 *   1. 初始化        allocator_init
 *   2. 内部工具      pull_from_op / select_plane_block_from_pool / open_sb / close_sb
 *   3. 关闭/回收     allocator_close_open_sb / allocator_remove_closed_sb
 *   4. 分配          allocator_alloc_pair / allocator_alloc_log_block
 *   5. 归还          allocator_free_block / allocator_free_log_block / allocator_remove_from_free
 *   6. 重建入口      allocator_add_closed_sb
 *
 * ⚠ 本文件只做**账本**，不做介质操作：块的擦除由调用者（migrate.c 的搬移 job 等）在归还前完成。
 */

#include "ftl/allocator.h"

#include "common/assertion.h"
#include "common/log.h" /* LOGE（判据 9 的 release 运行期拒绝要留日志，不能静默丢） */

#include <string.h>

/*
 * allocator_init 初始化 Allocator
 *
 *   - 绑定**本实例自持表**（指针指向 ftl_tables_t，不复制数据）
 *   - 初始化温度参数、free pool / OP / Spare 计数、closed 列表
 *   - 第一个 Super Block 分配需延迟到首次 allocator_alloc_pair()
 *
 * Free Pool 是**一个已擦除池**（先擦后归还 ⇒ 池内无未擦除块），
 * 「三级」是建在 free_count 上的**水位**，不是三个分区——见 allocator.h 文件头。
 */
void allocator_init(ftl_allocator_t *alloc, ftl_topology_t topo, ftl_tables_t *tables)
{
    /* 前置条件（判据 2：值从哪来）——唯一调用者 ftl.c 传 &ftl->alloc / ftl->tables，
     * 结构上恒非空。 */
    ASSERT(alloc != NULL);
    ASSERT(tables != NULL);

    alloc->ch           = topo.ch;
    alloc->ce           = topo.ce;
    alloc->lun          = topo.lun;
    alloc->total_blocks = 0U; /* 由 HIL 回填 free pool 时按好块数置定（见 main.c） */

    /* 自持表绑定：本实例的数组指针指向自己的 ftl_tables_t，不复制数据。
     * 三处计数（free/op/spare）与 closed 列表计数在此归零——数组内容由 HIL 回填
     * （free pool 回填 / bbt_init / ftl_rebuild_closed_sb_list 重建）逐项写入。 */
    alloc->free_blocks = tables->free_blocks;
    alloc->free_count  = 0U;

    alloc->op_blocks    = tables->op_blocks;
    alloc->spare_blocks = tables->spare_blocks;
    memset(alloc->op_count, 0, sizeof(alloc->op_count));
    memset(alloc->spare_count, 0, sizeof(alloc->spare_count));

    /* Open Super Block（暂无效，首次 alloc 时分配） */
    memset(&alloc->hot_sb, 0, sizeof(alloc->hot_sb));
    memset(&alloc->cold_sb, 0, sizeof(alloc->cold_sb));
    alloc->hot_sb.temperature  = FTL_TEMP_HOT;
    alloc->cold_sb.temperature = FTL_TEMP_COLD;

    alloc->closed_sb_list  = tables->closed_sb_list;
    alloc->closed_sb_count = 0U;

    /* Block State Table（每 Block 状态统一承载；无位图表，见 bst.h） */
    alloc->blocks    = tables->blocks;
    alloc->bad_count = 0U;

    /* 元数据池游标（per-instance）——本实例一份，后续由 bbt_init / wear_init 恢复。
     * 必须在此归零：bbt_state_t.cursor_valid / dirty 与 wear_state_t.dirty[] 决定首次
     * 落盘是「扫池定位」还是「从池首开始」，残留值会让新实例写到别人的槽上。 */
    memset(&alloc->bbt, 0, sizeof(alloc->bbt));
    memset(&alloc->wear, 0, sizeof(alloc->wear));

    /* 反向索引初始：未坏块一律 NO_SB（坏块由 bbt_init 置 BAD，不在此动）。
     * ⚠ 必须显式置：首启 `tables` 在 BSS 里全 0，`sb_id = 0` 会被读成「属于 Closed
     *   列表第 0 条」，而语义上「未归属」是 FTL_BLOCK_NO_SB(UINT16_MAX)。
     *
     * ⚠ **此处不置 FTL_BLOCK_STATE_FREE**：本函数早于元数据目录加载，拿不到
     *   「哪些是元数据块」；而固定元数据块（per-plane 0..62）与 Spare / OP 池块
     *   **都不进** Free Pool。FREE 的语义是「**在 Free Pool 里**」——只由入池闸门
     *   allocator_free_block 置、出池闸门 allocator_remove_from_free 校验（见 allocator.h 不变量②）。
     *   Free Pool 的实际填充在 main.c 的开机回填（走 allocator_free_block）⇒ 池内块带 FREE；
     *   元数据 / Spare / OP 块保持 state = 0（无 BST 状态，按几何地址寻址，不参与分配）。 */
    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        ftl_block_t *b = &alloc->blocks[i];
        if (!ftl_block_is_bad(b))
        {
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        }
    }
}

/* ── 内部工具 ── */

/*
 * 从**指定 plane** 的 OP 池取一个**可用块**归还 Free Pool，并回传其 gblock
 * （容量不动语义：OP 是 GC 缓冲池）。
 * 分配器该 plane 的 free pool 耗尽时由 allocator_open_sb 回退调用。
 * ⚠ 必须按 plane 取：OP 池是按 super block 粒度预留的，取错 plane 的块对该 plane 的
 *   耗尽毫无帮助（只会把块丢进通用 free pool 而选不中）。
 * ⚠ OP 池可能残留运行时被标坏的块（BAD 终态）⇒ 跳过继续弹；坏块弹出即丢弃，
 *   不进 Free Pool（bad_count 已由 bbt_mark_bad 记）。
 * @return true 取到可用块（*out_gblock 有效），false 该 plane OP 池已空
 */
static bool allocator_pull_from_op(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    uint16_t gblock;

    ASSERT(out_gblock != NULL);

    while (allocator_op_try_pop(alloc, plane, &gblock))
    {
        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            continue; /* 坏块不进 Free Pool */
        }
        allocator_free_block(alloc, gblock);
        *out_gblock = gblock;
        return true;
    }
    return false;
}

/*
 * 内部工具：从 **Free Pool 数组**里选该 plane 擦除最少的一块（磨损均衡）。
 *
 * ⚠ **Free Pool 的唯一选块入口**。判据是**数组**（Free Pool 的成员关系），不是 BST 的 FREE 位。
 * 选块与出池由同一对闸门（allocator_free_block / allocator_remove_from_free）维护一致；
 * 以数组为准给出的是**结构性**保证：「选中 ⇒ allocator_remove_from_free 必然成功」，
 * 不会出现「选中 → 移除失败 → 重选同一块」的死循环。
 *
 * Policy B：各 plane **各自独立选块**，不要求 block 同号——multi-plane 行地址
 * 每个 plane 各带自己的 block 位，同号不是 NAND 约束。
 *
 * 返回 gblock；该 plane 无可选块返回 FTL_GBLOCK_INVALID。
 */
static uint16_t allocator_select_plane_block_from_pool(const ftl_allocator_t *alloc, uint8_t plane)
{
    uint16_t best      = FTL_GBLOCK_INVALID;
    uint32_t min_erase = UINT32_MAX;

    /* 前置条件（判据 2）：plane 由调用者（allocator_open_sb 的循环变量 /
     * allocator_alloc_log_block 的 metadata 轮转值）给出；alloc/blocks 由 allocator_init 绑定。 */
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);
    ASSERT(plane < PLANE_NUM);

    for (uint16_t i = 0U; i < alloc->free_count; i++)
    {
        uint16_t           gblock = alloc->free_blocks[i];
        const ftl_block_t *blk    = &alloc->blocks[gblock];

        if (ftl_gblock_plane(gblock) != plane)
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
 * 内部工具：为 Open SB 槽位分配各 plane 的物理块（各自独立选块）。
 *
 * 每个 plane 独立取块；某 plane 拿不到时空闲池耗尽 → 从**该 plane** 的 OP 池补
 * （容量不动语义：OP 是 GC 缓冲池，擦除即用）。
 * 全部就位后：各 plane 的 gblock 从 Free Pool 移除、置 OPEN、绑拓扑。
 *
 * 返回 true 成功，false 失败（调用者触发紧急 GC）。
 */
static bool allocator_open_sb(ftl_allocator_t *alloc, super_block_t *sb, uint8_t temperature)
{
    uint16_t gblock[PLANE_NUM];

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        gblock[plane] = allocator_select_plane_block_from_pool(alloc, plane);
        if (gblock[plane] == FTL_GBLOCK_INVALID)
        {
            /* OP 补池：该 plane 的空闲池刚为空，池内没有其他候选 ⇒ 直接采用刚拉进的块，
             * 不再重扫（坏块由 allocator_pull_from_op 跳过）。失败 = 该 plane 真无空间；
             * 已选的 plane 还没移出 Free Pool，无需回滚。 */
            if (!allocator_pull_from_op(alloc, plane, &gblock[plane]))
            {
                return false;
            }
        }
    }

    /* 从 Free Pool 移除各 plane 的 gblock（swap-remove），写入 SB
     * （SB 存 **plane 内块号**；gblock 是跨 plane 全局编号，需解码还原）；
     * 同时置 OPEN、记录温度流与反向索引 sb_id。
     * ⚠ 顺序：remove_from_free 必须先于 set_state(OPEN)——它校验「在池 ⇒ state == FREE」。
     * OPEN 时 sb_id 占位 NO_SB，关闭后由 allocator_close_sb 写 closed 索引。 */
    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
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
 * 内部工具：从 Open 转移到 Closed 列表
 *
 * 注意：sb_id 语义统一为「Closed 列表索引」——块在 Open SB 时 sb_id=FTL_BLOCK_NO_SB（占位），
 * 关闭后指向 closed_sb_list 中的条目索引；GC 回收从列表移除时恢复 FTL_BLOCK_NO_SB。
 */
static void allocator_close_sb(ftl_allocator_t *alloc, super_block_t *sb)
{
    uint16_t closed_idx = alloc->closed_sb_count;

    /* 写满关闭：各物理块置 CLOSED，记录反向索引 = closed 列表索引 */
    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (sb->plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue; /* 退化单 plane 条目（掉电重建产物） */
        }
        ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])];

        ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
        ftl_block_set_sb(b, closed_idx);
    }

    /* ⚠ 必须先清 is_open 再拷贝（顺序不可换）：GC（gc_select_victim）与 WL 遍历
     * closed_sb_list 时按 `sb->is_open` 跳过（「跳过仍为 Open 的 SB」）。若把 Open 态
     * 原样拷进列表，本条目对 GC/WL **永久不可见** ⇒ 选不出 victim ⇒ 空闲池只出不进
     * ⇒ 写最终永久失败。与重建路径 allocator_add_closed_sb 保持同一约定。 */
    sb->is_open = false;

    /* 列表容量 = FTL_BLOCK_TOTAL（见 allocator.h），而每条 SB 至少占 1 个块 ⇒ 满不可能。
     * 若在此静默丢弃：块的 BST 已置 CLOSED、sb_id 已写成越界索引，却不在列表里
     * ⇒ 既不在 Free Pool、GC 也永不可见 = 永久漏账（容量无声流失、写最终永久 FTL_AGAIN）
     * ⇒ 必须是 ASSERT，不能是 if-guard。 */
    ASSERT(alloc->closed_sb_count < SUPER_BLOCK_LIST_MAX);
    alloc->closed_sb_list[alloc->closed_sb_count++] = *sb;
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
    /* 前置条件（判据 2）：唯一调用者 migrate.c 在找 victim 时已把「不在列表里」换成
     * UINT16_MAX 并拦掉 ⇒ 走到这里的 closed_idx 恒在域内，无需越界早退。 */
    ASSERT(alloc != NULL);
    ASSERT(closed_idx < alloc->closed_sb_count);

    /* 被移除条目的各 block 恢复"未归属"（已由 allocator_free_block 置 FREE + 清 sb_id，
     * 这里兜底再清一次索引，避免残留） */
    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (alloc->closed_sb_list[closed_idx].plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue; /* 退化单 plane 条目 */
        }
        ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, alloc->closed_sb_list[closed_idx].plane_blocks[plane])];

        if (ftl_block_sb_id(b) == closed_idx)
        {
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        }
    }

    /* swap-remove：末位条目搬到被移除位，并修正其各 block 的 sb_id */
    alloc->closed_sb_count--;
    if (closed_idx != alloc->closed_sb_count)
    {
        super_block_t *moved = &alloc->closed_sb_list[closed_idx];
        *moved               = alloc->closed_sb_list[alloc->closed_sb_count];

        for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (moved->plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
            {
                continue; /* 退化单 plane 条目 */
            }
            ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, moved->plane_blocks[plane])];

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
 *   返回：FTL_OK 成功；FTL_ERROR 对应 Open SB 已关闭（幂等早退，见下）。
 *   实现要点：置 CLOSED + 进 closed_sb_list + is_open=false（行业坏块替换流程 ①）。
 * -------------------------------------------------------------------- */
ftl_status_t allocator_close_open_sb(ftl_allocator_t *alloc, uint8_t temperature)
{
    /* 前置条件（判据 2）：唯一调用者 ftl.c 传 &ftl->alloc；
     * temperature 是编译期常量 HOT/COLD，非法值不得静默落到 cold 槽。 */
    ASSERT(alloc != NULL);
    ASSERT(temperature < FTL_TEMP_NUM);

    super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    /* ⚠ **幂等早退，保留返回码**（不转 ASSERT）：对已关闭的槽再关一次是合法 no-op，
     * 不是调用者编程错误。先例见 assertion_guidelines.md §十三 的 fc_command_release。
     * 调用者 ftl.c 在 program 失败路径上调它，同一 Open SB 连续失败两次即走到这里。 */
    if (!sb->is_open)
    {
        return FTL_ERROR; /* 无对应 Open SB（已关闭） */
    }

    /* 关闭：置 CLOSED + 进 closed_sb_list + is_open=false（行业坏块替换流程 ①） */
    allocator_close_sb(alloc, sb);
    return FTL_OK;
}

/* ── 分配 ── */

/* --------------------------------------------------------------------
 * allocator_alloc_pair
 *   功能：分配各 plane 的同页 PBA——推进 Open SB 写指针。
 *   参数：alloc —— 分配器；temperature —— 热/冷流；out[PLANE_NUM] —— 出参各 plane PBA。
 *   返回：FTL_OK；FTL_AGAIN = 某 plane 无块可分配（瞬态资源结果，调用者触发 GC / 背压）。
 *   实现要点：当前 SB 无效或写满 ⇒ 先关闭当前 SB（若有效）再 allocator_open_sb 取新对；
 *             各 plane 取同一页号（multi-plane 一次烧同一页）；最后推进写指针。
 * -------------------------------------------------------------------- */
ftl_status_t allocator_alloc_pair(ftl_allocator_t *alloc, uint8_t temperature, nand_pba_t out[PLANE_NUM])
{
    /* 前置条件（判据 2）：调用者传 &ftl->alloc 与自己的 out 数组（ftl.c 的栈上 new_pba /
     * migrate.c 的 m->dst），结构上恒非空；temperature 是编译期常量 HOT/COLD。
     * 池空返回 FTL_AGAIN 是资源类结果，保留。 */
    ASSERT(alloc != NULL);
    ASSERT(out != NULL);
    ASSERT(temperature < FTL_TEMP_NUM);

    super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    /* 当前 Super Block 无效或已写满 → 关闭并分配新 Super Block */
    if (!sb->is_open || (sb->next_page >= PAGES_PER_BLOCK))
    {
        if (sb->is_open)
        {
            allocator_close_sb(alloc, sb);
        }
        if (!allocator_open_sb(alloc, sb, temperature))
        {
            return FTL_AGAIN; /* Free/OP 池都拿不出某个 plane 的块 → 调用者触发紧急 GC */
        }
    }

    /* 各 plane 同一页：multi-plane 写一次烧同一页 */
    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        out[plane] = super_block_page_pba(allocator_topo(alloc), sb, plane, sb->next_page);
    }

    /* 推进写指针 */
    sb->next_page++;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * allocator_free_block
 *   功能：把 gblock 归还 Free Pool，置 FREE，清反向索引。
 *   参数：alloc —— 分配器；gblock —— 全局块号。
 *   返回：无。
 *   实现要点：**调用者必须先擦除该块**（本函数只还编号）。除 BAD（终态，入池闸门
 *             拒绝）外，互斥枚举一次赋值即覆盖任何前态（无位掩码残位风险）。
 *             已擦除池，无派生计数需同步（三级水位直接建在 free_count 上）。
 * -------------------------------------------------------------------- */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t gblock)
{
    /* 前置条件（判据 2）：调用者传 &ftl->alloc 与自己算出的 gblock。 */
    ASSERT(alloc != NULL);

    /* 四条入池校验（判据 9）：越界会写坏相邻的 free_blocks[]；BAD 是终态，赋 FREE
     * 会复活坏块；state == FREE 表示块已在池里（重复归还）；池满 = 更晚的迹象。
     * 四者后果都是静默漏账/介质破坏 ⇒ debug 用 ASSERT 留现场，release 用 if 拒绝
     * （ASSERT 在 NDEBUG 下裁空）。 */
    ASSERT(gblock < FTL_BLOCK_TOTAL);
    if (gblock >= FTL_BLOCK_TOTAL)
    {
        LOGE("[FTL] free_block: gblock %u out of range\r\n", (unsigned)gblock);
        return;
    }

    ASSERT(!ftl_block_is_bad(&alloc->blocks[gblock]));
    if (ftl_block_is_bad(&alloc->blocks[gblock]))
    {
        LOGE("[FTL] free_block: gblock %u is bad, dropped\r\n", (unsigned)gblock);
        return;
    }

    /* 不变量②：state == FREE ⟺ 在池 ⇒ 入池前已 FREE 就是重复归还。
     * 重复插入会产生重复条目 ⇒ 同一块可能被再次分配（静默介质破坏）。 */
    ASSERT(ftl_block_state(&alloc->blocks[gblock]) != FTL_BLOCK_STATE_FREE);
    if (ftl_block_state(&alloc->blocks[gblock]) == FTL_BLOCK_STATE_FREE)
    {
        LOGE("[FTL] free_block: gblock %u double free, dropped\r\n", (unsigned)gblock);
        return;
    }

    /* Block 擦除由调用者确保在调用本函数前已完成；本函数只归还编号。 */
    ASSERT(alloc->free_count < FTL_BLOCK_TOTAL);
    if (alloc->free_count < FTL_BLOCK_TOTAL)
    {
        alloc->free_blocks[alloc->free_count++] = gblock;
    }
    else
    {
        LOGE("[FTL] free_block: free pool full, gblock %u dropped\r\n", (unsigned)gblock);
        return; /* 不还，块账保持原样（不得再置 FREE） */
    }

    /* 状态机：置 FREE（本入口的契约是「调用者已擦除」⇒ 进池即已擦除态。GC 擦除 /
     * OP 补池 / Spare 补池 / rebuild 拉回全走本入口），清反向索引。
     * 状态是**互斥枚举** ⇒ 一次赋值覆盖任何前态（BAD 已在上方拒绝；LOG/OPEN/CLOSED 都不会残留）。
     * 温度不在此清：它与状态**正交**（见 bst.h 的 ftl_block_set_temp），且每次分配都显式写入。
     * 无派生计数需要同步：Free Pool 是**一个**已擦除池，三级水位直接建在 free_count 上。 */
    ftl_block_t *b = &alloc->blocks[gblock];

    ftl_block_set_state(b, FTL_BLOCK_STATE_FREE);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

/* --------------------------------------------------------------------
 * allocator_alloc_log_block
 *   功能：从指定 plane 的 Free Pool 取一块并置为 LOG（独占，元数据日志块用）。
 *   参数：alloc —— 分配器；plane —— 目标 plane；out_gblock —— 出参 gblock。
 *   返回：FTL_OK；FTL_AGAIN 该 plane Free Pool 空（资源类结果，保留）。
 *   实现要点：两件事必须一起做——① 从 Free Pool 数组拿掉 ② 置 LOG。
 *             只置 LOG 挡不住分配（选块以数组为准），必须先从数组拿掉；
 *             **不向 OP 池借**（OP 是 GC 的紧急缓冲）。
 * -------------------------------------------------------------------- */
ftl_status_t allocator_alloc_log_block(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    /* 前置条件（判据 2）：alloc / plane 由 allocator_select_plane_block_from_pool
     * 统一校验（§二十六：同一入参不逐层复检）；out_gblock 是本层出参，只有本层写它。 */
    ASSERT(out_gblock != NULL);

    uint16_t gblock = allocator_select_plane_block_from_pool(alloc, plane);
    if (gblock == FTL_GBLOCK_INVALID)
    {
        return FTL_AGAIN; /* 该 plane 的 Free Pool 空（不向 OP 池借：OP 是 GC 的紧急缓冲） */
    }

    /* 两件事：① 从 Free Pool 拿掉 ② 置 LOG。
     * 只置 LOG 挡不住分配——选块以 Free Pool 数组为准，不先从数组拿掉
     * 就会被下一次 allocator_select_plane_block_from_pool 再选中。 */
    allocator_remove_from_free(alloc, gblock);

    ftl_block_t *b = &alloc->blocks[gblock];

    ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    *out_gblock = gblock;

    return FTL_OK;
}

/* allocator_free_log_block — 归还独占块；与 allocator_free_block 同一入口语义（调用者保证已擦除） */
void allocator_free_log_block(ftl_allocator_t *alloc, uint16_t gblock)
{
    /* 前置条件（判据 2）：metadata.c 传 &ftl->alloc 与自己持有的 LOG 块号。
     * alloc / gblock 范围的校验统一由 allocator_free_block（唯一漏斗）负责，
     * 本层不重复（§二十六）。调用者保证块已擦除——与漏斗契约一致。 */
    allocator_free_block(alloc, gblock);
}

/* ── 重建入口 ── */

/* --------------------------------------------------------------------
 * allocator_add_closed_sb
 *   功能：开机重建时把一个 Closed SB 条目装进 closed_sb_list（并设块状态/温度/反向索引）。
 *   参数：alloc —— 分配器；plane_blocks[PLANE_NUM] —— 各 plane 内块号（可含 FTL_BLOCK_NO_PLANE）；
 *         temperature —— 温度流。
 *   返回：新条目在列表中的索引（无失败返回——实参是前置条件，非法即 ASSERT）。
 *   实现要点：列表容量 = FTL_BLOCK_TOTAL，而本函数**每写过一个物理块最多建一条** ⇒
 *             满在结构上不可能 ⇒ ASSERT（真满了只可能是块账本已破坏）。
 *             条目 next_page=64（=满）、is_open=false（Closed，gc_select_victim 依赖）。
 * -------------------------------------------------------------------- */
uint16_t allocator_add_closed_sb(ftl_allocator_t *alloc,
                                 const uint16_t   plane_blocks[PLANE_NUM],
                                 uint8_t          temperature)
{
    /* 前置条件（判据 2）：唯一调用者 power_failure_recovery.c 传 &ftl->alloc 与局部数组；
     * temperature 是编译期常量 HOT/COLD。 */
    ASSERT(alloc != NULL);
    ASSERT(plane_blocks != NULL);
    ASSERT(temperature < FTL_TEMP_NUM);

    /* 每条占 1 个块、容量 = 块总数 ⇒ 满不可能（见 allocator.h 的 SUPER_BLOCK_LIST_MAX） */
    ASSERT(alloc->closed_sb_count < SUPER_BLOCK_LIST_MAX);

    /* 组装条目（next_page=64=满；is_open=false=Closed，gc_select_victim 依赖） */
    super_block_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.next_page   = (uint8_t)PAGES_PER_BLOCK;
    sb.temperature = temperature;
    sb.is_open     = false;

    /* 各 plane 置 CLOSED + 温度 + 反向索引 = closed 列表索引。
     * plane_blocks[plane] 允许为 FTL_BLOCK_NO_PLANE（重建产生的退化单 plane 条目）。 */
    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        sb.plane_blocks[plane] = plane_blocks[plane];

        if (plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue;
        }
        ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, plane_blocks[plane])];

        ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
        ftl_block_set_temp(b, temperature);
        ftl_block_set_sb(b, alloc->closed_sb_count);
    }

    alloc->closed_sb_list[alloc->closed_sb_count++] = sb;

    return (uint16_t)(alloc->closed_sb_count - 1U);
}

/* allocator_remove_from_free — 从 Free Pool 移除一个 gblock（swap-remove，O(1)；**出池不改状态**）
 *
 * ⚠ **本函数是 Free Pool 的唯一出池闸门**。出池只是**容器操作**：它**校验**
 *   「在池 ⇒ state == FREE」（allocator.h 不变量②），**不赋新态**——赋态是
 *   **转移的归属者**的事。全部 5 个出池调用点都紧跟着赋新态（open_sb→OPEN /
 *   alloc_log→LOG / migrate 退役→RETIRED / rebuild→CLOSED / bbt_mark_bad→BAD）。
 *
 * ⚠ 块不在池内 ⇒ no-op，是**文档化契约**（判据 4 明文举例），不是调用者错误。 */
void allocator_remove_from_free(ftl_allocator_t *alloc, uint16_t gblock)
{
    /* 前置条件（判据 2）：调用者传 &ftl->alloc 与自己算出的 gblock。 */
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

/* ── 统计访问器 ── */

/* 账本统计的**唯一实现**：GC / WL / 搬移引擎都经这里读，不再各自遍历 blocks[]。
 * 三条口径固定于此（见 allocator.h）：好块、跳过哨兵 plane、valid_count 单位 = CW。 */
void allocator_sb_stats(const ftl_allocator_t *alloc, const super_block_t *sb, allocator_sb_stats_t *out)
{
    /* 前置条件（判据 2）：alloc 由调用者持有实例、sb 是 Closed 条目或其副本、
     * out 是调用者栈上变量 ⇒ 非法即 ASSERT。 */
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
            continue; /* 退化单 plane 条目的哨兵 plane 不可展开 */
        }
        const ftl_block_t *blk = &alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])];

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

        if (ftl_block_is_bad(blk))
        {
            continue; /* 坏块不计入磨损统计 */
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

uint16_t allocator_count_blocks_below(const ftl_allocator_t *alloc, uint32_t threshold)
{
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);

    uint16_t count = 0U;

    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *blk = &alloc->blocks[i];

        if (ftl_block_is_bad(blk))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (blk->erase_count < threshold)
        {
            count++;
        }
    }

    return count;
}

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
