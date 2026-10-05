# FTL `addr_to_cmd` 漏配 desc 链回归

**日期**: 2026-10-05
**分支**: main
**引入提交**: `09c9622`（v0.28「同步操作内联化」，2026-09-30）
**修复**: 当前工作区改动（`include/ftl/ftl_fc_interface.h`，未提交）
**发现方式**: 上板 `make tests` 断言停机（`src/fc/fc_interface.c:258`）

---

## 概述

v0.28 把 FTL 的同步 NAND 操作从包装函数内联化时，新增了 `addr_to_cmd()` 作为 `ftl_cmd_setup_ppa()` 的替代（头文件注释声称二者**等价**）。但 `addr_to_cmd()` **漏掉了 `fc_command_configure_descriptors()` 调用**，导致自 v0.28 起 FTL 全部 9 个模块的同步读写页操作都是**静默空转**：数据从未搬进 NAND buffer，命令在 `fc_submit` 阶段就被拒绝。

该 bug 潜伏了 5 天未被发现，直到 FTL 随 `main.c` 首次上板运行，且被同期落地的断言捕获。

## 背景：FC 的数据模型

FC 命令（`fc_command_t`）通过 **desc 链**（descriptor chain）关联 CW buffer：

```
fc_command_alloc()                      → 命令分配，desc_id = FC_DESCRIPTOR_ID_INVALID (0xFFFF)
fc_command_configure_descriptors(cmd)   → 每 CW 分配一个 descriptor + buffer，串联成链，
                                          把链首记入 cmd->desc_id
fc_write_cmd_data(cmd, data, len)       → 按链把数据写进各 CW buffer
fc_submit(cmd)                          → 入队
```

**数据只能写进 desc 链上的 buffer**。不建链 = 命令没有 buffer = 无数据可搬。

`fc_command_validate()`（`fc_command.c:43-49`，自早期提交 `a4013dd` 起就存在）会挡住这种命令：

```c
if ((operation != OP_SET_FEATURES) && (operation != OP_GET_FEATURES) &&
    nand_operation_has_data(operation) &&
    (command->desc_id == FC_DESCRIPTOR_ID_INVALID))
{
    return FC_STATUS_INVALID_ARGUMENT;
}
```

## 根因

`ftl_cmd_setup_ppa()`（`src/ftl/ftl_fc_interface.c`）做两件事：

```c
cmd->config.ch/ce/lun = ...;   // ① 填拓扑
cmd->paa = ...;                // ② 填页地址
return (fc_command_configure_descriptors(cmd) == FC_STATUS_OK) ? FTL_OK : FTL_ERROR;  // ③ 建链
```

v0.28 新增的 `addr_to_cmd()` 只做了 ① ②：

```c
/* 修复前 */
static inline ftl_status_t addr_to_cmd(fc_command_t *cmd, const nand_addr_t *addr)
{
    if ((cmd == NULL) || (addr == NULL)) { return FTL_ERROR; }
    cmd->config.ch/ce/lun = ...;
    cmd->paa = ...;
    return FTL_OK;                 // ← 漏了 fc_command_configure_descriptors()
}
```

头文件注释"等价于 `ftl_cmd_setup_ppa(cmd, pba_from_addr(addr))`"因此与实现不符——重构在语义上不等价。

## 影响范围

`addr_to_cmd()` 的 9 个调用方模块（共 21 个调用点），全部同步读写页路径：

| 文件 | 用途 |
|---|---|
| `bbt.c` | 坏块表读写 |
| `metadata.c` | 元数据目录读写 |
| `journal.c` | journal 落盘 / 回放 |
| `checkpoint.c` | checkpoint 读写 |
| `l2p_cache.c` | L2P region 读写 |
| `gc.c` | GC 搬移读 / 写 / 擦除 |
| `wear_leveling.c` | 磨损均衡搬移 |
| `read_recovery.c` | 慢死读恢复 |
| `power_failure_recovery.c` | 掉电恢复重建 |

每个操作的实际后果：

- **写**：`ftl_cmd_write_data()` 搬不进数据（返回 `INVALID_ARGUMENT`），`fc_submit()` 被 validate 以 `INVALID_ARGUMENT` 拒 → helper 返回 `FTL_ERROR`
- **读**：`fc_submit()` 就失败，压根到不了 `ftl_cmd_read_data()`

## 为什么潜伏 5 天

1. **FTL 首次上板** —— `main.c` 的 FTL 启动流程直到本次才接通，此前 FTL 代码从未在板上执行。
2. **返回值被丢弃** —— 各调用点写作 `(void)ftl_cmd_write_data(cmd, data, length);`，`FTL_ERROR` 被 `(void)` 吞掉。
3. **无单测兜底** —— FTL 单测（`docs/design/ftl_ut_munit.md`）尚未实施。

## 为什么这次暴露

同期落地的「全部入口守卫」断言扫除把 `fc_write_cmd_data()` 里 `desc_id == FC_DESCRIPTOR_ID_INVALID` 的守卫从**返回错误码**改成了 **`ASSERT`**（`fc_interface.c:258`）。改之前该路径静默返回错误、被吞；改之后**停机**——于是首次上板 boot 阶段第一个写 NAND 的调用（BBT / 元数据落盘）就撞停：

```
[ERR] assertion failed: command->desc_id != FC_DESCRIPTOR_ID_INVALID
      (/home/miao_yang/projects/soft_ssd/src/fc/fc_interface.c:258)
[ERR] [RES] cmd=1/64 desc=0/256 buf=0/128 sq=0 hsq=0 cq=0
```

`desc=0/256` 印证：整个描述符池一个都没分配过。断言在此是**正确**的（`desc_id != INVALID` 是 `fc_write_cmd_data` 的真实前置条件），它把一条静默失效的数据通路拽了出来。

## 修复

`addr_to_cmd()` 补上漏掉的一步，恢复与 `ftl_cmd_setup_ppa()` 的语义等价：

```c
return (fc_command_configure_descriptors(cmd) == FC_STATUS_OK) ? FTL_OK : FTL_ERROR;
```

- 头文件 include 由 `fc/fc_command.h` 换成 `fc/fc_interface.h`（取 `fc_command_configure_descriptors` 声明）。
- 无数据操作（ERASE / RESET 等）由 `fc_command_configure_descriptors_internal()` 内部短路为 `desc_id = INVALID` 并返回 OK，不受影响。
- 配置失败（如描述符池满）时 `configure_descriptors_internal` 内部已回滚，调用方的 `!= FTL_OK` 分支照常 `fc_command_release(cmd)`。

## 验证

| 项 | 结果 |
|---|---|
| `make debug` | 零告警零错误 |
| `make tests` | 零告警零错误（203832 B） |
| `make release` | 零告警零错误 |
| `make format-check` | 通过 |
| **上板 `make tests`** | boot 干净通过（无断言停机），42/42 用例全绿 |

上板通过确认 FTL 的同步读写页路径首次真正在搬数据。

## 遗留（本次未动）

- **同步 helper 丢弃返回值**：21 处 `(void)ftl_cmd_write_data(...)` / `(void)ftl_cmd_read_data(...)` 仍吞掉错误码。根因已修，当前不再触发；但若将来再次失败仍会被静默忽略。是否上抛属单独议题。
- **同类模式复查**：已核对 `ftl/` 全部 23 处 `fc_command_alloc()` —— 21 处经 `addr_to_cmd`（本次修复覆盖），2 处经 `ftl_cmd_setup_ppa`（本就配置 desc 链），无遗漏。
