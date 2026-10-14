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
 * Store: 固定元数据区的 Checkpoint 池（`CHECKPOINT_POOL_BLOCKS` 块轮转，
 * **一条 record = 1 页**，块满换池内下一块并先擦）。池首块/块数由
 * ftl/metadata.h 按几何推导（「固定元数据区布局」）。
 *
 * 写模型与 Anchor / BBT 一致（见 ftl/metadata.h §元数据写模型）：
 * 只追加到擦除过的页 + 每条 record 带单调 sequence + 池内轮转 + 最新有效胜。
 * ⚠ 旧实现把 Header **就地重写**在固定 6 页上：第二次 checkpoint 起头即损坏 ⇒
 *   checkpoint_find_latest 失败 ⇒ 开机 ftl_boot_fresh_start ⇒ **映射全丢**。
 */

#include "ftl/allocator.h"
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"
#include "ftl/metadata.h" /* CHECKPOINT_BLOCK0 / CHECKPOINT_POOL_BLOCKS */

#include <stdint.h>

/* ── 前向声明（ftl_context_t 在 ftl.h 中定义，ftl.h 会 include 本文件） ── */
struct ftl_context;

/* ── Checkpoint 池游标/序号（序 4：per-instance）──
 *
 * **原先是 checkpoint.c 的 file-scope static**——单实例下无害，多实例（多 LUN）下
 * 所有实例共享同一份游标/序号 ⇒ 各自往同一个池槽写、sequence 互相踩。
 * 收进 ftl_context_t ⇒ 每实例一份；开机由 checkpoint_find_latest 恢复。
 *
 * ⚠ 与 journal / Anchor / BBT 的 sequence **互不相干**：本池自己的单调序号。 */
typedef struct
{
    uint32_t sequence;     /* 本池自己的单调序号（最新有效 record） */
    uint16_t write_idx;    /* 池内下一个待写槽（环形） */
    uint8_t  cursor_valid; /* 游标是否已建立；未建立则首次写入先扫池定位 */
} l2p_cp_state_t;

/* ── Checkpoint 池位置与块数（CHECKPOINT_BLOCK0 / CHECKPOINT_POOL_BLOCKS）
 *    见 ftl/metadata.h「固定元数据区布局」── */

/* ── Header Magic ── */
#define CHECKPOINT_MAGIC (0xCCCCCCCCUL)

/* ── Header 格式：**恰好一页主区**（2048 B）──
 * 字段 70 字 = 6 个标量 + region_bitmap[64]；padding 补齐到 2048。
 * ⚠ padding 长度按 70 字算（旧版写 7 字 ⇒ sizeof 变成 2296 > 一页 ⇒ 写入被
 *   ftl_cmd_write_data 按链长静默截到 2048；"一条 record = 一页"名不副实）。
 * 本页**无独立 CRC**：完整性由下层 FC 的 per-CW CRC + ECC 保证（ADR v0.62，
 * DRAM-less 主流做法，见 docs/design/ftl_revision_history.md）。*/
typedef struct
{
    uint32_t magic;                  /* CHECKPOINT_MAGIC */
    uint32_t sequence;               /* 本池自己的单调序号（最新有效）*/
    uint32_t timestamp;              /* timer_get_ms() 快照 */
    uint32_t region_count;           /* 本 checkpoint 包含的 region 数 */
    uint32_t region_bitmap[64];      /* 2048 bit，哪些 region 在 checkpoint 中 */
    uint32_t journal_sequence;       /* 截断到此 sequence 之前的 journal */
    uint32_t layout_version;         /* FTL_MEDIA_LAYOUT_VERSION（版本不符 ⇒ 该记录作废）*/
    uint8_t  padding[2048 - 70 * 4]; /* 补齐到一页主区（1768 B）*/
} l2p_checkpoint_header_t;

_Static_assert(sizeof(l2p_checkpoint_header_t) == PAGE_MAIN_SIZE, "Checkpoint Header 必须恰好一页主区");

/* ── 接口 ── */

/*
 * checkpoint_create - 创建全量 Checkpoint
 *
 * ① 快照 region 脏标记 → ② flush 脏 region 到 NAND L2P 日志块
 * → ③ flush Metadata Directory（Anchor）→ ④ 追加写 Header（池内下一槽）
 * → ⑤ 截断 Journal → ⑥ 复位水位标志
 *
 * ⚠ 顺序不可换：③ 必须早于 ④。旧序（header 先于 Anchor）在两者之间掉电会留下
 *   「header 新 / Directory 旧」⇒ 已声称落盘的 region 在 Directory 里找不到 ⇒ 丢更新。
 *   现序掉电在 ④ 之前 ⇒ 本次 CP 未记录，boot 用上一个 CP 并回放 journal（⑤ 还没跑）⇒ 一致。
 *
 * @param ftl  FTL 上下文
 * @return 0 成功，-1 失败
 */
ftl_status_t checkpoint_create(struct ftl_context *ftl);

/*
 * checkpoint_find_latest - 扫描 Checkpoint 池找最新有效 Header
 *
 * 池内轮转扫描（metadata_ring_locate）：magic 正确且 sequence 最大者胜。
 * 命中时同时恢复本实例的 sequence 计数与写游标（后续 checkpoint_create 从下一槽续写）。
 *
 * @param ftl  FTL 上下文（池路由取 ftl->topo；游标/序号写回 ftl->cp）
 * @param cp   输出：找到的 Header
 * @return 0 找到，-1 无有效 Checkpoint
 */
ftl_status_t checkpoint_find_latest(struct ftl_context *ftl, l2p_checkpoint_header_t *cp);

/*
 * checkpoint_write_initial - 全量格式化后在池首槽写一条「空快照」标记
 *
 * 只由 ftl_media_format() 在擦完全片后调用。写出的 Header 带当前
 * FTL_MEDIA_LAYOUT_VERSION 与 sequence=1，使下次开机 checkpoint_find_latest
 * 立即成功 ⇒ 版本判据命中 ⇒ 自动格式化只发生一次。
 *
 * @param topo  拓扑信息（Checkpoint 池所在芯片路由）
 * @return FTL_OK / FTL_ERROR
 */
ftl_status_t checkpoint_write_initial(ftl_topology_t topo);

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