/*
 * assertion.c — 断言设施实现（生产断言 + 富断言集的失败后端）
 *
 * 文件结构：
 *   ① 常量
 *   ② assertion_report_and_terminate —— 落记录 + 日志 + 停机
 *   ③ 失败入口 assertion_fail / assertion_failf / _i64 / _u64
 *   ④ assertion_terminate —— flush → 平台上下文 → 断言现场终端
 *   ⑤ 平台上下文弱钩子
 *   ⑥ 固件 abort() 重定向
 *
 * 与 CPU 异常的区别：ASSERT / abort 是软件检测到的失败，不接管故障控制台，
 * 也不复位；它保留当前调用栈，在正常 UART 上进入 CLI 循环让用户分析现场。
 * HardFault/MemManage/BusFault/UsageFault 走 platform/fault.c，dump 后停机。
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

static _Noreturn void assertion_report_and_terminate(const char *text, const char *function, const char *file, int line)
{
    crash_record_assert(text, file, line);

    /* 断言文本必须无条件可见：测试镜像默认把项目日志设在 LOG_OFF，
     * 若只走 LOGE 会被静默过滤，现场只剩一行 console marker。 */
    log_set_level(LOG_INFO);
    LOGE("assertion failed: %s (%s @ %s:%d)\r\n", text, function, file, line);
    assertion_terminate();
}

/* ── ③ 失败入口 ── */

_Noreturn void assertion_fail(const char *expression, const char *function, const char *file, int line)
{
    assertion_report_and_terminate(expression, function, file, line);
}

_Noreturn void assertion_failf(const char *function, const char *file, int line, const char *format, ...)
{
    char    buffer[ASSERTION_TEXT_CAPACITY];
    va_list ap;

    va_start(ap, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    assertion_report_and_terminate(buffer, function, file, line);
}

_Noreturn void assertion_fail_i64(const char *expression_a, const char *op, const char *expression_b, int64_t a, int64_t b, const char *function, const char *file, int line)
{
    assertion_failf(function, file, line, "%s %s %s (%lld %s %lld)", expression_a, op, expression_b, a, op, b);
}

_Noreturn void assertion_fail_u64(const char *expression_a, const char *op, const char *expression_b, QWORD a, QWORD b, const char *function, const char *file, int line)
{
    assertion_failf(function, file, line, "%s %s %s (%llu %s %llu)", expression_a, op, expression_b, a, op, b);
}

/* ── ④ 终端路径 ── */

/* 由 FTL 强符号覆盖；此处先声明，避免调用点隐式声明。 */
__attribute__((weak)) void ftl_assert_context(void);

/* 无 HIL CLI 的镜像（例如最小化 host 构建）只保留现场；有 CLI 时由 cli.c
 * 提供强符号，在调用者栈上循环解析命令。 */
__attribute__((weak)) _Noreturn void assertion_console_run(void)
{
    for (;;)
    {
    }
}

_Noreturn void assertion_terminate(void)
{
    assertion_platform_context();
    ftl_assert_context();
    (void)log_flush(ASSERTION_FLUSH_TIMEOUT_MS);

#if defined(SSD_FIRMWARE_BUILD)
    /* 不接管故障控制台（fault_port_console_init）：那是 CPU 异常路径。断言仍在
     * 正常日志/CLI 环境中，保留调用栈让串口和调试器都能看现场。 */
    assertion_console_run();
#else
    abort();
#endif
}

/* ── ⑤ 平台上下文弱钩子（默认空；由 FC 等模块强符号覆盖） ── */

__attribute__((weak)) void assertion_platform_context(void)
{
}

/* FTL 现场弱钩子：固件链接 ftl.c 时由强符号覆盖；无 FTL 的构建保持空实现。 */
__attribute__((weak)) void ftl_assert_context(void)
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
    log_set_level(LOG_INFO);
    LOGE("abort() called\r\n");
    assertion_terminate();
}
#endif
