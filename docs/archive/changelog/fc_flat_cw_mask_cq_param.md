# FC：flat CW 掩码 + CQ 返回项重做

**日期**: 2026-10-06
**相关设计**: `docs/design/fc_nand_design.md` §2.2、§2.5、§13、§14、**§25**；`docs/design/fc_api_usage.md`
**前置**: `fc_cw_subset_read.md`（同日，v6：`cw_mask : 4` + 逐 CW 取数读路径）

---

## 概述

两件事一起做，因为改同一批文件（`include/fc/fc_command.h`、`fc_complete_from_nand`、`fc_build_dispatch_request`）。

**① flat CW 掩码（读恢复精度）**

v6 的 `cw_mask : 4` 是 **CW 号掩码广播到每个 plane**——建链处对每 plane 套同一组 CW，
`failed_cw_mask` 同理无 plane 维度。多 plane 读恢复只能把失败并集回填 → 两个 plane 都重读并集里的每个 CW，
即使某 CW 只在 plane 0 失败。

改成 **flat plane-major 槽位掩码**（bit `n = plane*CW_NUM + cw`，覆盖 `CW_TOTAL_MAX = 8`）：两 plane 子集可不同，
回填即最小重读集。

**② CQ 返回项重做（SQ 不可变原则）**

v6 的 GET_FEATURES 把 NAND 读缓冲指针**直接指向命令自己**（`&command->parameter0`），结果原地回填进命令体——
提交后还在改 SQ。按"命令配置完即完成、返回项只走 CQ"的原则，改成：CQ 用同一个 `fc_param_t` 联合体承载命令级返回；
SQ 提交后不被写。

**范围**：FC + NAND dispatch。②迫使 FTL 改两个调用点（`ftl.c` / `gc.c` 消费 CQ 的 per-CW ECC）——见「风险」。

---

## 掩码编号（绝对，不是每-plane 低 4 位）

| 选择 | 掩码 |
|---|---|
| 两个 plane 各 4 CW（满读） | `0x0F \| 0xF0 = 0xFF` |
| **只读 plane 1 的 4 个 CW** | **`0xF0`**（不是 `0x0F`） |
| plane 0 取 {CW0,CW3} + plane 1 取 {CW1} | `0b0010_1001 = 0x29` |

single-plane 命令同理：plane 1 的单 plane 命令掩码也是 `0xF0`，bit 落在 `paa_to_plane(command->paa)` 那个 plane 的区间；
建链处 `allowed` 掩码挡住"plane1 命令写了 bit0..3"。

`cw_mask` 取 `uint32_t` 而非 4 位位域，正是"位要够用"：`32 / CW_NUM = 8`，掩码天然容纳 `PLANE_NUM ≤ 8` 的将来扩展——
这也是把它放进 `fc_param_t` 联合体（而非 `fc_command_t` 里一个 4 位位域）的直接理由。

---

## 已定决策

| # | 决策 | 取值 |
|---|------|------|
| 1 | 掩码语义 | flat：bit `n = plane*CW_NUM + cw`；0 = 未指定（退化：每 plane 取 `CW 0..config.cw`） |
| 2 | 联合体 | 新类型 `fc_param_t`（`parameter0..3` / `bytes[4]` / `cw_mask`），**SQ 与 CQ 共用** |
| 3 | SQ 侧 | `fc_command_t.param`：读命令 = 请求掩码 `cw_mask`；SET/GET_FEATURES = 内联数据 |
| 4 | CQ 侧 | `fc_command_status_t.param`：读命令 = 失败掩码 `cw_mask`；GET_FEATURES = 寄存器值 |
| 5 | CQ 的 ECC | **完全去掉** `ecc_bit0..3`；per-CW ECC 只留在 `fc_descriptor_t.ecc_bits`，FTL 改走新 API |
| 6 | CQ 的失败掩码 | 去掉独立 `failed_cw_mask` 位域，改用 `param.cw_mask` |
| 7 | 每-plane 计数 | `cw_count` 标量 → `uint8_t cw_count[PLANE_NUM]`；链长度以 **desc 链**为真值 |
| 8 | GET_FEATURES 暂存 | 控制器级 `g_fc_return_staging[FC_COMMAND_POOL_DEPTH][4]`，按 `command_id` 索引。NAND 层 `nand_dispatch_completion_t` **不加字段** |
| 9 | `config.cw` 归宿 | **纯输入提示，configure 阶段不再回写**；`fc_descriptor_t.config.cw` 语义不变 = 本 desc 是 plane 内第几个 CW |
| 10 | `fc_get_cmd_data_ptr` | 改成按 **CW 号**匹配（`paa_to_cw(desc->paa) == cw_index`）+ 链尾返回 NULL |
| 11 | RAID / stripe | **ASSERT 禁止非对称 flat 掩码**（XOR 按 CW 配对，两 plane 必须同集合） |
| 12 | 非法掩码 | `fc_command_validate` 返回 `FC_STATUS_INVALID_ARGUMENT`；建链处保留 ASSERT 兜内部不变量 |
| 13 | 掩码位宽上限 | validate 硬限 `mask <= (1 << (PLANE_NUM * CW_NUM)) - 1`（今天 `0xFF`） |
| 14 | 写命令掩码 | **允许非零**（`req_program_page` 天然按 `paa_to_cw` 落位）；FTL 写路径继续写整 page（mask 恒 0） |
| 15 | 单 plane 多 plane 命令 | **不做降级优化**（不把 `00h-00h-30h` 改回单 plane 加载）；op 选择只由 `multi_plane` 决定 |
| 16 | 单元测试 | **全部重新适配**（机械适配 + 补 flat 语义用例） |
| 17 | 文档 | 与代码同批更新 |

---

## 改动

### 1. `include/fc/fc_command.h`

- 新增 `fc_param_t` 联合体（`parameter0..3` / `bytes[4]` / `cw_mask`），SQ 与 CQ 共用。
- `fc_command_t`：删 `cw_mask : 4`（`reserved0` 回 `: 16`），`parameter0..3` → `fc_param_t param;`。**24 字节不变**。
- `fc_command_status_t`：删 `ecc_bit0..3 : 8×4` 与 `failed_cw_mask : 4`；新增 `fc_param_t param;`。**12 字节不变**。
- 新增 `uint8_t fc_cw_mask_is_plane_symmetric(uint32_t mask);`。

### 2. `include/nand/nand_dispatch.h` + `src/nand/nand_dispatch_internal.h`

`uint8_t cw_count;` → `uint8_t cw_count[PLANE_NUM];`；删 `uint8_t parameter[4];`。注释同步。

### 3. `src/fc/fc.c`

- **`fc_command_configure_descriptors_internal`**：掩码读 `command->param.cw_mask`；`mask==0` 时**对命令的每个 plane `p` 置位 `[p*CW_NUM, p*CW_NUM + config.cw]`**（P0：绝不能照抄 `(1<<(config.cw+1))-1`，那只有 plane 0）；
  枚举 `for plane: for cw: if (mask >> (plane*CW_NUM+cw)) & 1`。**删掉 `config.cw` 回写**。single-plane 加 `allowed` 掩码校验。
- **`fc_command_descriptor_count`**：改走链计数（供所有权校验）。
- **`fc_build_dispatch_request`**：
  - 删 `request->parameter[0..3]` 拷贝（死链，NAND 侧 `task->parameter[]` 一并删）。
  - SET/GET_FEATURES：`cws[plane][0].buffer` 指向 `g_fc_return_staging[command_id]`（**不再指向 command**）。
  - `cw_count[plane]` 由 `slot[plane]` 计数落位；multi-plane 的链长推导改走链。
  - **空 plane 的行地址**：为命令 plane 集合里的**每个** plane 都填 `cws[plane][0].paa`（CW=0），与 `cw_count[plane]` 是否为 0 无关。
- **`fc_complete_from_nand`**：Step 1/3 的 `total_cw = multi_plane*(config.cw+1)` 改走链；删 `ecc_bit0..3` 填充；
  失败掩码 `completion.param.cw_mask |= 1U << (paa_to_plane(d->paa)*CW_NUM + paa_to_cw(d->paa))`；
  GET_FEATURES 从 `g_fc_return_staging` 拷进 `completion.param.bytes`。
- **`fc_fill_cw_meta_paa`**：`meta_total` 改走链。
- **`fc_stripe_submit`**：`mp_req.cw_count = cw_count` → `[p] = cw_count`；加对称性 ASSERT。
- **`fc_init_internal`**：加 `g_fc_return_staging` 的 memset。

### 4. `src/nand/nand_dispatch.c`

- **`nand_dispatch_task_load`**：`cw_count` 逐 plane 拷；**槽位 0 无条件拷**（承载行地址，空 plane 也要带对 BA6）+ 槽位 `1..count-1` 按界拷；**删 `task->parameter[0..3]`**；single-plane ASSERT 改 `cw_count[request->plane] > 0`。
- **新增 `nand_dispatch_task_total_slots`**（多 plane = Σ `cw_count[]`）与 **`nand_dispatch_slot_to_plane_cw`**（在 `cw_count[]` 上走减，替换除法/取模）。
- **`nand_dispatch_task_cw_chain`**：`total_slots` 走 helper；槽位→`(plane, ordinal)` 走 helper。
- **`nand_dispatch_scatter_page_buffer`**：循环界改 `cw_count[plane]`。
- `nand_dispatch_submit` 的 `ASSERT(request->cw_count <= CW_NUM)` 改逐 plane。

### 5. `src/nand/nand_dispatch_tables.c`

- `completion_all_cws` / `req_program_page` / `req_single` / `req_no_data` / `req_fetch_page` / `req_cache_read` → `cw_count[task->plane]`。
- `completion_multi_plane` → plane-major 链走位（`slot++`，不是 `plane*CW_NUM+cw`）。
- `req_multi_plane_program` 内层 → `cw_count[plane]`。
- 删 `task->parameter[]` 相关注释。

### 6. `src/fc/fc_data_path.c` / `fc_command.c` / `fc_interface.c`

- `fc_data_path_process_read` 的 `total_desc` 改走链；stripe parity 加对称性 ASSERT。
- `fc_command_status_init`：删 `ecc_bit1..3 = FC_CW_STATUS_UNUSED` 三行 + 随之无用的 `count` 变量。
- `fc_command_validate`：新增掩码校验（越界 / 命令 plane 区间内零置位 → `FC_STATUS_INVALID_ARGUMENT`）。
- `fc_get_cmd_data_ptr`：改成按 CW 号匹配 + 链尾返回 NULL（uniform 下逐字节等价）。
- `fc_write_cmd_data` / `fc_read_cmd_data`：循环上界由 `config.cw+1` 改为走链。
- **新增 API** `uint8_t fc_get_cmd_ecc_max(const fc_command_t *command)`：走 desc 链取 `ecc_bits` 最大值（**取全链**，含两 plane）。

### 7. 调用点机械替换

| 位置 | 说明 |
|---|---|
| `src/ftl/ftl.c` / `src/ftl/gc.c` | `completion->ecc_bit0..3` 的 max 循环 → `fc_get_cmd_ecc_max(cmd)` |
| `src/ftl/read_recovery.c` | `cmd->parameter0..3` → `cmd->param.parameter0..3` |
| `tests/integration/test_fc_nand.c` | `->parameter0..3` → `->param.parameter0..3`；新增 `fc_nand_test_wait_completion_ex` 回传 CQ |
| `tests/unit/test_fc_level1.c` | 删 ecc 断言；改断 `param.cw_mask` |
| `tests/unit/test_fc_level3.c` | `CMD_FIELD_U(parameter0..3)` → `CMD_FIELD_U(param.cw_mask)` |

---

## 关键不变量

1. **desc 链是长度的唯一真值**：链多长 / 每 plane 几个 CW 一律走链得出，不从 `config.cw` 反推。
2. **链走位仍与 CW 号解耦**（v6 已立）：`cw_status[]` / `cws[plane][slot]` 下标是**链走位**；CW 号一律 `paa_to_cw(desc->paa)`。
3. **`config.cw` 是纯输入提示，configure 阶段不再回写**：语义 = `mask==0` 时的「每-plane CW 数 − 1」；`mask!=0` 时被忽略。uniform 命令下与 v6 逐字节等价。
4. **SQ 命令提交后不被写**：唯一的运行期写入是引擎侧 `task`（NAND 内部），不碰 `fc_command_t`。

---

## 风险

1. **链长度真值切换（最高）**：把 `multi_plane*(config.cw+1)` 换成走链，漏改一处就在 flat 命令下算错长度。**uniform 命令（全部 FTL 现状）必须逐字节等价** —— 回归护栏。
2. **`cw_count` 标量 → 数组**：公共结构体字段变更，漏改在 uniform 下可能静默正确，只在 flat 下暴露。必须静态审计。
3. **single-plane 掩码编号是绝对的**：plane1 的单 plane 命令若按老习惯写 bit0..3 会选不中任何 CW；建链 ASSERT 只在 debug 兜底。
4. **CQ 去 ECC 波及 FTL**：`ftl.c` / `gc.c` 的 `rr_record_ecc` 口径从"plane 0 的 4 CW 最大值"变成"全链最大值"。对多 plane 读**数值会变**。
   理由不只是"更准"——`fc_complete_from_nand` Step 3 的 ECC 折叠带 `if (paa_to_plane(d->paa) == 0U)`（v6），plane 0 空时它根本不写；flat 掩码下"只取 plane 0"会恒为 0，是错的而非保守的。
5. **SQ 不可变靠约定**：`fc_param_t` 在 CQ 侧是返回值，谁若在提交后读 `command->param.cw_mask` 会读到请求集（不是结果）。
6. **上板未验**：op 序列不变（读路径仍是"加载 + 逐 CW 05h/06h-E0h"），但平面选择、列地址、返回项来源都变了。
7. **空 plane 的行地址（flat 独有）**：只有 flat 语义才允许某 plane 计数为 0（如只读 plane 1 = `0xF0`）。多 plane 加载/编程的行地址取自 `cw_paa[plane][0]`，空 plane 不显式填 PAA 会退化成 block0/page0 —— 静默误操作，且**在 uniform 命令下完全测不出来**。
8. **`mask == 0` 展开（与风险 1 并列最高）**：展开若仍写 `(1 << (config.cw+1)) - 1`，flat 下只覆盖 plane 0，**所有 FTL 多 plane 命令当场退化**。
9. **RAID / stripe 与 flat 掩码不兼容**：XOR 要求两 plane 同 CW 集合，加 ASSERT 禁止 stripe 命令用 flat 掩码。

---

## 验证

1. **编译**（debug 与 tests 共用 build 目录，中间要 `make clean`）：
   ```
   make format && make clean && make debug
   make clean && make tests
   make clean && make release
   ```
2. **静态审计**：
   - `src/` 里不应再有 `multi_plane * ...(config.cw + 1)` 的链长推导；`config.cw + 1` 只允许出现在 `mask==0` 展开处（`fc.c` 一处）、stripe 配对数与 FEATURES 分支。
   - `src/nand/` 里不应再有 `cw_index / cw_count` 或 `% cw_count`。
   - `src/` 里不应再有 `ecc_bit0`；`task->parameter` / `request->parameter` 应完全消失。
   - `src/` 里不应再有裸 `->cw_mask`（`fc_command_t` 侧）——一律 `->param.cw_mask`。
3. **上板**（按平台规则由用户执行，`make tests` 固件经 UART CLI）：现有全部读用例（单/多 plane）+ SET/GET_FEATURES 用例必须通过。
4. **单元测试**：
   - 机械适配：`test_fc_level1/2/3.c` + `test_fc_nand.c` 的字段引用全部改齐。
   - 新语义用例：`0x29` 混合掩码；只选 plane 1（`0xF0`）；`mask==0 + multi_plane=2` 建 8 条 desc。
