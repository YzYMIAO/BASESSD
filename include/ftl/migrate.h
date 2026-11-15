#ifndef FTL_MIGRATE_H
#define FTL_MIGRATE_H

/*
 * migrate — Super Block 搬移引擎（GC / 静态 WL / 预测性退役**共用一台**异步 job）
 *
 * 把「读 victim 的有效 CW → 写进冷块 → 更新 L2P/Journal → 擦除归还」实现为
 * **异步 job 状态机**。GC / WL / 预测性退役的搬移主体逐字同构，差别只在
 *   · 谁当 victim（gc_select_victim / wl_select_victim / rr_predictive_scan）；
 *   · 擦完怎么处置（GC / WL：归还 Free Pool；预测性退役：退役 RETIRED）。
 * 故共用**一个** mig_handle_s：`kind` 选 victim，`post` 决定收尾。
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
 *   SELECT（在 mig_gc_begin / mig_wl_begin / mig_predict_begin 里做，不是 job 阶段）
 *   → READ → READ_SETTLE → WRITE → WRITE_SETTLE
 *   → FLUSH_JRN → ERASE → DONE
 *
 *   · READ  逐页逐 plane：异步读 → 完成时解 META 得 cw_lba、记 rr_record_ecc / read_count
 *           （预测性退役闭环）。读失败 / 全页空洞 ⇒ 该 plane 无有效 CW。
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
 *           （预测性退役用，见 allocator.h 不变量②）；
 *           失败（且尚未 BAD）`bbt_mark_bad`，并置 `erase_failed`：该 SB 不计入
 *           `state.counters.reclaimed`、不打印 predict 退役日志。
 *   · DONE  alc_super_closed_find + alc_super_closed_remove + 进展计数
 *           （`state.counters.reclaimed`）/ 预测命中日志。
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
 * 每实例同时只允许一个 mig job（`active`）；上一个 job 放弃但命令仍在 FC 侧时
 * `state.inflight != 0`，三个 `*_begin` 拒绝启新 job（迟到完成会按新 phase 结算）。
 * GC / WL / predict 共用引擎：
 * 调度器任务表把 GC 求值排在 WL 之前，但两者各自受触发条件/节拍门控，**不存在**
 * 硬性的“WL 让 GC 先跑”；真正的互斥是 `active` 单飞行。
 *
 * ⚠ 本文件**不被** gc.h / wear_leveling.h include —— migrate.c 单向依赖它们的选块策略，
 *   反向包含会成环。ftl.h include 本文件以放置 mig_handle_s。
 */

#include "allocator.h"  /* super_block_s */
#include "fc_command.h" /* fc_cmd_status_s（服务函数签名） */
#include "ftl_config.h" /* DWORD, CW_NUM, PAGE_MAIN_SIZE */
#include "ftl_module.h" /* FTL_SUBMIT_TIMEOUT_MS / FTL_WAIT_TIMEOUT_MS */
#include "ftl_type.h"   /* DWORD */
#include "gc.h"         /* gc_benefit_mode_e */
#include "journal.h"    /* l2p_jrn_s（端口） */
#include "l2p_cache.h"  /* l2p_cache_s（端口） */
#include "log.h"
#include "nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK */
#include "timer.h"

#include <stdbool.h>
#include <stdint.h>

/* ── 提交/等待 deadline（绝对 ms，docs §6.3）──
 * 提交侧（命令池被在飞的 host I/O 占满 ⇒ ftl_async_* 返 FTL_AGAIN）用
 * `FTL_SUBMIT_TIMEOUT_MS`；等外部模块（L2P region / Journal 刷页）用
 * `FTL_WAIT_TIMEOUT_MS`。命令一旦在飞，NAND 层的 wait/jump 超时保证它必有完成
 * （超时置 FC_CMD_STATUS_TIMEOUT），故不在飞时强拆 job。
 * 超过 deadline ⇒ 中止 job（不擦 victim，数据无损，下轮重试）。
 * ⚠ 平台时钟缺失（now_ms == 0）时 deadline 不生效——退化为「无超时」；本板
 *   `ssd_platform_register` 强制 time_us 非空 ⇒ 该分支不可达。 */

/* ── 同一 victim 块连续读失败多少次后标坏──
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

/* cache program 批次最多 4 页（= NAND per-LUN FIFO 深度）。 */
#define MIG_BATCH_PAGES (4U)

/* ── 目的页写失败──
 * 目的页写失败**不能**把整块标 BAD：该冷 SB 可能已有成功迁移的页，L2P 正指向它们。
 * 正确处置是**立即**关闭这个 Cold SB：失败页可能已半编程，不能 rewind；关闭后
 * 下一次 GC 从 Free Pool 取新的目的块，已写成功的页仍可读，残留页由 GC 正常回收。
 * 不能在同一 Open SB 上继续写下一页——那会把失败页留成 hole。 */

/* ── 状态机的 phase ──
 * 约定同 rr_stage_e：纯计算 phase 只从 mig_job_step 进入、跑完立刻转下一个（不提交命令）；
 * 提交 phase 由 mig_job_step 提交命令后**原地停留**，等完成服务函数改写。
 * 判「有没有命令在飞」用 waiting 而不是 phase：提交可能因背压失败（phase 不变但无命令）。 */
typedef enum MIGRATE_PHASE_E
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

/* 收尾方式（决定擦除后怎么处置 victim 的块）。 */
typedef enum MIGRATE_POST_E
{
    MIG_POST_FREE = 0, /* GC / 静态 WL：擦完归还 Free Pool，重新进入磨损轮转 */
    MIG_POST_RETIRE    /* 预测性退役 / read recovery：**不入池**，置 RETIRED
                        * （不再分配、不标坏、仍可读）。
                        * ⚠ 退役块不得进 Free Pool——选块判据是 Free Pool **数组**，
                        *    留在池里会被再次分配 */
} mig_post_e;

/* victim 来源（只用于日志与收尾差异）。 */
typedef enum MIGRATE_KIND_E
{
    MIG_KIND_GC = 0, /* 空间回收（gc_select_victim） */
    MIG_KIND_WL,     /* 静态磨损均衡（wl_select_victim） */
    MIG_KIND_PREDICT /* 预测性退役旁路（rr_predictive_scan） */
} mig_kind_e;

/* GC 会话的启动原因（只用于观测；不影响选块/回收决策）。 */
typedef enum MIGRATE_GC_START_REASON_E
{
    MIG_GC_START_URGENT = 0, /* wbuf 分配不到页对 */
    MIG_GC_START_PERIODIC    /* T1/T4 周期评估（磨损均衡走独立 WL） */
} mig_gc_start_reason_e;

/* 批内单页的搬运元数据（数据在 batch_buf 的同下标槽）。 */
typedef struct MIGRATE_BATCH_PAGE_S
{
    DWORD base[PLANE_NUM];
    DWORD cw_bitmap[PLANE_NUM];
    DWORD cw_lba[PLANE_NUM][CW_NUM];
    DWORD src[PLANE_NUM];
    DWORD dst[PLANE_NUM];
} mig_batch_page_s;

/* ── 搬移 job（per-instance，放 ftl->mig_handle）──
 *
 * ⚠ page_buf 是 **逐 plane** 的页镜像（`PLANE_NUM × PAGE_MAIN_SIZE`，当前共 4KB），
 *   常驻上下文而非调用栈：异步 job 的栈帧在提交命令时就已返回，数据必须活到完成
 *   回调。它是真实的 `.bss` 占用（见内存预算）。
 */
/* 单个 job 的 transient 状态：job 起点整体 memset，不碰 state。 */
typedef struct MIGRATE_JOB_S
{
    BOOL             active;    /* true = job 在跑（单飞行门） */
    BOOL             waiting;   /* true = 有一条搬移命令在飞 */
    BOOL             aborted;   /* true = 中止（不擦 victim、不移除 closed 条目、不计数） */
    BOOL             decode_ok; /* 本 plane READ 解 META 成功（决定 SETTLE 是否反查 L2P） */
    BOOL             write_ok;  /* 本 plane WRITE 成功（决定 SETTLE 是否改 L2P/Journal） */
    BOOL             dummy_page0;
    DWORD            dummy_pba;
    BOOL             dst_allocated;
    BOOL             erase_failed;
    BOOL             closed_removed;
    mig_phase_e      phase;
    mig_kind_e       kind;
    const char      *log_reason;
    DWORD            log_triggers;
    mig_post_e       post;
    BYTE             page;
    BYTE             plane;
    timer_deadline_s submit_deadline;
    timer_deadline_s external_deadline;
    timer_deadline_s command_deadline;
    BOOL             timed_out;
    mig_phase_e      timeout_phase;
    WORD             job_valid_cw;
    super_block_s    victim;
    DWORD            cw_bitmap[PLANE_NUM];
    DWORD            base[PLANE_NUM];
    DWORD            cw_lba[PLANE_NUM][CW_NUM];
    DWORD            src[PLANE_NUM];
    DWORD            dst[PLANE_NUM];
    DWORD            migrated;
    WORD             erased;
    BOOL             batch_mode;
    BYTE             batch_count;
    BYTE             batch_outstanding;
    BOOL             batch_failed;
    BOOL             batch_cleanup_done;
    mig_batch_page_s batch[MIG_BATCH_PAGES];
    log_heartbeat_s  hb;
    DWORD            last_fc_status;
    BOOL             dst_high_wear;
} mig_job_s;

/* 跨 job 保留的持久状态：job 起点不清。 */
typedef struct MIGRATE_GC_BENEFIT_S
{
    BOOL              nogain;
    gc_benefit_mode_e nogain_mode;
    timer_ms_t        nogain_ms;
    DWORD             nogain_writes;
    WORD              nogain_free;
    WORD              best_valid_pct;
    gc_benefit_mode_e last_mode;
    DWORD             writes_last;   /* 上次求值看到的 *total_writes */
    timer_ms_t        write_last_ms; /* total_writes 最近一次变化的时刻 */
    BOOL              host_writing;  /* 最近一次 host 写流判定 */
} mig_gc_benefit_s;

typedef struct MIGRATE_GC_SESSION_S
{
    BOOL            active;
    timer_ms_t      start_ms;
    timer_ms_t      last_ms;
    DWORD           jobs;
    DWORD           moved;
    DWORD           reclaimed;
    DWORD           noop;
    DWORD           aborted;
    log_ratelimit_s rl;
} mig_gc_session_s;

typedef struct MIGRATE_FAILURE_S
{
    BOOL paused;
    WORD fail_gblock[PLANE_NUM];
    BYTE read_fail_streak;
    BYTE fail_count[PLANE_NUM];
} mig_failure_s;

typedef struct MIGRATE_COUNTERS_S
{
    DWORD reclaimed;
    DWORD moved;
    DWORD noop;
    DWORD novictim;
    DWORD nogain;

    /* cache-program 批次诊断（跨 job 累计；放 job 里会被 job 起点 memset 清零）。 */
    DWORD batch_jobs;   /* 已提交的批次数 */
    DWORD batch_pages;  /* 批次内总页数 */
    DWORD batch_aborts; /* 批次失败/中止数 */
} mig_counters_s;

typedef struct MIGRATE_STATE_S
{
    mig_failure_s failure;

    /* 已提交、尚未收到完成的命令数（跨 job 保留）。>0 时禁止启新 job：
     * job 超时放弃后命令仍在 FC 侧，迟到完成会按新 job 的 phase 结算
     * （erase 阶段会把未擦块当已擦回收）⇒ 必须等它回来再复用引擎。 */
    WORD inflight;

    BOOL            batch_disabled;
    bg_level_e      level;
    BOOL            timers_registered;
    log_ratelimit_s rl_eval;
    DWORD           wl_last_writes;

    mig_gc_benefit_s benefit;
    mig_gc_session_s session;
    mig_counters_s   counters;

    BOOL    urgent_gc;
    int32_t error;
} mig_state_s;

typedef struct MIGRATE_HANDLE_S
{
    /* 窄上下文：只持有实际使用的 owner 指针。 */
    alc_s       *alloc;
    l2p_jrn_s   *journal;
    l2p_cache_s *cache;
    const DWORD *total_writes;
    ftl_topo_s   topo;

    mig_job_s   job;
    mig_state_s state;

    /* 常驻缓冲：job 间复用，不清零。 */
    BYTE batch_buf[MIG_BATCH_PAGES][PLANE_NUM][PAGE_MAIN_SIZE];
    BYTE page_buf[PLANE_NUM][PAGE_MAIN_SIZE];
} mig_handle_s;

/**
 * @brief 所有 plane 的读失败计数均已清空（boot 契约审计；纯函数）。
 *
 * @param m handle（只读）。
 *
 * @return true  —— 每个 plane 的 fail_gblock[] 都是 UINT16_MAX；
 *         false —— 至少一个 plane 仍记着源块。
 */
static inline BOOL mig_fail_state_is_clean(const mig_handle_s *m)
{
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if (m->state.failure.fail_gblock[plane] != UINT16_MAX)
        {
            return false;
        }
    }
    return true;
}

/* ── 接口 ── */

/**
 * @brief 绑定窄上下文并复位 handle。
 *
 * @param m            handle（= ftl->mig_handle）；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param alloc        allocator（拓扑也从这里取）
 * @param journal      Journal 端口
 * @param cache        L2P cache 端口
 * @param total_writes host 累计写计数指针（host 写流判定与 WL 让路用）
 *
 * @note handle 整体清零后重新绑定；fail_gblock[] 置 UINT16_MAX（= 无读失败计数）。
 */
void mig_init(mig_handle_s *m, alc_s *alloc, l2p_jrn_s *journal, l2p_cache_s *cache, const DWORD *total_writes);
#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 测试专用：直接启动一个搬移 job（GC-18 等状态机负例）。
 */
void mig_test_job_start(mig_handle_s *m, const super_block_s *victim, mig_kind_e kind, const char *reason, DWORD triggers);
#endif

/**
 * @brief 登记一次紧急 GC 请求（写路径分配不到页对）。
 * 只登记，不立即启动；由下一次 mig_handle 消费。
 *
 * @param m      handle；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 */
void mig_gc_request_urgent(mig_handle_s *m);

/**
 * @brief 推进搬移引擎（每轮 must-run；提交即返回）
 *
 * 语义：
 *   ① job 在飞 ⇒ 只推进/心跳（等完成服务函数）；
 *   ② 不在飞 ⇒ 会话收尾/进展日志、紧急触发（mig_gc_request_urgent）；
 *   ③ 周期求值不在本函数：GC 由 timer 表的 `mig_eval`（10 ms）、WL 由
 *      `mig_wl_eval`（5 s）、predict 由 `mig_predict_eval`（1 s）分别驱动。
 *
 * @param m     handle；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param level 本轮后台档位（IDLE / NORMAL / BUSY），发布到 m->state.level 供各 timer 任务使用
 */
void mig_handle(mig_handle_s *m, bg_level_e level);

/**
 * @brief GC 周期求值入口（timer 表每 10 ms 唤醒；也可由测试直接调用）
 *
 * WL / predict 已拆成独立 timer 任务（5 s / 1 s），三者按注册顺序在同一轮
 * `timer_scheduler_poll` 内串行执行；`m->job.active` 保证每轮最多启动一个 job。
 * `m->state.level` 必须由 mig_handle 在最近一轮发布。
 *
 * @note 时钟分配：timer 任务的 period 只决定**唤醒频率**；本函数内部的
 *       deadline/interval 一律读 `platform_now_ms()`（SysTick），与
 *       mig_handle / mig_job_* 使用同一时间坐标。TIM6 的 tick 不参与 FTL
 *       时间戳。
 *
 * @param m handle
 */
void mig_eval(mig_handle_s *m);

/**
 * @brief 把 GC/WL/predict 三个求值任务注册进 timer 表（每实例一次；幂等）。
 *
 * 注册顺序 = 同轮仲裁顺序：GC（10 ms）→ WL（5 s）→ predict（1 s）。
 *
 * @param m handle
 * @return true 已注册或本次注册成功；false timer 表满
 */
BOOL mig_timer_register(mig_handle_s *m);

/* 完成服务函数 ftl_migrate_callback 的声明在 ftl_op.h —— 那里是 g_op_callbacks 各槽的唯一权威列表。 */

#endif /* FTL_MIGRATE_H */
