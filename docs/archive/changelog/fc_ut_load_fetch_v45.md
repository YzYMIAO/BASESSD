# v45：加载 / 取数两段式显式下发 —— op 33/29 支持「不带 desc」，op 8/41 带校验取数

- **日期**：2026-10-11
- **范围**：**测试 + 源码**。`src/nand/onfi_1_0.c`、`src/fc/fc.c`、`src/nand/nand_dispatch_tables.c`（让加载型 op 可无 desc 下发）；
  `tests/unit/fc_test_common.{c,h}` + `tests/unit/test_fc_level3.c`（新夹具 + 新用例）；本文档 + `docs/design/fc_nand_ut.md`。
  **`src/ftl/**` 一行不动**、`tests/framework/**`、`ut_main.c`、`tests/integration/**` 不动。
- **介质格式**：无变更。
- **测试**：**用例数 51 → 52**（Level 3 **9 → 10**，新增 `block_load_fetch`）。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、链接 **223572 B**（原 220908）。
- ⚠ ~~**需上板**：op 33/29 **首次作为独立命令下发**（生产里只作 rewrite 目标）。~~
  ✅ **上板已验**（2026-10-11，seed `0x5a4f21e4`）：`block_load_fetch` **1 of 1**、全套 **53 of 53 (100%)**。

## 〇、起因：盘点 Level 3 的 OP 码覆盖（承接 v44）

v44 盘覆盖时确认：op 33/29/8/41 在生产读路径里**每读必跑**，却**从未作为独立命令被点名**——

- `fc_dispatch_rewrite_operation`（`src/fc/fc.c:1221`）下发前把 `OP_NORMAL_READ_PAGE_WITH_DATA_OUT`(12)
  改写成 `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT`(33)（多 plane：30→29）；
- `nand_dispatch_task_cw_chain`（`src/nand/nand_dispatch.c:574`）再对加载型 op 逐 CW 发取数 op
  `OP_RANDOM_DATA_READ`(8, 05h-E0h) / `OP_RANDOM_DATA_READ_MULTI_PLANE`(41, 06h-E0h)。

⇒ 这四个 op 的 **uop 序列**、**page-read 分类**、**页缓冲散落**三者**零测试背书**，写错了没人拦。

用户要求把它做成**可独立下发的两段**：op 33/29 作**不带 desc 链**的纯加载命令，随后用 op 8/41 带 desc 链取回并逐 CW 校验。

## 一、`src/` 改动：让加载型 op 可「无 desc」下发

| # | 文件 | 改动 | 理由 |
|---|---|---|---|
| A | `src/nand/onfi_1_0.c` | op 33 / op 29 的 `has_data` `1U` → `0U` | `has_data` = 「是否涉及用户数据传输」。加载型 op **不搬 host 数据** ⇒ `ftl_cmd_needs_desc_chain` / `fc_command_validate` 不再强制建链 |
| B | `src/fc/fc.c` | `fc_is_page_read` 补 `OP_RANDOM_DATA_READ_MULTI_PLANE` | 与 op 8 对称；否则直接下发 op 41 拿到**未解码**数据 |
| C | `src/fc/fc.c` | `fc_build_dispatch_request` 新增「无 desc 加载」分支（op 33/29） | 无 desc 时行地址只能取自 `command->pba`：op 33 取本 plane、op 29 逐 plane 补 plane 位；**不建 CW 位图、不占 buffer** |
| D | `src/nand/nand_dispatch_tables.c` | `req_read_page` 空掩码时 `data = NULL, data_length = 0` | `cw_buffers[]` 是**上一次占用该 slot 的残留值**（`nand_dispatch_internal.h` 契约注释）；空掩码下 `cw_first(0)=0` 会读到残留指针 ⇒ 显式清掉 |

### ⚠ 生产路径零影响（已核实）

- `fc_build_dispatch_request` 的分支按 **FC 命令的 op**（12/30）选；op 33/29 的 `has_data` **只在「命令本身就是 33/29」时**才被读——即本版新增的直接下发。
- `fc_is_page_read(41)` 在生产里**从不被查**（生产命令 op 恒 30，走 30 分支建链）。
- `req_read_page` 的空掩码分支：生产 op 33 的位图**恒非空**（由 op 12 的 rewrite 带出来）⇒ 新分支只服务本版的直接下发。

⇒ 上述 `src/` 改动对现有读路径都是 **dead path**，不改行为。

## 二、`tests/` 改动

### 夹具（`tests/unit/fc_test_common.{c,h}`）

- `test_is_multi_op` 补 op 29（多 plane 加载）与 op 41（06h-E0h 多 plane 取数）⇒ `test_cmd_build` 派生 `multi_plane = PLANE_NUM`。
- 新 `test_load_page(block, plane_bitmap, page, config)` —— **纯加载**：`test_cmd_build(..., cw_bitmap = 0, meta = 0, ...)`（无 desc ⇒ `desc_id` 保持 `INVALID`），submit + 校验 status，**不**校验数据（无 desc ⇒ 无 CW 落位）。ASSERT `config->operation` ∈ {op 33, op 29}；多 plane 要求 `plane_bitmap == TEST_PLANE_BM_ALL`，单 plane 要求恰一个 plane。
- 新 `test_random_data_read(block, plane_bitmap, page, config)` —— **取数**：op 8/41 + **整段掩码** desc 链，`test_memset_read_buffer` 预填 0xCC → submit → `test_verify_page_paa` 逐 CW 校验。ASSERT `config->operation` ∈ {op 8, op 41}；op 8（05h 无 plane 位）要求单 plane，op 41 要求 `TEST_PLANE_BM_ALL`。
  - **一条命令取全段**：`nand_dispatch.c` 的 fetch 推进——每完成一个取数 op 就推进到掩码的下一个置位 ⇒ 整段掩码一次取回全部 CW。

### 新用例 `block_load_fetch`（`tests/unit/test_fc_level3.c`）

```
Phase 1（单 plane）：erase + prog block(plane)
  → test_load_page(op 33，无 desc)  → test_random_data_read(op 8，带 desc)  → 逐 CW PAA 校验
Phase 2（双 plane）：erase + prog block_mp(两 plane)
  → test_load_page(op 29，无 desc)  → test_random_data_read(op 41，带 desc) → 两 plane 校验
```

抽 plane / block / block_mp（顺序即复现契约）。证据：`test_trace_summary`（info）+ `test_trace_detail`（debug）打出加载/取数的 op 与掩码。

## 三、验证

```bash
make format-check                  # 过
make clean && make tests           # 0 warning 0 error；链接 223572 B；用例数 52
```

**上板（用户跑，本版唯一验收方式）**：

```
test --log-visible debug /fc-nand/lifecycle/block_load_fetch
```

验收点：两段各自 `status` 无错误位；`test_random_data_read` 的逐 CW PAA 校验通过（单 plane 与双 plane 各一）；用例 1 of 1；全套 **52 of 52**。

⚠ **本版的风险点（如实记录）**：`op 33/29 作为独立命令下发` 在本板上**从未跑过**（生产里只作 rewrite 目标）。若器件不接受「`00h-30h` 加载后无后续 `30h` 输出即驻留页缓冲」，Phase 1 会当场红——那本身就是本版要暴露的结论（要么 uop 编码有误，要么该命令不能独立用）。此时按「断言失败即真实信号」处理，不绕过。

## 不做

- 不动 `src/ftl/**`、`tests/framework/**`、`ut_main.c`、`tests/integration/**`；
- 不翻 `CONFIG_FC_RAID`；
- 不新增 op（只让已有 op 33/29 可无 desc 下发）；
- 不处理 OTP（op 26/27/28）——资源不可恢复，不做测试。

## 出处

- rewrite：`src/fc/fc.c:1221`（`fc_dispatch_rewrite_operation`）。
- 逐 CW 取数：`src/nand/nand_dispatch.c:574`（`nand_dispatch_task_cw_chain`）。
- op 表：`src/nand/onfi_1_0.c`（op 29/33 的 `has_data`）。
- 残留指针契约：`src/nand/nand_dispatch_internal.h:41`。
- 夹具：`tests/unit/fc_test_common.c` 的 `test_read_cache_random`（v44 同型 helper）。
