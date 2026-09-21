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

## 参考文档

- 接口使用指南：👉 [fc_api_usage.md](../design/fc_api_usage.md)
- 设计方案（含 §13 贯穿分析）：👉 [fc_nand_design.md](../design/fc_nand_design.md#13-控制状态位贯穿分析与-dispatch-边界设计)