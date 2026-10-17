/*
 * wear_leveling.c — 静态磨损均衡（触发判定 + 选 victim）
 *
 * 职责：
 *   在块擦除次数分布不均时，挑出「擦得少」的冷 SB 作为搬运候选；另有一条「慢死预测」
 *   旁路的巡检（rr_predictive_scan）由搬移引擎调用。**搬移本身不在本文件**。
 *
 * 与 gc.c 的分工：
 *   GC —— 回收**无效页多**的块（空间回收，顺带磨损均衡）；
 *   WL —— 搬运**擦得少**的块（磨损拉平，顺带 read disturb 缓解）。两者共用
 *         gc_read_page_with_lba 读原语与 migrate.c 的搬移引擎。
 *
 * 触发三条件（wl_should_trigger，任一命中即搬）：最大/最小差值、最大/平均比值、低擦除区占比。
 *
 * 文件结构：
 *   1. 触发 / 选块     wl_should_trigger / wl_select_victim
 *   2. 磨损统计        wl_max_erase / wl_min_erase / wl_avg_erase / wl_count_blocks_below
 *
 * ⚠ 搬移（原第 2 节 wl_relocate / wl_execute / wl_predictive_relocate）已随阶段 4 迁入
 *   `ftl/migrate.c` 的异步 job（GC / WL 共用一台引擎，靠 kind/post 区分）——本文件
 *   只留触发判定与选块策略。
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 *
 * ⚠ 入口前置守卫口径（docs/design/assertion_guidelines.md §二十六 / §二十八）：
 *   本文件全部函数都取 ftl 实例指针，且**只被 migrate.c（已持实例）与本文件内部**调用
 *   ⇒ ftl 非空是**前置条件**，不逐函数复检（原 6 处守卫已删）。
 */

#include "ftl/wear_leveling.h"

#include "common/assertion.h"
#include "ftl/ftl.h"

#include <string.h>

/* ── 1. 触发 / 选块 ── */

/* --------------------------------------------------------------------
 * wl_should_trigger
 *   功能：判定是否该做一轮 WL（三条件任一命中即 true）。
 *   参数：ftl —— FTL 实例。
 *   返回：true 触发；false 不触发（未达最小间隔 / 三条件均未命中）。
 *   ⚠ ftl 非空是**前置条件**（调用者 migrate.c 持有实例指针），不在此复检——
 *     见 docs/design/assertion_guidelines.md §二十六「模块内部不逐函数复检同一入参」。
 *   实现要点：先过 WL_MIN_INTERVAL 最小间隔门（避免刚开机抖动）；条件 1 最大/最小差值
 *             ≥ WL_DELTA_THRESHOLD；条件 2 最大 ≥ 平均 × WL_MAX_AVG_RATIO；
 *             条件 3 低擦除区（min + WL_LOW_ZONE_WIDTH 以下）块占比 > WL_LOW_BLOCK_PCT%。
 * -------------------------------------------------------------------- */
bool wl_should_trigger(const struct ftl_context *ftl)
{
    /* WL 最小间隔检查 */
    if (ftl->total_writes < WL_MIN_INTERVAL)
    {
        return false;
    }

    uint32_t max_ec = wl_max_erase(ftl);
    uint32_t min_ec = wl_min_erase(ftl);
    uint32_t avg_ec = wl_avg_erase(ftl);

    /* 条件 1：最大/最小差值 */
    if ((max_ec - min_ec) >= WL_DELTA_THRESHOLD)
    {
        return true;
    }

    /* 条件 2：最大/平均比值 */
    if ((avg_ec > 0U) && (max_ec >= avg_ec * WL_MAX_AVG_RATIO))
    {
        return true;
    }

    /* 条件 3：低擦除区 Block 占比 */
    uint32_t low_zone  = min_ec + WL_LOW_ZONE_WIDTH;
    uint32_t low_count = wl_count_blocks_below(ftl, low_zone);
    if (low_count > (uint32_t)ftl->alloc.total_blocks * WL_LOW_BLOCK_PCT / 100U)
    {
        return true;
    }

    return false;
}

/* --------------------------------------------------------------------
 * wl_select_victim
 *   功能：从 Closed SB 列表里挑一个搬运候选（擦得少、有效 CW 多、read_count 高者优先）。
 *   参数：ftl —— FTL 实例；victim —— 出参。
 *   返回：true 找到；false 未找到。
 *   实现要点：只考虑擦除次数落在低擦除区（min + WL_LOW_ZONE_WIDTH 内）的 Closed SB；
 *             有效 CW 须 ≥ 最小有效 CW 门（WL_MIN_VALID_RATIO，太碎不值当搬）；
 *             排序键 = 有效 CW 多（越冷越优先）→ 同有效 CW 下 read_count 高（read disturb 风险高优先）。
 * -------------------------------------------------------------------- */
bool wl_select_victim(const struct ftl_context *ftl, wl_victim_t *victim)
{
    wl_victim_t best;
    memset(&best, 0, sizeof(best));

    uint32_t min_ec = wl_min_erase(ftl);

    /* 门限单位是**有效 CW**（CW 级映射：1 页 = CW_NUM 个映射单元） */
    uint16_t total_cw_per_sb = (uint16_t)(PLANE_NUM * PAGES_PER_BLOCK * CW_NUM);
    uint16_t min_valid_cw    = (uint16_t)(total_cw_per_sb * WL_MIN_VALID_RATIO / 100U);

    for (uint16_t i = 0U; i < allocator_closed_count(&ftl->alloc); i++)
    {
        const super_block_t *sb = allocator_closed_at(&ftl->alloc, i);

        /* 跳过仍为 Open 的 SB */
        if (sb->is_open)
        {
            continue;
        }

        /* 计算该 Super Block 的平均擦除次数（全局 Block 号索引 BST）。
         * 退化单 plane 条目（重建产物）跳过哨兵 plane，按实际 plane 数取平均。 */
        uint32_t sb_ec;
        {
            uint32_t ec_sum = 0U;
            uint8_t  n      = 0U;

            for (uint8_t p = 0U; p < PLANE_NUM; p++)
            {
                if (!super_block_has_plane(sb, p))
                {
                    continue;
                }
                ec_sum += ftl->alloc.blocks[ftl_gblock(p, sb->plane_blocks[p])].erase_count;
                n++;
            }
            sb_ec = (n > 0U) ? (ec_sum / n) : 0U;
        }

        /* 不在低擦除区 → 跳过（擦除够多了） */
        if (sb_ec > (min_ec + WL_LOW_ZONE_WIDTH))
        {
            continue;
        }

        /*
         * 有效 CW = 双 Plane 各物理块 valid_count 之和（CW 级真实计数）。
         * 有效 CW 越多（越冷）越优先。
         * B5：同一有效数下，read_count 越高的冷块越优先（read disturb
         * 风险高 → 优先搬移；read_count 来自 host 读 + GC/WL 搬移读累加）。
         */
        {
            uint16_t valid  = 0U;
            uint32_t rc_sum = 0U;

            for (uint8_t p = 0U; p < PLANE_NUM; p++)
            {
                if (!super_block_has_plane(sb, p))
                {
                    continue; /* 退化单 plane 条目 */
                }
                const ftl_block_t *blk = &ftl->alloc.blocks[ftl_gblock(p, sb->plane_blocks[p])];

                valid = (uint16_t)(valid + blk->valid_count);
                rc_sum += blk->read_count;
            }

            if (valid < min_valid_cw)
            {
                continue;
            }

            if ((valid > best.valid_cw) ||
                ((valid == best.valid_cw) && (rc_sum > best.read_count)))
            {
                best.found           = 1U;
                best.avg_erase       = sb_ec;
                best.valid_cw        = valid;
                best.read_count      = rc_sum;
                best.plane_blocks[0] = sb->plane_blocks[0];
                best.plane_blocks[1] = sb->plane_blocks[1];
            }
        }
    }

    if (!best.found)
    {
        return false;
    }

    *victim = best;
    return true;
}

/* ── 2. 磨损统计（全部跳过坏块）── */

/* wl_max_erase — 全体好块擦除次数的最大值（无块返回 0） */
uint32_t wl_max_erase(const struct ftl_context *ftl)
{
    uint32_t max_ec = 0U;

    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count > max_ec)
        {
            max_ec = b->erase_count;
        }
    }

    return max_ec;
}

/* wl_min_erase — 全体好块擦除次数的最小值（无好块返回 0） */
uint32_t wl_min_erase(const struct ftl_context *ftl)
{
    uint32_t min_ec = UINT32_MAX;

    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count < min_ec)
        {
            min_ec = b->erase_count;
        }
    }

    return (min_ec == UINT32_MAX) ? 0U : min_ec;
}

/* wl_avg_erase — 全体好块擦除次数的平均值（无好块返回 0） */
uint32_t wl_avg_erase(const struct ftl_context *ftl)
{
    uint32_t sum   = 0U;
    uint16_t count = 0U;

    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        sum += b->erase_count;
        count++;
    }

    if (count == 0U)
    {
        return 0U;
    }
    return sum / count;
}

/* wl_count_blocks_below — 擦除次数严格小于 threshold 的好块数（低擦除区占比统计用） */
uint32_t wl_count_blocks_below(const struct ftl_context *ftl, uint32_t threshold)
{
    uint32_t count = 0U;

    for (uint16_t i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count < threshold)
        {
            count++;
        }
    }

    return count;
}
