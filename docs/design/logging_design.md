# 日志与观测设计（logging design）

> 目的：把「首次故障带全上下文、重复事件只留计数、状态变化立即打、周期性一行摘要」固化成规则。
> 样本来自 2026-10-21 H2testw 失败：Mode 5 运行期读回 UNC → GC 逐个 victim 标 BAD →
> `free` 耗尽 → host 读 FAT 元数据失败 → Windows Code 21；日志同时刷屏。

## 1. 分类：预期 / 条件预期 / 非预期

- **E（预期）**：健康盘或已知降级状态下设计内会发生，代码有明确处置路径。
- **C（条件预期）**：只有明确前提成立时才算预期；前提不成立即升级为 U。日志必须带前提字段。
- **U（非预期）**：实现缺陷、系统性时序/介质问题，或设计没预料到的状态。

级别原则：**分类决定「要不要查」，级别决定「现在要不要人动手」**。预期但造成 host 可见失败的，仍可为 ERROR。

## 2. 需要打的信息

| 类别 | 内容 | 级别 | 频率 |
|---|---|---|---|
| 启动 | reset reason、NAND ID/ONFI、**实际 timing mode**、FC 拓扑、FTL boot type、`[CAP ]`、`[BLK ] bad areas`、USB mounted | INFO | 一次 |
| 格式/体检 | start、每 512 块进度、done tested/bad、`old_bad/recovered/new_bad` | INFO/WARN | 进度 + 汇总 |
| 首次故障 | RR UNC/CRC：lba/pba/paa/g/plane/block/page/cw/retry/解码状态；program-fail；GC victim；erase-fail；dangling（非 BAD）；候选池/BBT 失败；CP abort；USB CSW | ERROR/WARN | 首次全上下文 |
| 状态变化 | format required/done、FORMAT_IN_PROGRESS、REBUILD_REQUIRED、GC pause/resume、free/spare/OP 跨水位、journal 水位、L2P degrade、timing switch | INFO/WARN | 变化时 |
| 搬移进度 | `[GC] start` 一行带触发上下文；逐 victim 的 `[MIG] move` 只在 DEBUG 打变化字段 | INFO / DEBUG | start 每会话一次；move 仅 `moved>0` |
| 周期摘要 | `[STAT]`：ch/t/free/bad/mig（活跃/暂停）；op/spare/cp/jrn/wr/dng/rrab/mode 见 `info`/`diag` | INFO | **10 s** |
| DEBUG | per-CP done、per-GC trig、wbuf flush、per-region load、ECC 趋势 | DEBUG | 默认关闭 |

### 2.1 `[STAT]` 字段

```
[STAT] ch=0 t=123s free=199 bad=284 mig=1/0 mv=0 nop=0
```

- `free`/`bad`：块分配与坏块；
- `mig`：active/paused（**不是 GC 次数**；GC 见 `[GC  ] progress/end` 与 `[MIG ] move`）；
- `mv`/`nop`：累计搬移 CW / 空块回收数；
- `op`/`spare`/`cp`/`jrn`/`wr`/`dng`/`rrab`/`mode` 已移到 `info`/`diag` 按需查看。

### 2.2 写轨迹飞行记录器

`alc_t` 内 64 条环形记录（`alc_trace_t`：seq/lba/pba/plane/block/page/cw/kind）。

- `kind=0` 写确认成功；`1` program fail；`2` GC 写。
- 首次 UNC（好块）或 program-fail 时 `alc_trace_dump(lba,pba)`：
  - 同一 PBA 出现两次 ⇒ **同页二次编程**；
  - 只出现一次 ⇒ 写后读坏（时序/介质）；
  - `kind=1` ⇒ 该次 program 失败。
- 平时不打印，RAM 约 1.3 KB。

### 2.3 事件日志（GC / MIG 生命周期）

> 文本格式是**本仓自拟**（没有公开的主控日志格式出处）；事件分层沿用本文档的
> “首次故障全上下文、重复事件只留计数、状态变化立即打、周期摘要一行”。

日志只带 `lba` / `pba` / `gblock` 身份和本事件结果；`ch/ce/lun/plane/block/page/cw/paa`
由串口 `map` / `addr` 按需展开（见 `testing_guide.md`）。

| 事件 | 级别 | 落点 | 字段 | 频率 |
|---|---|---|---|---|
| `[GC  ] start` | INFO | `mig_gc_begin` | `reason trig mode free` | 每会话一次（仅选中有效 victim 时） |
| `[GC  ] progress` | INFO | `mig_handle` | `jobs moved rec noop abort free` | 仅会话有实际搬移时，5 s |
| `[GC  ] end` | INFO | `mig_handle` | `jobs moved rec noop abort elapsed` | 仅 `moved>0` 的会话收尾 1 行 |
| `[WL  ] start` | INFO | `mig_wl_begin` | `free writes` | 每个静态 WL job 1 行（只在 host 空闲档启动） |
| `[MIG ] move` | DEBUG（仅 `vcw>0`；空块完全不生成） | `mig_job_finish` | `k r trig src0 src1 dst0 dst1 v mv t` | 每个真实 victim 1 行，默认不输出 |
| `[MIG ] abort` | WARN（限流） | `mig_job_abort` | `kind g0 g1 phase page plane cause fc supp` | 首次/限流 |
| `[L2P ] job inflight` | INFO | `l2p_owner_flush_kick` | `load flush dirty` | 有在飞时 1 s |
| `[TRIM] start/done` | INFO | `ftl_trim_begin/step` | start `lba n`；done `n elapsed` | 每次控制面 TRIM |
| `[RR  ] unc` | ERROR（限流） | `rr_settle_plane` | `lba pba paa g pl bk pg cw retry st supp` | 首次故障 |
| `[RR  ] job` | INFO | `rr_service_step` | `stage plane attempt retry stall age` | 卡住后 5 s |
| `[FTL ] predictive relocate retired` | WARN | `mig_job_finish` | `g` | 每次 predict 命中 |

空 victim（`vcw==0`）**不产生任何** `[MIG ] move`，也不会开启可见的 GC
会话；`[GC  ] progress/end` 只在 `moved>0` 时出现。`[GC  ] start` 每会话只打一次，
带 `reason/trig/mode/free`；逐 victim 的 `[MIG ] move` 降为 DEBUG，且只保留
`src/dst/v/mv/t` 等变化字段，避免大批搬移时重复打印同一上下文。
⚠ 不能用 `sel.best_valid_pct` 判断：它是域内**最低**有效率，存在任一空块就为 0。

CP / Journal 是高频元数据维护（CP 每次水位触发、JRN 每页/换块）：**不逐次打
start/rotate**；当前序号看 `info` 的 `cp=` / `jrn=`，异常仍按原 ERROR/WARN 输出。

## 3. 不需要打（或降级/聚合）

| 事件 | 处理 |
|---|---|
| `[CP  ] start/done` 每次 CP | **不打**；`info` 给 `cp=` |
| `[JRN ] rotate` 每次换块 | **不打**；`info` 给 `jrn=`；仅错误行保留 |
| `[GC  ] trig` 每次评估 | DEBUG，5 s 一行（`GC_LOG_INTERVAL_MS`） |
| `[MIG ] read` / 逐页/逐 CW 细节 | **不打**（debug 也不打）；地址用 `map`/`addr` 查 |
| `[JRN ] flush` 每页 | **不打**（debug 也不打）；只保留 `rotate` 与错误行 |
| `[RR  ] start/done` 每次恢复 | **不打**；失败现场由 `[RR  ] unc`（限流）承担，卡住由 `[RR  ] job` 心跳承担 |
| `[GC  ] no-gain` | **DEBUG 边沿**：仅在首次进入「无可回收收益」时打；之后由写入/紧急状态解除，不得每次选块重试都打 |
| `[MIG ] victim read failed` 每块 | 单块 WARN；连续 ≥8 个不同 victim ⇒ **熔断** `mig.paused`（ERROR 一行） |
| `[FTL ] dangling` 每个 LBA | `st=BAD`：每个旧 gblock 一行 + `lbas=` 聚合；`FREE/LOG/NONE`：ERROR（缺陷） |
| `read unrecoverable ... already-BAD` | 每个 gblock 只打一次；其余计数进 `rrab` |
| `[L2P ] load start failed` | 按 cause 聚合，5 s 一行 |
| 每页/每 CW 读成功、ECC 趋势 | 只计数，不打 |
| `[MIG ] abort` / `write retry` | 保留限流；`no free block` 且 `moved==0` 不打印（满盘死路，不是搬移进展）；计数进 `info`/`diag` |

### 3.1 刷屏预算（本仓自拟）

- 115200 8N1 ≈ 11.5 KB/s；一条 100–150 B 的 INFO ≈ 9–13 ms 线速。
- 稳态 INFO 目标 ≤5 行/s（≤~600 B/s，约 5% 串口带宽）；突发必须由真实事件驱动。
- 一个 128 KiB victim 的搬移受 NAND 读/写/擦物理速率约束，真实 victim 的
  `[MIG ] move` 不会成为稳态刷屏源。
- 空 victim 只进 `noop=` 计数；DEBUG 也不做 per-page/per-CW。
- 重复异常继续用 `supp=` / `count=`。

## 4. 消息格式

- 级别前缀固定 3 字符：`[ERR]` / `[WRN]` / `[INF]` / `[DBG]`。
- 模块标签固定 **4 字符宽**，右括号对齐，不足补空格：`[GC  ]` / `[MIG ]` /
  `[L2P ]` / `[JRN ]` / `[CP  ]` / `[RR  ]` / `[STAT]` / `[TRIM]` / `[PRED]` 等。
  `[FAULT]` / `[BOUND]` 是断言/故障类别，不参与模块标签对齐。
- 固定字段序：`lba pba paa g pl bk pg cw st retry count supp`；事件行只带本事件
  相关字段，其余靠紧邻行推导或 `map`/`addr` 查。
- FC 状态必须解码：`fc=0x48(READY|UNC)`，不裸给 hex。
- 每条 ≤ 192 B（`log_vprintf` 缓冲），超了会截断。
- 重复事件带 `supp=` 或 `count=`；关键事件带 `t=` / `wr=` 便于与 H2testw 进度对齐。

## 5. 落地状态（2026-10-23）

| 项 | 状态 |
|---|---|
| `[STAT]` 10 s slim 摘要（main.c） | 已改、未上板 |
| GC 实际搬移事件 `[MIG ] move`（migrate.c） | 已改、未上板 |
| 空块/无搬移 GC 不刷 `[GC  ] start`；`[GC  ] progress/end` 仅 `moved>0` | 已改、未上板 |
| `diag` 增加累计 `mv=`（搬移 CW）与 `nop=`（空块回收） | 已改、未上板 |
| CP start / JRN rotate 高频元数据事件不逐次打（看 `info` 的 `cp=`/`jrn=`） | 已改、未上板 |
| 模块标签 4 字符对齐（log.h + src 批量替换） | 已改、未上板 |
| `map` 增加 `paa=`；新增 `addr pba/paa/g`（cli.c） | 已改、未上板 |
| `FREE_POOL_TARGET` = 32（2026-10-23 回退）；BUSY 档 `free<32` 仍做水位 GC | 已上板（H2testw 后 free=64，未触发 T1） |
| DEBUG 收敛：GC trig 5 s、删逐页/逐 CW MIG/JRN/RR 流水 | 已改、未上板 |
| dangling 按旧 gblock 聚合（ftl.c） | 已改、未上板 |
| RR：已退役块每 gblock 一次、好块首故障全字段 + `st=`（read_recovery.c） | 已改、未上板 |
| `fc_cw_status_str()` 解码（fc_command.c） | 已改、未上板 |
| 写轨迹飞行记录器（allocator.c / ftl.c / read_recovery.c） | 已改、未上板 |
| CP done / GC trig 降 DEBUG；MIG 单块降 WARN | 已改、未上板 |
| GC 系统性读失败熔断（`MIG_SYSTEMIC_READ_FAIL_MAX=8`） | 已改、未上板（RISK-GC-10） |
| `info` 增加 `diag[...] dng/rrab/mig/streak` | 已改、未上板 |
| 只对**真正搬了数据**的 victim 打 `[MIG ] move ...`（`migrated==0` 不打印；aborted 不打） | 已改、未上板 |
| 统一 `log_ratelimit_t` 抽象 | 未做（现有各类节流已覆盖；后续收敛；见 RISK-LOG-02） |

## 6. 验证判据

1. H2testw 失败时，日志只出现：首次 UNC 一行（全字段）+ 最多 8 条 `[TRC ]` + 10 s 一条 `[STAT]`；不再逐 LBA/逐 CP/逐 GC 刷屏。
2. 已退役块被 host 反复读时，每个 gblock 只出现一次 `[RR  ] retired ...`，`rrab` 增长。
3. `[TRC ] dup>0` ⇒ 同页二次编程；`dup=0` ⇒ 写后读坏/时序。
4. 系统性读失败时，`[MIG ] victim read failed` 最多 8 行后出现 `suspend GC until reboot`，之后不再有 GC 触发。
5. GC 正常（INFO 档）只出现：每会话一行 `[GC  ] start`、5 s 一行 `[GC  ] progress`、有实际搬移的会话一行 `[GC  ] end`；逐 victim 的 `[MIG ] move` 只在 `log level debug` 出现，空 victim/无搬移会话不打印。
6. `log level debug` 下也不出现逐页/逐 CW 的 `[MIG ] read/move`、逐页 `[JRN ] flush`、逐次 `[RR  ] start/done`；`[GC  ] trig` ≤1 行/5 s。
7. 所有模块标签右括号在同一列（`[GC  ]` / `[MIG ]` / `[STAT]`）；`[FAULT]`/`[BOUND]` 例外。
