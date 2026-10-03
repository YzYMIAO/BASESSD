# FC 与 NAND 模块设计方案

## 概述

本文档记录 soft_ssd 项目中 FC（Flash Controller）模块和 NAND 底层驱动模块的完整设计方案，包括设计理念、模块架构、数据结构、接口约定、命令生命周期以及各子模块的职责划分。

> **📊 流程图说明**：本文档在各关键章节内嵌了 14 张流程图（标注「图 N」），覆盖命令生命周期、数据路径编解码、nand_engine 微码状态机、dispatch poll 两阶段、LUN 相位机、调度器、Stripe RAID 与完成回写。建议按文末 [附录 B 阅读顺序](#附录-b建议阅读顺序) 学习；图 → 源码定位见 [附录 A](#附录-a图--源码-grep-对照)。
>
> 本文档覆盖的变更历史：
> - **Phase A**: buffer_id 便捷接口（`fc_write_cmd_data` / `fc_get_cmd_data_ptr`）
> - **Phase B**: 核心结构重组（status/descriptor/ECC/dispatch 边界）
> - **Phase C**: SET/GET_FEATURES 内联数据路径（§14）
> - **Phase D**: nand_dispatch 函数指针表重构（§15）
> - **Phase D(续)**: per-LUN chain buffer 命令链执行（§16）
> - **Phase E**: Multi-Plane 操作支持 + 行地址数组重构 + OP 码拆分（§17）
> - **Phase F/G/H**: WAIT_READY 中断驱动、DMA 中断完成、ONFI 时序切换
> - **Phase I**: 读饥饿保护（QoS）
> - **Phase J**: 加扰引擎扩展（多算法 + 多种子策略）
> - **Phase K**: BCH ECC 引擎移植 + QC-LDPC 硬判决
> - **Phase L**: CRC-32 引擎实现 + 数据路径集成（§5.1）
> - **Phase M**: Buffer 偏移重构（FC_BUFFER_USER_OFFSET, fc_buffer_get_base）
> - **Phase N**: 跨 Plane Stripe RAID 重构（删旧覆盖末 CW，新增 generate_stripe_parity，fc_poll_internal stripe 积累 + Multi-Plane 提交）
> - **Phase V**: 多 LUN 并发 ISSUE/WAIT/FETCH 拆分 + FMC 后端数组化 + 调度器 phase 机（§18.7 完整实现）
> - **Phase X**: FC-NAND 全模块注释补全 + 行业方案对比补全（§21）

目标硬件平台：STM32H743 + MT29F4G08ABADAWP（1 Channel × 1 CE × 1 LUN × 2 Plane × 2048 Block × 64 Page，512 MiB SLC NAND）。

---

## 1. 整体架构层次

```
┌─────────────────────────────────────────────────┐
│  FTL / Controller / NVMe                         │  ← 上层调用者
├─────────────────────────────────────────────────┤
│  FC 模块 (Flash Controller)                       │  ← 命令配置、队列、调度
│  fc.h  ─ 对上层提供的统一接口（类型/宏，include fc_interface.h）│
│  ├── fc_interface.h/c  对外接口声明 + 实现（薄壳，单例控制器） │
│  ├── fc_internal.h     fc.c ↔ fc_interface.c 共享内部签名     │
│  ├── fc_command.c/h   命令 & Descriptor 结构定义    │
│  ├── fc_queue.c/h     SQ / HSQ / CQ 环形队列       │
│  ├── fc_scheduler.c/h HSQ 优先调度器               │
│  ├── fc_registers.c/h 软件寄存器模型               │
│  ├── fc_data_path.c/h 数据路径编排（Random→LDPC→RAID）│
│  ├── fc_random_engine 加扰/解扰引擎                │
│  ├── fc_ldpc_engine   LDPC 编解码引擎              │
│  ├── fc_raid_engine   RAID XOR 引擎               │
│  └── fc.c             核心：资源池、生命周期、服务主循环     │
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
3. **FC 封装内部状态**：`fc_t` 结构体定义在 `fc_internal.h`，仅 `fc.c`/`fc_interface.c` 内部可见，对外只提供函数接口。
4. **异步非阻塞**：提交命令立即返回，通过 `fc_service()` 轮询推进，`fc_poll_completion()` 获取结果。

### 流程图：分层架构与数据流总览（图 0）

```text
┌─────────────────────────────────────────────────────────────┐
│  FTL / Controller / NVMe  （上层调用者）                      │
│    alloc → 配参 → configure_descriptors → submit → poll      │
└──────────────────────────┬──────────────────────────────────┘
                           │ fc_command_t*（ID 引用，非裸指针）
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  FC 模块（firmware 层：命令池 / desc 链 / buffer 池）          │
│    fc_command.c     命令与 Descriptor 结构                    │
│    fc_queue.c       SQ / HSQ / CQ 环形队列                    │
│    fc_scheduler.c   HSQ 优先 + 读饥饿保护 + 链收集             │
│    fc_data_path.c   写: META→CRC→加扰→ECC→Stripe RAID        │
│                    读: ECC→解扰→CRC→META (+ RAID 恢复)       │
│    fc_random/ldpc/raid/crc_engine.c  软件引擎                 │
└──────────────┬──────────────────────────────────────────────┘
               │ nand_dispatch_request_t（预解析，字段穿越防火墙）
               ▼
┌─────────────────────────────────────────────────────────────┐
│  nand_dispatch（调度桥接层）                                  │
│    submit / submit_chain 入队 → poll 出队 + 推进              │
│    g_nand_dispatch_chains[ch][ce][lun]  per-LUN 链缓冲        │
│    g_nand_channel_page_buffer[ch]     多 plane 拼接页缓冲     │
└──────────────┬──────────────────────────────────────────────┘
               │ nand_fc_start / nand_fc_step（异步非阻塞）
               ▼
┌─────────────────────────────────────────────────────────────┐
│  nand_engine（UOP 微码解释器，软件 micro-sequencer）           │
│    step() 逐条执行 UOP 数组；JUMP 由引擎直接处理               │
└──────────────┬──────────────────────────────────────────────┘
               │ handler(uop, request, timing, completion)
               ▼
┌─────────────────────────────────────────────────────────────┐
│  nand_fmc（FMC 硬件后端：15 个 UOP handler 的函数指针表）       │
│    data / command / address 三个 volatile 窗口                │
│    PIO 逐字节 或 DMA（CONFIG_NAND_DMA）                       │
└──────────────┬──────────────────────────────────────────────┘
               ▼
        ┌── FMC 并行总线 → MT29F4G08 NAND（1ch × 1CE × 1LUN × 2plane）
```

**阅读要点**：越往下越贴近硬件；`nand_dispatch_request_t` 是 FC firmware 与 NAND 硬件引擎之间的"防火墙"，只有硬件要消费的字段才跨越（§13）。上层完全异步：`fc_submit()` 立即返回，推进全部发生在 `fc_service()` 轮询里。

---

## 2. FC 模块详细设计

### 2.1 资源池

三个固定大小的资源池，均以空闲链表方式管理：

| 资源池 | 容量 | 元素大小 | 用途 |
|--------|------|----------|------|
| Command Pool | 64 | `fc_command_t` | 命令控制块 |
| Descriptor Pool | 256 | `fc_descriptor_t` | 数据描述符（每 CW 一个） |
| Buffer Pool | 128 | 528 字节（= PAGE_SIZE / 4） | 数据缓冲区（CW 内部布局见 §5.3） |

Buffer Pool 内部每个 CW（528 字节）分层管理：

```
对外暴露区域（FTL 可见）：
  [USER:512B]    ← fc_buffer_id_to_real_buffer() 返回此首地址

内部管理区域（FC 管控，FTL 不感知）：
  [META:4B]      ← FC 管理（LBA 等 META 信息），读回时自动校验
  [CRC:4B]       ← USER 数据的 CRC-32 校验值
  [USER:512B]    ← 用户数据（FTL 填入）
  [PARITY:8B]    ← ECC 校验数据

对外接口通过 FC_BUFFER_USER_OFFSET(8) 偏移，只暴露 USER 区给 FTL。
FC 内部通过 fc_buffer_get_base() 获取 CW 基址（META 开头），用于加扰/ECC/RAID。
```

### 各模块数据处理范围

| 模块 | 处理大小 | 影响区域 | 原因 |
|------|---------|---------|------|
| CRC | 512 | `[4:8]` 写入 CRC 值 | 只保护 USER 数据 |
| Random 加扰 | **520** | `[0:520]` 全数据区 | ECC 会覆盖 parity 区，RN 白做 |
| ECC 编解码 | 520 → 8B parity | 读 `[0:520]`，写 `[520:528]` | BCH 输入 520 数据，输出 8B parity |
| RAID XOR | 528 | 全 CW（含 parity）| 需要保护 parity 自身完整性 |

ECC 引擎的 `data_length` 参数直接表示实际数据大小（520B），parity 始终写入 `data + data_length`。
RAID XOR 使用 `FC_BUFFER_POOL_CW_SIZE(528)`，读全 CW。

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
    DWORD meta;              // META 值，当前传 LBA（Host 逻辑地址），未来可扩展
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
    DWORD meta;              // META 值：写时从 command->meta 转换填入，读后存放 NAND 读回的 META 值。
    DWORD status    : 8;    // CW 状态（FC_CW_STATUS_*），完成时由 FC 回写
    DWORD ecc_bits  : 8;    // ECC 纠正比特数，完成时由 FC 回写（0=无错误）
};
```

设计决策：
- **Descriptor 中 CW 字段为"当前 CW 索引"**，不同于 Command 中 CW 为"数量减一"
- **Buffer 与 Descriptor 解耦**：通过 buffer_id 引用，Buffer 可独立释放（如 UNC 时释放 buffer 保留 descriptor 信息）
- **linked list 链式连接**：通过 next_id 遍历，无需 Command 中维护 descriptor_count
- **状态 & ECC 回写**：NAND 完成时 FC 将 `cw_status` 和 `ecc_bit_count` 回写至 descriptor 的 `status` 和 `ecc_bits` 字段。FTL 遍历 desc 链即可获取每 CW 详细状态，无需 CQ 查询。这对 Read Retry、RAID 恢复路径至关重要。
- **per-CW META**：`meta` 替代原 `reserved:16`，每个 Descriptor 拥有独立的 META 值。`fc_command_configure_descriptors()` 中通过 `lba_to_cw_meta(command->lba, cw_index)` 自动计算 `desc->meta`，encode 时写入 `buffer[0..4]`（META 区），用于读回时的端到端地址校验。FC 在 decode 路径中自动对比 desc->meta 与读回值，不匹配时置 MATA_ERROR。

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

对外接口分两个头文件，`fc.h` include `fc_interface.h`，外部只 `#include "fc/fc.h"`：

| 文件 | 内容 |
|------|------|
| `include/fc/fc.h` | 类型、宏、资源池配置、功能开关、资源统计结构 + `#include "fc_interface.h"` |
| `include/fc/fc_interface.h` | 对外函数声明（18 个，6 组：①初始化 ②命令生命周期 ③buffer 访问 ④数据便利 ⑤查询工具 ⑥服务入口） |
| `include/fc/fc_internal.h` | fc.c ↔ fc_interface.c 共享内部签名（`fc_t` 定义、`g_fc_controller` extern、`fc_*_internal` 声明）——FTL 不得 include |
| `src/fc/fc.c` | 核心实现：三池、生命周期状态机、`fc_poll_internal` 服务主循环、`fc_complete_from_nand`、Stripe RAID（全部 `_internal`） |
| `src/fc/fc_interface.c` | 对外函数实现：薄壳（参数校验 + 转发 internal） |

```c
/* 全部声明见 fc_interface.h，此处仅示意签名 */
fc_status_t   fc_init(void);                           // 初始化所有资源池和队列
fc_command_t *fc_command_alloc(void);                   // 申请命令，失败返回 NULL
fc_command_t *fc_command_id_to_command(uint16_t id);    // ID→指针转换
fc_descriptor_t *fc_descriptor_id_to_descriptor(uint16_t id);
fc_status_t   fc_command_configure_descriptors(fc_command_t *cmd); // 自动遍历 CW 数量配置 Descriptor 链
fc_buffer_id_t fc_buffer_alloc(void);                   // 申请 Buffer（返 ID）
fc_status_t   fc_buffer_release(fc_buffer_id_t id);     // 释放 Buffer
uint8_t      *fc_buffer_id_to_real_buffer(fc_buffer_id_t); // ID→指针，返回 USER 数据区首地址（跳过 META+CRC 共 8B）
uint8_t      *fc_buffer_get_base(fc_buffer_id_t);       // ID→指针，返回 CW 基址（META 开头，FC 数据路径内部使用）
uint8_t      *fc_buffer_get_meta(fc_command_t *, uint8_t cw_index); // 指定 CW 的 META 指针（地址校验）
fc_status_t   fc_write_cmd_data(fc_command_t *cmd, const void *, uint32_t); // 数据便利：写命令 buffer
fc_status_t   fc_read_cmd_data(fc_command_t *cmd, void *, uint32_t);        // 数据便利：读命令 buffer
uint8_t      *fc_get_cmd_data_ptr(fc_command_t *cmd, uint8_t cw_index);     // 指定 CW 的 USER 指针
fc_status_t   fc_submit(fc_command_t *cmd);             // 提交到 SQ 或 HSQ（根据 high_priority）
fc_status_t   fc_service(void);                         // 调度执行（上层 while(1) 轮询）
fc_status_t   fc_poll_completion(fc_command_status_t *); // 从 CQ 取完成项
fc_status_t   fc_command_release(fc_command_t *cmd);    // 释放命令及关联 Descriptor 链
fc_status_t   fc_get_resource_stats(fc_resource_stats_t *); // 资源使用统计
```

> **移除**：`fc_real_buffer_to_buffer_id()`（指针→ID 反向转换）——FC 数据引用统一走 buffer_id，无调用者，连同其 internal 一并删除。`fc_descriptor_alloc()`（孤儿声明）同步删除。`fc_buffer_alloc()` 保留但标注"⚠️ 手动分配接口，常规路径走 `fc_command_configure_descriptors()`"。

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

### 流程图：命令生命周期（图 1）与 fc_service() 主循环（图 2）

**图 1 — 命令生命周期（FTL 视角）**

```text
FTL 调用                    FC 内部                         NAND 层
─────────────────────────────────────────────────────────────────────
fc_command_alloc()
  └─ 取 command_pool 空槽，状态 ALLOCATED
FTL 填字段: operation/ch/ce/lun/cw/meta/paa
  └─ 如配置 config.high_priority = 1 → 走 HSQ
fc_command_configure_descriptors()
  └─ 按 config.cw 自动分配 (cw+1) 个 desc + buffer，串联链表
fc_write_cmd_data(cmd, data, len)   ← 便捷接口（可选，避免手遍历）
  └─ 遍历 desc 链 memcpy 数据到各 buffer USER 区
fc_submit(cmd)
  └─ fc_submit_internal → 入队 SQ 或 HSQ（不执行）
      ┌────────────── while(1) { fc_service(); } ──────────────┐
      │  单次 fc_service():                                    │
      │    ① nand_dispatch_poll 推进 NAND                      │
      │    ② fc_scheduler_select_chain 取命令                  │
      │    ③ fc_data_path_encode（写命令）                     │
      │    ④ 构建 request → nand_dispatch_submit              │
      └────────────────────────────────────────────────────────┘
fc_poll_completion(&status)
  └─ 从 CQ 取完成项（12B：id/desc_id/module/ecc_bit/status）
FTL 按 status.module 路由处理；遍历 desc 链读每 CW 状态/ECC
fc_command_release(cmd)
  └─ 回收 command + desc 链 + buffer（挂回空闲链表）
```

命令状态机：`FREE → ALLOCATED → QUEUED → ACTIVE → COMPLETED → FREE`（越界直接出错）。

**图 2 — fc_service() 主循环单步**（`fc.c::fc_poll_internal`，每次调用先推进 NAND 再补充新命令）

```text
fc_service()
  │
  ├─ ① nand_dispatch_poll(&nand_completion)
  │     └─ 有命令完成？（返回 != NOT_READY）
  │          ├─ YES → fc_complete_from_nand()     ← 回写 desc + 数据路径 + CQ
  │          │         [CONFIG_FC_RAID] 若完成的是 stripe 数据命令
  │          │           → fc_stripe_release_parity()
  │          │         返回
  │          └─ NO  → 继续 ②
  │
  ├─ ② fc_scheduler_select_chain(&sched_chain)    ← HSQ 优先 / 读饥饿保护
  │     └─ 无命令可取 → 返回（调用方稍后重试）
  │     └─ 取到 → 按 count 分派：
  │
  ├─ 分支 A：count == 1（单命令）
  │     ├─ 校验 QUEUED → 置 ACTIVE
  │     ├─ page write → 写 PAA 到 CW META 区 → fc_data_path_encode()
  │     ├─ raid=ENCODE → fc_stripe_submit()        ← 图 12
  │     ├─ 预解析 desc 链 → request：
  │     │    ├─ 单 plane：cws[0][cw] 顺序填
  │     │    ├─ multi_plane：按 desc.paa 的 plane 分桶填
  │     │    ├─ ERASE：只填 cws[0][0].paa, buffer=NULL
  │     │    └─ SET/GET_FEATURES：buffer = &command->parameter0
  │     └─ nand_dispatch_submit(&request)          ← 入队即返回
  │
  └─ 分支 B：count >= 2（链式命令）
        ├─ 逐条：校验 / 置 ACTIVE / page write 则 encode
        ├─ 构建 reqs[0..count-1]
        └─ nand_dispatch_submit_chain(reqs, count) ← 原子入队
```

**要点**：一次 `fc_service()` 要么推进完成并立即返回，要么提交一批新命令——两件事不交叉。提交只占住目标 LUN，完成由后续 `fc_service()` 推进。

---

## 3. NAND 底层模块详细设计

### 3.1 微操作引擎 (nand_engine)

设计理念：**软件微码（Software Microcode）**。每条 UOP（Micro-Operation）自包含、不查外部表、不含隐式逻辑，模仿企业级 SSD 控制器硬件 micro-sequencer 的设计。

```
企业级硬件微码（Intel / Samsung / Kioxia）:
  INST_0: SET_CMD(0x00)    ← 指令自带命令值
  INST_1: SET_ADDR(col0)   ← 值随指令走
  INST_2: SET_CMD(0x30)    ← 又一个命令，自有值
  INST_3: WAIT_RB#         ← 纯硬件等待
  ...

软件微码（当前实现）:
  { COMMAND,   NAND_CMD_READ_PAGE_SETUP    },  // 自包含
  { COLUMN_0,                            0U  },  // 列地址
  { COLUMN_1,                            0U  },
  { ROW_0,                               0U  },
  { ROW_1,                               0U  },
  { ROW_2,                               0U  },
  { COMMAND,   NAND_CMD_READ_PAGE_CONFIRM },
  { DELAY_US,  NAND_TIME_TWB_US           },  // 显式 tWB
  { WAIT_READY,                          0U  },  // 纯 RB# 轮询
```

三段式演进：

| 阶段 | UOP 数量 | 设计问题 |
|------|---------|---------|
| Phase S 前 | 15 | `COMMAND_PRIMARY/SECONDARY` 查 `operation_map` 表，命令值不直观；隐式延时（tWB/tWHR/tADL）藏在 handler 内；`CHECK_READY` + `uop_index--` hack |
| Phase S 改后 | 13 | 命令统一 `COMMAND` 直写；延时显式 `DELAY_US`；条件跳转 `JUMP_IF_NOT_READY` 替代引擎 hack；WAIT_READY 纯化 |

**UOP 类型**（13 种）：

| UOP | 说明 | 职责 |
|-----|------|------|
| WAIT_READY | 纯 RB# GPIO spin-wait | 不含 tWB/tADL 保护，不查表 |
| COMMAND | 发送命令字节 | operand 直写命令值，自包含 |
| COLUMN_0/1 | 发送列地址 | — |
| ROW_0/1/2 | 发送行地址 | operand 选 row[] 索引（多 plane） |
| DATA_IN | 数据写入 NAND | — |
| DATA_OUT | 从 NAND 读取数据 | — |
| DATA_OUT_STATUS | 读取状态字节 | 纯读 DQ，不含延时 |
| JUMP_IF_NOT_READY | 条件跳转微指令 | operand=相对偏移步数，引擎直接处理 |
| CHECK_FAIL | 检查 Fail 位 | nand_status & 0x01，失败终止序列 |
| DELAY_US | 固定微秒延时 | delay_us(operand)，无硬件依赖 |
| END | 序列终止 | — |

**删除的 UOP**：

| 删除的类型 | 替代方案 | 原因 |
|-----------|---------|------|
| ~~COMMAND_PRIMARY~~ | `COMMAND` + operand 直写 | 查 operation_map 表，不直观、不自包含 |
| ~~COMMAND_SECONDARY~~ | `COMMAND` + operand 直写 | 同上 |
| ~~READ_STATUS~~ | `COMMAND(70h) → DELAY_US → DATA_OUT_STATUS` | 复合操作拆为三步单责 UOP |
| ~~CHECK_READY~~ | `JUMP_IF_NOT_READY(-3)` | 引擎不再需要 uop_index-- hack |

**与主控微码对比**：

| 特性 | 企业主控硬件微码 | 软件微码（当前） |
|------|----------------|----------------|
| 命令值来源 | 微指令内嵌 | `COMMAND` operand 直写 |
| 固定延时 | 延时微指令 | `DELAY_US` |
| 条件跳转 | `JUMP_IF_NOT_READY` | 引擎内置，相对偏移 |
| tWB 保护 | 显式 DELAY 在 CONFIRM 后 | `DELAY_US(tWB)` + `WAIT_READY` |
| tWHR | 显式 DELAY 在 70h 后 | `DELAY_US(tWH)` 在序列中 |
| RB# 轮询 | 纯引脚电平检测 | `WAIT_READY` 纯 spin-wait |
| 引擎层 | 无 UOP 特判 | 除 `WAIT_READY` / `JUMP` 超时外无特判 |
| 无条件跳转 | 有 | 无（当前无需求） |
| 子程序调用 | 有 | 无（当前无需求） |

#### WAIT_READY — 纯 RB# GPIO 轮询

WAIT_READY handler（`nand_fmc_exec_wait_ready`）在 handler 内部完成 RB# GPIO 轮询 + 超时等待，不返回 NOT_READY，**不依赖引擎的 DWT 超时**。

流程（简化后）：
1. 读 RB# GPIO：LOW → NAND busy → 1µs/步 spin-wait 直到 HIGH 或超时
2. RB# HIGH → 直接返回 OK（NAND 已 ready）

**不再包含 tWB 保护**。tWB（CONFIRM 后最长 1µs 等待 NAND 拉低 RB#）已由 UOP 序列中的 `{ DELAY_US, NAND_TIME_TWB_US }` 显式保证。

#### JUMP_IF_NOT_READY — 条件跳转微指令

JUMP_IF_NOT_READY 由引擎直接处理，不走 handler 表（handler 表项为 NULL）：

1. 检查 `nand_status & NAND_STATUS_READY`：ready 则推进到下一 UOP
2. busy 则检查 DWT 超时，未超时则 `uop_index -= operand` 跳回重发 70h 轮询序列

典型的 70h 状态轮询 4 步循环：

```
步 N+0: { NAND_UOP_COMMAND,            0x70 }    // 发 70h
步 N+1: { NAND_UOP_DELAY_US,             1U }    // tWHR
步 N+2: { NAND_UOP_DATA_OUT_STATUS,     0U }     // 读状态
步 N+3: { NAND_UOP_JUMP_IF_NOT_READY,   3U }     // busy 则跳回步 N
```

每次循环完整重发 70h、等 tWHR、读状态、检查 ready，无需引擎层特殊逻辑。

#### CHECK_FAIL — 使用规则

CHECK_FAIL 检查 `completion->nand_status & 0x01`（NAND 状态寄存器的 FAIL bit），
失败则返回 `NAND_RESULT_OPERATION_FAILED`，引擎终止序列。不是所有 OP 都需要它：

| 有 CHECK_FAIL | 原因 |
|-------------|------|
| 所有含 10h/D0h 的程序/擦除操作 | CONFIRM 后 NAND 可能内部烧录/擦除失败 |
| SET_FEATURES（写 ONFI 寄存器） | 写寄存器可能被 NAND 拒绝 |
| RESET | 复位后检查 NAND 状态是否正常 |
| 所有含 30h 的 READ 操作（含 MULTI_PLANE_READ） | 30h 后页加载可能内部出错 |
| INTERNAL_READ_FOR_DATA_MOVE（35h） | 内部搬移读取阶段可能出错 |
| CACHE_READ_SEQ/LAST/RANDOM（31h/3Fh） | Cache 读序列中 NAND 内部可能出错 |

| 无 CHECK_FAIL | 原因 |
|-------------|------|
| GET_FEATURES / READ_ID / READ_PARAM_PAGE / READ_UNIQUE_ID | 读芯片信息/寄存器，不触发内部操作，无"失败"概念 |
| RANDOM_DATA_READ | 05h-E0h 读已加载的页缓冲，不触发 NAND 内部操作 |
| RANDOM_DATA_INPUT | 85h 只写页缓冲，无 CONFIRM，写缓冲不会失败 |
| READ_STATUS / READ_STATUS_ENHANCED | 读状态寄存器本身，查自己无意义 |
| READ_MODE | 发 00h 设置读模式，无内部操作 |
| BLOCK_UNLOCK/LOCK/LOCK_TIGHT/LOCK_STATUS | 锁操作由 NAND 内部逻辑完成，不通过 FAIL bit 报告 |

**例外**：PROGRAM_FOR_INTERNAL_DATA_MOVE 有 CHECK_FAIL，因为它含 85h-addr-1B-10h 的完整编程序列（有 10h CONFIRM）。而 PROGRAM_FOR_INTERNAL_MOVE 是纯 copy-back（无 DATA_IN ），同样有 10h CONFIRM，也有 CHECK_FAIL。

#### 隐式延时显式化

此前 tWB、tWHR、tADL 等固定延时分散在各 handler 内部，UOP 序列不可见。改后全部显式化：

| 隐式延时 | 原位置 | 现位置 |
|---------|--------|--------|
| tWB（CONFIRM→RB# 拉低，1µs） | WAIT_READY handler 内 `delay_us(2U)` | `{ DELAY_US, NAND_TIME_TWB_US }` 在 CONFIRM 后 |
| tWHR（70h→DQ 有效，1µs） | DATA_OUT_STATUS handler 内 `delay_us(twh_us)` | `{ DELAY_US, NAND_TIME_TWH_US }` 在 70h 后 |
| tADL（11h 切换 plane，1µs） | WAIT_READY 伪装（RB# 不变） | `{ DELAY_US, 1U }` 在 11h 后 |

#### 引擎执行模型

```
nand_engine_execute()  —— 同步模式，阻塞直到完成
nand_engine_start()    —— 异步模式，初始化状态机
nand_engine_step()     —— 异步模式，推进一步，返回 NOT_READY 表示需继续轮询
```

### 流程图：nand_engine 微码状态机（图 6）

`nand_engine_step()` 的核心循环（每 step 可消费多个 UOP）：

```text
step(state)
  │
  └─ for(; uop_index < MAX_UOPS(64);)
       │
       ├─ uop.type == END
       │    └─ 置 active=0，返回 transfer_status（命令结果）
       │
       ├─ uop.type >= UOP_COUNT → INVALID_ARGUMENT
       │
       ├─ uop.type == JUMP_IF_NOT_READY（引擎直接处理，不经 handler）
       │    ├─ nand_status & READY bit6 → uop_index++（推进）
       │    └─ busy → 双超时判定：
       │         DWT elapsed > busy_timeout_us  或
       │         wait_polls ≥ busy_timeout_us
       │           ├─ 超时 → active=0，返回 TIMEOUT
       │           └─ 未超时 → uop_index -= operand（回跳），返回 NOT_READY
       │
       ├─ 其余 UOP → handler = backend->handlers[uop->type]
       │    ├─ handler == NULL → INVALID_ARGUMENT
       │    └─ WAIT_READY 首次进入：记录 DWT 起点（与 JUMP 共用计时）
       │         result = handler(...)
       │           ├─ OK → uop_index++，清零 wait 状态，continue（一步多推进）
       │           ├─ NOT_READY → WAIT_READY 额外双超时检测；让出
       │           └─ 其他 → active=0，返回错误
       │
       └─ 循环跑满无 END → 非法序列
```

**协作式让出**：`NOT_READY` 是"让出点"——引擎不阻塞，上层 `while(NOT_READY) step()`（同步 `nand_engine_execute`）或由 dispatch 轮询推进（异步 `start/step`）。

### 流程图：70h 状态轮询循环（图 7）

几乎所有 uop 序列开头都是这个循环，`operand=3` 表示跳回 3 步前重发：

```text
步 N+0: { COMMAND          0x70 }    ← 发 70h（读状态寄存器）
步 N+1: { DELAY_US           1U }    ← tWHR（70h → DQ 有效）
步 N+2: { DATA_OUT_STATUS   0U  }    ← 读状态字节（含 READY bit6 / FAIL bit0）
步 N+3: { JUMP_IF_NOT_READY 3U  }    ← busy → uop_index -= 3 回跳 N+0
                                        ready → 推进到下一 UOP
        （循环期间 DWT + 轮询计数双超时，超时返回 TIMEOUT）
```

读命令 30h 后的页加载、编程 10h 后的 tPROG、擦除 D0h 后的 tBERS 都靠这个循环轮询到 ready；`CHECK_FAIL` 检查 FAIL bit 失败即终止序列（图 6 的 OPERATION_FAILED 路径）。

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

#### DATA_IN / DATA_OUT — PIO/DMA 双模

DATA_IN（CPU→FMC→NAND，PROGRAM 方向）和 DATA_OUT（NAND→FMC→CPU，READ 方向）handler 支持编译态和运行态双模切换：

| 模式 | 配置 | 行为 |
|------|------|------|
| PIO（默认） | `CONFIG_NAND_DMA=0` | CPU 逐字节 `*config->data = buf[i]` |
| DMA（可选） | `CONFIG_NAND_DMA=1` | 三态非阻塞：①done→OK ②busy→NOT_READY ③首次→启动DMA_IT |

运行态可通过 `g_nand_dma_enable` 全局变量切换（PIO↔DMA，NAND 空闲时安全）。

### 流程图：FMC 后端 UOP 执行（图 8）

`g_nand_fmc_handlers[NAND_UOP_COUNT]` 函数指针表（`src/nand/nand_fmc.c`）：

```text
┌───────────────────────────────────────────────────────────────┐
│ [WAIT_READY]      nand_fmc_exec_wait_ready  RB# GPIO spin-wait │
│ [COMMAND]         nand_fmc_exec_command     写 FMC 命令窗口     │
│ [COLUMN_0/1]      nand_fmc_exec_column_x    写列地址（低/高字节）│
│ [ROW_0/1/2]       nand_fmc_exec_row_x       写行地址（operand   │
│                                               = row[] 索引，多  │
│                                               plane 时选 plane）│
│ [DATA_IN]         nand_fmc_exec_data_in     CPU→FMC→NAND 数据   │
│ [DATA_OUT]        nand_fmc_exec_data_out    NAND→FMC→CPU 数据   │
│ [DATA_OUT_STATUS] nand_fmc_exec_data_out_status 直读 70h 状态   │
│ [JUMP_IF_NOT_READY] NULL（由引擎直接处理，见图 6/7）            │
│ [CHECK_FAIL]      nand_fmc_exec_check_fail  检查 FAIL bit       │
│ [DELAY_US]        nand_fmc_exec_delay_us    delay_us(operand)   │
│ [END]             （无 handler，引擎处理）                       │
└───────────────────────────────────────────────────────────────┘
         ▲
         │ handler 通过三个 volatile 窗口访问 FMC：
         │   *config->data / *config->command / *config->address
         └── 上下文 nand_fmc_config_t（Phase V 已数组化 per (ch,ce,lun)）

WAIT_READY handler 细节（图 6 与之共用超时字段）：
  读 RB# GPIO
    ├─ LOW  → while LOW: polls++；polls ≥ busy_timeout_us 超时；delay_us(1)
    └─ HIGH → 直接返回 OK（极短 busy / 无 busy）
  tWB 保护不在 handler 内——由 uop 序列中 CONFIRM 后的 {DELAY_US, 1} 显式保证

DATA_IN / DATA_OUT 双模：
  PIO（默认）   CPU 逐字节写/读 FMC 窗口
  DMA（可选）   三态非阻塞：①done→OK ②busy→NOT_READY ③首次→启动 DMA_IT
```

**DMA 不可用结论**：STM32H743 的 FMC NAND 控制器**没有专用 DMA 请求线**（DMAMUX1 映射表 0~137 中无 FMC/NAND 条目）。DMA memory-to-memory 模式下 DMA2 无法与 FMC NAND 等待状态握手（FMC 在 AXI 域，DMA2 在 AHB 域）。FMC NAND 只能走 PIO 逐字节访问。如需 DMA + 硬件 ECC NAND 控制器，建议换 NXP i.MX RT 系列（GPMI 控制器内置 DMA + BCH）。

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

### 流程图：nand_dispatch poll 两阶段（图 9）

`nand_dispatch_poll()`（round-robin 遍历通道，从上次游标续走）：

```text
for channel = cursor..(cursor+CH_NUM-1)
  │
  ├─ Phase 1：推进本通道各 CE/LUN 的活跃 chain（每 LUN 一 chain）
  │    for ce, for lun:
  │      chain = &chains[ch][ce][lun];  chain->count==0 → 跳过
  │      task = chain->tasks[chain->cursor]
  │
  │      ── phase == ISSUED（LUN 内部操作中）──
  │         min_busy_us 未到 → continue（等最短忙期）
  │         到点 → pair_op 切换（LOAD→FETCH / ISSUE→WAIT）
  │               → req_table[op](task) → nand_fc_start → phase=FETCH
  │
  │      ── 内层 while(1)（started 判定）──
  │         started==0 → req_table 构建 request → nand_fc_start
  │                      （ISSUE 型置 phase=ISSUE；否则 FETCH）
  │                      NOT_READY → break；同步完成则消费队列 entry
  │         started!=0 → transaction.active ? nand_fc_step
  │                                     : 用已存的 execution_status
  │         NOT_READY → break（等下一轮）
  │
  │      ── 命令完成后的处理链 ──
  │         phase==ISSUE → task_complete → phase=ISSUED
  │                        → 记 DWT 起点 → break（不清 LUN busy）
  │         CW chaining（多 CW READ）→ task_complete
  │           若 (普通读/OTP读/随机读) 且还有下一 CW：
  │             cw_index++ → operation=OP_RANDOM_DATA_READ
  │             → 列地址 = cw_index×(PAGE_SIZE/4) → nand_fc_start
  │         page_buffer 拆回（CACHE 读 / OP_READ_PAGE_FETCH /
  │                         MP_WITH_DATA_OUT 三种拆页）
  │         chain->cursor+1 < chain->count → cursor++ → 构建+start 下一命令
  │                                        （不清 LUN busy！）
  │         整链结束 → *completion = task->completion
  │                    chain->count=0, phase=IDLE → 返回（有完成）
  │
  ├─ Phase 2：从本通道 FIFO 出队新命令
  │    queue->count!=0：
  │      取 head entry（command_id/ce/lun）
  │      目标 chain 忙 → continue（entry 留队）
  │      出队 → task 已由 submit 预加载
  │           → req_table 构建 → nand_fc_start → 置相位
  │           同步完成 → task_complete
  │           phase==ISSUE → 切 ISSUED，不返回 completion
  │           count>1（链）→ 交给 Phase 1
  │           单命令立即完成 → 返回 completion（返回点之二）
  │
  └─ 全部遍历完无完成 → 返回 NOT_READY
```

**要点**：Phase 1 每轮先推进已有事务，Phase 2 才启动新命令。链执行期间 LUN busy，HSQ 无法插入（保证 Read Retry 原子性）。

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

FC 数据路径分两个方向：

- **写路径 (encode)**：FC 内部在 NAND Program 前调用，对 FTL 完全透明
  - CRC 计算（`config.crc == FC_CRC_ENABLE` 时）
  - Random 加扰
  - ECC 编码
  - RAID XOR 编码（如配置）
- **读路径 (decode)**：分为两阶段
  - **Stage 1**：FC 内部在 NAND Read 完成后、CQ push 前自动执行
    - ECC 解码
    - 全部成功 → 解扰 → CRC 校验
    - 有 CW 失败 → 不解扰（供 RAID XOR 恢复在 scrambled domain 进行）
  - **Stage 2**：FTL 触发的 RAID 恢复路径

### 5.1 编码流程 (Page Pack)

```
User Data (512B/CW)
  → [META] 写入 per-CW META（desc->meta，存 buffer[0..4]）
  → [CRC Engine] CRC-32 计算（仅 USER 区域，写入 CRC 字段 [4:8]）
  → [Random Engine] 加扰（LFSR，种子 PAA × program_sequence，操作 520B 数据区）
  → [LDPC Engine] 编码（520B 数据 → 8B parity，写入 [520:528]）
  → （数据命令累积到 PLANE_NUM-1 条后）
  → [Stripe RAID] fc_data_path_generate_stripe_parity() 跨命令 XOR → parity 写入独立 Plane
  → 数据命令 + parity 命令一起提交 nand_dispatch → Multi-Plane PROGRAM
```

RAID XOR 不再覆盖最后一个 CW 的用户数据。Plane 数为 2 的幂（2/4/8），Parity 独立占用最后一个 Plane：

| Plane 数 | 数据命令 | parity 命令 |
|----------|---------|------------|
| 2        | D0 (Plane 0) | P0 (Plane 1) |
| 4        | D0/D1/D2 (Plane 0-2) | P0 (Plane 3) |
| 8        | D0~D6 (Plane 0-6) | P0 (Plane 7) |

FTL 只提交 PLANE_NUM-1 条数据命令（config.raid=2），FC 内部自动：
1. 累积 stripe 成员（PLANE_NUM=2 时在单次 fc_service() 内同步完成）
2. 内部分配 parity 命令资源（command + desc 链 + buffer）
3. 调用 fc_data_path_generate_stripe_parity() 做跨命令 XOR
   - XOR 数据命令各 CW 的 scrambled data（520B）到 parity buffer
   - ECC 编码 parity buffer
4. parity 命令不经过 META/CRC/Random（parity buffer 内容已在 scrambled domain，META 为 XOR 结果，只靠 ECC 保护完整性）
5. 数据命令 + parity 命令以 Multi-Plane PROGRAM 一次提交 nand_dispatch
6. 完成时 parity 命令资源自动释放，FTL 只感知数据命令的 completion

RAID XOR 在 ECC 编码之后进行，保证了 parity CW 同样经过 ECC 保护。所有步骤在 FC 内部完成。
META（LBA）和 CRC 在加扰前写入，受后续加扰和 ECC 的完整保护。

### 流程图：CW 布局与数据处理范围（图 3）

每个 Page（2112B）= 4 个 CW × 528B。Buffer Pool 里每个 CW 分层管理：

```text
┌─────────────────────────── 528 字节 CW ───────────────────────────┐
│ [0:4]    [4:8]    [8:520]            [520:528]                    │
│  META     CRC     USER（512B）          PARITY                    │
│ FC 管理   CRC-32   FTL 可见区          ECC 引擎生成               │
└──────────────────────────────────────────────────────────────────┘
   ▲                  ▲                    ▲
   fc_buffer_get_base  fc_buffer_id_to_real_buffer   （对 FTL 透明）
   （数据路径入口）      （对外接口，+8B 偏移）

模块处理范围：
  CRC    → 512B  [8:520]     算 CRC 写 [4:8]（只保护 USER）
  加扰   → 520B  [0:520]     LFSR 全数据区（ECC 会覆盖 parity，加扰它白做）
  ECC    → 520B→8B parity    读 [0:520]，写 [520:528]（BCH m=13 t=4）
  RAID   → 528B 全 CW        跨命令 XOR（需保护 parity 自身）
```

**为什么**：ECC 输入是 520B（META+CRC+USER），parity 总是写在 `data + data_length`。加扰作用域不含 parity——因为 ECC 编码后 parity 才生成，加扰了 parity 在解码前会被 ECC 覆盖。

### 流程图：写路径编码（图 4，Page Pack）

`fc_data_path_encode()`（FC 内部，NAND Program 提交前，对 FTL 透明）：

```text
FTL 数据填入各 CW buffer USER 区 [8:520]
  │
  ├─ Stage 0  META：desc->meta（LBA 派生）memcpy 到 buffer[0:4]
  ├─ Stage 1  CRC：对 USER 512B 算 CRC-32 → 写 buffer[4:8]
  ├─ Stage 1  加扰：seed = f(PAA)，LFSR 变换 buffer[0:520]
  ├─ Stage 2  ECC：BCH 编码 520B → 8B parity → 写 buffer[520:528]
  └─ (Stage 3 RAID 由 stripe 路径单独处理，不在此函数内)
        │
        ▼
   数据命令累积到 PLANE_NUM-1 条（raid=ENCODE 时）
        │
        ├─ fc_data_path_generate_stripe_parity()    ← 图 12
        │    XOR 各数据命令对应 CW 的 [0:520]（scrambled domain）
        │    → parity buffer → ECC 编码 parity
        └─ fc_stripe_submit()：数据命令 + parity 命令拼成一个
             Multi-Plane PROGRAM 提交 nand_dispatch
```

**要点**：META 和 CRC 在加扰前写入，因此受加扰+ECC 的完整保护；parity CW 的 META 是 XOR 结果，只靠 ECC 保护完整性，不重算 CRC/加扰。

### 5.2 解码流程 (Page Unpack)

```
NAND 读取完成
  │
  ├─ Stage 1 / FC 内部（CQ push 前）:
  │   ┌──────────────────────────────────────┐
  │   │ 逐 CW: ECC 解码                      │
  │   │   ├─ 全部成功 → 解扰所有 buffer       │
  │   │   │              → CRC 校验          │
  │   │   │              → push CQ (status=READY/CRC_ERROR)
  │   │   └─ 有 CW 失败 → 不解扰 buffer       │
  │   │                → push CQ (status=UNC)
  │   └──────────────────────────────────────┘
  │
  └─ Stage 2 / FTL 触发（poll CQ 后）:
      遍历 desc 链，发现 status=UNC
      ┌──────────────────────────────────┐
      │ 调用 fc_data_path_raid_recover() │
      │   ├─ RAID XOR 重建 failed CW     │
      │   ├─ ECC 重试                   │
      │   ├─ 解扰所有 CW                │
      │   └─ 更新 desc->status = READY  │
      └──────────────────────────────────┘
      │
      └─ 仍失败 → BBT / read retry / 其他恢复策略
```

### 流程图：读路径解码（图 5，Page Unpack）

两阶段，以 CQ 为分界：

```text
NAND 读取完成
  │
  ├─ Stage 1 / FC 内部（CQ push 前）：fc_data_path_process_read()
  │   Pass 1  ECC 解码每个 CW：
  │     ├─ 全部成功 → 各 desc->status = READY，记录 ecc_bits
  │     └─ 有 CW 不可纠 → desc->status = UNC，
  │                       保持 ECC-corrected + scrambled（供 RAID 恢复）
  │                       提前返回 IO_ERROR，跳过 Pass 2~4
  │   Pass 2  解扰所有 CW（seed = f(PAA)）
  │   Pass 3  CRC 校验 → 不匹配置 CRC_ERROR
  │   Pass 4  META 读回校验（lba_to_cw_meta 对比）→ 不匹配置 MATA_ERROR
  │
  ├─ 完成状态经 fc_complete_from_nand() 聚合进 CQ（status/desc_id/ECC）
  │
  └─ Stage 2 / FTL 触发：fc_data_path_raid_recover()（status==UNC 时）
        ├─ fc_collect_decode_buffers 收集全 CW buffer+PAA
        ├─ fc_raid_recover：XOR 重建 failed CW（scrambled domain 内）
        ├─ ECC 重试：重建后仍不可纠 → 返回 IO_ERROR（硬件不可恢复）
        ├─ 解扰所有 CW
        └─ 全部 desc->status = READY
              │
              └─ 仍失败 → BBT / read retry / 其他恢复策略
```

**关键约束**：RAID XOR 在 scrambled domain 进行——ECC 失败时其余 CW 处于"已纠错但仍加扰"，重建结果与被恢复 CW 同 domain，重建后再统一解扰。加解扰对 FTL 完全透明。

**关键设计约束**：
- RAID XOR 在 scrambled domain 进行：ECC 失败时，其他 CW 处于 "ECC 已纠正但仍加扰" 状态，XOR 重建的结果与被恢复 CW 处于同一 domain，重建后 ECC 解码再统一解扰
- 加解扰完全对 FTL 透明：FTL 从不直接调用 Random 引擎
- RAID 恢复后的解扰统一在 `raid_recover()` 内部完成，不暴露到 FTL

### 5.3 CW 布局

每个 Page（2112 字节）= 4 个 CW × 528 字节，CW 布局：

```
[0:4]   META    — FC 内部管理（LBA 等 META 信息）
[4:8]   CRC     — FC 计算写入（CRC-32 覆盖 USER 区域）
[8:520] USER    — FTL 填入的用户数据（512 字节）
[520:528] PARITY — ECC 编码引擎生成的校验数据

FTL 通过 fc_buffer_id_to_real_buffer() 拿到的指针指向 [8]，
直接读写 512 字节 USER 数据，不感知其余区域。

FC 内部通过 fc_buffer_get_base() 获取 CW 基址（[0]），
数据路径（加扰/ECC/RAID）操作全 528 字节。
```

数据路径编码流程（加扰/ECC/RAID）：

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
| `fc_ldpc_engine` | ✅ 已完成 | BCH(m=13, t=4) 编码/解码；QC-LDPC 硬判决编码/解码（可选） |
| `fc_raid_engine` | ✅ 已完成 | per-LUN START/ACCUMULATE/END 序列；fc_raid_recover 静态恢复 |
| `fc_data_path` | ✅ 已完成 | 写路径：Random→ECC→RAID；读路径：ECC→条件解扰（FC 内部）/ RAID 恢复（FTL 触发） |

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

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | 多 LUN 并发：nand_engine 异步步进 + 每 LUN 独立状态机 | ✅ |
| Phase 2 | DMA 数据传输：FMC 数据窗口改为 DMA 驱动，中断完成通知 | ❌ STM32H7 限制 |
| Phase 3 | 中断驱动：R/B# EXTI 替代轮询 WAIT_READY | ✅ |
| Phase 4 | LDPC 硬解码实现 + BCH ECC 移植 | ✅ |
| Phase 5 | RAID 跨 Plane XOR 恢复 + 读路径管线拆分 | ✅ |

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

> **注**：`fc_real_buffer_to_buffer_id(ptr)`（指针 → ID）已随对外接口拆分移除——FC 数据引用统一走 buffer_id，无调用者（详见 §2.9 移除说明）。

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
- `fc_real_buffer_to_buffer_id()`（已随拆分移除，见 §2.9）

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

### 流程图：完成回写与 CQ 组装（图 13）

`fc_complete_from_nand()`（NAND 完成 → FC 侧收尾）：

```text
nand_dispatch_completion_t（command_id / execution_status / nand_status /
                            cw_status[4] / ecc_bit_count[4] / failed_desc_id）
  │
  ├─ Step 1：回写 descriptor（双重交付之一）
  │     command->desc_id != INVALID 时：
  │     遍历 desc 链（total_cw = multi_plane? plane×cw : cw+1）：
  │       desc->status   = cw_status[cw]
  │       desc->ecc_bits = ecc_bit_count[cw]
  │       desc_id = desc->next_id
  │
  ├─ Step 2：数据路径（仅 Page Read 命令）
  │     fc_is_page_read() → fc_data_path_process_read()   ← 图 5
  │     （即使 IO_ERROR，已解码的 desc status 已更新）
  │
  ├─ Step 3：组装 fc_command_status_t（12B CQ 条目）
  │     command_id = nand completion 的 command_id
  │     desc_id    = failed_descriptor_id（首个 UNC 的 desc_id）
  │     module     = command->module（直接路由，无需查命令）
  │     Page Read → 从 descriptor 读 ECC 解码后的 status/ecc
  │                 （前 4 CW 填 slot；多 plane 后续 CW 累加 OR）
  │     （非 Page Read → 直接透传 NAND 状态）
  │
  └─ Step 4：fc_complete_internal → CQ 入队
        CQ 满 → 强制标记 COMPLETED 防泄漏 + 返回 IO_ERROR（fatal 背压）
```

**为什么双重交付（CQ 快照 + Descriptor 回写）**：

| 场景 | 需要 Desc 回写的原因 |
|------|---------------------|
| Read Retry | 要知道哪个 CW 失败 → desc_id 标记 + status |
| RAID Recovery | 逐 CW 检查解码状态（CQ 只有命令级） |
| 长链命令完成，CQ 已出队 | CQ 状态丢失，Descriptor 保留状态 |
| 错误注入/调试 | 状态持续可查 |

`module` 字段覆盖全部 FC 使用者：HOST_IO / FTL / GC / READ_RECOVERY / METADATA / BBT / CHECKPOINT / WEAR_LEVELING。

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

---

## 16. 命令链执行设计 (Phase D — 续)

### 16.1 问题背景

Read Retry 流程要求三个命令严格依次执行，中间不能被任何其他命令插入：

```
SET_FEATURES(偏移电压)  →  RETRY READ  →  SET_FEATURES(恢复电压)
  └── 命令 1            └── 命令 2       └── 命令 3
```

同一 LUN 的 HSQ 命令（如 GC 强制擦除）可能在任意时刻插入，打乱 Read Retry 的顺序，导致电压未被恢复。

### 16.2 方案对比

实现"不可打断的命令序列"有两种候选方案：

#### 方案 A：chain_next_id（在 poll 路径中动态跳转）

在 `nand_dispatch_task_t` 中增加 `uint16_t chain_next_id`。当前命令完成后 poll 检查该字段，非 INVALID 则不清 LUN busy、直接加载下一命令。

```c
if (task->chain_next_id != FC_COMMAND_ID_INVALID) {
    // 不清 LUN busy, 从 command pool 加载下一命令
    task = load_from_command_id(task->chain_next_id);
    nand_fc_start(…);
    continue;  // 继续等
}
```

#### 方案 B：per-LUN chain buffer（加 task 维度，选中方案）

将 `g_nand_dispatch_tasks[ch][ce][lun]` 扩展为 `g_nand_dispatch_chains[ch][ce][lun].tasks[NAND_DISPATCH_CHAIN_DEPTH]`，submit_chain 预加载整链，poll 通过 cursor 自动推进。

```c
// 当前命令完成
if (chain->cursor + 1U < chain->count) {
    chain->cursor++;           // 推进到下一命令
    task = &chain->tasks[chain->cursor];
    nand_fc_start(…);           // 不清 LUN busy, 不返回 completion
    continue;
}
// 整链结束
*completion = task->completion;
chain->count = 0;               // 释放 LUN
```

#### 对比

| 维度 | A: chain_next_id | B: per-LUN chain buffer ✅ |
|------|-----------------|--------------------------|
| 修改范围 | poll 完成路径 + task | nand_dispatch_submit / poll / scheduler |
| poll() 纯度 | ❌ 被污染，要感知链跳转 | ✅ 纯粹，与单命令代码路径一致 |
| 整链可见性 | ❌ 链只有部分在 task 中 | ✅ tasks[0..N-1] 完整可见 |
| 调试友好 | ❌ 链信息分散 | ✅ dump chain buffer 即得全貌 |
| watchdog / QoS | ⚠️ 难区分"长命令"还是"链" | ✅ 知道整链长度 |
| 扩展性 | 链内跳转逻辑与 CW chaining 交织 | 与 CW chaining 正交（各自独立循环） |
| 资源预先评估 | ❌ 走一步看一步 | ✅ 提前知道占多少链 slot |
| 队列深度需求 | 1 就够 | 需能容整链（当前 64 足够） |
| **企业类似方案** | 少量消费级固件使用 | 主流：模拟硬件 sequencer 槽位 |

**选择方案 B 的理由**：
1. poll() 不被链语义污染——它看到的仍然是"逐命令执行"，仅多了"连续执行不返回"的能力
2. 与 CW chaining 正交——CW 链在外层 for(;;) 循环中处理，命令链在内层 while(1) 中推进，互不干扰
3. 架构与企业级 SSD 固件一致——本质是在 FC 层用软件模拟硬件 sequencer 的槽位机制

### 16.3 整体架构

```
FTL / Read Recovery (构造 chain 命令)
  │ config.chain=1 → config.chain=0
  ▼
SQ: [cmd_A(SET_FEATURES, chain=1)] [cmd_B(READ, chain=1)] [cmd_C(SET_FEATURES, chain=0)]
                              │
                              ▼
fc_scheduler_select_chain():
  1. 从 SQ 弹出 cmd_A
  2. 检测 cmd_A.config.chain == 1 → 连续 peek 后续命令
  3. 收集 cmd_B(chain=1), cmd_C(chain=0) → 验证同一 ch/ce/lun
  4. 一次性弹出整链 → chain={ids: [A, B, C], count=3}
                              │
                              ▼
fc_poll_internal():
  for i=0..2:
    fc_command_validate(command_i)  → 合法性检查
    resolve descriptor chain        → 预解析 PAA/buffer
    reqs[i] = { … }
  nand_dispatch_submit_chain(reqs, 3)
                              │
                              ▼
nand_dispatch_submit_chain():
  原子检查: LUN chain->count==0 AND 队列空间够 3 条
  chain->tasks[0] = cmd_A   ← 预加载
  chain->tasks[1] = cmd_B   ← 预加载
  chain->tasks[2] = cmd_C   ← 预加载
  chain->count = 3, cursor = 0
  入队 command_id_A, command_id_B, command_id_C
                              │
                              ▼
nand_dispatch_poll():
  Phase 1 ─── cursor=0 → step tasks[0] → 完成 → cursor++ → start tasks[1]
  Phase 1 ─── cursor=1 → step tasks[1] → 完成 → cursor++ → start tasks[2]
  Phase 1 ─── cursor=2 → step tasks[2] → 完成 → count=0 → 返回 completion
                                ↑ 链执行期间 LUN busy, HSQ 无法插入
```

### 16.4 数据结构

#### per-LUN chain buffer

```c
// nand_dispatch_internal.h

#define NAND_DISPATCH_CHAIN_DEPTH 4U

typedef struct {
    nand_dispatch_task_t tasks[NAND_DISPATCH_CHAIN_DEPTH];
    uint8_t              count;   /* 0=空闲, 1=单命令, 3=3条链 */
    uint8_t              cursor;  /* 当前执行 tasks[cursor] */
} nand_dispatch_lun_chain_t;
```

#### 调度器链结构

```c
// fc_scheduler.h

#define FC_SCHEDULER_CHAIN_MAX NAND_DISPATCH_CHAIN_DEPTH /* = 4U，与 dispatch 链缓冲深度一致 */

typedef struct {
    fc_command_id_t ids[FC_SCHEDULER_CHAIN_MAX];
    uint8_t         count;  /* 0=空, 1=单命令, 2+=链 */
} fc_scheduler_chain_t;
```

`nand_dispatch_task_t` 新增 `ce` 和 `lun` 字段（此前只有 `ch`，单 CE/LUN 时代硬编码），供后续多 CE/LUN 使用。

### 16.5 submit_chain 原子性

`nand_dispatch_submit_chain()` 提供 ACID 语义的链提交：

```
原子性检查:
  if (chain->count != 0)  → LUN 忙, 拒绝
  if (queue->count + count > NAND_CHANNEL_QUEUE_DEPTH)  → 队列空间不够, 拒绝
  // 注意：两个条件同时满足才通过，不会出现"部分入队"的情况

预加载:
  for i = 0..count-1:
    tasks[i] ← 从 requests[i] 拷贝
  chain->count = count, cursor = 0

入队:
  for i = 0..count-1:
    queue->entries[tail++] = requests[i].command_id
```

关键语义：**队列空间不足整链时返回 NOT_READY，无一进入队列。** 调用者下次轮询时再试。

### 16.6 poll 链推进逻辑

poll() 中 Phase 1 stepping 循环内的链推进：

```c
/* 内层 while(1): 单命令完成 → CW chaining → 命令链推进 → 继续 step */
while (1) {
    task = &chain->tasks[chain->cursor];
    result = nand_fc_step(&task->transaction, &nand_completion);
    if (result == NAND_RESULT_NOT_READY) break;

    /* CW 级 chaining (多 CW READ) */
    for (;;) {
        nand_dispatch_task_complete(task, result, &nand_completion);
        if (CW 全部完成) break;
        task->cw_index++;
        task->operation = OP_RANDOM_DATA_READ;
        nand_fc_start(…);
    }

    /* 命令链推进 */
    if (chain->cursor + 1U < chain->count) {
        chain->cursor++;           /* ← 不下发 completion, 不清 LUN busy */
        task = &chain->tasks[chain->cursor];
        g_nand_dispatch_req_table[task->operation](task);
        nand_fc_start(…);
        continue;                  /* while(1) 继续 step 下一命令 */
    }

    /* 整链结束 → 唯一出口 */
    *completion = task->completion;
    chain->count = 0;
    return result;
}
```

### 16.7 调度器链检测

`fc_scheduler_select_chain()` 取代原有的 `fc_scheduler_select()` 成为 FC 的主入口：

```c
fc_status_t fc_scheduler_select_chain(fc_scheduler_t *scheduler, fc_scheduler_chain_t *chain)
{
    /* Phase 1: HSQ 优先 — HSQ 命令不参与链 */
    if (HSQ 非空) {
        找第一个目标不忙的命令;
        弹出一条 → chain->count = 1;
        返回;
    }

    /* Phase 2: 普通 SQ */
    for (遍历 SQ) {
        if (目标忙) { 旋转跳过; continue; }

        弹出队头 → chain->ids[0];

        if (队头.config.chain == 0) {
            /* 单命令 */
            chain->count = 1;
            return;
        }

        /* chain == 1: 收集同 target 的后续命令 */
        while (chain_len < FC_SCHEDULER_CHAIN_MAX) {
            next = peek(0);
            if (next.ch/ce/lun != 当前) break;
            if (next.config.chain == 0) { 加入; break; }
            加入; /* chain=1, 继续 */
        }
        chain->count = chain_len;
        return;
    }
    return 队列空;
}
```

**连续性保证**：链中命令必须在 SQ 中连续（由 FTL/Read Recovery 在提交时保证），中间不能插入其他 LUN 的命令。`scheduler_collect_chain` 会在发现 ch/ce/lun 变化时截断链，回退到单命令模式。

### 16.8 文件变更

| 文件 | 说明 |
|------|------|
| `include/nand/nand_dispatch.h` | 新增 `NAND_DISPATCH_CHAIN_DEPTH`、`nand_dispatch_submit_chain()` 声明 |
| `src/nand/nand_dispatch_internal.h` | 新增 `task.ce/lun`、`nand_dispatch_lun_chain_t` |
| `src/nand/nand_dispatch.c` | `g_nand_dispatch_tasks[][][]` → `g_nand_dispatch_chains[][][].tasks[]`；新增 `nand_dispatch_task_load()`；`submit()` 改用 chain；新增 `submit_chain()`；`poll()` Phase 1 chain 推进 + Phase 2 出队适配 |
| `include/fc/fc_scheduler.h` | 新增 `FC_SCHEDULER_CHAIN_MAX`、`fc_scheduler_chain_t`、`fc_scheduler_select_chain()` |
| `src/fc/fc_scheduler.c` | 新增 `fc_scheduler_select_chain()` 链识别 + 整链弹出 |
| `include/fc/fc_queue.h` | 新增 `fc_submission_queue_peek()`、`fc_submission_queue_pop_n()` |
| `src/fc/fc_queue.c` | 实现 `peek()`、`pop_n()` |
| `src/fc/fc.c` | `fc_poll_internal()` 改用 `fc_scheduler_select_chain()`，增加链式命令构建 + `submit_chain` 路径 |

### 16.9 使用方式（FTL / Read Recovery）

FTL 或 Read Recovery 模块提交 Read Retry 链时：

```c
/* 提交三条链式命令到 SQ */
command_A.config.chain = 1;  // SET_FEATURES(偏移)
command_B.config.chain = 1;  // RETRY_READ
command_C.config.chain = 0;  // SET_FEATURES(恢复) — chain=0 表示链结束

fc_command_submit(&command_A);  // 按 A → B → C 顺序连续 submit
fc_command_submit(&command_B);
fc_command_submit(&command_C);
```

调度器自动识别 `chain` 标志，dispatch 层自动按序执行。HSQ 在整个链执行期间无法插入该 LUN。

### 16.10 与 Phase D (函数指针表) 的关系

| 子阶段 | §15 函数指针表 | §16 命令链 |
|--------|---------------|-----------|
| 状态 | ✅ 已完成代码+文档 | ✅ 已完成代码+文档 |
| 解决的问题 | if-else 链 → O(1) 查表 | 命令原子性 + HSQ 防插入 |
| 改动范围 | nand_dispatch 内部 | nand_dispatch + fc_scheduler + fc |
| 代码编译 | ✅ 通过 | ✅ 通过 |

### 流程图：命令链执行（图 11 配套：调度器 select_chain）

`fc_scheduler_select_chain()` 每次取一批命令（chain）交给 `fc_poll_internal`：

```text
select_chain(scheduler, chain)
  │
  ├─ Phase 1：HSQ 非空？
  │    遍历 HSQ 找目标 (ch,ce,lun) 不忙的命令
  │      ├─ 找到 → pop → chain.count=1（HSQ 不参与链式）→ 返回
  │      └─ 全忙 → 返回 NOT_READY
  │
  └─ Phase 2：普通 SQ
       前置扫描：consecutive_write_count ≥ 预算(3) 时
                  扫描 SQ 是否存在读命令（sq_has_read）
                  （全写队列 → 不触发保护 → 避免死锁）
       for index in SQ:
         目标忙 → 旋转到队尾，继续
         读饥饿保护：写计数 ≥3 且队头非读且 sq_has_read → 旋转，继续
         pop 队头 → chain.ids[0]；更新写计数（读→0 / 非读→++）
         config.chain==0 → chain.count=1 → 返回
         config.chain==1 → 收集链：
           while chain_len < 4：
             peek 队头
             不同 ch/ce/lun → 截断（回退单命令）
             同 target → pop 加入；chain==0 → 结束；chain==1 → 继续
       queue 空 → QUEUE_EMPTY / 全忙 → NOT_READY
```

**旋转语义**：`pop(队头)+push(队尾)` 只动 head/tail 指针，Command Pool 内容不变，被旋转的命令一圈后回到队头——是轮转调度，不是丢弃。


---

## 17. Multi-Plane 操作支持与行地址数组重构 (Phase E)

### 17.1 动机

MT29F4G08ABADAWP 支持 2 Plane 操作（Multi-Plane READ/PROGRAM/ERASE），但现有架构将 `cw_paa/buffers` 设计为平面一维数组 `[CW_NUM]`(4)，无法支持 Multi-Plane 场景中 `CW_NUM × PLANE_NUM = 8` 的 CW 总数。

同时，`nand_command_request_t.row` 为单值 `uint32_t`，Multi-Plane 操作需要发送两组行地址（Plane 0 和 Plane 1），而地址周期中的 plane bit 差异仅在于 BA6。

### 17.2 数据维度扩展

#### 方案对比

| 方案 | 做法 | 评价 |
|------|------|------|
| A: 展平 cw_count | `cw_paa[CW_TOTAL_MAX]` 展平为 8 个，`cw_count` 语义变为"总 CW 数" | ❌ 丢失 plane 维度信息，每 plane CW 数量不明确 |
| B: 加 plane 维度（选中） | `cw_paa[PLANE_NUM][CW_NUM]`，`cw_count` 仍为"每 plane CW 数"，`plane_count` 标明 plane 数 | ✅ 维度正交，语义清晰 |

选中方案 B 后各层数据结构的变化：

```c
// nand_dispatch_request_t — 请求
nand_dispatch_cw_info_t cws[PLANE_NUM][CW_NUM];  // [plane][cw]

// nand_dispatch_task_t — 任务
nand_paa_t cw_paa[PLANE_NUM][CW_NUM];
uint8_t   *cw_buffers[PLANE_NUM][CW_NUM];
uint8_t    cw_count;     // 每 plane CW 数
uint8_t    plane_count;  // plane 数 (1/2)

// nand_dispatch_completion_t — 完成
uint8_t cw_status[CW_TOTAL_MAX];      // 8 个（vs 原有 4 个）
uint8_t ecc_bit_count[CW_TOTAL_MAX];
```

`fc_cmd_conf_t.cw` 位宽不变（仍为 2 位，最大表示 4 CW/plane），Multi-Plane 通过独立 OP 码区分。

### 17.3 行地址数组重构

#### 方案对比

| 方案 | 做法 | 评价 |
|------|------|------|
| A: 新增 uop 类型 | 加 `NAND_UOP_ROW_0_SECOND`/`ROW_1_SECOND`/`ROW_2_SECOND` 三个新 uop 类型 | ❌ 2 plane 加 3 个，4 plane 加 9 个，不可扩展 |
| B: **operand 选择索引** | `row` 改为 `row[NAND_PLANE_MAX]`，ROW_0/1/2 handler 通过 `uop->operand` 选择 row 索引 | ✅ 零新增 uop 类型，任意 plane 数不改 uop |

选中方案 B：

```c
// nand_fc_interface.h / nand_engine.h
uint32_t row[NAND_PLANE_MAX];  // row[0]=plane0, row[1]=plane1, ...

// nand_fmc.c — ROW handler 改动
*config->address = (uint8_t)(request->row[uop->operand]);
```

uop 序列中通过 operand 选择 plane：

```c
{ NAND_UOP_ROW_0,  0U },  // operand=0 → row[0] (Plane 0)
{ NAND_UOP_ROW_0,  1U },  // operand=1 → row[1] (Plane 1)
```

单 plane 操作 operand 全部为 0，完全向后兼容。

### 17.4 OP 码设计： WITH_DATA_OUT / WITHOUT_DATA_OUT

#### 设计原因

NAND 读取命令（00h→地址→30h→tR）执行后，数据被加载到 NAND 内部页缓冲。**是否立即输出数据**是两个独立场景：

| 场景 | 做法 | 适用 |
|------|------|------|
| 立即输出 | 30h→tR→**Dout** | 正常读路径、单页读完即用 |
| 延迟输出 | 30h→tR→**无 Dout** | Multi-Plane 读、配合 05h-E0h 随机读、Cache Read 链 |

两种场景执行完全相同的地址序列和 tR 等待，差异仅在 uop 序列末尾是否包含 `NAND_UOP_DATA_OUT` 一条。

为此，单 Plane 和多 Plane 的读操作均拆分为两个 OP 码：

| OP 码 | 值 | 说明 |
|-------|----|------|
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | 12 | 单 Plane 读 + 立即 Dout（原 `OP_NORMAL_READ_PAGE`） |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | 34 | 单 Plane 读，仅加载到页缓冲 |
| `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT` | 29 | 双 Plane 读，加载到两页缓冲 |
| `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | 30 | 双 Plane 读 + 立即双页 Dout |

PROGRAM 和 ERASE 不存在"延迟执行"场景，故不分拆。

#### uop 序列差异示例

```c
/* WITH_DATA_OUT: 末尾含 DATA_OUT */
{ NAND_UOP_COMMAND, 30h },
{ NAND_UOP_DELAY_US, 1U  },   /* tWB */
{ NAND_UOP_WAIT_READY,   },
{ NAND_UOP_DATA_OUT,     },   /* ← 立即输出 */
{ NAND_UOP_COMMAND, 70h  },   /* 70h 状态轮询 */
{ NAND_UOP_DELAY_US, 1U  },   /* tWHR */
{ NAND_UOP_DATA_OUT_STATUS },
{ NAND_UOP_JUMP_IF_NOT_READY, 3U },  /* busy 则跳回 70h */
{ NAND_UOP_CHECK_FAIL,     },
{ NAND_UOP_END,            },

/* WITHOUT_DATA_OUT: 末尾不含 DATA_OUT */
{ NAND_UOP_COMMAND, 30h },
{ NAND_UOP_DELAY_US, 1U  },   /* tWB */
{ NAND_UOP_WAIT_READY,   },
{ NAND_UOP_COMMAND, 70h  },   /* 70h 状态轮询（跳过 DATA_OUT） */
{ NAND_UOP_DELAY_US, 1U  },   /* tWHR */
{ NAND_UOP_DATA_OUT_STATUS },
{ NAND_UOP_JUMP_IF_NOT_READY, 3U },
{ NAND_UOP_CHECK_FAIL,     },
{ NAND_UOP_END,            },
```

### 17.5 Multi-Plane 操作 OP 码

| OP 码 | 值 | uop 序列 | data_length |
|-------|----|---------|-------------|
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | 12 | 70h轮询→00h→C1C2R1R2R3→30h→tWB→tR→Dout→70h轮询→CHECK_FAIL | PAGE_SIZE |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | 33 | 70h轮询→00h→C1C2R1R2R3→30h→tWB→tR→70h轮询→CHECK_FAIL | PAGE_SIZE |
| `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT` | 29 | 70h轮询→00h→C1C2R1R2R3(P0)→00h→C1C2R1R2R3(P1)→30h→tWB→tR→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | 30 | 70h轮询→00h→C1C2R1R2R3(P0)→00h→C1C2R1R2R3(P1)→30h→tR→06h→C1C2R1R2R3(P0)→E0h→Dout(P0)→06h→C1C2R1R2R3(P1)→E0h→Dout(P1)→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_PROGRAM_PAGE_MULTI_PLANE` | 31 | 70h轮询→80h→C1C2R1R2R3(P0)→Din(P0)→**11h→tADL→80h**→C1C2R1R2R3(P1)→Din(P1)→10h→tWB→tPROG→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_ERASE_BLOCK_MULTI_PLANE` | 32 | 70h轮询→60h→R1R2R3(P0)→60h→R1R2R3(P1)→D0h→tWB→tBERS→70h轮询→CHECK_FAIL | 0 |

> **Multi-Plane Cache 操作**：MT29F4G08ABADAWP 硬件不支持 Multi-Plane Cache Read/Program，无对应 OP 码。

### 17.6 内部搬移 OP 码：片上 Copy-Back

内部数据搬移利用 NAND 的 Read-Modify-Write 能力，将数据从源页读到 NAND 内部缓存寄存器后直接写入目标页，无需经过 Host 总线。MT29F4G08ABADAWP 的 ONFI copy-back 时序为：

```
00h + addr → 35h         // READ_FOR_INTERNAL_DATA_MOVE：源页读入缓存，tR 后 ready
85h + addr → 10h         // PROGRAM_FOR_INTERNAL_MOVE：缓存数据写入目标页，tPROG 后 ready
两种场景均有三个 OP 码支持内部操作：

| OP 码 | 值 | uop 序列 | 说明 |
|-------|----|---------|------|
| `OP_READ_FOR_INTERNAL_DATA_MOVE` | 19 | `70h→Status→00h→C1C2R1R2R3→35h→tR→70h→Status` | 源页读入 NAND 内部缓存 |
| `OP_PROGRAM_FOR_INTERNAL_MOVE` | 20 | `70h→Status→85h→C1C2R1R2R3→10h→tPROG→70h→Status` | 纯 copy-back，无 DATA_IN（NAND 内部转发） |
| `OP_PROGRAM_FOR_INTERNAL_DATA_MOVE` | 10 | `70h→Status→85h→C1C2R1R2R3→1B Data→10h→tPROG→70h→Status` | 写 1 字节替换后写回（含 DATA_IN） |

关键区别：

| 特性 | `PROGRAM_FOR_INTERNAL_MOVE` | `PROGRAM_FOR_INTERNAL_DATA_MOVE` |
|------|---------------------------|--------------------------------|
| DATA_IN | 无 | 1 字节 |
| 用途 | GC 纯搬移 | GC + ECC 纠正（替换损坏 CW 数据） |
| 等效可拆解 | 不可拆（移即是原子） | RANDOM_DATA_INPUT + 10h |
| `has_data` | 1（分配 desc chain 提供 PAA 映射） | 1 |
| `data_length` | 0 | 1 |

### 17.7 关键命令码：11h

`NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH (0x11)` 在 ONFI 标准中定义为 Multi-Plane COPYBACK READ 命令，在多 Plane PROGRAM 时序中起到切换 Plane 的作用：

```
80h → C1C2R1R2R3 → Din(Plane0 全页) →
11h → C1C2R1R2R3 → Din(Plane1 全页) →       ← 11h 切换 Plane
10h → tPROG（双 Plane 同时烧录）
```

### 17.7a 数据缓冲扩展

```c
#define PAGE_BUFFER_SIZE (PAGE_SIZE * PLANE_NUM)  // 4224 字节
static uint8_t g_nand_channel_page_buffer[CH_NUM][PAGE_BUFFER_SIZE];
```

单 Plane 操作只用首 `PAGE_SIZE` 字节，多 Plane 操作写入 `PAGE_BUFFER_SIZE` 字节。

req_multi_plane_program handler 将各 Plane 各 CW 的数据拼接到 page_buffer：

```c
for (plane = 0; plane < PLANE_NUM; plane++) {
    for (cw = 0; cw < task->cw_count; cw++) {
        memcpy(&page_buffer[plane * PAGE_SIZE + cw * (PAGE_SIZE / CW_NUM)],
               task->cw_buffers[plane][cw], PAGE_SIZE / CW_NUM);
    }
}
```

读完成后 poll 中拆回各 plane 各 CW buffer 的逻辑同理。

### 17.8 NAND 数据传输模式：中转 vs 直写

NAND 读写操作根据输出/输入方式不同，采用中转或直写两种数据传输模式。

#### 直写模式（Direct Write/Read）

NORMAL_READ_PAGE_WITH_DATA_OUT 利用 NAND 的 `05h-E0h` 随机列读能力实现无中转的 CW 级读取：

```
第1步: 00h-addr-30h → DATA_OUT(528B, CW0) → 直接送入 cw_buffers[0][0]
第2步: 05h-col(528)-E0h → DATA_OUT(528B, CW1) → 直接送入 cw_buffers[0][1]
第3步: 05h-col(1056)-E0h → DATA_OUT(528B, CW2) → 直接送入 cw_buffers[0][2]
第4步: 05h-col(1584)-E0h → DATA_OUT(528B, CW3) → 直接送入 cw_buffers[0][3]
```

每次 DATA_OUT 只有 528 字节（一个 CW），`req_read_page` 将 `data_length` 设为 `PAGE_SIZE / CW_NUM`，poll 中的 chaining 循环自动将后续 CW 的 operation 改为 `OP_RANDOM_DATA_READ`（05h-E0h），逐个 CW 读取到各自 buffer。

NORMAL_PROGRAM_PAGE 虽然也是单页操作，但 NAND 不支持 80h 后逐个 CW 写入再确认——必须一次送完整页数据再发 10h。因此走中转模式。

#### 中转模式（Page Buffer Bounce）

| 操作 | 原因 | 中转数据量 |
|------|------|-----------|
| NORMAL_PROGRAM_PAGE | NAND 编程必须连续输入整页 | 2112 → page_buffer |
| PROGRAM_PAGE_CACHE | 同上 | 2112 → page_buffer |
| PROGRAM_PAGE_MULTI_PLANE | 双 Plane 数据需拼接后两次 DATA_IN | 4224 → page_buffer |
| READ_PAGE_CACHE_SEQ/RANDOM/LAST | 31h/3Fh 一次性输出整页，无中途 05h-E0h 机会 | 2112 → page_buffer → 拆分回 CW |
| READ_PAGE_MULTI_PLANE_WITH_DATA_OUT | 双页连续输出 4224 字节 | 4224 → page_buffer → 拆分回 Plane/CW |

#### 对比

| | 直写模式 | 中转模式 |
|----|---------|---------|
| 拷贝次数 | 0 次额外拷贝 | 1 次 memcpy（读写各 1 次） |
| 适用场景 | 单个 CW 可独立寻址的操作 | 必须连续传输整页/多页的操作 |
| 实现方式 | poll chaining 循环 + RANDOM_DATA_READ | req_xx handler → page_buffer → 拆分 |
| 例 | NORMAL_READ_PAGE_WITH_DATA_OUT | 所有 Program、Cache Read、Multi-Plane |

### 17.9 文件变更

| 文件 | 说明 |
|------|------|
| `include/nand/nand_config.h` | 新增 `NAND_PLANE_MAX`、`CW_TOTAL_MAX`、`PAGE_BUFFER_SIZE` |
| `include/nand/nand_engine.h` | `nand_command_request_t.row` → `row[NAND_PLANE_MAX]` |
| `include/nand/nand_fc_interface.h` | `nand_fc_request_t.row` → `row[NAND_PLANE_MAX]` |
| `include/nand/onfi_1_0.h` | 新增 5 个 Multi-Plane OP 码 + `NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH`；拆读 OP 码为 WITH/WITHOUT_DATA_OUT 双版本；`OP_COUNT` 从 32 → 35 |
| `src/nand/onfi_1_0.c` | 新增 5 组 uop 序列 + 映射表 5 项 |
| `src/nand/nand_fmc.c` | ROW_0/1/2 handler 通过 `uop->operand` 索引 `row[]` |
| `src/nand/nand_mt29f4g08abadawp.c` | 默认读接口改为 `WITH_DATA_OUT` 版本 |
| `include/nand/nand_dispatch.h` | `cws[CW_NUM]` → `cws[PLANE_NUM][CW_NUM]`；`cw_status[4]` → `[CW_TOTAL_MAX]`；新增 `plane_count` |
| `src/nand/nand_dispatch_internal.h` | `cw_paa/buffers[CW_NUM]` → `[PLANE_NUM][CW_NUM]`；新增 `plane_count` |
| `src/nand/nand_dispatch.c` | `page_buffer` 尺寸扩展；`nand_dispatch_task_load()` 适配双 plane 拷贝；`completion_init` 适配 `CW_TOTAL_MAX`；CW chaining 条件增加 `WITHOUT_DATA_OUT` |
| `src/nand/nand_dispatch_tables.c` | 新增 5 个 req handler + completion handler + 两张函数指针表各加 5 项 |
| `src/fc/fc.c` | `cws[plane][cw]` 适配新维度 |

### 流程图：Stripe RAID 写提交（图 12，raid=ENCODE 的 page write）

`fc_stripe_submit()`（PLANE_NUM=2：单数据命令 + 单 parity 命令）：

```text
FC 拿到 raid=ENCODE 的 page write 命令（config.raid=2）
  │
  ├─ ① fc_command_alloc_internal → parity 命令
  │      配置：op=OP_PROGRAM_PAGE_MULTI_PLANE, raid=NORMAL
  │      PAA：同 page/block/lun/cw，plane = PLANE_NUM-1
  │
  ├─ ② fc_command_configure_descriptors_internal(parity)
  │      分配 parity desc 链 + buffer
  │
  ├─ ③ parity desc.meta ← 数据命令对应 CW 的 meta
  │
  ├─ ④ fc_data_path_generate_stripe_parity(data_ptrs, 1, parity)
  │      逐 CW：parity_buf = XOR(各数据命令该 CW 的 [0:520])
  │      先 memcpy 作初值，再按 32-bit 字异或
  │      然后 ECC 编码 parity buffer（[520:528] 由 ECC 生成）
  │
  ├─ ⑤ 构建 Multi-Plane dispatch request：
  │      op=OP_PROGRAM_PAGE_MULTI_PLANE, plane_count=2
  │      cws[0][cw] = 数据命令 desc（PAA+buffer 基址）
  │      cws[1][cw] = parity 命令 desc
  │      command_id = 数据命令 id（完成路由回数据命令）
  │
  ├─ ⑥ controller->stripe_parity_for_data[data_id] = parity_id
  │
  └─ ⑦ nand_dispatch_submit(mp_req)
           │
           ▼  Multi-Plane PROGRAM uop 序列：
           80h+地址+Din(P0) → 11h(tADL) → 80h+地址+Din(P1) → 10h → tWB
           → tPROG → 70h 轮询 → CHECK_FAIL
           （双 plane 数据在 page_buffer 拼接：P0@[0..2111]、P1@[2112..4223]）
           │
           ▼
        完成 → fc_complete_from_nand 识别数据命令
              → fc_stripe_release_parity(parity_id)  ← 自动回收 parity 资源
              FTL 只感知数据命令的 completion
```

失败路径：任何一步失败 → `fc_stripe_release_parity()` 回滚已分配资源 + 数据命令以 error completion 返回。

---

## 18. 多通道支持现状与硬件限制

### 18.1 当前硬件拓扑

```
STM32H743 FMC 总线（单条并行总线）
  └─ MT29F4G08ABADAWP (1 CE × 1 LUN × 2 Plane × 2048 Block × 64 Page)
```

当前物理连接：**1 通道 × 1 片选 × 1 LUN**。

### 18.2 软件架构的多通道准备

调度层和 dispatch 层已按 `[CH_NUM][CE_NUM][LUN_NUM]` 多维数组编写：

```c
// nand_config.h
#define CH_NUM (1U)
#define CE_NUM (1U)
#define LUN_NUM (1U)

// nand_dispatch.c — 各维度数组
static nand_channel_queue_t       g_nand_channel_queues[CH_NUM];
static nand_dispatch_lun_chain_t  g_nand_dispatch_chains[CH_NUM][CE_NUM][LUN_NUM];
static uint8_t                    g_nand_channel_page_buffer[CH_NUM][PAGE_BUFFER_SIZE];

// fc_scheduler.c — per-LUN busy 表
uint8_t busy[CH_NUM][CE_NUM][LUN_NUM];
```

`nand_dispatch_poll()` 遍历所有通道：
```c
for (count = 0; count < CH_NUM; count++) {
    channel = (uint8_t)((g_nand_channel_cursor + count) % CH_NUM);
    for (ce = 0; ce < CE_NUM; ce++)
        for (lun_idx = 0; lun_idx < LUN_NUM; lun_idx++) { ... }
}
```

FC 命令包含通道字段：
```c
// fc_cmd_conf_t
DWORD ch  : 2;   /* 目标通道号 */
DWORD ce  : 2;   /* 目标片选号 */
DWORD lun : 2;   /* 目标 LUN 号 */
```

这些代码在多通道场景下**逻辑正确**——改 `CH_NUM` 后数组和循环会自动扩展。

### 18.3 当前不支持多通道的模块

以下模块在**多通道维度**的现状（FMC 后端已 Phase V 数组化，但仅 (0,0,0) 实例被使用）：

| 模块 | 现状 | 问题 |
|------|------|------|
| `nand_fmc.c` — FMC 后端 | ✅ 已数组化 `g_nand_fmc_instances[CH][CE][LUN]` + `nand_fmc_init_instance()`（Phase V） | 仅 (0,0,0) 实例被 `nand_init()` 配置；第二片 NAND 需额外 CE 引脚配置 |
| `nand_rb_interrupt.c` — RB 中断 | 单 EXTI6（PD6） | 第二片 NAND 的 RB 引脚没有 EXTI 通道可接 |
| `nand_dma.c` — DMA | 单对 `DMA2_Stream0/1` | 两个通道同时传输时无额外 Stream 可用 |
| `nand_engine_state_t` — 引擎状态 | 不含 `ch`/`ce`/`lun` 字段 | step 推进时不知道自己在哪个通道上（依赖 transaction 的 backend 透传） |

### 18.4 硬件层面的本质限制

STM32H743 的 **FMC（Flexible Memory Controller）是共享总线控制器**，不是多通道 NAND 控制器：

| 对比项 | FMC（当前） | 专用 NAND 控制器（企业级/MPU） |
|--------|------------|-----------------------------|
| 总线结构 | **单条并行总线**，CLE/ALE/RE/WE 共用 | **每条通道独立**命令/地址/数据线 |
| 并行度 | ❌ 不能——总线一次只能操作一个设备 | ✅ 各通道独立执行，命令并发 |
| RB 引脚 | GPIO 或 EXTI，**数量有限** | 控制器内置，每通道独立采样 |
| DMA 通道 | 需要手动分配，有限 | 通常内置 per-channel DMA |
| 多片连接方式 | CE 分时复用总线（片选切换） | 独立通道，无总线竞争 |

**FMC 接多片 NAND 只能 CE 分时复用**：

```
FMC 总线 (地址/数据/控制线共用)
  ├── CE0 → NAND #0
  ├── CE1 → NAND #1
  └── CE2 → NAND #2

任何时候只能有一片 NAND 在使用总线。
```

不是真正的"多通道并行"，只是"多片分时复用一条总线"。

### 18.5 如果要支持多通道，需要改什么

| 改动项 | 说明 | 工作量 |
|--------|------|--------|
| `nand_fmc_config_t` 数组化 | `config[CH_NUM]`，每通道独立 FMC 地址 + CE + RB | ✅ 已完成（Phase V `g_nand_fmc_instances[CH][CE][LUN]`，但仅 (0,0,0) 实例使用） |
| `nand_engine_backend_t` 数组化 | `backend[CH_NUM]`，每通道独立 handler table | ✅ 已完成（`nand_fmc_init_instance` 每实例一份 backend） |
| `nand_fmc_exec_wait_ready` 多实例 | 每个 backend 有独立 RB 中断资源 | 中（需多 EXTI） |
| DMA 资源化 | 每通道独立 DMA Stream 或排队传输 | 中 |
| `nand_engine_state_t` 加通道 id | step 中知道自己操作的通道 | 小（现依赖 transaction->backend 透传） |
| FMC 总线仲裁 | 多通道共享总线时的冲突避免 | 大（需加锁或排队） |

### 18.6 验证结论

| 层面 | 多通道支持程度 | 能否验证 |
|------|--------------|---------|
| FC 调度器 + busy 表 | ✅ 代码完整，改 `CH_NUM` 即可 | ❌ 单通道硬件 |
| dispatch 队列 + chain buffer | ✅ 代码完整 | ❌ 单通道硬件 |
| dispatch poll 遍历 | ✅ 代码完整 | ❌ 单通道硬件 |
| FMC 后端 | ✅ 已数组化（仅 (0,0,0) 实例配置） | ❌ 多实例无法（单 NAND） |
| RB 中断 | ❌ 单 EXTI | ❌ 无法 |
| DMA | ❌ 单对 Stream | ❌ 无法 |
| 引擎状态机 | ⚠️ 无通道 id，靠 transaction->backend 透传 | ⚠️ 单通道可运行 |

**结论**：调度层和 dispatch 层的多通道架构是真实的。FMC/NAND 引擎层需要改造才能真正支持多通道。

### 18.7 多 LUN 并发：OP 码 ISSUE/WAIT/FETCH 拆分设计

#### 18.7.1 问题

当前所有 OP 码是原子序列——从 70h 轮询到命令发出到等待完成到数据输出，一条龙锁住 LUN：

```
OP_NORMAL_READ_PAGE_WITH_DATA_OUT:
  70h轮询 → 00h-addr-30h → tR(WAIT_READY) → DATA_OUT → 70h轮询 → CHECK_FAIL → END
  ├─────────────── LUN 独占整个序列 ────────────────┤
```

多 LUN 场景下，LUN 0 在 tR 期间（25µs），dispatch 只能干等，不能趁隙切到 LUN 1 发命令。

#### 18.7.2 企业做法：三阶段拆分

企业级主控将每个 NAND 操作拆为三段独立原语，调度器可交叉执行：

| 原语 | 描述 | 阻塞？ | 典型耗时 |
|------|------|--------|---------|
| **ISSUE** | 发命令 + 地址（或 + 数据），不等完成 | ❌ µs 级 | ~1-5µs |
| **WAIT** | 等 R/B# 或 70h 状态轮询到 ready | ✅ 阻塞 | tR 25µs / tPROG 200µs |
| **FETCH** | 读数据或检查最终状态 | ✅ 阻塞 | ~10µs（读整页）|

例如双 LUN 并发读取：

```
时间 ──────────────────────────────────────────────────────▶

LUN 0: ISSUE(00h-addr-30h) → [tR 25µs]
                              ↑ 总线空闲，可切 LUN
LUN 1:                       ISSUE(00h-addr-30h) → [tR 25µs]
                                                   ↑ 两 LUN 都 busy
LUN 0:                                               ← FETCH(Dout)
LUN 1:                                                   ← FETCH(Dout)
```

总耗时 ≈ tR(25µs) + FETCH(10µs)，而不是 2 × (tR+FETCH) = 70µs。

#### 18.7.3 现有雏形

当前架构已有部分拆分迹象：

| OP 码 | 接近的企业原语 | 差距 |
|-------|--------------|------|
| `OP_READ_PAGE_CACHE_START` | READ_ISSUE | 末尾有 70h 轮询，没做到"发完立即返回" |
| `OP_READ_PAGE_WITHOUT_DATA_OUT` | READ_ISSUE | 同上，含 tR + 70h 轮询 |
| `OP_READ_PAGE_CACHE_SEQ` | READ_FETCH | 结合了 WAIT + FETCH，没拆分 |
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | 未拆分 | 整条锁住的原子序列 |

#### 18.7.4 拆分方案

每个读/写/擦操作拆为两个 OP 码：

| 原 READ OP | 拆为 ISSUE | 拆为 WAIT+FETCH |
|-----------|-----------|----------------|
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | `OP_READ_PAGE_LOAD` | `OP_READ_PAGE_FETCH` |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | `OP_READ_PAGE_LOAD` | —（已加载，FETCH 由 RANDOM_DATA_READ 完成）|
| `OP_NORMAL_PROGRAM_PAGE` | `OP_PROGRAM_PAGE_ISSUE` | `OP_PROGRAM_PAGE_WAIT` |
| `OP_ERASE_BLOCK` | `OP_ERASE_BLOCK_ISSUE` | `OP_ERASE_BLOCK_WAIT` |

ISSUE 型 OP 的 UOP 序列只包含"发命令+地址+数据+CONFIRM"，**不含 WAIT_READY 和 70h 轮询**，执行完立即返回（µs 级），不阻塞 LUN。

WAIT/FETCH 型 OP 的 UOP 序列只包含 WAIT_READY + 70h 轮询 + DATA_OUT + CHECK_FAIL。

#### 18.7.5 调度器层面影响

| 层面 | 改动 |
|------|------|
| dispatch_poll Phase 1 | 执行完 ISSUE 后不清 LUN busy，记录"等待完成"状态，继续其他 LUN |
| dispatch_poll Phase 1 | WAIT/FETCH 只对"等待完成"的 LUN 执行，ready 后清除 LUN busy |
| fc_scheduler | 需识别 ISSUE/Wait/FETCH 类型，ISSUE 型不排队直接插入 |
| chain buffer | 同 LUN 的 ISSUE→WAIT 自动组成链，中间不可被其他命令插入 |

#### 18.7.6 当前是否支持并发（→ Phase V 已实现）

| 层面 | 支持程度 | 原因 |
|------|---------|------|
| dispatch 数据结构 | ✅ 就绪 | `g_nand_dispatch_chains[CH][CE][LUN]` 多维 |
| FMC 硬件总线 | ⚠️ 命令级可切 | 发完命令释放总线，WAIT_READY 卡住（硬件限制） |
| **OP 码层** | ✅ Phase V | 新增 6 个 ISSUE/WAIT/FETCH OP 码 + uop 序列 |
| **调度器层** | ✅ Phase V | poll phase 机（ISSUE→ISSUED→FETCH）+ LUN 状态切换 |
| **FMC 后端** | ✅ Phase V | per-(CH,CE,LUN) 实例数组化 |

### 流程图：LUN 相位机（图 10，Phase V 多 LUN 并发）

```text
        submit / Phase 1 启动
        （ISSUE 型命令）
                │
                ▼
        ┌───────────────┐
        │   LUN_PHASE_IDLE    │  count==0 空闲
        └───────┬───────────┘
                │ started==0 → nand_fc_start（ISSUE uop）
                ▼
        ┌───────────────┐
        │ LUN_PHASE_ISSUE    │  发完命令+地址(+数据)+CONFIRM，立即返回
        └───────┬───────────┘  （µs 级，不阻塞总线）
                │ step 完成 → task_complete → 记录 DWT 起点
                ▼
        ┌───────────────┐
        │ LUN_PHASE_ISSUED   │  LUN 内部操作中（tR 25µs / tPROG 200µs /
        └───────┬───────────┘   tBERS ~1ms）。此时 dispatch 可切其他 LUN！
                │ min_busy_us 到点（DWT elapsed）→ pair_op 切换
                ▼
        ┌───────────────┐
        │ LUN_PHASE_FETCH    │  执行配对 WAIT/FETCH：WAIT_READY + DATA_OUT
        └───────┬───────────┘   + 70h 轮询 + CHECK_FAIL
                │ step 完成 → task_complete → 整链结束？
                ▼
            count=0（IDLE）→ 返回 completion，LUN 空闲
```

并发收益示例（双 LUN）：

```text
时间 ──────────────────────────────────────────────────────▶
LUN 0: ISSUE(00h-addr-30h) → [tR 25µs]          ← 总线释放，可切 LUN
LUN 1:                       ISSUE(00h-addr-30h) → [tR 25µs]
LUN 0:                                               ← FETCH(Dout)
LUN 1:                                                   ← FETCH(Dout)
总耗时 ≈ tR + FETCH，而非 2×(tR+FETCH)
```

配对 OP 码（`nand_operation_map_t.pair_op`）：LOAD↔FETCH、ISSUE↔WAIT；原子 OP 的 pair_op 取自身（不切分）。多 LUN 场景 `fc_poll_internal` 自动把 splittable 命令拆成 ISSUE（`nand_operation_is_splittable`）。

### 17.10 设计演进

| 迭代 | 问题 | 方案 |
|------|------|------|
| v1 | 单 plane 数组 `cw_paa[CW_NUM]` 不能支持 multi-plane | 展平为 `cw_paa[CW_TOTAL_MAX]` |
| v2 | 展平后丢失 plane 语义，`cw_count` 意义模糊 | 加 plane 维度 `cw_paa[PLANE_NUM][CW_NUM]` |
| v3 | ROW_0/1/2 handler 固定读 `request->row`，多 plane 发第二组地址需新 uop | `row`→`row[NAND_PLANE_MAX]`，operand 选索引 |
| v4 | 读命令只有一种：30h 后必出数据 | 拆为 WITH/WITHOUT_DATA_OUT 双版本 |

---

## 19. ONFI 时序模式切换

### 19.1 问题背景

`nand_init()` 在 FMC 初始化时配置了保守时序，随后通过 SET_FEATURES(01h) 将 NAND 芯片切换到高速 Mode 4（tRC=20ns），但 **FMC 硬件时序寄存器从未更新**，FMC 始终跑在低速时序，浪费了 Mode 4 的速度提升。

### 19.2 控制宏

```c
// nand_config.h
#define CONFIG_NAND_TIMING_MODE_4 (0U)
```

| 值 | 模式 | 说明 |
|----|------|------|
| 0（默认） | 低速 | NAND 默认 Mode 0（tRC=50ns），FMC 保守时序。用于功能验证初期 |
| 1 | 高速 | NAND 切 Mode 4（tRC=20ns），FMC 重新初始化为匹配快速时序 |

### 19.3 两套时序参数

```c
// nand_mt29f4g08abadawp.c
static const nand_fmc_timing_t g_nand_timing_slow = { 70U, 5U, 10U, ... };  // Mode 0
static const nand_fmc_timing_t g_nand_timing_fast = { 20U, 5U,  5U, ... };  // Mode 4
```

### 19.4 ONFI → FMC 换算

```c
static void nand_fmc_timing_to_pcc(const nand_fmc_timing_t *onfi,
                                    FMC_NAND_PCC_TimingTypeDef *pcc);
```

| FMC 寄存器 | ONFI 参数 | 公式 |
|-----------|-----------|------|
| `SetupTime` | max(tCLS, tALS, tCS) | /5ns 向上取整 |
| `WaitSetupTime` | max(tRP, tWP) | /5ns 向上取整 |
| `HoldSetupTime` | max(tCH, tALH, tCLH, tDH) | /5ns 向上取整 |
| `HiZSetupTime` | tRHZ | /5ns 向上取整 |

FMC kernel clock = 220 MHz，tHCLK ≈ 4.55ns，取整为 5ns 计算。

### 19.5 nand_init() 双路径

低速模式：
```
HAL_NAND_Init(保守) → RESET/READ_ID/校验 → 跳过 Mode 4 → 完成
  NAND: Mode 0（tRC=50ns）
  FMC:  保守时序
```

高速模式：
```
HAL_NAND_Init(保守) → RESET/READ_ID/校验 → SET_FEATURES(Mode 4)
  → nand_fmc_timing_to_pcc(fast) → HAL_NAND_Init(快速) → nand_fmc_configure(fast) → 完成
  NAND: Mode 4（tRC=20ns）
  FMC:  快速时序
```

### 19.6 已知限制

`nand_fmc_timing_to_pcc()` 当前用 `/5ns` 简单除法向上取整，未考虑 FMC 建立/保持时间的余量。如需更精确的时序适配，需引入 FMC 时序模型根据 HCLK 频率动态计算。

---

## 20. 读饥饿保护（Read Starvation Protection / QoS）

### 20.1 问题

调度器（`fc_scheduler`）对读写命令一视同仁。写命令（PROGRAM, tPROG≈300μs）或擦除命令（ERASE, tBERS≈1ms）连续占用 LUN 时，后续的读命令可能被长时间阻塞，导致**读饥饿**。

### 20.2 控制宏

```c
// fc_scheduler.h
#define FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET (3U)
```

连续执行 N 条非读命令后，触发读优先调度。执行一条读命令后计数器清零。

### 20.3 实现

#### 读操作识别

`fc_scheduler_op_is_read()` 识别 16 种读相关操作，非读操作（PROGRAM/ERASE/SET_FEATURES 等）受饥饿保护约束。

#### select_chain() SQ 遍历逻辑

```
① 前置扫描：count ≥ 3 时扫描 SQ 是否存在读命令（sq_has_read）
② 旋转拦截：count ≥ 3 且队头为非读且 sq_has_read=1 → 旋转到队尾
③ 执行：弹出链头命令，更新 count（读→0，非读→++）
```

#### 死锁防护

`sq_has_read` 前置扫描确保**全写队列**场景下不触发保护。如果 SQ 内完全没有读命令，写命令正常执行，不会被无限旋转。

### 20.4 队列操作说明

SQ 是环形 FIFO，旋转通过 `pop(队头) + push(队尾)` 实现。只动 `head/tail` 指针，Command Pool 内容不变。旋转不是丢弃——被旋转的命令会在队列循环一圈后回到队头执行。

### 20.5 场景验证

| 场景 | 行为 |
|------|------|
| 全写队列 [W,W,W,W] | sq_has_read=0 → 不触发 → 全部正常执行 |
| 读写混合，写 ≤ 3 | count < 3 → 不触发 |
| 写 ≥ 3 + 队列有读 | sq_has_read=1 → 后续写旋转 → 读优先执行 → count 归零 |

### 20.6 当前局限性

- 仅在 SQ 层面生效。HSQ 为高优先级通道，跳过读饥饿保护。
- 命令链（chain=1）中无法插队读命令（链原子性），但链结束后下一轮 select 即可切换。
- `CH_NUM=1`、`LUN_NUM=1` 时读饥饿问题不明显，本设计主要为多 LUN/多通道架构预留。
- `CH_NUM=1`、`LUN_NUM=1` 时读饥饿问题不明显，本设计主要为多 LUN/多通道架构预留。

---

## 21. 存储主控行业对比与硬件限制（退而求其次的选择）

### 21.1 引言：为什么需要"退而求其次"

本项目目标平台是 **STM32H743 MCU + 1× MT29F4G08ABADAWP（单通道 SLC NAND）**。这与真正的 SSD 主控（企业级 Samsung/Intel、消费级 Phison/SMI、手机 UFS 控制器）之间存在**本质的硬件代差**——MCU 是通用处理器，不是专用 NAND 控制器。因此本项目的很多设计是对行业方案的"软件模拟 + 妥协"。

本章先给总览对比，再逐项说明"差异 → 硬件限制 → 我们的选择（无奈退而求其次）"。

### 21.2 总体对比总览

| 维度 | 企业级 NVMe SSD | 消费级 NVMe/SATA | UFS 主控 | 本项目（STM32H743） |
|------|---------------|-----------------|---------|-------------------|
| 控制器 | 专用 ASIC + 多核 CPU | 专用 ASIC + CPU | 专用 ASIC + CPU | **通用 MCU（Cortex-M7 480MHz）** |
| DRAM | 数 GB DDR | 256MB~1GB（或 HMB） | 数百 KB~MB SRAM | **0（DRAM-less，仅 1MB SRAM）** |
| NAND 通道 | 8~32 通道 | 4~8 通道 | 1~4 通道 | **1 通道 × 1 CE × 1 LUN** |
| ECC | 硬件 LDPC 引擎（数 Gbps） | 硬件 LDPC/BCH 引擎 | 硬件 LDPC | **软件 BCH(m=13,t=4)/LDPC（CPU 逐 CW）** |
| 数据搬移 | 硬件 DMA 多通道 | 硬件 DMA | 硬件 | **单对 DMA2 Stream（PIO 兜底）** |
| RAID | 硬件 XOR 引擎 | 部分硬件 | 部分 | **软件 XOR（跨命令/跨 Plane）** |
| 加扰 | 硬件 inline（DMA 内） | 硬件 inline | 硬件 | **软件 LFSR/XOR-shift** |
| 调度 | 硬件命令队列 + 固件调度器 | 硬件队列 + 固件 | 硬件 | **纯软件轮询调度器（单线程）** |
| 中断 | 命令完成中断 + 多队列 | 同上 | 同上 | **R/B# EXTI + 主循环轮询混合** |
| 功耗/性能 | 数 W / 数 GBps | 数 W / GBps | 数百 mW | **~百 mA / 单通道 SLC** |

### 21.3 逐维度差异与"退而求其次"的选择

#### 21.3.1 无 DRAM → L2P 全表落 NAND + SRAM Cache

- **行业**：企业/消费级用 DRAM 放完整 L2P 表（4GB SSD ≈ 8MB 表），UFS 用 SRAM cache 部分映射。
- **硬件限制**：本项目 **0 DRAM**（无外部 SDRAM 接 FMC 的可行方案），SRAM 仅 1MB 且被 FTL 元数据/BST 占据 ~220KB。
- **无奈选择**：DRAM-less 方案——L2P 全表存 NAND，SRAM 用 Region Cache（64 条 × 2KB ≈ 128KB），CLOCK 替换。这就是 UFS/消费级入门 SSD（SN580 HMB）同路线。
- **代价**：每次 L2P miss 要读 NAND 一个 region（2KB），随机读延迟增加；全表常驻做不到。

#### 21.3.2 单通道 → 无法并行，只能"分时复用 + 软件并发"

- **行业**：企业级 8~32 通道并行，命令级并发是常态。
- **硬件限制**：STM32H743 的 **FMC 是共享总线控制器**，不是多通道 NAND 控制器。接多片 NAND 只能 CE 分时复用一条总线，任何时候只有一片在传数据（见 §18）。
- **无奈选择**：`CH_NUM=1` 单通道；用 **Phase V ISSUE/WAIT/FETCH 拆分**在软件层模拟"并发"——发完命令（ISSUE）释放总线，等 tR/tPROG 时（WAIT）切其他 LUN。这是对多 LUN 并发的软件近似，**总线本身无法同时传两片 NAND 数据**。
- **代价**：多 LUN 只是"重叠等待时间"，不是真正并行搬移数据；单通道吞吐上限 ~50MB/s（SLC Mode 4）。

#### 21.3.3 无硬件 ECC → 软件 BCH/LDPC

- **行业**：企业级硬件 LDPC 引擎纠错数 Gbps；消费级硬件 BCH/LDPC。
- **硬件限制**：STM32H743 无 NAND ECC 外设；CPU 480MHz 跑软件 ECC。
- **无奈选择**：**软件 BCH(m=13, t=4)**（Linux lib/bch.c 移植，8B parity），可选软件 QC-LDPC 硬判决（码率 0.985，t≈1~2）。
- **代价**：
  - 吞吐低：BCH 编码/解码是 CPU 密集操作，无法达到硬件引擎速率。
  - 纠错能力弱：t=4（SLC 够用，TLC/QLC 远不够）；LDPC 硬判决在高码率下纠错有限，软判决未实现（stub）。
  - **每次读/写都要占用 CPU**，与数据搬移竞争 M7 周期。

#### 21.3.4 无硬件数据搬移 → 单对 DMA + PIO 兜底

- **行业**：每通道独立硬件 DMA，DMA 引擎内完成数据搬移 + ECC inline。
- **硬件限制**：STM32H743 DMA2 仅 Stream0/1 被 FMC 数据窗口占用（一对），多通道/多方向同时搬移不可行。
- **无奈选择**：单对 DMA（memory↔FMC）非阻塞中断模式，DMA 启动失败/禁用时**回退 PIO 逐字节**（见 §3.2）。
- **代价**：单方向同时只有一组 DMA；PIO 路径占用 CPU 逐字节写 FMC 窗口（慢但可用）。无硬件 ECC inline，需 CPU 先 ECC 再 DMA（或反序）。

#### 21.3.5 无硬件加扰 → 软件 LFSR

- **行业**：DMA 引擎内硬件 inline 加扰/解扰（企业级强制 AES-XTS 加密后数据天然随机）。
- **硬件限制**：无。
- **无奈选择**：软件 LFSR（x¹⁶+x¹⁴+x¹³+x¹¹+1）/ XOR-shift / 简单 XOR，编码前 CPU 逐字节异或。
- **代价**：占用 CPU 周期；无 AES 加密（消费/企业级数据安全要求未覆盖）。

#### 21.3.6 无硬件 XOR → 软件 RAID

- **行业**：企业级硬件 XOR 引擎做 RAID 编解码（每秒数十 GB）。
- **硬件限制**：无。
- **无奈选择**：软件 XOR（跨命令/跨 Plane），32bit 字对齐异或。
- **代价**：RAID 恢复是 CPU 密集；`CONFIG_FC_RAID=0` 默认关闭（代码完成未验证，等硬件闭环后开启）。

#### 21.3.7 无硬件命令队列 → 软件轮询调度

- **行业**：硬件多队列（SQ/CQ）+ 固件调度器，命令完成中断驱动。
- **硬件限制**：MCU 无专用 NAND 命令队列。
- **无奈选择**：软件 SQ/HSQ/CQ 环形队列 + 单线程 `fc_service()` 轮询 + R/B# EXTI 中断辅助（Phase F/G 已实现部分）。
- **代价**：单线程轮询 CPU 占用高；读饥饿保护（§20）是软件 QoS 的近似；无多队列优先级硬件支持（HSQ 为软件近似）。

#### 21.3.8 无硬件页缓冲 → 软件 page_buffer 拼接

- **行业**：控制器内部 SRAM 页缓冲 + 硬件页拼接（multi-plane 数据自动拼接）。
- **硬件限制**：FMC 是并行总线，无 NAND 控制器页缓冲硬件。
- **无奈选择**：软件 `g_nand_channel_page_buffer`（noncacheable 段）+ req_* handler 手动拼接/拆页（见 §17.7a）。
- **代价**：多 plane 数据要 CPU memcpy 拼接/拆回；占用 SRAM（4224B/通道）。

### 21.4 汇总：哪些是"不可逾越"的硬件限制

| 限制 | 本质原因 | 是否可软件弥补 |
|------|---------|--------------|
| 单通道总线 | FMC 共享总线 | ❌ 物理不可并行（只能分时） |
| 无 DRAM | 无外部内存接口可行方案 | ⚠️ 用 NAND+SRAM cache 近似 |
| 无硬件 ECC | MCU 无 ECC 外设 | ⚠️ 软件 BCH/LDPC（慢+弱） |
| 单对 DMA | DMA2 Stream 有限 | ⚠️ PIO 兜底（慢） |
| 无硬件加扰/XOR | MCU 无 | ⚠️ 软件实现（占 CPU） |
| 无硬件队列 | MCU 无 | ⚠️ 软件队列 + 轮询（占 CPU） |

### 21.5 结论

本项目的 FC-NAND 架构在**软件层面完整对齐了企业/消费级主控的分层模型**（命令池/描述符/缓冲池、调度器、数据路径、ECC/RAID/加扰引擎、ISSUE/WAIT/FETCH 并发原语），这是与行业一致的"形"。

但受 STM32H743 MCU 硬件限制，所有**需要硬件引擎加速的部分**（ECC、DMA 多通道、加扰、XOR、页缓冲拼接、命令队列）都只能退化为**软件实现 + CPU 周期换功能**，单通道总线更是物理上限。因此本项目定位是：

> **"用 MCU 软件模拟企业级 SSD 主控架构的教学/研究实现"**——验证 FTL/FC/NAND 分层、命令并发、数据路径、ECC/RAID 的完整软件闭环；性能与真正的 SSD 主控不在一个数量级，但在架构正确性和功能完整性上是自洽的。

这正是"退而求其次"的必然：**形似而神不似，功能完整而性能有限**。

---

## 附录 A：图 → 源码 grep 对照

| 图 | 内容 | 定位方法 |
|----|------|---------|
| 图 0 | 分层架构与数据流总览 | `grep -rn "nand_dispatch_submit" src/fc/fc.c` |
| 图 1 | 命令生命周期 | `grep -n "fc_command_alloc\|fc_submit\|fc_command_release" include/fc/fc.h` |
| 图 2 | fc_service() 主循环 | `grep -n "fc_poll_internal" src/fc/fc.c` |
| 图 3 | CW 布局与数据处理范围 | `grep -n "FC_BUFFER_USER_OFFSET\|fc_buffer_get_base" include/fc/fc.h src/fc/fc.c` |
| 图 4 | 写路径编码（Page Pack） | `grep -n "fc_data_path_encode" src/fc/fc_data_path.c` |
| 图 5 | 读路径解码（Page Unpack） | `grep -n "fc_data_path_process_read\|fc_data_path_raid_recover" src/fc/fc_data_path.c` |
| 图 6 | nand_engine 微码状态机 | `grep -n "nand_engine_step" src/nand/nand_engine.c` |
| 图 7 | 70h 状态轮询循环 | `grep -n "JUMP_IF_NOT_READY" src/nand/onfi_1_0.c` |
| 图 8 | FMC 后端 UOP 执行 | `grep -n "g_nand_fmc_handlers" src/nand/nand_fmc.c` |
| 图 9 | nand_dispatch poll 两阶段 | `grep -n "nand_dispatch_poll" src/nand/nand_dispatch.c` |
| 图 10 | LUN 相位机（Phase V） | `grep -n "LUN_PHASE_" src/nand/nand_dispatch_internal.h src/nand/nand_dispatch.c` |
| 图 11 | 调度器 select_chain | `grep -n "fc_scheduler_select_chain" src/fc/fc_scheduler.c` |
| 图 12 | Stripe RAID 写提交 | `grep -n "fc_stripe_submit" src/fc/fc.c` |
| 图 13 | 完成回写与 CQ 组装 | `grep -n "fc_complete_from_nand" src/fc/fc.c` |

## 附录 B：建议阅读顺序

```
第一步（整体）  图 0 → 图 1 → 图 2
第二步（数据）  图 3 → 图 4 → 图 5
第三步（硬件）  图 6 → 图 7 → 图 8
第四步（并发）  图 9 → 图 10 → 图 11
第五步（进阶）  图 12 → 图 13
```
