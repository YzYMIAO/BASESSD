# FC 与 NAND 模块设计方案

## 概述

本文档记录 soft_ssd 项目中 FC（Flash Controller）模块和 NAND 底层驱动模块的完整设计方案，包括设计理念、模块架构、数据结构、接口约定、命令生命周期以及各子模块的职责划分。

> 本文档覆盖的变更历史：
> - **Phase A**: buffer_id 便捷接口（`fc_write_cmd_data` / `fc_get_cmd_data_ptr`）
> - **Phase B**: 核心结构重组（status/descriptor/ECC/dispatch 边界）
> - **Phase C**: SET/GET_FEATURES 内联数据路径（§14）
> - **Phase D**: nand_dispatch 函数指针表重构（§15）

目标硬件平台：STM32H743 + MT29F4G08ABADAWP（1 Channel × 1 CE × 1 LUN × 2 Plane × 2048 Block × 64 Page，512 MiB SLC NAND）。

---

## 1. 整体架构层次

```
┌─────────────────────────────────────────────────┐
│  FTL / Controller / NVMe                         │  ← 上层调用者
├─────────────────────────────────────────────────┤
│  FC 模块 (Flash Controller)                       │  ← 命令配置、队列、调度
│  fc.h  ─ 对上层提供的统一接口                      │
│  ├── fc_command.c/h   命令 & Descriptor 结构定义    │
│  ├── fc_queue.c/h     SQ / HSQ / CQ 环形队列       │
│  ├── fc_scheduler.c/h HSQ 优先调度器               │
│  ├── fc_registers.c/h 软件寄存器模型               │
│  ├── fc_data_path.c/h 数据路径编排（Random→LDPC→RAID）│
│  ├── fc_random_engine 加扰/解扰引擎                │
│  ├── fc_ldpc_engine   LDPC 编解码引擎              │
│  ├── fc_raid_engine   RAID XOR 引擎               │
│  └── fc.c             核心：资源池、生命周期管理     │
├─────────────────────────────────────────────────┤
│  NAND Dispatch (调度桥接层)                        │
│  nand_dispatch.c/h ─ 通道 FIFO，FC→NAND 命令路由    │
├─────────────────────────────────────────────────┤
│  NAND 底层驱动                                    │
│  ├── nand_engine      UOP 微操作引擎（start/step）  │
│  ├── nand_fmc         FMC 硬件后端（15 个 UOP handler）│
│  ├── onfi_1_0         29 种 ONFI 操作 UOP 序列表    │
│  └── nand_mt29f4g08   芯片 init/execute/start/step  │
├─────────────────────────────────────────────────┤
│  硬件抽象                                          │
│  ├── nand_config.h    编译期几何配置 + PAA 编解码    │
│  ├── nand_geometry    运行时几何校验                │
│  └── nand_device_t    ops 回调式抽象（备用/sim）     │
└─────────────────────────────────────────────────┘
```

核心设计原则：

1. **固定资源池 + 空闲链表**：所有资源（Command、Descriptor、Buffer）在 .bss 中静态分配，使用空闲链表实现 O(1) 分配释放，不使用动态内存。
2. **ID 引用代替指针**：对外暴露 16-bit 资源 ID，内部通过 ID→指针转换函数访问，避免指针悬挂和跨模块直接访问。
3. **FC 封装内部状态**：`fc_t` 结构体仅在 `fc.c` 内部可见，对外只提供函数接口。
4. **异步非阻塞**：提交命令立即返回，通过 `fc_service()` 轮询推进，`fc_poll_completion()` 获取结果。

---

## 2. FC 模块详细设计

### 2.1 资源池

三个固定大小的资源池，均以空闲链表方式管理：

| 资源池 | 容量 | 元素大小 | 用途 |
|--------|------|----------|------|
| Command Pool | 64 | `fc_command_t` | 命令控制块 |
| Descriptor Pool | 256 | `fc_descriptor_t` | 数据描述符（每 CW 一个） |
| Buffer Pool | 128 | 528 字节（= PAGE_SIZE / 4） | 数据缓冲区 |

容量设计理由：
- 64 个 Command：覆盖一个 Block 的所有 Page（64 页），支持 pipeline 提交
- 256 个 Descriptor：每个 Command 最多 4 个 CW，64 × 4 = 256
- 128 个 Buffer：至少支持与 Descriptor 1:2 的比例，确保读写并发

### 2.2 命令结构 (fc_command_t)

```c
struct fc_command_t {
    DWORD command_id : 16;     // Command Pool 槽位编号
    DWORD reserved0  : 16;     // 保留（Command Pool 元数据预留）
    DWORD desc_id    : 16;     // 首个 Descriptor ID；0xFFFF=无 Descriptor
    DWORD high_priority : 1;  // 0=SQ, 1=HSQ
    DWORD module     : 4;     // 完成回调路由（HOST_IO/FTL/GC/READ_RECOVERY/METADATA/BBT/CHECKPOINT/WEAR_LEVELING）
    DWORD feature_addr : 8;   // SET/GET_FEATURES 寄存器地址，通过 COLUMN_0 发送
    DWORD reserved1  : 3;     // 保留（未来命令控制扩展）
    DWORD faa;                 // FTL 逻辑地址（LAA）
    DWORD paa;                 // 物理地址（操作相关）
    fc_cmd_conf_t config;      // 操作配置（见下文）
    uint8_t parameter0;        // 命令参数字节 0
    uint8_t parameter1;        // 命令参数字节 1
    uint8_t parameter2;        // 命令参数字节 2
    uint8_t parameter3;        // 命令参数字节 3（≤4B 数据直接存此，>4B 使用 Descriptor+Buffer）
};
```

> **字段变更说明**：
> - `feature_addr:8` 取代原 `reserved1:11` 中的 8 位，专用于 SET/GET_FEATURES 的寄存器地址（通过 COLUMN_0 uop 发送）。
> - `parameter0..3` 从 `DWORD : 8`（位域）改为 `uint8_t`（纯字节），原因是 SET/GET_FEATURES 内联路径需要取 `&parameter0` 地址作为 buffer 指针。位域不能取地址，故以 4 字节空间换地址能力。Command Pool（64 条）总量增加 256 字节。

命令配置位域 `fc_cmd_conf_t`（32-bit 位打包）：

| 字段 | 位宽 | 说明 |
|------|------|------|
| operation | 8 | ONFI 操作码（OP_READ_ID / OP_NORMAL_READ_PAGE / OP_NORMAL_PROGRAM_PAGE / OP_ERASE_BLOCK ...）|
| ch | 2 | 目标通道号 |
| ce | 2 | 目标片选号 |
| lun | 2 | 目标 LUN 号 |
| cw | 2 | CW 数量减一（0=1个，3=4个） |
| crc | 1 | CRC 使能（0=enable, 1=disable） |
| ldpc | 2 | LDPC 模式（0=full, 1=hard, 2=soft, 3=raw） |
| raid | 3 | RAID 模式（0=normal, 1=decode, 2=encode, 3=recover） |
| random | 1 | 加扰使能（0=enable, 1=disable） |
| chain | 1 | 链式命令标志（0=结束, 1=后续还有） |
| page_type | 2 | 页类型（0=LP, 1=MP, 2=UP, 3=TP） |

### 2.3 描述符结构 (fc_descriptor_t)

```c
struct fc_descriptor_t {
    DWORD paa;               // 物理 NAND 地址（含 CW 编码）
    fc_cmd_conf_t config;    // 继承命令配置，config.cw 表示当前 CW 索引
    DWORD buffer_id : 16;   // Buffer Pool ID，0xFFFF=无效
    DWORD next_id   : 16;   // 下一个 Descriptor ID，0xFFFF=链表结束
    DWORD status    : 8;    // CW 状态（FC_CW_STATUS_*），完成时由 FC 回写
    DWORD ecc_bits  : 8;    // ECC 纠正比特数，完成时由 FC 回写（0=无错误）
    DWORD reserved  : 16;   // 保留
};
```

设计决策：
- **Descriptor 中 CW 字段为"当前 CW 索引"**，不同于 Command 中 CW 为"数量减一"
- **Buffer 与 Descriptor 解耦**：通过 buffer_id 引用，Buffer 可独立释放（如 UNC 时释放 buffer 保留 descriptor 信息）
- **linked list 链式连接**：通过 next_id 遍历，无需 Command 中维护 descriptor_count
- **状态 & ECC 回写**：NAND 完成时 FC 将 `cw_status` 和 `ecc_bit_count` 回写至 descriptor 的 `status` 和 `ecc_bits` 字段。FTL 遍历 desc 链即可获取每 CW 详细状态，无需 CQ 查询。这对 Read Retry、RAID 恢复路径至关重要。

### 2.4 CW 状态位

```c
FC_CW_STATUS_NAND_FAIL   // NAND 操作失败
FC_CW_STATUS_TIMEOUT     // 超时
FC_CW_STATUS_CRC_ERROR   // CRC 校验错误
FC_CW_STATUS_UNC         // 不可纠正错误
FC_CW_STATUS_MATA_ERROR  // MATA 校验错误
FC_CW_STATUS_RAID_ERROR  // RAID 恢复失败
FC_CW_STATUS_READY       // 操作完成（= SUCCESS）
FC_CW_STATUS_PROTECTED   // 目标块受保护
```

状态汇总规则：READY 需要所有有效 CW 就绪；失败位任意 CW 置位即置位。

### 2.5 完成状态 (fc_command_status_t)

```c
struct fc_command_status_t {
    DWORD command_id : 16;  // 完成命令的 Pool ID
    DWORD desc_id    : 16;  // 首个 Descriptor ID；失败时标记出错的 Descriptor
    DWORD module     : 4;   // 命令发起者模块，FTL 无需 ID 查询直接路由完成通知
    DWORD ecc_bit0   : 8;   // CW0 ECC 纠正比特数
    DWORD ecc_bit1   : 8;   // CW1 ECC 纠正比特数
    DWORD ecc_bit2   : 8;   // CW2 ECC 纠正比特数
    DWORD ecc_bit3   : 8;   // CW3 ECC 纠正比特数
    DWORD reserved   : 4;   // 保留
    DWORD status0    : 8;   // CW0 状态（FC_CW_STATUS_*）
    DWORD status1    : 8;   // CW1 状态
    DWORD status2    : 8;   // CW2 状态
    DWORD status3    : 8;   // CW3 状态
};
```

CQ 条目从原来的 8 字节（command_id + desc_id + status0~3）变为 12 字节，64 条 CQ 共 768 字节（+256 字节）。

**设计要点：**
- **module 字段**：拷贝自 `command->module`，FTL 收到完成通知后可直接路由到对应模块（Host IO / GC / Read Recovery 等），无需通过 command_id 查询再决定路由。
- **ECC 比特数**：每个 CW 的 ECC 纠正比特数（0~255），FTL 可据此监测 BER（Bit Error Rate），触发磨损均衡、Read Retry 阈值检测等策略。
- **状态回写**：各 CW 状态由 NAND 完成回调写入，FTL 通过 desc_id 遍历 Descriptor 链获取每 CW 的详细状态和 ECC 计数，无需从 CQ 条目中逐一解析。CQ 提供完整的冗余快照，FTL 可按需选择读取方式。

### 2.6 队列系统

**SQ（Submission Queue）**：普通优先级命令，深度 64
**HSQ（High-priority SQ）**：高优先级命令（如 Read Retry、恢复操作），深度 16
**CQ（Completion Queue）**：完成通知，深度 64

队列实现：环形 FIFO，存储 Command Handle 或 Completion 结构体。队列存储由 FC 内部静态数组提供。

### 2.7 调度器 (fc_scheduler_t)

第一版调度规则：HSQ 优先级高于 SQ。从 HSQ 取命令，为空则从 SQ 取。当前不处理 LUN busy / CE 冲突 / chain 锁定（后期扩展）。

### 2.8 软件寄存器 (fc_registers_t)

```c
typedef struct {
    uint32_t config;            // 功能使能（RANDOM/LDPC/RAID）
    uint32_t status;            // 状态（READY/BUSY/SQ_HSQ_CQ_NOT_EMPTY/ERROR）
    uint16_t sq_head/tail;     // SQ 头尾指针
    uint16_t hsq_head/tail;    // HSQ 头尾指针
    uint16_t cq_head/tail;     // CQ 头尾指针
    uint32_t interrupt_enable;  // 中断使能
    uint32_t interrupt_status;  // 中断状态
} fc_registers_t;
```

模拟硬件寄存器风格，方便未来硬件化。

### 2.9 对外接口 (fc.h)

```c
fc_status_t   fc_init(void);                           // 初始化所有资源池和队列
fc_command_t *fc_command_alloc(void);                   // 申请命令，失败返回 NULL
fc_command_t *fc_command_id_to_command(uint16_t id);    // ID→指针转换
fc_descriptor_t *fc_descriptor_id_to_descriptor(uint16_t id);
fc_status_t   fc_command_configure_descriptors(fc_command_t *cmd); // 自动遍历 CW 数量配置 Descriptor 链
fc_buffer_id_t fc_buffer_alloc(void);                   // 申请 Buffer（返 ID）
fc_status_t   fc_buffer_release(fc_buffer_id_t id);     // 释放 Buffer
uint8_t      *fc_buffer_id_to_real_buffer(fc_buffer_id_t); // ID→指针
fc_buffer_id_t fc_real_buffer_to_buffer_id(const uint8_t *);
fc_status_t   fc_submit(fc_command_t *cmd);             // 提交到 SQ 或 HSQ（根据 high_priority）
fc_status_t   fc_service(void);                         // 调度执行（上层 while(1) 轮询）
fc_status_t   fc_poll_completion(fc_command_status_t *); // 从 CQ 取完成项
fc_status_t   fc_command_release(fc_command_t *cmd);    // 释放命令及关联 Descriptor 链
fc_status_t   fc_get_resource_stats(fc_resource_stats_t *); // 资源使用统计
```

### 2.10 命令生命周期

```
1. fc_command_alloc()           → 获取 fc_command_t*
2. 上层配置 command 字段         → 设置 operation/ch/ce/lun/cw/ldpc/random/raid 等
3. fc_command_configure_descriptors() → 自动申请 N 个 Descriptor + Buffer，构建链表
   （N = config.cw + 1）
4. fc_submit(command)            → 根据 high_priority 入 SQ 或 HSQ
5. while(1) { fc_service(); ... }  → 调度器取命令→nand_dispatch_submit→nand_dispatch_poll
6. fc_poll_completion(&status)   → 非空则获取完成信息
7. 上层根据 status 判断结果
8. fc_command_release(command)   → 回收 Command + Descriptor 链 + Buffer
```

---

## 3. NAND 底层模块详细设计

### 3.1 微操作引擎 (nand_engine)

设计理念：将每个 ONFI 命令拆解为 UOP（Micro-Operation）序列，由引擎逐步执行。

**UOP 类型**（15 种）：

| UOP | 说明 |
|-----|------|
| WAIT_READY | 等待 R/B# 信号 |
| COMMAND | 发送命令字节 |
| COMMAND_PRIMARY | 发送首命令字节（READ_ID 等单命令操作） |
| COMMAND_SECONDARY | 发送次命令字节 |
| COLUMN_0/1 | 发送列地址 |
| ROW_0/1/2 | 发送行地址 |
| DATA_IN | 数据写入 NAND |
| DATA_OUT | 从 NAND 读取数据 |
| DATA_OUT_STATUS | 读取状态字节 |
| READ_STATUS | 发送 70h，读取状态 |
| CHECK_READY | 检查 Ready 位 |
| CHECK_FAIL | 检查 Fail 位 |
| END | 序列结束 |

**执行模式**：

```
nand_engine_execute()  —— 同步模式，阻塞直到完成
nand_engine_start()    —— 异步模式，初始化状态机
nand_engine_step()     —— 异步模式，推进一步，返回 NOT_READY 表示需继续轮询
```

WAIT_READY 和 CHECK_READY 内置超时检测（`busy_timeout_us`），超时返回 `NAND_RESULT_TIMEOUT`。

### 3.2 FMC 硬件后端 (nand_fmc)

实现全部 15 个 UOP handler，通过函数指针表挂载到 `nand_engine_backend_t`。

硬件访问方式：通过 `nand_fmc_config_t` 配置的 volatile 指针访问 FMC 的 Command/Address/Data 窗口：

```c
typedef struct {
    volatile uint8_t *data;     // FMC 数据窗口地址
    volatile uint8_t *command;  // FMC 命令窗口地址
    volatile uint8_t *address;  // FMC 地址窗口地址
    nand_fmc_timing_t timing;  // FMC 时序参数
    nand_fmc_wait_ready_t wait_ready; // R/B# 检测函数
} nand_fmc_config_t;
```

### 3.3 ONFI 1.0 命令表 (onfi_1_0)

29 种 ONFI 操作，每种操作预编译为 UOP 数组。结构：

```c
typedef struct {
    uint8_t  operation;         // OP_* 常量
    const char *name;           // 调试名称
    uint8_t  command_1/2;       // 命令字节
    uint8_t  address_cycles;    // 地址周期数
    uint8_t  data_input_cycles; // 数据输入周期
    uint8_t  valid_while_*_busy; // 并发约束
    const nand_uop_t *uops;     // UOP 序列
    nand_operation_timing_t timing; // 超时参数
} nand_operation_map_t;
```

`OP_*` 常量直接作为 `g_nand_operation_map[OP_COUNT]` 数组下标，O(1) 查表。

### 3.4 NAND FC 接口 (nand_mt29f4g08abadawp)

芯片级封装，对上提供：

```c
uint8_t nand_fc_execute(operation, &request, &completion);  // 同步
uint8_t nand_fc_start(transaction, operation, &request);    // 异步 start
uint8_t nand_fc_step(transaction, &completion);             // 异步 step
uint8_t nand_fc_read_page(&request, &completion);           // 类型化包装
uint8_t nand_fc_program_page(&request, &completion);
uint8_t nand_fc_erase_block(&request, &completion);
uint8_t nand_fc_read_id(id[5], &completion);
uint8_t nand_fc_reset(&completion);
```

关键类型：

```c
// 请求结构
typedef struct {
    uint16_t column;
    uint32_t row;
    uint8_t *data;
    uint16_t data_length;
} nand_fc_request_t;

// 完成结构
typedef struct {
    uint8_t execution_status;  // NAND_RESULT_*
    uint8_t nand_status;       // 70h 状态值
} nand_fc_completion_t;

// 事务状态（异步模式）
typedef struct {
    nand_engine_state_t engine;
    nand_command_request_t request;
    nand_command_completion_t completion;
    uint8_t active;
} nand_fc_transaction_t;
```

---

## 4. FC ↔ NAND 调度桥接层 (nand_dispatch)

### 4.1 设计目的

在 FC 和 NAND 底层之间建立调度桥接，负责：
- 按 Channel/CE/LUN 路由命令到对应物理通道
- 管理每个 Channel 的命令 FIFO 队列
- 处理多 CW 命令的分段执行
- 将 NAND 完成状态转换为 FC 完成状态

### 4.2 架构

```
FC (fc_service)
  │
  ├─ fc_scheduler_select()      → 从 SQ/HSQ 取 command
  ├─ 预解析 desc 链为自包含请求
  │     PAA、buffer 指针在 FC 侧解析
  ├─ nand_dispatch_submit()     → 路由到对应通道 FIFO（不再回调 FC）
  └─ nand_dispatch_poll()       → 推进各通道/LUN 上的活跃事务
       │
       ├─ 从 FIFO 取命令 → 分配 task
       ├─ nand_fc_start() → nand_engine_start() → nand_fmc UOP
       ├─ nand_fc_step()  → nand_engine_step()  → 非阻塞推进
       └─ 完成 → 构建 nand_dispatch_completion_t
```

### 4.3 多 CW 处理策略

**PROGRAM（写）**：
1. 首次执行：遍历 Descriptor 链，将所有 CW 的 Buffer 数据拷贝到 Channel Page Buffer
2. 发送 `OP_NORMAL_PROGRAM_PAGE`（80h-地址-数据-10h）

**READ（读）**：
1. CW0：发送 `OP_NORMAL_READ_PAGE`（00h-地址-30h），读取 528B 到 Buffer
2. CW1~3：发送 `OP_RANDOM_DATA_READ`（05h-列地址-E0h），逐次读取剩余 CW
3. 每个 CW 的数据写入对应 Descriptor 的 Buffer

### 4.4 接口

```c
uint8_t nand_dispatch_init(void);
void    nand_dispatch_set_device_ready(uint8_t ready);
uint8_t nand_dispatch_submit(const nand_dispatch_request_t *request);
uint8_t nand_dispatch_poll(nand_dispatch_completion_t *completion);
```

`nand_dispatch_request_t` 是 FC → NAND 的完整请求结构，包含命令 ID、操作码、通道路由、page_type、参数和预解析的 CW 信息：

```c
typedef struct {
    uint16_t command_id;
    uint8_t  operation;
    uint8_t  ch, ce, lun;
    uint8_t  cw_count;
    uint8_t  page_type   : 2;   // LP/MP/UP/TP — LDPC 引擎需此选择解码阈值
    uint8_t  reserved    : 6;
    uint8_t  parameter[4];
    nand_dispatch_cw_info_t cws[CW_NUM];
} nand_dispatch_request_t;
```

`nand_dispatch_completion_t` 包含命令 ID、执行状态、NAND 状态、per-CW 状态、per-CW ECC 纠正比特数和失败 Descriptor ID：

```c
typedef struct {
    uint16_t command_id;
    uint8_t  execution_status;
    uint8_t  nand_status;
    uint8_t  cw_status[4];
    uint8_t  ecc_bit_count[4];   // 每 CW ECC 纠正比特数
    uint16_t failed_descriptor_id;
} nand_dispatch_completion_t;
```

FC 通过 `fc_complete_from_nand()` 将 `nand_dispatch_completion_t` 转换为 `fc_command_status_t` 写入 CQ，同时逐 CW 回写 descriptor 的 status 和 ecc_bits。

---

## 5. 数据路径 (Data Path)

### 5.1 编码流程 (Page Pack)

```
User Data (512B/CW)
  → [Random Engine] 加扰（LFSR，种子 PAA × program_sequence）
  → [LDPC Engine] 编码（添加 parity）
  → 写入 NAND
```

### 5.2 解码流程 (Page Unpack)

```
NAND 读取
  → [LDPC Engine] 解码（hard/soft/raw）
  → 若失败 → [RAID Engine] XOR 恢复
  → [Random Engine] 解扰
  → User Data
```

### 5.3 CW 布局

每个 Page（2112 字节）= 4 个 CW × 528 字节，CW 布局：

```
[META:16B][CRC:4B][USER DATA:476B][LDPC PARITY:32B] = 528B
```

META 目前使用 4 字节 FAA（FTL 逻辑地址），剩余 12 字节保留。CW 在 Page 中位置由 CW index 决定，第 0 个 CW 从 offset 0 开始，依次四等分。

---

## 6. PAA（Physical Array Address）设计

### 6.1 编码布局

```
PAA bit layout (uint32_t, 低位优先):
[CW:2][PAGE:6][PLANE:1][BLOCK:11][LUN:0]
```

- CW（2 bit）：Page 内 CW 索引（0~3）
- PAGE（6 bit）：Block 内页号（0~63）
- PLANE（1 bit）：Plane 选择
- BLOCK（11 bit）：Plane 内块号（0~2047）
- LUN（0 bit）：当前单 LUN，预留扩展

**注意**：Channel 和 CE 不在 PAA 内——它们属于命令级路由信息，保存在 `fc_cmd_conf_t` 的 ch/ce 字段中。

### 6.2 内联函数

```c
static inline uint32_t paa_to_page(nand_paa_t paa);
static inline uint32_t paa_to_plane(nand_paa_t paa);
static inline uint32_t paa_to_block(nand_paa_t paa);
static inline uint32_t paa_to_lun(nand_paa_t paa);
static inline nand_paa_t paa_make(page, plane, block, lun, cw);
static inline uint8_t    paa_is_valid(page, plane, block, lun, cw);
static inline uint32_t   paa_to_nand_row(nand_paa_t paa);  // 转换为 NAND 行地址
```

`paa_to_nand_row()` 将 PAA 转换为 MT29F4G08 的三周期行地址编码：PA[5:0]→bit[5:0]、BA6(Plane)→bit[6]、BA[17:7]→bit[17:7]。

---

## 7. 编译期配置 (nand_config.h)

```c
#define PAGE_SIZE   (2112)   // 每页总字节（含 OOB）
#define PAGE_NUM    (64)     // 每 Block 页数
#define BLOCK_NUM   (2048)   // 每 Plane 块数
#define PLANE_NUM   (2)
#define DIE_NUM     (1)
#define LUN_NUM     (1)
#define CH_NUM      (1)
#define CE_NUM      (1)
#define CW_NUM      (4U)     // 每页 CW 数
```

---

## 8. 已知待完善项

### 8.1 空壳/占位模块

| 模块 | 状态 | 说明 |
|------|------|------|
| `src/flash_controller/` | ✅ 已删除 | 死代码模块，已被 FC + nand_dispatch 取代 |
| `src/nand/bad_block.c` | ✅ 已删除 | 桩文件 |
| `src/nand/ecc.c` | ✅ 已删除 | 桩文件 |
| `src/nand/nand_command.c` | ✅ 已删除 | 桩文件，所有 ONFI 命令编码已在 `onfi_1_0.c` 实现 |
| `src/nand/nand_timing.c` | ✅ 已删除 | 桩文件，时序参数已在 `nand_operation_map_t.timing` 中 |
| `fc_ldpc_engine` | encode/decode 返回 NOT_SUPPORTED | 待实现 BCH/LDPC 算法 |
| `fc_raid_engine` | XOR 返回 NOT_SUPPORTED | 待实现 |
| `fc_data_path` | encode/decode 为 no-op | 接入 random→LDPC→RAID 管线 |

### 8.2 接口待改进项

| 问题 | 说明 | 状态 |
|------|------|------|
| `nand_fc_*` 类型位置 | 已提取到 `nand_fc_interface.h`（通用接口头） | ✅ 已解决 |
| `nand_config.h` 命名歧义 | `PAGE_NUM→PAGES_PER_BLOCK`，`BLOCK_NUM→BLOCKS_PER_PLANE` | ✅ 已解决 |
| 缺少 Main/Spare 分离 | 已添加 `PAGE_MAIN_SIZE` / `PAGE_SPARE_SIZE` 宏 | ✅ 已解决 |
| `nand_device_t` 遗留 | 与 FTL 模拟路径共存，未来 FTL 迁移到 FC 接口后可移除 | 📝 已文档化 |
| 双向头文件依赖 | `nand_dispatch.h` 现只 include `fc/fc_types.h`（仅基础类型）；FC 在 submit 时预解析 Descriptor 链，nand_dispatch 不再回调 FC | ✅ 已解决 |
| `fc_command_handle` 编译 bug | `fc.c` 内部类型已统一为 `fc_command_id_t` | ✅ 已解决 |
| 死代码 `flash_controller/` | 模块已删除 | ✅ 已解决 |
| 桩文件清理 | `bad_block.c`, `ecc.c`, `nand_command.c`, `nand_timing.c` 已删除 | ✅ 已解决 |
| 双完成结构转换 | `fc_complete_from_nand()` 简化为直接字段拷贝 | ✅ 已解决 |

### 8.3 功能扩展规划

| 阶段 | 内容 |
|------|------|
| Phase 1 | 多 LUN 并发：nand_engine 异步步进 + 每 LUN 独立状态机 |
| Phase 2 | DMA 数据传输：FMC 数据窗口改为 DMA 驱动 |
| Phase 3 | 中断驱动：R/B# EXTI 替代轮询 WAIT_READY |
| Phase 4 | LDPC 硬解码实现 |
| Phase 5 | RAID 跨 Plane XOR 恢复 |

---

## 9. 测试

### 9.1 集成测试 (test_fc_nand.c)

在 STM32H743 核心板上运行的 FC→NAND 全链路测试：

- `fc_nand_test_read_id()` — 验证 NAND ID
- `fc_nand_test_erase_block(paa)` — 擦除整个 Block
- `fc_nand_test_program_block(paa)` — 顺序编程 Block（0x55 模式）
- `fc_nand_test_read_block(paa)` — 顺序读取并校验
- `fc_nand_test_run_all(paa)` — 串行执行上述全部

### 9.2 串口控制台命令

```
nand id                          → READ_ID 测试
nand test <PAA>                  → 完整 Block 擦写读测试
nand paa <ch> <ce> <lun> <plane> <block> <page> <cw> → PAA 计算
```

### 9.3 编译

```sh
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 tests
```

---

## 10. 设计决策记录

1. **为什么用空闲链表而非遍历查找** — 企业级实现，O(1) 分配，避免 Pool 深度扩大时性能退化。
2. **为什么 Buffer 与 Descriptor 解耦** — UNC 发生时可以释放 buffer 供其他任务使用，只保留 descriptor 信息做错误处理。
3. **为什么 Command 中 CW 字段是"数量减一"** — 0 表示 1 个 CW，节省位宽。Descriptor 中 CW 是"当前索引"，用途不同。
4. **为什么 PAA 不含 Channel/CE** — 通道和片选是命令级路由信息（`fc_cmd_conf_t.ch/ce`），PAA 只编码芯片内物理地址。
5. **为什么数据 ≤ 4B 不分配 Descriptor** — GET_FEATURES 等命令返回值小，复用 Command 的 4 字节 parameter 字段避免浪费资源。
6. **为什么 command_module 字段放在 FC 而非 FTL** — CQ 中的完成项需要路由到正确的回调，由 FC 原样返回提交时的 module 值。


## 11. 接口重构策略与落地清单

### 11.1 设计原则

**参数校验分层：**
- 对外接口层：校验所有外部传入参数（NULL 检查、范围检查）
- 内部函数：默认参数有效，不做冗余校验（controller、command 等由 init 保证）

**返回值语义统一：**
使用 `FuncStatus` 枚举替代裸 `0U`/`1U`：
```c
typedef enum {
    FUNC_STATUS_SUCCESS = 0,
    FUNC_STATUS_ERROR   = !FUNC_STATUS_SUCCESS
} FuncStatus;
```

**命名规范：**
- `command_handle` → `command_id`（统一命名）
- 资源通过 id 访问，提供双向查询：`xxx_id_to_xxx()` + `xxx_to_xxx_id()`

### 11.2 重构项

| # | 项目 | 说明 | 状态 |
|---|------|------|------|
| 1 | FuncStatus 语义统一 | `0U`/`1U` → `FUNC_STATUS_SUCCESS`/`FUNC_STATUS_ERROR` | ✅ |
| 2 | command_handle → command_id | 统一命名 | ✅ |
| 3 | fc_command_configure_descriptors | 合并 descriptor 分配+配置为单接口 | ✅ |
| 4 | paa_make() | PAA 地址合成函数（LUN→Block→Plane→Page→CW） | ✅ |
| 5 | 条件判断宏封装 | 复合条件拆为独立命名宏 | ✅ |
| 6 | 资源 ID 双向查询 | 每种资源提供 id↔指针 双向转换函数 | ✅ |
| 7 | 冗余 NULL 检查移除 | 内部函数移除 controller/command 等 NULL 检查 | ✅ |
| 8 | fc_submit_internal 简化 | 三元嵌套提取为独立变量 | ✅ |
| 9 | fc_command_validate 精简 | 移除位域全组合均有含义的冗余检查 | ✅ |

### 11.3 Descriptor 分配重构

**旧接口：**
```c
fc_descriptor_id_t fc_command_descriptors_alloc(fc_command_t *command, uint8_t cw_count);
```

**新接口（一次完成分配+配置）：**
```c
fc_status_t fc_command_configure_descriptors(fc_command_t *command);
```
内部根据 `command->config.cw` 自动计算需要的 descriptor 数量，分配 descriptor 链+buffer 并串联链表。

### 11.4 命名修正

| 旧名 | 新名 | 位置 |
|------|------|------|
| `PAGE_NUM` | `PAGES_PER_BLOCK` | `nand_config.h` |
| `BLOCK_NUM` | `BLOCKS_PER_PLANE` | `nand_config.h` |

## 12. 数据 Buffer 方案设计讨论

### 12.1 问题背景

FC 模块需要决定数据 buffer（每个 CW 的数据载体）在接口层的表达方式。核心问题可以归结为一个选择：

```
如何把"某个 CW 的数据"和"FC 能访问到的内存位置"关联起来？

选项 A：buffer_id（资源 ID 引用）
选项 B：data_ptr（裸指针/地址）
```

这个选择不仅影响 FC 内部实现，更影响 FTl 与 FC 之间的协作模式、数据路径效率、以及整个系统的安全性。

### 12.2 各存储领域的行业实践

#### 12.2.1 企业级 NVMe SSD（有大量 DDR）

代表：Intel/Samsung/Kioxia 企业级 SSD 控制器。

| 特征 | 做法 |
|------|------|
| DDR 容量 | 几 GB~几十 GB |
| 数据 buffer 管理 | **物理地址（data_ptr）** |
| 控制结构管理 | **ID**（Command Tag、CID） |
| 数据描述方式 | PRP（Physical Region Page）或 SGL（Scatter-Gather List） |
| 传输方式 | DMA 引擎直接从 DDR 搬运，零 CPU 拷贝 |

**理由**：DMA 引擎只能消费物理地址，多一层 buffer_id → 地址的查表就是多一个硬件逻辑门延迟。数据路径上，地址直达是最优选择。

#### 12.2.2 消费级 NVMe/SATA SSD（有外置 DRAM 或 HMB）

代表：Phison/Silicon Motion 主控。

| 特征 | 做法 |
|------|------|
| DRAM 容量 | 0（DRAM-less）~512MB |
| 数据 buffer | **物理地址（有 DRAM）/ 内部 ID（DRAM-less）** |
| 控制结构 | **ID** |

有 DRAM 时：与企业的做法一致，地址直达。
DRAM-less 时：**内部 SRAM Pool + ID 方案**（见 12.2.3）。

#### 12.2.3 DRAM-less SSD（与你的项目最接近）

代表：DRAM-less NVMe 主控（如 Phison PS5013、SM2258 的 DRAM-less 变体），部分入门级 SATA SSD。

```
Host DDR ←→ PCIe/SATA 控制器
                    │
              内部 SRAM (256KB~1MB)
                ├── FTL 映射表（压缩 / 从 NAND 加载）
                ├── 数据 buffer Pool ←── 基于 ID 管理
                └── 控制结构（命令、队列）
                    │
               NAND 控制器 → NAND Flash
```

**数据路径（写入）：**

```
Host 数据（在 Host DDR 中）
  │  PRP/SGL 描述地址
  ▼
NVMe 控制器 DMA ──► 内部 SRAM Buffer Pool ──► NAND
                          ↑
                    基于 ID 管理！
                    控制器内部管理 ~128 个 SRAM buffer
                    每个 buffer 有内部 ID，对 Host 透明
```

**数据路径（读取）：**

```
NAND ──► 内部 SRAM Buffer Pool ──► DMA ──► Host DDR
                    ↑
              也是基于 ID
```

**关键结论**：DRAM-less SSD 在控制器内部清一色用 ID/Pool 方案。内部 SRAM 是稀缺资源，必须精细管理。DMA 引擎最终需要物理地址，但地址是由控制器内部从 buffer_id 转换得来——这个过程对外部 Host 完全透明。

| 层次对比 | DRAM-less SSD | 你的项目 |
|----------|--------------|---------|
| 数据源 | Host DDR（PRP/SGL 描述） | FTL 的 SRAM/SDRAM 数据区 |
| 中转 buffer | 控制器内部 SRAM Pool | FC Buffer Pool（SRAM） |
| 从源到 buffer 的方式 | 控制器 DMA 引擎自动搬运 | `fc_write_cmd_data()` 内部 memcpy（或 DMA）|
| 地址描述 | PRP/SGL 中的物理地址 | buffer_id（FC 内部转换） |
| ID 管理 | 控制器内部，对 Host 不可见 | FC 接口暴露 buffer_id |

#### 12.2.4 UFS（Universal Flash Storage）

代表：三星、铠侠、SK 海力士 UFS 主控。

```
UFS Controller SoC：
  ┌────────────────────────────────────────┐
  │  M-PHY → UTP（UPIU 解析器）             │
  │              │                          │
  │              ├── PRDT（物理地址列表）     │
  │              │    (Host ↔ Controller 之间) │
  │              │                          │
  │          FTL / Flash Management         │
  │              │                          │
  │              ├── 内部 SRAM（几十~几百 KB）│
  │              │    ├── 映射表 Cache       │
  │              │    └── 数据 Buffer Pool   │
  │              │                          │
  │              ▼                          │
  │          NAND Flash Controller          │
  └────────────────────────────────────────┘
```

**两层地址管理：**

| 层次 | 做法 | 原因 |
|------|------|------|
| 对外（Host ↔ Controller） | PRDT 物理地址列表 | UFS 协议规定 |
| 对内（FTL ↔ NAND） | **内部 ID/Pool 方案** | 无外置 DRAM，SRAM 由 Firmware 精细管理 |

UFS 的主流方案多年以来就是内部 ID/Pool。但近年趋势是：随着 UFS 控制器 SRAM 增大（从几十 KB → 512KB~1MB），新架构逐渐倾向 **"Shared SRAM + Pointer"** 的统一地址访问方式。

#### 12.2.5 行业规律总结

```
有 DRAM/SDRAM（≥几 MB） ──→ 数据用 data_ptr（零拷贝），控制用 ID
无 DRAM（仅内部 SRAM）   ──→ 数据和控制统一用 ID/Pool
SRAM 达到 512KB~1MB     ──→ 行业处于从 ID 向 Pointer 过渡的阶段
```

### 12.3 你的硬件参数

| 资源 | 大小 |
|------|------|
| 内部 SRAM | **1MB** |
| 板载 SDRAM | **8MB** |
| FC 控制结构总需求 | ~75 KB（占 SRAM 的 7%）|
| FC 数据 Buffer Pool（当前方案） | ~67.5 KB（128×528B）|

FC 控制结构仅占用 SRAM 的 7%，剩余 93% 可供 FTL 使用。1MB SRAM 的容量已经达到 **UFS 旗舰主控档次**。

### 12.4 方案一：buffer_id（当前方案）

#### 12.4.1 核心思想

FC 内部维护静态 Buffer Pool，所有数据 buffer 通过 16-bit ID 对外暴露。FTL 在使用数据时通过 ID → pointer 转换访问实际内存。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_conf_t    config;
    uint16_t         buffer_id : 16;  // ← 指向 FC 内部 Buffer Pool
    uint16_t         next_id   : 16;
    uint8_t          status    : 8;
} fc_descriptor_t;
```

**相关接口（4 个）：**

| 接口 | 说明 |
|------|------|
| `fc_buffer_alloc()` | 从 FC Buffer Pool 分配 |
| `fc_buffer_release(id)` | 释放回 Pool |
| `fc_buffer_id_to_real_buffer(id)` | ID → 指针 |
| `fc_real_buffer_to_buffer_id(ptr)` | 指针 → ID |

#### 12.4.2 FTL 使用流程

```c
// ① 分配 command + desc 链 + buffer
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.cw = 1;   // 2 个 CW
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);   // 自动分配 desc 链 + buffer

// ② FTL 将数据写入 buffer（需遍历 desc 链）
fc_descriptor_id_t desc_id = cmd->desc_id;
uint8_t cw = 0;
while (desc_id != FC_DESCRIPTOR_ID_INVALID && cw <= cmd->config.cw) {
    fc_descriptor_t *desc   = fc_descriptor_id_to_descriptor(desc_id);
    uint8_t         *buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
    memcpy(buffer, my_data + cw * CW_SIZE, CW_SIZE);
    desc_id = desc->next_id;
    cw++;
}

// ③ 提交
fc_submit(cmd);
```

#### 12.4.3 优点

- **安全性高**：FC 能验证 buffer_id 范围（0~127），天然防止野指针
- **生命周期明确**：FC 通过 command state 和 descriptor status 知道哪些 buffer 在使用中
- **资源可视化**：`fc_get_resource_stats()` 可直接输出 buffer 使用率
- **改动最小**：已有代码无需重构

#### 12.4.4 缺点

- **数据路径多一层转换**：每次读写都要 `buffer_id → real_buffer` 查表
- **多一次 memcpy**：FTL 的数据必须经过 memcpy 进入 FC Buffer Pool
- **不灵活**：FTL 无法控制 buffer 放在哪里（SRAM 还是 SDRAM）
- **接口膨胀**：4 个 buffer 管理接口增加了学习成本

### 12.5 方案二：data_ptr（裸指针/地址方案）

#### 12.5.1 核心思想

FC 不再管理数据 buffer，Descriptor 中直接用指针指向数据位置。数据由 FTL 自主分配和管理。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_conf_t    config;
    uint8_t         *data_ptr;    // ← 直接指向 SRAM/SDRAM 地址
    uint16_t         next_id : 16;
    uint8_t          status  : 8;
} fc_descriptor_t;
```

**删除的接口（4 个）：**
- `fc_buffer_alloc()`
- `fc_buffer_release()`
- `fc_buffer_id_to_real_buffer()`
- `fc_real_buffer_to_buffer_id()`

**接口总数从 14 个减少到 10 个。**

#### 12.5.2 FTL 使用流程

```c
// ① FTL 自己管理数据 buffer
uint8_t *buf = ftl_buf_alloc();           // FTL 自己的分配器
memcpy(buf, my_data, CW_SIZE);

// ② 分配 command + desc 链（不分配 buffer）
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.cw = 1;
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);    // 只分配 desc，不分配 buffer

// ③ FTL 直接指定地址
fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(cmd->desc_id);
desc->data_ptr = buf;                     // ← 直接给地址

// ④ 提交
fc_submit(cmd);

// ⑤ 完成后 FTL 释放
fc_poll_completion(&completion);
fc_command_release(cmd);
ftl_buf_free(buf);                        // FTL 自己释放
```

#### 12.5.3 优点

- **零拷贝**：FTL 直接写自己的 buffer，没有中间 memcpy
- **接口精简**：减少 4 个 buffer 接口
- **灵活性高**：FTL 可自由选择 buffer 位置（SRAM 或 SDRAM）
- **架构对齐行业**：与有 DRAM 的企业/消费级做法一致

#### 12.5.4 缺点

- **所有权/生命周期管理复杂**（见 12.5.5）
- **无防错机制**：FC 无法验证 data_ptr 的有效性（只能检查 NULL）
- **FTL 需要额外建立 buffer 管理代码**（~30 行 freelist）

#### 12.5.5 data_ptr 的生命周期问题

这是 data_ptr 方案最核心的难点。本质问题：

> **指针是一个裸地址，没有任何归属标记。同一个地址可以被多个模块同时引用，没有任何机制可以判断"谁在用"、"是否可用"**

**典型 UAF（Use-After-Free）场景：**

```
① FTL 分配 buf_A → 填入数据
② desc->data_ptr = buf_A → fc_submit(cmd)
③ FTL 判断 buf_A 不再需要 → ftl_buf_free(buf_A)
④ FC 还没发出 buf_A 的数据到 NAND ××× 数据已被其他人占用 ×××
```

**解决方案对比：**

| 方案 | 机制 | 安全等级 | 开销 | 实现复杂度 |
|------|------|---------|------|-----------|
| **合约约束** | submit 到 release 之间 FTL 不释放 | 低（靠自觉） | 零 | 零 |
| **FC 借用表** | FC 内部小表追踪借用中的地址 | 高 | ~1KB SRAM | 中 |
| **FTL 引用计数** | FTL 侧 ref_count 管理 | 中 | 小 | 低 |
| **Owner Tag** | 地址附带归属标记 | 高 | 小 | 中 |
| **调试断言** | 释放前检查 FC 是否还在使用 | 开发期高 | 调试时 | 低 |

**推荐组合：合约约束 + 调试断言。**

```c
// FTL 侧的契约（注释即文档）
// 约束：从 fc_submit() 到 fc_command_release() 之间，
//       不允许释放 desc->data_ptr 指向的 buffer。
// 违反此契约会导致数据错乱。

// FTL 释放时的调试断言
#ifdef DEBUG_FTL
void ftl_buf_free(uint8_t *ptr) {
    // 检查 FC 是否还有 pending 命令引用此地址
    DEBUG_ASSERT(!fc_is_addr_pending(ptr));
    // ... 实际释放
}
#endif
```

### 12.6 方案三：双模设计（推荐过渡方案）

#### 12.6.1 核心思想

Descriptor 同时支持 buffer_id 和 data_ptr，通过标志位选择使用哪种模式。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_conf_t    config;
    
    union {
        uint8_t     *data_ptr;    // FTL 直接指定地址（零拷贝）
        uint16_t     buffer_id;   // FC 内部 Pool（传统路径）
    };
    
    uint16_t         next_id : 16;
    uint8_t          status  : 8;
    uint8_t          own_buf : 1;  // 0=FC Pool, 1=FTL 自己提供
} fc_descriptor_t;
```

#### 12.6.2 适用场景

| 场景 | 用哪种 | 原因 |
|------|--------|------|
| 常规 Page 写/读 | data_ptr | FTL 直接在 SDRAM/SRAM 准备数据，零拷贝 |
| 小操作（SET_FEATURES 等） | parameter 字段 | ≤ 4 字节，不需要 desc |
| 临时数据/测试代码 | buffer_id | 兼容旧路径，无需 FTL 管理 buffer |
| 内部 GC/校验操作 | data_ptr | FTL 自己管理缓冲，灵活分配 |

#### 12.6.3 优点

- **平滑迁移**：旧代码可以不修改继续使用 buffer_id
- **灵活选路**：FTL 根据场景选择最佳方式
- **兼容所有硬件变体**：即使不接 SDRAM，也能用内部 Pool

#### 12.6.4 缺点

- **两套路径维护**：FC 内部需要同时支持两种数据获取方式
- **接口复杂度**：双模增加了理解成本
- **可能永远不会用另一条路径**：如果确定了方向，双模就是死代码

### 12.7 方案四：buffer_id + 便捷数据接口（推荐方案）

#### 12.7.1 核心思想

保持 buffer_id 作为唯一的数据引用方式，保留 FC 的 Buffer Pool 所有权管理。但**新增便捷接口**让 FTL 不必手动遍历 desc 链写数据，解决易用性问题。

```c
// 新增接口：一次写入命令关联的所有 CW 数据
fc_status_t fc_write_cmd_data(fc_command_t *cmd,
                               const uint8_t *data,
                               uint16_t length);
// 内部校验 length == (cmd->config.cw + 1) * CW_SIZE
// 自动遍历 desc 链：buffer_id → pointer → memcpy

// 新增接口：获取指定 CW 的数据指针（读操作使用）
uint8_t *fc_get_cmd_data_ptr(fc_command_t *cmd, uint8_t cw_index);
// 遍历 desc 链找到第 cw_index 个 desc
// 返回 buffer_id → real_buffer
```

**接口总数从 14 个变为 16 个。**

#### 12.7.2 FTL 使用流程

**写操作：**

```c
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.cw = 3;                  // 4 个 CW
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);  // 自动分配 desc 链 + buffer

// 一行完成数据填充，FC 内部处理 buffer_id → pointer → memcpy
fc_write_cmd_data(cmd, my_source_data, 4 * CW_SIZE);

fc_submit(cmd);
```

**读操作：**

```c
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_READ;
cmd->config.cw = 3;
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);

fc_submit(cmd);
// ... fc_service() 循环 ...

fc_poll_completion(&completion);
fc_command_t *done = fc_command_id_to_command(completion.command_id);

// 一行获取数据指针，FTL 直接读取
uint8_t *data = fc_get_cmd_data_ptr(done, 0);   // 第 0 个 CW 的数据

fc_command_release(done);
```

**非常规场景**（如只写某个 CW 的数据）：FTL 仍可回退到手动遍历 desc 链的方式。

#### 12.7.3 与 DRAM-less SSD 的对应关系

| DRAM-less SSD | 你的项目（本方案） |
|--------------|------------------|
| Host 数据 → PRP/SGL 描述地址 | FTL 有数据 → 调用 `fc_write_cmd_data()` |
| 控制器 DMA 自动将数据拉入内部 SRAM Pool | FC 内部 `buffer_id → pointer → memcpy` |
| 控制器内部 Pool ID（对 Host 透明） | buffer_id（对 FTL 可见） |
| 完成后 DMA 自动将数据写出给 Host | FTL 调用 `fc_get_cmd_data_ptr()` 读数据 |

逻辑完全一致，只是你的项目中 memcpy 替代了 DMA 的角色。未来可升级为 DMA。

#### 12.7.4 优点

- **保持所有权管理**：FC 仍然拥有完整的 buffer 生命周期控制，避免 UAF
- **易用性好**：FTL 无需手动遍历 desc 链，两行完成读写
- **安全性高**：FC 可验证 buffer_id 范围，拦截非法访问
- **改动小**：保留现有实现，只新增两个接口
- **可进化**：未来 memcpy 可替换为 DMA，接口不变

#### 12.7.5 缺点

- **多一次 memcpy**：相比 data_ptr 的零拷贝，有 CPU 开销
- **接口总数增加**：14 → 16 个
- **SRAM 占用**：需要预留 ~67.5KB 的 Buffer Pool

### 12.8 方案汇总对比

| 维度 | ① buffer_id（现状） | ② data_ptr | ③ 双模 | ④ buffer_id + helper（**推荐**） |
|------|-------------------|------------|--------|-------------------------------|
| **接口数** | 14 个 | 10 个 | 14 个 | 16 个 |
| **数据路径** | memcpy | 零拷贝 | 两者 | memcpy（可升级 DMA） |
| **所有权管理** | FC 完全管理 | FTL 管理 | 混合 | FC 完全管理 |
| **安全性** | 高（范围检查） | 低（裸指针） | 中（取决于模式） | 高（范围检查） |
| **FTL 代码量** | 中（需遍历 desc） | 高（需自建 buffer） | 中 | **低**（一行接口） |
| **灵活性** | 低 | 高 | **最高** | 中 |
| **SRAM 占用** | ~67.5KB Pool | FTL 自定 | 两者共存 | ~67.5KB Pool |
| **行业对齐** | DRAM-less | 有 DRAM | 过渡架构 | **DRAM-less** |
| **实现难度** | ✅ 已完成 | 大改 | 大改 | **小改** |

### 12.9 推荐方案

**选择方案④：buffer_id + 便捷数据接口。**

理由：

1. **所有权清晰** — FC 全权管理 buffer 的生命周期，FTL 无需关心地址泄露或 UAF
2. **易用性好** — 新增 `fc_write_cmd_data()` 和 `fc_get_cmd_data_ptr()` 后，FTL 一行代码完成数据读写
3. **对齐行业** — DRAM-less SSD 的做法与当前设计高度一致，方向可靠
4. **可进化** — 内部 memcpy 未来可以无损替换为 DMA，接口不变
5. **适合 1MB SRAM** — 有充足空间维持 Buffer Pool，不必为节省 ~67KB 引入复杂度

**后续改进方向：**

- **短期**：实现 `fc_write_cmd_data()` 和 `fc_get_cmd_data_ptr()`，简化 FTL 使用
- **中期**：将 memcpy 替换为 DMA 传输（DMA: FTL buffer → FC Buffer Pool）
- **长期**（如需零拷贝）：在双模或纯 data_ptr 方案上扩展，但保持接口层不变

---

## 13. 控制/状态位贯穿分析与 Dispatch 边界设计

### 13.1 问题背景

FC 和 NAND Dispatch 之间存在一条**硬件语义边界**：

```
FTL → FC (firmware) → [nand_dispatch_request_t] → NAND Engine (硬件状态机)
NAND Engine → [nand_dispatch_completion_t] → FC (firmware) → FTL
```

这条边界是 **FC firmware 与 NAND 硬件引擎之间的防火墙**。FC 内部处理 firmware 层面的功能（数据加扰/解扰、LDPC 编解码、RAID XOR 编解码），只有 NAND 硬件引擎真正需要的字段才跨越 dispatch 边界。

核心问题：`fc_command_t` / `fc_descriptor_t` / `fc_cmd_conf_t` 中的所有控制位和状态位，哪些应该跨越 dispatch 边界进入 NAND 硬件，哪些应该留在 FC firmware 内部处理？

### 13.2 边界设计原则

| 原则 | 说明 |
|------|------|
| **硬件消费原则** | 只有 NAND 硬件引擎（LDPC 解码器、状态机）实际消费的字段才跨越边界 |
| **Firmware 封装原则** | FC firmware 内部处理的功能（LDPC 编码、RAID、Randomizer）不在 dispatch 层暴露 |
| **RAID 透明原则** | RAID 是 FC 内部功能，dispatch 层感知不到 RAID 的存在——看到的是标准 PROGRAM 命令 |
| **ECC 反馈原则** | 硬件解码结果（ECC 纠正比特数、per-CW 状态）必须带回 FC |

### 13.3 字段级贯穿映射

#### 13.3.1 fc_cmd_conf_t → Dispatch Request

| 字段 | 位宽 | 过 Dispatch？ | 理由 |
|------|------|-------------|------|
| `operation` | 8 | **✅ 通过** | 硬件需要知道执行什么 ONFI 操作 |
| `ch` | 2 | **✅ 通过** | 通道路由，硬件必须知道 |
| `ce` | 2 | **✅ 通过** | 片选路由，硬件必须知道 |
| `lun` | 2 | **✅ 通过** | LUN 路由，硬件必须知道 |
| `cw` | 2 | **→ cw_count** | 硬件需要知道 CW 数量以规划传输 |
| `crc` | 1 | **❌ 不过** | CRC 校验由 FC 在 firmware 层面完成（写入前附加 CRC，读出后校验），硬件不感知 |
| `ldpc` | 2 | **❌ 不过** | LDPC 编码/解码模式是 FC firmware 控制数据路径的参数。硬件 LDPC 引擎在解码时通过 `page_type` 选择阈值，不需要知道 `ldpc_mode` |
| `raid` | 3 | **❌ 不过** | RAID 是 FC 内部功能。Start XOR / Continue XOR / End XOR 由 FC 处理，dispatch 只看到标准 PROGRAM。End XOR 的 parity 写入哪个 plane 由 FC 推导 PAA |
| `random` | 1 | **❌ 不过** | 加扰/解扰在 FC 数据路径中完成，对硬件透明 |
| `chain` | 1 | **❌ 不过** | 命令链是 FC 调度器的概念，NAND 硬件单条执行 |
| `page_type` | 2 | **✅ 通过** | TLC/QLC LDPC 引擎需要 LP/MP/UP/TP 信息选择解码阈值——硬件必须知道当前读取的页类型 |
| `reserved` | 6 | — | 保留 |

#### 13.3.2 fc_descriptor_t → Dispatch Request

| 字段 | 过 Dispatch？ | 理由 |
|------|-------------|------|
| `paa` | **✅ 通过 → cws[n].paa** | NAND 物理地址，硬件目标寻址 |
| `buffer_id` | **✅ 解析为指针 → cws[n].buffer** | FC 预解析为内存指针，硬件直接使用 |
| `next_id` | **❌ 不过** | 链表是 FC 内部结构，FC 在提交时已遍历解析 |
| `status` | **❌ 不过** | 完成状态由硬件回写，不在请求方向携带 |
| `ecc_bits` | **❌ 不过** | 同 status，是回写字段 |
| `config.cw` | **→ index 编码入 cws[]** | FC 按 index 顺序填充 cws 数组 |

#### 13.3.3 Dispatch Completion → fc_command_status_t

| Dispatch Completion 字段 | 回写目标 | 说明 |
|------------------------|---------|------|
| `command_id` | `fc_command_status_t.command_id` | CQ 条目标识 |
| `execution_status` | 驱动层处理 | NAND_RESULT_OK/TIMEOUT/NAND_FAIL |
| `nand_status` | `nand_dispatch_completion_t`保留 | 70h 状态值，供 FC 调试 |
| `cw_status[4]` | → `fc_command_status_t.status0~3` **+** 回写 `fc_descriptor_t.status` | 双重交付：CQ 快照 + Descriptor 持久化 |
| `ecc_bit_count[4]` | → `fc_command_status_t.ecc_bit0~3` **+** 回写 `fc_descriptor_t.ecc_bits` | 双重交付 |
| `failed_descriptor_id` | → `fc_command_status_t.desc_id` | 定位错误 Descriptor |

### 13.4 关键设计决策详解

#### 13.4.1 page_type 过、LDPC_MODE 不过

```
page_type (LP/MP/UP/TP) ──→ Dispatch Request ──→ NAND LDPC Engine
                                                     选择解码阈值
ldpc_mode (hard/soft/raw) ──→ FC 数据路径内部 ──→ LDPC Engine 配置
                              （不通过 dispatch）
```

TLC/QLC NAND 的不同页类型（LP=Lower Page, MP=Middle Page, UP=Upper Page, TP=Triple Page）具有不同的原始 BER 特性，LDPC 解码器需要根据页类型选择最优解码阈值。这是硬件 LDPC 引擎必须的信息。

而 `ldpc_mode`（full/hard/soft/raw）是 FC firmware 控制解码策略的参数——FC 在数据路径中配置 LDPC 引擎的工作模式，不需要 NAND 硬件状态机感知。

#### 13.4.2 RAID 在 FC 内部实现

```
FTL: fc_command.config.raid = RAID_ENCODE
                                    │
                                    ▼
FC firmware 内部：
  Start XOR： 读取第 0~2 plane 的数据 → 计算 XOR
  Continue XOR：读取下一个数据块 → 累加 XOR
  End XOR：    将 XOR 结果写入最后一个 plane

dispatch 看到的命令序列：
  [0] OP_NORMAL_PROGRAM_PAGE  plane=0  → 正常写入
  [1] OP_NORMAL_PROGRAM_PAGE  plane=1  → 正常写入
  [2] OP_NORMAL_PROGRAM_PAGE  plane=2  → 正常写入
  [3] OP_NORMAL_PROGRAM_PAGE  plane=3  → 写入 XOR parity（PAA 由 FC 推导）
```

关键点：
- Dispatch 层不感知 RAID 模式——它看到的是 4 个独立的 PROGRAM 命令
- RAID parity 写入哪个 plane 由 FC 决定并生成对应 PAA
- 解码时 FC 读取所有 plane 数据，若某个 CW 解码失败则 XOR 恢复

#### 13.4.3 ECC 比特数反馈链

```
NAND 硬件 LDPC 解码完成
       │
       ├── cw_status[4]  (成功/失败/UNC)
       └── ecc_bit_count[4] (纠正了多少比特)
                │
                ▼
       nand_dispatch_completion_t
                │
                ▼
       fc_complete_from_nand()
                │
                ├──→ fc_command_status_t.ecc_bit0~3  → CQ → FTL
                └──→ fc_descriptor_t.ecc_bits         → Descriptor 回写
                                                    FTL 遍历 desc 链获取
```

**ECC 比特数的价值：**
- **BER 监测**：FTL 持续跟踪每 CW 的 ECC 纠正比特数，生成 BER 热力图
- **磨损均衡触发**：当纠正比特数持续升高时触发 wear-leveling
- **Read Retry 阈值**：纠正比特数逼近 LDPC 解码能力上限时启动 read-retry
- **RAID 恢复决策**：纠正比特数异常时提前触发 RAID recovery

**为什么使用 8 位（最大 255）？** 当前主流 LDPC 硬解码能力约 80~120 bits/1KB，软解码约 200~300 bits/1KB。8 位可覆盖硬解码全范围，软解码超限时直接标记 UNC，不需要更大的字段。

#### 13.4.4 Descriptor 状态回写机制

NAND 完成后，FC 不仅将完成信息写入 CQ，还**逐 CW 回写 descriptor**：

```c
/* fc_complete_from_nand() 中的回写逻辑 */
desc_id = command->desc_id;
for (cw = 0; cw < cw_count; cw++) {
    descriptor               = &controller->descriptor_pool[desc_id];
    descriptor->status       = nand_completion->cw_status[cw];
    descriptor->ecc_bits     = nand_completion->ecc_bit_count[cw];
    desc_id = descriptor->next_id;
}
```

**为何需要双重交付（CQ + Descriptor 回写）？**

| 场景 | CQ 快照足够？ | 需要 Descriptor 回写？ |
|------|-------------|---------------------|
| 正常完成，FTL 检查状态 | ✅ 是 | 可选 |
| Read Retry：FTL 想对特定 CW 重试 | ❌ 需要知道哪个 CW 失败 | **✅ desc_id 标记 + status** |
| RAID Recovery：逐 CW 检查解码状态 | ❌ CQ 只有命令级信息 | **✅ 每个 desc 独立状态** |
| 长链路命令完成，CQ 已出队 | ❌ 状态已丢失 | **✅ Descriptor 保留状态** |
| 错误注入/调试分析 | ❌ | **✅ 状态持续可查** |

#### 13.4.5 module 字段直接路由

`fc_command_status_t.module` 拷贝自 `command->module`，使 FTL 收到 CQ 条目后无需通过 `command_id` 查询命令再决定路由：

```c
// 不用 module 的方式：
status = fc_poll_completion(&completion);
cmd = fc_command_id_to_command(completion.command_id);
switch (cmd->module) { ... }  // 需要一次额外指针查询

// 用 module 的方式：
status = fc_poll_completion(&completion);
switch (completion.module) { ... }  // 直接路由
```

module 枚举值覆盖所有 FC 使用者：

| module | 用途 |
|--------|------|
| `FC_COMMAND_MODULE_HOST_IO` | 主机读写命令 |
| `FC_COMMAND_MODULE_FTL` | FTL 内部元数据操作 |
| `FC_COMMAND_MODULE_GC` | 垃圾回收 |
| `FC_COMMAND_MODULE_READ_RECOVERY` | Read Retry 恢复 |
| `FC_COMMAND_MODULE_METADATA` | 元数据读写 |
| `FC_COMMAND_MODULE_BBT` | Bad Block Table 管理 |
| `FC_COMMAND_MODULE_CHECKPOINT` | Checkpoint/恢复 |
| `FC_COMMAND_MODULE_WEAR_LEVELING` | 磨损均衡 |

### 13.5 Dispatch 请求/完成结构总览

#### 请求方向（FC → NAND）

```
nand_dispatch_request_t (跨边界字段):
┌────────────┬──────┬──────────────────────────────────────┐
│ 字段        │ 来源  │ 说明                                 │
├────────────┼──────┼──────────────────────────────────────┤
│ command_id │ cmd  │ 完成关联                              │
│ operation  │ conf │ 硬件执行的 ONFI 操作                   │
│ ch/ce/lun  │ conf │ 硬件路由                              │
│ cw_count   │ conf │ CW 数量（= conf.cw + 1）              │
│ page_type  │ conf │ LP/MP/UP/TP → LDPC 解码阈值选择        │
│ parameter  │ cmd  │ ≤4B 参数（SET/GET_FEATURES 内联数据）   │
│ feat_addr  │ cmd  │ SET/GET_FEATURES 寄存器地址（= column） │
│ cws[n].paa │ desc │ NAND 物理地址                         │
│ cws[n].buf │ desc │ 数据指针（已从 buffer_id 解析）         │
└────────────┴──────┴──────────────────────────────────────┘
```

#### 完成方向（NAND → FC）

```
nand_dispatch_completion_t (跨边界字段):
┌────────────────┬─────────────────────────────────────────┐
│ 字段            │ 说明                                    │
├────────────────┼─────────────────────────────────────────┤
│ command_id     │ 关联回哪个命令                            │
│ execution_stat │ NAND_RESULT_OK / TIMEOUT / NAND_FAIL     │
│ nand_status    │ 70h 状态寄存器值                          │
│ cw_status[4]   │ 每 CW 完成状态                            │
│ ecc_bit_c[4]   │ 每 CW ECC 纠正比特数（0~255）            │
│ failed_desc_id │ 失败 Descriptor ID                       │
└────────────────┴─────────────────────────────────────────┘
```

### 13.6 内存开销分析

| 结构 | 变更 | 单条大小 | 数量 | 总量变化 |
|------|------|---------|------|---------|
| `fc_command_status_t` | 8B → 12B（+4B） | 12B | 64 | +256B |
| `fc_descriptor_t` | 12B → 16B（+4B） | 16B | 256 | +1KB |
| `nand_dispatch_completion_t` | 新增 `ecc_bit_count[4]` | +4B | — | +4B |
| `nand_dispatch_request_t` | 新增 `page_type`（占位已有 reserved） | 0B | — | 0B |
| **合计** | | | | **约 +1.3KB** |

1.3KB 的开销相对于 1MB SRAM 可以忽略不计（0.13%），但获得的能力——BER 监测、Read Retry 触发、RAID 恢复、无 CQ 的 Descriptor 状态查询——是 enterprise-grade SSD 的必要功能。

### 13.7 与行业方案对比

| 维度 | 企业级 SSD（Intel/Samsung） | 消费级（Phison/SMI） | 本方案 |
|------|---------------------------|--------------------|--------|
| ECC 比特数反馈 | ✅ 有，PCIe VDM/MI 接口 | ✅ 有 | ✅ 8-bit/CW |
| Descriptor 回写 | ✅ 硬件自动 | ✅ 固件回写 | ✅ FC 回写 |
| page_type 过 LDPC | ✅ | ✅ | ✅ |
| RAID in firmware | ✅ | ✅ | ✅ |
| module 路由 | ✅ Tag 路由 | ✅ | ✅ |
| CQ 冗余状态 | ✅ 双重交付 | ✅ | ✅ |
| 总开销 | ≤0.5% DRAM | 可忽略 | 0.13% SRAM |

---

## 14. SET/GET_FEATURES 内联数据路径优化

### 14.1 背景

SET_FEATURES（0xEF）和 GET_FEATURES（0xEE）是 ONFI 协议中读写 NAND 内部寄存器的命令，格式为：

- **SET_FEATURES**：发送 1 字节寄存器地址（COLUMN_0）→ 4 字节参数数据
- **GET_FEATURES**：发送 1 字节寄存器地址（COLUMN_0）→ 回读 4 字节参数数据

原始实现中，即使 SET/GET_FEATURES 只传输 ≤5 字节，也分配了完整的 descriptor + buffer（528 字节），浪费了有限的 SRAM 资源池。优化目标是消除这次无谓的分配，让命令通过 `fc_command_t` 内部字段直接承载这 5 字节信息。

### 14.2 数据结构变更

在 `fc_command_t` 中：

1. **`feature_addr:8`** — 从原 `reserved1:11` 中划分 8 位，存储 NAND 寄存器地址。FC → nand_dispatch 传递后，由 COLUMN_0 uop 发送 `(uint8_t)request->column`，即此值。
2. **`parameter[0..3]` 由 `DWORD : 8` → `uint8_t`** — 位域不能取地址，但内联路径需要 `&command->parameter0` 作为 buffer 指针（让 GET_FEATURES 回写数据直接落在此处）。改为纯字节后增加了 ~4 字节/command，Command Pool 总量 +256 字节。

### 14.3 数据流追踪

```
SET_FEATURES:
─────────────
command->feature_addr ──► request.feature_addr ──► task.feature_addr ──► nand_fc_request_t.column (= 0x01)
command->parameter[0..3] ──► request.parameter[0..3] ──► task.parameter[0..3]
                                │ (cw_buffers[0] = &command->parameter0)
                                ▼
                           NAND 从 command->parameter0 读取 4 字节发送至寄存器

GET_FEATURES:
─────────────
command->feature_addr ──► (同上) ──► column (= 0x01)
                                │ (cw_buffers[0] = &command->parameter0)
                                ▼
                           NAND 回写 4 字节直接写入 command->parameter[0..3]
                                │
                                ▼
                           测试 / FTL 直接读取 command->parameter0..3
```

### 14.4 关键代码路径

#### FC 层 — 跳过 descriptor 分配

`fc_command_validate()` (fc_command.c) — 允许 SET/GET_FEATURES 的 desc_id = FC_DESCRIPTOR_ID_INVALID：

```c
if ((command->config.operation != OP_SET_FEATURES) &&
    (command->config.operation != OP_GET_FEATURES) &&
    nand_operation_has_data((uint8_t)command->config.operation) &&
    (command->desc_id == FC_DESCRIPTOR_ID_INVALID))
{
    return FC_STATUS_INVALID_ARGUMENT;
}
```

`fc_command_configure_descriptors_internal()` (fc.c) — 跳过 desc 分配：

```c
if ((nand_operation_has_data((uint8_t)command->config.operation) == 0U) ||
    (command->config.operation == OP_SET_FEATURES) ||
    (command->config.operation == OP_GET_FEATURES))
{
    command->desc_id = FC_DESCRIPTOR_ID_INVALID;
    return FC_STATUS_OK;
}
```

`fc_poll_internal()` (fc.c) — 构造 dispatch request 时直接指向 command parameter 地址：

```c
request.parameter[0] = (uint8_t)command->parameter0;
request.parameter[1] = (uint8_t)command->parameter1;
request.parameter[2] = (uint8_t)command->parameter2;
request.parameter[3] = (uint8_t)command->parameter3;
request.feature_addr = (uint8_t)command->feature_addr;

/* 在 descriptor 遍历分支之前插入 SET/GET_FEATURES 分支 */
else if ((command->config.operation == OP_SET_FEATURES) ||
         (command->config.operation == OP_GET_FEATURES))
{
    request.cws[0U].buffer = (uint8_t*)&command->parameter0;
}
```

#### Dispatch 层 — 正确处理 column 和数据

`nand_dispatch_submit()` 拷贝 feature_addr 到 task：

```c
task->parameter[3] = request->parameter[3];
task->feature_addr = request->feature_addr;
```

`nand_dispatch_poll()` SET/GET_FEATURES 分支：

```c
else if ((task->operation == OP_SET_FEATURES) || (task->operation == OP_GET_FEATURES))
{
    task->request = (nand_fc_request_t){(uint16_t)task->feature_addr, 0U,
                                        task->cw_buffers[0U], 4U};
}
```

> **说明**：此处 cw_buffers[0] 已经通过 `nand_dispatch_submit()` 从 request.cws[0].buffer（即 `&command->parameter0`）拷贝，直接指向 FC Command Pool 中的稳定内存。GET_FEATURES 的回写数据通过此通道直接写入 command->parameter[0..3]。

#### 完成路径 — 无需特殊处理

`fc_complete_from_nand()` 检查 desc_id 后才执行 descriptor 回写循环。SET/GET_FEATURES 的 desc_id = FC_DESCRIPTOR_ID_INVALID，自动跳过回写。数据已在 command->parameter[0..3] 中。

### 14.5 读回修复说明

**问题**：在最初实现中，`nand_dispatch_submit()` 将 `cw_buffers[0]` 重定向到 `task->parameter[]`，导致 GET_FEATURES 的回写数据进入 task.parameter 而非 command.parameter，测试读取 command->parameter[0..3] 时看到的是零值。

**修复方案**：去掉重定向。buffer 始终指向 command->parameter0（FC Command Pool 中的稳定内存）。这要求：
1. `parameter[0..3]` 从位域改为 `uint8_t`（可取地址）
2. `nand_dispatch_submit()` 保持 cw_buffers[0] = request.cws[0].buffer（即 `&command->parameter0`）

### 14.6 资源节省

| 资源 | 优化前（每条 SET/GET_FEATURES 命令） | 优化后 |
|------|-----------------------------------|--------|
| Descriptor | 1 个（16 字节） | 0 |
| Buffer | 1 个（528 字节） | 0 |
| 总 SRAM | 544 字节/次 | 0 |
| 全局开销 | — | Command Pool +256 字节（parameter 类型变更） |

实际节省：每次 SET/GET_FEATURES 操作省去 544 字节分配。在 GC 流程中频繁读/写 ONFI 寄存器的场景下，可显著降低资源池竞争。

### 14.7 设计演进

| 阶段 | 方案 | 问题 |
|------|------|------|
| 原始方案 | 分配 desc + buffer（528B）传递 5 字节 | 浪费资源 |
| 内联方案 v1 | `fc_command_t.reserved1:11` → `feature_addr:8`；保留 `DWORD parameter : 8` | 位域不可取地址，读回数据无法送达 |
| 内联方案 v2（最终） | parameter 改为 `uint8_t`；buffer 直接指向 `&command->parameter0`；去掉 task.parameter 重定向 | 读回数据正确落入 command |

---

## 15. nand_dispatch 函数指针表重构 (Phase D)

### 15.1 动机

`nand_dispatch_execute_command()` 和 `nand_dispatch_poll()` 中大量 if-else 链根据 `task->operation` 判断 NAND 操作类型并构造 `nand_fc_request_t`。每条 if-else 链包含 6-7 次条件比较，不仅增加执行时间，还带来分支预测惩罚。

在嵌入式 Cortex-R5/M7 上，分支预测错误代价约 12-14 周期，而函数指针间接跳转固定为 2-3 周期。将条件链替换为函数指针表可显著提升 dispatch 层的执行确定性。

### 15.2 设计方案

两张独立函数指针表，均以 `OP_*`（0~28）为直接下标：

| 表 | 类型 | 用途 | 替代目标 |
|----|------|------|---------|
| `g_nand_dispatch_req_table[OP_COUNT]` | `uint8_t (*)(nand_dispatch_task_t *)` | 从 task 预解析数据构造 `nand_fc_request_t` | `nand_dispatch_poll()` 中的 ~90 行 if-else 链 |
| `g_nand_dispatch_completion_table[OP_COUNT]` | `void (*)(nand_dispatch_task_t *, uint8_t, const nand_fc_completion_t *)` | 设置 cw_status 和 failed_descriptor_id | `nand_dispatch_task_complete()` 中的 if-else 链 |

### 15.3 Request 构建 handler

共 7 个 handler，覆盖全部 29 个 OP：

| Handler | 覆盖的 OP | 行为 |
|---------|----------|------|
| `req_program_page` | NORMAL_PROGRAM_PAGE, OTP_DATA_PROGRAM, PROGRAM_PAGE_CACHE | 将各 CW buffer 拼接到 `g_nand_channel_page_buffer[task->ch]`，request.data_length = PAGE_SIZE |
| `req_read_page` | NORMAL_READ_PAGE, OTP_DATA_READ | 仅 CW0：request = (column=0, row, buffer=cw_buffers[0], length=PAGE_SIZE/4) |
| `req_cache_read` | READ_PAGE_CACHE_SEQ/RANDOM/LAST | 一次性读 full page：request = (0, row, page_buffer, PAGE_SIZE) |
| `req_erase` | ERASE_BLOCK | 纯地址：request = (0, row, NULL, 0) |
| `req_feature` | SET_FEATURES, GET_FEATURES | column = feature_addr：request = (feature_addr, 0, cw_buffers[0], 4) |
| `req_single` | READ_ID, READ_PARAM_PAGE, READ_UNIQUE_ID, READ_STATUS, READ_STATUS_ENH, RANDOM_DATA_READ, RANDOM_DATA_INPUT 等 | 通用固定长度：length 从 `g_nand_operation_map[op].data_length` 读取 |
| `req_no_data` | RESET, READ_MODE, BLOCK_LOCK, BLOCK_UNLOCK 等 | 无数据传输，纯 row 地址 |

### 15.4 Completion 策略 handler

共 3 个 handler：

| Handler | 行为 | 适用 OP |
|---------|------|--------|
| `completion_all_cws` | 所有 CW 共享同一完成状态；fail → failed_descriptor_id = 0 | PROGRAM (3)、ERASE |
| `completion_per_cw` | 只设当前 cw_index 的状态；fail → failed_descriptor_id = cw_index | READ (2)、CACHE_READ (3) |
| `completion_single` | 只设 cw_status[0] | 其余所有 |

### 15.5 分派点简化

**重构前**（`nand_dispatch_poll()` 出队处）：

```c
if (op == OP_NORMAL_PROGRAM_PAGE || op == OP_OTP_DATA_PROGRAM || ...) {
    // ~10 行 PROGRAM 拼 buffer 逻辑
} else if (op == OP_NORMAL_READ_PAGE || ...) {
    // ~6 行 READ 构建逻辑
} else if (op == OP_READ_PAGE_CACHE_SEQ || ...) {
    // ~6 行 CACHE 构建逻辑
} else if (...) ... // 共 7 个分支 ~90 行
```

**重构后**（1 行）：

```c
result = g_nand_dispatch_req_table[task->operation](task);
```

### 15.6 `nand_operation_map_t` 协议属性增强

`nand_operation_map_t` 新增两个协议属性字段（它们是 ONFI 协议属性，不是 dispatch 策略）：

```c
typedef struct {
    // ... 原有字段不变
    uint8_t  has_data;     /* 操作是否需要数据传输 */
    uint16_t data_length;  /* 数据传输字节数（0 = 无数据） */
    // ...
} nand_operation_map_t;
```

对应删除两个独立函数 `nand_operation_has_data()` 和 `nand_operation_data_length()`，改为宏直接查表：

```c
#define nand_operation_has_data(op) \
    ((op) < OP_COUNT ? g_nand_operation_map[(op)].has_data : 0U)
#define nand_operation_data_length(op) \
    ((op) < OP_COUNT ? g_nand_operation_map[(op)].data_length : 0U)
```

初始化表改用 designated initializer，每个字段名显式标注，消除之前依赖字段顺序可能引发的歧义：

```c
[OP_NORMAL_READ_PAGE] = {
    .operation   = OP_NORMAL_READ_PAGE,
    .name        = "READ_PAGE",
    .command_1   = NAND_CMD_READ_PAGE_SETUP,
    .command_2   = NAND_CMD_READ_PAGE_CONFIRM,
    .has_data    = 1U,
    .data_length = PAGE_SIZE,
    .uops        = g_nand_normal_read_page_uops,
    // ...
},
```

### 15.7 废弃代码清理

以下函数被标记为废弃并删除：

| 函数 | 原因 |
|------|------|
| `nand_dispatch_execute_request()` | 已被 `nand_fc_start/step` 非阻塞接口替代 |
| `nand_dispatch_execute_page()` | 逻辑分散到 `req_program_page` / `req_read_page` handler |
| `nand_dispatch_execute_command()` | 标记 `__attribute__((unused))`，功能完全由函数指针表覆盖 |

### 15.8 文件结构

```
src/nand/
├── nand_dispatch.c              # 核心：init, submit, poll, task_complete (~200 行)
├── nand_dispatch_internal.h     # NEW：内部类型定义（task_t + 函数指针表类型）
├── nand_dispatch_tables.c       # NEW：7 个 req handler + 3 个 completion handler + 表 (~200 行)
├── onfi_1_0.c                   # 改：map 表 +2 字段，改用 designated initializer

include/nand/
├── nand_dispatch.h              # 公用接口不变
├── onfi_1_0.h                   # map_t +2 字段，函数声明 → 宏
```

### 15.9 扩展性

未来加新操作（如 Phase 4 LDPC 的专用读取命令）只需三步：

1. 在 `onfi_1_0.h` 定义 `OP_LDPC_READ = 29`，`OP_COUNT` 递增
2. 在 `onfi_1_0.c` 表加一行 `[OP_LDPC_READ] = { ... has_data = 1U, ... }`
3. 在 `nand_dispatch_tables.c` 加 handler + 一行表项 `[OP_LDPC_READ] = req_ldpc_read`

**不需要修改任何 if-else 或 switch 语句。**

### 15.10 性能对比

| 指标 | if-else 链（重构前） | 函数指针表（重构后） |
|------|-------------------|-------------------|
| 分支数 | 6-7 次条件比较 + 嵌套 | 1 次间接跳转（O(1)） |
| 执行路径确定性 | ❌ 取决于 OP 值 | ✅ 固定 2-3 周期 |
| 分支预测惩罚 | 高（每 if 都可能预测失败） | 无（jump table） |
| 添加新操作 | 改 2 处 if-else 链 | 加 handler + 表项 |

### 15.11 与 nand_engine 的架构一致性

`nand_engine` 层本身已使用函数指针表（`backend->handlers[uop->type]`）驱动 UOP 执行：
```c
handler = state->backend->handlers[uop->type];
result = handler(state->backend->context, uop, ...);
```

本次重构使 dispatch 层与 engine 层的架构风格一致：**上层（dispatch）用函数指针表选择策略，下层（engine）用函数指针表执行微操作**。两层均无条件判断链，均为纯表驱动。

### 15.12 RANDOM_DATA_READ 的数据传输长度说明

RANDOM_DATA_READ（05h-C1-C2-E0h）等命令的数据传输长度不由命令码本身决定，而是由 FMC 硬件产生的 RE# 时钟脉冲数控制。`nand_operation_map_t.data_length` 中记录的值不是命令的"固有属性"，而是**传输长度的默认值**，通过 `nand_fc_request_t.data_length` 传递给 FMC 层作为 DMA/RE# 脉冲计数器：

```c
[OP_RANDOM_DATA_READ] = {
    ...
    .data_length = (PAGE_SIZE / CW_NUM),
    /* 注：RE# 时钟次数由 FMC 控制，data_length 仅作为传输长度参数
     * 传递给 FMC 层的 DMA/RE# 脉冲计数器 */
},
```

`req_single` handler 通过 `g_nand_operation_map[op].data_length` 读取该值，无需为每个小操作各写一个 handler。

