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

/* ── 掉电重建闭环的标志量（上板验证用，UART 可读） ── */
typedef struct
{
    uint16_t sb_rebuilt;      /* 重建的 Closed Super Block 数 */
    uint16_t partial_pairs;   /* 扫描到的部分写 Super Block 对数 */
    uint16_t valid_pages;     /* 重建回填的有效页总数 */
    uint16_t open_restored;   /* 恢复的 Open Super Block 数（0/1/2） */
} ftl_rebuild_stats_t;

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
ftl_status_t ftl_boot_recovery(struct ftl_context *ftl, ftl_boot_result_t *result);

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
 * @return FTL_OK 成功，FTL_ERROR 失败
 */
ftl_status_t ftl_boot_fresh_start(struct ftl_context *ftl);

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
 * @return FTL_OK 成功，FTL_ERROR 失败
 */
ftl_status_t ftl_boot_clean_start(struct ftl_context *ftl, uint32_t cp_sequence);

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
 * @return FTL_OK 成功，FTL_ERROR 失败
 */
ftl_status_t ftl_boot_power_failure(struct ftl_context *ftl,
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

/*
 * ftl_rebuild_closed_sb_list - 掉电重启后全 NAND 扫描重建 Closed SB 列表（F2）
 *
 * closed_sb_list 是纯 RAM 态，掉电即失。开机恢复（Clean/Power Failure）后
 * 已写满的 Super Block 不在列表里 → GC/WL/rr_predictive_scan 看不到 → 容量
 * 泄漏；且这些块还被 free pool 回填，可能被复用（覆盖未擦除半满块）。
 *
 * 本函数同步扫描全 NAND：
 *   ① 坏块 / 元数据块（ftl_is_metadata_block）跳过
 *   ② 逐 Plane 判空找写指针（LDPC_RAW + 全 0xFF）
 *   ③ 未写过（全 FF）→ 保持 FREE，跳过
 *   ④ 满块（双 Plane 全非空）→ allocator_add_closed_sb 进 Closed 列表，
 *      逐页 gc_read_page_with_lba 取 LBA + l2p_cache_read 反查回填 valid_map
 *      （只置 L2P 仍指向本页的真有效页，覆盖/torn 页不置位）
 *   ⑤ 部分写块 → 前 2 对恢复 Open SB，其余强制 Closed（GC 迁移自动清 torn）
 *   ⑥ 结尾重同步 pre_cond_count = free_count（拉出写过的块后）
 *
 * 调用时机（ftl_boot_recovery 内）：
 *   Clean Start → l2p_cache_init 之后（反查触发按需 region 加载，正确但慢）
 *   Power Failure → journal_replay 之后（cache 已含未回放 entry，零加载）
 *
 * @param ftl    FTL 上下文
 * @param stats  输出：重建统计（可为 NULL）
 * @return FTL_OK 成功，FTL_ERROR 失败
 */
ftl_status_t ftl_rebuild_closed_sb_list(struct ftl_context *ftl,
                                        ftl_rebuild_stats_t *stats);

#endif /* FTL_POWER_FAILURE_RECOVERY_H */