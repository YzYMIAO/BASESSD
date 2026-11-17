---
name: arch-design
description: 架构/分层决策 —— 新增或重构结构、判断「这件事该放哪一层」、增删对外接口、跨层调用。当用户说「架构」「分层」「该放哪层」「重构结构」「加个模块」「对外接口」「跨层」时使用。
---

# 架构与分层决策

⚠ **本 skill 不装架构知识** —— 知识在本仓已有四份文档里（见 §3），装进来就是第 5 份副本、必然漂移（H6）。
本 skill 只做两件事：**定位现状**（§1）+ **动手前的决策流程**（§2）。

判据永远是 **`docs/design/fc_nand_design.md` §26**（HIL / FTL / FCC(硬件) / DRAM + 队列·门铃·完成），
**不是 Linux、不是通用 OS 编程**（§〇 / C1）。

---

## 1. 本仓分层现状

### 1.1 四层落地（细节见 `fc_nand_design.md §26.2`）

| 行业部件 | 本项目 | 说明 |
|---|---|---|
| HIL | `src/main.c` + `src/hil/cli.c` + FTL 对外接口 | 命令入口、调度驱动 |
| FTL | `src/ftl/` | L2P · GC · WL · Journal · Checkpoint · BBT · 缓冲调度 |
| **FCC（硬件）** | **`src/fc/` + `src/nand/`** | ⚠ **两者合起来才等于行业的一个硬件 FCC** |
| DRAM | `FC_BUFFER_POOL` + FTL 的 SRAM 数组 | DRAM-less：缓冲池就是「片上页缓冲」 |

`src/fc/` 与 `src/nand/` 的分界 = **FCC 的内部再拆分**：
`src/nand/` = 时序执行单元（发命令 / 等 R-B# / 搬数据）；`src/fc/` = 前端（描述符链 / 队列 / ECC / 加扰）。

### 1.2 层间契约（改动前先看这里）

| 边界 | 契约 | 要点 |
|---|---|---|
| FC ↔ NAND | `nand_dispatch_request_t`（`include/nand/nand_dispatch.h`） | **唯一索引空间 = flat 绝对 CW 号** `n = plane*CW_NUM + cw`。行地址不走这个空间，走 `plane_paa[plane]` |
| FTL ↔ FC | `include/ftl/ftl_fc_interface.h` | **PBA↔PAA 转换的唯一漏斗**（C9）；现场手写位操作 = 缺陷 |
| 平台 | `include/platform/<svc>.h`（抽象接口，零 HAL 类型）→ `src/platform/<svc>.c`（共享逻辑，零 SoC 头）→ `src/platform/stm32h743/*_port.c`（只碰 HAL/MMIO） | 见 `docs/architecture.md` |

**下界（`nand_dispatch.h` 头注释）**：FC 必须在 submit 时把 desc 链解析完并交出 `cw_buffers[]`，
`nand_dispatch` 之后**不再回调 FC**。⇒「**造请求者必须拥有 buffer 池**」——
这条约束排除了「FC 只留三池、其余全给 FTL」的方案。**动分层前先确认没撞这条。**

### 1.3 跨层违规形态（照着找）

- FTL 直接摸 FC 内部数组（该走 `ftl_fc_interface.h`）。
- 现场手写 PBA↔PAA 位操作（该走单一漏斗，C9）。
- 出现**第二套坐标**（`cw_count[]` / `cws[][]` / `slot_to_plane_cw` 之类，C9）。
- SoC 相关代码进入 Controller / FTL / NAND **算法**模块（`docs/architecture.md` 的硬规则）。
- 核心模块直接 MMIO 或直接调 `HAL_*`。

⚠ **已知现状偏差（别当已修）**：NAND 层**仍有** FMC MMIO 直写（`src/nand/nand_fmc.c`）与
`HAL_NAND_*` 直连（`nand_mt29f4g08abadawp.c`）—— 平台抽象的 **Stage 3 待办**，
include 耦合已在 Stage 1~2.6 归零。权威记录：`docs/design/platform_abstraction.md`。

⚠ **内存分区表会过时**：`docs/hardware_contract.md` 的 AXI_RAM 表是 **2026-10-13 实测快照**，
此后 v0.78 改过 `SUPER_BLOCK_LIST_MAX` ⇒ **数字可能已不准**。
**活判据是 `src/main.c:201` 的 `_Static_assert(sizeof(g_ftl) + FTL_AXI_RAM_RESERVED <= BOARD_AXI_RAM_SIZE)`** ——
**提高 CH/CE/LUN 维度前必须先重新规划内存**。

---

## 2. 结构决策流程（动手前跑一遍）

### 步骤 1 · 定层（先答这两问，答不出就别动手）

1. 这属于 **HIL / FTL / FCC / DRAM** 哪一层？
2. 归哪条**主线**：**数据面**（数据怎么走）/ **控制面**（命令怎么排）/ **可靠性面**（出错怎么救）？

自检（`essentials §1.3`）：**归不到「哪条主线的哪一层」= 理解还没到位。**

### 步骤 2 · 考点 → 出处 定位表（说「主流怎么做」时从这里引）

| 考点（§〇） | 出处 |
|---|---|
| **分层** | `fc_nand_design.md` **§26**（唯一判据）· `essentials §〇/§一` |
| **队列 + 门铃 + 完成** | `fc_nand_design.md §26.1`（NVMe / eMMC CMDQ / ZNS 三形态表）· SPDK FTL 官方文档 <https://spdk.io/doc/ftl.html>（索引见 `docs/reference/README.md` §A4） |
| **数据面轮询** | `essentials §四 FCC` · `fc_nand_design.md §5`（数据路径） |
| **通道并行仲裁** | `essentials §三/§七` · `fc_nand_design.md §17/§18`（multi-plane / 多通道） |
| **ECC-LDPC** | `essentials §四/§八` · `fc_nand_design.md §25` |
| **掉电恢复** | `essentials §五` · `docs/design/ftl_design.md` 对应章节 |

⚠ **对不上出处的，只能标「本仓自拟」，不得写成「行业标准形态」**（§〇）。
⚠ 出处只认 `docs/reference/`（论文 / 网页存档）+ `docs/design/` 两处，**不许凭记忆**。

### 步骤 3 · 要新增/修改对外接口？→ 走 §26.3 判据

> 拿这个函数去比 **硬件 FCC 的寄存器表**：**有对应项则保留，无对应项则剔除。**

本仓已有先例：`fc_command_configure_descriptors`、`fc_write_cmd_data` 被判「无此寄存器」⇒ 上移 FTL。
反例（§26.5）：把 FC 收敛成「6 个裸池原语」是**反向**的 —— 会把 FC 变成内存分配器。

### 步骤 4 · 问场景轴（主流本身分岔）

**DRAM 型 vs DRAM-less** —— 对齐时**必须说清对齐的是哪一支**。
本仓是 **DRAM-less**（`FC_BUFFER_POOL` + SRAM 数组）。场景对照见 `essentials §九`。

### 步骤 5 · 过一遍取舍清单

`essentials §十一 设计取舍清单` —— 逐条问「**我的选择是什么？代价是什么？**」
算法选型的「主流选择」见 `essentials §十`。

### 步骤 6 · 偏离留痕（P5）

有意不遵守某条规则时，在该处（代码注释 / 提交信息）写明「**偏离 Hxx，因为…**」。
**沉默偏离 = 下次读代码的人分不清「有意」与「漏了」。**

---

## 3. 出处层（本仓的架构知识都在这四份里）

| 文档 | 规模 | 内容 |
|---|---|---|
| `docs/design/storage_controller_design_essentials.md` | 733 行 | **知识参照**：按 §26 分层组织；§九 场景×方案 · §十 算法速查 · **§十一 取舍清单** · §十二 术语表 |
| `docs/design/fc_nand_design.md` **§26** | ~100 行 | 本仓**唯一**分层判据（§26.2 映射 · §26.3 寄存器表判据 · §26.5 反例） |
| `docs/design/storage_controller_learning.md` | 284 行 | 学习路线 + 开源对照 |
| `docs/reference/` | README 223 行 | 7 篇论文（FEMU/MQSim/SimpleSSD/Amber/LeaFTL…）+ 9 个网页存档 |

⚠ **本仓现状以 `docs/design/` 为准**，`essentials` 是**知识参照不是设计文档**（它自己文档头就写了）。
⚠ 空壳文档别当权威：`docs/nand_model.md` 仅 5 行。

---

## 4. 收尾

- 决策要能落到**文件:行号**或**具体文档章节**，落不到说明还只是印象。
- 报告写清：**定到哪一层 / 引的哪条出处 / 本仓现状差在哪 / 代价是什么**。
- 改代码后按 R2 过三关；碰了**建链 / 位图 / 地址**必须上板（skill `board-run`）。
- **不主动 commit / push**（R3）；未上板就标「未上板」（R1）。
