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
 *     — 块元数据快照有效 ⇒ 加载块元数据 + probe 池块 + 建池（不跑 F2）；否则 carve + F2
 *
 *   Power Failure Recovery（异常掉电）
 *     — 有 Checkpoint，Journal 有未回放的条目（sequence > cp_seq）
 *     — ① 块元数据快照恢复（若有效）② Journal 回放（映射 + valid 增量）③ F4
 *       ④ probe + build_pools；任一步不一致 ⇒ reset + carve + F2
 *
 * 设计文档参考：§8.3 启动恢复完整流程
 */

#include "common/ssd_types.h"
#include "ftl/allocator.h"
#include "ftl/checkpoint.h" /* l2p_checkpoint_header_t */
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"

#include <stdint.h>

/* ── 前向声明 ── */
struct ftl_context;

/* ── 掉电重建闭环的标志量（上板验证用，UART 可读） ── */
typedef struct
{
    uint16_t blocks_rebuilt; /* 重建的 Closed 条目数（= 扫到的写过物理块数，per-plane） */
    uint16_t partial_blocks; /* 其中部分写块数（写指针 < PAGES_PER_BLOCK，整块交 GC） */
    uint16_t valid_cw;       /* 重建回填的有效 **CW** 总数（逐 CW L2P 反查确认；双 plane 合计） */
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
    uint8_t         blocks_restored; /* 1 = 块元数据快照已加载；0 = F2 兜底 */
} ftl_boot_result_t;

/* ── 块元数据快照恢复（Checkpoint 的 16-bit/块 word）──
 * 格式编码在 checkpoint.h（记录格式的 owner）；恢复编排在本模块（开机恢复的 owner）。
 * 统计仅用于观测日志。 */
typedef struct
{
    uint16_t closed_blocks; /* 恢复出的 Closed 条目数（退化单 plane 计） */
    uint16_t log_blocks;    /* 恢复出的活 LOG 块数 */
    uint16_t free_blocks;   /* 探测中转回 Free 的块数 */
    uint16_t op_blocks;     /* OP 池块数（全 plane 合计） */
    uint16_t spare_blocks;  /* Spare 池块数（全 plane 合计） */
    uint16_t anomalies;     /* 探测发现的「池块却有数据」块数 */
    uint32_t valid_cw;      /* 探测补算出的有效 CW 数 */
} ftl_block_restore_stats_t;

bool ftl_recovery_restore_blocks(struct ftl_context *ftl, uint16_t words[FTL_BLOCK_TOTAL]);
void ftl_recovery_probe_blocks(struct ftl_context *ftl, uint16_t words[FTL_BLOCK_TOTAL], ftl_block_restore_stats_t *stats);
bool ftl_recovery_build_pools(struct ftl_context *ftl, const uint16_t words[FTL_BLOCK_TOTAL]);
void ftl_recovery_reset_blocks(struct ftl_context *ftl);

/* ── F2 / 块元数据探测共用的块原语 ──
 * 三者必须同源：F2 全盘重建与 块元数据快照 异常块补算用的是同一套判空/写指针/逐 CW 反查。 */
bool    ftl_recovery_page_is_blank(nand_pba_t pba, uint8_t *raw_buf);
uint8_t ftl_recovery_scan_write_pointer(const struct ftl_context *ftl, uint8_t plane, uint16_t block, uint8_t *raw_buf);
void    ftl_recovery_close_block(struct ftl_context  *ftl,
                                 uint8_t              plane,
                                 uint16_t             block,
                                 uint8_t              pages_written,
                                 uint8_t              temperature,
                                 ftl_rebuild_stats_t *stats);

/*
 * ftl_boot_carve_pools - 按出厂/几何顺序切分 Spare/OP/Free 池（无快照 或 快照失败时用）
 *
 * ⚠ 从 main.c 移入：块元数据快照恢复必须先于 carve，否则池条目会重复。
 */
ftl_status_t ftl_boot_carve_pools(struct ftl_context *ftl);

/* ── 接口 ── */

/*
 * ftl_media_needs_format - 判定介质是否需要（重新）格式化
 *
 * 判据 = 「本实例 checkpoint_find_latest 失败」。版本校验已在 checkpoint_header_valid
 * 内 ⇒ 该判据自动涵盖：有当前版本 Checkpoint（否）/ 有但版本不符（是）/ 完全没有（是）。
 *
 * @param ftl  FTL 上下文
 * @return true = 需要格式化；false = 介质已是当前布局版本
 */
bool ftl_media_needs_format(struct ftl_context *ftl);

/*
 * ftl_media_format - 全量格式化（擦本实例全部块 + 复位易失元数据状态）
 *
 * ① 擦全片（出厂坏块标记已在 bbt_init 读进 BST——**空片时**才读）→ ② bbt_save 重新持久化坏块图
 * → ③ metadata_dir_init 复位 Directory → ④ checkpoint_write_initial 写当前版本标记。
 *
 * ⚠ 必须在 ftl_init 之后、free pool 回填之前调用（见 main.c ssd_ftl_boot）。
 *
 * @param ftl  FTL 上下文
 * @return FTL_OK / FTL_ERROR
 */
ftl_status_t ftl_media_format(struct ftl_context *ftl);

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
 * 无 Checkpoint，映射全丢，全初始化：
 *   ① L2P Cache 全 INVALID
 *   ② F2 全盘重建 closed SB 列表
 *   ③ Allocator 初始化
 *   ④ Journal RAM 写态恢复（journal_resume，序号下限 1）
 *   ⑤ Metadata Directory 初始化（固定映射）
 *
 * @param ftl   FTL 上下文
 * @param ring  Journal 环形块池快照（journal_scan_latest 的结果）
 * @return FTL_OK 成功，FTL_ERROR 失败
 */
ftl_status_t ftl_boot_fresh_start(struct ftl_context *ftl, const journal_page_ref_t *ring);

/*
 * ftl_boot_clean_start - 干净关机启动
 *
 * Journal 已全在 Checkpoint 里（journal_seq ≤ cp.journal_sequence），无需回放：
 *   ① Cache 全 INVALID（首次 miss 时按需加载）
 *   ② Journal RAM 写态恢复（**关键**：环形块并没被擦，必须按 NAND 实际内容把
 *      写指针/序号接回 cp_journal_sequence，见 journal_resume）
 *   ③ 块元数据快照有效 ⇒ 恢复块元数据 + 池 + Closed 列表（probe 池块）；否则 carve + F2
 *   ④ Journal RAM 写态恢复（journal_resume，水位 = cp.journal_sequence）
 *
 * @param ftl          FTL 上下文
 * @param cp           最新 Checkpoint Header
 * @param blocks_valid    块元数据快照 是否已通过 checkpoint_load_block_table（1/0）
 * @param blocks_restored 输出：1 = 走 快照路径，0 = F2 兜底
 * @param ring         Journal 环形块池快照（journal_scan_latest 的结果）
 * @return FTL_OK 成功，FTL_ERROR 失败
 */
ftl_status_t ftl_boot_clean_start(struct ftl_context            *ftl,
                                  const l2p_checkpoint_header_t *cp,
                                  uint8_t                        blocks_valid,
                                  uint8_t                       *blocks_restored,
                                  const journal_page_ref_t      *ring);

/*
 * ftl_boot_power_failure - 异常掉电恢复
 *
 * Journal > Checkpoint，需回放未写到 L2P Table 的条目：
 *   ① Cache 全 INVALID
 *   ② 块元数据快照恢复（有效时）→ ③ Journal 回放（映射 + valid 增量 + materialize）
 *   ④ Checkpoint region_bitmap 应用（F4）
 *   ⑤ 块元数据快照收尾（probe + build_pools）；失败 ⇒ reset + carve + F2
 *
 * @param ftl       FTL 上下文
 * @param cp        最新 Checkpoint header（含 region_bitmap，回放起点 = journal_sequence）
 * @param blocks_valid 块元数据快照 是否已通过 checkpoint_load_block_table（1/0）
 * @param result    输出：回放的 entry 数 / 是否走 块元数据快照
 * @return FTL_OK 成功，FTL_ERROR 失败
 */
ftl_status_t ftl_boot_power_failure(struct ftl_context            *ftl,
                                    const l2p_checkpoint_header_t *cp,
                                    uint8_t                        blocks_valid,
                                    ftl_boot_result_t             *result);

/*
 * ftl_rebuild_closed_sb_list - 掉电重启后全 NAND 扫描重建 Closed SB 列表（F2）
 *
 * closed_sb_list 是纯 RAM 态，掉电即失。开机恢复（Clean/Power Failure）后
 * 已写满的 Super Block 不在列表里 → GC/WL/rr_predictive_scan 看不到 → 容量
 * 泄漏；且这些块还被 free pool 回填，可能被复用（覆盖未擦除半满块）。
 *
 * 本函数同步扫描全 NAND（**逐 plane 独立**，Policy B：两 plane 块不再同号）：
 *   ① 坏块 / 元数据块（ftl_is_metadata_block）跳过
 *   ② 判空找写指针（LDPC_RAW + 全 0xFF）
 *   ③ 未写过（全 FF）→ 保持 FREE，跳过
 *   ④ 写过的块 → allocator_add_closed_sb 进 Closed 列表（另一 plane 记
 *      FTL_BLOCK_NO_PLANE 哨兵），逐页 gc_read_page_with_lba 逐 CW 取 META LBA +
 *      l2p_cache_read 反查回填 valid_count（只计 L2P 仍指向本页本 CW 的真有效 CW）
 *   ⑤ 部分写块（掉电时未写满的 Open SB）同样进 Closed，交 GC 迁移——不再恢复 Open SB
 *      （per-block 下无法确定两个部分写块是否曾是一对）
 *   ⑥ 写过的块由 allocator_remove_from_free 从 free pool 拉出（free_count 自维护）
 *
 * 调用时机（ftl_boot_recovery 内）：
 *   Clean Start → l2p_cache_init 之后（反查触发按需 region 加载，正确但慢）
 *   Power Failure → journal_replay 之后（cache 已含未回放 entry，零加载）
 *                   （F10：journal_replay 全量回放 post-CP 条目）
 *
 * @param ftl    FTL 上下文
 * @param stats  输出：重建统计（可为 NULL）
 * @return FTL_OK 成功，FTL_ERROR 失败
 */
ftl_status_t ftl_rebuild_closed_sb_list(struct ftl_context  *ftl,
                                        ftl_rebuild_stats_t *stats);

#endif /* FTL_POWER_FAILURE_RECOVERY_H */