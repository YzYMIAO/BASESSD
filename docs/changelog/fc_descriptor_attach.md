# 建链拆为机制 / 策略两层：新增 `fc_command_attach_cw()`

**日期**: 2026-10-07
**相关设计**: `docs/design/fc_nand_design.md` §2.9（接口表 + 两层说明）、§2.10（生命周期）、§25.4（建链）
**前置**: `fc_flat_cw_mask_cq_param.md`（flat 掩码）、`fc_cw_index_unification.md`（CW 索引空间统一）

---

## 概述

`fc_command_configure_descriptors_internal` 把两件事揉在一起：

1. **策略**——决定"访问哪些 CW"（按 `param.cw_mask` 枚举，`mask == 0` 时按 `config.cw` 展开）；
2. **机制**——分配 desc + buffer、填 PAA/config/meta、串到链尾。

v11 把机制抽成独立原语 `fc_command_attach_cw_internal` 并公开为 `fc_command_attach_cw()`；
策略函数保留、签名与返回码不变，内层循环改为调用该原语。上层因此可以**自己算集合**，
逐条追加（同页内任意 CW 子集），**链序即 `fc_read_cmd_data` / `fc_write_cmd_data` 的数据布局顺序**。

**对现有行为逐字节中性**：FTL 生产路径全部走 `fc_command_configure_descriptors()`，
其掩码一律由 `config.cw` + `multi_plane` 推导（连续满掩码），展开与串链顺序均未变。

---

## 动机

用户提出的问题：

> 不太灵活，如果是有读取同读取离散 cw 的场景这个就无法使用，有什么好的设计能把功能拆分，上层自己组装

核实后的事实：

- **缺的不是表达能力，是组装入口**。`param.cw_mask` 本就是 flat 绝对掩码，
  单测 `test_cw_mask_flat_mixed`（`tests/unit/test_fc_level2.c:834-868`）用的就是 `0x29`
  （plane0 `{CW0, CW3}` + plane1 `{CW1}`）——稀疏子集本来就能表达。
- **但没有任何公共入口能挂一条 desc**：`fc_command_configure_descriptors(command)` 是唯一建链入口，
  单 desc 的 static helper 外部摸不到；且 `param` 是与 FEATURES 内联字节共用的 union，
  直接改掩码要动 SQ 提交后的不可变约定。

**用户澄清（二选一）**：场景范围 = **同一页内任意 CW 子集**（不要求各 plane 行地址不同、不做跨页 gather）；
API 形状 = **逐条追加原语**（上层循环调用），现有策略函数退化为"按掩码循环调它"。

---

## 改动

### 1. 机制层（`src/fc/fc.c`）

| 函数 | 职责 |
|---|---|
| `fc_command_plane_span(command, &first, &last)` | 给出命令能触及的 plane 段（multi-plane 从 plane 0 起；单 plane 只含 `paa_to_plane(command->paa)`）——**从策略函数里抽出的共享逻辑** |
| `fc_command_configure_descriptor_internal(...)` | 既有 helper，**不动**：分配 desc + buffer、填 PAA/config/meta |
| `fc_command_chain_append(controller, command, descriptor)` | 走 `next_id` 找链尾并挂接；前驱 `config.chain = 1`，空链时记入 `command->desc_id` |
| `fc_command_cw_attached(controller, command, plane, cw)` | 判重（仅用于 `ASSERT`，故 `__attribute__((unused))`，同 `fc_cw_mask_popcount`） |
| `fc_command_attach_cw_internal(controller, command, plane, cw)` | **机制原语** = 段校验 → `configure_descriptor_internal` → `chain_append` |

链长 ≤ `CW_TOTAL_MAX` = 8，找尾的 O(N) 线性走链可接受，且省掉了策略循环里原来的 `previous` 簿记。

### 2. 策略层（`fc_command_configure_descriptors_internal`）

保留全部语义（NO_DATA 早退、`mask == 0` 逐 plane 展开、`mask & ~allowed` 校验、
失败整体 `fc_command_resources_rollback`），**只把内层循环换成调 `fc_command_attach_cw_internal`**。

### 3. 公开面

| 文件 | 改动 |
|---|---|
| `include/fc/fc_interface.h` | 新增 `fc_command_attach_cw()` 声明 + 文档块；接口分组补 `attach_cw`；生命周期注释补"自组装"分支 |
| `src/fc/fc_interface.c` | 新增薄壳（`command != NULL` 断言 + `fc_command_to_id` 身份校验 + 状态 `ALLOCATED` 校验 → 转 internal），与 `fc_command_configure_descriptors` 同构 |
| `include/fc/fc_internal.h` | 声明 `fc_command_attach_cw_internal`，标注机制 / 策略分工 |
| `include/fc/fc.h` | 生命周期注释补"或按需自组装"分支 |

> `fc_command_attach_cw_internal` 是**非 static**（对外薄壳要调它），与其余 `fc_*_internal` 一致；
> 首次编译时误标 `static` 触发 `error: static declaration follows non-static declaration`，已改正。

---

## 校验取舍

| 情形 | 行为 | 理由 |
|---|---|---|
| `cw >= CW_NUM` | `INVALID_ARGUMENT` | 调用者传入的数据 |
| `plane` 越出本命令 plane 段 | `INVALID_ARGUMENT` | 与策略层对越段掩码的处理一致（`mask & ~allowed`） |
| 池空（desc/buffer 耗尽） | `RESOURCE_EMPTY`，**不动已挂好的前序 desc** | 半成品 desc 仍在所有权链上，由 `fc_command_release` / `fc_command_resources_rollback` 统一回收 |
| 同一 `(plane, cw)` 追加两次 | **`ASSERT`** | 掩码是集合，重复置位会静默丢数据；与 `fc_build_request_data_planes` 的"同一 `n` 不被两条 desc 覆盖"同源同因 |

参数取 `(plane, cw)` 而非整个 `nand_paa_t`：范围锁定"同页"，PAA 里的行地址位必然被忽略，
传进来只会误导；也与内部 helper 的既有签名一致。

---

## 行为中性论证

- **不回写 `param.cw_mask`**：机制层只动 desc 链；掩码仍由 `fc_build_request_data_planes` 从链推导。
  故 `fc_command_validate` 的掩码段校验（`fc_command.c`，`mask == 0` 时跳过）天然通过，
  保持"SQ 提交后不可变"约定。
- **链序不变**：策略层枚举顺序仍是 plane-major（先 plane 0 全部置位 CW，再 plane 1），
  与 v9 起的行为逐字节相同；desc 的 `config.cw`、`meta`、`paa` 组装路径未动。
- **FTL 零影响**：FTL 从不写 `param.cw_mask`，全部经策略层建链，本次不改变其任何输入输出。

---

## 不做的事

- **不动行地址**：`command->paa` 仍是全命令唯一的行地址来源，`fc_build_request_data_planes` 的
  `plane_paa[]` 构造不变。要支持"各 plane 不同页"是另一件事（需 desc 自带行地址 + 改请求构造）。
- **不动 `fc_stripe_submit` / `fc_data_path` 的"链序 == CW 号"假设**（`CONFIG_FC_RAID = 0`，
  且只对连续满掩码成立）——本次不扩大稀疏面；已知软点记录在此。
- **不改测试源文件**（平台规则：测试归用户）；不新建测试。

---

## 验证

| 项 | 结果 |
|---|---|
| `make format-check` | 通过 |
| debug | 零警告（152200 B；+492 B vs v10 的 151708 B） |
| release（`NDEBUG`） | 零警告（86856 B；+48 B vs 86808 B）——`fc_command_cw_attached` 标 `unused`，无 `-Wunused-function` |
| `make -k tests` | `test_fc_level2.o`（含稀疏掩码用例 `0x29` / `0xF0` / `mask==0` 展开）**零诊断**；仅卡在**用户维护**的 `test_fc_level1.c`（旧 scheduler API）与 `test_fc_level3.c:48`（旧 `nand_dispatch_set_device_ready`），与本次改动无关 |
| 静态核对 | `fc_command_attach_cw` 对 (a) 段外 plane、(b) `cw >= CW_NUM` 返回 `INVALID_ARGUMENT`；(c) 重复 `n` 走 `ASSERT` |

**上板验证未做**（平台规则：用户执行）。本次为纯拆分，正常路径行为不变，
常规 `wr/rd/tr/fl/info` 回归即可。
