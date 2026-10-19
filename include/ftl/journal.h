#ifndef FTL_JOURNAL_H
#define FTL_JOURNAL_H

/*
 * journal.h — L2P 增量日志：页/条目格式 + 环形状态 + API
 *
 * 职责：
 *   定义 Journal 的磁盘/内存布局（页头、条目、环形状态 l2p_journal_t）与对外 API。
 *   实现见 src/ftl/journal.c。
 *
 * 与 checkpoint.h 的分工：
 *   Journal  —— L2P 的**增量**日志（页追加，掉电回放的最小单位）；
 *   Checkpoint —— L2P 的**全量**快照（把 journal 的已写条目固化进 NAND L2P 表并截断 journal）。
 *
 * 布局：
 *   Journal 是 JOURNAL_BLOCK_COUNT 个块的环形池（块位置 JOURNAL_BLOCK0 起）；
 *   每页 = 页头（12B：magic + page_seq + entry_count + block_seq）+ N 条 l2p_journal_entry_t。
 *   N 由 (页主区 − 页头) ÷ 条目大小**推导**，不写死（见 JOURNAL_ENTRIES_PER_PAGE）。
 *
 * ⚠ **介质读回**不转断言（assertion_guidelines §六 辨析 C）：replay 读的是 NAND 页，
 *   页头可能因掉电撕裂而非法，属预期情形，返回错误/跳过而非断言。模块入口与命令
 *   派发路径的 phase / 内部句柄前置条件仍用 ASSERT；实例 handle 非空由
 *   ftl_contract_audit 在 boot 期证明，运行期入口不再逐次复检。
 *
 * 写模型：
 *   追加进 RAM 页缓冲（page_buf）——**页满异步落盘**，交给 job
 *   （journal_handle / g_op_callbacks[FTL_OP_JOURNAL]），使 host 写完成回调里不嵌套
 *   完成泵。需要屏障语义的调用者显式调 journal_flush_page（只剩结构性同步上下文：
 *   HIL `fl` / 开机路径 / CLI——GC/WL 的 F10 走 journal_handle 的异步等待 + 失败 pending）。
 */

#include "allocator.h"  /* ftl_allocator_t（持续擦除失败 → bbt_mark_bad） */
#include "ftl_config.h" /* nand_pba_t */
#include "ftl_module.h" /* ftl_bg_info_t / ftl_module_status_t */
#include "ftl_type.h"
#include "l2p_cache.h"
#include "metadata.h"    /* JOURNAL_BLOCK0 / JOURNAL_BLOCK_COUNT 块位置 */
#include "nand_config.h" /* PAGE_MAIN_SIZE */

#include <stdint.h>

/* Journal 块位置：第 b 块 = JOURNAL_BLOCK0 + b，共 JOURNAL_BLOCK_COUNT 块
 * （块位置与块数见 ftl/metadata.h「固定元数据区布局」）*/

/* Journal 页大小 = 一页主区（由 nand_config.h 推出）*/
#define JOURNAL_PAGE_SIZE (PAGE_MAIN_SIZE)

#define JOURNAL_FULL_THRESHOLD (85U)
#define JOURNAL_CRITICAL       (95U)

/* 「无预擦除块」哨兵（truncate 预擦除记账，见 l2p_journal_t） */
#define JOURNAL_BLOCK_NONE (0xFFU)

/* Journal Page Header */
#define JOURNAL_HEADER_SIZE (12U)          /* magic(4) + page_seq(4) + entry_count(2) + block_seq(2) */
#define JOURNAL_MAGIC       (0x524F554AUL) /* "JOUR" */

/* 一条 L2P 更新：lba 的映射从 old_pba 变为 new_pba（16B，packed 无填充） */
typedef struct
{
    uint32_t   lba;      /* 逻辑地址 */
    nand_pba_t old_pba;  /* 更新前物理地址（回放不读；仅首条保留，供诊断） */
    nand_pba_t new_pba;  /* 更新后物理地址（回放按此写 L2P） */
    uint32_t   sequence; /* 全局单调序号（回放按 (page_seq, 页内下标) 定序） */
} __attribute__((packed)) l2p_journal_entry_t;

/* 每页条目数 = (页主区 − 页头) ÷ 条目大小，向下取整。
 * ⚠ 必须推导：旧版写死 128 而条目 16B ⇒ 12 + 128×16 = 2060 > 2048，
 *   第 127 条会写出 page_buf 之外（踩到 l2p_journal_t 的后续字段）。*/
#define JOURNAL_ENTRIES_PER_PAGE ((JOURNAL_PAGE_SIZE - JOURNAL_HEADER_SIZE) / sizeof(l2p_journal_entry_t))

/* ── 异步刷页 job ──
 *
 * 不需要 ping-pong 快照缓冲：page_buf 在 ftl_async_write_page 里**已拷进
 * FC buffer 池**（ftl_cmd_write_data 是 memcpy）⇒ 提交后即可复用/清空。
 * 在飞的只剩「写指针推进 + 换块擦除」这点记账，job 只需记住落点。
 *
 * 单飞行：phase != JRN_IDLE 时不再起新页写（否则两条写会争同一个页地址）。
 * 页满且 job 忙 ⇒ **非派发上下文**走同步回退（journal_flush_page）；派发上下文
 * （host 写完成 / 搬移完成）**丢弃本条 + LOGE**，整页留给 journal_handle 下轮刷。 */
typedef enum
{
    JRN_IDLE = 0, /* 无在飞 */
    JRN_WRITE,    /* 页写命令在飞 */
    JRN_ERASE     /* 页写已完成，换块擦除在飞 */
} journal_phase_t;

/*
 * jrn_truncate_step_t - journal_truncate_plan 的结果
 *
 * ⚠ 这里的 -1（JRN_TRUNC_NO_TRUNCATE）**不是硬失败**——是「水位不满足 ⇒ 安全方向地
 *   放弃预擦」，调用者照旧收尾（预擦只是磨损优化）。故它不能与 cp_flush_step_t /
 *   md_compact_step_t 共用一套名字（那两处的 -1 都要求 abort）。
 */
typedef enum
{
    JRN_TRUNC_NO_TRUNCATE = -1, /* 水位不满足：不截断（调用者不得复位水位标志）*/
    JRN_TRUNC_ALREADY     = 0,  /* 无需擦（该块已预擦过）*/
    JRN_TRUNC_NEEDS_ERASE = 1,  /* 需擦 out_addr / out_block */
} jrn_truncate_step_t;

/*
 * journal_handle_t — 本模块的**执行状态**（FSM / 写游标 / 持久性失败观测）
 *
 * 归属：`l2p_journal_t.handle`（per-instance）。共享账本（L2P cache / BST /
 * allocator）不在这里；需要它们时走 `l2p_journal_t` 里的端口。
 *
 * 主流程**不读**本结构的 phase / 游标 / 失败位：只读 `journal_status_t` 与查询
 * （docs/design/ftl_module_contracts.md §4.2）。
 */
typedef struct
{
    /* ── 异步刷页 job ── */
    uint8_t    phase;        /* journal_phase_t */
    uint8_t    need_erase;   /* 有**未起**的擦除：target_block 待擦（擦命令起不来时留待重试） */
    uint8_t    target_block; /* 页写成功后 current_block 的落点 */
    uint8_t    target_page;  /* 页写成功后 current_page 的落点（已做块内回绕） */
    uint8_t    from_block;   /* 起写时的 current_block（防指针回退的守卫，见 journal.c） */
    uint8_t    from_page;    /* 起写时的 current_page */
    nand_pba_t waddr;        /* 在飞页地址（诊断 / 心跳） */

    /* ── 环形写游标 ── */
    uint8_t  current_block;
    uint8_t  current_page;
    uint8_t  current_entry;
    uint32_t sequence_counter;

    /* CP 边界：sequence < merge_floor 的旧条目**不得**被同 LBA 原地合并。
     * 否则 CP 后的覆盖写会复用 CP 前的 sequence 却带新的 new_pba ⇒
     * 块元数据快照 增量回放按 sequence 过滤时漏记，之后覆盖写可能撞 valid_count 下溢断言。
     * 0 = 不设门（开机/初始化）。 */
    uint32_t merge_floor;

    /* 被 journal_truncate **预擦除**、等待 flush_page 回切复用的环形块号
     * （JOURNAL_BLOCK_NONE = 无）。记账目的 = 同一块每圈只擦一次：truncate 擦环形
     * 下一块后，flush_page 回切时跳过擦除。 */
    uint8_t pre_erased_block;

    uint8_t page_buf[JOURNAL_PAGE_SIZE];

    uint8_t checkpoint_needed;
    uint8_t emergency;

    /* ── 环形块坏块策略（RISK-JN-01，全部 RAM 态）──
     *   bad_mask     —— 已判定不可用的环形块（bit b）；持久化由 bad_block_port 承担，
     *                   重启后 BST BAD 会重新挡住这些块。
     *   erase_fail_count / erase_fail_block —— 同一块连续擦除失败计数；
     *                   达 JRN_ERASE_FAIL_MAX 即标坏并跳到下一可用块。
     *   ring_full    —— 全部环形块不可用：journal 停止写入（不擦未擦块、不无限重试）。 */
    uint8_t bad_mask;
    uint8_t erase_fail_count;
    uint8_t erase_fail_block;
    uint8_t ring_full;

    /* ── 持久性失败观测（全部 RAM，不随介质持久化）──
     * 页写 / 换块擦失败时 +1 并置 pending。`write_fail_seq` 是失败时刻的
     * sequence_counter；pending 一直阻止 victim 擦除（GC/WL/predict），直到一次
     * Checkpoint Header 成功且其 journal_seq ≥ write_fail_seq —— 此时迁移后的
     * L2P 已随 CP 落盘，丢掉的那条 journal 条目不再是唯一副本。
     * ⚠ 不尝试恢复已丢条目；pending 只负责“不撒谎地说屏障已达成”。 */
    uint32_t write_fail_count;
    uint32_t write_fail_seq;
    uint8_t  write_fail_pending;

    /* 心跳基准（见 ftl_job.h）：在飞期间每满 1s 打一行 phase/age */
    uint32_t start_ms;
    uint32_t log_ms;
} journal_handle_t;

/*
 * journal_bad_block_port_t — 环形块坏块查询/标记端口（docs §4.2）
 *
 * journal 不持有 allocator，也不 include BST 语义：只通过端口问「这块坏吗」、
 * 「把这块标坏」。端口实现由 FTL 装配层（ftl.c）提供，ctx 自持。
 */
typedef struct
{
    void *ctx;
    bool (*block_is_bad)(void *ctx, uint16_t gblock);
    void (*mark_bad)(void *ctx, uint16_t gblock);
} journal_bad_block_port_t;

typedef struct
{
    ftl_topology_t topo; /* 目标 NAND 芯片路由 */

    /* 坏块端口（持续擦除失败 → 标坏）。未绑定时只维护 RAM bad_mask。 */
    journal_bad_block_port_t bad_block_port;

    /* 异步刷页 job / 环形写游标 / 失败观测（见 journal_handle_t） */
    journal_handle_t handle;
} l2p_journal_t;

/* Journal 环形页引用：位置 + 页序号 + 条目数（journal_scan_latest 出参） */
typedef struct
{
    uint32_t page_seq;    /* 页头记录的**首条**条目序号 */
    uint8_t  block;       /* 环形块号（0..JOURNAL_BLOCK_COUNT-1） */
    uint8_t  page;        /* 块内页号 */
    uint8_t  entry_count; /* 本页条目数 */
} journal_page_ref_t;

/*
 * journal_scan_latest - 只读扫环，取**最新页**（最大 page_seq）。
 *
 * 用途：① 启动判型（干净关机 / 掉电）；② 干净关机的写指针恢复。**不改 journal 状态**，
 * 与回放共用同一套页头判定。
 *
 * @param journal Journal；out 出参（环空时全置 0，可为 NULL）
 * @param out_read_error 出参（可为 NULL）：true = 环内存在读失败页 ⇒ 最新页与
 *        「干净/掉电」判型都不可信，启动恢复应走掉电回放路径
 * @return true = 环非空（*out 有效）；false = 环空（全部擦除 / 无有效页）
 */
bool journal_scan_latest(const l2p_journal_t *journal, journal_page_ref_t *out, bool *out_read_error);

/*
 * journal_resume - 恢复 Journal 的 RAM 写态（写指针 + 序号），**不回放任何条目**。
 *
 * 为什么必须做：`journal_init` 把写指针复位到 block0/page0、序号复位到 1，但
 * **NAND 上的环形块并没有被擦**（`journal_truncate` 只预擦环形下一块）——「Journal
 * 是空的」这个前提只对刚格式化过的介质成立。不恢复会有两个后果：
 *   ① 序号从 1 重来 ⇒ 新条目的 page_seq 排到环内旧页**之后**，下次开机回放的
 *      page_seq 过滤把它们当「已进 L2P 表」整页丢弃；
 *   ② 写指针回到 block0/page0 ⇒ 对**已编程**的页二次编程（NAND 只能 1→0 = AND 损坏）。
 *
 * 落点：最新页**之后一页**——最新页已编程，续填会在页满时对同一页二次编程（页头
 * entry_count 被 AND 成错值）；跨块时先擦下一块（那里还留着上一圈的旧页）。
 * 擦除失败 ⇒ 重新挂起 `job.need_erase`（LOGE），首次写前由 kick/flush 补擦——
 * 绝不能把新页写进未擦块。
 *
 * @param journal Journal
 * @param latest  journal_scan_latest 的结果（NULL 或 page_seq==0 视为环空）
 * @param floor_sequence 序号下限 = Checkpoint 的 journal_sequence（「下一个待分配」），
 *        兜住「环空」与「环内最新页序号反而落后」两种情形（0 会被抬到 1）
 */
void journal_resume(l2p_journal_t *journal, const journal_page_ref_t *latest, uint32_t floor_sequence);

/* journal_init - 初始化 Journal：记录拓扑、复位写指针与序号、清空端口 */
void journal_init(l2p_journal_t *journal, ftl_topology_t topo);

/* journal_bind_bad_block_port - 绑定坏块端口（装配层在 journal_init 之后调用）。
 * 端口为纯函数指针 + ctx，生命周期须覆盖 Journal；未绑定时只维护 RAM bad_mask。 */
void journal_bind_bad_block_port(l2p_journal_t *journal, const journal_bad_block_port_t *port);
/* 追加一条 L2P 更新。若当前页缓冲里已有**同一 LBA** 的条目，就地更新
 * （只改 old/new PBA，不新增条目、不消耗 sequence）——覆盖写工作集下减少 Journal 页数。
 * 返回：true = 已追加 / 就地合并；false = 页满且异步刷页起不来，本条目被丢弃。
 * ⚠ 返回 false 时调用者必须按“映射未持久化”处理：迁移侧中止 job（victim 不擦）；
 *   host 写路径当前只能记日志继续（其 drop 分支按注释结构性不可达）。 */
bool journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old_pba, nand_pba_t new_pba);

/* ── 模块 handle（typed 入口，docs/design/ftl_module_contracts.md §3.3 / §4.2）── */

/* journal 执行状态（上报用，不是内部 phase 编号） */
typedef enum
{
    JOURNAL_STATE_IDLE = 0,
    JOURNAL_STATE_WRITE_WAIT, /* 页写在飞 */
    JOURNAL_STATE_ERASE_WAIT, /* 换块擦在飞 */
    JOURNAL_STATE_RING_FULL,  /* 全部环形块不可用：停止写入 */
    JOURNAL_STATE_FAILED,     /* 持久性失败 pending（条目已丢，等 CP 兜底） */
} journal_state_t;

typedef struct
{
    ftl_module_status_t base;

    journal_state_t state;
    uint32_t        sequence;     /* sequence_counter（单调） */
    bool            write_failed; /* write_fail_pending */
    bool            ring_full;
    bool            erase_pending; /* need_erase */
    uint8_t         current_block;
    uint8_t         current_page;
    uint16_t        current_entry;
} journal_status_t;

/*
 * journal_trigger_t — 本轮调用的意图
 *
 * FLUSH       推进「补擦 + 页写」完整流程（预算任务）；
 * ERASE_ONLY  只补换块擦除，不起页写（must-run：新环形块没擦过就不能写，但
 *             host 饱和档要让路给前台）。
 */
typedef enum
{
    JOURNAL_TRIGGER_FLUSH = 0,
    JOURNAL_TRIGGER_ERASE_ONLY,
} journal_trigger_t;

/*
 * journal_handle - 后台推进 Journal（提交即返回；job 在飞或无事可做时为空操作）
 *
 * @param journal  Journal；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param trigger  本轮意图（NULL 视作 FLUSH）
 * @param info     本轮动态信息（now_ms / load_level；当前只用于观测，不改变判据）
 * @param out      出参：本轮结束后的上报（**非空**）
 */
void journal_handle(l2p_journal_t *journal, const journal_trigger_t *trigger, const ftl_bg_info_t *info, journal_status_t *out);

/* ── 查询（替代外部直读 handle 字段）── */

/* journal_is_idle — 无在飞 job（写 / 擦都完成）。 */
bool journal_is_idle(const l2p_journal_t *journal);

/* journal_can_erase_victim — F10 屏障：页写失败 pending 已清（迁移后的 L2P 已随
 * CP 落盘）⇒ 允许 GC/WL 擦 victim。 */
bool journal_can_erase_victim(const l2p_journal_t *journal);

/* journal_sequence — 下一个待分配序号（= CP 截断水位）。 */
uint32_t journal_sequence(const l2p_journal_t *journal);

/* journal_watermark — 当前使用率（0..100，全局写位置 / 全环容量）。 */
uint8_t journal_watermark(const l2p_journal_t *journal);

/* journal_write_failed — 持久性失败 pending 是否置位。 */
bool journal_write_failed(const l2p_journal_t *journal);

/* journal_has_pending_entries — 页缓冲里是否有未落盘条目。 */
bool journal_has_pending_entries(const l2p_journal_t *journal);

/* journal_erase_pending — 是否有「未起」的换块擦除（新环形块尚未擦）。 */
bool journal_erase_pending(const l2p_journal_t *journal);

/* journal_checkpoint_needed / journal_is_emergency — CP 水位标志（≥85% / ≥95%）。 */
bool journal_checkpoint_needed(const l2p_journal_t *journal);
bool journal_is_emergency(const l2p_journal_t *journal);

/* journal_set_merge_floor — CP Header 成功后设置合并边界（Checkpoint 模块调用）。 */
void journal_set_merge_floor(l2p_journal_t *journal, uint32_t sequence);

/* journal_update_watermark - 按当前写位置重算并置位 CP 水位标志。
 * 由写确认路径在每次 L2P 更新后调用（原 journal_update_watermark 的归属收回
 * journal 模块：它是 journal 自己的状态）。 */
void journal_update_watermark(l2p_journal_t *journal);

/* journal_flush_page - **同步**把 Journal 落盘：先排空在飞 job，再把当前页缓冲
 *   （含页头）写进 NAND 并推进写指针。
 *   · 本函数是**唯一**的同步环形块擦除点，块满换块时先擦后写（跳过 pre_erased_block 预擦块）；
 *   · 调用者用它当**屏障**（HIL `fl`）。host 写路径的常规刷页走 journal_handle 的异步路径。
 *   · drain 超时 ⇒ **拒绝同步刷**（在飞命令仍拥有当前页地址，再写同页 = 双重编程），
 *     置 write_fail_pending 并返回 FTL_ERROR。
 * @return FTL_OK = 屏障达成；FTL_ERROR = drain 超时 / 擦除失败 / 页写失败。 */
ftl_status_t journal_flush_page(l2p_journal_t *journal);
/* F10：全量回放**未进 L2P 表**的页（按 (page_seq, entry_index) 递增序应用，
 * per-LBA last-write-wins）。cp_sequence 为 checkpoint 截断水位（旧 journal_replay
 * 只回放最大一页，掉电跨 ≥2 页时其余条目静默丢失）。
 * ⚠ 判据是**整页**：`page_seq + entry_count > cp_sequence`。只看页首页序号
 *   （`page_seq > cp_sequence`）会把跨水位的那一页整页跳过，丢掉它的新条目。
 * out_entries（可为 NULL）回填**实际应用成功的条目数**（读失败的页不计入）。
 * ⚠ 该数**不能**由 journal 的 RAM 写指针反推：末尾 journal_resume 把落点放到最新页
 *   之后并清 current_entry ⇒ 用 current_entry/current_page 算出来恒为 0。
 * @return FTL_OK = 完整回放；FTL_ERROR = 环内存在读失败页（写态已恢复，但回放可能
 *         不完整 ⇒ 调用者应回退 F2 全盘重建）。 */
/*
 * journal_replay_block_ctx_t - 回放时的块元数据增量端口（可选）
 *
 * 三个回调都由调用者（开机恢复模块）注入，journal 不 cast ctx、不反向依赖 PFR：
 *   materialize  把 PBA 所属块从池状态转成「可记账」的 Closed 状态；
 *   valid_add    给该 PBA 的块 valid_count +1（带越界/上溢守卫，失败置 *ok=0）；
 *   valid_sub    给该 PBA 的块 valid_count -1（带下溢守卫，失败置 *ok=0）。
 *
 * 只对 sequence ≥ cp_sequence 的条目记账：new_pba +1、old_pba −1
 * （TRIM 只减 old；old==new 跳过）。materialize 失败 / 越界 / 下溢 ⇒ ok=0，
 * 调用者据此丢弃快照恢复结果并回退 F2。
 */
typedef struct
{
    bool (*materialize)(void *ctx, nand_pba_t pba);
    void (*valid_add)(void *ctx, nand_pba_t pba, uint8_t *ok);
    void (*valid_sub)(void *ctx, nand_pba_t pba, uint8_t *ok);
    void   *ctx; /* 调用者自持（PFR 传 &g_ftl[...]） */
    uint8_t ok;
} journal_replay_block_ctx_t;

ftl_status_t journal_replay(l2p_journal_t              *journal,
                            l2p_cache_t                *cache,
                            uint32_t                    cp_sequence,
                            uint32_t                   *out_entries,
                            journal_replay_block_ctx_t *block_ctx);
/*
 * journal_truncate_plan - 截断 Journal 的**第一步**：水位校验 + 算出待预擦的块
 *
 * 语义：Checkpoint 已把 sequence ≤ up_to_sequence 的条目 flush 进 NAND L2P 表 ⇒
 * 环形里**最旧的一块**（= 环形下一块）整块可弃。预擦该块并记账（pre_erased_block），
 * 使 journal_flush_page 回切到它时**跳过**擦除——保证同一块每圈只擦一次。
 *
 * 本函数只做**判定与算址**（纯 RAM，不动介质）：擦除由调用者提交（Checkpoint job
 * 提交异步擦命令），成功完成后再调 journal_truncate_commit 记账。
 *
 * @param journal        Journal
 * @param up_to_sequence 截断水位；**必须覆盖全部已写条目**（调用者传 CP 时刻的
 *                       sequence_counter）。水位不足以覆盖（有未 checkpoint 的条目）
 *                       时**不截断**——最旧块可能仍持有回放需要的条目，擦掉即丢数据。
 * @param out_addr       出参：待预擦块的基地址（需返回 1 时有效）
 * @param out_block      出参：待预擦的环形块号（需返回 1 时有效；commit 要用）
 * @return jrn_truncate_step_t：JRN_TRUNC_NEEDS_ERASE = 需擦 *out_addr / *out_block；
 *         JRN_TRUNC_ALREADY = 无需擦（水位 OK，该块已预擦过）；
 *         JRN_TRUNC_NO_TRUNCATE = 不截断（水位不满足）——**调用者不得据此复位水位标志**
 */
jrn_truncate_step_t journal_truncate_plan(l2p_journal_t *journal,
                                          uint32_t       up_to_sequence,
                                          nand_pba_t    *out_addr,
                                          uint8_t       *out_block);

/*
 * journal_truncate_commit - 截断 Journal 的**第二步**：记账 + 复位水位标志
 *
 * 在擦除命令**成功完成**后调用。记 pre_erased_block 使 journal_flush_page 回切时
 * 跳过擦除；复位 emergency / checkpoint_needed（设计 §7.5.4：本次 CP 已把
 * sequence ≤ cp_seq 的条目落盘并截断，水位清零。不复位会让 T1 永久命中——每圈必 CP）。
 *
 * ⚠ erased_block == JOURNAL_BLOCK_NONE ⇒ **只复位水位标志，不动 pre_erased_block**
 *   ——"本次没有擦"（水位不满足 / 该块已被预擦过）时调用者用它收尾；若此时把
 *   pre_erased_block 覆写成 NONE，journal_flush_page 回切时会把已预擦的块再擦一次。
 *
 * @param journal   Journal
 * @param erased_block 刚擦成功的环形块号（journal_truncate_plan 的回传值）；
 *                     JOURNAL_BLOCK_NONE = 本次未擦
 */
void journal_truncate_commit(l2p_journal_t *journal, uint8_t erased_block);

/* journal_clear_write_failure - Checkpoint Header 成功后清除持久性失败 pending。
 * 仅当该 CP 的 journal 水位（起点快照）≥ 失败时刻的 sequence 才清除：说明 CP 在
 * 失败之后启动，已把迁移后的 L2P 区域落盘；否则保留 pending，让下一次 CP 再试。 */
void journal_clear_write_failure(l2p_journal_t *journal, uint32_t cp_journal_seq);

#endif /* FTL_JOURNAL_H */
