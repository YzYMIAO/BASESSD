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

// 实现要点：时间优先；条数只在无时钟时兜底（有 clock 时 count 仅累计、不参与放行）。
// 放行时先把被抑制条数写进 out_suppressed，再复位 count / last_ms —— 调用方拿到的
// 是「本次这一行之前压掉了多少条」。
bool ftl_job_log_due(DWORD *last_ms, DWORD *count, DWORD interval_ms, DWORD *out_suppressed)
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

    if (out_suppressed != NULL)
    {
        *out_suppressed = (count != NULL) ? *count : 0U;
    }
    *last_ms = now_ms;
    if (count != NULL)
    {
        *count = 0U;
    }
    return true;
}
