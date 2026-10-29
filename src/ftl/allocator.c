/*
 * allocator.c — 块分配器实现
 *
 * 只做账本，不碰介质：块的擦除由调用者在 alc_free_push 之前完成。
 * 不变量见 allocator.h。
 *
 * 文件结构：
 *   1. 初始化
 *   2. 通用计数
 *   3. Free Pool
 *   4. OP Pool
 *   5. Spare Pool
 *   6. Super Block：Open
 *   7. Super Block：Closed
 *   8. BST
 *   9. Trace
 */

#include "allocator.h"

#include "assertion.h"
#include "log.h" /* LOGE：release 运行期拒绝要留日志 */

#include <string.h>

/**
 * @brief 内部前向声明：Closed 提交在 Open 分配之后定义。
 */
static WORD alc_super_close_live(alc_t *alloc, super_block_t *sb);

/* ── 1. 初始化 ── */

// 实现要点：只绑指针与清计数；不置 FREE（FREE ⟺ 在池，池由恢复路径回填）。
// refs 指向的五张表由 HIL 独立定义；其 state/temp/valid 依赖显式清零，
// 调用者不得传入脏存储（ftl_init 逐表 memset）。
void alc_init(alc_t *alloc, ftl_topo_t topo, const ftl_table_refs_t *refs)
{
    ASSERT(alloc != NULL);
    ASSERT(refs != NULL);

    alloc->ch           = topo.ch;
    alloc->ce           = topo.ce;
    alloc->lun          = topo.lun;
    alloc->total_blocks = 0U; /* 恢复路径回填 */

    alloc->free_blocks = refs->free_blocks;
    alloc->free_count  = 0U;

    alloc->op_blocks    = refs->op_blocks;
    alloc->spare_blocks = refs->spare_blocks;
    memset(alloc->op_count, 0, sizeof(alloc->op_count));
    memset(alloc->spare_count, 0, sizeof(alloc->spare_count));

    memset(&alloc->hot_sb, 0, sizeof(alloc->hot_sb));
    memset(&alloc->cold_sb, 0, sizeof(alloc->cold_sb));
    alloc->hot_sb.temperature  = FTL_TEMP_HOT;
    alloc->cold_sb.temperature = FTL_TEMP_COLD;

    alloc->closed_sb_list  = refs->closed_sb_list;
    alloc->closed_sb_count = 0U;

    alloc->blocks    = refs->blocks;
    alloc->bad_count = 0U;

    memset(&alloc->bbt, 0, sizeof(alloc->bbt));
    memset(alloc->trace, 0, sizeof(alloc->trace));
    alloc->trace_seq = 0U;

    // 反向索引：未坏块置 NO_SB；坏块由 bbt_init 置 BAD，不在此动。
    for (WORD i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        if (!ftl_block_is_bad(&alloc->blocks[i]))
        {
            ftl_block_set_sb(&alloc->blocks[i], FTL_BLOCK_NO_SB);
        }
    }
}

/* ── 2. 通用计数 ── */

// 同一类 per-plane 计数的求和；元素宽度由 WORD * 类型确定。
WORD alc_pool_sum_counts(const WORD *counts, BYTE count)
{
    WORD n = 0U;

    for (BYTE p = 0U; p < count; p++)
    {
        n = (WORD)(n + counts[p]);
    }
    return n;
}

/* ── 3. Free Pool ── */

// 实现要点：调用者保证块已擦除。入池前拒绝越界、BAD、重复归还、池满；
// debug ASSERT + release 拒绝（后果是静默漏账或坏块复活）。
void alc_free_push(alc_t *alloc, WORD gblock)
{
    ASSERT(gblock < FTL_BLOCK_TOTAL);
    if (gblock >= FTL_BLOCK_TOTAL)
    {
        LOGE(LOG_TAG_FTL "free_block: gblock %hu out of range\r\n", gblock);
        return;
    }

    // BAD 是终态，入池会把它改回 FREE。
    ASSERT(!ftl_block_is_bad(&alloc->blocks[gblock]));
    if (ftl_block_is_bad(&alloc->blocks[gblock]))
    {
        LOGE(LOG_TAG_FTL "free_block: gblock %hu is bad, dropped\r\n", gblock);
        return;
    }

    // state == FREE ⟺ 在池 ⇒ 已 FREE 即重复归还。
    ASSERT(ftl_block_state(&alloc->blocks[gblock]) != FTL_BLOCK_STATE_FREE);
    if (ftl_block_state(&alloc->blocks[gblock]) == FTL_BLOCK_STATE_FREE)
    {
        LOGE(LOG_TAG_FTL "free_block: gblock %hu double free, dropped\r\n", gblock);
        return;
    }

    // 池满只是重复条目积累后的晚期症状；这里兜住前两道检查漏掉的坏账。
    ASSERT(alloc->free_count < FTL_BLOCK_TOTAL);
    if (alloc->free_count >= FTL_BLOCK_TOTAL)
    {
        LOGE(LOG_TAG_FTL "free_block: free pool full, gblock %hu dropped\r\n", gblock);
        return;
    }

    alloc->free_blocks[alloc->free_count++] = gblock;

    bst_block_t *b = &alloc->blocks[gblock];

    ftl_block_set_state(b, FTL_BLOCK_STATE_FREE);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

// 实现要点：线性查找 + swap-remove（查找 O(free_count)，交换 O(1)）。
// 不在池内是文档化 no-op；出池只校验 state == FREE，不赋新态。
void alc_free_remove(alc_t *alloc, WORD gblock)
{
    for (WORD i = 0U; i < alloc->free_count; i++)
    {
        if (alloc->free_blocks[i] == gblock)
        {
            ASSERT(ftl_block_state(&alloc->blocks[gblock]) == FTL_BLOCK_STATE_FREE);
            alloc->free_blocks[i] = alloc->free_blocks[--alloc->free_count];
            return;
        }
    }
}

// 实现要点：LIFO 弹出；跳过运行时被标坏的块，坏块弹出即丢弃、不回 Free Pool。
WORD alc_free_refill_from_op(alc_t *alloc, BYTE plane)
{
    WORD gblock;

    while (alloc->op_count[plane] != 0U)
    {
        alloc->op_count[plane]--;
        gblock = alloc->op_blocks[plane][alloc->op_count[plane]];

        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            continue;
        }

        alc_free_push(alloc, gblock);
        return gblock;
    }
    return FTL_GBLOCK_INVALID;
}

// Free Pool 中「各 plane 最大擦除次数」的最小值（UBI 的 e2，按 plane 取限制侧）。
// 目的页对每 plane 各取一块，目标块的擦除次数受最紧的那个 plane 限制；取全局最大值
// 会高估，可能让一个 plane 的冷数据落到并不更旧的块上。任一 plane 无空闲块 ⇒ 0。
DWORD alc_free_get_max_erase(const alc_t *alloc)
{
    DWORD limit = UINT32_MAX;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        DWORD plane_max = 0U;
        bool  any       = false;

        for (WORD i = 0U; i < alloc->free_count; i++)
        {
            const WORD         gblock = alloc->free_blocks[i];
            const bst_block_t *blk    = &alloc->blocks[gblock];

            if ((ftl_gblock_plane(gblock) != plane) || ftl_block_is_bad(blk))
            {
                continue;
            }

            any = true;
            if (blk->erase_count > plane_max)
            {
                plane_max = blk->erase_count;
            }
        }

        if (!any)
        {
            return 0U;
        }
        if (plane_max < limit)
        {
            limit = plane_max;
        }
    }

    return (limit == UINT32_MAX) ? 0U : limit;
}

/**
 * @brief 从 Free Pool 选该 plane 擦除最少的块
 *
 * @return gblock 或 FTL_GBLOCK_INVALID。
 *
 * @note 实现要点：判据是 Free Pool 数组成员关系，不是 BST 的 FREE 位；
 * 选中 ⇒ alc_free_remove 必然成功。
 *
 * @note 参数：alloc — 分配器；plane — 目标 plane。
 */
static WORD alc_free_find_block(const alc_t *alloc, BYTE plane, alc_super_alloc_mode_e mode)
{
    const bool high_wear = (mode == ALC_SUPER_ALLOC_COLD_HIGH_WEAR);
    WORD       best      = FTL_GBLOCK_INVALID;
    DWORD      key       = high_wear ? 0U : UINT32_MAX; /* high_wear：取最大擦除；否则取最小（动态 WL） */

    for (WORD i = 0U; i < alloc->free_count; i++)
    {
        WORD gblock = alloc->free_blocks[i];

        if (ftl_gblock_plane(gblock) != plane)
        {
            continue;
        }

        const bst_block_t *blk = &alloc->blocks[gblock];

        if (ftl_block_is_bad(blk))
        {
            continue;
        }

        if ((best == FTL_GBLOCK_INVALID) || (high_wear ? (blk->erase_count > key) : (blk->erase_count < key)))
        {
            key  = blk->erase_count;
            best = gblock;
        }
    }

    return best;
}

/* ── 4. OP Pool ── */

// 实现要点：按 plane 追加到 op_blocks；容量由 FTL_OP_BLOCKS_PER_PLANE 限定。
bool alc_op_push(alc_t *alloc, BYTE plane, WORD gblock)
{
    if (alloc->op_count[plane] >= FTL_OP_BLOCKS_PER_PLANE)
    {
        return false;
    }

    alloc->op_blocks[plane][alloc->op_count[plane]] = gblock;
    alloc->op_count[plane]++;
    return true;
}

// 调用者保证：plane < PLANE_NUM、index < op_count[plane]（checkpoint 遍历循环）。
// （偏离 H14：现有唯一调用链已保证。）
WORD alc_op_get_at(const alc_t *alloc, BYTE plane, WORD index)
{
    return alloc->op_blocks[plane][index];
}

// 从 **OP 池**借一块作为动态 L2P 日志块（固定日志区满时的有界溢出）。
// 为什么从 OP 而不是用户 Free Pool：OP 是容量公式之外的 GC headroom，借还都在 OP
// 内闭环，不动用户可用容量；固定区满时正是写放大高、OP 最该被借用的场景。
// 只取同 plane；成功后该块先从 OP/Free Pool 摘除再置 LOG。
WORD alc_op_alloc_log_block(alc_t *alloc, BYTE plane)
{
    ASSERT(alloc != NULL);
    ASSERT(plane < PLANE_NUM);

    // alc_free_refill_from_op 弹出后会把块放进 Free Pool（state FREE）；这里立即再摘出来。
    WORD gblock = alc_free_refill_from_op(alloc, plane);

    if (gblock == FTL_GBLOCK_INVALID)
    {
        return FTL_GBLOCK_INVALID;
    }

    alc_free_remove(alloc, gblock);

    bst_block_t *b = &alloc->blocks[gblock];

    ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
    ftl_block_clear_valid(b);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    return gblock;
}

/* ── 5. Spare Pool ── */

// 实现要点：按 plane 追加到 spare_blocks；容量由 FTL_SPARE_POOL_PER_PLANE 限定。
bool alc_spare_push(alc_t *alloc, BYTE plane, WORD gblock)
{
    if (alloc->spare_count[plane] >= FTL_SPARE_POOL_PER_PLANE)
    {
        return false;
    }

    alloc->spare_blocks[plane][alloc->spare_count[plane]] = gblock;
    alloc->spare_count[plane]++;
    return true;
}

// 实现要点：LIFO 弹出；调用者保证 out_gblock 非空。
bool alc_spare_pop(alc_t *alloc, BYTE plane, WORD *out_gblock)
{
    if (alloc->spare_count[plane] == 0U)
    {
        return false;
    }

    alloc->spare_count[plane]--;
    *out_gblock = alloc->spare_blocks[plane][alloc->spare_count[plane]];
    return true;
}

// 实现要点：逐 plane 从 Free Pool 摘块补 Spare；只补 state == FREE 的块，
// 摘出后置 NONE（Spare 池块无归属），push 失败则回滚到 Free。
bool alc_spare_refill_from_free(alc_t *alloc, BYTE plane)
{
    bool moved = false;

    while (alloc->spare_count[plane] < FTL_SPARE_POOL_PER_PLANE)
    {
        WORD pick = UINT16_MAX;

        for (WORD i = 0U; i < alloc->free_count; i++)
        {
            const WORD g = alloc->free_blocks[i];

            if (ftl_gblock_plane(g) == plane)
            {
                pick = g;
                break;
            }
        }

        if (pick == UINT16_MAX)
        {
            break; /* 该 plane 没有可补的空闲块 */
        }

        alc_free_remove(alloc, pick);
        ftl_block_set_state(&alloc->blocks[pick], FTL_BLOCK_STATE_NONE);

        if (!alc_spare_push(alloc, plane, pick))
        {
            alc_free_push(alloc, pick); /* 回滚（目标未满时不应发生） */
            break;
        }
        moved = true;
    }

    return moved;
}

// 调用者保证：plane < PLANE_NUM、index < spare_count[plane]。
// （偏离 H14：现有唯一调用链已保证。）
WORD alc_spare_get_at(const alc_t *alloc, BYTE plane, WORD index)
{
    return alloc->spare_blocks[plane][index];
}

/* ── 6. Super Block：Open ── */

/**
 * @brief 为 Open SB 分配各 plane 的物理块
 *
 * @return true 成功；false 某 plane 无块。
 *
 * @note 实现要点：选块阶段无副作用；全部就位后才出池并置 OPEN，失败无需回滚。
 *
 * @note 参数：alloc — 分配器；sb — 目标槽；mode — 分配策略。
 */
static bool alc_super_open(alc_t *alloc, super_block_t *sb, alc_super_alloc_mode_e mode)
{
    const BYTE temperature = (mode == ALC_SUPER_ALLOC_HOT) ? FTL_TEMP_HOT : FTL_TEMP_COLD;
    const bool high_wear   = (mode == ALC_SUPER_ALLOC_COLD_HIGH_WEAR);
    const bool allow_op    = !high_wear;
    WORD       gblock[PLANE_NUM];

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        gblock[plane] = alc_free_find_block(alloc, plane, mode);
        if ((gblock[plane] == FTL_GBLOCK_INVALID) && allow_op)
        {
            gblock[plane] = alc_free_refill_from_op(alloc, plane);
        }
        if (gblock[plane] == FTL_GBLOCK_INVALID)
        {
            return false; /* high_wear 路径不允许 OP 兜底：OP 块的擦除次数不受本策略控制 */
        }
    }

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        // 先出池（校验 state == FREE），再由本转移赋 OPEN。
        alc_free_remove(alloc, gblock[plane]);
        sb->plane_blocks[plane] = ftl_gblock_block(gblock[plane]);

        bst_block_t *b = &alloc->blocks[gblock[plane]];

        ftl_block_set_state(b, FTL_BLOCK_STATE_OPEN);
        ftl_block_set_temp(b, temperature);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
    }

    sb->next_page        = 0U;
    sb->temperature      = temperature;
    sb->is_open          = true;
    sb->page0_programmed = 0U;

    return true;
}

// 实现要点：SB 未开、写满或含已标坏块时先换新 SB；各 plane 取同一页。
// `ALC_SUPER_ALLOC_COLD_HIGH_WEAR` 只影响**新开** SB 的选块方向：每 plane 取擦除最多的
// 空闲块（UBI 模型：冷数据搬到已磨损块）。其余模式保持动态 WL：取擦除最少。
// ⚠ 复用已打开 SB 时不换块——同一 job 内要连续写页；启动前用
// `alc_super_prepare_wl_destination` 摘掉 host 的 cold SB，保证第一次分配是新开。
ftl_status_e alc_super_open_alloc_pair(alc_t *alloc, alc_super_alloc_mode_e mode, DWORD out[PLANE_NUM])
{
    super_block_t *sb = (mode == ALC_SUPER_ALLOC_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    // 读恢复 / 擦除失败可能把 Open SB 内的块标 BAD；BAD 终态不能再写。
    if (sb->is_open)
    {
        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (!super_block_has_plane(sb, plane))
            {
                continue;
            }
            if (ftl_block_is_bad(&alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])]))
            {
                if (alc_super_close_live(alloc, sb) == FTL_BLOCK_NO_SB)
                {
                    return FTL_ERROR;
                }
                break;
            }
        }
    }

    if (!sb->is_open || (sb->next_page >= PAGES_PER_BLOCK))
    {
        if (sb->is_open)
        {
            if (alc_super_close_live(alloc, sb) == FTL_BLOCK_NO_SB)
            {
                return FTL_ERROR;
            }
        }
        // 策略只影响**新开** SB 的选块方向；已打开的 SB 继续追加（同一 job 内
        // 连续写页）。高磨损模式不允许 OP 兜底：OP 块擦除次数不受该策略控制。
        if (!alc_super_open(alloc, sb, mode))
        {
            return FTL_AGAIN;
        }
    }

    // 注：page0 异步写屏障在**写提交点**检查（alc_super_open_get_pba_state），不在分配点。
    // 分配本身不产生介质写；稀疏 plane 可能永远不写 page0，在分配点拒绝会饿死
    // 另一 plane 的正常追加。

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        out[plane] = super_block_page_pba(alc_topo(alloc), sb, plane, sb->next_page);
    }

    sb->next_page++;
    return FTL_OK;
}

BYTE alc_super_open_get_room(const alc_t *alloc, BYTE temperature)
{
    const super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if ((!sb->is_open) || (sb->next_page >= PAGES_PER_BLOCK))
    {
        return 0U;
    }

    BYTE room = (BYTE)(PAGES_PER_BLOCK - sb->next_page);

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (!super_block_has_plane(sb, plane))
        {
            return 0U;
        }
        // page0 屏障：未成功编程的 plane 不允许继续分配。
        if ((sb->page0_programmed & (BYTE)(1U << plane)) == 0U)
        {
            return 0U;
        }
        // BAD 块不能继续写；alloc 主路径会关闭该 SB，这里同样按 0 处理。
        if (ftl_block_is_bad(&alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])]))
        {
            return 0U;
        }
    }

    return room;
}

alc_super_pba_state_e alc_super_open_get_pba_state(const alc_t *alloc, BYTE temperature, DWORD pba)
{
    const super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if ((!sb->is_open) || (pba == NAND_PBA_INVALID))
    {
        return ALC_SUPER_PBA_NOT_OPEN;
    }

    const BYTE plane = pba_to_plane(pba);

    if ((!super_block_has_plane(sb, plane)) || (sb->plane_blocks[plane] != pba_to_block(pba)))
    {
        return ALC_SUPER_PBA_NOT_OPEN; /* 旧 SB 的迟到命令 */
    }

    return ((sb->page0_programmed & (BYTE)(1U << plane)) != 0U) ? ALC_SUPER_PBA_OPEN_PAGE0_DONE : ALC_SUPER_PBA_OPEN_PAGE0_PENDING;
}

void alc_super_open_note_page_programmed(alc_t *alloc, BYTE temperature, DWORD pba)
{
    super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if ((!sb->is_open) || (pba == NAND_PBA_INVALID) || (pba_to_page(pba) != 0U))
    {
        return;
    }

    // 迟到的完成可能属于已关闭的旧 SB：必须按 plane+block 确认它仍指向当前 Open SB，
    // 否则会把新 SB 的 page0 屏障误置为已通过。
    const BYTE plane = pba_to_plane(pba);

    if ((!super_block_has_plane(sb, plane)) || (sb->plane_blocks[plane] != pba_to_block(pba)))
    {
        return;
    }

    sb->page0_programmed |= (BYTE)(1U << plane);
}

// 实现要点：只回退写指针——**不**回退可能刚发生的开新 SB（见头文件的两条前提）。
// 归还后若 SB 恰是刚开的，next_page == 0 是合法态（等价于「刚开、未写」）。
// 调用者保证：紧邻一次成功的 alc_super_open_alloc_pair，且期间没有其他页对分配/关闭 SB。
// （偏离 H14：这两条前提由现有调用链保证；新增调用点时必须重新评估。）
void alc_super_open_rewind_pair(alc_t *alloc, BYTE temperature)
{
    super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    sb->next_page--;
}

// 静态 WL 启动前把 host 当前的 cold SB 摘掉，保证第一页会**新开**一个
// high-wear SB（否则会续写动态 WL 选出的低擦除块）。
// · 已写过（next_page > 0）⇒ 正常关闭，进 Closed 列表；
// · 未写过（next_page == 0）⇒ 把块归还 Free Pool，不产生空 Closed 条目。
void alc_super_prepare_wl_destination(alc_t *alloc)
{
    ASSERT(alloc != NULL);

    super_block_t *sb = &alloc->cold_sb;

    if (!sb->is_open)
    {
        return;
    }

    if (sb->next_page == 0U)
    {
        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (super_block_has_plane(sb, plane))
            {
                alc_free_push(alloc, ftl_gblock(plane, sb->plane_blocks[plane]));
            }
        }
        sb->is_open = false;
        return;
    }

    // 返回 void 的既有接口：列表满属结构异常，debug 由 alc_super_close_live 的 ASSERT 抓；
    // release 下拒绝写入并打 LOG。此处没有可上抛的返回码。
    (void)alc_super_close_live(alloc, sb);
}

/* ── 7. Super Block：Closed ── */

/**
 * @brief Open → Closed：唯一 live setter + 列表追加
 *
 * @return 新列表索引；FTL_BLOCK_NO_SB = 列表满，条目被拒绝。
 *
 * @note 实现要点：live 标志与列表副本必须在同一函数内完成——只改副本会让 Open SB
 * 继续被分配、或被重复关闭（RISK-ALLOC-01）。调用者必须检查失败码；列表满
 * 属结构异常，不能静默吞掉。
 *
 * @note 参数：alloc — 分配器；sb — 待关闭 SB（live 或重建时的临时条目）。
 */
static WORD alc_super_close_live(alc_t *alloc, super_block_t *sb)
{
    ASSERT(alloc->closed_sb_count < SUPER_BLOCK_LIST_MAX);

    // release 兜底：结构上不应满；真满时拒绝写入，避免越界。
    // 此时不改变 live is_open：让调用者看到失败并保留原状态，避免“已关闭但
    // 未进列表”的不可恢复漏账。
    if (alloc->closed_sb_count >= SUPER_BLOCK_LIST_MAX)
    {
        LOGE(LOG_TAG_FTL "closed_sb_list full, entry dropped\r\n");
        return FTL_BLOCK_NO_SB;
    }

    sb->is_open = false;

    const WORD    closed_idx = alloc->closed_sb_count;
    super_block_t entry      = *sb;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (!super_block_has_plane(&entry, plane))
        {
            continue;
        }

        // release 兜底：非法块号按哨兵处理，避免索引越界。
        ASSERT(entry.plane_blocks[plane] < BLOCKS_PER_PLANE);
        if (entry.plane_blocks[plane] >= BLOCKS_PER_PLANE)
        {
            LOGE(LOG_TAG_FTL "closed_sb: plane%hhu block %hu out of range\r\n", plane, entry.plane_blocks[plane]);
            entry.plane_blocks[plane] = FTL_BLOCK_NO_PLANE;
            continue;
        }

        bst_block_t *b = &alloc->blocks[ftl_gblock(plane, entry.plane_blocks[plane])];

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

// 实现要点：已关闭的槽再关一次是合法 no-op ⇒ 返回 FTL_ERROR，不转 ASSERT。
ftl_status_e alc_super_close(alc_t *alloc, BYTE temperature)
{
    super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if (!sb->is_open)
    {
        return FTL_ERROR;
    }

    return (alc_super_close_live(alloc, sb) == FTL_BLOCK_NO_SB) ? FTL_ERROR : FTL_OK;
}

// 实现要点：比较全部 plane——不同 SB 的哨兵 plane 可能相同，只比一个会误中。
bool alc_super_closed_find(const alc_t *alloc, const super_block_t *sb, WORD *out_idx)
{
    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_t *e    = &alloc->closed_sb_list[i];
        bool                 same = true;

        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
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

// 实现要点：重建按单个物理块建条目，另一 plane 记哨兵；next_page 记满。
WORD alc_super_closed_add(alc_t *alloc, const WORD plane_blocks[PLANE_NUM], BYTE temperature)
{
    super_block_t sb;

    memset(&sb, 0, sizeof(sb));
    sb.next_page   = PAGES_PER_BLOCK;
    sb.temperature = temperature;
    sb.is_open     = false;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        sb.plane_blocks[plane] = plane_blocks[plane];
    }

    return alc_super_close_live(alloc, &sb);
}

// 实现要点：swap-remove；末位条目搬家后修正其各 plane 的 sb_id。
// 首循环兜住“free/retire 被拒绝、sb_id 未清”的路径。
void alc_super_closed_remove(alc_t *alloc, WORD closed_idx)
{
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (alloc->closed_sb_list[closed_idx].plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue;
        }

        bst_block_t *b = &alloc->blocks[ftl_gblock(plane, alloc->closed_sb_list[closed_idx].plane_blocks[plane])];

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

        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (moved->plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
            {
                continue;
            }

            bst_block_t *b = &alloc->blocks[ftl_gblock(plane, moved->plane_blocks[plane])];

            if (ftl_block_sb_id(b) == alloc->closed_sb_count)
            {
                ftl_block_set_sb(b, closed_idx);
            }
        }
    }
}

// 实现要点：GC/WL/predict 逐 plane 擦除 victim；若擦完一个 plane 后 job 中止，
// 旧条目仍引用已归还 Free Pool 的块。该块被重新分配并再次 Closed 后，
// 旧条目会把它误认成原 SB 的一员（RISK-GC-04）。擦完一个 plane 就把它
// 从条目摘除，条目只在仍有未处理 plane 时存在。
// ⚠ 擦除成功路径已由 alc_free_push/retire 改过 BST 状态；本函数只
// 负责 Closed 条目与 sb_id 的同步，不碰池。
bool alc_super_closed_drop_plane(alc_t *alloc, BYTE plane, WORD block)
{
    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        super_block_t *e = &alloc->closed_sb_list[i];

        if (e->plane_blocks[plane] != block)
        {
            continue;
        }

        bst_block_t *b = &alloc->blocks[ftl_gblock(plane, block)];

        if (ftl_block_sb_id(b) == i)
        {
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        }
        e->plane_blocks[plane] = FTL_BLOCK_NO_PLANE;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (e->plane_blocks[p] != FTL_BLOCK_NO_PLANE)
            {
                return false;
            }
        }

        alc_super_closed_remove(alloc, i);
        return true;
    }

    return false;
}

// 实现要点：跳过哨兵与 BAD plane；valid_count 单位是 CW。
void alc_super_get_stats(const alc_t *alloc, const super_block_t *sb, alc_super_stats_t *out)
{
    WORD  valid     = 0U;
    DWORD erase_max = 0U;
    DWORD erase_sum = 0U;
    DWORD pe_min    = UINT32_MAX;
    DWORD read_sum  = 0U;
    BYTE  n         = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (!super_block_has_plane(sb, plane))
        {
            continue;
        }

        const bst_block_t *blk = &alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])];

        if (ftl_block_is_bad(blk))
        {
            continue;
        }

        valid = (valid + blk->valid_count);
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

/* ── 8. BST ── */

// 实现要点：与 migrate 擦除完成同口径；供元数据日志块等不经 migrate 的擦除路径调用。
void alc_bst_note_erase(alc_t *alloc, WORD gblock, DWORD now)
{
    ASSERT(!ftl_block_is_bad(&alloc->blocks[gblock]));

    bst_block_t *b = &alloc->blocks[gblock];

    b->erase_count++;
    b->last_pe_ts = now;
    ftl_block_clear_valid(b);

    // read disturb 是「自上次擦除以来」的计数：擦除把它清零。
    // rr.health / last_ecc / rising_count **保留**——慢死趋势要跨擦除周期观察。
    b->read_count = 0U;
}

// 实现要点：一次扫描；跳过 BAD 与 NONE（固定元数据 / OP / Spare / 未初始化）。
void alc_bst_get_erase_stats(const alc_t *alloc, alc_bst_erase_stats_t *out)
{
    DWORD min_ec = UINT32_MAX;
    DWORD max_ec = 0U;
    DWORD sum    = 0U;
    DWORD max_t  = 0U;
    WORD  n      = 0U;

    for (WORD i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const bst_block_t *blk = &alloc->blocks[i];

        // 跳过 BAD 与 NONE：NONE 的 erase_count 不随擦除累加，计入会把 erase_min
        // 永久钉在 0。
        if (ftl_block_is_bad(blk) || (ftl_block_state(blk) == FTL_BLOCK_STATE_NONE))
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

void alc_bst_get_wear(const alc_t *alloc, WORD gblock, bst_wear_t *out)
{
    // out 由唯一调用者 checkpoint.c 传本地快照地址；模块内部不重复守卫。
    const bst_block_t *blk = &alloc->blocks[gblock];

    out->erase_count = blk->erase_count;
    out->read_count  = blk->read_count;
    out->rr          = blk->rr;
}

void alc_bst_set_wear(alc_t *alloc, WORD gblock, const bst_wear_t *wear)
{
    // wear 由唯一调用者 checkpoint.c 传本地快照地址；模块内部不重复守卫。
    bst_block_t *blk = &alloc->blocks[gblock];

    blk->erase_count = wear->erase_count;
    blk->read_count  = wear->read_count;
    blk->rr          = wear->rr;
}

/* ── 9. Trace ── */

void alc_trace_record(alc_t *alloc, DWORD lba, DWORD pba, BYTE kind)
{
    if ((alloc == NULL) || (!pba_is_valid(pba)))
    {
        return;
    }

    alc_trace_t *t = &alloc->trace[alloc->trace_seq % ALC_TRACE_DEPTH];

    t->seq   = alloc->trace_seq++;
    t->lba   = lba;
    t->pba   = pba;
    t->plane = pba_to_plane(pba);
    t->block = pba_to_block(pba);
    t->page  = pba_to_page(pba);
    t->cw    = pba_to_cw(pba);
    t->kind  = kind;
}

void alc_trace_dump(const alc_t *alloc, DWORD lba, DWORD pba)
{
    if (alloc == NULL)
    {
        return;
    }

    const DWORD count = (alloc->trace_seq < ALC_TRACE_DEPTH) ? alloc->trace_seq : ALC_TRACE_DEPTH;
    const DWORD start = (alloc->trace_seq < ALC_TRACE_DEPTH) ? 0U : (alloc->trace_seq % ALC_TRACE_DEPTH);

    DWORD matches = 0U;
    DWORD dups    = 0U;
    DWORD seen[8];
    BYTE  seen_n = 0U;

    for (DWORD i = 0U; i < count; i++)
    {
        const alc_trace_t *t = &alloc->trace[(start + i) % ALC_TRACE_DEPTH];

        if ((t->pba != pba) && (t->lba != lba))
        {
            continue;
        }

        matches++;
        for (BYTE k = 0U; k < seen_n; k++)
        {
            if (seen[k] == t->pba)
            {
                dups++;
                break;
            }
        }
        if (seen_n < 8U)
        {
            seen[seen_n++] = t->pba;
        }
    }

    LOGW(LOG_TAG_TRC "fail lba=%lu pba=0x%08lx match=%lu dup=%lu\r\n", lba, pba, matches, dups);

    DWORD shown = 0U;

    for (DWORD i = 0U; (i < count) && (shown < 8U); i++)
    {
        const alc_trace_t *t = &alloc->trace[(start + i) % ALC_TRACE_DEPTH];

        if ((t->pba != pba) && (t->lba != lba))
        {
            continue;
        }

        LOGW(LOG_TAG_TRC "seq=%lu lba=%lu pba=0x%08lx pl=%hhu bk=%hu pg=%hhu cw=%hhu %s\r\n", t->seq, t->lba, t->pba, t->plane, t->block, t->page, t->cw, (t->kind == 0U) ? "write-ok" : ((t->kind == 1U) ? "program-fail" : "gc-write"));
        shown++;
    }
}
