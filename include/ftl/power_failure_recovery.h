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
    uint16_t blocks_rebuilt;  /* 重建的 Closed 条目数（= 扫到的写过物理块数，per-plane） */
    uint16_t partial_blocks;  /* 其中部分写块数（写指针 < PAGES_PER_BLOCK，整块交 GC） */
    uint16_t l2p_read_errors; /* 逐 CW 反查 L2P 的读失败次数（valid_cw 可能少算） */
    uint32_t valid_cw;        /* 重建回填的有效 **CW** 总数（全实例；最大 4096×256=1,048,576） */
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
    uint8_t         blocks_restored; /* 1 = 块元数据快照已加载；0 = full metadata rebuild 兜底 */
} ftl_boot_result_t;

/* ── 块元数据快照恢复（Checkpoint 的 16-bit/块 word）──
 * 格式编码在 checkpoint.h（记录格式的 owner）；恢复编排在本模块（开机恢复的 owner）。
 * 统计仅用于观测日志。 */
typedef struct
{
    uint16_t closed_blocks;   /* 恢复出的 Closed 条目数（退化单 plane 计） */
    uint16_t log_blocks;      /* 恢复出的活 LOG 块数 */
    uint16_t anomalies;       /* 探测发现的「池块却有数据」块数 */
    uint16_t l2p_read_errors; /* 探测中 L2P 反查失败次数（非 0 ⇒ 快照结果不完整，回退 full metadata rebuild） */
    uint32_t valid_cw;        /* 探测补算出的有效 CW 数 */
} ftl_block_restore_stats_t;

/* ── full metadata rebuild / 块元数据恢复原语是**模块内部实现**（static，定义在 .c）──
 * 上游只能通过 ftl_boot_recovery 触发；full metadata rebuild 的判空/写指针/逐 CW 反查三者同源。 */

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

#endif /* FTL_POWER_FAILURE_RECOVERY_H */
