# FC 模块结构变更与 Dispatch 边界设计记录

**日期**: 2026-09-21
**涉及的提交**: （尚未合并，当前工作区改动）

---

## 概述

本次变更是 FC 模块自参数验证重构后的第二次结构性改动，涵盖三个方面：

1. **Phase A — buffer_id 便捷接口**：新增 `fc_write_cmd_data()` 和 `fc_get_cmd_data_ptr()` 两个公接口
2. **Phase B — 核心结构重组**：`fc_command_status_t`、`fc_descriptor_t`、dispatch 边界结构的字段级重新设计
3. **控制/状态位贯穿分析**：明确 fc_cmd_conf_t 中每个字段是否跨越 FC↔NAND Dispatch 边界

---

## Phase A: buffer_id 便捷接口

### 动机

FTL 在使用 buffer_id 方案时，需要手动遍历 descriptor 链、逐 CW 通过 `buffer_id → real_buffer` 转换后 memcpy 数据。这增加了 FTL 的样板代码量。虽然保持了 FC 对 buffer 生命周期的完全所有权，但易用性差。

### 改动

| 文件 | 改动 |
|------|------|
| `include/fc/fc.h` | 新增 `fc_write_cmd_data()` 声明（第 38 行）、`fc_get_cmd_data_ptr()` 声明（第 41 行） |
| `src/fc/fc.c` | 实现 `fc_write_cmd_data()`（第 726~786 行）：遍历 desc 链，逐 CW memcpy，未填满部分用 0xFF dummy |
| `src/fc/fc.c` | 实现 `fc_get_cmd_data_ptr()`（第 788~824 行）：遍历 desc 链到 cw_index，返回 buffer 指针或 NULL |

### 设计要点

- `fc_write_cmd_data()` 的 0xFF dummy 填充策略：

| 情况 | 行为 |
|------|------|
| `length >= CW_SIZE` | 拷贝 CW_SIZE 字节，length 递减 |
| `0 < length < CW_SIZE` | 拷贝 length 字节，剩余部分 memset(0xFF) |
| `length == 0`（或后续 CW 数据耗尽）| 整 CW 全部 memset(0xFF) |

- 这是 DRAM-less SSD 标准做法：NAND PROGRAM 必须写完整 page，未提供数据的 CW 用 0xFF（擦除态）填充。
- 两个函数都在公接口层进行完整参数验证（command/data NULL 检查、state 检查、desc_id 有效、buffer_id 有效）。

---

## Phase B: 核心结构重组

### B1. fc_command_status_t 重组

#### 旧结构（8 字节）

```c
struct fc_command_status_t {       // 64 bits
    DWORD command_id : 16;
    DWORD desc_id    : 16;
    DWORD status0    : 8;
    DWORD status1    : 8;
    DWORD status2    : 8;
    DWORD status3    : 8;
};
```

#### 新结构（12 字节）

```c
struct fc_command_status_t {       // 96 bits
    DWORD command_id : 16;
    DWORD desc_id    : 16;
    DWORD module     : 4;   // ← NEW
    DWORD ecc_bit0   : 8;   // ← NEW
    DWORD ecc_bit1   : 8;   // ← NEW
    DWORD ecc_bit2   : 8;   // ← NEW
    DWORD ecc_bit3   : 8;   // ← NEW
    DWORD reserved   : 4;
    DWORD status0    : 8;
    DWORD status1    : 8;
    DWORD status2    : 8;
    DWORD status3    : 8;
};
```

#### 新增字段说明

| 字段 | 来源 | 用途 |
|------|------|------|
| `module` | `command->module` | FTL 可直接路由完成通知，无需 command_id 查询再决定回调模块 |
| `ecc_bit0~3` | `nand_completion.ecc_bit_count[4]` | 每 CW ECC 纠正比特数，用于 BER 监测、磨损均衡触发 |

#### 影响文件

| 文件 | 改动 |
|------|------|
| `include/fc/fc_command.h` | 结构体定义重写（第 100~114 行） |
| `src/fc/fc_command.c` | `fc_command_status_init()` 新增 `module` 赋值和 `ecc_bit0~3` 初始化（第 25~46 行） |
| `src/fc/fc.c` | `fc_complete_from_nand()` 填充 `module` 和 `ecc_bit0~3`（第 477~487 行） |

#### 内存开销

64 条 CQ × 12 字节 = 768 字节（原 512 字节，+256 字节）。

---

### B2. fc_descriptor_t 结构调整

#### 旧结构（12 字节）

```c
struct fc_descriptor_t {           // 96 bits
    DWORD paa;
    fc_cmd_conf_t config;
    DWORD buffer_id : 16;
    DWORD next_id   : 16;
    DWORD status    : 8;
    DWORD reserved  : 8;
};
```

#### 新结构（16 字节）

```c
struct fc_descriptor_t {           // 128 bits
    DWORD paa;
    fc_cmd_conf_t config;
    DWORD buffer_id : 16;
    DWORD next_id   : 16;
    DWORD status    : 8;     // CW 状态，完成时由 FC 回写
    DWORD ecc_bits  : 8;     // ← NEW：ECC 纠正比特数，完成时由 FC 回写
    DWORD reserved  : 16;
};
```

#### 新增字段说明

| 字段 | 来源 | 用途 |
|------|------|------|
| `ecc_bits` | `nand_completion.ecc_bit_count[cw]` | 持久化 ECC 计数，FTL 遍历 desc 链即可获取，无需 CQ 查询 |

#### 回写机制

`fc_complete_from_nand()` 在收到 NAND 完成后，遍历 desc 链逐 CW 回写：

```c
descriptor->status   = nand_completion->cw_status[cw];
descriptor->ecc_bits = nand_completion->ecc_bit_count[cw];
```

#### 双重交付的价值

| 场景 | CQ 快照 | Descriptor 回写 |
|------|---------|----------------|
| 正常完成，FTL 检查状态 | ✅ 足够 | 可选 |
| Read Retry：定位失败 CW | ❌ 需 desc_id | ✅ desc_id + status |
| RAID Recovery：逐 CW 检查 | ❌ 命令级 | ✅ 每 desc 独立 |
| 长链路命令，CQ 已出队 | ❌ 丢失 | ✅ 持久保留 |

#### 内存开销

256 个 descriptor × 16 字节 = 4 KB（原 3 KB，+1 KB）。

---

### B3. Dispatch 边界结构变更

#### nand_dispatch_request_t 新增 page_type

```c
/* dispatch.h 中新增字段（第 32 行） */
uint8_t  page_type   : 2;   /* LP/MP/UP/TP — LDPC 引擎解码阈值选择 */
```

#### nand_dispatch_completion_t 新增 ecc_bit_count

```c
/* dispatch.h 中新增字段（第 44 行） */
uint8_t  ecc_bit_count[4];   /* 每 CW ECC 纠正比特数 */
```

#### nand_dispatch_task_t 新增 page_type

```c
/* dispatch.c 中新增字段（第 24 行） */
uint8_t  page_type   : 2;   /* Copy from dispatch request for LDPC engine. */
```

#### 影响文件

| 文件 | 改动 |
|------|------|
| `include/nand/nand_dispatch.h` | request 增加 `page_type`、completion 增加 `ecc_bit_count[4]` |
| `src/nand/nand_dispatch.c` | task struct 增加 `page_type`，`nand_dispatch_submit()` 保存 `request->page_type`（第 281 行） |
| `src/fc/fc.c` | `fc_poll_internal()` 传递 `request.page_type = command->config.page_type`（第 556 行） |

---

## Phase C: SET/GET_FEATURES 内联数据路径

### 动机

SET_FEATURES（0xEF）和 GET_FEATURES（0xEE）每次操作只需传输 1 字节寄存器地址 + 4 字节数据，但原始实现却分配了完整的 descriptor（16 字节）+ buffer（528 字节）来承载这 5 字节，浪费资源池。GC 和 Read Retry 等流程频繁读/写 ONFI 寄存器（如 Feature Register 0x01 启用 ECC、0x90 设置时序模式），累积的资源消耗不可忽视。

### 方案

在 `fc_command_t` 中嵌入 `feature_addr:8` 位域和 `parameter[0..3]` 纯字节，使 SET/GET_FEATURES 命令完全跳过 descriptor+buffer 分配。

### 文件变更

| 文件 | 变更类型 | 变更说明 |
|------|---------|---------|
| `include/fc/fc_command.h` | 结构体修改 | `reserved1:11` → `feature_addr:8, reserved1:3`；`parameter[0..3]` 由 `DWORD : 8` 改为 `uint8_t` |
| `include/nand/nand_dispatch.h` | 结构体新增字段 | `nand_dispatch_request_t` 增加 `uint8_t feature_addr` |
| `src/fc/fc_command.c` | 条件判断 | `fc_command_validate()` 跳过 SET/GET_FEATURES 的 desc_id 检查 |
| `src/fc/fc.c` | 跳过分发 | `fc_command_configure_descriptors_internal()` 跳过 desc 分配；`fc_poll_internal()` 传递 feature_addr 和内联 buffer 指针 |
| `src/nand/nand_dispatch.c` | 新增处理 | task 增加 `feature_addr`；`submit()` 拷贝；`poll()` 新增 SET/GET_FEATURES 分支设置 column=feature_addr |
| `tests/integration/test_fc_nand.c` | 测试重写 | `fc_nand_test_set_get_features()` 改用内联参数，移除 descriptor/buffer 操作 |

### 设计要点

#### parameter 类型变更：`DWORD : 8` → `uint8_t`

**原因**：内联数据路径需要 `&command->parameter0` 作为 buffer 指针传递给 NAND，使 GET_FEATURES 回写数据直接落入 command 结构体。C 标准禁止取位域地址，故改为纯 `uint8_t`。

**影响范围**：
- `fc_command_t` 大小增加 ~4 字节（从 ~32B 增至 ~36B）
- Command Pool（64 条）：~2304 字节（原 ~2048 字节，+256 字节）
- 所有现有代码读取 `parameter0..3` 的语法不变；`(uint8_t)` 转型兼容

#### buffer 指向策略

内联路径中 `cw_buffers[0]` 直接指向 `&command->parameter0`（FC Command Pool 中的稳定内存），而非重定向到 task 内部的 `parameter[]`：

```
SET_FEATURES:
  cw_buffers[0] = &command->parameter0
    → NAND 从此地址读取 4 字节发出 ✓

GET_FEATURES:
  cw_buffers[0] = &command->parameter0
    → NAND 回写 4 字节写入 command->parameter[0..3] ✓
    → 测试/FTL 直接读取 command->parameter0..3 ✓
```

#### 完成路径

`fc_complete_from_nand()` 检查 `command->desc_id != FC_DESCRIPTOR_ID_INVALID` 才执行 descriptor 回写。SET/GET_FEATURES 的 desc_id 固定为 INVALID，自动跳过，无回归。

### 内存开销

| 资源 | 变更前 | 变更后 | 差值 |
|------|--------|--------|------|
| Command Pool | ~2048 字节 | ~2304 字节 | +256 字节 |
| Descriptor Pool | 4 KB | 4 KB | 0 |
| Buffer Pool | 66 KB | 66 KB | 0 |

+256 字节换来每次 SET/GET_FEATURES 省去 544 字节（1 desc + 1 buffer）的动态分配。在 GC/Read Retry 高频寄存器操作的场景下净节省显著。

### 功能影响

| 能力 | 变更前 | 变更后 |
|------|--------|--------|
| SET/GET_FEATURES 资源消耗 | 1 desc + 1 buffer（544 字节） | 0 |
| GET_FEATURES 读回数据位置 | desc buffer（需额外 API 读取） | command->parameter[0..3]（直接访问） |
| desc_id 要求 | 必须有合法 desc_id | SET/GET_FEATURES 可为 INVALID |
| parameter[0..3] 取地址 | ❌（位域） | ✅（uint8_t） |

---

## 控制/状态位贯穿决策记录

以下记录 `fc_cmd_conf_t` 中每个控制位是否跨越 FC↔NAND Dispatch 边界的决策依据：

| 字段 | 过 Dispatch？ | 决策理由 |
|------|-------------|---------|
| `operation` | ✅ | 硬件需要知道执行什么 ONFI 操作 |
| `ch/ce/lun` | ✅ | 硬件路由必须的寻址信息 |
| `cw` | ✅ → cw_count | 硬件需要知道 CW 数量规划传输 |
| `crc` | ❌ | CRC 在 FC firmware 完成，硬件不感知 |
| `ldpc` | ❌ | FC 内部配置 LDPC 引擎；硬件解码器只需 `page_type` 选阈值 |
| `raid` | ❌ | RAID 是 FC 内部功能；dispatch 只看到标准 PROGRAM 命令 |
| `random` | ❌ | 加扰/解扰在 FC 数据路径内完成，对硬件透明 |
| `chain` | ❌ | 命令链是 FC 调度器概念，NAND 硬件单条执行 |
| `page_type` | ✅ → 新增 | TLC/QLC LDPC 引擎需要 LP/MP/UP/TP 选择解码阈值 |

**总开销**：所有结构变更合计约 +1.3 KB SRAM（占 1 MB SRAM 的 0.13%）。

---

## 功能影响

| 能力 | 变更前 | 变更后 |
|------|--------|--------|
| FTL 数据写入便利性 | 需手动遍历 desc 链 | `fc_write_cmd_data()` 一行完成 |
| FTL 读取数据 | 需手动遍历 desc 链 | `fc_get_cmd_data_ptr()` 一行获取 |
| CQ 完成路由 | 需 command_id 查表 | `module` 字段直接路由 |
| BER 监测 | 无 | `ecc_bit0~3` + desc 回写 |
| Read Retry 定位 | 靠 CQ 快照（可能消失）| desc 持久化状态 |
| RAID 恢复 | 无 per-CW 状态 | desc 链遍历获取 |
| LDPC 解码阈值选择 | 默认阈值 | `page_type` 传硬件引擎 |

---

## Phase D: nand_dispatch 函数指针表重构

### 动机

`nand_dispatch_poll()` 中根据 `task->operation` 判断 NAND 操作类型的 if-else 链包含 6-7 次条件比较，不仅增加执行时间，还带来分支预测惩罚。同期 `nand_dispatch_execute_command()`（已 `unused`）也包含同样的模式。

### 改动

整体变更涉及 **三张函数指针表** + ONFI 映射表属性增强：

#### 1. Request 构建函数指针表

替换 `nand_dispatch_poll()` 出队处的 ~90 行 if-else 链（L253-346，旧文件行号）。

**7 个 handler 覆盖全部 29 个 OP**：

| Handler | 覆盖操作 | 行为 |
|---------|---------|------|
| `req_program_page` | NORMAL_PROGRAM_PAGE, OTP_DATA_PROGRAM, PROGRAM_PAGE_CACHE | 拼 CW buffer → page_buffer，全页写入 |
| `req_read_page` | NORMAL_READ_PAGE, OTP_DATA_READ | 仅 CW0 读取（后续由 poll chaining 处理） |
| `req_cache_read` | READ_PAGE_CACHE_SEQ/RANDOM/LAST | 一次性全页读到 page_buffer |
| `req_erase` | ERASE_BLOCK | 纯 row 地址，无 buffer |
| `req_feature` | SET/GET_FEATURES | column = feature_addr, length = 4 |
| `req_single` | READ_ID, READ_PARAM_PAGE, READ_STATUS 等 | 通用：length 从 `g_nand_operation_map[].data_length` 读取 |
| `req_no_data` | RESET, READ_MODE, BLOCK_LOCK 等 | 无数据，纯地址 |

**分派点从 ~90 行 if-else 简化为 1 行**：

```c
// 重构前：
if ((task->operation == OP_NORMAL_PROGRAM_PAGE) || ...) { /* ~10 行 */ }
else if ((task->operation == OP_NORMAL_READ_PAGE) || ...) { /* ~6 行 */ }
else if (...) ...  // 共 7 个分支 ~90 行

// 重构后：
result = g_nand_dispatch_req_table[task->operation](task);
```

#### 2. Completion 策略函数指针表

替换 `nand_dispatch_task_complete()` 的 if-else 链（3 分支 → 3 handler）：

| Handler | 策略 | 适用 OP |
|---------|------|--------|
| `completion_all_cws` | 所有 CW 共享完成状态 | PROGRAM, ERASE |
| `completion_per_cw` | 只设当前 cw_index 状态 | READ, CACHE_READ |
| `completion_single` | 只设 cw_status[0] | 其余 |

#### 3. ONFI 映射表属性增强

`nand_operation_map_t` 新增两个字段（它们是 ONFI 协议属性，不是 dispatch 策略）：

```c
typedef struct {
    ...
    uint8_t  has_data;     /* 替代 nand_operation_has_data() */
    uint16_t data_length;  /* 替代 nand_operation_data_length() */
    ...
} nand_operation_map_t;
```

- 对应函数声明改为宏直接查表
- 映射表改用 designated initializer（消除字段顺序依赖）
- `nand_operation_has_data()` / `nand_operation_data_length()` 函数体从 `onfi_1_0.c` 中删除

#### 4. 废弃代码清理

| 函数 | 原因 |
|------|------|
| `nand_dispatch_execute_request()` | 已被 `nand_fc_start/step` 非阻塞接口替代 |
| `nand_dispatch_execute_page()` | 逻辑分散到 handler |
| `nand_dispatch_execute_command()` | 标记 `unused`，功能完全由函数指针表覆盖 |

### 文件变更

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `src/nand/nand_dispatch_internal.h` | **新建** | 内部类型：`nand_dispatch_task_t` + 函数指针表类型声明 |
| `src/nand/nand_dispatch_tables.c` | **新建** | 7 个 req handler + 3 个 completion handler + 表定义 |
| `src/nand/nand_dispatch.c` | 重构 | 移出 ~100 行废弃代码；if-else 链 → 查表；`task` 新增 `ch` 字段 |
| `src/nand/onfi_1_0.c` | 重构 | 映射表 +2 字段，改用 designated initializer；删除 2 个函数 |
| `include/nand/onfi_1_0.h` | 重构 | `map_t` +2 字段；函数声明 → 宏 |
| `src/fc/fc.c` | 无实质变更 | 宏替换（`nand_operation_has_data` 调用不变） |
| `src/fc/fc_command.c` | 无实质变更 | 宏替换 |

### 性能

| 指标 | 重构前（if-else 链） | 重构后（函数指针表） |
|------|-------------------|-------------------|
| 分支数 | 6-7 次比较 | 1 次间接跳转（O(1)） |
| 执行时间 | 不确定，取决于分支预测 | 固定 2-3 周期 |
| 分支预测惩罚 | 高 | 无（jump table） |
| 扩展新操作 | 改 2-3 处 if-else | 加 handler + 表项 |

### 架构一致性

本次重构使 dispatch 层与 engine 层的架构风格一致：

```
dispatch 层：g_nand_dispatch_req_table[op](task)          ← 函数指针表选策略
engine  层：backend->handlers[uop->type](context, uop)    ← 函数指针表执行微操作
```

两层均无任何条件判断链，均为纯表驱动。

---

## 参考文档

- 接口使用指南：👉 [fc_api_usage.md](../design/fc_api_usage.md)
- 设计方案（含 §15 Phase D）：👉 [fc_nand_design.md](../design/fc_nand_design.md#15-nand_dispatch-函数指针表重构-phase-d)
---

## Phase E: Multi-Plane 操作支持 + 行地址数组重构 + OP 码拆分

### 动机

现有架构中 `cw_paa/buffers` 为一维数组 `[CW_NUM]`(4)，不支持 Multi-Plane 操作（双 Plane 需要 8 CW 槽位）。同时 `nand_command_request_t.row` 为单值，Multi-Plane 需要发送两组行地址。

厂商设计中，读操作是否带 DATA OUT 是两个独立场景——"读到缓冲就停下"和"读到缓冲立即输出数据"。为此将读 OP 码拆分为 WITH/WITHOUT_DATA_OUT 双版本。

### 1. 数据维度扩展

`cw_paa/buffers[CW_NUM]` → `cw_paa/buffers[PLANE_NUM][CW_NUM]`

| 结构 | 变更 |
|------|------|
| `nand_dispatch_request_t.cws` | `[CW_NUM]` → `[PLANE_NUM][CW_NUM]` |
| `nand_dispatch_task_t.cw_paa/buffers` | `[CW_NUM]` → `[PLANE_NUM][CW_NUM]` |
| `nand_dispatch_task_t` | 新增 `plane_count` 字段 |
| `nand_dispatch_completion_t.cw_status` | `[4]` → `[CW_TOTAL_MAX]`（=8） |
| `nand_dispatch_completion_t.ecc_bit_count` | `[4]` → `[CW_TOTAL_MAX]` |

### 2. 行地址数组重构

`nand_command_request_t.row` / `nand_fc_request_t.row`：`uint32_t` → `uint32_t[NAND_PLANE_MAX]`

ROW_0/1/2 handler 通过 `uop->operand` 选择 row 索引，**零新增 uop 类型**：

```c
// FMC handler 改动（只加一行）
*config->address = (uint8_t)(request->row[uop->operand]);

// uop 序列中 operand=0 选 row[0] (plane 0), operand=1 选 row[1] (plane 1)
{ NAND_UOP_ROW_0,  0U },  /* row[0] byte 0 */
{ NAND_UOP_ROW_0,  1U },  /* row[1] byte 0 */
```

### 3. OP 码拆分：WITH_DATA_OUT / WITHOUT_DATA_OUT

| 原 OP 码 | 拆分为 | 新增 OP 码 |
|---------|--------|-----------|
| `OP_NORMAL_READ_PAGE (12)` | `OP_NORMAL_READ_PAGE_WITH_DATA_OUT (12)` | `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT (34)` |
| — | `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT (29)` | `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT (30)` |

uop 序列差异仅在于 30h→tR→DATA_OUT 一行的有无。

### 4. 新增 Multi-Plane OP 码

| OP 码 | 值 | 说明 |
|-------|----|------|
| `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT` | 29 | 双 Plane 读，仅加载到页缓冲（无 Dout） |
| `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | 30 | 双 Plane 读 + 立即双页 Dout |
| `OP_PROGRAM_PAGE_MULTI_PLANE` | 31 | 双 Plane 编程，用 11h 切换 plane |
| `OP_ERASE_BLOCK_MULTI_PLANE` | 32 | 双 Plane 擦除 |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | 34 | 单 Plane 读，仅加载到页缓冲（无 Dout） |

`OP_COUNT` 从 32 → 35。

### 5. 新增配置宏

```c
#define NAND_PLANE_MAX    (4U)           /* 行地址数组最大深度，未来扩展余量 */
#define CW_TOTAL_MAX      (CW_NUM * PLANE_NUM)  /* 每 plane CW 数 × plane 数 = 8 */
#define PAGE_BUFFER_SIZE  (PAGE_SIZE * PLANE_NUM)  /* 通道页缓冲：2112×2 = 4224 */
```

### 6. 文件变更

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `include/nand/nand_config.h` | 新增宏 | `NAND_PLANE_MAX`、`CW_TOTAL_MAX`、`PAGE_BUFFER_SIZE` |
| `include/nand/nand_engine.h` | 结构体修改 | `nand_command_request_t.row` → 数组；`#include "nand_config.h"` |
| `include/nand/nand_fc_interface.h` | 结构体修改 | `nand_fc_request_t.row` → 数组；`#include "nand/nand_config.h"` |
| `include/nand/onfi_1_0.h` | 新增 OP + 注释 | 新增 `NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH (0x11)`；新增 5 个 OP 码；`OP_COUNT=35`；所有 Multi-Plane 时序注释 |
| `src/nand/onfi_1_0.c` | 新增 uop + 表项 | 新增 5 组 uop 序列 + 映射表 5 项 |
| `src/nand/nand_fmc.c` | 修改 handler | ROW_0/1/2 handler 用 `uop->operand` 索引 `row[]` |
| `src/nand/nand_mt29f4g08abadawp.c` | 修改引用 | `OP_NORMAL_READ_PAGE` → `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` |
| `include/nand/nand_dispatch.h` | 结构体修改 | `cws` 增加 plane 维度、`cw_status/ecc_bit_count` 扩展到 `CW_TOTAL_MAX`、新增 `plane_count` |
| `src/nand/nand_dispatch_internal.h` | 结构体修改 | `cw_paa/buffers` 增加 plane 维度、新增 `plane_count` |
| `src/nand/nand_dispatch.c` | 适配 | `page_buffer` 尺寸扩展、`nand_dispatch_task_load()` 双 plane 拷贝、`completion_init` 循环到 `CW_TOTAL_MAX`、CW chaining 条件增加 `WITHOUT_DATA_OUT` |
| `src/nand/nand_dispatch_tables.c` | 新增 handler + 表项 | 新增 `req_multi_plane_read/program/erase`、`completion_multi_plane`；两张函数指针表各加 5 项 |
| `src/fc/fc.c` | 适配 | `cws[plane][cw]` 新索引语法 |

---

## Phase F: WAIT_READY 中断驱动 + 引擎超时分层修复

**日期**: 2026-09-21
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

WAIT_READY 原先通过 GPIO 轮询（`delay_us(1)` 循环）等待 NAND R/B# 引脚变高，浪费 CPU 资源。同时引擎层与 handler 层存在双重 DWT 超时竞争，导致 handler 的 GPIO 后备确认被引擎超时的早返回绕开。

### 改动

| 文件 | 改动 |
|------|------|
| `src/platform/stm32h743/nand_rb_interrupt.c` | `arm()` 在使能 EXTI6 后立即检查 RB 电平。若已为高（NAND 空闲）则直接标记 `triggered=1`，消除 arm 时 RB 已是高电平导致上升沿中断永不触发的问题 |
| `src/nand/nand_engine.c` | WAIT_READY 超时判定移交给 handler 全权处理，引擎层直接返回 NOT_READY。CHECK_READY 继续保持引擎层超时（依赖 70h 重发机制）。解决了原有引擎层 DWT 超时抢在 handler GPIO 后备之前返回 TIMEOUT 的问题 |
| `src/nand/nand_fmc.c` | `nand_fmc_exec_wait_ready()` 从 GPIO 轮询改为 EXTI6 中断方案：首次布防中断，后续检查 triggered/timed_out，超时后 GPIO 轮询最后一次确认 |
| `src/nand/nand_mt29f4g08abadawp.c` | `nand_init()` 中新增 `nand_rb_interrupt_init()` 调用 |
| `docs/design/fc_nand_design.md` | 更新 WAIT_READY/CHECK_READY 超时策略说明 |
| `src/platform/stm32h743/nand_rb_interrupt.c` | 模块注释更新工作流程 |

### 设计要点

#### WAIT_READY 与 CHECK_READY 的分层超时策略

| UOP 类型 | 超时判定方 | 后备机制 | 原因 |
|----------|-----------|---------|------|
| WAIT_READY | handler 全权处理 | 超时→GPIO 轮询最后一次确认 | 走 EXTI6 中断路径，需要 GPIO 回退防中断丢失 |
| CHECK_READY | 引擎层（DWT + wait_polls） | 超时→直接 TIMEOUT | 依赖 70h 状态寄存器，超时后重发 70h |

#### arm 时 RB 高电平的处理

`nand_rb_interrupt_arm()` 使能 EXTI6 后立即读一次 GPIO 电平。若 RB 已高（NAND 空闲），上升沿永不发生，直接标记 `triggered=1`，下次 step 立即返回 OK。

#### 中断丢失保护

即使 EXTI6 中断因噪声等原因丢失，WAIT_READY handler 在 `timed_out()==1` 时会回退到 GPIO 轮询确认，确保 NAND 不会因为中断丢失而被误判为超时。

### 资源开销

EXTI6 中断路径：中断进出约 60~80 cycles（Cortex-M7），远小于 GPIO 轮询的 `busy_timeout_us`（38~1050μs）。

---

## Phase G: DMA 中断驱动（DATA_IN/DATA_OUT 非阻塞）

**日期**: 2026-09-21
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

DATA_IN（PROGRAM 方向）和 DATA_OUT（READ 方向）的 DMA 传输使用 `HAL_DMA_PollForTransfer` 同步轮询等待完成。DMA 传输 528 字节期间（约 3μs）CPU 空转。

### 改动

将 DATA_IN/DATA_OUT 改为**两步模式**，与 WAIT_READY 的中断驱动方案完全一致：

| 步骤 | WAIT_READY | DATA_IN/DATA_OUT |
|------|-----------|-----------------|
| ① 首次 | `nand_rb_interrupt_arm()` 布防 EXTI6 | `nand_dma_*_start_IT()` 启动 DMA（中断模式） |
| ② 返回 | `NOT_READY` | `NOT_READY` |
| ③ ISR | `g_rb_int.triggered = 1` | `g_dma_data_out/in_done = 1` |
| ④ 后续 step | 检查 `triggered` → OK | 检查 `done` → OK |

### 文件变更

| 文件 | 改动 |
|------|------|
| `src/platform/stm32h743/nand_dma.c` | 重构：提取公共 init 函数；新增 `g_dma_data_out_done`/`g_dma_data_in_done` 完成标志；新增 `nand_dma_*_start_IT()` 非阻塞启动函数；新增 `nand_dma_data_out/in_done()` 检查函数；新增 `nand_dma_irq_handler_data_out/in()` ISR 入口 |
| `include/platform/stm32h743/nand_dma.h` | 新增中断版 API 声明；轮询版保留做后备 |
| `src/nand/nand_fmc.c` | `nand_fmc_exec_data_in()` 改为两步模式：首次启动 DMA(IT) 返回 NOT_READY，后续检查完成标志；`nand_fmc_exec_data_out()` 同理，完成后 Invalidate D-Cache；DMA 失败均回退 PIO |
| `src/platform/stm32h743/stm32h7rsxx_it.c` | 新增 `DMA2_Stream0_IRQHandler()` / `DMA2_Stream1_IRQHandler()`，分别调 `nand_dma_irq_handler_data_out/in()` |
| `include/platform/stm32h743/stm32h7rsxx_it.h` | 新增两个 ISR 声明 |

### 设计要点

1. **DMA 完成中断在 ISR 中只设标志**，真正的传输完成判定在 handler step 中检查。与 WAIT_READY 的 `triggered` 标志完全一致。
2. **DATA_OUT（READ 方向）的 D-Cache Invalidate** 在 handler 检查到 `done==1` 时执行，不在 ISR 中做（ISR 应尽量轻量）。
3. **DMA 失败回退链路**：中断版启动失败 → 轮询版 → PIO，逐级降级。
4. **DATA_IN（PROGRAM 方向）不需要 ISR 中 Invalidate**，因为 Clean 在启动前已做，DMA 只读不写 buffer。

---

## Phase H: ONFI 时序模式切换（低速/高速双模）

**日期**: 2026-09-21
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

NAND 初始化时使用保守时序（Mode 0, tRC=50ns）完成 RESET/READ_ID 后，通过 SET_FEATURES(01h) 将 NAND 切换到 Mode 4（tRC=20ns），但 FMC 硬件时序寄存器从未更新——FMC 始终跑在低速时序，浪费了 Mode 4 的速度提升。

### 改动

| 文件 | 改动 |
|------|------|
| `include/nand/nand_config.h` | 新增 `CONFIG_NAND_TIMING_MODE_4` 宏，0=低速（默认），1=高速 |
| `src/nand/nand_mt29f4g08abadawp.c` | 新增 `g_nand_timing_slow` / `g_nand_timing_fast` 两套 ONFI 时序参数表；新增 `nand_fmc_timing_to_pcc()` 将 ONFI 时序换算为 FMC PCC 硬件寄存器值；`nand_init()` 的条件分支：低速模式跳过 Mode 4 切换，高速模式先切 NAND 再重新初始化 FMC 硬件 |

### 设计要点

#### 双路径

低速模式（`CONFIG_NAND_TIMING_MODE_4=0`，默认）：
```
HAL_NAND_Init(保守) → RESET/READ_ID/校验 → 跳过 Mode 4 → 完成
  NAND: Mode 0（默认, tRC=50ns）
  FMC:  保守时序
```

高速模式（`CONFIG_NAND_TIMING_MODE_4=1`）：
```
HAL_NAND_Init(保守) → RESET/READ_ID/校验 → SET_FEATURES(Mode 4)
  → nand_fmc_timing_to_pcc(fast) → HAL_NAND_Init(快速) → nand_fmc_configure(fast) → 完成
  NAND: Mode 4（tRC=20ns）
  FMC:  快速时序
```

#### ONFI → FMC 换算规则

| FMC 寄存器 | ONFI 参数 | 公式 |
|-----------|-----------|------|
| `SetupTime` | max(tCLS, tALS, tCS) | /5ns 向上取整 |
| `WaitSetupTime` | max(tRP, tWP) | /5ns 向上取整 |
| `HoldSetupTime` | max(tCH, tALH, tCLH, tDH) | /5ns 向上取整 |
| `HiZSetupTime` | tRHZ | /5ns 向上取整 |

FMC kernel clock = 220 MHz，tHCLK ≈ 4.55ns，取整为 5ns 计算周期数。

### 后续工作

- 当前 `nand_fmc_timing_to_pcc()` 用 `/5ns` 简单换算，未考虑 FMC 建立/保持时间的余量。如需更精确的时序适配，需引入 FMC 时序模型根据 HCLK 频率动态计算。
- 默认保持低速模式，功能验证稳定后改为高速模式。

---

## Phase I: 读饥饿保护（Read Starvation Protection / QoS）

**日期**: 2026-09-21
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

调度器对读写命令一视同仁，写命令或擦除命令连续占用 LUN 时，读命令可能被长时间阻塞。在单 LUN 场景下虽不明显，但在多 LUN/多通道架构下读延迟可能大幅恶化。

### 改动

| 文件 | 改动 |
|------|------|
| `include/fc/fc_scheduler.h` | 新增 `FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET` 宏（默认 3）；`fc_scheduler_t` 新增 `consecutive_write_count` 计数器 |
| `src/fc/fc_scheduler.c` | 新增 `fc_scheduler_op_is_read()` 识别 16 种读操作；`select_chain()` SQ 遍历中增加读饥饿保护逻辑 + `sq_has_read` 前置死锁检测 |

### 设计要点

#### 工作原理

调度器维护一个 `consecutive_write_count` 计数器，连续执行非读命令（PROGRAM/ERASE/SET_FEATURES 等）时递增，执行读命令时清零。当计数达到 `FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET(3)` 时，后续非读命令被**旋转到队尾**，让排队的读命令先执行。

#### 死锁防护

添加 `sq_has_read` 前置扫描：仅在 SQ 中确实存在读命令时才触发优先保护。全写队列场景下不拦截，避免写命令被无限旋转。

#### 队列操作

SQ 是环形 FIFO，旋转通过 `pop(队头) + push(队尾)` 实现。只动 `head/tail` 指针，Command Pool 内容不变。旋转不是丢弃——命令最终会回到队头执行。

#### 场景验证

| 场景 | 行为 |
|------|------|
| 全写队列 | sq_has_read=0 → 不触发保护 → 所有写正常执行 |
| 读写混合，写 ≤ 3 | count < 3 → 不触发保护 |
| 写 ≥ 3 + 队列有读 | sq_has_read=1 → 后续写旋转 → 读优先执行 → count 归零 |

---

## Phase J: 加扰引擎扩展（多算法 + 多种子策略）

**日期**: 2026-09-21
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

加扰引擎仅支持 XOR-shift 一种算法，种子策略单一。实际消费级/企业级 SSD 更广泛使用 LFSR 标准算法，且不同写入场景需要不同的种子扩散策略。

### 改动

| 文件 | 改动 |
|------|------|
| `include/fc/fc_random_engine.h` | 新增 `FC_RANDOM_ALGO_*` 三种算法宏、`FC_RANDOM_SEED_STRATEGY_*` 三种种子策略宏；新增 `fc_random_algo_name()` / `fc_random_seed_strategy_name()` 调试接口 |
| `src/fc/fc_random_engine.c` | 重构为算法/策略分离的内置实现；新增 LFSR 16（ONFI 标准多项式）实现、新增 Simple XOR 实现、新增 Enhanced / Program-sequence-weighted 两种种子策略；默认算法从 XOR-shift 改为 **LFSR 16** |

### 默认配置

```c
#define FC_RANDOM_ALGO          FC_RANDOM_ALGO_LFSR       /* ONFI 标准多项式的 LFSR 16 */
#define FC_RANDOM_SEED_STRATEGY FC_RANDOM_SEED_BASIC      /* program_sequence ^ PAA */
```

### 算法对比

| 算法 | 默认 | 核心机制 | 周期 | 每字节开销 | 行业对标 |
|------|------|---------|------|-----------|---------|
| **LFSR 16** | ✅ 默认 | 多项式 x¹⁶ + x¹⁴ + x¹³ + x¹¹ + 1 | 2¹⁶-1 | ~40 ops | ONFI 标准，消费/企业级最主流 |
| XOR-shift 32 | — | seed ^= seed<<13; ^= seed>>17; ^= seed<<5 | 2³²-1 | ~6 ops | 三星等消费级同类 |
| Simple XOR | — | 固定 64B pattern 循环异或 | pattern 长度 64 | ~2 ops | 低端方案 |

### 种子策略对比

| 策略 | 默认 | 特点 |
|------|------|------|
| **Basic** | ✅ 默认 | program_sequence ^ PAA 简单混合 |
| Enhanced | — | 各字段独立 bit 位 + 黄金分割扩散 + CW 维度 |
| Program-seq-weighted | — | 高权重写入序列，高耐久场景 |

### 企业级注意

企业级与消费级在加扰算法上没有本质区别，都使用 LFSR 或 XOR-shift。真正区别在于：企业级强制 AES-256 XTS 加密（加密后数据天然随机化）、硬件 DMA 引擎 inline 实现零 CPU 开销、种子可能混入更多维度（Die ID 等）。

---

## Phase K: BCH ECC 引擎移植 + QC-LDPC 硬判决

**日期**: 2026-09-22
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

ECC 引擎长期为 stub（返回 NOT_SUPPORTED），无法实际纠错。同时需要为未来 LDPC 算法预留架构扩展点。

### 改动

| 文件 | 改动 |
|------|------|
| `include/fc/fc_command.h` | `fc_ldpc_mode_t` 重定义为 `fc_ecc_mode_t`，语义变更为 ldpc=0→BCH, 1→LDPC hard, 2→LDPC soft, 3→LDPC raw |
| `include/fc/fc_ldpc_engine.h` | 新增 `FC_ECC_ALGO_BCH/LDPC` 编译宏；`fc_ldpc_config_t` 保留不变 |
| `src/fc/fc_ldpc_engine.c` | 重构为分发层：`FC_ECC_ALGO` 宏选择 BCH 或 LDPC 实现，调用下层引擎 |
| `include/fc/fc_bch_engine.h` | **新建**：BCH 引擎接口（`fc_bch_init/encode/decode/free/ecc_bytes/t`） |
| `src/fc/fc_bch_engine.c` | **新建**：BCH(m=13, t=4) 完整实现，移植自 Linux 内核 lib/bch.c（GPL v2），作者 Ivan Djelic |
| `include/fc/fc_ldpc_qc_engine.h` | **新建**：QC-LDPC 引擎接口（单例初始化，encode/decode） |
| `src/fc/fc_ldpc_qc_engine.c` | **新建**：QC-LDPC 硬判决 Bit-flipping 完整实现，全局静态数组零动态分配 |

### BCH 参数

| 参数 | 值 | 说明 |
|------|-----|------|
| m | 13 | Galois 域阶数，n=2¹³-1=8191 |
| t | 4 | 纠错能力（bit） |
| parity | 7B（52bit）→ 8B 够用 | BCH 校验码长度 |
| DATA 编码范围 | 520B | META(4) + CRC(4) + USER(512) |
| 查表大小 | ~40 KB | a_pow_tab(16KB) + a_log_tab(16KB) + mod8_tab(8KB) |

### QC-LDPC 参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 码长 n | 4224 bit (528B) | 完整 CW |
| 信息位 k | 4160 bit (520B) | META+CRC+USER |
| 校验位 m | 64 bit (8B) | parity 区 |
| 码率 R | 0.985 | 极高码率 |
| Z | 16 | 循环子矩阵大小 |
| H_base | 4 × 264 | 基矩阵 |
| 列重 dv | 3 | 每比特参与 3 个校验方程 |
| 解码 | Bit-flipping | 硬判决，阈值自适应递减 |

### 存储优化

全局静态数组，全部使用最小数据类型，零动态分配：

| 数组 | 类型 | 大小 |
|------|------|------|
| `g_col_to_check[4224][3]` | `uint8_t` | 12 KB |
| `g_col_weight[4224]` | `uint8_t` | 4 KB |
| `g_row_to_var[64][~66]` | `uint16_t` | ~8.5 KB |
| `g_h_base[4][264]` | `int8_t` | 1 KB |
| **合计** | | **~25 KB** |

### CW 布局（ECC 部分不变）

```
[META:4][CRC:4][USER:512][PARITY:8] = 528B
                        ↑ BCH t=4 或 QC-LDPC
```

### fc_data_path.c 调用链路

BCH 或 LDPC 算法选择由编译宏 `FC_ECC_ALGO` 控制，运行时 `ldpc` 位域预留未来切换能力。`fc_data_path.c` 代码零改动。

### 已知限制

- 码率 0.985 下 LDPC 硬判决纠错能力约 t=1~2，低于 BCH t=4
- LDPC 的真正优势在低码率（r<0.9）+ 软判决场景
- 软判决需要 NAND 支持多电压读取（multi-read → LLR），当前 SLC 硬件不支持

---

## Phase L: RAID XOR 引擎实现 + 管线重排

**日期**: 2026-09-22
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

RAID 引擎长期为 stub（返回 NOT_SUPPORTED）。同时 decode 管线中 RAID 放在 ECC 之前，顺序错误——ECC 未解码时无法知道哪个 CW 需要 RAID 恢复。

### 改动

| 文件 | 改动 |
|------|------|
| `include/fc/fc_raid_engine.h` | 重写：新增 `fc_raid_context_t`（per-LUN 累加器）、START/ACCUMULATE/END 序列接口、`fc_raid_save/restore` 中断保护、`fc_raid_recover` 静态恢复 |
| `src/fc/fc_raid_engine.c` | 完整实现：全局静态上下文池、32bit 对齐 XOR 加速、`processed>=total` 溢出保护、影子缓冲 save/restore |
| `src/fc/fc_data_path.c` | 重写：写路径接入 RAID XOR 编码、读路径修正为 ECC→条件 RAID→Random 顺序、新增 `fc_collect_buffers` 辅助、decode 中 buffer=NULL 不再静默跳过 |
| `src/fc/fc.c` | `fc_init_internal()` 中新增 `fc_raid_init_ctx()` 调用，遍历所有 LUN 初始化 |

### 设计要点

#### 管线顺序（已修正）

```
写入 (encode):
  Random → ECC → RAID XOR  ← RAID 保护 ECC 后的完整码字

读取 (decode):
  ECC → (若 UNC) RAID 恢复 → 重试 ECC → Random
  RAID 不是固定 stage，是条件性恢复路径。ECC 全部成功则 RAID 完全跳过。
```

#### RAID 序列模型

三个基础指令模拟企业硬件 RAID 加速器：

| 指令 | 功能 |
|------|------|
| START | 拷贝第一个 CW 到 acc_buf，标记 active |
| ACCUMULATE | acc_buf ^= cw_buffer |
| END | acc_buf 写入 parity_buffer，清 active |

per-LUN 独立上下文，支持多 LUN 并发 RAID 序列。

#### 中断保护

`fc_raid_save/restore` 为多 Plane（≥4）场景预留。当前 PLANE_NUM=2 时一次 START→ACCUMULATE→END 瞬间完成，无中断风险。影子缓冲机制已预留，待调度器中 HSQ 打断路径接入后生效。

#### 静态恢复（fc_raid_recover）

用于 ECC 解码失败后的条件恢复。输入所有 CW buffer（含 parity）和 failed_idx，输出恢复后的数据。不依赖 LUN 上下文，支持跨 Plane 恢复。

### RAID 恢复的数学原理

```
Parity = CW0 ⊕ CW2 ⊕ CW3        (写时计算)
CW2(损坏) = CW0 ⊕ Parity ⊕ CW3  (读时恢复)  → 数学精确，无损
```

XOR 恢复是线性无损的。正确的管线顺序保证 parity CW 同样经过 ECC 编码，受 NAND 存储保护。

### 当前限制

- 只支持 1 个 CW 失败（单 XOR 极限）
- 多个 CW 同时失败时返回 IO_ERROR
- PLANE_NUM=4 以上时需要调度器接入 fc_raid_save/restore 保护累加器

---

## Phase M: 读路径管线拆分 — FC 内部 ECC 解码 + FTL 触发 RAID 恢复

**日期**: 2026-09-22
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

之前的 `fc_data_path_decode` 将 ECC 解码、RAID 恢复、Random 解扰捆绑在一个全自动管线中，存在三个问题：

1. **架构分层错误**：FC controller 不应该替 FTL 决策 RAID 恢复策略；FTL 需要根据 per-CW status 精细化处理
2. **dead code**：`count == failed_idx` 用于跳过 RAID 恢复后的 CW，但控制流是单遍的，永远不会触发
3. **不在主路径上**：`fc_data_path_decode` 从未被调用，ECC/RAID 管线在 CQ push 前完全未执行

### 改动

| 文件 | 改动 |
|------|------|
| `include/fc/fc_data_path.h` | 接口重设计：`fc_data_path_decode` → `fc_data_path_process_read()` + `fc_data_path_raid_recover()`，头文件文档明确分工和约束 |
| `src/fc/fc_data_path.c` | 重写实现：拆为 FC 内部 Stage1 + FTL 触发 Stage2；去掉了 dead code；修复 `count < 2` 时静默吞没失败的问题 |
| `src/fc/fc.c` | `fc_complete_from_nand()` 中插入 `fc_data_path_process_read()` 调用，CQ entry 的 status 从 descriptor 读取（ECC 解码后结果）；新增 `fc_is_page_read()` 白名单区分 Page Read 与非数据类命令 |
| `src/fc/fc_raid_engine.c` | 添加 `#include "fc.h"` 修复 `FC_BUFFER_POOL_CW_SIZE` 未声明编译错误 |

### 设计要点

#### 读路径两阶段分工

```
Stage 1 — FC 内部（CQ push 前）:
  fc_data_path_process_read()
    遍历 desc 链逐 CW:
      ECC 解码 → 更新 desc->status
    全部成功 → 解扰所有 buffer → 推 CQ (status=READY)
    有失败   → 不解扰任何 buffer → 推 CQ (status=UNC)
    此时非失败 CW 处于 "ECC 已纠正但仍加扰" 状态

Stage 2 — FTL 触发（poll CQ 后）:
  fc_data_path_raid_recover()
    收到 CQ entry → 遍历 desc 链发现 status=UNC
    调用 fc_data_path_raid_recover()
      ├─ RAID XOR 重建 failed CW
      ├─ ECC 重试
      ├─ 解扰所有 CW
      └─ 更新所有 desc->status = READY
    仍失败 → 走其他策略（read retry / BBT 等）
```

#### FC 内部集成

- 仅对 **Page Read 操作**（`fc_is_page_read()` 白名单）执行 Data Path
- `READ_ID` / `GET_FEATURES` / `READ_STATUS` 等绕行
- CQ entry 的 per-CW status 从 descriptor 读取（ECC 解码后的结果）
- 非 Page Read 命令透传 NAND completion 原始状态

#### 文档更新

- `docs/design/fc_nand_design.md` §5 数据路径章节：重写为两阶段 decode 流程图，更新 encode 路径加上 RAID，更新 8.1 占位模块状态为 ✅ 已完成，更新 8.3 功能扩展规划 Phase 4/5 为 ✅

### 解决的问题

| 编号 | 问题 | 状态 |
|------|------|------|
| #1 | 架构分层错误：FC 不应替 FTL 决策 RAID 恢复 | ✅ 已修复 |
| #2 | `count == failed_idx` dead code | ✅ 已移除 |
| #3 | decode 管线不在主路径上 | ✅ 已集成到 `fc_complete_from_nand` |
| #4 | `count < 2` 时静默吞没失败 | ✅ 已修复 |

---

## Phase N: FC↔NAND 层数据路径闭环 — encode 集成 + 边界修正 + 缺陷修复

**日期**: 2026-09-22
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

Phase M 完成了读路径的管线拆分和白名单集成，但在后续的全面审查中发现了多个闭环问题：

1. **encode 路径未集成**：`fc_data_path_encode` 从未被调用，NAND 写入的是未编码的原始数据，与读路径的 ECC 解码不对称
2. **Page Read 白名单不准确**：`WITHOUT_DATA_OUT` 操作无数据传输不应走 Data Path；`READ_FOR_INTERNAL_DATA_MOVE` 使用 `req_no_data` 无 host buffer
3. **Multi-Plane 读数据未拆分**：OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT 读入 page_buffer 后未拆分到各 CW buffer
4. **failed_descriptor_id 偏离**：ECC 解码后失败 CW 可能变化，但 CQ entry 的 desc_id 仍指向 NAND 层原始值
5. **CQ 满时命令泄漏**：`fc_complete_internal` 失败时命令状态保持 ACTIVE 导致泄漏
6. **迭代风格不统一**：fc_data_path.c 使用 `while (cw <= config.cw)` 而 fc.c 使用 `for (cw < config.cw+1)`

### 改动

| 文件 | 改动 |
|------|------|
| `src/fc/fc.c` | ① 新增 `fc_is_page_write()` 白名单 ② 修正 `fc_is_page_read()` 白名单：移除 `WITHOUT_DATA_OUT`、`READ_FOR_INTERNAL_DATA_MOVE` ③ 单命令/链命令路径插入 `fc_data_path_encode()` 调用 ④ `failed_descriptor_id` 在 ECC 解码后更新为第一个 UNC desc 的 ID ⑤ CQ push 失败时强制标记 COMPLETED 防止命令泄漏 |
| `src/nand/nand_dispatch.c` | 新增 Multi-Plane WITH_DATA_OUT 完成后的 page_buffer→CW buffer 拆分循环 |
| `src/fc/fc_data_path.c` | Pass 1 loop 统一为 `for (cw = 0; cw < config.cw + 1)` 写法 |

### 设计要点

#### encode 集成点

```
FTL submit → fc_poll_internal 调度
  → fc_data_path_encode(command)         ← 新增：Random 加扰 → ECC 编码 → RAID XOR
  → 构建 nand_dispatch_request
  → nand_dispatch_submit / submit_chain
```

encode 在 resolve descriptor 链之前执行，直接修改 buffer pool 中的数据。与 decode 路径对称：encode 在 submit 前、decode 在 complete 后。

#### fc_is_page_read / fc_is_page_write 白名单

```
fc_is_page_read() — Page Read 操作（数据经 ECC 编码）:
  OP_NORMAL_READ_PAGE_WITH_DATA_OUT          ✅
  OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT     ✅（需配合 buffer 拆分）
  OP_READ_PAGE_CACHE_SEQ/RANDOM/LAST         ✅
  OP_RANDOM_DATA_READ                        ✅
  ---
  OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT       ❌ 无数据输出
  OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT  ❌ 无数据输出
  OP_READ_FOR_INTERNAL_DATA_MOVE             ❌ req_no_data，无 host buffer

fc_is_page_write() — Page Program 操作（数据需 ECC 编码）:
  OP_NORMAL_PROGRAM_PAGE                     ✅
  OP_PROGRAM_PAGE_CACHE                      ✅
  OP_OTP_DATA_PROGRAM                        ✅
  OP_PROGRAM_PAGE_MULTI_PLANE                ✅
  ---
  OP_PROGRAM_FOR_INTERNAL_MOVE               ❌ 内部搬移，非 ECC 页数据
```

#### Multi-Plane 读数据拆分

```
poll 中 Multi-Plane WITH_DATA_OUT 完成后：
  page_buffer[PAGE_BUFFER_SIZE] (4224B)
    ├─ Plane 0 [0..2111] → cw_buffers[0][0..3] (每个 528B)
    └─ Plane 1 [2112..4223] → cw_buffers[1][0..3] (每个 528B)
```

拆分后在 fc_complete_from_nand 中走 Data Path ECC 解码。

#### CQ 满时命令泄漏保护

```
fc_complete_from_nand:
  Step 1: writeback desc status
  Step 2: process_read (ECC 解码)
  Step 3: populate CQ entry
    → fc_complete_internal() 失败（CQ 满）
    → 强制 command_state = COMPLETED
    → 返回 IO_ERROR
```

强制切 COMPLETED 后，FTL 无法通过 fc_poll_completion 感知此命令（CQ entry 丢失）。
这是 fatal 级系统背压错误，需要上层（FTL/主机驱动）通过寄存器轮询检测 CQ 满条件后干预。

### 解决的问题

| # | 问题 | 状态 |
|---|---|---|
| #A | `fc_data_path_encode` 未调用，读写编码不对称 | ✅ 已修复 |
| #B | Multi-Plane WITH_DATA_OUT 数据未拆分到 CW buffer | ✅ 已修复 |
| #C | `fc_is_page_read` 白名单含不应走 Data Path 的操作 | ✅ 已修复 |
| #C2 | `fc_is_page_write` 白名单含不应 encode 的操作 | ✅ 已修复 |
| #1 | `failed_descriptor_id` 在 ECC 后指向错误 CW | ✅ 已修复 |
| #E | CQ push 失败时命令泄漏（ACTIVE 状态） | ✅ 已修复 |
| #4 | 迭代条件写法不统一 | ✅ 已修复 |

---

## Phase O: 编译警告清理 — 链接修复 + 类型安全 + 死代码防护

**日期**: 2026-09-22
**涉及的提交**: （尚未合并，当前工作区改动）

### 背景

Phase M/N 引入的代码以及历史遗留问题累计了约 30 个编译警告（剔除占位文件的 `#warning` TODO 标记和 OP 宏文档区重复定义），包括链接错误、类型不兼容、未用变量/函数、指针对齐等问题。

### 改动

| 文件 | 改动 |
|------|------|
| `include/nand/onfi_1_0.h` | 删除 OP_PROGRAM_PAGE_MULTI_PLANE(30U) 和 OP_ERASE_BLOCK_MULTI_PLANE(33U) 的文档区重复定义（值分别为 30U/33U，与正式定义区 31U/32U 冲突） |
| `src/fc/fc_bch_engine.c` | ① 实现 `deg()` 函数体（原仅前向声明无定义，链接错误） ② 实现 `gf_div()` 函数体（原未定义，链接错误） ③ `xi_tab` 分配类型从 `uint32_t*` 修复为 `unsigned int*`（与结构体成员类型匹配） ④ sign-compare 警告统一加 `(int)` 显式转换 ⑤ cast-align 警告改为 `(const uint32_t *)(const void *)` 中间转换 |
| `src/fc/fc_ldpc_engine.c` | `g_qc` 变量和 `fc_ecc_init_ldpc()` 用 `#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)` 条件编译保护 |
| `src/fc/fc_raid_engine.c` | 4 处 `uint8_t*`→`uint32_t*` cast 加 `(void*)` 中间转换消除 cast-align 警告 |
| `src/fc/fc_scheduler.c` | `fc_submission_queue_pop(..., &cmd)` 类型不兼容（cmd 为 `fc_command_t*`，pop 需 `fc_command_id_t*`）修复 |
| `src/nand/nand_dispatch.c` | `g_nand_channel_page_buffer` 去掉 `static` 使 nand_dispatch_tables.c 的 `extern` 引用可链接 |
| `src/nand/nand_mt29f4g08abadawp.c` | `g_nand_timing_fast` 和 `nand_fmc_timing_to_pcc()` 用 `#if (CONFIG_NAND_TIMING_MODE_4 != 0)` 条件编译保护 |
| `src/platform/stm32h743/delay.c` | 添加 `g_dwt_cyccnt_per_us` 全局变量定义并与 `g_dwt_cycles_per_us` 同步初始化，修复 nand_rb_interrupt.c 的 `extern` 链接 |
| `src/platform/stm32h743/nand_rb_interrupt.c` | 删除未用的 `GPIO_InitTypeDef gpio` 变量 |

### 解决的问题

| # | 问题 | 状态 |
|---|---|---|
| #1 | `fc_bch_engine.c`: `deg()` 声明无定义，链接失败 | ✅ 已修复 |
| #2 | `fc_bch_engine.c`: `gf_div()` 未声明未定义，`find_poly_deg3/4_roots` 链接失败 | ✅ 已修复 |
| #3 | `fc_bch_engine.c`: `xi_tab` 类型不匹配 (`uint32_t*` vs `unsigned int*`) | ✅ 已修复 |
| #4 | `fc_bch_engine.c`: 3 处 sign-compare + 1 处三目符号变化 | ✅ 已修复 |
| #5 | `fc_bch_engine.c` / `fc_raid_engine.c`: cast-align 警告 | ✅ 已修复 |
| #6 | `fc_ldpc_engine.c`: `g_qc` / `fc_ecc_init_ldpc` 在 BCH 编译模式下未使用 | ✅ 已修复 |
| #7 | `fc_scheduler.c`: `pop(&cmd)` 传参类型不兼容 | ✅ 已修复 |
| #8 | `nand_dispatch.c`: `g_nand_channel_page_buffer` static 导致跨文件链接失败 | ✅ 已修复 |
| #9 | `nand_mt29f4g08abadawp.c`: Mode 4 函数/常量在默认配置下未使用 | ✅ 已修复 |
| #10 | `delay.c`: `g_dwt_cyccnt_per_us` 未定义导致 nand_rb_interrupt 链接失败 | ✅ 已修复 |
| #11 | `nand_rb_interrupt.c`: 未用变量 `gpio` | ✅ 已修复 |
| - | `onfi_1_0.h`: OP_PROGRAM/ERASE_MULTI_PLANE 宏重复定义 | ✅ 已修复 |

---

## Phase P: FC dispatch Phase 1 修复 + WAIT_READY spin-wait + META 接口

**日期**: 2026-09-24

### 背景

nand_dispatch_poll() 中 Phase 1 看到 task->started == 0 后直接 break，期望 Phase 2 启动命令。但 submit() 已设 chain->count=1，Phase 2 看到 chain->count != 0 以为 LUN busy 而跳过，命令永远无法启动。

WAIT_READY handler 原返回 NOT_READY 依赖引擎 DWT 超时，但 DWT 在 tWB 窗口期间启动计时，导致 handler 确认 NAND busy 后下一次 step 就被误判超时。

### 改动

| 文件 | 改动 |
|------|------|
| `src/nand/nand_fmc.c` | `nand_fmc_exec_wait_ready` 重写为 handler 内 spin-wait + 超时，不再返回 NOT_READY；DATA_IN/DATA_OUT 改为 PIO/DMA 双模（CONFIG_NAND_DMA 编译开关 + g_nand_dma_enable 运行态切换） |
| `src/fc/fc.c` | program 前写 META（PAA 到 CW[0..3]）；新增 `fc_buffer_get_meta()` |
| `include/fc/fc.h` | 声明 `fc_buffer_get_meta()` |
| `include/nand/nand_config.h` | 新增 `CONFIG_NAND_DMA` 宏定义（PIO/DMA 编译开关） |
| `include/platform/stm32h743/nand_dma.h` | 声明 `g_nand_dma_enable` 运行态全局变量 |
| `src/platform/stm32h743/nand_dma.c` | 新增 `g_nand_dma_enable` 定义，默认等于 CONFIG_NAND_DMA |
| `tests/unit/test_fc_level3.c` | 新增 `memset_read_buffer()` + `unlock_block()` + `memset 0xCC` debug 辅助；全量 `FC_ECC_MODE_BCH` → `FC_ECC_MODE_LDPC_RAW` 跳过 ECC |
| `docs/design/fc_nand_design.md` | WAIT_READY 策略说明从"中断驱动"改为"handler 内 spin-wait" |

### 设计要点

#### Phase 1 直接启动 pre-loaded 命令

单命令场景：submit() 预加载 chain->count=1、task->started=0 的同时推入 channel queue。Phase 1 检查到 started==0 后直接调用 req_table 初始化 request，然后 nand_fc_start() 启动。同步完成时消耗 queue entry 避免 Phase 2 重复启动。

#### WAIT_READY handler 内 spin-wait

确认 RB#=LOW（NAND busy）后，在 handler 内部 1µs/步 poll，直到 RB#=HIGH 或超时。tWB 窗口保护：第一次读 RB#=HIGH 时等 2µs 后重读。不再返回 NOT_READY，引擎 DWT 超时变为死代码。

#### DATA_IN/DATA_OUT PIO/DMA 双模

引入 `CONFIG_NAND_DMA` 编译开关 + `g_nand_dma_enable` 运行态标志：

```
CONFIG_NAND_DMA = 0（默认，PIO）：
  DATA_IN/DATA_OUT handler → CPU 逐字节循环

CONFIG_NAND_DMA = 1（DMA）：
  DATA_IN/DATA_OUT handler → 三态 DMA 非阻塞：
    ① done 标志 → OK
    ② DMA EN 位 busy → NOT_READY
    ③ 首次 → 启动 DMA_IT → 失败回退 PIO
```

运行时可通过 `g_nand_dma_enable = 1/0` 动态切换，切换需在 NAND 空闲时进行。

#### META 字段自动填写

FC 在 program 前将 desc->paa 写入 CW buffer[0..3]（META 区域），ECC 覆盖整个数据区包括 META。读回后可通过 `fc_buffer_get_meta()` 获取 META 指针，验证 PAA 一致性。

### DMA 不可用结论

STM32H743 的 FMC NAND 控制器**没有专用硬件 DMA 请求线**（对照 stm32h7xx_hal_dma.h 所有 DMA_REQUEST_* 定义，0~137 中无 FMC/NAND。DMAMUX1 映射表中 FMC 不存在）。DMA memory-to-memory 模式下 DMA2 无法与 FMC NAND 的等待状态握手，导致数据错位或卡死。

FMC NAND 只能走 **PIO 逐字节访问**。这在工业产品中是真实可用的方案。如需 DMA + 硬件 ECC 的 NAND 控制器，建议换 NXP i.MX RT 系列（GPMI 控制器内建 DMA + BCH ECC）。

### 当前状态

测试通过（100%）。ECC 仍绕过（FC_ECC_MODE_LDPC_RAW）。

---

## Phase Q: 全测试流程修复 — Buffer 泄漏 + g_nand_device_ready 重置 + NOCACHE 段 + 多 Plane 支持

**日期**: 2026-09-26

### 背景

全量跑 `test` 时 Level 3 lifecycle 测试全部失败，单跑 `test /fc-nand/lifecycleinit_sequence` 却全部通过。异同分析暴露了两个问题。

### 问题 1: g_nand_device_ready 被 fc_init() 重置（根因）

`fc_init()` 内部调用 `nand_dispatch_init()` 将 `g_nand_device_ready` 置零。

| 场景 | 调用链 | device_ready |
|------|--------|-------------|
| main() 启动 | fc_init() → nand_dispatch_init() → 0 → nand_init() → ... → 1 | ✅ 1 |
| Level 1 setup | fc_init() → nand_dispatch_init() → 0 | ❌ 0 |
| Level 2 setup | fc_init() → 已 init → 跳过 | ❌ 0（保持）|
| 全量跑 L3 | nand_dispatch_submit → device_ready=0 → NAND_FAIL | ❌ |
| 单跑 L3 | 不触发 fc_init() → device_ready=1 → 正常 | ✅ |

**修复**：`setup_level3` 中调用 `nand_dispatch_set_device_ready(1)` 恢复。

### 问题 2: Level 2 Buffer 泄漏

`fc_command_release()` 释放 command 和 descriptor，但不释放 buffer（buffer 由测试显式释放）。Level 2 每测试分配 4 个 buffer（4 CW × 1 cmd），`fc_command_release` 只释放 command + descriptor，buffer 不释放。10 个测试后泄漏 22 个 buffer。

**修复**：每个 `alloc_cmd` + `fc_command_release` 对之间补 `release_command_buffers(cmd)`。

### 文件变更

| 文件 | 变更 | 说明 |
|------|------|------|
| `include/fc/fc_command.h` | `reserved:6` → `multi_plane:3 + reserved:3` | 多 Plane 控制位域 |
| `src/fc/fc.c` | 多处修改 | descriptor PAA 生成、request 多 Plane 分拣、OP_ERASE_BLOCK_MULTI_PLANE 独立分支、buffer 出口策略（page vs non-page） |
| `src/fc/fc_data_path.c` | `total_desc` 计算 | process_read 支持多 desc |
| `src/nand/onfi_1_0.c` | `g_nand_read_address_uops` 加 WAIT_READY | 修复 READ_PARAMETER_PAGE/GET_FEATURES 读回 0xFF |
| `linker/stm32h743xx_flash.ld` | AXI_NOCACHE 32KB → 128KB | 容纳 buffer pool |
| `src/platform/stm32h743/sys.c` | MPU Region 11 128KB | 对应链接脚本 |
| `src/fc/fc.c` | `g_fc_buffer_pool` 加 `.noncacheable_buffer` | 解决 D-Cache 不一致 |
| `src/nand/nand_dispatch.c` | `g_nand_channel_page_buffer` 加 `.noncacheable_buffer` | 同上 |
| `tests/unit/test_fc_level1.c` | 6 个 scheduler 测试补 `fc_command_release` | 消除 command 泄漏 |
| `tests/unit/test_fc_level2.c` | 补 `release_cmd_buffers` | 消除 buffer 泄漏 |
| `tests/unit/test_fc_level3.c` | 重写 3 个测试 + 资源打印 | multi_plane / random_data / lock_tight |

## Phase R: 内部搬移 OP 码修正 — 去 DATA_IN + 4CW 对齐 + 设计文档

**日期**: 2026-09-26

### 背景

`test_internal_data_move` 运行失败，报错 `page=0 cw=0 offset=0: got 0xFF expected 0x00`：
1. READ_FOR_INTERNAL_DATA_MOVE 和 PROGRAM_FOR_INTERNAL_MOVE 的 `config.cw=0`（仅 1 个 desc），与写/读的 4 CW 不匹配，验证阶段读回 4 CW 时报多出的 CW 为 0xFF
2. `g_nand_internal_program_uops` 含有 NAND_UOP_DATA_IN（发送 1 字节未初始化数据），破坏了 NAND cache register 内容，导致目标页全 0xFF

### 改动

| 文件 | 变更 | 说明 |
|------|------|------|
| `src/nand/onfi_1_0.c` | `g_nand_internal_program_uops` 删除 NAND_UOP_DATA_IN | copy-back 真 85h-10h，无数据输入 |
| `src/nand/onfi_1_0.c` | `OP_PROGRAM_FOR_INTERNAL_MOVE` data_input_cycles=0, data_length=0, has_data=1 | 纯内部搬移，无数据输入但有 desc chain |
| `src/nand/onfi_1_0.c` | `OP_READ_FOR_INTERNAL_DATA_MOVE` has_data=0→1 | 需要 desc chain 提供 CW PAA 映射 |
| `tests/unit/test_fc_level3.c` | `test_internal_data_move` 读/写 `config.cw = CW_NUM - 1` + call `fc_command_configure_descriptors` | 4 CW 对齐，desc chain 提供 per-CW PAA |
| `docs/design/fc_nand_design.md` | 新增 §17.6 内部搬移 OP 码表格 | `PROGRAM_FOR_INTERNAL_MOVE` vs `PROGRAM_FOR_INTERNAL_DATA_MOVE` 对比 |

### 设计要点

#### OP 码语义明确化

| OP | 命令序列 | 数据路径 | 用途 |
|----|---------|---------|------|
| `READ_FOR_INTERNAL_DATA_MOVE` | 00h+addr→35h→tR | NAND cache 内部，无 host 数据 | GC 读源页 |
| `PROGRAM_FOR_INTERNAL_MOVE` | 85h+addr→10h→tPROG | NAND cache 内部转发，无 DATA_IN | GC 纯搬移 |
| `PROGRAM_FOR_INTERNAL_DATA_MOVE` | 85h+addr+1B→10h→tPROG | 1 字节 DATA_IN | GC + ECC 纠正 |

#### `has_data=1, data_length=0` 含义

`has_data=1` 使 fc_command_configure_descriptors() 分配 desc chain，提供 per-CW PAA 映射给 nand_dispatch。但 internal move OP 的 data 不走 host buffer（data_length=0），nand_fc_request_t 的 data=NULL 且 data_length=0，NAND 引擎跳过 DATA_IN/DATA_OUT uop。desc chain 仅用于"行走 PAA 空间"——dispatch 需要知道每个 CW 映射到哪个物理 page/block。`

---

## Phase S: nand_dispatch_poll 按企业后端分层拆分

**日期**: 2026-10-04

### 背景

`nand_dispatch_poll()` 单函数 350 行（旧 `src/nand/nand_dispatch.c:347-696`），把企业主控里本来分开的两个模块焊死在一起：

- **Arbiter / Scheduler**（决定"轮到哪个 die"）：Phase 2 出队 + `g_nand_channel_cursor` round-robin
- **Per-die FSM**（决定"这个 die 走到哪一步"）：`chain->phase` 相位机 + ISSUE/ISSUED/FETCH 推进

外加 Data path（CW chaining、三种拆页）与 Completion 聚合。后果：8 个局部变量作用域横跨 350 行，"构建 request → `nand_fc_start` → 置相位"在 4 处逐字重复，读不懂、改不动。

### 改动

| 文件 | 变更 | 说明 |
|------|------|------|
| `src/nand/nand_dispatch.c` | 新增 3 个结果枚举 + 1 个 ctx 结构体 + 11 个 `static` 辅助函数 | poll 压到 71 行（只留两阶段循环 + 返回值分派） |
| `src/nand/nand_dispatch_internal.h` | `nand_dispatch_lun_chain_t` 注释补 `count` 双语义说明 | 见下方"既有隐患显式化" |
| `docs/design/fc_nand_design.md` | 图 9 改为分层表 + 带函数名的流程图 | 企业模块 ↔ helper 名一一对应 |

**纯重构，零行为变化**。不新建文件——`g_nand_dispatch_chains` / `g_nand_channel_queues` / `g_nand_channel_cursor` 都是本文件 `static`，拆到新 `.c` 要把它们改成非 static + 加 extern，风险大于收益。

### 分层映射

| 企业模块 | 职责 | 拆到哪个函数 | 原行号 |
|---|---|---|---|
| **Command issue** | 构造请求 + 发起事务 | `nand_dispatch_task_build_request` / `task_fc_start` / `task_start` | 4 处去重 |
| **Arbiter / Scheduler** | 选 (ch,ce,lun)，轮转 | `nand_dispatch_queue_consume_match` / `phase2_dequeue` | :459, :622 |
| **Per-die FSM** | 单 die 状态推进 | `nand_dispatch_issued_advance` / `lun_drive` / `chain_advance` / `phase1_lun` | :382, :426, :579, :366 |
| **Data path** | CW 链式续读、页缓冲拆页 | `nand_dispatch_task_cw_chain` / `scatter_page_buffer` | :498, :536 |
| **Completion Aggregator** | 每 CW 状态 → 命令级 | `nand_dispatch_tables.c`（已独立，未动） | — |

Sequencer（`g_nand_operation_map[].uops` + `nand_engine.c`）已经独立，本次不碰。

### 三个结果枚举（拆分的语义骨架）

```c
/* Per-die FSM 单步推进结果 */
NAND_DISPATCH_LUN_WAIT      /* 本轮无进展，跳到下一 LUN */
NAND_DISPATCH_LUN_CONTINUE  /* 链已推进到下一命令，继续内层 while */
NAND_DISPATCH_LUN_DONE      /* 拿到可返回的 completion */
NAND_DISPATCH_LUN_ERROR     /* req 构建失败，调用方须 abort poll */

/* 命令发起结果——必须把 req_table 失败与 nand_fc_start 自身错误码分开 */
NAND_DISPATCH_START_REQ_ERROR  /* req_table 构建失败 → abort poll */
NAND_DISPATCH_START_ASYNC      /* nand_fc_start 返回 NOT_READY，待下轮 step */
NAND_DISPATCH_START_SYNC       /* 同步完成或返回错误码，*fc_result 有效 */

/* CW 链式续读结果（无 ERROR 出口：链内 start 错误仍走完成路径） */
NAND_DISPATCH_CW_ASYNC / NAND_DISPATCH_CW_DONE
```

### 拆分中必须原样保留的语义陷阱

1. **`req_table` 失败 ≠ `nand_fc_start` 失败**。前者致命（清 `count` + poll 立即返回 `INVALID_ARGUMENT`）；后者是"带错误状态的完成"，继续走完成处理链。这是 `start_outcome_t` 存在的唯一理由。
2. **`CONTINUE` 出口不能省**。链推进后 `continue`（继续 while）与启动异步后 `break`（离开 while）必须用不同枚举值，否则要么死循环要么提前退出。
3. **ISSUE→ISSUED 转换归 `lun_drive`**，不属于 cw_chain。放错会让 ISSUE 段继续跑 CW 链。
4. **Phase 2 日志点位置**：`SSD_LOGD` 必须夹在 `req_table` 与 `nand_fc_start` 之间，所以单独拆出 `task_build_request`。
5. **`nand_completion` 是每 LUN 迭代的局部量**，`active==0` 分支读它依赖同轮先写。ctx 必须每 LUN 重新构造。
6. **两处 DONE 不对称**：Phase 1 链尾重置 `phase=IDLE` + 更新 `g_nand_channel_cursor`；Phase 2 单命令立即完成**都不做**。不能合并成同一个"结束链" helper。
7. **abort 时只清 `chain->count`**，不碰 `phase`/`cursor`/`started`/队列。
8. **ctx 不缓存 `task` 指针**——`chain->cursor++` 后旧指针失效，一律由 `&chain->tasks[chain->cursor]` 现取。

### 既有隐患显式化（只加注释，逻辑未动）

`chain->count` 双语义：既是链深，也是 LUN 忙标志（poll 没有独立 busy 位）。存在一条"命令已离开队列、尚未完成"的中间态——Phase 2 出队后若 `nand_fc_start` 返回异步，队列 entry 已被消费（`queue->count--`），但 `count` 仍 >0、`tasks[0].started` 已置 1，命令不在任何队列里，靠 Phase 1 下一轮按 `started`/`transaction->active` 接管推进。

在 `nand_dispatch_internal.h` 的结构体注释里写为正典说明，`phase2_dequeue` 注释指向它。**"队列空 + count>0"是正常中间态，不是命令丢失。**

### 验证

- **机械等价性比对**：原 poll（348-696 行）与新 11 个 helper（381-931 行）去注释、归一化空白后做多重集比对，并逐项数关键副作用出现次数——`chain->count = 0U` 6=6、`task_complete` 3=3、`nand_fc_start` 6=6、`delay_dwt` 3=3、`req_table` 调用点 4=4、`NOT_READY` 判断 7=7。无一条语句凭空消失。
- **出口逐条映射**：原函数 15 个出口（含 4 个 abort、6 个 WAIT、2 个 DONE、3 个 Phase 2 `continue`）全部有对应落点。
- **终止性**：`phase1_lun` 的 `while(1)` 唯一 `continue` 在 `chain_advance` 之后，必 `cursor++`，`count ≤ NAND_DISPATCH_CHAIN_DEPTH(4)`，最多 4 圈必达链尾。
- **编译**：`make` / `make tests` 均零警告零错误；二进制 86936 字节，与拆分前一致。
- **上板回归**：待用户执行（重点：单命令读写、多 CW 普通读、多 plane 读、擦除 ISSUE→ISSUED + `min_busy_us`、Read Retry 链）。当前无 nand_dispatch 专项 UT，FC 测试经 `fc_submit → nand_dispatch_poll` 间接覆盖。

---

## Phase T: 引擎上下文三指针上收 — nand_engine_start 6 参 → 3 参

**日期**: 2026-10-04

### 背景

`nand_fc_start` 里这行调用有 6 个参数，读起来费劲：

```c
result = nand_engine_start(&transaction->engine, transaction->backend, map->uops, &map->timing,
                           &transaction->request, &transaction->completion);
```

根因不在"参数多"，而在**状态存了两份**：`nand_fc_transaction_t` 内嵌了一个 `nand_engine_state_t`，却又在自己身上平铺了 `backend` / `active` 两个字段，与 `engine.backend` / `engine.active` 完全重复，全靠手工同步（`nand_fc_start` 里 `transaction->active = 1U`、`task_load` 里 `transaction->backend = task->backend`）。6 个参数里有 4 个是"把重复字段再传回引擎"。

### 改动

| 文件 | 变更 |
|------|------|
| `include/nand/nand_fc_interface.h` | 删 `nand_fc_transaction_t.backend` / `.active` 两个重复字段 |
| `include/nand/nand_engine.h` | `nand_engine_start(state, backend, uops, timing, request, completion)` → `(state, uops, timing)` |
| `src/nand/nand_engine.c` | `start` 不再 `*state = {0}` 整体清零，改为逐个复位游标/超时/active，保留已绑定的 backend/request/completion；`nand_engine_execute` 改用指定初始化器自行绑定三指针 |
| `src/nand/nand_mt29f4g08abadawp.c` | `nand_fc_start` / `nand_fc_step` 的 `transaction->active` → `transaction->engine.active`；`transaction->backend` → `transaction->engine.backend` |
| `src/nand/nand_dispatch.c` | `nand_dispatch_task_load` 一次性绑定 `engine.backend/request/completion`（地址随事务固定，只绑一次） |
| `docs/design/fc_nand_design.md` | 结构体定义、流程图、多通道差距表同步 |

### 设计要点

- **指针归属**：`engine.request` / `engine.completion` 指向 transaction 自己的两个存储体，`engine.backend` 指向 per-LUN 后端。三者在 `nand_dispatch_task_load` 绑定一次，地址随 task slot 固定，之后 `start` 只管复位游标。
- **清零语义必须改**：原实现 `*state = (nand_engine_state_t){0}` 会把调用方刚绑好的三指针一起清掉，所以 start 必须先接后清。现在改为**显式逐个复位** 8 个游标/超时/active 字段——与原整体清零等价（原清零的 11 个字段里，另 3 个正是保留项）。
- **`active` 兜底清除保留**：`nand_engine_step` 的 6 条返回路径里有 3 条（入参校验 / 非法 UOP 码 / handler 缺失）不清 `active`，故 `nand_fc_start` 在 `result != NOT_READY` 时统一清一次——把原来的 if/else-if 两支合并成单支，语义不变（OK 时重复清一次无副作用）。
- **`nand_engine_execute`（@deprecated，仅 nand_init 用）** 用指定初始化器补上三指针，其余字段由 C 的零初始化补齐，行为与原来一致。

### 验证

- `make` / `make tests` 零警告零错误；固件二进制 86936 → **86904 字节**（-32，去掉重复字段的赋值与存储）。
- 行为等价性：三处 API 变更均为"把已存在的状态换个存放位置"，未改任何判定条件；`active` 的读写点一一对应平移。
- **上板回归待执行**：本次动到 `active` 的语义归属，上板时重点看 `nand_fc_step` 的 active 判定路径（单命令读/写、擦除 ISSUE→ISSUED、多 CW 读、Read Retry 链）。

---

## Phase U: nand_engine_step 忙等/日志装饰抽 helper

**日期**: 2026-10-04

### 背景

`nand_engine_step()` 的**主分派本来就是表驱动**（`state->backend->handlers[uop->type]`，14 个 UOP 类型一张表）。"判断太多"的观感来自另外三类混在循环体里的东西：

| 分支 | 性质 | 处理 |
|------|------|------|
| `END` → 终止循环返回状态 | 引擎控制流 | **保留**（表项是"干活"的，这个是"结束"） |
| `type >= NAND_UOP_COUNT` | 边界检查 | 保留 |
| `JUMP_IF_NOT_READY` → 改 `uop_index` | 引擎控制流 | 保留 |
| `handler == NULL` | 防御性检查 | 保留（给非 FMC 后端留的） |
| WAIT_READY 调前锁存 DWT 起点 | 类型特化装饰 | → `wait_latch()` |
| OK 时的日志白名单 | 类型特化装饰 | → `uop_log_ok()` |
| NOT_READY 时 WAIT_READY 超时 | 类型特化装饰 | → `wait_timeout()` |

前四行是解释器骨架，换任何写法都消不掉——除非让 handler 返回控制码（CONTINUE/JUMP/TERMINATE），但那只是把 switch 从"按类型"挪到"按返回码"，分支数不变，还把控制流藏进间接调用里，上板调试更难。

**未采用方案**：再加一张 `g_nand_uop_attrs[NAND_UOP_COUNT]` 属性位表。三个特化只涉及 2 个类型（WAIT_READY 独占 2 个），且属性表**漏填 = 静默无标志**（handler 表漏填会被 `handler == NULL` 当场抓住）——把"编译期/上电即查的错"换成"上板才炸的错"，不划算。

### 改动

`src/nand/nand_engine.c` 新增 3 个 `static`，`nand_engine_step` 循环体只剩骨架：

| helper | 职责 | 消掉的重复 |
|--------|------|-----------|
| `nand_engine_wait_latch(state)` | 一次性锁存 DWT 起点 + `wait_polls` 清零；已锁存则直接 OK | JUMP 与 WAIT_READY 两处**逐字重复的 8 行** |
| `nand_engine_wait_timeout(state, op_name)` | `wait_polls` 饱和 +1 后判 DWT/计数双条件；超时清 `active` | 两处几乎相同的双条件超时块（原只差日志文案） |
| `nand_engine_uop_log_ok(state)` | 按类型白名单打 OK 日志 | 循环体内的 11 行白名单 |

顺带修掉一处**死条件**：原白名单里的 `NAND_UOP_END` 永远不成立——`END` 在进入 handler 之前就无条件 `return` 了，到不了成功分支。

### 设计要点

- **零语义变化**：三处全是纯提取，判定条件、执行顺序、副作用一一对应。`wait_timeout` 的 `op_name` 参数只为日志文案，拼出的字符串与原来逐字相同（`"nand: JUMP_IF_NOT_READY timeout ..."` / `"nand: WAIT_READY timeout ..."`）。
- **锁存顺序保持**：`wait_latch`（可能清 `wait_polls`）必须在 `wait_timeout`（递增 `wait_polls`）之前，两处调用点顺序一致。
- **`active` 兜底清除保留**：`wait_latch` 失败时清 `active` 后返回 `INVALID_ARGUMENT`，与原两处一致。

### 验证

- `make` / `make tests` 零警告零错误。
- 固件二进制 86904 → **86840 字节**（-64，重复的锁存/超时块合并）。
- **上板回归待执行**：本次动到忙等超时路径，重点看擦除/编程的 `WAIT_READY` 超时判定与 70h 轮询循环（`JUMP_IF_NOT_READY`）。

---

## Phase W: nand_fmc.c 长签名前向声明清理

**日期**: 2026-10-04

> 跳号说明：字母 V 已被代码里的 **"Phase V"（多 LUN 并发）** 概念占用（`nand_dispatch_internal.h` / `nand_fmc.h` / `fc.c` 等 10+ 处注释），故本节用 W，避免与既有概念撞名。

### 背景

`nand_fmc.c` 里 12 个 UOP handler 共用同一个 `nand_uop_handler_t` 契约（5 个参数）。其中两个纯外观瑕疵：

1. **12 条前向声明**（原 `:35-46`）只是为了在 `:56` 的调度表里引用它们，每条都是 198 字符的长签名，还要挂 `__attribute__((used))`。
2. **`row_0` / `row_1` / `row_2` 里的 `(void)uop;` 是死语句**——这三个函数紧接着就用 `uop->operand` 当 plane 下标。

### 改动

| 位置 | 变更 |
|------|------|
| 文件开头 | 删 12 条 handler 前向声明（含 `__attribute__((used))`），换成 **1 条调度表前向声明** |
| 文件末尾 | `g_nand_fmc_handlers` 表定义从开头移到最后一个 handler（`nand_fmc_exec_delay_us`）之后 |
| `row_0/1/2` | 删 3 行死的 `(void)uop;` |

净减 **11 条 198 字符长签名**。

### 设计要点

- **为什么可以后置定义**：表定义在 handler 之后，handler 的**定义即声明**，编译器此时已见过全部 12 个符号，不再需要前向声明；也因此不需要 `__attribute__((used))`（符号被表引用，本来就不会判"未使用"）。
- **为什么开头还留一条表前向声明**：`nand_fmc_init_instance()`（`inst->backend.handlers = g_nand_fmc_handlers;`）在 handler 定义之前就要用这张表，所以表本身仍需前置声明。这是 `static const` 的**暂定定义**（tentative definition）+ 后置带初始化器的定义，C 标准允许；已确认在项目全部警告开关（含 `-Wredundant-decls`）下零警告。
- **表长度不变**：`nm` 实测符号 `g_nand_fmc_handlers` = **0x38 = 56 字节 = 14 × 4**，与枚举 `NAND_UOP_COUNT` 一致，`[JUMP_IF_NOT_READY] = NULL` 保留。

### 验证

- `make` / `make tests` 零警告零错误。
- 固件二进制 **86840 字节，与改动前完全一致**——纯搬运 + 删死语句，零代码变化。
- 反汇编抽查：`nand_fmc_exec_row_0` 机器码逐字节不变（`ldr r3,[r0,#8] / ldr r1,[r1,#4] / add.w r2,r2,r1,lsl#2 / ldrb r2,[r2,#4] / strb r2,[r3,#0] / movs r0,#0 / bx lr`）。
- 顺带补上文件末尾缺失的换行符。

---

## Phase X: fc.c 按职责重排 + fc_poll_internal 拆分

**日期**: 2026-10-04

### 背景

`src/fc/fc.c` 1520 行，函数按"写入时间"而非"职责"排列，且 `fc_poll_internal` 单函数 **425 行**。两个具体问题：

1. **顺序混乱**
   - `#include <stddef.h>/<string.h>` 落在第 86-87 行——夹在 `fc_is_page_write` 和静态资源区之间，两个函数之后。
   - `fc_command_id_is_valid` 孤悬在 ID→指针转换器中间；getter 簇楔在静态数据区与 ID 转换器之间。
   - `fc_command_release_internal_api`（`:1496`）与其同类 `fc_command_resources_rollback`（`:459`）相隔 1000 行。
   - `fc_complete_internal`（`:1457`）定义在 `fc_complete_from_nand`（`:652`）**之后 800 行**，靠 `:37-38` 一条前向声明兜着。
2. **poll 过长**：单命令路径（`:1076-1279`）与链式路径（`:1280-1447`）各有一份**近似重复**的 request 构建（约 130 行 ×2），外加 6 处逐字重复的"合成失败完成项"。

### 改动

| 位置 | 变更 |
|------|------|
| 文件开头 | `#include <stddef.h>/<string.h>` 上移并入 include 块；删 `fc_complete_internal` 前向声明；补 10 个分区 banner + 文件结构说明 |
| 全文 | 按 10 个职责分区重排：静态资源 / 查询转换工具 / 初始化 / 资源分配回滚 / 命令配置 / 提交 / 完成 / Stripe RAID / 调度轮询 / CQ 出口。**定义顺序 = 依赖顺序**，全文件零前向声明 |
| `fc_poll_internal` | 425 行 → **~60 行骨架**；两条提交路径拆成 `fc_poll_dispatch_single()` / `fc_poll_dispatch_chain()` |
| 新增 static | `fc_build_dispatch_request()`（合并两份重复的 request 构建）、`fc_fill_cw_meta_paa()`、`fc_poll_fail_command()`（6 处失败兜底合一） |
| 寄存器刷新 | `fc_register_queue_state()` **保持逐点调用**（13 处，与改动前一一对应），未收敛 |
| 文档同步 | `docs/design/fc_nand_design.md`：图 2 重绘（5 个 helper 名 + META/PAA 不对称注记）、§16.3 链式示意图改为经 `fc_poll_dispatch_chain()`/`fc_build_dispatch_request()`、§2 文件表与 §17.9 splittable 说明的函数归属改为 `fc_build_dispatch_request()`、附录 A 图 2 锚点补 helper 名 |

### 设计要点

- **零行为变化**：脚本核对（花括号配对抽函数体逐字节比对）确认 31 个既有函数体**逐字节不变**，唯一被改写的是 `fc_poll_internal` 本身。
- **删掉前向声明的办法**：把 `fc_complete_internal` 前移到 `fc_complete_from_nand` 之前。全文件按依赖 DAG 排序后不再需要任何前向声明。
- **`fc_register_queue_state` 未收敛的原因（自查后回退）**：初版把散落的调用收敛到骨架末尾，理由是它是纯状态快照、幂等。复查发现该推理有漏洞——`fc_scheduler_select_chain()` 成功时会**从 HSQ/SQ 弹出**已选中的命令，于是"命令 id 非法/状态非 QUEUED"的早退路径上，队列计数已被弹掉而寄存器尚未刷新。改动前这些路径**不刷新**寄存器（保持陈旧值），收敛版却会刷新，属于可观测行为差异。故回退为逐点调用，位置与改动前严格一致，并在两处早退点加注释 `与拆分前一致：此校验失败早退，不刷新寄存器`。
- **splittable→ISSUE 转换收进 `fc_build_dispatch_request` 末尾**：原单命令路径在 cws 填充之后做转换，链式路径在 cws 填充**之前**做；但两处的 cws 分支读的都是 `command->config.operation` 而非 `request.operation`，故转换位置对结果无影响，可安全统一到 helper 末尾。
- **保留的 4 条行为不对称**（拆 helper 时未"顺手统一"）：① META/PAA 预填只在单命令路径；② 链式 desc 链断裂时从 `for` 循环体内直接返回、放弃整条链；③ 链式 submit 失败对整链逐个注失败完成项、不提前返回；④ `sched_chain.count == 0` 直接返回且不刷寄存器。

### 验证

- `make` / `make tests` 零警告零错误。
- 固件二进制 **86840 → 85960 字节**：两份重复的 request 构建被 GCC 保留为**单个 out-of-line 函数**，净省 ~880 字节（纯去重带来的体积收益）。
- 函数体逐字节比对：31 个既有函数全部与改动前一致（`fc_poll_internal` 除外），新增 5 个 static helper。
- `fc_register_queue_state()` 调用点计数：改动前后均为 **13**（逐点对齐的机器核对）。
- `clang-format-15 -i src/fc/fc.c` 已跑，diff 只含结构性移动。
- **上板回归**：`make tests` 烧写 → UART CLI `test` → `ut_run_all()`；重点覆盖单命令读/写（META/PAA 预填）、多 plane 读写、擦除、Read Retry 链（`submit_chain`）、RAID encode（`fc_stripe_submit`）。

---

## Phase Y: 初始化 / 运行时全链路走读 + 源码关键路径注释补全

**日期**: 2026-10-04

### 背景

作者反馈 fc-nand 层代码"还是有点看不懂"，希望注释更详细并补流程图解，尤其是**初始化**和**运行时**两条主线。体检发现三个缺口：

1. **文档没有初始化图**：`fc_nand_design.md` 已有图 0–13，但**没有一张讲"上电后按什么顺序把 FC/NAND/FTL 立起来"**；初始化只散落在 §2.9 的 API 表与 §19.5 的 `nand_init` 代码块里。
2. **运行时缺贯通主线**：图 1/2/9/10/11/13 都是单点视角，没有"从 `main()` 到一条命令跑完"的端到端视图。
3. **一个会误导读者的事实**：§16 + 图 11 花大篇幅讲"命令链执行"，但 FTL 运行时**从不使用命令链**——所有命令 `config.chain = FC_CHAIN_END`（`src/ftl/` 共 23 处），运行时 `count` 恒为 1，只有 `fc_poll_dispatch_single` 是活路径；Read Retry 也是三条独立同步等待命令、非原子 `submit_chain`。
4. **源码注释**：文件头/函数 doc-block 质量尚可，但**函数体内关键跳转点注释稀疏**；`main.c` 甚至没有文件头块。

### 改动

| 文件 | 变更 |
|------|------|
| `docs/design/fc_nand_design.md` | 新增 **§22 初始化全流程**（图 14 分层调用时序 / 图 15 资源装配 / 图 16 NAND 芯片初始化 + 三条硬约束表 + 上电走读 + 编译期开关表）；新增 **§23 运行时全流程**（图 17 端到端生命周期 / 图 18 命令生命周期状态机 / 图 19 `fc_service` 单步全景 / 图 20 相位机状态机 / 图 21 多 LUN 时序波形 / 图 22 调度器取命令决策 + 运行时走读 + §23.9 澄清不走链）；头部图数 14 → **23**（图 0–22）并登记 Phase Y；附录 A 增 图 14–22 共 9 行锚点；附录 B 阅读顺序 5 → **7 步**（新增第六步初始化 / 第七步运行时） |
| `docs/design/fc_nand_design.md` §16 | §16 开头加「运行时未启用命令链」警示框并指向 §23.9；§16.9 加历史示例与实现不符的警示（Read Retry 实为三条独立同步命令） |
| `src/main.c` | 新增文件头块（职责/调用链/关键约定/生命周期）；`main()` 初始化级联补「全系统初始化顺序 + else-if 级联会整段跳过」注释；`ssd_ftl_boot_all` 补 ①–⑤ 步骤标记；superloop 补「无 ISR 参与、驱动源唯一」注释 |
| `src/fc/fc.c` | `fc_init_internal` 函数体补 ①–⑧ 编号块注释（三池 memset / 指针挂载 / 队列 / 调度器 / `nand_dispatch_init` 唯一 NAND 依赖 / CRC / 空闲链 `0→1→…→N→INVALID` 图 / 末尾寄存器快照）；`fc_poll_internal` doc-block 修正「寄存器统一刷新」的过时表述为**逐点刷新**，并加「FTL 恒 chain=END → 只有 single 是活路径」注；`fc_poll_dispatch_single`/`_chain` 各加一行活路径/仅单测注 |
| `src/nand/nand_mt29f4g08abadawp.c` | `nand_init` 补 Step 1–8 分段标记（FMC 句柄 / HAL+Msp / EXTI / per-LUN 后端 / DMA / RESET / READ_ID / Mode 4），并注明 **RESET/READ_ID 绕过 dispatch 直连 engine** |
| `src/nand/nand_dispatch.c` | 文件头相位机块补「写/读点总表见 internal.h + 本文件内 `── 相位 X → Y ──` 标记」说明；`lun_drive` / `phase1_lun` / `phase2_dequeue` 的相位分支加 `── 相位 ISSUE → ISSUED ──`、`── 相位 ISSUED 预检 ──`、`── 驱动循环 ──`、`── 整链收尾 ──` 标记；`nand_dispatch_init` 补调用链注 |

### 设计要点

- **纯注释 / 纯文档，零逻辑改动**：不触碰任何控制流、数据结构与头文件声明。
- **遵守既有约定：源码内不放框图**。源码只用 `→` 箭头链与 `── 标题 ──` / `── 相位 X → Y ──` 标记；方框 ASCII 图只进设计文档。已核实 `src/fc/*.c`、`src/nand/*.c`、`src/main.c` 改动前后均无 `┌│└` 方框字符。
- **图号与既有图的关系**：图 14–22 为**新增**，不改动图 0–13 的编号；图 19 扩展图 2、图 20 扩展图 10、图 21 取代图 10 末尾的时序简例，均在正文显式标注"扩展/配套"关系，不重复画既有内容。
- **单 LUN 事实必须标注**：图 21 在标题与图内两处声明 `CH_NUM = CE_NUM = LUN_NUM = 1`、相位机当前退化为单 LUN 顺序执行，避免读者误以为并发已实测。
- **行号漂移防护**：§22/§23 正文以**函数名**为主定位，行号仅辅助，并在章首注明"行号随重构漂移、以函数名为准"；附录 A 一律给 grep 锚点而非裸行号。

### 验证

- `make` / `make tests` 零警告零错误。
- 固件二进制 **85960 字节**（与 Phase X 完全一致）——纯注释改动不改变编译产物。
- `make format-check`（clang-format-15）通过。
- 文档自检：图号连续无重复（图 0–22 共 23 张）、头部计数与实际图数一致、附录 A 共 23 行且新增 9 条锚点 `grep` 实测均命中、附录 B 七步顺序与图号一致。
- **上板回归（待执行）**：`make tests` 烧写 → UART CLI `test` → `ut_run_all()`，确认注释改动无副作用。
