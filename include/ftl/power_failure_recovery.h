#ifndef FTL_POWER_FAILURE_RECOVERY_H
#define FTL_POWER_FAILURE_RECOVERY_H

/*
 * Power Failure Recovery — 掉电恢复启动路径
 *
 * 三类启动场景：
 *
 *   Fresh Start（出厂首次启动）
 *     — 全无 Checkpoint/Journal → 全初始化
 *
 *   Clean Start（干净关机）
 *     — 有 Checkpoint，Journal 已全部回填到 L2P Table
 *     — 只需加载 Checkpoint header，不需要回放 Journal
 *
 *   Power Failure Recovery（异常掉电）
 *     — 有 Checkpoint，Journal 有未回放的条目（sequence > cp_seq）
 *     — ① 加载 Checkpoint ② Journal 回放 ③ 写指针扫描恢复
 *
 * 设计文档参考：§8.3 启动恢复完整流程
 */

#include "common/ssd_types.h"
#include "ftl/allocator.h"
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"

#include <stdint.h>

/* ── 前向声明 ── */
struct ftl_context;

/* ── 启动类型 ── */
typedef enum
{
    BOOT_FRESH_START = 0, /* 出厂首次启动 */
    BOOT_CLEAN_START,     /* 干净关机 */
    BOOT_POWER_FAILURE,   /* 异常掉电恢复 */
} ftl_boot_type_t;

/* ── 启动结果 ── */
typedef struct
{
    ftl_boot_type_t boot_type;       /* 启动类型 */
    uint32_t        cp_sequence;     /* Checkpoint sequence（0 = 无） */
    uint32_t        journal_entries; /* 回放的 Journal entry 数 */
} ftl_boot_result_t;

/* ── 接口 ── */

/*
 * ftl_boot_recovery - 主启动恢复入口
 *
 * 判断关机类型并执行对应恢复流程。
 * 在 fc_init() 之后、ftl_service() 开始之前调用。
 *
 * 流程：
 *   ① 扫描 Checkpoint（找最新有效 Header）
 *   ② 扫描 Journal（获取最后 sequence）
 *   ③ 判断类型 → fresh / clean / power failure
 *   ④ 对应恢复路径
 *
 * @param ftl  FTL 上下文（已部分初始化：ch/ce/lun）
 * @param result 输出：启动结果
 * @return SSD_STATUS_OK 成功，负值失败
 */
int ftl_boot_recovery(struct ftl_context *ftl, ftl_boot_result_t *result);

/*
 * ftl_boot_fresh_start - 出厂首次启动
 *
 * 无 Checkpoint、无 Journal，全初始化：
 *   ① L2P Cache 全 INVALID
 *   ② Allocator 初始化
 *   ③ Journal 初始化（空）
 *   ④ Metadata Directory 初始化（固定映射）
 *
 * @param ftl  FTL 上下文
 * @return SSD_STATUS_OK
 */
int ftl_boot_fresh_start(struct ftl_context *ftl);

/*
 * ftl_boot_clean_start - 干净关机启动
 *
 * Journal ≥ Checkpoint，无需回放：
 *   ① Cache 全 INVALID（首次 miss 时按需加载）
 *   ② Allocator 重建 closed SB 列表
 *   ③ 写指针恢复（内存值）
 *
 * @param ftl  FTL 上下文
 * @param cp_sequence  Checkpoint sequence
 * @return SSD_STATUS_OK
 */
int ftl_boot_clean_start(struct ftl_context *ftl, uint32_t cp_sequence);

/*
 * ftl_boot_power_failure - 异常掉电恢复
 *
 * Journal > Checkpoint，需回放未写到 L2P Table 的条目：
 *   ① Cache 全 INVALID
 *   ② Journal 回放 → 重建 L2P Cache
 *   ③ Allocator 重建 + 写指针扫描（异常掉电需扫描 NAND）
 *   ④ cp_sequence 记录
 *
 * @param ftl  FTL 上下文
 * @param cp_sequence  Checkpoint sequence（回放起点）
 * @param result       输出：回放的 entry 数
 * @return SSD_STATUS_OK 成功，负值失败
 */
int ftl_boot_power_failure(struct ftl_context *ftl,
                           uint32_t            cp_sequence,
                           ftl_boot_result_t  *result);

/*
 * ftl_boot_scan_write_pointer - 写指针扫描恢复
 *
 * 异常掉电时 Open Super Block 写到第 N 页时掉电，
 * 不知道当前写指针位置。扫描 NAND 找第一个全 0xFF 页。
 *
 * @param alloc  Allocator 上下文
 * @param ch     CH
 * @param ce     CE
 * @param sb     Open Super Block 指针
 */
void ftl_boot_scan_write_pointer(ftl_allocator_t *alloc,
                                 uint8_t          ch,
                                 uint8_t          ce,
                                 super_block_t   *sb);

#endif /* FTL_POWER_FAILURE_RECOVERY_H */