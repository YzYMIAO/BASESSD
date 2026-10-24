/*
 * assertion.c — 断言设施实现（生产断言 + 富断言集的失败后端）
 *
 * 文件结构：
 *   ① 常量
 *   ② assertion_report_and_terminate —— 落记录 + 日志 + 停机
 *   ③ 失败入口 assertion_fail / assertion_failf / _i64 / _u64
 *   ④ assertion_terminate —— flush → 平台上下文 → dump → 停机
 *   ⑤ 平台上下文弱钩子
 *   ⑥ 固件 abort() 重定向
 *
 * 终端顺序（不可颠倒）：
 *   platform_context（异步日志）→ flush 排空 → crash_platform_report（独占 UART）
 *   dump 会关掉 TX DMA/中断，若排在 flush 之前，队列里还没发出去的日志会被一起掐掉。
 */

#include "assertion.h"

#include "crash.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* 等待异步日志排空的上限（ms）；超时也继续走 dump，不能卡在 flush。 */
#define ASSERTION_FLUSH_TIMEOUT_MS 1000U

/* 单条断言文本的最大长度（含结尾 '\0'）。 */
#define ASSERTION_TEXT_CAPACITY 192U

/* ── ② 公共终端：落记录 → 日志 → 停机 ── */

static _Noreturn void assertion_report_and_terminate(const char *text, const char *file, int line)
{
    crash_record_assert(text, file, line);
    LOGE("assertion failed: %s (%s:%d)\r\n", text, file, line);
    assertion_terminate();
}

/* ── ③ 失败入口 ── */

_Noreturn void assertion_fail(const char *expression, const char *file, int line)
{
    assertion_report_and_terminate(expression, file, line);
}

_Noreturn void assertion_failf(const char *file, int line, const char *format, ...)
{
    char    buffer[ASSERTION_TEXT_CAPACITY];
    va_list ap;

    va_start(ap, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    assertion_report_and_terminate(buffer, file, line);
}

_Noreturn void assertion_fail_i64(const char *expression_a, const char *op, const char *expression_b, int64_t a, int64_t b, const char *file, int line)
{
    assertion_failf(file, line, "%s %s %s (%lld %s %lld)", expression_a, op, expression_b, a, op, b);
}

_Noreturn void assertion_fail_u64(const char *expression_a, const char *op, const char *expression_b, QWORD a, QWORD b, const char *file, int line)
{
    assertion_failf(file, line, "%s %s %s (%llu %s %llu)", expression_a, op, expression_b, a, op, b);
}

/* ── ④ 终端路径 ── */

_Noreturn void assertion_terminate(void)
{
    assertion_platform_context();
    (void)log_flush(ASSERTION_FLUSH_TIMEOUT_MS);
    crash_platform_report(crash_record_get());

#if defined(SSD_FIRMWARE_BUILD)
    /* 运行中的 IWDG 会把这里变成一次有界、可诊断的复位。 */
    for (;;)
    {
    }
#else
    abort();
#endif
}

/* ── ⑤ 平台上下文弱钩子（默认空；由 FC 等模块强符号覆盖） ── */

__attribute__((weak)) void assertion_platform_context(void)
{
}

/* ── ⑥ 固件 abort() 重定向 ── */

#if defined(SSD_FIRMWARE_BUILD)
/*
 * 覆盖 libc abort()：munit 回归上游后失败路径只调 abort()，此处承接终端路径，
 * 让已经落到异步 UART 队列的断言文本能被 flush 排空，并保留崩溃现场。
 * （host 构建不覆盖，直接用 libc abort()。）
 */
_Noreturn void abort(void)
{
    crash_record_abort();
    assertion_terminate();
}
#endif
