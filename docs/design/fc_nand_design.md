# FC 与 NAND 模块设计方案

## 概述

本文档记录 soft_ssd 项目中 FC（Flash Controller）模块和 NAND 底层驱动模块的完整设计方案，包括设计理念、模块架构、数据结构、接口约定、命令生命周期以及各子模块的职责划分。

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
    DWORD desc_id    : 16;     // 首个 Descriptor ID
    DWORD high_priority : 1;  // 0=SQ, 1=HSQ
    DWORD module     : 4;     // 完成回调路由（HOST_IO/FTL/GC/READ_RECOVERY/METADATA/BBT/CHECKPOINT/WEAR_LEVELING）
    DWORD faa;                 // FTL 逻辑地址（LAA）
    DWORD paa;                 // 物理地址（操作相关）
    fc_cmd_conf_t config;      // 操作配置（见下文）
    DWORD parameter[4];        // 4×1 字节参数（≤4B 数据直接存此，>4B 使用 Descriptor+Buffer）
};
```

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
    DWORD status    : 8;    // CW 状态（FC_CW_STATUS_*）
};
```

设计决策：
- **Descriptor 中 CW 字段为"当前 CW 索引"**，不同于 Command 中 CW 为"数量减一"
- **Buffer 与 Descriptor 解耦**：通过 buffer_id 引用，Buffer 可独立释放（如 UNC 时释放 buffer 保留 descriptor 信息）
- **linked list 链式连接**：通过 next_id 遍历，无需 Command 中维护 descriptor_count

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
    DWORD desc_id    : 16;  // 首个 Descriptor ID
    DWORD status0~3  : 8×4; // 各 CW 的状态
};
```

FTL 通过 command_id 解析 Command 信息，通过 desc_id 遍历 Descriptor 链获取各 CW 状态。

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
  ├─ fc_scheduler_select()  → 从 SQ/HSQ 取 command_handle
  ├─ nand_dispatch_submit() → 路由到对应通道 FIFO
  └─ nand_dispatch_poll()   → 推进各通道/LUN 上的活跃事务
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
uint8_t nand_dispatch_submit(fc_command_t *command);
uint8_t nand_dispatch_poll(nand_dispatch_completion_t *completion);
```

`nand_dispatch_completion_t` 包含 command_id、execution_status、nand_status、per-CW status 和 failed_descriptor_id，由 FC 转换为 `fc_command_status_t` 写入 CQ。

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

| 模块 | 状态 | 计划 |
|------|------|------|
| `fc_ldpc_engine` | encode/decode 返回 NOT_SUPPORTED | 待实现 BCH/LDPC 算法 |
| `fc_raid_engine` | XOR 返回 NOT_SUPPORTED | 待实现 |
| `fc_data_path` | encode/decode 为 no-op | 接入 random→LDPC→RAID 管线 |
| `nand_command.c` | #warning 占位 | 命令编码与状态转换 |
| `nand/bad_block.c` | #warning 占位 | 坏块状态记录 |
| `nand/ecc.c` | #warning 占位 | ECC 可纠/不可纠模拟 |
| `nand/nand_timing.c` | #warning 占位 | 异步完成时间推进 |

### 8.2 接口待改进项

| 问题 | 说明 |
|------|------|
| `nand_fc_*` 类型位置 | 定义在 `nand_mt29f4g08abadawp.h`（芯片头文件），应提取到通用接口头 |
| `nand_config.h` 命名歧义 | `PAGE_NUM→PAGES_PER_BLOCK`，`BLOCK_NUM→BLOCKS_PER_PLANE` |
| 缺少 Main/Spare 分离 | 没有 `PAGE_MAIN_SIZE` / `PAGE_SPARE_SIZE` 宏 |
| `nand_device_t` 遗留 | 与 nand_dispatch 路径并存但未连通，flash_controller 模块为死代码 |
| 双向头文件依赖 | `nand_dispatch.h` include `fc.h`，FC include `nand_dispatch.h` |

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

