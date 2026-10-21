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
 * 触发判据（UBI 模型，见 wl_should_trigger）：是否存在「高擦除空闲块」显著旧于
 * 「最冷的 used SB」；是则把冷数据搬到高磨损块，低磨损块空出来给热数据。
 *
 * 文件结构：
 *   1. 触发 / 选块     wl_should_trigger / wl_select_victim
 *   2. 磨损统计        wl_min_used_erase（文件内 static）
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

/* Closed SB 中最小的平均擦除次数（UBI 的 e1）。
 * 跳过：Open 条目、含坏 plane 的 SB（migrate 读它必 abort）、全哨兵条目、
 * 有效 CW 不足 WL_MIN_VALID_RATIO 的 SB（与 wl_select_victim 同门限）。 */
static DWORD wl_min_used_erase(const alc_t *alloc)
{
    DWORD min_ec = UINT32_MAX;

    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_t *sb = alc_closed_at(alloc, i);

        if (sb->is_open)
        {
            continue;
        }

        bool has_bad_plane = false;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (!super_block_has_plane(sb, p))
            {
                continue;
            }
            if (ftl_block_is_bad(&alloc->blocks[ftl_gblock(p, sb->plane_blocks[p])]))
            {
                has_bad_plane = true;
                break;
            }
        }
        if (has_bad_plane)
        {
            continue;
        }

        alc_sb_stats_t st;
        alc_get_sb_stats(alloc, sb, &st);
        if (st.plane_count == 0U)
        {
            continue;
        }

        /* 与 wl_select_victim 同一有效门限：触发判据必须与实际能搬的候选一致，
         * 否则可能按一个「不可选」的极冷块触发，却把更热的块搬到更冷的空闲块上。 */
        const WORD min_valid_cw = (WORD)(FTL_CW_PER_SB * WL_MIN_VALID_RATIO / 100U);

        if (st.valid_cw < min_valid_cw)
        {
            continue;
        }

        const DWORD sb_ec = st.erase_sum / st.plane_count;

        if (sb_ec < min_ec)
        {
            min_ec = sb_ec;
        }
    }

    return (min_ec == UINT32_MAX) ? 0U : min_ec;
}

/* --------------------------------------------------------------------
 * wl_should_trigger
 *   功能：判定是否该做一轮 WL（UBI 模型）。
 *   参数：alloc —— 分配器；total_writes —— 累计编程次数（启动门）。
 *   返回：true 触发；false 不触发。
 *   实现要点：先过 WL_MIN_WRITES 启动门（刚开机不抖）；唯一判据是
 *             `max_free_ec - min_used_ec >= WL_THRESHOLD`：
 *               · e1 = 最冷的 used SB（Closed 列表平均擦除次数最小）；
 *               · e2 = Free Pool 中擦除次数最大的块。
 *             搬移不改变擦除计数：差值不足时必须停止，否则就是纯写放大
 *             （板端 `kind=wl vcw=512` 无限搬迁，RISK-WL-08）。
 * -------------------------------------------------------------------- */
bool wl_should_trigger(const alc_t *alloc, DWORD total_writes)
{
    /* 启动门：累计编程次数不足时不求值。这是一次性门，不是周期节拍；
     * 周期节拍由 mig_handle 的 WL 求值 的 WL_EVAL_INTERVAL_MS 负责。 */
    if (total_writes < WL_MIN_WRITES)
    {
        return false;
    }

    const DWORD min_used = wl_min_used_erase(alloc);
    const DWORD max_free = alc_free_max_erase(alloc);

    return (max_free > min_used) && ((max_free - min_used) >= WL_THRESHOLD);
}

/* --------------------------------------------------------------------
 * wl_select_victim
 *   功能：挑「最冷」的 Closed SB（UBI 的 e1）：平均擦除次数最少。
 *   参数：alloc —— 分配器；victim —— 出参。
 *   返回：true 找到；false 未找到。
 *   实现要点：有效 CW 须 ≥ WL_MIN_VALID_RATIO（太碎不值当搬）；
 *             排序键 = SB 平均擦除次数**最少** → 同擦除次数下 valid_cw 多
 *             （越冷越优先）→ 再按 read_sum 高（read disturb 风险高优先）。
 *             ⚠ 不再按「低擦除区」过滤：低区阈值在低磨损量下恒真（RISK-WL-08）。
 * -------------------------------------------------------------------- */
bool wl_select_victim(const alc_t *alloc, wl_victim_t *victim)
{
    wl_victim_t best;
    memset(&best, 0, sizeof(best));

    DWORD best_ec = UINT32_MAX;

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
        alc_sb_stats_t st;
        alc_get_sb_stats(alloc, sb, &st);
        if (st.plane_count == 0U)
        {
            continue; /* 全哨兵条目 */
        }

        const DWORD sb_ec = st.erase_sum / st.plane_count;

        if (st.valid_cw < min_valid_cw)
        {
            continue;
        }

        if ((sb_ec < best_ec) ||
            ((sb_ec == best_ec) && ((st.valid_cw > best.valid_cw) ||
                                    ((st.valid_cw == best.valid_cw) && (st.read_sum > best.read_count)))))
        {
            best_ec         = sb_ec;
            best.found      = true;
            best.valid_cw   = st.valid_cw;
            best.read_count = st.read_sum;
            best.next_page  = sb->next_page;
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

/* ── 2. 磨损统计 ──
 * 口径（好块 / 坏块排除）由 allocator 定义；本模块只用一个文件内 static 的
 * used-SB 最小值（wl_min_used_erase），不再导出单点访问器。 */
