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

`log_printf` / `log_vprintf` / `log_write` 只表达副作用，**不返回逐次结果**；写入失败
由内部计数，用 `log_get_write_failure_count()` 观测（H38：无人消费的返回值不做 API）。

## 3. 级别与编译期裁剪

| 级别 | 宏 | 说明 |
|---|---|---|
| `LOG_ERROR` | `LOGE` | 必须保留 |
| `LOG_WARNING` | `LOGW` | 必须保留 |
| `LOG_INFO` | `LOGI` | 正常运行信息 |
| `LOG_DEBUG` | `LOGD` | **release（`NDEBUG`）下整段裁掉**，参数不求值 |
| `LOG_TRACE` | `LOGT` | 数据面逐事件 trace；**只在 DEBUG 构建里编译**，CLI `log level trace` 显式打开 |
| `LOG_OFF` | — | 全部静默；CLI `log level` 不暴露，供 UT `--log-project` 使用 |

- `LOG_TRACE` 用于逐页 / 逐写缓冲 / 逐请求等**默认不该出现**的事件；稳定态日志
  （生命周期、状态跃迁、失败归因）不得使用它。release 下 `LOGT(...)` 与 `LOGD(...)`
  一样整段裁掉。

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
  调用点必须再 gate `age >= LOG_RATE_SLOW_AFTER_MS`（5 s）：正常 job 不打，
  只有持续 ≥5 s 才每 5 s 一行。
- `log_ratelimit_*` / `log_rate_due()`：同类异常短时间重复时限制输出，避免写满 TX 环。

## 7. 日志内容契约（允许期间打什么）

一条日志只在**它能改变判读**时存在：能回答「哪个 job / 哪一步 / 多久 / 结果」。
只回答「发生了多少次」的，进 `stats`，不进日志。

| 层 | 级别 | 内容 | 预算 |
|---|---|---|---|
| 生命周期 | INFO | boot 横幅、NAND/FC/FTL 拓扑、boot type/replay、format/mount、终端 IO_ERROR | 每 boot ≤80 行 |
| 状态跃迁 | DEBUG | CP done、GC emergency enter/exit、no-gain suspend/resume、GC job start/end/abort、META compact、L2P load/flush 失败、RR/WL 触发、host IO 失败 | 边沿触发；单模块 ≤1 行/s |
| 数据面 trace | TRACE | `wbuf flush`、`hdr_prep`、逐 host 请求、逐 page/plane 细节 | 默认关 |

规则：

- **边沿触发**：只在状态变化时打，不按求值/轮询节拍打快照（`[GC] trig` 已删除）。
- **失败在起源处打**，原因用与统计同源的枚举 / 短名（`gate` / `submit` / `no_command`），
  不另写一套字符串。
- **限流行必须带 `suppressed=N`**：N = 自上次放行以来被抑制的条数。不刷屏 ≠ 看不见量级。
  写法用 `LOGx_RL(&rl, fmt, ...)`（`log_ratelimit_s *`）或
  `LOGx_RATE(cond, suppressed_expr, fmt, ...)`（raw pair / 额外前置条件）：
  判据、计数、打印在一行，`format` 不写 `\r\n`（宏统一追加
  ` suppressed=%lu\r\n`）。两行共享一次判据时保留显式 `if`（如 no-space 的
  归因行 + stall 行），并在代码里注明原因。
- **分支体只有日志时用 `LOGx_IF`**：`if (cond) { LOGx(...); }` 收成
  `LOGx_IF(cond, ...)`；条件本身是动作（`init/add/load/register`）时必须保留显式
  `if`，不能把动作藏进日志宏。`if/else` 成对、计数型限流
  （`s_logged++ % N`）、心跳判据（`log_heartbeat_due`）也保留显式块。
- **不逐页 / 逐轮**：`wbuf flush`、per-CP start、逐页 move 归 TRACE；成功 host 请求不打。
  `wbuf flush` 的 TRACE 行带 `lba/n_cw/planes/pba0/pba1`；status/ms 由完成/失败路径打。
- **不周期打印**：`[STAT]` 10 s 心跳已删；需要时间序列用 `stats --delta`。
- **失败只打起源**：host 失败回执在 HIL 组完成处打 `[CTRL] host op/lba/n/status`
  （限流）；USB/FLUSH/TRIM 超时仍由各自路径打。
- **预期背压慢节拍**：L2P gate/submit 背压 10 s 一行，`suppressed=N` 携带量级；
  其他 L2P 加载失败 1 s。
- **常量不在每 boot 重复**：raw path / BCH ready / topology dies / contract ok /
  CAP plane_usable_min / GHWCFG2 / STK guard armed / timer task table 等常量行已删；
  只保留会变化的状态与故障。
- **时间戳由串口工具提供**，固件不写（`ISS-OBS-07` 裁决）；日志行本身不带时间。
- 每行 ≤192 B；长表格拆行（`stats` 已按行输出）。

预算判据：一次满盘 H2testw 的 DEBUG 日志目标 **≤800 行**（boot ≈60 + 状态跃迁 ≈500）；
超过时优先怀疑「把计数 / 快照写进了日志」，而不是去调限流。

## 8. 调用约束

- **禁止在 ISR 中调用**（`LOG*`、`ASSERT` 都不行）；违反会记入 `tx_context_errors`。
- **停机前必须 flush**：断言/故障路径先 `log_flush()` 再停；顺序收敛在
  `assertion_terminate()`。详见 [`assertion_guidelines.md`](assertion_guidelines.md) 与
  [`fault_capture.md`](fault_capture.md)。
- UT 期间项目日志默认 `LOG_OFF`：`test --log-project info|debug` 才打开；
  munit 自己的证据输出由 `--log-visible` 控制，两者独立。
- 日志不承担同步/时序语义；需要顺序保证时由调用点显式 flush 或走同步路径。

## 9. 代码与相关文档

| 项 | 位置 |
|---|---|
| 接口 | `include/common/log.h` |
| 门面实现 | `src/common/log.c` |
| UART 传输（环、DMA、flush） | `src/platform/usart.c`、`include/platform/usart.h` |
| 停机顺序 / 现场 CLI | [`fault_capture.md`](fault_capture.md)、[`cli.md`](cli.md) |
