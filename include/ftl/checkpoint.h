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

/* ── 异步 Checkpoint job（v0.75 阶段 6）──
 *
 * checkpoint_create 原是**严格有序的同步流水线**（八步掉电安全契约，见 .c 头注释）。
 * 异步化后同一份顺序由本状态机的 phase 承载：每一步提交命令后即返回，完成由
 * g_op_service[FTL_OP_CHECKPOINT]（ftl_op_checkpoint）按 phase 收尾，下一轮
 * checkpoint_kick 续跑。
 *
 * ⚠ **掉电安全契约不变**：先 flush region → 再 flush Anchor → 最后写 Header。
 *   Header 是"承诺书"——它一旦落盘，boot 就信 region_bitmap 里置位的 region 在
 *   NAND 上，故任何被 Header 声称的落盘必须**先于** Header 完成。
 *
 * ⚠ **快照点前移（与旧同步实现的刻意差异，见 checkpoint.c 的长注释）**：
 *   region_bitmap 与 journal_sequence 都在 job **起点**一次取定（旧实现取
 *   journal_sequence 是在 Anchor 之后）。异步 CP 跨多轮，CP 期间完成的 host 写
 *   会在 journal 里留下 > 快照水位的新条目——水位取早 ⇒ 它们一定被回放，不会丢。
 *
 * 完成服务函数 `ftl_op_checkpoint` 声明在 ftl/ftl_op.h（与其余 op 服务函数同处）；
 * 它按 phase 记结果并推进，**不**调用 checkpoint_kick（CP 阶段含同步原语，见 .c）。
 */

/* 连续提交不出去的轮数上限（提交侧背压兜底；与 MIG/RR_SUBMIT_STALL_MAX 同族）。
 * 只在「有脏行却起不动」时累计——等 FLUSHING 完成算有进展，不计数。 */
#define CP_SUBMIT_STALL_MAX (256U)

/* job 心跳节拍见 ftl_job.h 的 FTL_JOB_LOG_INTERVAL_MS（各 feature 统一，不再各自定义）。 */

typedef enum
{
    CP_IDLE = 0,      /* 无 job */
    CP_FLUSH_REGIONS, /* ① 按快照 bitmap 逐个落盘脏 region（起 L2P 异步写回，等全部完成） */
    CP_ANCHOR_PREP,   /* ② 定位 Anchor 游标 + 取槽（可能含一次性同步定位） */
    CP_ANCHOR_ERASE,  /* ② 换块擦（仅需换块时） */
    CP_ANCHOR_WRITE,  /* ② 逐页写 Anchor record */
    CP_HDR_PREP,      /* ③ 定位 Checkpoint 游标 + 取 Header 槽 */
    CP_HDR_ERASE,     /* ③ 换块擦（仅需换块时） */
    CP_HDR_WRITE,     /* ③ 写 Header */
    CP_TRUNC_PREP,    /* ④ 水位判定：算 journal 预擦块 */
    CP_TRUNC_ERASE,   /* ④ 预擦 journal 环形最旧块 */
    CP_DONE,          /* 收尾：释放 active，记日志 */
} cp_phase_t;

/*
 * cp_flush_step_t - cp_flush_regions 一步的结果（消除调用点的 `r < 0` / `r == 0` 魔法数字）
 */
typedef enum
{
    CP_FLUSH_FAILED  = -1, /* 硬失败：调用者 cp_abort */
    CP_FLUSH_PENDING = 0,  /* 仍有脏行在飞：调用者 return，下轮再来 */
    CP_FLUSH_DONE    = 1,  /* 全部 region 已持久：推进 phase */
} cp_flush_step_t;

typedef struct
{
    uint8_t  active;       /* 1 = job 在跑（单飞行：ftl_service 每轮推进它） */
    uint8_t  phase;        /* cp_phase_t */
    uint8_t  waiting;      /* 1 = 有一条本 job 的命令在飞（等完成服务函数） */
    uint8_t  failed;       /* 1 = 前置步骤失败 ⇒ 中止（不写 Header / 不截断） */
    uint8_t  fail_phase;   /* 中止发生在哪个 phase（观测；cp_abort 记录） */
    uint16_t stall_rounds; /* 连续提交不出去的轮数（见 CP_SUBMIT_STALL_MAX） */

    /* ① job 起点快照（**必须先于任何 flush**：flush 成功会清 region 脏位） */
    uint32_t region_bitmap[64]; /* = cache.region_dirty_bitmap 的副本 */
    uint32_t journal_seq;       /* = journal.sequence_counter（截断/回放水位） */

    /* ② Anchor（一条 record = ANCHOR_PAGE_COUNT 页，逐页提交） */
    metadata_ring_t anchor_ring;
    nand_pba_t      anchor_base;       /* record 第 0 页地址（metadata_dir_anchor_prepare 回传） */
    uint16_t        anchor_idx;        /* 池内槽位（commit 要用） */
    uint32_t        anchor_seq;        /* ++dir->anchor_sequence（prepare 时已自增） */
    uint8_t         anchor_chunk;      /* 下一页要写的页序 */
    uint8_t         anchor_ok;         /* 0 = 有条分页写失败（整条作废） */
    uint8_t         anchor_need_erase; /* 1 = 写之前先擦 *anchor_base 所在块 */
    uint8_t         anchor_skip;       /* 1 = Directory 无变更 ⇒ 整段跳过 */

    /* ③ Header（一条 record = 1 页） */
    metadata_ring_t hdr_ring;
    nand_pba_t      hdr_addr;
    uint16_t        hdr_idx;        /* 写这条 Header 用的池内槽位 */
    uint32_t        hdr_seq;        /* ++ftl->cp.sequence（prepare 时取，与旧序一致） */
    uint8_t         hdr_need_erase; /* 1 = 写之前先擦 *hdr_addr 所在块 */

    /* ④ Journal 截断 */
    nand_pba_t trunc_addr; /* 待预擦块基地址（plan 回传） */
    uint8_t    trunc_next; /* 待预擦的环形块号（plan 回传） */

    /* 心跳 */
    uint32_t start_ms; /* job 起点（观测用） */
    uint32_t log_ms;   /* 上次心跳时刻 */
} cp_job_t;

/* 快照位图的 memcpy 长度取 cp_job_t 自己的字段 ⇒ 必须与 cache 的位图**同尺寸**，
 * 否则会读越界（源小）或漏拷（源大）。钉死它。 */
_Static_assert(sizeof(((cp_job_t *)0)->region_bitmap) == sizeof(((l2p_cache_t *)0)->region_dirty_bitmap),
               "cp_job_t.region_bitmap 必须与 l2p_cache_t.region_dirty_bitmap 同尺寸");

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
 * checkpoint_begin - 启动一次全量 Checkpoint（异步 job，提交即返回）
 *
 * ① 快照 region 脏标记 + journal 水位 → ② flush 脏 region 到 NAND L2P 日志块
 * → ③ flush Metadata Directory（Anchor）→ ④ 追加写 Header（池内下一槽）
 * → ⑤ 截断 Journal → ⑥ 复位水位标志
 *
 * ⚠ 顺序不可换：③ 必须早于 ④。旧序（header 先于 Anchor）在两者之间掉电会留下
 *   「header 新 / Directory 旧」⇒ 已声称落盘的 region 在 Directory 里找不到 ⇒ 丢更新。
 *   现序掉电在 ④ 之前 ⇒ 本次 CP 未记录，boot 用上一个 CP 并回放 journal（⑤ 还没跑）⇒ 一致。
 *
 * ⚠ 本函数**只装配 job 并推进第一步**，不等任何 NAND 命令。此后由每轮 ftl_service
 *   调 checkpoint_kick 续跑，各阶段的完成由 g_op_service[FTL_OP_CHECKPOINT] 收尾。
 *   调用者**不能**据返回值判断 CP 成败（与旧 checkpoint_create 的差别）。
 *
 * ⚠ 单飞行：已在跑（`ftl->cp_job.active`）⇒ 返回 FTL_AGAIN，不重入。
 *   （旧同步实现里 CP 是原子的一整段，不存在"上一次还没跑完"；异步化后必须挡住。）
 *
 * @param ftl  FTL 上下文
 * @return FTL_OK 已启动 / FTL_AGAIN job 忙 / FTL_ERROR 参数非法
 */
ftl_status_t checkpoint_begin(struct ftl_context *ftl);

/*
 * checkpoint_kick - 推进 Checkpoint job（每轮 ftl_service 调一次，任何档位都要调）
 *
 * job 未在跑 ⇒ 空操作。已在跑 ⇒ 从当前 phase 续跑（有命令在飞时只做心跳）。
 *
 * @param ftl  FTL 上下文
 */
void checkpoint_kick(struct ftl_context *ftl);

/*
 * checkpoint_find_latest - 扫描 Checkpoint 池找最新有效 Header
 *
 * 池内轮转扫描（metadata_ring_locate）：magic 正确且 sequence 最大者胜。
 * 命中时同时恢复本实例的 sequence 计数与写游标（后续 Checkpoint job 从下一槽续写）。
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
 * @return true 需要触发，false 不需要
 */
bool checkpoint_check_trigger(const struct ftl_context *ftl, uint32_t elapsed_ms);

/*
 * checkpoint_is_emergency - 紧急水位判断
 *
 * Journal 使用率 ≥ 95% 时，BUSY 模式也强制执行。
 *
 * @param ftl  FTL 上下文
 * @return true 紧急
 */
bool checkpoint_is_emergency(const struct ftl_context *ftl);

/*
 * checkpoint_journal_watermark - 计算 Journal 水位并设标志
 *
 * @param journal  Journal 上下文
 */
void checkpoint_journal_watermark(l2p_journal_t *journal);

#endif /* FTL_CHECKPOINT_H */