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
 * 状态机**：每个 job 的 phase 枚举仍由各自的头文件定义（cp_phase_t / md_job_phase_t /
 * mig_phase_t / rr_stage_t / journal_phase_t / bbt_job_phase_t）。
 *
 * ── 用法（每个 *_kick 的固定骨架）──
 *
 *   if (<job 空闲>) {
 *       job.log_ms = 0U;   // 空闲：清基准（两个时间戳必须一起清）
 *       job.start_ms = 0U;
 *   } else if (ftl_job_heartbeat(&job.log_ms, &job.start_ms)) {
 *       LOGI("[XX] job phase=%u ... age=%lums\r\n",
 *            (unsigned)job.phase, ..., (unsigned long)ftl_job_age_ms(&job.start_ms));
 *   }
 *
 * 基准由 ftl_job_heartbeat **懒初始化**（首次见到在飞时记 start）⇒ job 起点无需显式
 * 赋值。只有需要「精确到某一步」的 job（Checkpoint）才自己写 start_ms。
 *
 * ⚠ 平台时钟缺失（time_us 未注册）时 ftl_job_now_ms() 恒 0 ⇒ 心跳**永不触发**
 *   （`0 - 0 < interval`）。这是有意的取舍：心跳是纯观测，缺时钟时静默优于每轮刷屏。
 *   （与 GC 求值节拍的取舍相反——那里 0 作「尚未求值」哨兵、恒求值，因为它是**功能**，
 *   不是观测。）
 */

#include "platform.h"

#include <stdint.h>

/* 心跳节拍（毫秒）：正常 job 远快于 1s ⇒ 平时静默，只有卡住才持续可见。 */
#define FTL_JOB_LOG_INTERVAL_MS (1000U)

/*
 * ftl_job_now_ms - 当前单调毫秒（经平台抽象 time_us 换算）。平台未注册时返回 0。
 *
 * ⚠ ftl.c 另有一份等价的 ftl_now_ms()（用于 GC 求值/日志节拍、RR 巡检、CP 定时触发）
 *   ——同一个平台时钟，只是归属不同模块（那边是**功能**节拍，这里是**观测**心跳）。
 *   将来合并只留一份即可，本次不动 ftl.c 的热路径。
 */
static inline uint32_t ftl_job_now_ms(void)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();

    if ((platform == NULL) || (platform->time_us == NULL))
    {
        return 0U;
    }
    return (uint32_t)(platform->time_us() / 1000U);
}

/*
 * ftl_job_age_ms - job 已运行时长（毫秒）。基准未建立（*start_ms == 0）时返回 0。
 */
static inline uint32_t ftl_job_age_ms(const uint32_t *start_ms)
{
    const uint32_t start = *start_ms;

    return (start == 0U) ? 0U : (ftl_job_now_ms() - start);
}

/*
 * ftl_job_heartbeat - 心跳节拍到点了吗？
 *
 * 首次见到（*start_ms == 0）只**记基准**、不打（返回 false）；此后每满
 * FTL_JOB_LOG_INTERVAL_MS 更新 *log_ms 并返回 true。
 * 返回 true 时调用方打一行，age 用 ftl_job_age_ms(start_ms) 取。
 */
static inline bool ftl_job_heartbeat(uint32_t *log_ms, uint32_t *start_ms)
{
    const uint32_t now_ms = ftl_job_now_ms();

    if (*start_ms == 0U)
    {
        *start_ms = now_ms;
        *log_ms   = now_ms;
        return false;
    }

    if ((now_ms - *log_ms) < FTL_JOB_LOG_INTERVAL_MS)
    {
        return false;
    }

    *log_ms = now_ms;
    return true;
}

/*
 * ftl_job_tick - 只报计数的模块用的心跳（L2P cache：没有"单 job"结构）
 *
 * 节拍判据与 ftl_job_heartbeat 逐字相同，只是不维护 start_ms——在飞的是若干 cache
 * line（load / flush），没有"某个 job 从哪一刻开始"可言 ⇒ 调用方只报计数，不报 age。
 * 调用方在**无在飞**时把 *log_ms 直接置 0 清基准（无 start 可清）。
 */
static inline bool ftl_job_tick(uint32_t *log_ms)
{
    const uint32_t now_ms = ftl_job_now_ms();

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

#endif /* FTL_JOB_H */
