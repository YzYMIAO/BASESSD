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
 *     — 块元数据快照有效 ⇒ 加载块元数据 + probe 池块 + 建池（不跑 full metadata rebuild）；否则 full metadata rebuild → carve
 *
 *   Power Failure Recovery（异常掉电）
 *     — 有 Checkpoint，Journal 有未回放的条目（sequence > cp_seq）
 *     — ① 块元数据快照恢复（若有效）② Journal 回放（映射 + valid 增量）③ F4
 *       ④ probe + build_pools；任一步不一致 ⇒ reset → full metadata rebuild → carve
 *
 * 设计文档参考：§8.3 启动恢复完整流程
 */

#include "allocator.h"
#include "checkpoint.h" /* cp_header_s */
#include "ftl_type.h"   /* ftl_ctx_s */
#include "journal.h"
#include "l2p_cache.h"
#include "ssd_types.h"

#include <stdint.h>

/* ── 启动类型 ── */
typedef enum PFR_BOOT_TYPE_E
{
    BOOT_FRESH_START = 0, /* 出厂首次启动 */
    BOOT_CLEAN_START,     /* 干净关机 */
    BOOT_POWER_FAILURE,   /* 异常掉电恢复 */
} pfr_boot_type_e;

/* ── 启动结果 ── */
typedef struct PFR_BOOT_RESULT_S
{
    pfr_boot_type_e boot_type;       /* 启动类型 */
    DWORD           cp_sequence;     /* Checkpoint sequence（0 = 无） */
    DWORD           journal_entries; /* 回放的 Journal entry 数 */
    BOOL            blocks_restored; /* true = 块元数据快照已加载；false = full metadata rebuild 兜底 */
    BOOL            scoped;          /* true = 快照保留，但有降级 region 的旧映射丢失（范围化恢复） */
} pfr_boot_result_s;

/* ── 接口 ── */

/**
 * @brief 判定介质是否需要（重新）格式化（区分 I/O 错误）
 *
 * 判据来自 `cp_media_status_probe`：
 *   · 当前版本 CP        ⇒ 不格式化；
 *   · 空片 / 版本不符    ⇒ 需要格式化；
 *   · CP 读不通（IO_ERROR）⇒ **拒绝格式化**，返回 FTL_ERROR——一次瞬时读错不能变成
 *     整片擦除，由调用者停止启动/重试。
 *
 * @param ftl            FTL 上下文
 * @param out_need_format 输出：true = 需要格式化
 * @return FTL_OK 判定完成；FTL_ERROR CP 介质读不通（未格式化）
 */
ftl_status_e pfr_media_check_format(ftl_ctx_s *ftl, BOOL *out_need_format);

/**
 * @brief 全量格式化（擦本实例全部块 + 复位易失元数据状态）
 *
 * ① `bbt_format_erase`（两阶段：先持久化当前坏块图 + FORMAT_IN_PROGRESS，再擦 BBT 池之外的块，
 *    最后 best-effort 并入新 BAD；阶段①失败则不擦用户块）
 * → ②（可选）`verify`：逐块擦除体检，擦除失败即标 BAD 并落盘。
 *    用于**出厂标记已丢失的二手片**重建可信坏块表（本仓自拟的体检实现；耗时数分钟）
 * → ③ `md_init` 复位 Directory
 * → ④ `cp_initial_record_write` 写当前版本标记。
 *
 * ⚠ 必须在 ftl_init 之后、free pool 回填之前调用（见 main.c ssd_ftl_boot）。
 * ⚠ `verify=true` 会显著拉长 format（约数分钟），自动格式化路径固定传 false。
 *
 * @param ftl    FTL 上下文
 * @param verify true = 追加全片擦除体检（破坏性）
 * @return FTL_OK / FTL_ERROR
 */
ftl_status_e pfr_media_format(ftl_ctx_s *ftl, BOOL verify);

/**
 * @brief 主启动恢复入口
 *
 * 判断关机类型并执行对应恢复流程。
 * 在 fc_init() 之后、ftl_service() 开始之前调用。
 *
 * 流程：
 *   ① 扫描 Journal（jrn_page_scan_latest）与 Checkpoint（cp_pool_find_latest）
 *   ② 判型产出恢复计划（fresh / snapshot / replay / force-rebuild）
 *   ③ 按计划执行；快照不可用 ⇒ full metadata rebuild 兜底
 *
 * @param ftl  FTL 上下文（已部分初始化：ch/ce/lun）
 * @param result 输出：启动结果
 * @return FTL_OK 成功；FTL_ERROR 失败
 */
ftl_status_e pfr_boot_recovery(ftl_ctx_s *ftl, pfr_boot_result_s *result);

/**
 * @brief 按固定分区统计 BAD 块（只读 BST，不碰介质）
 *
 * 在每个实例 boot 完成、BAD 集合（BBT + CP 快照并集）已确定后调用一次。
 * 目的：区分坏块落在数据区还是元数据区——后者由固定元数据区候选块池 + shared metadata
 * spare 吸收（``）；本行是验证「元数据区是否开始出现 BAD」的基线。
 * ⚠ 这是**开机基线**：运行期新增的 BAD 不在此行体现（下一次开机才计入）。
 *
 * @param ftl 只读：从 BST 统计各分区（dir_ckpt/cp/bbt/journal/l2p_log/spare/data）的 BAD 块数
 */
void pfr_boot_log_bad_areas(const ftl_ctx_s *ftl);

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 测试专用：扫一个物理块的写指针（BOOT-06 hole 判据）。
 */
BYTE pfr_test_scan_write_pointer(const ftl_ctx_s *ftl, BYTE plane, WORD block);
#endif

#endif /* FTL_POWER_FAILURE_RECOVERY_H */
