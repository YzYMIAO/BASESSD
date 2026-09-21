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
