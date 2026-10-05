# `cw_paa` / `cw_buffers` 第一维统一为物理 plane 索引

**日期**: 2026-10-06
**涉及的提交**: （尚未合并，当前工作区改动）
**相关设计**: `docs/design/fc_nand_design.md` §4、§14、§17

---

## 概述

`nand_dispatch_request_t.cws[PLANE_NUM][CW_NUM]` 与 `nand_dispatch_task_t.cw_paa/cw_buffers[PLANE_NUM][CW_NUM]` 的第一维此前有**两套语义**：

- **multi-plane 命令**：物理 plane 索引（`[0]` / `[1]` 都用，由 `paa_to_plane(desc->paa)` 决定）。
- **单 plane 命令**：`[0]` 是"第 0 组槽位"——**恒取 `[0]`**，即使命令实际打的是 plane 1。plane 1 的寻址信息不在下标里，而编码在 PAA 的 bit8（经 `paa_to_nand_row()` 映射到 NAND 行地址 bit6 = BA6）。

功能上原本是对的，但下标名不副实：`[0]` 看起来像"plane 0"，实际是"第 0 组"。10 个 `req_*` handler + `cw_chain` + `scatter` 一律写死 `[0]`，正确性完全依赖 FC 侧一条**无断言守卫的隐式约定**（"单 plane 命令必须全填 `cws[0][*]`"）。

本次把第一维**统一为物理 plane 索引**：单 plane 命令落到 `cws[plane][*]` / `cw_paa[plane][*]`；multi-plane 的 `[0]/[1]` 不变。

## 行为中性说明

**行地址始终由 `paa_to_nand_row()` 从 PAA 值算出，与数组下标无关**，故本次是纯语义修正，上板行为不变。唯一的破坏方式是 producer（FC 落位）与 consumer（`req_*` 读取）索引不一致——对 plane=1 命令会读到全零行 → `paa_to_nand_row(0) = 0` → 误操作 plane0/block0/page0。为此在 `nand_dispatch_task_load()` 增加断言自检。

## 改动

### 1. 新增贯穿字段 `plane`

| 文件 | 改动 |
|------|------|
| `include/nand/nand_dispatch.h` | `nand_dispatch_request_t` 新增 `uint8_t plane`（插在 `plane_count` 与 `page_type : 2` 位域之间，无重新打包） |
| `src/nand/nand_dispatch_internal.h` | `nand_dispatch_task_t` 新增 `uint8_t plane`（插在 `plane_count` 与 `started` 之间）；注释说明必须无条件赋值（task slot 复用、`task_load` 不整体 memset） |

语义：单 plane = `paa_to_plane(command->paa)`；multi-plane 恒 0（多 plane uop 硬编码 `row[0]/row[1]`）。

### 2. FC 侧（`src/fc/fc.c`）

| 位置 | 改法 |
|---|---|
| `fc_build_dispatch_request` ERASE_BLOCK | `request->plane = paa_to_plane(command->paa)`，落位 `cws[request->plane][0]`（此分支无 desc，只能用 `command->paa`） |
| 同函数 ERASE_BLOCK_MULTI_PLANE | 显式补 `request->plane = 0U`（文档锚点，防误改） |
| 同函数 SET/GET_FEATURES | `request->plane = paa_to_plane(command->paa)`，buffer 写到 `cws[request->plane][0]`。行为恒等（feature 命令从不设 `paa`，plane 恒 0），改它是为保住"单 plane 数据恒在 `cws[request->plane][*]`"这条无例外规则 |
| 同函数 multi-plane 数据分支 | `cws[plane][cw_s]` 保持；补 `request->plane = 0U` |
| 同函数 single-plane 数据分支 | 分支开头取一次 `plane = paa_to_plane(command->paa)`；三处 `cws[0][...]` → `cws[plane][...]`；加 `ASSERT(paa_to_plane(descriptor->paa) == plane)` 把隐式不变式显式化 |
| `fc_stripe_submit` | 补 `mp_req.plane = 0U` |

### 3. NAND 侧

| 文件 | 改动 |
|------|------|
| `src/nand/nand_dispatch.c` `nand_dispatch_task_load` | 新增 `task->plane = request->plane`（无条件赋值）；照拷循环改为 `for (plane) for (cw)` 嵌套（不能按 `plane_count` 循环——单 plane 的 `plane_count == 0` 会拷 0 行）；新增单 plane PAA 自检断言（排除无 PAA 语义的 SET/GET_FEATURES） |
| 同文件 `nand_dispatch_task_cw_chain` | `cw_paa[0][...]` / `cw_buffers[0][...]` → `[task->plane][...]` |
| 同文件 `nand_dispatch_scatter_page_buffer` | 单 plane 分支 `cw_buffers[0][cw]` → `[task->plane][cw]`；multi-plane 分支不动 |
| `src/nand/nand_dispatch_tables.c` | 单 plane handler 全部 `[0]` → `[task->plane]`：`req_program_page`、`req_read_page`、`req_fetch_page`、`req_cache_read`、`req_erase`、`req_feature`、`req_single`、`req_no_data`（含头注释）。**`req_multi_plane_read/program/erase` 不动**（起始 plane 恒 0，`[0]/[1]` 字面量保持） |

### 4. 文档同步

- `docs/design/fc_nand_design.md`：§4 流程图（`cws[plane][cw]`）、§14 Feature 路径（`cw_buffers[task->plane]`）、§15 handler 表、§16 单 plane 读链（`cw_buffers[task->plane][N]`）、§17.1 加"后续修订"注、§17.10 设计演进表加 v5 行。
- `docs/design/assertion_guidelines.md`：`5×cw_buffers` 计数说明补一句索引已改为 `[plane]`（计数不变）。

## 风险

1. **producer/consumer 索引不一致（最高危）**：plane=1 命令若一侧写 `cws[0]`、另一侧读 `[task->plane]`，会读到全零行 → 行地址 0 → 误擦 plane0/block0。缓解：一次性改齐 + `task_load` 断言 + grep 审计。
2. **误改 multi-plane 路径**：若把 `req_multi_plane_*` 也换成 `[task->plane]`，因 `plane == 0` 会让两个 plane 都取 `[0]`，plane1 行地址丢失、双 plane 退化为同 plane。
3. **`task->plane` 漏赋值** → 读到上一条命令残值。
4. **`cw_status` 被误动**：它索引的是 **desc 链序号**（plane-major，由 `fc.c` 的 `plane = index/CW_NUM` 保证），与 `cw_paa` 的物理 plane 索引是两套独立坐标。本次一行未改。

## 验证

1. `make debug` 编译通过（二进制 151616 → 152248 字节，增量来自新增断言的字面量与调用）。
2. `make format` 幂等，`git status --porcelain` 只列本次改动文件。
3. 静态 grep 审计：`grep -rn "cw_paa\[0\]\|cw_buffers\[0\]\|cws\[0\]" src/ include/` 仅剩 multi-plane / stripe 站点。
4. **测试覆盖缺口（明确）**：现有单 plane 用例全部用 plane=0，只能验证行为中性；没有任何"单 plane 命令打 plane 1"的用例。建议补 on-target 用例 `test_block_single_plane1`（erase(block, plane=1) → write → read 逐字节比对），这是本次唯一的新语义路径。测试源文件由用户决定是否新增。
5. 上板验证按平台规则由用户执行。
