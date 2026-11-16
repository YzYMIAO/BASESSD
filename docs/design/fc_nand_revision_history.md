# FC-NAND 设计变更历史

> 本文档记录 FC（Flash Controller）与 NAND 底层模块设计过程中的关键讨论、方案反转、
> 被否方案和验证记录，结构对齐 `docs/design/ftl_revision_history.md`。
>
> **单一事实源**：当前设计只在 `docs/design/fc_nand_design.md`；本文只回答「为什么变成现在这样」。
> 本文中的符号一律按**当时**的名字出现（`nand_dispatch_*`、`nand_fc_*`、`status.module` 等），
> 不回改成现行名——改名本身就是这里要记录的事件。现行名与旧名的对照见 §6。
>
> 变更的完整 diff 以 `docs/archive/changelog/` 为准；本文给索引、决策理由与验证记录，不复制提交内容。

---

## 0. 文档定位与使用

| 问题 | 看哪里 |
|------|--------|
| 现在怎么设计、当前结构体/接口是什么 | `docs/design/fc_nand_design.md` |
| 某个机制为什么是现在这样、换过什么方案 | 本文 §3 / §4 |
| 某个版本改了哪些文件、验证到什么程度 | 本文 §4.11 与对应 `docs/archive/changelog/*.md` |
| 缺陷、未验证项、待决策 | `docs/design/known_risks.md` / `known_iss.md` |
| FTL 侧的演进 | `docs/design/ftl_revision_history.md` |

**阅读顺序**：先读 §1 版本总览定位到版本；再读 §2（Phase）或 §3（vN）中对应条目；
需要方案对比时跳 §4 专题；§5 是被否方案清单，避免重新提议已经否决过的设计。

**记录口径**：

- 「内容」= 当轮做了什么；「决策」= 为什么这样选；「拒绝方案」= 当时比较过但没有采用的方向。
- 验证记录只写**当时**的结论与证据位置。未上板的一律写「未上板」，不因后续版本通过而追认。
- 历史事实不改写。后续版本的更正写在新条目里，并注明它推翻了哪一条。
- 裸写的 `§N` 指**当时**的 `fc_nand_design.md` 章节（章节号随重构变化）；指向本文条目的写作
  `§2.x` / `§3.x` / `§4.x`；指向原始讨论稿的写作 `docs/archive/fc_design_discussions.md` §N。

---

## 1. 版本总览

### 1.1 功能阶段（Phase A–Y）

| 阶段 | 主题 | 详述 |
|------|------|------|
| A | `buffer_id` 便捷接口（`fc_write_cmd_data` / `fc_get_cmd_data_ptr`） | §2.1 |
| B | 核心结构重组（status / descriptor / ECC / dispatch 边界） | §2.2 |
| C | SET/GET_FEATURES 内联数据路径 | §2.3；`docs/archive/fc_design_discussions.md` §14 |
| D | dispatch 函数指针表重构 | §2.4；`docs/archive/fc_design_discussions.md` §15 |
| D(续) | per-LUN chain buffer 命令链执行 | §2.5；`docs/archive/fc_design_discussions.md` §16 |
| E | Multi-Plane 操作 + 行地址数组 + OP 码拆分 | §2.6、§4.5 |
| F/G/H | WAIT_READY 中断驱动、DMA 中断完成、ONFI 时序切换 | §2.7 |
| I | 读饥饿保护（QoS） | §2.8、§4.7（后被 v8 删除） |
| J | 加扰引擎扩展（多算法 + 多种子策略） | §2.9、§4.9 |
| K | BCH ECC 引擎移植 + QC-LDPC 硬判决 | §2.10 |
| L | CRC-32 引擎实现 + 数据路径集成 | §2.11、§3.1 v33 |
| M | Buffer 偏移重构（`FC_BUFFER_USER_OFFSET` / `fc_buffer_get_base`） | §2.12 |
| N | 跨 Plane Stripe RAID 重构 | §2.13 |
| V | 多 LUN 并发 ISSUE/WAIT/FETCH + FMC 后端数组化 + 调度器相位机 | §2.14、§4.6（FMC 数组化后被 Phase Z 撤销） |
| X | FC-NAND 全模块注释补全 + 行业方案对比 | §2.15 |
| Y | 初始化 / 运行时全链路走读 | §2.16 |
| — | 注释规范：全模块函数契约（`/** */` + `//`） | §2.17 |

### 1.2 版本化重构（有独立变更记录）

| 版本 | 日期 | 主题 |
|------|------|------|
| v8 | 2026-10-06 | per-LUN FIFO（深 4，每槽一条命令），删 Phase 1/2、busy 表、读饥饿保护 |
| v9 | 2026-10-06 | CW 索引空间统一为 flat 绝对号 |
| v10 | 2026-10-07 | 删除软件寄存器影子 |
| v11 | 2026-10-07 | 建链拆为机制/策略两层 |
| v12 | 2026-10-07 | per-plane 行地址（Policy B） |
| v13 | 2026-10-07 | FC 定位下沉，策略层上移 FTL |
| v15 | 2026-10-07 | 删除独立「所有权链」，所有权真值收敛回 desc 链 |
| v16 | 2026-10-08 | 消除「已出池未挂链」中间态，改为 desc 出池即挂链 |
| v17 | 2026-10-08 | 建链三层接力合并为唯一公共入口 |
| v23 | 2026-10-08 | FTL 建链收敛为「单一掩码旋钮」 |
| v24 | 2026-10-08 | LBA/PBA/PAA 地址布局统一 + 加扰器按存储主控对齐 |
| v25 | 2026-10-08 | 地址构造统一走 `nand_addr_s` 具名字段 |
| v27 | 2026-10-09 | `cw_mask` → `cw_bitmap`，取消「0 = 段内全部」 |
| v28 | 2026-10-09 | CW 集合退出命令结构体，改作建链形参 |
| v29 | 2026-10-09 | 数据路径拆成单 CW 步骤原语 |
| v33 | 2026-10-10 | CRC 引擎改走 STM32H743 HAL 驱动 |
| v47 | 2026-10-11 | `fc_cmd_s.pba` → `.paa`，PBA 退出 FC 层 |
| v52 | 2026-10-11 | 链锁：链未结束期间该 LUN 的填充权归链所在队列 |
| v53 | 2026-10-11 | 命令级 buffer 归属位域 `buf_own : 2` |
| — | 2026-10-09 | 协议查证：multi-plane 不在 ONFI 1.0 里 |
| — | 2026-10-23 | 器件扩展：two-plane cache 读写落地 |
| — | 2026-10-30 | FTL cache read / cache program 组链（推翻「FTL 恒 END」口径） |

未列出的 v0.x~v60 见 `docs/archive/changelog/` 与 `docs/design/ftl_revision_history.md`（FTL 侧版本）。

---

## 2. 功能阶段（Phase A–Y）

### 2.1 Phase A — buffer_id 便捷接口

**内容**：在 `buffer_id` 句柄模型之上增加两条便捷接口 `fc_write_cmd_data` / `fc_get_cmd_data_ptr`，
让上层不必每次自己按 CW 号算 buffer 基址。

**结局**：两条接口后来随 v13「策略层上移 FTL」删除——数据搬运按硬件 FCC 判据不属于 FC。
`fc_get_cmd_data_ptr` 的读侧能力由 ID 版 `fc_buffer_get_user_data` / `fc_get_cmd_data_ptr` 保留。

### 2.2 Phase B — 核心结构重组

**内容**：status / descriptor / ECC / dispatch 边界一次性重组：
`fc_cmd_s` 承载命令级配置与地址，`fc_desc_s` 承载 per-CW 的 PAA + buffer + ECC 状态，
CQ 条目只承载命令级聚合状态与返回段，Dispatch 只跨硬件真正消费的字段。

**决策理由**：CQ 是命令级快照，per-CW 细节若塞进 CQ 会随出队丢失；Descriptor 链是命令生命周期内
唯一保留 per-CW 状态的地方（Read Retry / RAID 恢复都要用）。

### 2.3 Phase C — SET/GET_FEATURES 内联数据路径

**内容**：Feature 操作的内联数据（4 字节）经 `cw_buffers[plane*CW_NUM]` 指向控制器级暂存
`g_fc_return_staging[command_id]`，不再在命令对象里另开一段参数区。

**决策理由**：

- Feature 值 4 字节，但要经过与普通数据相同的 dispatch 数据指针路径，才能让 NAND 引擎只有一种搬运模式；
- 结果回读必须离开 SQ 槽位——早先直接回填进命令自身，提交后命令被复用会与上层读到脏值；
- 控制器级按 `command_id` 索引的暂存区同时服务 GET_FEATURES / READ_META / 动态参数三类返回段。

**详述**：`docs/archive/fc_design_discussions.md` §14。

### 2.4 Phase D — dispatch 函数指针表重构

**内容**：`nand_dispatch` 的 request 构建与 completion 处理改为按 `operation` 查表，
表项分 request handler / completion handler 两类；协议属性（是否带数据、数据长度、pair op）
收敛进 `nand_op_map_s`。

**决策理由**：早先 `switch (request->operation)` 分散在多处，新增 OP 码要同步改多处且容易漏；
查表让「新增一个 OP = 填一行表」，并把协议属性从 handler 内联常量搬到一个可审计的地方。

**详述**：`docs/archive/fc_design_discussions.md` §15。

### 2.5 Phase D(续) — per-LUN chain buffer 命令链执行

**内容**：一条命令链在 dispatch 侧占连续 FIFO 槽，`submit_chain` 原子入队；链内逐条推进、
逐条完成；调度器按 `config.chain` 检测链头/链尾。

**决策理由**：多 CW / 多 plane 操作若拆成多条独立命令，同 LUN 上会被其他命令插队，
导致「逻辑上一条命令」的部分执行顺序不可控。链把「连续槽 + 不可插入」变成结构保证。

**详述**：`docs/archive/fc_design_discussions.md` §16。

### 2.6 Phase E — Multi-Plane 支持

**内容**：支持器件 2-plane READ/PROGRAM/ERASE；`cw_paa` / `cw_buffers` 从 `[CW_NUM]` 扩为
`[PLANE_NUM][CW_NUM]`；`row` 改为 `row[NAND_PLANE_MAX]` 并由 UOP `operand` 选择 plane；
读命令拆 WITH/WITHOUT_DATA_OUT 两个 OP 码；新增 5 个 multi-plane OP 码。

**决策**：见 §4.5 的方案对比（plane 维度、operand 索引、双版本读）。

**后续**：v9 把二维数组整体删除，改为 flat 绝对 CW 号 + per-plane `plane_paa[]`（§4.4）。

### 2.7 Phase F/G/H — WAIT_READY 中断驱动、DMA 中断完成、ONFI 时序切换

**内容**：WAIT_READY 由 R/B# 中断驱动；DMA 传输以完成中断收尾；NAND 由低速时序切到 ONFI 高速模式。

**结局**：R/B# EXTI 后因「单 EXTI 无法扩展到多 LUN」被移除，WAIT_READY 回到纯 GPIO 轮询；
DMA 仍是单对 Stream；ONFI 时序切换保留（当前 Mode 0 → Mode 5）。

### 2.8 Phase I — 读饥饿保护

**内容**：调度器按「连续 N 条非读后优先读」旋转 SQ，防止写/擦长期占用 LUN 饿死读。

**结局**：v8 随 per-LUN FIFO 重构整体删除。当前平台 `CH_NUM=CE_NUM=LUN_NUM=1`，
保护本就近乎无效；完整记录见 §4.7。

### 2.9 Phase J — 加扰引擎扩展

**内容**：加扰引擎支持多算法与多种子策略，`FC_RANDOM_SEED_STRATEGY` 提供三选一旋钮。

**结局**：v24 删除种子策略旋钮，种子固定为「本 CW 的逻辑地址（`desc->meta`）」；
v24 同时修正 LFSR 抽头错误（状态空间曾坍缩到周期 63）。见 §3.1 v24、§4.9。

### 2.10 Phase K — BCH ECC + QC-LDPC 硬判决

**内容**：移植 BCH 引擎，并加入 QC-LDPC 硬判决模式。

**现状口径**：编译引擎为 BCH（m=13, t=4）；LDPC hard/soft 枚举存在但未完整支持，
`FC_ECC_MODE_LDPC_RAW` 表示跳过解码。

### 2.11 Phase L — CRC-32 引擎

**内容**：CRC-32 引擎实现并接入数据路径，覆盖 USER 512 B。

**后续**：v33 发现 `FC_CRC_BASE` 写错（ADC3 地址）导致 `g_crc_ready = 0`、
`fc_crc_compute()` 恒返 0，CRC 校验自始恒通过；改走 platform HAL 驱动后修正。

### 2.12 Phase M — Buffer 偏移重构

**内容**：引入 `FC_BUFFER_USER_OFFSET`（8 B）与 `fc_buffer_get_base`，把「CW 基址」与
「USER 区基址」分开命名。USER 区固定为 `[8, 520)`，前 8 B 是 META(4) + CRC(4)。

### 2.13 Phase N — 跨 Plane Stripe RAID

**内容**：删除旧的「覆盖末 CW」写法，新增 `fc_data_path_generate_stripe_parity`；
`fc_poll_internal` 做 stripe 积累并以 Multi-Plane 提交；parity 命令由 FC 内部申请/释放。

**现状口径**：`CONFIG_FC_RAID = 0`，功能代码完成但未验证（R5）。

### 2.14 Phase V — 多 LUN 并发 ISSUE/WAIT/FETCH

**内容**：每个读写擦操作拆成 ISSUE / WAIT / FETCH 三段 OP 码，dispatch 用 LUN 相位机
（IDLE → ISSUE → ISSUED → FETCH）在 tR/tPROG/tBERS 期间切到其他 LUN；FMC 后端曾数组化为
per-(ch,ce,lun)，调度器改为填充轮次（fill pass）。

**后续**：FMC 后端数组化被 Phase Z（`22707cb`）撤销回单例 `g_nand_fmc_instance`；
OP 码拆分与相位机保留。完整记录见 §4.6。

### 2.15 Phase X — 注释与行业对比

**内容**：FC-NAND 全模块注释补全；新增行业方案对比章节（现设计文档 §21）。

### 2.16 Phase Y — 初始化 / 运行时全链路走读

**内容**：设计文档新增初始化（图 14–16）与运行时（图 17–22）章节，补源码关键路径注释。

### 2.17 注释规范（2026-10-29）

**内容**：全模块函数契约补全（`@brief` / `@param` / `@return`），统一 `/** */` 契约块 +
`//` 实现细节（CLAUDE.md S5/S6）。

**决策理由**：契约块与函数紧贴、标题等于函数名，便于工具抽取；实现细节只写 why /
不变量 / 失败后果，不重复 what。

---

## 3. 版本条目

> 以下条目按版本顺序排列；每条保留当时的问题、方案与验证结论。
> 条目内的 `§x.y` 指**设计文档**章节（设计文档保留原章节号）；本文专题在 §4。

### 3.1 v8–v53

#### v8 — 2026-10-06

FC→NAND 分发重构为 **per-LUN FIFO（深 4，每槽 = 一条命令）**——删 per-channel FIFO / Phase 1 / Phase 2 / busy 表 / Phase I 读饥饿保护；FC 侧改填充 pass（§2.7、§4.2/4.3、§16、§20、§23.5–23.9）

#### v9 — 2026-10-06

CW 索引空间统一为 **flat 绝对号 `n = plane*CW_NUM + cw`**——删 `cw_count[]`/`cws[][]`/`cw_paa[][]`/`slot_to_plane_cw`，新增 `plane_paa[PLANE_NUM]`（§25.11、§13–§14、§23.8）

#### v10 — 2026-10-07

删除**软件寄存器影子** `fc_registers` / `fc_register_queue_state()`——字段重复 + 零读者，队列用量改由 `fc_get_*_count` / `fc_get_resource_stats()` 提供（§2.8）

#### v11 — 2026-10-07

建链拆为**机制 / 策略**两层——新增逐条追加原语 `fc_command_attach_cw()`（分配 desc+buffer 并挂链），`fc_command_configure_descriptors()` 退化为「按掩码循环调它」的策略层，上层可自行组装任意 CW 子集（§2.9、§25.4）

#### v12 — 2026-10-07

**per-plane 行地址**——`fc_command_attach_cw()` 增 `row_paa` 形参（该 plane 的行地址），`fc_build_request_data_planes` 的 `plane_paa[]` 改为从 desc 链推导。multi-plane 命令的两个 plane 自此可指向**不同 block**（Policy B：每 plane 独立分配块 + 坏块替换）。策略层传 `command->paa` 派生的同页地址，对 v11 及以前**逐字节中性**（§2.9、§25.4）

#### v13 — 2026-10-07

**FC 定位下沉——策略层上移 FTL**。按「硬件 FCC 寄存器表」判据（§26）删除 FC 的 `fc_command_configure_descriptors()`（掩码枚举）与 `fc_write_cmd_data()` / `fc_read_cmd_data()`（数据搬运），改由 FTL 侧 `ftl_cmd_configure_descriptors()` / `ftl_cmd_write_data()` / `ftl_cmd_read_data()` 承担；FC 侧建链只剩 `fc_command_attach_cw()` 一个入口，新增 `fc_get_cmd_cw_mask()`（从链重建掩码，供 RAID 断言）。顺带修掉 v12 的 `ftl_cmd_setup_ppa_pair` plane 下标回归（§2.9、§25.4、§26）

#### v17 — 2026-10-08

**建链收敛——三层接力 → 一条公共入口**。`fc_command_attach_cw`（公共壳）→ `fc_command_attach_cw_internal`（段校验）→ `fc_command_configure_descriptor_internal`（机制体）三层合并为**唯一一条** `fc_command_configure_descriptor`（**单数**），实现在 `src/fc/fc_interface.c`；后两条删除，`fc.c` 只留池/链原语（`fc_descriptor_alloc_internal` / `fc_command_chain_append` 去 `static` 进 `fc_internal.h`）。Stripe RAID parity 建链改调公共入口。校验全部前置于出池，非法实参不污染池子（§2.9、§2.10、§25.4；changelog `fc_build_chain_convergence_v17.md`）

#### v23 — 2026-10-08

**FTL 建链收敛为「单一掩码旋钮」**。`param.cw_mask == 0` 重定义为「**本命令 plane 段内的全部 CW**」（v7~v22 是"每 plane 连续 `0..config.cw`"），两个建链入口（`ftl_cmd_configure_descriptors` / `ftl_cmd_setup_ppa_pair`）共用唯一定义处 `ftl_cmd_resolve_cw_mask()`；`config.cw` 自此退出建链（读者只剩 FC 内 RAID stripe 的 `cw_count`），`sync_page_prepare` 的 `cw` 死参数删除；掩码越段由 `FTL_ERROR` 改 `ASSERT`（§2.10、§25.2、§25.4；changelog `ftl_cw_mask_single_knob_v23.md`）

#### v24 — 2026-10-08

**地址布局统一（LBA / PBA / PAA）+ 加扰器对齐存储主控**。三套地址各自定位：**PAA** 是芯片地址（MT29F4G08 的 BA6/PA 规则决定，**一个字节都没动**）、**PBA** 是系统物理地址、**LBA** 是逻辑地址；PBA 与 LBA 改为**逐位同构**的 `[ch:ce:lun:block:page:plane:cw]`（plane 在 page 之下），PAA 保持 `[cw:page:plane:block:lun]` 不动。于是建链时 `desc->paa` 与 `desc->meta` 用**同一个公式**（基值不同）算出。加扰种子由物理地址 PAA 改为**本 CW 的逻辑地址**（`desc->meta`，即扇区 LBA），并**删除 `FC_RANDOM_SEED_STRATEGY` 三选一旋钮**；加扰范围由 `[0,520)` 收窄到 `[8,520)`，**META/CRC 保持明文**，使读路径能从页内明文 META 自举出种子（GC / 掉电重建不再需要为取种子而改）。顺带修掉 LFSR 抽头错误（状态空间坍缩到周期 63，见 §21.3.5）（§2.2、§2.3、§2.9、§21.3.5；changelog `fc_scrambler_lba_seed_v24.md`）

#### v27 — 2026-10-09

**`cw_mask` → `cw_bitmap`，并取消「0 = 段内全部」重载**。字段改名（FC 侧 `fc_cmd_s.param.cw_bitmap`、NAND 侧 `nand_dispatch_request_t.cw_bitmap`，以及 `fc_get_cmd_cw_bitmap` / `fc_cw_bitmap_is_plane_symmetric` / `ftl_cmd_resolve_cw_bitmap` / `nand_dispatch_cw_first`/`_next` 形参）——名字叫 bitmap 就必须**逐位字面**：置 1 的位 = 要访问的 CW，**全 0 = 一个都不访问**（v23~v26 的「0 = 本命令 plane 段内的全部 CW」取消）。代价是「生产者忘填」与「有意访问 0 个 CW」不可区分，唯一拦网是 `ftl_cmd_resolve_cw_bitmap()` 的两条 ASSERT（release 下被裁掉 ⇒ 忘填 = 静默空操作）。故新增 `fc_command_plane_span_cw_bitmap(cmd)` 作为「整段访问」的**显式**写法，三个整段入口（`addr_to_cmd` / `ftl_cmd_setup_ppa` / `ftl_cmd_setup_ppa_pair`）各自调用它填位图（须在 `cmd->pba` 与 `config.multi_plane` 都写好之后）；只有 `ftl_cmd_configure_descriptors()` 接受调用者给的部分位图。顺带消除「两层 0 方向相反」：SQ 请求集、CQ 失败集、NAND 请求集三处现在**同口径**（0 = 空集）（§2.2、§2.10、§25.2、§25.4；changelog `fc_cw_bitmap_literal_v27.md`）

#### v28 — 2026-10-09

**CW 集合退出命令结构体——改为建链形参**。删除 `fc_param_u.cw_bitmap` 这个联合体视图：SQ 侧（请求集）改作 `ftl_cmd_configure_descriptors(cmd, cw_bitmap)` 的**形参**（它是建链的输入、读者只有 FTL，没有留在命令里被回读的理由），CQ 侧（失败集）**整个删除**——上层要定位失败 CW，遍历 desc 链 OR 各 `desc->status` 即可（FC 不再代为派生）。`fc_cmd_status_s.param` **保留**（GET_FEATURES 的寄存器回读仍走它）。顺带删掉擦除的**假 CW 位**（v7~v27 给 ERASE 伪造一个 CW0 位，只为让 completion 有格可写）——擦除请求位图自此是**合法空集**，由 `completion_all_cws` 的空集分支写 `cw_status[0]` 兜住（§2.2、§25.4；changelog `fc_cw_bitmap_param_v28.md`）

#### v33 — 2026-10-10

**CRC 引擎改走 STM32H743 HAL 驱动**（changelog `fc_crc_hal_driver_v33.md`）。原 `fc_crc_engine.c` 裸写外设寄存器，`FC_CRC_BASE` 写成了 **ADC3** 的 `0x58026000`（真值 `0x58024C00`）⇒ IDR 自检不过 ⇒ `g_crc_ready = 0`（「失败静默继续」）⇒ `fc_crc_compute()` 恒返 0 ⇒ **CRC 校验自始恒通过**。改为新增平台层 `src/platform/stm32h743/crc.{h,c}` 唯一持有 `CRC_HandleTypeDef`（地址由 CMSIS 宏保证）、管时钟、做 IDR 写读回环自检（失败转 `ASSERT`，删除 `g_crc_ready` 静默降级路径）；`src/fc/fc_crc_engine.c` 只留「对哪 512B 算、结果怎么判」，`src/fc/` 保持零 HAL 依赖。**介质格式变更**：此前写入的 CW 里 CRC 字段恒为 0，改后是真值。⚠ 两坑：`stm32h7xx_hal_crc_ex.c` 必须同编（`HAL_CRC_Init` 无条件调 `HAL_CRCEx_Polynomial_Set`）、`ASSERT(HAL_CRC_Init(...))` 在 release 下会把初始化整个裁掉（须先求值再断言）

#### v47 — 2026-10-11

**`fc_cmd_s.pba` → `.paa`——PBA 退出 FC 层**。SQ 里的 PBA 会经 FC 接口一路带到 FC-NAND 层，而 FC 层在 10+ 处 `pba_to_paa()` 现场转换——PBA 与 PAA **位序不同**（`pba_to_paa` 是真正的字段重排），每处转换都是一次静默算错地址的机会。改为：**FTL 在自己的空间组合 PBA，在边界处 `pba_to_paa()` 一次，赋给 `cmd->paa`**；`src/fc/**` 里 `pba_to_paa()` / `pba_to_*()` 归零。建链入口 `fc_command_configure_descriptor()` 一并改收 `DWORD row_paa`（FC 层最后一个收 PBA 的地方）；`desc->paa` 由 `paa_with_cw(paa_with_plane(row_paa, plane), cw)` 造出（PAA 的 plane 与 cw **不相邻**，v24 的 plane_cw 掩码技巧只在 LBA/PBA 空间有效），`desc->meta` 仍是 LBA 公式、不变。FTL 侧 `ftl_paa_gblock(cmd->paa)` 与旧 `ftl_pba_gblock(cmd->pba)` **逐位等价**（`bst.h` 里前者本就是后者的展开）。**PAA 无 ch/ce ⇒ 路由一律走 `config.ch/ce/lun`**（生产路径与测试本就如此，本版把它写死为结构不变式）。RAID 段（`#if CONFIG_FC_RAID`，恒关）一并机械改 PAA、**无法编译验证**。用例数不变（仍 53）；`make clean && make tests` 0 warning 0 error、链接 225212 B；固件 166708 B（§2.3.1、§6.1；changelog `fc_command_paa_v47.md`）

#### v52 — 2026-10-11

**链锁——链未结束期间该 LUN 的填充权归链所在提交队列独占**。补上一处真实缺口：链长于 per-LUN FIFO 深（`FC_SCHEDULER_CHAIN_MAX = 4`）时被 `fc_poll_chain_length` 截成多批、分多次填充 pass 下发，而 `fc_poll_fill` 的 `suspended[]` 每趟 `memset` 清零 ⇒ **批与批之间无跨 pass 状态**，另一条提交队列（HSQ/SQ）的命令可插进链的已下发部分与未下发部分之间。`fc_sched_s` 新增跨 pass 持久的 `uint8_t chain_owner[CH_NUM][CE_NUM][LUN_NUM]`（取值 `fc_chain_owner_t` NONE/SQ/HSQ），`fc_poll_fill_queue` 加 `owner` 形参、三处规则：**准入**（`chain_owner != NONE && != 本队列` → 回推跳过，**不置 suspended**，否则把链自己的续批一并锁死）/ **更新**（下发成功后看**批尾** `chain.ids[need-1].config.chain`——`MORE` ⇒ 锁继续转本队列、`END` ⇒ 解锁）/ **失败清锁**（整链注 `NAND_FAIL` 时清，免留残锁）。锁的粒度是「**哪条提交队列能喂这个 LUN**」而非「LUN 不能用」：链所在队列照常能喂（续批正从这条队列取），另一条队列一律回推，直到链 END 下发。与既有「共享 `suspended` 防优先级倒挂」**正交**。**对生产路径零影响**：FTL 全仓恒 `FC_CHAIN_END` ⇒ `chain_owner` 恒 NONE ⇒ 准入判断恒假、更新恒写 NONE ⇒ 逐字不变（§2.7、`docs/archive/fc_design_discussions.md` §16.9、§23.7、§23.9；changelog `fc_chain_priority_lock_v52.md`）

#### v53 — 2026-10-11

**命令级 buffer 归属位域 `fc_cmd_cfg_s.buf_own : 2`——谁申请 / 谁释放**。用户要一个 2-bit 旋钮表达「这条命令的数据 buffer 由谁申请、由谁归还」，把上层（FTL）频繁的 buffer 申请/释放动作按需**下沉到 FC 层**。编码：**bit1 = 申请方**（0 = FC 申请，1 = 上层申请）、**bit0 = 释放方**（0 = 上层释放，1 = FC 释放），四档 `00` FC申请/上层释放 / `01` FC申请/FC释放 / `10` 上层申请/上层释放 / `11` 上层申请/FC释放。**编码刻意让 `00` = 今天的生产行为**（`fc_cmd_cfg_s` 是 32 bit，取原 `reserved : 3` 中的 2 bit 放新字段，命令出池 `(fc_cmd_s){0}` 清零即得默认 `00`）⇒ **`src/ftl/**` 一行不动**。**机制**：建链机制体改名 `fc_command_configure_descriptor_with_buffer(command, plane, cw, row_paa, buffer_id)`（5 参，上层申请时由调用者 `fc_buffer_alloc()` 后传入），原 4 参入口退化为 `_with_buffer(..., FC_BUFFER_ID_INVALID)` 薄包装（FTL 两个调用点 / RAID parity / 既有单测零改动）；`fc_complete_from_nand` 在 Step 3 组装完 CQ 条目之后、`fc_complete_internal` 之前加 **Step 4**——`buf_own` 释放位 = FC 释放（`01`/`11`）时沿 desc 链归还全部 buffer 并把各 desc 的 `buffer_id` 抹成 `FC_BUFFER_ID_INVALID`（**幂等**：随后的 `fc_command_release` 自然跳过，不双重归还；命令没走到完成就释放则 buffer 仍有效、链释放照常归还 ⇒ 不泄漏）。**释放原语一行不改**（读法 A）：「上层释放」= 上层调 `fc_command_release`，它恒归还链上所有未归还的 buffer，不管谁申请的——`10` 与 `00` 的差别**只在谁调 `fc_buffer_alloc()`**。RAID parity（`#if CONFIG_FC_RAID`，恒关、无法编译验证）补一行 `parity_cmd->config.buf_own = FC_BUF_OWN_FC_ALLOC_FC_FREE`（parity 是 FC 内部命令，不继承 data_cmd 的归属）。用例数 **56 → 57**（Level 3 新增 `buf_own_release`，四档账目闭合 + 空闲链完整性守卫）；`make clean && make tests` 0 warning 0 error、`.bin` **238980 B**（§12.10、§13.3.1；changelog `fc_buf_own_v53.md`）

> **前提已被推翻**：v52 条目里「FTL 全仓恒 `FC_CHAIN_END` ⇒ `chain_owner` 恒 NONE」只对当时成立。
> 2026-10-30 起 FTL cache read / cache program 会组链，`chain_owner` 跨轮次锁成为该路径的正确性前提；
> 其余命令仍恒 `END`。见 §4.8。

### 3.2 未逐条展开的版本

| 版本 | 主题 | 变更记录 |
|------|------|---------|
| v6 | CW 子集读（`cw_mask` 初版）与完成项重构 | `fc_cw_subset_read.md`、`fc_flat_cw_mask_cq_param.md` |
| v7 | flat 绝对编号掩码 + CQ 重做为「命令级聚合 + 返回段」 | `fc_flat_cw_mask_cq_param.md` |
| v14 | 建链接口拆分中间版本（无独立记录） | — |
| v15 | 删除独立所有权链 | `fc_chain_ownership_v16.md`、§4.2 |
| v16 | 消除「已出池未挂链」中间态 | `fc_chain_ownership_v16.md`、§4.2 |
| v18~v22 | 断言分层落地（外壳/内部/C 层/缺失守卫 + UT 适配） | `fc_configure_guard_assert_v18.md`、`fc_shell_guard_assert_v19.md`、`fc_internal_guard_assert_v20.md`、`fc_c_tier_missing_guard_v21.md`、`fc_ut_level2_rework_v22.md` |
| v25 | 地址构造统一走 `nand_addr_s` | `nand_config_addr_ctor_v25.md` |
| v26 | 建链接口的过渡版本（无独立记录） | — |
| v29 | 数据路径单 CW 步骤原语 | `fc_data_path_step_primitives_v29.md`；当前设计见设计文档 §5 |
| v30~v32 | UT Level 2/3 参数化与迁移 | `fc_ut_level2_param_v30.md`、`fc_ut_level3_migration_v31.md`、`fc_ut_level3_param_v32.md` |
| v34~v46 | UT 覆盖扩展（META/建链位图/证据打印/地址随机/块锁/读 META/OFNI 签名） | `fc_ut_meta_pba_v34.md` … `fc_ut_read_meta_v46.md` |
| v48~v51 | CACHE_RANDOM 语义、块锁、压力用例、流式提交 | `fc_ut_cache_random_v48.md`、`fc_ut_block_lock_v49.md`、`fc_ut_stress_v50.md`、`fc_ut_stress_v51.md` |
| v54~v60 | UT 与器件扩展（UNIQUE_ID / ONFI 参数页 / 签名读） | `fc_ut_read_unique_id_buffer_meta_v54.md`、`fc_ut_read_onfi_signature_v59.md`、`nand_onfi_param_page_print_v60.md` |

### 3.3 协议查证 — 2026-10-09

**multi-plane 不在 ONFI 1.0 里**。逐字读 ONFI 1.0（Rev 1.0, 2006-12-28）Table 14 命令集：cache 只有单 plane 的 `Read Cache` 31h / `Read Cache End` 3Fh / `Read Cache Enhanced` 00h-31h / `Page Cache Program` 80h-15h，**全文 `plane` 一词出现 0 次**（1.0 唯一的「多设备」是 multiple LUN，走 `Interleaved` D1h / 11h）。multi-plane 全部来自器件侧。顺带逐字核对了 Micron「4Gb, 8Gb, and 16Gb x8 NAND Flash Memory」（MT29FxG08xAA, Rev B）的 Table 7 Two-Plane Command Set / §Two-Plane Addressing，并发现两处待办：① 「two-plane 命令族要求 on-die ECC 关闭（Note 4）」的出处对不上（该 PDF 全文无 `on-die`）；② 器件有 `TWO-PLANE PROGRAM PAGE CACHE MODE 80h-11h-80h-15h`，本项目 op 表缺此 op（§25.6 前置约束 3、§25.8 风险 2 与风险 8）

### 3.4 器件扩展 — 2026-10-23

**two-plane cache 读写落地 NAND/FC 层**——新增
`OP_TWO_PLANE_PROGRAM_CACHE`(44)、`OP_READ_PAGE_CACHE_START_SEQ_MP`(45)、
`OP_READ_PAGE_CACHE_SEQ_MP`(46)、`OP_READ_PAGE_CACHE_LAST_MP`(47)；
单 plane `OP_READ_PAGE_CACHE_START`(34) 改为 `START_SEQ`（含首条 `31h`，输出第一页）。
`OP_COUNT` 44→48；新增 `block_cache_multi_plane` UT。

**2026-10-23 上板**：`test /fc-nand/` 56/56（3 skipped），
`block_cache_ops` / `block_cache_multi_plane` 均 OK。
⚠ 本次只到 NAND/FC + UT，FTL batch 与掉电恢复未接；失败注入未验。
（后续又补 op 50，当前 `OP_COUNT = 51`。）

---

## 4. 专题深挖

### 4.1 Buffer 方案讨论（v13 前）

> **⚠ 历史章节（v13 前的接口）**：本章代码示例里的 `fc_command_configure_descriptors()`（**复数**）与
> `fc_write_cmd_data()` 已随 v13 策略层上移改由 FTL 侧 `ftl_cmd_configure_descriptors()` /
> `ftl_cmd_write_data()` 承担。方案比较的**结论**（buffer_id 方案胜出）不受影响，只是调用方换了层。

#### 12.1 问题背景

FC 模块需要决定数据 buffer（每个 CW 的数据载体）在接口层的表达方式。核心问题可以归结为一个选择：

```
如何把"某个 CW 的数据"和"FC 能访问到的内存位置"关联起来？

选项 A：buffer_id（资源 ID 引用）
选项 B：data_ptr（裸指针/地址）
```

这个选择不仅影响 FC 内部实现，更影响 FTl 与 FC 之间的协作模式、数据路径效率、以及整个系统的安全性。

#### 12.2 各存储领域的行业实践

##### 12.2.1 企业级 NVMe SSD（有大量 DDR）

代表：Intel/Samsung/Kioxia 企业级 SSD 控制器。

| 特征 | 做法 |
|------|------|
| DDR 容量 | 几 GB~几十 GB |
| 数据 buffer 管理 | **物理地址（data_ptr）** |
| 控制结构管理 | **ID**（Command Tag、CID） |
| 数据描述方式 | PRP（Physical Region Page）或 SGL（Scatter-Gather List） |
| 传输方式 | DMA 引擎直接从 DDR 搬运，零 CPU 拷贝 |

**理由**：DMA 引擎只能消费物理地址，多一层 buffer_id → 地址的查表就是多一个硬件逻辑门延迟。数据路径上，地址直达是最优选择。

##### 12.2.2 消费级 NVMe/SATA SSD（有外置 DRAM 或 HMB）

代表：Phison/Silicon Motion 主控。

| 特征 | 做法 |
|------|------|
| DRAM 容量 | 0（DRAM-less）~512MB |
| 数据 buffer | **物理地址（有 DRAM）/ 内部 ID（DRAM-less）** |
| 控制结构 | **ID** |

有 DRAM 时：与企业的做法一致，地址直达。
DRAM-less 时：**内部 SRAM Pool + ID 方案**（见 12.2.3）。

##### 12.2.3 DRAM-less SSD（与你的项目最接近）

代表：DRAM-less NVMe 主控（如 Phison PS5013、SM2258 的 DRAM-less 变体），部分入门级 SATA SSD。

```
Host DDR ←→ PCIe/SATA 控制器
                    │
              内部 SRAM (256KB~1MB)
                ├── FTL 映射表（压缩 / 从 NAND 加载）
                ├── 数据 buffer Pool ←── 基于 ID 管理
                └── 控制结构（命令、队列）
                    │
               NAND 控制器 → NAND Flash
```

**数据路径（写入）：**

```
Host 数据（在 Host DDR 中）
  │  PRP/SGL 描述地址
  ▼
NVMe 控制器 DMA ──► 内部 SRAM Buffer Pool ──► NAND
                          ↑
                    基于 ID 管理！
                    控制器内部管理 ~128 个 SRAM buffer
                    每个 buffer 有内部 ID，对 Host 透明
```

**数据路径（读取）：**

```
NAND ──► 内部 SRAM Buffer Pool ──► DMA ──► Host DDR
                    ↑
              也是基于 ID
```

**关键结论**：DRAM-less SSD 在控制器内部清一色用 ID/Pool 方案。内部 SRAM 是稀缺资源，必须精细管理。DMA 引擎最终需要物理地址，但地址是由控制器内部从 buffer_id 转换得来——这个过程对外部 Host 完全透明。

| 层次对比 | DRAM-less SSD | 你的项目 |
|----------|--------------|---------|
| 数据源 | Host DDR（PRP/SGL 描述） | FTL 的 SRAM/SDRAM 数据区 |
| 中转 buffer | 控制器内部 SRAM Pool | FC Buffer Pool（SRAM） |
| 从源到 buffer 的方式 | 控制器 DMA 引擎自动搬运 | `fc_write_cmd_data()` 内部 memcpy（或 DMA）|
| 地址描述 | PRP/SGL 中的物理地址 | buffer_id（FC 内部转换） |
| ID 管理 | 控制器内部，对 Host 不可见 | FC 接口暴露 buffer_id |

##### 12.2.4 UFS（Universal Flash Storage）

代表：三星、铠侠、SK 海力士 UFS 主控。

```
UFS Controller SoC：
  ┌────────────────────────────────────────┐
  │  M-PHY → UTP（UPIU 解析器）             │
  │              │                          │
  │              ├── PRDT（物理地址列表）     │
  │              │    (Host ↔ Controller 之间) │
  │              │                          │
  │          FTL / Flash Management         │
  │              │                          │
  │              ├── 内部 SRAM（几十~几百 KB）│
  │              │    ├── 映射表 Cache       │
  │              │    └── 数据 Buffer Pool   │
  │              │                          │
  │              ▼                          │
  │          NAND Flash Controller          │
  └────────────────────────────────────────┘
```

**两层地址管理：**

| 层次 | 做法 | 原因 |
|------|------|------|
| 对外（Host ↔ Controller） | PRDT 物理地址列表 | UFS 协议规定 |
| 对内（FTL ↔ NAND） | **内部 ID/Pool 方案** | 无外置 DRAM，SRAM 由 Firmware 精细管理 |

UFS 的主流方案多年以来就是内部 ID/Pool。但近年趋势是：随着 UFS 控制器 SRAM 增大（从几十 KB → 512KB~1MB），新架构逐渐倾向 **"Shared SRAM + Pointer"** 的统一地址访问方式。

##### 12.2.5 行业规律总结

```
有 DRAM/SDRAM（≥几 MB） ──→ 数据用 data_ptr（零拷贝），控制用 ID
无 DRAM（仅内部 SRAM）   ──→ 数据和控制统一用 ID/Pool
SRAM 达到 512KB~1MB     ──→ 行业处于从 ID 向 Pointer 过渡的阶段
```

#### 12.3 你的硬件参数

| 资源 | 大小 |
|------|------|
| 内部 SRAM | **1MB** |
| 板载 SDRAM | **8MB** |
| FC 控制结构总需求 | ~75 KB（占 SRAM 的 7%）|
| FC 数据 Buffer Pool（当前方案） | ~67.5 KB（128×528B）|

FC 控制结构仅占用 SRAM 的 7%，剩余 93% 可供 FTL 使用。1MB SRAM 的容量已经达到 **UFS 旗舰主控档次**。

#### 12.4 方案一：buffer_id（当前方案）

##### 12.4.1 核心思想

FC 内部维护静态 Buffer Pool，所有数据 buffer 通过 16-bit ID 对外暴露。FTL 在使用数据时通过 ID → pointer 转换访问实际内存。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_cfg_s    config;
    uint16_t         buffer_id : 16;  // ← 指向 FC 内部 Buffer Pool
    uint16_t         next_id   : 16;
    uint8_t          status    : 8;
} fc_desc_s;
```

**相关接口（4 个）：**

| 接口 | 说明 |
|------|------|
| `fc_buffer_alloc()` | 从 FC Buffer Pool 分配 |
| `fc_buffer_release(id)` | 释放回 Pool |
| `fc_buffer_id_to_real_buffer(id)` | ID → 指针 |

> **注**：`fc_real_buffer_to_buffer_id(ptr)`（指针 → ID）已随对外接口拆分移除——FC 数据引用统一走 buffer_id，无调用者（详见 §2.9 移除说明）。

##### 12.4.2 FTL 使用流程

```c
// ① 分配 command + desc 链 + buffer
fc_cmd_s *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.cw = 1;   // 2 个 CW
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);   // 自动分配 desc 链 + buffer

// ② FTL 将数据写入 buffer（需遍历 desc 链）
fc_desc_id_t desc_id = cmd->desc_id;
uint8_t cw = 0;
while (desc_id != FC_DESCRIPTOR_ID_INVALID && cw <= cmd->config.cw) {
    fc_desc_s *desc   = fc_descriptor_id_to_descriptor(desc_id);
    uint8_t         *buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
    memcpy(buffer, my_data + cw * CW_SIZE, CW_SIZE);
    desc_id = desc->next_id;
    cw++;
}

// ③ 提交
fc_submit(cmd);
```

##### 12.4.3 优点

- **安全性高**：FC 能验证 buffer_id 范围（0~127），天然防止野指针
- **生命周期明确**：FC 通过 command state 和 descriptor status 知道哪些 buffer 在使用中
- **资源可视化**：`fc_get_resource_stats()` 可直接输出 buffer 使用率
- **改动最小**：已有代码无需重构

##### 12.4.4 缺点

- **数据路径多一层转换**：每次读写都要 `buffer_id → real_buffer` 查表
- **多一次 memcpy**：FTL 的数据必须经过 memcpy 进入 FC Buffer Pool
- **不灵活**：FTL 无法控制 buffer 放在哪里（SRAM 还是 SDRAM）
- **接口膨胀**：4 个 buffer 管理接口增加了学习成本

#### 12.5 方案二：data_ptr（裸指针/地址方案）

##### 12.5.1 核心思想

FC 不再管理数据 buffer，Descriptor 中直接用指针指向数据位置。数据由 FTL 自主分配和管理。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_cfg_s    config;
    uint8_t         *data_ptr;    // ← 直接指向 SRAM/SDRAM 地址
    uint16_t         next_id : 16;
    uint8_t          status  : 8;
} fc_desc_s;
```

**删除的接口（4 个）：**
- `fc_buffer_alloc()`
- `fc_buffer_release()`
- `fc_buffer_id_to_real_buffer()`
- `fc_real_buffer_to_buffer_id()`（已随拆分移除，见 §2.9）

**接口总数从 14 个减少到 10 个。**

##### 12.5.2 FTL 使用流程

```c
// ① FTL 自己管理数据 buffer
uint8_t *buf = ftl_buf_alloc();           // FTL 自己的分配器
memcpy(buf, my_data, CW_SIZE);

// ② 分配 command + desc 链（不分配 buffer）
fc_cmd_s *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.cw = 1;
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);    // 只分配 desc，不分配 buffer

// ③ FTL 直接指定地址
fc_desc_s *desc = fc_descriptor_id_to_descriptor(cmd->desc_id);
desc->data_ptr = buf;                     // ← 直接给地址

// ④ 提交
fc_submit(cmd);

// ⑤ 完成后 FTL 释放
fc_poll_completion(&completion);
fc_command_release(cmd);
ftl_buf_free(buf);                        // FTL 自己释放
```

##### 12.5.3 优点

- **零拷贝**：FTL 直接写自己的 buffer，没有中间 memcpy
- **接口精简**：减少 4 个 buffer 接口
- **灵活性高**：FTL 可自由选择 buffer 位置（SRAM 或 SDRAM）
- **架构对齐行业**：与有 DRAM 的企业/消费级做法一致

##### 12.5.4 缺点

- **所有权/生命周期管理复杂**（见 12.5.5）
- **无防错机制**：FC 无法验证 data_ptr 的有效性（只能检查 NULL）
- **FTL 需要额外建立 buffer 管理代码**（~30 行 freelist）

##### 12.5.5 data_ptr 的生命周期问题

这是 data_ptr 方案最核心的难点。本质问题：

> **指针是一个裸地址，没有任何归属标记。同一个地址可以被多个模块同时引用，没有任何机制可以判断"谁在用"、"是否可用"**

**典型 UAF（Use-After-Free）场景：**

```
① FTL 分配 buf_A → 填入数据
② desc->data_ptr = buf_A → fc_submit(cmd)
③ FTL 判断 buf_A 不再需要 → ftl_buf_free(buf_A)
④ FC 还没发出 buf_A 的数据到 NAND ××× 数据已被其他人占用 ×××
```

**解决方案对比：**

| 方案 | 机制 | 安全等级 | 开销 | 实现复杂度 |
|------|------|---------|------|-----------|
| **合约约束** | submit 到 release 之间 FTL 不释放 | 低（靠自觉） | 零 | 零 |
| **FC 借用表** | FC 内部小表追踪借用中的地址 | 高 | ~1KB SRAM | 中 |
| **FTL 引用计数** | FTL 侧 ref_count 管理 | 中 | 小 | 低 |
| **Owner Tag** | 地址附带归属标记 | 高 | 小 | 中 |
| **调试断言** | 释放前检查 FC 是否还在使用 | 开发期高 | 调试时 | 低 |

**推荐组合：合约约束 + 调试断言。**

```c
// FTL 侧的契约（注释即文档）
// 约束：从 fc_submit() 到 fc_command_release() 之间，
//       不允许释放 desc->data_ptr 指向的 buffer。
// 违反此契约会导致数据错乱。

// FTL 释放时的调试断言
#ifdef DEBUG_FTL
void ftl_buf_free(uint8_t *ptr) {
    // 检查 FC 是否还有 pending 命令引用此地址
    DEBUG_ASSERT(!fc_is_addr_pending(ptr));
    // ... 实际释放
}
#endif
```

#### 12.6 方案三：双模设计（推荐过渡方案）

##### 12.6.1 核心思想

Descriptor 同时支持 buffer_id 和 data_ptr，通过标志位选择使用哪种模式。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_cfg_s    config;

    union {
        uint8_t     *data_ptr;    // FTL 直接指定地址（零拷贝）
        uint16_t     buffer_id;   // FC 内部 Pool（传统路径）
    };

    uint16_t         next_id : 16;
    uint8_t          status  : 8;
    uint8_t          own_buf : 1;  // 0=FC Pool, 1=FTL 自己提供
} fc_desc_s;
```

##### 12.6.2 适用场景

| 场景 | 用哪种 | 原因 |
|------|--------|------|
| 常规 Page 写/读 | data_ptr | FTL 直接在 SDRAM/SRAM 准备数据，零拷贝 |
| 小操作（SET_FEATURES 等） | parameter 字段 | ≤ 4 字节，不需要 desc |
| 临时数据/测试代码 | buffer_id | 兼容旧路径，无需 FTL 管理 buffer |
| 内部 GC/校验操作 | data_ptr | FTL 自己管理缓冲，灵活分配 |

##### 12.6.3 优点

- **平滑迁移**：旧代码可以不修改继续使用 buffer_id
- **灵活选路**：FTL 根据场景选择最佳方式
- **兼容所有硬件变体**：即使不接 SDRAM，也能用内部 Pool

##### 12.6.4 缺点

- **两套路径维护**：FC 内部需要同时支持两种数据获取方式
- **接口复杂度**：双模增加了理解成本
- **可能永远不会用另一条路径**：如果确定了方向，双模就是死代码

#### 12.7 方案四：buffer_id + 便捷数据接口（推荐方案）

##### 12.7.1 核心思想

保持 buffer_id 作为唯一的数据引用方式，保留 FC 的 Buffer Pool 所有权管理。但**新增便捷接口**让 FTL 不必手动遍历 desc 链写数据，解决易用性问题。

```c
// 新增接口：一次写入命令关联的所有 CW 数据
fc_status_t fc_write_cmd_data(fc_cmd_s *cmd,
                               const uint8_t *data,
                               uint16_t length);
// 内部校验 length == (cmd->config.cw + 1) * CW_SIZE
// 自动遍历 desc 链：buffer_id → pointer → memcpy

// 新增接口：获取指定 CW 的数据指针（读操作使用）
uint8_t *fc_get_cmd_data_ptr(fc_cmd_s *cmd, uint8_t cw_index);
// 遍历 desc 链找到第 cw_index 个 desc
// 返回 buffer_id → real_buffer
```

**接口总数从 14 个变为 16 个。**

##### 12.7.2 FTL 使用流程

**写操作：**

```c
fc_cmd_s *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.cw = 3;                  // 4 个 CW
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);  // 自动分配 desc 链 + buffer

// 一行完成数据填充，FC 内部处理 buffer_id → pointer → memcpy
fc_write_cmd_data(cmd, my_source_data, 4 * CW_SIZE);

fc_submit(cmd);
```

**读操作：**

```c
fc_cmd_s *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_READ;
cmd->config.cw = 3;
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);

fc_submit(cmd);
// ... fc_service() 循环 ...

fc_poll_completion(&completion);
fc_cmd_s *done = fc_command_id_to_command(completion.command_id);

// 一行获取数据指针，FTL 直接读取
uint8_t *data = fc_get_cmd_data_ptr(done, 0);   // 第 0 个 CW 的数据

fc_command_release(done);
```

**非常规场景**（如只写某个 CW 的数据）：FTL 仍可回退到手动遍历 desc 链的方式。

##### 12.7.3 与 DRAM-less SSD 的对应关系

| DRAM-less SSD | 你的项目（本方案） |
|--------------|------------------|
| Host 数据 → PRP/SGL 描述地址 | FTL 有数据 → 调用 `fc_write_cmd_data()` |
| 控制器 DMA 自动将数据拉入内部 SRAM Pool | FC 内部 `buffer_id → pointer → memcpy` |
| 控制器内部 Pool ID（对 Host 透明） | buffer_id（对 FTL 可见） |
| 完成后 DMA 自动将数据写出给 Host | FTL 调用 `fc_get_cmd_data_ptr()` 读数据 |

逻辑完全一致，只是你的项目中 memcpy 替代了 DMA 的角色。未来可升级为 DMA。

##### 12.7.4 优点

- **保持所有权管理**：FC 仍然拥有完整的 buffer 生命周期控制，避免 UAF
- **易用性好**：FTL 无需手动遍历 desc 链，两行完成读写
- **安全性高**：FC 可验证 buffer_id 范围，拦截非法访问
- **改动小**：保留现有实现，只新增两个接口
- **可进化**：未来 memcpy 可替换为 DMA，接口不变

##### 12.7.5 缺点

- **多一次 memcpy**：相比 data_ptr 的零拷贝，有 CPU 开销
- **接口总数增加**：14 → 16 个
- **SRAM 占用**：需要预留 ~67.5KB 的 Buffer Pool

#### 12.8 方案汇总对比

| 维度 | ① buffer_id（现状） | ② data_ptr | ③ 双模 | ④ buffer_id + helper（**推荐**） |
|------|-------------------|------------|--------|-------------------------------|
| **接口数** | 14 个 | 10 个 | 14 个 | 16 个 |
| **数据路径** | memcpy | 零拷贝 | 两者 | memcpy（可升级 DMA） |
| **所有权管理** | FC 完全管理 | FTL 管理 | 混合 | FC 完全管理 |
| **安全性** | 高（范围检查） | 低（裸指针） | 中（取决于模式） | 高（范围检查） |
| **FTL 代码量** | 中（需遍历 desc） | 高（需自建 buffer） | 中 | **低**（一行接口） |
| **灵活性** | 低 | 高 | **最高** | 中 |
| **SRAM 占用** | ~67.5KB Pool | FTL 自定 | 两者共存 | ~67.5KB Pool |
| **行业对齐** | DRAM-less | 有 DRAM | 过渡架构 | **DRAM-less** |
| **实现难度** | ✅ 已完成 | 大改 | 大改 | **小改** |

#### 12.9 推荐方案

**选择方案④：buffer_id + 便捷数据接口。**

理由：

1. **所有权清晰** — FC 全权管理 buffer 的生命周期，FTL 无需关心地址泄露或 UAF
2. **易用性好** — 新增 `fc_write_cmd_data()` 和 `fc_get_cmd_data_ptr()` 后，FTL 一行代码完成数据读写
3. **对齐行业** — DRAM-less SSD 的做法与当前设计高度一致，方向可靠
4. **可进化** — 内部 memcpy 未来可以无损替换为 DMA，接口不变
5. **适合 1MB SRAM** — 有充足空间维持 Buffer Pool，不必为节省 ~67KB 引入复杂度

**后续改进方向：**

- **短期**：实现 `fc_write_cmd_data()` 和 `fc_get_cmd_data_ptr()`，简化 FTL 使用
- **中期**：将 memcpy 替换为 DMA 传输（DMA: FTL buffer → FC Buffer Pool）
- **长期**（如需零拷贝）：在双模或纯 data_ptr 方案上扩展，但保持接口层不变

### 4.2 所有权链与中间态（v15 → v16）

> **v15：「一条命令拥有哪些 desc」只有一份真值**——命令的 desc 链本身
> （`command->desc_id` + `descriptor_pool[].next_id`）。此前另有一条「所有权链」
> （`descriptor_owned_next[256]` + `command_descriptor_head[64]`，共 640B），
> 专门为「desc 已出池但尚未挂进数据链」的中间态兜底，v15 整条删除。
>
> **v16：把那个中间态本身消除**——`fc_command_configure_descriptor_internal()` 改为
> **分配 desc → 填字段 → 挂链 → 分配 buffer**（buffer 池 128 < desc 池 256，
> 「desc 成功而 buffer 失败」是最现实的路径）。不变量因此变成
> **desc 出池即挂链**：链恒是「所有已分配 desc」的完整记录，任何一步失败后
> 沿链释放即完备，**不需要任何回滚逻辑**（v15 曾用「就地回滚半成品」打补丁，
> v16 改为消除中间态）。代价：链上允许存在 `buffer_id == FC_BUFFER_ID_INVALID`
> 的 desc，走链的读者都要容忍——释放侧已按此实现。
> 详见 `docs/archive/changelog/fc_chain_ownership_v16.md`。
>
> **v17：这套机制体即 `fc_command_configure_descriptor`**（v16 时在 `fc.c`，v17 随
> 三层合并搬到 `src/fc/fc_interface.c`，函数名去 `_internal` 后缀）。机制顺序不变，
> 唯一新增约束是**校验段整体前置于出池**——非法实参在分配任何 desc 之前就返回，
> 故「污染池子」的可能被进一步排除。详见 `docs/archive/changelog/fc_build_chain_convergence_v17.md`。

### 4.3 建链收敛（v11 → v28）


> **v13 起本节主体（掩码展开）已在 FTL 侧**：`ftl_cmd_configure_descriptors()`
> （`src/ftl/ftl_fc_interface.c`）。FC 内只剩机制原语，**v17 起为 `fc_command_configure_descriptor()`**
> （`src/fc/fc_interface.c`；v13~v16 为 `fc_command_attach_cw()`）。
> 下面的演进记录保留，便于理解为什么规则是这个形状（其中 v11/v12/v13 小节里的函数名是**当时的名字**）。

**位图校验规则**（`ftl_cmd_resolve_cw_bitmap`，v27 前名 `ftl_cmd_resolve_cw_mask`）：

```c
/* 命令能触及的 plane 位段 [plane_first, plane_last)：
 *   multi-plane：从 plane 0 起覆盖 plane_count 个；单 plane：只覆盖 paa_to_plane(command->paa) 那一个 */
plane_first = (plane_count == 1U) ? paa_to_plane(command->paa) : 0U;
plane_last  = plane_first + plane_count;
allowed     = ((1U << (plane_count * CW_NUM)) - 1U) << (plane_first * CW_NUM);

bitmap = cw_bitmap;                      /* v28：形参，不再是 command->param.cw_bitmap */
ASSERT(bitmap != 0U);                    /* 字面位图：0 = 空集 = 调用者忘了给（v27 起无隐式展开） */
ASSERT((bitmap & ~allowed) == 0U);       /* 越出本命令 plane 位段 = 编程错误
                                          * （v13 前返回 FC_STATUS_INVALID_ARGUMENT，v13~v22 返回 FTL_ERROR，
                                          *   v23 起 ASSERT） */
return bitmap;
```

> ⚠ 第一条 ASSERT 是「生产者忘了填」的**唯一**拦网，而 ASSERT 在 release 下会被裁掉
> ⇒ 忘填的代价是「编译期无声、release 下静默空操作」。这就是 v27 取消 `0 = 全部` 重载的已知代价。

**`config.cw` 不参与建链**（v23 起；v7~v22 是"`mask==0` 时的输入提示"）：链长、完成项折叠、
`fc_build_dispatch_request` 的落位位图一律走 desc 链（v9 前是 `cw_count`，同一个道理）。
`config.cw` 本身**不回写**——保持调用者原值，不伪造一个误导性的数；当前唯一的生产读者是 FC 内 RAID
stripe 的 `cw_count`（`fc.c` 的 `fc_stripe_submit` / parity 建链，默认不编译）。
`fc_desc_s.config.cw` 语义不变 = 本 desc 在 plane 内是第几个 CW。

链序为 **plane-major**（先 plane 0 的全部置位 CW，再 plane 1）——这只是 desc 的**建立顺序**，
v9 起不再被任何消费点当作坐标使用（落位按 `n` 独立进行，链序无关紧要）。`fc_cw_mask_popcount`
手写循环到 `CW_TOTAL_MAX`，不引 `__builtin_popcount`（避免 libgcc `__popcountsi2` 依赖）；
**该函数在 v13.1 随其唯一调用点（同源查重 ASSERT）一并删除**（见 §25.4 v13.1）。

`fc_command_configure_descriptor_internal` 的签名从 `index` 改为显式的 `(plane, cw)`——CW 号由调用方按 bitmap 枚举给出，不再由 desc 序号推导。（v17 后该函数即 `fc_command_configure_descriptor`，见下 §25.4 v17。）

##### v11：机制 / 策略拆分

上面的掩码展开是**策略**（"访问哪些 CW"），把 desc 分配 + buffer 分配 + 挂链那半截是**机制**。
v11 把机制抽成独立原语，公开给上层：

```c
/* 机制层：分配一个 desc + buffer、填 PAA/config/meta、挂到命令 desc 链尾。 */
fc_status_t fc_command_attach_cw_internal(fc_ctl_s *c, fc_cmd_s *cmd, uint8_t plane, uint8_t cw, DWORD row_paa); /* fc.c */
fc_status_t fc_command_attach_cw(fc_cmd_s *cmd, uint8_t plane, uint8_t cw, DWORD row_paa);                  /* 公共入口 */
```

拆开后，上层可以**自己算集合**（例如同页内只读 `{plane0:CW0, plane0:CW3, plane1:CW1}`）：

```c
fc_cmd_s *cmd = fc_command_alloc();
/* 填 config；各 plane 传自己的行地址（此处同页，故只有 plane 位不同） */
DWORD row0 = paa_from_addr(&(nand_addr_s){.lun = lun, .block = block, .plane = 0, .page = page});
DWORD row1 = paa_from_addr(&(nand_addr_s){.lun = lun, .block = block, .plane = 1, .page = page});
fc_command_attach_cw(cmd, 0, 0, row0);
fc_command_attach_cw(cmd, 0, 3, row0);
fc_command_attach_cw(cmd, 1, 1, row1);
fc_submit(cmd);
```

四个约束：

| 约束 | 行为 |
|---|---|
| 行地址 | 由 `row_paa` 实参给出（该 plane 的 page/block/lun；CW 位忽略），其 plane 位必须等于 `plane`。同一 plane 的所有 CW 必须传同一 `row_paa`——一个 plane 一次只访问一个物理位置 |
| plane 段 | `plane` 必须落在本命令段内（单 plane = `paa_to_plane(cmd->paa)`；multi-plane = `[0, multi_plane)`），越段返回 `INVALID_ARGUMENT` |
| 坐标范围 | `cw >= CW_NUM` 返回 `INVALID_ARGUMENT` |
| 重复 `n` | 同一 `(plane, cw)` 追加两次 → `ASSERT`（掩码是集合，重复置位会静默丢数据），与 `fc_build_request_data_planes` 的同源断言一致 |

**不回写 `param.cw_bitmap`**：机制层只动 desc 链，位图由 `fc_build_request_data_planes` 从链推导。
**链序即数据布局**：FTL 侧的 `ftl_cmd_read_data` / `ftl_cmd_write_data` 按链序搬运（v13 前是 `fc_read_cmd_data` / `fc_write_cmd_data`），上层因此也拿到了布局控制权。

**空 plane 的行地址（flat 独有）**：多 plane 加载 `00h(P0)-00h(P1)-30h` 是原子操作，两个行地址
都必须带正确的 plane 位（BA6）。故 `fc_build_dispatch_request` 为命令 plane 集合里的**每个**
plane 都填 `plane_paa[plane]`（`paa_to_nand_row()` 丢掉 CW 位，故 PAA 里的 CW 号不影响行地址）。

> **v9 起这条风险消失了一半**：行地址不再寄居在某个"槽位 0"的 CW 项里，而是独立的一维数组
> `plane_paa[PLANE_NUM]`——"空 plane 也得填"从"槽位 0 无条件照拷"这种隐式约定，变成一条直白的
> 按 plane 遍历赋值。但**必须填**这个要求本身不变（不填 → 退化成 block0/page0）。

> **v39 起「空 plane」在多 plane 命令里不再合法。** 本段此前写的是「flat 位图下某 plane 可以是
> 0 个 CW（如只读 plane 1 = `0xF0`）」——**这个组合现在是调用者的编程错误**：NAND 侧的多 plane
> handler **无条件遍历 `PLANE_NUM`** 下行地址，既不看掩码也不看 `plane_count`
> （`nand_dispatch_tables.c` 的 `req_multi_plane_{program,read,erase}`），器件侧多 plane program
> 会把点名过的 plane **全部编程**。未覆盖的 plane 既无数据（`cw_buffers[n]` 恒 `NULL`）也无地址，
> 旧实现只能凭空造一条回退地址 ⇒ **静默给该 plane 编一页全 0xFF**（页缓冲 `memset 0xFF`、只填
> 掩码内的 CW），调用者完全不知道。这条不变式此前**只在测试夹具里守着**
> （`fc_test_common.c` 的 `test_cmd_build`），生产侧无守卫——`ftl_cmd_resolve_cw_bitmap` 只查
> **越段**，部分位图对整段而言是"合法"的。
>
> 现在 `fc_build_request_data_planes` 对该组合**双档拦截**：debug 档 `ASSERT` 停机
> （§24 判据 2 / §十三：配置错误 → 断言，不当返回码）；release 档 `ASSERT` 被裁空，故紧跟着
> 返回 `FC_STATUS_INVALID_ARGUMENT`（新判据见 `assertion_guidelines.md` **§二十四 判据 9**：
> 后果是**静默写坏介质**的配置错误，除 `ASSERT` 外必须再补一道运行期拒绝）——**命令被拒绝，绝不提交**。`fc_build_dispatch_request`
> 返回 `FC_STATUS_INVALID_ARGUMENT`——**命令被拒绝，绝不提交**。`fc_build_dispatch_request`
> 透传该状态，两条派发路径（`fc_poll_dispatch_single` / `fc_poll_dispatch_chain`）收到非 OK 就
> 走 `fc_poll_fail_command` 注一条 `NAND_FAIL` 完成项（FTL 的 pending 照常释放，不会挂死）。
> 这是 `fc_build_dispatch_request` **唯一**的失败出口（此前它标注为"构建不可失败"）。
> 想只访问某个 plane ⇒ 用**单 plane op**（`multi_plane = 0`），其 plane 段
> 就是 `paa_to_plane(cmd->paa)` 那一个，窄位图合法。

##### v12：per-plane 行地址（Policy B）

v11 的机制原语不收行地址，各 plane 的行地址仍由 `fc_build_request_data_planes` 从
`command->paa` **复制 + 替换 plane 位**——隐含"两 plane 同 block"。v12 把它打开：

| 位置 | v11 及以前 | v12 |
|---|---|---|
| `fc_command_attach_cw(_internal)` | 无行地址参数 | 增 `row_paa`（该 plane 的行地址，CW 位忽略；plane 位必须等于 `plane`） |
| `fc_command_configure_descriptor_internal` | 从 `command->paa` 组装 | 直接用实参 `row_paa`（保留其 LUN 字段），只填传入的 `plane`/`cw` |
| `fc_build_request_data_planes` | `plane_paa[plane]` = 复制 `command->paa` + 替换 plane 位 | **从 desc 链推导**：取链上落在该 plane 的第一条 desc 的 `paa`，落成行地址（CW 位丢掉）。链上该 plane 无 desc（如只读 plane1 的 `0xF0`）时保留旧行为（从 `command->paa` 复制），否则多 plane uop 会退化成 block0/page0 |

**新增 ASSERT**：链上属于同一 plane 的所有 desc，其行地址位（block/page/lun）必须一致——
一个 plane 一次只访问一个物理位置（与 `fc_command_plane_span` 的 `ASSERT(plane_count <= PLANE_NUM)` 同风格）。
单 plane 命令也加了一条"若该 plane 在链上，则其行地址须与 `command->paa` 一致"的兜底断言。

**行为中性论证**：现有全部调用者（策略层、FTL 的 `pba_to_cmd`）传进来的
`row_paa` 都是"`command->paa` 的 page/block/lun + 目标 plane"，与 v11 的
`paa_make(paa_to_page(cmd->paa), plane, paa_to_block(cmd->paa), paa_to_lun(cmd->paa), cw)` 逐字节相同。
FTL 侧的 `ftl_cmd_setup_ppa_pair()`（`src/ftl/ftl_fc_interface.c`）是第一个真正传不同行地址的调用者。

**不改**：erase / SET/GET_FEATURES 分支仍读 `command->paa`（非数据命令无 desc 链）；
`fc_command_plane_span` 仍按 `command->paa` 定 plane 段（`multi_plane > 0` 时恒 `[0, multi_plane)`，
故 multi-plane 命令的行地址来源变化不影响 plane 段判定）。

> 详细落地见 `docs/archive/changelog/fc_per_plane_row_address.md` 与 FTL 侧的 `ftl_policy_b_per_plane_block.md`。

##### v13：策略层上移 FTL

v11 把"策略 / 机制"拆成了两层，但**两层都在 FC 内**。v13 把策略层整个搬到 FTL——
FC 的对外函数按"硬件 FCC 寄存器表"判据过了一遍（见 §26），只留下有寄存器对应物的部分。

| 项 | v12 | v13 |
|---|---|---|
| 位图枚举 | `fc_command_configure_descriptors_internal`（`fc.c`，`_internal`） | `ftl_cmd_configure_descriptors()`（`src/ftl/ftl_fc_interface.c`），**读同一个 `cmd->param.cw_bitmap`** |
| plane 段规则 | `fc_command_plane_span`（`fc.c` static） | 同规则另有一份 `ftl_cmd_plane_span`（`ftl_fc_interface.h` static inline）——两份实现（**v13.1 起合并为一份，见下**） |
| 位图越段校验 | `fc_command_validate` 内 | `ftl_cmd_configure_descriptors` 内（FC 不再校验 `param.cw_bitmap`） |
| 无数据命令短路 | `fc_command_configure_descriptors_internal` 内 | `ftl_cmd_needs_desc_chain()`（`ftl_fc_interface.h` static inline） |
| 数据搬运 | `fc_write_cmd_data` / `fc_read_cmd_data`（`fc_interface.c`） | `ftl_cmd_write_data` / `ftl_cmd_read_data`（`ftl_fc_interface.c`），走公共访问器 `fc_descriptor_id_to_descriptor` + `fc_buffer_id_to_real_buffer` |
| 建链入口 | `configure_descriptors` + `attach_cw` 两个 | **只有 `fc_command_attach_cw`**（`configure_descriptors` 已删） |
| 接口数量 | 20 | 18（删 2 增 1：新增 `fc_get_cmd_cw_bitmap`，v13 时名 `fc_get_cmd_cw_mask`） |

**FC 侧的连带收敛**：

- `fc_command_resources_rollback` 的唯一剩余调用者在 `#if CONFIG_FC_RAID` 块内（`fc_stripe_submit` 的 parity 建链失败路径），
  RAID 关闭时无引用 → 标 `__attribute__((unused))`。
- `fc_stripe_submit` 的 parity 建链（原第 ③ 步调 `configure_descriptors_internal`）改为直接循环 `fc_command_attach_cw_internal`——
  parity 是单 plane（`PLANE_NUM-1`）命令、CW 集合 = 连续 `0..config.cw`，与 FTL 侧 `mask == 0` 的展开结果一致
  （**v23 起** FTL 侧的展开是"本命令 plane 段内的全部 CW"，plane 段 = `[PLANE_NUM-1, PLANE_NUM)` → 仍是同一个集合）。
- 三处 `fc_cw_bitmap_is_plane_symmetric` 断言（`fc.c` 的 `fc_stripe_submit`、`fc_data_path.c` 的 stripe parity 两处）
  原来读 `command->param.cw_bitmap`（v13 前该字段在 `src/` 里**从无写入者**，故三处恒真）。
  现改为读 **`fc_get_cmd_cw_bitmap(cmd)`**——从 desc 链重建位图再断言，与"链是 CW 集合的唯一真值"一致，
  顺带把这个既有死角变成真检查。

**离散 CW 读能力保留**：`cmd->param.cw_bitmap = (1U << 4) | (1U << 6)`（plane1 的 CW0/CW2）
再调 `ftl_cmd_configure_descriptors()` 即得离散链——能力与 v12 逐字节相同，只是发起者在 FTL。

> 详细落地见 `docs/archive/changelog/fc_policy_layer_uplift.md`。

##### v13.1：建链收敛（plane 段规则合并 + 删冗余断言）

v13 留下了两处可收敛的冗余，v13.1 一并处理，**行为中性**（产出给 NAND 的 `nand_dispatch_request_t` 逐字节不变）：

1. **plane 段规则合并为一份**。`fc_command_plane_span` 上移到 `include/fc/fc_command.h` 作 `static inline`
   （它只读 `command->config.multi_plane` / `command->paa`，不碰 `fc_ctl_s`，故不受"FTL 看不到 `fc_ctl_s`"约束），
   FC 的越段校验与 FTL 的掩码展开**共用同一份定义**；`ftl_cmd_plane_span`（`ftl_fc_interface.h`）删除。
   改 plane 语义（`PLANE_NUM` / BA6 含义 / 单 plane 起点）从此只需动一处。
   ⚠ FC 侧的越段校验（`fc_command_attach_cw_internal` 里 `plane` 不在 `[first, last)` 返回 `INVALID_ARGUMENT`）
   **保留**——它挡的是"desc 落到 `plane_paa[]` 覆盖范围之外"这个静默腐坏，不是重复实现。

2. **同源查重断言二选一**。"无重复 `n`"（掩码位数 == 链长）原先在两层各查一次：
   `fc_command_attach_cw_internal` 的 attach 侧（`fc_command_cw_attached`，覆盖单/多 plane 全部路径）
   与 `fc_build_request_data_planes` 单 plane 分支（`fc_cw_mask_popcount(mask) == fc_command_descriptor_count(...)`）。
   保留前者——它在**污染点**（attach）就炸，定位更准，且覆盖完整；后者连同 `fc_cw_mask_popcount` 删除。
   `fc_command_descriptor_count` 不受影响（`fc_command_release` 等仍在用）。

> 详细落地见 `docs/archive/changelog/fc_plane_span_consolidation.md`。

> **v16 更正（理由，非结论）**：上面保留 `fc_command_cw_attached` 的**结论**不变，但
> 「掩码迭代可能产生重复」这个**理由不成立**——掩码是集合，枚举不会重复置位。
> 真正可达的重复路径是 `ftl_cmd_setup_ppa_pair`：plane 号取自 `pba_to_plane(pba[i])`
> （L2P 查表结果），两个 LBA 可能解析到同一 plane → 重复 `(plane, cw)`。
> ⚠ release 构建下该断言被裁空，此违规无守卫。详见 `docs/archive/changelog/fc_chain_ownership_v16.md`。

##### v17：建链三层接力 → 一条公共入口

上面各小节里的 `fc_command_attach_cw` / `fc_command_attach_cw_internal` /
`fc_command_configure_descriptor_internal` 三个名字，是 v13~v16 期间的三层接力：

| # | 名字（v13~v16） | 位置 | 职责 |
|---|---|---|---|
| 1 | `fc_command_attach_cw` | `fc_interface.c` | NULL 断言 + 指针身份 + 生命周期（须 `ALLOCATED`） |
| 2 | `fc_command_attach_cw_internal` | `fc.c` | plane 段 / CW 范围校验 + 重复 CW 断言 |
| 3 | `fc_command_configure_descriptor_internal`（static） | `fc.c` | 机制体：出池 desc → 填字段 → 挂链 → 出池 buffer |

2 与 3 **签名逐字相同、纯直通**，且 3 只有 2 一个调用者——属投机性分层。v17 合并为**唯一一条**
`fc_command_configure_descriptor`（**单数**），机制体落在 `src/fc/fc_interface.c`：

- **校验段整体前置于出池**：指针身份 / 生命周期 / plane 段 / `cw >= CW_NUM` 全部在
  `fc_descriptor_alloc_internal` 之前返回 `INVALID_ARGUMENT`，非法实参不污染池子。
- 机制段照搬：`fc_descriptor_alloc_internal` → 填 `paa`/`config`/`next_id`/`meta` →
  **立刻 `fc_command_chain_append`** → `fc_buffer_alloc_internal`（`RESOURCE_EMPTY` 时不回滚，
  desc 已在链上、`buffer_id` 保持 INVALID）。
- `fc.c` 只留池/链原语：`fc_descriptor_alloc_internal` / `fc_command_chain_append` 去 `static`
  并声明进 `include/fc/fc_internal.h`（附「分配后必须立刻挂链」契约）。
- Stripe RAID parity 建链改调公共入口 `fc_command_configure_descriptor(parity_cmd, parity_plane, pcw, parity_row)`
  （该路径由此硬绑单例 `g_fc_controller`——当前全仓只有一个 `fc_ctl_s` 实例，多实例时须改回 `controller` 参数化入口）。

详见 `docs/archive/changelog/fc_build_chain_convergence_v17.md`。

##### v23：FTL 建链收敛为「单一掩码旋钮」（2026-10-08）

**问题**：`mask == 0` 的展开规则有两份实现，两个入口各认一个旋钮——

| 入口 | 调用者 | v22 前认哪个旋钮 |
|---|---|---|
| `ftl_cmd_configure_descriptors`（`ftl_fc_interface.c`） | GC / 同步原语 / 测试 | `param.cw_bitmap`，0 时退回 `config.cw` |
| `ftl_cmd_setup_ppa_pair`（同文件） | **host 读写**（`ftl.c`） | 只认 `config.cw`，**完全不读 `param.cw_bitmap`** |

即 `param.cw_bitmap` 这条通道**主数据路径根本不看它**。今天没出事只因为生产路径从不设它
（`src/` 内语义写入者为 0），而 `config.cw` 在生产恒为 3。

**改动**：把 `0` 定义成「**本命令 plane 段内的全部 CW**」，两个入口共用同一个
`ftl_cmd_resolve_cw_mask()`（`src/ftl/ftl_fc_interface.c`，static）。`config.cw` 自此
**退出建链**（写入者保留；读者只剩 FC 内 RAID stripe 的 `cw_count`，默认不编译）。
越段掩码由 `return FTL_ERROR` 改为 **`ASSERT`**（判据见 `assertion_guidelines.md` §十三，
v18 只收了 FC 侧入口，FTL 侧这一处是同判据的漏网）。

**行为中性**：生产 `config.cw` 恒为 3 → 旧的逐 plane 展开得 `0x0F`（单 plane）/ `0xFF`（双 plane），
与新语义"段内全部 CW"**逐位相同**；`ftl_cmd_setup_ppa_pair` 的唯一生产调用者不设
`param.cw_bitmap`（恒 0）→ 链几何逐字不变。`sync_page_prepare` 的 `cw` 死参数（唯一用途是喂建链）
随之删除。

**当时遗留的「两层 0 方向相反」**：`cmd->param.cw_bitmap`（FC 命令层，FTL 唯一读者）是**意图**，
0 = 未指定；`request->cw_bitmap`（NAND 层）是**已解析结果**，0 = **合法空集**。
两者共用 flat 编号但值不相等，FC 按链/按操作重新铸出后者。**v27 已消除这条不对称**（见下）。

详见 `docs/archive/changelog/ftl_cw_mask_single_knob_v23.md`。

##### v27：`cw_mask` → `cw_bitmap`，取消「0 = 段内全部」重载（2026-10-09）

**动机**：v23 的「`0` = 本命令 plane 段内全部 CW」是**重载**——同一个字段既有"字面集合"读法，
又有"未指定 → 展开"读法。它带来两个代价：

1. **两层 0 方向相反**（见上）：FC 侧 0 = 全部，NAND 侧 0 = 空集。这不是笔误，是重载的直接产物，
   只能靠文档解释，任何"顺手统一一下"的改动都会静默改变语义。
2. **名字与语义不符**：字段叫 mask 时"0 = 全部"勉强说得通（位掩码的常见重载），但它逐位表达的是
   **集合**，与 NAND 层的同名字段口径不一致。

**改动**：字段改名 **`cw_bitmap`**（FC 侧 `fc_cmd_s.param.cw_bitmap`、NAND 侧
`nand_dispatch_request_t.cw_bitmap`），语义统一为**字面位图**：置 1 的位 = 要访问的 CW，
**全 0 = 一个都不访问**。v23~v26 的重载取消。

**「整段访问」改为显式**——新增 `fc_command_plane_span_cw_bitmap(cmd)`（`fc_command.h` static inline），
返回本命令 plane 段内全部 CW 的 flat 位图（plane1 单 plane ⇒ `0xF0`，双 plane ⇒ `0xFF`）。
三个语义为"整段访问"的 FTL 入口各自调用它：

| 入口 | 调用者 | v27 起的位图来源 |
|---|---|---|
| `pba_to_cmd`（`ftl_fc_interface.h`） | `sync_page_prepare` → 四个 `ftl_sync_*` 页原语；`gc.c` 的同步读 | 入口自己填 `plane_span_cw_bitmap` |
| `ftl_cmd_setup_ppa_pair_cw` | **host 读写**（`ftl.c`） | 同上；**调用者先前设的位图会被丢弃** |
| `ftl_cmd_configure_descriptors` | GC / 测试 / Read Recovery | **唯一**接受调用者给的部分位图 |

⚠ `fc_command_plane_span_cw_bitmap` 读的是 `cmd->paa` 与 `config.multi_plane`，
**必须在两者都写好之后**调用；否则算出的段是错的（而段错时越段 ASSERT 可能不炸——段算小了
只会少访问几个 CW，属于静默错）。

**已知代价**：取消重载后，「生产者忘了填位图」与「有意访问 0 个 CW」**不可区分**。
唯一拦网是 `ftl_cmd_resolve_cw_bitmap()` 的第一条 `ASSERT(bitmap != 0U)`，而 ASSERT 在
release 下会被裁掉 ⇒ 忘填的代价是**编译期无声、release 下静默空操作**。这是本轮接受的权衡
（备选方案是把"未指定"编码进 `config.reserved` 或改用 `bitmap == 0` + 显式 flag，都被判为
引入第二个旋钮、得不偿失）。

**连带**：`fc_get_cmd_cw_mask` → `fc_get_cmd_cw_bitmap`、`fc_cw_mask_is_plane_symmetric` →
`fc_cw_bitmap_is_plane_symmetric`、`ftl_cmd_resolve_cw_mask` → `ftl_cmd_resolve_cw_bitmap`、
`nand_dispatch_cw_first`/`_next` 形参改名；SQ 请求集 / CQ 失败集 / NAND 请求集**三处现在同口径**
（0 = 空集），「两层 0 方向相反」这条不再需要解释。

详见 `docs/archive/changelog/fc_cw_bitmap_literal_v27.md`。

##### v28：CW 集合退出命令结构体，改作建链形参（2026-10-09）

**动机**：v27 之后，`cmd->param.cw_bitmap` 这个字段在 FC 内**既无写入者也无读者**——
唯一读者 `ftl_cmd_resolve_cw_bitmap()` 就在建链那一刻把它取出来用掉。它是**建链的输入**，
不是命令的属性，没有留在命令里被回读的理由。同理 CQ 侧的失败位图是**派生量**：
上层本就能遍历 desc 链 OR 各 `desc->status` 自己算。

**改动**：

| 项 | v27 | v28 |
|---|---|---|
| SQ 请求集 | `cmd->param.cw_bitmap` 字段 | **形参** `ftl_cmd_configure_descriptors(cmd, cw_bitmap)` |
| CQ 失败集 | `completion.param.cw_bitmap`（`fc_complete_from_nand` 聚合） | **删除**；上层遍历 desc 链自取 |
| `fc_param_u` | `parameter0..3` / `bytes[4]` / `cw_bitmap` | `parameter0..3` / `bytes[4]`（仅 FEATURES 内联数据） |
| `fc_cmd_status_s.param` | 失败位图 / FEATURES 回读 | **保留**（仅 FEATURES 回读） |
| ERASE 请求位图 | 伪造 CW0 位（给 completion 一格写 status） | **合法空集**，由 `completion_all_cws` 空集分支写 `cw_status[0]` |

`ftl_cmd_resolve_cw_bitmap()` 的首参由 `const fc_cmd_s *cmd` 改为 `uint32_t bitmap`——
它现在校验的是**调用者给的值**，与命令结构体无关。

**擦除假位的来龙去脉**：v7 起 FC 给无数据的 ERASE 请求硬置一个 CW0 位（`fc.c` 的 ERASE 分支），
唯一目的是让 `completion_all_cws` 的 `cw_next(bitmap, 0)` 循环至少跑一次、把 `cw_status[]` 写上——
否则 FC 的非 page read 聚合分支（全表 OR）读不到擦除成败。v28 把这个"为 completion 服务的假数据"
从 FC 侧移到 NAND 侧：`completion_all_cws` 补一条 `bitmap == 0 ⇒ 写 cw_status[0]` 分支
（与 `completion_single` 早已有的一条**同口径**）。好处是请求位图从此**只表达"要访问哪些 CW"**
这一个意思，不再兼职"给完成项留格子"。

**风险**：`request->cw_bitmap == 0` 的消费面因此扩大（此前只有 RESET/STATUS/块锁等，现在加上擦除）。
`nand_dispatch_task_load` 对空掩码一格都不搬 `cw_buffers[]`（残留旧值），而 `req_erase` 只用
`plane_paa[]` 不读 `cw_buffers[]`，故安全——但"空掩码 = 无 buffer 可读"这条契约现在**依赖
消费点真的不读**，新增无数据 op 时须复核（已在 `nand_dispatch.c` 的 `task_load` 注释里写明）。

详见 `docs/archive/changelog/fc_cw_bitmap_param_v28.md`。

### 4.4 索引空间统一（v9）

##### 动机

v7 的掩码是 flat 的，但**掩码下游不是**：`fc_build_request_data_planes` 仍把 CW 分 plane 桶、
在每个 plane 内**密排**成槽位（`cws[plane][slot]`），NAND 侧再用 `cw_count[]` + 走减把槽位换算回
`(plane, ordinal)`。于是"掩码"与"槽位"是两套坐标，靠一条隐式不变量对齐：

> 链序 == plane-major（先 plane 0 的全部置位 CW，再 plane 1）

代价有三：

1. **隐式不变量**：链序一旦与 plane-major 不符，数据静默落到错误的列——而链序由建链处的
   枚举顺序决定，离消费点很远。
2. **空 plane 的补丁**：多 plane 加载的行地址取自 `cws[plane][0].paa`，空 plane（如只读
   plane1 = `0xF0`）必须补一条 CW=0 的 PAA，否则行地址退化成 block0/page0。这是"行地址寄居在
   CW 项里"逼出来的补丁。
3. **两套坐标的换算代码**：`cw_count[PLANE_NUM]`、`nand_dispatch_slot_to_plane_cw()`、
   `nand_dispatch_task_total_slots()`，以及 `completion_multi_plane`（与 `completion_all_cws`
   在统一后逐字节相同，已合并删除）。

##### 设计：一个坐标

**统一为 flat 绝对 CW 号 `n = plane*CW_NUM + cw`（0..CW_TOTAL_MAX-1）**，与 `param.cw_mask`
的 bit 编号、CQ 回传的 `param.cw_mask` 完全同一空间。行地址从 CW 项里**拆出来**，改由
per-plane 的一维数组承载：

```c
uint32_t cw_mask;                    /* bit n = plane*CW_NUM + cw */
DWORD plane_paa[PLANE_NUM];     /* 各 plane 的行地址承载 PAA（与掩码解耦） */
uint8_t   *cw_buffers[CW_TOTAL_MAX]; /* [n]，仅 n ∈ cw_mask 有效 */
```

为什么 `plane_paa[]` 不能省：`paa_to_nand_row()` 把 plane 编码进行地址（BA6），
所以 multi-plane 命令的两个行地址**必须**分别是两个 plane 的——一条行地址推不出另一条。
拆成显式数组后，"空 plane 也要填"从"槽位 0 无条件照拷"的隐式约定，变成一条按 plane 遍历的
直白赋值。

##### 改动

| 文件 | 改动 |
|------|------|
| `include/nand/nand_dispatch.h` | 删 `nand_dispatch_cw_info_t`；请求改 `cw_mask` + `plane_paa[]` + `cw_buffers[]`；完成项的 `cw_status[]` / `ecc_bit_count[]` 下标语义改为 flat `n` |
| `src/nand/nand_dispatch_internal.h` | 新增 `static inline nand_dispatch_cw_next()` / `nand_dispatch_cw_first()`（掩码遍历，两文件共用）；task 删 `cw_count[]` / `cw_paa[][]`，`cw_index` 改存 flat `n` |
| `src/nand/nand_dispatch.c` | `task_load` 按掩码拷 buffer + 逐 plane 拷 PAA；删 `total_slots` / `slot_to_plane_cw`；`cw_chain` 用 `cw_next` 推进并直接由 `n` 算列地址/plane；`scatter` 按掩码遍历 |
| `src/nand/nand_dispatch_tables.c` | 12 个 `req_*` 全改按掩码遍历；`completion_all_cws` 按掩码写；**`completion_multi_plane` 删除**（与 `completion_all_cws` 逐字节相同，3 个表项改指）；`completion_single` 加掩码分支（空掩码才写 `[0]`，否则按 `n`——否则 `OP_READ_PAGE_LOAD` 这类带掩码 op 的失败状态会被 FC 按 `n` 读时丢失） |
| `src/fc/fc.c` | `fc_build_request_data_planes` 落位改 `cw_mask` + `cw_buffers[n]`（新增"重复落位"断言）+ 逐 plane 填 `plane_paa[]`；`fc_build_dispatch_request` 的 ERASE / ERASE_MULTI_PLANE / FEATURES 改掩码；`fc_complete_from_nand` Step 1 按 desc 自己的 PAA 算 `n` 取状态 |
| `include/nand/nand_config.h` | 注释：`cw_paa` 的 plane 维 → `plane_paa` 的长度 |

**行为中性**：满掩码（全部 FTL 现状）下新旧逐字节等价——`n` 与槽位一一对应。
差异只在稀疏掩码下，且差异是"修对了"（见 §25.3 历史注）。

##### 新增断言

| 位置 | 断言 | 理由 |
|------|------|------|
| `fc_build_request_data_planes` | 同一 `n` 不被两条 desc 覆盖 | 掩码是集合，重复置位会静默丢数据 = desc 链构造错误 |
| `fc_build_request_data_planes` | `popcount(cw_mask) == 链上 desc 数`（单 plane） | 多一条或少一条都说明有 desc 落到了别的 plane |
| `fc_complete_from_nand` Step 1 | `n < CW_TOTAL_MAX` | 由 PAA 现算，越界即 PAA 构造错误 |

##### 验证

- 三档零警告：debug 152128B / release 87016B / `make format-check` 通过。
- `make -k tests`：仅卡在用户维护的 `test_fc_level1.c`（旧 scheduler API）与
  `test_fc_level3.c:48`（旧 `set_device_ready`），与本次改动无关。
- **上板未验证**（用户执行）。

##### 风险

| # | 风险 | 说明 |
|---|------|------|
| 1 | `completion_single` 的掩码分支 | 空掩码写 `[0]`、非空按 `n`。漏掉这个分支时 `OP_READ_PAGE_LOAD`（带掩码、走 `completion_single`）的失败状态会丢——已修，且只在稀疏掩码 + 失败路径才显形 |
| 2 | 三处 completion handler 的下标语义 | `completion_all_cws` / `per_cw` / `load` 现在写的都是 flat `n`，FC 也按 `n` 读——两侧必须同时是 v9，不能一新一旧 |

### 4.5 Multi-Plane 与行地址（Phase E）

#### 17.1 动机

MT29F4G08ABADAWP 支持 2 Plane 操作（Multi-Plane READ/PROGRAM/ERASE），但现有架构将 `cw_paa/buffers` 设计为平面一维数组 `[CW_NUM]`(4)，无法支持 Multi-Plane 场景中 `CW_NUM × PLANE_NUM = 8` 的 CW 总数。

同时，`nand_cmd_req_s.row` 为单值 `uint32_t`，Multi-Plane 操作需要发送两组行地址（Plane 0 和 Plane 1），而地址周期中的 plane bit 差异仅在于 BA6。

> **注（Phase E 之后的修订）**：Phase E 引入 `[PLANE_NUM][CW_NUM]` 时，第一维在**单 plane** 命令下仍恒取 `[0]`（"第几组槽位"语义），只有 multi-plane 命令按物理 plane 落位。后续修订把第一维统一为**物理 plane 索引**：新增 `nand_dispatch_request_t.plane` / `nand_dispatch_task_t.plane`，单 plane 命令按 `command->paa` 的 plane 落位到 `cws[plane][*]` / `cw_paa[plane][*]`。
> **再后续（v9）**：二维 `cws[PLANE_NUM][CW_NUM]` 整体删除，改为**一维 flat 索引** `cw_mask` + `cw_buffers[n]`（`n = plane*CW_NUM + cw`），行地址拆到 per-plane 的 `plane_paa[PLANE_NUM]`。§17.2 起的方案对比/结构/代码均为 Phase E 历史记录，未随该修订重写，看当前实现请读 §25.11。

#### 17.2 数据维度扩展

##### 方案对比

| 方案 | 做法 | 评价 |
|------|------|------|
| A: 展平 cw_count | `cw_paa[CW_TOTAL_MAX]` 展平为 8 个，`cw_count` 语义变为"总 CW 数" | ❌ 丢失 plane 维度信息，每 plane CW 数量不明确 |
| B: 加 plane 维度（选中） | `cw_paa[PLANE_NUM][CW_NUM]`，`cw_count` 仍为"每 plane CW 数"，`plane_count` 标明 plane 数 | ✅ 维度正交，语义清晰 |

选中方案 B 后各层数据结构的变化：

```c
// nand_dispatch_request_t — 请求
nand_dispatch_cw_info_t cws[PLANE_NUM][CW_NUM];  // [plane][cw]

// nand_dispatch_task_t — 任务
DWORD cw_paa[PLANE_NUM][CW_NUM];
uint8_t   *cw_buffers[PLANE_NUM][CW_NUM];
uint8_t    cw_count;     // 每 plane CW 数
uint8_t    plane_count;  // plane 数 (1/2)

// nand_dispatch_completion_t — 完成
uint8_t cw_status[CW_TOTAL_MAX];      // 8 个（vs 原有 4 个）
uint8_t ecc_bit_count[CW_TOTAL_MAX];
```

`fc_cmd_cfg_s.cw` 位宽不变（仍为 2 位，最大表示 4 CW/plane），Multi-Plane 通过独立 OP 码区分。

#### 17.3 行地址数组重构

##### 方案对比

| 方案 | 做法 | 评价 |
|------|------|------|
| A: 新增 uop 类型 | 加 `NAND_UOP_ROW_0_SECOND`/`ROW_1_SECOND`/`ROW_2_SECOND` 三个新 uop 类型 | ❌ 2 plane 加 3 个，4 plane 加 9 个，不可扩展 |
| B: **operand 选择索引** | `row` 改为 `row[NAND_PLANE_MAX]`，ROW_0/1/2 handler 通过 `uop->operand` 选择 row 索引 | ✅ 零新增 uop 类型，任意 plane 数不改 uop |

选中方案 B：

```c
// nand_fc_interface.h / nand_engine.h
uint32_t row[NAND_PLANE_MAX];  // row[0]=plane0, row[1]=plane1, ...

// nand_fmc.c — ROW handler 改动
*config->address = (uint8_t)(request->row[uop->operand]);
```

uop 序列中通过 operand 选择 plane：

```c
{ NAND_UOP_ROW_0,  0U },  // operand=0 → row[0] (Plane 0)
{ NAND_UOP_ROW_0,  1U },  // operand=1 → row[1] (Plane 1)
```

单 plane 操作 operand 全部为 0，完全向后兼容。

#### 17.4 OP 码设计： WITH_DATA_OUT / WITHOUT_DATA_OUT

##### 设计原因

NAND 读取命令（00h→地址→30h→tR）执行后，数据被加载到 NAND 内部页缓冲。**是否立即输出数据**是两个独立场景：

| 场景 | 做法 | 适用 |
|------|------|------|
| 立即输出 | 30h→tR→**Dout** | 正常读路径、单页读完即用 |
| 延迟输出 | 30h→tR→**无 Dout** | Multi-Plane 读、配合 05h-E0h 随机读、Cache Read 链 |

两种场景执行完全相同的地址序列和 tR 等待，差异仅在 uop 序列末尾是否包含 `NAND_UOP_DATA_OUT` 一条。

为此，单 Plane 和多 Plane 的读操作均拆分为两个 OP 码：

| OP 码 | 值 | 说明 |
|-------|----|------|
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | 12 | 单 Plane 读 + 立即 Dout（原 `OP_NORMAL_READ_PAGE`） |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | 34 | 单 Plane 读，仅加载到页缓冲 |
| `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT` | 29 | 双 Plane 读，加载到两页缓冲 |
| `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | 30 | 双 Plane 读 + 立即双页 Dout |

PROGRAM 和 ERASE 不存在"延迟执行"场景，故不分拆。

##### uop 序列差异示例

```c
/* WITH_DATA_OUT: 末尾含 DATA_OUT */
{ NAND_UOP_COMMAND, 30h },
{ NAND_UOP_DELAY_US, 1U  },   /* tWB */
{ NAND_UOP_WAIT_READY,   },
{ NAND_UOP_DATA_OUT,     },   /* ← 立即输出 */
{ NAND_UOP_COMMAND, 70h  },   /* 70h 状态轮询 */
{ NAND_UOP_DELAY_US, 1U  },   /* tWHR */
{ NAND_UOP_DATA_OUT_STATUS },
{ NAND_UOP_JUMP_IF_NOT_READY, 3U },  /* busy 则跳回 70h */
{ NAND_UOP_CHECK_FAIL,     },
{ NAND_UOP_END,            },

/* WITHOUT_DATA_OUT: 末尾不含 DATA_OUT */
{ NAND_UOP_COMMAND, 30h },
{ NAND_UOP_DELAY_US, 1U  },   /* tWB */
{ NAND_UOP_WAIT_READY,   },
{ NAND_UOP_COMMAND, 70h  },   /* 70h 状态轮询（跳过 DATA_OUT） */
{ NAND_UOP_DELAY_US, 1U  },   /* tWHR */
{ NAND_UOP_DATA_OUT_STATUS },
{ NAND_UOP_JUMP_IF_NOT_READY, 3U },
{ NAND_UOP_CHECK_FAIL,     },
{ NAND_UOP_END,            },
```

#### 17.5 Multi-Plane 操作 OP 码

| OP 码 | 值 | uop 序列 | data_length |
|-------|----|---------|-------------|
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | 12 | 70h轮询→00h→C1C2R1R2R3→30h→tWB→tR→Dout→70h轮询→CHECK_FAIL | PAGE_SIZE |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | 33 | 70h轮询→00h→C1C2R1R2R3→30h→tWB→tR→70h轮询→CHECK_FAIL | PAGE_SIZE |
| `OP_RANDOM_DATA_READ` | 8 | 70h轮询→05h→C1C2→E0h→tR→Dout→70h轮询 | CW_SIZE |
| `OP_RANDOM_DATA_READ_MULTI_PLANE` | 41 | 70h轮询→06h→C1C2→R1R2R3→E0h→tR→Dout→70h轮询 | CW_SIZE |
| `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT` | 29 | 70h轮询→00h→C1C2R1R2R3(P0)→00h→C1C2R1R2R3(P1)→30h→tWB→tR→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | 30 | 70h轮询→00h→C1C2R1R2R3(P0)→00h→C1C2R1R2R3(P1)→30h→tR→06h→C1C2R1R2R3(P0)→E0h→Dout(P0)→06h→C1C2R1R2R3(P1)→E0h→Dout(P1)→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_PROGRAM_PAGE_MULTI_PLANE` | 31 | 70h轮询→80h→C1C2R1R2R3(P0)→Din(P0)→**11h→tADL→80h**→C1C2R1R2R3(P1)→Din(P1)→10h→tWB→tPROG→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_TWO_PLANE_PROGRAM_CACHE` | 44 | 70h轮询→80h→C1C2R1R2R3(P0)→Din(P0)→11h→tADL→80h→C1C2R1R2R3(P1)→Din(P1)→15h→tWB→tCBSY→70h轮询→**CHECK_FAIL_CACHE** | PAGE_BUFFER_SIZE |
| `OP_TWO_PLANE_PROGRAM_CACHE_END` | 48 | 70h轮询→**CHECK_FAIL_CACHE**（上一页 FAILC）→80h→C1C2R1R2R3(P0)→Din(P0)→11h→tADL→80h→C1C2R1R2R3(P1)→Din(P1)→10h→tWB→tPROG→70h轮询→**CHECK_FAIL_CACHE** | PAGE_BUFFER_SIZE |
| `OP_TWO_PLANE_PROGRAM_CACHE_FIRST` | 49 | 同 op44，但末尾只 `CHECK_FAIL`（首页前无本批 cache 页，FAILC 可能残留） | PAGE_BUFFER_SIZE |
| `OP_READ_PAGE_CACHE_START_SEQ_MP` | 45 | 70h轮询→00h(addr P0)→00h(addr P1)→30h→tR→31h→tRCBSY→06h(P0)→E0h→Dout(P0)→06h(P1)→E0h→Dout(P1)→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_READ_PAGE_CACHE_SEQ_MP` | 46 | 31h→tRCBSY→06h(P0)→E0h→Dout(P0)→06h(P1)→E0h→Dout(P1)→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_READ_PAGE_CACHE_LAST_MP` | 47 | 3Fh→tRCBSY→06h(P0)→E0h→Dout(P0)→06h(P1)→E0h→Dout(P1)→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_ERASE_BLOCK_MULTI_PLANE` | 32 | 70h轮询→60h→R1R2R3(P0)→60h→R1R2R3(P1)→D0h→tWB→tBERS→70h轮询→CHECK_FAIL | 0 |

> **Two-Plane Cache 操作（器件扩展，非 ONFI 1.0）**：MT29F4G08ABADAWP
> datasheet Rev. N 明确支持
> - **Two-Plane Program Page Cache**（Figure 74，p.100–101）：
>   中间页 `80h-11h-80h-15h`，末页 `80h-11h-80h-10h`；
>   ⚠ 状态寄存器 **bit1 = FAILC** 表示**上一页** cache program 的成败（bit0 = FAIL 只表当前页）。
>   只查 FAIL 会漏掉中间页失败，因此 op44/op48 使用 `CHECK_FAIL_CACHE`（FAIL|FAILC）；
>   失败归因可用 `78h` 逐 plane 查状态。末页必须用 op48，不能用普通 op31（只查 FAIL）。
> - **Two-Plane Read Cache**（p.62–63）：
>   `00h-00h-30h` + `31h/3Fh`，用 `06h-E0h` 分别选 P0/P1 的 cache register 输出。
>
> 本仓新增的器件扩展 op：`OP_TWO_PLANE_PROGRAM_CACHE`(44)、
> `OP_READ_PAGE_CACHE_START_SEQ_MP`(45)、`OP_READ_PAGE_CACHE_SEQ_MP`(46)、
> `OP_READ_PAGE_CACHE_LAST_MP`(47)。OP_COUNT = 48。
> 单 plane `OP_READ_PAGE_CACHE_START` 已改为 `START_SEQ`（含首条 `31h`，输出第一页）。

#### 17.6 内部搬移 OP 码：片上 Copy-Back

内部数据搬移利用 NAND 的 Read-Modify-Write 能力，将数据从源页读到 NAND 内部缓存寄存器后直接写入目标页，无需经过 Host 总线。MT29F4G08ABADAWP 的 ONFI copy-back 时序为：

```
00h + addr → 35h         // READ_FOR_INTERNAL_DATA_MOVE：源页读入缓存，tR 后 ready
85h + addr → 10h         // PROGRAM_FOR_INTERNAL_DATA_MOVE：缓存数据写入目标页，tPROG 后 ready
两个 OP 码成对支持内部操作：

| OP 码 | 值 | uop 序列 | 说明 |
|-------|----|---------|------|
| `OP_READ_FOR_INTERNAL_DATA_MOVE` | 19 | `70h→Status→00h→C1C2R1R2R3→35h→tR→70h→Status` | 源页读入 NAND 内部缓存 |
| `OP_PROGRAM_FOR_INTERNAL_DATA_MOVE` | 10 | `70h→Status→85h→C1C2R1R2R3→10h→tPROG→70h→Status` | 纯 copy-back，无 DATA_IN（NAND 内部转发） |

> 操作码 20（原 `OP_PROGRAM_FOR_INTERNAL_MOVE`）已删除：它与上面 op 10 功能完全重复，只保留 ONFI 标准命名的那一个。

`has_data` 均为 1（分配 desc chain 提供 PAA 映射），`data_length` 均为 0。
若要在搬移中改写页内数据，走普通读 + 普通写路径（GC 已如此），不在这两个 op 上加 DATA_IN。

#### 17.7 关键命令码：11h

`NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH (0x11)` 在 ONFI 标准中定义为 Multi-Plane COPYBACK READ 命令，在多 Plane PROGRAM 时序中起到切换 Plane 的作用：

```
80h → C1C2R1R2R3 → Din(Plane0 全页) →
11h → C1C2R1R2R3 → Din(Plane1 全页) →       ← 11h 切换 Plane
10h → tPROG（双 Plane 同时烧录）
```

#### 17.7a 数据缓冲扩展

```c
#define PAGE_BUFFER_SIZE (PAGE_SIZE * PLANE_NUM)  // 4224 字节
uint8_t g_nand_channel_page_buffer[CH_NUM][CE_NUM][LUN_NUM][PAGE_BUFFER_SIZE];
```

按 (ch, ce, lun) 各持一块：同一通道的多个 LUN 由 `lun_step` 轮流驱动，而 `request.data`
一旦指向本缓冲就跨多次 poll 存活（program ISSUE 常被 70h 忙轮询挡下，数据已拼好却要
等下一轮才 DATA_IN）——全通道共用一块会被后驱动的 LUN 覆盖。

单 Plane 操作只用首 `PAGE_SIZE` 字节，多 Plane 操作写入 `PAGE_BUFFER_SIZE` 字节。

req_multi_plane_program handler 将各 Plane 各 CW 的数据拼接到 page_buffer：

```c
for (plane = 0; plane < PLANE_NUM; plane++) {
    for (cw = 0; cw < task->cw_count; cw++) {
        memcpy(&page_buffer[plane * PAGE_SIZE + cw * (PAGE_SIZE / CW_NUM)],
               task->cw_buffers[plane][cw], PAGE_SIZE / CW_NUM);
    }
}
```

读完成后 poll 中拆回各 plane 各 CW buffer 的逻辑同理。

#### 17.8 NAND 数据传输模式：中转 vs 直写

NAND 读写操作根据输出/输入方式不同，采用中转或直写两种数据传输模式。

##### 直写模式（Direct Write/Read）

NORMAL_READ_PAGE_WITH_DATA_OUT 利用 NAND 的 `05h-E0h` 随机列读能力实现无中转的 CW 级读取：

```
第1步: 00h-addr-30h → DATA_OUT(528B, CW0) → 直接送入 cw_buffers[task->plane][0]
第2步: 05h-col(528)-E0h → DATA_OUT(528B, CW1) → 直接送入 cw_buffers[task->plane][1]
第3步: 05h-col(1056)-E0h → DATA_OUT(528B, CW2) → 直接送入 cw_buffers[task->plane][2]
第4步: 05h-col(1584)-E0h → DATA_OUT(528B, CW3) → 直接送入 cw_buffers[task->plane][3]
```

每次 DATA_OUT 只有 528 字节（一个 CW），`req_read_page` 将 `data_length` 设为 `PAGE_SIZE / CW_NUM`，poll 中的 chaining 循环自动将后续 CW 的 operation 改为 `OP_RANDOM_DATA_READ`（05h-E0h），逐个 CW 读取到各自 buffer。

NORMAL_PROGRAM_PAGE 虽然也是单页操作，但 NAND 不支持 80h 后逐个 CW 写入再确认——必须一次送完整页数据再发 10h。因此走中转模式。

##### 中转模式（Page Buffer Bounce）

| 操作 | 原因 | 中转数据量 |
|------|------|-----------|
| NORMAL_PROGRAM_PAGE | NAND 编程必须连续输入整页 | 2112 → page_buffer |
| PROGRAM_PAGE_CACHE | 同上 | 2112 → page_buffer |
| PROGRAM_PAGE_MULTI_PLANE | 双 Plane 数据需拼接后两次 DATA_IN | 4224 → page_buffer |
| READ_PAGE_CACHE_SEQ/RANDOM/LAST | 31h/3Fh 一次性输出整页，无中途 05h-E0h 机会 | 2112 → page_buffer → 拆分回 CW |
| READ_PAGE_MULTI_PLANE_WITH_DATA_OUT | 双页连续输出 4224 字节 | 4224 → page_buffer → 拆分回 Plane/CW |

##### 对比

| | 直写模式 | 中转模式 |
|----|---------|---------|
| 拷贝次数 | 0 次额外拷贝 | 1 次 memcpy（读写各 1 次） |
| 适用场景 | 单个 CW 可独立寻址的操作 | 必须连续传输整页/多页的操作 |
| 实现方式 | poll chaining 循环 + RANDOM_DATA_READ | req_xx handler → page_buffer → 拆分 |
| 例 | NORMAL_READ_PAGE_WITH_DATA_OUT | 所有 Program、Cache Read、Multi-Plane |

#### 17.9 文件变更

| 文件 | 说明 |
|------|------|
| `include/nand/nand_config.h` | 新增 `NAND_PLANE_MAX`、`CW_TOTAL_MAX`、`PAGE_BUFFER_SIZE` |
| `include/nand/nand_engine.h` | `nand_cmd_req_s.row` → `row[NAND_PLANE_MAX]` |
| `include/nand/nand_fc_interface.h` | `nand_fc_request_t.row` → `row[NAND_PLANE_MAX]` |
| `include/nand/onfi_1_0.h` | 新增 5 个 Multi-Plane OP 码 + `NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH`；拆读 OP 码为 WITH/WITHOUT_DATA_OUT 双版本；`OP_COUNT` 从 32 → 35 |
| `src/nand/onfi_1_0.c` | 新增 5 组 uop 序列 + 映射表 5 项 |
| `src/nand/nand_fmc.c` | ROW_0/1/2 handler 通过 `uop->operand` 索引 `row[]` |
| `src/nand/nand_mt29f4g08abadawp.c` | 默认读接口改为 `WITH_DATA_OUT` 版本 |
| `include/nand/nand_dispatch.h` | `cws[CW_NUM]` → `cws[PLANE_NUM][CW_NUM]`；`cw_status[4]` → `[CW_TOTAL_MAX]`；新增 `plane_count`（后续修订另加 `plane`，见 §17.1 注） |
| `src/nand/nand_dispatch_internal.h` | `cw_paa/buffers[CW_NUM]` → `[PLANE_NUM][CW_NUM]`；新增 `plane_count`（后续修订另加 `plane`） |
| `src/nand/nand_dispatch.c` | `page_buffer` 尺寸扩展；`nand_dispatch_task_load()` 适配双 plane 拷贝；`completion_init` 适配 `CW_TOTAL_MAX`；CW chaining 条件增加 `WITHOUT_DATA_OUT` |
| `src/nand/nand_dispatch_tables.c` | 新增 5 个 req handler + completion handler + 两张函数指针表各加 5 项 |
| `src/fc/fc.c` | `cws[plane][cw]` 适配新维度 |

#### 流程图：Stripe RAID 写提交（图 12，raid=ENCODE 的 page write）

`fc_stripe_submit()`（PLANE_NUM=2：单数据命令 + 单 parity 命令）：

```text
FC 拿到 raid=ENCODE 的 page write 命令（config.raid=2）
  │
  ├─ ① fc_command_alloc_internal → parity 命令
  │      配置：op=OP_PROGRAM_PAGE_MULTI_PLANE, raid=NORMAL
  │      PAA：同 page/block/lun/cw，plane = PLANE_NUM-1
  │
  ├─ ② 循环 fc_command_configure_descriptor(parity_cmd, plane=PLANE_NUM-1, cw=0..config.cw)
  │      分配 parity desc 链 + buffer（v13：FC 内不再有掩码枚举；v17：改调公共入口）
  │
  ├─ ③ parity desc.meta ← 数据命令对应 CW 的 meta
  │
  ├─ ④ fc_data_path_generate_stripe_parity(data_ptrs, 1, parity)
  │      逐 CW：parity_buf = XOR(各数据命令该 CW 的 [0:520])
  │      先 memcpy 作初值，再按 32-bit 字异或
  │      然后 ECC 编码 parity buffer（[520:528] 由 ECC 生成）
  │
  ├─ ⑤ 构建 Multi-Plane dispatch request：
  │      op=OP_PROGRAM_PAGE_MULTI_PLANE, plane_count=2
  │      cws[0][cw] = 数据命令 desc（PAA+buffer 基址）
  │      cws[1][cw] = parity 命令 desc
  │      command_id = 数据命令 id（完成路由回数据命令）
  │
  ├─ ⑥ controller->stripe_parity_for_data[data_id] = parity_id
  │
  └─ ⑦ nand_dispatch_submit(mp_req)
           │
           ▼  Multi-Plane PROGRAM uop 序列：
           80h+地址+Din(P0) → 11h(tADL) → 80h+地址+Din(P1) → 10h → tWB
           → tPROG → 70h 轮询 → CHECK_FAIL
           （双 plane 数据在 page_buffer 拼接：P0@[0..2111]、P1@[2112..4223]）
           │
           ▼
        完成 → fc_complete_from_nand 识别数据命令
              → fc_stripe_release_parity(parity_id)  ← 自动回收 parity 资源
              FTL 只感知数据命令的 completion
```

失败路径：任何一步失败 → `fc_stripe_release_parity()` 回滚已分配资源 + 数据命令以 error completion 返回。

#### 17.10 设计演进


| 迭代 | 问题 | 方案 |
|------|------|------|
| v1 | 单 plane 数组 `cw_paa[CW_NUM]` 不能支持 multi-plane | 展平为 `cw_paa[CW_TOTAL_MAX]` |
| v2 | 展平后丢失 plane 语义，`cw_count` 意义模糊 | 加 plane 维度 `cw_paa[PLANE_NUM][CW_NUM]` |
| v3 | ROW_0/1/2 handler 固定读 `request->row`，多 plane 发第二组地址需新 uop | `row`→`row[NAND_PLANE_MAX]`，operand 选索引 |
| v4 | 读命令只有一种：30h 后必出数据 | 拆为 WITH/WITHOUT_DATA_OUT 双版本 |
| v5 | 第一维单 plane 恒 `[0]`，名不副实（"槽位"vs"plane 索引"），单 plane 打 plane 1 时依赖无断言守卫的隐式约定 | 第一维统一为物理 plane 索引：新增 `plane` 字段贯穿 request→task，单 plane 命令落位 `cws[plane][*]`；`plane_count` 语义不变 |
| v6 | 读命令只能读"前 N 个 CW"（`config.cw` 是"CW 数减一"），表达不了 `{0,1,3}` 这种子集，读恢复只能重读整页 | 加 CW 掩码：建链按掩码置位逐 CW 建 desc；**链槽位与 CW 号解耦**（CW 号一律 `paa_to_cw(desc->paa)`）；读路径统一为「加载进页缓冲 + 逐 CW 取数」，新增 `OP_RANDOM_DATA_READ_MULTI_PLANE`；完成项暴露失败掩码。详见 §25 |
| v9 | 掩码是 flat 的，但掩码下游仍是"plane 分桶 + 每 plane 密排槽位"的第二套坐标（`cw_count[]` / `cws[plane][slot]` / `slot_to_plane_cw()` 走减），两套坐标必须靠"链序 == plane-major"的隐式不变量对齐，且空 plane 需回填槽位 0 的 PAA 才不误寻址 | **索引空间统一为 flat 绝对 CW 号 `n = plane*CW_NUM + cw`**：`cw_mask` / `cw_buffers[n]` / `cw_status[n]` 同空间，`cw_count[]`、`cws[][]`、`cw_paa[][]`、`slot_to_plane_cw()` 全删；行地址拆到 `plane_paa[PLANE_NUM]`（per-plane，与掩码解耦）。详见 §25.11 |
| v7 | v6 的 `cw_mask : 4` 是"CW 号掩码**广播到每个 plane**"，多 plane 读恢复只能重读失败并集；且 GET_FEATURES 把 NAND 读缓冲指向命令自身、结果原地回填进 SQ | ① 掩码改 **flat 绝对编号**（`fc_param_u.cw_mask`，bit `n = plane*CW_NUM + cw`，覆盖 8 位），两 plane 子集可不同，回填即最小重读集；`cw_count` 标量 → `cw_count[PLANE_NUM]`，链长度一律以 desc 链为真值，`config.cw` 不再回写。② CQ 重做为"命令级聚合状态 + 命令级返回段（`fc_param_u`）"：删 `ecc_bit0..3` / `failed_cw_mask`，GET_FEATURES 值经控制器级 `g_fc_return_staging` 中转进 CQ，SQ 提交后不被写。详见 §25 |

### 4.6 多 LUN 并发与 FMC 后端（Phase V / Phase Z）


| 层面 | 多通道支持程度 | 能否验证 |
|------|--------------|---------|
| FC 填充轮次（`fc_poll_fill`） | ✅ 代码完整，改 `CH_NUM` 即可（"忙"查 `nand_dispatch_lun_free`） | ❌ 单通道硬件 |
| dispatch per-LUN FIFO | ✅ 代码完整 | ❌ 单通道硬件 |
| dispatch poll 遍历 | ✅ 代码完整 | ❌ 单通道硬件 |
| FMC 后端 | ✅ 单例共享（`g_nand_fmc_instance`，窗口/时序与坐标无关） | ❌ 多 bank 需重构（单 NAND） |
| RB 中断 | 已移除（单 EXTI6 无法扩展到多 LUN） | — |
| DMA | ❌ 单对 Stream | ❌ 无法 |
| 引擎状态机 | ⚠️ 无通道 id，靠 engine.backend 透传 | ⚠️ 单通道可运行 |

**结论**：调度层和 dispatch 层的多通道架构是真实的。FMC/NAND 引擎层需要改造才能真正支持多通道。

#### 18.7 多 LUN 并发：OP 码 ISSUE/WAIT/FETCH 拆分设计

##### 18.7.1 问题

当前所有 OP 码是原子序列——从 70h 轮询到命令发出到等待完成到数据输出，一条龙锁住 LUN：

```
OP_NORMAL_READ_PAGE_WITH_DATA_OUT:
  70h轮询 → 00h-addr-30h → tR(WAIT_READY) → DATA_OUT → 70h轮询 → CHECK_FAIL → END
  ├─────────────── LUN 独占整个序列 ────────────────┤
```

多 LUN 场景下，LUN 0 在 tR 期间（25µs），dispatch 只能干等，不能趁隙切到 LUN 1 发命令。

##### 18.7.2 企业做法：三阶段拆分

企业级主控将每个 NAND 操作拆为三段独立原语，调度器可交叉执行：

| 原语 | 描述 | 阻塞？ | 典型耗时 |
|------|------|--------|---------|
| **ISSUE** | 发命令 + 地址（或 + 数据），不等完成 | ❌ µs 级 | ~1-5µs |
| **WAIT** | 等 R/B# 或 70h 状态轮询到 ready | ✅ 阻塞 | tR 25µs / tPROG 200µs |
| **FETCH** | 读数据或检查最终状态 | ✅ 阻塞 | ~10µs（读整页）|

例如双 LUN 并发读取：

```
时间 ──────────────────────────────────────────────────────▶

LUN 0: ISSUE(00h-addr-30h) → [tR 25µs]
                              ↑ 总线空闲，可切 LUN
LUN 1:                       ISSUE(00h-addr-30h) → [tR 25µs]
                                                   ↑ 两 LUN 都 busy
LUN 0:                                               ← FETCH(Dout)
LUN 1:                                                   ← FETCH(Dout)
```

总耗时 ≈ tR(25µs) + FETCH(10µs)，而不是 2 × (tR+FETCH) = 70µs。

##### 18.7.3 现有雏形

当前架构已有部分拆分迹象：

| OP 码 | 接近的企业原语 | 差距 |
|-------|--------------|------|
| `OP_READ_PAGE_CACHE_START_SEQ` | READ_ISSUE | 含 30h+tR+31h+DATA_OUT，末尾还有 70h 轮询，没做到"发完立即返回" |
| `OP_READ_PAGE_WITHOUT_DATA_OUT` | READ_ISSUE | 同上，含 tR + 70h 轮询 |
| `OP_READ_PAGE_CACHE_SEQ` | READ_FETCH | 结合了 WAIT + FETCH，没拆分 |
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | 未拆分 | 整条锁住的原子序列 |

##### 18.7.4 拆分方案

每个读/写/擦操作拆为两个 OP 码：

| 原 READ OP | 拆为 ISSUE | 拆为 WAIT |
|-----------|-----------|----------------|
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | `OP_READ_PAGE_LOAD` | `OP_READ_PAGE_WAIT` |
| `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | `OP_READ_PAGE_MULTI_PLANE_LOAD` | `OP_READ_PAGE_WAIT` |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | —（LUN=1 原子路径，tR 等待含在 op 内） | —（取数由 CW 链的 RANDOM_DATA_READ 完成）|
| `OP_NORMAL_PROGRAM_PAGE` | `OP_PROGRAM_PAGE_ISSUE` | `OP_PROGRAM_PAGE_WAIT` |
| `OP_ERASE_BLOCK` | `OP_ERASE_BLOCK_ISSUE` | `OP_ERASE_BLOCK_WAIT` |

ISSUE 型 OP 的 UOP 序列只包含"发命令+地址+数据+CONFIRM"，**不含 WAIT_READY 和 70h 轮询**，执行完立即返回（µs 级），不阻塞 LUN。

读的 WAIT 型 OP 只包含 WAIT_READY + 70h 轮询 + CHECK_FAIL；页数据留在 NAND 页缓冲，
由 `nand_dispatch_task_cw_chain` 逐 CW 用 05h（单 plane）/ 06h（多 plane）取回。
程序/擦除的 WAIT 同理但不取数。

##### 18.7.5 调度器层面影响

| 层面 | 改动 |
|------|------|
| dispatch lun_step | 执行完 ISSUE 后不报完成，切 ISSUED 相位，继续其他 LUN |
| dispatch lun_step | ISSUED 到点后走配对 WAIT/FETCH，完成才出队 |
| fc_poll_fill | 按 LUN room 填充（v8：无 ISSUE/Wait/FETCH 类型识别，靠相位机内部处理） |
| per-LUN FIFO | 同 LUN 的命令严格先进先出，链 = 连续槽，中间不可被其他命令插入 |

##### 18.7.6 当前是否支持并发（→ Phase V 已实现）

| 层面 | 支持程度 | 原因 |
|------|---------|------|
| dispatch 数据结构 | ✅ 就绪 | `g_nand_dispatch_queues[CH][CE][LUN]` 多维（v8 per-LUN FIFO） |
| FMC 硬件总线 | ⚠️ 命令级可切 | 发完命令释放总线，WAIT_READY 卡住（硬件限制） |
| **OP 码层** | ✅ Phase V | 新增 6 个 ISSUE/WAIT/FETCH OP 码 + uop 序列 |
| **调度器层** | ✅ Phase V | poll phase 机（ISSUE→ISSUED→FETCH）+ LUN 状态切换 |
| **FMC 后端** | ⚠️ 单例 | Phase V 曾数组化，Phase Z（22707cb）撤销回单例 `g_nand_fmc_instance`；多通道需 per-bank base 表 |

#### 流程图：LUN 相位机（图 10，Phase V 多 LUN 并发）

```text
        submit 追加槽 / lun_step 完成一条后复位
        （ISSUE 型命令）
                │
                ▼
        ┌───────────────┐
        │   LUN_PHASE_IDLE    │  队头槽待启动
        └───────┬───────────┘
                │ started==0 → nand_fc_start（ISSUE uop）
                ▼
        ┌───────────────┐
        │ LUN_PHASE_ISSUE    │  发完命令+地址(+数据)+CONFIRM，立即返回
        └───────┬───────────┘  （µs 级，不阻塞总线）
                │ step 完成 → task_complete → 记录 SysTick µs 起点
                ▼
        ┌───────────────┐
        │ LUN_PHASE_ISSUED   │  LUN 内部操作中（tR 25µs / tPROG 200µs /
        └───────┬───────────┘   tBERS ~1ms）。此时 dispatch 可切其他 LUN！
                │ min_busy_us 到点（SysTick µs elapsed）→ pair_op 切换
                ▼
        ┌───────────────┐
        │ LUN_PHASE_FETCH    │  执行配对 WAIT/FETCH：WAIT_READY + DATA_OUT
        └───────┬───────────┘   + 70h 轮询 + CHECK_FAIL
                │ step 完成 → task_complete → 本条命令完成
                ▼
            出队（head++/count--）+ phase=IDLE → 返回 completion
            （FIFO 里还有槽 → 下一轮 lun_step 处理下一条）
```

并发收益示例（双 LUN）：

```text
时间 ──────────────────────────────────────────────────────▶
LUN 0: ISSUE(00h-addr-30h) → [tR 25µs]          ← 总线释放，可切 LUN
LUN 1:                       ISSUE(00h-addr-30h) → [tR 25µs]
LUN 0:                                               ← FETCH(Dout)
LUN 1:                                                   ← FETCH(Dout)
总耗时 ≈ tR + FETCH，而非 2×(tR+FETCH)
```

配对 OP 码（`nand_op_map_s.pair_op`）：LOAD↔FETCH、ISSUE↔WAIT；原子 OP 的 pair_op 取自身（不切分）。多 LUN 场景 `fc_build_dispatch_request()` 在填完 request 后自动把 splittable 命令拆成 ISSUE（`nand_operation_is_splittable`），单命令与链式两条路径共用这一处转换。

### 4.7 读饥饿保护（Phase I，v8 删除）

#### 20.1 问题

调度器（`fc_scheduler`）对读写命令一视同仁。写命令（PROGRAM, tPROG≈300μs）或擦除命令（ERASE, tBERS≈1ms）连续占用 LUN 时，后续的读命令可能被长时间阻塞，导致**读饥饿**。

#### 20.2 控制宏

```c
// fc_scheduler.h
#define FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET (3U)
```

连续执行 N 条非读命令后，触发读优先调度。执行一条读命令后计数器清零。

#### 20.3 实现

##### 读操作识别

`fc_scheduler_op_is_read()` 识别 16 种读相关操作，非读操作（PROGRAM/ERASE/SET_FEATURES 等）受饥饿保护约束。

##### select_chain() SQ 遍历逻辑

```
① 前置扫描：count ≥ 3 时扫描 SQ 是否存在读命令（sq_has_read）
② 旋转拦截：count ≥ 3 且队头为非读且 sq_has_read=1 → 旋转到队尾
③ 执行：弹出链头命令，更新 count（读→0，非读→++）
```

##### 死锁防护

`sq_has_read` 前置扫描确保**全写队列**场景下不触发保护。如果 SQ 内完全没有读命令，写命令正常执行，不会被无限旋转。

#### 20.4 队列操作说明

SQ 是环形 FIFO，旋转通过 `pop(队头) + push(队尾)` 实现。只动 `head/tail` 指针，Command Pool 内容不变。旋转不是丢弃——被旋转的命令会在队列循环一圈后回到队头执行。

#### 20.5 场景验证

| 场景 | 行为 |
|------|------|
| 全写队列 [W,W,W,W] | sq_has_read=0 → 不触发 → 全部正常执行 |
| 读写混合，写 ≤ 3 | count < 3 → 不触发 |
| 写 ≥ 3 + 队列有读 | sq_has_read=1 → 后续写旋转 → 读优先执行 → count 归零 |

#### 20.6 当前局限性

- 仅在 SQ 层面生效。HSQ 为高优先级通道，跳过读饥饿保护。
- 命令链（chain=1）中无法插队读命令（链原子性），但链结束后下一轮 select 即可切换。
- `CH_NUM=1`、`LUN_NUM=1` 时读饥饿问题不明显，本设计主要为多 LUN/多通道架构预留。
- `CH_NUM=1`、`LUN_NUM=1` 时读饥饿问题不明显，本设计主要为多 LUN/多通道架构预留。

### 4.8 命令链口径演变（v8 / v52 / 2026-10-30）

> **口径更新（2026-10-30）**：标题与下面三条**保留为历史口径**（其余命令仍恒 `END`）；
> **cache read / cache program 的 run 现在会组链**——见本节末尾「v-streaming 更新」。

**这是最容易被 §16 与图 11 误导的一点。** §16 / 图 11 用大量篇幅讲"命令链执行"（per-LUN chain buffer、`submit_chain`、原子入队），但：

- FTL 运行时**所有**命令都设 `config.chain = FC_CHAIN_END`（`src/ftl/` 下共 **23 处**，含 `ftl.c` / `metadata.c` / `checkpoint.c` / `gc.c` / `journal.c` / `bbt.c` / `pfr` 等）。因此调度器**永不组链**，`count` 恒为 1。
- 于是运行时**只有 `fc_poll_dispatch_single` 是活路径**；`fc_poll_dispatch_chain` / `nand_dispatch_submit_chain` 仅由单元测试覆盖。
- Read Retry（`src/ftl/read_recovery.c`）**不是**原子 `submit_chain`，而是**三条独立提交 + 同步等待**的命令（各自 `fc_submit` + `ftl_sync_submit_wait`）。

结论：把图 11 的链收集分支理解为"**能力存在但运行时未启用**"，主干是单命令路径（图 17/图 19）。链式执行的设计与验证保留在 §16，供单测与未来扩展参考。

> 🔁 **v8 更新（2026-10-06，per-LUN FIFO）**：上面的结论不变（FTL 仍恒 `FC_CHAIN_END`，运行时仍走单命令路径），但**单命令路径本身换了模型**：
> - 调度不再是 FC 侧 busy 表 + `fc_scheduler_select`，而是 `fc_poll_fill()` 一次扫描 HSQ/SQ，按 LUN 保序把能放下的命令（含链）提交进 NAND 的 per-LUN FIFO（深 4）；
> - NAND 侧不再有 per-channel FIFO / Phase 1 / Phase 2，`nand_dispatch_poll()` 遍历各 LUN FIFO、每 LUN 每轮推进一条、完成一条回一份 completion；
> - "忙"由 `nand_dispatch_lun_free()` 表达，`NOT_READY` 不再被 FC 当失败标 `NAND_FAIL`；
> - `fc_scheduler_set_busy` / `clear_busy` / `busy[][][]` / `consecutive_write_count` 均已删除（见 [§24](#24-fc-层断言判据assert--assume) 判据 4 的历史条目）。
>
> 图 9（§4.3）已重绘为遍历模型；图 11 的链收集分支语义改为"连续槽 + 逐条完成"。详见 `docs/archive/changelog/fc_nand_per_lun_fifo.md`。

> 🔁 **v52 更新（2026-10-11，链锁）**：结论仍不变（FTL 恒 `FC_CHAIN_END`，运行时 `chain_owner` 恒 NONE），但补上了一处**与"运行时不走链"无关**的真实缺口——**链长 > 4 时的跨批原子性**。链被拆成多批、分多次填充轮次下发，v51 及以前批与批之间无锁，另一条提交队列（HSQ/SQ）的命令可插在批边界上。v52 在 `fc_sched_s` 加了一张跨轮次持久的 per-LUN 链锁 `chain_owner[ch][ce][lun]`：链一旦开始，该 LUN 的填充权归链所在队列独占，直到链 END 下发（规则见 §2.7、流程图见 §23.7）。**这是「链能力」本身的补强，不改变运行时主干**——FTL 不组链 ⇒ 锁恒 NONE ⇒ 生产路径逐字不变。由 Level 3 用例 `chain_priority_lock` 覆盖（8 命令 SQ 链 + 同 LUN HSQ 命令，判据 = 终值）。详见 `docs/archive/changelog/fc_chain_priority_lock_v52.md`。

> 🔁 **v-streaming 更新（2026-10-30，P1 读流式，未上板）**：上面的「FTL 恒 `FC_CHAIN_END`
> ⇒ 运行时 `chain_owner` 恒 NONE」**不再成立**。FTL cache read / cache program 把一条 run
> 标成一条链：窗口末条在断点是 `FC_CHAIN_END`、否则 `FC_CHAIN_MORE`（允许跨填充轮次、
> 读序列还允许跨 USB chunk 续链），`ftl_op_submit_batch()` 原子入队，序列打开期间
> `ftl_op_submit*` 门控阻止同 LUN 的其他提交插入。运行时因此**会**走
> `fc_poll_dispatch_chain`，`chain_owner` 的跨轮次锁是这条路径的正确性前提。其余命令仍恒
> `END`。**未上板**：需要命令 trace 与中止/恢复负例（`known_risks.md` RISK-IF-07/09/10；
> 设计见 `docs/design/ftl_design.md` §3.16，实现见 `src/ftl/cache_batch.c`）。

### 4.9 加扰器：LFSR 抽头与种子（v24 / v38）


- **行业**：DMA 引擎内硬件 inline 加扰/解扰（企业级强制 AES-XTS 加密后数据天然随机）。
- **硬件限制**：无。
- **无奈选择**：软件 LFSR（x¹⁶+x¹⁴+x¹³+x¹¹+1，周期 65535）/ XOR-shift / 简单 XOR，编码前 CPU 逐字节异或。
- **代价**：占用 CPU 周期；无 AES 加密（消费/企业级数据安全要求未覆盖）。

**v24 的两处修正**（都在 `src/fc/fc_random_engine.c`）：

1. **抽头错误导致状态空间坍缩**。原实现是右移 LFSR（输出 bit0、反馈进 bit15），却照抄了左移写法的抽头
   `bit15^bit13^bit12^bit10`——新状态的 16 位**全部只依赖旧 bit1..bit15**，被移出的 bit0 没有反馈回去，
   映射不可逆（`1 → 0` 是吸收态），状态空间坍缩到**周期 63**。后果：520 字节的 CW = 63 字节图案重复
   8.25 遍，写 `0101…` 进去出来仍是周期 63 字节的图案，加扰等于没做。正确抽头是多项式的**镜像**
   `{0,2,3,5}`（`fb = bit0 ^ bit2 ^ bit3 ^ bit5`）。
2. **种子从物理地址改为逻辑地址**。原 `fc_random_seed_from_paa(desc->paa, 0U)` 用物理地址当种子：
   ① GC 搬页后物理地址变 ⇒ 密文变，正是业界要避免的（业界做法是**种子 = 被写页的逻辑地址**，
   搬页后逻辑地址不变 ⇒ 不需要"解扰再重加扰"）；② 第二个形参 `program_sequence` 恒传 `0U`，该维度从未接上。
   现改为 `fc_random_seed_from_meta(desc->meta)`（本 CW 的扇区 LBA），并**删除 `FC_RANDOM_SEED_STRATEGY`
   三选一旋钮**（种子来源只剩这一种，不再需要策略开关）。

**为什么必须同时把 META 移出加扰范围**：种子既然取自 META，读路径就需要 META 才能解扰——但 GC 与掉电重建
**不知道** LBA，只能从页内 META 读回来。若 META 仍在加扰区内，就构成"要 META 才能解扰 / 要解扰才能拿 META"
的循环依赖；强行改种子会让 GC 解出垃圾 LBA → L2P 查不到 → 源块有效页被逐个标记无效却从未搬迁 →
静默数据丢失。故加扰范围由 `[0,520)` 收窄到 `[8,520)`（`FC_CRC_USER_OFFSET` 起、`FC_BUFFER_USER_SIZE` 长），
**META 与 CRC 保持明文**，读路径 步骤 2 直接读明文 META 自举种子。

> 加扰粒度 = **被寻址逻辑单元**。本工程一个 CW（512 B 用户数据）= 一个扇区 = 一个逻辑单元，故每条 CW 用
> 自己那条扇区 LBA 当种子；这正是业界"种子 = 被寻址逻辑单元的地址"（一个逻辑块由若干 512 B–2 kB 的
> ECC 码字组成，块内码字共用该逻辑块的种子）。详见 `docs/archive/changelog/fc_scrambler_lba_seed_v24.md`。

**副作用（v38 记录）**：明文 META + 种子耦合还带来一个**隐式**的保护——**META 损坏会传染成 CRC 失败**
（种子错 ⇒ USER 解成垃圾 ⇒ 步骤 3 不过）。这是「期望值未知」路径（GC / 掉电重建）上 META 的**唯一动态保护**，
但它没写进任何契约，且依赖 `random` 开启。详见 §5.3 的「四条读取路径 META 保护矩阵」。

### 4.10 时序模式（2026-10-05 / 2026-10-20）


3.3V 的 MT29F4G08ABADAWP 数据手册 **Table 29/31 明确按 ONFI timing mode 5 给出 AC 参数**
（Table 29 Note 1: "Operating mode timings meet ONFI timing mode 5 parameters"），
且 Mode 5 为 3.3V-only（Table 15 Note 3）。

⚠ 电压域与模式对应（订正）：
- **Table 32（1.8V）Note 1 = Mode 4**；
- **Table 29/31（3.3V）Note 1 = Mode 5**；
- Table 15 Note 2 只表示 **Mode 4 特性**在 1.8V/3.3V 都支持，具体 AC 仍按电压域选表。

2026-10-20 的失败不是“Mode 4 不支持 3.3V”，而是把 **1.8V 口径的 fast 表**用在了 3.3V 上，
外加旧 PCC 公式少算周期；该表已删除。本板是 3.3V，故统一采用 Table 29/31 + Mode 5。

因此本仓改为：**启动全程 Mode 0 低速，FTL boot/恢复完成后切 Mode 5**，FMC 时序按
Table 29/31 的 3.3V 参数重算。

### 4.11 文件变更与验证快照（v6 / v7 / v9）

以下逐文件表记录 v6/v7/v11/v12/v17 当时改了哪些文件，用于追溯**当时的实现形态**；
表中标识符多半已被后续版本删除，不代表现行代码。

| 文件 | 改动 |
|------|------|
| `include/fc/fc_command.h` | v6：`fc_cmd_s` 加 `cw_mask:4`；`fc_cmd_status_s` 加 `failed_cw_mask:4`。**v7**：删 `cw_mask:4` / `parameter0..3` → `fc_param_u param`；删 CQ 的 `ecc_bit0..3` / `failed_cw_mask` → `fc_param_u param`；新增 `fc_cw_mask_is_plane_symmetric()` |
| `src/fc/fc.c` | v6：新增 `fc_cw_mask_popcount`；建链按 mask 枚举 + `config.cw` 回写；desc 配置签名 `index`→`(plane, cw)`；读 op 转换；`fc_complete_from_nand` 按 CW 号折叠 + 填 `failed_cw_mask`。**v7**：建链掩码改 flat + 逐 plane 展开 + 不回写；`fc_command_descriptor_count` 改走链；新增 `g_fc_return_staging` + `fc_get_cmd_ecc_max` 调用；`fc_build_dispatch_request` 每 plane 填槽位 0 + 删 `request->parameter[]`；`fc_stripe_submit` 加对称性 ASSERT + `cw_count[p]`。**v11**：新增 `fc_command_plane_span` / `fc_command_cw_attached` / `fc_command_chain_append` / `fc_command_attach_cw_internal`；`fc_command_configure_descriptors_internal` 内层循环改调机制原语（删局部 `previous` 与内联串链）。**v12**：`fc_command_configure_descriptor_internal` / `fc_command_attach_cw_internal` 增 `row_paa` 形参；`fc_build_request_data_planes` 的 `plane_paa[]` 改为从 desc 链推导 + 同 plane 行地址一致性 ASSERT。**v17**：删 `fc_command_configure_descriptor_internal` / `fc_command_attach_cw_internal` / `fc_command_cw_attached`（均搬去 `fc_interface.c` 并入 `fc_command_configure_descriptor`）；`fc_descriptor_alloc_internal` / `fc_command_chain_append` 去 `static` 并进 `fc_internal.h`；RAID parity 建链改调公共入口 |
| `src/fc/fc_data_path.c` | v6：Pass 4 META 校验改用 `paa_to_cw(desc->paa)`。**v7**：`fc_data_path_process_read` 链长走链；stripe parity 加对称性 ASSERT |
| `src/fc/fc_interface.c` | **v7**：`fc_write_cmd_data` / `fc_read_cmd_data` / `fc_get_cmd_data_ptr` / `fc_buffer_get_meta` 循环界改走链；`fc_get_cmd_data_ptr` 按 CW 号匹配；新增 `fc_get_cmd_ecc_max()`。**v11**：新增公共入口 `fc_command_attach_cw()`（薄壳：身份 + `ALLOCATED` 校验 → 转 internal）。**v12**：薄壳签名同步增 `row_paa`。**v17**：建链三层合并——`fc_command_attach_cw` 与两个 `_internal` 收敛为 `fc_command_configure_descriptor`（机制体落此文件，校验前置于出池）；新增 static `fc_command_cw_attached` |
| `src/fc/fc_command.c` | **v7**：新增 `fc_cw_mask_is_plane_symmetric()`；`fc_command_validate` 加掩码校验；`fc_command_status_init` 删 ecc 初始化 |
| `src/fc/fc_scheduler.c` | v6：`fc_scheduler_op_is_read` 补 `OP_RANDOM_DATA_READ_MULTI_PLANE` |
| `include/nand/onfi_1_0.h` | v6：新增 `OP_RANDOM_DATA_READ_MULTI_PLANE (41U)`；`OP_COUNT` → 42 |
| `src/nand/onfi_1_0.c` | v6：新增 `g_nand_random_read_multi_plane_uops[]` + op 表项 |
| `include/nand/nand_dispatch.h` | v6：`cws` 加 plane 维。**v7**：`cw_count` → `cw_count[PLANE_NUM]`；删 `parameter[4]` |
| `src/nand/nand_dispatch_internal.h` | **v7**：`cw_count` → `cw_count[PLANE_NUM]`；删 `parameter[4]`；`cw_index` 注释更新 |
| `src/nand/nand_dispatch.c` | v6：新增 `is_load_op` / `is_fetch_op`；重写 `cw_chain`；scatter 偏移改用 `paa_to_cw`。**v7**：新增 `nand_dispatch_task_total_slots` / `nand_dispatch_slot_to_plane_cw`；`task_load` 每 plane 拷 + 槽位 0 无条件；删 `task->parameter[]`；scatter 界改每 plane |
| `src/nand/nand_dispatch_tables.c` | **v7**：所有 `task->cw_count` 界改 `cw_count[plane]`；`completion_multi_plane` 改 plane-major 链走位 |
| `src/ftl/ftl.c` / `src/ftl/gc.c` | **v7**：`completion->ecc_bit0..3` max 循环 → `fc_get_cmd_ecc_max(cmd)` |
| `src/ftl/read_recovery.c` | **v7**：`cmd->parameter0..3` → `cmd->param.parameter0..3` |
| `tests/unit/test_fc_level1/2/3.c`、`tests/integration/test_fc_nand.c` | **v7**：字段引用机械适配；`fc_nand_test_wait_completion_ex` 回传 CQ；补 flat 语义用例 |
| `docs/archive/fc_api_usage.md`、当时的设计文档 §2 / §13 / §14 / §25 | **v7**：flat 掩码 + CQ 返回项同步 |

> **v9**（索引空间统一）的逐文件改动见 §25.11——它把上表里 v6/v7 的 `cws[plane][slot]` /
> `cw_count[PLANE_NUM]` / `cw_paa[][]` 全部删除，上表对应行只作历史记录。
> **v27**（`cw_mask` → `cw_bitmap`）的逐项改动见 §25.4 v27——上表里的 `cw_mask` / `fc_cw_mask_is_plane_symmetric`
> / `fc_cw_mask_popcount` 等标识符均为 v6/v7 当时名，只作历史记录。

#### 25.10 验证状态

- v6：`make format` + `make debug` / `make tests` / `make release` 三档**零警告**通过（153412 / — / 87660 字节）。
- v7：见 `docs/archive/changelog/fc_flat_cw_mask_cq_param.md` 的验证记录。
- v9：见 `docs/archive/changelog/fc_cw_index_unification.md` 的验证记录。
- v27：仅 `make format-check` 通过（101 文件）。**未编译**——本轮处于测试代码重构期（Level 2/3 尚在
  引用旧夹具符号），编译验证留到调用点迁移完成之后。
- **上板未验证**（按平台规则由用户执行）。回归护栏：现有全部读用例必须通过。
- **新语义用例**：v7 补齐 flat 掩码三类用例（`0x29` 混合 / `0xF0` 只选 plane1 / `mask==0 + multi_plane=2` 建 8 desc）。
- **datasheet 复核未做**（风险 2）。

---

## 5. 被否方案与已删除设计

重新提议下列设计前先读对应条目。

| 设计 | 否决/删除原因 | 记录 |
|------|--------------|------|
| **软件寄存器影子** `fc_registers` / `fc_register_queue_state()` | 字段与真实队列重复、11 个刷新点、零读者；队列用量改由 `fc_get_*_count` / `fc_get_resource_stats()` 直接算 | v10，`fc_registers_removal.md` |
| **读饥饿保护**（连续 N 条非读后旋转 SQ） | v8 per-LUN FIFO 重构后填充轮次改为「HSQ 优先 + 按 LUN 保序」，不再做读写计数；当前单 LUN 平台保护近乎无效 | §4.7，`fc_nand_per_lun_fifo.md` |
| **`data_ptr` 裸指针 buffer 方案** | 生命周期无主：上层持有指针跨异步完成，完成时 buffer 可能已归还（UAF）；DRAM-less 主控主流是 ID 池 | §4.1 |
| **per-channel FIFO / Phase 1 / Phase 2 / busy 表** | 每 LUN 一条深 4 FIFO 已足够表达「同 LUN 保序 + 跨 LUN 并发」；busy 表是第二份状态 | v8，`fc_nand_per_lun_fifo.md` |
| **第二套 CW 坐标** `cw_count[]` / `cws[plane][slot]` / `slot_to_plane_cw()` | 与 flat 掩码构成两套坐标，靠「链序 == plane-major」隐式对齐；空 plane 需回填槽位 0 的 PAA 才不误寻址 | v9，§4.4，`fc_cw_index_unification.md` |
| **RB# EXTI 中断** | 单 EXTI6 无法扩展到多 LUN；WAIT_READY 回到纯 GPIO 轮询 | Phase F 后移除 |
| **「6 原语」重构**（把 FC 对外接口压成 6 个硬件原语） | FC 需要策略层之外的状态查询/完成聚合/资源账本；压成 6 原语会把 FTL 概念塞回 FC | 设计文档 §26.5 |
| **重复 OP 码 20**（`OP_PROGRAM_FOR_INTERNAL_MOVE`） | 与 ONFI 标准命名的 op 10 功能完全重复 | `internal_move_op_merge.md` |
| **「FTL 恒 `FC_CHAIN_END`，运行时不走链」** | 2026-10-30 FTL cache read / cache program 的 run 会组链，`chain_owner` 锁是正确性前提；其余命令仍恒 END | §4.8 |
| **FMC 后端 per-(ch,ce,lun) 数组化** | Phase Z（`22707cb`）判为「假的 per-LUN config 分化」，撤销回单例；后续多 target 泛化改为 per-(ch,ce) 实例 | §4.6 |
| **1.8V 口径 fast timing 表** | 3.3V 器件应使用 Table 29/31 + Mode 5；2026-10-20 把 1.8V fast 表用在 3.3V 上并叠加 PCC 少算周期，导致失败；该表删除 | §4.10、设计文档 §19 |
| **独立「所有权链」** `descriptor_owned_next[256]` + `command_descriptor_head[64]` | 所有权真值只需一份：desc 链本身；640 B 专门兜「已出池未挂链」中间态 | v15，§2.1 注记，`fc_chain_ownership_v16.md` |
| **12 B CQ 条目**（`desc_id` / `module` / per-CW 位图） | CQ 是命令级快照，per-CW 细节归 desc 链；`module` 由 FTL request 表按 `cmd_id` 路由；`desc_id` 不参与 CQ | §4.1 及 v7/v28 |
| **ERASE 伪造 CW0 位** | 擦除没有 CW，位图合法空集；completion 空集分支写 `cw_status[0]` 兜住 | v28，`fc_cw_bitmap_param_v28.md` |
| **`fc_command_status_set_cw`** | 零生产调用者，按断言判据 2 整体删除 | 设计文档 §24 |
| **`fc_scheduler_set_busy` / `clear_busy` / `busy[][][]`** | v8 起「忙」由 `fc_nand_dispatch_lun_free()` 表达 | §4.8 |
| **`g_crc_ready` 静默降级** | CRC 自检失败必须停机（`ASSERT`），不能静默让校验恒通过 | v33，`fc_crc_hal_driver_v33.md` |
| **`FC_RANDOM_SEED_STRATEGY` 三选一** | 种子固定为本 CW 逻辑地址（`desc->meta`），读路径可从页内明文 META 自举 | v24，`fc_scrambler_lba_seed_v24.md` |
| **加扰范围 `[0,520)`** | META/CRC 必须保持明文，否则读路径无法自举种子；范围收窄为 `[8,520)` | v24 |
| **PBA 跨进 FC 层** | PBA 与 PAA 位序不同，FC 内 10+ 处现场转换都是静默算错地址的机会；FTL 在边界一次 `pba_to_paa()` | v47，`fc_command_paa_v47.md` |

---

## 6. 旧名 → 现行名对照

旧名只出现在本文与 `docs/archive/`；新代码与现行文档一律用右列。

### 6.1 模块与文件

| 旧 | 现行 |
|----|------|
| `nand_dispatch_*` | `fc_nand_dispatch_*` |
| `nand_dispatch_request_t` | `fc_nand_dispatch_req_s` |
| `nand_dispatch_completion_t` | `fc_nand_dispatch_comp_s` |
| `nand_dispatch_init` | `fc_nand_dispatch_init` |
| `nand_dispatch_set_armed` / `g_nand_dispatch_armed` | `fc_nand_engine_set_armed` / `fc_nand_engine_is_armed` |
| `nand_dispatch_poll` | `fc_nand_dispatch_poll` |
| `nand_dispatch_submit` / `_submit_chain` | `fc_nand_dispatch_submit` / `fc_nand_dispatch_submit_chain` |
| `nand_dispatch_lun_free` / `_lun_used` | 同名，前缀 `fc_` |
| `nand_fc_*`（`start` / `step` / `init` / `enable_high_speed`） | `fc_nand_*` |
| `nand_fc_request_t` / `nand_fc_completion_t` / `nand_fc_transaction_t` | `nand_cmd_req_s` / `nand_cmd_comp_s` / `fc_nand_txn_s` |
| `nand_engine` / `nand_engine_state_t` | `fc_nand_engine` / `fc_uop_engine_state_s`（微码执行器 `fc_uop_engine_*`） |
| `nand_enable_high_speed` | `fc_nand_enable_high_speed(DWORD fmc_hz)` |
| `include/nand/nand_dispatch.h` | `include/fc/fc_nand_dispatch.h` |
| `include/nand/nand_engine.h` / `nand_fc_interface.h` | `include/fc/fc_nand_engine.h` |
| `src/nand/nand_dispatch*.c` / `nand_engine.c` | `src/fc/fc_nand_dispatch.c` / `fc_nand_engine.c` |
| `src/nand/nand_mt29f4g08abadawp.c` | `src/nand/devices/mt29f___g08a.c` |

### 6.2 类型、字段与接口

| 旧 | 现行 | 原因 |
|----|------|------|
| `fc_status_t` | `fc_status_e` | H50 枚举后缀 |
| `fc_buffer_id_to_real_buffer` | `fc_buffer_get_user_data` / `fc_buffer_get_base` | 名字区分 USER 基址与 CW 基址 |
| `FC_CW_STATUS_MATA_ERROR` / `FC_CMD_STATUS_MATA_ERROR` | `FC_CW_STATUS_META_ERROR` / `FC_CMD_STATUS_META_ERROR` | 拼写错误 |
| `fc_param_u.cw_bitmap` | 无 | v28 CW 集合改作建链形参；CQ 失败集由遍历 desc 链得到 |
| `command->config.cw` | `config.cw_count` | 语义是「每-plane CW 数减一」，仅 RAID stripe 读 |
| `fc_cmd_s.module` / `fc_cmd_status_s.desc_id` | 无 | CQ 不携带 op/module；失败 CW 从 desc 链查 |
| `fc_cmd_s.pba` | `fc_cmd_s.paa` | v47 PBA 退出 FC 层 |
| `fc_command_attach_cw` / `_internal` 三层 | `fc_command_configure_descriptor` | v17 建链收敛 |
| `fc_command_configure_descriptors()`（FC 侧） | `ftl_cmd_configure_descriptors()`（FTL 侧） | v13 策略层上移 |
| `fc_write_cmd_data` / `fc_read_cmd_data` | `ftl_cmd_write_data` / `ftl_cmd_read_data` | v13 数据搬运上移 |
| `fc_get_cmd_cw_mask` | `fc_get_cmd_cw_bitmap` | v27 字面位图 |
| `ftl_internal_sync_wait` | `ftl_sync_submit_wait` | 命名规范化 |
| `NAND_UOP_DELAY_US` | `NAND_UOP_DELAY_TIMING` | 延时按 ONFI 时序字段取值，不是固定 µs |
| `status.module` 路由 | FTL request 表按 `cmd_id` 取 `{ctx, op}` | 单一真相 |

### 6.3 Phase / 版本 / 变更记录 / 提交

| 版本 | 变更记录 | 提交（若已知） |
|------|---------|---------------|
| v8 | `fc_nand_per_lun_fifo.md` | — |
| v9 | `fc_cw_index_unification.md` | — |
| v10 | `fc_registers_removal.md` | — |
| v11 | `fc_cw_subset_read.md` | — |
| v12 | `fc_per_plane_row_address.md` | — |
| v13 | `fc_policy_layer_uplift.md` | — |
| v15/v16 | `fc_chain_ownership_v16.md` | — |
| v17 | `fc_build_chain_convergence_v17.md` | — |
| v23 | `ftl_cw_mask_single_knob_v23.md` | — |
| v24 | `fc_scrambler_lba_seed_v24.md` | — |
| v25 | `nand_config_addr_ctor_v25.md` | — |
| v27 | `fc_cw_bitmap_literal_v27.md` | — |
| v28 | `fc_cw_bitmap_param_v28.md` | — |
| v29 | `fc_data_path_step_primitives_v29.md` | — |
| v33 | `fc_crc_hal_driver_v33.md` | — |
| v38 | `fc_ecc_crc_coverage_v38.md` | — |
| v39 | `fc_multiplane_narrow_bitmap_v39.md` | — |
| v47 | `fc_command_paa_v47.md` | — |
| v52 | `fc_chain_priority_lock_v52.md` | — |
| v53 | `fc_buf_own_v53.md` | — |
| Phase V → Phase Z | `fc_structural_changes.md` | `bdb192b`（Phase V 拆分）、`22707cb`（单例化） |
| 多 target 泛化 | — | `2d50244` |

### 6.4 旧图号 → 新图号

设计文档重排后图号变化如下（旧图号只在历史引文中出现）：

| 旧图 | 内容 | 新图 |
|---|---|---|
| 图 0 | 分层架构与数据流总览 | 图 1 |
| 图 1 | 命令生命周期 | 图 2 |
| 图 2 | `fc_service()` 主循环 | 图 3 |
| 图 3 | CW 布局与数据处理范围 | 图 8 |
| 图 4 | 写路径编码 | 图 9 |
| 图 5 | 读路径解码 | 图 10 |
| 图 6 | 微码引擎状态机 | 图 4 |
| 图 7 | 70h 状态轮询 | 图 5 |
| 图 8 | FMC 后端 UOP 执行 | 图 6 |
| 图 9 | dispatch poll 遍历模型 | 图 7 |
| 图 10 | LUN 相位机 | 图 7（状态机）、图 14（时序） |
| 图 11 | 调度器填充轮次 | 图 21 |
| 图 12 | Stripe RAID 写提交 | 无（RAID 未启用，机制见 §5 被否方案） |
| 图 13 | 完成回写与 CQ 组装 | 图 11 |
| 图 14 | 上电初始化分层调用时序 | 图 15 |
| 图 15 | FC 控制面资源装配 | 图 16 |
| 图 16 | NAND 芯片初始化 | 图 17 |
| 图 17 | 命令端到端生命周期 | 图 18 |
| 图 18 | 命令生命周期状态机 | 图 19 |
| 图 19 | `fc_service()` 单步全景 | 图 20 |
| 图 20 | LUN 相位机状态机 | 图 7、图 14 |
| 图 21 | 多 LUN ISSUE/WAIT/FETCH 时序 | 图 14 |
| 图 22 | 填充轮次决策 | 图 21 |

新图 12（链的原子提交）、图 13（multi-plane 页缓冲）此前没有独立图号。

---

## 7. 验证记录索引

验证记录只证明**当时**的结论；后续版本改动后需重新验证。

### 7.1 Phase E 相关

- Phase E 落地时 `OP_COUNT` 32 → 35，新增 5 个 multi-plane OP 码；
  `make tests` 编译与链接通过，**未上板**（当时用例未覆盖 multi-plane 全路径）。
- v9 索引空间统一后：固件 `.text` 变化，`make tests` 通过，**未上板**。

### 7.2 协议与器件扩展

- 2026-10-09：逐字核对 ONFI 1.0（Rev 1.0, 2006-12-28）Table 14，全文 `plane` 出现 0 次；
  multi-plane 全部来自器件侧。同时记录两处待办：on-die ECC Note 4 出处对不上、
  器件有 `TWO-PLANE PROGRAM PAGE CACHE` 而 op 表缺。
- 2026-10-23：新增 op 44–47，`OP_COUNT` 44 → 48；上板 `test /fc-nand/` 56/56（3 skipped），
  `block_cache_ops` / `block_cache_multi_plane` OK。⚠ 只到 NAND/FC + UT，FTL batch 与掉电恢复未接，
  失败注入未验。
- 当前 `OP_COUNT = 51`（后续又补 op 50）；以 `include/nand/onfi_1_0.h` 为准。

### 7.3 修复类

- v33 CRC：修正后介质里的 CRC 字段首次成为真值；**此前写入的 CW CRC 恒为 0**（旧介质不作废，
  但校验口径自该版本起才成立）。
- v24 加扰：抽头修正后 LFSR 周期恢复正常；旧介质若用旧种子/旧抽头写入，读回需按旧参数解扰（未做兼容）。
- v47 PAA：`src/fc/**` 中 `pba_to_paa()` 归零；用例数不变；`make clean && make tests`
  0 warning 0 error。**未上板**（PBA/PAA 位序属编译期无声量，按 R2 应上板）。

### 7.4 运行口径

- 2026-10-30 P1/P2 cache 流式：正常路径已上板；**序列/链路径的负例未上板**。
  已知风险 RISK-IF-07/09/10。
- 后续 HIL / FTL UT 的上板结论见 `docs/design/hil_ut_design.md`、`docs/design/ftl_ut.md`
  与 `docs/design/known_risks.md`，本文不复制。

---

## 8. 维护规则

1. **当前设计进 `fc_nand_design.md`，演进只进本文**（H37）。设计文档不写版本号、日期、
   「原先是…」「已删除…」。
2. 新增版本条目按 §3 追加：版本、日期、内容、决策、拒绝方案、验证状态。
   没有独立变更记录的版本，写清「无 changelog」。
3. 方案反转必须写「推翻了哪一条」，不能只写新方案——否则旧条目读起来仍像现行设计。
4. 被否方案先查 §5；同一条目重复出现时更新原条目，不新开一条。
5. 验证结论按当时事实记录；后续重新验证通过时**追加**新条目，不改写旧结论。
6. 旧名保留在本文中；改名事件写进 §6 对照表。
7. 本文与 `ftl_revision_history.md` 的分工：FC/NAND 内部机制在本文，FTL 的映射/GC/
   元数据/日志机制在 FTL 文档；跨层决策两边各写一句并互相链接。
