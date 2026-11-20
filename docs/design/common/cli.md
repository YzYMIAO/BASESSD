# UART CLI（调试前端）

> 本文描述 `src/hil/cli.c` 的**当前设计**。传输层（RX/TX 环、DMA）在
> `src/platform/usart.c`；命令经 `controller` 驱动 FTL 全链路。

## 1. 定位

- CLI 是 HIL 的调试前端：消费 `uart_rx_read` / `uart_log_write`，驱动
  `ssd_controller_*` / FTL。
- 它是**唯一直接引用 `g_ctrl`** 的调试入口（USB MSC 走自己的 host 队列）。
- 命令处理函数返回 `void`：失败原因由 handler 自己打印，dispatcher 只做语法与
  策略检查，不做返回码分派。

## 2. 主循环集成

| 函数 | 时机 | 约束 |
|---|---|---|
| `cli_init()` | `sys_init()` 之后、superloop 之前 | 预填 `g_wr_buf` 的 0x55/0xAA pattern；`.sram2_data` 是 NOLOAD，必须主动填 |
| `cli_service_poll()` | 主循环每轮 | **绝不能在 ISR 中调用**；负责回显、组行、执行命令、轮询未决请求 |
| `cli_host_busy()` | USB MSC 查询 | 返回「CLI host 请求在飞」或「`wp` 组在飞」；两者共用 host busy gate |

## 3. 行输入与解析

- 行缓冲 `USART_CLI_LINE_CAPACITY = 128 B`；超过时进入丢弃状态直到下一个 `\n`，
  避免截断的半行被当成完整命令执行。
- 单条命令最多 `UART_CLI_MAX_ARGUMENTS = 24` 个 token（`test` 的 `--param` 与过滤串需要）。
- 提示符固定 `ssd> `；DWORD 参数走 `uart_cli_parse_dword`，非法输入由 handler 报错。

## 4. 命令表模型

```c
typedef struct UART_CLI_COMMAND_S {
    const char *name;
    const char *syntax;   /* <required> / [optional] / 子命令 */
    const char *help;
    BYTE min_argc, max_argc;
    BYTE flags;           /* UART_CLI_FLAG_* */
    BYTE group;           /* UART_CLI_GROUP_* */
    uart_cli_handler_fn_t handler;
} uart_cli_cmd_s;
```

- **分组**只决定 `help` 的分节顺序：`system` / `host_io` / `diag` / `destructive` / `test`。
- **行为位**（本仓自拟）：
  - `UART_CLI_FLAG_BUSY`：提交前要求 controller 无在飞 host 请求；
  - `UART_CLI_FLAG_DESTRUCTIVE`：破坏性命令；release（`NDEBUG`）下整行不注册。
- `test` 只在 `FC_NAND_TEST_ENABLE` 构建中存在；`format`/`bbt erase` 只在非 `NDEBUG`
  构建中存在。

## 5. 命令清单

| 组 | 命令 | 作用 |
|---|---|---|
| system | `help` / `status` / `version` / `log` / `stack` / `crash` / `pc` / `info` / `stat` / `id` | 帮助、UART 状态、日志级别、栈守卫、崩溃记录、PC 采样、控制器/FTL 身份与计数 |
| host I/O | `wr` / `rd` / `tr` / `fl` / `wp` | 固定 pattern 写读、trim、L2P+Journal flush、页打包写缓冲诊断 |
| diag | `q` / `cp` / `jrn` / `ftl_ctx` / `map` / `addr` / `rdpaa` / `pool` / `bbt` / `ecc` / `wear` | 队列/三池、Checkpoint、Journal、FTL 现场、L2P 映射、地址转换、按 PAA 读页、块池、坏块图、ECC 健康、磨损分布 |
| destructive | `format` | 全片擦除 + 写初始元数据；release 档不注册 |
| test | `test` | 跑 μnit 套件；语法见 `include/hil/cli.h` 与 `--help` |

## 6. 执行语义

- **同步命令自带泵**：`wr/rd/tr/fl/wp` 等 handler 内部调 `ssd_runtime_pump()` 等待完成，
  命令返回时请求已结束。因此断言现场 CLI 里这些命令仍能独立跑完。
- `wr/rd` 使用 `g_wr_buf`（约 16 KiB，AXI `.bss`）；`n` 超过容量直接拒绝。
  ⚠ `rd` 会把读到的数据写回同一块缓冲，**覆盖 pattern**；需要再次 `--compare` 前先重写。
- `wp` 组直连 `ftl_submit_write`，不占 host 队列，但计入 `cli_host_busy()`。

## 7. 断言现场 CLI

`assertion_console_run()` 覆盖断言设施的弱钩子：

- 清空行编辑残留（防止断言发生在命令执行中途时，残留 CR/LF 把同一条命令重跑）；
- 把日志级别临时提到 `LOG_INFO`；
- 打印提示后进入 `for(;;) cli_service_poll();`，**冻结 FTL/FC 后台推进**。

命令里只有自带泵的同步命令能完整执行；依赖后台服务的命令在冻结状态下不会前进。

## 8. 日志与测试选项

- `log level <error|warn|info|debug|trace>`：运行期调整项目日志级别。`trace` 只在
  DEBUG 构建里有输出（数据面逐事件；稳定态日志不使用）。
- `test --log-visible info|debug`：munit 自己的证据输出（`[EV]`），与项目日志独立。
- `test --log-project info|debug`：打开项目日志；默认 `LOG_OFF`，避免全量 UT 时
  UART 成为瓶颈。

## 9. 约束与已知限制

- 命令行 128 B / 24 token；超长命令被整行丢弃。
- 破坏性命令在 release 镜像中不可用（编译期不注册）。
- CLI 与 USB MSC 共用 host busy gate；CLI 未决请求会让 USB 侧让路。
- ISR 中不得调用任何 CLI/日志接口。
