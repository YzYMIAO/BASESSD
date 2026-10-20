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
 *         migrate.c 的搬移引擎；异步读路径用 ftl_async_read_page + gc_decode_cw_lba
 *         （gc_read_page_with_lba 是掉电重建专用的同步原语）。
 *
 * 触发三条件（wl_should_trigger，任一命中即搬）：最大/最小差值、最大/平均比值、低擦除区占比。
 *
 * 文件结构：
 *   1. 触发 / 选块     wl_should_trigger / wl_select_victim
 *   2. 磨损统计        wl_get_min_erase
 *
 * ⚠ 搬移在 `ftl/migrate.c` 的异步 job 里完成（GC / WL 共用一台引擎，靠 kind/post 区分）
 *   ——本文件只留触发判定与选块策略。
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 *
 * ⚠ 入口前置守卫口径（docs/design/assertion_guidelines.md §二十六 / §二十八）：
 *   本文件全部函数都取 ftl 实例指针，且**只被 migrate.c（已持实例）与本文件内部**调用
 *   ⇒ ftl 非空是**前置条件**，不逐函数复检。
 */

#include "wear_leveling.h"

#include "ftl.h"

#include <string.h>

/* ── 1. 触发 / 选块 ── */

/* --------------------------------------------------------------------
 * wl_should_trigger
 *   功能：判定是否该做一轮 WL（三条件任一命中即 true）。
 *   参数：ftl —— FTL 实例。
 *   返回：true 触发；false 不触发（未达最小间隔 / 三条件均未命中）。
 *   ⚠ ftl 非空是**前置条件**（调用者 migrate.c 持有实例指针），不在此复检——
 *     见 docs/design/assertion_guidelines.md §二十六「模块内部不逐函数复检同一入参」。
 *   实现要点：先过 WL_MIN_WRITES 启动门（刚开机不抖）；条件 1 最大/最小差值
 *             ≥ WL_DELTA_THRESHOLD；条件 2 最大 ≥ 平均 × WL_MAX_AVG_RATIO；
 *             条件 3 低擦除区（min + WL_LOW_ZONE_WIDTH 以下）块占比 **>** WL_LOW_BLOCK_PCT%。
 * -------------------------------------------------------------------- */
bool wl_should_trigger(const ftl_allocator_t *alloc, DWORD total_writes)
{
    /* 启动门：累计编程次数不足时不求值。这是一次性门，不是周期节拍；
     * 周期节拍由 mig_handle 的 WL 求值 的 WL_EVAL_INTERVAL_MS 负责。 */
    if (total_writes < WL_MIN_WRITES)
    {
        return false;
    }

    /* 一次全表扫描取全部派生量（口径由 allocator 定义）。 */
    allocator_erase_stats_t est;
    alc_get_erase_stats(alloc, &est);

    const DWORD max_ec = est.erase_max;
    const DWORD min_ec = est.erase_min;
    const DWORD avg_ec = (est.good_count == 0U) ? 0U : (est.erase_sum / est.good_count);

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

    /* 条件 3：低擦除区 Block 占比。
     * ⚠ 分子由 alc_count_blocks_below 统计（非 BAD/NONE 的全部块，含 RETIRED），
     *   分母是 alloc->total_blocks（用户区好块数）。RETIRED 极少时两者近似一致；
     *   分子集合更大 ⇒ 占比略偏高，判据偏保守（更容易触发，均衡方向安全）。 */
    const DWORD low_zone  = min_ec + WL_LOW_ZONE_WIDTH;
    const DWORD low_count = alc_count_blocks_below(alloc, low_zone);
    if (low_count > alloc->total_blocks * WL_LOW_BLOCK_PCT / 100U)
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
bool wl_select_victim(const ftl_allocator_t *alloc, wl_victim_t *victim)
{
    wl_victim_t best;
    memset(&best, 0, sizeof(best));

    DWORD min_ec = wl_get_min_erase(alloc);

    /* 门限单位是**有效 CW**（CW 级映射：1 页 = CW_NUM 个映射单元） */
    WORD total_cw_per_sb = FTL_CW_PER_SB;
    WORD min_valid_cw    = (total_cw_per_sb * WL_MIN_VALID_RATIO / 100U);

    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_t *sb = alc_closed_at(alloc, i);

        /* 跳过仍为 Open 的 SB */
        if (sb->is_open)
        {
            continue;
        }

        /* 任一 present plane 的块已坏 ⇒ 跳过：migrate 读它必然失败并 abort，
         * 而该 SB 仍可能是“有效 CW 最多”的候选 ⇒ 每轮重选、WL 永久卡死。 */
        bool has_bad_plane = false;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (!super_block_has_plane(sb, p))
            {
                continue;
            }
            const WORD block  = sb->plane_blocks[p];
            const WORD gblock = ftl_gblock(p, block);

            if (ftl_block_is_bad(&alloc->blocks[gblock]))
            {
                has_bad_plane = true;
                break;
            }
        }
        if (has_bad_plane)
        {
            continue;
        }

        /* 各 plane 聚合值由 allocator 统一计算（有效 CW / 擦除平均 / read disturb）。
         * 有效 CW 越多（越冷）越优先；同一有效数下 read_count 高者优先
         * （read disturb 风险高 → 优先搬移；read_count 来自 host 读 + GC/WL 搬移读累加）。 */
        allocator_sb_stats_t st;
        alc_get_sb_stats(alloc, sb, &st);
        if (st.plane_count == 0U)
        {
            continue; /* 全哨兵条目 */
        }

        const DWORD sb_ec = st.erase_sum / st.plane_count;

        /* 不在低擦除区 → 跳过（擦除够多了） */
        if (sb_ec > (min_ec + WL_LOW_ZONE_WIDTH))
        {
            continue;
        }

        if (st.valid_cw < min_valid_cw)
        {
            continue;
        }

        if ((st.valid_cw > best.valid_cw) ||
            ((st.valid_cw == best.valid_cw) && (st.read_sum > best.read_count)))
        {
            best.found      = true;
            best.valid_cw   = st.valid_cw;
            best.read_count = st.read_sum;
            for (BYTE p = 0U; p < PLANE_NUM; p++)
            {
                best.plane_blocks[p] = sb->plane_blocks[p];
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

/* 磨损统计访问器 —— 口径（好块 / 坏块排除）由 allocator 定义。
 * 热路径 wl_should_trigger 用 alc_get_erase_stats 一次取全；本访问器只服务
 * wl_select_victim 的低擦除区判定。 */

/* wl_get_min_erase — 全体好块擦除次数的最小值（无好块返回 0） */
DWORD wl_get_min_erase(const ftl_allocator_t *alloc)
{
    allocator_erase_stats_t st;

    alc_get_erase_stats(alloc, &st);
    return st.erase_min;
}
