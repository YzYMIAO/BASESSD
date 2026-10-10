#ifndef MUNIT_H_
#define MUNIT_H_
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if !defined(MUNIT_OUTPUT_FILE)
#    define MUNIT_OUTPUT_FILE stdout
#endif

#if !defined(UNIT_MAX)
#    define UNIT_MAX 1000
#endif

#define MUNIT_VERSION(major, minor, revision) ((major) << 16 | (minor) << 8 | (revision))
#define MUNIT_CURRENT_VERSION                 MUNIT_VERSION(0, 4, 1)

/*
 * Do not define MUNIT_ENABLE_ASSERT_ALIASES here. Production and ordinary unit
 * tests may use the standard <assert.h>; globally redefining assert causes test
 * framework behavior to leak into unrelated code. Suites opting in may define
 * it before including this header, but new tests should prefer munit_assert_*.
 */

#define MUNIT_TEST_NAME_LEN 55

/**************************************************************************
 *                            断言宏定义
 ***************************************************************************/
typedef enum Munit_LOG_Level_e
{
    MUNIT_LOG_DEBUG,
    MUNIT_LOG_INFO,
    MUNIT_LOG_WARNING,
    MUNIT_LOG_ERROR
} MunitLogLevel;

#if defined(__GNUC__) && !defined(__MINGW32__)
#    define MUNIT_PRINTF(string_index, first_to_check) \
        __attribute__((format(printf, string_index, first_to_check)))
#else
#    define MUNIT_PRINTF(string_index, first_to_check)
#endif

MUNIT_PRINTF(4, 5)
/* 声明一个函数。参数为日志等级 level、文件名、行号和格式字符串 */
void munit_logf_ex(MunitLogLevel level, const char *filename, int line, const char *format, ...);

/* 对上面函数进行封装，自动传入当前文件名和行号 */
#define munit_logf(level, format, ...) \
    munit_logf_ex(level, __FILE__, __LINE__, format, __VA_ARGS__)

/* 该等级当前是否会被 munit_logf 输出（即 >= 当前的 --log-visible）。
 * 供测试夹具复用同一个开关来打印自己的输出，而不必走 munit_logf——
 * munit_logf 会给每行都加 "Info:文件:行号" 前缀，逐页明细会被撑得过长。 */
bool munit_log_visible(MunitLogLevel level);

/* 构建一个只打印指定等级和字符串信息的日志函数 */
#define munit_log(level, msg) \
    munit_logf(level, "%s", msg)

MUNIT_PRINTF(3, 4)
void munit_errorf_ex(const char *filename, int line, const char *format, ...);

#define munit_errorf(format, ...) \
    munit_errorf_ex(__FILE__, __LINE__, format, __VA_ARGS__)
#define munit_error(msg) \
    munit_errorf("%s", msg)

#if defined(__GNUC__)
#    define MUNIT_LIKELY(expr)   (__builtin_expect((expr), 1))
#    define MUNIT_UNLIKELY(expr) (__builtin_expect((expr), 0))
#    define MUNIT_UNUSED         __attribute__((__unused__))
#else
#    define MUNIT_LIKELY(expr)   (expr)
#    define MUNIT_UNLIKELY(expr) (expr)
#    define MUNIT_UNUSED
#endif

#define munit_assert(expr)                          \
    do                                              \
    {                                               \
        if (!MUNIT_LIKELY(expr))                    \
        {                                           \
            munit_error("assertion failed:" #expr); \
        }                                           \
    } while (0)

#define munit_assert_true(expr)                                   \
    do                                                            \
    {                                                             \
        if (!MUNIT_LIKELY(expr))                                  \
        {                                                         \
            munit_error("assertion failed:" #expr "is not true"); \
        }                                                         \
    } while (0)

#define munit_assert_false(expr)                                   \
    do                                                             \
    {                                                              \
        if (!MUNIT_LIKELY(!(expr)))                                \
        {                                                          \
            munit_error("assertion failed:" #expr "is not false"); \
        }                                                          \
    } while (0)

/*
 * 格式串限制（newlib-nano）：本文件的断言格式串只允许 nano 的 vfprintf 认得的写法。
 * nano-vfprintf.c 的长度修饰符集合是 "hlL"，C99 的 hh / ll / j / z / t 一律不认——
 * 遇到时它把修饰符当普通字符打出、并错位后续可变参数（表现为二进制乱码），
 * 而 GCC 不会告警。支持的转换字符：c d i n o p s u x X（浮点转换 f/e/g/a 另在
 * nano-vfprintf_float.o，由 config.mk 的 -Wl,--undefined=_printf_float 拉起）。
 *
 * 目标平台（arm-none-eabi / stm32h743）的实际类型（已用 _Generic 实测）：
 *   char                -> 无符号（ARM EABI 默认），可变参数提升为 int
 *   int8_t / uint8_t    -> signed char / unsigned char，提升为 int
 *   int16_t / uint16_t  -> short / unsigned short，提升为 int
 *   int32_t / uint32_t  -> long / unsigned long（newlib 如此定义，**不是 int**）
 *   int64_t / uint64_t  -> long long / unsigned long long
 *   size_t              -> unsigned int
 * 8/16 位提升后数值不变，故去掉 hh / h 直接用 %d/%i/%u/%x；
 * 32 位对应 long，用 %ld/%li/%lu；64 位 nano 打不了，走下面的十进制转换函数 + %s。
 */

/* 64 位整数转十进制字符串（nano 无 %ll）。buf 建议 >= MUNIT_I64_TEXT_SIZE，返回 buf。 */
#define MUNIT_I64_TEXT_SIZE 24U
char *munit_u64_to_str(uint64_t value, char *buf, size_t size);
char *munit_i64_to_str(int64_t value, char *buf, size_t size);

/*
 * 通用类型断言宏，用于比较两个值的大小关系
 * prefix/suffix: 用于格式化输出时的前后缀
 * T: 类型
 * fmt: printf 格式字符串
 * a, b: 要比较的值
 * op: 比较运算符
 */

#define munit_assert_type_full(prefix, suffix, T, fmt, a, op, b)                                              \
    do                                                                                                        \
    {                                                                                                         \
        T munit_tmp_a_ = (a);                                                                                 \
        T munit_tmp_b_ = (b);                                                                                 \
        if (!(munit_tmp_a_ op munit_tmp_b_))                                                                  \
        {                                                                                                     \
            munit_errorf("assertion failed:%s %s %s(" prefix "%" fmt suffix " %s " prefix "%" fmt suffix ")", \
                         #a,                                                                                  \
                         #op,                                                                                 \
                         #b,                                                                                  \
                         munit_tmp_a_,                                                                        \
                         #op,                                                                                 \
                         munit_tmp_b_);                                                                       \
        }                                                                                                     \
    } while (0)

#define munit_assert_type(T, fmt, a, op, b) \
    munit_assert_type_full("", "", T, fmt, a, op, b)

// 打印示例：assertion failed: c == 'a' ('\x78' == '\x61')
// 本平台 char 无符号，提升为 int 后值域 0..255，%02x 恰好两位十六进制。
#define munit_assert_char(a, op, b) \
    munit_assert_type_full("'\\x", "'", char, "02x", a, op, b)

// 打印示例：assertion failed: uc == 0x00 ('\xff' == '\x00')
#define munit_assert_uchar(a, op, b) \
    munit_assert_type_full("'\\x", "'", unsigned char, "02x", a, op, b)

// 打印示例：assertion failed: s == 100 (42 == 100)
#define munit_assert_short(a, op, b) \
    munit_assert_type(short, "d", a, op, b)

// 打印示例：assertion failed: us == 65535 (42 == 65535)
#define munit_assert_ushort(a, op, b) \
    munit_assert_type(unsigned short, "u", a, op, b)

// 打印示例：assertion failed: result == 5 (10 == 5)
#define munit_assert_int(a, op, b) \
    munit_assert_type(int, "d", a, op, b)

// 打印示例：assertion failed: count == 100 (42 == 100)
#define munit_assert_uint(a, op, b) \
    munit_assert_type(unsigned int, "u", a, op, b)

// 打印示例：assertion failed: value == 100 (42 == 100)
#define munit_assert_long(a, op, b) \
    munit_assert_type(long int, "ld", a, op, b)

// 打印示例：assertion failed: ul == 42 (100 == 42)
#define munit_assert_ulong(a, op, b) \
    munit_assert_type(unsigned long int, "lu", a, op, b)

/*
 * 64 位断言：nano 无 %ll，故先把两个操作数转成十进制字符串再以 %s 打印。
 * to_str 取 munit_i64_to_str / munit_u64_to_str。
 */
#define munit_assert_wide(T, to_str, a, op, b)                                      \
    do                                                                              \
    {                                                                               \
        T munit_tmp_a_ = (a);                                                       \
        T munit_tmp_b_ = (b);                                                       \
        if (!(munit_tmp_a_ op munit_tmp_b_))                                        \
        {                                                                           \
            char munit_str_a_[MUNIT_I64_TEXT_SIZE];                                 \
            char munit_str_b_[MUNIT_I64_TEXT_SIZE];                                 \
            munit_errorf("assertion failed:%s %s %s(%s %s %s)",                     \
                         #a,                                                        \
                         #op,                                                       \
                         #b,                                                        \
                         to_str(munit_tmp_a_, munit_str_a_, sizeof(munit_str_a_)),  \
                         #op,                                                       \
                         to_str(munit_tmp_b_, munit_str_b_, sizeof(munit_str_b_))); \
        }                                                                           \
    } while (0)

// 打印示例：assertion failed: ll == 100 (42 == 100)
#define munit_assert_llong(a, op, b) \
    munit_assert_wide(long long int, munit_i64_to_str, a, op, b)

// 打印示例：assertion failed: ull == 42 (100 == 42)
#define munit_assert_ullong(a, op, b) \
    munit_assert_wide(unsigned long long int, munit_u64_to_str, a, op, b)

// 打印示例：assertion failed: len == 10 (5 == 10)
// 本平台 size_t 即 unsigned int。
#define munit_assert_size(a, op, b) \
    munit_assert_type(size_t, "u", a, op, b)

// 打印示例：assertion failed: pi == 3.14 (3.14 == 3.14159)
// ⚠ %f/%g 的实现在 nano-vfprintf_float.o，默认不被链接；本工程靠 config.mk 的
//   -Wl,--undefined=_printf_float 拉起。去掉该选项 ⇒ 浮点断言值打印成空串（不报错、不乱码）。
#define munit_assert_float(a, op, b) \
    munit_assert_type(float, "f", a, op, b)

// 打印示例：assertion failed: pi == 3.14 (3.14 == 3.141592653589793)
#define munit_assert_double(a, op, b) \
    munit_assert_type(double, "g", a, op, b)

// 打印示例：assertion failed: ptr == NULL (0x7fff == 0x0)
#define munit_assert_ptr(a, op, b) \
    munit_assert_type(const void *, "p", a, op, b)

// 打印示例：assertion failed: i8 == 100 (42 == 100)
#define munit_assert_int8(a, op, b) \
    munit_assert_type(int8_t, "i", a, op, b)

// 打印示例：assertion failed: u8 == 255 (42 == 255)
#define munit_assert_uint8(a, op, b) \
    munit_assert_type(uint8_t, "u", a, op, b)

// 打印示例：assertion failed: i16 == 10000 (42 == 10000)
#define munit_assert_int16(a, op, b) \
    munit_assert_type(int16_t, "i", a, op, b)

// 打印示例：assertion failed: u16 == 65535 (42 == 65535)
#define munit_assert_uint16(a, op, b) \
    munit_assert_type(uint16_t, "u", a, op, b)

// 打印示例：assertion failed: i32 == 100000 (42 == 100000)
// 本平台 int32_t 即 long（不是 int），故用 %li/%lu。
#define munit_assert_int32(a, op, b) \
    munit_assert_type(int32_t, "li", a, op, b)

// 打印示例：assertion failed: u32 == 4294967295 (42 == 4294967295)
#define munit_assert_uint32(a, op, b) \
    munit_assert_type(uint32_t, "lu", a, op, b)

// 打印示例：assertion failed: i64 == 10000000000 (42 == 10000000000)
#define munit_assert_int64(a, op, b) \
    munit_assert_wide(int64_t, munit_i64_to_str, a, op, b)

// 打印示例：assertion failed: u64 == 18446744073709551615 (42 == 18446744073709551615)
#define munit_assert_uint64(a, op, b) \
    munit_assert_wide(uint64_t, munit_u64_to_str, a, op, b)

/* 判断两个 double 值在给定精度内是否相等 */
//	assertion failed: pi == expected (3.141592654 == 3.141592653)
// ⚠ 同 munit_assert_float：%g 的实现在 nano-vfprintf_float.o，靠 config.mk 的
//   -Wl,--undefined=_printf_float 拉起。

#define munit_assert_double_equal(a, b, precision)                                                                                          \
    do                                                                                                                                      \
    {                                                                                                                                       \
        const double munit_tmp_a_   = (a);                                                                                                  \
        const double munit_tmp_b_   = (b);                                                                                                  \
        const double munit_tmp_diff = ((munit_tmp_a_ - munit_tmp_b_) < 0) ? -(munit_tmp_a_ - munit_tmp_b_) : (munit_tmp_a_ - munit_tmp_b_); \
        if (MUNIT_UNLIKELY(munit_tmp_diff > 1e-##precision))                                                                                \
        {                                                                                                                                   \
            munit_errorf("assertion failed: %s==%s (%0." #precision "g==%0." #precision "g)",                                               \
                         #a,                                                                                                                \
                         #b,                                                                                                                \
                         munit_tmp_a_,                                                                                                      \
                         munit_tmp_b_);                                                                                                     \
        }                                                                                                                                   \
    } while (0)

#define munit_assert_string_equal(a, b)                                        \
    do                                                                         \
    {                                                                          \
        const char *munit_tmp_a_ = (a);                                        \
        const char *munit_tmp_b_ = (b);                                        \
        if (MUNIT_UNLIKELY(strcmp(munit_tmp_a_, munit_tmp_b_) != 0))           \
        {                                                                      \
            munit_errorf("assertion failed: string %s == %s (\"%s\"==\"%s\")", \
                         #a,                                                   \
                         #b,                                                   \
                         munit_tmp_a_,                                         \
                         munit_tmp_b_);                                        \
        }                                                                      \
    } while (0)

#define munit_assert_string_not_equal(a, b)                                    \
    do                                                                         \
    {                                                                          \
        const char *munit_tmp_a_ = (a);                                        \
        const char *munit_tmp_b_ = (b);                                        \
        if (MUNIT_UNLIKELY(strcmp(munit_tmp_a_, munit_tmp_b_) == 0))           \
        {                                                                      \
            munit_errorf("assertion failed: string %s != %s (\"%s\"==\"%s\")", \
                         #a,                                                   \
                         #b,                                                   \
                         munit_tmp_a_,                                         \
                         munit_tmp_b_);                                        \
        }                                                                      \
    } while (0)

/* 内存块断言宏 */
// assertion failed: memory expected == actual, at offset 3
#define munit_assert_memory_equal(size, a, b)                                        \
    do                                                                               \
    {                                                                                \
        const unsigned char *munit_tmp_a_   = (const unsigned char *)(a);            \
        const unsigned char *munit_tmp_b_   = (const unsigned char *)(b);            \
        const size_t         munit_tmp_size = (size);                                \
        if (MUNIT_UNLIKELY(memcmp(munit_tmp_a_, munit_tmp_b_, munit_tmp_size) != 0)) \
        {                                                                            \
            size_t munit_tmp_pos;                                                    \
            for (munit_tmp_pos = 0; munit_tmp_pos < munit_tmp_size; munit_tmp_pos++) \
            {                                                                        \
                if (munit_tmp_a_[munit_tmp_pos] != munit_tmp_b_[munit_tmp_pos])      \
                {                                                                    \
                    munit_errorf("assertion failed:memory %s == %s,at offset %u",    \
                                 #a,                                                 \
                                 #b,                                                 \
                                 munit_tmp_pos);                                     \
                    break;                                                           \
                }                                                                    \
            }                                                                        \
        }                                                                            \
    } while (0)

#define munit_assert_memory_not_equal(size, a, b)                                    \
    do                                                                               \
    {                                                                                \
        const unsigned char *munit_tmp_a_   = (const unsigned char *)(a);            \
        const unsigned char *munit_tmp_b_   = (const unsigned char *)(b);            \
        const size_t         munit_tmp_size = (size);                                \
        if (MUNIT_UNLIKELY(memcmp(munit_tmp_a_, munit_tmp_b_, munit_tmp_size) == 0)) \
        {                                                                            \
            munit_errorf("assertion failed: memory %s != %s (%u bytes)",             \
                         #a,                                                         \
                         #b,                                                         \
                         munit_tmp_size);                                            \
        }                                                                            \
    } while (0)

/* 指针断言宏 */
#define munit_assert_ptr_equal(a, b) \
    munit_assert_ptr(a, ==, b)
#define munit_assert_ptr_not_equal(a, b) \
    munit_assert_ptr(a, !=, b)
#define munit_assert_null(ptr) \
    munit_assert_ptr(ptr, ==, NULL)
#define munit_assert_not_null(ptr) \
    munit_assert_ptr(ptr, !=, NULL)
#define munit_assert_ptr_null(ptr) \
    munit_assert_ptr(ptr, ==, NULL)
#define munit_assert_ptr_not_null(ptr) \
    munit_assert_ptr(ptr, !=, NULL)

/*** 内存分配 ***/

void *munit_malloc_ex(const char *filename, int line, size_t size);

#define munit_malloc(size) \
    munit_malloc_ex(__FILE__, __LINE__, (size))
#define munit_new(type) \
    ((type *)munit_malloc(sizeof(type)))

#define munit_calloc(nmemb, size) \
    munit_malloc((nmemb) * (size))

#define munit_newa(type, nmemb) \
    ((type *)munit_calloc(nmemb, sizeof(type)))

#if defined(MUNIT_ENABLE_ASSERT_ALIASES)
#    define assert_true(expr)       munit_assert_true(expr)
#    define assert_false(expr)      munit_assert_false(expr)
#    define assert_char(a, op, b)   munit_assert_char(a, op, b)
#    define assert_uchar(a, op, b)  munit_assert_uchar(a, op, b)
#    define assert_short(a, op, b)  munit_assert_short(a, op, b)
#    define assert_ushort(a, op, b) munit_assert_ushort(a, op, b)
#    define assert_int(a, op, b)    munit_assert_int(a, op, b)
#    define assert_uint(a, op, b)   munit_assert_uint(a, op, b)
#    define assert_long(a, op, b)   munit_assert_long(a, op, b)
#    define assert_ulong(a, op, b)  munit_assert_ulong(a, op, b)
#    define assert_llong(a, op, b)  munit_assert_llong(a, op, b)
#    define assert_ullong(a, op, b) munit_assert_ullong(a, op, b)
#    define assert_size(a, op, b)   munit_assert_size(a, op, b)
#    define assert_float(a, op, b)  munit_assert_float(a, op, b)
#    define assert_double(a, op, b) munit_assert_double(a, op, b)
#    define assert_ptr(a, op, b)    munit_assert_ptr(a, op, b)

#    define assert_int8(a, op, b)   munit_assert_int8(a, op, b)
#    define assert_uint8(a, op, b)  munit_assert_uint8(a, op, b)
#    define assert_int16(a, op, b)  munit_assert_int16(a, op, b)
#    define assert_uint16(a, op, b) munit_assert_uint16(a, op, b)
#    define assert_int32(a, op, b)  munit_assert_int32(a, op, b)
#    define assert_uint32(a, op, b) munit_assert_uint32(a, op, b)
#    define assert_int64(a, op, b)  munit_assert_int64(a, op, b)
#    define assert_uint64(a, op, b) munit_assert_uint64(a, op, b)

#    define assert_double_equal(a, b, precision) munit_assert_double_equal(a, b, precision)
#    define assert_string_equal(a, b)            munit_assert_string_equal(a, b)
#    define assert_string_not_equal(a, b)        munit_assert_string_not_equal(a, b)
#    define assert_memory_equal(size, a, b)      munit_assert_memory_equal(size, a, b)
#    define assert_memory_not_equal(size, a, b)  munit_assert_memory_not_equal(size, a, b)
#    define assert_ptr_equal(a, b)               munit_assert_ptr_equal(a, b)
#    define assert_ptr_not_equal(a, b)           munit_assert_ptr_not_equal(a, b)
#    define assert_ptr_null(ptr)                 munit_assert_ptr_null(ptr)
#    define assert_ptr_not_null(ptr)             munit_assert_ptr_not_null(ptr)

#    define assert_null(ptr)     munit_assert_null(ptr)
#    define assert_not_null(ptr) munit_assert_not_null(ptr)
#endif

/* 如果启用了断言别名，将标准 assert 宏也映射到 munit_assert */
#if defined(MUNIT_ENABLE_ASSERT_ALIASES)
#    if defined(assert)
#        undef assert
#    endif
#    define assert(expr) munit_assert(expr)
#endif

/**************************************************************************
 *                             核心上下文结构体定义
 ***************************************************************************/

typedef enum Munit_Result_e
{
    MUNIT_OK,   // 测试结构成功
    MUNIT_FAIL, // 测试结果失败
    MUNIT_SKIP, // 测试结果跳过
    MUNIT_ERROR // 测试因非预期的情况而失败 如网络错误、无效参数值、测试框架内存分配失败等
} MunitResult;

typedef struct Munit_Parameter_s
{
    const char *name;  // 参数名称
    const char *value; // 参数值
} MunitParameter;

/* 取参数值：按 name 在 params 里查，查不到返回 NULL。
 * 上游 munit 同名同签名（munit.h）；参数化用例用它读自己那组参数。 */
const char *munit_parameters_get(const MunitParameter params[], const char *key);

// 测试函数类型定义
typedef MunitResult (*MunitTestFunc)(const MunitParameter params[], void *user_data_or_fixture);
typedef void *(*MunitTestSetup)(const MunitParameter params[], void *user_data);
typedef void (*MunitTestTearDown)(void *fixture);

// 测试选项枚举，可以按位组合
typedef enum Munit_Test_Options_s
{
    MUNIT_TEST_OPTION_NONE             = 0,      // 无选项
    MUNIT_TEST_OPTION_SINGLE_ITERATION = 1 << 0, // 单次迭代模式
    MUNIT_TEST_OPTION_TODO             = 1 << 1  // 标记为待实现
} MunitTestOptions;

typedef struct Munit_Parameter_Enum_s
{
    const char  *name;   // 参数名称
    const char **values; // 参数的可能值列表以 NULL结尾
} MunitParameterEnum;

typedef struct Munit_Test_s
{
    const char         *name;       // 测试名称
    MunitTestFunc       test;       // 测试函数指针
    MunitTestSetup      setup;      // 测试前设置函数，如资源的分配和初始化
    MunitTestTearDown   tear_down;  // 测试后清理函数，如内存释放和资源回收
    MunitTestOptions    option;     // 测试选项（单次迭代、TODO等），按位开启
    MunitParameterEnum *parameters; // 参数化测试参数，让同一个测试函数用不同的参数运行多次
} MunitTest;

typedef enum Munit_Suite_Option_e
{
    MUNIT_SUITE_OPTION_NONE = 0, // 不启用任何选项
} MunitSuiteOption;

// 根套件定义
typedef struct Munit_Suite_s MunitSuite;
typedef struct Munit_Suite_s
{
    const char      *prefix;     // 套件测试前缀
    MunitTest       *tests;      // 本套件的测试数组
    MunitSuite      *suites;     // 子套件数组(支持套件)
    unsigned int     iterations; // 默认迭代次数
    MunitSuiteOption options;    // 套件选项
} MunitSuite;

/*** 随机数 ***
 *
 * 与运行器共用同一份 PRNG 状态：`munit_test_runner_exec` 在**每个用例开头**
 * 用 `runner.seed` 重新播种（`munit.c`），迭代循环推进状态 ⇒ 同一个种子下，
 * 第 N 次迭代与单跑第 N 次的结果一致，用例之间互不干扰。
 *
 * 运行器启动时打印 `Running test suite with seed 0x%08x...`；失败后可用
 * `--seed 0x…` 原样重放（`--iterations N` 则重放整个 N 次序列）。
 * ⇒ 用例里的随机取值**天然可复现**，不必自己另开一路 PRNG。
 *
 * ⚠ `munit_rand_int_range` 是**闭区间**。
 * ⚠ `munit_rand_at_most` 在 munit.c 里是 static（上游如此），不对外暴露——
 *   要 [0,max] 闭区间请用 `munit_rand_int_range(0, (int)max)`。 */

/* 重新播种（运行器自己用；用例一般不需要） */
void munit_rand_seed(uint32_t seed);

/* 均匀 32 位随机数 */
uint32_t munit_rand_uint32(void);

/* 用随机字节填充 data[0..size) */
void munit_rand_memory(size_t size, uint8_t data[size]);

/* [min, max] 闭区间内的均匀随机整数（min > max 时自动交换） */
int munit_rand_int_range(int min, int max);

/* [0.0, 1.0) 内的均匀随机浮点 */
double munit_rand_double(void);

/* 前向声明：测试运行器上下文，用于裸机直接调用的入口。 */
typedef struct Munit_Test_Runner_s MunitTestRunner;

/* 测试运行器主入口（裸机入口）：绕过 munit_suite_main 的 CLI 解析，
 * 直接构建 MunitTestRunner 后调用此函数执行套件。 */
void munit_test_runner_run(MunitTestRunner *runner);

/* 标准测试入口：从 argc/argv 解析 CLI 参数，执行指定测试套件。 */
int munit_suite_main(const MunitSuite *suite, void *user_data, int argc, char *const argv[argc + 1]);

typedef struct Munit_Argument_s MunitArgument;
typedef struct Munit_Argument_s
{
    char *name;
    bool (*parse_argument)(const MunitSuite *suite, void *user_data, int *arg, int argc, char *const argv[argc + 1]);
    void (*write_help)(const MunitArgument *argument, void *user_data);
} MunitArgument;

#endif
