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
 * **一条 record = 21 页**（Header 1 + 块元数据 4 + 磨损 16），块满换池内下一块并先擦；
 * 每条 record 在 P0/P1 镜像）。池首块/块数由 ftl/metadata.h 按几何推导
 * （「固定元数据区布局」）。
 *
 * 写模型与 Anchor / BBT 一致（见 ftl/metadata.h §元数据写模型）：
 * 只追加到擦除过的页 + 每条 record 带单调 sequence + 池内轮转 + 最新有效胜。
 * ⚠ Header 绝不就地重写：NAND 只能 1→0，第二次编程会静默损坏旧 Header，
 *   使 cp_pool_find_latest 失败 ⇒ 开机误走 fresh ⇒ 映射全丢。
 */

#include "allocator.h"
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h" /* CHECKPOINT_BLOCK0 / CHECKPOINT_POOL_BLOCKS */

#include <stdint.h>

/* ── 前向声明（ftl_ctx_t 在 ftl.h 中定义，ftl.h 会 include 本文件） ── */
struct ftl_context;

/* ── 异步 Checkpoint job ──
 *
 * CP 是严格有序的流水线（掉电安全契约，见 .c 头注释）；顺序由本状态机的 phase
 * 承载：每一步提交命令后即返回，完成由 g_op_callbacks[FTL_OP_CHECKPOINT]
 * （ftl_checkpoint_callback）按 phase 收尾，下一轮 cp_handle 续跑。
 *
 * ⚠ **掉电安全契约不变**：先 flush region → 再 flush Anchor → 最后写 Header。
 *   Header 是"承诺书"——它一旦落盘，boot 就信 region_bitmap 里置位的 region 在
 *   NAND 上，故任何被 Header 声称的落盘必须**先于** Header 完成。
 *
 * ⚠ **快照点在 job 起点**（见 checkpoint.c 的长注释）：region_bitmap 与
 *   journal_sequence 在 job **起点**一次取定。CP 跨多轮，期间完成的 host 写会在
 *   journal 里留下 > 快照水位的新条目——水位取早 ⇒ 它们一定被回放，不会丢。
 *
 * 完成服务函数 `ftl_checkpoint_callback` 声明在 ftl/ftl_op.h（与其余 op 服务函数同处）；
 * 它按 phase 记结果并推进，**不**调用 cp_handle（CP 阶段含同步原语，见 .c）。
 */

/* 提交 deadline：连续提交不出去超过 `FTL_SUBMIT_TIMEOUT_MS` 即中止本次 CP。
 * 只在「有脏行却起不动」时计时——等 FLUSHING 完成算有进展，清零。
 * 时钟缺失（now_ms == 0）时 deadline 不生效；本板 time_us 恒非空 ⇒ 不可达。 */

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
    CP_BLOCK_WRITE,   /* ③ 逐页写 块元数据快照（**必须早于 Header**：Header 是整条 record 的 commit） */
    CP_WEAR_WRITE,    /* ③ 逐页写 磨损快照（erase/read/ECC health；同样早于 Header） */
    CP_HDR_WRITE,     /* ③ 写 Header */
    CP_TRUNC_PREP,    /* ④ 水位判定：算 journal 预擦块 */
    CP_TRUNC_ERASE,   /* ④ 预擦 journal 环形最旧块 */
    CP_DONE,          /* 收尾：释放 active，记日志 */
} cp_phase_e;

/* 触发原因（观测 / 策略分流；行为上统一走同一状态机） */
typedef enum
{
    CP_REASON_ADVANCE = 0, /* 只推进在飞 job，不评估启动（must-run 表首） */
    CP_REASON_WATERMARK,   /* Journal 水位 */
    CP_REASON_WRITES,      /* 写入量 */
    CP_REASON_TIMER,       /* 定时 */
    CP_REASON_EMERGENCY,   /* 紧急水位（只认 trigger->emergency） */
    CP_REASON_EXPLICIT,    /* force */
} cp_reason_e;

/* 开机格式化判据的结果 */
typedef enum
{
    CP_MEDIA_CURRENT = 0,      /* 读到当前版本 CP */
    CP_MEDIA_NO_RECORD,        /* 空片：CP 池无内容 */
    CP_MEDIA_VERSION_MISMATCH, /* 读到 CP 头，但布局版本不是当前 */
    CP_MEDIA_IO_ERROR,         /* 有介质内容但 CP 读不通 / 头损坏 */
} cp_media_status_e;

/* ── Checkpoint 池游标/序号（per-instance，归 handle）──
 *
 * 开机由 cp_pool_find_latest 恢复。不能做成 file-scope static：多实例会共享同一份
 * 游标/序号，各自往同一个池槽写、sequence 互相踩。
 *
 * ⚠ 与 journal / Anchor / BBT 的 sequence **互不相干**：本池自己的单调序号。 */
typedef struct
{
    DWORD sequence;              /* 本池自己的单调序号（最新有效 record） */
    DWORD last_journal_sequence; /* 上一次成功 CP 的 journal 水位（T3/空转判据） */
    WORD  write_idx;             /* 池内下一个待写槽（环形） */
    bool  cursor_valid;          /* 游标是否已建立；未建立则首次写入先扫池定位 */
    bool  advance_next;          /* true = 本 session 首次 CP 先跳过一槽（避开掉电残留的半编程槽） */
} cp_state_t;

/*
 * cp_trigger_t — FTL 每轮汇总的触发信息（docs §4.6）
 *
 * 只传**每轮会变**的动态量；编译期常量 / 策略表由本模块自己持有。
 */
typedef struct
{
    DWORD       now_ms;
    DWORD       total_writes;
    DWORD       journal_seq;
    BYTE        journal_watermark; /* 0..100 */
    bool        emergency;         /* Journal ≥ 95% */
    bool        force;             /* 显式请求（开机/诊断） */
    cp_reason_e reason;            /* force 时优先用 */
} cp_trigger_t;

typedef struct
{
    ftl_mod_status_t base;

    cp_phase_e phase; /* 观测；外部不要据此驱动 */
    bool       active;
    DWORD      journal_seq;    /* 本次 job 起点的 journal 水位 */
    BYTE       anchor_pages;   /* 已写 Anchor 页数 */
    bool       header_written; /* Header（commit 页）已写 */
    bool       truncated;      /* 本次已做 journal 截断 */
    cp_phase_e fail_phase;     /* 中止发生的 phase */
    bool       timed_out;
    cp_phase_e timeout_phase;
} cp_status_t;

typedef struct
{
    bool       active;        /* true = job 在跑（单飞行：ftl_service 每轮推进它） */
    cp_phase_e phase;         /* 异步 job 状态机 */
    bool       waiting;       /* true = 有一条本 job 的命令在飞（等完成服务函数） */
    bool       failed;        /* true = 前置步骤失败 ⇒ 中止（不写 Header / 不截断） */
    cp_phase_e fail_phase;    /* 中止发生在哪个 phase（观测；cp_job_abort 记录） */
    DWORD      stall_ms;      /* 本轮连续「提交不出去」的起点（0 = 未计时） */
    bool       timed_out;     /* true = 本次 job 因 deadline 中止 */
    cp_phase_e timeout_phase; /* 超时发生的 phase */

    /* ① job 起点快照（**必须先于任何 flush**：flush 成功会清 region 脏位）
     * **本 die 的局部 2048-bit 位图**：由共享 cache 的全局脏位图按 owner 过滤、
     * global→local 映射得到（l2p_owner_bitmap_snapshot）。 */
    DWORD region_bitmap[L2P_LOCAL_DIRTY_WORDS];
    DWORD journal_seq; /* = journal.sequence_counter（截断/回放水位） */

    /* ② Anchor（一条 record = ANCHOR_PAGE_COUNT 页，逐页提交） */
    md_ring_t anchor_ring;
    DWORD     anchor_base;       /* record 第 0 页地址（md_anchor_prepare 回传） */
    WORD      anchor_idx;        /* 池内槽位（commit 要用） */
    DWORD     anchor_seq;        /* ++dir->anchor_sequence（prepare 时已自增） */
    BYTE      anchor_chunk;      /* 下一页要写的页序 */
    bool      anchor_ok;         /* true = 全部分页写成功（否则整条作废） */
    bool      anchor_need_erase; /* true = 写之前先擦 *anchor_base 所在块 */
    bool      anchor_skip;       /* true = Directory 无变更 ⇒ 整段跳过 */

    /* ③ Header + 块元数据快照（一条 record = 1 + FTL_CHECKPOINT_BLOCK_PAGES 页） */
    md_ring_t hdr_ring;
    DWORD     hdr_addr;
    WORD      hdr_idx;                                  /* 写这条 Header 用的池内槽位 */
    DWORD     hdr_seq;                                  /* ++h->cp_state.sequence（槽位确定后取） */
    bool      hdr_need_erase;                           /* true = 写之前先擦 *hdr_addr 所在块 */
    BYTE      block_page;                               /* 下一页要写的 块元数据页（0..FTL_CHECKPOINT_BLOCK_PAGES） */
    BYTE      wear_page;                                /* 下一页要写的 磨损页（0..FTL_CHECKPOINT_WEAR_PAGES） */
    bool      block_ok;                                 /* true = 全部快照页写成功（否则整条作废，不写 Header） */
    WORD      block_words[FTL_BLOCK_TOTAL];             /* 8KB：CP 起点快照 + boot 加载缓冲 */
    DWORD     wear_page_crc[FTL_CHECKPOINT_WEAR_PAGES]; /* 每页磨损快照的 CRC32（写进 Header）*/
    BYTE      wear_page_buf[PAGE_MAIN_SIZE];            /* 组一页磨损快照（2KB）*/
    /* Header 页（2KB）常驻 handle：CP_HDR_WRITE 每次都会组一页；放栈上会让
     * 运行期 Checkpoint 路径多背 2 KB 栈帧（DTCM 与栈共用）。类型与 cp_header_t
     * 对齐，使用处强转。 */
    DWORD hdr_buf[PAGE_MAIN_SIZE / sizeof(DWORD)];

    /* ④ Journal 截断 */
    DWORD trunc_addr; /* 待预擦块基地址（plan 回传） */
    BYTE  trunc_next; /* 待预擦的环形块号（plan 回传） */

    /* 窄上下文：只持有实际使用的 owner 指针（total_writes 端口原本无人读，已删）。
     * 跨 job 保留，不能随 memset 清。 */
    l2p_cache_t    *cache;
    l2p_jrn_t      *journal;
    md_dir_cache_t *metadata_dir;
    alc_t          *alloc;
    ftl_topo_t      topo;

    cp_state_t cp_state;
    DWORD      last_ms; /* 上次触发的 ms 时基（T3） */

    /* 心跳 */
    DWORD start_ms;     /* job 起点（观测用） */
    DWORD log_ms;       /* 上次心跳时刻 */
    DWORD log_abort_ms; /* 上次 abort 日志时刻（限流；跨 job 保留） */
    DWORD log_done_ms;  /* 上次 `done seq=` INFO 时刻（限流；跨 job 保留） */
} cp_handle_t;

/* cp_handle 的位图是**本 die 的局部**尺寸；共享 cache 的脏位图是全局尺寸，二者
 * 不再同尺寸（阶段 B）。这里钉死本地位图 = L2P_REGIONS_PER_INSTANCE bit。 */
_Static_assert(sizeof(((cp_handle_t *)0)->region_bitmap) == (L2P_LOCAL_DIRTY_WORDS * sizeof(DWORD)),
               "cp_handle_t.region_bitmap 必须是本 die 的局部 2048-bit 位图");
_Static_assert(sizeof(((cp_handle_t *)0)->block_words) == (FTL_BLOCK_TOTAL * sizeof(WORD)),
               "cp_handle_t.block_words 必须恰好覆盖全部块");

/* ── Header Magic ── */
/* Checkpoint 池位置与块数（CHECKPOINT_BLOCK0 / CHECKPOINT_POOL_BLOCKS）见
 * ftl/metadata.h「固定元数据区布局」。 */
#define CP_MAGIC (0xCCCCCCCCUL)

/* ── Header 格式：**恰好一页主区**（2048 B）──
 * 字段 94 字 = 14 个标量 + region_bitmap[64] + wear_page_crc[16]；padding 补齐到 2048。
 * ⚠ padding 长度必须按实际字算：算错会让 sizeof > 一页 ⇒ 写入被静默截断。
 * 本页**无独立 CRC**：完整性由下层 FC 的 per-CW CRC + ECC 保证
 * （DRAM-less 主流做法，见 docs/design/ftl_revision_history.md）。*/
typedef struct
{
    DWORD magic;                                    /* CP_MAGIC */
    DWORD sequence;                                 /* 本池自己的单调序号（最新有效）*/
    DWORD timestamp;                                /* timer_get_ms() 快照 */
    DWORD region_count;                             /* 非零 bitmap word 数（无消费方，仅信息量）*/
    DWORD region_bitmap[64];                        /* 2048 bit，哪些 region 在 checkpoint 中 */
    DWORD journal_sequence;                         /* 截断到此 sequence 之前的 journal */
    DWORD layout_version;                           /* FTL_MEDIA_LAYOUT_VERSION（版本不符 ⇒ 该记录作废）*/
    DWORD block_word_count;                         /* 块元数据 word 数（= FTL_BLOCK_TOTAL；0 = 无快照）*/
    DWORD block_word_bytes;                         /* 每 word 字节数（2；0 = 无快照）*/
    DWORD block_page_count;                         /* 块元数据页数（FTL_CHECKPOINT_BLOCK_PAGES；0 = 无快照）*/
    DWORD block_crc32;                              /* 块元数据 words 的 boot_crc32() */
    DWORD wear_entry_count;                         /* 磨损条目数（= FTL_BLOCK_TOTAL；0 = 无快照）*/
    DWORD wear_entry_bytes;                         /* 磨损条目字节（8；0 = 无快照）*/
    DWORD wear_page_count;                          /* 磨损页数（FTL_CHECKPOINT_WEAR_PAGES；0 = 无快照）*/
    DWORD wear_page_crc[FTL_CHECKPOINT_WEAR_PAGES]; /* 每页磨损快照的 CRC32 */
    DWORD record_pages;                             /* 1 + block_page_count + wear_page_count */
    BYTE  padding[2048 - 94 * 4];                   /* 补齐到一页主区（1672 B）*/
} cp_header_t;

_Static_assert(sizeof(cp_header_t) == PAGE_MAIN_SIZE, "Checkpoint Header 必须恰好一页主区");
_Static_assert(sizeof(((cp_header_t *)0)->region_bitmap) == (L2P_LOCAL_DIRTY_WORDS * sizeof(DWORD)),
               "Header 位图必须是本 die 的局部 2048-bit 位图");

/* ── 块元数据快照 word 编解码（与 power_failure_recovery 共用）──
 * 术语：公开资料称 per-block metadata / block descriptor；本仓统一叫「块元数据快照」。
 * word 位域：valid_count 10 / state 3 / temp 1 / pool 2（小端、显式移位，H27）。 */
#define CP_BLOCK_WORD_VALID_MASK  (0x03FFU)
#define CP_BLOCK_WORD_STATE_SHIFT (10U)
#define CP_BLOCK_WORD_STATE_MASK  (0x7U)
#define CP_BLOCK_WORD_TEMP_SHIFT  (13U)
#define CP_BLOCK_WORD_POOL_SHIFT  (14U)
#define CP_BLOCK_WORD_POOL_MASK   (0x3U)
#define CP_BLOCK_WORD_MAX_VALID   ((PAGES_PER_BLOCK * CW_NUM))
#define CP_BLOCK_POOL_NONE        (0U)
#define CP_BLOCK_POOL_OP          (1U)
#define CP_BLOCK_POOL_SPARE       (2U)

_Static_assert((unsigned)CP_BLOCK_WORD_MAX_VALID <= (unsigned)CP_BLOCK_WORD_VALID_MASK,
               "valid_count 超出 word 位宽");
_Static_assert((unsigned)FTL_BLOCK_STATE_BAD <= (unsigned)CP_BLOCK_WORD_STATE_MASK,
               "state 超出 word 位宽");

static inline WORD cp_block_word_pack(WORD valid, BYTE state, BYTE temp, BYTE pool)
{
    return ((valid & CP_BLOCK_WORD_VALID_MASK) |
            ((state & CP_BLOCK_WORD_STATE_MASK) << CP_BLOCK_WORD_STATE_SHIFT) |
            ((temp & 0x1U) << CP_BLOCK_WORD_TEMP_SHIFT) |
            ((pool & CP_BLOCK_WORD_POOL_MASK) << CP_BLOCK_WORD_POOL_SHIFT));
}

static inline WORD cp_block_word_valid(WORD word)
{
    return (word & CP_BLOCK_WORD_VALID_MASK);
}

static inline BYTE cp_block_word_state(WORD word)
{
    return ((word >> CP_BLOCK_WORD_STATE_SHIFT) & CP_BLOCK_WORD_STATE_MASK);
}

static inline BYTE cp_block_word_temp(WORD word)
{
    return ((word >> CP_BLOCK_WORD_TEMP_SHIFT) & 0x1U);
}

static inline BYTE cp_block_word_pool(WORD word)
{
    return ((word >> CP_BLOCK_WORD_POOL_SHIFT) & CP_BLOCK_WORD_POOL_MASK);
}

/* ── 接口 ── */

/* ── 初始化 / 恢复 ── */

/*
 * cp_init - 复位 checkpoint handle 并绑定窄上下文
 *
 * @param h            checkpoint handle（由 storage 提供）
 * @param cache        L2P cache
 * @param journal      Journal
 * @param metadata_dir Directory
 * @param alloc        allocator（ch/ce/lun 同时作为本实例拓扑来源）
 */
void cp_init(cp_handle_t    *h,
             l2p_cache_t    *cache,
             l2p_jrn_t      *journal,
             md_dir_cache_t *metadata_dir,
             alc_t          *alloc);

/*
 * cp_pool_find_latest - 扫描 Checkpoint 池找最新有效 Header
 *
 * 池内轮转扫描（md_ring_locate）：magic 正确且 sequence 最大者胜。
 * 命中时同时恢复本实例的 sequence 计数与写游标（后续 Checkpoint job 从下一槽续写）。
 *
 * @param h              Checkpoint handle（池路由取 h->topo；游标/序号写回 h->cp_state）
 * @param cp             输出：找到的 Header
 * @param out_record_idx 输出：该 Header 在池内的槽位（可为 NULL；块元数据快照 加载要用）
 * @return FTL_OK 找到；FTL_ERROR 无有效 Checkpoint / 重读失败 / 校验不符
 */
ftl_status_e cp_pool_find_latest(cp_handle_t *h, cp_header_t *cp, WORD *out_record_idx);

/*
 * cp_media_status_probe - 开机格式化判据：区分“空片 / 版本不符 / 读失败”
 *
 * `cp_pool_find_latest` 把“无记录 / 版本不符 / 读失败”都压成 FTL_ERROR；
 * 直接据此格式化会把一次瞬时读错误变成整片擦除。本函数只做只读探测：
 *   · 用 magic-only 探针定位任意版本的 CP 头 → 比 layout_version；
 *   · 找不到 record 时，raw 读 CP 池首页区分“空片”与“读不通/有内容但无有效 CP”；
 *   · 再以 `bbt_medium_has_ftl_data` 判断固定元数据区是否已写过（用过介质上
 *     CP 缺失/损坏 ⇒ IO_ERROR，不自动格式化）。
 *
 * @param h                   Checkpoint handle
 * @param out_layout_version  输出：读到的 CP layout_version（可 NULL）
 * @return CP_MEDIA_CURRENT / NO_RECORD / VERSION_MISMATCH / IO_ERROR
 */
cp_media_status_e cp_media_status_probe(cp_handle_t *h, DWORD *out_layout_version);

/*
 * cp_block_table_load - 读取并校验一条 Checkpoint 记录的 块元数据快照
 *
 * 校验 header 的 bit_* 字段、逐页读入 words、再用 boot_crc32 验证。
 * 失败（含 block_page_count == 0 的 initial CP）只表示「没有可用 块元数据快照」，
 * **不得**据此触发格式化；块元数据改走 full metadata rebuild。
 *
 * @param h           Checkpoint handle
 * @param cp          最近 Header（cp_pool_find_latest 的输出）
 * @param record_idx  该 Header 的池内槽位
 * @param block_words 输出：FTL_BLOCK_TOTAL 个 16-bit word
 * @return true = 已加载且 CRC 通过；false = 无快照 / 损坏
 */
bool cp_block_table_load(cp_handle_t       *h,
                         const cp_header_t *cp,
                         WORD               record_idx,
                         WORD               block_words[FTL_BLOCK_TOTAL]);

/*
 * cp_wear_table_load - 读取并校验 Checkpoint 记录里的磨损快照
 *
 * 逐页读入 16 页磨损快照、用 Header 里的 wear_page_crc[] 验证，然后回填 BST 的
 * erase_count / read_count / rr（last_ecc / rising_count / health）。
 * ⚠ 逐页应用：中途某页失败时，**已读页已回填**，其余块保持默认计数。
 * 失效（无快照 / CRC 错 / 读失败）只表示「磨损计数不完整」——**软状态**，
 * 不得触发格式化，也不影响用户数据。
 *
 * @param h           Checkpoint handle
 * @param cp          最近 Header
 * @param record_idx  该 Header 的池内槽位
 * @return true = 已加载并回填；false = 无快照 / 损坏
 */
bool cp_wear_table_load(cp_handle_t       *h,
                        const cp_header_t *cp,
                        WORD               record_idx);

/* cp_block_words_get - 块元数据快照缓冲（full metadata rebuild / 恢复的端口；FTL_BLOCK_TOTAL 个 word）。
 * 返回的是 handle 内部常驻缓冲，调用者只读/按恢复流程填充。 */
WORD *cp_block_words_get(cp_handle_t *h);

/* ── 运行期 Checkpoint ── */

/*
 * cp_handle - 推进 Checkpoint（typed handle；提交即返回）
 *
 * 未在跑时按 trigger 判定是否启动：emergency / force / T1 水位 / T2 写入量 / T3 定时
 * （T3 带最小 journal 增量门，见 checkpoint.c）。已在跑则续跑下一步（任何档位都要调）。
 *
 * @param h       handle；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param trigger 本轮触发信息（**非空**）
 * @param out    出参：本轮结束后的上报（**非空**）
 */
void cp_handle(cp_handle_t *h, const cp_trigger_t *trigger, cp_status_t *out);

/* cp_timer_reset - 复位 T3 定时基准（boot 恢复 / 格式化后调用）。 */
void cp_timer_reset(cp_handle_t *h);

/* ── 格式化 / 快照辅助 ── */

/*
 * cp_initial_record_write - 全量格式化后写一条完整的 initial Checkpoint
 *
 * 只由 pfr_media_format() 在擦完全片后调用。写出完整 21 页 record：
 * 4 页 per-block metadata snapshot + 16 页磨损快照 + Header（最后写 = commit）。
 * sequence=1、journal_sequence=0；下次开机 cp_pool_find_latest 立即命中，
 * 版本判据只格式化一次；即使随后发生 power failure，也能用这份快照 + journal
 * 增量恢复，不必 full metadata rebuild。
 *
 * @param ftl  FTL 实例（含 allocator / checkpoint handle / topo）
 * @return FTL_OK / FTL_ERROR（任一分页写失败 ⇒ 不写 Header，无有效 record）
 */
ftl_status_e cp_initial_record_write(struct ftl_context *ftl);

/* 把当前 BST + OP/Spare 池归属编码成块元数据快照（Checkpoint 起点调用）。
 * md_dir 是动态 LOG 块身份的权威：live 元数据块即使 BST state 不是 LOG，
 * 也按 LOG 编码（否则下次开机快照校验整份失败，RISK-BOOT-15）。
 * md_dir 允许为 NULL（只按 BST/池编码；格式化后的 initial CP 用）。 */
void cp_block_snapshot(const alc_t          *alloc,
                       const md_dir_cache_t *md_dir,
                       WORD                  words[FTL_BLOCK_TOTAL]);

/* 把第 page_index 页磨损快照从 BST 组进 page（256 条 × 8 B）。
 * 条目格式是 checkpoint.c 的内部布局；调用者只提供输出页。 */
void cp_wear_page_build(const alc_t *alloc, WORD page_index, BYTE page[PAGE_MAIN_SIZE]);

#endif /* FTL_CHECKPOINT_H */
