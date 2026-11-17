# 日志设施（logging）

> 本文描述全工程日志门面的**当前设计**。接口在 `include/common/log.h`，实现在
> `src/common/log.c`；传输在平台层（`src/platform/usart.c`）。

## 1. 定位

- 业务模块只调用 `LOGE/LOGW/LOGI/LOGD` 或 `LOG_IF` 系列，**不直接操作 UART、文件描述符
  或平台寄存器**。
- 日志门面只做「级别过滤 + 格式化 + 截断」；**传输策略属于 sink**：
  - 固件在启动时把 sink 绑到 UART（`uart_log_write`）；
  - 裸机早期可用 `log_set_sink()` 在平台注册前先有输出。

## 2. 数据流

```
LOGx / LOG_IF
  → log_printf / log_vprintf        （级别过滤、192 B 栈缓冲、vsnprintf）
  → log_write                       （截断检测、写失败计数）
  → sink（固件 = uart_log_write）   → 4096 B TX ring → DMA 或 TXE 中断回退
```

所以「日志被级别过滤掉」时**不进入格式化、也不占 TX 环空间**；这是把过滤放在格式化
之前的原因。

## 3. 级别与编译期裁剪

| 级别 | 宏 | 说明 |
|---|---|---|
| `LOG_ERROR` | `LOGE` | 必须保留 |
| `LOG_WARNING` | `LOGW` | 必须保留 |
| `LOG_INFO` | `LOGI` | 正常运行信息 |
| `LOG_DEBUG` | `LOGD` | **release（`NDEBUG`）下整段裁掉**，参数不求值 |
| `LOG_OFF` | — | 全部静默；CLI `log level` 不暴露，供 UT `--log-project` 使用 |

- `LOG_IF(level, cond, fmt, ...)`：`cond` 为假时**完全不求值**格式参数；分支体只有日志时
  用它，避免显式 `if` 块。
- 模块标签固定 4 字符（`LOG_TAG_*`），`[ERR]/[WRN]/[INF]/[DBG]` 固定 3 字符；
  `[FAULT]/[BOUND]` 是类别标签，不参与对齐。

## 4. 长度与截断

- 单条格式化结果上限 **192 B**（`log_vprintf` 的栈缓冲）。
- 超长时追加 `...[truncated]`，并递增 `log_get_truncated_count()`。
- 单行日志应明显小于 192 B；长表格/多字段 dump 应拆成多行。

## 5. 失败语义与计数

| 计数 | 含义 |
|---|---|
| `log_get_truncated_count()` | 格式化超长被截断的条数 |
| `log_get_write_failure_count()` | sink 未完整接收（返回长度不符）的次数 |
| UART `tx_dropped` | TX 环满，新日志被丢弃 |
| UART `tx_context_errors` | 在 ISR 或关中断上下文写日志 |
| UART `tx_flush_timeouts` | `log_flush` 在超时内未见队列空 + DMA idle |
| UART `tx_transport_errors` | UART/DMA 传输故障，无法证明可靠送达 |

## 6. 节流与心跳

- `LOG_RATE_INTERVAL_MS` / `LOG_RATE_SLOW_AFTER_MS`：默认心跳 1 s；超过 5 s 未推进后
  降到 5 s/行。
- `log_heartbeat_begin/due/end/age_ms`：用于异步 job 的「phase 不动 = 卡死」判据。
- `log_ratelimit_*` / `log_rate_due()`：同类异常短时间重复时限制输出，避免写满 TX 环。

## 7. 调用约束

- **禁止在 ISR 中调用**（`LOG*`、`ASSERT` 都不行）；违反会记入 `tx_context_errors`。
- **停机前必须 flush**：断言/故障路径先 `log_flush()` 再停；顺序收敛在
  `assertion_terminate()`。详见 [`assertion_guidelines.md`](assertion_guidelines.md) 与
  [`fault_capture.md`](fault_capture.md)。
- UT 期间项目日志默认 `LOG_OFF`：`test --log-project info|debug` 才打开；
  munit 自己的证据输出由 `--log-visible` 控制，两者独立。
- 日志不承担同步/时序语义；需要顺序保证时由调用点显式 flush 或走同步路径。

## 8. 代码与相关文档

| 项 | 位置 |
|---|---|
| 接口 | `include/common/log.h` |
| 门面实现 | `src/common/log.c` |
| UART 传输（环、DMA、flush） | `src/platform/usart.c`、`include/platform/usart.h` |
| 停机顺序 / 现场 CLI | [`fault_capture.md`](fault_capture.md)、[`cli.md`](cli.md) |
