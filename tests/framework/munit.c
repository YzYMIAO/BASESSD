
#include "munit.h"

#include <stdio.h>
#include <time.h>

#if defined(MUNIT_ENABLE_TIMING)
#    include "sys.h"

/* 纳秒/微秒常量 */
#    define MUNIT_NS_PER_US (1000UL)
#    define MUNIT_US_PER_MS (1000UL)

/* DWT CYCCNT 读取（直接访问寄存器，不依赖 delay 模块） */
#    define MUNIT_DWT_CYCCNT (*((volatile uint32_t *)0xE0001004UL))

/*
 * munit_print_time — 打印纳秒时间值，自适应选择单位
 */
static void munit_print_time(FILE *fp, uint64_t ns)
{
    if (ns >= 1000000000ULL)
    {
        fprintf(fp, "%lu.%02lu s", (unsigned long)(ns / 1000000000ULL), (unsigned long)((ns % 1000000000ULL) / 10000000ULL));
    }
    else if (ns >= 1000000ULL)
    {
        fprintf(fp, "%lu.%02lu ms", (unsigned long)(ns / 1000000ULL), (unsigned long)((ns % 1000000ULL) / 10000ULL));
    }
    else if (ns >= 1000ULL)
    {
        fprintf(fp, "%lu.%02lu us", (unsigned long)(ns / 1000ULL), (unsigned long)((ns % 1000ULL) / 10ULL));
    }
    else
    {
        fprintf(fp, "%lu ns", (unsigned long)ns);
    }
}

/* 首次调用时使能 DWT CYCCNT（如果尚未使能） — 被 MUNIT_TIMING_INIT() 宏替代，保留仅供参考。 */
static void __attribute__((unused)) munit_timing_init(void)
{
    volatile uint32_t *dwt_ctrl   = (volatile uint32_t *)0xE0001000UL;
    volatile uint32_t *demcr      = (volatile uint32_t *)0xE000EDFCUL;
    volatile uint32_t *dwt_cyccnt = (volatile uint32_t *)0xE0001004UL;

    if ((*dwt_ctrl & 0x00000001U) == 0U)
    {
        *demcr |= 0x01000000U;
        *dwt_cyccnt = 0U;
        *dwt_ctrl |= 0x00000001U;
    }
}

/* 检查 DWT CYCCNT 是否已使能（在 main 初始化中由 sys/delay 完成） */
#    define MUNIT_TIMING_INIT() \
        do                      \
        {                       \
        } while (0)

typedef struct
{
    uint32_t start_cycles;
} munit_timing_ctx_t;

static void munit_timing_start(munit_timing_ctx_t *ctx)
{
    ctx->start_cycles = MUNIT_DWT_CYCCNT;
}

static uint64_t munit_timing_stop(const munit_timing_ctx_t *ctx)
{
    uint32_t now     = MUNIT_DWT_CYCCNT;
    uint32_t elapsed = now - ctx->start_cycles;

    /* cycles → ns = cycles * 1e9 / 400MHz（STM32H743 @400MHz） */
    uint64_t ns = (uint64_t)elapsed * 1000000000ULL;
    ns /= 400000000ULL;
    return ns;
}
#endif /* MUNIT_ENABLE_TIMING */
static MunitLogLevel munit_log_level_fatal   = MUNIT_LOG_ERROR;
static MunitLogLevel munit_log_level_visible = MUNIT_LOG_ERROR;

// 重定义输出函数
MUNIT_PRINTF(5, 0)
static void munit_logf_exv(MunitLogLevel level, FILE *fp, const char *filename, int line, const char *format, va_list ap)
{
    if (level < munit_log_level_visible)
    {
        return;
    }

    switch (level)
    {
        case MUNIT_LOG_DEBUG:
            fputs("Debug", fp);
            break;

        case MUNIT_LOG_INFO:
            fputs("Info", fp);
            break;

        case MUNIT_LOG_WARNING:
            fputs("Warning", fp);
            break;

        case MUNIT_LOG_ERROR:
            fputs("Error", fp);
            break;

        default:
            munit_logf_ex(MUNIT_LOG_ERROR, filename, line, "Invalid log level(%d)", level);
            return;
    }

    fputs(":", fp);

    if (filename != NULL)
    {
        fprintf(fp, "%s:%d", filename, line);
    }

    vfprintf(fp, format, ap);
    fputc('\n', fp);
}

void munit_logf_ex(MunitLogLevel level, const char *filename, int line, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    munit_logf_exv(level, stderr, filename, line, format, ap);
    va_end(ap);

    if (level >= munit_log_level_fatal)
    {
        abort();
    }
}

MUNIT_PRINTF(3, 4)
static void munit_logf_internal(MunitLogLevel level, FILE *fp, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    munit_logf_exv(level, fp, NULL, 0, format, ap);
    va_end(ap);
}

static void munit_log_internal(MunitLogLevel level, FILE *fp, const char *message)
{
    munit_logf_internal(level, fp, "%s", message);
}

void munit_errorf_ex(const char *filename, int line, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    munit_logf_exv(MUNIT_LOG_ERROR, stderr, filename, line, format, ap);
    va_end(ap);

    abort();
}
/*** 内存分配 ***/
/* 内存申请 */
void *munit_malloc_ex(const char *filename, int line, size_t size)
{
    void *ptr;

    if (size == 0)
    {
        return NULL;
    }

    ptr = calloc(1, size);

    if (MUNIT_UNLIKELY(ptr == NULL))
    {
        munit_logf_ex(MUNIT_LOG_ERROR, filename, line, "Failed to allocate %zu byte", size);
    }

    return ptr;
}

/**
 * 测试执行结果报告
 *
 * 本结构体用于累积和记录测试执行的统计结果，是最终输出报告的数据源。
 * 在测试执行过程中，每次测试完成都会更新相应的计数器，
 * 最终用于生成测试汇总报告。
 *
 * 字段说明：
 *	 - successful: 测试返回 MUNIT_OK 的次数
 *	 - skipped:    测试返回 MUNIT_SKIP 的次数
 *	 - failed:	   测试返回 MUNIT_FAIL 或断言失败的次数
 *	 - errored:    测试返回 MUNIT_ERROR 或子进程崩溃的次数
 *	 - cpu_clock:  所有成功测试的 CPU 时间总和（纳秒）
 *	 - wall_clock: 所有成功测试的挂钟时间总和（纳秒）
 */
typedef struct Munit_Report_s
{
    unsigned int successful;
    unsigned int failed;
    unsigned int skiped;
    unsigned int errored;
#if defined(MUNIT_ENABLE_TIMING)
    uint64_t cpu_clock;
    uint64_t wall_clock;
#endif
} MunitReport;

/**
 * 测试运行器上下文
 *
 * 本结构体是测试执行的核心上下文对象，贯穿整个测试执行过程。
 * 它存储了所有运行时配置、用户指定的测试过滤列表、PRNG 状态、
 * 以及累积的测试结果报告。
 *
 * 字段说明：
 *	 - prefix: 当前遍历到的套件路径前缀，用于构建完整测试名称
 *	 - suite: 当前执行的根测试套件指针
 *	 - tests: 用户命令行指定的测试过滤列表，不为空时只执行匹配的测试
 *	 - seed: PRNG 随机种子，用于控制测试的可重现性
 *	 - iterations: 每个测试的默认迭代次数
 *	 - parameters: 命令行通过 --param 传入的参数键值对
 *	 - single_parameter_mode: --single 模式标志，为真时只随机运行一种参数组合
 *	 - user_data: 用户自定义数据，从 munit_suite_main 传入
 *	 - report: 累积的测试结果报告
 *	 - colorize: 是否启用 ANSI 颜色输出
 *	 - fork: 是否使用 fork 隔离执行（Unix 下默认启用）
 *	 - show_stderr: 是否显示测试的 stderr 输出（即使测试成功）
 *	 - fatal_failures: 遇错即停标志，为真时遇到失败立即停止执行
 */
typedef struct Munit_Test_Runner_s
{
    const char       *prefix;
    const MunitSuite *suite;
    const char      **tests;
    uint32_t          seed;
    uint32_t          iterations;
    MunitParameter   *parameters;
    bool              single_parameter_mode;
    void             *user_data;
    MunitReport       report;
    bool              colorize;
    bool              fork;
    bool              show_stder;
    bool              fatal_failures;
} MunitTestRunner;

// 连接两个字符串，但如果其中一个为 NULL 或 ""，则仅原样返回另一个组件
static char *munit_maybe_concat(size_t *len, char *prefix, char *suffix)
{
    char  *res;
    size_t res_len;

    const size_t prefix_len = (prefix != NULL) ? strlen(prefix) : 0;
    const size_t suffix_len = (suffix != NULL) ? strlen(suffix) : 0;

    if (prefix_len == 0 && suffix_len == 0)
    {
        res     = NULL;
        res_len = 0;
    }
    else if (prefix_len != 0 && suffix_len == 0)
    {
        res     = prefix;
        res_len = prefix_len;
    }
    else if (prefix_len == 0 && suffix_len != 0)
    {
        res     = suffix;
        res_len = suffix_len;
    }
    else
    {
        res_len = prefix_len + suffix_len;
        res     = malloc(res_len + 1);
        memcpy(res, prefix, prefix_len);
        memcpy(res + prefix_len, suffix, suffix_len);
        res[res_len] = 0;
    }

    if (len != NULL)
    {
        *len = res_len;
    }

    return res;
}

void munit_maybe_free_concat(char *s, const char *prefix, const char *suffix)
{
    if (prefix != s && suffix != s)
    {
        free(s);
    }
}

#define munit_atomic_store(dest, value) \
    do                                  \
    {                                   \
        *(dest) = (value);              \
    } while (0)
#define munit_atomic_load(src) (*(src))
static inline bool munit_atomic_cas(uint32_t *dest, uint32_t *expected, uint32_t desired)
{
    if (*dest == *expected)
    {
        *dest = desired;
        return 1;
    }
    else
    {
        return 0;
    }
}

#define MUNIT_PRNG_MULTIPLIER (747796405U)
#define MUNIT_PRNG_INCREMENT  (1729U)
static uint32_t munit_rand_state = 42;

/* 简单的字符串哈希函数，仅用于为 PRNG 加盐。 */
static uint32_t
munit_str_hash(const char *name)
{
    const char *p;
    uint32_t    h = 5381U;

    for (p = name; *p != '\0'; p++)
    {
        h = (h << 5) + h + *p;
    }

    return h;
}

static uint32_t munit_rand_next_state(uint32_t state)
{
    return state * MUNIT_PRNG_MULTIPLIER + MUNIT_PRNG_INCREMENT;
}

static uint32_t munit_rand_from_state(uint32_t state)
{
    uint32_t res = ((state >> ((state >> 28) + 4)) ^ state) * (277803737U);
    res ^= res >> 22;
    return res;
}

void munit_rand_seed(uint32_t seed)
{
    uint32_t state = munit_rand_next_state(seed + MUNIT_PRNG_INCREMENT);
    munit_atomic_store(&munit_rand_state, state);
}

static uint32_t munit_rand_generate_seed(void)
{
    uint32_t seed, state;
    seed = (uint32_t)time(NULL);

    state = munit_rand_next_state(seed + MUNIT_PRNG_INCREMENT);
    return munit_rand_from_state(state);
}

static uint32_t munit_rand_state_uint32(uint32_t *state)
{
    const uint32_t old = *state;
    *state             = munit_rand_next_state(old);
    return munit_rand_from_state(old);
}

uint32_t munit_rand_uint32(void)
{
    uint32_t old, state;

    do
    {
        old   = munit_atomic_load(&munit_rand_state);
        state = munit_rand_next_state(old);
    } while (!munit_atomic_cas(&munit_rand_state, &old, state));

    return munit_rand_from_state(old);
}

static void
munit_rand_state_memory(uint32_t *state, size_t size, uint8_t data[size])
{
    size_t   members_remaining = size / sizeof(uint32_t);
    size_t   bytes_remaining   = size % sizeof(uint32_t);
    uint8_t *b                 = data;
    uint32_t rv;
    while (members_remaining-- > 0)
    {
        rv = munit_rand_state_uint32(state);
        memcpy(b, &rv, sizeof(uint32_t));
        b += sizeof(uint32_t);
    }
    if (bytes_remaining != 0)
    {
        rv = munit_rand_state_uint32(state);
        memcpy(b, &rv, bytes_remaining);
    }
}

void munit_rand_memory(size_t size, uint8_t data[size])
{
    uint32_t old, state;

    do
    {
        state = old = munit_atomic_load(&munit_rand_state);
        munit_rand_state_memory(&state, size, data);
    } while (!munit_atomic_cas(&munit_rand_state, &old, state));
}

static uint32_t
munit_rand_state_at_most(uint32_t *state, uint32_t salt, uint32_t max)
{
    /* 我们想要 (UINT32_MAX + 1) % max，在无符号算术中这等同于
     * (UINT32_MAX + 1 - max) % max = -max % max。我们使用按位取反来计算 -max
     * 以避免编译器警告。
     */
    const uint32_t min = (~max + 1U) % max;
    uint32_t       x;

    if (max == (~((uint32_t)0U)))
    {
        return munit_rand_state_uint32(state) ^ salt;
    }

    max++;

    do
    {
        x = munit_rand_state_uint32(state) ^ salt;
    } while (x < min);

    return x % max;
}

static uint32_t
munit_rand_at_most(uint32_t salt, uint32_t max)
{
    uint32_t old, state;
    uint32_t retval;

    do
    {
        state = old = munit_atomic_load(&munit_rand_state);
        retval      = munit_rand_state_at_most(&state, salt, max);
    } while (!munit_atomic_cas(&munit_rand_state, &old, state));

    return retval;
}

int munit_rand_int_range(int min, int max)
{
    uint64_t range = (uint64_t)max - (uint64_t)min;

    if (min > max)
    {
        return munit_rand_int_range(max, min);
    }

    if (range > (~((uint32_t)0U)))
    {
        range = (~((uint32_t)0U));
    }

    return min + munit_rand_at_most(0, (uint32_t)range);
}

double
munit_rand_double(void)
{
    uint32_t old, state;
    double   retval = 0.0;

    do
    {
        state = old = munit_atomic_load(&munit_rand_state);

        /* 参见 http://mumble.net/~campbell/tmp/random_real.c 了解如何
         * 正确实现。如果你觉得这太有偏差，欢迎提交补丁。 */
        retval = munit_rand_state_uint32(&state) / ((~((uint32_t)0U)) + 1.0);
    } while (!munit_atomic_cas(&munit_rand_state, &old, state));

    return retval;
}

/* 向参数数组添加一个参数。 */
static MunitResult
munit_parameters_add(size_t *params_size, MunitParameter *params[*params_size], const char *name, const char *value)
{
    *params = realloc(*params, sizeof(MunitParameter) * (*params_size + 2));
    if (*params == NULL)
    {
        return MUNIT_ERROR;
    }

    (*params)[*params_size].name  = name;
    (*params)[*params_size].value = value;
    (*params_size)++;
    (*params)[*params_size].name  = NULL;
    (*params)[*params_size].value = NULL;

    return MUNIT_OK;
}

static const MunitArgument *
munit_arguments_find(const MunitArgument arguments[], const char *name)
{
    const MunitArgument *arg;

    for (arg = arguments; arg != NULL && arg->name != NULL; arg++)
    {
        if (strcmp(arg->name, name) == 0)
        {
            return arg;
        }
    }

    return NULL;
}

static bool munit_stream_supports_ansi(FILE *stream)
{
    (void)stream;
    return 0;
}
static void munit_print_help(int argc, char *const argv[argc + 1], void *user_data, const MunitArgument arguments[])
{
    const MunitArgument *arg;

    (void)argc;

    printf("USAGE: %s [OPTIONS...] [TEST...] \n\n", argv[0]);
    puts(" --seed SEED\n"
         "			用于为PRNG的播种值。必须是一个32位整树,使用十进制\n"
         "			表示法，无分隔符(逗号、小数点、空格等),或者\n"
         "			以\"0x\"为前缀的十六进制树。\n"
         " --iterations N\n"
         "			每个测试运行N次,0表示默认次数。\n"
         " --param name value\n"
         "			一个参数键/值对，将传递给任何接受该名称参数的测试。\n"
         "			如果未提供，测试将为每个可能的参数值运行一次。\n"
         " --list	列出所有可用的测试。\n"
         " --list-params\n"
         "			列出所可用测试及其可能的参数。\n"
         " --single	以单一配置运行每个参数化测试，而不是\n"
         "			每种可能的组合。\n"
         " --log-visible debug|info|warning|error\n"
         " --log-fatal debug|info|warning|error\n"
         "			设置不同严重性消息的可见级别，\n"
         "			或导致测试终止的级别"

#if !defined(MUNIT_NO_FORK)
         " --no-fork 不在子进程中执行测试。如果提供了次选项\n"
         "			并且测试崩溃(包含因断言失败)，则不会执行进一步的测试。\n"
#endif

         " --fata-failures\n"
         "			一旦发现失败，立即停止执行测试\n"
         " --show-stderr\n"
         "			显示测试写入 stderr 的数据，即使测试成功。\n"
         " --color auto|always|never\n"

         "			对输出进行着色(或不着色)。\n" /*
                    12345678901234567890123456789012345678901234567890123456789012345678901234567890 */
         " --help	打印此帮助信息并退出\n");
    puts("munit");
    printf("%d.%d.%d\n"
           "参考项目网站： https://nemequ.github.io/munit/\n",
           (MUNIT_CURRENT_VERSION >> 16) & 0xff,
           (MUNIT_CURRENT_VERSION >> 8) & 0xff,
           (MUNIT_CURRENT_VERSION >> 0) & 0xff);

    for (arg = arguments; arg != NULL && arg->name != NULL; arg++)
    {
        arg->write_help(arg, user_data);
    }
}

static void munit_suite_list_tests(const MunitSuite *suite, bool show_params, const char *prefix)
{
    size_t                    pre_len;
    char                     *pre = munit_maybe_concat(&pre_len, (char *)prefix, (char *)suite->prefix);
    const MunitTest          *test;
    const MunitParameterEnum *params;
    bool                      first;
    const char              **val;
    const MunitSuite         *child_suite;

    for (test = suite->tests; test != NULL && test->name != NULL; test++)
    {
        if (pre != NULL)
        {
            fputs(pre, stdout);
        }

        puts(test->name);

        if (show_params)
        {
            for (params = test->parameters; params != NULL && params->name != NULL; params++)
            {
                fprintf(stdout, " - %s:", params->name);

                if (params->values == NULL)
                {
                    puts("Any");
                }
                else
                {
                    first = 1;

                    for (val = params->values; val != NULL; val++)
                    {
                        if (first == 0)
                        {
                            fputs(",", stdout);
                        }
                        else
                        {
                            first = 0;
                        }

                        fputs(*val, stdout);
                    }

                    putc('\n', stdout);
                }
            }
        }
    }

    for (child_suite = suite->suites; child_suite != NULL && child_suite->prefix != NULL; child_suite++)
    {
        munit_suite_list_tests(child_suite, show_params, pre);
    }

    munit_maybe_free_concat(pre, prefix, suite->prefix);
}

/**
 * @brief 实际执行测试函数的核心函数（在子进程或当前进程中调用）
 *
 * 本函数是 MUnit 测试执行的最底层函数，负责实际调用测试函数，
 * 并处理测试的迭代执行、计时、setup/teardown 调用等核心逻辑。
 *
 * 执行流程：
 *	 1. 确定迭代次数：
 *		- 如果测试标记了 MUNIT_TEST_OPTION_SINGLE_ITERATION，则只执行 1 次
 *		- 否则使用 runner->iterations，如果为 0 则使用套件默认迭代次数
 *	 2. 重新设置随机种子（确保可重现性）
 *	 3. 循环执行迭代次数：
 *		a. 调用 setup 函数（如果存在）获取 fixture 数据
 *		b. 记录开始时间（wall-clock 和 CPU）
 *		c. 执行测试函数 test->test()
 *		d. 记录结束时间
 *		e. 调用 tear_down 清理（如果存在）
 *		f. 如果结果不是 MUNIT_OK，记录失败/跳过/错误并退出循环
 *		g. 如果结果是 MUNIT_OK，累加报告计数器和时间
 *	 4. 返回最终结果
 *
 * @param[in,out] runner 测试运行器指针，包含运行配置
 * @param[in]	  test	 当前要执行的测试用例
 * @param[in]	  params 参数组合（可能为 NULL）
 * @param[out]	  report 输出参数，存储测试执行的详细报告
 *
 * @return 返回当前测试的执行结果：
 *		   - MUNIT_OK	  测试成功
 *		   - MUNIT_FAIL   测试失败
 *		   - MUNIT_SKIP   测试跳过
 *		   - MUNIT_ERROR  测试错误
 */
static MunitResult munit_test_runner_exec(MunitTestRunner *runner, const MunitTest *test, const MunitParameter params[], MunitReport *report)
{
    unsigned int iterations = runner->iterations;
    MunitResult  result     = MUNIT_FAIL;

    unsigned int i = 0;

    if ((test->option & MUNIT_TEST_OPTION_SINGLE_ITERATION) == MUNIT_TEST_OPTION_SINGLE_ITERATION)
    {
        iterations = 1;
    }
    else if (iterations == 0)
    {
        iterations = runner->iterations;
    }

    munit_rand_seed(runner->seed);

#if defined(MUNIT_ENABLE_TIMING)
    uint64_t iter_ns = 0;
#endif

    do
    {
        void *data = (test->setup == NULL) ? runner->user_data : test->setup(params, runner->user_data);

#if defined(MUNIT_ENABLE_TIMING)
        munit_timing_ctx_t timing_ctx;
        munit_timing_start(&timing_ctx);
#endif

        result = test->test(params, data);

#if defined(MUNIT_ENABLE_TIMING)
        iter_ns = munit_timing_stop(&timing_ctx);
        report->wall_clock += iter_ns;
#endif

        if (test->tear_down != NULL)
        {
            test->tear_down(data);
        }

        if (MUNIT_LIKELY(result == MUNIT_OK))
        {
            report->successful++;
        }
        else
        {
            switch ((int)result)
            {
                case MUNIT_SKIP:
                    report->skiped++;
                    break;

                case MUNIT_FAIL:
                    report->failed++;
                    break;

                case MUNIT_ERROR:
                    report->errored++;
                    break;

                default:
                    break;
            }

            break;
        }
    } while (++i < iterations);

    return result;
}

#define MUNIT_RESULT_STRING_OK    "OK    "
#define MUNIT_RESULT_STRING_SKIP  "SKIP  "
#define MUNIT_RESULT_STRING_FAIL  "FAIL  "
#define MUNIT_RESULT_STRING_ERROR "ERROR "
#define MUNIT_RESULT_STRING_TODO  "TODO  "

static void munit_test_runner_print_color(const MunitTestRunner *runner, const char *string, char color)
{
    if (runner->colorize)
    {
        fprintf(MUNIT_OUTPUT_FILE, "\x1b[3%cm%s\x1b[39m", color, string);
    }
    else
    {
        fputs(string, MUNIT_OUTPUT_FILE);
    }
}
/**
 * 使用指定的参数执行单个测试用例（核心执行函数）
 *
 * 本函数是 MUnit 测试执行的最终执行器，负责在确定的参数组合下运行测试。
 * 它处理测试的完整生命周期：捕获 stderr 输出、管理子进程（fork）、
 * 执行测试函数、收集结果、显示输出等。
 *
 * 执行流程：
 *	 1. 如果 params 不为 NULL，打印参数信息（格式：name=value, ...）
 *	 2. 创建临时文件用于捕获 stderr 输出
 *	 3. 如果启用 fork（且平台支持）：
 *		a. 创建管道用于父子进程通信
 *		b. 在子进程中：
 *		   - 将 stderr 重定向到临时文件
 *		   - 调用 munit_test_runner_exec 执行测试
 *		   - 通过管道将测试报告传回父进程
 *		c. 在父进程中：
 *		   - 从管道读取测试报告
 *		   - 等待子进程结束并检查退出状态
 *		   - 处理子进程异常（信号、崩溃等）
 *	 4. 如果不启用 fork：
 *		a. 重定向 stderr 到临时文件
 *		b. 使用 setjmp/longjmp 捕获断言失败
 *		c. 调用 munit_test_runner_exec 执行测试
 *		d. 恢复 stderr
 *	 5. 根据测试结果（TODO 标记、失败数、错误数等）打印相应的状态符号
 *	 6. 如果测试失败或启用 show_stderr，将临时文件中的 stderr 输出转储
 *	 7. 清理临时文件
 *
 * @param[in,out] runner 测试运行器指针，包含运行配置和结果报告
 * @param[in]	  test	 当前要执行的测试用例
 * @param[in]	  params 当前测试使用的参数组合（键值对数组），可能为 NULL
 *
 * @note  测试状态输出格式：[ OK ]、[ FAIL ]、[ SKIP ]、[ ERROR ]、[ TODO ]
 * @note  支持彩色输出（通过 runner->colorize 控制）
 * @note  测试时间（wall-clock 和 CPU）会在 MUNIT_ENABLE_TIMING 时显示
 * @note  如果测试标记为 TODO 且执行成功，会被视为错误（因为不该成功）
 * @see   munit_test_runner_exec 实际执行测试逻辑的子函数
 * @see   munit_replace_stderr 重定向 stderr 到临时文件
 * @see   munit_splice 将临时文件内容转储到 stderr
 */
static void munit_test_runner_run_test_with_params(MunitTestRunner *runner, const MunitTest *test, const MunitParameter params[])
{
    MunitResult result = MUNIT_OK;
    MunitReport report = {
        0,
        0,
        0,
        0,

#if defined(MUNIT_ENABLE_TIMING)
        0,
        0
#endif
    };
    unsigned int          output_l;
    bool                  first;
    const MunitParameter *param;
    if (params != NULL)
    {
        output_l = 4;
        fputs("    ", MUNIT_OUTPUT_FILE);
        first = 1;
        for (param = params; param != NULL && param->name != NULL; param++)
        {
            if (!first)
            {
                fputs(", ", MUNIT_OUTPUT_FILE);
                output_l += 2;
            }
            else
            {
                first = 0;
            }

            output_l += fprintf(MUNIT_OUTPUT_FILE, "%s=%s", param->name, param->value);
        }
        while (output_l++ < MUNIT_TEST_NAME_LEN)
        {
            fputc(' ', MUNIT_OUTPUT_FILE);
        }
    }

    fflush(MUNIT_OUTPUT_FILE);

    result = munit_test_runner_exec(runner, test, params, &report);

#if defined(MUNIT_ENABLE_TIMING)
    /* 累加本次测试耗时到 runner 全局报告 */
    runner->report.wall_clock += report.wall_clock;
#endif

    fputs("[ ", MUNIT_OUTPUT_FILE);
    if ((test->option & MUNIT_TEST_OPTION_TODO) == MUNIT_TEST_OPTION_TODO)
    {
        if (report.failed != 0 || report.errored != 0 || report.skiped != 0)
        {
            munit_test_runner_print_color(runner, MUNIT_RESULT_STRING_TODO, '3');
            result = MUNIT_OK;
        }
        else
        {
            munit_test_runner_print_color(runner, MUNIT_RESULT_STRING_ERROR, '1');
            runner->report.failed++;
            result = MUNIT_ERROR;
        }
    }
    else if (report.failed > 0)
    {
        munit_test_runner_print_color(runner, MUNIT_RESULT_STRING_FAIL, '1');
        runner->report.failed++;
        result = MUNIT_FAIL;
    }
    else if (report.errored > 0)
    {
        munit_test_runner_print_color(runner, MUNIT_RESULT_STRING_ERROR, '1');
        runner->report.errored++;
        result = MUNIT_ERROR;
    }
    else if (report.skiped > 0)
    {
        munit_test_runner_print_color(runner, MUNIT_RESULT_STRING_SKIP, '3');
        runner->report.skiped++;
        result = MUNIT_SKIP;
    }
    else if (report.successful > 0)
    {
        munit_test_runner_print_color(runner, MUNIT_RESULT_STRING_OK, '2');
#if defined(MUNIT_ENABLE_TIMING)
        fputs(" ] [ ", MUNIT_OUTPUT_FILE);
        if (report.successful > 1)
        {
            munit_print_time(MUNIT_OUTPUT_FILE, report.wall_clock / report.successful);
        }
        else
        {
            munit_print_time(MUNIT_OUTPUT_FILE, report.wall_clock);
        }
#endif
        fputs(" ]\n", MUNIT_OUTPUT_FILE);
        runner->report.successful++;
        result = MUNIT_OK;
    }

    if (result == MUNIT_FAIL || result == MUNIT_ERROR || runner->show_stder)
    {
        fflush(MUNIT_OUTPUT_FILE);
    }
}

/**
 * 递归展开通配参数并执行测试（内部函数）
 *
 * 本函数是参数化测试的核心展开引擎，负责递归处理所有未通过命令行指定的
 * 通配参数（wildcard parameters）。它会将测试中定义的参数枚举值逐个填充，
 * 生成所有可能的参数组合，并逐一执行测试。
 *
 * 执行流程：
 *	 1. 在当前参数 p 对应的测试参数枚举中查找匹配的定义
 *	 2. 如果找不到匹配（pe == NULL），直接返回（不应发生）
 *	 3. 遍历当前参数的所有可能值：
 *		a. 将当前值赋给 p->value
 *		b. 检查下一个参数是否为空：
 *		   - 如果为空（next->name == NULL），说明所有参数已填充完毕
 *			 → 执行测试（调用 munit_test_runner_run_test_with_params）
 *		   - 如果不为空，递归调用自身处理下一个参数
 *	 4. 如果启用 fatal_failures 且已有失败/错误，提前终止循环
 *
 * @param[in,out] runner	测试运行器指针，包含运行配置和结果报告
 * @param[in]	  test		当前执行的测试用例
 * @param[in]	  test_name 完整的测试名称（用于日志输出）
 * @param[in]	  params	已填充的参数数组（包含从 CLI 获取的参数）
 * @param[in,out] p 		当前要处理的通配参数指针（指向 params 数组中的元素）
 *
 * @note  递归终止条件：p->name == NULL（即已到达参数数组末尾）
 * @note  本函数会深度遍历所有参数组合，形成笛卡尔积
 * @note  对于每个参数组合，都会调用 munit_test_runner_run_test_with_params 执行测试
 * @see   munit_test_runner_run_test 本函数的调用者
 * @see   munit_test_runner_run_test_with_params 实际执行测试的函数
 */
static void munit_test_runner_run_test_wild(MunitTestRunner *runner, const MunitTest *test, const char *test_name, MunitParameter *params, MunitParameter *p)
{
    const MunitParameterEnum *pe;
    const char              **values;
    MunitParameter           *next;

    for (pe = test->parameters; pe != NULL && pe->name != NULL; pe++)
    {
        if (p->name == pe->name)
        {
            break;
        }
    }

    if (pe == NULL)
    {
        return;
    }

    for (values = pe->values; *values != NULL; values++)
    {
        next     = p + 1;
        p->value = *values;

        if (next->name == NULL)
        {
            munit_test_runner_run_test_with_params(runner, test, params);
        }
        else
        {
            munit_test_runner_run_test_wild(runner, test, test_name, params, p);
        }

        if (runner->fatal_failures && (runner->report.failed != 0 || runner->report.errored != 0))
        {
            break;
        }
    }
}

/**
 * 运行单个测试用例（包括参数化展开）
 *
 * 本函数负责执行一个测试用例，处理参数化测试的展开逻辑。
 * 如果测试定义了参数（parameters），则根据命令行输入或 --single 模式
 * 决定是运行所有参数组合还是随机选择一种组合。
 *
 * 执行流程：
 *	 1. 拼接测试名称前缀：prefix + test->name
 *	 2. 如果测试没有参数 (parameters == NULL)：
 *		→ 直接执行该测试一次
 *	 3. 如果测试有参数：
 *		a. 检查命令行是否通过 --param 指定了该参数的值
 *		b. 如果指定了，使用指定的值
 *		c. 如果 --single 模式启用：
 *		   → 从所有可能值中随机选择一个组合
 *		d. 否则：
 *		   → 通配模式：生成所有参数值组合并逐个执行
 *	 4. 释放拼接的测试名称字符串
 *
 * @param[in,out] runner 测试运行器指针，包含运行配置和结果报告
 * @param[in]	  test	 当前要执行的测试用例
 * @param[in]	  prefix 父级路径前缀，用于构建完整测试名称
 *
 * @note  参数化展开的核心逻辑：
 *		  - 命令行未指定参数时，默认运行所有组合
 *		  - 使用 --single 可随机选择一种组合（用于快速验证）
 *		  - 使用 --param 可固定特定参数值
 * @note  通配参数会递归展开，形成笛卡尔积组合
 * @see   munit_test_runner_run_test_with_params 执行单个参数组合
 * @see   munit_test_runner_run_test_wild 递归展开通配参数
 */
static void munit_test_runner_run_test(MunitTestRunner *runner, const MunitTest *test, const char *prefix)
{
    char *test_name = munit_maybe_concat(NULL, (char *)prefix, (char *)test->name);

    /* 要传递给 munit_test_runner_run_test_with_params 的参数数组 */
    MunitParameter *params     = NULL; // 已确定值的参数数组（最终传递给测试函数）
    size_t          params_len = 0;    // 数组中当前已有的参数数量

    /* 通配参数是指那些在测试中指定了可能值，
     * 但没有通过 CLI 传递特定值的参数。
     * 这意味着我们希望为每种可能的参数值组合运行一次测试，
     * 或者，如果向 CLI 传递了 --single，则使用随机的一组参数运行一次。 */
    MunitParameter           *wild_params     = NULL; // 未确定值的通配参数列表（需要展开的参数）
    size_t                    wild_params_len = 0;    // 组中通配参数的数量
    const MunitParameterEnum *pe;                     // 历 test->parameters 的游标
    const MunitParameter     *cli_p;                  // 遍历 runner->parameters 的游标
    bool                      filled;                 // 标记当前参数是否已被 CLI 指定
    unsigned int              possible;               // 当前参数枚举中的值个数
    const char              **vals;                   // 遍历 pe->values 的游标
    size_t                    first_wild;             // params 数组中第一个通配参数的索引位置
    const MunitParameter     *wp;                     // 遍历 wild_params 的游标
    int                       pidx;                   // 随机选择的参数值索引

    /* 重新设置随机种子，确保每次测试的可重现性 */
    munit_rand_seed(runner->seed);

    /* -- print test name with alignment -- */
    {
        unsigned int name_len = (unsigned int)strlen(test_name);
        fputs(test_name, MUNIT_OUTPUT_FILE);
        fputc(':', MUNIT_OUTPUT_FILE);
        name_len++;
        /* 如果有参数化测试，在测试名后换行；否则对齐到 MUNIT_TEST_NAME_LEN */
        if (test->parameters != NULL && test->parameters[0].name != NULL)
        {
            fputc('\n', MUNIT_OUTPUT_FILE);
        }
        else
        {
            while (name_len++ < MUNIT_TEST_NAME_LEN)
            {
                fputc(' ', MUNIT_OUTPUT_FILE);
            }
        }
    }

    if (test->parameters == NULL)
    {
        munit_test_runner_run_test_with_params(runner, test, NULL);
    }
    else
    {
        for (pe = test->parameters; pe != NULL && pe->name != NULL; pe++)
        {
            /* 我们是否从 CLI 收到了此参数的值？ */
            filled = 0;

            /*需要在test数组中事先挂载想要测试参数名和值吗，
            然后再命令行中在依据枚举的那些参数名和值输入正确的键值对，
            在这个函数中遍历匹配*/
            for (cli_p = runner->parameters; cli_p != NULL && cli_p->name != NULL; cli_p++)
            {
                if (strcmp(cli_p->name, pe->name) == 0)
                {
                    if (MUNIT_UNLIKELY(munit_parameters_add(&params_len, &params, pe->name, cli_p->value) !=
                                       MUNIT_OK))
                    {
                        goto cleanup;
                    }

                    filled = 1;
                    break;
                }
            }

            /* 加载完所有参数 */
            if (filled == 1)
            {
                continue;
            }

            /* 没有来自 CLI 的内容，枚举是 NULL/空吗？我们不是模糊测试器…… */
            if (pe->values == NULL || pe->values[0] == NULL)
            {
                continue;
            }

            /* 如果向 CLI 传递了 --single，则从可能性列表中随机选择一个值。 */
            if (runner->single_parameter_mode == 1)
            {
                possible = 0;
                for (vals = pe->values; *vals != NULL; vals++)
                {
                    possible++;
                }

                /* possible >= 1 由上面 pe->values[0] != NULL 保证 */
                pidx = munit_rand_at_most(munit_str_hash(test_name), possible - 1);

                if (MUNIT_UNLIKELY(munit_parameters_add(&params_len, &params, pe->name, pe->values[pidx]) !=
                                   MUNIT_OK))
                {
                    goto cleanup;
                }
            }
            else
            {
                /* 我们希望尝试每种排列。放入一个占位符条目，
                 * 我们稍后会遍历它们。 */
                if (MUNIT_UNLIKELY(munit_parameters_add(&wild_params_len, &wild_params, pe->name, NULL) !=
                                   MUNIT_OK))
                {
                    goto cleanup;
                }
            }
        }

        if (wild_params_len != 0)
        {
            first_wild = params_len;

            /* 为所有通配参数设置初始值（通常是第一个可能值） */
            for (wp = wild_params; wp != NULL && wp->name != NULL; wp++)
            {
                for (pe = test->parameters; pe != NULL && pe->name != 0 && pe->values != NULL; pe++)
                {
                    if (strcmp(wp->name, pe->name) == 0)
                    {
                        if (MUNIT_UNLIKELY(munit_parameters_add(&params_len, &params, pe->name, pe->values[0]) !=
                                           MUNIT_OK))
                        {
                            goto cleanup;
                        }
                    }
                }
            }

            munit_test_runner_run_test_wild(runner, test, test_name, params, params + first_wild);
        }
        else
        {
            munit_test_runner_run_test_with_params(runner, test, params);
        }

cleanup:
        free(params);
        free(wild_params);
    }

    munit_maybe_free_concat(test_name, prefix, test->name);
}

/**
 * 递归遍历并执行测试套件及其所有子套件
 *
 * 本函数是 MUnit 测试执行引擎的核心，负责递归遍历整棵测试套件树。
 * 它会依次处理当前套件中的所有测试用例，然后递归处理所有子套件。
 * 支持通过 runner->tests 过滤指定测试，以及遇错即停 (fatal_failures) 机制。
 *
 * 执行流程：
 *	 1. 拼接前缀：将父级前缀与当前套件前缀组合成完整路径
 *	 2. 遍历当前套件的 tests 数组：
 *		- 如果 runner->tests 为 NULL，执行所有测试
 *		- 否则只执行名称匹配的测试
 *	 3. 检查 fatal_failures 标志，决定是否继续执行
 *	 4. 递归遍历当前套件的 suites 数组（深度优先）
 *	 5. 释放拼接的前缀字符串
 *
 * @param[in,out] runner  测试运行器指针，包含运行配置和结果报告
 * @param[in]	  suite   当前要执行的测试套件
 * @param[in]	  prefix  父级路径前缀（由上层套件传入），用于构建完整测试路径
 *
 * @note  测试名称匹配规则：
 *		  - 完整路径 = prefix + suite->prefix + test->name
 *		  - 用户指定的测试名与完整路径进行前缀匹配
 *		  - 例如：用户指定 "/math"，则匹配 "/math/add"、"/math/sub" 等
 * @note  tests 数组和 suites 数组均以 NULL 结尾
 * @note  如果 fatal_failures 为真且发生失败/错误，立即停止执行
 * @see   munit_test_runner_run_test 执行单个测试用例
 * @see   munit_test_runner_run_suite 递归调用自身处理子套件
 */
static void munit_test_runner_run_suite(MunitTestRunner *runner, const MunitSuite *suite, const char *prefix)
{
    size_t            pre_len;
    char             *pre = munit_maybe_concat(&pre_len, (char *)prefix, (char *)suite->prefix);
    const MunitTest  *test;
    const char      **test_name;
    const MunitSuite *child_suite;

    // 运行测试
    for (test = suite->tests; test != NULL && test->test != NULL; test++)
    {
        // 这里虽然最后调用的都是同一个函数，但其区别是调用次数上不同有过滤的作用
        if (runner->tests != NULL)
        {
            // 运行特定测试
            for (test_name = runner->tests; test != NULL && *test_name != NULL; test_name++)
            {
                // 前缀长度为0
                // 命令行参数的前缀部分与当前套件前缀匹配
                // 测试名称与命令行参数的剩余部分匹配
                if ((pre_len == 0 || strncmp(pre, *test_name, pre_len) == 0) &&
                    strncmp(test->name, *test_name + pre_len, strlen(*test_name + pre_len)) == 0)
                {
                    munit_test_runner_run_test(runner, test, pre);
                }

                if (runner->fatal_failures && (runner->report.failed != 0 || runner->report.errored != 0))
                {
                    goto cleanup;
                }
            }
        }
        else
        {
            // 运行所有测试
            munit_test_runner_run_test(runner, test, pre);
        }

        if (runner->fatal_failures && (runner->report.failed != 0 || runner->report.errored != 0))
        {
            goto cleanup;
        }
    }

    // 运行任何子套件（在 test 循环外面，确保每个子套件只执行一次）
    for (child_suite = suite->suites; child_suite != NULL && child_suite->prefix != NULL; child_suite++)
    {
        munit_test_runner_run_suite(runner, child_suite, pre);
    }

cleanup:
    munit_maybe_free_concat(pre, prefix, suite->prefix);
}

/**
 * 启动测试运行器的主入口函数
 *
 * 本函数是测试执行流程的顶层入口，负责启动整个测试套件的执行过程。
 * 作为 munit_test_runner_run_suite 的简化包装，自动传入顶级套件和空前缀。
 *
 * 执行流程：
 *	 1. 以 runner->suite 作为根套件开始遍历
 *	 2. 前缀初始为 NULL，表示从根套件开始
 *	 3. 递归遍历所有子套件和测试用例
 *
 * @param[in,out] runner 测试运行器指针，包含：
 *						 - suite: 要执行的根测试套件
 *						 - tests: 用户指定的测试过滤列表（可选）
 *						 - report: 用于累积测试结果
 *						 - 其他运行时配置（种子、迭代次数等）
 *
 * @note  本函数是递归遍历的起点，最终会遍历整棵测试套件树
 * @note  如果 runner->tests 不为空，则只会执行匹配的测试用例
 * @see   munit_test_runner_run_suite 实际执行递归遍历的内部函数
 */
void munit_test_runner_run(MunitTestRunner *runner)
{
    munit_test_runner_run_suite(runner, runner->suite, NULL);
}

/**
 * MUnit 测试框架的自定义入口函数
 *
 * 这是 MUnit 测试框架的核心入口函数，提供了完整的测试执行能力。
 * 与 munit_suite_main 相比，本函数额外支持自定义命令行参数扩展，
 * 允许用户为测试程序添加自己的命令行选项。
 *
 * 该函数会完成以下工作：
 *	 1. 生成或获取随机种子（用于测试的可重现性）
 *	 2. 解析内置命令行参数（如 --seed、--iterations、--list 等）
 *	 3. 解析用户自定义命令行参数（通过 arguments 数组注册）
 *	 4. 构建测试运行器上下文 (MunitTestRunner)
 *	 5. 递归遍历测试套件树，执行匹配的测试用例
 *	 6. 收集并输出测试结果报告
 *
 * @param[in] suite 	  测试套件指针，包含测试层次结构的根节点
 *						  该套件定义了所有测试用例及其组织方式
 * @param[in] user_data   用户自定义数据指针，将传递给测试套件中的所有测试
 *						  可用于传递全局配置、共享资源等
 *						  如无需要可传入 NULL
 * @param[in] argc		  命令行参数个数，通常直接传递 main 函数的 argc
 * @param[in] argv		  命令行参数数组，通常直接传递 main 函数的 argv
 *						  数组长度为 argc + 1，最后一个元素为 NULL
 * @param[in] arguments   用户自定义命令行参数定义数组
 *						  用于扩展测试程序支持的命令行选项
 *						  数组以 { NULL, NULL, NULL } 结尾
 *						  如无自定义参数需要可传入 NULL
 *
 * @return 返回程序退出状态码：
 *		   - EXIT_SUCCESS (0)  表示所有测试通过，无任何失败或错误
 *		   - EXIT_FAILURE (1)  表示存在测试失败或错误
 *
 * @note   argv 数组遵循 C 标准约定：argv[argc] == NULL
 * @note   自定义参数通过 MunitArgument 结构体注册，需同时提供
 *		   解析函数和帮助信息输出函数
 * @see    munit_suite_main 标准入口函数（简化版）
 * @see    MunitArgument 自定义命令行参数结构体
 */
int munit_suite_main_custom(const MunitSuite   *suite,
                            void               *user_data,
                            int                 argc,
                            char *const         argv[argc + 1],
                            const MunitArgument arguments[])
{
    int             result = EXIT_FAILURE; // 默认失败，只有全部通过才成功
    MunitTestRunner runner;                // 核心上下文
    size_t          parameters_size = 0;   // 命令行 --param参数数量
    size_t          test_size       = 0;   // 用户指定测试名称数量
    int             arg;                   // 当前解析的argv索引

    // 各参数的临时存储变量
    char                *envpter;      // strtoul 的结束指针
    char                *endptr;       // strtoul的结束指针
    unsigned long        ts;           // 种子值临时存储
    unsigned long long   iterations;   // 迭代次数临时存储值
    MunitLogLevel        level;        // 日志级别
    const MunitArgument *argument;     // 匹配到的自定义参数
    const char         **runner_tests; // 测试列表临时指针
    unsigned int         test_run;     // 实际运行的测试数
    unsigned int         tests_total;  // 总测试数(含跳过的)

    // runner初始化
    // 初始化指针
    runner.prefix     = NULL;
    runner.suite      = NULL;
    runner.tests      = NULL;
    runner.parameters = NULL;
    runner.user_data  = NULL;

    // 初始化数值
    runner.seed                  = 0;
    runner.iterations            = 0;
    runner.single_parameter_mode = 0;

    // 初始化报告
    runner.report.successful = 0;
    runner.report.failed     = 0;
    runner.report.skiped     = 0;
    runner.report.errored    = 0;
#if defined(MUNIT_ENABLE_TIMING)
    runner.report.cpu_clock  = 0;
    runner.report.wall_clock = 0;
#endif
    // 设置标志位默认值
    runner.colorize       = 0;
    runner.fork           = 0; // unix开启暂不支持
    runner.show_stder     = 0;
    runner.fatal_failures = 0;

    // 从参数获取核心配置
    runner.suite     = suite;
    runner.user_data = user_data;
    runner.seed      = munit_rand_generate_seed();                    // 生成随机种子
    runner.colorize  = munit_stream_supports_ansi(MUNIT_OUTPUT_FILE); // 自动检测颜色分组

    // 命令行参数解析
    for (arg = 1; arg < argc; arg++)
    {
        if (strncmp("--", argv[arg], 2) == 0)
        {
            if (strcmp("seed", argv[arg] + 2) == 0)
            {
                if (arg + 1 >= argc)
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "%s requires an argument", argv[arg]);
                    goto cleanup;
                }

                envpter = argv[arg + 1];
                ts      = strtoul(argv[arg + 1], &envpter, 0);

                if (*envpter != '\0' || ts > (~(uint32_t)0U))
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "invalid value ('%s') passed to %s", argv[arg + 1], argv[arg]);
                }

                runner.seed = (uint32_t)ts;
                arg++; // 加上for中的++就可跳过
            }
            else if (strcmp("iterations", argv[arg] + 2) == 0)
            {
                if (arg + 1 >= argc)
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "%s requires an argument", argv[arg]);
                    goto cleanup;
                }

                endptr     = argv[arg + 1];
                iterations = strtoul(argv[arg + 1], &endptr, 0);

                if (*endptr != '\0' || iterations > UNIT_MAX)
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "invalid value ('%s') passed to %s", argv[arg + 1], argv[arg]);
                    goto cleanup;
                }

                runner.iterations = (unsigned int)iterations;
                arg++;
            }
            else if (strcmp("param", argv[arg] + 2) == 0)
            {
                if (arg + 2 >= argc)
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "%s requires two arguments", argv[arg]);
                    goto cleanup;
                }

                runner.parameters = realloc(runner.parameters,
                                            sizeof(MunitParameter) * (parameters_size + 2));

                if (runner.parameters == NULL)
                {
                    munit_log_internal(MUNIT_LOG_ERROR, stderr, "failed to allocate memory");
                    goto cleanup;
                }

                runner.parameters[parameters_size].name  = argv[arg + 1];
                runner.parameters[parameters_size].value = argv[arg + 2];
                parameters_size++;
                runner.parameters[parameters_size].name  = NULL;
                runner.parameters[parameters_size].value = NULL;
                arg += 2;
            }
            else if (strcmp("color", argv[arg] + 2) == 0)
            {
                if (arg + 1 >= argc)
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "%s requires an argument", argv[arg]);
                    goto cleanup;
                }

                if (strcmp("always", argv[arg + 1]) == 0)
                {
                    runner.colorize = 1;
                }
                else if (strcmp("never", argv[arg + 1]) == 0)
                {
                    runner.colorize = 0;
                }
                else if (strcmp("auto", argv[arg + 1]) == 0)
                {
                    runner.colorize = munit_stream_supports_ansi(MUNIT_OUTPUT_FILE);
                }
                else
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "invalid value ('%s') passed to %s", argv[arg + 1], argv[arg]);
                    goto cleanup;
                }

                arg++;
            }
            else if (strcmp("help", argv[arg] + 2) == 0)
            {
                munit_print_help(argc, argv, user_data, arguments);
                result = EXIT_SUCCESS;
                goto cleanup;
            }
            else if (strcmp("single", argv[arg] + 2) == 0)
            {
                runner.single_parameter_mode = 1;
            }
            else if (strcmp("show-stderr", argv[arg] + 2) == 0)
            {
                runner.show_stder = 1;
            }
            else if (strcmp("no-fork", argv[arg] + 2) == 0)
            {
                runner.fork = 0;
            }
            else if (strcmp("fatal-failures", argv[arg] + 2) == 0)
            {
                runner.fatal_failures = 1;
            }
            else if (strcmp("log-visible", argv[arg] + 2) == 0 || strcmp("log-fatal", argv[arg] + 2) == 0)
            {
                if (arg + 1 >= argc)
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "%s requires an argument", argv[arg]);
                    goto cleanup;
                }

                if (strcmp("debug", argv[arg + 1]) == 0)
                {
                    level = MUNIT_LOG_DEBUG;
                }
                else if (strcmp("info", argv[arg + 1]) == 0)
                {
                    level = MUNIT_LOG_INFO;
                }
                else if (strcmp("warning", argv[arg + 1]) == 0)
                {
                    level = MUNIT_LOG_WARNING;
                }
                else if (strcmp("error", argv[arg + 1]) == 0)
                {
                    level = MUNIT_LOG_ERROR;
                }
                else
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "invalid value ('%s') passed to %s", argv[arg + 1], argv[arg]);
                    goto cleanup;
                }

                if (strcmp("log-visible", argv[arg] + 2) == 0)
                {
                    munit_log_level_visible = level;
                }
                else
                {
                    munit_log_level_fatal = level;
                }

                arg++;
            }
            else if (strcmp("list", argv[arg] + 2) == 0)
            {
                munit_suite_list_tests(suite, 0, NULL);
                result = EXIT_SUCCESS;
                goto cleanup;
            }
            else if (strcmp("list-params", argv[arg] + 2) == 0)
            {
                munit_suite_list_tests(suite, 1, NULL);
                result = EXIT_SUCCESS;
                goto cleanup;
            }
            else
            {
                argument = munit_arguments_find(arguments, argv[arg] + 2);

                if (argument == NULL)
                {
                    munit_logf_internal(MUNIT_LOG_ERROR, stderr, "unknown argument ('%s')", argv[arg]);
                    goto cleanup;
                }

                if (!argument->parse_argument(suite, user_data, &arg, argc, argv))
                {
                    goto cleanup;
                }
            }
        }

        // 执行特定的测试
        else
        {
            runner_tests = realloc((void *)runner.tests, sizeof(char *) * (test_size + 2));

            if (runner_tests == NULL)
            {
                munit_log_internal(MUNIT_LOG_ERROR, stderr, "failed to allocate memory");
                goto cleanup;
            }

            runner.tests = runner_tests;

            // 循环覆盖传入的测试
            runner.tests[test_size++] = argv[arg];
            runner.tests[test_size]   = NULL;

            // printf("tests[%ld]:%s [%ld]:%s\n",tests_size-1,runner.tests[tests_size-1],tests_size,runner.tests[tests_size]);
        }
    }

    fflush(stderr);
    fprintf(MUNIT_OUTPUT_FILE, "Running test suite with seed 0x%08" PRIx32 "...\n", runner.seed);

    munit_test_runner_run(&runner);

    // 报告输出
    test_run    = runner.report.successful + runner.report.failed + runner.report.errored;
    tests_total = test_run + runner.report.skiped;

    if (test_run == 0)
    {
        fprintf(stderr, "NO test run,%d(100%%) skipped.\n", runner.report.skiped);
    }
    else
    {
        /* 嵌入式 printf 通常不支持 %f，用整数百分比避免浮点 */
        unsigned long pct_success = 0UL;
        unsigned long pct_skipped = 0UL;
        if (test_run > 0)
        {
            pct_success = ((unsigned long)runner.report.successful * 100UL) / (unsigned long)test_run;
        }
        if (tests_total > 0)
        {
            pct_skipped = ((unsigned long)runner.report.skiped * 100UL) / (unsigned long)tests_total;
        }

        fprintf(MUNIT_OUTPUT_FILE, "%d of %d (%lu%%) tests successful, %d (%lu%%) test skipped.", runner.report.successful, test_run, pct_success, runner.report.skiped, pct_skipped);
#if defined(MUNIT_ENABLE_TIMING)
        fprintf(MUNIT_OUTPUT_FILE, " Total wall-clock: ");
        munit_print_time(MUNIT_OUTPUT_FILE, runner.report.wall_clock);
#endif
        fputc('\n', MUNIT_OUTPUT_FILE);
    }

    if (runner.report.failed == 0 && runner.report.errored == 0)
    {
        result = EXIT_SUCCESS;
    }
cleanup:
    free(runner.parameters);
    free((void *)runner.tests);

    return result;
}

/**
 * MUnit 测试框架的主入口函数
 *
 * 这是 MUnit 测试框架的标准入口点，负责初始化测试运行器、解析命令行参数
 * 并执行指定的测试套件。该函数是 munit_suite_main_custom 的简化包装，
 * 默认不启用自定义命令行参数扩展。
 *
 * 该函数会完成以下工作：
 *	 1. 生成或获取随机种子（用于测试的可重现性）
 *	 2. 解析命令行参数（如 --seed、--iterations、--list 等）
 *	 3. 构建测试运行器上下文 (MunitTestRunner)
 *	 4. 递归遍历测试套件树，执行匹配的测试用例
 *	 5. 收集并输出测试结果报告
 *
 * @param[in] suite 	 测试套件指针，包含测试层次结构的根节点
 *						 该套件定义了所有测试用例及其组织方式
 * @param[in] user_data  用户自定义数据指针，将传递给测试套件中的所有测试
 *						 可用于传递全局配置、共享资源等
 *						 如无需要可传入 NULL
 * @param[in] argc		 命令行参数个数，通常直接传递 main 函数的 argc
 * @param[in] argv		 命令行参数数组，通常直接传递 main 函数的 argv
 *						 数组长度为 argc + 1，最后一个元素为 NULL
 *
 * @return 返回程序退出状态码：
 *		   - EXIT_SUCCESS (0)  表示所有测试通过，无任何失败或错误
 *		   - EXIT_FAILURE (1)  表示存在测试失败或错误
 *
 * @note   argv 数组遵循 C 标准约定：argv[argc] == NULL
 * @note   可通过命令行参数 --help 查看所有支持的选项
 * @see    munit_suite_main_custom 用于需要自定义命令行参数的场景
 */
int munit_suite_main(const MunitSuite *suite, void *user_data, int argc, char *const argv[argc + 1])
{
    return munit_suite_main_custom(suite, user_data, argc, argv, NULL);
}
