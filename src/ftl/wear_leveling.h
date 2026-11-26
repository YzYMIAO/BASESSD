#ifndef FTL_WEAR_LEVELING_H
#define FTL_WEAR_LEVELING_H

/*
 * Wear Leveling — 动态 + 静态磨损均衡
 *
 * 动态磨损均衡（Dynamic WL）：
 *   alc_free_find_block() 每次为**每个 plane 各自**从 Free Pool 选该 plane
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
 *     动态 WL 优先选中写入，擦除计数回升；**不退役**（退役只属于预测性退役/RR）
 */

#include "allocator.h" /* alc_s（共享数据面账本，策略只读） */
#include "nand_config.h"
#include "ssd_types.h" /* BOOL */

#include <stdint.h>

/* ── WL 参数 ── */

#define WL_THRESHOLD       (200U)  /* 触发差值：高擦除空闲块 − 最冷 used SB（本仓自拟，见下） */
#define WL_MIN_WRITES      (1000U) /* 启动门：累计编程次数达到后才开始求值（一次性，不是周期）*/
#define WL_MIN_VALID_RATIO (70U)   /* 只搬有效 **CW** ≥ 该条目容量 70% 的冷 Block（CW 级映射；单 plane 条目按自身容量算） */
/* ⚠ WL_THRESHOLD 的出处与缩放：
 *   Linux MTD UBI 的 `MTD_UBI_WL_THRESHOLD` 默认 4096，含义是「最高与最低擦除计数
 *   的最大差值」，超过即把最冷 used PEB 搬到高擦除空闲 PEB
 *   （drivers/mtd/ubi/Kconfig / wl.c）。默认值面向高耐久 SLC；本器件无本地 P/E 一手
 *   数据，按本仓磨损量级取 200（本仓自拟，不是器件规格）。上板观察后可调。 */

/* 静态 WL 的**求值节拍**（由 migrate handle 自持，不在后台调度器任务表里）：判据要扫 Closed 列表 + Free Pool，
 * 不能每轮都跑。静态 WL 本身是慢过程，5 s 一次足以收敛，同时把 4096 块扫描的
 * CPU 成本压到可忽略。 */
#define WL_EVAL_INTERVAL_MS (5000U)

/* ── WL Victim 选择结果 ── */

typedef struct WL_VICTIM_S
{
    BOOL  found;                   /* 是否找到 victim */
    WORD  plane_blocks[PLANE_NUM]; /* 各 plane Block 号 */
    BYTE  next_page;               /* 该 SB 已编程页数上界（搬移只读 [0, next_page)） */
    WORD  valid_cw;                /* 有效 **CW** 数（各 plane valid_count 之和；CW 级映射） */
    DWORD read_count;              /* 各 plane read disturb 计数之和（冷块优先权重） */
} wl_victim_s;

/* ── 接口 ── */

/**
 * @brief 取 WL 触发判据的两个擦除计数（与 wl_trigger_is_due 同一漏斗）。
 *
 * @note 只求值、不判定；供 `wl_trigger_is_due` 与 CLI `wear` 共用，避免第二套
 *       擦除计数口径（C9）。
 */
BOOL wl_ec_spread_get(const alc_s *alloc, DWORD total_writes, DWORD *out_max_free, DWORD *out_min_used);

/**
 * @brief 检查是否需要触发静态 WL（UBI 模型）。
 *
 * @note 判据：`max_free_ec − min_used_ec ≥ WL_THRESHOLD`；min_used 取 Closed SB 平均
 *       擦除次数最小者，max_free 取 Free Pool 最大 erase_count。搬移本身不改变擦除
 *       计数，差值不满足时必须停止。
 */
BOOL wl_trigger_is_due(const alc_s *alloc, DWORD total_writes);

/**
 * @brief 选择 WL victim Super Block。
 *
 * @note 与 GC 相反：选平均擦除次数最少（最冷）且有效 CW ≥ 70% 的 Closed SB
 *       （UBI 的 e1）；同擦除次数下按 valid_cw 多、read_sum 高排序。只遍历
 *       closed_sb_list，跳过打开的 SB 与含坏 plane 的 SB。
 */
BOOL wl_select_victim(const alc_s *alloc, wl_victim_s *victim);

/*
 * ⚠ 搬移在 `ftl/migrate.h` 的异步 job 里完成：GC 与 WL 共用同一台引擎，
 *   `mig_wl_begin` / `mig_predict_begin` 是这两个站点的入口（本文件只留触发判定与选块策略）。
 */

/*
 * 磨损统计 —— 口径（好块 / 坏块排除）由 allocator 定义；候选项过滤（Open / 坏 plane /
 * 有效 CW 门限）收敛在 `wl_candidate_get`，Free Pool 最大擦除值走 allocator 的
 * `alc_free_erase_limit`。
 */

#endif /* FTL_WEAR_LEVELING_H */