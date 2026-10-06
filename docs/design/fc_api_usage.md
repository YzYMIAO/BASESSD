# FC 模块接口使用指南

**日期**: 2026-09-21

---

## FC 对外接口一览

FC（Flash Controller）模块对外暴露以下 16 个公接口（声明于 `include/fc/fc.h`）：

| 接口 | 类型 | 说明 |
|---|---|---|
| `fc_init()` | 初始化 | 启动时调用一次，初始化 Pool、SQ/HSQ/CQ、调度器 |
| `fc_command_alloc()` | 资源分配 | 从 command pool 分配一个 `fc_command_t` |
| `fc_command_configure_descriptors(cmd)` | 资源分配 | 根据 cmd 的 cw 数量自动分配 descriptor 链 + buffer；SET/GET_FEATURES 操作跳过此步（见 §SET/GET_FEATURES 特殊流程） |
| `fc_buffer_alloc()` | 资源分配 | 手动从 buffer pool 分配一个 buffer |
| `fc_write_cmd_data(cmd, data, length)` | 数据填充 | 将外部数据一次性写入命令关联的所有 desc buffer（内部遍历 desc 链） |
| `fc_get_cmd_data_ptr(cmd, cw_index)` | 数据读取 | 获取指定 CW 索引的 buffer 指针（读操作后使用） |
| `fc_submit(cmd)` | 命令下发 | 提交到 SQ 或 HSQ（取决于 `cmd->high_priority`） |
| `fc_service()` | 驱动调度 | 主循环中持续调用，调度并执行命令，提交给 NAND |
| `fc_poll_completion(&completion)` | 状态查询 | 从 CQ 取出已完成命令的状态 |
| `fc_command_release(cmd)` | 资源回收 | 释放 command 及其关联的 descriptor 链 |
| `fc_buffer_release(buffer_id)` | 资源回收 | 手动释放 buffer |
| `fc_command_id_to_command(id)` | ID 查找 | command_id → `fc_command_t*` |
| `fc_descriptor_id_to_descriptor(id)` | ID 查找 | descriptor_id → `fc_descriptor_t*` |
| `fc_buffer_id_to_real_buffer(id)` | ID 查找 | buffer_id → 真实 buffer 指针 |
| `fc_real_buffer_to_buffer_id(ptr)` | ID 查找 | 真实 buffer 指针 → buffer_id |
| `fc_get_resource_stats(&stats)` | 监控 | 查看 pool/queue 使用率 |

---

## 设计原则

### 信任边界

```
┌─────────────────────────────────────────────────────────┐
│  上层模块（FTL / 测试代码）                               │
│                                                          │
│  传入的参数在此验证（NULL、状态、归属、ownership）          │
│  ─────────────────── 信任边界 ───────────────────          │
│  内部函数假定输入全部合法，不再重复验证                      │
│                                                          │
│  controller 全局只有一个实例（g_fc_controller），          │
│  内部函数无需检查 controller == NULL                       │
└─────────────────────────────────────────────────────────┘
```

### 参数验证位置

所有参数有效性检查集中在公接口（`fc.h` 中声明的函数），内部 `_internal` 函数假定输入合法：

| 公接口 | 验证项 |
|---|---|
| `fc_command_configure_descriptors()` | `command==NULL`、不在 pool 中、state 不是 ALLOCATED、已配置过 des |
| `fc_buffer_release()` | buffer_id 不在范围内、buffer 未分配 |
| `fc_buffer_id_to_real_buffer()` | buffer_id 不在范围内、buffer 未分配 |
| `fc_real_buffer_to_buffer_id()` | `buffer==NULL` |
| `fc_write_cmd_data()` | `command==NULL`、`data==NULL`、不在 pool 中、state 不是 ALLOCATED、无 desc 链 |
| `fc_get_cmd_data_ptr()` | `command==NULL`、不在 pool 中、无 desc 链、cw_index 超过 CW 范围 |
| `fc_submit()` | `command==NULL`、不在 pool 中、state 不是 ALLOCATED、`fc_command_validate()` 失败、descriptor 所有权不完整 |
| `fc_poll_completion()` | `completion==NULL` |
| `fc_command_release()` | `command==NULL`、不在 pool 中、state 不是 ALLOCATED/COMPLETED、仍在 CQ 中未取出 |
| `fc_command_id_to_command()` | command_id 超范围 |
| `fc_descriptor_id_to_descriptor()` | descriptor_id 超范围 |
| `fc_get_resource_stats()` | `stats==NULL` |

### 内部函数优化

4 个内部函数改用指针算术 `command - controller->command_pool` 直接计算 command_id，替代 `fc_command_to_id()` 的线性查找：
- `fc_descriptor_alloc_internal()`
- `fc_command_configure_descriptors_internal()`
- `fc_submit_to_queue()`
- `fc_command_release_internal_api()`

---

## 完成通知与状态读取

### 完成状态结构（fc_command_status_t）

CQ 条目包含命令 ID、module（直接路由）、per-CW ECC 纠正比特数和 per-CW 状态：

```c
struct fc_command_status_t {
    DWORD command_id     : 16;
    DWORD desc_id        : 16;  // 出错时更新为第一个失败的 desc
    DWORD module         : 4;   // 发起者模块，直接路由用
    DWORD ecc_bit0       : 8;   // CW0 ECC 纠正比特数
    DWORD ecc_bit1       : 8;   // CW1
    DWORD ecc_bit2       : 8;   // CW2
    DWORD ecc_bit3       : 8;   // CW3
    DWORD status         : 8;   // 命令级聚合状态（各 CW 的 FC_CW_STATUS_* 位 OR）
    DWORD failed_cw_mask : 4;   // bit n = CW n 解码失败（UNC / CRC_ERROR）；0 = 全通过
    DWORD reserved       : 16;
};
```

`failed_cw_mask` 按 **CW 号**（不是 desc 链走位）置位，因此配合 `fc_command_t.cw_mask` 做非满 mask 读时，
可以直接定位是哪几个 CW 失败、无需再查 desc 链。仅 page read 填充，其余命令恒 0。

### 读命令的 CW 子集选择（fc_command_t.cw_mask）

`fc_command_t` 新增 `cw_mask : 4`（bit n = CW n），用于只读一页中**任意 CW 子集**（读恢复场景：ECC 只挂了
2~3 个 CW 时只重读失败的那几个，不必重读整页）：

```c
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
cmd->config.cw        = 3U;      // 满页 4 个 CW（mask=0 时按此字段取连续 0..cw）
cmd->cw_mask          = 0b1011U; // 只读 CW0 / CW1 / CW3
// configure 时按 cw_mask 置位建 desc 链，并把 config.cw 回写为"置位数减一"（此处 2）
```

`cw_mask = 0` 表示未指定，退化为"连续 `0..config.cw`"——所有既有调用点都是这个取值，行为不变。
该字段**仅读类命令有效**；写路径仍按连续 CW 处理。

**不在本次范围**：FTL 读恢复流程（`src/ftl/read_recovery.c`）对 `failed_cw_mask` / `cw_mask` 的接线。

### 使用 module 直接路由完成通知

```c
while (1) {
    fc_service();

    fc_command_status_t completion;
    if (fc_poll_completion(&completion) == FC_STATUS_OK) {
        // 直接按 module 路由，无需 command_id 查询
        switch ((fc_command_module_t)completion.module) {
            case FC_COMMAND_MODULE_HOST_IO:
                host_io_complete(&completion);
                break;
            case FC_COMMAND_MODULE_GC:
                gc_complete(&completion);
                break;
            case FC_COMMAND_MODULE_READ_RECOVERY:
                recovery_complete(&completion);
                break;
            default:
                ftl_complete(&completion);
                break;
        }

        fc_command_t *done = fc_command_id_to_command(completion.command_id);
        fc_command_release(done);
    }
}
```

### 读取 ECC 纠正比特数

```c
// 从 CQ 读取（快照方式）
uint8_t cw0_ecc = completion.ecc_bit0;  // CW0 的 ECC 纠正比特数
uint8_t max_ecc = MAX(completion.ecc_bit0, completion.ecc_bit1);

// 从 descriptor 读取（持久化方式——适合错误恢复）
fc_command_t *done = fc_command_id_to_command(completion.command_id);
fc_descriptor_id_t did = done->desc_id;
while (did != FC_DESCRIPTOR_ID_INVALID) {
    fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(did);
    if (desc->ecc_bits > ECC_THRESHOLD) {
        // 触发 Read Retry 或磨损均衡
        trigger_read_retry(did);
    }
    did = desc->next_id;
}
```

### 使用 descriptor 回写状态进行错误恢复

当 CQ 通知某命令失败时，FTL 可以直接从 desc 链获取每 CW 的详细失败原因，无需依赖 CQ（CQ 条目可能已被后续完成覆盖）：

```c
void recover_failed_command(fc_command_t *cmd) {
    fc_descriptor_id_t did = cmd->desc_id;
    uint8_t cw = 0;
    while (did != FC_DESCRIPTOR_ID_INVALID) {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(did);
        if (desc->status & FC_CW_STATUS_NAND_FAIL) {
            // 对此 CW 执行 Read Retry
            retry_read(did);
        } else if (desc->status & FC_CW_STATUS_UNC) {
            // 启动 RAID 恢复
            raid_recover(cmd, cw);
        } else if (desc->ecc_bits > 0) {
            // 虽然成功但 ECC 偏高——记录 BER
            update_ber_tracking(did, desc->ecc_bits);
        }
        did = desc->next_id;
        cw++;
    }
}
```

---

## 标准使用流程

### 初始化

```c
fc_init();
```

### 执行一条命令（写操作为例）

```c
// ① 分配 command
fc_command_t *cmd = fc_command_alloc();

// ② 填充命令字段
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.ch        = 0;
cmd->config.ce        = 0;
cmd->config.lun       = 0;
cmd->config.cw        = 1;   // CW 数量 - 1（此处表示 2 个 CW）
cmd->paa              = target_paa;

// ③ 自动分配 descriptor 链 + buffer
fc_command_configure_descriptors(cmd);

// ④ 写入数据（FC 内部遍历 desc 链，buffer_id → pointer → memcpy）
fc_write_cmd_data(cmd, source_data, (cmd->config.cw + 1) * CW_SIZE);

// ⑤ 提交到硬件队列
fc_submit(cmd);
```

> **注意**：步骤④也可以还原为手动遍历 desc 链方式（详见下方的"标准使用流程—数据遍历"），`fc_write_cmd_data()` 是一个便捷封装。

### SET/GET_FEATURES 特殊流程

SET_FEATURES 和 GET_FEATURES 属于小数据命令（1 字节寄存器地址 + 4 字节参数），不走 descriptor+buffer 分配，数据以内联方式传递：

```c
// ① 分配 command
fc_command_t *cmd = fc_command_alloc();

// ② 填充命令字段
cmd->config.operation = OP_SET_FEATURES;  // 或 OP_GET_FEATURES
cmd->config.ch        = 0;
cmd->config.ce        = 0;
cmd->config.lun       = 0;

// ③ 跳过 fc_command_configure_descriptors() — SET/GET_FEATURES 内联传递

// ④ 设置寄存器地址和数据（feature_addr = 寄存器地址，parameter[0..3] = 4 字节数据）
cmd->feature_addr = 0x01;      // 目标寄存器地址（例：Feature Register 0x01）
cmd->parameter0   = 0x40;      // 写：数据字节 0；读：完成后从此读取回读值
cmd->parameter1   = 0x00;      // 写：数据字节 1；读：完成后从此读取回读值
cmd->parameter2   = 0x00;      // 写：数据字节 2；读：完成后从此读取回读值
cmd->parameter3   = 0x00;      // 写：数据字节 3；读：完成后从此读取回读值

// ⑤ 提交到硬件队列
fc_submit(cmd);

// 完成后，GET_FEATURES 结果直接从 parameter0..3 读取
// 注意：无需 fc_command_configure_descriptors()，也无需 fc_command_release() 释放 desc/buffer
```

**关键区别**：

| 步骤 | 普通数据命令 | SET/GET_FEATURES |
|------|------------|-----------------|
| descriptor 分配 | `fc_command_configure_descriptors()` | 跳过（FC 内部自动识别） |
| 数据写入 | `fc_write_cmd_data()` | `cmd->parameter[0..3]` 直接赋值 |
| 数据读取（读操作） | `fc_get_cmd_data_ptr()` | `cmd->parameter[0..3]` 直接读取 |
| 资源释放 | `fc_command_release()` 自动释放 desc+buffer | `fc_command_release()` 仅释放 command |

> **参数类型说明**：`parameter0..3` 为 `uint8_t`（非位域），可取其地址（`&cmd->parameter0`）。位域不能取地址，此变更专门为了内联数据路径的 buffer 指针传递需求。

### 主循环轮询

```c
while (1) {
    fc_service();  // 调度并执行命令

    fc_command_status_t completion;
    if (fc_poll_completion(&completion) == FC_STATUS_OK) {
        // 处理完成的命令
        fc_command_t *done = fc_command_id_to_command(completion.command_id);
        // 如果是读操作，从这里读取数据...
        fc_command_release(done);
    }
}
```

### 读操作

读操作不需要预先填充 buffer，NAND 控制器会自动将数据写入 descriptor 对应的 buffer。完成后通过 `fc_get_cmd_data_ptr()` 获取数据指针：

```c
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_READ;
// ... 其他配置 ...
fc_command_configure_descriptors(cmd);
fc_submit(cmd);

// 完成后直接获取指定 CW 的 buffer 指针
fc_command_t *done = fc_command_id_to_command(completion.command_id);
uint8_t *buffer = fc_get_cmd_data_ptr(done, 0);  // CW0 的数据
// 从 buffer 读取数据...
```

---

## 与行业企业级设计的对比

### 当前设计（嵌入式 SOC）

- FC 内部通过静态 pool 管理所有资源（command / descriptor / buffer）
- Descriptor 关联到 FC 内部 buffer pool（`buffer_id`），buffer 位于 SRAM
- FTL 需要手动遍历 descriptor 链 → 拿到 buffer 指针 → memcpy 数据
- 适合：资源受限的嵌入式平台，无外挂 DDR

### 企业级设计（NVMe SSD）

- FC 不管理数据 buffer，只负责控制和 DMA
- FTL 在 DDR 中管理数据 buffer，FC 的 descriptor 里放 DDR 物理地址
- DMA 引擎直接从 DDR 读写，FC 内部的 buffer 只是透明的中间 FIFO
- NVMe 使用 PRP（Physical Region Page）或 SGL（Scatter-Gather List）描述数据分布
- FTL 不需要拷贝数据到 FC 内部 buffer

```
企业级数据流（写）
  DDR（FTL 管理的 buffer）
      │ PRP/SGL 描述地址
      ▼
  FC DMA 引擎 ──► FC 内部 FIFO ──► NAND Page

当前设计数据流（写）
  FTL memcpy ──► FC 内部 buffer pool（SRAM）──► NAND Page
```

---

## statawaiting_next_descriptor bug 修复

2026-09-20 重构中修复了 `fc_command_descriptors_owned()` 的一个 bug：

**修复前**：
```c
for (descriptor_count = 0U; descriptor_count < (controller, command); descriptor_count++)
```
逗号表达式 `(controller, command)` 的值是 `command` 指针，与整数 `descriptor_count` 比较永远是假，循环从不执行。

**修复后**：
```c
for (descriptor_count = 0U; descriptor_count < fc_command_descriptor_count(command); descriptor_count++)
```
使用正确的 descriptor 计数函数。

---
## 附录：数据 Buffer 方案设计讨论

关于 `buffer_id` 与 `data_ptr` 两种方案的完整讨论（包括企业级 SSD、消费级 SSD、DRAM-less SSD、UFS 的行业实践，以及 4 种方案对比与推荐选择），详见：

👉 **[fc_nand_design.md 第 12 章](./fc_nand_design.md#12-数据-buffer-方案设计讨论)**