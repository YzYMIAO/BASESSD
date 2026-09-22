# FC-NAND 模块单元测试设计方案

**日期**: 2026-09-22

---

## 1. 概述

本文档记录 soft_ssd 项目中 FC（Flash Controller）与 NAND 底层驱动模块的 munit 单元测试设计方案。测试目标平台为 **STM32H743** 裸机，通过 `make tests` 交叉编译进固件，下载后通过 UART CLI 触发执行。

测试框架基于 **munit v0.4.1**（位于 `tests/framework/munit.c/h`），在嵌入式环境中做了以下适配：

| 适配项 | 说明 |
|---|---|
| **无 fork 隔离** | 裸机没有子进程，断言失败后 `munit_errorf_ex` 执行 `while(1)` 挂起，保留现场供调试器检查 |
| **stdout 重定向** | `_write()` → `uart_log_write()` 经 UART 输出，`printf` / `fprintf` 自动走串口 |
| **argc/argv 输入** | UART CLI 的 `uart_cli_tokenize()` 解析串口输入为 argv，调用 `munit_suite_main` |
| **无 `abort()`** | `munit_logf_ex` 中移除 `abort()`，断言失败的统一由 `while(1)` 处理 |

---

## 2. 测试分层

参考 [UT Plan FC-NAND munit](ut-plan-fc-nand-munit.md)，测试分三个层级：

```
Level 1: Pure Logic ──── 零硬件依赖，纯函数测试
    ├── fc_command validate / status
    ├── fc_queue push / pop / peek / pop_n / full / empty
    └── fc_scheduler init / select / select_chain / busy

Level 2: Data Path ──── 需 BCH 堆初始化，无 NAND 硬件
    ├── fc_data_path_encode (random/ecc/raid on/off 组合)
    ├── fc_data_path_process_read (全 OK / 有 UNC)
    └── fc_data_path_raid_recover (XOR 重建 + ECC 重试)

Level 3: FC Lifecycle ── 真实 NAND 硬件执行
    └── 34 个 OP 码 × user-specified block → alloc → configure → submit → service → poll → release
```

### 2.1 Level 1 — 纯逻辑测试

**测试目标**：`fc_command.c`、`fc_queue.c`、`fc_scheduler.c` 中的纯逻辑函数，不依赖任何硬件或动态内存。

| 测试函数 | 被测函数 | 验证点 |
|---|---|---|
| `test_command_validate_null` | `fc_command_validate` | NULL → INVALID_ARGUMENT |
| `test_command_validate_op_range` | `fc_command_validate` | 0..OP_COUNT-1 合法，≥OP_COUNT 拒绝 |
| `test_command_validate_desc_needed` | `fc_command_validate` | has_data + desc_id=INVALID → 拒绝 |
| `test_command_validate_set_get_no_desc` | `fc_command_validate` | SET/GET_FEATURES 内联例外 |
| `test_command_status_init_null` | `fc_command_status_init` | status=NULL 不崩溃 |
| `test_command_status_init_cw1` | `fc_command_status_init` | 1 CW → status1/2/3=UNUSED |
| `test_command_status_init_cw4` | `fc_command_status_init` | 4 CW → 全部 READY |
| `test_command_status_set_cw_valid` | `fc_command_status_set_cw` | 各 CW slot + 错误位记录 desc_id |
| `test_command_status_set_cw_overflow` | `fc_command_status_set_cw` | cw≥4 不写入 |
| `test_command_status_set_cw_null` | `fc_command_status_set_cw` | status=NULL 不崩溃 |
| `test_queue_push_pop` | fc_submission_queue push/pop | FIFO 顺序正确 |
| `test_queue_wrap_around` | fc_submission_queue push/pop | 环形环绕后 FIFO 正确 |
| `test_queue_overflow` | fc_submission_queue_push | 满队列返回 QUEUE_FULL |
| `test_queue_peek` | fc_submission_queue_peek | 不消耗元素 |
| `test_queue_peek_empty` | fc_submission_queue_peek | 空队列返回 INVALID |
| `test_queue_pop_n` | fc_submission_queue_pop_n | 批量弹出 N 个 |
| `test_queue_empty_full` | fc_submission_queue 状态 | peek 空 / push 满 |
| `test_queue_init_null` | fc_submission_queue_init | NULL 输入返回错误 |
| `test_queue_push_null` | fc_submission_queue_push | NULL 输入返回错误 |
| `test_queue_pop_null` | fc_submission_queue_pop | NULL 输入返回错误 |
| `test_queue_peek_null` | fc_submission_queue_peek | NULL 返回 INVALID |
| `test_queue_pop_n_null` | fc_submission_queue_pop_n | queue=NULL 不崩溃 |
| `test_scheduler_init` | `fc_scheduler_init` | 初始化后状态正确 |
| `test_scheduler_init_null` | `fc_scheduler_init` | NULL 输入返回错误 |
| `test_scheduler_set_clear_busy` | `fc_scheduler_set/clear_busy` | set→clear 状态切换 |
| `test_scheduler_select_empty` | `fc_scheduler_select` | 空队列返回 QUEUE_EMPTY |
| `test_scheduler_select_hsq_first` | `fc_scheduler_select` | HSQ 优先于 SQ |
| `test_scheduler_select_sq_order` | `fc_scheduler_select` | SQ FIFO 顺序 |
| `test_scheduler_select_busy_skip` | `fc_scheduler_select` | LUN 忙 → 旋转 → NOT_READY |
| `test_scheduler_select_chain_single` | `fc_scheduler_select_chain` | 单命令 chain |
| `test_scheduler_select_chain_multi` | `fc_scheduler_select_chain` | 3 命令链收集 |

**共计 31 个测试函数。**

### 2.2 Level 2 — 数据路径测试

**测试目标**：`fc_data_path.c` 中的编解码管线。需要 `fc_init()` 初始化资源池，但不需要 NAND 硬件——全部在 buffer 层面做 round-trip 验证。

Level 2 不关心 OP 码（operation 字段），只测试 data_path 内部的数据变换逻辑。三个被测函数：

| 函数 | 功能 | 读取的 config 字段 |
|---|---|---|
| `fc_data_path_encode` | 写路径：Random 加扰 → ECC 编码 → RAID XOR | `random`, `ldpc`, `raid`, `cw` |
| `fc_data_path_process_read` | 读路径 Stage1：ECC 解码 → 条件解扰 | `ldpc`, `random`, `cw` |
| `fc_data_path_raid_recover` | 读路径 Stage2：RAID XOR 重建 → ECC 重试 → 解扰 | `ldpc`, `random` |

> **注意**：实现过程中发现 `fc_data_path.c` 原有的 encode/decode/raid_recover 三处 ECC 调用**没有读取 `config.ldpc` 字段**，始终执行 ECC。已在实现时补上 `ldpc == FC_ECC_MODE_LDPC_RAW` 时的跳过逻辑。

### 参数组合

`config.ldpc` 字段影响 ECC 路径（BCH / LDPC_RAW 跳过），但并不作为测试参数化维度（当前编译配置默认 BCH）。

| 字段 | 有效取值 | 影响 |
|---|---|---|
| `random` | `FC_RANDOM_ENABLE(0)` / `FC_RANDOM_DISABLE(1)` | 是否加扰/解扰 |
| `raid` | `FC_RAID_MODE_NORMAL(0)` / `FC_RAID_MODE_ENCODE(2)` | encode 是否做 RAID XOR |
| `cw` | 0 (1CW) / 3 (4CW) | desc 链长度，多 CW 边界 |
| ECC 结果 | 全部成功 / 有 UNC | process_read 是否走解扰 |

### 测试用例

| 测试函数 | 参数组合 | 验证策略 |
|---|---|---|
| `encode_random_off` | random=off, 4CW, raid=normal | ECC 编码后末 8B parity 非零；round-trip decode 恢复原文 |
| `encode_random_on` | random=on, 4CW, raid=normal | 加扰 + ECC；decode + 解扰后恢复原文 |
| `encode_raid_on` | random=on, 4CW, raid=encode | 全路径编码；清空 CW0 → RAID recover → 恢复 |
| `encode_single_cw` | random=off, 1CW, raid=normal | 单 CW 边界 |
| `encode_null` | command=NULL | NULL 输入 |
| `process_read_ok_random_on` | random=on, 4CW, 全 OK | 解码 + 解扰后原文恢复 |
| `process_read_ok_random_off` | random=off, 4CW, 全 OK | 只解码，不涉及解扰 |
| `process_read_unc` | random=on, 4CW, CW1 损坏 | 返回 IO_ERROR；desc status CW1=UNC, 其余=READY |
| `process_read_single_cw` | random=on, 1CW, 全 OK | 单 CW 边界 |
| `process_read_null` | command=NULL | NULL 输入 |
| `raid_recover_first` | random=on, 4CW, failed_idx=0 | 恢复首个 CW |
| `raid_recover_mid` | random=on, 4CW, failed_idx=1 | 恢复中间 CW |
| `raid_recover_last` | random=on, 4CW, failed_idx=3 | 恢复末位 parity CW |
| `raid_recover_no_random` | random=off, 4CW, failed_idx=1 | 无加扰恢复 |
| `raid_recover_null` | cmd=NULL | NULL 输入 |

**共计 15 个测试函数。**

### 2.3 Level 3 — FC 全链路测试

**测试目标**：FC 命令完整生命周期（alloc → configure → submit → service → poll → release）在真实 NAND 硬件上执行，覆盖全部 34 个 OP 码。

Level 3 不验证数据变换正确性（那是 Level 2 的职责），只验证 FC 层命令编排的正确性：
- 每个 OP 码的命令能完整走完 FC 生命周期
- 各阶段状态转换正确（不崩溃、不泄漏）
- completion->command_id 与提交的命令匹配
- 资源（command/descriptor/buffer）无泄漏

#### 参数化维度

| 参数 | 来源 | 说明 |
|---|---|---|
| `operation` | munit 参数化展开 | 34 个 OP 码（笛卡尔积） |
| `block` | CLI `--param block N` | 测试用 Block 号，默认 block=10 |

#### PAA 约定

```
ch=0 ce=0 lun=0 plane=0 block=<user> page=0 cw=0
多 Plane 操作使用 plane=0/1 双 Plane 地址
```

#### 测试用例

```c
test_lifecycle_op — 参数化，每个 (operation, block) 组合执行一次生命周期
```

`test_lifecycle_op` 在运行时根据 OP 属性做不同配置：

| OP 类别 | 配置方式 |
|---|---|
| SET/GET_FEATURES | 内联数据（parameter0..3），不配置 desc chain |
| has_data 且非 SET/GET | `fc_command_configure_descriptors()` 分配 desc + buffer，填 pattern |
| 无数据 + needs_paa | `desc_id=INVALID`，设置 PAA |
| 无数据 + 不需 PAA | `desc_id=INVALID`，PAA=0 |

#### 前置状态依赖

部分 OP 有 NAND 状态机前置依赖（如 `RANDOM_DATA_READ` 前需要 READ 先加载页缓冲）。表中 `standalone=0` 的 OP 在独立执行时 NAND 可能返回错误，但 FC 生命周期仍应正常完成。

#### 34 个 OP 覆盖表

| 值 | 宏名称 | standalone | 说明 |
|---|---|---|---|
| 0 | `OP_RESET` | ✅ | 复位 NAND |
| 1 | `OP_READ_ID` | ✅ | 读 ID |
| 2 | `OP_READ_PARAMETER_PAGE` | ✅ | 读参数页 |
| 3 | `OP_READ_UNIQUE_ID` | ✅ | 读唯一 ID |
| 4 | `OP_GET_FEATURES` | ✅ | 读 Feature |
| 5 | `OP_SET_FEATURES` | ✅ | 写 Feature |
| 6 | `OP_READ_STATUS` | ✅ | 读状态 |
| 7 | `OP_READ_STATUS_ENHANCED` | ✅ | 增强读状态 |
| 8 | `OP_RANDOM_DATA_READ` | ❌ | 随机列读 |
| 9 | `OP_RANDOM_DATA_INPUT` | ❌ | 随机列输入 |
| 10 | `OP_PROGRAM_FOR_INTERNAL_DATA_MOVE` | ❌ | 内部搬移编程 |
| 11 | `OP_READ_MODE` | ✅ | 读模式 |
| 12 | `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | ✅ | 标准读（有数据输出） |
| 13 | `OP_READ_PAGE_CACHE_SEQ` | ❌ | 缓存读顺序 |
| 14 | `OP_READ_PAGE_CACHE_RANDOM` | ❌ | 缓存读随机 |
| 15 | `OP_READ_PAGE_CACHE_LAST` | ❌ | 缓存读最后 |
| 16 | `OP_NORMAL_PROGRAM_PAGE` | ✅ | 标准编程 |
| 17 | `OP_PROGRAM_PAGE_CACHE` | ❌ | 缓存编程 |
| 18 | `OP_ERASE_BLOCK` | ✅ | 擦除 Block |
| 19 | `OP_READ_FOR_INTERNAL_DATA_MOVE` | ❌ | 内部搬移读 |
| 20 | `OP_PROGRAM_FOR_INTERNAL_MOVE` | ❌ | 内部搬移写 |
| 21 | `OP_BLOCK_UNLOCK_LOW` | ✅ | 解锁低段 |
| 22 | `OP_BLOCK_UNLOCK_HIGH` | ✅ | 解锁高段 |
| 23 | `OP_BLOCK_LOCK` | ✅ | 锁定 |
| 24 | `OP_BLOCK_LOCK_TIGHT` | ✅ | 紧锁定 |
| 25 | `OP_BLOCK_LOCK_STATUS` | ✅ | 锁定状态 |
| 26 | `OP_OTP_DATA_LOCK` | ❌ | OTP 锁定 |
| 27 | `OP_OTP_DATA_PROGRAM` | ❌ | OTP 编程 |
| 28 | `OP_OTP_DATA_READ` | ❌ | OTP 读 |
| 29 | `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT` | ❌ | 多 Plane 读无数据 |
| 30 | `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | ❌ | 多 Plane 读有数据 |
| 31 | `OP_PROGRAM_PAGE_MULTI_PLANE` | ❌ | 多 Plane 编程 |
| 32 | `OP_ERASE_BLOCK_MULTI_PLANE` | ❌ | 多 Plane 擦除 |
| 33 | `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | ✅ | 标准读（无数据输出） |

---

## 3. 测试套件组织

```c
/* test_fc_level1.c — 三个子套件，共 31 个测试 */

static MunitTest g_fc_command_tests[] = {
    { "validate_null",           test_command_validate_null,        NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "validate_op_range",       test_command_validate_op_range,    NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "validate_desc_needed",    test_command_validate_desc_needed, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "validate_set_get_no_desc", test_command_validate_set_get_no_desc, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "status_init_null",        test_command_status_init_null,     NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "status_init_cw1",         test_command_status_init_cw1,      NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "status_init_cw4",         test_command_status_init_cw4,      NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "status_set_cw_valid",     test_command_status_set_cw_valid,  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "status_set_cw_overflow",  test_command_status_set_cw_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "status_set_cw_null",      test_command_status_set_cw_null,   NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};

static MunitTest g_fc_queue_tests[] = {
    { "push_pop",    test_queue_push_pop,    NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "wrap_around", test_queue_wrap_around, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "overflow",    test_queue_overflow,    NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "peek",        test_queue_peek,        NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "peek_empty",  test_queue_peek_empty,  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "pop_n",       test_queue_pop_n,       NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "empty_full",  test_queue_empty_full,  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "init_null",   test_queue_init_null,   NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "push_null",   test_queue_push_null,   NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "pop_null",    test_queue_pop_null,    NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "peek_null",   test_queue_peek_null,   NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "pop_n_null",  test_queue_pop_n_null,  NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};

static MunitTest g_fc_scheduler_tests[] = {
    { "init",              test_scheduler_init,              NULL,                  NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "init_null",         test_scheduler_init_null,         NULL,                  NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "set_clear_busy",    test_scheduler_set_clear_busy,    NULL,                  NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "select_empty",      test_scheduler_select_empty,      setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "select_hsq_first",  test_scheduler_select_hsq_first,  setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "select_sq_order",   test_scheduler_select_sq_order,   setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "select_busy_skip",  test_scheduler_select_busy_skip,  setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "select_chain_single", test_scheduler_select_chain_single, setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "select_chain_multi",  test_scheduler_select_chain_multi,  setup_scheduler_suite, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};

static MunitSuite g_fc_nand_suite = {
    (char *)"/fc-nand/",
    NULL,
    (MunitSuite[]){
        { (char *)"command",   g_fc_command_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"queue",     g_fc_queue_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"scheduler", g_fc_scheduler_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"data_path", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"lifecycle", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE },
    },
    1,
    MUNIT_SUITE_OPTION_NONE,
};
```

```c
/* test_fc_level2.c — data_path 子套件，共 15 个测试 */
extern MunitTest g_fc_data_path_tests[];

/* test_fc_level3.c — lifecycle 子套件，1 个参数化测试展开为 34 个 OP */
extern MunitTest g_fc_lifecycle_tests[];

---

## 4. 测试入口与集成

### 4.1 裸机入口

在 `tests/unit/ut_main.c` 中提供 `ut_run_all()`，供 UART CLI 调用：

```c
#include "munit.h"

extern MunitTest g_fc_command_tests[];
extern MunitTest g_fc_queue_tests[];
extern MunitTest g_fc_scheduler_tests[];
extern MunitTest g_fc_data_path_tests[];
extern MunitTest g_fc_lifecycle_tests[];

static MunitSuite g_fc_nand_suite = {
    (char *)"/fc-nand/", NULL,
    (MunitSuite[]){
        { (char *)"command",   g_fc_command_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"queue",     g_fc_queue_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"scheduler", g_fc_scheduler_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"data_path", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"lifecycle", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE },
    },
    1, MUNIT_SUITE_OPTION_NONE,
};

int ut_run_all(int argc, char *argv[])
{
    return munit_suite_main(&g_fc_nand_suite, NULL, argc, argv);
}
```

配套头文件 `tests/unit/ut_main.h` 声明该接口供 `usart.c` 包含。

### 4.2 UART CLI 集成

在 `stm32h743/usart.c` 中加入 `test` 命令：

```c
#if defined(FC_NAND_TEST_ENABLE)
#    include "ut_main.h"
static int uart_cli_test(int argc, char *argv[]);

/* ... 命令表中注册 test 命令 ... */
{ "test", "test [--seed N] [--list] [test_name...]", "run munit unit test suite", 1U, 10U, uart_cli_test },

static int uart_cli_test(int argc, char *argv[])
{
    return ut_run_all(argc, argv);
}
#endif
```

UART 串口输入示例：

```
ssd> test --seed 0x1234 /fc-nand/command/validate_null
ssd> test --list
ssd> test --list-params
ssd> test
```

### 4.3 构建集成

在 `src/fc/makefile.mk` 中条件加入测试源码：

```makefile
ifeq ($(FC_NAND_TEST_ENABLE),1)
TEST_C_SRCS += $(PROJECT_ROOT)/tests/framework/munit.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level1.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level2.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level3.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/ut_main.c
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
endif
```

---

## 5. 测试编写规范

### 5.1 文件组织

```
tests/
├── framework/
│   ├── munit.c            # 测试框架实现（已修复 15 项）
│   ├── munit.h            # 测试框架头文件
│   └── README.md          # 框架边界说明
├── unit/
│   ├── ut_main.c          # 裸机统一入口
│   ├── ut_main.h          # 入口声明（供 usart.c 包含）
│   ├── test_fc_level1.c   # Level 1: 31 个测试（command + queue + scheduler）
│   ├── test_fc_level2.c   # Level 2: 15 个测试（data_path encode/process_read/raid_recover）
│   └── test_fc_level3.c   # Level 3: 1 个参数化测试（34 OP 全生命周期真实 NAND）
├── integration/
│   ├── test_fc_nand.c     # 旧硬件集成测试（暂保留，待废弃）
│   └── test_fc_nand.h
└── Makefile               # host-only 构建（不变）
```

### 5.2 断言使用

- **优先使用 `munit_assert_*` 宏**，不直接使用 `assert.h`
- 类型明确时使用带类型的断言（`munit_assert_uint32`、`munit_assert_size` 等）
- 字符串比较使用 `munit_assert_string_equal` / `not_equal`
- 内存块比较使用 `munit_assert_memory_equal` / `not_equal`
- 指针 NULL 检查使用 `munit_assert_null` / `not_null`
- **禁用 `MUNIT_ENABLE_ASSERT_ALIASES`**，避免污染`assert`宏

### 5.3 测试函数签名

```c
static MunitResult test_fc_queue_push_pop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    /* setup (if needed) */
    /* exercise */
    /* verify */
    return MUNIT_OK;
}
```

- 测试函数使用 `static` 链接属性
- 参数/返回值使用 `void`，除非需要 fixture
- 成功返回 `MUNIT_OK`，失败通过断言宏或显式返回 `MUNIT_FAIL`

---

## 6. 注意事项

### 6.1 BCH 引擎全局变量

`g_bch` 是全局状态，`fc_ecc_init_bch` 只在首次分配时初始化，跨测试用例持续存在。Level 2 测试需要在第一个测试前完成初始化，不应在每个测试的 setup 中重复初始化。

### 6.2 资源池状态

`fc_command_pool` / `fc_descriptor_pool` / `fc_buffer_pool` 是全局静态池。Level 1 测试涉及纯逻辑函数时不直接依赖池内容；Level 3 测试需要 `fc_init()` 预先初始化。

### 6.3 CQ entry slot 限制

CQ entry 只有 4 个 status slot（`status0..3`），`CW_NUM=4`。超出的 slot 应填 `FC_CW_STATUS_UNUSED`。

### 6.4 断言失败的现场保留

裸机环境下断言失败后会卡在 `while(1)`，不会继续执行后续测试。因此：
- 一个测试文件内建议的测试粒度为**一个 test 函数只验证一个行为**
- 使用 `--fatal-failures` 可以遇错即停（裸机下这是默认行为）

---

## 7. 修订历史

| 日期 | 版本 | 变更描述 |
|---|---|---|
| 2026-09-22 | v1.3 | Level 3 实现：34 个 OP 全生命周期测试编译通过。参数化 `test_lifecycle_op`，CLI 传入 block，PAA 约定。设计文档同步更新用例表、OP 覆盖表、套件模板、文件组织 |
| 2026-09-22 | v1.2 | Level 2 实现：15 个测试编译通过。同步 config.ldpc 判断（encode/decode/raid_recover 三处），用例表、套件模板、构建配置、文件组织 |
| 2026-09-22 | v1.1 | Level 1 实现：31 个测试编译通过。文件合并、UART CLI 集成、旧代码残留清理 |
| 2026-09-22 | v1.0 | 初始版本：测试分层、用例清单、套件组织、集成方案 |