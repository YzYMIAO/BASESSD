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
| 启动 | reset reason、NAND ID/ONFI、**实际 timing mode**、FC 拓扑、FTL boot type、`[CAP]`、`[BLK] bad areas`、USB mounted | INFO | 一次 |
| 格式/体检 | start、每 512 块进度、done tested/bad、`old_bad/recovered/new_bad` | INFO/WARN | 进度 + 汇总 |
| 首次故障 | RR UNC/CRC：lba/pba/paa/g/plane/block/page/cw/retry/解码状态；program-fail；GC victim；erase-fail；dangling（非 BAD）；候选池/BBT 失败；CP abort；USB CSW | ERROR/WARN | 首次全上下文 |
| 状态变化 | format required/done、FORMAT_IN_PROGRESS、REBUILD_REQUIRED、GC pause/resume、free/spare/OP 跨水位、journal 水位、L2P degrade、timing switch | INFO/WARN | 变化时 |
| 搬移进度 | **每个 victim 块收尾一行**：`kind=gc/wl/pred p0= p1= vcw= pages= post=free/retire err= elapsed=` | INFO | 每块一次（aborted 不打） |
| 周期摘要 | `[STAT]`：free/op/spare/bad、cp/jrn、writes、mig active/paused、dangling、already-bad reads、timing mode | INFO | 5 s |
| DEBUG | per-CP done、per-GC trig、wbuf flush、per-region load、ECC 趋势 | DEBUG | 默认关闭 |

### 2.1 `[STAT]` 字段

```
[STAT] ch=0 t=123s mode=5 free=31 op=202/203 spare=100/100 bad=284
       cp=230 jrn=151046 wr=109168 mig=1/0 dng=40 rrab=128
```

- `op` / `spare`：used/total；
- `mig`：active/paused；
- `dng`：L2P 指向 `valid_count==0` 的累计次数；
- `rrab`：host 重读已退役块的累计次数。

### 2.2 写轨迹飞行记录器

`alc_t` 内 64 条环形记录（`alc_trace_t`：seq/lba/pba/plane/block/page/cw/kind）。

- `kind=0` 写确认成功；`1` program fail；`2` GC 写。
- 首次 UNC（好块）或 program-fail 时 `alc_trace_dump(lba,pba)`：
  - 同一 PBA 出现两次 ⇒ **同页二次编程**；
  - 只出现一次 ⇒ 写后读坏（时序/介质）；
  - `kind=1` ⇒ 该次 program 失败。
- 平时不打印，RAM 约 1.3 KB。

## 3. 不需要打（或降级/聚合）

| 事件 | 处理 |
|---|---|
| `[CP] done` 每次 CP | 降 DEBUG；`[STAT]` 给 `cp=` |
| `[GC] trig` 每次评估 | 降 DEBUG；只在 free 跨水位时 INFO |
| `[MIG] victim read failed` 每块 | 单块 WARN；连续 ≥8 个不同 victim ⇒ **熔断** `mig.paused`（ERROR 一行） |
| `[FTL] dangling` 每个 LBA | `st=BAD`：每个旧 gblock 一行 + `lbas=` 聚合；`FREE/LOG/NONE`：ERROR（缺陷） |
| `read unrecoverable ... already-BAD` | 每个 gblock 只打一次；其余计数进 `rrab` |
| `[L2P] load start failed` | 按 cause 聚合，5 s 一行 |
| 每页/每 CW 读成功、ECC 趋势 | 只计数，不打 |
| `[MIG] abort` / `write retry` | 保留限流；计数进 `[STAT]` |

## 4. 消息格式

- 固定字段序：`lba pba paa g pl bk pg cw st retry count supp`。
- FC 状态必须解码：`fc=0x48(READY|UNC)`，不裸给 hex。
- 每条 ≤ 192 B（`log_vprintf` 缓冲），超了会截断。
- 重复事件带 `supp=` 或 `count=`；关键事件带 `t=` / `wr=` 便于与 H2testw 进度对齐。

## 5. 落地状态（2026-10-21）

| 项 | 状态 |
|---|---|
| `[STAT]` 5 s 摘要（main.c） | 已改、未上板 |
| dangling 按旧 gblock 聚合（ftl.c） | 已改、未上板 |
| RR：已退役块每 gblock 一次、好块首故障全字段 + `st=`（read_recovery.c） | 已改、未上板 |
| `fc_cw_status_str()` 解码（fc_command.c） | 已改、未上板 |
| 写轨迹飞行记录器（allocator.c / ftl.c / read_recovery.c） | 已改、未上板 |
| CP done / GC trig 降 DEBUG；MIG 单块降 WARN | 已改、未上板 |
| GC 系统性读失败熔断（`MIG_SYSTEMIC_READ_FAIL_MAX=8`） | 已改、未上板（RISK-GC-10） |
| `info` 增加 `diag[...] dng/rrab/mig/streak` | 已改、未上板 |
| 只对**真正搬了数据**的 victim 打 `[MIG] done ...`（`migrated==0 && vcw==0` 的空块擦除不打印；aborted 不打） | 已改、未上板 |
| 统一 `log_ratelimit_t` 抽象 | 未做（现有各类节流已覆盖；后续收敛） |

## 6. 验证判据

1. H2testw 失败时，日志只出现：首次 UNC 一行（全字段）+ 最多 8 条 `[TRC]` + 5 s 一条 `[STAT]`；不再逐 LBA/逐 CP/逐 GC 刷屏。
2. 已退役块被 host 反复读时，每个 gblock 只出现一次 `[RR] retired ...`，`rrab` 增长。
3. `[TRC] dup>0` ⇒ 同页二次编程；`dup=0` ⇒ 写后读坏/时序。
4. 系统性读失败时，`[MIG] victim read failed` 最多 8 行后出现 `suspend GC until reboot`，之后不再有 GC 触发。
