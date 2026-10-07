# v13：FC 定位下沉——策略层上移 FTL

**日期**: 2026-10-07
**相关设计**: `docs/design/fc_nand_design.md` §2.9（接口表）、§25.4（建链，新增「v13」小节）、§26（与行业分层模型的对照，**新增**）
**前置**: `fc_descriptor_attach.md`（v11 机制/策略拆分）、`fc_flat_cw_mask_cq_param.md`（flat 掩码）、`fc_per_plane_row_address.md`（v12 per-plane 行地址）、`ftl_policy_b_per_plane_block.md`（Policy B）

---

## 概述

v11 把建链拆成"策略 / 机制"两层，但**两层都在 FC 内**。v13 把策略层整个搬到 FTL：

| 项 | v12（改动前） | v13（改动后） |
|---|---|---|
| 掩码枚举 | `fc_command_configure_descriptors_internal`（`src/fc/fc.c`） | `ftl_cmd_configure_descriptors()`（`src/ftl/ftl_fc_interface.c`） |
| 数据搬运 | `fc_write_cmd_data` / `fc_read_cmd_data`（`src/fc/fc_interface.c`） | `ftl_cmd_write_data` / `ftl_cmd_read_data`（`src/ftl/ftl_fc_interface.c`） |
| 建链入口 | `configure_descriptors` + `attach_cw` 两个 | **只有 `fc_command_attach_cw`** |
| 对外函数数 | 20 | 18（删 2 增 1） |

**FTL 侧 15+ 个调用点零改动**：它们全部走 `ftl_cmd_write_data` / `ftl_cmd_read_data` 这两个薄壳，
本次把薄壳**原地换成完整实现**（照搬原逻辑），调用方一行不动。

---

## 判据：硬件 FCC 寄存器表

> 拿 FC 的每一个对外函数去比对**硬件 FCC 的寄存器表**——**有对应项则保留，无对应项则剔除。**

行业里固件驱动 FCC 靠"SQ/CQ + doorbell + 描述符链"；FCC **不提供**"按掩码建链"寄存器
（描述符链是固件自己填的），也**不提供**"数据拷贝"寄存器（数据是固件 DMA 填的）。
这两件事在行业里都在**固件（FTL）侧**完成。详见设计文档 §26。

**下界**（`include/nand/nand_dispatch.h:22-24`）：FC 必须在 submit 时解析完 desc 链并交出 `cw_buffers[]`，
`nand_dispatch` 之后不再回调 FC。故造请求者必须拥有 buffer 池 → **FC 不能整体下沉**
（"只留三池、其余全给 FTL"的方案据此排除）。

---

## 逐项审计结果

**保留**（有寄存器对应物）：`fc_init`、`fc_command_alloc/release`、`fc_command_attach_cw`、
`fc_submit`、`fc_service`、`fc_poll_completion`、`fc_buffer_*`（5 个）、`fc_get_cmd_data_ptr`、
`fc_get_cmd_ecc_max`、`fc_command_id_to_command` / `fc_descriptor_id_to_descriptor`、`fc_get_resource_stats`。

**上移 FTL**（无寄存器对应物）：`fc_command_configure_descriptors`、`fc_write_cmd_data` / `fc_read_cmd_data`。

**保留判定记录**（容易被误认为"便利接口"而该删，实际有硬件对应物）：

| 函数 | 为什么保留 |
|---|---|
| `fc_get_cmd_data_ptr` | 等价于"读第 n 条描述符取缓冲地址"——硬件有对应（描述符表可读） |
| `fc_get_cmd_ecc_max` | 完成聚合（ECC 纠错计数寄存器），且 CQ 已删 `ecc_bit0..3`，聚合值只能走链 |
| `fc_get_cmd_cw_mask`（**v13 新增**） | 同样是"读描述符链"；只为 RAID 断言服务（见下） |

---

## 改动明细

### 1. FC 侧删除（`src/fc/`）

| 文件 | 改动 |
|---|---|
| `src/fc/fc.c` | 删 `fc_command_configure_descriptors_internal`（含注释块） |
| `include/fc/fc_internal.h` | 删对应声明 |
| `src/fc/fc_interface.c` | 删薄壳 `fc_command_configure_descriptors` + `fc_write_cmd_data` / `fc_read_cmd_data` |
| `include/fc/fc_interface.h` | 删三条声明；头部分组注释改「④ 数据查询」 |
| `src/fc/fc_command.c` | `fc_command_validate` 删 `param.cw_mask` 越段校验（掩码不再是 FC 的**输入**概念） |

### 2. FTL 侧新增（`src/ftl/ftl_fc_interface.c`、`include/ftl/ftl_fc_interface.h`）

| 新增 | 说明 |
|---|---|
| `ftl_cmd_plane_span()`（static inline） | plane 段规则，**与 `src/fc/fc.c` 的 `fc_command_plane_span` 是同语义的两份实现** |
| `ftl_cmd_needs_desc_chain()`（static inline） | 无数据操作 / SET·GET_FEATURES 不建链的短路条件 |
| `ftl_cmd_configure_descriptors()` | 掩码枚举 + 逐条 `fc_command_attach_cw`；**读 `cmd->param.cw_mask`** |
| `ftl_cmd_write_data()` / `ftl_cmd_read_data()` | 原地实现（原薄壳 → 完整逻辑） |

**`config.cw` 不回写**、**链序即数据布局**两条 v11/v12 约定保持不变。

### 3. 连带收敛

| # | 项 | 处理 |
|---|---|---|
| ① | `command->param.cw_mask` 字段 | **保留**。核查：v13 前该字段在 `src/` 里**只被读、从无写入者**（写入只发生在另一个实例 `completion.param.cw_mask`）。v13 后它的读者 = FTL。字段不动 |
| ② | 三处 `fc_cw_mask_is_plane_symmetric` 断言（`fc.c` 的 `fc_stripe_submit`、`fc_data_path.c` 两处） | 原读 `command->param.cw_mask` → **恒真**（无写入者）。改为读 **`fc_get_cmd_cw_mask(cmd)`**（从 desc 链重建掩码），顺带把这个既有死角变成真检查 |
| ③ | `fc_command_resources_rollback` | 唯一剩余调用者在 `#if CONFIG_FC_RAID` 内 → RAID 关闭时无引用 → 标 `__attribute__((unused))` |
| ④ | `fc_stripe_submit` 的 parity 建链 | 原调 `configure_descriptors_internal` → 改为直接循环 `fc_command_attach_cw_internal`（单 plane、CW `0..config.cw`，与 FTL 侧 `mask == 0` 展开一致） |
| ⑤ | `fc_cw_mask_popcount` / `fc_command_plane_span` / `fc_command_chain_append` / `fc_command_attach_cw_internal` | **保留**（前两者仍被 `fc_build_request_data_planes` / `attach_cw_internal` 使用；后两者是机制层） |
| ⑥ | 注释同步 | `fc_command.h`（`cw` 字段、`fc_param_t`、`param` 成员）、`fc.h`、`fc_interface.h`、`fc.c`、`fc_interface.c` 全部改写为 v13 语义 |

---

## ⚠ 顺带修掉的一个既有 bug（**非行为中性**）

原 `fc_write_cmd_data` 在"数据已耗尽"分支里：

```c
/* 旧代码 */
buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);   /* = base + FC_BUFFER_USER_OFFSET(8) */
memset(buf, 0xFF, FC_BUFFER_POOL_CW_SIZE);                  /* 528 —— 越界 8B！ */
```

`buf` 指向 CW 的 USER 区起点（偏移 8），而 `FC_BUFFER_POOL_CW_SIZE = 528` 是整个 CW 的大小。
对最后一个 CW 这会写出池尾 8 字节；对中间的 CW 会踩到**下一个 CW 的 META/CRC**。

v13 上移时改为 `FC_BUFFER_USER_SIZE`（512）并在代码里留了 `⚠` 注释说明原因。
**这是本次唯一一处刻意的行为变更**——原逻辑在任何"数据长度不是 512 整数倍且需填 dummy"的路径上
都可能静默污染相邻 CW。上板验证时值得留意。

---

## 测试影响（**必须由用户处理**，平台规则：测试源文件归用户）

`fc_command_configure_descriptors` 在测试中 **24 处**：`test_fc_level3.c` 14、`test_fc_level2.c` 5、`test_fc_nand.c` 5。
`fc_write_cmd_data` / `fc_read_cmd_data`：**0 处**。

**迁移方向**：这 24 处改为循环调 `fc_command_attach_cw`（FC 的机制原语）。
FC 层仍可测（三池、desc 链、队列、数据路径、ISSUE/WAIT/FETCH 全在）；
被移走的是**掩码枚举逻辑**，其覆盖应移到 FTL 层（`ftl_cmd_configure_descriptors` / `ftl_cmd_setup_ppa_pair`）。

`make -k tests` 实测：`test_fc_level2.c:122` / `test_fc_level3.c:168` 报
`implicit declaration of function 'fc_command_configure_descriptors'`（warning，对象仍生成）；
`test_fc_level1.c` 的编译错误是**既有问题**（v8 调度器重构后未同步），与本次无关。

---

## 顺带修掉的 v12 回归

`ftl_cmd_setup_ppa_pair()`（`src/ftl/ftl_fc_interface.c`）把"数组下标 == plane 号"当不变量，
写死 `for (plane = 0U; plane < plane_count; plane++)`。但单 plane 命令的 plane 段是
`[paa_to_plane(cmd->paa), +1)`，`fc_command_attach_cw` 对越段是**真返回** `INVALID_ARGUMENT`。

**复现链**（逐段跟代码，未上板）：

1. `wr 0 2` → `allocator_alloc_pair` 给 `new_pba[0]`(plane0) / `new_pba[1]`(plane1)；
   完成时 `lba 0 → new_pba[0]`、`lba 1 → new_pba[1]` → **lba 1 映射到 plane 1**。
2. `rd 1 1` → `pages == 1` → `multi_plane = 0`，`cmd->paa` 的 plane 位 = 1。
3. `setup_ppa_pair` 循环 `plane = 0`，span 却是 `[1,2)` → `INVALID_ARGUMENT` → 读失败。
4. `rd 1 2`（奇数 LBA 起的两页读）同样错：`pba[0]`=plane1、`pba[1]`=plane0，按下标用会把两 plane 的 PAA 对调。

**修法**：不用下标，用每个 PBA 自己的 plane 位（`pba_to_plane(pba[i])`），
循环顺序仍按 `pba[]` 数组序 → `ftl_cmd_read_data` 拷出的布局与调用者期望一致。
单 plane 落在 plane1、双 plane 顺序颠倒，都自动正确。

---

## 已知风险

| # | 风险 | 说明 |
|---|------|------|
| 1 | **plane 段规则有两份实现** | `src/fc/fc.c` 的 `fc_command_plane_span` 与 `include/ftl/ftl_fc_interface.h` 的 `ftl_cmd_plane_span` 是同语义的两份代码。**改 plane 语义（如 PLANE_NUM、BA6 含义）时必须同步改两处**——已在两处注释里互相标注 |
| 2 | 掩码越段校验移到了 FTL | FC 侧 `fc_command_validate` 不再校验 `param.cw_mask`。若将来有 FC 内部路径直接写 `param.cw_mask`，越段不再被拦。当前无此类写入者 |
| 3 | `ftl_cmd_configure_descriptors` 的失败语义 | 越段返回 `FTL_ERROR`（原 FC 返回 `FC_STATUS_INVALID_ARGUMENT`），且**不再自动回滚**已挂的半成品链——由调用者 `fc_command_release` 统一回收（与 `ftl_cmd_setup_ppa_pair` 一致） |
| 4 | `fc_get_cmd_cw_mask` 只服务 RAID 断言 | RAID 关闭（`CONFIG_FC_RAID = 0`）时是"编译进去但无调用者"的公共函数。公共函数不触发 `-Wunused-function`，无警告 |

---

## 验证

| 项 | 结果 |
|---|---|
| `make format && make clean && make debug` | ✅ 零警告零错误 |
| `make clean && make release` | ✅ 零警告零错误 |
| `make format-check` | ✅ Format check passed |
| `make -k tests` | ⚠ 仅 `test_fc_level2/3.c` 的 `fc_command_configure_descriptors` 隐式声明（预期，等用户同步）+ `test_fc_level1.c` 既有错误 |
| 上板：`wr 0 2 → rd 1 1` | ⏳ 待用户验证（回归修复前：读失败） |
| 上板：`wr 0 2 → rd 1 2` | ⏳ 待用户验证（回归修复前：两 plane PAA 对调） |
| 上板常规回归 `info → wr 0 2 → rd 0 2 c → tr 0 → fl → info` | ⏳ 待用户验证 |

**行为中性论证**：段 1 / 段 2 是"同一逻辑换位置"，FC 产出给 NAND 的 `nand_dispatch_request_t`
（`cw_mask` / `plane_paa[]` / `cw_buffers[]`）应逐字节不变——**唯一例外是上文那个 `memset` 越界修复**。
