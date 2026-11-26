#ifndef FTL_JOURNAL_H
#define FTL_JOURNAL_H

/*
 * journal.h — L2P 增量日志：页/条目格式 + 环形状态 + API
 *
 * 职责：
 *   定义 Journal 的磁盘/内存布局（页头、条目、环形状态 l2p_jrn_s）与对外 API。
 *   实现见 src/ftl/journal.c。
 *
 * 与 checkpoint.h 的分工：
 *   Journal  —— L2P 的**增量**日志（页追加，掉电回放的最小单位）；
 *   Checkpoint —— L2P 的**全量**快照（把 journal 的已写条目固化进 NAND L2P 表并截断 journal）。
 *
 * 布局：
 *   Journal 是 JOURNAL_BLOCK_COUNT 个块的环形池（块位置 JOURNAL_BLOCK0 起）；
 *   每页 = 页头（16B：magic + page_seq + entry_count + block_seq + base_lba）+ N 条条目。
 *   两种页型由 magic 区分、条目大小不同，N 由 (页主区 − 页头) ÷ 条目大小**推导**：
 *     · LBA 显式页（JRN_MAGIC，16B/条）——随机写 / GC / TRIM 的通用形态；
 *     · 顺序流映射页（JRN_MAGIC_SEG，4B/条）——只存 new_pba，LBA = base_lba + 下标。
 *   顺序流映射页是**本仓自拟**的页型：形态取自 DFTL 的 translation page（顺序流下
 *   LBA 可由位置隐式表达），差异是它放在 Journal 环形页里、页头带全局 page_seq 供回放
 *   定序；每页条目数从 127 提到 508 ⇒ 一次 START/END 可覆盖 ~252 KiB。
 *   详见 docs/design/ftl_design.md §3.7 / §3.16.9。
 *
 * ⚠ **介质读回**不转断言（assertion_guidelines §六 辨析 C）：replay 读的是 NAND 页，
 *   页头可能因掉电撕裂而非法，属预期情形，返回错误/跳过而非断言。模块入口与命令
 *   派发路径的 phase / 内部句柄前置条件仍用 ASSERT；实例 handle 非空由
 *   ftl_contract_verify 在 boot 期证明，运行期入口不再逐次复检。
 *
 * 写模型：
 *   追加进 RAM 页缓冲（page_buf）——**页满异步落盘**，交给 job
 *   （jrn_handle / g_op_callbacks[FTL_OP_JOURNAL]）。数据面一律先预留、不足则待入账
 *   重试，**模块不提供同步刷页出口**：通用页用 jrn_reserve，cache 顺序写确认用
 *   jrn_seq_is_ready + jrn_append_seq（顺序流映射页，见下）。
 */

#include "allocator.h"  /* alc_s（持续擦除失败 → bbt_mark_bad） */
#include "ftl_config.h" /* DWORD */
#include "ftl_type.h"
#include "l2p_cache.h"
#include "log.h"
#include "metadata.h"    /* JOURNAL_BLOCK0 / JOURNAL_BLOCK_COUNT 块位置 */
#include "nand_config.h" /* PAGE_MAIN_SIZE */

#include <stdint.h>

/* Journal 块位置：第 b 块 = JOURNAL_BLOCK0 + b，共 JOURNAL_BLOCK_COUNT 块
 * （块位置与块数见 ftl/metadata.h「固定元数据区布局」）*/

/* Journal 页大小 = 一页主区（由 nand_config.h 推出）*/
#define JRN_PAGE_SIZE (PAGE_MAIN_SIZE)

/* 「无预擦除块」哨兵（truncate 预擦除记账，见 l2p_jrn_s） */
#define JRN_BLOCK_NONE (0xFFU)

/* Journal Page Header
 *   magic(4) + page_seq(4) + entry_count(2) + block_seq(2) + base_lba(4)。
 * base_lba 只对顺序流映射页有效（LBA 显式页恒 0）。
 * ⚠ 介质字节序：页头与条目的多字节字段**一律小端**（与 ONFI 参数页口径一致，
 *   见 `src/nand/onfi_1_0.c` 的字节序说明）。写入/解析都用 memcpy 原生字，
 *   目标平台（Cortex-M7，小端）与格式一致；换大端平台必须在此处集中转换。 */
#define JRN_HEADER_SIZE (16U)

#define JRN_MAGIC (0x524F554AUL) /* "JOUR"：LBA 显式页（16B 条目） */

#define JRN_MAGIC_SEG (0x4745534AUL) /* "JSEG"：顺序流映射页（4B 条目，LBA = base_lba + 下标） */

/* 每页条目数 = (页主区 − 页头) ÷ 条目大小，向下取整。
 * ⚠ 必须推导：条目 16B 时 128 条 ⇒ 16 + 128×16 = 2064 > 2048，
 *   第 127 条会写出 page_buf 之外（踩到 l2p_jrn_s 的后续字段）。*/
#define JRN_ENTRIES_PER_PAGE ((JRN_PAGE_SIZE - JRN_HEADER_SIZE) / sizeof(l2p_jrn_entry_s))

#define JRN_SEG_ENTRIES_PER_PAGE ((JRN_PAGE_SIZE - JRN_HEADER_SIZE) / sizeof(l2p_jrn_seg_entry_t))

/* 一条 L2P 更新：lba 的映射从 old_pba 变为 new_pba（16B，packed 无填充） */
typedef struct L2P_JOURNAL_ENTRY_S
{
    DWORD lba;      /* 逻辑地址 */
    DWORD old_pba;  /* 更新前物理地址。回放的块元数据增量用它做 valid_sub
                     * （SEG 页不落盘，回放时从 L2P 现读）；就地合并保留首条 */
    DWORD new_pba;  /* 更新后物理地址（回放按此写 L2P） */
    DWORD sequence; /* 全局单调序号（回放按 (page_seq, 页内下标) 定序） */
} __attribute__((packed)) l2p_jrn_entry_s;

/* 顺序流映射页的一条：只存 new_pba；lba = base_lba + 页内下标，
 * sequence = page_seq + 页内下标（两条坐标都由位置隐式给出，故不落盘）。 */
typedef DWORD l2p_jrn_seg_entry_t;

/* 当前页缓冲的条目类型。页型由**本页第一条 append 的调用方**决定：
 * 顺序流的确认走 jrn_append_seq（顺序页），其余一律走 jrn_append（LBA 显式页）。 */
typedef enum JOURNAL_PAGE_KIND_E
{
    JRN_PAGE_LBA = 0, /* 页头 JRN_MAGIC，16B 条目 */
    JRN_PAGE_SEG = 1, /* 页头 JRN_MAGIC_SEG，4B 条目（base_lba 由首条 append 落定） */
} jrn_page_kind_e;

/* ── 异步刷页 job ──
 *
 * 不需要 ping-pong 快照缓冲：page_buf 在 ftl_async_write_page 里**已拷进
 * FC buffer 池**（ftl_cmd_write_data 是 memcpy）⇒ 提交后即可复用/清空。
 * 在飞的只剩「写指针推进 + 换块擦除」这点记账，job 只需记住落点。
 *
 * 单飞行：phase != JRN_IDLE 时不再起新页写（否则两条写会争同一个页地址）。
 * 页满且 job 忙 ⇒ **任何上下文都不做同步回退**；后续追加丢弃 + LOGE，
 * 整页留给 jrn_handle 下轮刷。需要不丢条目的调用者先用 jrn_reserve 预留。 */
typedef enum JOURNAL_PHASE_E
{
    JRN_IDLE = 0, /* 无在飞 */
    JRN_WRITE,    /* 页写命令在飞 */
    JRN_ERASE     /* 页写已完成，换块擦除在飞 */
} jrn_phase_e;

/**
 * @brief jrn_truncate_plan 的结果
 *
 * ⚠ 这里的 JRN_TRUNC_NO_TRUNCATE **不是硬失败**——是「水位不满足 ⇒ 安全方向地
 *   放弃预擦」，调用者照旧收尾（预擦只是磨损优化）。故它不能与
 *   md_compact_step_e / Checkpoint flush 步进结果共用一套名字
 *   （那两处的 FAILED 都要求 abort）。
 */
typedef enum JOURNAL_TRUNCATE_STEP_E
{
    JRN_TRUNC_ALREADY     = 0U, /* 无需擦（该块已预擦过）*/
    JRN_TRUNC_NEEDS_ERASE = 1U, /* 需擦 out_addr / out_block */
    JRN_TRUNC_NO_TRUNCATE = 2U, /* 水位不满足：不截断（调用者不得复位水位标志）*/
} jrn_truncate_step_e;

/*
 * jrn_handle_s — 本模块的**执行状态**（FSM / 写游标 / 持久性失败观测）
 *
 * 归属：`l2p_jrn_s.handle`（per-instance）。共享账本（L2P cache / BST /
 * allocator）不在这里；需要它们时走 `l2p_jrn_s` 里的端口。
 *
 * 主流程**不读**本结构的 phase / 游标 / 失败位：只走查询接口
 * （docs/archive/ftl_module_contracts.md §4.2）。
 */
typedef struct JOURNAL_JOB_S
{
    jrn_phase_e     phase;
    BOOL            need_erase;
    BYTE            target_block;
    BYTE            target_page;
    BYTE            from_block;
    BYTE            from_page;
    DWORD           waddr;
    BYTE            page_buf[JRN_PAGE_SIZE];
    log_heartbeat_s hb;
} jrn_job_s;

typedef struct JOURNAL_STATE_S
{
    /* 当前页缓冲（跨 flush 保留，不是 job 瞬态） */
    BYTE page_buf[JRN_PAGE_SIZE];

    /* 环形写游标 */
    BYTE            current_block;
    BYTE            current_page;
    WORD            current_entry;
    DWORD           sequence_counter;
    jrn_page_kind_e page_kind;
    DWORD           page_base;
    DWORD           merge_floor;
    BYTE            pre_erased_block;

    BOOL checkpoint_needed;
    BOOL emergency;

    /* 环形块坏块策略 */
    BYTE bad_mask;
    BYTE erase_fail_count;
    BYTE erase_fail_block;
    BOOL ring_full;

    DWORD erase_kick_blocked; /* 换块擦除因背压起不来的累计次数（不是擦除失败） */

    /* 持久性失败观测 */
    DWORD write_fail_count;
    DWORD write_fail_seq;
    BOOL  write_fail_pending;

    DWORD page_writes;

    /* WAL 水位 */
    DWORD persisted_seq;
    DWORD write_page_seq;
    WORD  write_entry_count;
} jrn_state_s;

typedef struct JOURNAL_HANDLE_S
{
    jrn_job_s   job;   /* 每 job 清零 */
    jrn_state_s state; /* 跨 job 保留 */
} jrn_handle_s;

typedef struct L2P_JOURNAL_S
{
    ftl_topo_s topo; /* 目标 NAND 芯片路由 */

    /* 坏块查询/标坏只走 allocator 的 owner API（持续擦除失败 → 标坏）。
     * journal 不直接读写 allocator 的 BST/BBT 内部字段。 */
    alc_s *alloc;

    /* 环形块候选列表（ring 下标 → 物理块号）。由 jrn_init 从
     * alloc->pools->jrn 复制；固定 JOURNAL_BLOCK_COUNT 项，缺额由 shared
     * metadata spare 补足（凑不齐则池构建失败，不进入运行期）。 */
    WORD blocks[JOURNAL_BLOCK_COUNT];

    /* 异步刷页 job / 环形写游标 / 失败观测（见 jrn_handle_s） */
    jrn_handle_s handle;
} l2p_jrn_s;

/* Journal 环形页引用：位置 + 页序号 + 页型 + 条目数（jrn_page_scan_latest 出参） */
typedef struct JOURNAL_PAGE_REF_S
{
    DWORD           page_seq;    /* 页头记录的**首条**条目序号 */
    DWORD           base_lba;    /* JRN_PAGE_SEG：本页首条条目的 LBA（其余页型为 0） */
    BYTE            block;       /* 环形块号（0..JOURNAL_BLOCK_COUNT-1） */
    BYTE            page;        /* 块内页号 */
    WORD            entry_count; /* 本页条目数（顺序页可到 508 ⇒ WORD） */
    jrn_page_kind_e kind;        /* 页型（回放按页型解析条目） */
} jrn_page_ref_s;

/*
 * jrn_trigger_e — 本轮调用的意图
 *
 * FLUSH       推进「补擦 + 页写」完整流程（预算任务）；
 * ERASE_ONLY  只补换块擦除，不起页写（must-run：新环形块没擦过就不能写，但
 *             host 饱和档要让路给前台）。
 */
typedef enum JOURNAL_TRIGGER_E
{
    JRN_TRIGGER_FLUSH = 0,
    JRN_TRIGGER_ERASE_ONLY,
} jrn_trigger_e;

/**
 * @brief 回放时的块元数据增量端口（可选）
 *
 * 三个回调都由调用者（开机恢复模块）注入，journal 不 cast ctx、不反向依赖 PFR：
 *   materialize  把 PBA 所属块从池状态转成「可记账」的 Closed 状态；
 *   valid_add    给该 PBA 的块 valid_count +1（带越界/上溢守卫，失败置 *ok=0）；
 *   valid_sub    给该 PBA 的块 valid_count -1（带下溢守卫，失败置 *ok=0）。
 *
 * 只对 sequence ≥ cp_sequence 的条目记账：new_pba +1、old_pba −1
 * （TRIM 只减 old；old==new 跳过）。materialize 失败 / 越界 / 下溢 ⇒ ok=0，
 * 调用者据此丢弃快照恢复结果并回退 full metadata rebuild。
 */
/* 回放记账失败的原因码：`ok=false` 是个布尔，分不出是哪一条守卫挂的。
 * 开机只打一次（错误路径，无 I/O），供定位「首次违反的是哪条不变量」。 */
typedef enum JOURNAL_REPLAY_FAIL_E
{
    JRN_REPLAY_FAIL_NONE = 0,    /* 未失败 */
    JRN_REPLAY_FAIL_L2P_READ,    /* 顺序页取 old PBA 失败（l2p_read_sync）*/
    JRN_REPLAY_FAIL_L2P_WRITE,   /* 回放写 L2P 失败（l2p_write_sync）*/
    JRN_REPLAY_FAIL_MATERIALIZE, /* PBA 的块无法进入可记账状态 */
    JRN_REPLAY_FAIL_VALID_ADD,   /* valid_count+1 守卫失败（越界 / 上溢）*/
    JRN_REPLAY_FAIL_VALID_SUB,   /* valid_count−1 守卫失败（越界 / 下溢）*/
} jrn_replay_fail_e;

typedef struct JOURNAL_REPLAY_CONTEXT_S
{
    BOOL(*materialize)
    (void *ctx, DWORD pba);
    void (*valid_add)(void *ctx, DWORD pba, BOOL *ok);
    void (*valid_sub)(void *ctx, DWORD pba, BOOL *ok);
    void *ctx; /* 调用者自持（PFR 传 &g_ftl[...]） */
    BOOL  ok;
    BYTE  fail_reason; /* jrn_replay_fail_e：**首次**失败的原因（后续不覆盖）*/
    DWORD fail_pba;    /* 首次失败时正在记账的 PBA（NAND_PBA_INVALID = 无）*/
} jrn_replay_ctx_s;

/**
 * @brief 本次开机已提交的 Journal 页写次数（观测：页写放大 / 断点频率）。
 *
 * @param journal Journal
 * @return 页写命令累计提交数
 */
static inline DWORD jrn_page_writes(const l2p_jrn_s *journal)
{
    return journal->handle.state.page_writes;
}

/* ── 查询（替代外部直读 handle 字段）── */

/**
 * @brief 无在飞 job（写 / 擦都完成）。
 *
 * @param journal Journal
 * @return true = phase 为 JRN_IDLE
 *         false = 页写或换块擦除在飞
 */
static inline BOOL jrn_is_idle(const l2p_jrn_s *journal)
{
    return journal->handle.job.phase == JRN_IDLE;
}

/**
 * @brief 下一个待分配序号（= CP 截断水位）。
 *
 * @param journal Journal
 * @return sequence_counter 当前值
 */
static inline DWORD jrn_sequence_get(const l2p_jrn_s *journal)
{
    return journal->handle.state.sequence_counter;
}

/**
 * @brief 已成功落盘的 Journal 序号上界（exclusive）
 *
 * 用途：L2P 区域写回的 WAL 次序闸门——区域页只能在覆盖其映射的
 * Journal 条目落盘之后写。调用者（`l2p_flush_kick`）在 `!jrn_seq_ge(persisted, dirty)`
 * 时退让重试（串行号语义，回绕安全），由 Journal 服务把当前页冻结并落盘。
 *
 * @param journal Journal
 * @return 已落盘条目的上界：序号 < 该值的条目均已持久化
 */
static inline DWORD jrn_persisted_seq(const l2p_jrn_s *journal)
{
    return journal->handle.state.persisted_seq;
}

/* ── 序号比较（32-bit 回绕安全，串行号算术）──
 *
 * 页头 `page_seq` / `sequence_counter` / CP `journal_sequence` 都是 32-bit 且跨重启
 * 单调；累计约 2 TiB host 写（2^32 条）后回绕，直接 `>` / `>=` 会在回绕点判反。
 * 串行号语义（RFC 1982，公开资料；本仓出处层未收录）：只要**所有活值间距 < 2^31**
 * 就比较正确——本仓活序号只有环形页（4×64×508 ≈ 13 万）、`dirty_seq`（CP 前必刷，
 * 同受环容量约束）与 `last_journal_sequence`（一个 CP 间隔），远小于 2^31。
 *
 * ⚠ 凡是「两个 Journal 序号谁更新」的判断一律走这里，不写裸比较（C9：一套判据）。
 */
/**
 * @brief a 在 b 之后（a 更新，且 a != b）。
 *
 * @param a 序号
 * @param b 序号
 * @return true = a 比 b 新
 */
static inline BOOL jrn_seq_after(DWORD a, DWORD b)
{
    return ((int32_t)(a - b) > 0);
}

/**
 * @brief a 不早于 b（a == b 或 a 更新）。
 *
 * @param a 序号
 * @param b 序号
 * @return true = a >= b（串行号语义）
 */
static inline BOOL jrn_seq_ge(DWORD a, DWORD b)
{
    return ((int32_t)(a - b) >= 0);
}

/**
 * @brief 持久性失败 pending 是否置位。
 *
 * @param journal Journal
 * @return true = 有条目已丢，等 Checkpoint 兜底
 *         false = 无持久性失败
 */
static inline BOOL jrn_has_write_failure(const l2p_jrn_s *journal)
{
    return journal->handle.state.write_fail_pending;
}

/**
 * @brief 页缓冲里是否有未落盘条目。
 *
 * @param journal Journal
 * @return true = current_entry 非 0
 *         false = 页缓冲为空
 */
static inline BOOL jrn_has_pending_entries(const l2p_jrn_s *journal)
{
    return journal->handle.state.current_entry != 0U;
}

/**
 * @brief 是否有「未起」的换块擦除（新环形块尚未擦）。
 *
 * @param journal Journal
 * @return true = need_erase 置位
 *         false = 无待偿擦除
 */
static inline BOOL jrn_has_erase_pending(const l2p_jrn_s *journal)
{
    return journal->handle.job.need_erase;
}

/**
 * @brief CP 水位标志是否置位（≥85%）。
 *
 * @param journal Journal
 * @return true = checkpoint_needed 置位
 *         false = 未置位
 */
static inline BOOL jrn_needs_checkpoint(const l2p_jrn_s *journal)
{
    return journal->handle.state.checkpoint_needed;
}

/**
 * @brief 紧急水位标志是否置位（≥95%）。
 *
 * @param journal Journal
 * @return true = emergency 置位
 *         false = 未置位
 */
static inline BOOL jrn_is_emergency(const l2p_jrn_s *journal)
{
    return journal->handle.state.emergency;
}

/** @brief 取本实例的 Journal 环形工作区（定义在 journal.c，DTCM）。 */
l2p_jrn_s *jrn_workspace_get(BYTE die);

/**
 * @brief 只读扫环，取最新页（最大 page_seq），不改 journal 状态。
 *
 * @note 与回放共用同一套页头判定；`out_read_error == true` ⇒ 环内有读失败页，
 *       最新页与「干净/掉电」判型都不可信，启动恢复应走掉电回放路径。
 */
BOOL jrn_page_scan_latest(const l2p_jrn_s *journal, jrn_page_ref_s *out, BOOL *out_read_error);

/**
 * @brief 恢复 Journal 的 RAM 写态（写指针 + 序号），不回放任何条目。
 *
 * @note 必须做：`jrn_init` 复位写指针/序号，但 NAND 环形块并未擦除。
 *       不恢复 ⇒ ① 序号从 1 重来，新条目被回放的 page_seq 过滤整页丢弃；
 *       ② 写指针回到 block0/page0，对已编程页二次编程。
 * @note 落点 = 最新页之后一页；跨块时先擦下一块，擦除失败挂起 `job.need_erase`。
 */
void jrn_resume(l2p_jrn_s *journal, const jrn_page_ref_s *latest, DWORD floor_sequence);

/**
 * @brief 非阻塞预留当前页的 entries 个槽位（16 B 通用页的调用者用）。
 *
 * @note 顺序页（JRN_PAGE_SEG）只接顺序条目：遇到它会先起异步刷页并返回 false。
 * @note 只可能起一条**异步**页写，绝不同步刷页 / 等待 / ftl_op_drain——
 *       它会被 ftl_confirm_write 调用，必须保持 completion/service 调用栈无泵。
 */
BOOL jrn_reserve(l2p_jrn_s *journal, WORD entries);

/**
 * @brief 追加一条 L2P 更新；同一 LBA 在当前页缓冲里就地更新（不新增条目、不耗 sequence）。
 *
 * @note 返回 false（页满 / 当前页是顺序页）时调用者必须按「映射未持久化」处理：
 *       迁移侧中止 job（victim 不擦）。**cache 顺序写确认不走本函数**（走
 *       jrn_append_seq）。
 */
BOOL jrn_append(l2p_jrn_s *journal, DWORD lba, DWORD old_pba, DWORD new_pba);

/** @brief 当前顺序页还能追加多少条（非顺序页 / 页未打开 = 0；页满处主动 END 的唯一判据）。 */
WORD jrn_seq_room(const l2p_jrn_s *journal);

/**
 * @brief 顺序页容量预留（提交路径：只判页型 / 容量，不判坐标）。
 *
 * @note 提交时 `current_entry` 只反映**已确认**条目，落后于提交点 ⇒ 这里不能做
 *       隐式坐标校验（会把有未确认页的窗口误判成断裂）；坐标校验在确认路径的
 *       jrn_seq_is_ready 做。
 */
BOOL jrn_seq_reserve(l2p_jrn_s *journal, WORD entries);

/**
 * @brief 顺序页确认前检查：页型 / 隐式坐标 / 容量（host 写确认路径动账前调用）。
 *
 * @note 坐标与容量必须在追加**之前**判掉：部分追加后再失败会让重试从页首重发，
 *       与隐式下标冲突。
 */
BOOL jrn_seq_is_ready(l2p_jrn_s *journal, DWORD lba, WORD entries);

/** @brief 顺序页追加一条：lba 必须等于 base_lba + 已追加条数。 */
BOOL jrn_append_seq(l2p_jrn_s *journal, DWORD lba, DWORD new_pba);

/** @brief 当前 Journal 使用率（0..100，全局已用条目 / 全环容量）。 */
BYTE jrn_watermark_get(const l2p_jrn_s *journal);

/**
 * @brief Checkpoint Header 成功后清除持久性失败 pending。
 *
 * @note 仅当该 CP 的 journal 水位 ≥ 失败时刻的 sequence 才清除；否则保留 pending，
 *       让下一次 CP 再试。
 */
void jrn_write_failure_clear(l2p_jrn_s *journal, DWORD cp_journal_seq);

/** @brief CP Header 成功后设置合并边界（sequence 小于它的旧条目不得被同 LBA 原地合并）。 */
void jrn_merge_floor_set(l2p_jrn_s *journal, DWORD sequence);

/** @brief 按当前写位置重算并置位 CP 水位标志（写确认路径每次 L2P 更新后调用）。 */
void jrn_watermark_update(l2p_jrn_s *journal);

/** @brief 强制请求一次 Checkpoint（只置水位标志；由后台 CP 任务在安全点执行）。 */
void jrn_checkpoint_request(l2p_jrn_s *journal);

/**
 * @brief 全量回放未进 L2P 表的页（按 (page_seq, entry_index) 递增序，per-LBA LWW）。
 *
 * @note 需回放判据是**整页**：`page_seq + entry_count > cp_sequence`；只看页首
 *       `page_seq > cp_sequence` 会丢掉跨水位那一页的新条目。
 * @note out_entries 不能由 RAM 写指针反推：回放末尾 jrn_resume 把落点放到最新页之后
 *       并清 current_entry ⇒ 用 current_entry/current_page 算出来恒为 0。
 * @note 返回 FTL_ERROR 表示环内有读失败页（写态已恢复，但回放可能不完整 ⇒ 调用者
 *       应回退 full metadata rebuild）。
 */
ftl_status_e jrn_replay(l2p_jrn_s *journal, l2p_cache_s *cache, DWORD cp_sequence, DWORD *out_entries, jrn_replay_ctx_s *block_ctx);

/** @brief 最近一次 jrn_replay 命中「降级 L2P region」的条目数（只读诊断）。 */
DWORD jrn_replay_degraded_entries(void);

/**
 * @brief 截断 Journal 的第一步：水位校验 + 算出待预擦的块（纯 RAM，不动介质）。
 *
 * @note 水位**必须覆盖全部已写条目**（调用者传 CP 时刻的 sequence_counter）；
 *       水位不足时**不截断**——最旧块可能仍持有回放需要的条目。
 * @note 预擦记账使 jrn_handle 换块回切时跳过擦除（同一块每圈只擦一次）。
 */
jrn_truncate_step_e jrn_truncate_plan(l2p_jrn_s *journal, DWORD up_to_sequence, DWORD *out_addr, BYTE *out_block);

/**
 * @brief 截断 Journal 的第二步：记账 + 复位水位标志（擦除命令成功完成后调用）。
 *
 * @note `erased_block == JRN_BLOCK_NONE` ⇒ **只复位水位标志，不动
 *       pre_erased_block**；覆写成 NONE 会让已预擦的块被再擦一次。
 */
void jrn_truncate_commit(l2p_jrn_s *journal, BYTE erased_block);

/** @brief 初始化 Journal：记录拓扑、绑定 allocator、复位写指针与序号。 */
void jrn_init(l2p_jrn_s *journal, ftl_topo_s topo, alc_s *alloc);

/* ── 顺序流映射页（JRN_PAGE_SEG；docs/design/ftl_design.md §3.16 / §3.7.3）──
 *
 * 写入模型：一条顺序流 = 连续的 host 写页；其确认条目按**提交顺序**到达，因此
 * LBA 可以由 `base_lba + 页内下标` 隐式表达，每页装 508 条（16B 通用页只有 127）。
 * 四个入口的分工：
 *   jrn_seq_begin   —— 顺序流 START 时调用：把当前页切换/刷出成空白顺序页；
 *   jrn_seq_reserve —— **提交路径**的容量预留（不校验坐标：确认尚未发生）；
 *   jrn_seq_is_ready   —— **确认路径**的页型 / 隐式坐标 / 容量检查；
 *   jrn_append_seq  —— 追加一条（只记 new_pba）。
 * ⚠ 顺序页**专用于顺序流**：普通 append/reserve 遇到顺序页会先把它刷出
 *   （空页也刷，保持页型判定单一），因此两种页型不会混装。 */

/**
 * @brief 顺序流开始：确保当前页可接收新的顺序段（尽量跨命令续用同一张页）。
 *
 * @note 续用判据：页型是顺序页、已有条目、剩余空间 ≥ need、且
 *       `lba == page_base + current_entry`（LBA 连续）⇒ 不刷页，直接续写。
 *       否则旧页只起**异步**刷页，刷出后切到新顺序页。
 */
BOOL jrn_seq_begin(l2p_jrn_s *journal, DWORD lba, WORD need);

/* ── 模块 handle（typed 入口，docs/archive/ftl_module_contracts.md §3.3 / §4.2）── */

/** @brief 后台推进 Journal（提交即返回；job 在飞或无事可做时为空操作）。 */
void jrn_handle(l2p_jrn_s *journal, const jrn_trigger_e *trigger);

#endif /* FTL_JOURNAL_H */
