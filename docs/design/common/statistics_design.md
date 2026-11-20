# 统计架构（草案）

状态：**草案，未实现**。实现项登记见 `docs/design/known_open_items.md` 的 `ISS-STATS-01`。

本文件只定架构与统计口径；具体插桩在实现时按本文件落地。未上板前不得在别处写「已验证」。

---

## 1. 目标与非目标

**目标**

- 统一采集、查看跨模块统计：一条 `stats` 命令按模块打印，出问题时不需要临时加日志。
- 失败**按原因分类**（gate / queue full / 池空 / 超时 / 介质错误），而不是一个 `submit_backpressure` 字符串。
- 编译期可整体关闭（`SSD_STATS_ENABLE`）；关闭时不进镜像。
- 统计存储仍归模块自己（H10 单写者、C9 单一坐标），不建立第二套值存储。

**非目标**

- 不做持久化 SMART / 终身计数（要动介质格式，另立决策）。
- 不做事件 trace / 时间线（需要环形缓冲与解码器，二期另议）。
- 不改变数据面行为；统计只读宿主对象，不参与控制流。
- 不做主机侧 telemetry Log Page（本仓主机接口是 USB MSC，没有对应 ABI）。

---

## 2. 方案选择

采用 **B'：模块自持存储 + 自描述名字表 + 中央注册/遍历**。

```text
模块结构体（唯一存储）          名字表（Flash, const）             中央注册表（RAM）
l2p_cache_stats_s  ──base──┐   { "hit_count", off, U32, COUNTER }  ┌─ group[0] {module, inst, base, names}
                           ├──► { "miss_count", off, U32, COUNTER } ─┤
                           │    ...                                  └─ group[1] ...
                           └────────────────────────────────────────► stats_print() 遍历打印
```

工程对照（来源见 §11）：

| 系统 | 形态 |
|---|---|
| lwIP | `LWIP_STATS` 开关；每模块结构体（`struct stats_proto` 等）；`STATS_INC()` 关闭时空展开；`stats_display()` + 每模块 display 宏 |
| Zephyr | `CONFIG_STATS`；模块用 `STATS_SECT_START/ENTRY/END` 定义组；`stats_register()` 注册；`stats list` shell 遍历打印；`CONFIG_STATS_NAMES` 控制名字是否进镜像 |

**为什么不把值集中到一个中央数组**（本文件不采用）：

- 值搬离 owner ⇒ 与现有 `l2p_cache_stats_s` / `fc_resource_stats_s` / `mig_counters_s` 形成两份坐标（C9）；
- 多实例（多 die）要给每个计数展开 ID 或强制聚合；
- 中央 ID 清单是合并热点，新增计数要改两个文件；
- 固定 ID 的唯一优势是给主机/硬件做固定 ABI，本仓用不上。

---

## 3. 术语

| 术语 | 含义 |
|---|---|
| group | 一个模块（或一个实例）的一组统计 |
| name table | 组内的字段描述：名字、相对 base 的偏移、宽度、种类 |
| counter | 单调累计；`stats --clear` 只清这一类 |
| gauge | 瞬时值（如 `command_used`、`free_count`），**不清零**、由 owner 维护 |
| registry | 中央注册表：只存 group 描述（指针 + 名字表），不存值 |

---

## 4. 架构

### 4.1 文件与分层

```text
include/common/stats.h   类型 / API / 开关宏
src/common/stats.c       注册表、遍历打印、清零
（各模块 .c/.h）         结构体字段 + 名字表 + 注册调用
src/hil/cli.c            stats 命令（唯一 CLI 入口）
```

- `src/common/makefile.mk` 已 `wildcard common/*.c`，新增 `.c` 不改构建；`include/common` 已在 include 路径。
- 依赖方向：`ftl/`、`fc/`、`nand/`、`platform/`、`hil/` → `stats.h`（common，freestanding）；`stats.c` 不反向依赖任何模块（不知道模块名，只遍历注册表）。
- 打印只经 `log.h`；不在模块里写打印代码。

### 4.2 类型

```c
/* include/common/stats.h */
#include "ssd_types.h"

#ifndef SSD_STATS_ENABLE
#define SSD_STATS_ENABLE (1U)       /* 0 = 注册/打印/名字表全部不编译 */
#endif

typedef enum STATS_KIND_E
{
    STATS_COUNTER = 0,              /* 单调累计；stats --clear 只清它 */
    STATS_GAUGE,                    /* 瞬时值；不清零 */
    STATS_KIND_NUM
} stats_kind_e;

typedef enum STATS_WIDTH_E
{
    STATS_U8  = 1,
    STATS_U16 = 2,
    STATS_U32 = 4
} stats_width_e;

typedef struct STATS_NAME_S
{
    const char   *name;             /* 字段名，打印用 */
    WORD          offset;           /* offsetof(宿主结构体, 字段) */
    stats_width_e width;            /* 只允许 8/16/32 位无符号 */
    stats_kind_e  kind;
} stats_name_s;

#define STATS_HIST_BUCKET_NUM (9U)  /* <50/100/200/500/1m/2m/5m/10m/50m µs；>=50m 进 over */
typedef struct STATS_HIST_S
{
    DWORD bucket[STATS_HIST_BUCKET_NUM];
    DWORD over;
    DWORD count;   /* 不变量：sum(bucket) + over == count */
    DWORD max_us;  /* 高水位 gauge，--clear 不清 */
} stats_hist_s;

typedef struct STATS_HIST_NAME_S
{
    const char *name;
    WORD        offset;  /* offsetof(宿主, stats_hist_s) */
} stats_hist_name_s;

typedef struct STATS_GROUP_S
{
    const char              *module; /* "ftl.l2p" */
    BYTE                     inst;   /* 实例号（die / target）；STATS_INST_NONE = 无实例维度 */
    BYTE                    *base;   /* 宿主对象地址（模块自己的结构体） */
    const stats_name_s      *names;
    BYTE                     count;
    const stats_hist_name_s *hists;
    BYTE                     hist_count;
    WORD                     entry_base; /* 本组第一项在 delta 基线数组里的扁平下标 */
} stats_group_s;

#define STATS_INST_NONE (0xFFU)
#define STATS_GROUP_MAX (32U)        /* 模块数 × 实例数上限（多 die/target 的代码上界） */
#define STATS_ENTRY_MAX (320U)       /* 所有组标量条目总数上限（delta 基线容量） */
#define STATS_HIST_TOTAL_MAX (24U)   /* 所有组直方图总数上限 */

/* 窄化只在 producer 侧：模块名字表 / 注册调用不写强转。 */
#define STATS_ENTRY_COUNT(array) ((BYTE)(sizeof(array) / sizeof((array)[0])))
#define STATS_OFFSET(type, field) ((WORD)offsetof(type, field))
#define STATS_OFFSET_NESTED(t1, f1, t2, f2) ((WORD)(offsetof(t1, f1) + offsetof(t2, f2)))
#define STATS_CHECK_FIELD(type, field, width) \
    _Static_assert(sizeof(((type *)0)->field) == (width), #field " width mismatch")
#define STATS_CHECK_HIST_FIELD(type, field) \
    _Static_assert(sizeof(((type *)0)->field) == sizeof(stats_hist_s), #field " hist size mismatch")
```

**约束**

- 宽度只支持 `U8/U16/U32`；枚举/位域/有符号量不得直接登记。
- `offset` 用 `offsetof`；字段删除/改名后名字表编译失败（正向保护）。
- `base` 必须是**稳定地址**（模块的静态对象或实例工作区），不得是栈上临时结构。
- `STATS_GROUP_MAX` 满、重复注册不同 base、`names == NULL` 等属配置错误：debug `ASSERT`，release `LOGE` 后拒绝。

### 4.3 API

```c
#if SSD_STATS_ENABLE
BOOL stats_register(const char *module, BYTE inst, void *base,
                    const stats_name_s *names, BYTE count);
BOOL stats_register_hist(const char *module, BYTE inst, void *base,
                         const stats_hist_name_s *hists, BYTE count);
void stats_hist_observe(stats_hist_s *hist, DWORD value_us);
typedef enum STATS_FORMAT_E { STATS_FORMAT_TEXT = 0, STATS_FORMAT_CSV, STATS_FORMAT_BRIEF, STATS_FORMAT_NUM } stats_format_e;
BOOL stats_print(const char *prefix, stats_format_e format);
void stats_list(void);                  /* 组名 + 条目数 */
BOOL stats_delta(const char *prefix);   /* 标量差值；直方图打当前值 */
void stats_clear(const char *prefix);   /* 清匹配的 counter + 直方图桶；gauge 不动 */
#else
#define stats_register(...)      ((void)0)
#define stats_register_hist(...) ((void)0)
#define stats_hist_observe(...)  ((void)0)
#define stats_print(...)         ((void)0)
#define stats_list()             ((void)0)
#define stats_delta(...)         ((void)0)
#define stats_clear(...)         ((void)0)
#endif
```

- `stats_register` **幂等**：同一 `(module, inst)` 重复注册时覆盖旧描述（UT 反复 init 不会撑满注册表）。
- 注册时给每组分配连续的 `entry_base`；条目总数超过 `STATS_ENTRY_MAX` 则注册失败（`ASSERT` + `LOGE`）。
- `stats_print` 按 `(module, inst)` **稳定排序**输出；`prefix` 匹配 `module` 或 `module.name`
  （`stats fc`、`stats ftl.l2p.load`）。
- `stats_delta` 用 `static DWORD s_baseline[STATS_ENTRY_MAX]`，按 `entry_base` 索引：首次调用只建立
  基线并打印 `baseline stored`；后续对 counter 打印差值、对 gauge 打印当前值。直方图只打当前值
  （桶差值需要额外基线 RAM，未做）。
- `stats_clear(prefix)` 按名字表 `kind` 逐字段清零（counter）/ 清直方图桶，**绝不** `memset`
  整个 base；`max_us` 与 gauge 不清，delta 基线同时重置。

### 4.4 模块侧写法（每模块一张 X-macro 表）

```c
/* l2p_cache.c */
#define L2P_STATS_LIST(X)                            \
    X(hit_count,        STATS_U32, STATS_COUNTER)    \
    X(miss_count,       STATS_U32, STATS_COUNTER)    \
    X(eviction_count,   STATS_U32, STATS_COUNTER)    \
    X(load_fail_gate,   STATS_U32, STATS_COUNTER)    \
    ...

#define L2P_STATS_NAME(field, width, kind) \
    { #field, (WORD)offsetof(l2p_cache_stats_s, field), width, kind },
static const stats_name_s s_l2p_stats_names[] = { L2P_STATS_LIST(L2P_STATS_NAME) };
#undef L2P_STATS_NAME

void l2p_stats_register(l2p_cache_s *cache, BYTE inst)
{
    (void)stats_register("ftl.l2p", inst, &cache->stats, s_l2p_stats_names,
                         (BYTE)(sizeof(s_l2p_stats_names) / sizeof(s_l2p_stats_names[0])));
}
```

- 新增计数 = 结构体加字段 + `L2P_STATS_LIST` 加一行 + 自增点一行；**只改本模块文件**。
- 自增统一走宏，便于整体关闭：
  ```c
  #if SSD_STATS_ENABLE
  #define STATS_INC(field)    ((field)++)
  #define STATS_ADD(field, n) ((field) += (n))
  #else
  #define STATS_INC(field)    ((void)0)
  #define STATS_ADD(field, n) ((void)0)
  #endif
  ```
  已有自增点是否迁移按 §7 的节奏进行，不一次性清扫。
- 需要打印**别人的字段**时才把该字段登记进来；登记不改变写者（H10）。

### 4.5 注册与生命周期

- 注册在模块 init 末尾执行（模块自己知道自己的对象），不集中到 main 的注册清单（H13）。
- 多实例：`inst` 是「实例号」——FTL 模块用紧凑 die 号，共享 L2P cache 用 target 下标——组名打印为 `ftl.l2p[0]`。
- 每实例组由 FTL 组合根（`ftl_init`）注册，不在模块 init 里直接注册：直接调用 `mig_init` 的测试用例不能把栈上的 handle 地址写进注册表。
- UT：模块可能反复 init；靠 `stats_register` 的幂等性收敛。
- 注册只写注册表，不做动态分配；注册表是 `static stats_group_s g_groups[STATS_GROUP_MAX]`。

### 4.6 打印契约

```text
ssd> stats
[INF] [STAT] fc.pool
[INF] [STAT]   counter                 value
[INF] [STAT]   command_used                3
[INF] [STAT]   descriptor_used             0
[INF] [STAT] ftl.l2p[0]
[INF] [STAT]   counter                 value
[INF] [STAT]   hit_count               34567
[INF] [STAT]   load_fail_gate           1203
[INF] [STAT]   load_lat_us  n=1722  max=2058us  over=0
[INF] [STAT]     <50u 0 | <100u 0 | <200u 0 | <500u 0 | <1m 1710
[INF] [STAT]     <2m 11 | <5m 1 | <10m 0 | <50m 0 | >=50m 0
...

ssd> stats ftl.l2p          # 按 module 前缀过滤
ssd> stats ftl.l2p[0]       # 按实例过滤（module[inst]）
ssd> stats ftl.l2p.load     # 按字段前缀过滤（module.name）
ssd> stats --csv            # module,inst,name,value；直方图逐桶一行（行首仍有 [INF] [STAT] 前缀）
ssd> stats --brief          # 只打直方图摘要行，省略 2×5 桶表
ssd> stats --delta          # 与上次 --delta 基线比较；首次只建基线
[INF] [STAT] ftl.l2p[0]
[INF] [STAT]   counter                 value
[INF] [STAT]   load_start                +812
[INF] [STAT]   load_done_ok              +800
[INF] [STAT]   load_fail_gate             +12
[INF] [STAT] fc.pool
[INF] [STAT]   command_used                3   # gauge 打当前值，不打差值
ssd> stats --clear          # 只清 counter，不动 gauge
[STAT] cleared
```

- 文本输出按组打表：组头 `<module>[<inst>]` 一行，随后 `counter / value` 列头，
  每个标量一行（名字左对齐、数值右对齐，列宽按组内最长名字、上限 24）。
- 直方图 = 摘要一行 + 2×5 紧凑桶表（每格「标签 值」，`>=50m` 格是 `over`）。
- `--delta` 只对 `STATS_COUNTER` 打 `+差`；gauge 打当前值。基线数组占
  `STATS_ENTRY_MAX × 4 B`（128 项 = 512 B）。
- 全局 `--delta`（无 prefix）额外打印窗口吞吐：
  `window elapsed=…ms host_read=…B (…KB/s) host_write=…B (…KB/s)`。
  基线记录 `platform_now_ms` + lifetime host 字节；带 prefix 的 delta 不打印吞吐，
  但仍推进吞吐基线。`SSD_STATS_PERSIST_ENABLE=0` 时只保留差值、不打印吞吐。
- 值按宽度读取后统一升到 `DWORD`，用 `%lu` 打印（H43/H44：`DWORD` 配 `%lu`，不加强转）。
- 只由命令触发，**不做周期打印**（`[STAT]` 心跳保持 DEBUG 级）。
- 输出量 = 每组 2 行头 + 标量 N 行 + 每个直方图 5 行；实际按模块数量线性。

### 4.7 编译开关与开销

- `SSD_STATS_ENABLE=0`：
  - `stats.c` 整个不编译；注册表、名字表、`stats` 命令不进镜像；
  - `STATS_INC/ADD` 展开为空，自增点零指令；
  - **统计专用对象/表/函数/字段整体裁掉**（`s_stats_*` 静态对象、名字表、`*_stats_register`、
    进展签名、HIL note 函数、以及 `l2p_cache_stats_s` / `mig_counters_s` / `ftl_stats_s`
    中新增的统计专用字段）；关闭档构建实测 debug 的 text/bss 均下降，且不产生未使用告警。
  - 仍保留的是**既有功能字段**（如 `l2p_cache_stats_s` 的 hit/miss——`stat` CLI 在关闭档仍可读；
    `ftl_stats_s.total_writes` 被 mig 当功能量用），它们不是统计框架引入的。
- `SSD_STATS_PERSIST_ENABLE=0`：P3 的 lifetime 读写/计数/`smart` 与 `lifetime_port` 全部不编译；
  CP Header 的 lifetime 块保持全 0（padding 仍是 padding）。
- `SSD_STATS_ENABLE=1`：每次计数 = 一个 `DWORD` 读改写（DTCM），在“每条命令/每次失败”频度，不在字节循环。
- 默认值（已裁决）：debug/release 均 **1**；用 release `/speed` 记录开/关差值，若超阈值再议是否默认关（见 §8）。

### 4.8 并发与中断

- 当前单线程 superloop，无原子性要求。
- 禁止在 ISR 里写统计（会与主循环竞争）；确需 ISR 计数时按 H16/H18 另立规则。
- `stats_clear` 只在 CLI 上下文调用，调用方保证没有在飞业务正在读同一组。

### 4.9 命名与类型约定

- 统计专用标识符统一以 `stats` 为词根：API `stats_*`，静态对象 `s_stats_*`，名字表
  `s_stats_*_names`，类型 `<module>_<noun>_stats_s`，宏 `STATS_*`。
- 打印名是 `module[inst].name`（CLI 行带 `[STAT]` 标签）；模块前缀由组名给，字段名不重复模块名。
- `SSD_STATS_NAMES=0` 时名字表存 NULL，打印 `s<index>`（`inst + index` 定位）；字段级过滤关闭。
- **窄化只允许发生在 producer 侧的三个宏**：`STATS_ENTRY_COUNT`（`size_t` → `BYTE`）、
  `STATS_OFFSET` / `STATS_OFFSET_NESTED`（`size_t` → `WORD`）。模块名字表与注册调用不写强转。
- `stats_value_read/store` 里的 `(BYTE)` / `(WORD)` 是运行期宽度决定的真实截断；
  其余同型 / 加宽强转一律不加（H43/H44/H48）。
- ⚠ `STATS_OFFSET` 的 type 形参不加括号 —— `offsetof` 展开为 `__builtin_offsetof(type, member)`，
  给类型加括号会让内建宏解析失败（H25 的例外，已就地注明）。

---

## 5. 统计什么

### 5.1 选取原则

1. **诊断驱动**：每个计数必须回答一个具体问题（“为什么没提交进去 / 为什么 abort / 为什么慢”）。
2. **有区分力**（C7）：原因分开计，不合并成一个“失败”。
3. **单写者**（H10）：自增点唯一；聚合值与原因值可以并存，但语义必须不同（C9）。
4. **不逐轮询计数**：不在每次 `service`/`poll` 循环里加计数。
5. **不记数据内容**：只记事件、原因、资源水位。
6. **不重复现有语义**：已有字段优先登记，不另起一份。

### 5.2 P1：直接登记现有结构体（零新增存储）

| group | base | 字段 | kind |
|---|---|---|---|
| `ftl.l2p` | `l2p_cache_s.stats`（`l2p_cache_stats_s`） | `hit_count` / `miss_count` / `eviction_count` / `wal_defer_count` / `hint_hits` / `hint_misses` / `cmt_hits` / `cmt_misses` / `cmt_wr_hits` / `cmt_wr_misses` / `cmt_dirty` / `cmt_writebacks` / `cmt_evict_clean` / `cmt_evict_dirty` / `cmt_wb_defer` | counter（`cmt_dirty` 是 gauge） |
| `ftl.l2p.prefetch` | `l2p_cache_s.prefetch`（`l2p_prefetch_stats_s`） | `calls` / `started` / `skipped` / `resident` / `hits` | counter |
| `ftl.gc` | `mig_handle_s.state.counters`（`mig_counters_s`） | `reclaimed` / `moved` / `noop` / `novictim` / `nogain` / `batch_jobs` / `batch_pages` / `batch_aborts` | counter |
| `ftl.stats` | `ftl_ctx_s.stats`（`ftl_stats_s`） | `total_writes` / `dangling_total` | `total_writes` 是**功能量**（mig 判 host 写流）⇒ 按 gauge 登记，`--clear` 不清；`dangling_total` counter |
| `fc.pool` | `&g_fc_controller`（`fc_ctl_s` 字段） | `command_used` / `descriptor_used` / `buffer_used` / `command_high` / `descriptor_high` / `buffer_high`（`WORD`）+ `sq_used` / `hsq_used` / `cq_used` | gauge |
| `uart` | `usart.c` 的 `g_uart_stats` | `rx_overflow` / `tx_dropped` / `tx_context_errors` / `tx_flush_timeouts` / `tx_dma_errors` / `tx_transport_errors` / `overrun_errors` / `framing_errors` / `noise_errors` / `parity_errors` | counter |

> `fc.pool` 已裁决**入表**：SQ/HSQ/CQ 当前占用用嵌套偏移登记 ——
> `offsetof(fc_ctl_s, sq) + offsetof(fc_sub_queue_s, count)`（HSQ/CQ 同理）。

### 5.3 P1：新增计数（诊断驱动）

| group | 字段 | 自增点 | 用途 / 判据 |
|---|---|---|---|
| `fc.submit` | `ok` / `reject_sq_full` / `reject_hsq_full` / `complete_ok` / `complete_err` / `cq_full` | `fc_sched_submit`（唯一入队漏斗）/ `fc_complete_from_nand` | 区分“队列满”与“完成错误”；非法提交由 ASSERT_REJECT 当场拒绝，不单列计数 |
| `fc.alloc` | `cmd_fail_bg` / `cmd_fail_crit` / `desc_fail` / `buf_fail` | `fc_command_alloc_class` / `fc_pool_descriptor_alloc` / `fc_buffer_alloc` 的 NULL 返回 | 区分“BG 额度用完”与“CRIT 预留也被吃完” |
| `ftl.op` | `gate_block` / `break_request` / `yield_request` / `yield_clear` / `req_add_fail` / `host_submitted` / `drain_rounds` / `drain_completed` | `ftl_op_seq_gate` / `ftl_op_submit` / `ftl_op_drain` | 区分“序列门控”与“FC 队列满”；drain 轮数 vs 完成数 |
| `fc.fill` | `rounds` / `pops` / `dispatched` / `empty` / `not_ready` / `defer_chain_locked` | `fc_poll_fill` / `fc_poll_fill_queue` | 轮数在涨、派发不涨 ⇒ 填充空扫；带 `pops` 与 `defer_chain_locked` 看回推量（`fc_debug_state_s` 的 fill_calls/fill_pops 与本组共用存储） |
| `fc.poll` | `rounds` / `completions` | `fc_service` | 完成泵空转 |
| `sys.loop` | `rounds` / `progress_rounds` + gauge `idle_streak` / `idle_streak_max` | 主循环 `ssd_runtime_pump`（比较 FC/FTL 进展签名） | 绝大多数轮无进展；高水位大 = 空转 |
| `ftl.l2p`（扩展 `l2p_cache_stats_s`） | `load_start` / `load_done_ok` / `load_done_err` / `load_fail_no_cmd` / `load_fail_gate` / `load_fail_submit` / `degrade` / `flush_start` / `flush_ok` / `flush_fail` | `l2p_load_submit` / `ftl_l2p_callback` / `l2p_load_fail_settle` | 满盘时判断加载卡在哪一步 |
| `ftl.gc`（扩展 `mig_counters_s`） | `job_start` / `job_end` / `abort_l2p_timeout` / `abort_submit_timeout` / `abort_read_fail` / `abort_jrn_timeout` / `abort_other` | `mig_job_start` / `mig_job_abort` / `mig_job_wait` / `mig_job_stall` | abort 原因分项；配合 `abort_other` 兜底 |
| `hil` | `cmd_read` / `cmd_write` / `cmd_flush` / `cmd_trim` / `timeout` / `bad_status` | `ssd_controller_submit` / `ssd_request_mark_timeout` / `on_sub_complete` | 主机侧总量与异常 |
| `ftl.stats`（扩展 `ftl_stats_s`） | `deferred_taken` | `ftl_take_deferred_error` | 迟到失败被 host 取走的次数（单写者在 FTL 层） |

**`mig_abort_e`（P1）**：`mig_job_abort` / `mig_job_wait` 收具名枚举（`MIG_ABORT_L2P_TIMEOUT` 等）；
日志字符串由 `mig_abort_name()` 唯一映射，计数按枚举分类。

### 5.4 P2 预留（本期不实现）

> 本节是统计面扩展的摘要；实施步骤、顺序与检查点见 §9.3。

- 延迟直方图：L2P load、GC job、CP job、主机命令的耗时分桶（固定桶边界用宏，H4）。
- `ftl.cp` / `ftl.jrn` / `nand` 的细项（abort/stall/reserve_fail/UNC/read_retry/timing_switch）。
- per-die 资源水位；多通道/多 CE 的仲裁计数。
- 事件 trace（ring）与解码。

### 5.5 明确不统计

- 数据内容 / LBA 值 / 用户数据哈希。
- 用字符串承载原因的“计数”（原因必须枚举化，字符串只进日志）。
- 已有语义的重抄（例如 `l2p_cache_stats_s.hit_count` 不再建第二个）。
- 逐字节 / 逐位循环里的计数（用完成数/错误数聚合，不在数据面内层加）。

### 5.6 空转 / 无进展检测

“代码频繁在某处做无用工”分三类，判据不同：

| 类型 | 现象 | 判据 |
|---|---|---|
| 空转 / 忙等 | 循环反复执行，状态没变 | `rounds` 涨、`progress` 不涨；PC 采样热点 |
| 低效重复 | 有调用但绝大多数不产出 | `progress / rounds` 比值低；`idle_streak_max` 大 |
| 死代码 / 重复计算 | 某段逻辑永不产出 | 统计看不出来；靠代码审视与覆盖，`pc` 只能证明“没执行” |

**判据必须成对**：只加 `attempts` 看不出空转。P1 已包含下列计数对（见 §5.3）：

| 位置 | 尝试 | 进展 |
|---|---|---|
| FC 填充 | `fc.fill.rounds` | `fc.fill.dispatched` |
| FC 完成泵 | `fc.poll.rounds` | `fc.poll.completions` |
| 主循环 | `sys.loop.rounds` | `sys.loop.progress_rounds` |
| 完成派发 | `ftl.op.drain_rounds` | `ftl.op.drain_completed` |
| L2P 加载 | `l2p.load_start` | `l2p.load_done_ok` |

`rounds` 是每轮一次自增：主循环高频时 u32 会回绕（10 万轮/s 约 12 h）。只用于比例与
`--delta` 差分，回绕不影响判读；不把它当终身计数。

**连续无进展高水位**（P1，gauge）：`sys.loop.idle_streak` / `idle_streak_max`。
“本轮有进展”定义为本轮至少发生一项：完成被派发、主机子操作下发、后台 job 推进一步。
一有进展清 0；高水位很大 ⇒ 系统在某些时段一直空转。比累计总数直观。

**差分视图**（P1）：`stats --delta` 两次调用之间打印 counter 差值，避免拿累计值做心算。
`--delta` 只打差值，不改变采样；`stats --clear` 同时重置基线。

**时间维度**（P2）：计数器给不出“每次多久”。需要时在服务入口读 µs 时基
（`delay_time_now_us`），只记 `over_1ms` 次数与 `max_us`，不逐轮读时间。

**PC 采样**（现有工具，先于新增计数使用）：`src/common/pc_sample.c` 每 1 ms 采样
被中断上下文的 PC：

```
pc            # 上一轮 top-N（复位后仍可看）
pc live       # 本轮
pc rate 10    # 10 ms 一次
```

限制：关中断 / 更高优先级 ISR 里的空转采不到；`.noinit` 只在软复位/看门狗复位后保留，
POR 会丢；必须用当场镜像的 map/ELF 反解。

**区分“有意的等待”**：背压重试、cache sequence 门控、`FTL_AGAIN` 都是设计内等待。
判据：

1. 等待期间**其他模块**是否有进展（`stats --delta` 看全局 counter）；
2. 等待方是否请求并兑现了 `yield` / `break`（`yield_request` vs `yield_clear`）；
3. PC 是否被该等待占满。

三条同时成立（无全局进展、yield 不生效、PC 占满）才是无用工；否则是调度/背压问题，
应按 §5.3 的 gate / queue / 池额度计数继续分因。

**判定流程**：

```
pc                        # 1. 先看热点是否停在某个循环
stats --delta             # 2. 建基线，等 10 s 再执行一次
stats --delta             #    看 attempts 是否涨、progress 是否不涨
stats ftl.op              # 3. 有意的等待是否有 yield/break 动作
stats fc.submit           #    还是队列满/额度满
```

统计负责**发现现象**，`pc` 负责**定位位置**，代码审视负责**判性质**——三者不能互相替代。

---

## 6. 插桩点规则

- 在**起源**处计数：谁拒绝/失败，谁计数；观察者（CLI/上层）不重复计数。
- 聚合与原因可并存，但要有明确关系：`l2p.load_fail_*` 是原因，`fc.submit.reject_*` 是 FC 层结果；`l2p` 不再自建一份 `sq_full`。
- 失败路径只加计数、不改返回码语义（H38：`FTL_AGAIN` 仍是瞬态）。
- 不在 `ASSERT` 里计数（`NDEBUG` 下不求值，H14）。
- 计数不影响控制流；任何“计数后提前返回”都是缺陷。

---

## 7. 现有统计的迁移规则

1. P1 只新增 + 登记，不改现有字段的写者。
2. 以后改到某模块时：把它原有的专用打印并入 `stats`，删旧入口（H37 节奏），不一次性清扫。
3. 同一语义只留一处（C9）：如果 `stats` 已打印某字段，就不再在新结构体里重复一份。
4. `fc_resource_stats_s` 的 snapshot API 保留（FTL host 预算在用）；`q` / `stat` / `jrn` / `cp` / `ecc` / `wear` 暂保留。

---

## 8. 验证

- 四关：`make all` / `make release` / `make tests UT=FTL`（涉及 FTL）/ `make check`，零新告警。
- **门控负例**（C7）：`make release CFLAGS_EXTRA=-DSSD_STATS_ENABLE=0` 后，`strings` 搜不到任一统计名，`help` 里没有 `stats`；
  `-DSSD_STATS_PERSIST_ENABLE=0` 后 lifetime 符号/字符串为 0、`smart` 不注册；
  `-DSSD_STATS_NAMES=0` 后 `.bin` 无字段名字符串（字段级过滤关闭）。
- **生效证据**（C8）：上板做一次 `wr` + `rd`，`stats ftl.l2p` 的 `load_start` / `hit_count` 必须增长；`stats --clear` 后 counter 归零、gauge 不变。
- **区分力负例**：构造一次满队列（UT 或注入），`fc.submit.reject_sq_full` 必须增长；构造一次序列门控，`ftl.op.gate_block` 必须增长。
- **性能**：release 下 `SSD_STATS_ENABLE=1/0` 各跑一次 `/speed`，差值记录；若超阈值，release 默认关。
- **输出有界**：`stats` 行数 ≤ 组数 × 每组成员数；不出现未注册项。

---

## 9. 分期与工作量

### 9.1 P1：4 个实现阶段

| 阶段 | 内容 | 落点 | 完成判据 |
|---|---|---|---|
| **S0 骨架** | `stats.h` / `stats.c`：类型、注册表（有界、幂等）、`print/clear/delta`、`SSD_STATS_ENABLE`；CLI `stats [prefix] [--clear] [--delta]` | `include/common/stats.h`、`src/common/stats.c`、`src/hil/cli.c` | 四关零告警；`SSD_STATS_ENABLE=0` 负例（`strings` 无名字、help 无 `stats`）；`--list` 显示 0 组；**不接任何模块** |
| **S1 现有结构体登记** | 零新增存储：`l2p_cache_stats_s`、`l2p_prefetch_stats_s`、`mig_counters_s`、`ftl_stats_s`、`fc_ctl_s` gauge（含 SQ/HSQ/CQ）、`uart_stats_s`；FC/uart/主循环在各自 init 注册，FTL 每实例组由 `ftl_init` 组合根注册（避免测试直接 `mig_init` 时把栈地址写进注册表） | 各模块 `.c/.h` + 名字表 | 上板 `wr`/`rd` 后计数真实增长（C8）；`--clear` 只清 counter、gauge 不变；行为与改前一致 |
| **S2 原因计数** | `fc.submit` / `fc.alloc` / `ftl.op`（gate/yield）/ `ftl.l2p` load/flush 原因 / `ftl.gc` abort 原因 / `hil`；`mig_abort_e` 重构（单独提交） | `fc_pool.c`、`fc.c`、`ftl_op.c`、`l2p_cache.c`、`migrate.c/.h`、`ftl.c`、`controller.c` | 满盘写复现时，`stats ftl.l2p` + `fc.submit` + `fc.alloc` 能区分 **gate / 队列满 / 额度满 / 无进展**；不再靠 `submit_backpressure` 猜 |
| **S3 空转/无进展** | `fc.fill`、`fc.poll`、`sys.loop`（rounds/progress + `idle_streak_max`）、`ftl.op.drain` 计数对 | `fc.c`、`ftl.c`、`ftl_op.c` | 空闲时 `stats --delta` 显示 rounds 涨、progress 不涨、`idle_streak_max` 增长；有负载时比例合理；与 `pc` 热点交叉验证 |

**顺序理由**：S0 先定契约（API/门控/打印）；S1 证明“注册表 + 多实例 + 打印”真的工作且零行为变化；
S2 才是当前 GC/L2P 问题需要的诊断载荷；S3 依赖 S2 的稳定计数集，最后做。

### 9.2 P1 构建 / 上板检查点

按 P11 与“不反复上板”，P1 只安排 **3 个检查点、2 次上板**：

| 检查点 | 时机 | 内容 |
|---|---|---|
| **A（只编译）** | S0 完成 | `make all` / `make release` + `SSD_STATS_ENABLE=0` 负例；不上板（纯骨架，无用户可见行为） |
| **B（上板 1 次）** | S1 + S2 完成 | 四关 + 烧 release 跑一次满盘写/读；同时验证 **HSQ 回退**（GC 不再 abort）与 S1/S2 的计数分类 |
| **C（上板 1 次）** | S3 完成 | 四关 + 烧 release，空闲 10 s 与负载下各做一次 `stats --delta`，配 `pc` 交叉验证 |

现有 HSQ 回退验证并入 B，不额外增加上板轮次。

### 9.3 P2：把统计变成常用诊断面

| 步骤 | 内容 | 完成判据 |
|---|---|---|
| **P2.1 现有打印迁移** | 已落地口径：**存储只有一份；`stat`/`q`/`jrn`/`cp`/`ecc`/`wear`/`info` 保留为同源紧凑视图**。补齐旧视图里尚未登记、且是**存储字段**的标量：`ftl.alloc`（free/bad/closed）、`ftl.stream`（15 项）、`ftl.jrn`（11 项）、`ftl.gc.state`（6 项）、`ftl.rr`（already_bad_reads）；派生量（pend 计数、`ftl_op_req_count`、live 块数、watermark）继续由旧视图现算，不另建存储。删除旧的 debug `[STAT]` 10 s 心跳（由 `stats --delta` 取代） | 同一语义无第二份存储；`stats` 能覆盖旧视图的标量；`[STAT]` 周期行消失 |
| **P2.2 延迟直方图** | 独立 `stats_hist_s` + `stats_register_hist()`（不占标量条目表）：9 个固定桶 + `over/count/max_us`；已接 **L2P load、GC job、CP job、host 命令**四点。打印 3 行/直方图（摘要 + b0~b4 + b5~b8）避免 192 B 日志行截断；`--delta` 只打当前值；`--clear` 清桶/over/count，`max_us` 保留 | `sum(bucket)+over == count`；`>=50 ms` 进 `over`；四点各只观测一次（L2P 重试先摘旧时间戳，避免把备份的新起点当旧起点） |
| **P2.3 时间维度** | 已落地到 `sys.loop`：`over_1ms` counter + `max_us` gauge；**每 32 轮采样一次**（每轮 `delay_time_now_us` + 除法太贵），时钟失败时跳过该轮 | 空闲/负载下 `max_us` 有界且可解释；打印时按采样口径解释 |
| **P2.4 per-die / 多维** | 已落地代码层泛化：所有 per-instance 组按 `inst` 注册；CLI 支持 `module[inst]` / `module[inst].name` 过滤；容量上界放宽到 32 组 / 320 标量 / 24 直方图（覆盖多 die/target 的代码路径）。per-plane op/spare 与多通道/多 LUN 仲裁计数仍留到拓扑扩展时（H39：不为静态名字表硬编码 `[0]/[1]`） | 多实例时能按 `inst` 区分；不引入第二套 die 坐标 |
| **P2.5 CLI 增强** | 已落地：`--list`、稳定排序、`stats_clear(prefix)`、module/`module.name`/`module[inst]` 过滤、每字段 `_Static_assert(width)`、`--csv`（module,inst,name,value，直方图逐桶一行）、`--brief`（省略直方图桶行）、`SSD_STATS_NAMES=0`（名字表置 NULL，打印 `s<index>`） | 宽度/直方图尺寸断言能编译期失败（C7）；`SSD_STATS_NAMES=0` 构建 0 告警且 `.bin` 无字段名；名字裁剪档字段级过滤关闭（文档化） |

P2 检查点：P2.1（迁移，行为不变）一次上板；P2.2 + P2.3（直方图/时间，带 `/speed` 对比）一次上板；
P2.4 + P2.5（多维与 CLI）一次上板。每步独立提交，不混在一起。

### 9.4 P3：掉电保持的终身计数（SMART 式）

**已裁决**：1A 存储（CP Header padding 内自描述块，不升全局介质版本）、2A 更新节拍（只随 CP）、
3A per-instance 存储 + 聚合打印、4A 成功完成才计数、7A stats CRC 失败只清零计数、
读出口 `smart`、最小计数器集（host 字节 / host 命令 / 上电时长 / 启动次数 / PFR 次数 /
ECC 纠正命令数 / UNC / 编程失败 / 擦除失败）。

| 步骤 | 实现 |
|---|---|
| **P3.0 决策** | 上述 7 项已裁决，登记见 `known_open_items.md` `ISS-STATS-02` |
| **P3.1 存储格式** | `cp_header_s` 的 padding 内放 `lifetime_disk_s`（magic/version/crc32，80 B），**不升** `FTL_MEDIA_LAYOUT_VERSION`；旧固件把它当 padding 忽略，新固件对旧介质视为「无有效块」⇒ 计数从 0 开始。CRC/魔数失败只把计数当 0，CP 记录仍然有效（7A） |
| **P3.2 采集口径** | 字节用 QWORD（磁盘块拆 lo/hi，H27）；事件用 DWORD。QWORD 用 `%llu`（本仓 CAP 行已在用）。host 字节在**成功完成**时计（page0 内部屏障写排除；cache 成员在编程成功时计）；host 命令在 HIL 最终成功回执计；ECC/UNC/编程/擦除失败在 `lifetime_port`（非热对象）按完成命令计 |
| **P3.3 读出口** | CLI `smart`：聚合 per-instance 字节/ECC/UNC/失败 + controller-wide 上电/启动/PFR/命令数；不做主机 Log Page |
| **P3.4 验证** | 正常 POR、掉电中断、旧介质升级（无有效块 ⇒ 计数 0、CP 仍有效）、CRC 损坏；H4/H2testw 无回归。**板端未验** |

**P10 六行契约**

1. **坐标**：per-instance 用紧凑 die 号（host 字节 / ECC / UNC / 编程失败 / 擦除失败）；
   controller-wide 字段（上电时长 / 启动次数 / PFR / host 命令）每 die 记录复制，加载取 max。
   与 LBA/PBA 无映射关系。
2. **表**：`lifetime_disk_s` 嵌在 `cp_header_s` padding 内；`magic/version/crc32`；QWORD 拆 lo/hi；
   总 80 B；`cp_header_s` 仍恰好 2048 B；**不升**全局布局版本。
3. **计数**：RAM `s_inst[LIFETIME_INST_MAX]` + `s_global`；只随 CP 快照落盘（T3 10 s / journal 水位
   1024 写）⇒ 掉电最多丢一个 CP 间隔；`boot_count`/PFR 用 once 标志，每 boot 只加一次。
4. **归属**：存储/打印在 `common/lifetime.c`；FTL 完成接线在 `ftl/lifetime_port.c`（非热对象）；
   CP Header 快照/加载在 `checkpoint.c`；host 命令计数在 HIL。
5. **表示链**：`LIFETIME_DISK_S`/`lifetime_disk_s`、`LIFETIME_CMD_E`/`lifetime_cmd_e`；
   `lifetime_*` API；`smart` 命令；不进 stats 注册表（框架只支持 ≤32 位标量）。
6. **判据**：用户 2026-11-19 裁决；无协议断言（本仓自拟的 CP padding 扩展）；文件定位见证据。

**ITCM 边界**：P3 的完成记账逻辑放 `lifetime_port.o`（不在热代码清单），热路径只留一次调用；
为给 release UT 留 ITCM 余量，`fc_crc_engine.o` 从热清单移到 Flash（产品 ITCM ≈99.2%）。

### 9.5 工作量预估

| 期 | 预估 |
|---|---|
| P1（S0~S3） | `stats.c` ≈ 200 行、`stats.h` ≈ 100 行、CLI ≈ 60 行、各模块登记/插桩 ≈ 80~120 行；名字表 Flash 约 3~4 KB、注册表约 0.5 KB RAM、delta 基线 ≈ 0.5 KB RAM |
| P2 | 迁移以删代码为主；直方图 + 时间 ≈ 150~200 行 + 每直方图约 40 B RAM；CLI 增强逐项独立 |
| P3 | 已实现（代码层）：`lifetime.h/.c` + `lifetime_port` + CP Header 块 + `smart`；板端待验证 |

P1 后 1 target 为 **13 组 / 103 项**；P2 A~E 后为 **20 组 / 154 项 / 4 个直方图**
（`STATS_GROUP_MAX=32`、`STATS_ENTRY_MAX=320`、`STATS_HIST_TOTAL_MAX=24`，为多 die/target 留出上界）。
新增 target/die 前先评估条目上限（满时注册失败并 `LOGE`，不会静默覆盖）。

---

## 10. 风险与开放问题

| 项 | 说明 |
|---|---|
| 清零点误伤 | `stats_clear` 只按 `kind == COUNTER` 逐字段清零；把活状态登记成 COUNTER 是缺陷（review 必查） |
| 宽度/偏移失配 | 名字表带宽度；字段类型改变时靠 review/UT 发现，不靠编译器（`offsetof` 只能发现字段消失） |
| 注册表满 | `STATS_GROUP_MAX` 满或重名不同 base：ASSERT/LOGE 后拒绝；UT 依赖幂等注册 |
| 热路径成本 | 每次事件一个 DWORD 读改写；不在字节循环；release 用 `/speed` 对比 |
| 输出阻塞 | 只在 CLI 命令触发；不周期打印；大组按前缀过滤 |
| 开关边界 | `SSD_STATS_ENABLE=0` 只保证“注册/打印/新插桩”消失；字段 RAM 仍在（本期接受） |
| delta 基线 | `STATS_ENTRY_MAX` 是注册期上限；超出时注册失败。基线只用于差值，不持久化 |
| 已裁决 | `fc.pool` 入表（含 SQ/HSQ/CQ 嵌套 offset）；release 默认 `SSD_STATS_ENABLE=1`（`/speed` 对比后再议）；`mig_abort_e` 进 P1 |
| 已落地（P2） | `--list`、前缀级 `--clear`、module/`module.name`/`module[inst]` 过滤、`--csv`、`--brief`、稳定排序、宽度/直方图尺寸断言、`SSD_STATS_NAMES` 裁剪 |
| 开放（未做） | per-plane op/spare 计数、多通道/多 LUN 仲裁计数（拓扑扩展时；H39 约束）；直方图桶差值（需额外基线 RAM） |

---

## 11. 参考

- lwIP `stats.h`：<https://raw.githubusercontent.com/lwip-tcpip/lwip/master/src/include/lwip/stats.h>（`LWIP_STATS`、每模块结构体、`stats_display`）
- Zephyr `stats`：<https://github.com/zephyrproject-rtos/zephyr/blob/main/include/zephyr/stats/stats.h>（`STATS_SECT_*`、`stats_register`）
- Zephyr `stats` shell 命令（`stats list` 遍历注册组）：<https://github.com/nrfconnect/sdk-zephyr/commit/fc34338d77f009befea5867feb58a9cba79a214f>
- SPDK FTL 官方页：<https://spdk.io/doc/ftl.html>（已核：无公开 stats API）
- NVMe SMART / Telemetry Log 作为“主机可拉取统计 Log Page”的方向对照；本仓资料区尚无 NVMe Base Spec 条目，按 R12 暂不引具体字段，待补进 `docs/reference/README.md` 后再写。
