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
#include "checkpoint.h" /* cp_header_t */
#include "journal.h"
#include "l2p_cache.h"
#include "ssd_types.h"

#include <stdint.h>

/* ── 前向声明 ── */
struct ftl_context;

/* ── 掉电重建闭环的标志量（上板验证用，UART 可读） ── */
typedef struct
{
    WORD  blocks_rebuilt;  /* 重建的 Closed 条目数（= 扫到的写过物理块数，per-plane） */
    WORD  partial_blocks;  /* 其中部分写块数（写指针 < PAGES_PER_BLOCK，整块交 GC） */
    WORD  l2p_read_errors; /* 逐 CW 反查 L2P 的读失败次数（valid_cw 可能少算） */
    WORD  l2p_degraded_cw; /* 命中的「降级 region」CW 数：映射已丢，不再计 read_errors */
    DWORD valid_cw;        /* 重建回填的有效 **CW** 总数（全实例；最大 4096×256=1,048,576） */
} pfr_rebuild_stats_t;

/* ── 启动类型 ── */
typedef enum
{
    BOOT_FRESH_START = 0, /* 出厂首次启动 */
    BOOT_CLEAN_START,     /* 干净关机 */
    BOOT_POWER_FAILURE,   /* 异常掉电恢复 */
} pfr_boot_type_e;

/* ── 启动结果 ── */
typedef struct
{
    pfr_boot_type_e boot_type;       /* 启动类型 */
    DWORD           cp_sequence;     /* Checkpoint sequence（0 = 无） */
    DWORD           journal_entries; /* 回放的 Journal entry 数 */
    bool            blocks_restored; /* true = 块元数据快照已加载；false = full metadata rebuild 兜底 */
    bool            scoped;          /* true = 快照保留，但有降级 region 的旧映射丢失（范围化恢复） */
} pfr_boot_result_t;

/* ── 块元数据快照恢复（Checkpoint 的 16-bit/块 word）──
 * 格式编码在 checkpoint.h（记录格式的 owner）；恢复编排在本模块（开机恢复的 owner）。
 * 统计仅用于观测日志。 */
typedef struct
{
    WORD  closed_blocks;   /* 恢复出的 Closed 条目数（退化单 plane 计） */
    WORD  log_blocks;      /* 恢复出的活 LOG 块数 */
    WORD  anomalies;       /* 探测发现的「池块却有数据」块数 */
    WORD  l2p_read_errors; /* 探测中 L2P 反查失败次数（非 0 ⇒ 快照结果不完整，回退 full metadata rebuild） */
    WORD  l2p_degraded_cw; /* 命中的降级 region CW 数：只丢映射，不回退全量重建（RISK-BOOT-18） */
    DWORD valid_cw;        /* 探测补算出的有效 CW 数 */
} pfr_block_restore_stats_t;

/* ── full metadata rebuild / 块元数据恢复原语是**模块内部实现**（static，定义在 .c）──
 * 上游只能通过 ftl_boot_recovery 触发；full metadata rebuild 的判空/写指针/逐 CW 反查三者同源。 */

/* ── 接口 ── */

/*
 * ftl_media_check_format - 判定介质是否需要（重新）格式化（区分 I/O 错误）
 *
 * 判据来自 `cp_probe_media_status`：
 *   · 当前版本 CP        ⇒ 不格式化；
 *   · 空片 / 版本不符    ⇒ 需要格式化；
 *   · CP 读不通（IO_ERROR）⇒ **拒绝格式化**，返回 FTL_ERROR——一次瞬时读错不能变成
 *     整片擦除，由调用者停止启动/重试。
 *
 * @param ftl            FTL 上下文
 * @param out_need_format 输出：true = 需要格式化
 * @return FTL_OK 判定完成；FTL_ERROR CP 介质读不通（未格式化）
 */
ftl_status_e ftl_media_check_format(struct ftl_context *ftl, bool *out_need_format);

/*
 * ftl_media_format - 全量格式化（擦本实例全部块 + 复位易失元数据状态）
 *
 * ① `bbt_format_erase`（两阶段：先持久化当前坏块图 + FORMAT_IN_PROGRESS，再擦 BBT 池之外的块，
 *    最后 best-effort 并入新 BAD；阶段①失败则不擦用户块）
 * → ②（可选）`verify`：逐块「写图样 → 读回比对 → 再擦」，失败块标 BAD 并落盘。
 *    用于**出厂标记已丢失的二手片**重建可信坏块表（本仓自拟的体检实现；耗时数分钟）
 * → ③ `md_init` 复位 Directory
 * → ④ `cp_write_initial` 写当前版本标记。
 *
 * ⚠ 必须在 ftl_init 之后、free pool 回填之前调用（见 main.c ssd_ftl_boot）。
 * ⚠ `verify=true` 会显著拉长 format（约数分钟），自动格式化路径固定传 false。
 *
 * @param ftl    FTL 上下文
 * @param verify true = 追加全片读写体检（破坏性）
 * @return FTL_OK / FTL_ERROR
 */
ftl_status_e ftl_media_format(struct ftl_context *ftl, bool verify);

/*
 * ftl_boot_log_bad_areas - 按固定分区统计 BAD 块（只读 BST，不碰介质）
 *
 * 在每个实例 boot 完成、BAD 集合（BBT + CP 快照并集）已确定后调用一次。
 * 目的：区分坏块落在数据区还是元数据区——后者由 v11 候选块池 + shared metadata
 * spare 吸收（`RISK-BBT-12/13`）；本行是验证「元数据区是否开始出现 BAD」的基线。
 * ⚠ 这是**开机基线**：运行期新增的 BAD 不在此行体现（下一次开机才计入）。
 */
void ftl_boot_log_bad_areas(const struct ftl_context *ftl);

/*
 * ftl_boot_recovery - 主启动恢复入口
 *
 * 判断关机类型并执行对应恢复流程。
 * 在 fc_init() 之后、ftl_service() 开始之前调用。
 *
 * 流程：
 *   ① 扫描 Journal（jrn_scan_latest，获取最新页/sequence）
 *   ② 扫描 Checkpoint（cp_find_latest）
 *   ③ 判断类型 → fresh / clean / power failure
 *   ④ 对应恢复路径
 *
 * @param ftl  FTL 上下文（已部分初始化：ch/ce/lun）
 * @param result 输出：启动结果
 * @return FTL_OK 成功；FTL_ERROR 失败
 */
ftl_status_e ftl_boot_recovery(struct ftl_context *ftl, pfr_boot_result_t *result);

#endif /* FTL_POWER_FAILURE_RECOVERY_H */
