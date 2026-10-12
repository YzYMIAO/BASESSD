# v63：新增 `OP_READ_COLUMN`（槽位 42，`OP_COUNT` 首次 42 → 43）—— 按**显式列地址**读 4 字节；`bbt` 改为非破坏性出厂坏块扫描

## 〇、起因：`bbt` 打出来的坏块图永远是全 `FF`

用户报「`bbt` 的坏块图看不出任何东西」。核过源码，根因**两条**，互相独立：

| # | 根因 | 证据 |
|---|---|---|
| ① | **先擦后读**——`bbt_probe_block_bad()` 先 `ftl_sync_erase_block()` 再读 | 器件手册（Micron **AN1819**）逐字：*"the bad block information is **erasable** and cannot be recovered once erased"*；*"System software should check the first spare area location on the first page of each block **prior to** performing any PROGRAM or ERASE operations"*。擦完再读 = 读「擦后状态」= 必然全 `FF`。`include/ftl/bbt.h` / `src/ftl/bbt.c` 原先写的「出厂坏块标记是**擦不掉的硬标记**」**是错的**，已删 |
| ② | **列地址根本到不了 2048**——出厂标记在 **列 2048**（spare 首字节，x8 为 `00h`），而全仓唯一能指定列的 op 11（`OP_READ_META`）的列 = `paa_to_cw(paa) * CW_SIZE`，cw 只有 **2 位** ⇒ 列只能取 {0, 528, 1056, 1584} | `src/nand/nand_dispatch_tables.c` 的 `req_meta_read`；`CW_SIZE = PAGE_SIZE / CW_NUM = 2112/4 = 528`。`PAGE_SPARE_SIZE` **零读者** |

**目标**：加一条**不擦也能读任意列**的命令；把 `bbt` 改成「默认只读、`--erase` 才擦」。

## 一、接口（用户拍板，三条约束）

1. **不用 desc 链**（`has_data = 0`）；
2. **列地址走 SQ 的 `command->param.bytes[0..1]`（小端）**——**不用** `paa` 的 cw 字段（cw 只有 2 位，够不到 spare 区）；
3. **结果回 CQ 的 `completion.param.bytes[0..3]`**，`param[0]` 即坏块标记。

一次读 **4 字节**（列 2048..2051 = spare[0..3]），与已上板验过的 `OP_READ_META` **完全同形**——
同款控制器侧暂存 `g_fc_return_staging` 机制，**不新增出参 API**。

## 二、设计定案

| 决策 | 结论 |
|---|---|
| OP 槽位 | **槽位 42**（0..41 已满；唯一空位 20 是**墓碑**「已删除…不要复用」）⇒ **`OP_COUNT` 42 → 43，全仓首次抬升** |
| uop 序列 | **复用 `g_nand_normal_read_page_uops`**（`WAIT_READY → 00h → COLUMN_0/1 → ROW_0/1/2 → 30h → WAIT → DATA_OUT → END`）——与 op 11 逐字节相同，差别仅在**列来源**与输出长度 4 |
| 列地址来源 | **命令级 SQ `param.bytes[0..1]`**（小端）⇒ 需要一条 16 位列地址穿过 dispatch 层（`feature_addr` 是 8 位、装不下 2048，**不合并**） |
| 行地址 | 仍取 `command->paa`（`paa_to_nand_row` 丢掉 cw） |
| 数据长度 | 4 字节，落本 plane 的 CW0 那一格（掩码位 = `plane*CW_NUM`），与 `req_feature` / `req_meta_read` 同口径 |
| 改动范围 | `src/` + `bbt` + CLI + 文档。**`tests/` 一行不动**（`test_command_validate_op_range` 遍历 `0..OP_COUNT-1`，新 op `has_data == 0` 自动通过） |

## 三、内部承载（实现细节）

数据要穿三层，**最内层已有 16 位列地址，中间两层没有**：

```
fc_command_t.param.bytes[0..1]   ← 用户定的入口（SQ）
   ↓ fc_build_dispatch_request()          [src/fc/fc.c]
nand_dispatch_request_t          ← 新增 uint16_t column    [include/nand/nand_dispatch.h]
   ↓ nand_dispatch_task_load()            [src/nand/nand_dispatch.c]
nand_dispatch_task_t             ← 新增 uint16_t column    [src/nand/nand_dispatch_internal.h]
   ↓ req_read_column()                    [src/nand/nand_dispatch_tables.c]
nand_fc_request_t.column         ← 已有 uint16_t column ✓  [include/nand/nand_fc_interface.h]
```

`nand_dispatch_request_t.feature_addr`（8 位）**保持原样**——它与 FC 层 `fc_command_t.feature_addr`
同名，改名反而伤 grep 对齐；新增独立 `uint16_t column`，其余 op 恒 0。

## 四、`src/` 改动

| # | 文件 | 改动 |
|---|---|---|
| A1 | `include/nand/onfi_1_0.h` | 加 `#define OP_READ_COLUMN (42U)` + 时序图注释块；`OP_COUNT` **42 → 43**（`0..42 inclusive`） |
| A2 | `src/nand/onfi_1_0.c` | `g_nand_operation_map` 加 `[OP_READ_COLUMN]` 一行：`00h`/`30h`、`address_cycles = 5U`、`.has_data = 0U`、`.data_length = 4U`、`.uops = g_nand_normal_read_page_uops`（**逐字抄 op 11 那行**，uop 数组不新建） |
| A3 | `src/nand/nand_dispatch_tables.c` | 新增 `req_read_column()`（照 `req_meta_read` 写，`.column` 换成 **`task->column`**）；注册进**两张表**：`g_nand_dispatch_req_table`（`= req_read_column`）与 `g_nand_dispatch_completion_table`（`= completion_single`）；补文档注释块 |
| B1 | `include/nand/nand_dispatch.h` | `nand_dispatch_request_t` 加 `uint16_t column;` + 注释 |
| B2 | `src/nand/nand_dispatch_internal.h` | `nand_dispatch_task_t` 加同名字段 |
| B3 | `src/nand/nand_dispatch.c` | `nand_dispatch_task_load()` 加 `task->column = request->column;`（与 `feature_addr` 相邻） |
| C1 | `src/fc/fc.c` | `fc_build_dispatch_request()` 加 `OP_READ_COLUMN` 分支（照 op 11 分支写，**额外**填 `request->plane_paa[plane] = command->paa`（行地址需要）与 `request->column = param.bytes[0] \| (param.bytes[1] << 8)`） |
| C2 | `src/fc/fc.c` | `fc_complete_from_nand()` 的「暂存 → `param.bytes`」拷贝条件加 `OP_READ_COLUMN`（原为 GET_FEATURES / READ_META / READ_ONFI_SIGNATURE / BLOCK_LOCK_STATUS） |
| C3 | `src/fc/fc.c` | 补 `fc_build_dispatch_request` 的 per-op 文档条目 + `completion.param = {0}` 的注释 |
| D1 | `include/ftl/ftl_fc_interface.h` | 声明 `ftl_status_t ftl_sync_read_column(nand_addr_t addr, uint16_t column, uint8_t out[4]);` + 文档 |
| D2 | `src/ftl/ftl_fc_interface.c` | 实现：照 `ftl_sync_read_meta` 写，`sync_page_prepare(addr, OP_READ_COLUMN, …)` 之后把列写进 `cmd->param.parameter0/1`（高字节进 `parameter1`，2/3 显式清 0）；`ASSERT(column + 4 <= PAGE_SIZE)` |
| E1 | `include/ftl/bbt.h` | `bbt_print_bad_map(ftl_allocator_t *alloc, uint8_t do_erase)`；**重写文档注释**（删「擦不掉的硬标记」错误论断，写清「擦除前读列 2048，非 0xFF ⇒ 出厂坏块」+ AN1819 引文） |
| E2 | `src/ftl/bbt.c` | `bbt_probe_block_bad()` 改为**先读后擦**：① `ftl_sync_read_column(addr, PAGE_MAIN_SIZE, mark)`，`mark[0] != 0xFF` ⇒ 坏块立即返回；② `do_erase` 非 0 时才 `ftl_sync_erase_block()`，擦失败 ⇒ 坏块 |
| E3 | `src/controller/cli.c` | `uart_cli_bbt()` 解析 `--erase`；命令表 `max_argc` 1 → 2、`usage` 改 `"bbt [--erase]"`、description 改 `"print factory bad block map (--erase: also erase each block)"` |

### 关键不变量（踩过的坑）

- ⚠ **三张 `[OP_COUNT]` 表必须都填**（op-map / req-table / completion-table）。漏 req 或 completion 表
  = **NULL 函数指针调用 = HardFault**（`nand_dispatch.c` 的调用点是裸 `table[op](...)`）；
  漏 op-map = `uops == NULL` 直接 `INVALID_ARGUMENT`。
- ⚠ **`fc_build_dispatch_request` 必须建这一支**：否则落进末尾 `else` ⇒ CW 位图保持**空集** ⇒
  `nand_dispatch_task_load` **一格 `cw_buffers[]` 都不搬** ⇒ NAND 侧拿到残留指针，`DATA_OUT` **写向野地址**。
  （与 v59 的 `OP_READ_ONFI_SIGNATURE` 同一个坑。）
- ⚠ `ftl_cmd_needs_desc_chain()` **不用改**：`has_data == 0` ⇒ 自动短路不建链。
- ⚠ `fc_is_page_read` / `fc_is_page_write` / `fc_dispatch_rewrite_operation` **都不动**——本 op 不带 ECC 页数据、不是 load/fetch op。

## 五、验证

`make format-check` 过；**`make clean && make all`** → 0 warning 0 error、`.bin` **181296 B**；
**`make clean && make tests`** → 0 warning 0 error、`.bin` **259404 B**。

> ⚠ **`make all` 后直接 `make tests` 是空操作**——`tests:` 是子 make、共用同一 `OBJ_DIR`，且 CFLAGS 不被跟踪。
> 换目标必须先 `make clean`。

### 上板步骤（待办）

| 步骤 | 命令 | 期望 |
|---|---|---|
| 1 | `bbt` | **仍全 `FF`**。**这不是 bug**——本片芯片已被上一次 `bbt`（旧版先擦后读）整片擦过，**真出厂标记已经没了**。本条只验「读路径跑通、不崩、不擦芯片」：跑完 `rd 0 8 c` 仍应 `PASS` |
| 2 | `wr 0 8` → `fl` → `bbt` | 坏块图出现**少量非 `FF` 格子**（刚写过的块）。依据：FC 把 528B CW 连续铺在列 `cw*528`，CW3 覆盖列 1584..2111 ⇒ **列 2048 落在 CW3 的 USER 区**，被写过且加扰 ⇒ 读回非 `FF`。⚠ 这是**误报**（那些是好块），只用来证明「列 2048 确实读到了程序化的数据」 |
| 3 | `bbt --erase` | **破坏性，放最后**。擦除失败的块被标出（同旧行为）；跑完 FTL 数据与元数据全没了 |
| 4 | `make clean && make tests` → `test` | 期望 **58/58、0 skipped**（基线 ~870 ms） |

⚠ 只读 `bbt` 要跑 `BLOCKS_PER_PLANE × PLANE_NUM` 次页读（本仓 4096 次），数百毫秒到数秒属正常。

## 六、不做

- **不动 `feature_addr` 路径**（SET/GET_FEATURES 零改动）。
- **不建测试源**（项目规则：测试源由用户自建）。本 op 靠 `test_command_validate_op_range` 的遍历自动覆盖。
- **`bbt_scan_factory()`（开机路径）本次不动**——它另有「LDPC_RAW 不报 ECC 失败」的老问题，且开机路径要小心；只在注释里留 TODO。
- 不动 `nand_config.h`。
