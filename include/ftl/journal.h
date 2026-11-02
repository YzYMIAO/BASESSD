#ifndef FTL_JOURNAL_H
#define FTL_JOURNAL_H

/*
 * journal.h — L2P 增量日志：页/条目格式 + 环形状态 + API
 *
 * 职责：
 *   定义 Journal 的磁盘/内存布局（页头、条目、环形状态 l2p_jrn_t）与对外 API。
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
 *   ftl_contract_audit 在 boot 期证明，运行期入口不再逐次复检。
 *
 * 写模型：
 *   追加进 RAM 页缓冲（page_buf）——**页满异步落盘**，交给 job
 *   （jrn_handle / g_op_callbacks[FTL_OP_JOURNAL]）。数据面一律先预留、不足则停泊
 *   重试，**模块不提供同步刷页出口**：通用页用 jrn_reserve，cache 顺序写确认用
 *   jrn_seq_ready + jrn_append_seq（顺序流映射页，见下）。
 */

#include "allocator.h"  /* alc_t（持续擦除失败 → bbt_mark_bad） */
#include "ftl_config.h" /* DWORD */
#include "ftl_type.h"
#include "l2p_cache.h"
#include "metadata.h"    /* JOURNAL_BLOCK0 / JOURNAL_BLOCK_COUNT 块位置 */
#include "nand_config.h" /* PAGE_MAIN_SIZE */

#include <stdint.h>

/* Journal 块位置：第 b 块 = JOURNAL_BLOCK0 + b，共 JOURNAL_BLOCK_COUNT 块
 * （块位置与块数见 ftl/metadata.h「固定元数据区布局」）*/

/* Journal 页大小 = 一页主区（由 nand_config.h 推出）*/
#define JRN_PAGE_SIZE (PAGE_MAIN_SIZE)

/* 「无预擦除块」哨兵（truncate 预擦除记账，见 l2p_jrn_t） */
#define JRN_BLOCK_NONE (0xFFU)

/* Journal Page Header
 *   magic(4) + page_seq(4) + entry_count(2) + block_seq(2) + base_lba(4)。
 * base_lba 只对顺序流映射页有效（LBA 显式页恒 0）。
 * ⚠ 介质字节序：页头与条目的多字节字段**一律小端**（与 ONFI 参数页口径一致，
 *   见 `src/nand/onfi_1_0.c` 的字节序说明）。写入/解析都用 memcpy 原生字，
 *   目标平台（Cortex-M7，小端）与格式一致；换大端平台必须在此处集中转换。 */
#define JRN_HEADER_SIZE (16U)
#define JRN_MAGIC       (0x524F554AUL) /* "JOUR"：LBA 显式页（16B 条目） */
#define JRN_MAGIC_SEG   (0x4745534AUL) /* "JSEG"：顺序流映射页（4B 条目，LBA = base_lba + 下标） */

/* 一条 L2P 更新：lba 的映射从 old_pba 变为 new_pba（16B，packed 无填充） */
typedef struct
{
    DWORD lba;      /* 逻辑地址 */
    DWORD old_pba;  /* 更新前物理地址（回放不读；仅首条保留，供诊断） */
    DWORD new_pba;  /* 更新后物理地址（回放按此写 L2P） */
    DWORD sequence; /* 全局单调序号（回放按 (page_seq, 页内下标) 定序） */
} __attribute__((packed)) l2p_jrn_entry_t;

/* 顺序流映射页的一条：只存 new_pba；lba = base_lba + 页内下标，
 * sequence = page_seq + 页内下标（两条坐标都由位置隐式给出，故不落盘）。 */
typedef DWORD l2p_jrn_seg_entry_t;

/* 每页条目数 = (页主区 − 页头) ÷ 条目大小，向下取整。
 * ⚠ 必须推导：条目 16B 时 128 条 ⇒ 16 + 128×16 = 2064 > 2048，
 *   第 127 条会写出 page_buf 之外（踩到 l2p_jrn_t 的后续字段）。*/
#define JRN_ENTRIES_PER_PAGE     ((JRN_PAGE_SIZE - JRN_HEADER_SIZE) / sizeof(l2p_jrn_entry_t))
#define JRN_SEG_ENTRIES_PER_PAGE ((JRN_PAGE_SIZE - JRN_HEADER_SIZE) / sizeof(l2p_jrn_seg_entry_t))

/* 当前页缓冲的条目类型。页型由**本页第一条 append 的调用方**决定：
 * 顺序流的确认走 jrn_append_seq（顺序页），其余一律走 jrn_append（LBA 显式页）。 */
typedef enum
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
typedef enum
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
typedef enum
{
    JRN_TRUNC_ALREADY     = 0U, /* 无需擦（该块已预擦过）*/
    JRN_TRUNC_NEEDS_ERASE = 1U, /* 需擦 out_addr / out_block */
    JRN_TRUNC_NO_TRUNCATE = 2U, /* 水位不满足：不截断（调用者不得复位水位标志）*/
} jrn_truncate_step_e;

/*
 * jrn_handle_t — 本模块的**执行状态**（FSM / 写游标 / 持久性失败观测）
 *
 * 归属：`l2p_jrn_t.handle`（per-instance）。共享账本（L2P cache / BST /
 * allocator）不在这里；需要它们时走 `l2p_jrn_t` 里的端口。
 *
 * 主流程**不读**本结构的 phase / 游标 / 失败位：只走查询接口
 * （docs/archive/ftl_module_contracts.md §4.2）。
 */
typedef struct
{
    /* ── 异步刷页 job ── */
    jrn_phase_e phase;        /* 异步刷页状态机 */
    bool        need_erase;   /* 有**未起**的擦除：target_block 待擦（擦命令起不来时留待重试） */
    BYTE        target_block; /* 页写成功后 current_block 的落点 */
    BYTE        target_page;  /* 页写成功后 current_page 的落点（已做块内回绕） */
    BYTE        from_block;   /* 起写时的 current_block（防指针回退的守卫，见 journal.c） */
    BYTE        from_page;    /* 起写时的 current_page */
    DWORD       waddr;        /* 在飞页地址（诊断 / 心跳） */

    /* ── 环形写游标 ── */
    BYTE  current_block;
    BYTE  current_page;
    WORD  current_entry; /* 当前页已追加条数（顺序页可到 JRN_SEG_ENTRIES_PER_PAGE=508 ⇒ 不能是 BYTE） */
    DWORD sequence_counter;

    /* 当前页缓冲的条目类型与顺序页基准 LBA（JRN_PAGE_SEG 时有效）。
     * base 在**首条 jrn_append_seq 时**落定，不在 jrn_seq_begin 时定：顺序流的
     * 确认是异步的，上个序列的尾页可能在新序列 START 之后才确认——按首条确认的
     * LBA 定基准，隐含坐标（base + 下标）才与确认顺序一致。 */
    jrn_page_kind_e page_kind;
    DWORD           page_base;

    /* CP 边界：sequence < merge_floor 的旧条目**不得**被同 LBA 原地合并。
     * 否则 CP 后的覆盖写会复用 CP 前的 sequence 却带新的 new_pba ⇒
     * 块元数据快照 增量回放按 sequence 过滤时漏记，之后覆盖写可能撞 valid_count 下溢断言。
     * 0 = 不设门（开机/初始化）。 */
    DWORD merge_floor;

    /* 被 journal_truncate **预擦除**、等待 flush_page 回切复用的环形块号
     * （JRN_BLOCK_NONE = 无）。记账目的 = 同一块每圈只擦一次：truncate 擦环形
     * 下一块后，flush_page 回切时跳过擦除。 */
    BYTE pre_erased_block;

    BYTE page_buf[JRN_PAGE_SIZE];

    bool checkpoint_needed;
    bool emergency;

    /* ── 环形块坏块策略（RISK-JN-01，全部 RAM 态）──
     *   bad_mask     —— 已判定不可用的环形块（bit b）；持久化由 allocator/BBT owner API 承担，
     *                   重启后 BST BAD 会重新挡住这些块。
     *   erase_fail_count / erase_fail_block —— 同一块连续擦除失败计数；
     *                   达 JRN_ERASE_FAIL_MAX 即标坏并跳到下一可用块。
     *   ring_full    —— 全部环形块不可用：journal 停止写入（不擦未擦块、不无限重试）。 */
    BYTE bad_mask;
    BYTE erase_fail_count;
    BYTE erase_fail_block;
    bool ring_full;

    /* ── 持久性失败观测（全部 RAM，不随介质持久化）──
     * 页写 / 换块擦失败时 +1 并置 pending。`write_fail_seq` 是失败时刻的
     * sequence_counter；pending 一直阻止 victim 擦除（GC/WL/predict），直到一次
     * Checkpoint Header 成功且其 journal_seq ≥ write_fail_seq —— 此时迁移后的
     * L2P 已随 CP 落盘，丢掉的那条 journal 条目不再是唯一副本。
     * ⚠ 不尝试恢复已丢条目；pending 只负责“不撒谎地说屏障已达成”。 */
    DWORD write_fail_count;
    DWORD write_fail_seq;
    bool  write_fail_pending;

    /* 心跳基准（见 ftl_job.h）：在飞期间每满 1s 打一行 phase/age */
    DWORD start_ms;
    DWORD log_ms;

    /* 本次开机提交的页写次数（观测：元数据页写放大 / 断点频率）。 */
    DWORD page_writes;

    /* ── WAL 水位（RISK-L2P-09）──
     * persisted_seq = 已**成功落盘**的条目序号上界（exclusive）。
     * 在飞页的序号区间在 freeze 时记入 [write_page_seq, +write_entry_count)，
     * 页写成功后推进 persisted_seq。L2P 区域写回必须等 persisted_seq ≥ 该行的
     * dirty_seq，保证「日志先于数据」——否则掉电后会出现「映射在、valid_count 缺」，
     * GC 可能把仍被 L2P 引用的块当空块回收（静默丢数据）。 */
    DWORD persisted_seq;
    DWORD write_page_seq;
    WORD  write_entry_count;
} jrn_handle_t;

typedef struct l2p_jrn_s
{
    ftl_topo_t topo; /* 目标 NAND 芯片路由 */

    /* 坏块查询/标坏只走 allocator 的 owner API（持续擦除失败 → 标坏）。
     * journal 不直接读写 allocator 的 BST/BBT 内部字段。 */
    alc_t *alloc;

    /* 环形块候选列表（ring 下标 → 物理块号）。由 jrn_init 从
     * alloc->pools->jrn 复制；固定 JOURNAL_BLOCK_COUNT 项，缺额由 shared
     * metadata spare 补足（凑不齐则池构建失败，不进入运行期）。 */
    WORD blocks[JOURNAL_BLOCK_COUNT];

    /* 异步刷页 job / 环形写游标 / 失败观测（见 jrn_handle_t） */
    jrn_handle_t handle;
} l2p_jrn_t;

/* Journal 环形页引用：位置 + 页序号 + 页型 + 条目数（jrn_page_scan_latest 出参） */
typedef struct
{
    DWORD           page_seq;    /* 页头记录的**首条**条目序号 */
    DWORD           base_lba;    /* JRN_PAGE_SEG：本页首条条目的 LBA（其余页型为 0） */
    BYTE            block;       /* 环形块号（0..JOURNAL_BLOCK_COUNT-1） */
    BYTE            page;        /* 块内页号 */
    WORD            entry_count; /* 本页条目数（顺序页可到 508 ⇒ WORD） */
    jrn_page_kind_e kind;        /* 页型（回放按页型解析条目） */
} jrn_page_ref_t;

/*
 * jrn_trigger_e — 本轮调用的意图
 *
 * FLUSH       推进「补擦 + 页写」完整流程（预算任务）；
 * ERASE_ONLY  只补换块擦除，不起页写（must-run：新环形块没擦过就不能写，但
 *             host 饱和档要让路给前台）。
 */
typedef enum
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
typedef enum
{
    JRN_REPLAY_FAIL_NONE = 0,    /* 未失败 */
    JRN_REPLAY_FAIL_L2P_READ,    /* 顺序页取 old PBA 失败（l2p_read_sync）*/
    JRN_REPLAY_FAIL_L2P_WRITE,   /* 回放写 L2P 失败（l2p_write_sync）*/
    JRN_REPLAY_FAIL_MATERIALIZE, /* PBA 的块无法进入可记账状态 */
    JRN_REPLAY_FAIL_VALID_ADD,   /* valid_count+1 守卫失败（越界 / 上溢）*/
    JRN_REPLAY_FAIL_VALID_SUB,   /* valid_count−1 守卫失败（越界 / 下溢）*/
} jrn_replay_fail_e;

typedef struct
{
    bool (*materialize)(void *ctx, DWORD pba);
    void (*valid_add)(void *ctx, DWORD pba, bool *ok);
    void (*valid_sub)(void *ctx, DWORD pba, bool *ok);
    void *ctx; /* 调用者自持（PFR 传 &g_ftl[...]） */
    bool  ok;
    BYTE  fail_reason; /* jrn_replay_fail_e：**首次**失败的原因（后续不覆盖）*/
    DWORD fail_pba;    /* 首次失败时正在记账的 PBA（NAND_PBA_INVALID = 无）*/
} jrn_replay_ctx_t;

/**
 * @brief 初始化 Journal：记录拓扑、绑定 allocator、复位写指针与序号
 *
 * @param journal 目标实例
 * @param topo    目标 NAND 芯片路由（ch/ce/lun）
 * @param alloc   allocator；取环形块候选池，擦除持续失败时经其 owner API 标坏
 */
void jrn_init(l2p_jrn_t *journal, ftl_topo_t topo, alc_t *alloc);

/**
 * @brief 只读扫环，取**最新页**（最大 page_seq）。
 *
 * 用途：① 启动判型（干净关机 / 掉电）；② 干净关机的写指针恢复。**不改 journal 状态**，
 * 与回放共用同一套页头判定。
 *
 * @param journal        Journal
 * @param out            出参：最新页引用（环空时全置 0，可为 NULL）
 * @param out_read_error 出参（可为 NULL）：true = 环内存在读失败页 ⇒ 最新页与
 *        「干净/掉电」判型都不可信，启动恢复应走掉电回放路径
 * @return true = 环非空（*out 有效）
 *         false = 环空（全部擦除 / 无有效页）
 */
bool jrn_page_scan_latest(const l2p_jrn_t *journal, jrn_page_ref_t *out, bool *out_read_error);

/**
 * @brief 恢复 Journal 的 RAM 写态（写指针 + 序号），**不回放任何条目**。
 *
 * 为什么必须做：`jrn_init` 把写指针复位到 block0/page0、序号复位到 1，但
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
 * @param latest  jrn_page_scan_latest 的结果（NULL 或 page_seq==0 视为环空）
 * @param floor_sequence 序号下限 = Checkpoint 的 jrn_sequence_get（「下一个待分配」），
 *        兜住「环空」与「环内最新页序号反而落后」两种情形（0 会被抬到 1）
 */
void jrn_resume(l2p_jrn_t *journal, const jrn_page_ref_t *latest, DWORD floor_sequence);

/**
 * @brief 非阻塞预留当前页的 entries 个槽位（16 B 通用页的调用者用）。
 *
 * 成功 ⇒ 调用者随后追加 entries 条时**不会**触发 jrn_append 的页满分支；
 * 失败 ⇒ 当前页不够 / 当前页是顺序页 / 异步刷页起不来（在飞 / 待擦 / 池空 / 环满），
 * 调用者应返 FTL_AGAIN 停泊，**不得**追加条目或改 L2P。
 *
 * ⚠ 顺序页（JRN_PAGE_SEG）只接顺序条目：本函数遇到它会先起异步刷页并返回 false，
 *   下一轮重试落到一张通用页（空顺序页也刷，见 §3.16.5）。
 * ⚠ 本函数只可能起一条**异步**页写，绝不同步刷页 / 等待 / ftl_op_drain：
 *   它会被 ftl_confirm_write 调用，必须保持 completion/service 调用栈无泵。
 * @param journal Journal
 * @param entries 本次最多追加的条目数（1..JRN_ENTRIES_PER_PAGE-1）
 * @return true = 已预留
 *         false = 背压 / 页型不符（已起刷页）
 */
bool jrn_reserve(l2p_jrn_t *journal, WORD entries);

/**
 * @brief 追加一条 L2P 更新。若当前页缓冲里已有**同一 LBA** 的条目，就地更新
 * （只改 new_pba，不新增条目、不消耗 sequence）——覆盖写工作集下减少 Journal 页数。
 *
 * @param journal Journal
 * @param lba     逻辑地址
 * @param old_pba 更新前物理地址（就地合并时保留首条，回放不读）
 * @param new_pba 更新后物理地址
 * @return true = 已追加 / 就地合并
 *         false = 页满 / 当前页是顺序页，本条目未写入。⚠ 返回 false 时调用者必须按
 *               “映射未持久化”处理：迁移侧中止 job（victim 不擦）；
 *               GC/WL / TRIM / wbuf 写确认先用 jrn_reserve 预留，故其页满分支结构性
 *               不可达；**cache 顺序写确认不走本函数**（走 jrn_append_seq）。
 */
bool jrn_append(l2p_jrn_t *journal, DWORD lba, DWORD old_pba, DWORD new_pba);

/* ── 顺序流映射页（JRN_PAGE_SEG；docs/design/ftl_design.md §3.16 / §3.7.3）──
 *
 * 写入模型：一条顺序流 = 连续的 host 写页；其确认条目按**提交顺序**到达，因此
 * LBA 可以由 `base_lba + 页内下标` 隐式表达，每页装 508 条（16B 通用页只有 127）。
 * 四个入口的分工：
 *   jrn_seq_begin   —— 顺序流 START 时调用：把当前页切换/刷出成空白顺序页；
 *   jrn_seq_reserve —— **提交路径**的容量预留（不校验坐标：确认尚未发生）；
 *   jrn_seq_ready   —— **确认路径**的页型 / 隐式坐标 / 容量检查；
 *   jrn_append_seq  —— 追加一条（只记 new_pba）。
 * ⚠ 顺序页**专用于顺序流**：普通 append/reserve 遇到顺序页会先把它刷出
 *   （空页也刷，保持页型判定单一），因此两种页型不会混装。 */

/**
 * @brief 顺序流开始：确保当前页可接收新的顺序段（**尽量跨命令续用同一张页**）。
 *
 * @param journal Journal
 * @param lba     本顺序段第一条条目的 LBA（窗口基址）
 * @param need    本段至少需要多少条空间（通常 = 一个窗口的条目数）
 * @return true = 当前页可接收顺序条目（已切换 / 已续用）
 *         false = 旧页刷出起不来（在飞 / 待擦 / 池空 / 序列门控）⇒ 调用者本次不要
 *                 开 cache 序列，回退普通写路径
 *
 * @note 续用判据（把"一张页填满再断"做实）：页型是顺序页、已有条目、剩余空间
 *       ≥ need、且 `lba == page_base + current_entry`（LBA 连续）⇒ **不刷页**，
 *       直接在同一页继续追加。这样一张 JSEG 页可以覆盖多条 host 命令的连续写入
 *       （最多 63 pair ≈ 252 KiB），元数据落盘频率按页而不是按命令。
 *       否则旧页只起**异步**刷页（jrn_freeze），刷出后页缓冲归零并切到新顺序页。
 *       基准 LBA 仍由首条 append 落定（见 jrn_handle_t）。
 */
bool jrn_seq_begin(l2p_jrn_t *journal, DWORD lba, WORD need);

/**
 * @brief 当前顺序页还能追加多少条（非顺序页 / 页未打开 = 0）。
 *
 * 写路径按它收缩窗口并在**页满**处主动 END：这是"顺序页写满再断"的唯一判据。
 *
 * @param journal Journal
 * @return 剩余条目数（0..JRN_SEG_ENTRIES_PER_PAGE）
 */
WORD jrn_seq_room(const l2p_jrn_t *journal);

/**
 * @brief 本次开机已提交的 Journal 页写次数（观测：页写放大 / 断点频率）。
 *
 * @param journal Journal
 * @return 页写命令累计提交数
 */
DWORD jrn_page_writes(const l2p_jrn_t *journal);

/**
 * @brief 顺序页容量预留（**提交路径**用：只判页型 / 容量，不判坐标）。
 *
 * @param journal Journal
 * @param entries 本次窗口最多追加的条目数（1..JRN_SEG_ENTRIES_PER_PAGE-1）
 * @return true = 当前页装得下（顺序页，或可转顺序页的空普通页）
 *         false = 需要先刷页 / 环不可用 ⇒ 调用者把本窗口当断点（END）
 *
 * @note 提交时 `current_entry` 只反映**已确认**条目，落后于提交点，因此这里
 *       **不能**做隐式坐标校验（那会把每条有未确认页的窗口误判成断裂）。
 *       坐标校验在确认路径的 jrn_seq_ready 做。
 */
bool jrn_seq_reserve(l2p_jrn_t *journal, WORD entries);

/**
 * @brief 顺序页确认前检查：页型 / 隐式坐标 / 容量（host 写确认路径动账前调用）。
 *
 * @param journal Journal
 * @param lba     本批条目的起始 LBA
 * @param entries 本批条目数（1..JRN_SEG_ENTRIES_PER_PAGE-1）
 * @return true = 随后 jrn_append_seq 不会因页满 / 坐标断裂失败
 *         false = 页型或坐标不符（已起异步刷页）/ 环不可用 ⇒ 调用者停泊重试
 *
 * @note 坐标与容量必须在追加**之前**判掉：部分追加后再失败会让重试从页首重发，
 *       与隐式下标冲突。确认按提交顺序到达，故 current_entry 就是本批的位置。
 */
bool jrn_seq_ready(l2p_jrn_t *journal, DWORD lba, WORD entries);

/**
 * @brief 顺序页追加一条：lba 必须等于 current 页的 base_lba + 已追加条数。
 *
 * 首条 append 落定本页 base_lba；entry_count 与 sequence_counter 同步 +1
 * （页头 page_seq 仍由 `sequence_counter − current_entry` 反推）。
 *
 * @param journal Journal
 * @param lba     逻辑地址（必须与隐式位置一致）
 * @param new_pba 更新后物理地址
 * @return true = 已追加
 *         false = 环不可用 / 页型或隐式坐标不符（调用者停泊，由刷页后重试恢复）
 */
bool jrn_append_seq(l2p_jrn_t *journal, DWORD lba, DWORD new_pba);

/* ── 模块 handle（typed 入口，docs/archive/ftl_module_contracts.md §3.3 / §4.2）── */

/**
 * @brief 后台推进 Journal（提交即返回；job 在飞或无事可做时为空操作）
 *
 * @param journal  Journal；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param trigger  本轮意图（NULL 视作 FLUSH）
 */
void jrn_handle(l2p_jrn_t *journal, const jrn_trigger_e *trigger);

/* ── 查询（替代外部直读 handle 字段）── */

/**
 * @brief 无在飞 job（写 / 擦都完成）。
 *
 * @param journal Journal
 * @return true = phase 为 JRN_IDLE
 *         false = 页写或换块擦除在飞
 */
bool jrn_is_idle(const l2p_jrn_t *journal);

/**
 * @brief 下一个待分配序号（= CP 截断水位）。
 *
 * @param journal Journal
 * @return sequence_counter 当前值
 */
DWORD jrn_sequence_get(const l2p_jrn_t *journal);

/**
 * @brief 已成功落盘的 Journal 序号上界（exclusive）
 *
 * 用途：L2P 区域写回的 WAL 次序闸门（RISK-L2P-09）——区域页只能在覆盖其映射的
 * Journal 条目落盘之后写。调用者（`l2p_flush_kick`）在 `persisted_seq < dirty_seq`
 * 时退让重试，由 Journal 服务把当前页冻结并落盘。
 *
 * @param journal Journal
 * @return 已落盘条目的上界：序号 < 该值的条目均已持久化
 */
DWORD jrn_persisted_seq(const l2p_jrn_t *journal);

/**
 * @brief 当前使用率。
 *
 * @param journal Journal
 * @return 0..100（全局已用条目 / 全环条目容量）
 */
BYTE jrn_watermark_get(const l2p_jrn_t *journal);

/**
 * @brief 持久性失败 pending 是否置位。
 *
 * @param journal Journal
 * @return true = 有条目已丢，等 Checkpoint 兜底
 *         false = 无持久性失败
 */
bool jrn_has_write_failure(const l2p_jrn_t *journal);

/**
 * @brief Checkpoint Header 成功后清除持久性失败 pending。
 *
 * 仅当该 CP 的 journal 水位（起点快照）≥ 失败时刻的 sequence 才清除：说明 CP 在
 * 失败之后启动，已把迁移后的 L2P 区域落盘；否则保留 pending，让下一次 CP 再试。
 *
 * @param journal        Journal
 * @param cp_journal_seq 本次 CP 的 journal 水位（起点快照）
 */
void jrn_write_failure_clear(l2p_jrn_t *journal, DWORD cp_journal_seq);

/**
 * @brief 页缓冲里是否有未落盘条目。
 *
 * @param journal Journal
 * @return true = current_entry 非 0
 *         false = 页缓冲为空
 */
bool jrn_has_pending_entries(const l2p_jrn_t *journal);

/**
 * @brief 是否有「未起」的换块擦除（新环形块尚未擦）。
 *
 * @param journal Journal
 * @return true = need_erase 置位
 *         false = 无待偿擦除
 */
bool jrn_has_erase_pending(const l2p_jrn_t *journal);

/**
 * @brief CP 水位标志是否置位（≥85%）。
 *
 * @param journal Journal
 * @return true = checkpoint_needed 置位
 *         false = 未置位
 */
bool jrn_needs_checkpoint(const l2p_jrn_t *journal);

/**
 * @brief 紧急水位标志是否置位（≥95%）。
 *
 * @param journal Journal
 * @return true = emergency 置位
 *         false = 未置位
 */
bool jrn_is_emergency(const l2p_jrn_t *journal);

/**
 * @brief CP Header 成功后设置合并边界（Checkpoint 模块调用）。
 *
 * @param journal  Journal
 * @param sequence 新的 merge_floor；sequence 小于它的旧条目不得被同 LBA 原地合并
 */
void jrn_merge_floor_set(l2p_jrn_t *journal, DWORD sequence);

/**
 * @brief 按当前写位置重算并置位 CP 水位标志。
 *
 * 由写确认路径在每次 L2P 更新后调用；水位标志是 journal 自己的状态，由本模块维护。
 *
 * @param journal Journal
 */
void jrn_watermark_update(l2p_jrn_t *journal);

/**
 * @brief 强制请求一次 Checkpoint（元数据失块后的恢复动作）
 *
 * 只置水位标志；由后台 CP 任务在安全点执行。不直接写介质。
 *
 * @param journal Journal
 */
void jrn_checkpoint_request(l2p_jrn_t *journal);

/**
 * @brief 全量回放**未进 L2P 表**的页（按 (page_seq, entry_index) 递增序应用，
 *        per-LBA last-write-wins）。
 *
 * @param journal     Journal；回放结束时由 jrn_resume 恢复写指针与序号
 * @param cache       共享 L2P cache（映射写入目标）
 * @param cp_sequence checkpoint 截断水位（收集页与块元数据增量都按它过滤）
 * @param out_entries 出参（可为 NULL）：实际应用成功的条目数（读失败的页不计入）
 * @param block_ctx   块元数据增量端口（可为 NULL = 不记 valid_count）
 * @return FTL_OK = 完整回放
 *         FTL_ERROR = 环内存在读失败页（写态已恢复，但回放可能
 *         不完整 ⇒ 调用者应回退 full metadata rebuild）。
 *
 * @note 需回放判据是**整页**：`page_seq + entry_count > cp_sequence`。只看页首页序号
 *       （`page_seq > cp_sequence`）会把跨水位的那一页整页跳过，丢掉它的新条目。
 * @note out_entries **不能**由 journal 的 RAM 写指针反推：末尾 jrn_resume 把落点放到
 *       最新页之后并清 current_entry ⇒ 用 current_entry/current_page 算出来恒为 0。
 */
ftl_status_e jrn_replay(l2p_jrn_t *journal, l2p_cache_t *cache, DWORD cp_sequence, DWORD *out_entries, jrn_replay_ctx_t *block_ctx);

/**
 * @brief 最近一次 jrn_replay 命中「降级 L2P region」的条目数（只读诊断 / 启动结果）。
 *
 * 非 0 表示该 region 未被 Journal 覆盖的旧映射已丢。该计数只作诊断；
 * 范围化恢复不据它回退全量重建（RISK-BOOT-18）。调用前须已有一次 jrn_replay。
 *
 * @return 命中降级 region 的条目数（0 = 未命中）
 */
DWORD jrn_replay_degraded_entries(void);

/**
 * @brief 截断 Journal 的**第一步**：水位校验 + 算出待预擦的块
 *
 * 语义：Checkpoint 已把 sequence ≤ up_to_sequence 的条目 flush 进 NAND L2P 表 ⇒
 * 环形里**最旧的一块**（= 环形下一块）整块可弃。预擦该块并记账（pre_erased_block），
 * 使 jrn_handle 的换块擦除回切到它时**跳过**擦除——保证同一块每圈只擦一次。
 *
 * 本函数只做**判定与算址**（纯 RAM，不动介质）：擦除由调用者提交（Checkpoint job
 * 提交异步擦命令），成功完成后再调 jrn_truncate_commit 记账。
 *
 * @param journal        Journal
 * @param up_to_sequence 截断水位；**必须覆盖全部已写条目**（调用者传 CP 时刻的
 *                       sequence_counter）。水位不足以覆盖（有未 checkpoint 的条目）
 *                       时**不截断**——最旧块可能仍持有回放需要的条目，擦掉即丢数据。
 * @param out_addr       出参：待预擦块的基地址（需返回 1 时有效）
 * @param out_block      出参：待预擦的环形块号（需返回 1 时有效；commit 要用）
 * @return jrn_truncate_step_e：JRN_TRUNC_NEEDS_ERASE = 需擦 *out_addr / *out_block；
 *         JRN_TRUNC_ALREADY = 无需擦（水位 OK，该块已预擦过）；
 *         JRN_TRUNC_NO_TRUNCATE = 不截断（水位不满足）——**调用者不得据此复位水位标志**
 */
jrn_truncate_step_e jrn_truncate_plan(l2p_jrn_t *journal, DWORD up_to_sequence, DWORD *out_addr, BYTE *out_block);

/**
 * @brief 截断 Journal 的**第二步**：记账 + 复位水位标志
 *
 * 在擦除命令**成功完成**后调用。记 pre_erased_block 使 jrn_handle 换块擦除回切时
 * 跳过擦除；复位 emergency / checkpoint_needed（设计 §7.5.4：本次 CP 已把
 * sequence ≤ cp_seq 的条目落盘并截断，水位清零。不复位会让 T1 永久命中——每圈必 CP）。
 *
 * ⚠ erased_block == JRN_BLOCK_NONE ⇒ **只复位水位标志，不动 pre_erased_block**
 *   ——"本次没有擦"（水位不满足 / 该块已被预擦过）时调用者用它收尾；若此时把
 *   pre_erased_block 覆写成 NONE，jrn_handle 换块擦除回切时会把已预擦的块再擦一次。
 *
 * @param journal   Journal
 * @param erased_block 刚擦成功的环形块号（jrn_truncate_plan 的回传值）；
 *                     JRN_BLOCK_NONE = 本次未擦
 */
void jrn_truncate_commit(l2p_jrn_t *journal, BYTE erased_block);

#endif /* FTL_JOURNAL_H */
