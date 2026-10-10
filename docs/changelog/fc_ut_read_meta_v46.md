# v46：新增 `OP_READ_META`（槽位 11）—— 无 desc 链只读回读 CW 的 4 字节 META，写入 CQ 的 `param`

- **日期**：2026-10-11
- **范围**：**源码 + 测试 + 文档**。`include/nand/onfi_1_0.h`、`src/nand/onfi_1_0.c`、`src/nand/nand_dispatch_tables.c`、
  `src/fc/fc.c`（新增 op + FC 派发/完成两处）；`tests/unit/fc_test_common.{c,h}` + `tests/unit/test_fc_level3.c`
  （新夹具 + 新用例）；本文档 + `docs/design/fc_nand_ut.md`。
  **`src/ftl/**` 一行不动**、`tests/framework/**`、`ut_main.c`、`tests/integration/**` 不动。
- **介质格式**：无变更（只**读**回 CW 前 4 字节，不写）。
- **测试**：**用例数 52 → 53**（Level 3 **10 → 11**，新增 `block_read_meta`）。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、链接 **225140 B**（原 223572）；
  固件 `make` 0/0、**166628 B**。
- ⚠ ~~**需上板**：op 11 **首次下发**（`00h`-addr-`30h` 后只输出 4 字节 = 部分页输出）。~~
  ✅ **上板已验**（2026-10-11，seed `0x5a4f21e4`）：`block_read_meta` **1 of 1**、全套 **53 of 53 (100%)**。

## 〇、起因：坏块扫描要一个「只读 META」的接口

坏块扫描需要**只读**回读页内 META——CW 布局 `[META:4][CRC:4][USER:512][PARITY:8]`，
**前 4 字节就是 META**，位于加扰区之前（`FC_BUFFER_USER_OFFSET = 8`）、**明文**——
据此判工厂坏块标记。现状只有 `ftl_sync_read_page_raw`（op 12 LDPC_RAW，**整页 + ECC**）
能靠**返回码**判坏块；`include/ftl/bbt.h:62` 一直挂着一个待补项：等一个「读 spare/OOB 的 FC 接口」。

用户要求：**新增一个 op，只读回读 META，不带 desc 链**（格式固定，不需要数据路径介入），
把 4 字节 META 直接写进 CQ 的 `fc_param_t param`（`include/fc/fc_command.h:178`）。

**FTL 集成不在本次范围**（`src/ftl/**` 一行不动）——本版只把 op 造出来 + 单测背书，供后续/自建扫描用。

## 一、设计定案

| 决策 | 结论 |
|---|---|
| 选哪个 CW | **由 `command->pba` 的 cw 字段选**：`pba.cw → pba_to_paa()`（`nand_config.h:360`，**保留 cw 位**）`→ plane_paa[] → paa_to_cw(...) * CW_SIZE` 作列地址。无需 desc |
| 加载方式 | **自带页加载（自洽）**：op 内含 `70h` 轮询 → `00h`+列/行 → `30h` → `tR` → `DATA_OUT`，单条命令完成整件事 |
| OP 槽位 | **复用已删除槽位 11**（原 `OP_READ_MODE`，读语义最贴）⇒ **`OP_COUNT` 保持 42 不变**（空位只剩 9 / 20） |
| 改动范围 | src + Level 3 单测 + 文档/changelog（同 v43~v45） |

**命名**：`OP_READ_META (11U)`，op 表 `name = "READ_META"`。

## 二、`src/` 改动

| # | 文件 | 改动 | 理由 |
|---|---|---|---|
| A | `include/nand/onfi_1_0.h` | 「操作码 11 已删除」墓碑注释 → `OP_READ_META (11U)` 定义 + 说明（`onfi_1_0.h:169-182`） | 复用槽位，`OP_COUNT` 不变 |
| B | `src/nand/onfi_1_0.c` | op 表加 `[OP_READ_META]`（`onfi_1_0.c:825`） | 字段照 op 12，但 `.has_data = 0U`、`.data_length = 4U`、`.op_type = 0U`、`.pair_op = 0U`、`.uops = g_nand_normal_read_page_uops`。**不新增 uop 数组**（线上序列与普通读页相同，复用即可） |
| C | `src/nand/nand_dispatch_tables.c` | 新 handler `req_meta_read()`（`:322` 附近）+ 注册进 req 表（`:577`）与 completion 表（`:517`） | 列地址 = `paa_to_cw(plane_paa[plane]) * CW_SIZE`，行地址取自同一 PAA，`data_length = 4` |
| D | `src/fc/fc.c` | `fc_build_dispatch_request` 新增 `OP_READ_META` 分支（`:1359`，与 FEATURES **同型**） | 无 desc 时行地址只能取自 `command->pba`；位图只置本 plane 的 CW0；`cw_buffers[]` 挂 `g_fc_return_staging[command_id]`；**必须建这一支**——否则落到末尾 `else`，暂存指针不落位、`DATA_OUT` 写向残留指针 |
| E | `src/fc/fc.c` | `fc_complete_from_nand` 的 staging→`param.bytes` 拷贝条件由「仅 `OP_GET_FEATURES`」扩为「`OP_GET_FEATURES` 或 `OP_READ_META`」（`:798-802`）；同步 `:758` 注释 | 4 字节从控制器侧暂存拷进 CQ `param` |

### 关键机制（已核实）

- **uop → FMC**：`COLUMN_0/1` 写 `request->column`，`ROW_0/1/2` 写 `request->row[]`，
  `DATA_OUT` 转移 `request->data_length` 字节到 `request->data + uop->operand`。
  故设 `column = cw * CW_SIZE`、`data_length = 4` ⇒ 从该 CW 起只输出 META 四字节。
- **4 字节落点** = 控制器侧暂存 `g_fc_return_staging[command_id]`（`fc.c:101`），
  与 **`OP_GET_FEATURES` 完全同款**（建 `fc.c:1333-1356`、拷回 `fc.c:800-807`）——
  该机制已在板上验证过（v36 SET/GET_FEATURES 回环）。
- **无 desc 链**：`has_data = 0` ⇒ `ftl_cmd_needs_desc_chain`（`ftl_fc_interface.h:85`）与
  `fc_command_validate`（`fc_command.c:69`）都不要求链。
- **不进 ECC / 数据路径**：op 11 不在 `fc_is_page_read`（`fc.c:118`）名单 ⇒
  `fc_complete_from_nand` 走「非 page read」分支（`fc.c:787`），只透传状态 + 拷 staging。
- **不参与 CW 链**：op 11 不是 load/fetch op（`nand_dispatch.c:538-549`），
  `nand_dispatch_task_cw_chain` 落到 `else break`（`:616`）；`scatter_page_buffer` 对它也是 no-op。
- **不被改写**：`fc_dispatch_rewrite_operation`（`fc.c:1222`）只动 op 12/30；op 11 `op_type = ATOMIC`、
  `pair_op = 0` ⇒ 不可拆。

### ⚠ 生产路径零影响（已核实）

`fc_is_page_read` 未改（op 11 默认 0）；`fc_dispatch_rewrite_operation` 未改；
`fc_command_validate` 未改（`has_data = 0` 已放行）。新增的 FC 分支只对「命令本身就是 op 11」生效，
现有读/写路径不受影响。

## 三、`tests/` 改动

### 夹具（`tests/unit/fc_test_common.{c,h}`）

- `test_cmd_build` 的 `ASSERT(addr->cw == 0U)` 放宽为
  `ASSERT((addr->cw == 0U) || (config->operation == (DWORD)OP_READ_META))`（`fc_test_common.c:312`）——
  该 op 的 `cw` 就是**列选择器**，其余 op 维持原断言。
- 新 `test_read_meta(block, plane, page, cw, out[4])`（`fc_test_common.c:816` 起）——
  `test_cmd_build(&addr{cw=cw}, 0U, 0U, &conf{operation=OP_READ_META})` → `test_submit_wait`
  → 校验 `completion.status` 无错误位 → 拷 `completion.param.bytes[0..3]` 到 `out`。
  头文件加声明 + 说明块（`fc_test_common.h:307`）。

### 新用例 `block_read_meta`（`tests/unit/test_fc_level3.c`）

```
erase + prog block(plane)                      ← 由 FC 写路径写入 META
  for cw ∈ {0, 2}:
    test_read_meta(block, plane, page=PAGES_PER_BLOCK/2, cw, got)
    assert got == (prog_cmd->meta & ~PLANE_CW_MASK) | (plane << PBA_PLANE_SHIFT) | cw
```

抽 plane / block（顺序即复现契约）。这条同时验证**页地址**与 **CW 选择**两条轴——
META 高位含 page、低位含 plane/cw（每 CW 的 META 公式见 `src/fc/fc_interface.c:197`）。
证据走 `test_trace_summary`（info）+ `test_trace_detail`（debug）打出读回值。

## 四、验证

```bash
make format-check                  # 过
make clean && make tests           # 0 warning 0 error；链接 225140 B；用例数 53
```

**上板（用户跑，本版唯一验收方式）**：

```
test --log-visible debug /fc-nand/lifecycle/block_read_meta
```

验收点：读回 4 字节 == 编程时写入的 META（CW0 与 CW2 各一，值不同）；`status` 无错误位；
用例 1 of 1；全套 **53 of 53**。

## 五、风险 / 需上板确认（如实记录）

- **op 11 从未在板上跑过**（新命令）；`00h`+列+`30h`+`tR` 后只输出 4 字节属**部分页输出**——
  与 GET_FEATURES 的 4 字节输出同族，但作用在**页读路径**，需上板确认器件接受。
- 4 字节落点是 `.bss` 的 `g_fc_return_staging`（非显式 non-cacheable 段）。**先例 = GET_FEATURES
  已用同一暂存在板上跑通**（v36），若存在 D-Cache 隐患则两者同源，非本版引入。
- 坏块扫描语义：本 op 带 `CHECK_FAIL` ⇒ 读失败时命令报 `NAND_FAIL`，正好对应「读页失败 = 坏块」。
- **FTL 未接入**：`src/ftl/**` 不动，`bbt_scan_factory` 仍走 op 12。本 op 供后续/自建扫描用。

## 不做

- 不动 `src/ftl/**`、`tests/framework/**`、`ut_main.c`、`tests/integration/**`；
- 不翻 `CONFIG_FC_RAID`；
- **不改 `OP_COUNT`**（复用槽位 11）；不改 `fc_is_page_read` / `fc_dispatch_rewrite_operation`；
- 不处理 OTP（op 26/27/28）——资源不可恢复，不做测试。

## 出处

- META 布局与写入：`src/fc/fc_data_path.c:91`（`fc_data_path_meta_write`）、`:225`（调用点，取 `desc->meta`）。
- 每 CW META 公式：`src/fc/fc_interface.c:196-197`（`(command->meta & ~PLANE_CW_MASK) | plane_cw`）。
- PBA→PAA 保留 cw：`include/nand/nand_config.h:360`（`pba_to_paa`）；`paa_to_cw` / `paa_to_nand_row` 同文件 `:193-203`。
- GET_FEATURES 同款暂存：`src/fc/fc.c:101`（`g_fc_return_staging`）、`:1333-1356`（建）、`:800-807`（拷回）。
- op 表：`src/nand/onfi_1_0.c`（`[OP_READ_META]`）。
- 待补接口：`include/ftl/bbt.h:62`（读 spare/OOB 的 FC 接口）。
