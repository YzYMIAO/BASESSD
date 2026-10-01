#ifndef FTL_READ_RECOVERY_H
#define FTL_READ_RECOVERY_H

/*
 * Read Recovery — 读取失败恢复 + ECC 趋势预测
 *
 * 企业级 SSD 在读取失败时不是立即返回错误，而是走多级恢复管线：
 *
 *   Stage 1 — Read Retry（读重试）
 *     切换 NAND 读电压/时序参数，重试读取。
 *     ONFI 标准通过 SET_FEATURES 写入 Read Retry 模式。
 *
 *   Stage 2 — 邻近页辅助
 *     读取目标页的上下邻居页，利用 NAND 单元间电压干扰
 *     的物理特性辅助纠错。适用于 ECC 略超纠错上限的场景。
 *
 *   Stage 3 — XOR/RAID 恢复（后续阶段，当前未实现）
 *     跨 Plane 的 Stripe RAID 数据恢复。
 *
 *   Stage 4 — 坏块标记 + 数据搬移
 *     确定 Block 不可靠后，标记坏块并搬移剩余有效数据。
 *
 * ECC 趋势预测：
 *   ECC 连续 N 次上升 → 该 Block 进入"慢死"（Slow Death）状态
 *   → 在数据彻底损坏前主动搬移。
 *
 * 当前实现：
 *   ① ECC 趋势跟踪（per-Block bit 计数）
 *   ② 预测性坏块检测
 *   ③ Read Retry 重试循环（通过 FC GET/SET_FEATURES）
 *   ④ 读失败后的恢复调度入口
 */

#include "ftl/allocator.h"
#include "ftl/bst.h"        /* rr_ecc_track_t, rr_block_health_t, RR_ECC_HISTORY_DEPTH, ftl_block_t */
#include "ftl/ftl_type.h"
#include "ftl/l2p_cache.h"
#include "nand/nand_config.h"

#include <stdint.h>

/* ── ECC 趋势参数 ── */
#define RR_ECC_RISING_THRESHOLD (4U)  /* 连续上升 ≥4 次触发预测 */
#define RR_ECC_HIGH_WATER       (6U)  /* 单次 ECC ≥6 bits → 告警 */
#define RR_ECC_MAX              (15U) /* BCH t=4 最大可纠 bit 数（保守上限）*/
#define RR_MAX_RETRY            (8U)  /* 最大 read retry 次数 */
#define RR_NEIGHBOR_ATTEMPTS    (2U)  /* 邻近页辅助尝试次数 */

/* ── Read Retry ONFI Feature Address ── */
#define RR_FEATURE_READ_RETRY      (0x89U) /* Micron/ONFI Read Retry */
#define RR_FEATURE_READ_RETRY_MODE (0x00U) /* Mode 0 = normal, 1-7 = retry modes */

/* 预测性坏块状态 rr_block_health_t / ECC 历史 rr_ecc_track_t 已迁入 ftl/bst.h */

/* ── Read Recovery 上下文 ──
 * ECC 追踪数组已并入 ftl_block_t.rr（Block State Table 单数据源），
 * 本上下文只保留重试计数。 */
typedef struct
{
    uint8_t retry_count; /* 当前重试计数 */
} ftl_read_recovery_t;

/* ── 接口 ── */

/*
 * rr_record_ecc - 记录一次读取的 ECC bit 数，更新趋势
 *
 * 企业级做法：每次 read 完成后记录 ecc_bits，
 * 连续上升 → warning → critical → 触发搬移。
 *
 * @param track       per-Block ECC 追踪（&ftl->alloc.blocks[gblock].rr）
 * @param ecc_bits    本次读取的 ECC 纠正 bit 数（取 4 CW 中的最大值）
 * @param total_writes 当前写入总次数（用于时间戳）
 */
void rr_record_ecc(rr_ecc_track_t *track,
                   uint8_t         ecc_bits,
                   uint32_t        total_writes);

/*
 * rr_recover_read - 读取失败后的恢复尝试
 *
 * 多级恢复管线：
 *   ① 标准 read retry（切换读电压模式）
 *   ② 邻近页辅助读取（读 +1/-1 页辅助纠错）
 *   ③ 恢复失败 → 调用方标记坏块 + 搬移（XOR/RAID 恢复后续阶段实现）
 *
 * @param rr       Read Recovery 上下文
 * @param ch       CH
 * @param ce       CE
 * @param pba      读出错的物理地址
 * @param buffer   输出 buffer
 * @param length   读取长度
 * @param lba      LBA（META 验证）
 * @param retry_count 输出：重试次数
 * @return 0 恢复成功，-1 恢复失败（调用方标记坏块，XOR/RAID 恢复待后续阶段）
 */
int rr_recover_read(ftl_read_recovery_t *rr,
                    uint8_t              ch,
                    uint8_t              ce,
                    nand_pba_t           pba,
                    void                *buffer,
                    uint32_t             length,
                    lba_t                lba,
                    uint8_t             *retry_count);

/*
 * rr_check_predictive - 检查是否需要预测性搬移
 *
 * ECC 趋势连续上升 ≥ RR_ECC_RISING_THRESHOLD 次
 * 或单次 ECC ≥ RR_ECC_HIGH_WATER → 返回该 Block。
 *
 * @param blocks    Block State Table（全表）
 * @param max_blocks 表大小（FTL_BLOCK_TOTAL）
 * @param gblock    输出：需搬移的全局 Block 编号
 * @return 1 需要搬移，0 不需要
 */
int rr_check_predictive(const ftl_block_t *blocks, uint16_t max_blocks, uint16_t *gblock);

/*
 * rr_get_health - 获取 Block 健康状态
 *
 * @param track per-Block ECC 追踪（&blocks[gblock].rr）
 * @return rr_block_health_t
 */
rr_block_health_t rr_get_health(const rr_ecc_track_t *track);

#endif /* FTL_READ_RECOVERY_H */