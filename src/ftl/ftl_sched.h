#ifndef FTL_SCHED_H
#define FTL_SCHED_H

/*
 * ftl_sched — FTL request scheduler（L2P + host + 后台 job）
 *
 * 当前职责：
 *   · work item 静态池 + 唯一分派顺序表 `FTL_SCHED_PRIO_E`；
 *   · CP 把 dirty L2P region 提交为 `L2P_FLUSH` item，统一分派启动写回；
 *   · demand / 预取 L2P load 以 `L2P_LOAD` item 分别计入独立 credit，完成由 L2P callback 结算；
 *   · host group 以 `HOST_READ` / `HOST_WRITE` item 进入 host 调度；读先于写；
 *   · CP/metadata/Journal/RR/GC/BBT/L2P CMT flush 以后台 job 进入统一分派；
 *   · host 写 admission。
 */

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

/* 后台 job 触发（bg_policy 只提交请求，scheduler 执行）。
 * 顺序 = bg_policy 任务表顺序；ftl_sched_dispatch 按此顺序执行预留 item。 */
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
#if SSD_STATS_ENABLE
    ftl_sched_stats_s stats;
#endif
} ftl_sched_s;

/**
 * @brief 初始化本 die 的调度器（绑定 workspace、清空 item / credit）
 *
 * @param sched 调度器（非空）
 * @param die   紧凑 die 号；决定使用哪一行静态 workspace
 */
void ftl_sched_init(ftl_sched_s *sched, BYTE die);

/**
 * @brief 提交一个 L2P dirty-region 写回请求（幂等）
 *
 * @param sched  调度器
 * @param region 全局 region
 * @return true 已登记；false 静态池满（CP 下一轮重试）
 */
BOOL ftl_sched_l2p_flush_request(ftl_sched_s *sched, DWORD region);

/**
 * @brief 登记一个 L2P_LOAD（已由 L2P 层启动）
 *
 * demand 与预取分开计 credit：预取不能占用 demand 的保底额度。
 *
 * @param sched    调度器
 * @param region   全局 region
 * @param prefetch true = 预取加载
 * @return true 已登记；false class credit / 静态池满（调用者稍后重试）
 */
BOOL ftl_sched_l2p_load_begin(ftl_sched_s *sched, DWORD region, BOOL prefetch);

/**
 * @brief 结束一个 L2P_LOAD（成功、重试耗尽或降级后）
 *
 * @param sched    调度器
 * @param region   全局 region
 * @param prefetch true = 预取加载（必须与 begin 时一致）
 */
void ftl_sched_l2p_load_complete(ftl_sched_s *sched, DWORD region, BOOL prefetch);

/**
 * @brief 登记一个 host 完成组（同一 group 幂等）
 *
 * @param sched 调度器
 * @param group pending 组下标
 * @param is_read true = HOST_READ；false = HOST_WRITE
 * @return true 已登记；false item 池 / host 上限满
 */
BOOL ftl_sched_host_begin(ftl_sched_s *sched, WORD group, BOOL is_read);

/**
 * @brief 是否仍有活动 L2P flush item
 *
 * @param sched 调度器
 * @return true 仍有 READY / WAIT_DEP / INFLIGHT L2P_FLUSH item
 */
BOOL ftl_sched_l2p_flush_pending(const ftl_sched_s *sched);

/**
 * @brief 清空 L2P_FLUSH 登记（新 CP job 起点；保留在飞 L2P_LOAD）
 *
 * @param sched 调度器
 */
void ftl_sched_l2p_flush_reset(ftl_sched_s *sched);

/**
 * @brief host 写启动许可
 *
 * 读路径不调用。CMT dirty 或 free 水位过高时返回 false，host 写应保持
 * 未提交（HIL 的 WAIT_GATE），让 scheduler/GC/metadata 先推进。
 *
 * @param ftl FTL 实例
 * @return true 允许启动新 host 写；false 暂缓
 */
BOOL ftl_sched_host_write_admit(const ftl_ctx_s *ftl);

/**
 * @brief 提交一个后台 job 请求（幂等；bg_policy 只产生触发）
 *
 * 后台 job 使用预留 item，不会因池满失败。
 *
 * @param sched 调度器
 * @param job   后台 job
 */
void ftl_sched_bg_request(ftl_sched_s *sched, ftl_bg_job_e job);

/**
 * @brief 唯一仲裁入口：host 读 → host 写 → 后台 job → L2P flush
 *
 * 后台 job 的执行顺序由 `FTL_SCHED_PRIO_E` 决定（`ftl_bg_job_e` 表序）；
 * 是否提交仍由 `bg_policy` 的档位/credit 决定。
 *
 * @param ftl FTL 实例
 */
void ftl_sched_dispatch(ftl_ctx_s *ftl);

/**
 * @brief 只推进一个 host 组（提交快路径；不扫其它组）
 *
 * 服务循环仍用 `ftl_sched_run_host()` 批量派发。提交快路径只 kick 本组，
 * 避免一次 submit 顺带推进所有组的 stall 计数（无时间流逝却判卡死）。
 *
 * @param ftl   FTL 实例
 * @param group pending 组下标
 */
void ftl_sched_host_kick(ftl_ctx_s *ftl, WORD group);

/**
 * @brief 回收已完成 host 组的 item（提交新组前调用，防止 group 槽复用）
 *
 * @param ftl FTL 实例
 */
void ftl_sched_host_reap(ftl_ctx_s *ftl);

/**
 * @brief FC 完成通知：回收已完成 item 并唤醒 MD_SLOT 依赖
 *
 * 由 `ftl_op_drain` 在模块完成回调返回后调用；它是所有 FC 完成进入
 * scheduler 的唯一钩子，不替代模块自己的数据/状态收尾。
 *
 * @param ftl FTL 实例
 */
void ftl_sched_note_completion(ftl_ctx_s *ftl);

/**
 * @brief 打印本实例 scheduler 现场（CLI `ftl_ctx sched` / 断言终端）
 *
 * @param ftl FTL 实例（只读）
 * @param die 实例号（打印用）
 */
void ftl_sched_debug_dump(const ftl_ctx_s *ftl, BYTE die);

#if SSD_STATS_ENABLE
/**
 * @brief 注册 scheduler 计数器（`stats ftl.sched`）
 *
 * @param sched 调度器
 * @param inst  实例号
 */
void ftl_sched_stats_register(ftl_sched_s *sched, BYTE inst);
#endif

/**
 * @brief host 调度一步：回收已完成的组，读组先于写组补发
 *
 * @param ftl FTL 实例
 */
void ftl_sched_run_host(ftl_ctx_s *ftl);

#endif /* FTL_SCHED_H */
