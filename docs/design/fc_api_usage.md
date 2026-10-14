# FC 模块接口使用指南

**日期**: 2026-09-21（**v13 部分过时，见下方横幅**）

---

> ## ⚠ v13 变更横幅（2026-10-07）
>
> 本文件成文于 v13 之前，其中若干条目已不反映当前接口。**以
> `include/fc/fc_interface.h` + `docs/design/fc_nand_design.md` §2.9 / §26 为准。**
>
> v13 把 FC 的策略层上移 FTL（判据：硬件 FCC 寄存器表）：
>
> | 已删除（FC 侧） | 现在的等价物（FTL 侧） |
> |---|---|
> | `fc_command_configure_descriptors(cmd)` | `ftl_cmd_configure_descriptors(cmd, cw_bitmap)`（`include/ftl/ftl_fc_interface.h`；v28 起位图是形参） |
> | `fc_write_cmd_data()` / `fc_read_cmd_data()` | `ftl_cmd_write_data()` / `ftl_cmd_read_data()`（同上） |
>
> FC 侧建链现在**只有** `fc_command_configure_descriptor(cmd, plane, cw, row_paa)` 一个入口。
> 详见 `docs/changelog/fc_policy_layer_uplift.md`。
>
> **v17 补充**：建链入口由「`fc_command_attach_cw` → `_internal` → `_internal`」三层接力
> 收敛为**一条** `fc_command_configure_descriptor`（**单数**，实现在 `src/fc/fc_interface.c`），
> 原公共名 `fc_command_attach_cw` 与两个 `_internal` 均已删除。详见
> `docs/changelog/fc_build_chain_convergence_v17.md`。
>
> **v16 补充**：建链失败的回滚入口是 `fc_descriptor_chain_release_all(head)`
> （单个是 `fc_descriptor_chain_release_head(head)`）；`fc_command_release()` 现在
> **真的**会回收各 CW buffer（此前一直漏着）。详见 `docs/changelog/fc_chain_ownership_v16.md`。
>
> 本文件更早的过时项（未随版本同步）：`fc_real_buffer_to_buffer_id()` 已于 v10 删除；
> §内部函数优化 里的 `fc_command_configure_descriptors_internal` 已不存在。

---

## FC 对外接口一览

FC（Flash Controller）模块对外暴露以下 21 个公接口（声明于 `include/fc/fc_interface.h`）：

| 接口 | 类型 | 说明 |
|---|---|---|
| `fc_init()` | 初始化 | 启动时调用一次，初始化 Pool、SQ/HSQ/CQ、调度器 |
| `fc_command_alloc()` | 资源分配 | 从 command pool 分配一个 `fc_command_t` |
| `fc_command_configure_descriptor(cmd, plane, cw, row_paa)` | 资源分配 | **建链的唯一入口**（v17 由三层收敛而来）：分配一条 desc + buffer 并挂到链尾。上层按自己算出的 CW 集合循环调用（FTL 侧封装见 `ftl_cmd_configure_descriptors()`）。**失败时链上可能已有 desc**（含本条）——FC 不回滚，调用方用下面两个函数沿链释放 |
| `fc_descriptor_chain_release_head(head)` | 资源回收 | 释放链头那一个 desc + 其 buffer，返回新链头。传值/取回而非传指针：`cmd->desc_id` 是位域取不了地址 |
| `fc_descriptor_chain_release_all(head)` | 资源回收 | 沿链释放**这条命令**的全部 desc + buffer，返回 `FC_DESCRIPTOR_ID_INVALID`。"all" 指本命令的 desc 链（= 该命令已申请的全部资源），**不是**清空 desc/buffer 池。**建链中途失败的回滚入口**：`cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);` |
| `fc_buffer_alloc()` | 资源分配 | 手动从 buffer pool 分配一个 buffer |
| `fc_get_cmd_data_ptr(cmd, cw_index)` | 数据读取 | 获取指定 CW 索引的 buffer 指针（读操作后使用） |
| `fc_submit(cmd)` | 命令下发 | 提交到 SQ 或 HSQ（取决于 `cmd->high_priority`） |
| `fc_service()` | 驱动调度 | 主循环中持续调用，调度并执行命令，提交给 NAND |
| `fc_poll_completion(&completion)` | 状态查询 | 从 CQ 取出已完成命令的状态 |
| `fc_command_release(cmd)` | 资源回收 | 释放 command + descriptor 链 + **各 CW buffer** |
| `fc_buffer_release(buffer_id)` | 资源回收 | 手动释放 buffer |
| `fc_command_id_to_command(id)` | ID 查找 | command_id → `fc_command_t*` |
| `fc_command_to_id(cmd, &id)` | 反查 | 指针 → command_id（返回 0 = 指针不在池内） |
| `fc_descriptor_id_to_descriptor(id)` | ID 查找 | descriptor_id → `fc_descriptor_t*` |
| `fc_buffer_id_to_real_buffer(id)` | ID 查找 | buffer_id → 真实 buffer 指针（USER 数据区首地址） |
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
| `fc_command_configure_descriptor()` | `command==NULL`、不在 pool 中、state 不是 ALLOCATED、plane/cw 越界（越段返回 `INVALID_ARGUMENT`）；**失败时链上可能已挂上本条 desc**（v16：desc 出池即挂链，无回滚窗口），调用方须 `fc_descriptor_chain_release_all(cmd->desc_id)`。v17：全部校验在出池之前，非法实参不污染池子 |
| `fc_descriptor_chain_release_head(head)` | head 越界（空链）→ 无操作、原样返回 |
| `fc_descriptor_chain_release_all(head)` | head 越界 → 置 `FC_DESCRIPTOR_ID_INVALID` 返回 |
| `fc_buffer_release()` | buffer_id 不在范围内、buffer 未分配 |
| `fc_buffer_id_to_real_buffer()` | buffer_id 不在范围内、buffer 未分配 |
| `fc_get_cmd_data_ptr()` | `command==NULL`、不在 pool 中、无 desc 链、cw_index 超过 CW 范围 |
| `fc_submit()` | `command==NULL`、不在 pool 中、state 不是 ALLOCATED、`fc_command_validate()` 失败、descriptor 所有权不完整 |
| `fc_poll_completion()` | `completion==NULL` |
| `fc_command_release()` | `command==NULL`、不在 pool 中；**幂等**——已在 FREE 直接返回 OK（判活只能看 `command_state[]`，结构体释放后被整体清零）；**释放 desc 链 + 各 CW buffer**（v16 修复：此前只放 desc，buffer 一直漏） |
| `fc_command_id_to_command()` | command_id 超范围 |
| `fc_descriptor_id_to_descriptor()` | descriptor_id 超范围 |
| `fc_get_resource_stats()` | `stats==NULL` |

### 命令 ID 由指针派生

命令池 ID **不是 `fc_command_t` 里的字段**（v15 删除 `command_id` / `reserved0`，结构体 24 → 20 字节），
而是 `command - command_pool` 的差值。全仓只有一份实现：

| 函数 | 位置 | 检查 |
|---|---|---|
| `fc_command_to_id_internal(controller, command)` | `fc.c` | 仅 `ASSERT`（前置条件：指针属本池） |
| `fc_command_to_id(command, &id)` | `fc_interface.c` | 真运行时区间检查，返回 0 = 指针不在池内 |

内部三处（`fc_descriptor_alloc_internal` / `fc_submit_to_queue` / `fc_command_release_internal_api`）
与 FTL 侧（`ftl_cmd_id()`，见 `include/ftl/ftl_fc_interface.h`）都走这两个函数。

⚠ 减法**不能放进 `ASSERT`**：`NDEBUG` 会把赋值一起裁掉，留下未初始化变量。

（原先 `fc_command_configure_descriptors_internal()` 已于 v13 删除——掩码枚举上移 FTL。）

---

## 完成通知与状态读取

### 完成状态结构（fc_command_status_t）

CQ 条目只携带**命令级聚合状态 + 命令级返回段**。Per-CW 细节（status / ecc_bits）留在 descriptor 链中，
聚合 ECC 走 `fc_get_cmd_ecc_max()`：

```c
struct fc_command_status_t {
    DWORD command_id : 16;
    DWORD desc_id    : 16;  // 出错时更新为第一个失败的 desc
    DWORD module     : 4;   // 发起者模块，直接路由用
    DWORD status     : 8;   // 命令级聚合状态（各 CW 的 FC_CW_STATUS_* 位 OR）
    DWORD reserved   : 20;
    fc_param_t param;       // 命令级返回段（见下）
};
```

`param` 是与 SQ 共用的 4 字节联合体 `fc_param_t`：

```c
typedef union {
    struct { uint8_t parameter0, parameter1, parameter2, parameter3; };
    uint8_t  bytes[4];  // 内联数据的字节视图
    // v28：删除 cw_bitmap 视图——CW 集合改作建链形参（见下节），CQ 侧不再聚合失败集
} fc_param_t;
```

- **GET_FEATURES**：`param.bytes` = 读回的 4 字节寄存器值。SQ 命令体提交后不再被写
  （值经控制器级暂存 `g_fc_return_staging` 中转进 CQ）。
- **SET_FEATURES**：内联数据由 SQ 的 `command->param.bytes` 携带。
- **其余命令恒 0**（v28 起不再回传失败 CW 位图；上层要定位失败 CW，遍历 desc 链 OR 各
  `desc->status` 自取）。

### 读命令的 CW 子集选择（建链形参 `cw_bitmap`）

CW 子集是 **flat 32 位位图**（bit `n = plane * CW_NUM + cw`，覆盖
`CW_TOTAL_MAX = 8` 位），用于只读一页中**任意 CW 子集**（读恢复场景：ECC 只挂了 2~3 个 CW 时只重读
失败的那几个，不必重读整页）。编号是**绝对**的，不是"每 plane 各自的低 4 位"。
**v28 起它是 `ftl_cmd_configure_descriptors()` 的形参**，不再是命令结构体的字段
（v13~v27 为 `cmd->param.cw_bitmap`）：

| 选择 | 位图 |
|---|---|
| 两个 plane 各 4 CW（满读） | `0x0F \| 0xF0 = 0xFF` |
| **只读 plane 1 的 4 个 CW** | **`0xF0`**（不是 `0x0F`） |
| plane 0 取 {CW0,CW3} + plane 1 取 {CW1} | `0b0010_1001 = 0x29` |

```c
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
cmd->config.cw        = 3U;      // 已退出建链（v23）；读者只剩 FC 内 RAID stripe 的 cw_count
// v28：位图作实参传给建链入口，不再是命令字段
ftl_cmd_configure_descriptors(cmd, 0x29U);   // 只读 plane0{CW0,CW3} + plane1{CW1}
// 按位图置位建 desc 链（plane-major 序）；长度一律走链
```

⚠ **`cw_bitmap` 是字面位图（v27）**：置 1 的位 = 要访问的 CW，**全 0 = 一个都不访问**。
v23~v26 期间 `0` 曾被重载为"本命令 plane 段内的全部 CW"，该重载**已取消**——
「生产者忘了填」与「有意访问 0 个 CW」必须可区分。

**想整段访问**（绝大多数调用者的意图）：不要手写 `0xFF`/`0xF0`，用

```c
ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd));  // 须在 pba 与 multi_plane 写好之后
```

它按 `cmd->pba` 的 plane 位与 `config.multi_plane` 算出"段内全部 CW"（plane1 单 plane ⇒ `0xF0`，
双 plane ⇒ `0xFF`）。三个语义为"整段访问"的 FTL 入口（`pba_to_cmd` / `ftl_cmd_setup_ppa` /
`ftl_cmd_setup_ppa_pair`）**自己替你调**，不必管；只有 `ftl_cmd_configure_descriptors()` 需要你给位图。

**校验**：位图越出本命令 plane 位段（或位宽超过 `PLANE_NUM * CW_NUM`）由
`ftl_cmd_resolve_cw_bitmap()` 的 `ASSERT` 拦下（v23 起由返回码改 ASSERT）；
全 0 同样被该函数的第一条 `ASSERT` 拦下。⚠ 两条 ASSERT 在 release 下会被裁掉。
RAID / stripe 命令要求两 plane 的 CW 集合相同（XOR 按 CW 配对），非对称位图会被 ASSERT 拒绝。

**不在本次范围**：FTL 读恢复流程（`src/ftl/read_recovery.c`）对失败位图的接线。

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
cmd->config.operation = OP_NORMAL_PROGRAM_PAGE;
cmd->config.ch        = 0;
cmd->config.ce        = 0;
cmd->config.lun       = 0;
cmd->pba              = target_pba;

// ③ 只建 desc 链 + buffer（FTL 侧封装：按形参 cw_bitmap 展开后逐条 configure_descriptor）
//    v28：整段访问的位图作实参算好传进去（须在 pba 与 multi_plane 写好之后）
//    不搬数据——搬数据是④，且只有写操作才做。建链失败时链已回滚，此处再还命令槽位。
if (ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd)) != FTL_OK) {
    fc_command_release(cmd);   // desc 链已由 ftl_cmd_* 沿链释放（v16）
    return;
}

// ④ 写入数据（仅写操作；FTL 侧遍历 desc 链，buffer_id → pointer → memcpy）
ftl_cmd_write_data(cmd, source_data, user_length);   // 逐 desc 按链序搬；长度由调用者按业务给

// ⑤ 提交到硬件队列
fc_submit(cmd);
```

> **注意**：`ftl_cmd_write_data()` 内部就是"遍历 desc 链逐 CW memcpy"的封装，
> 想手动控制时用 `fc_get_cmd_data_ptr(cmd, cw_index)` 取指针自己写。

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

// ③ 跳过建链（ftl_cmd_configure_descriptors() 会短路为 desc_id = INVALID）— SET/GET_FEATURES 内联传递

// ④ 设置寄存器地址和数据（feature_addr = 寄存器地址，parameter[0..3] = 4 字节数据）
cmd->feature_addr = 0x01;      // 目标寄存器地址（例：Feature Register 0x01）
cmd->parameter0   = 0x40;      // 写：数据字节 0；读：完成后从此读取回读值
cmd->parameter1   = 0x00;      // 写：数据字节 1；读：完成后从此读取回读值
cmd->parameter2   = 0x00;      // 写：数据字节 2；读：完成后从此读取回读值
cmd->parameter3   = 0x00;      // 写：数据字节 3；读：完成后从此读取回读值

// ⑤ 提交到硬件队列
fc_submit(cmd);

// 完成后，GET_FEATURES 结果直接从 parameter0..3 读取
// 注意：无需建链，也无需 fc_command_release() 释放 desc/buffer
```

**关键区别**：

| 步骤 | 普通数据命令 | SET/GET_FEATURES |
|------|------------|-----------------|
| descriptor 分配 | `ftl_cmd_configure_descriptors()` | 短路为 `desc_id = INVALID`（`ftl_cmd_needs_desc_chain()`） |
| 数据写入 | `ftl_cmd_write_data()` | `cmd->parameter[0..3]` 直接赋值 |
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
ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd));
fc_submit(cmd);

// 完成后直接获取指定 CW 的 buffer 指针
fc_command_t *done = fc_command_id_to_command(completion.command_id);
uint8_t *buffer = fc_get_cmd_data_ptr(done, 0);  // CW0 的数据
// 从 buffer 读取数据...
```

> 读操作**不调** `ftl_cmd_write_data()`（没有要搬进去的数据）；取数据除了上面的
> `fc_get_cmd_data_ptr()` 逐 CW 手动读，也可用 `ftl_cmd_read_data(cmd, buf, len)`
> 一次性按链序拷出。两者都在 `fc_command_release()` **之前**调用。

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