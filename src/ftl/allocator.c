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

// 前向声明：Closed 提交在 Open 分配之后定义；契约写在定义处。
static WORD alc_super_close_live(alc_s *alloc, super_block_s *sb);

/* ── 1. 初始化 ── */

/**
 * @brief 绑定表指针并清计数
 *
 * 不置 FREE（FREE ⟺ 在池，池由恢复路径回填）；调用者必须传入已清零的表存储。
 *
 * @param alloc 分配器
 * @param topo  本实例拓扑
 * @param refs  五张表的引用
 */
void alc_init(alc_s *alloc, ftl_topo_s topo, const ftl_table_refs_s *refs)
{
    ASSERT(alloc != NULL);
    ASSERT(refs != NULL);

    alloc->ch           = topo.ch;
    alloc->ce           = topo.ce;
    alloc->lun          = topo.lun;
    alloc->total_blocks = 0U; /* 恢复路径回填 */

    alloc->free_blocks = refs->free_blocks;
    alloc->free_count  = 0U;
    memset(alloc->free_plane_count, 0, sizeof(alloc->free_plane_count));

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
        if (ftl_block_is_bad(&alloc->blocks[i]) == false)
        {
            ftl_block_set_sb(&alloc->blocks[i], FTL_BLOCK_NO_SB);
        }
    }
}

/* ── 2. 通用计数 ── */

/**
 * @brief 同一类 per-plane 计数的求和
 *
 * @param counts per-plane 计数数组
 * @param count  元素个数
 * @return 总和
 */
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

/**
 * @brief 归还一个已擦除块到 Free Pool（唯一入池点）
 *
 * 入池前拒绝越界、BAD、重复归还、池满；debug ASSERT + release 拒绝。
 *
 * @param alloc  分配器
 * @param gblock 全局块号
 */
void alc_free_push(alc_s *alloc, WORD gblock)
{
    ASSERT(gblock < FTL_BLOCK_TOTAL);
    if (gblock >= FTL_BLOCK_TOTAL)
    {
        LOGE(LOG_TAG_FTL "free_block: gblock %hu out of range\r\n", gblock);
        return;
    }

    // BAD 是终态，入池会把它改回 FREE。
    ASSERT(ftl_block_is_bad(&alloc->blocks[gblock]) == false);
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

    // per-plane 账本与平铺池同步维护（唯一入池点）。
    {
        const BYTE plane = ftl_gblock_plane(gblock);

        ASSERT(plane < PLANE_NUM);
        if (plane < PLANE_NUM)
        {
            alloc->free_plane_count[plane]++;
        }
    }

    bst_block_s *b = &alloc->blocks[gblock];

    // 「Free = 已擦除」：擦除成功必经 alc_bst_note_erase 清 valid_count（坏块替换/
    // 恢复建池的块本就为 0）。漏掉记账的调用者不会报错，只会静默污染 GC/WL 的
    // 有效率 ⇒ 在这里抓（H14 内部前置条件）。
    ASSERT(b->valid_count == 0U);

    ftl_block_set_state(b, FTL_BLOCK_STATE_FREE);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

/**
 * @brief 从 Free Pool 移除一个块（唯一出池点，swap-remove）
 *
 * 出池即置 NONE（「FREE ⟺ 在池」由本闸门单点维护），随后由调用者赋目标态
 * （OPEN / LOG / CLOSED / BAD）。不在池内是文档化 no-op，状态不变。
 *
 * @param alloc  分配器
 * @param gblock 全局块号
 */
void alc_free_remove(alc_s *alloc, WORD gblock)
{
    ASSERT(gblock < FTL_BLOCK_TOTAL);

    for (WORD i = 0U; i < alloc->free_count; i++)
    {
        if (alloc->free_blocks[i] == gblock)
        {
            ASSERT(ftl_block_state(&alloc->blocks[gblock]) == FTL_BLOCK_STATE_FREE);
            // per-plane 账本与平铺池同步维护（唯一出池点）。
            {
                const BYTE plane = ftl_gblock_plane(gblock);

                ASSERT(plane < PLANE_NUM);
                ASSERT(alloc->free_plane_count[plane] > 0U);
                if ((plane < PLANE_NUM) && (alloc->free_plane_count[plane] > 0U))
                {
                    alloc->free_plane_count[plane]--;
                }
            }
            alloc->free_blocks[i] = alloc->free_blocks[--alloc->free_count];

            // 出池即脱离 FREE：目标状态由调用者在返回后赋值。若在这里保持 FREE，
            // 「FREE ⟺ 在池」在两次调用之间不成立，漏赋态的块会永久漏账。
            ftl_block_set_state(&alloc->blocks[gblock], FTL_BLOCK_STATE_NONE);
            return;
        }
    }

    // 未命中：不在池的块不可能是 FREE，否则池账与 BST 已分叉。
    ASSERT(ftl_block_state(&alloc->blocks[gblock]) != FTL_BLOCK_STATE_FREE);
}

/**
 * @brief 从 OP 池 LIFO 取一块补进 Free Pool
 *
 * 跳过运行时被标坏的块（弹出即丢弃，不回 Free Pool）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 补入的 gblock；FTL_GBLOCK_INVALID = OP 池无可用块
 */
WORD alc_free_refill_from_op(alc_s *alloc, BYTE plane)
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

/**
 * @brief Free Pool 中「各 plane 最大擦除次数」的最小值（UBI 的 e2）
 *
 * 目的页对每 plane 各取一块，目标受最紧的 plane 限制；任一 plane 无空闲块 ⇒ 0。
 *
 * @param alloc 分配器
 * @return 擦除次数上界；0 = 某 plane 无空闲块
 */
DWORD alc_free_get_max_erase(const alc_s *alloc)
{
    DWORD limit = UINT32_MAX;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        DWORD plane_max = 0U;
        BOOL  any       = false;

        for (WORD i = 0U; i < alloc->free_count; i++)
        {
            const WORD         gblock = alloc->free_blocks[i];
            const bst_block_s *blk    = &alloc->blocks[gblock];

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

        if (any == false)
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
 * @brief 从 Free Pool 为指定 plane 选一个块
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @param mode  分配策略（决定取擦除最少还是最多）
 *
 * @return 选中的 gblock
 *         FTL_GBLOCK_INVALID = 该 plane 无可用空闲块
 *
 * @note 判据是 Free Pool 数组成员关系，不是 BST 的 FREE 位；
 * 选中 ⇒ alc_free_remove 必然成功。
 */
static WORD alc_free_find_block(const alc_s *alloc, BYTE plane, alc_super_alloc_mode_e mode)
{
    const BOOL high_wear = (mode == ALC_SUPER_ALLOC_COLD_HIGH_WEAR);
    WORD       best      = FTL_GBLOCK_INVALID;
    DWORD      key       = high_wear ? 0U : UINT32_MAX; /* high_wear：取最大擦除；否则取最小（动态 WL） */

    for (WORD i = 0U; i < alloc->free_count; i++)
    {
        WORD gblock = alloc->free_blocks[i];

        if (ftl_gblock_plane(gblock) != plane)
        {
            continue;
        }

        const bst_block_s *blk = &alloc->blocks[gblock];

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

/**
 * @brief 按 plane 追加一个块到 OP 池
 *
 * @param alloc  分配器
 * @param plane  plane 号
 * @param gblock 全局块号
 * @return true 已入池；false 该 plane OP 池已满
 */
BOOL alc_op_push(alc_s *alloc, BYTE plane, WORD gblock)
{
    if (alloc->op_count[plane] >= FTL_OP_BLOCKS_PER_PLANE)
    {
        return false;
    }

    alloc->op_blocks[plane][alloc->op_count[plane]] = gblock;
    alloc->op_count[plane]++;
    return true;
}

/**
 * @brief 从 OP 池借一块作为动态 L2P 日志块
 *
 * 只取同 plane；成功后先从 OP/Free Pool 摘除再置 LOG。固定日志区满时的有界溢出。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 借到的 gblock；FTL_GBLOCK_INVALID = OP 池无可用块
 */
WORD alc_op_alloc_log_block(alc_s *alloc, BYTE plane)
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

    bst_block_s *b = &alloc->blocks[gblock];

    ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
    ftl_block_clear_valid(b);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    return gblock;
}

/* ── 5. Spare Pool ── */

/**
 * @brief 按 plane 追加一个块到 Spare 池
 *
 * @param alloc  分配器
 * @param plane  plane 号
 * @param gblock 全局块号
 * @return true 已入池；false 该 plane Spare 池已满
 */
BOOL alc_spare_push(alc_s *alloc, BYTE plane, WORD gblock)
{
    if (alloc->spare_count[plane] >= FTL_SPARE_POOL_PER_PLANE)
    {
        return false;
    }

    alloc->spare_blocks[plane][alloc->spare_count[plane]] = gblock;
    alloc->spare_count[plane]++;
    return true;
}

/**
 * @brief 从 Spare 池 LIFO 弹出一个块
 *
 * @param alloc      分配器
 * @param plane      plane 号
 * @param out_gblock 输出：弹出的 gblock（调用者保证非空）
 * @return true 弹出成功；false 该 plane Spare 池为空
 */
BOOL alc_spare_pop(alc_s *alloc, BYTE plane, WORD *out_gblock)
{
    // 池内被运行期标坏的块直接丢弃（与 alc_free_refill_from_op 同口径）：
    // 返回一个 BAD 块会让调用者 alc_free_push 静默丢弃它，表面成功、实际没补到
    // 块。
    while (alloc->spare_count[plane] != 0U)
    {
        alloc->spare_count[plane]--;
        const WORD g = alloc->spare_blocks[plane][alloc->spare_count[plane]];

        if (ftl_block_is_bad(&alloc->blocks[g]))
        {
            continue;
        }

        *out_gblock = g;
        return true;
    }

    return false;
}

/**
 * @brief 从 Free Pool 逐块补足指定 plane 的 Spare 池
 *
 * 只补 state == FREE 的块；摘出后置 NONE，push 失败回滚到 Free。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return true 至少补了一块；false 没有可补的空闲块
 */
BOOL alc_spare_refill_from_free(alc_s *alloc, BYTE plane)
{
    BOOL moved = false;

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

        alc_free_remove(alloc, pick); /* 出池闸门已置 NONE */

        if (alc_spare_push(alloc, plane, pick) == false)
        {
            alc_free_push(alloc, pick); /* 回滚（目标未满时不应发生） */
            break;
        }
        moved = true;
    }

    return moved;
}

/* ── 6. Super Block：Open ── */

/**
 * @brief 只读判断：现在能不能开出一个新的 SB（不改变任何状态）。
 *
 * why：写序列跨 block 续写时，必须在**吃完当前 SB 之前**确认下一个开得出来。
 * 否则序列会停在「设备序列已打开、没有可提交的数据页」——序列门控挡住 GC，
 * padding END 也无页可分配（盘满例外会中止一条**已回执**的序列）。
 *
 * @param alloc 分配器
 * @param mode  与即将使用的 `alc_super_open_alloc_pair` 相同的策略
 *
 * @return true = 每个 plane 都能选到块（Free Pool，或 OP 池里有非 BAD 块）
 */
BOOL alc_super_can_open(const alc_s *alloc, alc_super_alloc_mode_e mode)
{
    // 与 alc_super_open 同口径：除静态 WL 外，free 不足时都可以从 OP 补。
    const BOOL allow_op = (mode != ALC_SUPER_ALLOC_COLD_HIGH_WEAR);

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (alc_free_find_block(alloc, plane, mode) != FTL_GBLOCK_INVALID)
        {
            continue;
        }

        if (allow_op == false)
        {
            return false;
        }

        BOOL any_op = false;

        for (WORD i = 0U; (i < alloc->op_count[plane]) && (any_op == false); i++)
        {
            any_op = (ftl_block_is_bad(&alloc->blocks[alloc->op_blocks[plane][i]]) == false);
        }

        if (any_op == false)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 为 Open SB 分配各 plane 的物理块
 *
 * @param alloc 分配器
 * @param sb    目标槽
 * @param mode  分配策略
 *
 * @return true = 分配成功
 *         false = 某 plane 无块
 *
 * @note 选块阶段不改 SB / 池账（**例外**：某 plane 的 Free 用尽时 OP 兜底会把一个
 *       OP 块移入 Free Pool——块仍在 Free，后续 plane 选块失败时它会留在那里，
 *       等价于提前从 OP 移入 Free，不泄漏）；全部 plane 就位后才出池并置 OPEN。
 */
static BOOL alc_super_open(alc_s *alloc, super_block_s *sb, alc_super_alloc_mode_e mode)
{
    const BYTE temperature = (mode == ALC_SUPER_ALLOC_HOT) ? FTL_TEMP_HOT : FTL_TEMP_COLD;
    // free 在某 plane 先耗尽时从该 plane 的 OP 池补一块：成对分配要求每 plane 各一块，
    // 只看总 free 会在两侧不均时永久失败（host 写窗口 flush 不出去 ⇒ USB 超时）。
    // OP 是设备 formation 时从 host 可用空间扣掉的余量，所有分配路径都可在 free 不足时
    // 使用；GC 的低水位按块净收益闸门负责防止无收益搬移把 OP 抽干。
    // 静态 WL 例外：OP 块的擦除次数不受该策略控制，不能借。
    const BOOL allow_op    = (mode != ALC_SUPER_ALLOC_COLD_HIGH_WEAR);
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
            return false;
        }
    }

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        // 先出池（校验 state == FREE），再由本转移赋 OPEN。
        alc_free_remove(alloc, gblock[plane]);
        sb->plane_blocks[plane] = ftl_gblock_block(gblock[plane]);

        bst_block_s *b = &alloc->blocks[gblock[plane]];

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

/**
 * @brief 为 Open SB 分配各 plane 的下一页
 *
 * 换块条件：未开 / 写满 / 含 BAD 块。策略只影响新开 SB 的选块方向；已打开的
 * SB 继续追加（同一 job 内必须连续写页）。
 *
 * @param alloc 分配器
 * @param mode  分配策略
 * @param out   输出：各 plane 的页 PBA
 * @return FTL_OK；FTL_AGAIN 某 plane 无块；FTL_ERROR 关闭旧 SB 失败
 */
ftl_status_e alc_super_open_alloc_pair(alc_s *alloc, alc_super_alloc_mode_e mode, DWORD out[PLANE_NUM])
{
    super_block_s *sb = (mode == ALC_SUPER_ALLOC_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    // 读恢复 / 擦除失败可能把 Open SB 内的块标 BAD；BAD 终态不能再写。
    if (sb->is_open)
    {
        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (super_block_has_plane(sb, plane) == false)
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

    if ((sb->is_open == false) || (sb->next_page >= PAGES_PER_BLOCK))
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
        if (alc_super_open(alloc, sb, mode) == false)
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

/**
 * @brief 当前 Open SB 还能分配几页（非序列形态）
 *
 * 无 Open SB / 已满返回 0；其余判定委托序列形态（page0 屏障不旁路）。
 *
 * @param alloc       分配器
 * @param temperature 温度流
 * @return 可分配页数；0 = 无 Open SB / 已满 / 屏障未过
 */
BYTE alc_super_open_get_room(const alc_s *alloc, BYTE temperature)
{
    const super_block_s *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if ((sb->is_open == false) || (sb->next_page >= PAGES_PER_BLOCK))
    {
        return 0U; /* 与序列形态的差异点：这里返回 0，序列形态报整块容量 */
    }

    return alc_super_open_get_room_seq(alloc, temperature, false);
}

/**
 * @brief 按序列语义计算当前 Open SB 的剩余页数
 *
 * 无 Open SB / 已满时报整块容量；page0 由本序列自己写时可越过屏障。
 *
 * @param alloc         分配器
 * @param temperature   温度流
 * @param page0_from_seq page0 是否由本序列写
 * @return 可分配页数
 */
BYTE alc_super_open_get_room_seq(const alc_s *alloc, BYTE temperature, BOOL page0_from_seq)
{
    const super_block_s *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    // 无 Open SB / 已满：下一次 alloc_pair 会关旧块、开新块。新块的 page0 由本序列
    // 在第一页程序时承担；能否真的开出新块由 alloc_pair 的返回值决定（这里只报容量）。
    if ((sb->is_open == false) || (sb->next_page >= PAGES_PER_BLOCK))
    {
        return PAGES_PER_BLOCK;
    }

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (super_block_has_plane(sb, plane) == false)
        {
            return 0U;
        }
        // page0 屏障：page0 属于**别的写者**时必须等它成功；由本序列自己写的 page0
        // 可以继续（有序管线保证 page0 失败会中止其后所有页，不击穿 rebuild 捷径）。
        if (((sb->page0_programmed & (BYTE)(1U << plane)) == 0U) && (page0_from_seq == false))
        {
            return 0U;
        }
        // BAD 块不能继续写；alloc 主路径会关闭该 SB，这里同样按 0 处理。
        if (ftl_block_is_bad(&alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])]))
        {
            return 0U;
        }
    }

    return (BYTE)(PAGES_PER_BLOCK - sb->next_page);
}

/**
 * @brief 查询 PBA 所属 plane 在当前 Open SB 的 page0 状态
 *
 * 不属于当前 SB（迟到命令）按未打开处理。
 *
 * @param alloc       分配器
 * @param temperature 温度流
 * @param pba         目标 PBA
 * @return ALC_SUPER_PBA_NOT_OPEN / OPEN_PAGE0_PENDING / OPEN_PAGE0_DONE
 */
alc_super_pba_state_e alc_super_open_get_pba_state(const alc_s *alloc, BYTE temperature, DWORD pba)
{
    const super_block_s *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if ((sb->is_open == false) || (pba == NAND_PBA_INVALID))
    {
        return ALC_SUPER_PBA_NOT_OPEN;
    }

    const BYTE plane = pba_to_plane(pba);

    if ((super_block_has_plane(sb, plane) == false) || (sb->plane_blocks[plane] != pba_to_block(pba)))
    {
        return ALC_SUPER_PBA_NOT_OPEN; /* 旧 SB 的迟到命令 */
    }

    return ((sb->page0_programmed & (BYTE)(1U << plane)) != 0U) ? ALC_SUPER_PBA_OPEN_PAGE0_DONE : ALC_SUPER_PBA_OPEN_PAGE0_PENDING;
}

/**
 * @brief 记 page0 编程完成，置对应 plane 的屏障位
 *
 * 按 plane+block 确认仍是当前 SB；迟到完成属于旧 SB 时忽略。
 *
 * @param alloc       分配器
 * @param temperature 温度流
 * @param pba         完成的 PBA（page 必须为 0）
 */
void alc_super_open_note_page_programmed(alc_s *alloc, BYTE temperature, DWORD pba)
{
    super_block_s *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if ((sb->is_open == false) || (pba == NAND_PBA_INVALID) || (pba_to_page(pba) != 0U))
    {
        return;
    }

    // 迟到的完成可能属于已关闭的旧 SB：必须按 plane+block 确认它仍指向当前 Open SB，
    // 否则会把新 SB 的 page0 屏障误置为已通过。
    const BYTE plane = pba_to_plane(pba);

    if ((super_block_has_plane(sb, plane) == false) || (sb->plane_blocks[plane] != pba_to_block(pba)))
    {
        return;
    }

    sb->page0_programmed |= (BYTE)(1U << plane);
}

/**
 * @brief 回退一次 alloc_pair 的写指针
 *
 * 只回退 next_page，不回退可能刚发生的开新 SB。调用者必须保证紧邻一次成功的
 * alloc_pair，且期间没有其他页对分配/关闭 SB。
 *
 * @param alloc       分配器
 * @param temperature 温度流
 *
 * @note next_page == 0 时拒绝并留日志：BYTE 下溢会变成 255，而 PBA 的页域只有
 * 6 bit（PAGES_PER_BLOCK=64）⇒ 静默落到本块最后一页，形成二次编程。
 */
void alc_super_open_rewind_pair(alc_s *alloc, BYTE temperature)
{
    super_block_s *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    ASSERT(sb->next_page > 0U);
    if (sb->next_page == 0U)
    {
        LOGE(LOG_TAG_FTL "rewind without alloc_pair, ignored\r\n");
        return;
    }

    sb->next_page--;
}

/**
 * @brief 静态 WL 启动前摘掉 host 当前的 cold SB
 *
 * 已写过 ⇒ 正常关闭进 Closed；未写过 ⇒ 块归还 Free Pool，不产生空 Closed 条目。
 *
 * @param alloc 分配器
 * @return true 已摘掉（或本来就无 cold SB）；false 列表满，关不掉
 */
BOOL alc_super_prepare_wl_destination(alc_s *alloc)
{
    ASSERT(alloc != NULL);

    super_block_s *sb = &alloc->cold_sb;

    if (sb->is_open == false)
    {
        return true;
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
        return true;
    }

    // 关不掉时 cold SB 仍 open ⇒ 调用者必须放弃本次 WL（否则续写普通块）。
    return (alc_super_close_live(alloc, sb) != FTL_BLOCK_NO_SB);
}

/* ── 7. Super Block：Closed ── */

/**
 * @brief Open → Closed：唯一 live setter + 列表追加
 *
 * @param alloc 分配器
 * @param sb    待关闭 SB（live 或重建时的临时条目）
 *
 * @return 新条目在 closed_sb_list 的索引
 *         FTL_BLOCK_NO_SB = 列表满，条目被拒绝
 *
 * @note live 标志与列表副本必须在同一函数内完成——只改副本会让 Open SB
 * 继续被分配、或被重复关闭。调用者必须检查失败码；列表满
 * 属结构异常，不能静默吞掉。
 * @note 全部 plane 都不可用（BAD / 越界）时不追加条目（避免 plane_count==0 的
 * 死条目泄漏列表槽），返回当前 count —— 调用者只判 `== NO_SB`。
 */
static WORD alc_super_close_live(alc_s *alloc, super_block_s *sb)
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
    super_block_s entry      = *sb;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (super_block_has_plane(&entry, plane) == false)
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

        bst_block_s *b = &alloc->blocks[ftl_gblock(plane, entry.plane_blocks[plane])];

        if (ftl_block_is_bad(b))
        {
            entry.plane_blocks[plane] = FTL_BLOCK_NO_PLANE; /* 坏块不进入 Closed SB */
            continue;
        }

        ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
        ftl_block_set_temp(b, entry.temperature);
        ftl_block_set_sb(b, closed_idx);
    }

    // 全部 plane 都已被过滤（BAD / 越界）：没有可回收的块，**不产生条目**。
    // 追加会让 plane_count==0 的条目永远选不中、也永远没人移除 ⇒ closed 列表槽
    // 泄漏。SB 仍算已关闭；返回当前 count，调用者只与
    // FTL_BLOCK_NO_SB 比较（此时返回值不是有效下标）。
    {
        BOOL any_plane = false;

        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (entry.plane_blocks[plane] != FTL_BLOCK_NO_PLANE)
            {
                any_plane = true;
                break;
            }
        }

        if (any_plane == false)
        {
            LOGW(LOG_TAG_FTL "closed_sb: all planes unusable, entry not added\r\n");
            return alloc->closed_sb_count;
        }
    }

    alloc->closed_sb_list[alloc->closed_sb_count++] = entry;
    return closed_idx;
}

/**
 * @brief 关闭当前 Open SB（Open → Closed）
 *
 * 已关闭的槽再关一次是合法 no-op，返回 FTL_ERROR，不转 ASSERT。
 *
 * @param alloc       分配器
 * @param temperature 温度流
 * @return FTL_OK；FTL_ERROR 未打开 / 列表满
 */
ftl_status_e alc_super_close(alc_s *alloc, BYTE temperature)
{
    super_block_s *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

    if (sb->is_open == false)
    {
        return FTL_ERROR;
    }

    return (alc_super_close_live(alloc, sb) == FTL_BLOCK_NO_SB) ? FTL_ERROR : FTL_OK;
}

/**
 * @brief 在 Closed 列表中查找与 sb 相同的条目
 *
 * 比较全部 plane——不同 SB 的哨兵 plane 可能相同，只比一个会误中。
 *
 * @param alloc   分配器
 * @param sb      待查 SB
 * @param out_idx 输出：命中的列表下标
 * @return true 找到；false 未找到
 */
BOOL alc_super_closed_find(const alc_s *alloc, const super_block_s *sb, WORD *out_idx)
{
    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_s *e    = &alloc->closed_sb_list[i];
        BOOL                 same = true;

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

/**
 * @brief 按物理块重建一个 Closed 条目
 *
 * 按单个物理块建条目，另一 plane 记哨兵；next_page 记满。
 *
 * @param alloc        分配器
 * @param plane_blocks 各 plane 的块号
 * @param temperature  温度流
 * @return 新条目下标；FTL_BLOCK_NO_SB = 列表满
 */
WORD alc_super_closed_add(alc_s *alloc, const WORD plane_blocks[PLANE_NUM], BYTE temperature)
{
    super_block_s sb;

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

/**
 * @brief 从 Closed 列表移除条目（swap-remove）
 *
 * 末位条目搬家后修正其各 plane 的 sb_id；首循环兜住 free/retire 被拒绝、
 * sb_id 未清的路径。
 *
 * @param alloc      分配器
 * @param closed_idx 待移除的列表下标
 */
void alc_super_closed_remove(alc_s *alloc, WORD closed_idx)
{
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (alloc->closed_sb_list[closed_idx].plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue;
        }

        bst_block_s *b = &alloc->blocks[ftl_gblock(plane, alloc->closed_sb_list[closed_idx].plane_blocks[plane])];

        if (ftl_block_sb_id(b) == closed_idx)
        {
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        }
    }

    alloc->closed_sb_count--;
    if (closed_idx != alloc->closed_sb_count)
    {
        super_block_s *moved = &alloc->closed_sb_list[closed_idx];
        *moved               = alloc->closed_sb_list[alloc->closed_sb_count];

        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (moved->plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
            {
                continue;
            }

            bst_block_s *b = &alloc->blocks[ftl_gblock(plane, moved->plane_blocks[plane])];

            if (ftl_block_sb_id(b) == alloc->closed_sb_count)
            {
                ftl_block_set_sb(b, closed_idx);
            }
        }
    }
}

/**
 * @brief 从 Closed 条目摘除一个已擦除/已标坏的 plane 块
 *
 * GC/WL/predict 逐 plane 擦 victim。擦完即摘除，条目只在仍有未处理 plane 时
 * 存在；本函数只同步 Closed 条目与 sb_id，不碰池。
 *
 * @param alloc 分配器
 * @param plane plane 号
 * @param block 该 plane 内的块号
 * @return true 条目已无任何 plane（被移除）；false 仍有其他 plane 或未找到
 */
BOOL alc_super_closed_drop_plane(alc_s *alloc, BYTE plane, WORD block)
{
    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        super_block_s *e = &alloc->closed_sb_list[i];

        if (e->plane_blocks[plane] != block)
        {
            continue;
        }

        bst_block_s *b = &alloc->blocks[ftl_gblock(plane, block)];

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

/**
 * @brief 汇总一个 SB 的 valid/erase/read 统计
 *
 * 跳过哨兵与 BAD plane；valid_count 单位是 CW。
 *
 * @param alloc 分配器
 * @param sb    目标 SB
 * @param out   输出统计
 */
void alc_super_get_stats(const alc_s *alloc, const super_block_s *sb, alc_super_stats_s *out)
{
    WORD  valid     = 0U;
    DWORD erase_max = 0U;
    DWORD erase_sum = 0U;
    DWORD pe_min    = UINT32_MAX;
    DWORD read_sum  = 0U;
    BYTE  n         = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (super_block_has_plane(sb, plane) == false)
        {
            continue;
        }

        const bst_block_s *blk = &alloc->blocks[ftl_gblock(plane, sb->plane_blocks[plane])];

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

/**
 * @brief 记录一次块擦除：erase_count++、清零 read_count/valid_count
 *
 * 与 migrate 擦除完成同口径；供元数据日志块等不经 migrate 的擦除路径调用。
 * rr.health / last_ecc / rising_count 保留（预测性退役趋势跨擦除周期）。
 *
 * @param alloc  分配器
 * @param gblock 全局块号
 * @param now    擦除时刻
 */
void alc_bst_note_erase(alc_s *alloc, WORD gblock, DWORD now)
{
    ASSERT(ftl_block_is_bad(&alloc->blocks[gblock]) == false);

    bst_block_s *b = &alloc->blocks[gblock];

    b->erase_count++;
    b->last_pe_ts = now;
    ftl_block_clear_valid(b);

    // read disturb 是「自上次擦除以来」的计数：擦除把它清零。
    // rr.health / last_ecc / rising_count **保留**——预测性退役趋势要跨擦除周期观察。
    b->read_count = 0U;
}

/**
 * @brief 扫描 BST 汇总擦除次数分布
 *
 * 跳过 BAD 与 NONE（固定元数据/OP/Spare/未初始化）；NONE 的 erase_count 不随
 * 擦除累加，计入会把最小值钉在 0。
 *
 * @param alloc 分配器
 * @param out   输出统计
 */
void alc_bst_get_erase_stats(const alc_s *alloc, alc_bst_erase_stats_s *out)
{
    DWORD min_ec = UINT32_MAX;
    DWORD max_ec = 0U;
    DWORD sum    = 0U;
    DWORD max_t  = 0U;
    WORD  n      = 0U;

    for (WORD i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const bst_block_s *blk = &alloc->blocks[i];

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

/**
 * @brief 用磨损快照覆盖块的 wear 字段（Checkpoint 回放）
 *
 * 只覆盖 erase/read/rr，不触发任何记账。
 *
 * @param alloc  分配器
 * @param gblock 全局块号
 * @param wear   磨损快照
 */
void alc_bst_set_wear(alc_s *alloc, WORD gblock, const bst_wear_s *wear)
{
    // wear 由唯一调用者 checkpoint.c 传本地快照地址；模块内部不重复守卫。
    bst_block_s *blk = &alloc->blocks[gblock];

    blk->erase_count = wear->erase_count;
    blk->read_count  = wear->read_count;
    blk->rr          = wear->rr;
}

/* ── 9. Trace ── */

/**
 * @brief 记录一跳映射变更到诊断环
 *
 * alloc 为空 / pba 非法时静默返回；只写 RAM，不参与决策。
 *
 * @param alloc 分配器（可 NULL）
 * @param lba   host LBA
 * @param pba   目标 PBA
 * @param kind  记录类型
 */
void alc_trace_record(alc_s *alloc, DWORD lba, DWORD pba, alc_trace_kind_e kind)
{
    if ((alloc == NULL) || (pba_is_valid(pba) == false))
    {
        return;
    }

    alc_trace_s *t = &alloc->trace[alloc->trace_seq % ALC_TRACE_DEPTH];

    t->seq   = alloc->trace_seq++;
    t->lba   = lba;
    t->pba   = pba;
    t->plane = pba_to_plane(pba);
    t->block = pba_to_block(pba);
    t->page  = pba_to_page(pba);
    t->cw    = pba_to_cw(pba);
    t->kind  = kind;
}

/**
 * @brief 在诊断环里查指定 LBA/PBA 的历史并打印
 *
 * 统计重复 PBA，最多打前 8 条。
 *
 * @param alloc 分配器
 * @param lba   目标 LBA
 * @param pba   目标 PBA
 */
void alc_trace_dump(const alc_s *alloc, DWORD lba, DWORD pba)
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
        const alc_trace_s *t = &alloc->trace[(start + i) % ALC_TRACE_DEPTH];

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
        const alc_trace_s *t = &alloc->trace[(start + i) % ALC_TRACE_DEPTH];

        if ((t->pba != pba) && (t->lba != lba))
        {
            continue;
        }

        LOGW(LOG_TAG_TRC "seq=%lu lba=%lu pba=0x%08lx pl=%hhu bk=%hu pg=%hhu cw=%hhu %s\r\n", t->seq, t->lba, t->pba, t->plane, t->block, t->page, t->cw, (t->kind == ALC_TRACE_WRITE_OK) ? "write-ok" : ((t->kind == ALC_TRACE_PROGRAM_FAIL) ? "program-fail" : "gc-write"));
        shown++;
    }
}
