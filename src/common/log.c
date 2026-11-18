#include "log.h"

#include "platform.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* This filter applies before formatting, so suppressed logs consume no TX queue space. */
static log_level_e         g_log_level = LOG_INFO;
static log_sink_fn_t       g_log_sink;
static log_flush_sink_fn_t g_log_flush_sink;
static DWORD               g_log_truncated;
static DWORD               g_log_write_failures;

void log_write(log_level_e level, const char *message)
{
    const ssd_platform_ops_s *platform = ssd_platform_get();
    DWORD                     length;
    DWORD                     result;

    if ((message == NULL) || (level > g_log_level))
    {
        return;
    }

    length = strlen(message);

    /* An explicitly registered sink lets a bare-metal BSP log before platform registration. */
    if (g_log_sink != NULL)
    {
        result = g_log_sink(message, length);
    }
    else if ((platform != NULL) && (platform->log_write != NULL))
    {
        platform->log_write(message);
        return;
    }
    else
    {
        return;
    }

    if (result != length)
    {
        g_log_write_failures++;
    }
}

void log_vprintf(log_level_e level, const char *format, va_list args)
{
    char message[192];
    int  length;

    if ((format == NULL) || (level > g_log_level))
    {
        return;
    }

    length = vsnprintf(message, sizeof(message), format, args);
    if (length <= 0)
    {
        return;
    }
    if ((size_t)length >= sizeof(message))
    {
        static const char marker[]      = "...[truncated]";
        const size_t      marker_length = sizeof(marker) - 1U;
        const size_t      marker_offset = sizeof(message) - marker_length - 1U;

        g_log_truncated++;
        memcpy(&message[marker_offset], marker, marker_length);
        message[sizeof(message) - 1U] = '\0';
    }

    /* Pass already-formatted text to the platform sink to preserve transport independence. */
    log_write(level, message);
}

void log_printf(log_level_e level, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    log_vprintf(level, format, args);
    va_end(args);
}

void log_set_level(log_level_e level)
{
    if (level <= LOG_DEBUG)
    {
        g_log_level = level;
    }
}

log_level_e log_get_level(void)
{
    return g_log_level;
}

void log_set_sink(log_sink_fn_t sink)
{
    g_log_sink = sink;
}

BOOL log_flush(DWORD timeout_ms)
{
    return (g_log_flush_sink != NULL) ? g_log_flush_sink(timeout_ms) : true;
}

void log_set_flush_sink(log_flush_sink_fn_t sink)
{
    g_log_flush_sink = sink;
}

DWORD log_get_truncated_count(void)
{
    return g_log_truncated;
}

DWORD log_get_write_failure_count(void)
{
    return g_log_write_failures;
}

/* ── 日志节拍 / 限流 ── */

/* 无时钟（platform_now_ms() == 0）时的条数兜底：抑制满这么多条放行一行。 */
#define LOG_RATE_COUNT_FALLBACK (64U)

/**
 * @brief 起算心跳：记 start/log 并置 active
 *
 * 时钟缺失（platform_now_ms() == 0）时 start=0，log_heartbeat_due() 恒 false。
 *
 * @param hb 心跳对象
 */
void log_heartbeat_begin(log_heartbeat_s *hb)
{
    hb->start_ms = platform_now_ms();
    hb->log_ms   = hb->start_ms;
    hb->active   = true;
}

/**
 * @brief 心跳到点了吗
 *
 * 到点返回 true 并推进 log_ms：前 LOG_RATE_SLOW_AFTER_MS 每 1 s，之后每 5 s。
 *
 * @param hb 心跳对象
 * @return true 到点；false 未到点 / 未 active
 */
BOOL log_heartbeat_due(log_heartbeat_s *hb)
{
    if (hb->active == false)
    {
        return false;
    }

    const DWORD now_ms   = platform_now_ms();
    const DWORD interval = ((now_ms - hb->start_ms) >= LOG_RATE_SLOW_AFTER_MS) ? LOG_RATE_SLOW_INTERVAL_MS : LOG_RATE_INTERVAL_MS;

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
void log_heartbeat_end(log_heartbeat_s *hb)
{
    hb->start_ms = 0U;
    hb->log_ms   = 0U;
    hb->active   = false;
}

/**
 * @brief 心跳已运行时长（毫秒）
 *
 * @param hb 心跳对象
 * @return 已运行毫秒；未 active 时 0
 */
DWORD log_heartbeat_age_ms(const log_heartbeat_s *hb)
{
    return (hb->active == false) ? 0U : (platform_now_ms() - hb->start_ms);
}

/**
 * @brief 初始化限流状态
 *
 * last=0 ⇒ 首次调用立即放行。
 *
 * @param rl          限流对象
 * @param interval_ms 时间间隔（毫秒）
 */
void log_ratelimit_init(log_ratelimit_s *rl, DWORD interval_ms)
{
    rl->last_ms     = 0U;
    rl->interval_ms = interval_ms;
    rl->suppressed  = 0U;
}

/**
 * @brief 限流到点了吗
 *
 * 复用 log_rate_due 的引擎；interval 由对象携带。
 *
 * @param rl 限流对象
 * @return true 应打印；false 继续抑制
 */
BOOL log_ratelimit_due(log_ratelimit_s *rl)
{
    return log_rate_due(&rl->last_ms, &rl->suppressed, rl->interval_ms);
}

/**
 * @brief 只报计数的模块用的心跳（没有“单 job”结构）
 *
 * 首次只记基准并返回 false；之后每 LOG_RATE_INTERVAL_MS 放行一次。
 *
 * @param log_ms 上次打点时刻（进出参；到点时更新为当前时间）
 * @return true 到点；false 未到点或首次仅记基准
 */
BOOL log_tick(DWORD *log_ms)
{
    const DWORD now_ms = platform_now_ms();

    if (*log_ms == 0U)
    {
        *log_ms = now_ms;
        return false;
    }

    if ((now_ms - *log_ms) < LOG_RATE_INTERVAL_MS)
    {
        return false;
    }

    *log_ms = now_ms;
    return true;
}

/**
 * @brief 日志限流引擎：时间优先，无时钟时按条数兜底
 *
 * 放行时复位 count / last_ms；无时钟且 count == NULL 时恒 false（纯观测日志静默）。
 *
 * @param last_ms     上次打印时刻（0 = 尚未打印）
 * @param count       被抑制条数（可 NULL）
 * @param interval_ms 时间间隔（毫秒）
 * @return true 应打印；false 继续抑制
 */
BOOL log_rate_due(DWORD *last_ms, DWORD *count, DWORD interval_ms)
{
    const DWORD now_ms = platform_now_ms();
    BOOL        due;

    if (now_ms != 0U)
    {
        due = (*last_ms == 0U) || ((now_ms - *last_ms) >= interval_ms);
    }
    else
    {
        due = (count != NULL) && (*count >= LOG_RATE_COUNT_FALLBACK);
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
