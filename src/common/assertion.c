/*
 * assertion.c — 断言设施实现（生产断言 + 富断言集的失败后端）
 *
 * 文件结构：
 *   ① 常量与内部辅助（64 位十进制打印）
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

#include "common/assertion.h"

#include "common/crash.h"
#include "common/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* 等待异步日志排空的上限（ms）；超时也继续走 dump，不能卡在 flush。 */
#define ASSERTION_FLUSH_TIMEOUT_MS 1000U

/* 单条断言文本的最大长度（含结尾 '\0'）。 */
#define ASSERTION_TEXT_CAPACITY 192U

/* ── ① 64 位十进制打印 ──
 * newlib-nano 不支持 "%ll"（静默打成字面量，GCC 不警告），故不走 printf 家族。 */

static void assertion_u64_to_decimal(uint64_t value, char *out, size_t out_size)
{
    char   digits[20]; /* uint64 十进制最多 20 位 */
    size_t count = 0U;
    size_t i;

    do
    {
        digits[count++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    } while ((value != 0U) && (count < sizeof(digits)));

    if (count > (out_size - 1U))
    {
        count = out_size - 1U;
    }
    for (i = 0U; i < count; i++)
    {
        out[i] = digits[count - 1U - i];
    }
    out[count] = '\0';
}

static void assertion_i64_to_decimal(int64_t value, char *out, size_t out_size)
{
    if (value < 0)
    {
        /* 取模转换避免 INT64_MIN 取负的 UB。 */
        out[0] = '-';
        assertion_u64_to_decimal((uint64_t)0 - (uint64_t)value, out + 1, out_size - 1U);
        return;
    }

    assertion_u64_to_decimal((uint64_t)value, out, out_size);
}

/* ── ② 公共终端：落记录 → 日志 → 停机 ── */

static _Noreturn void assertion_report_and_terminate(const char *text, const char *file, int line)
{
    crash_record_assert(text, file, (uint32_t)line);
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

static _Noreturn void assertion_fail_64(const char *expression_a, const char *op, const char *expression_b, const char *a_text, const char *b_text, const char *file, int line)
{
    char buffer[ASSERTION_TEXT_CAPACITY];

    (void)snprintf(buffer, sizeof(buffer), "%s %s %s (%s %s %s)", expression_a, op, expression_b, a_text, op, b_text);

    assertion_report_and_terminate(buffer, file, line);
}

_Noreturn void assertion_fail_i64(const char *expression_a, const char *op, const char *expression_b, int64_t a, int64_t b, const char *file, int line)
{
    char a_text[24];
    char b_text[24];

    assertion_i64_to_decimal(a, a_text, sizeof(a_text));
    assertion_i64_to_decimal(b, b_text, sizeof(b_text));

    assertion_fail_64(expression_a, op, expression_b, a_text, b_text, file, line);
}

_Noreturn void assertion_fail_u64(const char *expression_a, const char *op, const char *expression_b, uint64_t a, uint64_t b, const char *file, int line)
{
    char a_text[24];
    char b_text[24];

    assertion_u64_to_decimal(a, a_text, sizeof(a_text));
    assertion_u64_to_decimal(b, b_text, sizeof(b_text));

    assertion_fail_64(expression_a, op, expression_b, a_text, b_text, file, line);
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
