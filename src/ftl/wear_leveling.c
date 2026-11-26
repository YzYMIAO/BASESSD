/*
 * wear_leveling.c — 静态磨损均衡（触发判定 + 选 victim）
 *
 * 职责：
 *   · 从 Closed SB 中筛出静态 WL 的合法候选（同一套过滤给触发与选块共用）；
 *   · 求 Free Pool 最大擦除次数与最冷 used SB 的平均擦除次数，判定是否触发；
 *   · 选 victim：平均擦除次数最少 → 同擦除次数下 valid_cw 多 → read_sum 高。
 *
 * 关键机制：
 *   · 触发判据 = UBI 模型：max_free_ec − min_used_ec ≥ WL_THRESHOLD；
 *     搬移不改变擦除计数，差值不足必须停止，否则是纯写放大；
 *   · 候选过滤：跳过 Open / 含坏 plane / 全哨兵条目；有效 CW ≥ 条目容量 × WL_MIN_VALID_RATIO；
 *   · 全部按全局块号索引 BST（跨 plane），退化单 plane 条目按自身容量算分母。
 *
 * 与其他模块：
 *   · 上游：migrate.c 的 WL 求值 / 搬移引擎调用；
 *   · 下游：allocator（closed 列表 / free 最大擦除）、CLI `wear`（同一漏斗）；
 *   · 搬移本身不在本文件（GC / WL 共用 migrate.c 的异步 job）。
 */

#include "wear_leveling.h"

#include "ftl.h"

#include <string.h>

/* ── candidate ── */

/**
 * @brief 判定一个 Closed 条目是否是静态 WL 的合法候选，并给出聚合统计与平均擦除次数。
 *
 * @param alloc 分配器
 * @param sb Closed 条目（调用者从 closed_sb_list 取得）
 * @param out_stats 出参聚合统计（可 NULL）
 * @param out_ec 出参平均擦除次数（非 NULL）。
 *
 * @return true = 合法候选（出参有效）
 *         false = 跳过。
 *
 * @note **触发判据与选块必须用同一套过滤**，否则「触发时认为有候选、选块时
 * 却选不出」会让 WL 每轮空转（C2 / C9）：跳过 Open、跳过含坏 plane、
 * 跳过全哨兵条目、要求有效 CW ≥ WL_MIN_VALID_RATIO。
 */
static BOOL wl_candidate_get(const alc_s *alloc, const super_block_s *sb, alc_super_stats_s *out_stats, DWORD *out_ec)
{
    if (sb->is_open)
    {
        return false;
    }

    // 任一 present plane 的块已坏 ⇒ 不可搬：migrate 读它必然失败并 abort，
    // 而该 SB 仍可能是「有效 CW 最多」的候选 ⇒ 每轮重选、WL 永久卡死。
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (super_block_has_plane(sb, p) == false)
        {
            continue;
        }

        const WORD block  = sb->plane_blocks[p];
        const WORD gblock = ftl_gblock(p, block);

        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            return false;
        }
    }

    alc_super_stats_s st;

    alc_super_get_stats(alloc, sb, &st);
    if (st.plane_count == 0U)
    {
        return false; /* 全哨兵条目 */
    }

    // 门限单位是**有效 CW**，分母取**该条目自己的容量**（现存 plane × 每块 CW 数）：
    // 快照恢复出的单 plane 条目最多只有 FTL_CW_PER_BLOCK 个 CW，用整 SB 容量
    // 做分母会把它们全部拒之门外 ⇒ 每次快照启动后静态 WL 全程不触发。
    const DWORD total_cw     = (DWORD)st.plane_count * FTL_CW_PER_BLOCK;
    const DWORD min_valid_cw = (total_cw * WL_MIN_VALID_RATIO) / 100U;

    if ((DWORD)st.valid_cw < min_valid_cw)
    {
        return false; /* 太碎不值当搬 */
    }

    if (out_stats != NULL)
    {
        *out_stats = st;
    }
    *out_ec = st.erase_sum / st.plane_count;
    return true;
}

/* ── ec_spread ── */

// 契约见 wear_leveling.h。CLI `wear` 与 wl_trigger_is_due 共用本函数，
// 避免第二套擦除计数口径（C9）。
BOOL wl_ec_spread_get(const alc_s *alloc, DWORD total_writes, DWORD *out_max_free, DWORD *out_min_used)
{
    // 启动门：累计编程次数不足时不求值。这是一次性门，不是周期节拍；
    // 周期节拍由 mig_handle 的 WL 求值 的 WL_EVAL_INTERVAL_MS 负责。
    if (total_writes < WL_MIN_WRITES)
    {
        return false;
    }

    // e1：最冷的 used SB（Closed 列表平均擦除次数最小）。
    DWORD min_used = UINT32_MAX;

    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_s *sb    = alc_super_closed_get_at(alloc, i);
        DWORD                sb_ec = 0U;

        if (wl_candidate_get(alloc, sb, NULL, &sb_ec) == false)
        {
            continue;
        }
        if (sb_ec < min_used)
        {
            min_used = sb_ec;
        }
    }

    if (min_used == UINT32_MAX)
    {
        return false; /* 无可用 used 候选：判据无从比较 */
    }

    *out_max_free = alc_free_erase_limit(alloc);
    *out_min_used = min_used;
    return true;
}

/* ── trigger ── */

// 判据唯一：与 wl_ec_spread_get 同一漏斗。搬移不改变擦除计数，差值不足必须
// 停止，否则就是纯写放大。
BOOL wl_trigger_is_due(const alc_s *alloc, DWORD total_writes)
{
    DWORD max_free = 0U;
    DWORD min_used = 0U;

    if (wl_ec_spread_get(alloc, total_writes, &max_free, &min_used) == false)
    {
        return false;
    }

    return (max_free > min_used) && ((max_free - min_used) >= WL_THRESHOLD);
}

/* ── victim ── */

/**
 * @brief 选静态 WL 的搬运 victim（冷数据搬到高磨损块）。
 *
 * @param alloc  分配器。
 * @param victim 出参：选中的候选（plane_blocks / valid_cw / read_count / next_page）。
 *
 * @return true = 找到候选；false = 无合法候选。
 *
 * @note 排序键 = SB 平均擦除次数**最少** → 同擦除次数下 valid_cw 多
 *   （越冷越优先）→ 再按 read_sum 高（read disturb 风险高优先）。
 *   有效 CW 须 ≥ WL_MIN_VALID_RATIO（太碎不值当搬）。
 *   ⚠ 不按「低擦除区」过滤：低区阈值在低磨损量下恒真。
 */
BOOL wl_select_victim(const alc_s *alloc, wl_victim_s *victim)
{
    wl_victim_s best;
    memset(&best, 0, sizeof(best));

    DWORD best_ec = UINT32_MAX;

    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_s *sb = alc_super_closed_get_at(alloc, i);
        alc_super_stats_s    st;
        DWORD                sb_ec = 0U;

        if (wl_candidate_get(alloc, sb, &st, &sb_ec) == false)
        {
            continue;
        }

        // 排序键 = 平均擦除次数**最少** → 同擦除次数下 valid_cw 多（越冷越优先）
        // → 再按 read_sum 高（read disturb 风险高优先）。
        if ((sb_ec < best_ec) || ((sb_ec == best_ec) && ((st.valid_cw > best.valid_cw) || ((st.valid_cw == best.valid_cw) && (st.read_sum > best.read_count)))))
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

    if (best.found == false)
    {
        return false;
    }

    *victim = best;
    return true;
}
