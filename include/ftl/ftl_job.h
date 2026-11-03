#ifndef FTL_JOB_H
#define FTL_JOB_H

/*
 * ftl_job.h — 后台 job 的统一心跳（观测口）
 *
 * 异步化之后「job 卡在哪个 phase」不再有阻塞的调用者能看见：没有 waiter 会超时，
 * 也没有栈帧可以被 dump。统一心跳把这件事变成一行日志——job 在飞期间每满
 * FTL_JOB_LOG_INTERVAL_MS 打一行 phase/age；判据 = 两次心跳之间 phase 不动 ⇒ 卡死。
 *
 * 本头文件只提供「该不该打、上次什么时候打的」这层节拍与公共时钟，**不参与任何
 * 状态机**：每个 job 的 phase 枚举仍由各自的头文件定义（cp_phase_e / md_handle_phase_e /
 * mig_phase_e / rr_stage_e / jrn_phase_e / bbt_handle_phase_e）。
 *
 * 时间状态按拥有者分三类（与 `include/platform/timer.h` 的调度器同构）：
 *   · `ftl_deadline_t` —— job 内等待/超时：绝对时刻 + armed；
 *   · `ftl_heartbeat_t`—— job 观测心跳：start/log + active；
 *   · `ftl_ratelimit_t`—— 日志限流：last/interval + 抑制计数。
 * 业务结构只持有这些对象，不再裸放 `start_ms/log_ms/last_ms`。
 *
 * ⚠ 平台时钟缺失（time_us 未注册）时 ftl_job_now_ms() 恒 0 ⇒ 心跳**永不触发**。
 *   这是有意的取舍：心跳是纯观测，缺时钟时静默优于每轮刷屏。
 */

#include "platform.h"
#include "timer.h" /* timer_time_due：全仓唯一的回绕安全时刻比较 */

#include <stdint.h>

/* 心跳节拍（毫秒）：正常 job 远快于 1s ⇒ 平时静默，只有卡住才持续可见。
 * ⚠ 卡死时 1s/行会刷屏（UART 阻塞、拖慢主循环）：job 超过 SLOW_AFTER 后降到 5s/行，
 *   既保留“卡在哪一 phase”的证据，又把最坏情况压到 ~1 行/5s。 */
#define FTL_JOB_LOG_INTERVAL_MS      (1000U)
#define FTL_JOB_LOG_SLOW_AFTER_MS    (5000U)
#define FTL_JOB_LOG_SLOW_INTERVAL_MS (5000U)

/**
 * @brief 当前单调毫秒（平台 `time_ms` = SysTick/HAL tick）。平台未注册时返回 0。
 *
 * 本函数是**全仓唯一**的平台时钟→毫秒换算：job 心跳（观测）与 bg_scheduler 的
 * GC/RR/CP 功能节拍都调用它，避免两份同语义实现漂移。
 *
 * @return 当前毫秒；平台未注册时 0。`time_ms` 未提供（旧测试夹具）时退回 `time_us/1000`。
 */
static inline DWORD ftl_job_now_ms(void)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();

    if (platform == NULL)
    {
        return 0U;
    }
    if (platform->time_ms != NULL)
    {
        return platform->time_ms();
    }
    if (platform->time_us == NULL)
    {
        return 0U;
    }
    /* QWORD µs → DWORD ms 是接口边界上的有意窄化：所有消费方都用差值（回绕安全，
     * 约 49.7 天一圈），显式写出截断。 */
    return (DWORD)(platform->time_us() / 1000U);
}

/* ── 时间对象（业务结构只持有这些，不裸放时间戳）── */

typedef DWORD ftl_time_t;

/**
 * @brief 时刻比较：now 是否已到达/越过 deadline。
 *
 * 复用 `timer_time_due`（有符号差 ⇒ DWORD 毫秒回绕安全），全仓不再手写
 * `now - due >= 0`。
 */
static inline bool ftl_time_after(ftl_time_t now, ftl_time_t deadline)
{
    return timer_time_due(now, deadline);
}

/* job 内等待/超时：绝对时刻 + armed（不再用 0 当“未计时”哨兵）。 */
typedef struct
{
    ftl_time_t at;
    bool       armed;
} ftl_deadline_t;

static inline void ftl_deadline_arm(ftl_deadline_t *d, ftl_time_t now, DWORD timeout_ms)
{
    d->at    = now + timeout_ms;
    d->armed = true;
}

static inline bool ftl_deadline_expired(const ftl_deadline_t *d, ftl_time_t now)
{
    return d->armed && ftl_time_after(now, d->at);
}

static inline void ftl_deadline_clear(ftl_deadline_t *d)
{
    d->armed = false;
}

/* job 观测心跳：起止显式（active），不再靠 start_ms==0 判“未开始”。 */
typedef struct
{
    ftl_time_t start_ms;
    ftl_time_t log_ms;
    bool       active;
} ftl_heartbeat_t;

void ftl_heartbeat_begin(ftl_heartbeat_t *hb);
bool ftl_heartbeat_due(ftl_heartbeat_t *hb);
void ftl_heartbeat_end(ftl_heartbeat_t *hb);

/**
 * @brief job 已运行时长（毫秒）。未 active 时返回 0。
 *
 * @param hb 心跳对象
 * @return 已运行毫秒；未 active 时 0。
 */
static inline DWORD ftl_heartbeat_age_ms(const ftl_heartbeat_t *hb)
{
    return (hb->active == false) ? 0U : (ftl_job_now_ms() - hb->start_ms);
}

/**
 * @brief 原始起始时刻的已运行毫秒（用于非 heartbeat 对象，如 GC 会话 / TRIM）。
 *
 * @param start_ms 起始时刻（0 = 未开始）
 * @return 已运行毫秒；未开始时 0。
 */
static inline DWORD ftl_job_age_ms(const ftl_time_t *start_ms)
{
    return (*start_ms == 0U) ? 0U : (ftl_job_now_ms() - *start_ms);
}

/* 日志限流：last/interval + 被抑制条数（无时钟时按条数兜底）。 */
typedef struct
{
    ftl_time_t last_ms;
    DWORD      interval_ms;
    DWORD      suppressed;
} ftl_ratelimit_t;

void ftl_ratelimit_init(ftl_ratelimit_t *rl, DWORD interval_ms);
bool ftl_ratelimit_due(ftl_ratelimit_t *rl);

/**
 * @brief 只报计数的模块用的心跳（L2P cache：没有"单 job"结构）
 *
 * 节拍判据与 ftl_heartbeat_due 逐字相同，只是不维护 start_ms——在飞的是若干 cache
 * line（load / flush），没有"某个 job 从哪一刻开始"可言 ⇒ 调用方只报计数，不报 age。
 * 调用方在**无在飞**时把 *log_ms 直接置 0 清基准（无 start 可清）。
 *
 * @param log_ms 上次打点时刻（进出参：到点时更新为当前时间）。
 *
 * @return true  到点，调用方打一行
 *         false 未到点或首次仅记基准
 */
static inline bool ftl_job_tick(DWORD *log_ms)
{
    const DWORD now_ms = ftl_job_now_ms();

    if (*log_ms == 0U)
    {
        *log_ms = now_ms;
        return false;
    }

    if ((now_ms - *log_ms) < FTL_JOB_LOG_INTERVAL_MS)
    {
        return false;
    }

    *log_ms = now_ms;
    return true;
}

/**
 * @brief 日志限流：有时间源时按时间（首次立即放行），无时钟时按条数兜底。
 *
 * @param last_ms        上次打印时刻（进出参；0 = 尚未打印）
 * @param count          自上次打印以来被抑制的条数（进出参；NULL = 不做条数兜底）
 * @param interval_ms    时间间隔（毫秒）
 * @return true  本次应打印
 *         false 继续抑制
 *
 * @note 无时钟（ftl_job_now_ms() == 0）且 count == NULL ⇒ 恒 false：纯观测日志在
 *       缺时钟时静默，避免每轮刷屏（与 ftl_heartbeat_due 同取舍）。有 count 时退化
 *       为「抑制满 FTL_JOB_LOG_COUNT_FALLBACK 条打一行」。
 */
bool ftl_job_log_due(DWORD *last_ms, DWORD *count, DWORD interval_ms);

#endif /* FTL_JOB_H */
