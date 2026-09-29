#ifndef FTL_CHECKPOINT_H
#define FTL_CHECKPOINT_H

/*
 * Checkpoint — L2P 快照 + Journal 回收协同
 *
 * Checkpoint 定期 flush 所有 dirty L2P Cache → NAND L2P Table，
 * 写 Checkpoint Header（含当前 journal_sequence），截断 Journal。
 *
 * Journal 中 sequence ≤ cp_journal_sequence 的条目已全部写入 L2P Table，
 * 可在 Journal 回切时安全擦除。
 *
 * Store: Block 37-38（固定），各 3 页冗余
 */

#include "ftl/allocator.h"
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"

#include <stdint.h>

/* ── 前向声明（ftl_context_t 在 ftl.h 中定义，ftl.h 会 include 本文件） ── */
struct ftl_context;

/* ── Checkpoint Block 位置 ── */
#define CHECKPOINT_BLOCK0      (37U)
#define CHECKPOINT_BLOCK1      (38U)
#define CHECKPOINT_REDUNDANCY  (3U) /* 每 Block 3 页 */
#define CHECKPOINT_TOTAL_PAGES (6U)

/* ── Header Magic ── */
#define CHECKPOINT_MAGIC (0xCCCCCCCCUL)

/* ── Header 格式（~920B < 2048B） ── */
typedef struct
{
    uint32_t magic;                     /* CHECKPOINT_MAGIC */
    uint32_t sequence;                  /* 单调递增，最新有效 */
    uint32_t timestamp;                 /* timer_get_ms() 快照 */
    uint32_t region_count;              /* 本 checkpoint 包含的 region 数 */
    uint32_t region_bitmap[64];         /* 2048 bit，哪些 region 在 checkpoint 中 */
    uint32_t journal_sequence;          /* 截断到此 sequence 之前的 journal */
    uint32_t crc;                       /* 校验和 */
    uint8_t  padding[2048 - 7 * 4 - 4]; /* 填满 2048B */
} l2p_checkpoint_header_t;

/* ── 接口 ── */

/*
 * checkpoint_create - 创建全量 Checkpoint
 *
 * ① flush 所有 dirty cache → NAND L2P Table
 * ② 构造 Header 写冗余 6 页
 * ③ 截断 Journal（擦除 ≤ journal_sequence 的 Block）
 *
 * @param ftl  FTL 上下文
 * @return 0 成功，-1 失败
 */
ftl_status_t checkpoint_create(struct ftl_context *ftl);

/*
 * checkpoint_find_latest - 扫描 Block 37-38 找最新有效 Checkpoint
 *
 * 扫描 6 页，选 magic + CRC 正确 + sequence 最大的。
 *
 * @param cp  输出：找到的 Header
 * @return 0 找到，-1 无有效 Checkpoint
 */
ftl_status_t checkpoint_find_latest(l2p_checkpoint_header_t *cp);

/*
 * checkpoint_check_trigger - 检查 Checkpoint 触发条件
 *
 * 三级触发：
 *   T1: Journal 水位线（≥85% → checkpoint_needed flag）
 *   T2: 写入量（total_writes % CP_WRITE_INTERVAL == 0）
 *   T3: 定时（timer elapsed ≥ CP_TIME_INTERVAL_MS，仅 IDLE/NORMAL）
 *
 * @param ftl         FTL 上下文
 * @param elapsed_ms  距上次 Checkpoint 的 ms 数
 * @return 1 需要触发，0 不需要
 */
int checkpoint_check_trigger(const struct ftl_context *ftl, uint32_t elapsed_ms);

/*
 * checkpoint_is_emergency - 紧急水位判断
 *
 * Journal 使用率 ≥ 95% 时，BUSY 模式也强制执行。
 *
 * @param ftl  FTL 上下文
 * @return 1 紧急
 */
int checkpoint_is_emergency(const struct ftl_context *ftl);

/*
 * checkpoint_journal_watermark - 计算 Journal 水位并设标志
 *
 * @param journal  Journal 上下文
 */
void checkpoint_journal_watermark(l2p_journal_t *journal);

#endif /* FTL_CHECKPOINT_H */