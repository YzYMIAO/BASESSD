# FC→NAND 分发重构：per-LUN FIFO（深 4）

**日期**: 2026-10-06
**相关设计**: `docs/design/fc_nand_design.md` §2.7、§4.2/§4.3（图 9）、§16、§18.2、§18.6/18.7、§20、§23.5–23.9
**前置**: `fc_cw_subset_read.md`、`fc_flat_cw_mask_cq_param.md`（同日）

---

## 概述

FC 的链式分发路径有两个叠加缺陷，都是"链只在单测里跑、从没端到端"掩盖下来的：

1. **忙 LUN 提交被当失败**——`fc.c` 把 `nand_dispatch_submit` 的非 OK（含 `NOT_READY`）当失败，标 `NAND_FAIL`；而本该跳过忙 LUN 的 `fc_scheduler_target_is_busy` 是**死代码**（`fc_scheduler_set_busy`/`clear_busy` 生产零调用）。FC 于是会往忙 LUN 塞命令并把它判死。
2. **一条链只回一条 completion**——NAND 只在链尾返回 completion（`command_id` = 链尾那条）。FC 把链的 N 条命令全置 ACTIVE，但只有链尾到达 COMPLETED，**其余 N−1 条永久泄漏**；且中间命令（读恢复链里的 RETRY_READ）的 `cw_status[]` 填了却从不外传。

本次把 per-LUN 结构从"**执行中的链**"改成"**待处理 FIFO（深 4）**"：FC 扫描提交队列按 LUN 填充，NAND 遍历 FIFO 逐条排空。一次重构同时解掉两个缺陷——因为**每个槽 = 一条命令 = 一份 completion**，链的顺序/原子性由"原子批量入队 + FIFO 严格顺序"保证，NAND 侧看不到"链"。

**范围**：FC + NAND dispatch。**读恢复（`src/ftl/read_recovery.c`）保持同步、不改**，只保证编译通过。

## 方案对比（为何选 per-LUN FIFO）

三个待解缺陷：**(i)** 忙 LUN 提交被当失败；**(ii)** 同 LUN 待处理命令能否排队；**(iii)** 一条链 N 条命令各自完成。

| 方案 | (i) | (ii) 排队 | (iii) 每命令完成 | 动 NAND | 改动量 |
|---|---|---|---|---|---|
| α 最小（纯 FC） | ✓ | ✗ | ✗ | 否 | 小 |
| β α + NAND 逐 task 回 | ✓ | ✗ | ✓ | 小 | 小-中 |
| γ FC 软件串行链（纯 FC） | ✓ | ✓(仅链内) | ✓ | 否 | 中 |
| δ 保留 channel FIFO(64) 放开 | ✓ | ✓(深 64) | ✓ | 中 | 中 |
| **ε per-LUN FIFO(4)** ← 选定 | ✓ | ✓(深 4) | ✓ | 大 | 大 |

选 ε 的理由：**结构最统一**——每槽 = 一条命令，NAND 侧看不到"链"（链 = 连续槽），逐条 completion 天然落地；per-LUN 保序 + 原子批量入队即给出链的顺序/原子/不可插入三条语义，不需要在 `nand_dispatch_task_t` 里加链标记。代价是重写硬件已验证的 dispatch 核心（见「风险」）。

## 核心不变量

1. **每槽 = 一条命令**：NAND 逐条回 completion；FC 侧 `fc_complete_from_nand` 不变（本就是"一条命令一份"）。
2. **链原子**：`submit_chain` 先查 `room >= N` 再整体 append，失败**无副作用**（一个槽都不写）。
3. **链不可插入**：连续槽 + FIFO 顺序；同 LUN 的后续命令（含 HSQ）只能排到链后。
4. **per-LUN 保序**：填充时某 LUN 本轮挂起（`suspended[][][]`），本轮不再取该 LUN 的后续命令。
5. **`tasks[head]` 恒为当前执行/待执行槽**，完成才 `head++`/`count--`。
6. **命令内 CW 链不受影响**（`nand_dispatch_task_cw_chain` 是"一条命令内多 CW"，与命令链无关）。

## 改动

### 1. 数据结构（`src/nand/nand_dispatch_internal.h`）

`nand_dispatch_lun_chain_t` → `nand_dispatch_lun_queue_t`：

```c
typedef struct
{
    nand_dispatch_task_t tasks[NAND_DISPATCH_CHAIN_DEPTH]; /* 4 槽，沿用现有内存 */
    uint8_t              head;              /* 出队游标：执行中/待执行槽 */
    uint8_t              count;             /* 占用槽数（tail = (head+count)%DEPTH） */
    uint8_t              phase;             /* 当前执行槽的 LUN_PHASE_* */
    uint32_t             wait_start_cycles; /* Phase V：ISSUE 完成时间戳 */
} nand_dispatch_lun_queue_t;
```

- **无链标记**：链 = 连续 N 槽；`cursor` 概念消失，`tasks[head]` 恒为队头。
- 内存不变：仍是 `[CH_NUM][CE_NUM][LUN_NUM]` 个、每个 4 个 task。
- 相位表注释同步重写（写入：submit/submit_chain 新槽、lun_step 出队后复位 IDLE；ISSUE/ISSUED/FETCH 的置位与读点）。

### 2. NAND 侧（`src/nand/nand_dispatch.c` + `include/nand/nand_dispatch.h`）

| 项 | 变化 |
|---|---|
| `nand_dispatch_enqueue`（新 static） | 原子批量追加：`(uint16_t)(count + queue->count) > DEPTH` → `NOT_READY` 无副作用；否则逐槽 `task_load` 到 `tasks[(head+count+i)%DEPTH]`，**全部预加载完才提交 `count`**（poll 看不到半挂载的批） |
| `nand_dispatch_submit` | 保留 `g_nand_device_ready == 0` → `INVALID_ARGUMENT` 门控，再 `enqueue(request, 1)` |
| `nand_dispatch_submit_chain` | 直接 `enqueue(requests, count)`（不查 device_ready，保持原不对称语义） |
| `nand_dispatch_lun_free`（新） | 返回 `DEPTH - count`；越界参数返回 0（供 FC 判断能否放下整链） |
| `nand_dispatch_lun_step`（新） | 单 LUN 单步：ISSUED 预检 → `lun_drive` → `task_cw_chain` → `scatter_page_buffer` → **回填 completion + 出队（head++/count--）+ phase=IDLE + 通道游标前进** |
| `nand_dispatch_poll` | 骨架改为 round-robin 遍历通道 → CE → LUN，每 LUN 单步；`count==0` 跳过；`ERROR` 丢弃该 LUN 队列并返回 `INVALID_ARGUMENT`；`DONE` 返回 `ctx.result` |
| **删除** | `nand_channel_queue_t` / `g_nand_channel_queues` / `nand_dispatch_queue_entry_t` / `nand_dispatch_queue_consume_match` / `nand_dispatch_chain_advance` / `nand_dispatch_phase1_lun` / `nand_dispatch_phase2_dequeue` / `nand_dispatch_task_build_request`（并入 `task_start`）/ `NAND_CHANNEL_QUEUE_DEPTH` / `NAND_DISPATCH_LUN_CONTINUE` 枚举值 |

- 相位机（Phase V：IDLE → ISSUE → ISSUED → FETCH）逻辑**不变**，只是从"链对象"挂到"FIFO 队头槽"上；`ctx->chain` → `ctx->queue`、`chain->cursor` → `queue->head`。
- 逐 task 的 CW 链（`nand_dispatch_task_cw_chain`）**不动**——那是命令内多 CW，与命令链无关。

### 3. FC 侧（`src/fc/fc.c` + `fc_scheduler.c/.h`）

- **busy 表废弃**：删 `fc_scheduler_set_busy`/`clear_busy`/`fc_scheduler_target_is_busy` 及 `busy[][][]` 字段；"忙"由 `nand_dispatch_lun_free` 取代。`fc_scheduler_t` 只剩 `{sq, hsq}`。
- **删 `fc_scheduler_select` / `fc_scheduler_select_chain`** 与读饥饿保护（`FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET`、`fc_scheduler_op_is_read`、`consecutive_write_count`）。
- **新增 `fc_poll_fill()`（一次 pass，`fc_poll_internal` 内）**：
  1. 快照 HSQ 数、SQ 数；`suspended[CH_NUM][CE_NUM][LUN_NUM]` 清零。
  2. 先扫 HSQ（HSQ 命令不参与链，`need=1`），再扫 SQ。**HSQ 优先只体现在扫描顺序**，不抢已进 FIFO 的命令。
  3. 逐条按 LUN 保序：`need = fc_poll_chain_length(...)`；`nand_dispatch_lun_free(ch,ce,lun) >= need` → 取出（链则整条）→ `submit`/`submit_chain`；否则该 LUN 本轮挂起 + 命令回推队尾。
  4. FIFO 满或链放不下 → 该 LUN 停止入队。扫完即止（不因回推的命令再入队）。
- **新增 `fc_poll_chain_length()`**：链头 `chain==END` → 1；否则 peek 后续、收集同 `(ch,ce,lun)` 的连续命令直到 `chain==END` 或 `FC_SCHEDULER_CHAIN_MAX`。**只 peek 不消耗**——链放不下时可原样留在队列。
- **`fc_poll_dispatch_single/chain`**：`submit` 结果改为
  ```c
  ASSERT(submit_result != NAND_RESULT_NOT_READY);   /* 填充前已查 room */
  if (submit_result != NAND_RESULT_OK) { LOGW(...); fc_poll_fail_command(...); return FC_STATUS_IO_ERROR; }
  ```
  `NOT_READY` 不再被当失败标 `NAND_FAIL`。
- **保序实现**：把队头 pop 后按需回推队尾（`pop+push` 只动 head/tail 指针，池内容不变）。**一次完整 pass 等价于一次轮转**，未取出命令的相对顺序不变；同 LUN 的命令因 LUN 挂起必然一起回推，链头连链不被拆散。

### 4. 防御性边界

`fc_command_validate` **不校验 ch/ce/lun 范围**，旧调度器却用 `busy[config.ch][config.ce][config.lun]` 无保护索引。`fc_poll_fill` 因此对越界的命令**原样回推**（不索引数组），避免引入新的越界写。

## 风险

1. **重写硬件已验证的 `nand_dispatch.c` 核心（最高）**：Phase V 相位机、ISSUE/WAIT/FETCH、`started` 双语义都要在 FIFO 下标下重验。**必须上板跑全量 `make tests`**。
2. **per-LUN 深度 4 很浅**：一条 3 命令读恢复链只剩 1 槽，HSQ/正常 IO 常插不进。已定**保持 4**——SQ 深度 64 仍兜底积压，FIFO 只是交接缓冲，可接受。
3. **填充保序实现**：环形队列中间删除 + 链头连链易写错；用"pop + 回推"规避（等价一次轮转）。
4. **HSQ 语义变化**：HSQ 只体现"扫描顺序"，不抢已进 FIFO 的命令——与旧"忙则拒收"不同。
5. **`g_nand_channel_cursor` 保留**：通道级 round-robin 游标仍用于 LUN 遍历公平性（`lun_step` 完成时前进）。

## 验证

1. `make format` + `make clean && make debug` / `make release` 三档**零警告**通过。
2. 静态审计：`src/` 无 `busy[`、无 `g_nand_channel_queues`、无 `queue_consume_match`、无 `chain->cursor`、无 `fc_scheduler_select`。
3. **`make tests` 当前编译失败**（唯一失败点）：`tests/unit/test_fc_level1.c` 仍引用已删 API——`fc_scheduler_set_busy` / `fc_scheduler_clear_busy`、`scheduler.busy`、`scheduler.consecutive_write_count`、`fc_scheduler_select`、`fc_scheduler_select_chain`。按平台规则测试源文件由用户维护，**待用户同步更新**。
4. **上板未验证**（按平台规则由用户执行）。回归护栏：现有全部读/写/FEATURES 用例必须通过。
5. **需新覆盖的上板用例**：单 LUN 多命令排队、同 LUN 链不被打断、异 LUN 并行、HSQ 优先。
