#ifndef FTL_WEAR_LEVELING_H
#define FTL_WEAR_LEVELING_H

/*
 * Wear Leveling — 动态 + 静态磨损均衡
 *
 * 动态磨损均衡（Dynamic WL）：
 *   alc_select_plane_block_from_pool() 每次为**每个 plane 各自**从 Free Pool 选该 plane
 *   擦除次数最少的 Block（Policy B：各 plane 独立分配，不要求 block 同号），
 *   确保新分配的 Block 磨损均匀。已在 allocator 中实现。
 *
 * 静态磨损均衡（Static WL）：
 *   `mig_handle 的 WL 求值` 预算任务，按 WL_EVAL_INTERVAL_MS 节拍求值。
 *   主动搬移低擦除 Block 中的冷数据到 Cold Super Block，
 *   使这些 Block 被擦除一次，提升擦除次数。
 *
 * GC / WL 分工（本仓自拟的策略划分，不是某一份公开规范）：
 *   — GC 回收"有效页少"的 Block（回收空间）
 *   — WL 搬移"有效页多 + 擦除少"的 Block（均衡磨损）
 *   — 二者共用 migrate 引擎与同一收尾（擦完归还 Free Pool）：WL 的源块随后会被
 *     动态 WL 优先选中写入，擦除计数回升；**不退役**（退役只属于慢死预测/RR）
 */

#include "allocator.h" /* ftl_allocator_t（共享数据面账本，策略只读） */
#include "nand_config.h"
#include "ssd_types.h" /* bool */

#include <stdint.h>

/* ── WL 参数 ── */

#define WL_DELTA_THRESHOLD (5000U) /* max-min 差值触发 */
#define WL_MAX_AVG_RATIO   (3U)    /* max/avg 比值触发 */
#define WL_LOW_ZONE_WIDTH  (1000U) /* 低擦除区宽度 */
#define WL_LOW_BLOCK_PCT   (5U)    /* 低区 Block 占比阈值（代码判据是严格 >）*/
#define WL_MIN_WRITES      (1000U) /* 启动门：累计编程次数达到后才开始求值（一次性，不是周期）*/
#define WL_MIN_VALID_RATIO (70U)   /* 只搬有效 **CW** ≥ 70% 的冷 Block（CW 级映射） */

/* 静态 WL 的**求值节拍**（调度器 interval_ms）：三条件判定要扫全表，不能每轮都跑。
 * 静态 WL 本身是慢过程，5 s 一次足以收敛，同时把 4096 块扫描的 CPU 成本压到可忽略。 */
#define WL_EVAL_INTERVAL_MS (5000U)

/* ── WL Victim 选择结果 ── */

typedef struct
{
    bool  found;                   /* 是否找到 victim */
    WORD  plane_blocks[PLANE_NUM]; /* 各 plane Block 号 */
    WORD  valid_cw;                /* 有效 **CW** 数（各 plane valid_count 之和；CW 级映射） */
    DWORD read_count;              /* 各 plane read disturb 计数之和（冷块优先权重） */
} wl_victim_t;

/* ── 接口 ── */

/*
 * wl_should_trigger - 检查是否需要触发静态 WL
 *
 * 三条件 OR（**本仓自拟**：数值为本仓经验值，无公开出处）：
 *   条件 1：max_ec - min_ec ≥ WL_DELTA_THRESHOLD（5000）
 *   条件 2：max_ec / avg_ec ≥ WL_MAX_AVG_RATIO（3x）
 *   条件 3：低擦除区（min ~ min+1000）中的 Block 占比 **> 5%**（代码是严格大于）
 *
 * @param ftl  FTL 上下文
 * @return true 需要触发，false 不需要
 */
bool wl_should_trigger(const ftl_allocator_t *alloc, DWORD total_writes);

/*
 * wl_select_victim - 选择 WL victim Super Block
 *
 * 与 GC 相反：选有效 CW 最多 + 擦除次数最少的 Super Block。
 * 只遍历 closed_sb_list，跳过打开的 SB。
 *
 * @param ftl    FTL 上下文
 * @param victim 输出：选中的 victim
 * @return true 找到，false 无合适 victim
 */
bool wl_select_victim(const ftl_allocator_t *alloc, wl_victim_t *victim);

/*
 * ⚠ 搬移在 `ftl/migrate.h` 的异步 job 里完成：GC 与 WL 共用同一台引擎，
 *   `mig_begin_wl` / `mig_begin_predict` 是这两个站点的入口（本文件只留触发判定与选块策略）。
 */

/*
 * 磨损统计访问器 —— 口径（好块 / 坏块排除）由 allocator 定义，这里只做本模块的查询入口。
 * 热路径（wl_should_trigger）直接调 alc_get_erase_stats 一次取全，不逐个调用下面的 API。
 */

/*
 * wl_get_min_erase - 获取最小擦除次数（无好块返回 0）
 *
 * 唯一保留的统计访问器：wl_select_victim 用它确定低擦除区。
 * 热路径 wl_should_trigger 直接调 alc_get_erase_stats 一次取全。
 */
DWORD wl_get_min_erase(const ftl_allocator_t *alloc);

#endif /* FTL_WEAR_LEVELING_H */