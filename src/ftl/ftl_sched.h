#ifndef FTL_SCHED_H
#define FTL_SCHED_H

/*
 * ftl_sched — FTL 请求调度器 + 后台策略（L2P + host + 后台 job）
 *
 * 职责：
 *   · work item 静态池 + 唯一分派顺序表 `FTL_SCHED_PRIO_E`；
 *   · CP 把 dirty L2P region 提交为 `L2P_FLUSH` item，统一分派启动写回；
 *   · demand / 预取 L2P load 以 `L2P_LOAD` item 分别计入独立 credit，完成由 L2P callback 结算；
 *   · host group 以 `HOST_READ` / `HOST_WRITE` item 进入 host 调度；读先于写；
 *   · 后台策略：按负载选档 → token bucket 预算 → 按 `ftl_bg_job_e` 表序提交；
 *     执行由 `ftl_sched_dispatch` 统一仲裁；
 *   · host 写 admission。
 */

#include "ftl_type.h"
#include "ssd_types.h"
#include "stats.h"

/* 本池只承载活动 item。容量 = 后台 job 预留 10 + L2P_FLUSH 64 + L2P_LOAD 8
 * + 预取 2 + host group 32 + 12 余量；CP snapshot 与 pending groups 仍是各自 backlog。 */
#define FTL_SCHED_MAX_WORK (128U)

/* demand load 的 class credit：与 CRIT 命令预留 8 对齐（含在飞重试余量）。
 * 只是本仓上限，不是行业值。 */
#define FTL_SCHED_L2P_LOAD_CREDIT (8U)

/* 预取 load 的独立 credit：预取永远不能吃 demand 的 8 个额度。
 * 2 与 L2P_PREFETCH_MAX_INFLIGHT 同量级（本仓自拟）。 */
#define FTL_SCHED_L2P_PREFETCH_CREDIT (2U)

/* CP snapshot 位图仍是 backlog；本上限只限活动 flush item，超额部分由 CP
 * 下一轮继续登记，不丢。 */
#define FTL_SCHED_L2P_FLUSH_MAX (64U)

/* host group 并发上限，与 pending 组表同量级；写组最多用 HOST_MAX - 读保留，
 * 保证写队列再深也不会把读组槽吃光。4 是本仓自拟保留量。 */
#define FTL_SCHED_HOST_MAX          (32U)
#define FTL_SCHED_HOST_READ_RESERVE (4U)

/* 后台 token bucket：timer 节拍（ftl_bg_tick_task，周期 L2P_FLUSH_INTERVAL_MS）
 * 为每实例补 grant_units；上限 = grant_units × BG_TOKEN_CAP_TICKS。
 * 实际补充速率由 timer 周期决定，与主循环频率无关。 */
#define BG_TOKEN_CAP_TICKS (4U)

/* FTL 实例类型在 ftl.h 定义；本头只用指针，避免反向 include。 */
typedef struct FTL_CONTEXT_S ftl_ctx_s;

/* class 只表达请求类别与资源账户，**不表达优先级**；跨类顺序见 FTL_SCHED_PRIO_E。 */
typedef enum FTL_WORK_CLASS_E
{
    FTL_WORK_CLASS_HOST_READ = 0,
    FTL_WORK_CLASS_L2P_LOAD,
    FTL_WORK_CLASS_L2P_FLUSH,
    FTL_WORK_CLASS_CP_META,
    FTL_WORK_CLASS_JOURNAL,
    FTL_WORK_CLASS_GC,
    FTL_WORK_CLASS_RECOVERY,
    FTL_WORK_CLASS_HOST_WRITE,
    FTL_WORK_CLASS_NUM
} ftl_work_class_e;

/* 后台 job：任务表下标 = 本枚举，表序即提交序 / 执行优先级（唯一坐标）。
 * 任务策略（must_run / units / gate）见 ftl_sched.c 的 s_bg_tasks。 */
typedef enum FTL_BG_JOB_E
{
    FTL_BG_JOB_CP_ADVANCE = 0,
    FTL_BG_JOB_RECOVERY,
    FTL_BG_JOB_GC,
    FTL_BG_JOB_METADATA,
    FTL_BG_JOB_JOURNAL_ERASE,
    FTL_BG_JOB_CP_EMERGENCY,
    FTL_BG_JOB_L2P_FLUSH,
    FTL_BG_JOB_JOURNAL_FLUSH,
    FTL_BG_JOB_BBT,
    FTL_BG_JOB_CP_TRIGGER,
    FTL_BG_JOB_NUM
} ftl_bg_job_e;

/* 仲裁顺序：数值越小越先。这是**唯一**顺序来源——改 QoS 只改这张表。
 * host 读/写保留现有先后；后台 job 按 ftl_bg_job_e 表序；普通 L2P flush 在后台之后。 */
typedef enum FTL_SCHED_PRIO_E
{
    FTL_SCHED_PRIO_HOST_READ = 0,
    FTL_SCHED_PRIO_HOST_WRITE,
    FTL_SCHED_PRIO_BG_BASE, /* = + job；2..7 must-run，8..11 预算任务 */
    FTL_SCHED_PRIO_L2P_FLUSH = FTL_SCHED_PRIO_BG_BASE + FTL_BG_JOB_NUM,
    FTL_SCHED_PRIO_NUM
} ftl_sched_prio_e;

typedef enum FTL_WORK_STATE_E
{
    FTL_WORK_FREE = 0,
    FTL_WORK_READY,
    FTL_WORK_WAIT_DEP,
    FTL_WORK_INFLIGHT
} ftl_work_state_e;

/* 依赖位；L2P flush 等 metadata slot。 */
#define FTL_WORK_DEP_MD_SLOT (1UL << 0U)

#define FTL_SCHED_ITEM_MAGIC (0x5A5AU) /* item 未被复用时必须是该值 */

typedef struct FTL_WORK_ITEM_S
{
    DWORD            region; /* L2P class：全局 region */
    WORD             group;  /* host class：pending group 下标 */
    BYTE             job;    /* 后台 job：ftl_bg_job_e */
    BOOL             bg_job; /* true = 后台 job item（与普通 L2P_FLUSH item 区分） */
    DWORD            dep_mask;
    ftl_work_class_e class_id;
    ftl_work_state_e state;
    BOOL             prefetch; /* true = 预取 L2P_LOAD（与 demand load 分开计 credit） */
    WORD             magic;    /* 分配时写 FTL_SCHED_ITEM_MAGIC；防越界/脏值 */
} ftl_work_item_s;

/* 后台任务策略：下标 = ftl_bg_job_e；job 本身不存进表项（唯一坐标 = 表下标）。 */
typedef struct BG_TASK_S
{
    WORD units;    /* 单次提交的工作量单位（预算任务用） */
    BOOL must_run; /* 正确性关键集：不 gate、不占配额、每轮无条件提交 */
    BOOL gated;    /* true = 提交前检查 l2p_flush_due */
} bg_task_s;

/* 档位 → 每个 token interval 的 credit 配额（FTL 策略表，引擎不持有）。 */
typedef struct BG_LEVEL_CONFIGURATION_S
{
    DWORD grant_units; /* 每个 token interval 的 credit 数 */
    DWORD task_mask;   /* 本档位允许的预算任务位集合（1U << job） */
} bg_level_cfg_s;

/* 每实例后台策略状态（任务表是共享只读，不逐实例复制）。 */
typedef struct FTL_SCHED_BG_S
{
    const bg_task_s      *tasks;
    BYTE                  task_count;
    const bg_level_cfg_s *cfg;     /* 本轮档位配置（publish 起点写入） */
    bg_level_e            level;   /* 本轮档位（模块入口读它） */
    DWORD                 credits; /* token bucket 余额 */
} ftl_sched_bg_s;

#if SSD_STATS_ENABLE
typedef struct FTL_SCHED_STATISTICS_S
{
    /* 计数器语义：*_begin 是登记次数（同一 item 重试不重复计），*_reaped 是回收次数。
     * bg_request 每次 FREE→READY 计一次；must-run 每轮重新触发，数值大≠item 分配。 */
    DWORD host_begin;
    DWORD host_reaped;
    DWORD load_begin;
    DWORD load_complete;
    DWORD prefetch_begin;
    DWORD prefetch_complete;
    DWORD flush_request;
    DWORD flush_reaped;
    DWORD bg_request;
    DWORD completion_note;
} ftl_sched_stats_s;
#endif

typedef struct FTL_SCHEDULER_S
{
    ftl_work_item_s *items;                      /* 静态 workspace（SRAM3，按 die 一份） */
    BYTE             die;                        /* 本 scheduler 的 die 号（workspace 行 / 日志） */
    DWORD            credit[FTL_WORK_CLASS_NUM]; /* 只有 L2P_LOAD / L2P_FLUSH 使用；其余 class 仅观测 */
    WORD             host_items;                 /* 活动 host item 数（快速判空） */
    WORD             flush_items;                /* 普通 L2P_FLUSH item 数（不含后台 job） */
    WORD             prefetch_items;             /* 活动预取 L2P_LOAD item 数（独立 credit） */
    BOOL             host_running;               /* host 补发重入保护（完成回调可重入 submit） */
    BOOL             l2p_flush_failed;
    ftl_sched_bg_s   bg; /* 后台策略：任务表 / 档位 / token bucket */
#if SSD_STATS_ENABLE
    ftl_sched_stats_s stats;
#endif
} ftl_sched_s;

/* ── 查询 / 谓词 ── */

/** @brief 是否仍有活动 L2P flush item。 */
BOOL ftl_sched_l2p_flush_is_pending(const ftl_sched_s *sched);

/**
 * @brief host 写启动许可（读路径不调用）。
 *
 * @note CMT dirty 或 free 水位过高时返回 false，host 写应保持未提交（HIL 的
 *       WAIT_GATE），让 scheduler/GC/metadata 先推进。
 */
BOOL ftl_sched_host_write_is_admitted(const ftl_ctx_s *ftl);

/* ── L2P load / flush 登记 ── */

/** @brief 提交一个 L2P dirty-region 写回请求（幂等）。 */
BOOL ftl_sched_l2p_flush_request(ftl_sched_s *sched, DWORD region);

/**
 * @brief 结束一个 L2P_LOAD（成功、重试耗尽或降级后）。
 *
 * @note prefetch 必须与 begin 时一致。
 */
void ftl_sched_l2p_load_finish(ftl_sched_s *sched, DWORD region, BOOL prefetch);

/**
 * @brief 清空 L2P_FLUSH 登记（新 CP job 起点）。
 *
 * @note 保留在飞 L2P_LOAD。
 */
void ftl_sched_l2p_flush_reset(ftl_sched_s *sched);

/* ── host 组 ── */

/**
 * @brief 只推进一个 host 组（提交快路径；不扫其它组）。
 *
 * @note 服务循环用 `ftl_sched_run_host()` 批量派发。只 kick 本组，避免一次
 *       submit 顺带推进所有组的 stall 计数（无时间流逝却判卡死）。
 */
void ftl_sched_host_kick(ftl_ctx_s *ftl, WORD group);

/** @brief 回收已完成 host 组的 item（提交新组前调用，防止 group 槽复用）。 */
void ftl_sched_host_reap(ftl_ctx_s *ftl);

/* ── 后台 job / 完成通知 ── */

/**
 * @brief 提交一个后台 job 请求（幂等；后台策略只提交请求）。
 *
 * @note 后台 job 使用预留 item，不会因池满失败。
 */
void ftl_sched_bg_request(ftl_sched_s *sched, ftl_bg_job_e job);

/**
 * @brief FC 完成通知：回收已完成 item 并唤醒 MD_SLOT 依赖。
 *
 * @note 由 `ftl_op_drain` 在模块完成回调返回后调用；它是所有 FC 完成进入
 *       scheduler 的唯一钩子，不替代模块自己的数据/状态收尾。
 */
void ftl_sched_note_completion(ftl_ctx_s *ftl);

/* ── 计数器注册 ── */

#if SSD_STATS_ENABLE
/** @brief 注册 scheduler 计数器（`stats ftl.sched`）。 */
void ftl_sched_stats_register(ftl_sched_s *sched, BYTE inst);
#endif

/* ── 后台策略 ── */

/**
 * @brief 负载 → 后台档位（纯函数）。
 *
 * @note pending 与组计数全空 ⇒ IDLE；pending 过半池或有未决组 ⇒ BUSY；其余 NORMAL。
 */
bg_level_e ftl_sched_bg_level_select(const ftl_ctx_s *ftl);

/** @brief 补充预算 credit（timer 节拍调用）。 */
void ftl_sched_bg_refill(ftl_sched_s *sched);

#if defined(FC_NAND_TEST_ENABLE)
/** @brief 测试钩子：只做选档 + 发布 cfg，不提交任务。 */
void ftl_sched_bg_test_publish(ftl_ctx_s *ftl);
#endif

/* ── 运行 ── */

/** @brief host 调度一步：回收已完成的组，读组先于写组补发。 */
void ftl_sched_run_host(ftl_ctx_s *ftl);

/** @brief 打印本实例 scheduler 现场（CLI `ftl_ctx sched` / 断言终端）。 */
void ftl_sched_debug_dump(const ftl_ctx_s *ftl, BYTE die);

/**
 * @brief 唯一仲裁入口：host 读 → host 写 → 后台 job → L2P flush。
 *
 * @note 后台 job 的执行顺序由 `FTL_SCHED_PRIO_E` 决定（`ftl_bg_job_e` 表序）。
 *       本入口先做后台策略（选档 + token bucket + 按表序提交），再执行。
 */
void ftl_sched_dispatch(ftl_ctx_s *ftl);

/* ── 入口 ── */

/**
 * @brief 登记一个 L2P_LOAD（已由 L2P 层启动）。
 *
 * @note demand 与预取分开计 credit：预取不能占用 demand 的保底额度。
 */
BOOL ftl_sched_l2p_load_begin(ftl_sched_s *sched, DWORD region, BOOL prefetch);

/** @brief 登记一个 host 完成组（同一 group 幂等）。 */
BOOL ftl_sched_host_begin(ftl_sched_s *sched, WORD group, BOOL is_read);

/**
 * @brief 绑定内置任务表并复位档位 / credit。
 *
 * @note 必须在 ftl_sched_init 之后调用（init 会 memset 整个 sched）。
 */
void ftl_sched_bg_init(ftl_sched_s *sched);

/** @brief 初始化本 die 的调度器（绑定 workspace、清空 item / credit）。 */
void ftl_sched_init(ftl_sched_s *sched, BYTE die);

#endif /* FTL_SCHED_H */
