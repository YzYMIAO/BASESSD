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
 *     — 块元数据快照有效 ⇒ 加载块元数据 + probe 池块 + 建池（不跑 F2）；否则 F2 → carve
 *
 *   Power Failure Recovery（异常掉电）
 *     — 有 Checkpoint，Journal 有未回放的条目（sequence > cp_seq）
 *     — ① 块元数据快照恢复（若有效）② Journal 回放（映射 + valid 增量）③ F4
 *       ④ probe + build_pools；任一步不一致 ⇒ reset → F2 → carve
 *
 * 设计文档参考：§8.3 启动恢复完整流程
 */

#include "allocator.h"
#include "checkpoint.h" /* l2p_checkpoint_header_t */
#include "journal.h"
#include "l2p_cache.h"
#include "ssd_types.h"

#include <stdint.h>

/* ── 前向声明 ── */
struct ftl_context;

/* ── 掉电重建闭环的标志量（上板验证用，UART 可读） ── */
typedef struct
{
    uint16_t blocks_rebuilt; /* 重建的 Closed 条目数（= 扫到的写过物理块数，per-plane） */
    uint16_t partial_blocks; /* 其中部分写块数（写指针 < PAGES_PER_BLOCK，整块交 GC） */
    uint32_t valid_cw;       /* 重建回填的有效 **CW** 总数（全实例；最大 4096×256=1,048,576） */
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
 * ftl_boot_carve_pools - 按几何顺序切分 Spare/OP/Free 池（无快照或快照失败时用）
 *
 * **只把 state == NONE 的块入池**：F2 / 快照已认领的 CLOSED/LOG/RETIRED 块只计入
 * `alloc->total_blocks`，绝不进池——池的契约是“空闲且已擦除”。
 * 因此调用顺序必须是 **F2/快照 → carve**；反过来会让同一块同时是池成员与非空闲块。
 */
ftl_status_t ftl_boot_carve_pools(struct ftl_context *ftl);

/* ── 接口 ── */

/*
 * ftl_media_check_format - 判定介质是否需要（重新）格式化（区分 I/O 错误）
 *
 * 判据来自 `checkpoint_probe_media_status`：
 *   · 当前版本 CP        ⇒ 不格式化；
 *   · 空片 / 版本不符    ⇒ 需要格式化；
 *   · CP 读不通（IO_ERROR）⇒ **拒绝格式化**，返回 FTL_ERROR——一次瞬时读错不能变成
 *     整片擦除，由调用者停止启动/重试。
 *
 * @param ftl            FTL 上下文
 * @param out_need_format 输出：true = 需要格式化
 * @return FTL_OK 判定完成；FTL_ERROR CP 介质读不通（未格式化）
 */
ftl_status_t ftl_media_check_format(struct ftl_context *ftl, bool *out_need_format);

/*
 * ftl_media_format - 全量格式化（擦本实例全部块 + 复位易失元数据状态）
 *
 * ① `bbt_format_erase`（两阶段：先持久化当前坏块图 + FORMAT_IN_PROGRESS，再擦 BBT 池之外的块，
 *    最后 best-effort 并入新 BAD；阶段①失败则不擦用户块）→ ② `metadata_dir_init` 复位 Directory
 * → ③ `checkpoint_write_initial` 写当前版本标记。
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
 *   ① 扫描 Journal（journal_scan_latest，获取最新页/sequence）
 *   ② 扫描 Checkpoint（checkpoint_find_latest）
 *   ③ 判断类型 → fresh / clean / power failure
 *   ④ 对应恢复路径
 *
 * @param ftl  FTL 上下文（已部分初始化：ch/ce/lun）
 * @param result 输出：启动结果
 * @return FTL_OK 成功；FTL_ERROR 失败
 */
ftl_status_t ftl_boot_recovery(struct ftl_context *ftl, ftl_boot_result_t *result);

/*
 * ftl_boot_fresh_start - 出厂首次启动
 *
 * 无 Checkpoint，映射全丢，全初始化：
 *   ① 失效本 owner 的 L2P cache 行/脏位
 *   ② F2 全盘重建 Closed SB 列表
 *   ③ carve：只把 state==NONE 的块放进 Spare/OP/Free，并从 Free 补 Spare
 *   ④ journal_resume（序号下限 1）
 *   （Allocator / Metadata Directory 已由 ftl_init 初始化，不在本函数内）
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
 *   ① Cache 已由设备级 init 置空；本函数按 CP bitmap 约束本 owner 的行（F4）
 *   ② 块元数据快照有效 ⇒ restore + F4 + probe + build_pools；
 *      否则 reset → F2（认领写过块）→ carve（只把 state==NONE 的块入池）
 *   ③ journal_resume 接回写指针/序号（环形块未被擦；水位 = cp.journal_sequence）
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
 *   ① 共享 cache 已由设备级 init 置空
 *   ② 块元数据快照恢复（有效时）
 *   ③ Journal 回放（映射 + valid 增量 + materialize）
 *   ④ F4：按 CP region_bitmap **逐 region 预载**（不按 INVALID 行的 region_id 反查）
 *   ⑤ probe + build_pools + Spare 补池；失败 ⇒ reset → F2 → carve
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
 * @param ftl    FTL 上下文（前置条件，ASSERT）
 * @param stats  输出：重建统计（可为 NULL）
 * @return 无。这是最后的兜底重建；失败只表现为 valid_count 少算（LOGW/断言）。
 */
void ftl_rebuild_closed_sb_list(struct ftl_context *ftl, ftl_rebuild_stats_t *stats);

#endif /* FTL_POWER_FAILURE_RECOVERY_H */