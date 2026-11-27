# munit 测试框架适配说明与修订历史

**日期**: 2026-09-22

---

## 1. 概述

本文档记录 `tests/framework/munit.c` 和 `munit.h` 在 BASESSD 项目中的嵌入式适配设计、对外接口说明以及完整的修订历史。

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

通过在 UART 串口输入（如 `ssd> test --seed 0x1234 /fc-nand/pure/cmd_validate_op_range`）可以传递以下选项：

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
bool munit_log_visible(MunitLogLevel level);   /* v1.4：只读查询可见级 */
```

日志等级：`MUNIT_LOG_DEBUG`、`MUNIT_LOG_INFO`、`MUNIT_LOG_WARNING`、`MUNIT_LOG_ERROR`

**`munit_log_visible(level)`（v1.4 新增）** 返回该等级当前是否会真的被打印，
供**框架外**的代码（如 `tests/unit/fc_test_common.c` 的证据打印）复用同一套开关。
三条语义：

- 默认可见级是 **`ERROR`**（`munit.c`）⇒ 不加 `--log-visible` 时 INFO/DEBUG 一律不可见。
- **过滤发生在任何输出之前**（`munit.c` 的 `if (level < visible) return;`）⇒
  判不可见时连一个字节都不产出。这是「默认档报告格式逐字不变」的根据。
- 可见级是文件级 static，但 **`munit_suite_main_custom` 在每次调用开头重置为 `ERROR`**
  ⇒ 同一次上电内先跑 `test --log-visible info`、再裸跑 `test`，**证据重新隐藏**，
  符合「默认只打用例名 + 结果」。`--log-fatal` 同样重置为 `ERROR`。

⚠ **`--log-fatal` 不是「可见级」的同义词**：它与 `--log-visible` 共用同一套取值解析，
而 `munit_logf_ex` 在 `level >= fatal` 时 `abort()` ⇒ `--log-fatal info` 会让**每条 INFO 日志
都 abort**。默认 `fatal = ERROR` 是安全的，别动。

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
printf() → _write() → usart_log_write() → TX 环形队列 → DMA/IRQ → UART 引脚
```

munit 的 `MUNIT_OUTPUT_FILE`（默认 stdout）自动走此路径。

### 3.2 arg/argv 输入

UART CLI 框架（`cli_tokenize()` + `cli_execute_command()`）负责将串口输入切分为 args，注册 `test` 命令后直接调用 `munit_suite_main`：

```
ssd> test --seed 0x1234 /fc-nand/pure/cmd_validate_op_range
  → argv = {"test", "--seed", "0x1234", "/fc-nand/pure/cmd_validate_op_range"}
  → munit_suite_main(&g_fc_nand_suite, NULL, 4, argv)
```

### 3.3 断言失败处理（v1.2 起：munit 回归上游，终端路径由断言设施承接）

v1.2 起 `munit.c` 不再内联项目专有逻辑，`munit_errorf_ex` 只做上游的「打印 + `abort()`」：

```
断言失败 → munit_errorf_ex
          → munit_logf_exv(...) 打印 ERROR 日志（异步 UART 队列）
          → abort()                          ← 固件里被 assertion.c 重定向
              → crash_record_abort()      ← 写 .noinit 崩溃记录（kind=ABORT）
              → assertion_terminate():
                  assertion_platform_context() ← 弱钩子：FC 追加 [RES] 资源快照
                  log_flush(1000ms)        ← 关键：把异步 UART 队列排空
                  assertion_console_run()  ← cli.c 强符号
                      → while (1) { cli_service_poll(); }
                        本次 test 停止，其余用例不继续，CLI 保持可用
```

裸机没有 fork。断言失败后**不接管故障控制台、不复位**：当前调用栈原样保留，
JTAG/SWD 仍可回溯；正常 UART 上进入现场 CLI，可以继续敲命令分析。`wr/rd/tr/fl/wp`
的 handler 自带 `ssd_runtime_pump()` 同步等待，`format`/`test` 原本自带同步泵 ⇒
这些命令在现场 CLI 里也能独立跑完。若 assert 发生在 FTL/FC 内部，storage 命令的泵
会重入失败状态，可能二次断言——恢复仍需复位/重新初始化。测试夹具可能停在半途。

**为什么必须先 flush**：`fprintf(stderr, ...)` 经 `_write()` 落到 UART 的**异步** TX 环形队列，由 DMA/USART 中断慢慢排出。如果先进入 CLI 循环再 flush，失败信息可能混在后续输出里；`assertion_terminate()` 先 `log_flush()` 把队列排空，断言文本和现场日志的顺序才稳定。

**为什么走 `abort()` 重定向**：这样 `munit.c` 能保持近上游（只调标准 `abort()`），而项目专有的 flush + 崩溃记录 + 现场终端全部收敛到断言设施 `src/common/assertion.c` 的固件 `abort()` 覆盖里，两者双向无牵扯。

**与 CPU 异常的区别**：轮询 UART 接管（`fault_port_console_init`、关 TX DMA/中断）只属于
`platform/fault.c` 的 HardFault/MemManage/BusFault/UsageFault 路径；断言路径不走它。
崩溃记录仍写 `.noinit`，复位后由 `[FAULT] previous ...` 报告，或当场用 CLI `crash` 查看。

**`[RES]` FC 资源快照**：原内联在 `munit.c` 里（反向依赖 `fc_get_resource_stats`）。v1.2 迁到 `src/fc/fc.c`（`assertion_platform_context` 钩子），覆盖断言设施的弱钩子 `assertion_platform_context()`——依赖方向变为 FC → assertion。

### 3.4 无 fork 隔离的约束

因为没有 fork，一个测试的断言失败会中止当前 `test` 命令：断言现场终端接管正常 CLI，失败用例的 `teardown` 不会执行，夹具可能留下半完成状态。后续用例需要重新发起 `test`，必要时先 `format`/复位。因此推荐：
- **一个测试函数只验证一个行为**
- 测试间不共享可变全局状态
- setup/teardown 必须正确回收资源

---

> 修订历史已移入 `docs/archive/ut_revision_history.md`。
