# 删除软件寄存器影子：`fc_registers` / `fc_register_queue_state()`

**日期**: 2026-10-07
**相关设计**: `docs/design/fc_nand_design.md` §2.8（重写为墓碑小节）、§2.7、§22.3、§22.5、§23.4、附录 A 图 19
**前置**: `fc_structural_changes.md`（该影子"逐点刷新、不收敛"的由来）、`fc_nand_per_lun_fifo.md`

---

## 概述

FC 控制面曾有一套 `fc_registers_t` 影子结构（`config` / `status` / 三对 head-tail /
`interrupt_enable` / `interrupt_status`），由 `fc_register_queue_state()` 在 11 个调用点刷新，
设计意图是"模拟硬件寄存器风格，方便未来硬件化"。

**v10 整体删除**：删 `include/fc/fc_registers.h`、`src/fc/fc_registers.c`、
`fc_t.registers` 成员、`fc_register_queue_state()` 及其 11 处调用、`fc_registers_reset()` 唯一调用。
**不新增任何替代 API**（队列用量本就有现成路径）。

净效果：**-2 文件**（49 行）+ 结构体少一个成员；debug 二进制 152128 → 151708 B，
release 87016 → 86808 B。

---

## 动机

### 1. 起点是性能问题（用户提出）

> `fc_register_queue_state(controller);` 对于这个函数，FC 层好多地方都在调用，每次调用都会跑很多判断，
> 有方法优化一下吗

核实成本后发现：唯一非平凡的开销是**对 `command_state[0..63]` 的 64 字节线性扫描**
（找 `FC_COMMAND_STATE_ACTIVE`，命中即 `break`，空闲时要扫满 64 项），其余只是 6 次比较 + 8 次赋值。
而调用频率不低——`ftl_service` 超级循环里每轮最多调 4 次 `fc_service()`，每次 `fc_poll_internal`
末尾一次，再加每次 CQ 弹出、每条命令派发各一次。

### 2. 用户的关键一问

> 我这个的设计是准备模拟硬件行为，然后获取相关的 registers 信息，不过以当前的设计 controller 的
> 结构本身就可以当作寄存器，registers 这个还需要吗？

逐字段核对 `fc_registers_t`（10 个字段）后，答案是**不需要**：

| 字段 | 结论 |
|---|---|
| `sq_head` / `sq_tail` / `hsq_head` / `hsq_tail` / `cq_head` / `cq_tail`（6） | 与 `controller->sq`/`hsq`/`cq.head/tail` **逐字节重复** |
| `config`、`interrupt_enable`、`interrupt_status`（3） | **孤儿**：全仓零读零写 |
| `status`（1） | 唯一的派生量，缓存着 controller 里已有的信息 |

全仓（`src/` / `include/` / `tests/`）grep 确认**零读者**：没有任何代码读 `controller->registers.*`。

### 3. "模拟硬件"这条理由本身站不住

真硬件寄存器**每次访问都读当前值**；而这个影子只在 11 个点刷新，且在早退路径上**故意保留陈旧值**
——`fc_structural_changes.md` 记录过：初版把散落调用收敛到骨架末尾，复查发现链式选择成功时会先从
HSQ/SQ 弹出命令，于是"命令 id 非法/状态非 QUEUED"的早退路径上队列计数已变而寄存器未刷，
收敛版反而会刷新 → 属可观测行为差异 → 回退为逐点调用。

也就是说，这个影子**比"读时计算"更不像硬件**；对一个异步 DMA 读者也不可能正确（只在 11 个点更新）。

**用户决定：「整个删掉」**——不保留、不加替代 API。

---

## 改动

### 1. 删除文件

- `include/fc/fc_registers.h`（34 行）
- `src/fc/fc_registers.c`（15 行）

`src/fc/makefile.mk` 用 `$(wildcard $(SRC_ROOT)/fc/*.c)` 收集源文件，**无需改 makefile**。

> ⚠ 区分：`FC_STATUS_OK` / `FC_STATUS_IO_ERROR` / `FC_STATUS_INVALID_ARGUMENT` 是
> `fc_interface.h` 的**对外状态枚举**（FTL 大量使用），与本次删除的 `FC_STATUS_READY` /
> `FC_STATUS_BUSY` / `FC_STATUS_*_NOT_EMPTY` / `FC_STATUS_ERROR` **位掩码无关**，未动。

### 2. 结构体与 include

| 位置 | 改动 |
|---|---|
| `include/fc/fc_internal.h` | 删 `#include "fc_registers.h"`、删 `fc_t.registers` 成员 |
| `src/fc/fc.c` | 删 `#include "fc_registers.h"` |
| `src/fc/fc_interface.c` | 删 `#include "fc_registers.h"` |

### 3. 函数与调用点（`src/fc/fc.c`）

删 `fc_register_queue_state()` 定义 + 函数头注释块，及 11 处调用：

| 所在函数 | 处数 |
|---|---|
| `fc_init_internal` | 1 |
| `fc_submit_internal` | 1 |
| `fc_complete_internal` | 1 |
| `fc_poll_dispatch_single` | 3 |
| `fc_poll_dispatch_chain` | 2 |
| `fc_poll_internal` | 2 |
| `fc_poll_completion_internal` | 1 |

另删 `fc_registers_reset(&controller->registers);`（`fc_init_internal` 内，全仓唯一调用）与其注释，
以及文件结构清单、`fc_poll_internal` 文档块中对 `fc_register_queue_state` 的引用性描述。

> **顺带纠正一处陈旧数字**：设计文档与旧 changelog 里的"13 处"已过时——`17b7504` 让
> `fc_build_dispatch_request` 返回 `void`，删掉了两个早退刷新点，实际已是 11 处。本次整段删除。

---

## 不做的事

- **不新增替代 API**。队列用量已有现成路径：`fc_get_sq_count()` / `fc_get_hsq_count()` /
  `fc_get_cq_count()` / `fc_command_state_get()`（`fc.c`），以及聚合函数 `fc_get_resource_stats()`
  （`fc_interface.c`，被 `fc_assert_context.c` 与两个测试使用，读的是 controller 实时值）。
- **不重新暴露 ACTIVE/BUSY 汇总位**（YAGNI）：全仓无读者；将来真要，`fc_get_resource_stats()`
  里 3 行即可补上。

---

## 验证

| 项 | 结果 |
|---|---|
| `make format-check` | 通过 |
| debug | 零警告（151708 B） |
| release（`NDEBUG`） | 零警告（86808 B）——`static` 的 `fc_register_queue_state` 连定义一起删，无 `-Wunused-function` |
| `make -k tests` | 仅卡在**用户维护**的 `test_fc_level1.c`（旧 scheduler API）与 `test_fc_level3.c:48`（旧 `nand_dispatch_set_device_ready`），与本次改动无关 |
| 静态审计 | `fc_registers` / `fc_register_queue_state` / `FC_STATUS_READY|BUSY|*_NOT_EMPTY|ERROR` / `FC_CONFIG_*` / `interrupt_enable|status` / `->registers` 在 `src/`+`include/`+`tests/` 全仓 **0 命中** |

**上板验证未做**（平台规则：用户执行）。本次为纯删除，正常路径行为不变，
常规 `wr/rd/tr/fl/info` 回归即可。
