/*
 * ftl_job.c — 后台 job 观测口的 out-of-line 实现
 *
 * 头文件只放轻量 inline（时钟换算 / 心跳节拍）；日志限流要写多个状态字段、
 * 分支多于 H42 的 header inline 门槛，放 .c 保证全仓一份实现。
 */

#include "ftl_job.h"

#include <stddef.h>

/* 无时钟（ftl_job_now_ms() == 0）时的条数兜底：抑制满这么多条放行一行。 */
#define FTL_JOB_LOG_COUNT_FALLBACK (64U)

/**
 * @brief 起算心跳：记 start/log 并置 active
 *
 * 时钟缺失时 start=0，due 恒 false。
 *
 * @param hb 心跳对象
 */
void ftl_heartbeat_begin(ftl_heartbeat_t *hb)
{
    hb->start_ms = ftl_job_now_ms();
    hb->log_ms   = hb->start_ms;
    hb->active   = true;
}

/**
 * @brief 心跳到点了吗
 *
 * 到点返回 true 并推进 log_ms：前 5 s 每 1 s，之后每 5 s。
 *
 * @param hb 心跳对象
 * @return true 到点；false 未到点 / 未 active
 */
bool ftl_heartbeat_due(ftl_heartbeat_t *hb)
{
    if (hb->active == false)
    {
        return false;
    }

    const DWORD now_ms   = ftl_job_now_ms();
    const DWORD interval = ((now_ms - hb->start_ms) >= FTL_JOB_LOG_SLOW_AFTER_MS) ? FTL_JOB_LOG_SLOW_INTERVAL_MS : FTL_JOB_LOG_INTERVAL_MS;

    if ((now_ms - hb->log_ms) < interval)
    {
        return false;
    }

    hb->log_ms = now_ms;
    return true;
}

/**
 * @brief 清心跳基准
 *
 * 下一个 job 必须重新 begin。
 *
 * @param hb 心跳对象
 */
void ftl_heartbeat_end(ftl_heartbeat_t *hb)
{
    hb->start_ms = 0U;
    hb->log_ms   = 0U;
    hb->active   = false;
}

/**
 * @brief 初始化限流状态
 *
 * last=0 ⇒ 首次立即放行。
 *
 * @param rl          限流对象
 * @param interval_ms 时间间隔
 */
void ftl_ratelimit_init(ftl_ratelimit_t *rl, DWORD interval_ms)
{
    rl->last_ms     = 0U;
    rl->interval_ms = interval_ms;
    rl->suppressed  = 0U;
}

/**
 * @brief 限流到点了吗
 *
 * 复用 ftl_job_log_due 的引擎；interval 由对象携带。
 *
 * @param rl 限流对象
 * @return true 应打印；false 继续抑制
 */
bool ftl_ratelimit_due(ftl_ratelimit_t *rl)
{
    return ftl_job_log_due(&rl->last_ms, &rl->suppressed, rl->interval_ms);
}

/**
 * @brief 日志限流引擎：时间优先，无时钟时按条数兜底
 *
 * 放行时复位 count / last_ms。
 *
 * @param last_ms     上次打印时刻（0 = 尚未打印）
 * @param count       被抑制条数（可 NULL）
 * @param interval_ms 时间间隔
 * @return true 应打印；false 继续抑制
 */
bool ftl_job_log_due(DWORD *last_ms, DWORD *count, DWORD interval_ms)
{
    const DWORD now_ms = ftl_job_now_ms();
    bool        due;

    if (now_ms != 0U)
    {
        due = (*last_ms == 0U) || ((now_ms - *last_ms) >= interval_ms);
    }
    else
    {
        due = (count != NULL) && (*count >= FTL_JOB_LOG_COUNT_FALLBACK);
    }

    if (due == false)
    {
        if (count != NULL)
        {
            (*count)++;
        }
        return false;
    }

    *last_ms = now_ms;
    if (count != NULL)
    {
        *count = 0U;
    }
    return true;
}
