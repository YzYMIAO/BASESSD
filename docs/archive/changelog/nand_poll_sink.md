# NAND 分发层收尾：poll 改 sink 回调 + 三层守卫整条删除

**日期**: 2026-10-06
**相关设计**: `docs/design/fc_nand_design.md` §4.2（图 2）、§4.3（图 9）、§4.4、§15.2、§18.2
**前置**: `fc_nand_per_lun_fifo.md`（同日，v8 per-LUN FIFO）
**判据**: `docs/design/assertion_guidelines.md` §十、§十附（收法 1/2/3）

---

## 概述

v8（per-LUN FIFO）落地后的一轮收尾，四件事，都属"NAND 分发层"：

1. **`nand_dispatch_poll` 改为 sink 回调 + 全遍历不早退**（行为/API 变更，见下）。
2. **req 表三层守卫整条删除**——`req_*` handler 返回码 → `START_REQ_ERROR` → `LUN_ERROR` → poll 丢弃队列，
   守的是同一件不可能的事。
3. **`g_nand_device_ready` → `g_nand_dispatch_armed`**，并把放行门从 `nand_dispatch_submit` 收进两者共用的
   `nand_dispatch_enqueue()`（`submit_chain` 自 Phase D 起漏查）。
4. 断言补扫第三/四轮（判据见 assertion_guidelines.md §十/§十附）。

---

## 1. poll 改 sink 回调

### 旧实现的两个问题

```c
nand_result_t nand_dispatch_poll(nand_dispatch_completion_t *completion);
/* 内部：round-robin 遍历通道（游标 g_nand_channel_cursor），碰到第一条完成就 return */
```

- **公平性**：早退导致索引靠前的 LUN 系统性抢占；游标**只轮转通道一维**，`ce`/`lun` 恒从 0 起。
  `CH_NUM=1` 时游标恒为 0，等于没转 —— 多 LUN 下靠后的 LUN 步数递减、可被饿死。
- **返回码混用**：`return ctx.result` 把**命令执行状态**当成"poll 是否产出完成"的返回码。

### 新实现

```c
typedef void (*nand_dispatch_sink_t)(const nand_dispatch_completion_t *completion, void *ctx);
uint8_t nand_dispatch_poll(nand_dispatch_sink_t sink, void *ctx);  /* 返回本轮完成条数 */
```

- **全遍历、不早退**：每个 LUN 每轮恰好 step 一次 → 偏斜从根上消失，**`g_nand_channel_cursor` 整个删除**。
- **完成经 sink 逐条交出**：调用方在回调里当场消费，poll 只用一个 completion 槽复用，**无需批量数组**。
- **返回值只表"产出几条完成"**；命令执行状态由 `completion.cw_status` 承载
  （completion handler 把 `NAND_RESULT_*` 映射成 `FC_CW_STATUS_*`）。

### 调用方（`fc.c`）

新增 `fc_nand_completion_sink()`：每条完成当场 `fc_complete_from_nand()` + 清 stripe parity。
`fc_poll_internal` 改为"抽干本轮全部完成，无完成才走填充 pass"。

**失败出口只剩一个**：`fc_complete_from_nand != FC_STATUS_OK`（CQ 满强制 COMPLETED 防泄漏）→ 抽干后返回
`IO_ERROR`。**NAND 命令自身的失败不是 FC 故障**——状态已随 completion 进了 CQ entry；旧实现据此早退会
把同一次调用里其他 LUN 的完成一起丢掉。

### 为何不用批量数组

曾考虑 `nand_dispatch_poll(out[], max)` + 编译期容量宏 `CH_NUM*CE_NUM*LUN_NUM`。否决理由：每条完成都是
**当场处理完即丢**（写 desc/ECC、跑数据路径、组 CQ 项、清 parity），N 条完成不需要 N 个 buffer —— 一个槽
复用即可。数组方案额外引入"开多大/会不会爆栈"的问题，而 sink 方案没有。

---

## 2. req 表三层守卫整条删除

`nand_dispatch_req_table` 的 12 个 `req_*` handler 原签名 `uint8_t (*)(task)`，每个都无条件
`return NAND_RESULT_OK;`。调用点仍写着：

```c
if (nand_dispatch_req_table[task->operation](task) != NAND_RESULT_OK) return START_REQ_ERROR;
/* 上层 */ switch (task_start(...)) { case START_REQ_ERROR: return LUN_ERROR; ... }
/* poll  */ if (out == LUN_ERROR) { queue->count = 0; queue->phase = IDLE; return INVALID_ARGUMENT; }
```

三层守同一件不可能的事：handler 的输入在 `nand_dispatch_task_load()` 时已就绪且不变量已断言，**构建没有
失败路径**。改法：

| 位置 | 改动 |
|---|---|
| `nand_dispatch_tables.c` | 12 个 `req_*` → `void`，删 12 处 `return NAND_RESULT_OK;` 与过时「返回：」文档 |
| `nand_dispatch_internal.h` | `nand_dispatch_req_handler_t` → `void (*)(nand_dispatch_task_t *)` |
| `nand_dispatch.c` | `start_outcome_t` 收成 `{ASYNC, SYNC}`（删 `START_REQ_ERROR`）、`lun_outcome_t` 收成 `{WAIT, DONE}`（删 `LUN_ERROR`）；`task_start` 改裸调用；两处 `switch` 改两行 `if`；poll 删死丢弃分支 |

**判据**（assertion_guidelines.md §十附 收法 3）：函数返回值被证明恒为某常量时，"保留返回码当表的扩展点"
不是保守而是负担 —— 它让每个调用点都写一条永远不走的错误分支。**与收法 2 的区别**：收法 2 是守卫**可达**
但重复（收敛位置）；收法 3 是守卫**不可达**（整条删）。

---

## 3. `armed` 改名 + 放行门收敛

- `g_nand_device_ready` → `g_nand_dispatch_armed`（`set_device_ready`/`is_device_ready` 同步改名）。
  旧名与 ONFI 的 "device ready"（R/B# / status bit 6，**per-die 且瞬态**）撞名；实际语义是 boot 期一次性的
  "FMC 已由同步初始化路径移交给异步 dispatch"。per-LUN 运行时就绪由 `nand_dispatch_lun_free()` + 引擎
  WAIT_READY 承担，**不需要**缓存成标志。
- 放行检查从 `nand_dispatch_submit` **收进两者共用的 `nand_dispatch_enqueue()`** —— `submit_chain` 自
  Phase D（`eeedb8f`）起漏查，此前只因生产链长恒 1 未暴露。

---

## 验证

| 项 | 结果 |
|---|---|
| `make format-check` | 通过 |
| debug | 零警告 |
| release（`NDEBUG`） | 零警告（确认删 `fc_command_id_is_valid` 后无 `-Wunused-function`） |
| `make -k tests` | 仅卡在用户维护的 `test_fc_level1.c`（旧 scheduler API）与 `test_fc_level3.c:48`（旧 `set_device_ready`） |

**上板验证未做**（用户执行）。全部为断言策略 / 契约收窄 / poll 遍历模型改动，生产路径（`CH_NUM=CE_NUM=LUN_NUM=1`、
FTL 恒 `FC_CHAIN_END`）下 poll 行为与旧实现逐位等价：单 LUN 时"全遍历"与"扫到即返回"扫描集合相同。
