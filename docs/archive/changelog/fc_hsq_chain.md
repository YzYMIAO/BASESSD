# HSQ 参与命令链：HSQ/SQ 共用同一套填充扫描

**日期**: 2026-10-06
**相关设计**: `docs/design/fc_nand_design.md` §2.7、§4.2（图 2）、§16.4（图 11 配套）、§23.7（图 22）、§23.8
**前置**: `fc_nand_per_lun_fifo.md`（同日，v8 per-LUN FIFO）、`nand_poll_sink.md`（同日）

---

## 概述

`fc_poll_fill()` 的 HSQ 分支原先**不支持命令链**：它逐条 `pop`、`need` 恒为 1、
只走 `fc_poll_dispatch_single()`。本次把 HSQ 分支改成与 SQ **完全相同**的逻辑——
有链就整条取出、`submit_chain()` 原子追加。

实现上把两个分支合并为一个 static helper `fc_poll_fill_queue()`，`fc_poll_fill()` 依次对
`hsq`、`sq` 各调一次。

---

## 改动

### 1. 抽出 `fc_poll_fill_queue()`（`src/fc/fc.c`）

原两个 `{ ... }` 分支体（HSQ 逐条 / SQ 含链）合并为一个：

```c
static fc_status_t fc_poll_fill_queue(fc_t *controller, fc_submission_queue_t *queue,
                                      uint8_t suspended[CH_NUM][CE_NUM][LUN_NUM], uint8_t *dispatched);
```

逻辑即原 SQ 分支：`pop` → `need = fc_poll_chain_length(queue, cmd)` →
`nand_dispatch_lun_free() >= need` 才取 → 弹出整链 → 按 `need` 分流
`fc_poll_dispatch_single()` / `fc_poll_dispatch_chain()` → 否则该 LUN 挂起 + 链头回推。

```c
static fc_status_t fc_poll_fill(fc_t *controller)
{
    ...
    if (fc_poll_fill_queue(controller, scheduler->hsq, suspended, &dispatched) != FC_STATUS_OK) { return FC_STATUS_IO_ERROR; }
    if (fc_poll_fill_queue(controller, scheduler->sq,  suspended, &dispatched) != FC_STATUS_OK) { return FC_STATUS_IO_ERROR; }
    ...
}
```

**HSQ 优先仍然成立**：扫描顺序不变（HSQ 先），且两者**共享同一张 `suspended` 表**——
HSQ 因 room 不足挂起某 LUN 后，SQ 本轮也跳过该 LUN。否则同 LUN 的 SQ 命令会插到
HSQ 命令之前下发，优先级倒挂。这是把分支合并后必须显式说明的一条不变量。

### 2. `fc_poll_chain_length()` 参数改名 `sq` → `queue`

函数体本就只依赖 `fc_submission_queue_peek()`，与队列身份无关；HSQ 复用后改名以正名，
文档行「普通提交队列」→「提交队列，HSQ/SQ 通用」。

### 3. 行为差异（HSQ 有链时）

| 场景 | 改前 | 改后 |
|---|---|---|
| HSQ 链头 `chain=1` | 只取链头一条下发，成员当独立命令后续再取 | 整链取出，`submit_chain()` 原子追加 need 槽 |
| room 不足 | 逐条塞满为止（部分推进） | 整链不取，该 LUN 本轮挂起（链原子性优先） |

链原子性是链存在的理由（§16.1：Read Retry 三命令不可被插入），"部分推进"会破坏它。
SQ 分支早已是这个取舍，HSQ 现在与之一致。

---

## 影响面

- **当前为潜伏路径**：全仓**没有任何生产者**设置 `high_priority = 1`——
  `fc.c:1053` 的 `parity_cmd->high_priority = data_cmd->high_priority` 只是 stripe parity 继承，
  而 data 命令侧同样无人置位；命令级 `config.chain != FC_CHAIN_END` 在 `src/ftl/` 也无生产者
  （23 处全为 `FC_CHAIN_END`）。即：**生产运行时 HSQ 恒空、链恒为单命令**，本次改动不改运行时行为，
  修的是"能力缺口"——HSQ 一旦被启用（或 FTL 启用链），旧写法会把链静默拆成 N 条独立命令。
- 生产路径逐位等价：HSQ 空 → 第一次调用扫 0 条；SQ 分支逻辑与改前逐行相同。

---

## 验证

| 项 | 结果 |
|---|---|
| `make format-check` | 通过 |
| debug | 零警告（152448B） |
| release（`NDEBUG`） | 零警告（86612B；`fc_poll_fill_queue` 在 ASSERT 外被调用，无 `-Wunused-function`） |
| `make -k tests` | 仅卡在用户维护的 `test_fc_level1.c`（旧 scheduler API）与 `test_fc_level3.c:48`（旧 `set_device_ready`），与本次改动无关 |

**上板验证未做**（用户执行）。建议回归：常规 `wr/rd/tr/fl/info` 序列即可；
若要覆盖 HSQ+链，需先在 FTL 侧造出 `high_priority=1` / `chain=1` 的命令（当前无生产者）。
