# munit 测试框架适配说明与修订历史

**日期**: 2026-09-22

---

## 1. 概述

本文档记录 `tests/framework/munit.c` 和 `munit.h` 在 soft_ssd 项目中的嵌入式适配设计、对外接口说明以及完整的修订历史。

munit 原版来自 [nemequ/munit](https://nemequ.github.io/munit/)（MIT License），本项目基于 v0.4.1 做了嵌入式裸机适配。

---

## 2. 对外接口

### 2.1 测试编写接口

#### 断言宏

所有断言宏定义在 `munit.h`，测试代码直接使用：

| 宏 | 用途 |
|---|---|
| `munit_assert(expr)` | 通用断言 |
| `munit_assert_true(expr)` / `munit_assert_false(expr)` | 布尔断言 |
| `munit_assert_{type}(a, op, b)` | 类型化比较，支持：`int8/16/32/64`、`uint8/16/32/64`、`short`、`ushort`、`int`、`uint`、`long`、`ulong`、`llong`、`ullong`、`size`、`float`、`double`、`char`、`uchar`、`ptr` |
| `munit_assert_string_equal(a, b)` / `not_equal` | 字符串断言 |
| `munit_assert_memory_equal(size, a, b)` / `not_equal` | 内存块断言 |
| `munit_assert_null(ptr)` / `munit_assert_not_null(ptr)` | 指针断言 |
| `munit_assert_double_equal(a, b, precision)` | 浮点精度断言 |

#### 测试结构体

```c
typedef MunitResult (*MunitTestFunc)(const MunitParameter params[], void *user_data_or_fixture);
typedef void *(*MunitTestSetup)(const MunitParameter params[], void *user_data);
typedef void (*MunitTestTearDown)(void *fixture);

typedef struct {
    char               *name;       // 测试名称（路径格式）
    MunitTestFunc       test;       // 测试函数指针
    MunitTestSetup      setup;      // 可选：setup 函数
    MunitTestTearDown   tear_down;  // 可选：teardown 函数
    MunitTestOptions    option;     // 选项（SINGLE_ITERATION / TODO）
    MunitParameterEnum *parameters; // 可选：参数化测试
} MunitTest;
```

#### 套件定义

```c
typedef struct Munit_Suite_s {
    char            *prefix;     // 套件前缀
    MunitTest       *tests;      // 测试数组（NULL 结尾）
    MunitSuite      *suites;     // 子套件数组（NULL 结尾）
    unsigned int     iterations; // 默认迭代次数
    MunitSuiteOption options;    // 套件选项
} MunitSuite;
```

#### 入口函数

**标准入口（带 CLI 解析）**：
```c
int munit_suite_main(const MunitSuite *suite, void *user_data,
                     int argc, char *const argv[argc + 1]);
```

**自定义参数扩展入口**：
```c
int munit_suite_main_custom(const MunitSuite *suite, void *user_data,
                            int argc, char *const argv[argc + 1],
                            const MunitArgument arguments[]);
```

**裸机直接入口（绕过 CLI 解析）**：
```c
void munit_test_runner_run(MunitTestRunner *runner);
```

### 2.2 裸机入口使用方法

```c
#include "tests/framework/munit.h"

extern MunitSuite g_fc_nand_suite;

int ut_run_all(int argc, char *argv[])
{
    /* 直接使用 munit_suite_main，argc/argv 从 UART CLI 传入 */
    return munit_suite_main(&g_fc_nand_suite, NULL, argc, argv);
}

/* 或绕过 CLI 解析，直接构造 runner */
int ut_run_fixed(void)
{
    MunitTestRunner runner = {0};
    runner.suite       = &g_fc_nand_suite;
    runner.seed        = 0x1234;
    runner.iterations  = 1;
    runner.colorize    = 0;
    runner.fork        = 0;
    runner.fatal_failures = 0;

    munit_test_runner_run(&runner);

    /* 输出结果概要 */
    fprintf(stdout, "OK=%u FAIL=%u SKIP=%u ERROR=%u\n",
            runner.report.successful,
            runner.report.failed,
            runner.report.skiped,
            runner.report.errored);

    return (runner.report.failed == 0 && runner.report.errored == 0) ? 0 : 1;
}
```

### 2.3 命令行选项

通过在 UART 串口输入（如 `ssd> test --seed 0x1234 /fc-nand/command/validate_null`）可以传递以下选项：

| 选项 | 用途 |
|---|---|
| `--seed N` | 设置随机种子 |
| `--iterations N` | 每个测试运行 N 次 |
| `--single` | 每个参数化测试只运行一种随机组合 |
| `--param name value` | 固定参数化测试的某个参数值 |
| `--list` | 列出所有可用测试名称 |
| `--list-params` | 列出所有测试名称及其参数 |
| `--no-fork` | 不在子进程中执行（裸机默认） |
| `--fatal-failures` | 遇错即停 |
| `--show-stderr` | 显示 stderr 输出 |
| `--color auto\|always\|never` | 颜色输出 |
| `--log-visible level` | 设置日志可见级别 |
| `--log-fatal level` | 设置致命日志级别 |

### 2.4 日志接口

```c
void munit_logf(MunitLogLevel level, const char *format, ...);
void munit_log(MunitLogLevel level, const char *msg);
void munit_errorf(const char *format, ...);
void munit_error(const char *msg);
```

日志等级：`MUNIT_LOG_DEBUG`、`MUNIT_LOG_INFO`、`MUNIT_LOG_WARNING`、`MUNIT_LOG_ERROR`

### 2.5 内存分配

```c
void *munit_malloc(size_t size);
void *munit_calloc(size_t nmemb, size_t size);
#define munit_new(type)       ((type *)munit_malloc(sizeof(type)))
#define munit_newa(type, n)   ((type *)munit_calloc(n, sizeof(type)))
```

> **注意**：`munit_malloc` 内部调用 `calloc`（标准 C 库函数），在裸机上需确保堆已初始化。

### 2.6 PRNG（伪随机数生成器）

```c
void     munit_rand_seed(uint32_t seed);
uint32_t munit_rand_uint32(void);
void     munit_rand_memory(size_t size, uint8_t data[size]);
int      munit_rand_int_range(int min, int max);
double   munit_rand_double(void);
```

---

## 3. 嵌入式适配设计

### 3.1 stdout 重定向

`stm32h743/usart.c` 中的 `_write()` 将 `printf` / `fprintf` 输出重定向到 UART TX 队列：

```
printf() → _write() → uart_log_write() → TX 环形队列 → DMA/IRQ → UART 引脚
```

munit 的 `MUNIT_OUTPUT_FILE`（默认 stdout）自动走此路径。

### 3.2 arg/argv 输入

UART CLI 框架（`uart_cli_tokenize()` + `uart_execute_command()`）负责将串口输入切分为 args，注册 `test` 命令后直接调用 `munit_suite_main`：

```
ssd> test --seed 0x1234 /fc-nand/command/validate_null
  → argv = {"test", "--seed", "0x1234", "/fc-nand/command/validate_null"}
  → munit_suite_main(&g_fc_nand_suite, NULL, 4, argv)
```

### 3.3 断言失败处理

```
断言失败 → munit_errorf_ex → 打印 ERROR 日志 → while(1) 挂起
```

裸机没有 fork/abort 机制。断言失败后 CPU 进入 `while(1)` 循环，保留完整寄存器/堆栈现场，供 JTAG/SWD 调试器检查。

### 3.4 无 fork 隔离的约束

因为没有 fork，一个测试的断言失败会挂死整个固件，无法跳过该测试继续执行后续测试。因此推荐：
- **一个测试函数只验证一个行为**
- 测试间不共享可变全局状态
- setup/teardown 必须正确回收资源

---

## 4. 修订历史

记录 munit 框架从原版 v0.4.1 fork 后所有修改。

### v1.0 (2026-09-22) — 初始适配 + Bug 修复

#### Bug 修复

| # | 文件 | 位置 | 问题 | 修复 |
|---|---|---|---|---|
| 1 | `munit.h` | `munit_assert_string_equal` | `munit_tmp_a` 少下划线，编译错误 | → `munit_tmp_a_` |
| 2 | `munit.h` | `munit_assert_string_not_equal` | 同上 | → `munit_tmp_a_` |
| 3 | `munit.c` | `munit_maybe_concat` L210 | `prefix_len + prefix_len` 应为 `prefix_len + suffix_len`，buffer overflow | → `prefix_len + suffix_len` |
| 4 | `munit.c` | `--param` realloc L1337 | `sizeof(MunitParameter) * sizeof(size_t)` 永远只分配 4 或 8 字节 | → `sizeof(MunitParameter) * (parameters_size + 2)` |
| 5 | `munit.c` | `--param` boundary L1330 | `arg + 1 > argc` 只保护 1 个参数，但需要 2 个 | → `arg + 2 >= argc` |
| 6 | `munit.c` | `--help` / `--show-stderr` / `--no-fork` / `--fatal-failures` | 偏移错误：`argv[arg + N]` 应为 `argv[arg] + 2` | → `argv[arg] + 2` |
| 7 | `munit.c` | `--iterations` 参数校验 L1319 | `envpter` 拼写错误，应为 `endptr` | → `*endptr` |
| 8 | `munit.c` | 测试名过滤 L1130 | `strncmp(...)` 缺少 `== 0`，过滤逻辑颠倒 | → `== 0` |
| 9 | `munit.c` | `--single` 模式 L1012-1031 | 每个可能值都 add 一次，同一参数名出现多次 | 先计数，选一个随机值，只 add 一次 |
| 10 | `munit.c` | 子套件遍历 L1109-1161 | 嵌套在 test for 循环内，子套件重复执行 N 遍 | 移出 test 循环；`cleanup` 移到函数末尾 |

#### 嵌入式适配

| # | 文件 | 位置 | 适配 |
|---|---|---|---|
| 11 | `munit.c` | `munit_logf_ex` | 移除 `abort()`，避免与 `munit_errorf_ex` 的 `while(1)` 冲突 |
| 12 | `munit.c` | `munit_test_runner_run` | 从 `static` 改为非 `static`，供裸机直接调用 |
| 13 | `munit.h` | 类型定义后 | 添加 `MunitTestRunner` 前向声明和 `munit_test_runner_run()` 声明 |

#### 功能完善

| # | 文件 | 位置 | 改动 |
|---|---|---|---|
| 14 | `munit.c` | `--list` | `show_params=1` → `show_params=0`；新增 `--list-params` 分支 |

### 修订历史条目格式

```markdown
### vX.Y (YYYY-MM-DD) — 标题

#### Bug 修复

| # | 文件 | 位置 | 问题 | 修复 |
|---|---|---|---|---|

#### 功能变更

| # | 文件 | 位置 | 改动 |
|---|---|---|---|
```