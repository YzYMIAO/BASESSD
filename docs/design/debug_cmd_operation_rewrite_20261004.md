# 擦除命令 `config.operation` 18→17 排查记录 — 固件侧零改写，疑为调试器读数

> **调试日期**: 2026-10-04
> **涉及模块**: `tests/unit/test_fc_level3.c`（加插桩，现已注释保留）、`src/fc/fc.c`（只读）、`src/nand/nand_dispatch.c`（只读）、`src/platform/stm32h743/sys.c`（只读确认 MPU）
> **触发测试**: `test_block_write_read`（擦除 → 64 页写 → 64 页读）
> **芯片**: STM32H743II + MT29F4G08ABADAWP NAND
> **现状**: 固件侧结论已确认（CPU 侧逐字段比对零差异）；**用户观测到的 18→17 本身仍未复现**，主因候选是调试器读可缓存内存读到陈旧值，见 §5/§6

---

## 1. 现象

在 `test_block_write_read` 的擦除命令上，调试器观察 `cmd->config.operation`：
提交前 18（`OP_ERASE_BLOCK`），`fc_poll_completion` 返回后变成 17（`OP_PROGRAM_PAGE_CACHE`）。

18 = `0b10010`、17 = `0b10001`，**不是单比特翻转**，所以不像是位翻转类损坏，更像一次真实的赋值。

## 2. 排查手段

在 `run_lifecycle`（submit → service loop → poll → release 的强等封装）里加插桩：
提交前给 `cmd` 拍整结构体快照，release 前逐字段比对并打印差异。

- `cmd_snapshot()` / `cmd_diff_report()` / `CMD_FIELD_U` 宏 —— 现以 `#if 0` 整块保留在 `tests/unit/test_fc_level3.c`（`run_lifecycle` 上方），复活说明写在块头
- `run_lifecycle` 内两处调用改为注释，并指向该块
- 比对覆盖 `fc_command_t` 全部 28 项（含 `config` 的 13 个位域 + 整字兜底比对）

## 3. 结论：固件侧零改写

上板跑 `test /fc-nand/lifecycleblock_write_read`：**PASS，`0/28 fields changed`**。

即整条命令从 `fc_submit` 到 `fc_command_release`，**`fc_command_t` 的每一个字段都没被动过**——包括 `config.operation`。

这个比对是 CPU 侧普通 load 读的，与 D-Cache 自洽，所以对「代码有没有改写它」是权威答案。

## 4. 全树 `operation` 写入点清单（供复查）

### 4.1 写 `command->config.operation`

| 位置 | 写入者 |
|---|---|
| `src/fc/fc.c:1002` | parity 命令，`OP_PROGRAM_PAGE_MULTI_PLANE` |
| `src/ftl/*.c` | 各模块自建命令（`OP_NORMAL_PROGRAM_PAGE` / `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` / `OP_ERASE_BLOCK` / `OP_SET_FEATURES` …） |
| `tests/unit/test_fc_level3.c` | 各用例自建命令 |

**`fc.c` / `nand_dispatch.c` 在命令提交后没有任何一处写 `command->config.operation`。**

### 4.2 写 `request->operation` / `task->operation`（不是 `cmd`）

| 位置 | 动作 | 运行时是否生效 |
|---|---|---|
| `src/fc/fc.c:1222` | `request->operation = command->config.operation` | 是（单向复制） |
| `src/fc/fc.c:1338` | `nand_operation_to_issue()` 拆 ISSUE/WAIT | **否**：`LUN_NUM == 1`（`include/nand/nand_config.h:29`），该分支不执行 |
| `src/nand/nand_dispatch.c:139` | `task->operation = request->operation` | 是 |
| `src/nand/nand_dispatch.c:494` | `task->operation = pair_op` | 仅 `LUN_PHASE_ISSUED` 时 |
| `src/nand/nand_dispatch.c:608` | `task->operation = OP_RANDOM_DATA_READ` | CW 链续读 |

即使把 `LUN_NUM` 开到 2，擦除也只会走 18 → 39(`ERASE_BLOCK_ISSUE`) → 40(`ERASE_BLOCK_WAIT`)，**永远不会变成 17**（`[18].pair_op = 39`、`[39].pair_op = 40`、`[17].pair_op = 0`）。

## 5. 调试器读数可信度：命令池在可缓存写回内存里

这是本轮最可复用的发现。

- `g_fc_command_pool`（`src/fc/fc.c:69`）**没有 section 属性** → 落默认 `.bss` → 链接脚本 `> AXI_RAM`（`linker/stm32h743xx_flash.ld:121`），基址 `0x24000000`
- MPU region 3（`src/platform/stm32h743/sys.c:14`）：`0x24000000` / 512KB / `IsCacheable = CACHEABLE` + `IsBufferable = BUFFERABLE` + `TEX0` → **Normal, Write-Back, Write-Allocate**
- 只有最后 128KB（`0x24060000`，region 11，`NOT_CACHEABLE`，`sys.c:28`）留给 DMA/NAND buffer（`.noncacheable_buffer` → `AXI_NOCACHE`），`.bss` 不在其中

**后果**：Cortex-M7 的 D-Cache 与调试器不一致。SWD 经 AHB-AP 读内存**直接走总线、绕过 D-Cache**；CPU 的普通 load 走 cache。当某条 cache line 是 dirty 且尚未写回时，调试器读到的是 **SRAM 里的旧值**，而 CPU 看到的是 cache 里的新值。

命令池 64 槽 × 24B，跨用例复用槽位是常态，因此「调试器显示的 op 与代码逻辑对不上」在这里有充分的物理基础。

**注意区分**：这一条解释的是「调试器读到的值与 CPU 不一致」。它最自然的症状是**持续读到旧值**，而不是「先读到 18、后读到 17」。后者要成立还需额外条件（例如第一次的 18 来自寄存器驻留值而非内存读），见 §6。

## 6. 未决问题

用户指出：`test_block_write_read` 里擦除是**第一条命令**，**执行前没有任何地方用过 17**。所以「槽位被上一个 `OP_PROGRAM_PAGE_CACHE` 住户留下 17」这个假设站不住（除非同一次上电先跑过整个套件、`test_block_cache_ops` 写过 17，且此后未复位）。

因此 18→17 目前**没有闭合解释**。待确认：

1. 观测顺序究竟是 18 在前还是 17 在前
2. 用的是哪个工具（STM32CubeIDE / GDB+OpenOCD / 其他），watch 表达式具体是什么（`cmd->config.operation` 还是别的）
3. 观测时是否刚跑过其他套件、期间有没有复位

## 7. 复现 / 验证方法

想自己验「是不是调试器读陈旧」：

1. 在观察点前临时加一句 `SCB_CleanDCache();`（工程内已有用法，如 `src/platform/stm32h743/usart.c:141`），重烧后再看——若 18 出现，即 cache 陈旧实锤
2. 临时给 `g_fc_command_pool` 加 `__attribute__((section(".noncacheable_buffer")))`，让 CPU 与调试器看到同一份内存，做 A/B
3. 用 CPU 侧打印代替调试器观察（即 §2 的插桩，结论见 §3）

## 8. 本次附带改动

- `tests/unit/test_fc_level3.c` 的 `setup_level3()` 里 `[RES] L3_start: cmd=…/64 desc=…/256 buf=…/128 sq=… hsq=… cq=…` 一行**注释掉**（排查池泄漏时再打开），正常跑保持静默
- 结果：`make tests` 零告警零错误，**198968 B**（插桩前 202248 B，注释后反而更小）；`make format-check` 通过
