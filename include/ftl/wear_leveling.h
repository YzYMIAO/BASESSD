#ifndef FTL_WEAR_LEVELING_H
#define FTL_WEAR_LEVELING_H

/*
 * Wear Leveling — 动态 + 静态磨损均衡
 *
 * 动态磨损均衡（Dynamic WL）：
 *   allocator_select_plane_block_from_pool() 每次为**每个 plane 各自**从 Free Pool 选该 plane
 *   擦除次数最少的 Block（Policy B：两 plane 独立分配，不再要求 block 同号），
 *   确保新分配的 Block 磨损均匀。已在 allocator 中实现。
 *
 * 静态磨损均衡（Static WL）：
 *   独立 Daemon，在 ftl_service() Phase 3 中运行。
 *   主动搬移低擦除 Block 中的冷数据到 Cold Super Block，
 *   使这些 Block 被擦除一次，提升擦除次数。
 *
 * 三星企业级方案参考：
 *   — GC 回收的是"有效页少"的 Block（回收空间）
 *   — WL 搬移的是"有效页多 + 擦除少"的 Block（均衡磨损）
 *   — 二者独立运行，互不干扰
 */

#include "common/ssd_types.h"
#include "nand/nand_config.h"

#include <stdint.h>

/* ── 前向声明（ftl_context_t 在 ftl.h 中定义，.c 文件需先 include ftl.h） ── */
struct ftl_context;

/* ── WL 参数 ── */

#define WL_DELTA_THRESHOLD (5000U) /* max-min 差值触发 */
#define WL_MAX_AVG_RATIO   (3U)    /* max/avg 比值触发 */
#define WL_LOW_ZONE_WIDTH  (1000U) /* 低擦除区宽度 */
#define WL_LOW_BLOCK_PCT   (5U)    /* 低区 Block 占比阈值 */
#define WL_MIN_INTERVAL    (1000U) /* 最小间隔（写入次数） */
#define WL_MIN_VALID_RATIO (70U)   /* 只搬有效 **CW** ≥ 70% 的冷 Block（CW 级映射） */

/* ── WL Victim 选择结果 ── */

typedef struct
{
    uint8_t  found : 1;               /* 是否找到 victim */
    uint16_t plane_blocks[PLANE_NUM]; /* 双 Plane Block 号 */
    uint32_t avg_erase;               /* victim 平均擦除次数 */
    uint16_t valid_cw;                /* 有效 **CW** 数（双 Plane valid_count 之和；CW 级映射） */
    uint32_t read_count;              /* 双 Plane read disturb 计数之和（B5 冷块优先权重） */
} wl_victim_t;

/* ── 接口 ── */

/*
 * wl_should_trigger - 检查是否需要触发静态 WL
 *
 * 三条件 OR（三星企业级方案）：
 *   条件 1：max_ec - min_ec ≥ WL_DELTA_THRESHOLD（5000）
 *   条件 2：max_ec / avg_ec ≥ WL_MAX_AVG_RATIO（3x）
 *   条件 3：低擦除区（min ~ min+1000）中的 Block 占比 ≥ 5%
 *
 * @param ftl  FTL 上下文
 * @return 1 需要触发，0 不需要
 */
int wl_should_trigger(const struct ftl_context *ftl);

/*
 * wl_select_victim - 选择 WL victim Super Block
 *
 * 与 GC 相反：选有效 CW 最多 + 擦除次数最少的 Super Block。
 * 只遍历 closed_sb_list，跳过打开的 SB。
 *
 * @param ftl    FTL 上下文
 * @param victim 输出：选中的 victim
 * @return 1 找到，0 无合适 victim
 */
int wl_select_victim(const struct ftl_context *ftl, wl_victim_t *victim);

/*
 * wl_relocate - 搬移 WL victim 的所有有效数据
 *
 * 流程（与 GC 搬移一致）：
 *   ① 逐页索引遍历双 Plane，**逐 CW** 查 L2P 判有效性
 *   ② 读页（`gc_read_page_with_lba`，容忍页内空洞）→ 逐 CW 反查 L2P 确认有效
 *   ③ 逐页索引**成对**分配 Cold Super Block 新位置，只编有效 CW（`ftl_sync_write_page_cw`）
 *   ④ 更新 L2P Cache + Journal 记录
 *   ⑤ 擦除双 Block + 递增擦除计数
 *   ⑥ 归还 Free Pool
 *
 * @param ftl    FTL 上下文
 * @param victim 要搬移的 victim
 * @return SSD_STATUS_OK 成功，SSD_STATUS_IO_ERROR 失败
 */
int wl_relocate(struct ftl_context *ftl, const wl_victim_t *victim);

/*
 * wl_execute - 执行一轮静态 WL（should_trigger + select + relocate）
 *
 * @param ftl  FTL 上下文
 * @return 搬移的页数（>0 = 执行了搬移），0 = 未执行
 */
int wl_execute(struct ftl_context *ftl);

/*
 * wl_max_erase - 获取最大擦除次数
 */
uint32_t wl_max_erase(const struct ftl_context *ftl);

/*
 * wl_min_erase - 获取最小擦除次数
 */
uint32_t wl_min_erase(const struct ftl_context *ftl);

/*
 * wl_avg_erase - 获取平均擦除次数
 */
uint32_t wl_avg_erase(const struct ftl_context *ftl);

/*
 * wl_count_blocks_below - 统计擦除次数低于阈值的 Block 数
 */
uint32_t wl_count_blocks_below(const struct ftl_context *ftl, uint32_t threshold);

/*
 * wl_predictive_relocate - 慢死预测巡检 + 整体搬移
 *
 * 企业级 Predictive Failures 做法：后台周期扫 Closed 块 ECC 健康，
 * 命中 health ≥ CRITICAL 的慢死块 → 整 Super Block 搬移（wl_relocate）
 * → 双 plane 块退役（不再分配新数据，不标坏）。
 * 接入 ftl_service_idle（带时间节流）。
 *
 * @param ftl  FTL 上下文
 * @return 1 完成一轮搬移，0 无命中
 */
int wl_predictive_relocate(struct ftl_context *ftl);

#endif /* FTL_WEAR_LEVELING_H */