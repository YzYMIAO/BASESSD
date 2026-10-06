# FC 层支持「按 CW 子集读」

**日期**: 2026-10-06
**相关设计**: `docs/design/fc_nand_design.md` §17.4、§17.5、§17.10、**§25**；`docs/design/fc_api_usage.md`
**前置**: `nand_dispatch_plane_index.md`、`internal_move_op_merge.md`（同日）

---

## 概述

读恢复（read recovery）的场景是：一页 4 个 CW 里 ECC 只挂了其中 2 个或 3 个，恢复流程只需要**重读失败的那几个 CW**，不必重读整页。

改动前的 FC 表达不了这件事：

- `fc_cmd_conf_t.cw : 2` 的语义是 **"CW 数减一"**，只能表达"前 N 个 CW"，表达不了 `{0,1,3}`。
- 建链处单 plane 的 CW 号是 `paa_to_cw(command->paa) + index`——从命令 PAA 的 CW 位起**连续递增**。
- 读路径把 `column` 写死 0，靠首个 `00h-30h` 的 DATA_OUT 取 CW0，再用 `05h-列-E0h` 续读 CW1..3；而 `paa_to_nand_row()` **丢掉 CW 位**，命令 PAA 里的 CW 是被静默忽略的。

**范围**：只改 FC 层 + 它驱动的 NAND dispatch 层。**不含** `src/ftl/read_recovery.c` / `ftl.c` 的恢复流程接线（后续单独做）。

## 三项决策

| # | 决策 | 取值 |
|---|------|------|
| 1 | 掩码入口 | `fc_command_t` 新增 `cw_mask : 4`（占 `reserved0` 的 4 位） |
| 2 | 读流程 | **严格统一**：所有读命令都拆成「加载进 cache」+「按 mask 逐 CW `05h/06h-E0h`」，**CW0 也走逐 CW 取**（不与加载融合） |
| 3 | 范围与输出 | 单 plane + 多 plane 一并支持；失败 CW 掩码一并从 FC 暴露 |

## 核心不变量：链槽位与 CW 号解耦

```
mask = 0b1011（CW0/CW1/CW3）单 plane 读
  desc 链：          desc#0 → desc#1 → desc#2      （链槽位 0,1,2）
  desc->paa 的 CW 号：     0        1        3      （CW 号从 PAA 取）
```

- **链槽位**：desc 在链中的走位 → 决定 `cw_status[]` / `cws[plane][slot]` 下标、`failed_descriptor_id`。
- **CW 号**：决定 NAND 列地址（`column = paa_to_cw(paa) × CW_SIZE`）与页缓冲内偏移。

满 mask 时两者相等，改动前后逐字节等价；非满 mask 时**必须**一律从 PAA 取 CW 号。

**消费点共 5 处，必须一次改齐**（漏改任一处数据就落到错误的列）：

| # | 位置 | 作用 |
|---|------|------|
| 1 | `src/fc/fc.c` `fc_build_dispatch_request`（multi-plane 分支） | 按 plane 内的**槽位**填 `cws[plane][slot]` |
| 2 | `src/nand/nand_dispatch.c` `nand_dispatch_task_cw_chain` | 列地址（唯一 CW 号真值来源） |
| 3 | `src/nand/nand_dispatch.c` `nand_dispatch_scatter_page_buffer` | 页缓冲内偏移 |
| 4 | `src/nand/nand_dispatch_tables.c` `req_program_page` / `req_multi_plane_program` | 写侧拼接偏移（写侧 mask 恒 0，等价；统一以防日后误用） |
| 5 | `src/fc/fc_data_path.c` Pass 4 META 校验 | `lba_to_cw_meta(command->meta, paa_to_cw(desc->paa))` |

## 改动

### 1. 命令/完成结构（`include/fc/fc_command.h`）

```c
struct fc_command_t {
    DWORD command_id : 16;
    DWORD cw_mask    : 4;   /* 新增：bit n = CW n；0 = 连续 0..config.cw（向后兼容） */
    DWORD reserved0  : 12;  /* 原 16 */
    ...
};
struct fc_command_status_t {
    ...
    DWORD status         : 8;
    DWORD failed_cw_mask : 4;  /* 新增：bit n = CW n 解码失败（UNC/CRC_ERROR）；0 = 全通过 */
    DWORD reserved       : 16; /* 原 20 */
};
```

`fc_command_t` 在 `fc_command_alloc_internal` 里被 `(fc_command_t){0}` 整体清零 → **所有现有调用点 `cw_mask` 自动为 0 → 行为不变**。
`fc_command_status_t` 由 `fc_command_status_init` 整体清零 → `failed_cw_mask` 恒 0 起步。

### 2. 建链（`src/fc/fc.c`）

- 新增 `fc_cw_mask_popcount`（手写 4 位循环，不引 `__builtin_popcount`，避免 libgcc `__popcountsi2`）。
- `fc_command_configure_descriptors_internal`：解析 mask（`mask==0` → `(1 << (config.cw+1)) - 1`），按 **plane-major** 枚举置位 CW 建 desc，并把 **`config.cw` 回写为"置位数减一"**——链长、完成项折叠、`fc_build_dispatch_request` 的 `cw_count` 都按这个字段算，必须与实际 desc 数一致。
- `fc_command_configure_descriptor_internal` 签名 `index` → `(plane, cw)`：CW 号由调用方按 mask 枚举给出，不再由 desc 序号推导。

### 3. 读路径统一（`src/fc/fc.c`）

FC 层命令**保持 `WITH_DATA_OUT` 语义**（`fc_is_page_read` / 数据路径 / 完成判定都用它），但下发给 NAND 的是对应的「只加载、不输出」op，转换点位于 `fc_build_dispatch_request` 末尾：

```c
if (request->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT)
    request->operation = OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT;
else if (request->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT)
    request->operation = OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT;
```

### 4. 链推进（`src/nand/nand_dispatch.c`）

新增 `nand_dispatch_is_load_op` / `nand_dispatch_is_fetch_op`，重写 `nand_dispatch_task_cw_chain`：

| 当前 op | 完成后动作 |
|---------|-----------|
| **加载型**（两种 `WITHOUT_DATA_OUT`） | **不推进槽位**，从槽位 0 起发取数 op；失败则链终止 |
| **取数型**（`OP_RANDOM_DATA_READ` / `_MULTI_PLANE`） | 推进一个槽位，直到链尾 |
| **旧语义**（`WITH_DATA_OUT` 普通读 / OTP 读） | CW0 已由 DATA_OUT 取回，其余逐 CW 随机读（保留给直接下发场景） |

取数 op 按 plane 选：单 plane → `OP_RANDOM_DATA_READ`（`05h-列-E0h`）；多 plane → 新 op（`06h-列-行-E0h`）。
多 plane 槽位 plane-major 展开：`slot = plane × cw_count + cw`，与 FC 的 desc 链序、`cw_status[]` 下标三者一致。

`nand_dispatch_scatter_page_buffer` 的两处偏移同步改用 `paa_to_cw`。**cache 读族不动**（`31h`/`3Fh`/`FETCH` 是 cache 管线机制，不是"加载进 cache"）。

### 5. 新 op `OP_RANDOM_DATA_READ_MULTI_PLANE`（41）

`06h` 此前只**内联**在 `g_nand_multi_plane_read_with_data_out_uops` 里（多 plane 读后分 plane 取 cache），没有独立 OP 码。本次提成独立 op：

```
70h轮询 → 06h → C1 → C2 → R1 → R2 → R3 → E0h → tR → Dout → 70h轮询
```

| 文件 | 改动 |
|---|---|
| `include/nand/onfi_1_0.h` | `#define OP_RANDOM_DATA_READ_MULTI_PLANE (41U)`；`OP_COUNT` 41 → 42 |
| `src/nand/onfi_1_0.c` | 新增 `g_nand_random_read_multi_plane_uops[]` + op 表项（`.address_cycles = 5U`、`.data_length = CW_SIZE`、`.has_data = 1U`） |

命名说明：**协议命令常量 `NAND_CMD_RANDOM_DATA_READ_TWO_PLANE`（`0x06U`）不改名**——那是 ONFI 协议命令自身的名字（Micron 文档里 06h 就叫 Random Data Read Two Plane），且该定义早于本次改动就存在；改的只是本项目 OP 的命名。

### 6. 完成项

| 侧 | 改动 |
|---|---|
| NAND | 新增 `completion_load` 策略：两种加载型 op 用它——**成功时不写任何 `cw_status`**（各槽位由后续逐 CW 取数填入），失败时刷满 8 槽（加载失败后取数链不再启动，没有其它来源能反映这次失败）。替换这两种 op 原先误用的 `completion_per_cw` / `completion_multi_plane` |
| NAND | 新增 `req_random_read`（列地址 = `paa_to_cw(cw_paa) × CW_SIZE`），注册给 `OP_RANDOM_DATA_READ` 与新 op |
| FC | `fc_complete_from_nand` 第 3 步：status/ECC 折叠从「链走位 `i`」改成「按 **CW 号** `paa_to_cw(d->paa)`」（plane 1 折进同一 slot、ECC 只取 plane 0；满 mask 时逐字节等价）；顺带删掉一个已死的 `else` 分支（写 `FC_CW_STATUS_UNUSED` 而该值就是 `0U`） |
| FC | 新增 `completion.failed_cw_mask \|= (1U << cw_num)`（条件 `FC_CW_STATUS_UNC \| FC_CW_STATUS_CRC_ERROR`） |

### 7. 其它

- `src/fc/fc_scheduler.c`：`fc_scheduler_op_is_read` 补 `OP_RANDOM_DATA_READ_MULTI_PLANE`。
- `src/fc/fc_data_path.c`：Pass 4 的 META 回读校验改用 `paa_to_cw(desc->paa)`（写入侧用的是 CW 号，非满 mask 下用链走位会误报 `FC_CW_STATUS_MATA_ERROR`）。

## 风险

1. **热路径改变（最高）**：严格统一后，**每次整页读多一轮 `70h + 05h-列-E0h + WAIT_READY`**（原来 CW0 靠 `00h` 的 DATA_OUT 一次拿到）；多 plane 整页读从 1 条原子事务变成 1 次加载 + `2 × cw_count` 条事务。所有现有读用例语义上应等价，但**必须重新上板验证**。若上板发现延迟或行为不可接受，退回"CW0 融进加载"是局部改动。
2. **`06h` 的 plane 语义**：项目内既有实践（`onfi_1_0.c` 多 plane 读已在用），但**未对 MT29F4G08 datasheet 逐条核对** `06h` 的地址周期构成与"读哪个 plane 的 cache"的规则。上板前按 datasheet 复核一次。
3. **producer/consumer 索引不一致**：链槽位与 CW 号解耦后，上面 5 个消费点必须一次改齐。
4. **`LUN_NUM>1` 的 ISSUE/WAIT 拆分**：转换后的两个加载 op 不再命中 Phase V splittable 拆分，多 LUN 重叠随之失效。当前 `LUN_NUM=1`，该拆分本就不生效；启用多 LUN 前需把 LOAD/FETCH 拆分与逐 CW 取数模型重新对齐。

## 验证

1. `make format` + `make debug` / `make tests` / `make release` 三档**零警告**通过（153412 / — / 87660 字节）。
2. 静态审计：`src/` 里已无用槽位当 CW 号的偏移表达式——`page_buffer[cw * CW_SIZE]` 与 `cw_index * CW_SIZE` 都消失，只剩 `paa_to_cw(...) * CW_SIZE`。
3. **上板未验证**（按平台规则由用户执行）。回归护栏：现有全部读用例必须通过。
4. **新语义用例未加**：建议 FC 层加一条「写满 4 CW 后，用 `cw_mask = 0b1011` 读回，校验取回的是 CW0/CW1/CW3 且内容正确」。测试源文件按平台规则由用户决定是否新增。
5. **datasheet 复核未做**（风险 2）。
