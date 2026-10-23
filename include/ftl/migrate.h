#ifndef FTL_MIGRATE_H
#define FTL_MIGRATE_H

/*
 * migrate — Super Block 搬移引擎（GC / 静态 WL / 慢死预测**共用一台**异步 job）
 *
 * 把「读 victim 的有效 CW → 写进冷块 → 更新 L2P/Journal → 擦除归还」实现为
 * **异步 job 状态机**。GC / WL / 慢死预测的搬移主体逐字同构，差别只在
 *   · 谁当 victim（gc_select_victim / wl_select_victim / rr_predictive_scan）；
 *   · 擦完怎么处置（GC / WL：归还 Free Pool；慢死预测：退役 RETIRED）。
 * 故共用**一个** mig_handle_t：`kind` 选 victim，`post` 决定收尾。
 *
 * ── 为什么必须异步 ──
 *
 * 单次搬移 = 最多 64 页 × 2 plane 的读 + 写 + 2 次擦，同步执行可阻塞数十毫秒；
 * 而它有两个**在完成回调里**的入口（写路径紧急 GC，见 wb_handle(FLUSH)），
 * 于是 host 写完成的处理会长时间占住完成泵。job 化后每轮只推进**一步**
 * （一条 NAND 命令），host I/O 与后台搬移在 superloop 上真正重叠。
 *
 * ── 状态机 ──
 *
 *   SELECT（在 mig_begin_* 里做，不是 job 阶段）→ READ → READ_SETTLE → WRITE → WRITE_SETTLE
 *   → FLUSH_JRN → ERASE → DONE
 *
 *   · READ  逐页逐 plane：异步读 → 完成时解 META 得 cw_lba、记 rr_record_ecc / read_count
 *           （慢死预测闭环）。读失败 / 全页空洞 ⇒ 该 plane 无有效 CW。
 *   · READ_SETTLE 纯计算：逐 CW 反查 L2P 得 cw_bitmap。**先就绪前置**——
 *           region 未驻留会起异步加载并**原地等待**，全部就绪才动 bitmap；否则半途而废
 *           会让 bitmap 一半有一半没有（重试时又从头 `|=`，语义静默错乱）。
 *   · WRITE 本页各 plane 都读完且有有效 CW ⇒ alc_super_open_alloc_pair 拿目标页，逐 plane
 *           提交 CW 子集写。**任一写完成错误 ⇒ 中止 job（victim 不擦）**，绝不继续擦。
 *   · WRITE_SETTLE 纯计算：逐 CW **重查 L2P**（host 写/TRIM 可能已抢先），仍指向
 *           源 CW 才 jrn_append → l2p_write → 有效计数迁移。
 *           同样**先就绪前置**再动账——半途而废会重复记账。
 *   · FLUSH_JRN 等 Journal job 空闲，并检查 `write_fail_pending`
 *           （**F10 契约**：搬移条目先落盘再擦 victim；页写失败 ⇒ 条目已丢，
 *           pending 会阻止擦除，直到 Checkpoint 把 L2P 落盘）。
 *   · ERASE 逐 plane 异步擦：成功走 `alc_bst_note_erase`（erase_count / last_pe_ts /
 *           清 valid），然后按 `post` 二选一——`MIG_POST_FREE` 归还 Free Pool（入池，
 *           GC 与静态 WL 用），`MIG_POST_RETIRE` **不入池** + `rr_retire_block` 退役
 *           （慢死预测用，见 allocator.h 不变量②）；
 *           失败（且尚未 BAD）`bbt_mark_bad`，并置 `erase_failed`：该 SB 不计入
 *           `gc_reclaimed_count`、不打印 predict 退役日志。
 *   · DONE  alc_super_closed_find + alc_super_closed_remove + 进展计数
 *           （gc_reclaimed_count）/ 预测命中日志。
 *
 * ── 中止语义（数据安全约束）──
 *
 * 1. **分配不到目标页 ⇒ 中止且不擦 victim**（源数据仍在、L2P 未动、条目留在
 *    closed_sb_list），下轮 GC 再来。**不得**先擦后搬——那会静默丢掉尚未搬移的有效页。
 * 2. **擦除失败时不退役**：擦不掉的块归 bbt_mark_bad 管；同时标成 "RETIRED、仍可读"
 *    是自相矛盾的。擦除失败本身是极端分支（NAND 拒擦）。
 * 3. **提交侧 deadline**：连续超过 `FTL_SUBMIT_TIMEOUT_MS` 提交不出去 ⇒ 中止 job（同 1：
 *    不擦 victim，数据无损）。**永久错误**（建链 / 入参）立即中止，不空转重试。
 *
 * ── 单飞行 ──
 *
 * 每实例同时只允许一个 mig job（`active`）。GC / WL / predict 共用引擎：
 * 调度器任务表把 GC 求值排在 WL 之前，但两者各自受触发条件/节拍门控，**不存在**
 * 硬性的“WL 让 GC 先跑”；真正的互斥是 `active` 单飞行。
 *
 * ⚠ 本文件**不被** gc.h / wear_leveling.h include —— migrate.c 单向依赖它们的选块策略，
 *   反向包含会成环。ftl.h include 本文件以放置 mig_handle_t。
 */

#include "allocator.h"   /* super_block_t */
#include "fc_command.h"  /* fc_command_status_t（服务函数签名） */
#include "ftl_config.h"  /* DWORD, CW_NUM, PAGE_MAIN_SIZE */
#include "ftl_module.h"  /* bg_info_t / ftl_mod_status_t */
#include "ftl_type.h"    /* DWORD */
#include "journal.h"     /* l2p_jrn_t（端口） */
#include "l2p_cache.h"   /* l2p_cache_t（端口） */
#include "nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK */

#include <stdbool.h>
#include <stdint.h>

/* 前向声明（ftl_ctx_t 在 ftl.h 中定义；本文件被 ftl.h include，用 struct 标签打破环） */
struct ftl_context;
typedef struct ftl_context ftl_ctx_t;

/* ── 提交/等待 deadline（绝对 ms，docs §6.3）──
 * 提交侧（命令池被在飞的 host I/O 占满 ⇒ ftl_async_* 返 FTL_AGAIN）用
 * `FTL_SUBMIT_TIMEOUT_MS`；等外部模块（L2P region / Journal 刷页）用
 * `FTL_WAIT_TIMEOUT_MS`。命令一旦在飞，NAND 层的 wait/jump 超时保证它必有完成
 * （超时置 FC_CW_STATUS_TIMEOUT），故不在飞时强拆 job。
 * 超过 deadline ⇒ 中止 job（不擦 victim，数据无损，下轮重试）。
 * ⚠ 平台时钟缺失（now_ms == 0）时 deadline 不生效——退化为「无超时」；本板
 *   `ssd_platform_register` 强制 time_us 非空 ⇒ 该分支不可达。 */

/* ── 同一 victim 块连续读失败多少次后标坏（RISK-GC-01）──
 * 持续读失败（ECC 不可纠 / META 基址损坏）时 GC 会每轮重选同一 victim 并 abort，
 * 容量不再增长。3 次覆盖瞬态读错；超过即标 BAD 并从 Closed 条目摘除该 plane，
 * 让 GC 换下一个候选。源块的其他 plane 仍可迁移，数据不丢。 */
#define MIG_VICTIM_READ_FAIL_MAX (3U)

/* ── 系统性 victim 读失败熔断阈值 ──
 * 若连续这么多个**不同 victim 块**都读失败（每个都达到 MIG_VICTIM_READ_FAIL_MAX 被
 * 标 BAD），说明不是单块坏，而是 NAND 时序/读通路/控制器级问题。继续 GC 只会把
 * 每个 victim 都标 BAD，耗尽 Free/OP 并毁掉 BAD 图。
 * 达到阈值 ⇒ `mig_handle.paused = true`，停止 GC/WL/predict 直到重启；RAM 态。
 * 8 = 足够覆盖零星坏块，又能在死亡螺旋早期刹住。 */
#define MIG_SYSTEMIC_READ_FAIL_MAX (8U)

/* ── 目的页写失败（RISK-GC-02）──
 * 目的页写失败**不能**把整块标 BAD：该冷 SB 可能已有成功迁移的页，L2P 正指向它们。
 * 正确处置是**立即**关闭这个 Cold SB：失败页可能已半编程，不能 rewind；关闭后
 * 下一次 GC 从 Free Pool 取新的目的块，已写成功的页仍可读，残留页由 GC 正常回收。
 * 不能在同一 Open SB 上继续写下一页——那会把失败页留成 hole。 */

/* ── 状态机的 phase ──
 * 约定同 rr_stage_e：纯计算 phase 只从 mig_step 进入、跑完立刻转下一个（不提交命令）；
 * 提交 phase 由 mig_step 提交命令后**原地停留**，等完成服务函数改写。
 * 判「有没有命令在飞」用 waiting 而不是 phase：提交可能因背压失败（phase 不变但无命令）。 */
typedef enum
{
    MIG_IDLE = 0,           /* 空闲：无 job */
    MIG_READ,               /* 提交阶段：读 victim 当前页的当前 plane */
    MIG_READ_SETTLE,        /* 纯计算：反查 L2P 得 cw_bitmap（region 未就绪则原地等） */
    MIG_WRITE,              /* 提交阶段：写目标页的当前 plane */
    MIG_WRITE_SETTLE,       /* 纯计算 + jrn_reserve：改 L2P/Journal + 迁移有效计数 */
    MIG_WRITE_BATCH,        /* 提交阶段：一次提交 N 页 cache program 批次 */
    MIG_WRITE_SETTLE_BATCH, /* 纯计算：批内所有页两遍式确认（就绪 + Journal 预留后统一记账） */
    MIG_FLUSH_JRN,          /* 纯计算：等 Journal job 空闲（F10 屏障） */
    MIG_ERASE,              /* 提交阶段：擦 victim 的当前 plane */
    MIG_DONE                /* 纯计算：收尾（移除 closed 条目 + 计数 + 复位 job） */
} mig_phase_e;

/* cache program 批次最多 4 页（= NAND per-LUN FIFO 深度）。 */
#define MIG_BATCH_PAGES (4U)

/* 批内单页的搬运元数据（数据在 batch_buf 的同下标槽）。 */
typedef struct
{
    DWORD base[PLANE_NUM];
    DWORD cw_bitmap[PLANE_NUM];
    DWORD cw_lba[PLANE_NUM][CW_NUM];
    DWORD src[PLANE_NUM];
    DWORD dst[PLANE_NUM];
} mig_batch_page_t;

/* 收尾方式（决定擦除后怎么处置 victim 的块）。 */
typedef enum
{
    MIG_POST_FREE = 0, /* GC / 静态 WL：擦完归还 Free Pool，重新进入磨损轮转 */
    MIG_POST_RETIRE    /* 慢死预测 / read recovery：**不入池**，置 RETIRED
                        * （不再分配、不标坏、仍可读）。
                        * ⚠ 退役块不得进 Free Pool——选块判据是 Free Pool **数组**，
                        *    留在池里会被再次分配 */
} mig_post_e;

/* victim 来源（只用于日志与收尾差异）。 */
typedef enum
{
    MIG_KIND_GC = 0, /* 空间回收（gc_select_victim） */
    MIG_KIND_WL,     /* 静态磨损均衡（wl_select_victim） */
    MIG_KIND_PREDICT /* 慢死预测旁路（rr_predictive_scan） */
} mig_kind_e;

/* GC 会话的启动原因（只用于观测；不影响选块/回收决策）。 */
typedef enum
{
    MIG_GC_START_URGENT = 0, /* wbuf 分配不到页对 */
    MIG_GC_START_PERIODIC    /* T1/T4 周期评估（磨损均衡走独立 WL） */
} mig_gc_start_reason_e;

/* ── 搬移 job（per-instance，放 ftl->mig_handle）──
 *
 * ⚠ page_buf 是 **逐 plane** 的页镜像（`PLANE_NUM × PAGE_MAIN_SIZE`，当前共 4KB），
 *   常驻上下文而非调用栈：异步 job 的栈帧在提交命令时就已返回，数据必须活到完成
 *   回调。它是真实的 `.bss` 占用（见内存预算）。
 */
typedef struct mig_handle
{
    /* 窄上下文：只持有实际使用的 owner 指针，不持有 ftl_context / 端口结构体。 */
    alc_t       *alloc;
    l2p_jrn_t   *journal;
    l2p_cache_t *cache;
    const DWORD *total_writes;
    ftl_topo_t   topo;

    bool active;                /* true = job 在跑（单飞行门） */
    bool paused;                /* true = 系统性 victim 读失败熔断：暂停 GC/WL/predict 直到重启。
                                 * 防「每个 victim 都读失败 ⇒ 逐个标 BAD ⇒ free 池耗尽」的死亡螺旋。
                                 * RAM 态；重启后清，交给修复后的时序/介质重新评估。 */
    bool        waiting;        /* true = 有一条搬移命令在飞（等 g_op_callbacks[FTL_OP_GC]/[FTL_OP_WL]） */
    bool        aborted;        /* true = 中止（不擦 victim、不移除 closed 条目、不计数） */
    bool        decode_ok;      /* true = 本 plane 的 READ 解 META 成功（决定 SETTLE 要不要反查 L2P） */
    bool        write_ok;       /* true = 本 plane 的 WRITE 成功（决定 SETTLE 要不要改 L2P/Journal） */
    bool        dummy_page0;    /* true = 当前在飞命令是 page0 补零写（不结算映射） */
    DWORD       dummy_pba;      /* page0 补零写的目标 PBA */
    bool        dst_allocated;  /* true = 本 job 已成功分配过目的页（dst[] 有效） */
    bool        erase_failed;   /* true = 任一 plane 擦除失败/处置失败（回收计数与 predict 日志不计） */
    bool        closed_removed; /* true = 逐 plane 摘除后 Closed 条目已被移除（RISK-GC-04） */
    mig_phase_e phase;          /* 异步 job 状态机 */
    mig_kind_e  kind;           /* victim 来源（GC / WL / predict） */
    const char *log_reason;     /* 观测：urgent/periodic/wl/pred（静态字符串） */
    DWORD       log_triggers;   /* 观测：GC 触发位并集 */
    BYTE        post;           /* mig_post_e */
    BYTE        page;           /* 当前页索引 0..PAGES_PER_BLOCK；== PAGES_PER_BLOCK ⇒ 读阶段结束 */
    BYTE        plane;          /* 当前 plane（读/写/擦各自迭代） */
    DWORD       stall_ms;       /* 本轮连续「提交不出去」的起点（0 = 未计时） */
    DWORD       wait_ms;        /* 等外部模块（L2P / Journal）的起点（0 = 未计时） */
    DWORD       cmd_wait_ms;    /* 在飞命令的完成 deadline 起点（0 = 未计时） */
    bool        timed_out;      /* true = 本次 job 因 deadline 中止 */
    mig_phase_e timeout_phase;  /* 超时发生的 phase */
    /* 读失败计数**按 plane 的源块**分别记（RISK-GC-01，跨 job 保留）。
     * ⚠ 判据是「该块整块迁移成功」才清零（mig_finish），**不是**「读到另一页没报错」：
     *   坏页通常在第 N 页，重试时第 0..N-1 页会先成功读一遍；按「任一页成功」清零
     *   会让阈值永不达到 ⇒ 每轮重选同一 victim、永久 abort、容量不再增长。
     *   两个 plane 的块号不同，故计数不能共用一个槽位（交替失败会互相冲掉）。 */
    WORD fail_gblock[PLANE_NUM]; /* 该 plane 的读失败计数对应的源块；UINT16_MAX = 无计数 */

    WORD job_valid_cw; /* 本 job 起点 victim 的有效 CW 数（mig_finish 日志用） */

    /* 系统性读失败熔断计数：**整块迁移成功（擦除成功）**才清零；连续 mark BAD
     * 达到 MIG_SYSTEMIC_READ_FAIL_MAX ⇒ paused=true。 */
    BYTE read_fail_streak;
    BYTE fail_count[PLANE_NUM]; /* 该 plane 源块的连续读失败次数 */

    super_block_t victim; /* victim 的**副本**（closed_sb_list 会被 swap-remove 挪动，不持指针） */

    DWORD cw_bitmap[PLANE_NUM];      /* 本页逐 CW 有效位（bit = plane*CW_NUM+cw） */
    DWORD base[PLANE_NUM];           /* 本页各 plane 的页基址（CW0 的扇区 LBA） */
    DWORD cw_lba[PLANE_NUM][CW_NUM]; /* 本页逐 CW 的扇区 LBA（未写过 = FTL_LBA_INVALID） */
    DWORD src[PLANE_NUM];            /* 本页各 plane 的源 PBA */
    DWORD dst[PLANE_NUM];            /* 本页各 plane 的目标 PBA（alc_super_open_alloc_pair 出参） */

    /* ⚠ 源块的全局块号（BST 索引）**不缓存**在 job 里：真值是 victim.plane_blocks[plane]，
     *   快路径（valid_total == 0 ⇒ 直接 ERASE）不经过 READ 阶段，缓存的字段在快路径下
     *   是 0 ⇒ 擦除收尾会改块 0 的元数据。见 migrate.c 的 mig_src_gblock。 */

    DWORD migrated; /* 本 job 累计搬移的有效 CW 数（>0 才需要 F10 屏障） */
    WORD  erased;   /* 本 job 已成功擦除的 plane 数（观测） */

    /* ── cache program 批次（GC/WL）──
     * 读仍逐页走 READ/READ_SETTLE；满足条件时把最多 4 页攒进 batch_buf，
     * 一次提交 op44×(N-1) + op49。批次未结算前不启动新 job。 */
    bool  batch_mode;         /* true = 正在收集/提交 batch */
    bool  batch_disabled;     /* true = 本 job 禁用 batch（页面两 plane 基址不一致等） */
    BYTE  batch_count;        /* 已收集页数（0..MIG_BATCH_PAGES） */
    BYTE  batch_outstanding;  /* 已提交、未完成的命令数 */
    bool  batch_failed;       /* 任一成员失败 */
    bool  batch_cleanup_done; /* 失败后已关闭 cold SB（只做一次） */
    DWORD batch_jobs;         /* 成功提交的批数（观测） */
    DWORD batch_pages;        /* 成功提交的页数（观测） */
    DWORD batch_aborts;       /* 批次失败次数（观测） */

    mig_batch_page_t batch[MIG_BATCH_PAGES];
    BYTE             batch_buf[MIG_BATCH_PAGES][PLANE_NUM][PAGE_MAIN_SIZE];

    /* 心跳基准（见 ftl_job.h）：在飞期间每满 1s 打一行 kind/phase/page/age */
    DWORD start_ms;
    DWORD log_ms;

    /* 最近一次搬移读的 FC 完成状态（诊断）：abort 日志带上它，区分 ECC 不可纠 /
     * NAND 失败 / META 不符。只写不参与决策。 */
    DWORD last_fc_status;

    /* ── handle 自持的求值节拍（docs §4.4：GC 10ms / WL 5s / predict 1s）── */
    DWORD gc_eval_last_ms;
    DWORD gc_log_last_ms;
    DWORD wl_eval_last_ms;
    DWORD wl_last_writes; /* 上次 WL 求值时的 *total_writes 快照：两次求值间有增长 ⇒ host 在写，WL 让路 */
    DWORD predict_last_ms;
    /* 静态 WL 的目标块方向（RISK-WL-08）：true ⇒ 目的页对取**擦除最多**的空闲块
     * （`alc_super_open_alloc_pair(..., ALC_SUPER_ALLOC_COLD_HIGH_WEAR)`），把冷数据搬到已磨损块。GC / predict 恒 false。 */
    bool dst_high_wear;

    /* ── GC 收益判据（RISK-GC-13/14）──
     * host_writing 用 2 s 窗口（GC_HOST_WRITE_WINDOW_MS）：USB 写约 4 KB/13 ms，
     * 10 ms 单次采样会漏；漏判会让 GC 在写流中切到 IDLE 反复停走。 */
    DWORD gc_writes_last;   /* 上次求值看到的 *total_writes */
    DWORD gc_write_last_ms; /* total_writes 最近一次变化的时刻（0 = 尚未见过写） */
    bool  host_writing;     /* 最近一次求值的 host 写流判定 */

    bool  gc_no_gain;        /* 最近一次空间 GC 结论：没有值得回收的 victim */
    BYTE  gc_nogain_mode;    /* 产生该结论的 gc_benefit_mode_e（BYTE 存枚举，省 RAM） */
    DWORD gc_nogain_ms;      /* 结论时刻（退避用） */
    DWORD gc_nogain_writes;  /* 结论时的 *total_writes 快照 */
    WORD  gc_nogain_free;    /* 结论时的 free_count 快照 */
    WORD  gc_best_valid_pct; /* 最近一次扫描的域内最低有效率（0..100，观测） */
    BYTE  gc_last_mode;      /* 最近一次实际使用的 gc_benefit_mode_e（观测） */

    /* ── GC 会话观测（per-instance；跨 job 保留，[GC  ] end 后清零）──
     * 一次会话 = 从首个成功启动的 GC job 起，到连续 GC_SESSION_IDLE_MS 无 GC
     * job 活动为止。用于把「持续 GC」聚合成 start/progress/end，而不是逐 job 刷屏。 */
    bool  gc_session_active;
    DWORD gc_session_start_ms;
    DWORD gc_session_last_ms; /* 最近一次 GC job 活动 */
    DWORD gc_session_log_ms;  /* 最近一次 [GC  ] progress */
    DWORD gc_session_jobs;
    DWORD gc_session_moved;
    DWORD gc_session_reclaimed;
    DWORD gc_session_noop;
    DWORD gc_session_aborted;

    /* ── 观测计数（单调，只增不减）── */
    DWORD   reclaimed_count; /* 成功回收的 SB 数（GC/WL） */
    DWORD   moved_count;     /* 累计真正搬移的 CW 数（所有 kind，诊断用） */
    DWORD   noop_count;      /* 累计成功回收的**空** victim 数（GC） */
    DWORD   novictim_count;  /* 触发但选不出 victim 的次数（含 no-gain） */
    DWORD   nogain_count;    /* 其中「有候选但收益不足」的次数 */
    bool    urgent_gc;       /* mig_trigger(GC_URGENT) 登记（被 handle 消费） */
    int32_t error;           /* 最近一次中止的归因 */

    BYTE page_buf[PLANE_NUM][PAGE_MAIN_SIZE]; /* 页镜像（读落此、写从此取） */
} mig_handle_t;

/* 所有 plane 的读失败计数均已清空（boot 契约审计；纯函数）。 */
static inline bool mig_is_fail_state_clean(const mig_handle_t *m)
{
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (m->fail_gblock[plane] != UINT16_MAX)
        {
            return false;
        }
    }
    return true;
}

/* ── 接口 ── */

/*
 * mig_init - 绑定窄上下文并复位 handle。
 *
 * @param m     handle（= ftl->mig_handle）；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param alloc / journal / cache / total_writes  窄上下文（init 绑定；拓扑取 alloc 的 ch/ce/lun）
 */
void mig_init(mig_handle_t *m, alc_t *alloc, l2p_jrn_t *journal, l2p_cache_t *cache, const DWORD *total_writes);

/* 显式触发原因（设计 §4.4：只登记紧急/显式原因，周期评估由 handle 自持） */
typedef enum
{
    MIGRATE_REASON_GC_URGENT = 0, /* 写路径分配不到页对（wbuf / host 写） */
} mig_reason_e;

/*
 * mig_trigger - 登记一个显式搬移请求（不立即启动；由下一次 mig_handle 消费）。
 *
 * @param m      handle；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param reason 触发原因
 */
void mig_trigger(mig_handle_t *m, mig_reason_e reason);

typedef struct
{
    ftl_mod_status_t base;

    mig_phase_e phase;    /* 观测；外部不要据此驱动 */
    mig_kind_e  kind;     /* GC / WL / PREDICT */
    WORD        migrated; /* 本次 job 已搬 CW 数 */
    WORD        erased;   /* 本次 job 已擦 plane 数 */
    bool        aborted;
    bool        erase_failed;
    bool        timed_out;
    mig_phase_e timeout_phase;
    DWORD       reclaimed;  /* 累计成功回收 SB 数（单调） */
    DWORD       novictim;   /* 累计「触发但选不出 victim」次数（含 no-gain） */
    DWORD       nogain;     /* 其中「有候选但收益不足」的次数（单调） */
    BYTE        gc_mode;    /* 最近一次使用的 gc_benefit_mode_e（0=off/1=idle/2=emg） */
    bool        gc_no_gain; /* 最近一次空间 GC 结论 = 无收益 victim */
} mig_status_t;

/*
 * mig_handle - 推进搬移引擎（每轮 must-run；提交即返回）
 *
 * 语义：
 *   ① job 在飞 ⇒ 只推进/心跳（等完成服务函数）；
 *   ② 不在飞时，若 `load_level != BUSY`，按 handle 自持节拍评估新工作：
 *      紧急触发（mig_trigger）→ GC 10ms → WL 5s → predict 1s；
 *   ③ 每轮最多启动一个 job（单飞行）。
 *
 * @param m     handle；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param info  本轮动态信息（now_ms / load_level）
 * @param out   出参：本轮结束后的上报（**非空**）
 */
void mig_handle(mig_handle_t *m, const bg_info_t *info, mig_status_t *out);

/* 完成服务函数 ftl_migrate_callback 的声明在 ftl_op.h —— 那里是 g_op_callbacks 各槽的唯一权威列表。 */

#endif /* FTL_MIGRATE_H */
