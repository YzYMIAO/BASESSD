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
 * 写模型与目录检查点 / BBT 一致（见 ftl/metadata.h §元数据写模型）：
 * 只追加到擦除过的页 + 每条 record 带单调 sequence + 池内轮转 + 最新有效胜。
 * ⚠ Header 绝不就地重写：NAND 只能 1→0，第二次编程会静默损坏旧 Header，
 *   使 cp_pool_find_latest 失败 ⇒ 开机误走 fresh ⇒ 映射全丢。
 */

#include "allocator.h"
#include "ftl_module.h" /* FTL_SUBMIT_TIMEOUT_MS */
#include "ftl_type.h"   /* ftl_ctx_s */
#include "journal.h"
#include "l2p_cache.h"
#include "log.h"
#include "metadata.h" /* CHECKPOINT_BLOCK0 / CHECKPOINT_POOL_BLOCKS */
#include "timer.h"

#include <stdint.h>

/* ── 异步 Checkpoint job ──
 *
 * CP 是严格有序的流水线（掉电安全契约，见 .c 头注释）；顺序由本状态机的 phase
 * 承载：每一步提交命令后即返回，完成由 g_op_callbacks[FTL_OP_CHECKPOINT]
 * （ftl_checkpoint_callback）按 phase 收尾，下一轮 cp_handle 续跑。
 *
 * ⚠ **掉电安全契约不变**：先 flush region → 再 flush 目录检查点 → 最后写 Header。
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

/* job 心跳节拍见 log.h / timer.h 的 LOG_RATE_INTERVAL_MS（各 feature 统一，不再各自定义）。 */

typedef enum CP_PHASE_E
{
    CP_IDLE = 0,       /* 无 job */
    CP_FLUSH_REGIONS,  /* ① 按快照 bitmap 逐个落盘脏 region（起 L2P 异步写回，等全部完成） */
    CP_DIR_CKPT_PREP,  /* ② 定位目录检查点游标 + 取槽（可能含一次性同步定位） */
    CP_DIR_CKPT_ERASE, /* ② 换块擦（仅需换块时） */
    CP_DIR_CKPT_WRITE, /* ② 逐页写目录检查点记录 */
    CP_HDR_PREP,       /* ③ 定位 Checkpoint 游标 + 取 Header 槽 */
    CP_HDR_ERASE,      /* ③ 换块擦（仅需换块时） */
    CP_BLOCK_WRITE,    /* ③ 逐页写 块元数据快照（**必须早于 Header**：Header 是整条 record 的 commit） */
    CP_WEAR_WRITE,     /* ③ 逐页写 磨损快照（erase/read/ECC health；同样早于 Header） */
    CP_HDR_WRITE,      /* ③ 写 Header */
    CP_TRUNC_PREP,     /* ④ 水位判定：算 journal 预擦块 */
    CP_TRUNC_ERASE,    /* ④ 预擦 journal 环形最旧块 */
    CP_DONE,           /* 收尾：释放 active，记日志 */
} cp_phase_e;

/* 触发原因（观测 / 策略分流；行为上统一走同一状态机） */
typedef enum CP_REASON_E
{
    CP_REASON_ADVANCE = 0, /* 只推进在飞 job，不评估启动（must-run 表首） */
    CP_REASON_WATERMARK,   /* Journal 水位 */
    CP_REASON_WRITES,      /* 写入量 */
    CP_REASON_TIMER,       /* 定时 */
    CP_REASON_EMERGENCY,   /* 紧急水位（只认 trigger->emergency） */
    CP_REASON_EXPLICIT,    /* force */
} cp_reason_e;

/* 开机格式化判据的结果 */
typedef enum CP_MEDIA_STATUS_E
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
 * ⚠ 与 journal / 目录检查点 / BBT 的 sequence **互不相干**：本池自己的单调序号。 */
typedef struct CP_STATE_S
{
    DWORD      sequence;              /* 本池自己的单调序号（最新有效 record） */
    DWORD      last_journal_sequence; /* 上一次成功 CP 的 journal 水位（T3/空转判据） */
    WORD       write_idx;             /* 池内下一个待写槽（环形） */
    BOOL       cursor_valid;          /* 游标是否已建立；未建立则首次写入先扫池定位 */
    BOOL       advance_next;          /* true = 本 session 首次 CP 先跳过一槽（避开掉电残留的半编程槽） */
    timer_ms_t next_t3_ms;            /* T3 下次到期时刻（绝对；由 CP 起点重装） */
    timer_ms_t last_cp_ms;            /* 上次 CP 启动时刻（host flush 节流基准） */
    DWORD      last_cp_writes;        /* 上次 CP 起点看到的 total_writes（T2 增量判据） */
} cp_state_s;

/*
 * cp_trigger_s — FTL 每轮汇总的触发信息（docs §4.6）
 *
 * 只传**每轮会变**的动态量；编译期常量 / 策略表由本模块自己持有。
 */
typedef struct CP_TRIGGER_S
{
    timer_ms_t  now_ms;
    DWORD       total_writes;
    DWORD       journal_seq;
    BYTE        journal_watermark; /* 0..100 */
    BOOL        emergency;         /* Journal ≥ 95% */
    BOOL        force;             /* 显式请求（开机/诊断） */
    cp_reason_e reason;            /* force 时优先用 */
} cp_trigger_s;

/* Header + 块元数据/磨损快照 job 的一段（③）。 */
typedef struct CP_JOB_HEADER_S
{
    md_ring_s ring;
    DWORD     addr;
    WORD      idx;
    DWORD     seq;
    BOOL      need_erase;
    BYTE      block_page;
    BYTE      wear_page;
    BOOL      block_ok;
    /* in/out 约定：cp_block_snapshot 写入；cp_block_table_load 读出并校验；
     * wear_page_* 与 block_words 共用同一次 CP record 顺序，调用者不得重排。 */
    WORD  block_words[FTL_BLOCK_TOTAL];
    DWORD wear_page_crc[FTL_CHECKPOINT_WEAR_PAGES];
    BYTE  wear_page_buf[PAGE_MAIN_SIZE];
    DWORD hdr_buf[PAGE_MAIN_SIZE / sizeof(DWORD)];
} cp_job_hdr_s;

/* Journal 截断 job 的一段（④）。 */
typedef struct CP_TRUNCATION_S
{
    DWORD addr;
    BYTE  next;
} cp_trunc_s;

typedef struct CP_HANDLE_S
{
    BOOL             active;          /* true = job 在跑（单飞行） */
    cp_phase_e       phase;           /* 异步 job 状态机 */
    BOOL             waiting;         /* true = 有一条本 job 的命令在飞 */
    BOOL             failed;          /* true = 前置步骤失败 ⇒ 中止 */
    cp_phase_e       fail_phase;      /* 中止发生的 phase（观测） */
    timer_deadline_s submit_deadline; /* 提交 deadline */
    BOOL             timed_out;
    cp_phase_e       timeout_phase;

    /* ① job 起点快照（必须先于任何 flush）。 */
    DWORD region_bitmap[L2P_LOCAL_DIRTY_WORDS];
    DWORD journal_seq;

    md_dir_ckpt_write_s dir;   /* ② 目录检查点（CP/metadata 共用上下文） */
    cp_job_hdr_s        hdr;   /* ③ Header + 块元数据/磨损快照 */
    cp_trunc_s          trunc; /* ④ Journal 截断 */

    /* 窄上下文：跨 job 保留。 */
    l2p_cache_s    *cache;
    l2p_jrn_s      *journal;
    md_dir_cache_s *metadata_dir;
    alc_s          *alloc;
    ftl_topo_s      topo;

    cp_state_s cp_state;

    /* 心跳与限流（跨 job 保留） */
    log_heartbeat_s hb;
    log_ratelimit_s rl_abort;
    log_ratelimit_s rl_done;
} cp_handle_s;

/* cp_handle 的位图是**本 die 的局部**尺寸；共享 cache 的脏位图是全局尺寸，二者
 * 不再同尺寸（阶段 B）。这里钉死本地位图 = L2P_REGIONS_PER_INSTANCE bit。 */
_Static_assert(sizeof(((cp_handle_s *)0)->region_bitmap) == (L2P_LOCAL_DIRTY_WORDS * sizeof(DWORD)), "cp_handle_s.region_bitmap 必须是本 die 的局部 2048-bit 位图");
_Static_assert(sizeof(((cp_handle_s *)0)->hdr.block_words) == (FTL_BLOCK_TOTAL * sizeof(WORD)), "cp_handle_s.block_words 必须恰好覆盖全部块");

/* ── Header Magic ── */
/* Checkpoint 池位置与块数（CHECKPOINT_BLOCK0 / CHECKPOINT_POOL_BLOCKS）见
 * ftl/metadata.h「固定元数据区布局」。 */
#define CP_MAGIC (0xCCCCCCCCUL)

/* ── Header 格式：**恰好一页主区**（2048 B）──
 * 字段 94 字 = 14 个标量 + region_bitmap[64] + wear_page_crc[16]；padding 补齐到 2048。
 * ⚠ padding 长度必须按实际字算：算错会让 sizeof > 一页 ⇒ 写入被静默截断。
 * 本页**无独立 CRC**：完整性由下层 FC 的 per-CW CRC + ECC 保证
 * （DRAM-less 主流做法，见 docs/design/ftl_revision_history.md）。*/
typedef struct CP_HEADER_S
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
} cp_header_s;

_Static_assert(sizeof(cp_header_s) == PAGE_MAIN_SIZE, "Checkpoint Header 必须恰好一页主区");
_Static_assert(sizeof(((cp_header_s *)0)->region_bitmap) == (L2P_LOCAL_DIRTY_WORDS * sizeof(DWORD)), "Header 位图必须是本 die 的局部 2048-bit 位图");

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

_Static_assert((unsigned)CP_BLOCK_WORD_MAX_VALID <= (unsigned)CP_BLOCK_WORD_VALID_MASK, "valid_count 超出 word 位宽");
_Static_assert((unsigned)FTL_BLOCK_STATE_BAD <= (unsigned)CP_BLOCK_WORD_STATE_MASK, "state 超出 word 位宽");

/**
 * @brief 把块状态字段打包成一个块元数据快照 word
 *
 * @param valid  有效页数（valid_count）
 * @param state  块状态（FTL_BLOCK_STATE_*）
 * @param temp   温度码（0/1）
 * @param pool   池归属（CP_BLOCK_POOL_*）
 * @return 打包后的 word
 */
static inline WORD cp_block_word_pack(WORD valid, BYTE state, BYTE temp, BYTE pool)
{
    return ((valid & CP_BLOCK_WORD_VALID_MASK) | ((state & CP_BLOCK_WORD_STATE_MASK) << CP_BLOCK_WORD_STATE_SHIFT) | ((temp & 0x1U) << CP_BLOCK_WORD_TEMP_SHIFT) | ((pool & CP_BLOCK_WORD_POOL_MASK) << CP_BLOCK_WORD_POOL_SHIFT));
}

/**
 * @brief 取出快照 word 里的有效页数
 *
 * @param word  块元数据快照 word
 * @return 有效页数（valid_count）
 */
static inline WORD cp_block_word_valid(WORD word)
{
    return (word & CP_BLOCK_WORD_VALID_MASK);
}

/**
 * @brief 取出快照 word 里的块状态
 *
 * @param word  块元数据快照 word
 * @return 块状态（FTL_BLOCK_STATE_*）
 */
static inline BYTE cp_block_word_state(WORD word)
{
    return ((word >> CP_BLOCK_WORD_STATE_SHIFT) & CP_BLOCK_WORD_STATE_MASK);
}

/**
 * @brief 取出快照 word 里的温度码
 *
 * @param word  块元数据快照 word
 * @return 温度码（0/1）
 */
static inline BYTE cp_block_word_temp(WORD word)
{
    return ((word >> CP_BLOCK_WORD_TEMP_SHIFT) & 0x1U);
}

/**
 * @brief 取出快照 word 里的池归属
 *
 * @param word  块元数据快照 word
 * @return 池归属（CP_BLOCK_POOL_*）
 */
static inline BYTE cp_block_word_pool(WORD word)
{
    return ((word >> CP_BLOCK_WORD_POOL_SHIFT) & CP_BLOCK_WORD_POOL_MASK);
}

/* ── 接口 ── */

/* ── 初始化 / 恢复 ── */

/**
 * @brief 复位 checkpoint handle 并绑定窄上下文
 *
 * @param h            checkpoint handle（由 storage 提供）
 * @param cache        L2P cache
 * @param journal      Journal
 * @param metadata_dir Directory
 * @param alloc        allocator（ch/ce/lun 同时作为本实例拓扑来源）
 */
void cp_init(cp_handle_s *h, l2p_cache_s *cache, l2p_jrn_s *journal, md_dir_cache_s *metadata_dir, alc_s *alloc);

/**
 * @brief 扫描 Checkpoint 池找最新有效 Header
 *
 * 池内轮转扫描（md_ring_locate）：magic 正确且 sequence 最大者胜。
 * 命中时同时恢复本实例的 sequence 计数与写游标（后续 Checkpoint job 从下一槽续写）。
 *
 * @param h              Checkpoint handle（池路由取 h->topo；游标/序号写回 h->cp_state）
 * @param cp             输出：找到的 Header
 * @param out_record_idx 输出：该 Header 在池内的槽位（可为 NULL；块元数据快照 加载要用）
 * @return FTL_OK 找到；FTL_ERROR 无有效 Checkpoint / 重读失败 / 校验不符
 */
ftl_status_e cp_pool_find_latest(cp_handle_s *h, cp_header_s *cp, WORD *out_record_idx);

/**
 * @brief 开机格式化判据：区分“空片 / 版本不符 / 读失败”
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
cp_media_status_e cp_media_status_probe(cp_handle_s *h, DWORD *out_layout_version);

/**
 * @brief 读取并校验一条 Checkpoint 记录的 块元数据快照
 *
 * 校验 header 的 bit_* 字段、逐页读入 words、再用 boot_crc32 验证。
 * 失败（含 block_page_count == 0 的旧记录 / v12 前空 initial CP）只表示
 * 「没有可用 块元数据快照」，**不得**据此触发格式化；块元数据改走 full metadata rebuild。
 *
 * @param h           Checkpoint handle
 * @param cp          最近 Header（cp_pool_find_latest 的输出）
 * @param record_idx  该 Header 的池内槽位
 * @param block_words 输出：FTL_BLOCK_TOTAL 个 16-bit word
 * @return true = 已加载且 CRC 通过
 *         false = 无快照 / 损坏
 */
BOOL cp_block_table_load(cp_handle_s *h, const cp_header_s *cp, WORD record_idx, WORD block_words[FTL_BLOCK_TOTAL]);

/**
 * @brief 读取并校验 Checkpoint 记录里的磨损快照
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
 * @return true = 已加载并回填
 *         false = 无快照 / 损坏
 */
BOOL cp_wear_table_load(cp_handle_s *h, const cp_header_s *cp, WORD record_idx);

/**
 * @brief 块元数据快照缓冲（full metadata rebuild / 恢复的端口；FTL_BLOCK_TOTAL 个 word）。
 * 返回的是 handle 内部常驻缓冲，调用者只读/按恢复流程填充。
 *
 * @param h  Checkpoint handle
 * @return 指向 handle 内 FTL_BLOCK_TOTAL 个 word 的常驻缓冲（非空，所有权归 handle）
 */
static inline WORD *cp_block_words_get(cp_handle_s *h)
{
    return h->hdr.block_words;
}

/* ── 运行期 Checkpoint ── */

/**
 * @brief 推进 Checkpoint（typed handle；提交即返回）
 *
 * 未在跑时按 trigger 判定是否启动：emergency / force / T1 水位 / T2 写入量 / T3 定时
 * （T3 带最小 journal 增量门，见 checkpoint.c）。已在跑则续跑下一步（任何档位都要调）。
 *
 * @param h       handle；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param trigger 本轮触发信息（**非空**）
 */
void cp_handle(cp_handle_s *h, const cp_trigger_s *trigger);

/**
 * @brief 复位 T3 到期时刻（boot 恢复 / 格式化后调用）。
 *
 * @param h  Checkpoint handle（重装 cp_state.next_t3_ms）
 */
void cp_timer_reset(cp_handle_s *h);

/* ── 格式化 / 快照辅助 ── */

/**
 * @brief 全量格式化后写一条完整的 initial Checkpoint
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
ftl_status_e cp_initial_record_write(ftl_ctx_s *ftl);

/**
 * @brief 把当前 BST + OP/Spare 池归属编码成块元数据快照（Checkpoint 起点调用）。
 * md_dir 是动态 LOG 块身份的权威：live 元数据块即使 BST state 不是 LOG，
 * 也按 LOG 编码（否则下次开机快照校验整份失败）。
 * md_dir 允许为 NULL（只按 BST/池编码；格式化后的 initial CP 用）。
 *
 * @param alloc   allocator（BST 与 OP/Spare 池归属的来源）
 * @param md_dir  Metadata Directory；动态 LOG 块身份的权威（可为 NULL）
 * @param words   输出：FTL_BLOCK_TOTAL 个编码后的 word
 */
void cp_block_snapshot(const alc_s *alloc, const md_dir_cache_s *md_dir, WORD words[FTL_BLOCK_TOTAL]);

/**
 * @brief 把第 page_index 页磨损快照从 BST 组进 page（256 条 × 8 B）。
 * 条目格式是 checkpoint.c 的内部布局；调用者只提供输出页。
 *
 * @param alloc       allocator（BST 磨损计数的来源）
 * @param page_index  磨损快照页号（0 .. FTL_CHECKPOINT_WEAR_PAGES-1）
 * @param page        输出：一页主区（PAGE_MAIN_SIZE）
 */
void cp_wear_page_build(const alc_s *alloc, WORD page_index, BYTE page[PAGE_MAIN_SIZE]);

#endif /* FTL_CHECKPOINT_H */
