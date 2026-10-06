# 内部搬移编程 op 去重：删 op 20，op 10 改为纯 copy-back

**日期**: 2026-10-06
**相关设计**: `docs/design/fc_nand_design.md` §17.6
**前置**: `nand_dispatch_plane_index.md`（同日）

---

## 概述

内部搬移编程原本有两个功能重复的 OP 码：

| OP 码 | 值 | DATA_IN | 说明 |
|---|---|---|---|
| `OP_PROGRAM_FOR_INTERNAL_DATA_MOVE` | 10 | 1 字节 | `85h + addr + 1B → 10h`，号称"替换写入" |
| `OP_PROGRAM_FOR_INTERNAL_MOVE` | 20 | 无 | `85h + addr → 10h`，纯 copy-back |

两者除那一行 `NAND_UOP_DATA_IN` 外**逐字段相同**，且**都无构造路径**（生产代码从不发起；只有 `tests/unit/test_fc_level3.c` 的 `test_internal_data_move` 用到）。

本次：

- **删除 op 20**。
- **保留 op 10**，去掉 DATA_IN，成为纯 copy-back。

保留 op 10 的理由是**命名**：`85h-10h` 这条命令在 ONFI 里的标准名就是 *Program for Internal Data Move*，与 `OP_READ_FOR_INTERNAL_DATA_MOVE`（`35h`，*Read for Internal Data Move*）天然成对；op 20 的名字反而是本项目自造的。

## 为什么不需要"替换 1 字节"

1 字节替换**不是协议要求**（`85h` = RANDOM DATA INPUT，Micron 文档原文可 "modify one or more bytes"），是本项目的设计选择。而 GC 需要的"读出 → ECC 纠正 → 写回"已经用**普通读 + 普通写**实现（`src/ftl/gc.c` 的 `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` + `OP_NORMAL_PROGRAM_PAGE`），不需要在内部搬移 op 上挂 DATA_IN。故这个模式没有存在价值。

## 改动

| 文件 | 改动 |
|---|---|
| `include/nand/onfi_1_0.h` | 删 `OP_PROGRAM_FOR_INTERNAL_MOVE` 注释块 + `#define`，按 op 9 / op 11 先例补"操作码 20 已删除"；重写 op 10 注释（序列去掉 `Din(1B)`，说明改为纯 copy-back）；段注释去掉"单字节替换" |
| `src/nand/onfi_1_0.c` | 删 `g_nand_program_move_uops[]`（唯一消费者是 op 10；留着触发 `-Wunused-const-variable`）；op 10 条目 `data_input_cycles 1→0`、`data_length 1→0`、`.uops → g_nand_internal_program_uops`；删 op 20 条目 |
| `src/nand/nand_dispatch_tables.c` | 删 op 20 的 `completion_single` / `req_single` 两项 |
| `tests/unit/test_fc_level3.c` | `test_internal_data_move` 的 op 20 → op 10（纯改名，语义不变）+ 注释同步 |
| `docs/design/fc_nand_design.md` | §17.6 三行表删 op 20 行、op 10 行去 DATA_IN、"关键区别"对比表整块删除；「CHECK_FAIL — 使用规则」的例外段重写 |
| `docs/design/fc_nand_ut.md` | OP 码表删 op 20 行 |

`OP_COUNT` **保持 41**（`0..40`）：op 9 / op 11 删除时已留下空位，20 成为第三个空位，不重编号。

## 关键约束

`data_length = 1 → 0` **不能单独改**。op 10 原 uop 序列含 `{ NAND_UOP_DATA_IN, 0U }`，而 DATA_IN handler 首行是 `ASSERT(request->data_length != 0U)`（`src/nand/nand_fmc.c:254`）——只改长度会 debug/tests 构建踩断言、release 构建静默退化成纯 copy-back。必须同时去掉 DATA_IN uop 并把 `data_input_cycles` 归 0。

`has_data` 保持 1：它决定 `fc_build_dispatch_request` 是否分配 desc chain / 填 `cw_count`，`req_single` 又只在 `cw_count != 0` 时填行地址——置 0 会让 copy-back 编程到 page 0。

## 验证

1. `make debug` 编译零警告，二进制 152248 → 151920 字节（-328：删一个 op 表行 + 一个 20 项 uop 数组）。
2. `make release` 编译零警告，87132 → 86944 字节（-188）。
3. `make tests` 编译零警告（含改名后的 `test_fc_level3.c`）。
4. `make format` 幂等，`git status --porcelain` 只列本次 6 个文件。
5. 上板验证按平台规则由用户执行。
