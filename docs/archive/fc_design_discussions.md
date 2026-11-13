# FC-NAND 设计与重构讨论归档

> 从 `docs/design/fc_nand_design.md` §11、§14~§16 移出；正文不改写。

## 11. 接口重构策略与落地清单

> **⚠ 历史章节（v13 前的接口）**：本章记录的是 FC 侧还持有"掩码建链 + 数据搬运"时的重构过程。
> 本章出现的 `fc_command_configure_descriptors()`（**复数**）已在 v13 上移 FTL 并删除，现名
> `ftl_cmd_configure_descriptors()`；`fc_write_cmd_data()` / `fc_read_cmd_data()` 同理改为
> `ftl_cmd_write_data()` / `ftl_cmd_read_data()`。FC 侧如今只有**单数**的
> `fc_command_configure_descriptor()`（逐条建链机制）。判据见 §26。

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
| 3 | fc_command_configure_descriptors | 合并 descriptor 分配+配置为单接口（v13 上移 FTL，见 §26） | ✅ |
| 4 | paa_from_addr() | PAA 地址合成函数（由 `nand_addr_s` 具名字段打包；v25 前为 5 形参 `paa_make`） | ✅ |
| 5 | 条件判断宏封装 | 复合条件拆为独立命名宏 | ✅ |
| 6 | 资源 ID 双向查询 | 每种资源提供 id↔指针 双向转换函数 | ✅ |
| 7 | 冗余 NULL 检查移除 | 内部函数移除 controller/command 等 NULL 检查 | ✅ |
| 8 | fc_submit_internal 简化 | 三元嵌套提取为独立变量 | ✅ |
| 9 | fc_command_validate 精简 | 移除位域全组合均有含义的冗余检查 | ✅ |

### 11.3 Descriptor 分配重构

**旧接口：**
```c
fc_desc_id_t fc_command_descriptors_alloc(fc_cmd_s *command, uint8_t cw_count);
```

**新接口（一次完成分配+配置）：**
```c
fc_status_t fc_command_configure_descriptors(fc_cmd_s *command);
```
内部根据 `command->config.cw` 自动计算需要的 descriptor 数量，分配 descriptor 链+buffer 并串联链表。

### 11.4 命名修正

| 旧名 | 新名 | 位置 |
|------|------|------|
| `PAGE_NUM` | `PAGES_PER_BLOCK` | `nand_config.h` |
| `BLOCK_NUM` | `BLOCKS_PER_PLANE` | `nand_config.h` |

---

## 14. SET/GET_FEATURES 内联数据路径优化

### 14.1 背景

SET_FEATURES（0xEF）和 GET_FEATURES（0xEE）是 ONFI 协议中读写 NAND 内部寄存器的命令，格式为：

- **SET_FEATURES**：发送 1 字节寄存器地址（COLUMN_0）→ 4 字节参数数据
- **GET_FEATURES**：发送 1 字节寄存器地址（COLUMN_0）→ 回读 4 字节参数数据

原始实现中，即使 SET/GET_FEATURES 只传输 ≤5 字节，也分配了完整的 descriptor + buffer（528 字节），浪费了有限的 SRAM 资源池。优化目标是消除这次无谓的分配，让命令通过 `fc_cmd_s` 内部字段直接承载这 5 字节信息。

### 14.2 数据结构变更

在 `fc_cmd_s` 中：

1. **`feature_addr:8`** — 从原 `reserved1:11` 中划分 8 位，存储 NAND 寄存器地址。FC → nand_dispatch 传递后，由 COLUMN_0 uop 发送 `(uint8_t)request->column`，即此值。
2. **`parameter[0..3]` 由 `DWORD : 8` → `uint8_t`** — 位域不能取地址，但内联路径需要一个可取地址的 4 字节落点作为 buffer 指针。改为纯字节后增加了 ~4 字节/command，Command Pool 总量 +256 字节。**v7** 起这 4 个字节并入 `fc_param_u param` 联合体（见 §2.2）。

### 14.3 数据流追踪

> **v7 变更**：buffer 不再指向 `&command->parameter0`（那会让提交后的命令体被写，违反「SQ 不可变」）。
> 改为指向控制器级暂存 `g_fc_return_staging[command_id]`（按命令池槽位索引的 4 字节中转），返回值最终经
> CQ 的 `param.value` 交回上层。

```
SET_FEATURES:
─────────────
command->feature_addr ──► request.feature_addr ──► task.feature_addr ──► nand_fc_request_t.column (= 0x01)
&command->param.value  ──► memcpy → g_fc_return_staging[command_id]
                                │ (cw_buffers[plane*CW_NUM] = g_fc_return_staging[command_id]，feature 无 PAA 故 plane 恒 0)
                                ▼
                           NAND 从暂存读取 4 字节发送至寄存器

GET_FEATURES:
─────────────
command->feature_addr ──► (同上) ──► column (= 0x01)
                                │ (cw_buffers[plane*CW_NUM] = g_fc_return_staging[command_id]，plane 恒 0)
                                ▼
                           NAND 回写 4 字节写入暂存
                                │
                                ▼
                           fc_complete_from_nand → completion.param.value → CQ
                                │
                                ▼
                           测试 / FTL 从 CQ 条目读取
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

建链入口跳过 desc 分配——v13 前是 `fc_command_configure_descriptors_internal()` (fc.c)，
v13 后是 FTL 侧的 `ftl_cmd_configure_descriptors()`（判断收敛在 `ftl_cmd_needs_desc_chain()`）：

```c
if ((nand_operation_has_data((uint8_t)command->config.operation) == 0U) ||
    (command->config.operation == OP_SET_FEATURES) ||
    (command->config.operation == OP_GET_FEATURES))
{
    command->desc_id = FC_DESCRIPTOR_ID_INVALID;
    return FC_STATUS_OK;   /* v13 后返回 FTL_OK */
}
```

`fc_build_dispatch_request()` (fc.c) — 构造 dispatch request 时直接指向 command parameter 地址：

```c
request.feature_addr = (uint8_t)command->feature_addr;

/* 在 descriptor 遍历分支之前插入 SET/GET_FEATURES 分支 */
else if ((command->config.operation == OP_SET_FEATURES) ||
         (command->config.operation == OP_GET_FEATURES))
{
    /* feature 无 PAA，plane 恒 0。v9：掩码只置本 plane 的 CW0 位（Feature 恒 4 字节单次传输，
     * 与 config.cw 无关），数据落 cw_buffers[plane*CW_NUM]。
     * v7：buffer 指向控制器级暂存，不指向 command（SQ 提交后不可写）。 */
    request.cw_bitmap                           = 1UL << (request->plane * CW_NUM);
    request.cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
    if (SET) memcpy(g_fc_return_staging[command_id], &command->param.value, 4U);
    else     memset(g_fc_return_staging[command_id], 0, 4U);
}
```

#### Dispatch 层 — 正确处理 column 和数据

`nand_dispatch_task_load()` 拷贝 feature_addr 到 task（v7 起不再有 `parameter[]` 中转）：

```c
task->feature_addr = request->feature_addr;
```

`req_feature()`（nand_dispatch_tables.c）SET/GET_FEATURES 分支：

```c
task->request = (nand_fc_request_t){.column = (uint16_t)task->feature_addr, .row = {0U},
                                    .data = task->cw_buffers[task->plane * CW_NUM], .data_length = 4U};
```

> **说明**：此处 `cw_buffers[task->plane * CW_NUM]` 已经通过 `nand_dispatch_task_load()` 从
> `request.cw_buffers[plane*CW_NUM]`（即 `g_fc_return_staging[command_id]`）拷贝，指向控制器级暂存。
> feature 寄存器是 die 级、无 plane 语义，`command->paa` 从不设置，故 plane 恒 0。GET_FEATURES 的回写
> 数据经此通道写入暂存，再由 `fc_complete_from_nand` 拷进 CQ 的 `param.value`。

#### 完成路径 — 无 desc 回写，但有返回段搬运

`fc_complete_from_nand()` 检查 desc_id 后才执行 descriptor 回写循环。SET/GET_FEATURES 的
desc_id = FC_DESCRIPTOR_ID_INVALID，自动跳过回写。GET_FEATURES 的返回值从
`g_fc_return_staging[command_id]` 拷入 `completion.param.value`（SQ 命令体保持不被写）。

### 14.5 读回路径演进（v7 更新）

**v2 问题**：最初实现中，`nand_dispatch_submit()` 将 `cw_buffers[0]` 重定向到 `task->parameter[]`，
导致 GET_FEATURES 的回写数据进入 task.parameter 而非 command.parameter，测试读到的为零值。
**v2 修复**：去掉重定向，buffer 始终指向 command.parameter0。

**v7 问题**：v2 的修复让 NAND 引擎（经 `cw_buffers[plane*CW_NUM]`）直接写 FC 命令池中的 `command->param`——
命令已提交，却仍在被写，违反「SQ 不可变」（命令配置完即算完成）。
**v7 修复**：buffer 指向控制器级暂存 `g_fc_return_staging[FC_COMMAND_POOL_DEPTH][4]`（按 `command_id` 索引，
`command_id` 恒 < 池深，索引安全）；返回值唯一归宿是 CQ 的 `param` 联合体。
`nand_dispatch_completion_t` **不**为此新增字段。

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
| 内联方案 v1 | `fc_cmd_s.reserved1:11` → `feature_addr:8`；保留 `DWORD parameter : 8` | 位域不可取地址，读回数据无法送达 |
| 内联方案 v2 | parameter 改为 `uint8_t`；buffer 直接指向 `&command->parameter0`；去掉 task.parameter 重定向 | 读回数据正确落入 command，但提交后仍在写 SQ |
| 内联方案 v3（最终） | parameter 并入 `fc_param_u`；buffer 指向控制器级暂存 `g_fc_return_staging[command_id]`；返回值经 CQ 的 `param` 交回 | 读回数据正确且不违反「SQ 不可变」 |

---

## 15. nand_dispatch 函数指针表重构 (Phase D)

### 15.1 动机

`nand_dispatch_execute_command()` 和 `nand_dispatch_poll()` 中大量 if-else 链根据 `task->operation` 判断 NAND 操作类型并构造 `nand_fc_request_t`。每条 if-else 链包含 6-7 次条件比较，不仅增加执行时间，还带来分支预测惩罚。

在嵌入式 Cortex-R5/M7 上，分支预测错误代价约 12-14 周期，而函数指针间接跳转固定为 2-3 周期。将条件链替换为函数指针表可显著提升 dispatch 层的执行确定性。

### 15.2 设计方案

两张独立函数指针表，均以 `OP_*`（0~28）为直接下标：

| 表 | 类型 | 用途 | 替代目标 |
|----|------|------|---------|
| `g_nand_dispatch_req_table[OP_COUNT]` | `void (*)(nand_dispatch_task_t *)` | 从 task 预解析数据构造 `nand_fc_request_t` | `nand_dispatch_poll()` 中的 ~90 行 if-else 链 |
| `g_nand_dispatch_completion_table[OP_COUNT]` | `void (*)(nand_dispatch_task_t *, uint8_t, const nand_fc_completion_t *)` | 设置 cw_status 和 failed_descriptor_id | `nand_dispatch_task_complete()` 中的 if-else 链 |

### 15.3 Request 构建 handler

共 7 个 handler，覆盖全部 29 个 OP：

| Handler | 覆盖的 OP | 行为 |
|---------|----------|------|
| `req_program_page` | NORMAL_PROGRAM_PAGE, OTP_DATA_PROGRAM, PROGRAM_PAGE_CACHE | 将各 CW buffer 拼接到 `g_nand_channel_page_buffer[task->ch][task->ce][task->lun]`，request.data_length = PAGE_SIZE |
| `req_read_page` | NORMAL_READ_PAGE, OTP_DATA_READ | 仅 CW0：request = (column=0, row, buffer=cw_buffers[task->plane], length=PAGE_SIZE/4) |
| `req_cache_read` | READ_PAGE_CACHE_SEQ/RANDOM/LAST | 一次性读 full page：request = (0, row, page_buffer, PAGE_SIZE) |
| `req_erase` | ERASE_BLOCK | 纯地址：request = (0, row, NULL, 0) |
| `req_feature` | SET_FEATURES, GET_FEATURES | column = feature_addr：request = (feature_addr, 0, cw_buffers[task->plane], 4)（plane 恒 0） |
| `req_single` | READ_ID, READ_PARAM_PAGE, READ_UNIQUE_ID, READ_STATUS, READ_STATUS_ENH, RANDOM_DATA_READ 等 | 通用固定长度：length 从 `g_nand_operation_map[op].data_length` 读取 |
| `req_no_data` | RESET, BLOCK_LOCK, BLOCK_UNLOCK 等 | 无数据传输，纯 row 地址 |

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
g_nand_dispatch_req_table[task->operation](task);
```

### 15.6 `nand_op_map_s` 协议属性增强

`nand_op_map_s` 新增两个协议属性字段（它们是 ONFI 协议属性，不是 dispatch 策略）：

```c
typedef struct {
    // ... 原有字段不变
    uint8_t  has_data;     /* 操作是否需要数据传输 */
    uint16_t data_length;  /* 数据传输字节数（0 = 无数据） */
    // ...
} nand_op_map_s;
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

RANDOM_DATA_READ（05h-C1-C2-E0h）等命令的数据传输长度不由命令码本身决定，而是由 FMC 硬件产生的 RE# 时钟脉冲数控制。`nand_op_map_s.data_length` 中记录的值不是命令的"固有属性"，而是**传输长度的默认值**，通过 `nand_fc_request_t.data_length` 传递给 FMC 层作为 DMA/RE# 脉冲计数器：

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

> ⚠ **运行时状态提示**：除 **FTL cache read / cache program 的 run** 外，FTL 命令恒设 `config.chain = FC_CHAIN_END`（`src/ftl/` 共 23 处），运行时 `count` 恒为 1，走**单命令路径**（`fc_poll_dispatch_single`）。cache run 自 2026-10-30（P1 读流式，未上板）起标成链（窗口末条在断点是 `FC_CHAIN_END`、否则 `FC_CHAIN_MORE`，可跨填充轮次 / 跨 chunk 续链）⇒ 运行时**也会**走 `fc_poll_dispatch_chain`，见 [§23.9 的 v-streaming 更新](#239-澄清运行时其实不走命令链)。本章讲链式执行的设计与单测覆盖。
>
> 🔁 **v8 语义变更（2026-10-06，per-LUN FIFO）**：本章 16.2 起的方案描述**已被取代**。链不再是"执行中的 chain 对象 + cursor"，而是 **per-LUN 待处理 FIFO 里的连续 N 槽**（每槽 = 一条命令）：
> - **原子**：`nand_dispatch_submit_chain()` 先判 `room >= N`，不足则整批拒收、无副作用；
> - **不可插入**：连续槽 + FIFO 严格顺序，同 LUN 的后续命令（含 HSQ）只能排在链后；
> - **逐条完成**：每条命令各回一份 completion（旧模型只在链尾回一份，中间命令的 `cw_status[]` 被吞）；
> - NAND 侧**无链标记**，`chain->cursor` / `chain_advance` 已删除。
>
> 详见 `docs/archive/changelog/fc_nand_per_lun_fifo.md`；本节余下内容保留为历史设计记录。

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
fc_poll_internal() → fc_poll_dispatch_chain():
  for i=0..2:
    校验 QUEUED → 置 ACTIVE
    page write 则 fc_data_path_encode()   ← 注意：链式路径不做 META/PAA 预填
    fc_build_dispatch_request(cmd_i) → reqs[i] = { … }
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
    fc_cmd_id_t ids[FC_SCHEDULER_CHAIN_MAX];
    uint8_t         count;  /* 0=空, 1=单命令, 2+=链 */
} fc_sched_chain_s;
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
fc_status_t fc_scheduler_select_chain(fc_sched_s *scheduler, fc_sched_chain_s *chain)
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
| `include/fc/fc_scheduler.h` | 新增 `FC_SCHEDULER_CHAIN_MAX`、`fc_sched_chain_s`、`fc_scheduler_select_chain()` |
| `src/fc/fc_scheduler.c` | 新增 `fc_scheduler_select_chain()` 链识别 + 整链弹出 |
| `include/fc/fc_queue.h` | 新增 `fc_submission_queue_peek()`、`fc_submission_queue_pop_n()` |
| `src/fc/fc_queue.c` | 实现 `peek()`、`pop_n()` |
| `src/fc/fc.c` | `fc_poll_internal()` 改用 `fc_scheduler_select_chain()`，增加链式命令构建 + `submit_chain` 路径 |

### 16.9 使用方式（FTL / Read Recovery）

> ⚠ **与实现不符的历史示例**：以下代码为 Phase D 设计期的接口示意（`config.chain = 1` / `fc_command_submit`），**当前 FTL 运行时并不这样用**——Read Recovery 实际是**三条独立提交 + 同步等待**的命令（`src/ftl/read_recovery.c`：各自 `fc_submit` + `ftl_sync_submit_wait`），并非原子链。示例仅用于说明链式 API 的语义。

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

> ⚠ **「HSQ 在整个链执行期间无法插入」这句到 v52 才真正成立**。链长 ≤ `FC_SCHEDULER_CHAIN_MAX`（4）
> 时，整条链一批进 FIFO，`nand_dispatch_submit_chain` 保证一批内原子，确实插不进；但**链长 > 4**
> 时会被拆成多批、分多次填充轮次下发，而 v51 及以前**批与批之间无锁** ⇒ HSQ 可插在批边界上。
> v52 新增跨轮次持久的 per-LUN 链锁（`fc_sched_s.chain_owner`，见 §2.7）补上这段：
> 链一旦开始，该 LUN 的填充权归链所在队列独占，直到链 END 下发。

### 16.10 与 Phase D (函数指针表) 的关系

| 子阶段 | §15 函数指针表 | §16 命令链 |
|--------|---------------|-----------|
| 状态 | ✅ 已完成代码+文档 | ✅ 已完成代码+文档 |
| 解决的问题 | if-else 链 → O(1) 查表 | 命令原子性 + HSQ 防插入 |
| 改动范围 | nand_dispatch 内部 | nand_dispatch + fc_scheduler + fc |
| 代码编译 | ✅ 通过 | ✅ 通过 |

### 流程图：命令链执行（图 11 配套：调度器填充轮次）

> 🔁 **v8 起本节已取代**：`fc_scheduler_select_chain` / `fc_scheduler_select` 与 busy 表、读饥饿保护全部删除。
> 选择逻辑搬进 `src/fc/fc.c` 的 `fc_poll_fill()`——一次扫描 HSQ/SQ，按 LUN 保序把能放下的命令（含链）提交进
> NAND 的 per-LUN FIFO。下方为**旧实现**的历史记录，保留以对照语义变化。

```text
fc_poll_fill(controller)                ← 一次填充轮次（v8）
  │
  ├─ 快照 HSQ 数 / SQ 数
  │
  ├─ 扫 HSQ（快照数，HSQ 与 SQ 同一套链逻辑）
  │    need = fc_poll_chain_length(hsq, head)   // 链头：连续 chain=1 段，≤ CHAIN_MAX(4)
  │    room = nand_dispatch_lun_free(ch,ce,lun)
  │    room >= need → 取出 need 条（链则整条）→ submit / submit_chain
  │    否则 → 该 LUN 本轮挂起（同 LUN 后续跳过，含 SQ）
  │
  └─ 扫 SQ（快照数）
       need = fc_poll_chain_length(sq, head)
       room = nand_dispatch_lun_free(ch,ce,lun)
       room >= need → 取出 need 条（链则整条）→ submit / submit_chain
       否则 → 该 LUN 本轮挂起，命令回推队尾（保序）
       扫完即止（不因回推的命令再入队）

返回：有下发 → OK；下发失败 → IO_ERROR；两队列空 → QUEUE_EMPTY；无 room → NOT_READY
```

**旋转语义**：`pop(队头)+push(队尾)` 只动 head/tail 指针，Command Pool 内容不变；完整轮次走完等价于队列轮转一圈，
未下发的命令保持相对顺序。

<details>
<summary>旧实现（v7 及以前，历史记录）</summary>

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

</details>


---
