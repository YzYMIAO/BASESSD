# FC 模块参数验证重构记录

**日期**: 2026-09-20  
**分支**: main  

---

## 背景

FC 模块中参数有效性检查散布在公接口函数和内部函数中，导致多处重复验证（如 `controller == NULL`、`fc_command_to_id()`、状态检查等）。这些检查在内部函数中是冗余的——因为只有一个 `g_fc_controller` 实例，且公接口可以在调用内部函数之前完成所有验证。

## 目标

将所有参数验证上移到公接口（`fc.h` 中声明的函数），内部函数假定输入已合法。

## 改动汇总

### 1. 公接口增加参数验证

| 函数 | 新增验证项 |
|---|---|
| `fc_command_configure_descriptors()` | `command==NULL`、`fc_command_to_id()`、`state!=ALLOCATED`、`desc_id!=INVALID` |
| `fc_buffer_release()` | `fc_buffer_id_is_valid()`（检查 range + state 非 0） |
| `fc_buffer_id_to_real_buffer()` | `fc_buffer_id_is_valid()` |
| `fc_real_buffer_to_buffer_id()` | `buffer==NULL` |
| `fc_submit()` | `command==NULL`、`fc_command_to_id()`、`state!=ALLOCATED`、`fc_command_validate()`、`fc_command_descriptors_owned()` |
| `fc_poll_completion()` | `completion==NULL` |
| `fc_command_release()` | `command==NULL`、`fc_command_to_id()`、state 检查（ALLOCATED/COMPLETED）、`fc_cq_contains_command()` |

### 2. 内部函数移除冗余验证

| 函数 | 移除的检查 |
|---|---|
| `fc_init_internal()` | `controller==NULL` |
| `fc_command_alloc_internal()` | `controller==NULL` |
| `fc_descriptor_alloc_internal()` | `fc_command_to_id()`、state 检查 → 改用指针算术 `command - controller->command_pool` |
| `fc_buffer_alloc_internal()` | `controller==NULL` |
| `fc_buffer_release_internal()` | `fc_buffer_id_is_valid()` |
| `fc_buffer_release_internal_api()` | `controller==NULL` |
| `fc_buffer_id_to_real_buffer_internal()` | `fc_buffer_id_is_valid()` |
| `fc_real_buffer_to_buffer_id_internal()` | `controller==NULL`、`buffer==NULL` |
| `fc_buffer_id_is_valid()` | `controller!=NULL` → 仅保留 range + state 检查 |
| `fc_command_descriptor_count()` | `controller!=NULL`、`command!=NULL` → 简化参数为 `(command)` |
| `fc_command_configure_descriptor_internal()` | `fc_command_to_id()`、`state!=ALLOCATED`、`index>cw` |
| `fc_command_configure_descriptors_internal()` | `fc_command_to_id()`、`state!=ALLOCATED`、`desc_id!=INVALID` → 改用指针算术 |
| `fc_submit_to_queue()` | `fc_command_to_id()`、`state!=ALLOCATED`、`fc_command_validate()`、`fc_command_descriptors_owned()` → 改用指针算术 |
| `fc_submit_internal()` | `controller==NULL`、`command!=NULL`（三元运算符） |
| `fc_poll_internal()` | `controller==NULL` |
| `fc_complete_internal()` | `controller==NULL`、`completion==NULL`、`command_id>=depth`、`fc_command_to_id()`、`state!=ACTIVE` |
| `fc_poll_completion_internal()` | `controller==NULL`、`completion==NULL` |
| `fc_command_release_internal_api()` | `fc_command_to_id()`、state 检查、`fc_cq_contains_command()` → 改用指针算术 |

### 3. Bug 修复

| 位置 | 修复内容 |
|---|---|
| `fc_command_descriptors_owned()` 循环条件 | `(controller, command)` → `fc_command_descriptor_count(command)` |

原代码使用逗号表达式 `(controller, command)`，结果是指针 `command` 与整数 `descriptor_count` 比较，循环条件始终为假。修复后使用正确的 descriptor 计数函数。

### 4. 优化：指针算术替代线性查找

以下内部函数改用 `command - controller->command_pool` 直接计算 `command_id`，替代 `fc_command_to_id()` 的全量池遍历：

- `fc_descriptor_alloc_internal()`
- `fc_command_configure_descriptors_internal()`
- `fc_submit_to_queue()`
- `fc_command_release_internal_api()`

（公接口层已通过 `fc_command_to_id()` 确认 command 属于池，内部无需再次验证）

## 设计原则

1. **公接口是信任边界**：外部调用者（FTL 等上层模块）传入的参数在此验证
2. **内部函数无冗余检查**：假定 controller/command/completion 均为有效指针
3. **NAND 层数据仍需验证**：`fc_complete_from_nand()` 保留对 `nand_completion` 的范围检查（数据源来自 NAND 层）