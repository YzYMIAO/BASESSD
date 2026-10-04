#ifndef ASSERTION_H
#define ASSERTION_H

/*
 * assertion.h — 全项目唯一的断言设施（生产断言 + 富断言集）
 *
 * 职责分层：
 *   ① 生产断言   ASSERT / ASSUME —— 不变量检查；NDEBUG 下分别裁剪为空 / 不可达提示
 *   ② 富断言集   ASSERT_*        —— 类型化比较、指针、字符串、内存，供测试文件使用
 *   ③ 失败入口   assertion_fail / assertion_failf —— 格式化 + 落记录 + 停机
 *
 * 依赖边界：本头只依赖 C 标准头（stdint/string/inttypes），**不**引入 log.h、
 * 不引入任何 munit/FC/业务头。格式化在 .c 里完成，宏只负责取值与比较。
 *
 * 与 munit 的关系：本设施与 tests/framework/munit.c/.h 双向无牵扯——munit 回归
 * 上游后只调 abort()，由 assertion.c 里的 abort() 重定向承接终端路径。
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__MINGW32__)
#    define ASSERTION_PRINTF(string_index, first_to_check) \
        __attribute__((format(printf, string_index, first_to_check)))
#else
#    define ASSERTION_PRINTF(string_index, first_to_check)
#endif

/* ── 分支方向提示 / 不可达 ── */

#if defined(__GNUC__)
#    define ASSERT_LIKELY(expression)   (__builtin_expect(!!(expression), 1))
#    define ASSERT_UNLIKELY(expression) (__builtin_expect(!!(expression), 0))
#    define ASSERT_UNREACHABLE()        __builtin_unreachable()
#else
#    define ASSERT_LIKELY(expression)   (expression)
#    define ASSERT_UNLIKELY(expression) (expression)
#    define ASSERT_UNREACHABLE()        ((void)0)
#endif

/* ── 失败入口（均不返回：落崩溃记录 → 日志 → 停机） ── */

_Noreturn void assertion_fail(const char *expression, const char *file, int line);

ASSERTION_PRINTF(3, 4)
_Noreturn void assertion_failf(const char *file, int line, const char *format, ...);

/*
 * 64 位专用入口。newlib-nano 不支持 "%ll"（静默打成字面量，GCC 不警告），
 * 因此 64 位变体不走 printf 家族，由 assertion.c 里的十进制拆解打印。
 */
_Noreturn void assertion_fail_i64(const char *expression_a, const char *op, const char *expression_b, int64_t a, int64_t b, const char *file, int line);
_Noreturn void assertion_fail_u64(const char *expression_a, const char *op, const char *expression_b, uint64_t a, uint64_t b, const char *file, int line);

/* ── 终端路径：flush → 平台上下文 → 崩溃记录 dump → 停机 ── */

_Noreturn void assertion_terminate(void);

/*
 * 平台上下文弱钩子：断言停机前追加打印的现场信息（如 FC 资源占用）。
 * 默认空实现；由需要的一方（如 src/fc/fc_assert_context.c）强符号覆盖。
 * 注意它与 crash_platform_report 是两个不同的钩子，不要合并。
 */
void assertion_platform_context(void);

/* ── 生产断言 ── */

#if defined(NDEBUG)
/* release：ASSERT 完全裁剪；ASSUME 只把假设告诉编译器，不产生运行时代码。 */
#    define ASSERT(expression) ((void)0)
#    define ASSUME(expression) ASSERT_UNREACHABLE()
#else
#    define ASSERT(expression)                                   \
        do                                                       \
        {                                                        \
            if (ASSERT_UNLIKELY(!(expression)))                  \
            {                                                    \
                assertion_fail(#expression, __FILE__, __LINE__); \
            }                                                    \
        } while (0)

/* debug：ASSUME 与 ASSERT 同语义（检查 + 停机），release 下退化为不可达。 */
#    define ASSUME(expression) ASSERT(expression)
#endif

/*
 * ── 富断言集（重写自 munit，供测试文件使用；不随 NDEBUG 裁剪） ──
 *
 * 说明：这套宏是测试语义——断言永远生效，失败即停机并保留现场。生产代码请用
 * ASSERT/ASSUME。刻意不提供 llong/ullong（"%ll" 在本平台不可用），64 位一律走
 * ASSERT_INT64 / ASSERT_UINT64。
 *
 * 浮点断言（ASSERT_FLOAT / ASSERT_DOUBLE / ASSERT_DOUBLE_EQUAL）依赖 printf 浮点
 * 支持；本工程未链接 _printf_float，故仅在明确启用浮点打印的构建里可用。
 */

#define ASSERT_TRUE(expression)                                                 \
    do                                                                          \
    {                                                                           \
        if (ASSERT_UNLIKELY(!(expression)))                                     \
        {                                                                       \
            assertion_failf(__FILE__, __LINE__, "%s is not true", #expression); \
        }                                                                       \
    } while (0)

#define ASSERT_FALSE(expression)                                                 \
    do                                                                           \
    {                                                                            \
        if (ASSERT_UNLIKELY(!!(expression)))                                     \
        {                                                                        \
            assertion_failf(__FILE__, __LINE__, "%s is not false", #expression); \
        }                                                                        \
    } while (0)

/* 类型化比较：T 为取值类型，fmt 为 printf 格式；prefix/suffix 用于字符等特殊展示。 */
#define ASSERT_TYPE_FULL(prefix, suffix, T, fmt, a, op, b)                                                                                                                  \
    do                                                                                                                                                                      \
    {                                                                                                                                                                       \
        T assertion_tmp_a_ = (a);                                                                                                                                           \
        T assertion_tmp_b_ = (b);                                                                                                                                           \
        if (ASSERT_UNLIKELY(!(assertion_tmp_a_ op assertion_tmp_b_)))                                                                                                       \
        {                                                                                                                                                                   \
            assertion_failf(__FILE__, __LINE__, "%s %s %s (" prefix "%" fmt suffix " %s " prefix "%" fmt suffix ")", #a, #op, #b, assertion_tmp_a_, #op, assertion_tmp_b_); \
        }                                                                                                                                                                   \
    } while (0)

#define ASSERT_TYPE(T, fmt, a, op, b) \
    ASSERT_TYPE_FULL("", "", T, fmt, a, op, b)

#define ASSERT_CHAR(a, op, b)   ASSERT_TYPE_FULL("'\\x", "'", char, "02hhx", a, op, b)
#define ASSERT_UCHAR(a, op, b)  ASSERT_TYPE_FULL("'\\x", "'", unsigned char, "02hhx", a, op, b)
#define ASSERT_SHORT(a, op, b)  ASSERT_TYPE(short, "hd", a, op, b)
#define ASSERT_USHORT(a, op, b) ASSERT_TYPE(unsigned short, "hu", a, op, b)
#define ASSERT_INT(a, op, b)    ASSERT_TYPE(int, "d", a, op, b)
#define ASSERT_UINT(a, op, b)   ASSERT_TYPE(unsigned int, "u", a, op, b)
#define ASSERT_LONG(a, op, b)   ASSERT_TYPE(long int, "ld", a, op, b)
#define ASSERT_ULONG(a, op, b)  ASSERT_TYPE(unsigned long int, "lu", a, op, b)
#define ASSERT_SIZE(a, op, b)   ASSERT_TYPE(size_t, "zu", a, op, b)

#define ASSERT_INT8(a, op, b)   ASSERT_TYPE(int8_t, PRIi8, a, op, b)
#define ASSERT_UINT8(a, op, b)  ASSERT_TYPE(uint8_t, PRIu8, a, op, b)
#define ASSERT_INT16(a, op, b)  ASSERT_TYPE(int16_t, PRIi16, a, op, b)
#define ASSERT_UINT16(a, op, b) ASSERT_TYPE(uint16_t, PRIu16, a, op, b)
#define ASSERT_INT32(a, op, b)  ASSERT_TYPE(int32_t, PRIi32, a, op, b)
#define ASSERT_UINT32(a, op, b) ASSERT_TYPE(uint32_t, PRIu32, a, op, b)

#define ASSERT_INT64(a, op, b)                                                                       \
    do                                                                                               \
    {                                                                                                \
        int64_t assertion_tmp_a_ = (a);                                                              \
        int64_t assertion_tmp_b_ = (b);                                                              \
        if (ASSERT_UNLIKELY(!(assertion_tmp_a_ op assertion_tmp_b_)))                                \
        {                                                                                            \
            assertion_fail_i64(#a, #op, #b, assertion_tmp_a_, assertion_tmp_b_, __FILE__, __LINE__); \
        }                                                                                            \
    } while (0)

#define ASSERT_UINT64(a, op, b)                                                                      \
    do                                                                                               \
    {                                                                                                \
        uint64_t assertion_tmp_a_ = (a);                                                             \
        uint64_t assertion_tmp_b_ = (b);                                                             \
        if (ASSERT_UNLIKELY(!(assertion_tmp_a_ op assertion_tmp_b_)))                                \
        {                                                                                            \
            assertion_fail_u64(#a, #op, #b, assertion_tmp_a_, assertion_tmp_b_, __FILE__, __LINE__); \
        }                                                                                            \
    } while (0)

#define ASSERT_FLOAT(a, op, b)  ASSERT_TYPE(float, "f", a, op, b)
#define ASSERT_DOUBLE(a, op, b) ASSERT_TYPE(double, "g", a, op, b)

#define ASSERT_PTR(a, op, b)       ASSERT_TYPE(const void *, "p", a, op, b)
#define ASSERT_PTR_EQUAL(a, b)     ASSERT_PTR(a, ==, b)
#define ASSERT_PTR_NOT_EQUAL(a, b) ASSERT_PTR(a, !=, b)
#define ASSERT_NULL(ptr)           ASSERT_PTR(ptr, ==, NULL)
#define ASSERT_NOT_NULL(ptr)       ASSERT_PTR(ptr, !=, NULL)

#define ASSERT_DOUBLE_EQUAL(a, b, precision)                                                                                                        \
    do                                                                                                                                              \
    {                                                                                                                                               \
        const double assertion_tmp_a_    = (a);                                                                                                     \
        const double assertion_tmp_b_    = (b);                                                                                                     \
        const double assertion_tmp_diff_ = ((assertion_tmp_a_ - assertion_tmp_b_) < 0)                                                              \
                                               ? -(assertion_tmp_a_ - assertion_tmp_b_)                                                             \
                                               : (assertion_tmp_a_ - assertion_tmp_b_);                                                             \
        if (ASSERT_UNLIKELY(assertion_tmp_diff_ > 1e-##precision))                                                                                  \
        {                                                                                                                                           \
            assertion_failf(__FILE__, __LINE__, "%s == %s (%0." #precision "g == %0." #precision "g)", #a, #b, assertion_tmp_a_, assertion_tmp_b_); \
        }                                                                                                                                           \
    } while (0)

#define ASSERT_STRING_EQUAL(a, b)                                                                                                  \
    do                                                                                                                             \
    {                                                                                                                              \
        const char *assertion_tmp_a_ = (a);                                                                                        \
        const char *assertion_tmp_b_ = (b);                                                                                        \
        if (ASSERT_UNLIKELY(strcmp(assertion_tmp_a_, assertion_tmp_b_) != 0))                                                      \
        {                                                                                                                          \
            assertion_failf(__FILE__, __LINE__, "string %s == %s (\"%s\" == \"%s\")", #a, #b, assertion_tmp_a_, assertion_tmp_b_); \
        }                                                                                                                          \
    } while (0)

#define ASSERT_STRING_NOT_EQUAL(a, b)                                                                                              \
    do                                                                                                                             \
    {                                                                                                                              \
        const char *assertion_tmp_a_ = (a);                                                                                        \
        const char *assertion_tmp_b_ = (b);                                                                                        \
        if (ASSERT_UNLIKELY(strcmp(assertion_tmp_a_, assertion_tmp_b_) == 0))                                                      \
        {                                                                                                                          \
            assertion_failf(__FILE__, __LINE__, "string %s != %s (\"%s\" == \"%s\")", #a, #b, assertion_tmp_a_, assertion_tmp_b_); \
        }                                                                                                                          \
    } while (0)

#define ASSERT_MEMORY_EQUAL(size, a, b)                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        const unsigned char *assertion_tmp_a_    = (const unsigned char *)(a);                                         \
        const unsigned char *assertion_tmp_b_    = (const unsigned char *)(b);                                         \
        const size_t         assertion_tmp_size_ = (size);                                                             \
        if (ASSERT_UNLIKELY(memcmp(assertion_tmp_a_, assertion_tmp_b_, assertion_tmp_size_) != 0))                     \
        {                                                                                                              \
            size_t assertion_tmp_pos_;                                                                                 \
            for (assertion_tmp_pos_ = 0; assertion_tmp_pos_ < assertion_tmp_size_; assertion_tmp_pos_++)               \
            {                                                                                                          \
                if (assertion_tmp_a_[assertion_tmp_pos_] != assertion_tmp_b_[assertion_tmp_pos_])                      \
                {                                                                                                      \
                    assertion_failf(__FILE__, __LINE__, "memory %s == %s, at offset %zu", #a, #b, assertion_tmp_pos_); \
                    break;                                                                                             \
                }                                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ASSERT_MEMORY_NOT_EQUAL(size, a, b)                                                                  \
    do                                                                                                       \
    {                                                                                                        \
        const unsigned char *assertion_tmp_a_    = (const unsigned char *)(a);                               \
        const unsigned char *assertion_tmp_b_    = (const unsigned char *)(b);                               \
        const size_t         assertion_tmp_size_ = (size);                                                   \
        if (ASSERT_UNLIKELY(memcmp(assertion_tmp_a_, assertion_tmp_b_, assertion_tmp_size_) == 0))           \
        {                                                                                                    \
            assertion_failf(__FILE__, __LINE__, "memory %s != %s (%zu bytes)", #a, #b, assertion_tmp_size_); \
        }                                                                                                    \
    } while (0)

#endif /* ASSERTION_H */
