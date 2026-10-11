# FC 与 NAND 模块设计方案

## 概述

本文档记录 soft_ssd 项目中 FC（Flash Controller）模块和 NAND 底层驱动模块的完整设计方案，包括设计理念、模块架构、数据结构、接口约定、命令生命周期以及各子模块的职责划分。

> **📊 流程图说明**：本文档在各关键章节内嵌了 23 张流程图（图 0–图 22），覆盖命令生命周期、数据路径编解码、nand_engine 微码状态机、dispatch poll 遍历模型（per-LUN FIFO）、LUN 相位机、调度器、Stripe RAID、完成回写，以及新增的**初始化全流程（图 14–16，§22）**与**运行时全流程（图 17–22，§23）**。建议按文末 [附录 B 阅读顺序](#附录-b建议阅读顺序) 学习；图 → 源码定位见 [附录 A](#附录-a图--源码-grep-对照)。
>
> 本文档覆盖的变更历史：
> - **Phase A**: buffer_id 便捷接口（`fc_write_cmd_data` / `fc_get_cmd_data_ptr`）
> - **Phase B**: 核心结构重组（status/descriptor/ECC/dispatch 边界）
> - **Phase C**: SET/GET_FEATURES 内联数据路径（§14）
> - **Phase D**: nand_dispatch 函数指针表重构（§15）
> - **Phase D(续)**: per-LUN chain buffer 命令链执行（§16）
> - **Phase E**: Multi-Plane 操作支持 + 行地址数组重构 + OP 码拆分（§17）
> - **Phase F/G/H**: WAIT_READY 中断驱动、DMA 中断完成、ONFI 时序切换
> - **Phase I**: 读饥饿保护（QoS）
> - **Phase J**: 加扰引擎扩展（多算法 + 多种子策略）
> - **Phase K**: BCH ECC 引擎移植 + QC-LDPC 硬判决
> - **Phase L**: CRC-32 引擎实现 + 数据路径集成（§5.1）
> - **Phase M**: Buffer 偏移重构（FC_BUFFER_USER_OFFSET, fc_buffer_get_base）
> - **Phase N**: 跨 Plane Stripe RAID 重构（删旧覆盖末 CW，新增 generate_stripe_parity，fc_poll_internal stripe 积累 + Multi-Plane 提交）
> - **Phase V**: 多 LUN 并发 ISSUE/WAIT/FETCH 拆分 + FMC 后端数组化 + 调度器 phase 机（§18.7 完整实现）
> - **Phase X**: FC-NAND 全模块注释补全 + 行业方案对比补全（§21）
> - **Phase Y**: 初始化 / 运行时全链路走读章节（§22/§23，图 14–22）+ 源码关键路径注释补全
> - **v8 (2026-10-06)**: FC→NAND 分发重构为 **per-LUN FIFO（深 4，每槽 = 一条命令）**——删 per-channel FIFO / Phase 1 / Phase 2 / busy 表 / Phase I 读饥饿保护；FC 侧改填充 pass（§2.7、§4.2/4.3、§16、§20、§23.5–23.9）
> - **v9 (2026-10-06)**: CW 索引空间统一为 **flat 绝对号 `n = plane*CW_NUM + cw`**——删 `cw_count[]`/`cws[][]`/`cw_paa[][]`/`slot_to_plane_cw`，新增 `plane_paa[PLANE_NUM]`（§25.11、§13–§14、§23.8）
> - **v10 (2026-10-07)**: 删除**软件寄存器影子** `fc_registers` / `fc_register_queue_state()`——字段重复 + 零读者，队列用量改由 `fc_get_*_count` / `fc_get_resource_stats()` 提供（§2.8）
> - **v11 (2026-10-07)**: 建链拆为**机制 / 策略**两层——新增逐条追加原语 `fc_command_attach_cw()`（分配 desc+buffer 并挂链），`fc_command_configure_descriptors()` 退化为「按掩码循环调它」的策略层，上层可自行组装任意 CW 子集（§2.9、§25.4）
> - **v12 (2026-10-07)**: **per-plane 行地址**——`fc_command_attach_cw()` 增 `row_paa` 形参（该 plane 的行地址），`fc_build_request_data_planes` 的 `plane_paa[]` 改为从 desc 链推导。multi-plane 命令的两个 plane 自此可指向**不同 block**（Policy B：每 plane 独立分配块 + 坏块替换）。策略层传 `command->paa` 派生的同页地址，对 v11 及以前**逐字节中性**（§2.9、§25.4、§25.12）
> - **v13 (2026-10-07)**: **FC 定位下沉——策略层上移 FTL**。按「硬件 FCC 寄存器表」判据（§26）删除 FC 的 `fc_command_configure_descriptors()`（掩码枚举）与 `fc_write_cmd_data()` / `fc_read_cmd_data()`（数据搬运），改由 FTL 侧 `ftl_cmd_configure_descriptors()` / `ftl_cmd_write_data()` / `ftl_cmd_read_data()` 承担；FC 侧建链只剩 `fc_command_attach_cw()` 一个入口，新增 `fc_get_cmd_cw_mask()`（从链重建掩码，供 RAID 断言）。顺带修掉 v12 的 `ftl_cmd_setup_ppa_pair` plane 下标回归（§2.9、§25.4、§26）
> - **v17 (2026-10-08)**: **建链收敛——三层接力 → 一条公共入口**。`fc_command_attach_cw`（公共壳）→ `fc_command_attach_cw_internal`（段校验）→ `fc_command_configure_descriptor_internal`（机制体）三层合并为**唯一一条** `fc_command_configure_descriptor`（**单数**），实现在 `src/fc/fc_interface.c`；后两条删除，`fc.c` 只留池/链原语（`fc_descriptor_alloc_internal` / `fc_command_chain_append` 去 `static` 进 `fc_internal.h`）。Stripe RAID parity 建链改调公共入口。校验全部前置于出池，非法实参不污染池子（§2.9、§2.10、§25.4；changelog `fc_build_chain_convergence_v17.md`）
> - **v23 (2026-10-08)**: **FTL 建链收敛为「单一掩码旋钮」**。`param.cw_mask == 0` 重定义为「**本命令 plane 段内的全部 CW**」（v7~v22 是"每 plane 连续 `0..config.cw`"），两个建链入口（`ftl_cmd_configure_descriptors` / `ftl_cmd_setup_ppa_pair`）共用唯一定义处 `ftl_cmd_resolve_cw_mask()`；`config.cw` 自此退出建链（读者只剩 FC 内 RAID stripe 的 `cw_count`），`sync_page_prepare` 的 `cw` 死参数删除；掩码越段由 `FTL_ERROR` 改 `ASSERT`（§2.10、§25.2、§25.4；changelog `ftl_cw_mask_single_knob_v23.md`）
> - **v24 (2026-10-08)**: **地址布局统一（LBA / PBA / PAA）+ 加扰器对齐存储主控**。三套地址各自定位：**PAA** 是芯片地址（MT29F4G08 的 BA6/PA 规则决定，**一个字节都没动**）、**PBA** 是系统物理地址、**LBA** 是逻辑地址；PBA 与 LBA 改为**逐位同构**的 `[ch:ce:lun:block:page:plane:cw]`（plane 在 page 之下），PAA 保持 `[cw:page:plane:block:lun]` 不动。于是建链时 `desc->paa` 与 `desc->meta` 用**同一个公式**（基值不同）算出。加扰种子由物理地址 PAA 改为**本 CW 的逻辑地址**（`desc->meta`，即扇区 LBA），并**删除 `FC_RANDOM_SEED_STRATEGY` 三选一旋钮**；加扰范围由 `[0,520)` 收窄到 `[8,520)`，**META/CRC 保持明文**，使读路径能从页内明文 META 自举出种子（GC / 掉电重建不再需要为取种子而改）。顺带修掉 LFSR 抽头错误（状态空间坍缩到周期 63，见 §21.3.5）（§2.2、§2.3、§2.9、§21.3.5；changelog `fc_scrambler_lba_seed_v24.md`）
> - **v27 (2026-10-09)**: **`cw_mask` → `cw_bitmap`，并取消「0 = 段内全部」重载**。字段改名（FC 侧 `fc_command_t.param.cw_bitmap`、NAND 侧 `nand_dispatch_request_t.cw_bitmap`，以及 `fc_get_cmd_cw_bitmap` / `fc_cw_bitmap_is_plane_symmetric` / `ftl_cmd_resolve_cw_bitmap` / `nand_dispatch_cw_first`/`_next` 形参）——名字叫 bitmap 就必须**逐位字面**：置 1 的位 = 要访问的 CW，**全 0 = 一个都不访问**（v23~v26 的「0 = 本命令 plane 段内的全部 CW」取消）。代价是「生产者忘填」与「有意访问 0 个 CW」不可区分，唯一拦网是 `ftl_cmd_resolve_cw_bitmap()` 的两条 ASSERT（release 下被裁掉 ⇒ 忘填 = 静默空操作）。故新增 `fc_command_plane_span_cw_bitmap(cmd)` 作为「整段访问」的**显式**写法，三个整段入口（`addr_to_cmd` / `ftl_cmd_setup_ppa` / `ftl_cmd_setup_ppa_pair`）各自调用它填位图（须在 `cmd->pba` 与 `config.multi_plane` 都写好之后）；只有 `ftl_cmd_configure_descriptors()` 接受调用者给的部分位图。顺带消除「两层 0 方向相反」：SQ 请求集、CQ 失败集、NAND 请求集三处现在**同口径**（0 = 空集）（§2.2、§2.10、§25.2、§25.4；changelog `fc_cw_bitmap_literal_v27.md`）
> - **v28 (2026-10-09)**: **CW 集合退出命令结构体——改为建链形参**。删除 `fc_param_t.cw_bitmap` 这个联合体视图：SQ 侧（请求集）改作 `ftl_cmd_configure_descriptors(cmd, cw_bitmap)` 的**形参**（它是建链的输入、读者只有 FTL，没有留在命令里被回读的理由），CQ 侧（失败集）**整个删除**——上层要定位失败 CW，遍历 desc 链 OR 各 `desc->status` 即可（FC 不再代为派生）。`fc_command_status_t.param` **保留**（GET_FEATURES 的寄存器回读仍走它）。顺带删掉擦除的**假 CW 位**（v7~v27 给 ERASE 伪造一个 CW0 位，只为让 completion 有格可写）——擦除请求位图自此是**合法空集**，由 `completion_all_cws` 的空集分支写 `cw_status[0]` 兜住（§2.2、§25.4；changelog `fc_cw_bitmap_param_v28.md`）
>   （v14~v16、v18~v22、v25~v26 见各自 changelog 及 §2.10 内联注记——本列表未逐条回填。）
> - **v33 (2026-10-10)**: **CRC 引擎改走 STM32H743 HAL 驱动**（changelog `fc_crc_hal_driver_v33.md`）。原 `fc_crc_engine.c` 裸写外设寄存器，`FC_CRC_BASE` 写成了 **ADC3** 的 `0x58026000`（真值 `0x58024C00`）⇒ IDR 自检不过 ⇒ `g_crc_ready = 0`（「失败静默继续」）⇒ `fc_crc_compute()` 恒返 0 ⇒ **CRC 校验自始恒通过**。改为新增平台层 `src/platform/stm32h743/crc.{h,c}` 唯一持有 `CRC_HandleTypeDef`（地址由 CMSIS 宏保证）、管时钟、做 IDR 写读回环自检（失败转 `ASSERT`，删除 `g_crc_ready` 静默降级路径）；`src/fc/fc_crc_engine.c` 只留「对哪 512B 算、结果怎么判」，`src/fc/` 保持零 HAL 依赖。**介质格式变更**：此前写入的 CW 里 CRC 字段恒为 0，改后是真值。⚠ 两坑：`stm32h7xx_hal_crc_ex.c` 必须同编（`HAL_CRC_Init` 无条件调 `HAL_CRCEx_Polynomial_Set`）、`ASSERT(HAL_CRC_Init(...))` 在 release 下会把初始化整个裁掉（须先求值再断言）
> - **v47 (2026-10-11)**: **`fc_command_t.pba` → `.paa`——PBA 退出 FC 层**。SQ 里的 PBA 会经 FC 接口一路带到 FC-NAND 层，而 FC 层在 10+ 处 `pba_to_paa()` 现场转换——PBA 与 PAA **位序不同**（`pba_to_paa` 是真正的字段重排），每处转换都是一次静默算错地址的机会。改为：**FTL 在自己的空间组合 PBA，在边界处 `pba_to_paa()` 一次，赋给 `cmd->paa`**；`src/fc/**` 里 `pba_to_paa()` / `pba_to_*()` 归零。建链入口 `fc_command_configure_descriptor()` 一并改收 `nand_paa_t row_paa`（FC 层最后一个收 PBA 的地方）；`desc->paa` 由 `paa_with_cw(paa_with_plane(row_paa, plane), cw)` 造出（PAA 的 plane 与 cw **不相邻**，v24 的 plane_cw 掩码技巧只在 LBA/PBA 空间有效），`desc->meta` 仍是 LBA 公式、不变。FTL 侧 `ftl_paa_gblock(cmd->paa)` 与旧 `ftl_pba_gblock(cmd->pba)` **逐位等价**（`bst.h` 里前者本就是后者的展开）。**PAA 无 ch/ce ⇒ 路由一律走 `config.ch/ce/lun`**（生产路径与测试本就如此，本版把它写死为结构不变式）。RAID 段（`#if CONFIG_FC_RAID`，恒关）一并机械改 PAA、**无法编译验证**。用例数不变（仍 53）；`make clean && make tests` 0 warning 0 error、链接 225212 B；固件 166708 B（§2.3.1、§6.1；changelog `fc_command_paa_v47.md`）
> - **v52 (2026-10-11)**: **链锁——链未结束期间该 LUN 的填充权归链所在提交队列独占**。补上一处真实缺口：链长于 per-LUN FIFO 深（`FC_SCHEDULER_CHAIN_MAX = 4`）时被 `fc_poll_chain_length` 截成多批、分多次填充 pass 下发，而 `fc_poll_fill` 的 `suspended[]` 每趟 `memset` 清零 ⇒ **批与批之间无跨 pass 状态**，另一条提交队列（HSQ/SQ）的命令可插进链的已下发部分与未下发部分之间。`fc_scheduler_t` 新增跨 pass 持久的 `uint8_t chain_owner[CH_NUM][CE_NUM][LUN_NUM]`（取值 `fc_chain_owner_t` NONE/SQ/HSQ），`fc_poll_fill_queue` 加 `owner` 形参、三处规则：**准入**（`chain_owner != NONE && != 本队列` → 回推跳过，**不置 suspended**，否则把链自己的续批一并锁死）/ **更新**（下发成功后看**批尾** `chain.ids[need-1].config.chain`——`MORE` ⇒ 锁继续转本队列、`END` ⇒ 解锁）/ **失败清锁**（整链注 `NAND_FAIL` 时清，免留残锁）。锁的粒度是「**哪条提交队列能喂这个 LUN**」而非「LUN 不能用」：链所在队列照常能喂（续批正从这条队列取），另一条队列一律回推，直到链 END 下发。与既有「共享 `suspended` 防优先级倒挂」**正交**。**对生产路径零影响**：FTL 全仓恒 `FC_CHAIN_END` ⇒ `chain_owner` 恒 NONE ⇒ 准入判断恒假、更新恒写 NONE ⇒ 逐字不变（§2.7、§16.9、§23.7、§23.9；changelog `fc_chain_priority_lock_v52.md`）
> - **2026-10-09（协议查证，非代码版本）**: **multi-plane 不在 ONFI 1.0 里**。逐字读 ONFI 1.0（Rev 1.0, 2006-12-28）Table 14 命令集：cache 只有单 plane 的 `Read Cache` 31h / `Read Cache End` 3Fh / `Read Cache Enhanced` 00h-31h / `Page Cache Program` 80h-15h，**全文 `plane` 一词出现 0 次**（1.0 唯一的「多设备」是 multiple LUN，走 `Interleaved` D1h / 11h）。multi-plane 全部来自器件侧。顺带逐字核对了 Micron「4Gb, 8Gb, and 16Gb x8 NAND Flash Memory」（MT29FxG08xAA, Rev B）的 Table 7 Two-Plane Command Set / §Two-Plane Addressing，并发现两处待办：① 「two-plane 命令族要求 on-die ECC 关闭（Note 4）」的出处对不上（该 PDF 全文无 `on-die`）；② 器件有 `TWO-PLANE PROGRAM PAGE CACHE MODE 80h-11h-80h-15h`，本项目 op 表缺此 op（§25.6 前置约束 3、§25.8 风险 2 与风险 8）

目标硬件平台：STM32H743 + MT29F4G08ABADAWP（1 Channel × 1 CE × 1 LUN × 2 Plane × 2048 Block × 64 Page，512 MiB SLC NAND）。

---

## 1. 整体架构层次

```
┌─────────────────────────────────────────────────┐
│  FTL / Controller / NVMe                         │  ← 上层调用者
├─────────────────────────────────────────────────┤
│  FC 模块 (Flash Controller)                       │  ← 命令配置、队列、调度
│  fc.h  ─ 对上层提供的统一接口（类型/宏，include fc_interface.h）│
│  ├── fc_interface.h/c  对外接口声明 + 实现（薄壳，单例控制器） │
│  ├── fc_internal.h     fc.c ↔ fc_interface.c 共享内部签名     │
│  ├── fc_command.c/h   命令 & Descriptor 结构定义    │
│  ├── fc_queue.c/h     SQ / HSQ / CQ 环形队列       │
│  ├── fc_scheduler.c/h HSQ 优先调度器               │
│  ├── fc_data_path.c/h 数据路径编排（Random→LDPC→RAID）│
│  ├── fc_random_engine 加扰/解扰引擎                │
│  ├── fc_ldpc_engine   LDPC 编解码引擎              │
│  ├── fc_raid_engine   RAID XOR 引擎               │
│  └── fc.c             核心：资源池、生命周期、服务主循环     │
├─────────────────────────────────────────────────┤
│  NAND Dispatch (调度桥接层)                        │
│  nand_dispatch.c/h ─ per-LUN FIFO，FC→NAND 命令路由 │
├─────────────────────────────────────────────────┤
│  NAND 底层驱动                                    │
│  ├── nand_engine      UOP 微操作引擎（start/step）  │
│  ├── nand_fmc         FMC 硬件后端（15 个 UOP handler）│
│  ├── onfi_1_0         29 种 ONFI 操作 UOP 序列表    │
│  └── nand_mt29f4g08   芯片 init/execute/start/step  │
├─────────────────────────────────────────────────┤
│  硬件抽象                                          │
│  ├── nand_config.h    编译期几何配置 + PAA 编解码    │
│  ├── nand_geometry    运行时几何校验                │
│  └── nand_device_t    ops 回调式抽象（备用/sim）     │
└─────────────────────────────────────────────────┘
```

核心设计原则：

1. **固定资源池 + 空闲链表**：所有资源（Command、Descriptor、Buffer）在 .bss 中静态分配，使用空闲链表实现 O(1) 分配释放，不使用动态内存。
2. **ID 引用代替指针**：对外暴露 16-bit 资源 ID，内部通过 ID→指针转换函数访问，避免指针悬挂和跨模块直接访问。
3. **FC 封装内部状态**：`fc_t` 结构体定义在 `fc_internal.h`，仅 `fc.c`/`fc_interface.c` 内部可见，对外只提供函数接口。
4. **异步非阻塞**：提交命令立即返回，通过 `fc_service()` 轮询推进，`fc_poll_completion()` 获取结果。

### 流程图：分层架构与数据流总览（图 0）

```text
┌─────────────────────────────────────────────────────────────┐
│  FTL / Controller / NVMe  （上层调用者）                      │
│    alloc → 配参 → configure_descriptors → submit → poll      │
└──────────────────────────┬──────────────────────────────────┘
                           │ fc_command_t*（ID 引用，非裸指针）
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  FC 模块（firmware 层：命令池 / desc 链 / buffer 池）          │
│    fc_command.c     命令与 Descriptor 结构                    │
│    fc_queue.c       SQ / HSQ / CQ 环形队列                    │
│    fc_scheduler.c   调度器状态（SQ/HSQ 绑定；填充 pass 在 fc.c）│
│    fc_data_path.c   写: META→CRC→加扰→ECC→Stripe RAID        │
│                    读: ECC→解扰→CRC→META (+ RAID 恢复)       │
│    fc_random/ldpc/raid/crc_engine.c  软件引擎                 │
└──────────────┬──────────────────────────────────────────────┘
               │ nand_dispatch_request_t（预解析，字段穿越防火墙）
               ▼
┌─────────────────────────────────────────────────────────────┐
│  nand_dispatch（调度桥接层）                                  │
│    submit / submit_chain 入队 → poll 逐 LUN 出队 + 推进       │
│    g_nand_dispatch_queues[ch][ce][lun]  per-LUN FIFO（深 4）  │
│    g_nand_channel_page_buffer[ch][ce][lun] 多 plane 拼接缓冲  │
└──────────────┬──────────────────────────────────────────────┘
               │ nand_fc_start / nand_fc_step（异步非阻塞）
               ▼
┌─────────────────────────────────────────────────────────────┐
│  nand_engine（UOP 微码解释器，软件 micro-sequencer）           │
│    step() 逐条执行 UOP 数组；JUMP 由引擎直接处理               │
└──────────────┬──────────────────────────────────────────────┘
               │ handler(uop, request, timing, completion)
               ▼
┌─────────────────────────────────────────────────────────────┐
│  nand_fmc（FMC 硬件后端：15 个 UOP handler 的函数指针表）       │
│    data / command / address 三个 volatile 窗口                │
│    PIO 逐字节 或 DMA（CONFIG_NAND_DMA）                       │
└──────────────┬──────────────────────────────────────────────┘
               ▼
        ┌── FMC 并行总线 → MT29F4G08 NAND（1ch × 1CE × 1LUN × 2plane）
```

**阅读要点**：越往下越贴近硬件；`nand_dispatch_request_t` 是 FC firmware 与 NAND 硬件引擎之间的"防火墙"，只有硬件要消费的字段才跨越（§13）。上层完全异步：`fc_submit()` 立即返回，推进全部发生在 `fc_service()` 轮询里。

---

## 2. FC 模块详细设计

### 2.1 资源池

三个固定大小的资源池，均以空闲链表方式管理：

| 资源池 | 容量 | 元素大小 | 用途 |
|--------|------|----------|------|
| Command Pool | 64 | `fc_command_t` | 命令控制块 |
| Descriptor Pool | 256 | `fc_descriptor_t` | 数据描述符（每 CW 一个） |
| Buffer Pool | 128 | 528 字节（= PAGE_SIZE / 4） | 数据缓冲区（CW 内部布局见 §5.3） |

Buffer Pool 内部每个 CW（528 字节）分层管理：

```
对外暴露区域（FTL 可见）：
  [USER:512B]    ← fc_buffer_id_to_real_buffer() 返回此首地址

内部管理区域（FC 管控，FTL 不感知）：
  [META:4B]      ← FC 管理（LBA 等 META 信息），读回时自动校验
  [CRC:4B]       ← USER 数据的 CRC-32 校验值
  [USER:512B]    ← 用户数据（FTL 填入）
  [PARITY:8B]    ← ECC 校验数据

对外接口通过 FC_BUFFER_USER_OFFSET(8) 偏移，只暴露 USER 区给 FTL。
FC 内部通过 fc_buffer_get_base() 获取 CW 基址（META 开头），用于加扰/ECC/RAID。
```

### 各模块数据处理范围

| 模块 | 处理大小 | 影响区域 | 原因 |
|------|---------|---------|------|
| CRC | 512 | `[4:8]` 写入 CRC 值 | 只保护 USER 数据 |
| Random 加扰 | **520** | `[0:520]` 全数据区 | ECC 会覆盖 parity 区，RN 白做 |
| ECC 编解码 | 520 → 8B parity | 读 `[0:520]`，写 `[520:528]` | BCH 输入 520 数据，输出 8B parity |
| RAID XOR | 528 | 全 CW（含 parity）| 需要保护 parity 自身完整性 |

ECC 引擎的 `data_length` 参数直接表示实际数据大小（520B），parity 始终写入 `data + data_length`。
RAID XOR 使用 `FC_BUFFER_POOL_CW_SIZE(528)`，读全 CW。

容量设计理由：
- 64 个 Command：覆盖一个 Block 的所有 Page（64 页），支持 pipeline 提交
- 256 个 Descriptor：每个 Command 最多 4 个 CW，64 × 4 = 256
- 128 个 Buffer：至少支持与 Descriptor 1:2 的比例，确保读写并发

池外还有若干**旁路簿记数组**（`fc_t` 里存指针，本体是 `fc.c` 的 static 数组）：

| 数组 | 尺寸 | 用途 |
|------|------|------|
| `descriptor_free_next[]` / `buffer_free_next[]` / `command_free_next[]` | 256/128/64 | 三条空闲链的 next（下标即 ID） |
| `command_state[]` / `buffer_state[]` | 64 / 128 | 状态位 |
| `descriptor_owner[]` | 256 | **desc → 归属命令 ID 的 O(1) 反查表**：`fc_submit` 校验 desc 链所有权（`fc_command_descriptors_owned_internal`）+ `fc_get_resource_stats` 计数 |

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
> 详见 `docs/changelog/fc_chain_ownership_v16.md`。
>
> **v17：这套机制体即 `fc_command_configure_descriptor`**（v16 时在 `fc.c`，v17 随
> 三层合并搬到 `src/fc/fc_interface.c`，函数名去 `_internal` 后缀）。机制顺序不变，
> 唯一新增约束是**校验段整体前置于出池**——非法实参在分配任何 desc 之前就返回，
> 故「污染池子」的可能被进一步排除。详见 `docs/changelog/fc_build_chain_convergence_v17.md`。

### 2.2 命令结构 (fc_command_t)

```c
struct fc_command_t {
    DWORD desc_id    : 16;     // 首个 Descriptor ID；0xFFFF=无 Descriptor
    DWORD high_priority : 1;  // 0=SQ, 1=HSQ
    DWORD module     : 4;     // 完成回调路由（HOST_IO/FTL/GC/READ_RECOVERY/METADATA/BBT/CHECKPOINT/WEAR_LEVELING）
    DWORD feature_addr : 8;   // SET/GET_FEATURES 寄存器地址，通过 COLUMN_0 发送
    DWORD reserved1  : 3;     // 保留（未来命令控制扩展）
    DWORD meta;              // META 值，当前传 LBA（Host 逻辑地址），未来可扩展
    DWORD paa;                 // 物理地址（操作相关）
    fc_cmd_conf_t config;      // 操作配置（见下文）
    fc_param_t    param;       // 4 字节联合体（见下）：仅 FEATURES=内联数据 parameter0..3（v28 起无 cw_bitmap 视图）
};
```

> **字段变更说明**：
> - `feature_addr:8` 取代原 `reserved1:11` 中的 8 位，专用于 SET/GET_FEATURES 的寄存器地址（通过 COLUMN_0 uop 发送）。
> - `parameter0..3` 从 `DWORD : 8`（位域）改为 `uint8_t`（纯字节），原因是 SET/GET_FEATURES 内联路径需要取 `&parameter0` 地址作为 buffer 指针。位域不能取地址，故以 4 字节空间换地址能力。Command Pool（64 条）总量增加 256 字节。
> - **v7**：4 个散装字节合并为 `fc_param_t` 联合体（`parameter0..3` / `bytes[4]` / `cw_bitmap`），SQ 与 CQ 共用。
>   `cw_bitmap` 是 flat CW 位图（bit `n = plane*CW_NUM + cw`）。
>   ⚠ **SQ 提交后不可变**：命令配置完即算完成，运行期返回值一律走 CQ（见 §25）。
> - **v27**：`cw_mask` 改名 `cw_bitmap`（**两层同改**：FC 的 `param.cw_bitmap` 与 NAND 的
>   `nand_dispatch_request_t.cw_bitmap`），并取消 v23~v26 的「`0` = 本命令 plane 段内全部 CW」重载。
>   现在两层**都是字面位图**：置 1 的位 = 要访问的 CW，**全 0 = 一个都不访问**。
> - **v28**：**`cw_bitmap` 这个联合体视图整个删除**——它是**派生量**，不该住在命令里。
>   SQ 侧（请求集）读者只有 FTL 且就是建链的输入 ⇒ 改作 `ftl_cmd_configure_descriptors(cmd, cw_bitmap)`
>   的形参；CQ 侧（失败集）读者只有上层，而上层遍历 desc 链 OR 各 `desc->status` 即可 ⇒ FC 不再聚合。
>   整段访问仍由 `fc_command_plane_span_cw_bitmap(cmd)` 算，只是现在直接当**实参**传（§25.2、§25.4）。
> - **v15**：删除 `command_id : 16` + `reserved0 : 16`（结构体 **24 → 20 字节**，64 槽省 256B）。
>   命令池 ID 不再是缓存字段，而由 **指针派生**：`fc_command_to_id()` /
>   `fc_command_to_id_internal()` 做 `command - command_pool`。理由：缓存的副本在
>   `fc_command_release()` 把结构体整体清零后失效（归 0 而非 0xFFFF，判不出来），派生值不会；
>   而指针本来就在调用者手里，再存一份只是多一个不一致的机会。
>   派生的 ID 喂给 `nand_dispatch_request_t.command_id` 与 CQ 的 `command_id`，上层要回命令指针走
>   `fc_command_id_to_command()`。见 `docs/changelog/fc_command_identity_convergence.md`。

`fc_param_t` 联合体：

```c
typedef union {
    struct { uint8_t parameter0, parameter1, parameter2, parameter3; };
    uint8_t  bytes[4];  // 内联数据的字节视图
    // v28：删除 `uint32_t cw_bitmap` 视图——CW 集合是建链的输入（形参），不是命令的属性。
} fc_param_t;
```

命令配置位域 `fc_cmd_conf_t`（32-bit 位打包）：

| 字段 | 位宽 | 说明 |
|------|------|------|
| operation | 8 | ONFI 操作码（OP_READ_ID / OP_NORMAL_READ_PAGE / OP_NORMAL_PROGRAM_PAGE / OP_ERASE_BLOCK ...）|
| ch | 2 | 目标通道号 |
| ce | 2 | 目标片选号 |
| lun | 2 | 目标 LUN 号 |
| cw | 2 | Command：每-plane CW 数减一（**v23 起建链不读它**，读者只剩 FC 内 RAID stripe 的 `cw_count`）；Descriptor：本 desc 在 plane 内的 CW 序号 |
| crc | 1 | CRC 使能（0=enable, 1=disable） |
| ldpc | 2 | LDPC 模式（0=full, 1=hard, 2=soft, 3=raw） |
| raid | 3 | RAID 模式（0=normal, 1=decode, 2=encode, 3=recover） |
| random | 1 | 加扰使能（0=enable, 1=disable） |
| chain | 1 | 链式命令标志（0=结束, 1=后续还有） |
| page_type | 2 | 页类型（0=LP, 1=MP, 2=UP, 3=TP） |

### 2.3 描述符结构 (fc_descriptor_t)

```c
struct fc_descriptor_t {
    DWORD paa;               // 芯片物理地址 PAA（paa_with_cw(paa_with_plane(row_paa, plane), cw)，v47）
    fc_cmd_conf_t config;    // 继承命令配置，config.cw 表示当前 CW 索引
    DWORD buffer_id : 16;   // Buffer Pool ID，0xFFFF=无效
    DWORD next_id   : 16;   // 下一个 Descriptor ID，0xFFFF=链表结束
    DWORD meta;              // 本 CW 的**扇区 LBA**（v24）；读后存放 NAND 读回的 META 值。
    DWORD status    : 8;    // CW 状态（FC_CW_STATUS_*），完成时由 FC 回写
    DWORD ecc_bits  : 8;    // ECC 纠正比特数，完成时由 FC 回写（0=无错误）
};
```

#### 2.3.1 三套地址空间（v24 定稿，v47 分界调整）

同一颗芯片上有三套地址，**位序刻意不同**，各司其职（定义见 `include/nand/nand_config.h`）：

| | 低位优先布局 | 用途 | 谁定义 |
|---|---|---|---|
| **PAA** | `[cw:2][page:6][plane:1][block:11][lun]` | **芯片地址**——喂给 `paa_to_nand_row()` 编成器件行地址；**FC 层唯一的物理地址** | MT29F4G08 的 BA6/PA 规则，**不可改** |
| **PBA** | `[cw:2][plane:1][page:6][block:11][lun][ce:2][ch:2]` | **系统物理地址**——**FTL 内部**地址；在 FTL 侧 `pba_to_paa()` 转出后交 FC | 本工程自定 |
| **LBA** | `[ch:ce:lun:block:page:plane:cw]` | **逻辑地址**——主机寻址单位 = 512 B 扇区 | 本工程自定 |

- **v47 分界**：**FTL 交 LBA + PAA**（PBA 在 FTL 侧转出），`fc_command_t.paa` 是 FC 层唯一的物理地址字段——`src/fc/**` 里不再出现 `pba_to_paa()` / `pba_to_*()`。PBA 带 ch/ce，而 FC 的路由本就独立走 `config.ch/ce/lun`（见 `fc_build_dispatch_request`），两套编码并存只会制造静默算错地址的机会。
- **PBA 与 LBA 逐位同构**（除 PBA 多出的 cw 字段语义：LBA 里 cw 也是低 2 位）。这是 v24 的核心收益（v47 起只在 FTL 侧有效）：LBA 空间里两个值可用**同一个公式**造出，只是基值不同——
  ```c
  const uint32_t plane_cw = ((uint32_t)plane << PBA_PLANE_SHIFT) | cw;
  desc->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw;  /* 逻辑基值（LBA 空间，v47 不变） */
  ```
  **PAA 不参与这条公式**——它的 plane 与 cw **不相邻**（`[cw][page][plane][block]`），v47 起 `desc->paa` 改用现成 helper：
  ```c
  desc->paa = paa_with_cw(paa_with_plane(row_paa, plane), cw);  /* 物理基值（PAA 空间，v47） */
  ```
  代价：v24「一条公式造两个值」的便利消失，换来 FC 层内 PBA 归零。
- **为什么 PBA 的 plane 在 page 之下**（与 PAA 相反）：FTL 的页号 `internal_lba` 里 plane 就是最低位（`ftl.c` 的 `l2p_cache_read(internal_lba + 1)` 取 plane1）。取 `LBA >> CW_WIDTH` 必须**逐位等于**今天的 `internal_lba`，否则 plane1 会变成 `internal_lba + 64`，L2P 编号、journal / checkpoint 的页号、所有 `+1` 约定全要改。选 `[page][plane]` 而不是跟 PAA 的 `[plane][page]` 就是**为了这一点**。
- **PAA 布局不动**：`paa_to_nand_row`、器件行地址编码 `PA[5:0] | BA6<<6 | BA[17:7]` 保持原样，`paa_make` 的全部调用点字段值逐位不变（v25 只把形参序换成 `nand_addr_t` 具名字段，见 §6.2）。`pba_to_paa()` / `paa_to_pba()` 是**真正的字段重排**（不是掩码截取）——旧版 `PAA_FROM_PBA_MASK` 掩码宏已删除。
- **低 3 位是共享编码**：`PLANE_CW_MASK = 0x7`（`[plane:1][cw:2]`），PBA 与 LBA 共用（PAA 不共用），所以 `desc->meta` 的低 3 位与 `desc->paa` 的 plane/cw 天然一致，Pass 4 才能拿它做自洽校验。

设计决策：
- **Descriptor 中 CW 字段为"当前 CW 索引"**，不同于 Command 中 CW 为"数量减一"
- **Buffer 与 Descriptor 解耦**：通过 buffer_id 引用，Buffer 可独立释放（如 UNC 时释放 buffer 保留 descriptor 信息）
- **linked list 链式连接**：通过 next_id 遍历，无需 Command 中维护 descriptor_count
- **状态 & ECC 回写**：NAND 完成时 FC 将 `cw_status` 和 `ecc_bit_count` 回写至 descriptor 的 `status` 和 `ecc_bits` 字段。FTL 遍历 desc 链即可获取每 CW 详细状态，无需 CQ 查询。这对 Read Retry、RAID 恢复路径至关重要。
- **per-CW META**：`meta` 替代原 `reserved:16`，每个 Descriptor 拥有独立的 META 值。建链时（`fc_command_configure_descriptor()`，v17 前为 `fc_command_attach_cw()` → `fc_command_configure_descriptor_internal()`）用 `lba_to_meta(command->meta, plane, cw)` 自动计算 `desc->meta`，encode 时写入 `buffer[0..4]`（META 区），用于读回时的端到端地址校验。FC 在 decode 路径中自动对比 desc->meta 与读回值，不匹配时置 MATA_ERROR。
  - **v24 起**：`command->meta` 是**扇区 LBA**（`页 LBA << CW_WIDTH`），`desc->meta` 是该 CW 的完整扇区 LBA（不再是"页 LBA 低 2 位塞 CW 号"）；`lba_to_meta(lba, plane, cw)` 同时把 **plane** 写进低 3 位，使 plane0/plane1 的 4 条 CW 各有唯一 meta（旧编码里 plane1 的 desc 会拿到 plane0 的 meta，两条 desc 撞值）。低 3 位编码与 PBA 的 `[plane:1][cw:2]` **同一套**（`PLANE_CW_MASK` / `PBA_PLANE_SHIFT`）。

### 2.4 CW 状态位

```c
FC_CW_STATUS_NAND_FAIL   // NAND 操作失败
FC_CW_STATUS_TIMEOUT     // 超时
FC_CW_STATUS_CRC_ERROR   // CRC 校验错误
FC_CW_STATUS_UNC         // 不可纠正错误
FC_CW_STATUS_MATA_ERROR  // MATA 校验错误
FC_CW_STATUS_RAID_ERROR  // RAID 恢复失败
FC_CW_STATUS_READY       // 操作完成（= SUCCESS）
FC_CW_STATUS_PROTECTED   // 目标块受保护
```

状态汇总规则：READY 需要所有有效 CW 就绪；失败位任意 CW 置位即置位。

**谁在读这些失败位**：

- **FC 侧**：`FC_CW_STATUS_ERROR_MASK`（六个错误位的并集，见 `fc_command.h`）用于挑出**首个失败 CW** 的
  `desc_id`（`fc.c:778`），兑现 `fc_command_status_t.desc_id` 的契约（"first-failing desc on error"）。
- **FTL 侧**：目前**只判 `FC_CW_STATUS_NAND_FAIL`**（`ftl.c:92`、`ftl_fc_interface.c:431/459/527`、
  `gc.c:75`、`read_recovery.c:96`）⇒ `UNC` / `CRC_ERROR` / `MATA_ERROR` 被当成功。
  **已检出的错误会以 `SSD_STATUS_OK` 交给主机**——见 §8.2。

### 2.5 完成状态 (fc_command_status_t)

```c
struct fc_command_status_t {
    DWORD command_id : 16;  // 完成命令的 Pool ID
    DWORD desc_id    : 16;  // 首个 Descriptor ID；失败时标记出错的 Descriptor
    DWORD module     : 4;   // 命令发起者模块，FTL 无需 ID 查询直接路由完成通知
    DWORD status     : 8;   // 命令级聚合状态（各 CW 的 FC_CW_STATUS_* 位 OR）
    DWORD reserved   : 20;  // 保留
    fc_param_t param;       // 命令级返回段：仅 GET_FEATURES=寄存器值 bytes（v28 起无失败位图视图）
};
```

CQ 条目 12 字节，64 条 CQ 共 768 字节。

**设计要点：**
- **module 字段**：拷贝自 `command->module`，FTL 收到完成通知后可直接路由到对应模块（Host IO / GC / Read Recovery 等），无需通过 command_id 查询再决定路由。
- **只带命令级聚合**：**v7 删除了 `ecc_bit0..3`**。per-CW `status` / `ecc_bits` 只留在 Descriptor 链，
  命令级聚合 ECC 走 `fc_get_cmd_ecc_max()`（取全链最大值）。
  **v28 起不再聚合失败 CW 位图**（原走 `param.cw_bitmap`）——上层要定位失败 CW，遍历 desc 链
  OR 各 `desc->status` 即可（desc 链完整保留到 `fc_command_release()`）；CQ 只给命令级
  `status` 与 `desc_id`（首个失败 CW）。
- **SQ 不可变**：GET_FEATURES 的寄存器值经控制器级暂存 `g_fc_return_staging[command_id]` 中转进
  CQ 的 `param.bytes`；SQ 命令体提交后不再被写。

### FC 返回码 (fc_status_t)

`include/fc/fc_types.h` 的返回码分三层——**成功 / 调用者写错 / 运行结果**。摆在一起看，
`FC_STATUS_IO_ERROR` 的边界才清楚：

```c
FC_STATUS_OK               =  0    成功
FC_STATUS_INVALID_ARGUMENT = -1    ← 调用者写错了（编程 bug）
FC_STATUS_QUEUE_FULL       = -2    ← 状态类：此刻不行，条件会变
FC_STATUS_QUEUE_EMPTY      = -3
FC_STATUS_NOT_READY        = -4
FC_STATUS_IO_ERROR         = -5    ← 数据 / 硬件层真的坏了
FC_STATUS_NOT_SUPPORTED    = -6    ← 这条路没实现（≠ 失败）
FC_STATUS_RESOURCE_EMPTY   = -7    ← 状态类
```

| 码 | 谁的问题 | 该怎么办 |
|---|---|---|
| `INVALID_ARGUMENT` | **调用者**写错 | 不重试，是 bug（判据见 §24 判据 1） |
| `QUEUE_FULL` / `QUEUE_EMPTY` / `NOT_READY` / `RESOURCE_EMPTY` | 此刻不行，条件会变 | **可以等 / 重试** |
| **`IO_ERROR`** | **数据 / 硬件**真的坏了 | 同一操作重试**没用**，得换手段（RAID / read retry / 换块） |
| `NOT_SUPPORTED` | 这条路没实现 | **不是失败**——`fc_data_path_ecc_decode` 明确不吞它（`fc_data_path.c:133`），调用者当「成功但零纠正」处理 |

> **一句话**：`IO_ERROR` = 「这次操作在数据 / 硬件层面真的失败了，重试同一次没有意义」。

**本仓实际返回 `IO_ERROR` 的四类情形**：

| 类 | 位置 | 情形 |
|---|---|---|
| **A. 介质 / ECC** | `fc_data_path.c:543` | `process_read` Pass 1 有 CW 不可纠 ⇒ **短路**返回（Pass 2/3/4 全没跑） |
| | `fc_data_path.c:692` | `raid_recover`：RAID 重建后 ECC 仍失败 ⇒ 硬件不可恢复 |
| | `fc_ldpc_engine.c:170` | BCH 找不到 ≤ t 的解 |
| | `fc_ldpc_qc_engine.c:331` | LDPC 迭代到不动点仍不收敛 |
| **B. NAND 驱动** | `fc.c:1365` / `:1421` | `nand_dispatch_submit` 非 OK ⇒ 命令根本没提交下去 |
| | `fc.c:1038` | RAID stripe 提交失败：先给数据命令**合成**一条 `NAND_FAIL` completion 让 FTL 感知，再返回 |
| **C. FC 基础设施** | `fc.c:817` | **CQ 满**——数据路径已处理、desc 已更新，但 CQ entry 塞不进去 ⇒ 上层永远看不到这条完成（注释称 "fatal 级系统背压错误"） |
| | `fc.c:1674` | 同上。⚠ 但 **NAND 命令自身失败不算 `IO_ERROR`**——那种状态随 completion 进了 CQ entry |
| | `fc.c:1563` / `:1594` / `:1598` | 调度状态机内部出错 |
| | `fc.c:356` | `nand_dispatch_init()` 失败（初始化期） |
| **D. 上一层** | `ftl.c:234` / `:263` | `SSD_STATUS_IO_ERROR = -3`——**另一套枚举**（`include/common/ssd_types.h`），FTL 给主机的码：读恢复全失败（标坏块）/ 写失败（关 SB + 标坏块） |

**两个容易混的点**：

1. **FC 的 `IO_ERROR` ≠ 主机的 `IO_ERROR`。** 中间隔着 FTL 的翻译。而 `ftl.c:92` 那条路上只查
   `FC_CW_STATUS_NAND_FAIL`（bit 0），`UNC` 是 bit 3 ⇒ **读失败会被当成功**交给主机——见 §8.2。
2. **数据路径的返回值在生产里是「被丢弃」的。** `fc.c:744`：

   ```c
   (void)fc_data_path_process_read(command);
   /* 即使 process_read 返回 IO_ERROR（有 CW 不可纠），
    * 已完成 ECC 解码的 desc->status 已更新。
    * 后续 completion entry 将从 descriptor 读取最终 status。 */
   ```

   生产**不看返回值**，只看 `desc->status`（Step 3 全链 OR，`fc.c:772`）。所以单测里
   `munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_IO_ERROR)`（`test_fc_level2.c:1107`）
   钉的是**函数契约**——它仍是唯一钉住「Pass 1 失败即短路」的地方（后续「不解扰」「CW1=UNC 其余 READY」
   两条断言都依赖这个短路），但别把它当成「生产靠这个码判错」。

### 2.6 队列系统

**SQ（Submission Queue）**：普通优先级命令，深度 64
**HSQ（High-priority SQ）**：高优先级命令（如 Read Retry、恢复操作），深度 16
**CQ（Completion Queue）**：完成通知，深度 64

队列实现：环形 FIFO，存储 Command Handle 或 Completion 结构体。队列存储由 FC 内部静态数组提供。

### 2.7 调度器 (fc_scheduler_t)

**v8（per-LUN FIFO，2026-10-06）**：调度规则改为「填充 pass」——`fc_poll_fill()`（在 `src/fc/fc.c`）
一次扫描 HSQ/SQ，按 LUN 保序把能放下的命令（含链）提交进 NAND 侧 per-LUN FIFO：

- **HSQ 优先**：先扫 HSQ，再扫 SQ；两个队列走同一套扫描逻辑（HSQ 同样参与链）。
- **按 LUN 保序 + room 判定**：命令 `need` = 链长（链头 = 连续 `chain=1` 段长度，否则 1）；
  `nand_dispatch_lun_free(lun) >= need` 才取，否则该 LUN 本轮挂起，后续同 LUN 命令一律跳过。
- **一次扫描**：按本轮 HSQ/SQ 快照数量扫描，扫完即止；取不走的命令 pop 后回推队尾
  （整轮等价一次旋转，未取出命令的相对顺序不变）。
- **共享挂起表**：HSQ 挂起某 LUN 后，SQ 本轮也跳过该 LUN——否则同 LUN 的 SQ 命令会插到
  HSQ 命令之前下发，优先级倒挂。
- **链锁（v52）**：一条链可能长于 per-LUN FIFO 深（`FC_SCHEDULER_CHAIN_MAX = 4`），被拆成
  多批、分多次填充 pass 下发。批与批之间若无跨 pass 状态，另一条队列的命令就会插进链的
  「已下发部分」与「还没下发的部分」之间。故 `fc_scheduler_t` 持有一张**跨 pass 持久**的
  per-LUN `chain_owner[ch][ce][lun]`（取值 `fc_chain_owner_t`：NONE / SQ / HSQ）：
  链一旦开始，该 LUN 的**填充权归链所在的那条提交队列独占**，直到链的 END 下发。规则三处
  （`fc_poll_fill_queue`）：**准入**——`chain_owner != NONE && != 本队列` 的命令回推跳过
  （**只回推、不置 `suspended`**，置了会把链自己的续批一起锁死）；**更新**——下发成功后看
  **批尾**的 `chain` 位，`MORE` ⇒ 锁继续（转本队列）、`END` ⇒ 解锁；**失败清锁**——整链注
  `NAND_FAIL` 时清 `chain_owner`，免留残锁把 LUN 永久锁死。锁的粒度是「**哪条提交队列能喂
  这个 LUN**」，不是「LUN 不能用」：链所在队列照常能喂（续批正从这条队列取），另一条队列
  一律回推——**发 SQ 链时 HSQ 插不进、发 HSQ 链时 SQ 插不进**。它与上面那张**共享挂起表**
  正交（准入只回推、不置 `suspended`）：挂起表管「本趟 room 不足」、每趟清零；链锁管「链
  没完」、跨趟保留。⚠ **前置契约**：链成员必须在**同一条提交队列**里**连续入队**（现有链
  机制本就依赖它，`fc_submit` 一次一条、连续性无人保证）。**生产路径恒 NONE**——FTL 全仓恒
  `FC_CHAIN_END`（见 §23.9）⇒ 准入判断恒假、更新恒写 NONE ⇒ 逐字不变。详见
  `docs/changelog/fc_chain_priority_lock_v52.md`。

`fc_scheduler_t` 只剩 `{sq, hsq, chain_owner[][][]}`（v8 删到 `{sq, hsq}`，v52 加回链锁表）；
旧的 per-LUN busy 表（`set/clear_busy`、`target_is_busy`）
与读饥饿保护（`consecutive_write_count`）已删除——"忙"改由 NAND 侧 `nand_dispatch_lun_free()`
表达。详见 `docs/changelog/fc_nand_per_lun_fifo.md`。

### 2.8 软件寄存器（**v10 已删除**）

曾有一套 `fc_registers_t` 影子结构（`config`/`status`/三对 head-tail/`interrupt_*`），
由 `fc_register_queue_state()` 在 11 个点刷新，"模拟硬件寄存器风格"。**v10 整体删除**：

- 10 个字段里 6 个（三对 head/tail）与 `fc_t` 的 `sq`/`hsq`/`cq` 字段**逐字节重复**；
  3 个（`config`、`interrupt_enable`、`interrupt_status`）**全仓零读零写**；
  只剩 `status` 是派生量，缓存着 controller 里已有的信息。
- 全仓无任何读者。
- "模拟硬件"的理由不成立：真硬件寄存器每次访问都读当前值，而这个影子只在 11 个点刷新，
  早退路径上还**故意保留陈旧值**（见 `docs/changelog/fc_structural_changes.md`），
  比"读时计算"更不像硬件，也无法服务异步 DMA 读者。

队列用量改由 `fc_get_sq_count()` / `fc_get_hsq_count()` / `fc_get_cq_count()`（`fc.c`）
与聚合的 `fc_get_resource_stats()`（`fc_interface.c`，读 controller 实时值）提供。
详见 `docs/changelog/fc_registers_removal.md`。

### 2.9 对外接口 (fc.h)

对外接口分两个头文件，`fc.h` include `fc_interface.h`，外部只 `#include "fc/fc.h"`：

| 文件 | 内容 |
|------|------|
| `include/fc/fc.h` | 类型、宏、资源池配置、功能开关、资源统计结构 + `#include "fc_interface.h"` |
| `include/fc/fc_interface.h` | 对外函数声明（21 个，6 组：①初始化 ②命令生命周期 ③buffer 访问 ④数据查询 ⑤查询工具 ⑥服务入口） |
| `include/fc/fc_internal.h` | fc.c ↔ fc_interface.c 共享内部签名（`fc_t` 定义、`g_fc_controller` extern、`fc_*_internal` 声明）——FTL 不得 include |
| `src/fc/fc.c` | 核心实现：三池、生命周期状态机、`fc_poll_internal` 服务主循环（拆为 `fc_poll_dispatch_single` / `fc_poll_dispatch_chain` + `fc_build_dispatch_request` / `fc_poll_fail_command` 四个 static helper）、`fc_complete_from_nand`、Stripe RAID（全部 `_internal`） |
| `src/fc/fc_interface.c` | 对外函数实现（单例 `g_fc_controller`）+ **建链机制体**（v17 起 `fc_command_configure_descriptor` 在此；除建链编排外为参数校验 + 转发 internal） |

```c
/* 全部声明见 fc_interface.h，此处仅示意签名 */
fc_status_t   fc_init(void);                           // 初始化所有资源池和队列
fc_command_t *fc_command_alloc(void);                   // 申请命令，失败返回 NULL
fc_command_t *fc_command_id_to_command(uint16_t id);    // ID→指针转换
uint8_t       fc_command_to_id(const fc_command_t *, fc_command_id_t *); // 指针→ID（逆转换；返回 0=指针不在池内，如栈上的 fc_command_t）
fc_descriptor_t *fc_descriptor_id_to_descriptor(uint16_t id);
fc_status_t   fc_command_configure_descriptor(fc_command_t *cmd, uint8_t plane, uint8_t cw, nand_paa_t row_paa); // 逐条追加一个 CW 到链尾（**建链的唯一入口**，v17 由三层收敛而来；行地址由调用者按 plane 给出）
fc_descriptor_id_t fc_descriptor_chain_release_head(fc_descriptor_id_t head); // 释放链头一个 desc+buffer，返回新链头
fc_descriptor_id_t fc_descriptor_chain_release_all(fc_descriptor_id_t head);  // 沿链释放本命令全部 desc+buffer（非清空池），返回 INVALID
fc_buffer_id_t fc_buffer_alloc(void);                   // 申请 Buffer（返 ID）
fc_status_t   fc_buffer_release(fc_buffer_id_t id);     // 释放 Buffer
uint8_t      *fc_buffer_id_to_real_buffer(fc_buffer_id_t); // ID→指针，返回 USER 数据区首地址（跳过 META+CRC 共 8B）
uint8_t      *fc_buffer_get_base(fc_buffer_id_t);       // ID→指针，返回 CW 基址（META 开头，FC 数据路径内部使用）
uint8_t      *fc_buffer_get_meta(fc_command_t *, uint8_t cw_index); // 指定 CW 的 META 指针（地址校验）
uint8_t      *fc_get_cmd_data_ptr(fc_command_t *cmd, uint8_t cw_index);     // 指定 CW 的 USER 指针
uint32_t      fc_get_cmd_cw_bitmap(const fc_command_t *cmd);                // 从 desc 链重建 flat CW 位图
uint8_t       fc_get_cmd_ecc_max(const fc_command_t *cmd);                  // 命令级聚合 ECC（链上最大 ecc_bits）
fc_status_t   fc_submit(fc_command_t *cmd);             // 提交到 SQ 或 HSQ（根据 high_priority）
fc_status_t   fc_service(void);                         // 调度执行（上层 while(1) 轮询）
fc_status_t   fc_poll_completion(fc_command_status_t *); // 从 CQ 取完成项
fc_status_t   fc_command_release(fc_command_t *cmd);    // 释放命令 + Descriptor 链 + 各 CW buffer
fc_status_t   fc_get_resource_stats(fc_resource_stats_t *); // 资源使用统计
```

> **移除**：`fc_real_buffer_to_buffer_id()`（指针→ID 反向转换）——FC 数据引用统一走 buffer_id，无调用者，连同其 internal 一并删除。`fc_descriptor_alloc()`（孤儿声明）同步删除。

> **v13 策略层上移**：`fc_command_configure_descriptors()`（按掩码一次性建链）、`fc_write_cmd_data()` / `fc_read_cmd_data()`（按 CW 拷贝数据）**已从 FC 删除**，改由 FTL 层持有
> （`ftl_cmd_configure_descriptors()` / `ftl_cmd_write_data()` / `ftl_cmd_read_data()`，见 `include/ftl/ftl_fc_interface.h`）。
> 判据是"硬件 FCC 寄存器表"：硬件 FCC 既不提供"按掩码建链"寄存器（描述符链是固件填的），也不提供"数据拷贝"寄存器（数据是固件 DMA 搬的）——
> 这两件事在行业里都在**固件（FTL）侧**。FC 只保留槽位 / 描述符 / 队列 / 数据路径。详见 §26 与 `docs/changelog/fc_policy_layer_uplift.md`。
>
> **建链（v11 引入，v13 后 FC 只剩机制层，v17 收敛为一条）**：`fc_command_configure_descriptor()` 是 FC 的**唯一建链入口**——只做"分配 desc + buffer、填 PAA/config/meta、挂到链尾"。
> 上层按自己算出的 CW 集合循环调用即可，**链序即数据布局顺序**（FTL 侧拷贝按此顺序遍历链）。重复追加同一 `n` 是 `ASSERT`。
> 离散 CW 读（Read Recovery）由 FTL 侧 `ftl_cmd_configure_descriptors()` 按**形参 `cw_bitmap`** 展开为逐条 `fc_command_configure_descriptor`（v28 起位图是形参，不再是 `cmd->param` 字段）。
>
> **v17 合并**：v13~v16 期间此入口是「`fc_command_attach_cw`（公共壳）→ `_internal`（段校验）→ `_internal`（机制体）」三层接力，三层签名逐字相同、中间层纯直通。v17 合并为一条 `fc_command_configure_descriptor`，机制体落在 `src/fc/fc_interface.c`，并强制**全部校验前置于出池**。
>
> **v16 失败语义**：建链**失败时链上可能已有 desc（含本条）**——FC 不做任何回滚。
> 「已分配的 desc 全部在链上」由 `fc_command_configure_descriptor()` 保证
> （分配 desc → 填字段 → 挂链 → 分配 buffer），调用方沿链释放即完整回收：
> `cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);`。
> 数据搬运**不在这里**——它由 FTL 在链建好之后、且仅对写操作执行（`ftl_cmd_write_data`），
> 否则「建链失败 → 整链释放」会让已搬的数据白做。
>
> **v12 行地址入参**：`fc_command_configure_descriptor()` 的 `row_paa` = 该 plane 的行地址（CW 位忽略）。
> 上层可给每个 plane 传不同行地址，从而让 multi-plane 命令的两个 plane 指向**不同 block**（Policy B）。
> 同一 plane 的所有 CW 必须传同一 `row_paa`。**plane 号取自 PBA 自己的 plane 位，不是数组下标**（见 `ftl_cmd_setup_ppa_pair`）。

### 2.10 命令生命周期

```
1. fc_command_alloc()           → 获取 fc_command_t*
2. 上层配置 command 字段         → 设置 operation/ch/ce/lun/ldpc/random/raid 等（**v23 起 config.cw 与建链无关**）
3. 建 desc 链（v13：策略层在 FTL）
   · ftl_cmd_configure_descriptors(cmd, cw_bitmap) → 按**形参** cw_bitmap 展开 N 条 fc_command_configure_descriptor()，申请 N 个 Descriptor + Buffer 并串联
     （N = popcount(bitmap)；**字面位图**——v27 起没有「0 = 全部」这回事，bitmap == 0 由
      ftl_cmd_resolve_cw_bitmap() 的 ASSERT 拦下。整段访问须调用者先算
      fc_command_plane_span_cw_bitmap(cmd) 当实参传，或改用 addr_to_cmd / ftl_cmd_setup_ppa / _pair）
     · 任一条失败 → 本函数**自己回滚整条链**（desc+buffer 还池，desc_id 复位 INVALID）→ 返回 FTL_ERROR
   · 或 直接逐条 fc_command_configure_descriptor(plane, cw, row_paa) → 上层自己决定访问集合 + 每 plane 行地址，链序即数据布局序
3b.（仅写操作）ftl_cmd_write_data(cmd, data, len) → 遍历链把数据搬进各 CW 的 buffer USER 区
4. fc_submit(command)            → 根据 high_priority 入 SQ 或 HSQ
5. while(1) { fc_service(); ... }  → 调度器取命令→nand_dispatch_submit→nand_dispatch_poll
6. fc_poll_completion(&status)   → 非空则获取完成信息
7. 上层根据 status 判断结果（读操作在此之后 ftl_cmd_read_data 拷出）
8. fc_command_release(command)   → 回收 Command + Descriptor 链 + 各 CW Buffer
```

### 流程图：命令生命周期（图 1）与 fc_service() 主循环（图 2）

**图 1 — 命令生命周期（FTL 视角）**

```text
FTL 调用                    FC 内部                         NAND 层
─────────────────────────────────────────────────────────────────────
fc_command_alloc()
  └─ 取 command_pool 空槽，状态 ALLOCATED
FTL 填字段: operation/ch/ce/lun/cw/meta/pba（v28：cw_bitmap 不再是命令字段，作建链实参）
  └─ 如配置 config.high_priority = 1 → 走 HSQ
ftl_cmd_configure_descriptors(cmd, cw_bitmap)   ← FTL 侧（v13 前为 fc_command_configure_descriptors）
  └─ 按形参 cw_bitmap 展开为逐条 fc_command_configure_descriptor()，分配 desc + buffer 并串联（策略层）
     · 失败 → fc_descriptor_chain_release_all() 回滚整条链，返回 FTL_ERROR
     · 或直接循环 fc_command_configure_descriptor(plane, cw, row_paa) 自行组装（机制层，FC 侧）
ftl_cmd_write_data(cmd, data, len)  ← FTL 侧（v13 前为 fc_write_cmd_data）；仅写操作
  └─ 遍历 desc 链 memcpy 数据到各 buffer USER 区
fc_submit(cmd)
  └─ fc_submit_internal → 入队 SQ 或 HSQ（不执行）
      ┌────────────── while(1) { fc_service(); } ──────────────┐
      │  单次 fc_service():                                    │
      │    ① nand_dispatch_poll 推进 NAND                      │
      │    ② fc_poll_fill 取命令（填充 NAND per-LUN FIFO）      │
      │    ③ fc_data_path_encode（写命令）                     │
      │    ④ 构建 request → nand_dispatch_submit              │
      └────────────────────────────────────────────────────────┘
fc_poll_completion(&status)
  └─ 从 CQ 取完成项（12B：id/desc_id/module/status/param）
FTL 按 status.module 路由处理；遍历 desc 链读每 CW 状态/ECC
fc_command_release(cmd)
  └─ 回收 command + desc 链 + buffer（挂回空闲链表）
```

命令状态机：`FREE → ALLOCATED → QUEUED → ACTIVE → COMPLETED → FREE`（越界直接出错）。

**图 2 — fc_service() 主循环单步**（`fc.c::fc_poll_internal`，每次调用先推进 NAND 再补充新命令）

```text
fc_service()
  │
  ├─ ① nand_dispatch_poll(fc_nand_completion_sink, &sink_ctx)
  │     └─ 遍历全部 (ch,ce,lun)，每 LUN 每轮恰好 step 一次；每产出一条完成就回调 sink
  │          ├─ 有完成（返回条数 != 0）→ sink 内逐条当场消费：
  │          │     fc_complete_from_nand()          ← 回写 desc + 数据路径 + CQ
  │          │     [CONFIG_FC_RAID] 若完成的是 stripe 数据命令
  │          │       → fc_stripe_release_parity()
  │          │     抽干后返回（仅 CQ 满这一 FC 侧故障才报 IO_ERROR；
  │          │     NAND 命令自身失败不算——状态已在 CQ entry 里）
  │          └─ 无完成（返回 0）→ 继续 ②
  │
  ├─ ② fc_poll_fill(controller)                   ← 一次填充 pass（v8）
  │     ├─ 快照 HSQ 数 → 扫 HSQ（need = 链长）→ room 够则 submit/submit_chain，否则挂起 + 回推
  │     ├─ 快照 SQ 数  → 扫 SQ （need = 链长）→ room 够则 submit/submit_chain，否则挂起 + 回推
  │     │                （HSQ/SQ 共用同一张挂起表：HSQ 挂起的 LUN，SQ 本轮也跳过）
  │     └─ 两条队列皆空 → QUEUE_EMPTY；无 room → NOT_READY
  │
  ├─ 分支 A：单命令 → fc_poll_dispatch_single()
  │     ├─ 校验 QUEUED → 置 ACTIVE
  │     ├─ page write → fc_data_path_encode()（Stage 0 写 META）
  │     ├─ raid=ENCODE → fc_stripe_submit()        ← 图 12
  │     ├─ fc_build_dispatch_request() → request：
  │     │    ├─ 单 plane：按 desc.paa 置 cw_bitmap 的 bit(plane*CW_NUM+cw) + 填 cw_buffers[n]（n = flat 号）
  │     │    ├─ multi_plane：同上逐 desc；plane_paa[] 为命令 plane 集合内每个 plane 填行地址（空 plane 也填）
  │     │    ├─ ERASE：只填 plane_paa[plane]，**位图留空集**（v28 不再伪造 CW0 位），无 buffer
  │     │    └─ SET/GET_FEATURES：cw_buffers[plane*CW_NUM] = g_fc_return_staging[command_id]
  │     └─ nand_dispatch_submit(&request)          ← 按 room 追加一槽即返回
  │
  └─ 分支 B：链（count >= 2）→ fc_poll_dispatch_chain()
        ├─ 逐条：校验 / 置 ACTIVE / page write 则 encode
        ├─ 逐条 fc_build_dispatch_request() → reqs[0..count-1]
        └─ nand_dispatch_submit_chain(reqs, count) ← 整链原子追加
```

**要点**：一次 `fc_service()` 要么把本轮所有完成抽干（`nand_dispatch_poll` 全遍历 + sink 逐条消费），要么走一次填充 pass——两件事不交叉。填充只把命令放进目标 LUN 的 FIFO，实际执行由后续 `fc_service()` 的 `nand_dispatch_poll()` 逐 LUN 推进（每个 LUN 每轮恰好 step 一次，完成经 sink 逐条回一份 completion）。两条分支的失败兜底统一走 `fc_poll_fail_command()`（合成 NAND_FAIL 完成项）；**`nand_dispatch_submit*` 因填充前已查 room，生产路径上不会返回 `NOT_READY`（出现即 `ASSERT`）**，`NOT_READY` 的"忙"语义改由 `nand_dispatch_lun_free()` 前置表达。行为不对称：META/PAA 预填只在单命令路径（分支 A），链式路径不做。

---

## 3. NAND 底层模块详细设计

### 3.1 微操作引擎 (nand_engine)

设计理念：**软件微码（Software Microcode）**。每条 UOP（Micro-Operation）自包含、不查外部表、不含隐式逻辑，模仿企业级 SSD 控制器硬件 micro-sequencer 的设计。

```
企业级硬件微码（Intel / Samsung / Kioxia）:
  INST_0: SET_CMD(0x00)    ← 指令自带命令值
  INST_1: SET_ADDR(col0)   ← 值随指令走
  INST_2: SET_CMD(0x30)    ← 又一个命令，自有值
  INST_3: WAIT_RB#         ← 纯硬件等待
  ...

软件微码（当前实现）:
  { COMMAND,   NAND_CMD_READ_PAGE_SETUP    },  // 自包含
  { COLUMN_0,                            0U  },  // 列地址
  { COLUMN_1,                            0U  },
  { ROW_0,                               0U  },
  { ROW_1,                               0U  },
  { ROW_2,                               0U  },
  { COMMAND,   NAND_CMD_READ_PAGE_CONFIRM },
  { DELAY_US,  NAND_TIME_TWB_US           },  // 显式 tWB
  { WAIT_READY,                          0U  },  // 纯 RB# 轮询
```

三段式演进：

| 阶段 | UOP 数量 | 设计问题 |
|------|---------|---------|
| Phase S 前 | 15 | `COMMAND_PRIMARY/SECONDARY` 查 `operation_map` 表，命令值不直观；隐式延时（tWB/tWHR/tADL）藏在 handler 内；`CHECK_READY` + `uop_index--` hack |
| Phase S 改后 | 13 | 命令统一 `COMMAND` 直写；延时显式 `DELAY_US`；条件跳转 `JUMP_IF_NOT_READY` 替代引擎 hack；WAIT_READY 纯化 |

**UOP 类型**（13 种）：

| UOP | 说明 | 职责 |
|-----|------|------|
| WAIT_READY | 纯 RB# GPIO spin-wait | 不含 tWB/tADL 保护，不查表 |
| COMMAND | 发送命令字节 | operand 直写命令值，自包含 |
| COLUMN_0/1 | 发送列地址 | — |
| ROW_0/1/2 | 发送行地址 | operand 选 row[] 索引（多 plane） |
| DATA_IN | 数据写入 NAND | — |
| DATA_OUT | 从 NAND 读取数据 | — |
| DATA_OUT_STATUS | 读取状态字节 | 纯读 DQ，不含延时 |
| JUMP_IF_NOT_READY | 条件跳转微指令 | operand=相对偏移步数，引擎直接处理 |
| CHECK_FAIL | 检查 Fail 位 | nand_status & 0x01，失败终止序列 |
| DELAY_US | 固定微秒延时 | delay_us(operand)，无硬件依赖 |
| END | 序列终止 | — |

**删除的 UOP**：

| 删除的类型 | 替代方案 | 原因 |
|-----------|---------|------|
| ~~COMMAND_PRIMARY~~ | `COMMAND` + operand 直写 | 查 operation_map 表，不直观、不自包含 |
| ~~COMMAND_SECONDARY~~ | `COMMAND` + operand 直写 | 同上 |
| ~~READ_STATUS~~ | `COMMAND(70h) → DELAY_US → DATA_OUT_STATUS` | 复合操作拆为三步单责 UOP |
| ~~CHECK_READY~~ | `JUMP_IF_NOT_READY(-3)` | 引擎不再需要 uop_index-- hack |

**与主控微码对比**：

| 特性 | 企业主控硬件微码 | 软件微码（当前） |
|------|----------------|----------------|
| 命令值来源 | 微指令内嵌 | `COMMAND` operand 直写 |
| 固定延时 | 延时微指令 | `DELAY_US` |
| 条件跳转 | `JUMP_IF_NOT_READY` | 引擎内置，相对偏移 |
| tWB 保护 | 显式 DELAY 在 CONFIRM 后 | `DELAY_US(tWB)` + `WAIT_READY` |
| tWHR | 显式 DELAY 在 70h 后 | `DELAY_US(tWH)` 在序列中 |
| RB# 轮询 | 纯引脚电平检测 | `WAIT_READY` 纯 spin-wait |
| 引擎层 | 无 UOP 特判 | 除 `WAIT_READY` / `JUMP` 超时外无特判 |
| 无条件跳转 | 有 | 无（当前无需求） |
| 子程序调用 | 有 | 无（当前无需求） |

#### WAIT_READY — 纯 RB# GPIO 轮询

WAIT_READY handler（`nand_fmc_exec_wait_ready`）在 handler 内部完成 RB# GPIO 轮询 + 超时等待，不返回 NOT_READY，**不依赖引擎的 DWT 超时**。

流程（简化后）：
1. 读 RB# GPIO：LOW → NAND busy → 1µs/步 spin-wait 直到 HIGH 或超时
2. RB# HIGH → 直接返回 OK（NAND 已 ready）

**不再包含 tWB 保护**。tWB（CONFIRM 后最长 1µs 等待 NAND 拉低 RB#）已由 UOP 序列中的 `{ DELAY_US, NAND_TIME_TWB_US }` 显式保证。

#### JUMP_IF_NOT_READY — 条件跳转微指令

JUMP_IF_NOT_READY 由引擎直接处理，不走 handler 表（handler 表项为 NULL）：

1. 检查 `nand_status & NAND_STATUS_READY`：ready 则推进到下一 UOP
2. busy 则检查 DWT 超时，未超时则 `uop_index -= operand` 跳回重发 70h 轮询序列

典型的 70h 状态轮询 4 步循环：

```
步 N+0: { NAND_UOP_COMMAND,            0x70 }    // 发 70h
步 N+1: { NAND_UOP_DELAY_US,             1U }    // tWHR
步 N+2: { NAND_UOP_DATA_OUT_STATUS,     0U }     // 读状态
步 N+3: { NAND_UOP_JUMP_IF_NOT_READY,   3U }     // busy 则跳回步 N
```

每次循环完整重发 70h、等 tWHR、读状态、检查 ready，无需引擎层特殊逻辑。

#### CHECK_FAIL — 使用规则

CHECK_FAIL 检查 `completion->nand_status & 0x01`（NAND 状态寄存器的 FAIL bit），
失败则返回 `NAND_RESULT_OPERATION_FAILED`，引擎终止序列。不是所有 OP 都需要它：

| 有 CHECK_FAIL | 原因 |
|-------------|------|
| 所有含 10h/D0h 的程序/擦除操作 | CONFIRM 后 NAND 可能内部烧录/擦除失败 |
| SET_FEATURES（写 ONFI 寄存器） | 写寄存器可能被 NAND 拒绝 |
| RESET | 复位后检查 NAND 状态是否正常 |
| 所有含 30h 的 READ 操作（含 MULTI_PLANE_READ） | 30h 后页加载可能内部出错 |
| INTERNAL_READ_FOR_DATA_MOVE（35h） | 内部搬移读取阶段可能出错 |
| CACHE_READ_SEQ/LAST/RANDOM（31h/3Fh） | Cache 读序列中 NAND 内部可能出错 |

| 无 CHECK_FAIL | 原因 |
|-------------|------|
| GET_FEATURES / READ_ID / READ_PARAM_PAGE / READ_UNIQUE_ID | 读芯片信息/寄存器，不触发内部操作，无"失败"概念 |
| RANDOM_DATA_READ | 05h-E0h 读已加载的页缓冲，不触发 NAND 内部操作 |
| READ_STATUS / READ_STATUS_ENHANCED | 读状态寄存器本身，查自己无意义 |
| BLOCK_UNLOCK/LOCK/LOCK_TIGHT/LOCK_STATUS | 锁操作由 NAND 内部逻辑完成，不通过 FAIL bit 报告 |

**例外**：PROGRAM_FOR_INTERNAL_DATA_MOVE 有 CHECK_FAIL，因为它是 85h-addr-10h 的完整编程序列（有 10h CONFIRM）。

#### 隐式延时显式化

此前 tWB、tWHR、tADL 等固定延时分散在各 handler 内部，UOP 序列不可见。改后全部显式化：

| 隐式延时 | 原位置 | 现位置 |
|---------|--------|--------|
| tWB（CONFIRM→RB# 拉低，1µs） | WAIT_READY handler 内 `delay_us(2U)` | `{ DELAY_US, NAND_TIME_TWB_US }` 在 CONFIRM 后 |
| tWHR（70h→DQ 有效，1µs） | DATA_OUT_STATUS handler 内 `delay_us(twh_us)` | `{ DELAY_US, NAND_TIME_TWH_US }` 在 70h 后 |
| tADL（11h 切换 plane，1µs） | WAIT_READY 伪装（RB# 不变） | `{ DELAY_US, 1U }` 在 11h 后 |

#### 引擎执行模型

```
nand_engine_execute()  —— 同步模式，阻塞直到完成
nand_engine_start()    —— 异步模式，复位游标并推进第一步（state 须先绑定 backend/request/completion）
nand_engine_step()     —— 异步模式，推进一步，返回 NOT_READY 表示需继续轮询
```

### 流程图：nand_engine 微码状态机（图 6）

`nand_engine_step()` 的核心循环（每 step 可消费多个 UOP）：

```text
step(state)
  │
  └─ for(; uop_index < MAX_UOPS(64);)
       │
       ├─ uop.type == END
       │    └─ 置 active=0，返回 transfer_status（命令结果）
       │
       ├─ uop.type >= UOP_COUNT → INVALID_ARGUMENT
       │
       ├─ uop.type == JUMP_IF_NOT_READY（引擎直接处理，不经 handler）
       │    ├─ nand_status & READY bit6 → uop_index++（推进）
       │    └─ busy → wait_latch() 锁存起点 → wait_timeout() 双超时判定：
       │         DWT elapsed > busy_timeout_us  或
       │         wait_polls ≥ busy_timeout_us
       │           ├─ 超时 → active=0，返回 TIMEOUT
       │           └─ 未超时 → uop_index -= operand（回跳），返回 NOT_READY
       │
       ├─ 其余 UOP → handler = backend->handlers[uop->type]
       │    ├─ handler == NULL → INVALID_ARGUMENT
       │    └─ WAIT_READY 首次进入：wait_latch() 记录 DWT 起点（用独立 wait 状态）
       │         result = handler(...)
       │           ├─ OK → uop_log_ok() 白名单日志；uop_index++，清零 wait 状态，continue
       │           ├─ NOT_READY → WAIT_READY 额外 wait_timeout() 检测；让出
       │           └─ 其他 → active=0，返回错误
       │
       └─ 循环跑满无 END → 非法序列
```

**三个 static helper**（`nand_engine.c`）：`wait_latch` 一次性锁存 DWT 起点（入参指定 `wait` / `jump` 状态组）、`wait_timeout` 饱和递增计数后判双条件、`uop_log_ok` 按类型白名单打成功日志。抽出的目的是让 `step` 只留解释器骨架——END / JUMP 是控制流（不查表），其余一律走 handler 表；同时也消掉了原先 JUMP 与 WAIT_READY 两处逐字重复的锁存/超时块。

> **为什么 JUMP 与 WAIT_READY 用两组独立状态**（`wait` / `jump`，`nand_engine_wait_t`）：JUMP 回跳体会重跑若干**返回 OK** 的 UOP（70h / DELAY_US / DATA_OUT_STATUS），而引擎 OK 分支会清零 `wait`；若二者共用同一组，`jump` 的计数每轮回跳都被清零、DWT 起点也被重新锁存，双条件超时永不触发（NAND 卡忙时死循环）。`wait` 归 WAIT_READY——该 UOP 未就绪时原地重入、无前置 OK-UOP 复位，计数可正常累加。

**协作式让出**：`NOT_READY` 是"让出点"——引擎不阻塞，上层 `while(NOT_READY) step()`（同步 `nand_engine_execute`）或由 dispatch 轮询推进（异步 `start/step`）。

### 流程图：70h 状态轮询循环（图 7）

几乎所有 uop 序列开头都是这个循环，`operand=3` 表示跳回 3 步前重发：

```text
步 N+0: { COMMAND          0x70 }    ← 发 70h（读状态寄存器）
步 N+1: { DELAY_US           1U }    ← tWHR（70h → DQ 有效）
步 N+2: { DATA_OUT_STATUS   0U  }    ← 读状态字节（含 READY bit6 / FAIL bit0）
步 N+3: { JUMP_IF_NOT_READY 3U  }    ← busy → uop_index -= 3 回跳 N+0
                                        ready → 推进到下一 UOP
        （循环期间 DWT + 轮询计数双超时，超时返回 TIMEOUT）
```

读命令 30h 后的页加载、编程 10h 后的 tPROG、擦除 D0h 后的 tBERS 都靠这个循环轮询到 ready；`CHECK_FAIL` 检查 FAIL bit 失败即终止序列（图 6 的 OPERATION_FAILED 路径）。

### 3.2 FMC 硬件后端 (nand_fmc)

实现全部 15 个 UOP handler，通过函数指针表挂载到 `nand_engine_backend_t`。

硬件访问方式：通过 `nand_fmc_config_t` 配置的 volatile 指针访问 FMC 的 Command/Address/Data 窗口：

```c
typedef struct {
    volatile uint8_t *data;     // FMC 数据窗口地址
    volatile uint8_t *command;  // FMC 命令窗口地址
    volatile uint8_t *address;  // FMC 地址窗口地址
    nand_fmc_timing_t timing;  // FMC 时序参数
} nand_fmc_config_t;
```

#### DATA_IN / DATA_OUT — PIO/DMA 双模

DATA_IN（CPU→FMC→NAND，PROGRAM 方向）和 DATA_OUT（NAND→FMC→CPU，READ 方向）handler 支持编译态和运行态双模切换：

| 模式 | 配置 | 行为 |
|------|------|------|
| PIO（默认） | `CONFIG_NAND_DMA=0` | CPU 逐字节 `*config->data = buf[i]` |
| DMA（可选） | `CONFIG_NAND_DMA=1` | 三态非阻塞：①done→OK ②busy→NOT_READY ③首次→启动DMA_IT |

运行态可通过 `g_nand_dma_enable` 全局变量切换（PIO↔DMA，NAND 空闲时安全）。

### 流程图：FMC 后端 UOP 执行（图 8）

`g_nand_fmc_handlers[NAND_UOP_COUNT]` 函数指针表（`src/nand/nand_fmc.c`）：

```text
┌───────────────────────────────────────────────────────────────┐
│ [WAIT_READY]      nand_fmc_exec_wait_ready  RB# GPIO spin-wait │
│ [COMMAND]         nand_fmc_exec_command     写 FMC 命令窗口     │
│ [COLUMN_0/1]      nand_fmc_exec_column_x    写列地址（低/高字节）│
│ [ROW_0/1/2]       nand_fmc_exec_row_x       写行地址（operand   │
│                                               = row[] 索引，多  │
│                                               plane 时选 plane）│
│ [DATA_IN]         nand_fmc_exec_data_in     CPU→FMC→NAND 数据   │
│ [DATA_OUT]        nand_fmc_exec_data_out    NAND→FMC→CPU 数据   │
│ [DATA_OUT_STATUS] nand_fmc_exec_data_out_status 直读 70h 状态   │
│ [JUMP_IF_NOT_READY] NULL（由引擎直接处理，见图 6/7）            │
│ [CHECK_FAIL]      nand_fmc_exec_check_fail  检查 FAIL bit       │
│ [DELAY_US]        nand_fmc_exec_delay_us    delay_us(operand)   │
│ [END]             （无 handler，引擎处理）                       │
└───────────────────────────────────────────────────────────────┘
         ▲
         │ handler 通过三个 volatile 窗口访问 FMC：
         │   *config->data / *config->command / *config->address
         └── 上下文 nand_fmc_config_t（Phase V 已数组化 per (ch,ce,lun)）

WAIT_READY handler 细节（图 6 与之共用超时字段）：
  读 RB# GPIO
    ├─ LOW  → while LOW: polls++；polls ≥ busy_timeout_us 超时；delay_us(1)
    └─ HIGH → 直接返回 OK（极短 busy / 无 busy）
  tWB 保护不在 handler 内——由 uop 序列中 CONFIRM 后的 {DELAY_US, 1} 显式保证

DATA_IN / DATA_OUT 双模：
  PIO（默认）   CPU 逐字节写/读 FMC 窗口
  DMA（可选）   三态非阻塞：①done→OK ②busy→NOT_READY ③首次→启动 DMA_IT
```

**DMA 不可用结论**：STM32H743 的 FMC NAND 控制器**没有专用 DMA 请求线**（DMAMUX1 映射表 0~137 中无 FMC/NAND 条目）。DMA memory-to-memory 模式下 DMA2 无法与 FMC NAND 等待状态握手（FMC 在 AXI 域，DMA2 在 AHB 域）。FMC NAND 只能走 PIO 逐字节访问。如需 DMA + 硬件 ECC NAND 控制器，建议换 NXP i.MX RT 系列（GPMI 控制器内置 DMA + BCH）。

### 3.3 ONFI 1.0 命令表 (onfi_1_0)

29 种 ONFI 操作，每种操作预编译为 UOP 数组。结构：

```c
typedef struct {
    uint8_t  operation;         // OP_* 常量
    const char *name;           // 调试名称
    uint8_t  command_1/2;       // 命令字节
    uint8_t  address_cycles;    // 地址周期数
    uint8_t  data_input_cycles; // 数据输入周期
    uint8_t  valid_while_*_busy; // 并发约束
    const nand_uop_t *uops;     // UOP 序列
    nand_operation_timing_t timing; // 超时参数
} nand_operation_map_t;
```

`OP_*` 常量直接作为 `g_nand_operation_map[OP_COUNT]` 数组下标，O(1) 查表。

### 3.4 NAND FC 接口 (nand_mt29f4g08abadawp)

芯片级封装，对上提供：

```c
uint8_t nand_fc_execute(operation, &request, &completion);  // 同步
uint8_t nand_fc_start(transaction, operation, &request);    // 异步 start
uint8_t nand_fc_step(transaction, &completion);             // 异步 step
uint8_t nand_fc_read_id(id[5], &completion);
uint8_t nand_fc_reset(&completion);
```

关键类型：

```c
// 请求结构
typedef struct {
    uint16_t column;
    uint32_t row;
    uint8_t *data;
    uint16_t data_length;
} nand_fc_request_t;

// 完成结构
typedef struct {
    uint8_t execution_status;  // NAND_RESULT_*
    uint8_t nand_status;       // 70h 状态值
} nand_fc_completion_t;

// 事务状态（异步模式）
// backend / active 不另存，统一走 engine.backend / engine.active；
// engine.request / engine.completion 由 nand_dispatch_task_load 一次性绑定到下面两个存储体
typedef struct {
    nand_engine_state_t engine;
    nand_command_request_t request;
    nand_command_completion_t completion;
} nand_fc_transaction_t;
```

---

## 4. FC ↔ NAND 调度桥接层 (nand_dispatch)

### 4.1 设计目的

在 FC 和 NAND 底层之间建立调度桥接，负责：
- 按 Channel/CE/LUN 路由命令到对应物理 LUN
- 管理每个 LUN 的**待处理 FIFO**（深 `NAND_DISPATCH_CHAIN_DEPTH` = 4，每槽 = 一条命令）
- 处理多 CW 命令的分段执行（命令内 CW 链，与命令链无关）
- 将 NAND 完成状态转换为 FC 完成状态

### 4.2 架构

**v8（per-LUN FIFO，2026-10-06）**：

```
FC (fc_service)
  │
  ├─ fc_poll_fill()               → 一次扫描 SQ/HSQ，按 LUN 保序挑出可下发命令（含链）
  ├─ 预解析 desc 链为自包含请求
  │     PAA、buffer 指针在 FC 侧解析
  ├─ nand_dispatch_submit()/_chain() → 按 room 追加到目标 LUN FIFO（不再回调 FC）
  └─ nand_dispatch_poll()         → 遍历各 LUN FIFO，每 LUN 推进一条
       │
       ├─ 队头未启动 → nand_fc_start() → nand_engine_start() → nand_fmc UOP
       ├─ 已启动     → nand_fc_step()  → nand_engine_step()  → 非阻塞推进
       └─ 完成 → 构建 nand_dispatch_completion_t + 出队（head++/count--）
```

### 4.3 多 CW 处理策略

**PROGRAM（写）**：
1. 首次执行：遍历 Descriptor 链，将所有 CW 的 Buffer 数据拷贝到 LUN Page Buffer
2. 发送 `OP_NORMAL_PROGRAM_PAGE`（80h-地址-数据-10h）

**READ（读）**：
1. 整页先「只加载、不输出」进 NAND 页缓冲（`OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT`）
2. 再按 CW 位图逐 CW 取数（`OP_RANDOM_DATA_READ` 05h-列-E0h；多 plane 用 06h 版）
3. 每个 CW 的数据写入对应 Descriptor 的 Buffer

### 流程图：nand_dispatch poll 遍历模型（图 9）

**v8 起无 Phase 1 / Phase 2 之分**：`nand_dispatch_poll()` 只保留骨架
（全遍历通道 → CE → LUN），每 LUN 每轮恰好单步推进一条命令（完成经 sink 逐条回一份 completion），
具体逻辑拆成 static 辅助函数：

| 职责 | 函数 | 说明 |
|---|---|---|
| **Per-LUN FSM** | `nand_dispatch_lun_step` | 单步：ISSUED 预检 → 驱动 → CW 链 → scatter → 出队回完成 |
| | `nand_dispatch_issued_advance` | ISSUED 忙期 gate + pair_op 切换 |
| | `nand_dispatch_lun_drive` | started/step 单步推进 + ISSUE→ISSUED |
| **Command issue** | `nand_dispatch_task_start` | 构建 request + 日志 + 发起事务 |
| | `nand_dispatch_task_fc_start` | `nand_fc_start` + 置初始相位 |
| **Data path** | `nand_dispatch_task_cw_chain` | 命令内多 CW 链式续读 |
| | `nand_dispatch_scatter_page_buffer` | 三种拆页合并分派 |

**删除**：per-channel FIFO（`g_nand_channel_queues`）、Phase 2 Arbiter（`phase2_dequeue`）、
`nand_dispatch_queue_consume_match`、`nand_dispatch_chain_advance`、`nand_dispatch_phase1_lun`、
`nand_dispatch_task_build_request`（并入 `task_start`）。

```text
nand_dispatch_poll(sink, ctx)            ← 骨架：全遍历 + 每 LUN 单步（不早退）
for channel = 0..CH_NUM-1
  for ce, for lun:
      queue = &g_nand_dispatch_queues[ch][ce][lun];  count==0 → 跳过
      lun_ctx = { &completion, queue }               ← completion 槽复用
      out = lun_step(&lun_ctx)                       ← 每 LUN 只推进一步
      out == DONE → sink(&completion, ctx)           ← 完成当场交出，不缓存批量

nand_dispatch_lun_step(ctx)             ← 单命令推进（一次调用只处理 tasks[head]）
  │
  ├─ phase == ISSUED 时先过 issued_advance()：
  │     min_busy_us 未到 → WAIT（等最短忙期）
  │     到点 → pair_op 切换（LOAD→FETCH / ISSUE→WAIT）→ task_start() → phase=FETCH
  │
  ├─ lun_drive()：started 判定
  │     started==0 → task_start()（req_table 构建 + LOGD + nand_fc_start）
  │                  （ISSUE 型置 phase=ISSUE；否则 FETCH）
  │                  ASYNC → WAIT
  │     started!=0 → transaction.engine.active ? nand_fc_step
  │                                 : 用已存的 execution_status
  │     ASYNC → WAIT（等下一轮）
  │     phase==ISSUE → task_complete → phase=ISSUED
  │                    → 记 DWT 起点 → WAIT（LUN 仍在内部操作）
  │
  ├─ task_cw_chain()：CW chaining（多 CW READ）→ task_complete
  │     若 (普通读/OTP读/随机读) 且还有下一 CW：
  │       cw_index++ → operation=OP_RANDOM_DATA_READ
  │       → 列地址 = cw_index×(PAGE_SIZE/4) → nand_fc_start
  │     ASYNC → WAIT（本轮到此为止，仍占着 tasks[head]）
  │
  ├─ scatter_page_buffer()：page_buffer 拆回
  │     （CACHE 读 / OP_READ_PAGE_FETCH / MP_WITH_DATA_OUT）
  │
  └─ 本条命令完成 → *completion = tasks[head].completion
                    head++（模 DEPTH）, count--, phase=IDLE
                    → DONE（有完成，交给 sink）

全部遍历完 → 返回完成条数（0 = 无完成）
```

**要点**：一条命令在 FIFO 里"占住队头"直到它自己完成——链因此天然不可被同 LUN 的后续命令插入
（后续命令排在链后，FIFO 严格顺序）。每个 LUN 每轮最多完成一条；poll **不早退**，每个 LUN 每轮
恰好 step 一次，完成经 sink 逐条交出。但**每条命令各回一份 completion**（旧模型整链只回链尾一份，中间命令泄漏）。

**重构注意**：`nand_dispatch_lun_step` 的返回值依赖三个 outcome 枚举——`nand_dispatch_lun_outcome_t`
（WAIT / DONE）、`nand_dispatch_start_outcome_t`（ASYNC / SYNC）、
`nand_dispatch_cw_outcome_t`（ASYNC / DONE）。req 构建 handler 无返回码（构建不存在失败路径），
故 `start_outcome_t` 只需区分"启动即返回"（ASYNC）与"start 内同步完成"（SYNC）；
`nand_fc_start` 自身的错误码不是失败出口，而是"带错误状态的完成"，继续走完成处理链。

### 4.4 接口

```c
uint8_t nand_dispatch_init(void);
void    nand_dispatch_set_armed(uint8_t armed);            /* 放行异步提交（nand_init 成功后置 1） */
uint8_t nand_dispatch_submit(const nand_dispatch_request_t *request);                       /* 单命令：按 room 追加到 LUN FIFO */
uint8_t nand_dispatch_submit_chain(const nand_dispatch_request_t *requests, uint8_t count); /* 链：整批原子追加，room 不足则无副作用返回 NOT_READY */
uint8_t nand_dispatch_lun_free(uint8_t ch, uint8_t ce, uint8_t lun);                        /* 查询 LUN FIFO 剩余槽数（0..DEPTH） */
typedef void (*nand_dispatch_sink_t)(const nand_dispatch_completion_t *completion, void *ctx);
uint8_t nand_dispatch_poll(nand_dispatch_sink_t sink, void *ctx);  /* 全遍历，每完成回调一次；返回条数 */
```

`nand_dispatch_request_t` 是 FC → NAND 的完整请求结构，包含命令 ID、操作码、通道路由、page_type、参数和预解析的 CW 信息：

```c
typedef struct {
    uint16_t command_id;
    uint8_t  operation;
    uint8_t  ch, ce, lun;
    uint32_t cw_bitmap;                  // 选中的 CW 集合：bit n = plane*CW_NUM + cw（v9 起唯一索引空间）。
                                         // **字面位图**（v27）：0 = 空集（无 desc 的退化命令，合法）
    uint8_t  plane_count;                // 0 = 单 plane；N = multi-plane
    uint8_t  plane;                      // 本命令目标物理 plane 索引
    uint8_t  page_type   : 2;            // LP/MP/UP/TP — LDPC 引擎需此选择解码阈值
    uint8_t  reserved    : 6;
    uint8_t  feature_addr;               // SET/GET_FEATURES 寄存器地址
    nand_paa_t plane_paa[PLANE_NUM];     // 各 plane 的行地址承载 PAA（CW 位被 paa_to_nand_row 丢弃）
    uint8_t   *cw_buffers[CW_TOTAL_MAX]; // [n]，仅 n ∈ cw_bitmap 有效
} nand_dispatch_request_t;
```

> **v9 索引空间统一**：`cw_bitmap` / `cw_buffers[n]` / `completion.cw_status[n]` / `ecc_bit_count[n]`
> 一律用 **flat 绝对 CW 号 `n = plane*CW_NUM + cw`**（FC 侧的 desc 链也按同一公式展开）。
> 旧的 `cw_count[PLANE_NUM]`、`cws[plane][slot]`、`nand_dispatch_cw_info_t` 已删除；
> 行地址从 CW 项里拆出，改由 per-plane 的 `plane_paa[]` 承载。详见 §25.11。
>
> **v27/v28 两层同口径**：`request->cw_bitmap` 与 FTL 建链用的位图现在**都是字面位图**
> （置 1 = 访问该 CW，0 = 空集）。v23~v26 期间 SQ 侧 0 表示"段内全部"、NAND 侧 0 表示"空集"，
> 两层方向相反是结构性的、只能靠文档说清；v27 起这一条不再需要解释。v28 起 FC 命令结构体里
> **根本没有位图字段**（改作建链形参），FC 按 desc 链重新铸出 `request->cw_bitmap`。
> **v28 擦除是空集**：FC 不再为 ERASE 伪造 CW0 位，`request->cw_bitmap == 0` 时
> `completion_all_cws` 写 `cw_status[0]`（见 §25.4 v28）。

**v8 提交语义**：NAND 侧不再有"忙"的拒收——`submit`/`submit_chain` 只在 FIFO 装不下（`count + N > DEPTH`）时
返回 `NAND_RESULT_NOT_READY`，**且无副作用**（一个槽都不写）。FC 填充前先查 `nand_dispatch_lun_free()`，
因此生产路径上 `NOT_READY` 不会出现（出现即 `ASSERT`）；它保留是给"链放不下"的重试语义兜底。
`submit` / `submit_chain` 先经 `nand_dispatch_enqueue` 检查放行闩锁 `g_nand_dispatch_armed`（未放行返回 `INVALID_ARGUMENT`）——
两个入口共用同一道门，不再有单命令/整链的不对称。

`nand_dispatch_completion_t` 包含命令 ID、执行状态、NAND 状态、per-CW 状态、per-CW ECC 纠正比特数和失败 Descriptor ID：

```c
typedef struct {
    uint16_t command_id;
    uint8_t  execution_status;
    uint8_t  nand_status;
    uint8_t  cw_status[CW_TOTAL_MAX];     // 下标 = flat 绝对 CW 号 n = plane*CW_NUM + cw（v9 起）
    uint8_t  ecc_bit_count[CW_TOTAL_MAX]; // 同下标空间（v9 起）
    uint16_t failed_descriptor_id;
} nand_dispatch_completion_t;
```

FC 通过 `fc_complete_from_nand()` 将 `nand_dispatch_completion_t` 聚合进 `fc_command_status_t` 写入 CQ
（CQ 只带命令级聚合 status + `param` 返回段），同时逐 CW 回写 descriptor 的 status 和 ecc_bits
（聚合 ECC 走 `fc_get_cmd_ecc_max()`）。

---

## 5. 数据路径 (Data Path)

FC 数据路径分两个方向：

- **写路径 (encode)**：FC 内部在 NAND Program 前调用，对 FTL 完全透明
  - CRC 计算（`config.crc == FC_CRC_ENABLE` 时）
  - Random 加扰
  - ECC 编码
  - RAID XOR 编码（如配置）
- **读路径 (decode)**：分为两阶段
  - **Stage 1**：FC 内部在 NAND Read 完成后、CQ push 前自动执行
    - ECC 解码
    - 全部成功 → 解扰 → CRC 校验
    - 有 CW 失败 → 不解扰（供 RAID XOR 恢复在 scrambled domain 进行）
  - **Stage 2**：FTL 触发的 RAID 恢复路径

### 5.1 编码流程 (Page Pack)

```
User Data (512B/CW)
  → [META] 写入 per-CW META（desc->meta，存 buffer[0..4]）
  → [CRC Engine] CRC-32 计算（仅 USER 区域，写入 CRC 字段 [4:8]）
  → [Random Engine] 加扰（LFSR，种子 PAA × program_sequence，操作 520B 数据区）
  → [LDPC Engine] 编码（520B 数据 → 8B parity，写入 [520:528]）
  → （数据命令累积到 PLANE_NUM-1 条后）
  → [Stripe RAID] fc_data_path_generate_stripe_parity() 跨命令 XOR → parity 写入独立 Plane
  → 数据命令 + parity 命令一起提交 nand_dispatch → Multi-Plane PROGRAM
```

RAID XOR 不再覆盖最后一个 CW 的用户数据。Plane 数为 2 的幂（2/4/8），Parity 独立占用最后一个 Plane：

| Plane 数 | 数据命令 | parity 命令 |
|----------|---------|------------|
| 2        | D0 (Plane 0) | P0 (Plane 1) |
| 4        | D0/D1/D2 (Plane 0-2) | P0 (Plane 3) |
| 8        | D0~D6 (Plane 0-6) | P0 (Plane 7) |

FTL 只提交 PLANE_NUM-1 条数据命令（config.raid=2），FC 内部自动：
1. 累积 stripe 成员（PLANE_NUM=2 时在单次 fc_service() 内同步完成）
2. 内部分配 parity 命令资源（command + desc 链 + buffer）
3. 调用 fc_data_path_generate_stripe_parity() 做跨命令 XOR
   - XOR 数据命令各 CW 的 scrambled data（520B）到 parity buffer
   - ECC 编码 parity buffer
4. parity 命令不经过 META/CRC/Random（parity buffer 内容已在 scrambled domain，META 为 XOR 结果，只靠 ECC 保护完整性）
5. 数据命令 + parity 命令以 Multi-Plane PROGRAM 一次提交 nand_dispatch
6. 完成时 parity 命令资源自动释放，FTL 只感知数据命令的 completion

RAID XOR 在 ECC 编码之后进行，保证了 parity CW 同样经过 ECC 保护。所有步骤在 FC 内部完成。
META（LBA）和 CRC 在加扰前写入，受后续加扰和 ECC 的完整保护。

### 流程图：CW 布局与数据处理范围（图 3）

每个 Page（2112B）= 4 个 CW × 528B。Buffer Pool 里每个 CW 分层管理：

```text
┌─────────────────────────── 528 字节 CW ───────────────────────────┐
│ [0:4]    [4:8]    [8:520]            [520:528]                    │
│  META     CRC     USER（512B）          PARITY                    │
│ FC 管理   CRC-32   FTL 可见区          ECC 引擎生成               │
└──────────────────────────────────────────────────────────────────┘
   ▲                  ▲                    ▲
   fc_buffer_get_base  fc_buffer_id_to_real_buffer   （对 FTL 透明）
   （数据路径入口）      （对外接口，+8B 偏移）

模块处理范围：
  CRC    → 512B  [8:520]     算 CRC 写 [4:8]（只保护 USER）
  加扰   → 520B  [0:520]     LFSR 全数据区（ECC 会覆盖 parity，加扰它白做）
  ECC    → 520B→8B parity    读 [0:520]，写 [520:528]（BCH m=13 t=4）
  RAID   → 528B 全 CW        跨命令 XOR（需保护 parity 自身）
```

**为什么**：ECC 输入是 520B（META+CRC+USER），parity 总是写在 `data + data_length`。加扰作用域不含 parity——因为 ECC 编码后 parity 才生成，加扰了 parity 在解码前会被 ECC 覆盖。

### 流程图：写路径编码（图 4，Page Pack）

`fc_data_path_encode()`（FC 内部，NAND Program 提交前，对 FTL 透明）：

```text
FTL 数据填入各 CW buffer USER 区 [8:520]
  │
  ├─ Stage 0  META：desc->meta（本 CW 的扇区 LBA）memcpy 到 buffer[0:4]
  ├─ Stage 1  CRC：对 USER 512B 算 CRC-32 → 写 buffer[4:8]
  ├─ Stage 1  加扰：seed = fc_random_seed_from_meta(desc->meta)，LFSR 变换 buffer[8:520]
  │           （v24：META/CRC 明文，不进加扰区）
  ├─ Stage 2  ECC：BCH 编码 520B → 8B parity → 写 buffer[520:528]
  └─ (Stage 3 RAID 由 stripe 路径单独处理，不在此函数内)
        │
        ▼
   数据命令累积到 PLANE_NUM-1 条（raid=ENCODE 时）
        │
        ├─ fc_data_path_generate_stripe_parity()    ← 图 12
        │    XOR 各数据命令对应 CW 的 [0:520]（scrambled domain）
        │    → parity buffer → ECC 编码 parity
        └─ fc_stripe_submit()：数据命令 + parity 命令拼成一个
             Multi-Plane PROGRAM 提交 nand_dispatch
```

**要点**：META 和 CRC 在加扰前写入，因此受加扰+ECC 的完整保护；parity CW 的 META 是 XOR 结果，只靠 ECC 保护完整性，不重算 CRC/加扰。

### 5.2 解码流程 (Page Unpack)

```
NAND 读取完成
  │
  ├─ Stage 1 / FC 内部（CQ push 前）:
  │   ┌──────────────────────────────────────┐
  │   │ 逐 CW: ECC 解码                      │
  │   │   ├─ 全部成功 → 解扰所有 buffer       │
  │   │   │              → CRC 校验          │
  │   │   │              → push CQ (status=READY/CRC_ERROR)
  │   │   └─ 有 CW 失败 → 不解扰 buffer       │
  │   │                → push CQ (status=UNC)
  │   └──────────────────────────────────────┘
  │
  └─ Stage 2 / FTL 触发（poll CQ 后）:
      遍历 desc 链，发现 status=UNC
      ┌──────────────────────────────────┐
      │ 调用 fc_data_path_raid_recover() │
      │   ├─ RAID XOR 重建 failed CW     │
      │   ├─ ECC 重试                   │
      │   ├─ 解扰所有 CW                │
      │   └─ 更新 desc->status = READY  │
      └──────────────────────────────────┘
      │
      └─ 仍失败 → BBT / read retry / 其他恢复策略
```

### 流程图：读路径解码（图 5，Page Unpack）

两阶段，以 CQ 为分界：

```text
NAND 读取完成
  │
  ├─ Stage 1 / FC 内部（CQ push 前）：fc_data_path_process_read()
  │   Pass 1  ECC 解码每个 CW：
  │     ├─ 全部成功 → 各 desc->status = READY，记录 ecc_bits
  │     └─ 有 CW 不可纠 → desc->status = UNC，
  │                       保持 ECC-corrected + scrambled（供 RAID 恢复）
  │                       提前返回 IO_ERROR，跳过 Pass 2~4
  │   Pass 2  解扰所有 CW（v24：seed 从页内**明文 META** 自举，范围 [8,520)）
  │   Pass 3  CRC 校验 → 不匹配置 CRC_ERROR
  │   Pass 4  META 读回：自洽（低 3 位 == 本 desc 的 plane/cw）+ 交叉校验
  │           （lba_to_meta(command->meta, plane, cw) 对比，command->meta == FC_META_UNKNOWN 时跳过后者）
  │           → 不匹配置 MATA_ERROR
  │
  ├─ 完成状态经 fc_complete_from_nand() 聚合进 CQ（status/desc_id/ECC）
  │
  └─ Stage 2 / FTL 触发：fc_data_path_raid_recover()（status==UNC 时）
        ├─ fc_collect_decode_buffers 收集全 CW buffer（v24：不再输出 PAA，
        │  解扰种子从各 buffer 的明文 META 自举）
        ├─ fc_raid_recover：XOR 重建 failed CW（scrambled domain 内）
        ├─ ECC 重试：重建后仍不可纠 → 返回 IO_ERROR（硬件不可恢复）
        ├─ 解扰所有 CW
        └─ 全部 desc->status = READY
              │
              └─ 仍失败 → BBT / read retry / 其他恢复策略
```

**三道校验各抓什么**（上面的流程图只列了「做了什么」，这里说「为什么三道都要」）：

| 校验 | 抓什么 | 抓不到什么 |
|---|---|---|
| **Pass 1 ECC** | 介质里**随机散布**的位错（≤ t = 4） | **误纠**（报成功但解错，见 §21.3.3）；超能力 → `UNC` |
| **Pass 3 CRC** | ECC **误纠**（落在 USER 那 512B 里的部分）+ **数据路径逻辑错**（buffer / 加扰 / 搬运任一环出错都会让 CRC 不过） | 只盖 `[8,520)`；对**系统性**错误图样有线性盲区（`g(x)` 的倍数是恒通过的） |
| **Pass 4 META** | META 损坏——低 3 位**自洽**（== 本 desc 的 plane/cw）+ 与 `command->meta` **逐位精确比对** | `command->meta == FC_META_UNKNOWN` 时**跳过**精确比对（GC / 掉电重建不知道 LBA） |

**加扰开启时还有一道隐式的**：Pass 2 的种子取自**页内明文 META**（`fc_random_seed_from_meta`），所以 META 错 ⇒ 种子错 ⇒ USER 解成垃圾 ⇒ Pass 3 必然失败。**这正是 GC 路径跳过 Pass 4 交叉校验仍然安全的原因**——但它是**隐式耦合**，一旦 `random=OFF` 就失效（见 §5.3）。

### 单 CW 步骤原语（v29）

上面两条管线的每个算法步骤都拆成了**单 CW 原语**（`include/fc/fc_data_path.h` 的「单 CW 步骤原语」段），
管线本身退化为**编排**（取 buffer + 调原语）。原语一律只操作一块 528B 裸 CW buffer，不碰 command /
descriptor / 链：

| 原语 | 对应步骤 | 行为 |
|---|---|---|
| `fc_data_path_meta_write(cw, meta)` | 写 Stage 0 | `memcpy(cw, &meta, 4)` |
| `fc_data_path_crc_write(cw)` | 写 Stage 1a | `fc_crc_compute(cw + 8)` → 写 `cw[4..8)` |
| `fc_data_path_scramble(cw, seed)` | 写 Stage 1b / 读 Pass 2 | `fc_random_transform(cw + 8, 512, seed)`；自反 ⇒ 加解扰同一函数 |
| `fc_data_path_ecc_encode(cw)` | 写 Stage 2 | `fc_ldpc_encode(&FC_DATA_PATH_LDPC_CFG, cw, 520)` |
| `fc_data_path_ecc_decode(cw, &ecc_bits)` | 读 Pass 1 | `fc_ldpc_decode(...)` |
| `fc_data_path_crc_check(cw)` | 读 Pass 3 | 1 = 重算值 == `cw[4..8)` |
| `fc_data_path_meta_check(cw, command_meta, paa)` | 读 Pass 4 | `FC_CW_STATUS_MATA_ERROR` 或 0 |

**为什么拆**：让「调了哪些步骤」直接等于「开了哪些功能」。单测（Level 2 步骤级）拿一块 buffer 调这些
原语就能验算法本身，不必建链、也不必用 `config` 位去组合出「只开 CRC」这类状态。`fc_ldpc_config_t`
也顺带收敛为文件内唯一一份 `FC_DATA_PATH_LDPC_CFG`（原在 encode / process_read / stripe_parity /
raid_recover 各写了一遍字面量 `{520, 8, 4}`）。

⚠ **留在管线里**（原语覆盖不到的）：gate 三个 `if` / Pass 2 的 `if (all_ok)` / 链遍历 / `desc->meta`
写回 / `desc->status` 赋值 / 错误码透传——这些要验得建链，属「管线级」用例。
⚠ **提交出去的命令绝不能再手工调这些原语**——FC 自己会跑整条管线，重复编码 = 数据损坏。

用例与判据见 `fc_nand_ut.md` §2.2「步骤级用例」与 `docs/changelog/fc_data_path_step_primitives_v29.md`。

**关键约束**：RAID XOR 在 scrambled domain 进行——ECC 失败时其余 CW 处于"已纠错但仍加扰"，重建结果与被恢复 CW 同 domain，重建后再统一解扰。加解扰对 FTL 完全透明。

**关键设计约束**：
- RAID XOR 在 scrambled domain 进行：ECC 失败时，其他 CW 处于 "ECC 已纠正但仍加扰" 状态，XOR 重建的结果与被恢复 CW 处于同一 domain，重建后 ECC 解码再统一解扰
- 加解扰完全对 FTL 透明：FTL 从不直接调用 Random 引擎
- RAID 恢复后的解扰统一在 `raid_recover()` 内部完成，不暴露到 FTL

### 5.3 CW 布局

每个 Page（2112 字节）= 4 个 CW × 528 字节，CW 布局：

```
[0:4]   META    — FC 内部管理（LBA 等 META 信息）
[4:8]   CRC     — FC 计算写入（CRC-32 覆盖 USER 区域）
[8:520] USER    — FTL 填入的用户数据（512 字节）
[520:528] PARITY — ECC 编码引擎生成的校验数据

FTL 通过 fc_buffer_id_to_real_buffer() 拿到的指针指向 [8]，
直接读写 512 字节 USER 数据，不感知其余区域。

FC 内部通过 fc_buffer_get_base() 获取 CW 基址（[0]），
数据路径（加扰/ECC/RAID）操作全 528 字节。
```

**每个字段由谁保护**：

| 字段 | 范围 | 保护者 |
|---|---|---|
| META | `[0:4)` | **ECC**（在 `[0,520)` 码字内）+ Pass 4 自洽 / 精确比对 +（加扰开启时）种子耦合 |
| CRC | `[4:8)` | **ECC**——CRC 字段本身不自我覆盖，靠 ECC 保证它没在存储 / 传输中损坏 |
| USER | `[8:520)` | **ECC** + **CRC-32** |
| PARITY | `[520:528)` | **ECC**（它本身就是 ECC 的产物，没有第二层） |

**为什么「CRC 只盖 USER」是分工，不是漏**：CRC 在这里的职责是**抓 ECC 误纠**（§21.3.3）——误纠是**整码字**事件，可能落在 META 也可能落在 USER；落在 USER 的由 CRC 抓，落在 META 的由 Pass 4 抓。而 Pass 4 在**期望值已知**时用的是**逐位精确比对**，检出率 100%，**比 CRC-32 的 1−2⁻³² 更强**。

**四条读取路径的 META 保护矩阵**：

| 路径 | `random` | `crc` | `command->meta` | META 靠什么查 |
|---|---|---|---|---|
| 主机数据读（`FTL_PAGE_CFG_DATA`） | ON | ON | 已知 | Pass 4 **精确比对** |
| GC / 搬移读（`gc.c:52`） | ON | ON | `FC_META_UNKNOWN` | Pass 4 自洽（3 位）+ **种子耦合 → CRC 失败** |
| 元数据页读（`FTL_PAGE_CFG_META`） | OFF | OFF | `0`（期望值即 `plane_cw`） | Pass 4 **精确比对** |
| RAW 读（`LDPC_RAW`） | OFF | OFF | — | 无——**刻意**（坏页也要原始数据） |

**四条生产路径都能检出 META 损坏。**

**行业对照**：这不是本工程的特例。T10 PI / DIF 的 guard 是 CRC-16，也**只盖逻辑块数据**；「地址对不对」靠 reference tag **与期望值比对**，不靠 CRC——与本工程 Pass 4 是同一个思路。判据不是「CRC 该盖哪些字节」，而是「**这个字段出错时，你能不能用别的手段发现**」：期望值已知 → 比对；期望值未知 → 需要**独立**校验；字段本身无冗余 → 只能靠 ECC。

**⚠ 本工程的差异点**：GC / 掉电重建路径（期望值**未知**）靠的是**种子耦合**——它是加扰的一个**副产品**，没写进任何契约，且一旦 `random=OFF` 就失效。行业在这里会放一个**显式**校验（独立 CRC / 序号 / 冗余存储）。见 §8.2 与 §21.3.5。

### 5.4 加扰的目标：统计独立

**加扰的唯一目标**：让写入 NAND 的位串**统计独立**——给定**全部历史**，下一位仍是 1/2：

```
P(bit = 1 | 前面所有位) = 1/2        ← 目标（独立）
```

注意这是**条件**概率，不是**无条件**概率。这一字之差正是「随机」与「看起来均匀」的分界：

| 约束 | 含义 | `0101…` 满足？ |
|---|---|---|
| **无条件** `P(bit=1) = 1/2` | 整串里 1 占一半（**均衡**） | ✅ 满足 |
| **条件** `P(bit=1 \| 全部历史) = 1/2` | 知道历史后，下一位仍是 1/2（**独立**） | ❌ 不满足 |

`0101…` 里「上一位是 0」就**必然**推出「下一位是 1」——历史一暴露、未来就定了。它**均衡，却毫不随机**。

**三种序列对照**（4096 bit）：

| 序列 | `ones` | 段数 | 平均段长 | 每步翻转率 | 判定 |
|---|---|---|---|---|---|
| 全 0 / 全 1 | 0 / 4096 | 1 | 4096 | 0% | 极端有序 |
| **`0101…`** | 2048 | **4096** | **1.0** | **100%** | **极端有序**（另一个方向） |
| **随机** | ≈2048 | **≈2048** | **2.0** | **50%** | **目标** |

关键：`0101…` 与「全 0」在「有序」这件事上是一伙的，分别顶到段数的两个极端（4096 / 1）；**随机落在正中间**。
「段数 = 4096」不是「很随机」，而是「每一段长度都是 1」⟺ **序列只能是 `0101…`**——即**完美交替**，最不随机。

**为什么随机是对的**：随机数据**每一步有 1/2 概率翻转**（相邻两位相同 / 不同各半），于是

```
段数   = 1 + 翻转次数 ≈ 1 + 4095 × 1/2 ≈ 2048   （= 位数的一半）
平均段长 = 4096 / 2048 = 2
```

段长服从几何分布 `P(段长 = k) = 2^-k`——**一半的段只有 1 个 bit**。所以「段数 ≈ 位数的一半」不是巧合，
正是那个 1/2 的直接结果。统计方法（状态机 / 分桶 / 判据盲区）见 `fc_nand_ut.md` §5.6。

**为什么 `0101…` 对 NAND 反而有害**（不是「不够随机」，是「有害」）：

1. **零结构 = 固定图案**。加扰的全部意义是把主机数据的结构打散；若目标是 `0101…`，等于**用一个固定周期图案
   替换另一个固定图案**，结构一点没少。
2. **单元间耦合（CCI）最坏**。每个单元的相邻单元都和它状态相反，电容耦合 / 干扰最大；随机数据平均只有一半邻居相反。
3. **与阵列物理周期谐振**。位线 / 字线有固定 pitch，固定周期的数据图案会和它共振 ⇒ **系统性、可复现**的错误。
   **ECC 擅长纠随机散布的错误，系统性错误会整片超限**。

**落到实现**：加扰器（LFSR）干的活就是造一条「给定历史、下一位仍是 1/2」的伪随机序列，再与数据 XOR。
**均衡、游程均值 2、长游程稀少——全是「独立」的副产品，不是目标。**
见 §21.3.5（无硬件加扰 → 软件 LFSR）与 `docs/changelog/fc_scrambler_lba_seed_v24.md`。

---

## 6. PAA（Physical Array Address）设计

### 6.1 编码布局

```
PAA bit layout (uint32_t, 低位优先):
[CW:2][PAGE:6][PLANE:1][BLOCK:11][LUN:0]
```

- CW（2 bit）：Page 内 CW 索引（0~3）
- PAGE（6 bit）：Block 内页号（0~63）
- PLANE（1 bit）：Plane 选择
- BLOCK（11 bit）：Plane 内块号（0~2047）
- LUN（0 bit）：当前单 LUN，预留扩展

**注意**：Channel 和 CE 不在 PAA 内——它们属于命令级路由信息，保存在 `fc_cmd_conf_t` 的 ch/ce 字段中。

**v47 起 `fc_command_t.paa` 是 FC 层唯一的物理地址字段**：PBA（带 ch/ce）不跨进 FC——FTL 在自己的空间里组合 PBA，在边界处 `pba_to_paa()` 一次，赋给 `cmd->paa`；`src/fc/**` 里 `pba_to_paa()` / `pba_to_*()` 归零。凡需要行地址的地方（erase / Feature / READ_META / 单 plane 数据 / 无 desc 加载）都从 `command->paa` 取，逐 plane 换 plane 位改用 `paa_with_plane()`。

### 6.2 内联函数

```c
static inline uint32_t paa_to_page(nand_paa_t paa);
static inline uint32_t paa_to_plane(nand_paa_t paa);
static inline uint32_t paa_to_block(nand_paa_t paa);
static inline uint32_t paa_to_lun(nand_paa_t paa);
static inline nand_paa_t paa_from_addr(const nand_addr_t *addr);  // 展开地址 → PAA
static inline uint8_t    paa_is_valid_addr(const nand_addr_t *addr);
static inline uint32_t   paa_to_nand_row(nand_paa_t paa);         // 转换为 NAND 行地址
```

**构造入口一律走 `nand_addr_t` 具名字段**（v25）：`paa_from_addr()` / `pba_from_addr()` 是展开 → 打包，
`addr_from_paa()` / `addr_from_pba()` 是打包 → 展开。PAA / PBA / LBA 三套地址都以 `nand_addr_t` 为
唯一构造入口，不再有位置形参版本——七个同型小整数按位置传参写反了仍落在合法值域内（`page<64`、
`plane<2`、`block<2048`），会静默算出错地址；具名字段把这类错误变成编译错误。
（v25 前为 5 形参 `paa_make(page, plane, block, lun, cw)` / 7 形参
`pba_make(ch, ce, page, plane, block, lun, cw)`，字段顺序与 `nand_addr_t` 恰好相反。）

`paa_to_nand_row()` 将 PAA 转换为 MT29F4G08 的三周期行地址编码：PA[5:0]→bit[5:0]、BA6(Plane)→bit[6]、BA[17:7]→bit[17:7]。

---

## 7. 编译期配置 (nand_config.h)

```c
#define PAGE_SIZE   (2112)   // 每页总字节（含 OOB）
#define PAGE_NUM    (64)     // 每 Block 页数
#define BLOCK_NUM   (2048)   // 每 Plane 块数
#define PLANE_NUM   (2)
#define DIE_NUM     (1)
#define LUN_NUM     (1)
#define CH_NUM      (1)
#define CE_NUM      (1)
#define CW_NUM      (4U)     // 每页 CW 数
```

---

## 8. 已知待完善项

### 8.1 空壳/占位模块

| 模块 | 状态 | 说明 |
|------|------|------|
| `src/flash_controller/` | ✅ 已删除 | 死代码模块，已被 FC + nand_dispatch 取代 |
| `src/nand/bad_block.c` | ✅ 已删除 | 桩文件 |
| `src/nand/ecc.c` | ✅ 已删除 | 桩文件 |
| `src/nand/nand_command.c` | ✅ 已删除 | 桩文件，所有 ONFI 命令编码已在 `onfi_1_0.c` 实现 |
| `src/nand/nand_timing.c` | ✅ 已删除 | 桩文件，时序参数已在 `nand_operation_map_t.timing` 中 |
| `fc_ldpc_engine` | ✅ 已完成 | BCH(m=13, t=4) 编码/解码；QC-LDPC 硬判决编码/解码（可选） |
| `fc_raid_engine` | ✅ 已完成 | per-LUN START/ACCUMULATE/END 序列；fc_raid_recover 静态恢复 |
| `fc_data_path` | ✅ 已完成 | 写路径：Random→ECC→RAID；读路径：ECC→条件解扰（FC 内部）/ RAID 恢复（FTL 触发） |

### 8.2 接口待改进项

| 问题 | 说明 | 状态 |
|------|------|------|
| `nand_fc_*` 类型位置 | 已提取到 `nand_fc_interface.h`（通用接口头） | ✅ 已解决 |
| `nand_config.h` 命名歧义 | `PAGE_NUM→PAGES_PER_BLOCK`，`BLOCK_NUM→BLOCKS_PER_PLANE` | ✅ 已解决 |
| 缺少 Main/Spare 分离 | 已添加 `PAGE_MAIN_SIZE` / `PAGE_SPARE_SIZE` 宏 | ✅ 已解决 |
| `nand_device_t` 遗留 | 与 FTL 模拟路径共存，未来 FTL 迁移到 FC 接口后可移除 | 📝 已文档化 |
| 双向头文件依赖 | `nand_dispatch.h` 现只 include `fc/fc_types.h`（仅基础类型）；FC 在 submit 时预解析 Descriptor 链，nand_dispatch 不再回调 FC | ✅ 已解决 |
| `fc_command_handle` 编译 bug | `fc.c` 内部类型已统一为 `fc_command_id_t` | ✅ 已解决 |
| 死代码 `flash_controller/` | 模块已删除 | ✅ 已解决 |
| 桩文件清理 | `bad_block.c`, `ecc.c`, `nand_command.c`, `nand_timing.c` 已删除 | ✅ 已解决 |
| 双完成结构转换 | `fc_complete_from_nand()` 简化为直接字段拷贝 | ✅ 已解决 |
| **FTL 只判 `NAND_FAIL`** | FC 把 `UNC` / `CRC_ERROR` / `MATA_ERROR` 都 OR 进 `completion.status`（`fc.c:772` 全链聚合），但 FTL 五处判据只查 bit0 ⇒ **已检出的错误当成功交给主机**（`*status_out = SSD_STATUS_OK`）。本工程无 read retry / RAID 兜底（`CONFIG_FC_RAID=0`）⇒ 等效 UBER ≈ `P(误纠)` 而非 `P(误纠) × 2⁻³²`（理论见 §21.3.3）。**在 FTL 层，本次不改 `src/ftl/**`** | 📝 已文档化 |
| **GC / 掉电重建路径 META 缺显式校验** | `command->meta == FC_META_UNKNOWN` 时 Pass 4 交叉校验被跳过，META 只剩 3 位自洽 + **种子耦合**（隐式，且依赖加扰开启）。行业在此放显式校验（独立 CRC / 序号 / 冗余）。见 §5.3 | 📝 已文档化 |

### 8.3 功能扩展规划

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | 多 LUN 并发：nand_engine 异步步进 + 每 LUN 独立状态机 | ✅ |
| Phase 2 | DMA 数据传输：FMC 数据窗口改为 DMA 驱动，中断完成通知 | ❌ STM32H7 限制 |
| Phase 3 | 中断驱动：R/B# EXTI 替代轮询 WAIT_READY | ⚠️ 已回退：单 EXTI6 无法扩展到多 LUN，模块已移除，WAIT_READY 改回纯 GPIO 轮询 |
| Phase 4 | LDPC 硬解码实现 + BCH ECC 移植 | ✅ |
| Phase 5 | RAID 跨 Plane XOR 恢复 + 读路径管线拆分 | ✅ |

---

## 9. 测试

### 9.1 集成测试 (test_fc_nand.c)

在 STM32H743 核心板上运行的 FC→NAND 全链路测试：

- `fc_nand_test_read_id()` — 验证 NAND ID
- `fc_nand_test_erase_block(paa)` — 擦除整个 Block
- `fc_nand_test_program_block(paa)` — 顺序编程 Block（0x55 模式）
- `fc_nand_test_read_block(paa)` — 顺序读取并校验
- `fc_nand_test_run_all(paa)` — 串行执行上述全部

### 9.2 串口控制台命令

```
nand id                          → READ_ID 测试
nand test <PAA>                  → 完整 Block 擦写读测试
nand paa <ch> <ce> <lun> <plane> <block> <page> <cw> → PAA 计算
```

### 9.3 编译

```sh
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 tests
```

---

## 10. 设计决策记录

1. **为什么用空闲链表而非遍历查找** — 企业级实现，O(1) 分配，避免 Pool 深度扩大时性能退化。
2. **为什么 Buffer 与 Descriptor 解耦** — UNC 发生时可以释放 buffer 供其他任务使用，只保留 descriptor 信息做错误处理。
3. **为什么 Command 中 CW 字段是"数量减一"** — 0 表示 1 个 CW，节省位宽。Descriptor 中 CW 是"当前索引"，用途不同。
4. **为什么 PAA 不含 Channel/CE** — 通道和片选是命令级路由信息（`fc_cmd_conf_t.ch/ce`），PAA 只编码芯片内物理地址。
5. **为什么数据 ≤ 4B 不分配 Descriptor** — GET_FEATURES 等命令返回值小，复用 Command 的 4 字节 parameter 字段避免浪费资源。
6. **为什么 command_module 字段放在 FC 而非 FTL** — CQ 中的完成项需要路由到正确的回调，由 FC 原样返回提交时的 module 值。


## 11. 接口重构策略与落地清单

> **⚠ 历史章节（v13 前的接口）**：本章记录的是 FC 侧还持有"掩码建链 + 数据搬运"时的重构过程。
> 本章出现的 `fc_command_configure_descriptors()`（**复数**）已在 v13 上移 FTL 并删除，现名
> `ftl_cmd_configure_descriptors()`；`fc_write_cmd_data()` / `fc_read_cmd_data()` 同理改为
> `ftl_cmd_write_data()` / `ftl_cmd_read_data()`。FC 侧如今只有**单数**的
> `fc_command_configure_descriptor()`（逐条建链机制）。判据见 §26。

### 11.1 设计原则

**参数校验分层：**
- 对外接口层：校验所有外部传入参数（NULL 检查、范围检查）
- 内部函数：默认参数有效，不做冗余校验（controller、command 等由 init 保证）

**返回值语义统一：**
使用 `FuncStatus` 枚举替代裸 `0U`/`1U`：
```c
typedef enum {
    FUNC_STATUS_SUCCESS = 0,
    FUNC_STATUS_ERROR   = !FUNC_STATUS_SUCCESS
} FuncStatus;
```

**命名规范：**
- `command_handle` → `command_id`（统一命名）
- 资源通过 id 访问，提供双向查询：`xxx_id_to_xxx()` + `xxx_to_xxx_id()`

### 11.2 重构项

| # | 项目 | 说明 | 状态 |
|---|------|------|------|
| 1 | FuncStatus 语义统一 | `0U`/`1U` → `FUNC_STATUS_SUCCESS`/`FUNC_STATUS_ERROR` | ✅ |
| 2 | command_handle → command_id | 统一命名 | ✅ |
| 3 | fc_command_configure_descriptors | 合并 descriptor 分配+配置为单接口（v13 上移 FTL，见 §26） | ✅ |
| 4 | paa_from_addr() | PAA 地址合成函数（由 `nand_addr_t` 具名字段打包；v25 前为 5 形参 `paa_make`） | ✅ |
| 5 | 条件判断宏封装 | 复合条件拆为独立命名宏 | ✅ |
| 6 | 资源 ID 双向查询 | 每种资源提供 id↔指针 双向转换函数 | ✅ |
| 7 | 冗余 NULL 检查移除 | 内部函数移除 controller/command 等 NULL 检查 | ✅ |
| 8 | fc_submit_internal 简化 | 三元嵌套提取为独立变量 | ✅ |
| 9 | fc_command_validate 精简 | 移除位域全组合均有含义的冗余检查 | ✅ |

### 11.3 Descriptor 分配重构

**旧接口：**
```c
fc_descriptor_id_t fc_command_descriptors_alloc(fc_command_t *command, uint8_t cw_count);
```

**新接口（一次完成分配+配置）：**
```c
fc_status_t fc_command_configure_descriptors(fc_command_t *command);
```
内部根据 `command->config.cw` 自动计算需要的 descriptor 数量，分配 descriptor 链+buffer 并串联链表。

### 11.4 命名修正

| 旧名 | 新名 | 位置 |
|------|------|------|
| `PAGE_NUM` | `PAGES_PER_BLOCK` | `nand_config.h` |
| `BLOCK_NUM` | `BLOCKS_PER_PLANE` | `nand_config.h` |

## 12. 数据 Buffer 方案设计讨论

> **⚠ 历史章节（v13 前的接口）**：本章代码示例里的 `fc_command_configure_descriptors()`（**复数**）与
> `fc_write_cmd_data()` 已随 v13 策略层上移改由 FTL 侧 `ftl_cmd_configure_descriptors()` /
> `ftl_cmd_write_data()` 承担。方案比较的**结论**（buffer_id 方案胜出）不受影响，只是调用方换了层。

### 12.1 问题背景

FC 模块需要决定数据 buffer（每个 CW 的数据载体）在接口层的表达方式。核心问题可以归结为一个选择：

```
如何把"某个 CW 的数据"和"FC 能访问到的内存位置"关联起来？

选项 A：buffer_id（资源 ID 引用）
选项 B：data_ptr（裸指针/地址）
```

这个选择不仅影响 FC 内部实现，更影响 FTl 与 FC 之间的协作模式、数据路径效率、以及整个系统的安全性。

### 12.2 各存储领域的行业实践

#### 12.2.1 企业级 NVMe SSD（有大量 DDR）

代表：Intel/Samsung/Kioxia 企业级 SSD 控制器。

| 特征 | 做法 |
|------|------|
| DDR 容量 | 几 GB~几十 GB |
| 数据 buffer 管理 | **物理地址（data_ptr）** |
| 控制结构管理 | **ID**（Command Tag、CID） |
| 数据描述方式 | PRP（Physical Region Page）或 SGL（Scatter-Gather List） |
| 传输方式 | DMA 引擎直接从 DDR 搬运，零 CPU 拷贝 |

**理由**：DMA 引擎只能消费物理地址，多一层 buffer_id → 地址的查表就是多一个硬件逻辑门延迟。数据路径上，地址直达是最优选择。

#### 12.2.2 消费级 NVMe/SATA SSD（有外置 DRAM 或 HMB）

代表：Phison/Silicon Motion 主控。

| 特征 | 做法 |
|------|------|
| DRAM 容量 | 0（DRAM-less）~512MB |
| 数据 buffer | **物理地址（有 DRAM）/ 内部 ID（DRAM-less）** |
| 控制结构 | **ID** |

有 DRAM 时：与企业的做法一致，地址直达。
DRAM-less 时：**内部 SRAM Pool + ID 方案**（见 12.2.3）。

#### 12.2.3 DRAM-less SSD（与你的项目最接近）

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

#### 12.2.4 UFS（Universal Flash Storage）

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

#### 12.2.5 行业规律总结

```
有 DRAM/SDRAM（≥几 MB） ──→ 数据用 data_ptr（零拷贝），控制用 ID
无 DRAM（仅内部 SRAM）   ──→ 数据和控制统一用 ID/Pool
SRAM 达到 512KB~1MB     ──→ 行业处于从 ID 向 Pointer 过渡的阶段
```

### 12.3 你的硬件参数

| 资源 | 大小 |
|------|------|
| 内部 SRAM | **1MB** |
| 板载 SDRAM | **8MB** |
| FC 控制结构总需求 | ~75 KB（占 SRAM 的 7%）|
| FC 数据 Buffer Pool（当前方案） | ~67.5 KB（128×528B）|

FC 控制结构仅占用 SRAM 的 7%，剩余 93% 可供 FTL 使用。1MB SRAM 的容量已经达到 **UFS 旗舰主控档次**。

### 12.4 方案一：buffer_id（当前方案）

#### 12.4.1 核心思想

FC 内部维护静态 Buffer Pool，所有数据 buffer 通过 16-bit ID 对外暴露。FTL 在使用数据时通过 ID → pointer 转换访问实际内存。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_conf_t    config;
    uint16_t         buffer_id : 16;  // ← 指向 FC 内部 Buffer Pool
    uint16_t         next_id   : 16;
    uint8_t          status    : 8;
} fc_descriptor_t;
```

**相关接口（4 个）：**

| 接口 | 说明 |
|------|------|
| `fc_buffer_alloc()` | 从 FC Buffer Pool 分配 |
| `fc_buffer_release(id)` | 释放回 Pool |
| `fc_buffer_id_to_real_buffer(id)` | ID → 指针 |

> **注**：`fc_real_buffer_to_buffer_id(ptr)`（指针 → ID）已随对外接口拆分移除——FC 数据引用统一走 buffer_id，无调用者（详见 §2.9 移除说明）。

#### 12.4.2 FTL 使用流程

```c
// ① 分配 command + desc 链 + buffer
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.cw = 1;   // 2 个 CW
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);   // 自动分配 desc 链 + buffer

// ② FTL 将数据写入 buffer（需遍历 desc 链）
fc_descriptor_id_t desc_id = cmd->desc_id;
uint8_t cw = 0;
while (desc_id != FC_DESCRIPTOR_ID_INVALID && cw <= cmd->config.cw) {
    fc_descriptor_t *desc   = fc_descriptor_id_to_descriptor(desc_id);
    uint8_t         *buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
    memcpy(buffer, my_data + cw * CW_SIZE, CW_SIZE);
    desc_id = desc->next_id;
    cw++;
}

// ③ 提交
fc_submit(cmd);
```

#### 12.4.3 优点

- **安全性高**：FC 能验证 buffer_id 范围（0~127），天然防止野指针
- **生命周期明确**：FC 通过 command state 和 descriptor status 知道哪些 buffer 在使用中
- **资源可视化**：`fc_get_resource_stats()` 可直接输出 buffer 使用率
- **改动最小**：已有代码无需重构

#### 12.4.4 缺点

- **数据路径多一层转换**：每次读写都要 `buffer_id → real_buffer` 查表
- **多一次 memcpy**：FTL 的数据必须经过 memcpy 进入 FC Buffer Pool
- **不灵活**：FTL 无法控制 buffer 放在哪里（SRAM 还是 SDRAM）
- **接口膨胀**：4 个 buffer 管理接口增加了学习成本

### 12.5 方案二：data_ptr（裸指针/地址方案）

#### 12.5.1 核心思想

FC 不再管理数据 buffer，Descriptor 中直接用指针指向数据位置。数据由 FTL 自主分配和管理。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_conf_t    config;
    uint8_t         *data_ptr;    // ← 直接指向 SRAM/SDRAM 地址
    uint16_t         next_id : 16;
    uint8_t          status  : 8;
} fc_descriptor_t;
```

**删除的接口（4 个）：**
- `fc_buffer_alloc()`
- `fc_buffer_release()`
- `fc_buffer_id_to_real_buffer()`
- `fc_real_buffer_to_buffer_id()`（已随拆分移除，见 §2.9）

**接口总数从 14 个减少到 10 个。**

#### 12.5.2 FTL 使用流程

```c
// ① FTL 自己管理数据 buffer
uint8_t *buf = ftl_buf_alloc();           // FTL 自己的分配器
memcpy(buf, my_data, CW_SIZE);

// ② 分配 command + desc 链（不分配 buffer）
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_PROGRAM;
cmd->config.cw = 1;
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);    // 只分配 desc，不分配 buffer

// ③ FTL 直接指定地址
fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(cmd->desc_id);
desc->data_ptr = buf;                     // ← 直接给地址

// ④ 提交
fc_submit(cmd);

// ⑤ 完成后 FTL 释放
fc_poll_completion(&completion);
fc_command_release(cmd);
ftl_buf_free(buf);                        // FTL 自己释放
```

#### 12.5.3 优点

- **零拷贝**：FTL 直接写自己的 buffer，没有中间 memcpy
- **接口精简**：减少 4 个 buffer 接口
- **灵活性高**：FTL 可自由选择 buffer 位置（SRAM 或 SDRAM）
- **架构对齐行业**：与有 DRAM 的企业/消费级做法一致

#### 12.5.4 缺点

- **所有权/生命周期管理复杂**（见 12.5.5）
- **无防错机制**：FC 无法验证 data_ptr 的有效性（只能检查 NULL）
- **FTL 需要额外建立 buffer 管理代码**（~30 行 freelist）

#### 12.5.5 data_ptr 的生命周期问题

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

### 12.6 方案三：双模设计（推荐过渡方案）

#### 12.6.1 核心思想

Descriptor 同时支持 buffer_id 和 data_ptr，通过标志位选择使用哪种模式。

**Descriptor 结构：**

```c
typedef struct {
    uint32_t         paa;
    fc_cmd_conf_t    config;
    
    union {
        uint8_t     *data_ptr;    // FTL 直接指定地址（零拷贝）
        uint16_t     buffer_id;   // FC 内部 Pool（传统路径）
    };
    
    uint16_t         next_id : 16;
    uint8_t          status  : 8;
    uint8_t          own_buf : 1;  // 0=FC Pool, 1=FTL 自己提供
} fc_descriptor_t;
```

#### 12.6.2 适用场景

| 场景 | 用哪种 | 原因 |
|------|--------|------|
| 常规 Page 写/读 | data_ptr | FTL 直接在 SDRAM/SRAM 准备数据，零拷贝 |
| 小操作（SET_FEATURES 等） | parameter 字段 | ≤ 4 字节，不需要 desc |
| 临时数据/测试代码 | buffer_id | 兼容旧路径，无需 FTL 管理 buffer |
| 内部 GC/校验操作 | data_ptr | FTL 自己管理缓冲，灵活分配 |

#### 12.6.3 优点

- **平滑迁移**：旧代码可以不修改继续使用 buffer_id
- **灵活选路**：FTL 根据场景选择最佳方式
- **兼容所有硬件变体**：即使不接 SDRAM，也能用内部 Pool

#### 12.6.4 缺点

- **两套路径维护**：FC 内部需要同时支持两种数据获取方式
- **接口复杂度**：双模增加了理解成本
- **可能永远不会用另一条路径**：如果确定了方向，双模就是死代码

### 12.7 方案四：buffer_id + 便捷数据接口（推荐方案）

#### 12.7.1 核心思想

保持 buffer_id 作为唯一的数据引用方式，保留 FC 的 Buffer Pool 所有权管理。但**新增便捷接口**让 FTL 不必手动遍历 desc 链写数据，解决易用性问题。

```c
// 新增接口：一次写入命令关联的所有 CW 数据
fc_status_t fc_write_cmd_data(fc_command_t *cmd,
                               const uint8_t *data,
                               uint16_t length);
// 内部校验 length == (cmd->config.cw + 1) * CW_SIZE
// 自动遍历 desc 链：buffer_id → pointer → memcpy

// 新增接口：获取指定 CW 的数据指针（读操作使用）
uint8_t *fc_get_cmd_data_ptr(fc_command_t *cmd, uint8_t cw_index);
// 遍历 desc 链找到第 cw_index 个 desc
// 返回 buffer_id → real_buffer
```

**接口总数从 14 个变为 16 个。**

#### 12.7.2 FTL 使用流程

**写操作：**

```c
fc_command_t *cmd = fc_command_alloc();
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
fc_command_t *cmd = fc_command_alloc();
cmd->config.operation = ONFI_PAGE_READ;
cmd->config.cw = 3;
cmd->paa = target_paa;
fc_command_configure_descriptors(cmd);

fc_submit(cmd);
// ... fc_service() 循环 ...

fc_poll_completion(&completion);
fc_command_t *done = fc_command_id_to_command(completion.command_id);

// 一行获取数据指针，FTL 直接读取
uint8_t *data = fc_get_cmd_data_ptr(done, 0);   // 第 0 个 CW 的数据

fc_command_release(done);
```

**非常规场景**（如只写某个 CW 的数据）：FTL 仍可回退到手动遍历 desc 链的方式。

#### 12.7.3 与 DRAM-less SSD 的对应关系

| DRAM-less SSD | 你的项目（本方案） |
|--------------|------------------|
| Host 数据 → PRP/SGL 描述地址 | FTL 有数据 → 调用 `fc_write_cmd_data()` |
| 控制器 DMA 自动将数据拉入内部 SRAM Pool | FC 内部 `buffer_id → pointer → memcpy` |
| 控制器内部 Pool ID（对 Host 透明） | buffer_id（对 FTL 可见） |
| 完成后 DMA 自动将数据写出给 Host | FTL 调用 `fc_get_cmd_data_ptr()` 读数据 |

逻辑完全一致，只是你的项目中 memcpy 替代了 DMA 的角色。未来可升级为 DMA。

#### 12.7.4 优点

- **保持所有权管理**：FC 仍然拥有完整的 buffer 生命周期控制，避免 UAF
- **易用性好**：FTL 无需手动遍历 desc 链，两行完成读写
- **安全性高**：FC 可验证 buffer_id 范围，拦截非法访问
- **改动小**：保留现有实现，只新增两个接口
- **可进化**：未来 memcpy 可替换为 DMA，接口不变

#### 12.7.5 缺点

- **多一次 memcpy**：相比 data_ptr 的零拷贝，有 CPU 开销
- **接口总数增加**：14 → 16 个
- **SRAM 占用**：需要预留 ~67.5KB 的 Buffer Pool

### 12.8 方案汇总对比

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

### 12.9 推荐方案

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

---

## 13. 控制/状态位贯穿分析与 Dispatch 边界设计

### 13.1 问题背景

FC 和 NAND Dispatch 之间存在一条**硬件语义边界**：

```
FTL → FC (firmware) → [nand_dispatch_request_t] → NAND Engine (硬件状态机)
NAND Engine → [nand_dispatch_completion_t] → FC (firmware) → FTL
```

这条边界是 **FC firmware 与 NAND 硬件引擎之间的防火墙**。FC 内部处理 firmware 层面的功能（数据加扰/解扰、LDPC 编解码、RAID XOR 编解码），只有 NAND 硬件引擎真正需要的字段才跨越 dispatch 边界。

核心问题：`fc_command_t` / `fc_descriptor_t` / `fc_cmd_conf_t` 中的所有控制位和状态位，哪些应该跨越 dispatch 边界进入 NAND 硬件，哪些应该留在 FC firmware 内部处理？

### 13.2 边界设计原则

| 原则 | 说明 |
|------|------|
| **硬件消费原则** | 只有 NAND 硬件引擎（LDPC 解码器、状态机）实际消费的字段才跨越边界 |
| **Firmware 封装原则** | FC firmware 内部处理的功能（LDPC 编码、RAID、Randomizer）不在 dispatch 层暴露 |
| **RAID 透明原则** | RAID 是 FC 内部功能，dispatch 层感知不到 RAID 的存在——看到的是标准 PROGRAM 命令 |
| **ECC 反馈原则** | 硬件解码结果（ECC 纠正比特数、per-CW 状态）必须带回 FC |

### 13.3 字段级贯穿映射

#### 13.3.1 fc_cmd_conf_t → Dispatch Request

| 字段 | 位宽 | 过 Dispatch？ | 理由 |
|------|------|-------------|------|
| `operation` | 8 | **✅ 通过** | 硬件需要知道执行什么 ONFI 操作 |
| `ch` | 2 | **✅ 通过** | 通道路由，硬件必须知道 |
| `ce` | 2 | **✅ 通过** | 片选路由，硬件必须知道 |
| `lun` | 2 | **✅ 通过** | LUN 路由，硬件必须知道 |
| `cw` | 2 | **❌ 不过**（v23） | 硬件只知道**建链形参 `cw_bitmap`**（v28 起它不再住在命令里）；该位图是**字面**的（v27，置 1 = 访问该 CW），与 `config.cw` 无关（v7~v22 期间 `mask==0` 曾退化为「每个 plane 取连续 `0..cw`」，v23~v26 又重载为「段内全部」，均已取消） |
| `crc` | 1 | **❌ 不过** | CRC 校验由 FC 在 firmware 层面完成（写入前附加 CRC，读出后校验），硬件不感知 |
| `ldpc` | 2 | **❌ 不过** | LDPC 编码/解码模式是 FC firmware 控制数据路径的参数。硬件 LDPC 引擎在解码时通过 `page_type` 选择阈值，不需要知道 `ldpc_mode` |
| `raid` | 3 | **❌ 不过** | RAID 是 FC 内部功能。Start XOR / Continue XOR / End XOR 由 FC 处理，dispatch 只看到标准 PROGRAM。End XOR 的 parity 写入哪个 plane 由 FC 推导 PAA |
| `random` | 1 | **❌ 不过** | 加扰/解扰在 FC 数据路径中完成，对硬件透明 |
| `chain` | 1 | **❌ 不过** | 命令链是 FC 调度器的概念，NAND 硬件单条执行 |
| `page_type` | 2 | **✅ 通过** | TLC/QLC LDPC 引擎需要 LP/MP/UP/TP 信息选择解码阈值——硬件必须知道当前读取的页类型 |
| `reserved` | 6 | — | 保留 |

#### 13.3.2 fc_descriptor_t → Dispatch Request

| 字段 | 过 Dispatch？ | 理由 |
|------|-------------|------|
| `paa` | **✅ 拆两路**：plane 位 → `cw_bitmap` 的 bit n；行地址 → `plane_paa[plane]` | NAND 物理地址，硬件目标寻址 |
| `buffer_id` | **✅ 解析为指针 → cw_buffers[n]** | FC 预解析为内存指针，硬件直接使用 |
| `next_id` | **❌ 不过** | 链表是 FC 内部结构，FC 在提交时已遍历解析 |
| `status` | **❌ 不过** | 完成状态由硬件回写，不在请求方向携带 |
| `ecc_bits` | **❌ 不过** | 同 status，是回写字段 |
| `config.cw` | **→ 编码进 `n`**（`n % CW_NUM`） | desc 的 CW 号由 PAA 携带，落位时编入 flat 号 n |

#### 13.3.3 Dispatch Completion → fc_command_status_t

| Dispatch Completion 字段 | 回写目标 | 说明 |
|------------------------|---------|------|
| `command_id` | `fc_command_status_t.command_id` | CQ 条目标识 |
| `execution_status` | 驱动层处理 | NAND_RESULT_OK/TIMEOUT/NAND_FAIL |
| `nand_status` | `nand_dispatch_completion_t`保留 | 70h 状态值，供 FC 调试 |
| `cw_status[CW_TOTAL_MAX]` | 聚合 OR → `fc_command_status_t.status` **+** 回写 `fc_descriptor_t.status` | CQ 只带命令级聚合，per-CW 细节留 desc 链 |
| `ecc_bit_count[CW_TOTAL_MAX]` | 只回写 `fc_descriptor_t.ecc_bits`（聚合走 `fc_get_cmd_ecc_max`） | v7 起 CQ 不再携带 per-CW ECC |
| `failed_descriptor_id` | → `fc_command_status_t.desc_id` | 定位错误 Descriptor |

### 13.4 关键设计决策详解

#### 13.4.1 page_type 过、LDPC_MODE 不过

```
page_type (LP/MP/UP/TP) ──→ Dispatch Request ──→ NAND LDPC Engine
                                                     选择解码阈值
ldpc_mode (hard/soft/raw) ──→ FC 数据路径内部 ──→ LDPC Engine 配置
                              （不通过 dispatch）
```

TLC/QLC NAND 的不同页类型（LP=Lower Page, MP=Middle Page, UP=Upper Page, TP=Triple Page）具有不同的原始 BER 特性，LDPC 解码器需要根据页类型选择最优解码阈值。这是硬件 LDPC 引擎必须的信息。

而 `ldpc_mode`（full/hard/soft/raw）是 FC firmware 控制解码策略的参数——FC 在数据路径中配置 LDPC 引擎的工作模式，不需要 NAND 硬件状态机感知。

#### 13.4.2 RAID 在 FC 内部实现

```
FTL: fc_command.config.raid = RAID_ENCODE
                                    │
                                    ▼
FC firmware 内部：
  Start XOR： 读取第 0~2 plane 的数据 → 计算 XOR
  Continue XOR：读取下一个数据块 → 累加 XOR
  End XOR：    将 XOR 结果写入最后一个 plane

dispatch 看到的命令序列：
  [0] OP_NORMAL_PROGRAM_PAGE  plane=0  → 正常写入
  [1] OP_NORMAL_PROGRAM_PAGE  plane=1  → 正常写入
  [2] OP_NORMAL_PROGRAM_PAGE  plane=2  → 正常写入
  [3] OP_NORMAL_PROGRAM_PAGE  plane=3  → 写入 XOR parity（PAA 由 FC 推导）
```

关键点：
- Dispatch 层不感知 RAID 模式——它看到的是 4 个独立的 PROGRAM 命令
- RAID parity 写入哪个 plane 由 FC 决定并生成对应 PAA
- 解码时 FC 读取所有 plane 数据，若某个 CW 解码失败则 XOR 恢复

#### 13.4.3 ECC 比特数反馈链

```
NAND 硬件 LDPC 解码完成
       │
       ├── cw_status[CW_TOTAL_MAX]  (成功/失败/UNC)
       └── ecc_bit_count[CW_TOTAL_MAX] (纠正了多少比特)
                │
                ▼
       nand_dispatch_completion_t
                │
                ▼
       fc_complete_from_nand()
                │
                └──→ fc_descriptor_t.ecc_bits   → Descriptor 回写（v7 起 CQ 不再带 per-CW ECC）
                                                 FTL 走 fc_get_cmd_ecc_max() 取全链最大值
```

**ECC 比特数的价值：**
- **BER 监测**：FTL 持续跟踪每 CW 的 ECC 纠正比特数，生成 BER 热力图
- **磨损均衡触发**：当纠正比特数持续升高时触发 wear-leveling
- **Read Retry 阈值**：纠正比特数逼近 LDPC 解码能力上限时启动 read-retry
- **RAID 恢复决策**：纠正比特数异常时提前触发 RAID recovery

**为什么使用 8 位（最大 255）？** 当前主流 LDPC 硬解码能力约 80~120 bits/1KB，软解码约 200~300 bits/1KB。8 位可覆盖硬解码全范围，软解码超限时直接标记 UNC，不需要更大的字段。

#### 13.4.4 Descriptor 状态回写机制

NAND 完成后，FC 不仅将完成信息写入 CQ，还**逐 CW 回写 descriptor**：

```c
/* fc_complete_from_nand() 中的回写逻辑（v9：按 desc 自己的 PAA 算 flat 号 n） */
desc_id = command->desc_id;
while (desc_id < FC_DESCRIPTOR_POOL_DEPTH) {
    descriptor           = &controller->descriptor_pool[desc_id];
    n                    = paa_to_plane(descriptor->paa) * CW_NUM + paa_to_cw(descriptor->paa);
    descriptor->status   = nand_completion->cw_status[n];
    descriptor->ecc_bits = nand_completion->ecc_bit_count[n];
    desc_id = descriptor->next_id;
}
```

**为何需要双重交付（CQ + Descriptor 回写）？**

| 场景 | CQ 快照足够？ | 需要 Descriptor 回写？ |
|------|-------------|---------------------|
| 正常完成，FTL 检查状态 | ✅ 是 | 可选 |
| Read Retry：FTL 想对特定 CW 重试 | ❌ 需要知道哪个 CW 失败 | **✅ desc_id 标记 + status** |
| RAID Recovery：逐 CW 检查解码状态 | ❌ CQ 只有命令级信息 | **✅ 每个 desc 独立状态** |
| 长链路命令完成，CQ 已出队 | ❌ 状态已丢失 | **✅ Descriptor 保留状态** |
| 错误注入/调试分析 | ❌ | **✅ 状态持续可查** |

#### 13.4.5 module 字段直接路由

`fc_command_status_t.module` 拷贝自 `command->module`，使 FTL 收到 CQ 条目后无需通过 `command_id` 查询命令再决定路由：

```c
// 不用 module 的方式：
status = fc_poll_completion(&completion);
cmd = fc_command_id_to_command(completion.command_id);
switch (cmd->module) { ... }  // 需要一次额外指针查询

// 用 module 的方式：
status = fc_poll_completion(&completion);
switch (completion.module) { ... }  // 直接路由
```

module 枚举值覆盖所有 FC 使用者：

| module | 用途 |
|--------|------|
| `FC_COMMAND_MODULE_HOST_IO` | 主机读写命令 |
| `FC_COMMAND_MODULE_FTL` | FTL 内部元数据操作 |
| `FC_COMMAND_MODULE_GC` | 垃圾回收 |
| `FC_COMMAND_MODULE_READ_RECOVERY` | Read Retry 恢复 |
| `FC_COMMAND_MODULE_METADATA` | 元数据读写 |
| `FC_COMMAND_MODULE_BBT` | Bad Block Table 管理 |
| `FC_COMMAND_MODULE_CHECKPOINT` | Checkpoint/恢复 |
| `FC_COMMAND_MODULE_WEAR_LEVELING` | 磨损均衡 |

### 13.5 Dispatch 请求/完成结构总览

#### 请求方向（FC → NAND）

```
nand_dispatch_request_t (跨边界字段):
┌────────────┬──────┬──────────────────────────────────────┐
│ 字段        │ 来源  │ 说明                                 │
├────────────┼──────┼──────────────────────────────────────┤
│ command_id │ cmd  │ 完成关联                              │
│ operation  │ conf │ 硬件执行的 ONFI 操作                   │
│ ch/ce/lun  │ conf │ 硬件路由                              │
│ cw_bitmap  │ desc │ 选中的 CW 集合（bit n = plane*CW_NUM+cw，可稀疏；v27 起 0 = 空集）│
│ page_type  │ conf │ LP/MP/UP/TP → LDPC 解码阈值选择        │
│ feat_addr  │ cmd  │ SET/GET_FEATURES 寄存器地址（= column） │
│ plane_paa[p] │ desc │ 各 plane 的行地址承载 PAA（命令 plane 集合内每个 plane 都填）│
│ cw_buffers[n] │ desc │ 数据指针；FEATURES 命令指向 g_fc_return_staging │
└────────────┴──────┴──────────────────────────────────────┘
```

> **v7**：`parameter[4]` 字段已删除——内联数据直接经 `cw_buffers[plane*CW_NUM]` 指向控制器级暂存
> `g_fc_return_staging[command_id]`（见 §25.7）。
> **v9**：`cw_count[PLANE_NUM]` + `cws[plane][slot]` 删除，改为 `cw_bitmap` + `plane_paa[]` + `cw_buffers[]`（见 §25.11）。

#### 完成方向（NAND → FC）

```
nand_dispatch_completion_t (跨边界字段):
┌────────────────┬─────────────────────────────────────────┐
│ 字段            │ 说明                                    │
├────────────────┼─────────────────────────────────────────┤
│ command_id     │ 关联回哪个命令                            │
│ execution_stat │ NAND_RESULT_OK / TIMEOUT / NAND_FAIL     │
│ nand_status    │ 70h 状态寄存器值                          │
│ cw_status[8]   │ 每 CW 完成状态（下标 = flat 绝对 CW 号 n）│
│ ecc_bit_c[8]   │ 每 CW ECC 纠正比特数（0~255），同下标空间 │
│ failed_desc_id │ 失败 Descriptor ID                       │
└────────────────┴─────────────────────────────────────────┘
```

### 流程图：完成回写与 CQ 组装（图 13）

`fc_complete_from_nand()`（NAND 完成 → FC 侧收尾）：

```text
nand_dispatch_completion_t（command_id / execution_status / nand_status /
                            cw_status[8] / ecc_bit_count[8] / failed_desc_id）
  │
  ├─ Step 1：回写 descriptor（双重交付之一）
  │     command->desc_id != INVALID 时：
  │     遍历 desc 链（按每个 desc 自己的 PAA 算 flat 号 n = plane*CW_NUM+cw）：
  │       desc->status   = cw_status[n]
  │       desc->ecc_bits = ecc_bit_count[n]
  │       desc_id = desc->next_id
  │
  ├─ Step 2：数据路径（仅 Page Read 命令）
  │     fc_is_page_read() → fc_data_path_process_read()   ← 图 5
  │     （即使 IO_ERROR，已解码的 desc status 已更新）
  │
  ├─ Step 3：组装 fc_command_status_t（12B CQ 条目）
  │     command_id = nand completion 的 command_id
  │     desc_id    = failed_descriptor_id（初值；实为 flat CW 编号）
  │                 Page Read 再覆盖为首个**失败** CW 的 desc_id
  │                 （失败 = FC_CW_STATUS_ERROR_MASK 全集，不只 UNC）
  │     module     = command->module（直接路由，无需查命令）
  │     Page Read → 走链全链 OR status（v28：不再聚合失败位图，上层遍历链自取）
  │                 聚合 ECC 走 fc_get_cmd_ecc_max()（全链最大值）
  │     GET_FEATURES → param.bytes = g_fc_return_staging[command_id]
  │     （其余非 Page Read → 透传 NAND 状态；param 恒 0）
  │
  └─ Step 4：fc_complete_internal → CQ 入队
        CQ 满 → 强制标记 COMPLETED 防泄漏 + 返回 IO_ERROR（fatal 背压）
```

**为什么保留 Descriptor 回写**：

| 场景 | 需要 Desc 回写的原因 |
|------|---------------------|
| Read Retry | 要知道哪个 CW 失败 → desc_id 标记 + status |
| RAID Recovery | 逐 CW 检查解码状态（CQ 只有命令级聚合） |
| 长链命令完成，CQ 已出队 | CQ 状态丢失，Descriptor 保留状态 |
| 聚合 ECC 趋势（BER/慢死预测） | `fc_get_cmd_ecc_max()` 从 desc 链取全链最大值 |
| 错误注入/调试 | 状态持续可查 |

`module` 字段覆盖全部 FC 使用者：HOST_IO / FTL / GC / READ_RECOVERY / METADATA / BBT / CHECKPOINT / WEAR_LEVELING。

### 13.6 内存开销分析

| 结构 | 变更 | 单条大小 | 数量 | 总量变化 |
|------|------|---------|------|---------|
| `fc_command_status_t` | 8B → 12B（+4B） | 12B | 64 | +256B |
| `fc_descriptor_t` | 12B → 16B（+4B） | 16B | 256 | +1KB |
| `nand_dispatch_completion_t` | 新增 `ecc_bit_count[CW_TOTAL_MAX]` | +8B | — | +8B |
| `nand_dispatch_request_t` | 新增 `page_type`（占位已有 reserved） | 0B | — | 0B |
| **合计** | | | | **约 +1.3KB** |

1.3KB 的开销相对于 1MB SRAM 可以忽略不计（0.13%），但获得的能力——BER 监测、Read Retry 触发、RAID 恢复、无 CQ 的 Descriptor 状态查询——是 enterprise-grade SSD 的必要功能。

### 13.7 与行业方案对比

| 维度 | 企业级 SSD（Intel/Samsung） | 消费级（Phison/SMI） | 本方案 |
|------|---------------------------|--------------------|--------|
| ECC 比特数反馈 | ✅ 有，PCIe VDM/MI 接口 | ✅ 有 | ✅ 8-bit/CW |
| Descriptor 回写 | ✅ 硬件自动 | ✅ 固件回写 | ✅ FC 回写 |
| page_type 过 LDPC | ✅ | ✅ | ✅ |
| RAID in firmware | ✅ | ✅ | ✅ |
| module 路由 | ✅ Tag 路由 | ✅ | ✅ |
| CQ 冗余状态 | ✅ 双重交付 | ✅ | ✅ |
| 总开销 | ≤0.5% DRAM | 可忽略 | 0.13% SRAM |

---

## 14. SET/GET_FEATURES 内联数据路径优化

### 14.1 背景

SET_FEATURES（0xEF）和 GET_FEATURES（0xEE）是 ONFI 协议中读写 NAND 内部寄存器的命令，格式为：

- **SET_FEATURES**：发送 1 字节寄存器地址（COLUMN_0）→ 4 字节参数数据
- **GET_FEATURES**：发送 1 字节寄存器地址（COLUMN_0）→ 回读 4 字节参数数据

原始实现中，即使 SET/GET_FEATURES 只传输 ≤5 字节，也分配了完整的 descriptor + buffer（528 字节），浪费了有限的 SRAM 资源池。优化目标是消除这次无谓的分配，让命令通过 `fc_command_t` 内部字段直接承载这 5 字节信息。

### 14.2 数据结构变更

在 `fc_command_t` 中：

1. **`feature_addr:8`** — 从原 `reserved1:11` 中划分 8 位，存储 NAND 寄存器地址。FC → nand_dispatch 传递后，由 COLUMN_0 uop 发送 `(uint8_t)request->column`，即此值。
2. **`parameter[0..3]` 由 `DWORD : 8` → `uint8_t`** — 位域不能取地址，但内联路径需要一个可取地址的 4 字节落点作为 buffer 指针。改为纯字节后增加了 ~4 字节/command，Command Pool 总量 +256 字节。**v7** 起这 4 个字节并入 `fc_param_t param` 联合体（见 §2.2）。

### 14.3 数据流追踪

> **v7 变更**：buffer 不再指向 `&command->parameter0`（那会让提交后的命令体被写，违反「SQ 不可变」）。
> 改为指向控制器级暂存 `g_fc_return_staging[command_id]`（按命令池槽位索引的 4 字节中转），返回值最终经
> CQ 的 `param.bytes` 交回上层。

```
SET_FEATURES:
─────────────
command->feature_addr ──► request.feature_addr ──► task.feature_addr ──► nand_fc_request_t.column (= 0x01)
command->param.bytes  ──► memcpy → g_fc_return_staging[command_id]
                                │ (cw_buffers[plane*CW_NUM] = g_fc_return_staging[command_id]，feature 无 PAA 故 plane 恒 0)
                                ▼
                           NAND 从暂存读取 4 字节发送至寄存器

GET_FEATURES:
─────────────
command->feature_addr ──► (同上) ──► column (= 0x01)
                                │ (cw_buffers[plane*CW_NUM] = g_fc_return_staging[command_id]，plane 恒 0)
                                ▼
                           NAND 回写 4 字节写入暂存
                                │
                                ▼
                           fc_complete_from_nand → completion.param.bytes → CQ
                                │
                                ▼
                           测试 / FTL 从 CQ 条目读取
```

### 14.4 关键代码路径

#### FC 层 — 跳过 descriptor 分配

`fc_command_validate()` (fc_command.c) — 允许 SET/GET_FEATURES 的 desc_id = FC_DESCRIPTOR_ID_INVALID：

```c
if ((command->config.operation != OP_SET_FEATURES) &&
    (command->config.operation != OP_GET_FEATURES) &&
    nand_operation_has_data((uint8_t)command->config.operation) &&
    (command->desc_id == FC_DESCRIPTOR_ID_INVALID))
{
    return FC_STATUS_INVALID_ARGUMENT;
}
```

建链入口跳过 desc 分配——v13 前是 `fc_command_configure_descriptors_internal()` (fc.c)，
v13 后是 FTL 侧的 `ftl_cmd_configure_descriptors()`（判断收敛在 `ftl_cmd_needs_desc_chain()`）：

```c
if ((nand_operation_has_data((uint8_t)command->config.operation) == 0U) ||
    (command->config.operation == OP_SET_FEATURES) ||
    (command->config.operation == OP_GET_FEATURES))
{
    command->desc_id = FC_DESCRIPTOR_ID_INVALID;
    return FC_STATUS_OK;   /* v13 后返回 FTL_OK */
}
```

`fc_build_dispatch_request()` (fc.c) — 构造 dispatch request 时直接指向 command parameter 地址：

```c
request.feature_addr = (uint8_t)command->feature_addr;

/* 在 descriptor 遍历分支之前插入 SET/GET_FEATURES 分支 */
else if ((command->config.operation == OP_SET_FEATURES) ||
         (command->config.operation == OP_GET_FEATURES))
{
    /* feature 无 PAA，plane 恒 0。v9：掩码只置本 plane 的 CW0 位（Feature 恒 4 字节单次传输，
     * 与 config.cw 无关），数据落 cw_buffers[plane*CW_NUM]。
     * v7：buffer 指向控制器级暂存，不指向 command（SQ 提交后不可写）。 */
    request.cw_bitmap                           = 1UL << (request->plane * CW_NUM);
    request.cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
    if (SET) memcpy(g_fc_return_staging[command_id], command->param.bytes, 4U);
    else     memset(g_fc_return_staging[command_id], 0, 4U);
}
```

#### Dispatch 层 — 正确处理 column 和数据

`nand_dispatch_task_load()` 拷贝 feature_addr 到 task（v7 起不再有 `parameter[]` 中转）：

```c
task->feature_addr = request->feature_addr;
```

`req_feature()`（nand_dispatch_tables.c）SET/GET_FEATURES 分支：

```c
task->request = (nand_fc_request_t){.column = (uint16_t)task->feature_addr, .row = {0U},
                                    .data = task->cw_buffers[task->plane * CW_NUM], .data_length = 4U};
```

> **说明**：此处 `cw_buffers[task->plane * CW_NUM]` 已经通过 `nand_dispatch_task_load()` 从
> `request.cw_buffers[plane*CW_NUM]`（即 `g_fc_return_staging[command_id]`）拷贝，指向控制器级暂存。
> feature 寄存器是 die 级、无 plane 语义，`command->paa` 从不设置，故 plane 恒 0。GET_FEATURES 的回写
> 数据经此通道写入暂存，再由 `fc_complete_from_nand` 拷进 CQ 的 `param.bytes`。

#### 完成路径 — 无 desc 回写，但有返回段搬运

`fc_complete_from_nand()` 检查 desc_id 后才执行 descriptor 回写循环。SET/GET_FEATURES 的
desc_id = FC_DESCRIPTOR_ID_INVALID，自动跳过回写。GET_FEATURES 的返回值从
`g_fc_return_staging[command_id]` 拷入 `completion.param.bytes`（SQ 命令体保持不被写）。

### 14.5 读回路径演进（v7 更新）

**v2 问题**：最初实现中，`nand_dispatch_submit()` 将 `cw_buffers[0]` 重定向到 `task->parameter[]`，
导致 GET_FEATURES 的回写数据进入 task.parameter 而非 command.parameter，测试读到的为零值。
**v2 修复**：去掉重定向，buffer 始终指向 command.parameter0。

**v7 问题**：v2 的修复让 NAND 引擎（经 `cw_buffers[plane*CW_NUM]`）直接写 FC 命令池中的 `command->param`——
命令已提交，却仍在被写，违反「SQ 不可变」（命令配置完即算完成）。
**v7 修复**：buffer 指向控制器级暂存 `g_fc_return_staging[FC_COMMAND_POOL_DEPTH][4]`（按 `command_id` 索引，
`command_id` 恒 < 池深，索引安全）；返回值唯一归宿是 CQ 的 `param` 联合体。
`nand_dispatch_completion_t` **不**为此新增字段。

### 14.6 资源节省

| 资源 | 优化前（每条 SET/GET_FEATURES 命令） | 优化后 |
|------|-----------------------------------|--------|
| Descriptor | 1 个（16 字节） | 0 |
| Buffer | 1 个（528 字节） | 0 |
| 总 SRAM | 544 字节/次 | 0 |
| 全局开销 | — | Command Pool +256 字节（parameter 类型变更） |

实际节省：每次 SET/GET_FEATURES 操作省去 544 字节分配。在 GC 流程中频繁读/写 ONFI 寄存器的场景下，可显著降低资源池竞争。

### 14.7 设计演进

| 阶段 | 方案 | 问题 |
|------|------|------|
| 原始方案 | 分配 desc + buffer（528B）传递 5 字节 | 浪费资源 |
| 内联方案 v1 | `fc_command_t.reserved1:11` → `feature_addr:8`；保留 `DWORD parameter : 8` | 位域不可取地址，读回数据无法送达 |
| 内联方案 v2 | parameter 改为 `uint8_t`；buffer 直接指向 `&command->parameter0`；去掉 task.parameter 重定向 | 读回数据正确落入 command，但提交后仍在写 SQ |
| 内联方案 v3（最终） | parameter 并入 `fc_param_t`；buffer 指向控制器级暂存 `g_fc_return_staging[command_id]`；返回值经 CQ 的 `param` 交回 | 读回数据正确且不违反「SQ 不可变」 |

---

## 15. nand_dispatch 函数指针表重构 (Phase D)

### 15.1 动机

`nand_dispatch_execute_command()` 和 `nand_dispatch_poll()` 中大量 if-else 链根据 `task->operation` 判断 NAND 操作类型并构造 `nand_fc_request_t`。每条 if-else 链包含 6-7 次条件比较，不仅增加执行时间，还带来分支预测惩罚。

在嵌入式 Cortex-R5/M7 上，分支预测错误代价约 12-14 周期，而函数指针间接跳转固定为 2-3 周期。将条件链替换为函数指针表可显著提升 dispatch 层的执行确定性。

### 15.2 设计方案

两张独立函数指针表，均以 `OP_*`（0~28）为直接下标：

| 表 | 类型 | 用途 | 替代目标 |
|----|------|------|---------|
| `g_nand_dispatch_req_table[OP_COUNT]` | `void (*)(nand_dispatch_task_t *)` | 从 task 预解析数据构造 `nand_fc_request_t` | `nand_dispatch_poll()` 中的 ~90 行 if-else 链 |
| `g_nand_dispatch_completion_table[OP_COUNT]` | `void (*)(nand_dispatch_task_t *, uint8_t, const nand_fc_completion_t *)` | 设置 cw_status 和 failed_descriptor_id | `nand_dispatch_task_complete()` 中的 if-else 链 |

### 15.3 Request 构建 handler

共 7 个 handler，覆盖全部 29 个 OP：

| Handler | 覆盖的 OP | 行为 |
|---------|----------|------|
| `req_program_page` | NORMAL_PROGRAM_PAGE, OTP_DATA_PROGRAM, PROGRAM_PAGE_CACHE | 将各 CW buffer 拼接到 `g_nand_channel_page_buffer[task->ch][task->ce][task->lun]`，request.data_length = PAGE_SIZE |
| `req_read_page` | NORMAL_READ_PAGE, OTP_DATA_READ | 仅 CW0：request = (column=0, row, buffer=cw_buffers[task->plane], length=PAGE_SIZE/4) |
| `req_cache_read` | READ_PAGE_CACHE_SEQ/RANDOM/LAST | 一次性读 full page：request = (0, row, page_buffer, PAGE_SIZE) |
| `req_erase` | ERASE_BLOCK | 纯地址：request = (0, row, NULL, 0) |
| `req_feature` | SET_FEATURES, GET_FEATURES | column = feature_addr：request = (feature_addr, 0, cw_buffers[task->plane], 4)（plane 恒 0） |
| `req_single` | READ_ID, READ_PARAM_PAGE, READ_UNIQUE_ID, READ_STATUS, READ_STATUS_ENH, RANDOM_DATA_READ 等 | 通用固定长度：length 从 `g_nand_operation_map[op].data_length` 读取 |
| `req_no_data` | RESET, BLOCK_LOCK, BLOCK_UNLOCK 等 | 无数据传输，纯 row 地址 |

### 15.4 Completion 策略 handler

共 3 个 handler：

| Handler | 行为 | 适用 OP |
|---------|------|--------|
| `completion_all_cws` | 所有 CW 共享同一完成状态；fail → failed_descriptor_id = 0 | PROGRAM (3)、ERASE |
| `completion_per_cw` | 只设当前 cw_index 的状态；fail → failed_descriptor_id = cw_index | READ (2)、CACHE_READ (3) |
| `completion_single` | 只设 cw_status[0] | 其余所有 |

### 15.5 分派点简化

**重构前**（`nand_dispatch_poll()` 出队处）：

```c
if (op == OP_NORMAL_PROGRAM_PAGE || op == OP_OTP_DATA_PROGRAM || ...) {
    // ~10 行 PROGRAM 拼 buffer 逻辑
} else if (op == OP_NORMAL_READ_PAGE || ...) {
    // ~6 行 READ 构建逻辑
} else if (op == OP_READ_PAGE_CACHE_SEQ || ...) {
    // ~6 行 CACHE 构建逻辑
} else if (...) ... // 共 7 个分支 ~90 行
```

**重构后**（1 行）：

```c
g_nand_dispatch_req_table[task->operation](task);
```

### 15.6 `nand_operation_map_t` 协议属性增强

`nand_operation_map_t` 新增两个协议属性字段（它们是 ONFI 协议属性，不是 dispatch 策略）：

```c
typedef struct {
    // ... 原有字段不变
    uint8_t  has_data;     /* 操作是否需要数据传输 */
    uint16_t data_length;  /* 数据传输字节数（0 = 无数据） */
    // ...
} nand_operation_map_t;
```

对应删除两个独立函数 `nand_operation_has_data()` 和 `nand_operation_data_length()`，改为宏直接查表：

```c
#define nand_operation_has_data(op) \
    ((op) < OP_COUNT ? g_nand_operation_map[(op)].has_data : 0U)
#define nand_operation_data_length(op) \
    ((op) < OP_COUNT ? g_nand_operation_map[(op)].data_length : 0U)
```

初始化表改用 designated initializer，每个字段名显式标注，消除之前依赖字段顺序可能引发的歧义：

```c
[OP_NORMAL_READ_PAGE] = {
    .operation   = OP_NORMAL_READ_PAGE,
    .name        = "READ_PAGE",
    .command_1   = NAND_CMD_READ_PAGE_SETUP,
    .command_2   = NAND_CMD_READ_PAGE_CONFIRM,
    .has_data    = 1U,
    .data_length = PAGE_SIZE,
    .uops        = g_nand_normal_read_page_uops,
    // ...
},
```

### 15.7 废弃代码清理

以下函数被标记为废弃并删除：

| 函数 | 原因 |
|------|------|
| `nand_dispatch_execute_request()` | 已被 `nand_fc_start/step` 非阻塞接口替代 |
| `nand_dispatch_execute_page()` | 逻辑分散到 `req_program_page` / `req_read_page` handler |
| `nand_dispatch_execute_command()` | 标记 `__attribute__((unused))`，功能完全由函数指针表覆盖 |

### 15.8 文件结构

```
src/nand/
├── nand_dispatch.c              # 核心：init, submit, poll, task_complete (~200 行)
├── nand_dispatch_internal.h     # NEW：内部类型定义（task_t + 函数指针表类型）
├── nand_dispatch_tables.c       # NEW：7 个 req handler + 3 个 completion handler + 表 (~200 行)
├── onfi_1_0.c                   # 改：map 表 +2 字段，改用 designated initializer

include/nand/
├── nand_dispatch.h              # 公用接口不变
├── onfi_1_0.h                   # map_t +2 字段，函数声明 → 宏
```

### 15.9 扩展性

未来加新操作（如 Phase 4 LDPC 的专用读取命令）只需三步：

1. 在 `onfi_1_0.h` 定义 `OP_LDPC_READ = 29`，`OP_COUNT` 递增
2. 在 `onfi_1_0.c` 表加一行 `[OP_LDPC_READ] = { ... has_data = 1U, ... }`
3. 在 `nand_dispatch_tables.c` 加 handler + 一行表项 `[OP_LDPC_READ] = req_ldpc_read`

**不需要修改任何 if-else 或 switch 语句。**

### 15.10 性能对比

| 指标 | if-else 链（重构前） | 函数指针表（重构后） |
|------|-------------------|-------------------|
| 分支数 | 6-7 次条件比较 + 嵌套 | 1 次间接跳转（O(1)） |
| 执行路径确定性 | ❌ 取决于 OP 值 | ✅ 固定 2-3 周期 |
| 分支预测惩罚 | 高（每 if 都可能预测失败） | 无（jump table） |
| 添加新操作 | 改 2 处 if-else 链 | 加 handler + 表项 |

### 15.11 与 nand_engine 的架构一致性

`nand_engine` 层本身已使用函数指针表（`backend->handlers[uop->type]`）驱动 UOP 执行：
```c
handler = state->backend->handlers[uop->type];
result = handler(state->backend->context, uop, ...);
```

本次重构使 dispatch 层与 engine 层的架构风格一致：**上层（dispatch）用函数指针表选择策略，下层（engine）用函数指针表执行微操作**。两层均无条件判断链，均为纯表驱动。

### 15.12 RANDOM_DATA_READ 的数据传输长度说明

RANDOM_DATA_READ（05h-C1-C2-E0h）等命令的数据传输长度不由命令码本身决定，而是由 FMC 硬件产生的 RE# 时钟脉冲数控制。`nand_operation_map_t.data_length` 中记录的值不是命令的"固有属性"，而是**传输长度的默认值**，通过 `nand_fc_request_t.data_length` 传递给 FMC 层作为 DMA/RE# 脉冲计数器：

```c
[OP_RANDOM_DATA_READ] = {
    ...
    .data_length = (PAGE_SIZE / CW_NUM),
    /* 注：RE# 时钟次数由 FMC 控制，data_length 仅作为传输长度参数
     * 传递给 FMC 层的 DMA/RE# 脉冲计数器 */
},
```

`req_single` handler 通过 `g_nand_operation_map[op].data_length` 读取该值，无需为每个小操作各写一个 handler。

---

## 16. 命令链执行设计 (Phase D — 续)

> ⚠ **运行时状态提示**：命令链是「**能力已实现、但 FTL 运行时未启用**」的路径。FTL 所有命令恒设 `config.chain = FC_CHAIN_END`（`src/ftl/` 共 23 处），运行时 `count` 恒为 1，实际只走**单命令路径**（`fc_poll_dispatch_single`）。本章讲的是链式执行的设计与单测覆盖；运行时主干请见 [§23.9「澄清：运行时其实不走命令链」](#239-澄清运行时其实不走命令链)。
>
> 🔁 **v8 语义变更（2026-10-06，per-LUN FIFO）**：本章 16.2 起的方案描述**已被取代**。链不再是"执行中的 chain 对象 + cursor"，而是 **per-LUN 待处理 FIFO 里的连续 N 槽**（每槽 = 一条命令）：
> - **原子**：`nand_dispatch_submit_chain()` 先判 `room >= N`，不足则整批拒收、无副作用；
> - **不可插入**：连续槽 + FIFO 严格顺序，同 LUN 的后续命令（含 HSQ）只能排在链后；
> - **逐条完成**：每条命令各回一份 completion（旧模型只在链尾回一份，中间命令的 `cw_status[]` 被吞）；
> - NAND 侧**无链标记**，`chain->cursor` / `chain_advance` 已删除。
>
> 详见 `docs/changelog/fc_nand_per_lun_fifo.md`；本节余下内容保留为历史设计记录。

### 16.1 问题背景

Read Retry 流程要求三个命令严格依次执行，中间不能被任何其他命令插入：

```
SET_FEATURES(偏移电压)  →  RETRY READ  →  SET_FEATURES(恢复电压)
  └── 命令 1            └── 命令 2       └── 命令 3
```

同一 LUN 的 HSQ 命令（如 GC 强制擦除）可能在任意时刻插入，打乱 Read Retry 的顺序，导致电压未被恢复。

### 16.2 方案对比

实现"不可打断的命令序列"有两种候选方案：

#### 方案 A：chain_next_id（在 poll 路径中动态跳转）

在 `nand_dispatch_task_t` 中增加 `uint16_t chain_next_id`。当前命令完成后 poll 检查该字段，非 INVALID 则不清 LUN busy、直接加载下一命令。

```c
if (task->chain_next_id != FC_COMMAND_ID_INVALID) {
    // 不清 LUN busy, 从 command pool 加载下一命令
    task = load_from_command_id(task->chain_next_id);
    nand_fc_start(…);
    continue;  // 继续等
}
```

#### 方案 B：per-LUN chain buffer（加 task 维度，选中方案）

将 `g_nand_dispatch_tasks[ch][ce][lun]` 扩展为 `g_nand_dispatch_chains[ch][ce][lun].tasks[NAND_DISPATCH_CHAIN_DEPTH]`，submit_chain 预加载整链，poll 通过 cursor 自动推进。

```c
// 当前命令完成
if (chain->cursor + 1U < chain->count) {
    chain->cursor++;           // 推进到下一命令
    task = &chain->tasks[chain->cursor];
    nand_fc_start(…);           // 不清 LUN busy, 不返回 completion
    continue;
}
// 整链结束
*completion = task->completion;
chain->count = 0;               // 释放 LUN
```

#### 对比

| 维度 | A: chain_next_id | B: per-LUN chain buffer ✅ |
|------|-----------------|--------------------------|
| 修改范围 | poll 完成路径 + task | nand_dispatch_submit / poll / scheduler |
| poll() 纯度 | ❌ 被污染，要感知链跳转 | ✅ 纯粹，与单命令代码路径一致 |
| 整链可见性 | ❌ 链只有部分在 task 中 | ✅ tasks[0..N-1] 完整可见 |
| 调试友好 | ❌ 链信息分散 | ✅ dump chain buffer 即得全貌 |
| watchdog / QoS | ⚠️ 难区分"长命令"还是"链" | ✅ 知道整链长度 |
| 扩展性 | 链内跳转逻辑与 CW chaining 交织 | 与 CW chaining 正交（各自独立循环） |
| 资源预先评估 | ❌ 走一步看一步 | ✅ 提前知道占多少链 slot |
| 队列深度需求 | 1 就够 | 需能容整链（当前 64 足够） |
| **企业类似方案** | 少量消费级固件使用 | 主流：模拟硬件 sequencer 槽位 |

**选择方案 B 的理由**：
1. poll() 不被链语义污染——它看到的仍然是"逐命令执行"，仅多了"连续执行不返回"的能力
2. 与 CW chaining 正交——CW 链在外层 for(;;) 循环中处理，命令链在内层 while(1) 中推进，互不干扰
3. 架构与企业级 SSD 固件一致——本质是在 FC 层用软件模拟硬件 sequencer 的槽位机制

### 16.3 整体架构

```
FTL / Read Recovery (构造 chain 命令)
  │ config.chain=1 → config.chain=0
  ▼
SQ: [cmd_A(SET_FEATURES, chain=1)] [cmd_B(READ, chain=1)] [cmd_C(SET_FEATURES, chain=0)]
                              │
                              ▼
fc_scheduler_select_chain():
  1. 从 SQ 弹出 cmd_A
  2. 检测 cmd_A.config.chain == 1 → 连续 peek 后续命令
  3. 收集 cmd_B(chain=1), cmd_C(chain=0) → 验证同一 ch/ce/lun
  4. 一次性弹出整链 → chain={ids: [A, B, C], count=3}
                              │
                              ▼
fc_poll_internal() → fc_poll_dispatch_chain():
  for i=0..2:
    校验 QUEUED → 置 ACTIVE
    page write 则 fc_data_path_encode()   ← 注意：链式路径不做 META/PAA 预填
    fc_build_dispatch_request(cmd_i) → reqs[i] = { … }
  nand_dispatch_submit_chain(reqs, 3)
                              │
                              ▼
nand_dispatch_submit_chain():
  原子检查: LUN chain->count==0 AND 队列空间够 3 条
  chain->tasks[0] = cmd_A   ← 预加载
  chain->tasks[1] = cmd_B   ← 预加载
  chain->tasks[2] = cmd_C   ← 预加载
  chain->count = 3, cursor = 0
  入队 command_id_A, command_id_B, command_id_C
                              │
                              ▼
nand_dispatch_poll():
  Phase 1 ─── cursor=0 → step tasks[0] → 完成 → cursor++ → start tasks[1]
  Phase 1 ─── cursor=1 → step tasks[1] → 完成 → cursor++ → start tasks[2]
  Phase 1 ─── cursor=2 → step tasks[2] → 完成 → count=0 → 返回 completion
                                ↑ 链执行期间 LUN busy, HSQ 无法插入
```

### 16.4 数据结构

#### per-LUN chain buffer

```c
// nand_dispatch_internal.h

#define NAND_DISPATCH_CHAIN_DEPTH 4U

typedef struct {
    nand_dispatch_task_t tasks[NAND_DISPATCH_CHAIN_DEPTH];
    uint8_t              count;   /* 0=空闲, 1=单命令, 3=3条链 */
    uint8_t              cursor;  /* 当前执行 tasks[cursor] */
} nand_dispatch_lun_chain_t;
```

#### 调度器链结构

```c
// fc_scheduler.h

#define FC_SCHEDULER_CHAIN_MAX NAND_DISPATCH_CHAIN_DEPTH /* = 4U，与 dispatch 链缓冲深度一致 */

typedef struct {
    fc_command_id_t ids[FC_SCHEDULER_CHAIN_MAX];
    uint8_t         count;  /* 0=空, 1=单命令, 2+=链 */
} fc_scheduler_chain_t;
```

`nand_dispatch_task_t` 新增 `ce` 和 `lun` 字段（此前只有 `ch`，单 CE/LUN 时代硬编码），供后续多 CE/LUN 使用。

### 16.5 submit_chain 原子性

`nand_dispatch_submit_chain()` 提供 ACID 语义的链提交：

```
原子性检查:
  if (chain->count != 0)  → LUN 忙, 拒绝
  if (queue->count + count > NAND_CHANNEL_QUEUE_DEPTH)  → 队列空间不够, 拒绝
  // 注意：两个条件同时满足才通过，不会出现"部分入队"的情况

预加载:
  for i = 0..count-1:
    tasks[i] ← 从 requests[i] 拷贝
  chain->count = count, cursor = 0

入队:
  for i = 0..count-1:
    queue->entries[tail++] = requests[i].command_id
```

关键语义：**队列空间不足整链时返回 NOT_READY，无一进入队列。** 调用者下次轮询时再试。

### 16.6 poll 链推进逻辑

poll() 中 Phase 1 stepping 循环内的链推进：

```c
/* 内层 while(1): 单命令完成 → CW chaining → 命令链推进 → 继续 step */
while (1) {
    task = &chain->tasks[chain->cursor];
    result = nand_fc_step(&task->transaction, &nand_completion);
    if (result == NAND_RESULT_NOT_READY) break;

    /* CW 级 chaining (多 CW READ) */
    for (;;) {
        nand_dispatch_task_complete(task, result, &nand_completion);
        if (CW 全部完成) break;
        task->cw_index++;
        task->operation = OP_RANDOM_DATA_READ;
        nand_fc_start(…);
    }

    /* 命令链推进 */
    if (chain->cursor + 1U < chain->count) {
        chain->cursor++;           /* ← 不下发 completion, 不清 LUN busy */
        task = &chain->tasks[chain->cursor];
        g_nand_dispatch_req_table[task->operation](task);
        nand_fc_start(…);
        continue;                  /* while(1) 继续 step 下一命令 */
    }

    /* 整链结束 → 唯一出口 */
    *completion = task->completion;
    chain->count = 0;
    return result;
}
```

### 16.7 调度器链检测

`fc_scheduler_select_chain()` 取代原有的 `fc_scheduler_select()` 成为 FC 的主入口：

```c
fc_status_t fc_scheduler_select_chain(fc_scheduler_t *scheduler, fc_scheduler_chain_t *chain)
{
    /* Phase 1: HSQ 优先 — HSQ 命令不参与链 */
    if (HSQ 非空) {
        找第一个目标不忙的命令;
        弹出一条 → chain->count = 1;
        返回;
    }

    /* Phase 2: 普通 SQ */
    for (遍历 SQ) {
        if (目标忙) { 旋转跳过; continue; }

        弹出队头 → chain->ids[0];

        if (队头.config.chain == 0) {
            /* 单命令 */
            chain->count = 1;
            return;
        }

        /* chain == 1: 收集同 target 的后续命令 */
        while (chain_len < FC_SCHEDULER_CHAIN_MAX) {
            next = peek(0);
            if (next.ch/ce/lun != 当前) break;
            if (next.config.chain == 0) { 加入; break; }
            加入; /* chain=1, 继续 */
        }
        chain->count = chain_len;
        return;
    }
    return 队列空;
}
```

**连续性保证**：链中命令必须在 SQ 中连续（由 FTL/Read Recovery 在提交时保证），中间不能插入其他 LUN 的命令。`scheduler_collect_chain` 会在发现 ch/ce/lun 变化时截断链，回退到单命令模式。

### 16.8 文件变更

| 文件 | 说明 |
|------|------|
| `include/nand/nand_dispatch.h` | 新增 `NAND_DISPATCH_CHAIN_DEPTH`、`nand_dispatch_submit_chain()` 声明 |
| `src/nand/nand_dispatch_internal.h` | 新增 `task.ce/lun`、`nand_dispatch_lun_chain_t` |
| `src/nand/nand_dispatch.c` | `g_nand_dispatch_tasks[][][]` → `g_nand_dispatch_chains[][][].tasks[]`；新增 `nand_dispatch_task_load()`；`submit()` 改用 chain；新增 `submit_chain()`；`poll()` Phase 1 chain 推进 + Phase 2 出队适配 |
| `include/fc/fc_scheduler.h` | 新增 `FC_SCHEDULER_CHAIN_MAX`、`fc_scheduler_chain_t`、`fc_scheduler_select_chain()` |
| `src/fc/fc_scheduler.c` | 新增 `fc_scheduler_select_chain()` 链识别 + 整链弹出 |
| `include/fc/fc_queue.h` | 新增 `fc_submission_queue_peek()`、`fc_submission_queue_pop_n()` |
| `src/fc/fc_queue.c` | 实现 `peek()`、`pop_n()` |
| `src/fc/fc.c` | `fc_poll_internal()` 改用 `fc_scheduler_select_chain()`，增加链式命令构建 + `submit_chain` 路径 |

### 16.9 使用方式（FTL / Read Recovery）

> ⚠ **与实现不符的历史示例**：以下代码为 Phase D 设计期的接口示意（`config.chain = 1` / `fc_command_submit`），**当前 FTL 运行时并不这样用**——Read Recovery 实际是**三条独立提交 + 同步等待**的命令（`src/ftl/read_recovery.c`：各自 `fc_submit` + `ftl_internal_sync_wait`），并非原子链。示例仅用于说明链式 API 的语义。

FTL 或 Read Recovery 模块提交 Read Retry 链时：

```c
/* 提交三条链式命令到 SQ */
command_A.config.chain = 1;  // SET_FEATURES(偏移)
command_B.config.chain = 1;  // RETRY_READ
command_C.config.chain = 0;  // SET_FEATURES(恢复) — chain=0 表示链结束

fc_command_submit(&command_A);  // 按 A → B → C 顺序连续 submit
fc_command_submit(&command_B);
fc_command_submit(&command_C);
```

调度器自动识别 `chain` 标志，dispatch 层自动按序执行。HSQ 在整个链执行期间无法插入该 LUN。

> ⚠ **「HSQ 在整个链执行期间无法插入」这句到 v52 才真正成立**。链长 ≤ `FC_SCHEDULER_CHAIN_MAX`（4）
> 时，整条链一批进 FIFO，`nand_dispatch_submit_chain` 保证一批内原子，确实插不进；但**链长 > 4**
> 时会被拆成多批、分多次填充 pass 下发，而 v51 及以前**批与批之间无锁** ⇒ HSQ 可插在批边界上。
> v52 新增跨 pass 持久的 per-LUN 链锁（`fc_scheduler_t.chain_owner`，见 §2.7）补上这段：
> 链一旦开始，该 LUN 的填充权归链所在队列独占，直到链 END 下发。

### 16.10 与 Phase D (函数指针表) 的关系

| 子阶段 | §15 函数指针表 | §16 命令链 |
|--------|---------------|-----------|
| 状态 | ✅ 已完成代码+文档 | ✅ 已完成代码+文档 |
| 解决的问题 | if-else 链 → O(1) 查表 | 命令原子性 + HSQ 防插入 |
| 改动范围 | nand_dispatch 内部 | nand_dispatch + fc_scheduler + fc |
| 代码编译 | ✅ 通过 | ✅ 通过 |

### 流程图：命令链执行（图 11 配套：调度器填充 pass）

> 🔁 **v8 起本节已取代**：`fc_scheduler_select_chain` / `fc_scheduler_select` 与 busy 表、读饥饿保护全部删除。
> 选择逻辑搬进 `src/fc/fc.c` 的 `fc_poll_fill()`——一次扫描 HSQ/SQ，按 LUN 保序把能放下的命令（含链）提交进
> NAND 的 per-LUN FIFO。下方为**旧实现**的历史记录，保留以对照语义变化。

```text
fc_poll_fill(controller)                ← 一次 pass（v8）
  │
  ├─ 快照 HSQ 数 / SQ 数
  │
  ├─ 扫 HSQ（快照数，HSQ 与 SQ 同一套链逻辑）
  │    need = fc_poll_chain_length(hsq, head)   // 链头：连续 chain=1 段，≤ CHAIN_MAX(4)
  │    room = nand_dispatch_lun_free(ch,ce,lun)
  │    room >= need → 取出 need 条（链则整条）→ submit / submit_chain
  │    否则 → 该 LUN 本轮挂起（同 LUN 后续跳过，含 SQ）
  │
  └─ 扫 SQ（快照数）
       need = fc_poll_chain_length(sq, head)
       room = nand_dispatch_lun_free(ch,ce,lun)
       room >= need → 取出 need 条（链则整条）→ submit / submit_chain
       否则 → 该 LUN 本轮挂起，命令回推队尾（保序）
       扫完即止（不因回推的命令再入队）

返回：有下发 → OK；下发失败 → IO_ERROR；两队列空 → QUEUE_EMPTY；无 room → NOT_READY
```

**旋转语义**：`pop(队头)+push(队尾)` 只动 head/tail 指针，Command Pool 内容不变；一次完整 pass 等价于一轮轮转，
未下发的命令保持相对顺序。

<details>
<summary>旧实现（v7 及以前，历史记录）</summary>

```text
select_chain(scheduler, chain)
  │
  ├─ Phase 1：HSQ 非空？
  │    遍历 HSQ 找目标 (ch,ce,lun) 不忙的命令
  │      ├─ 找到 → pop → chain.count=1（HSQ 不参与链式）→ 返回
  │      └─ 全忙 → 返回 NOT_READY
  │
  └─ Phase 2：普通 SQ
       前置扫描：consecutive_write_count ≥ 预算(3) 时
                  扫描 SQ 是否存在读命令（sq_has_read）
                  （全写队列 → 不触发保护 → 避免死锁）
       for index in SQ:
         目标忙 → 旋转到队尾，继续
         读饥饿保护：写计数 ≥3 且队头非读且 sq_has_read → 旋转，继续
         pop 队头 → chain.ids[0]；更新写计数（读→0 / 非读→++）
         config.chain==0 → chain.count=1 → 返回
         config.chain==1 → 收集链：
           while chain_len < 4：
             peek 队头
             不同 ch/ce/lun → 截断（回退单命令）
             同 target → pop 加入；chain==0 → 结束；chain==1 → 继续
       queue 空 → QUEUE_EMPTY / 全忙 → NOT_READY
```

</details>


---

## 17. Multi-Plane 操作支持与行地址数组重构 (Phase E)

### 17.1 动机

MT29F4G08ABADAWP 支持 2 Plane 操作（Multi-Plane READ/PROGRAM/ERASE），但现有架构将 `cw_paa/buffers` 设计为平面一维数组 `[CW_NUM]`(4)，无法支持 Multi-Plane 场景中 `CW_NUM × PLANE_NUM = 8` 的 CW 总数。

同时，`nand_command_request_t.row` 为单值 `uint32_t`，Multi-Plane 操作需要发送两组行地址（Plane 0 和 Plane 1），而地址周期中的 plane bit 差异仅在于 BA6。

> **注（Phase E 之后的修订）**：Phase E 引入 `[PLANE_NUM][CW_NUM]` 时，第一维在**单 plane** 命令下仍恒取 `[0]`（"第几组槽位"语义），只有 multi-plane 命令按物理 plane 落位。后续修订把第一维统一为**物理 plane 索引**：新增 `nand_dispatch_request_t.plane` / `nand_dispatch_task_t.plane`，单 plane 命令按 `command->paa` 的 plane 落位到 `cws[plane][*]` / `cw_paa[plane][*]`。
> **再后续（v9）**：二维 `cws[PLANE_NUM][CW_NUM]` 整体删除，改为**一维 flat 索引** `cw_mask` + `cw_buffers[n]`（`n = plane*CW_NUM + cw`），行地址拆到 per-plane 的 `plane_paa[PLANE_NUM]`。§17.2 起的方案对比/结构/代码均为 Phase E 历史记录，未随该修订重写，看当前实现请读 §25.11。

### 17.2 数据维度扩展

#### 方案对比

| 方案 | 做法 | 评价 |
|------|------|------|
| A: 展平 cw_count | `cw_paa[CW_TOTAL_MAX]` 展平为 8 个，`cw_count` 语义变为"总 CW 数" | ❌ 丢失 plane 维度信息，每 plane CW 数量不明确 |
| B: 加 plane 维度（选中） | `cw_paa[PLANE_NUM][CW_NUM]`，`cw_count` 仍为"每 plane CW 数"，`plane_count` 标明 plane 数 | ✅ 维度正交，语义清晰 |

选中方案 B 后各层数据结构的变化：

```c
// nand_dispatch_request_t — 请求
nand_dispatch_cw_info_t cws[PLANE_NUM][CW_NUM];  // [plane][cw]

// nand_dispatch_task_t — 任务
nand_paa_t cw_paa[PLANE_NUM][CW_NUM];
uint8_t   *cw_buffers[PLANE_NUM][CW_NUM];
uint8_t    cw_count;     // 每 plane CW 数
uint8_t    plane_count;  // plane 数 (1/2)

// nand_dispatch_completion_t — 完成
uint8_t cw_status[CW_TOTAL_MAX];      // 8 个（vs 原有 4 个）
uint8_t ecc_bit_count[CW_TOTAL_MAX];
```

`fc_cmd_conf_t.cw` 位宽不变（仍为 2 位，最大表示 4 CW/plane），Multi-Plane 通过独立 OP 码区分。

### 17.3 行地址数组重构

#### 方案对比

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

### 17.4 OP 码设计： WITH_DATA_OUT / WITHOUT_DATA_OUT

#### 设计原因

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

#### uop 序列差异示例

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

### 17.5 Multi-Plane 操作 OP 码

| OP 码 | 值 | uop 序列 | data_length |
|-------|----|---------|-------------|
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | 12 | 70h轮询→00h→C1C2R1R2R3→30h→tWB→tR→Dout→70h轮询→CHECK_FAIL | PAGE_SIZE |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | 33 | 70h轮询→00h→C1C2R1R2R3→30h→tWB→tR→70h轮询→CHECK_FAIL | PAGE_SIZE |
| `OP_RANDOM_DATA_READ` | 8 | 70h轮询→05h→C1C2→E0h→tR→Dout→70h轮询 | CW_SIZE |
| `OP_RANDOM_DATA_READ_MULTI_PLANE` | 41 | 70h轮询→06h→C1C2→R1R2R3→E0h→tR→Dout→70h轮询 | CW_SIZE |
| `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT` | 29 | 70h轮询→00h→C1C2R1R2R3(P0)→00h→C1C2R1R2R3(P1)→30h→tWB→tR→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | 30 | 70h轮询→00h→C1C2R1R2R3(P0)→00h→C1C2R1R2R3(P1)→30h→tR→06h→C1C2R1R2R3(P0)→E0h→Dout(P0)→06h→C1C2R1R2R3(P1)→E0h→Dout(P1)→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_PROGRAM_PAGE_MULTI_PLANE` | 31 | 70h轮询→80h→C1C2R1R2R3(P0)→Din(P0)→**11h→tADL→80h**→C1C2R1R2R3(P1)→Din(P1)→10h→tWB→tPROG→70h轮询→CHECK_FAIL | PAGE_BUFFER_SIZE |
| `OP_ERASE_BLOCK_MULTI_PLANE` | 32 | 70h轮询→60h→R1R2R3(P0)→60h→R1R2R3(P1)→D0h→tWB→tBERS→70h轮询→CHECK_FAIL | 0 |

> **Multi-Plane Cache 操作**：MT29F4G08ABADAWP 硬件不支持 Multi-Plane Cache Read/Program，无对应 OP 码。

### 17.6 内部搬移 OP 码：片上 Copy-Back

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

### 17.7 关键命令码：11h

`NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH (0x11)` 在 ONFI 标准中定义为 Multi-Plane COPYBACK READ 命令，在多 Plane PROGRAM 时序中起到切换 Plane 的作用：

```
80h → C1C2R1R2R3 → Din(Plane0 全页) →
11h → C1C2R1R2R3 → Din(Plane1 全页) →       ← 11h 切换 Plane
10h → tPROG（双 Plane 同时烧录）
```

### 17.7a 数据缓冲扩展

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

### 17.8 NAND 数据传输模式：中转 vs 直写

NAND 读写操作根据输出/输入方式不同，采用中转或直写两种数据传输模式。

#### 直写模式（Direct Write/Read）

NORMAL_READ_PAGE_WITH_DATA_OUT 利用 NAND 的 `05h-E0h` 随机列读能力实现无中转的 CW 级读取：

```
第1步: 00h-addr-30h → DATA_OUT(528B, CW0) → 直接送入 cw_buffers[task->plane][0]
第2步: 05h-col(528)-E0h → DATA_OUT(528B, CW1) → 直接送入 cw_buffers[task->plane][1]
第3步: 05h-col(1056)-E0h → DATA_OUT(528B, CW2) → 直接送入 cw_buffers[task->plane][2]
第4步: 05h-col(1584)-E0h → DATA_OUT(528B, CW3) → 直接送入 cw_buffers[task->plane][3]
```

每次 DATA_OUT 只有 528 字节（一个 CW），`req_read_page` 将 `data_length` 设为 `PAGE_SIZE / CW_NUM`，poll 中的 chaining 循环自动将后续 CW 的 operation 改为 `OP_RANDOM_DATA_READ`（05h-E0h），逐个 CW 读取到各自 buffer。

NORMAL_PROGRAM_PAGE 虽然也是单页操作，但 NAND 不支持 80h 后逐个 CW 写入再确认——必须一次送完整页数据再发 10h。因此走中转模式。

#### 中转模式（Page Buffer Bounce）

| 操作 | 原因 | 中转数据量 |
|------|------|-----------|
| NORMAL_PROGRAM_PAGE | NAND 编程必须连续输入整页 | 2112 → page_buffer |
| PROGRAM_PAGE_CACHE | 同上 | 2112 → page_buffer |
| PROGRAM_PAGE_MULTI_PLANE | 双 Plane 数据需拼接后两次 DATA_IN | 4224 → page_buffer |
| READ_PAGE_CACHE_SEQ/RANDOM/LAST | 31h/3Fh 一次性输出整页，无中途 05h-E0h 机会 | 2112 → page_buffer → 拆分回 CW |
| READ_PAGE_MULTI_PLANE_WITH_DATA_OUT | 双页连续输出 4224 字节 | 4224 → page_buffer → 拆分回 Plane/CW |

#### 对比

| | 直写模式 | 中转模式 |
|----|---------|---------|
| 拷贝次数 | 0 次额外拷贝 | 1 次 memcpy（读写各 1 次） |
| 适用场景 | 单个 CW 可独立寻址的操作 | 必须连续传输整页/多页的操作 |
| 实现方式 | poll chaining 循环 + RANDOM_DATA_READ | req_xx handler → page_buffer → 拆分 |
| 例 | NORMAL_READ_PAGE_WITH_DATA_OUT | 所有 Program、Cache Read、Multi-Plane |

### 17.9 文件变更

| 文件 | 说明 |
|------|------|
| `include/nand/nand_config.h` | 新增 `NAND_PLANE_MAX`、`CW_TOTAL_MAX`、`PAGE_BUFFER_SIZE` |
| `include/nand/nand_engine.h` | `nand_command_request_t.row` → `row[NAND_PLANE_MAX]` |
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

### 流程图：Stripe RAID 写提交（图 12，raid=ENCODE 的 page write）

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

---

## 18. 多通道支持现状与硬件限制

### 18.1 当前硬件拓扑

```
STM32H743 FMC 总线（单条并行总线）
  └─ MT29F4G08ABADAWP (1 CE × 1 LUN × 2 Plane × 2048 Block × 64 Page)
```

当前物理连接：**1 通道 × 1 片选 × 1 LUN**。

### 18.2 软件架构的多通道准备

调度层和 dispatch 层已按 `[CH_NUM][CE_NUM][LUN_NUM]` 多维数组编写：

```c
// nand_config.h
#define CH_NUM (1U)
#define CE_NUM (1U)
#define LUN_NUM (1U)

// nand_dispatch.c — 各维度数组（v8：per-LUN FIFO 取代通道 FIFO + 链缓冲）
static nand_dispatch_lun_queue_t g_nand_dispatch_queues[CH_NUM][CE_NUM][LUN_NUM];
static uint8_t                   g_nand_channel_page_buffer[CH_NUM][CE_NUM][LUN_NUM][PAGE_BUFFER_SIZE];
```

`nand_dispatch_poll()` 全遍历通道 → CE → LUN，每个 LUN 每轮恰好单步推进一条（不早退，
完成经 sink 逐条交出）：
```c
for (channel = 0; channel < CH_NUM; channel++)
    for (ce = 0; ce < CE_NUM; ce++)
        for (lun_idx = 0; lun_idx < LUN_NUM; lun_idx++) {
            ... out = lun_step(&lun_ctx); if (out == DONE) sink(&completion, ctx); ...
        }
```

FC 命令包含通道字段：
```c
// fc_cmd_conf_t
DWORD ch  : 2;   /* 目标通道号 */
DWORD ce  : 2;   /* 目标片选号 */
DWORD lun : 2;   /* 目标 LUN 号 */
```

这些代码在多通道场景下**逻辑正确**——改 `CH_NUM` 后数组和循环会自动扩展。

### 18.3 当前不支持多通道的模块

以下模块在**多通道维度**的现状：

| 模块 | 现状 | 问题 |
|------|------|------|
| `nand_fmc.c` — FMC 后端 | ✅ 单例 `g_nand_fmc_instance` + `nand_fmc_init()`：单 bank 下窗口/时序对全 LUN 一致，一个后端共享 | 多 bank/多通道需 per-通道 base 表 + 各 bank 单独 `HAL_NAND_Init` + MPU 区；第二片 NAND 另需 CE 引脚 |
| ~~`nand_rb_interrupt.c` — RB 中断~~ | **已移除**：原单 EXTI6（PD6），第二片 NAND 的 RB 引脚没有 EXTI 通道可接 | — |
| `nand_dma.c` — DMA | 单对 `DMA2_Stream0/1` | 两个通道同时传输时无额外 Stream 可用 |
| `nand_engine_state_t` — 引擎状态 | 不含 `ch`/`ce`/`lun` 字段 | step 推进时不知道自己在哪个通道上（依赖 transaction 的 backend 透传） |

### 18.4 硬件层面的本质限制

STM32H743 的 **FMC（Flexible Memory Controller）是共享总线控制器**，不是多通道 NAND 控制器：

| 对比项 | FMC（当前） | 专用 NAND 控制器（企业级/MPU） |
|--------|------------|-----------------------------|
| 总线结构 | **单条并行总线**，CLE/ALE/RE/WE 共用 | **每条通道独立**命令/地址/数据线 |
| 并行度 | ❌ 不能——总线一次只能操作一个设备 | ✅ 各通道独立执行，命令并发 |
| RB 引脚 | GPIO 或 EXTI，**数量有限** | 控制器内置，每通道独立采样 |
| DMA 通道 | 需要手动分配，有限 | 通常内置 per-channel DMA |
| 多片连接方式 | CE 分时复用总线（片选切换） | 独立通道，无总线竞争 |

**FMC 接多片 NAND 只能 CE 分时复用**：

```
FMC 总线 (地址/数据/控制线共用)
  ├── CE0 → NAND #0
  ├── CE1 → NAND #1
  └── CE2 → NAND #2

任何时候只能有一片 NAND 在使用总线。
```

不是真正的"多通道并行"，只是"多片分时复用一条总线"。

### 18.5 如果要支持多通道，需要改什么

| 改动项 | 说明 | 工作量 |
|--------|------|--------|
| `nand_fmc_config_t` 数组化 | `config[CH_NUM]`，每通道独立 FMC 地址 + CE + RB | ⚠️ 原 `g_nand_fmc_instances[CH][CE][LUN]` 已**移除**：同 bank 各 LUN 窗口/时序同值，N 份拷贝零载荷。多通道需 **per-通道 base 表**（非 N 份同值 config） |
| `nand_engine_backend_t` 数组化 | `backend[CH_NUM]`，每通道独立 handler table | ⚠️ 已收敛为**单例共享** `g_nand_fmc_instance`（backend 只读，多 LUN 并发共享安全）；多通道需 per-通道 handler/context |
| ~~`nand_fmc_exec_wait_ready` 多实例~~ | **已不需要**：WAIT_READY 为纯 GPIO 轮询，无中断资源依赖 | — |
| DMA 资源化 | 每通道独立 DMA Stream 或排队传输 | 中 |
| `nand_engine_state_t` 加通道 id | step 中知道自己操作的通道 | 小（现依赖 engine.backend 透传） |
| FMC 总线仲裁 | 多通道共享总线时的冲突避免 | 大（需加锁或排队） |

### 18.6 验证结论

| 层面 | 多通道支持程度 | 能否验证 |
|------|--------------|---------|
| FC 填充 pass（`fc_poll_fill`） | ✅ 代码完整，改 `CH_NUM` 即可（"忙"查 `nand_dispatch_lun_free`） | ❌ 单通道硬件 |
| dispatch per-LUN FIFO | ✅ 代码完整 | ❌ 单通道硬件 |
| dispatch poll 遍历 | ✅ 代码完整 | ❌ 单通道硬件 |
| FMC 后端 | ✅ 单例共享（`g_nand_fmc_instance`，窗口/时序与坐标无关） | ❌ 多 bank 需重构（单 NAND） |
| RB 中断 | 已移除（单 EXTI6 无法扩展到多 LUN） | — |
| DMA | ❌ 单对 Stream | ❌ 无法 |
| 引擎状态机 | ⚠️ 无通道 id，靠 engine.backend 透传 | ⚠️ 单通道可运行 |

**结论**：调度层和 dispatch 层的多通道架构是真实的。FMC/NAND 引擎层需要改造才能真正支持多通道。

### 18.7 多 LUN 并发：OP 码 ISSUE/WAIT/FETCH 拆分设计

#### 18.7.1 问题

当前所有 OP 码是原子序列——从 70h 轮询到命令发出到等待完成到数据输出，一条龙锁住 LUN：

```
OP_NORMAL_READ_PAGE_WITH_DATA_OUT:
  70h轮询 → 00h-addr-30h → tR(WAIT_READY) → DATA_OUT → 70h轮询 → CHECK_FAIL → END
  ├─────────────── LUN 独占整个序列 ────────────────┤
```

多 LUN 场景下，LUN 0 在 tR 期间（25µs），dispatch 只能干等，不能趁隙切到 LUN 1 发命令。

#### 18.7.2 企业做法：三阶段拆分

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

#### 18.7.3 现有雏形

当前架构已有部分拆分迹象：

| OP 码 | 接近的企业原语 | 差距 |
|-------|--------------|------|
| `OP_READ_PAGE_CACHE_START` | READ_ISSUE | 末尾有 70h 轮询，没做到"发完立即返回" |
| `OP_READ_PAGE_WITHOUT_DATA_OUT` | READ_ISSUE | 同上，含 tR + 70h 轮询 |
| `OP_READ_PAGE_CACHE_SEQ` | READ_FETCH | 结合了 WAIT + FETCH，没拆分 |
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | 未拆分 | 整条锁住的原子序列 |

#### 18.7.4 拆分方案

每个读/写/擦操作拆为两个 OP 码：

| 原 READ OP | 拆为 ISSUE | 拆为 WAIT+FETCH |
|-----------|-----------|----------------|
| `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | `OP_READ_PAGE_LOAD` | `OP_READ_PAGE_FETCH` |
| `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | `OP_READ_PAGE_LOAD` | —（已加载，FETCH 由 RANDOM_DATA_READ 完成）|
| `OP_NORMAL_PROGRAM_PAGE` | `OP_PROGRAM_PAGE_ISSUE` | `OP_PROGRAM_PAGE_WAIT` |
| `OP_ERASE_BLOCK` | `OP_ERASE_BLOCK_ISSUE` | `OP_ERASE_BLOCK_WAIT` |

ISSUE 型 OP 的 UOP 序列只包含"发命令+地址+数据+CONFIRM"，**不含 WAIT_READY 和 70h 轮询**，执行完立即返回（µs 级），不阻塞 LUN。

WAIT/FETCH 型 OP 的 UOP 序列只包含 WAIT_READY + 70h 轮询 + DATA_OUT + CHECK_FAIL。

#### 18.7.5 调度器层面影响

| 层面 | 改动 |
|------|------|
| dispatch lun_step | 执行完 ISSUE 后不报完成，切 ISSUED 相位，继续其他 LUN |
| dispatch lun_step | ISSUED 到点后走配对 WAIT/FETCH，完成才出队 |
| fc_poll_fill | 按 LUN room 填充（v8：无 ISSUE/Wait/FETCH 类型识别，靠相位机内部处理） |
| per-LUN FIFO | 同 LUN 的命令严格先进先出，链 = 连续槽，中间不可被其他命令插入 |

#### 18.7.6 当前是否支持并发（→ Phase V 已实现）

| 层面 | 支持程度 | 原因 |
|------|---------|------|
| dispatch 数据结构 | ✅ 就绪 | `g_nand_dispatch_queues[CH][CE][LUN]` 多维（v8 per-LUN FIFO） |
| FMC 硬件总线 | ⚠️ 命令级可切 | 发完命令释放总线，WAIT_READY 卡住（硬件限制） |
| **OP 码层** | ✅ Phase V | 新增 6 个 ISSUE/WAIT/FETCH OP 码 + uop 序列 |
| **调度器层** | ✅ Phase V | poll phase 机（ISSUE→ISSUED→FETCH）+ LUN 状态切换 |
| **FMC 后端** | ⚠️ 单例 | Phase V 曾数组化，Phase Z（22707cb）撤销回单例 `g_nand_fmc_instance`；多通道需 per-bank base 表 |

### 流程图：LUN 相位机（图 10，Phase V 多 LUN 并发）

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
                │ step 完成 → task_complete → 记录 DWT 起点
                ▼
        ┌───────────────┐
        │ LUN_PHASE_ISSUED   │  LUN 内部操作中（tR 25µs / tPROG 200µs /
        └───────┬───────────┘   tBERS ~1ms）。此时 dispatch 可切其他 LUN！
                │ min_busy_us 到点（DWT elapsed）→ pair_op 切换
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

配对 OP 码（`nand_operation_map_t.pair_op`）：LOAD↔FETCH、ISSUE↔WAIT；原子 OP 的 pair_op 取自身（不切分）。多 LUN 场景 `fc_build_dispatch_request()` 在填完 request 后自动把 splittable 命令拆成 ISSUE（`nand_operation_is_splittable`），单命令与链式两条路径共用这一处转换。

### 17.10 设计演进

| 迭代 | 问题 | 方案 |
|------|------|------|
| v1 | 单 plane 数组 `cw_paa[CW_NUM]` 不能支持 multi-plane | 展平为 `cw_paa[CW_TOTAL_MAX]` |
| v2 | 展平后丢失 plane 语义，`cw_count` 意义模糊 | 加 plane 维度 `cw_paa[PLANE_NUM][CW_NUM]` |
| v3 | ROW_0/1/2 handler 固定读 `request->row`，多 plane 发第二组地址需新 uop | `row`→`row[NAND_PLANE_MAX]`，operand 选索引 |
| v4 | 读命令只有一种：30h 后必出数据 | 拆为 WITH/WITHOUT_DATA_OUT 双版本 |
| v5 | 第一维单 plane 恒 `[0]`，名不副实（"槽位"vs"plane 索引"），单 plane 打 plane 1 时依赖无断言守卫的隐式约定 | 第一维统一为物理 plane 索引：新增 `plane` 字段贯穿 request→task，单 plane 命令落位 `cws[plane][*]`；`plane_count` 语义不变 |
| v6 | 读命令只能读"前 N 个 CW"（`config.cw` 是"CW 数减一"），表达不了 `{0,1,3}` 这种子集，读恢复只能重读整页 | 加 CW 掩码：建链按掩码置位逐 CW 建 desc；**链槽位与 CW 号解耦**（CW 号一律 `paa_to_cw(desc->paa)`）；读路径统一为「加载进页缓冲 + 逐 CW 取数」，新增 `OP_RANDOM_DATA_READ_MULTI_PLANE`；完成项暴露失败掩码。详见 §25 |
| v9 | 掩码是 flat 的，但掩码下游仍是"plane 分桶 + 每 plane 密排槽位"的第二套坐标（`cw_count[]` / `cws[plane][slot]` / `slot_to_plane_cw()` 走减），两套坐标必须靠"链序 == plane-major"的隐式不变量对齐，且空 plane 需回填槽位 0 的 PAA 才不误寻址 | **索引空间统一为 flat 绝对 CW 号 `n = plane*CW_NUM + cw`**：`cw_mask` / `cw_buffers[n]` / `cw_status[n]` 同空间，`cw_count[]`、`cws[][]`、`cw_paa[][]`、`slot_to_plane_cw()` 全删；行地址拆到 `plane_paa[PLANE_NUM]`（per-plane，与掩码解耦）。详见 §25.11 |
| v7 | v6 的 `cw_mask : 4` 是"CW 号掩码**广播到每个 plane**"，多 plane 读恢复只能重读失败并集；且 GET_FEATURES 把 NAND 读缓冲指向命令自身、结果原地回填进 SQ | ① 掩码改 **flat 绝对编号**（`fc_param_t.cw_mask`，bit `n = plane*CW_NUM + cw`，覆盖 8 位），两 plane 子集可不同，回填即最小重读集；`cw_count` 标量 → `cw_count[PLANE_NUM]`，链长度一律以 desc 链为真值，`config.cw` 不再回写。② CQ 重做为"命令级聚合状态 + 命令级返回段（`fc_param_t`）"：删 `ecc_bit0..3` / `failed_cw_mask`，GET_FEATURES 值经控制器级 `g_fc_return_staging` 中转进 CQ，SQ 提交后不被写。详见 §25 |

---

## 19. ONFI 时序模式切换

### 19.1 问题背景

`nand_init()` 在 FMC 初始化时配置了保守时序，随后通过 SET_FEATURES(01h) 将 NAND 芯片切换到高速 Mode 4（tRC=20ns），但 **FMC 硬件时序寄存器从未更新**，FMC 始终跑在低速时序，浪费了 Mode 4 的速度提升。

### 19.2 控制宏

```c
// nand_config.h
#define CONFIG_NAND_TIMING_MODE_4 (0U)
```

| 值 | 模式 | 说明 |
|----|------|------|
| 0（默认） | 低速 | NAND 默认 Mode 0（tRC=50ns），FMC 保守时序。用于功能验证初期 |
| 1 | 高速 | NAND 切 Mode 4（tRC=20ns），FMC 重新初始化为匹配快速时序 |

### 19.3 两套时序参数

```c
// nand_mt29f4g08abadawp.c
static const nand_fmc_timing_t g_nand_timing_slow = { 70U, 5U, 10U, ... };  // Mode 0
static const nand_fmc_timing_t g_nand_timing_fast = { 20U, 5U,  5U, ... };  // Mode 4
```

### 19.4 ONFI → FMC 换算

```c
static void nand_fmc_timing_to_pcc(const nand_fmc_timing_t *onfi,
                                    FMC_NAND_PCC_TimingTypeDef *pcc);
```

| FMC 寄存器 | 含义 | 近似式（`nand_fmc_timing_to_pcc()` 实现，按 5ns 向上取整） |
|-----------|------|------|
| `SetupTime` | 命令/地址建立 | max(tCLS, tALS, tCS) / 5ns |
| `WaitSetupTime` | RE/WE 低电平脉宽 | max(tRP, tWP) / 5ns |
| `HoldSetupTime` | 控制/数据保持 | max(tCH, tALH, tCLH, tDH) / 5ns |
| `HiZSetupTime` | **写方向**总线高阻窗口 | [max(tCS, tCLS, tALS) + (tWP − tDS)] / 5ns |

FMC kernel clock = 220 MHz，tHCLK ≈ 4.55ns，取整为 5ns 计算。

> ⚠ **本表是近似式**。ST 官方知识库给的精确式多 `±1` 与 `−tWP` 项：
> `(SET+1)·tHCLK ≥ max(tCS,tCLS,tALS,tCLR,tAR) − tWP`、`(WAIT+1)·tHCLK ≥ max(tWP,tRP)`、
> `HOLD·tHCLK ≥ max(tCH,tCLH,tALH)`、`HiZ·tHCLK ≥ max(tCS,tCLS,tALS) + (tWP − tDS)`。
> 近似式在前三项上更保守（无害）。
>
> ⚠ **`HiZSetupTime` 早期误用读方向的 `tRHZ`，2026-10-05 更正为写方向公式**：`MEMHIZ` 是**写访问**期间"地址有效 → 数据被驱动"的总线高阻时长，与读方向的 `RE#↑ → 输出高阻`（`tRHZ`）是**两个方向相反、互不相关的量**。据此 slow 表算得 HiZ 下限 = ceil(18/4.545) = 4，`nand_init` 初值已由 3 上调为 4。

### 19.5 nand_init() 双路径

低速模式：
```
HAL_NAND_Init(保守) → RESET/READ_ID/校验 → 跳过 Mode 4 → 完成
  NAND: Mode 0（tRC=50ns）
  FMC:  保守时序
```

高速模式：
```
HAL_NAND_Init(保守) → RESET/READ_ID/校验 → SET_FEATURES(Mode 4)
  → nand_fmc_timing_to_pcc(fast) → HAL_NAND_Init(快速) → nand_fmc_init(fast) → 完成
  NAND: Mode 4（tRC=20ns）
  FMC:  快速时序
```

### 19.6 已知限制

`nand_fmc_timing_to_pcc()` 当前用 `/5ns` 简单除法向上取整，未考虑 FMC 建立/保持时间的余量。如需更精确的时序适配，需引入 FMC 时序模型根据 HCLK 频率动态计算。

---

## 20. 读饥饿保护（Read Starvation Protection / QoS）

> 🗑 **v8 起本章机制已删除（2026-10-06）**。`FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET`、
> `fc_scheduler_op_is_read()`、`consecutive_write_count` 与相关旋转逻辑随 per-LUN FIFO 重构一并移除：
> 填充 pass（`fc_poll_fill`）改为「HSQ 优先 + 按 LUN 保序」，不再做读写计数与读优先旋转。
> 在 `CH_NUM=CE_NUM=LUN_NUM=1` 的当前平台上该保护本就近乎无效（见 20.x 备注）。
> 本章保留为设计记录，供未来多 LUN/多通道 QoS 重启时参考。详见 `docs/changelog/fc_nand_per_lun_fifo.md`。

### 20.1 问题

调度器（`fc_scheduler`）对读写命令一视同仁。写命令（PROGRAM, tPROG≈300μs）或擦除命令（ERASE, tBERS≈1ms）连续占用 LUN 时，后续的读命令可能被长时间阻塞，导致**读饥饿**。

### 20.2 控制宏

```c
// fc_scheduler.h
#define FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET (3U)
```

连续执行 N 条非读命令后，触发读优先调度。执行一条读命令后计数器清零。

### 20.3 实现

#### 读操作识别

`fc_scheduler_op_is_read()` 识别 16 种读相关操作，非读操作（PROGRAM/ERASE/SET_FEATURES 等）受饥饿保护约束。

#### select_chain() SQ 遍历逻辑

```
① 前置扫描：count ≥ 3 时扫描 SQ 是否存在读命令（sq_has_read）
② 旋转拦截：count ≥ 3 且队头为非读且 sq_has_read=1 → 旋转到队尾
③ 执行：弹出链头命令，更新 count（读→0，非读→++）
```

#### 死锁防护

`sq_has_read` 前置扫描确保**全写队列**场景下不触发保护。如果 SQ 内完全没有读命令，写命令正常执行，不会被无限旋转。

### 20.4 队列操作说明

SQ 是环形 FIFO，旋转通过 `pop(队头) + push(队尾)` 实现。只动 `head/tail` 指针，Command Pool 内容不变。旋转不是丢弃——被旋转的命令会在队列循环一圈后回到队头执行。

### 20.5 场景验证

| 场景 | 行为 |
|------|------|
| 全写队列 [W,W,W,W] | sq_has_read=0 → 不触发 → 全部正常执行 |
| 读写混合，写 ≤ 3 | count < 3 → 不触发 |
| 写 ≥ 3 + 队列有读 | sq_has_read=1 → 后续写旋转 → 读优先执行 → count 归零 |

### 20.6 当前局限性

- 仅在 SQ 层面生效。HSQ 为高优先级通道，跳过读饥饿保护。
- 命令链（chain=1）中无法插队读命令（链原子性），但链结束后下一轮 select 即可切换。
- `CH_NUM=1`、`LUN_NUM=1` 时读饥饿问题不明显，本设计主要为多 LUN/多通道架构预留。
- `CH_NUM=1`、`LUN_NUM=1` 时读饥饿问题不明显，本设计主要为多 LUN/多通道架构预留。

---

## 21. 存储主控行业对比与硬件限制（退而求其次的选择）

### 21.1 引言：为什么需要"退而求其次"

本项目目标平台是 **STM32H743 MCU + 1× MT29F4G08ABADAWP（单通道 SLC NAND）**。这与真正的 SSD 主控（企业级 Samsung/Intel、消费级 Phison/SMI、手机 UFS 控制器）之间存在**本质的硬件代差**——MCU 是通用处理器，不是专用 NAND 控制器。因此本项目的很多设计是对行业方案的"软件模拟 + 妥协"。

本章先给总览对比，再逐项说明"差异 → 硬件限制 → 我们的选择（无奈退而求其次）"。

### 21.2 总体对比总览

| 维度 | 企业级 NVMe SSD | 消费级 NVMe/SATA | UFS 主控 | 本项目（STM32H743） |
|------|---------------|-----------------|---------|-------------------|
| 控制器 | 专用 ASIC + 多核 CPU | 专用 ASIC + CPU | 专用 ASIC + CPU | **通用 MCU（Cortex-M7 480MHz）** |
| DRAM | 数 GB DDR | 256MB~1GB（或 HMB） | 数百 KB~MB SRAM | **0（DRAM-less，仅 1MB SRAM）** |
| NAND 通道 | 8~32 通道 | 4~8 通道 | 1~4 通道 | **1 通道 × 1 CE × 1 LUN** |
| ECC | 硬件 LDPC 引擎（数 Gbps） | 硬件 LDPC/BCH 引擎 | 硬件 LDPC | **软件 BCH(m=13,t=4)/LDPC（CPU 逐 CW）** |
| 数据搬移 | 硬件 DMA 多通道 | 硬件 DMA | 硬件 | **单对 DMA2 Stream（PIO 兜底）** |
| RAID | 硬件 XOR 引擎 | 部分硬件 | 部分 | **软件 XOR（跨命令/跨 Plane）** |
| 加扰 | 硬件 inline（DMA 内） | 硬件 inline | 硬件 | **软件 LFSR/XOR-shift** |
| 调度 | 硬件命令队列 + 固件调度器 | 硬件队列 + 固件 | 硬件 | **纯软件轮询调度器（单线程）** |
| 中断 | 命令完成中断 + 多队列 | 同上 | 同上 | **纯主循环轮询（R/B# EXTI 已移除）** |
| 功耗/性能 | 数 W / 数 GBps | 数 W / GBps | 数百 mW | **~百 mA / 单通道 SLC** |

### 21.3 逐维度差异与"退而求其次"的选择

#### 21.3.1 无 DRAM → L2P 全表落 NAND + SRAM Cache

- **行业**：企业/消费级用 DRAM 放完整 L2P 表（4GB SSD ≈ 8MB 表），UFS 用 SRAM cache 部分映射。
- **硬件限制**：本项目 **0 DRAM**（无外部 SDRAM 接 FMC 的可行方案），SRAM 仅 1MB 且被 FTL 元数据/BST 占据 ~220KB。
- **无奈选择**：DRAM-less 方案——L2P 全表存 NAND，SRAM 用 Region Cache（64 条 × 2KB ≈ 128KB），CLOCK 替换。这就是 UFS/消费级入门 SSD（SN580 HMB）同路线。
- **代价**：每次 L2P miss 要读 NAND 一个 region（2KB），随机读延迟增加；全表常驻做不到。

#### 21.3.2 单通道 → 无法并行，只能"分时复用 + 软件并发"

- **行业**：企业级 8~32 通道并行，命令级并发是常态。
- **硬件限制**：STM32H743 的 **FMC 是共享总线控制器**，不是多通道 NAND 控制器。接多片 NAND 只能 CE 分时复用一条总线，任何时候只有一片在传数据（见 §18）。
- **无奈选择**：`CH_NUM=1` 单通道；用 **Phase V ISSUE/WAIT/FETCH 拆分**在软件层模拟"并发"——发完命令（ISSUE）释放总线，等 tR/tPROG 时（WAIT）切其他 LUN。这是对多 LUN 并发的软件近似，**总线本身无法同时传两片 NAND 数据**。
- **代价**：多 LUN 只是"重叠等待时间"，不是真正并行搬移数据；单通道吞吐上限 ~50MB/s（SLC Mode 4）。

#### 21.3.3 无硬件 ECC → 软件 BCH/LDPC

- **行业**：企业级硬件 LDPC 引擎纠错数 Gbps；消费级硬件 BCH/LDPC。
- **硬件限制**：STM32H743 无 NAND ECC 外设；CPU 480MHz 跑软件 ECC。
- **无奈选择**：**软件 BCH(m=13, t=4)**（Linux lib/bch.c 移植，8B parity），可选软件 QC-LDPC 硬判决（码率 0.985，t≈1~2）。
- **代价**：
  - 吞吐低：BCH 编码/解码是 CPU 密集操作，无法达到硬件引擎速率。
  - 纠错能力弱：t=4（SLC 够用，TLC/QLC 远不够）；LDPC 硬判决在高码率下纠错有限，软判决未实现（stub）。
  - **每次读/写都要占用 CPU**，与数据搬移竞争 M7 周期。

**⚠ 一个比「t 不够大」更本质的问题：ECC 的两种失败模式**

上面「代价」说的是**量**（t=4 纠不动 TLC）。但 ECC 还有一个**质**的问题：**它报「成功」时，数据不一定对**。

ECC 超能力时有**两种**表现，只有第一种是安全的：

| 失败模式 | 表现 | 上层能否发现 |
|---|---|---|
| ① **不可纠** | 解码器报失败（`FC_CW_STATUS_UNC`） | ✅ 可检测——上层能重读 / 换页 / RAID 恢复 |
| ② **误纠**（miscorrection） | 解码器**报成功**，但输出的是**另一个合法码字** | ❌ **不可检测**——静默错误 |

迭代解码的三种收尾，只有中间那种是危险的：

| 收尾 | 校验子 `H·c` | 报什么 | 可检测？ |
|---|---|---|---|
| 收敛到**正确**码字 | 0 | 成功 | — |
| 收敛到**错误**码字 | **0** | **成功** | ❌ **误纠** |
| 不收敛 / 停在非码字 | ≠0 | 失败 | ✅ |

**LDPC 的 error floor 就是②**。迭代消息传递（BP / min-sum）在小的 **trapping set / absorbing set**（也叫 near-codeword）里会卡死：集合内部的校验节点连着**偶数个**错误 bit，校验和正好为 0，于是**满足**——文献里管这叫 "**deceptively satisfied**" checks；剩下少数不满足的校验节点**无法定位**错误分布，校验子权重不再下降，额外迭代提取不到新信息，解码器停在不动点，输出一个**校验子为零的错误码字**。这是 sub-optimal 迭代解码的**结构性**后果（码的 Tanner 图里存在小 trapping set），不是实现 bug。

**BCH 也有同样的病，只是概率模型不同**：BCH 是**有界距离解码**，收到字落在某码字的距离 t 球内就纠过去；当错误数 ≥ t+1 **且**收到字落进**另一个**码字的 t 球内 → 误纠。

一个反直觉的点：**LDPC 因为没有干净的距离保证，报「失败」的频率其实比 BCH 高；但一旦它报成功，仍然可能是错的**——「ECC 报成功」这个信号**本身不可信**。

**推论：CRC 不是可选项。** 独立校验把②变成「可检测的失败」，行业指标 UBER（uncorrectable bit error rate，**纠错之后**的错误率）就是靠这个乘积达到的：

```
UE ≈ P(误纠) × P(CRC 漏检) ≈ P(误纠) × 2⁻³²
```

单靠 ECC 打不到 1e-15 / 1e-16 的 UBER 目标，**必须有 CRC 这一道**。本工程的读路径 Pass 3 就是它（见 §5.2）；「CRC 只覆盖 USER、不覆盖 META」是刻意的覆盖边界（见 §5.3）。

> 出处：Koopman, *32-Bit Cyclic Redundancy Codes for Internet Applications*, DSN 2002；
> *Error Characterization, Mitigation, and Recovery in Flash-Memory-Based SSDs*（arXiv:1706.08642）；
> USENIX FAST'13 *LDPC-in-SSD: Making Advanced Error Correction Codes Work Effectively in Solid State Drives*。

#### 21.3.4 无硬件数据搬移 → 单对 DMA + PIO 兜底

- **行业**：每通道独立硬件 DMA，DMA 引擎内完成数据搬移 + ECC inline。
- **硬件限制**：STM32H743 DMA2 仅 Stream0/1 被 FMC 数据窗口占用（一对），多通道/多方向同时搬移不可行。
- **无奈选择**：单对 DMA（memory↔FMC）非阻塞中断模式，DMA 启动失败/禁用时**回退 PIO 逐字节**（见 §3.2）。
- **代价**：单方向同时只有一组 DMA；PIO 路径占用 CPU 逐字节写 FMC 窗口（慢但可用）。无硬件 ECC inline，需 CPU 先 ECC 再 DMA（或反序）。

#### 21.3.5 无硬件加扰 → 软件 LFSR

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
**META 与 CRC 保持明文**，读路径 Pass 2 直接读明文 META 自举种子。

> 加扰粒度 = **被寻址逻辑单元**。本工程一个 CW（512 B 用户数据）= 一个扇区 = 一个逻辑单元，故每条 CW 用
> 自己那条扇区 LBA 当种子；这正是业界"种子 = 被寻址逻辑单元的地址"（一个逻辑块由若干 512 B–2 kB 的
> ECC 码字组成，块内码字共用该逻辑块的种子）。详见 `docs/changelog/fc_scrambler_lba_seed_v24.md`。

**副作用（v38 记录）**：明文 META + 种子耦合还带来一个**隐式**的保护——**META 损坏会传染成 CRC 失败**
（种子错 ⇒ USER 解成垃圾 ⇒ Pass 3 不过）。这是「期望值未知」路径（GC / 掉电重建）上 META 的**唯一动态保护**，
但它没写进任何契约，且依赖 `random` 开启。详见 §5.3 的「四条读取路径 META 保护矩阵」。

#### 21.3.6 无硬件 XOR → 软件 RAID

- **行业**：企业级硬件 XOR 引擎做 RAID 编解码（每秒数十 GB）。
- **硬件限制**：无。
- **无奈选择**：软件 XOR（跨命令/跨 Plane），32bit 字对齐异或。
- **代价**：RAID 恢复是 CPU 密集；`CONFIG_FC_RAID=0` 默认关闭（代码完成未验证，等硬件闭环后开启）。

#### 21.3.7 无硬件命令队列 → 软件轮询调度

- **行业**：硬件多队列（SQ/CQ）+ 固件调度器，命令完成中断驱动。
- **硬件限制**：MCU 无专用 NAND 命令队列。
- **无奈选择**：软件 SQ/HSQ/CQ 环形队列 + 单线程 `fc_service()` 轮询（无中断参与命令推进；R/B# EXTI 方案曾实现，因单 EXTI6 无法扩展到多 LUN 已移除，WAIT_READY 改回纯 GPIO 轮询）。
- **代价**：单线程轮询 CPU 占用高；读饥饿保护（§20）是软件 QoS 的近似；无多队列优先级硬件支持（HSQ 为软件近似）。

#### 21.3.8 无硬件页缓冲 → 软件 page_buffer 拼接

- **行业**：控制器内部 SRAM 页缓冲 + 硬件页拼接（multi-plane 数据自动拼接）。
- **硬件限制**：FMC 是并行总线，无 NAND 控制器页缓冲硬件。
- **无奈选择**：软件 `g_nand_channel_page_buffer`（noncacheable 段）+ req_* handler 手动拼接/拆页（见 §17.7a）。
- **代价**：多 plane 数据要 CPU memcpy 拼接/拆回；占用 SRAM（4224B/(通道×CE×LUN)）。

### 21.4 汇总：哪些是"不可逾越"的硬件限制

| 限制 | 本质原因 | 是否可软件弥补 |
|------|---------|--------------|
| 单通道总线 | FMC 共享总线 | ❌ 物理不可并行（只能分时） |
| 无 DRAM | 无外部内存接口可行方案 | ⚠️ 用 NAND+SRAM cache 近似 |
| 无硬件 ECC | MCU 无 ECC 外设 | ⚠️ 软件 BCH/LDPC（慢+弱） |
| 单对 DMA | DMA2 Stream 有限 | ⚠️ PIO 兜底（慢） |
| 无硬件加扰/XOR | MCU 无 | ⚠️ 软件实现（占 CPU） |
| 无硬件队列 | MCU 无 | ⚠️ 软件队列 + 轮询（占 CPU） |

### 21.5 结论

本项目的 FC-NAND 架构在**软件层面完整对齐了企业/消费级主控的分层模型**（命令池/描述符/缓冲池、调度器、数据路径、ECC/RAID/加扰引擎、ISSUE/WAIT/FETCH 并发原语），这是与行业一致的"形"。

但受 STM32H743 MCU 硬件限制，所有**需要硬件引擎加速的部分**（ECC、DMA 多通道、加扰、XOR、页缓冲拼接、命令队列）都只能退化为**软件实现 + CPU 周期换功能**，单通道总线更是物理上限。因此本项目定位是：

> **"用 MCU 软件模拟企业级 SSD 主控架构的教学/研究实现"**——验证 FTL/FC/NAND 分层、命令并发、数据路径、ECC/RAID 的完整软件闭环；性能与真正的 SSD 主控不在一个数量级，但在架构正确性和功能完整性上是自洽的。

这正是"退而求其次"的必然：**形似而神不似，功能完整而性能有限**。

---

## 22. 初始化全流程：从复位到 FTL 就绪

本章回答一个此前文档缺位的问题：**上电后到底按什么顺序把 FC / NAND / FTL 立起来？** 前面的图（图 1/2/9/10/11）都是运行时单点视角，没有一条"从 `main()` 冷启动到能发第一条命令"的主线。本章给出这条主线（图 14–16），并在 §23 接上运行时主线（图 17–22）。

> 说明：本章正文以**函数名**为主定位，行号仅为辅助且会随重构漂移，请以函数名为准；grep 锚点见 [附录 A](#附录-a图--源码-grep-对照)。

### 22.1 启动顺序与三条硬约束

`main()`（`src/main.c`）的初始化是一条**严格 if/else-if 级联**，不是循环：

```
sys_init → log 绑定 → fc_init → nand_init
        → nand_dispatch_set_armed(1) → stm32_platform_register
        → ssd_ftl_boot_all → while(1) { ... }
```

| # | 约束 | 原因 |
|---|------|------|
| ① | `fc_init()` 必须先于 `nand_init()` | `fc_init_internal()` 里**唯一的 NAND 依赖**是 `nand_dispatch_init()`，它只是 memset 软件调度状态、**不碰 FMC 硬件**（见 `nand_dispatch.h`） |
| ② | `else if` 级联：`fc_init` 失败 → `nand_init` 整段跳过 | 没有 FC 控制面，NAND 裸路径也无意义；后续 `platform_register` / `ssd_ftl_boot_all` 同样被跳过，主循环退化为裸 `fc_service()` |
| ③ | `nand_dispatch_set_armed(1)` 必须在 `nand_init()` 之后 | `nand_dispatch_submit()`/`submit_chain()` 经 `nand_dispatch_enqueue()` 用 `g_nand_dispatch_armed` 门控；而 `nand_init` 内部的 RESET/READ_ID 走 `nand_fc_execute` **直连 engine**、绕过 fc/dispatch，所以初始化期间该门控必须为 0 |

两个门控位的语义不要混淆：

- **`g_nand_dispatch_armed`**（`nand_dispatch.c`）——只在 `nand_dispatch_enqueue()` 一处门控（`submit`/`submit_chain` 共用），`nand_dispatch_poll()` 不检查。
- **`g_ftl_ready`**（`main.c`）——主循环守卫，决定走 `ftl_service()` 还是裸 `fc_service()`，由 `ssd_ftl_boot_all()` 末尾置位。

### 22.2 流程图：上电初始化分层调用时序（图 14）

```text
main() (src/main.c)          fc_init()                  nand_init()             ssd_ftl_boot_all()
  │
  ├─ sys_init()
  ├─ log_set_sink / set_flush_sink / set_level
  │
  ├─ fc_init() ──────────────▶ fc_interface.c（薄壳）→ fc_init_internal(fc.c)
  │                              ├ ① 三池 + 队列存储 + 命令状态位图 memset 归零
  │                              ├ ② *controller = {0} 后挂各静态数组指针
  │                              ├ ③ SQ(64)/HSQ(16)/CQ(64) init
  │                              ├ ④ fc_scheduler_init(sq,hsq) + 清 busy/饥饿计数
  │                              ├ ⑤ nand_dispatch_init()  ★本函数唯一 NAND 依赖（纯 memset）
  │                              ├ ⑥ fc_crc_init()
  │                              └ ⑦ 串三条空闲链表
  │
  ├─ else if nand_init() ─────────────────────────────▶ nand_init(mt29*.c)
  │                              Step1 FMC 句柄 + PCC 时序（Mode 0 保守值）
  │                              Step2 HAL_NAND_Init → MspInit（时钟 / GPIO / NCE）
  │                              Step3 nand_fmc_init（单 bank 后端）
  │                              Step4 nand_dma_init（本配置 no-op）
  │                              Step5 nand_fc_reset      ★绕过 dispatch，直连 engine
  │                              Step6 nand_fc_read_id + ID 校验
  │                              Step7 Mode 4（CONFIG_NAND_TIMING_MODE_4=0 → 整段编掉）
  │
  ├─ else nand_dispatch_set_armed(1)        ★放行 submit/submit_chain
  ├─ stm32_platform_register()               （注册 time_us，FTL 的 T3 定时器依赖）
  └─ ssd_ftl_boot_all() ───────────────────────────────────────────────────▶ (main.c)
                                ① 逐实例 ssd_ftl_boot → ftl_init / boot_recovery
                                ② 建 g_ftl_ptrs 三维指针表
                                ③ completion_table_set_route(g_ftl_ptrs)
                                ④ ssd_controller_init(&g_ctrl, g_ftl_ptrs)
                                ⑤ g_ftl_ready = 1   ★主循环切换点

  ★ if/else-if 级联：fc_init 失败 → 直接跳过 nand_init / 门控 / platform_register / FTL boot
```

### 22.3 流程图：FC 控制面资源装配（图 15）

```text
[链接期静态存储]                          [fc_init_internal 装配]                     [运行时使用者]
g_fc_command_pool[]      ─┐
g_fc_descriptor_pool[]   ─┼─▶ controller->command_pool / descriptor_pool    ← fc_command_alloc / release
g_fc_buffer_pool[]       ─┘   controller->buffer_pool
g_fc_*_free_next[]       ───▶ 三张「下标即 ID」静态链：0→1→…→N→INVALID，head=0
                              分配 = 取 head 并把 head 推到 next；释放 = 头插
g_fc_sq_storage[64]      ───▶ controller->sq    ◀─ fc_submit_to_queue（HSQ 优先门控）
g_fc_hsq_storage[16]     ───▶ controller->hsq   ◀─ config.high_priority 命令
g_fc_cq_storage[64]      ───▶ controller->cq    ◀─ fc_complete_internal → FTL completion_table_drain
```

要点：三池与队列存储都是**文件作用域静态数组**，地址在链接期固定；`fc_t` 只存指向它们的**指针**。因此三池的 DMA 物理地址在 `fc_init` 之后恒定不变——这是"先 `memset` 归零、后挂指针"这一顺序的原因。

### 22.4 流程图：NAND 芯片初始化（图 16）

```text
nand_init()
  │
  ├─ Step1  填 g_nand_handle.Init（Bank3 / 8 位总线 / ECC 关）+ PCC 时序（Mode 0 保守值）
  ├─ Step2  HAL_NAND_Init ──▶ HAL_NAND_MspInit（★首次碰硬件）
  │            ├ 使能 FMC / GPIOD / GPIOE / GPIOG 时钟
  │            ├ PD6 = R/B#（上拉输入）
  │            ├ PG9 = FMC NCE（AF12）
  │            └ PD8/11/12/14/15 + PE7/8/9/10 = D0~D7（AF12）
  ├─ Step3  nand_fmc_init（单 bank：全 LUN 共享窗口/时序）
  │            └ 绑 FMC 后端句柄（data/cmd/addr 窗口 = FMC_BASE | 0 / A16 / A17）+ handler 表
  ├─ Step4  nand_dma_init（CONFIG_NAND_DMA=0 → no-op；失败仅回退 PIO，非致命）
  ├─ Step5  delay_ms(1) → nand_fc_reset(FFh)      ★经 nand_fc_execute 直连 engine，绕过 dispatch
  ├─ Step6  nand_fc_read_id + 拼 id[1..4] 校验 MT29F4G08ABADAWP_ID（跳过厂商字节 id[0]=0x2C）
  └─ Step7  [CONFIG_NAND_TIMING_MODE_4] SET_FEATURES(01h,4) 切 Mode 4 + 重算 PCC + 重配 FMC
             （本固件 =0，整段编掉，NAND / FMC 停在默认慢速时序）
```

### 22.5 上电走读：一次冷启动

| 跳 | 位置（函数为主） | 这一步之后…… |
|----|------------------|----------------|
| 1 | `main` → `sys_init` / `log_set_*` | 时钟、UART 日志通道可用，但 FC/NAND 尚不存在 |
| 2 | `fc_init` → `fc_init_internal` ①②③ | 三池、三条环形队列就绪；命令可以 alloc/configure 但还提交不了 |
| 3 | `fc_init_internal` ④⑤⑥⑦ | 调度器、CRC、空闲链表就绪；`nand_dispatch_init` 已清空软件调度状态（NAND 硬件仍未初始化） |
| 4 | `nand_init` Step1–4 | FMC 引脚/时钟、per-LUN 后端、DMA 就绪；此时 `g_nand_dispatch_armed` 仍为 0 |
| 5 | `nand_init` Step5–6 | NAND 已复位、ID 已核对，芯片可用；但启动命令是绕过队列直接跑的 |
| 6 | `nand_dispatch_set_armed(1)` | `nand_dispatch_submit`/`submit_chain` 放行——从现在起 FC 才能向 NAND 派发命令 |
| 7 | `stm32_platform_register` | FTL 的 `time_us` 就绪，T3 定时触发（CP / GC）才有依据 |
| 8 | `ssd_ftl_boot_all` ①–④ | 每个 (ch,ce,lun) 完成 `ftl_init` + `boot_recovery`；controller 绑定 FTL 实例 |
| 9 | `ssd_ftl_boot_all` ⑤ `g_ftl_ready=1` | 主循环切换到 `ftl_service()`，系统进入可服务状态 |

### 22.6 编译期开关（图中哪些分支被编掉）

| 开关 | 当前值 | 影响 |
|------|-------|------|
| `CONFIG_FC_RAID` | 0 | 图 15 的 stripe 积累/RAID 编解码不参与；`fc_stripe_submit` 分支编掉（等硬件闭环后开启） |
| `CONFIG_NAND_TIMING_MODE_4` | 0 | 图 14/16 的 Step7 整段编掉，NAND/FMC 停在默认慢速时序 |
| `CONFIG_NAND_DMA` | 0 | Step4 退化为 no-op，数据搬运走 PIO（CPU 逐字节） |

---

## 23. 运行时全流程：从 FTL 提交到完成回收

§22 讲"怎么起来"，本章讲"怎么跑"。核心结论先给出：**运行时是一条裸机 superloop 驱动的同步轮询链，没有中断参与命令推进，且 FTL 从不使用命令链。**

### 23.1 运行时驱动模型

- 驱动源只有一个：`main()` 的 `while(1)`。**没有任何 ISR 调用 `fc_service()` / `ftl_service()`**；WAIT_READY 为纯 GPIO 轮询，无中断参与（原 R/B# EXTI 方案已移除）。
- 两个阻塞调用方：
  - `ftl_service()`（`ftl.c`）——每轮先跑 `fc_service()` **≤ `NAND_SUBMIT_BUDGET`（=4）次**推进 NAND，再 `completion_table_drain()` 消费完成项并回调 FTL。
  - `ftl_internal_sync_wait()`（`ftl_fc_interface.c`）——FTL 内部同步命令（如 Read Retry、元数据读）用它阻塞轮询 `fc_service()` 直到指定命令完成。

### 23.2 流程图：一条命令的端到端生命周期（图 17）

```text
[FTL]  构造 cmd（OP_*，config.chain = FC_CHAIN_END）
  │    fc_command_alloc → ftl_cmd_configure_descriptors → ftl_cmd_write_data → fc_submit
  ▼
[FC]   fc_submit_internal → fc_submit_to_queue（HSQ 优先）入 SQ/HSQ；状态 QUEUED
  │
  ▼   fc_service() → fc_poll_internal
  ├─ ① nand_dispatch_poll（先推进 NAND，见 图 9/图 20）
  └─ ② 无完成 → fc_poll_fill（图 22）→ 取到命令（need==1）
        └─ fc_poll_dispatch_single：置 ACTIVE
             ├─ page write → fc_data_path_encode（Stage 0 写 META，图 4）
             ├─ fc_build_dispatch_request → request.cw_bitmap / cw_buffers[n] / plane_paa[]
             └─ nand_dispatch_submit（门控 + enqueue 追加槽 + task_load 绑 backend/buffer）
  ▼
[dispatch] nand_dispatch_poll → lun_step → nand_dispatch_task_start → nand_fc_start（READ_PAGE_LOAD，ISSUE 型）
  │         → phase = ISSUE → 异步返回 WAIT
  ▼
[engine]   nand_engine_step（图 6）：00h / addr / 30h / CONFIRM → END
  ▼
[dispatch] lun_step → 转 ISSUED → issued_advance 等 tR
  │         pair_op → OP_READ_PAGE_FETCH → nand_fc_start
  ▼
[engine]   WAIT_READY + 70h 轮询（图 7）+ DATA_OUT（PIO/DMA）+ CHECK_FAIL
  │         scatter_page_buffer：把页缓冲拆回各 CW buffer
  ▼
[FC]       nand_dispatch_poll 返回 completion → fc_complete_from_nand（图 13）
  │         写 cw_status/ecc_bits 回 desc；fc_data_path_process_read（解扰 / ECC / RAID，图 5）
  │         → fc_complete_internal → CQ push；状态 COMPLETED
  ▼
[FTL]      completion_table_drain → fc_poll_completion → FTL 回调拷贝数据
           → fc_command_release（回收 command + desc 链 + buffer）
```

### 23.3 流程图：命令生命周期状态机（图 18）

补齐图 1 只给出一行状态链的不足，标出每个转移的**触发函数**与**失败旁路**：

```text
                        fc_command_alloc_internal
     ┌────────────────────────────────────────────────────────┐
     ▼                                                        │
  [FREE] ────────────────────────────────────────────────▶ [ALLOCATED]
     ▲                        fc_command_configure_descriptor
     │                                                        │
     │              fc_submit_to_queue（HSQ 优先）/ fc_submit  │
     │                                                        ▼
     │                                                    [QUEUED]
     │                                                        │ 出队即置
     │        fc_poll_dispatch_single / _chain（图 19）        ▼
     │                                                    [ACTIVE]
     │                                                        │ nand 完成
     │        fc_complete_from_nand → fc_complete_internal（CQ push）
     │                                                        ▼
     │                                                  [COMPLETED]
     │        FTL drain 后 fc_command_release_internal        │
     └────────────────────────────────────────────────────────┘

  失败旁路：fc_poll_fail_command → 合成 NAND_FAIL 完成项（QUEUED → COMPLETED）
  CQ 满旁路：fc_complete_internal 强制置 COMPLETED + IO_ERROR
  状态存储：g_fc_command_state[]（include/fc/fc_internal.h）
```

### 23.4 流程图：`fc_service()` 单步全景（图 19，扩展图 2）

图 2 只画了单次 `fc_service` 内部。本图补上**谁调它**、**失败兜底**、**寄存器刷新点**：

```text
调用方（三选一）：
  main.c while(1)  ── g_ftl_ready==0 时裸调 fc_service()
  ftl_service()    ── 每轮 fc_service() × ≤4（NAND_SUBMIT_BUDGET）
  ftl_internal_sync_wait() ── 阻塞轮询直到目标命令完成
        │
        ▼
fc_service() → fc_poll_internal
  ├─ ① nand_dispatch_poll
  │     ├─ 有完成 → fc_complete_from_nand → [RAID] fc_stripe_release_parity → 返回
  │     └─ 无完成 → ②
  └─ ② fc_poll_fill（图 22）
        ├─ 两队列皆空 → QUEUE_EMPTY；无 room → NOT_READY（稍后重试）
        ├─ 单命令 → fc_poll_dispatch_single（运行时唯一活路径）
        └─ 链 → fc_poll_dispatch_chain（仅单测覆盖，见 §23.9）
              └─ 失败兜底统一走 fc_poll_fail_command
```

### 23.5 流程图：`nand_dispatch` 相位机状态机（图 20，扩展图 10）

图 9 是 per-LUN FIFO 的**横向**遍历视图，图 10 是 LUN 相位机的初版。本图给出**每个相位的进入/退出条件、写点/读点、由谁置位**（v8：相位挂在 FIFO 队头槽上）：

```text
[IDLE]  phase == IDLE
  │ submit / submit_chain 追加槽；lun_step 完成一条命令后复位为 IDLE
  ▼
[ISSUE]  （仅 ISSUE 型 OP）
  │ task_fc_start 判 nand_operation_is_issue → 置 ISSUE
  │ lun_drive 命中 ISSUE 分支：
  │   task_complete + phase=ISSUED + delay_dwt_cycles_now() + return WAIT
  │   ⚠ 不报完成：LUN 还在 tR / tPROG / tBERS
  ▼
[ISSUED] wait_start_cycles = DWT 起点
  │ lun_step 先调 issued_advance 判 elapsed ≥ min_busy_us（tR=38µs / tPROG=700µs / tBERS=5000µs）
  │ pair_op 切换：LOAD↔FETCH / ISSUE↔WAIT / 原子 OP pair==自身则跳过
  ▼
[FETCH]  step 到 END → task_complete
  │ 本条命令结束 → *completion 返回 + head++/count-- + phase=IDLE
  ▼
[IDLE]

非 ISSUE 型 OP：task_fc_start 直接置 FETCH（跳过 ISSUE / ISSUED）
相位写点/读点总表：src/nand/nand_dispatch_internal.h（nand_dispatch_lun_queue_t 注释）
```

关键护栏：**ISSUE 段完成 ≠ 命令完成**。`lun_drive` 的 `phase == LUN_PHASE_ISSUE` 分支只做"切相位 + 记 DWT 起点 + return WAIT"，绝不下沉到 DONE 把结果当命令完成报出去；相位属于**当前队头槽**，命令出队即随 `phase=IDLE` 复位。

### 23.6 流程图：多 LUN ISSUE/WAIT/FETCH 并发时序波形（图 21）

```text
t →      ISSUE(µs)      tR=38µs / tPROG=700µs / tBERS=5000µs        FETCH(µs)
LUN0 ──[00h..CONFIRM]──────────── ISSUED ────────────────────────▶ [WAIT / Dout / status]──
              │ 总线空闲，可切 LUN                    ▲ issued_advance 到点
LUN1 ─────────┴──[00h..CONFIRM]──────────── ISSUED ─────────────▶ [WAIT / Dout / status]──
                                                                    ▲
dispatch 轮询：lun_step 逐个 LUN 推进（每 LUN 每轮一条）；ISSUED 段只做 DWT 比较，不占总线
设计目标：总耗时 ≈ tR + FETCH，而非 2×(tR + FETCH)

⚠ 当前 CH_NUM = CE_NUM = LUN_NUM = 1：没有第二个 LUN，相位机退化为单 LUN 顺序
  ISSUE → ISSUED → FETCH。本图仅表达**设计能力**（图 10 的时序部分），非当前实测行为。
```

### 23.7 流程图：调度器取命令决策（图 22）

```text
fc_poll_fill()   ← 一次填充 pass（快照 HSQ/SQ 数量后各扫一遍）
  │
  ├─ 先扫 HSQ（快照数）：与 SQ 同一套链逻辑（HSQ 同样参与链）
  │     链锁准入（v52）：chain_owner[ch][ce][lun] 非 NONE 且非 HSQ → 回推跳过（不置 suspended）
  │     need = fc_poll_chain_length(hsq, head)   // 链头：连续 chain=1 段，上限 CHAIN_MAX=4
  │     nand_dispatch_lun_free(ch,ce,lun) ≥ need ? → 取出提交
  │     否则该 LUN 本轮挂起（后续同 LUN 命令跳过，含 SQ）
  │
  └─ 再扫 SQ（快照数）：
        链锁准入（v52）：chain_owner[ch][ce][lun] 非 NONE 且非 SQ → 回推跳过（不置 suspended）
        need = fc_poll_chain_length(sq, head)   // 链头：连续 chain=1 段，上限 CHAIN_MAX=4
        nand_dispatch_lun_free(ch,ce,lun) ≥ need ?
          → 是：单命令 submit / 链 submit_chain（原子追加 need 槽）
                下发成功后按【批尾】chain 位更新 chain_owner（MORE→本队列 / END→NONE）
          → 否：该 LUN 本轮挂起 + 命令回推队尾（保序）
        ⚠ FTL 运行时恒 chain = FC_CHAIN_END → need 恒 1 ⇒ chain_owner 恒 NONE
          （链收集与链锁分支仅单测覆盖，见 §23.9）
```

要点：**HSQ 优先只体现在扫描顺序**——它不抢已进 FIFO 的命令（FIFO 严格先进先出）；
HSQ/SQ 共用同一张挂起表（HSQ 挂起的 LUN，SQ 本轮也跳过，避免优先级倒挂）；
**链锁（v52）**是另一张表（`chain_owner`，跨 pass 持久）——链没结束时，该 LUN 只接受链所在
队列的命令，另一条队列回推，直到链 END 下发（见 §2.7）；
"忙"由 `nand_dispatch_lun_free()` 表达，不再有 FC 侧 busy 表；读饥饿保护已删除。

### 23.8 运行时走读：一条读命令

| 跳 | 位置（函数为主） | 发生了什么 |
|----|------------------|-----------|
| 1 | FTL：`fc_command_alloc` → `configure_descriptors` → `fc_submit` | 分配 command/desc/buffer，入 SQ；状态 QUEUED |
| 2 | `main while(1)` → `ftl_service` → `fc_service` ×≤4 | 主循环驱动 |
| 3 | `fc_poll_internal` ① `nand_dispatch_poll` | 先推进 NAND（首轮无完成） |
| 4 | `fc_poll_internal` ② `fc_poll_fill` → `fc_poll_fill_queue` | 一次扫描 HSQ/SQ，按 LUN room 取命令，need==1 |
| 5 | `fc_poll_dispatch_single` → `fc_build_dispatch_request` | 置 ACTIVE；构建 request |
| 6 | `nand_dispatch_submit` | 门控通过 → `nand_dispatch_enqueue` 追加槽 → `task_load` 绑 backend/buffer |
| 7 | 下一轮 `nand_dispatch_poll` → `lun_step` → `nand_dispatch_task_start` → `nand_fc_start` | READ_PAGE_LOAD（ISSUE 型）→ phase=ISSUE → 异步 WAIT |
| 8 | `nand_engine_start/step` | UOP 解释：00h / addr / 30h / CONFIRM → END |
| 9 | `lun_step` → `issued_advance` | 等 tR 到点，`pair_op` 切 FETCH |
| 10 | `nand_fc_start`（FETCH）→ `nand_engine_step` | WAIT_READY + 70h 轮询 + DATA_OUT + CHECK_FAIL |
| 11 | `scatter_page_buffer` | 页缓冲拆回各 CW buffer |
| 12 | 返回 completion → `fc_complete_from_nand` | 写 cw_status/ecc_bits 回 desc；`fc_data_path_process_read` 解扰/ECC/RAID |
| 13 | `fc_complete_internal` | CQ push；状态 COMPLETED |
| 14 | `completion_table_drain` → `fc_poll_completion` → FTL 回调 | FTL 拿到数据 |

### 23.9 澄清：运行时其实不走命令链

**这是最容易被 §16 与图 11 误导的一点。** §16 / 图 11 用大量篇幅讲"命令链执行"（per-LUN chain buffer、`submit_chain`、原子入队），但：

- FTL 运行时**所有**命令都设 `config.chain = FC_CHAIN_END`（`src/ftl/` 下共 **23 处**，含 `ftl.c` / `metadata.c` / `checkpoint.c` / `gc.c` / `journal.c` / `bbt.c` / `pfr` 等）。因此调度器**永不组链**，`count` 恒为 1。
- 于是运行时**只有 `fc_poll_dispatch_single` 是活路径**；`fc_poll_dispatch_chain` / `nand_dispatch_submit_chain` 仅由单元测试覆盖。
- Read Retry（`src/ftl/read_recovery.c`）**不是**原子 `submit_chain`，而是**三条独立提交 + 同步等待**的命令（各自 `fc_submit` + `ftl_internal_sync_wait`）。

结论：把图 11 的链收集分支理解为"**能力存在但运行时未启用**"，主干是单命令路径（图 17/图 19）。链式执行的设计与验证保留在 §16，供单测与未来扩展参考。

> 🔁 **v8 更新（2026-10-06，per-LUN FIFO）**：上面的结论不变（FTL 仍恒 `FC_CHAIN_END`，运行时仍走单命令路径），但**单命令路径本身换了模型**：
> - 调度不再是 FC 侧 busy 表 + `fc_scheduler_select`，而是 `fc_poll_fill()` 一次扫描 HSQ/SQ，按 LUN 保序把能放下的命令（含链）提交进 NAND 的 per-LUN FIFO（深 4）；
> - NAND 侧不再有 per-channel FIFO / Phase 1 / Phase 2，`nand_dispatch_poll()` 遍历各 LUN FIFO、每 LUN 每轮推进一条、完成一条回一份 completion；
> - "忙"由 `nand_dispatch_lun_free()` 表达，`NOT_READY` 不再被 FC 当失败标 `NAND_FAIL`；
> - `fc_scheduler_set_busy` / `clear_busy` / `busy[][][]` / `consecutive_write_count` 均已删除（见 [§24](#24-fc-层断言判据assert--assume) 判据 4 的历史条目）。
>
> 图 9（§4.3）已重绘为遍历模型；图 11 的链收集分支语义改为"连续槽 + 逐条完成"。详见 `docs/changelog/fc_nand_per_lun_fifo.md`。

> 🔁 **v52 更新（2026-10-11，链锁）**：结论仍不变（FTL 恒 `FC_CHAIN_END`，运行时 `chain_owner` 恒 NONE），但补上了一处**与"运行时不走链"无关**的真实缺口——**链长 > 4 时的跨批原子性**。链被拆成多批、分多次填充 pass 下发，v51 及以前批与批之间无锁，另一条提交队列（HSQ/SQ）的命令可插在批边界上。v52 在 `fc_scheduler_t` 加了一张跨 pass 持久的 per-LUN 链锁 `chain_owner[ch][ce][lun]`：链一旦开始，该 LUN 的填充权归链所在队列独占，直到链 END 下发（规则见 §2.7、流程图见 §23.7）。**这是「链能力」本身的补强，不改变运行时主干**——FTL 不组链 ⇒ 锁恒 NONE ⇒ 生产路径逐字不变。由 Level 3 用例 `chain_priority_lock` 覆盖（8 命令 SQ 链 + 同 LUN HSQ 命令，判据 = 终值）。详见 `docs/changelog/fc_chain_priority_lock_v52.md`。

---

## 24. FC 层断言判据（ASSERT / ASSUME）

> **全工程判据已独立成文：`docs/design/assertion_guidelines.md`。** 本节保留 FC 层特有的例外说明，四条判据的完整表述与辨析以该文档为准（特别是 §一"值从哪来"的边界标准、§七辨析 D、§九第二轮补扫）。

断言设施见 `include/common/assertion.h`。FC 层从 v0.50 起按以下四条判据决定"守卫写成断言还是返回状态码"：

1. **外部输入校验保留返回码**——只有来自**外部**的输入（host 的 opcode/LBA、UART CLI `wr/rd/tr/fl` 的参数、介质读回的字节）才返回 `FC_STATUS_INVALID_ARGUMENT` 等错误码。**判定标准是"值从哪来"，不是"声明在哪个头文件"**：调用者自己铸出的句柄/算出的索引/持有的指针，即使经过 `fc_interface.h` 入口，也按判据 2 转断言。

   > **边界修正（辨析 D + 第二轮补扫）**：判据 1 原先把"`fc_interface.h` 的全部入口"整体划为边界，属过度概括。按来源细分后：FC 自铸句柄（`fc_command_id_to_command` / `fc_descriptor_id_to_descriptor` 的 ID）、纯参数前置条件（`command == NULL` 等）转 `ASSERT`；`fc_command_configure_descriptor` / `fc_submit` / `fc_command_release` 里"这个指针是不是本池的命令 + 生命周期阶段"的**身份/状态**校验保留返回码（v13 前第一项是 `fc_command_configure_descriptors`，v13~v16 为 `fc_command_attach_cw`）。详见 `assertion_guidelines.md` §七、§九、§十一。
2. **调用者已保证的前置条件转断言**——`static` 函数、内部头（`fc_internal.h` / `fc_queue.h` / `fc_scheduler.h` / `fc_data_path.h` 等）里的接口，以及跨层入口中调用者必然传有效值的指针/索引参数，守卫一律写成 `ASSERT(条件)`，并删掉原来的 `return` 与恢复/降级分支（"跳过坏 ID 继续找"、"回滚分配再返回"这类）。

   **FC 层不用 `ASSUME`。** 起初按"守卫带恢复分支 → `ASSUME`"划分，后来取消：恢复分支既然已经手工删掉，`ASSUME` 的 `__builtin_unreachable()` 就换不来额外代码生成收益（工程未开 LTO，`fc_command_id_to_command()` 这类跨 TU 调用点无从利用该假设）；而它会把"违反即 UB"引入一条本来只会 HardFault 的路径——同样是崩，`ASSERT` 的现场更可定位。`ASSUME` 宏保留在 `assertion.h` 里，留给将来确有热路径需要向编译器证明的场合。
3. **资源类结果与运行时状态保留返回码**——池耗尽（`FC_STATUS_RESOURCE_EMPTY`）、CQ 满背压（`FC_STATUS_IO_ERROR`）、NAND 报告的错误（`FC_STATUS_IO_ERROR`）、`ctx->active == 0U` / `fc_buffer_is_occupied(...) == 0U` 这类"此刻能不能做"的状态，都是**预期内**的运行结果，不是不变量破坏，必须返回给上层。（`g_crc_ready == 0U` 曾在此列，**v33 起随 CRC 改走 HAL 驱动整体删除**——CRC 自检失败转 `ASSERT`，见 `docs/changelog/fc_crc_hal_driver_v33.md`。）
4. **单测固化为契约的守卫保留返回码**——判据 2 的例外，但**只在测试是被测函数的正当调用者时成立**：`fc_command_validate`（校验器，返回判决是职责）、`test_rebuild.c` 的 `allocator_remove_from_free(..., 越界)`（文档化 no-op）等保留。（原条目还列了 `fc_scheduler_set_busy/clear_busy` 的越界忽略——**v8 起该 API 与 busy 表已整体删除**，见 §23.9。）反之，**只有测试会走"传非法参数"分支**的（`fc_command_status_set_cw` 的 `cw >= 4`（该函数 2026-10-08 因零生产调用者整体删除）、`fc_scheduler_init(NULL,…)`、`fc_submit(NULL)`、`fc_data_path_encode(NULL)` 等），按判据 2 转 `ASSERT` 并**删掉对应用例**。

**判据 4 的边界：什么时候可以删用例而不是保留守卫。** 反例是 `fc_queue.c` 的指针判空：`fc_submission_queue_*` / `fc_completion_queue_*` 的生产调用者只有 `fc.c` 与 `fc_scheduler.c`，**全部传有效指针**——唯一会传 `NULL` 的就是测试自己。这种"只有测试会走的分支"应当按判据 2 转 `ASSERT`，并**删掉对应用例**（它们验证的行为届时已不存在，留着反而是错的）。两轮补扫共删除 `test_fc_level1.c` 的 13 个 `*_null`/越界用例与 `test_fc_level2.c` 的 3 个。

**判据 2 的补丁（v39，全工程判据见 `assertion_guidelines.md` §二十四 判据 9）**：判据 2 假设「断言消失后坏状态会以别的方式暴露」。但有一类配置错误**不**满足——违反之后程序照常跑完、不报错，只是**悄悄写坏介质**（首例：multi-plane 窄位图，见 §25.4）。对这类错误，`ASSERT` 之外**必须**再补一道 release 档运行期拒绝（`return FC_STATUS_INVALID_ARGUMENT`，由调用者转成命令失败并注 `NAND_FAIL` 完成项），否则 release 档等于**没有守卫**。

> `make tests` 走 `BUILD_TYPE=debug`（无 `NDEBUG`），断言在板上测试固件里是**活的**：任何一条误判都会让板子停机而不是报失败。因此改动这些守卫后必须上板跑三套 munit 套件。

---

## 25. 按 CW 子集读（Read Recovery 的 FC 层支持）

### 25.1 动机

读恢复（read recovery）的场景是：一页 4 个 CW 里 ECC 只挂了其中 2 个或 3 个，恢复流程只需要**重读失败的那几个 CW**，不必重读整页。

但改动前的 FC 表达不了这件事：

- `fc_cmd_conf_t.cw : 2` 的语义是 **"CW 数减一"**，只能表达"前 N 个 CW"，表达不了 `{0,1,3}`。
- 建链处单 plane 的 CW 号是 `paa_to_cw(command->paa) + index`——从命令 PAA 的 CW 位起**连续递增**。
- 读路径把 `column` 写死 0，靠首个 `00h-30h` 的 DATA_OUT 取 CW0，再用 `05h-列-E0h` 续读 CW1..3；而 `paa_to_nand_row()` **丢掉 CW 位**，命令 PAA 里的 CW 是被静默忽略的。

**本次范围**：FC 层 + 它驱动的 NAND dispatch 层。**不含** `src/ftl/read_recovery.c` / `ftl.c` 的恢复流程接线。

### 25.2 三项设计决策

| # | 决策 | 取值 |
|---|------|------|
| 1 | 位图入口 | **建链形参 `cw_bitmap`**（v28 起；v13~v27 是 `fc_command_t.param.cw_bitmap` 字段）：**flat 32 位位图**，bit `n = plane * CW_NUM + cw`，覆盖 `CW_TOTAL_MAX = 8` 位。绝对编号，不是"每 plane 各自的低 4 位" |
| 2 | 读流程 | **严格统一**：所有读命令都拆成「加载进 cache」+「按 bitmap 逐 CW `05h/06h-E0h`」，**CW0 也走逐 CW 取**（不与加载融合） |
| 3 | 范围与输出 | 单 plane + 多 plane 一并支持；失败 CW 位图一并从 FC 暴露（同为 flat 编号，可与请求位图求交集） |

> **flat 编号要点**：只读 plane 1 的 4 个 CW 是 `0xF0`（不是 `0x0F`）；plane 0 取 {CW0,CW3} + plane 1 取 {CW1} 是
> `0b0010_1001 = 0x29`。单 plane 命令同理——plane 1 的单 plane 命令位图也是 `0xF0`，bit 落在
> `paa_to_plane(command->paa)` 那个 plane 的区间；建链处 `allowed` 位图挡住越出命令 plane 位段的位。
>
> **`bitmap == 0` 是空集，不是"全部"**（**v27**）。v23~v26 期间 `0` 被重载为「本命令 plane 段内的
> 全部 CW」（v7~v22 更早是"对命令的**每个** plane 取连续 `0..config.cw`"，逐 plane 平移，不能照抄
> 单 plane 的 `(1 << (config.cw+1)) - 1`——那只会点亮 plane 0）；该重载已取消，因为"生产者忘了填"
> 与"有意访问 0 个 CW"从此不可区分。**整段访问的写法是显式的**：
> ```c
> ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd));  /* 须在 pba 与 multi_plane 写好之后 */
> ```
> 三个「整段访问」入口（`addr_to_cmd` / `ftl_cmd_setup_ppa` / `ftl_cmd_setup_ppa_pair`）各自替你调它；
> 只有 `ftl_cmd_configure_descriptors()` 接受调用者给的部分位图。校验/展开规则唯一定义在
> `ftl_cmd_resolve_cw_bitmap()`（`src/ftl/ftl_fc_interface.c`，static），两个建链入口共用。

### 25.3 核心不变量：唯一索引空间 = flat 绝对 CW 号

**v9 起**，跨 FC/NAND 边界只有**一个** CW 坐标：

```
n = plane * CW_NUM + cw        （0 .. CW_TOTAL_MAX-1 = 0..7）
```

建链形参 `cw_bitmap` 的 bit `n`、请求的 `cw_bitmap` / `cw_buffers[n]`、完成的 `cw_status[n]` /
`ecc_bit_count[n]`，全部用这一个 `n`。物理 plane 由 `n / CW_NUM` 得出，
CW 号由 `n % CW_NUM` 得出——不再各存一份，也不再有任何"链走位 / plane 分桶密排槽位"的中间坐标。

```
bitmap = 0b1011（CW0/CW1/CW3）单 plane 读
  desc 链：      desc#0 → desc#1 → desc#2      （链序无关紧要：位图是集合）
  desc->paa：       CW0     CW1     CW3
  flat 号 n：        0       1       3         （= 位图置位的三个 bit，也 = cw_status 的下标）
```

- **列地址**由 `n % CW_NUM` 决定：`column = (n % CW_NUM) × CW_SIZE`。
- **行地址**不参与该空间：它是 per-plane 的，走 `plane_paa[plane]`（见 §25.4）。
- **链序与 `n` 无关**：`fc_build_request_data_planes` 沿 desc 链遍历，把每个 desc 落位到它自己
  PAA 算出的那一格；落位顺序不影响结果（位图是集合）。`nand_dispatch_task_t.cw_index` 存的就是
  `n`（当前 CW 的绝对号），`cw_chain` 用 `nand_dispatch_cw_next(bitmap, cw_index+1)` 推进到下一个
  置位，`completion_per_cw` 用它当 `cw_status[n]` 的下标。

> **历史（v6/v7）**：v6 引入 CW 掩码时，掩码是 flat 的、但下游仍是"plane 分桶 + 每 plane 密排槽位"，
> 靠"链序 == plane-major"这条隐式不变量把两套坐标对齐（`cws[plane][slot]`、`cw_count[plane]`、
> `slot_to_plane_cw()` 在 `cw_count[]` 上走减）。v9 把这条不变量连同整套中间坐标一起删掉——
> 掩码稀疏，下游也稀疏，两套坐标合一。

静态审计口径：`src/nand/` 里不应再出现用槽位当 CW 号的偏移表达式，只剩 `n % CW_NUM * CW_SIZE`。
结构字段/助手名 `cws[` / `cw_paa` / `slot_to_plane` / `nand_dispatch_cw_info_t` /
`completion_multi_plane` 全仓 0 命中；`cw_count` 仅剩 stripe/RAID 代码里的**局部计数器**
（`fc_stripe_submit`、`fc_data_path.c` 的 parity 循环，与 dispatch 契约无关）。

### 25.4 建链

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
`fc_descriptor_t.config.cw` 语义不变 = 本 desc 在 plane 内是第几个 CW。

链序为 **plane-major**（先 plane 0 的全部置位 CW，再 plane 1）——这只是 desc 的**建立顺序**，
v9 起不再被任何消费点当作坐标使用（落位按 `n` 独立进行，链序无关紧要）。`fc_cw_mask_popcount`
手写循环到 `CW_TOTAL_MAX`，不引 `__builtin_popcount`（避免 libgcc `__popcountsi2` 依赖）；
**该函数在 v13.1 随其唯一调用点（同源查重 ASSERT）一并删除**（见 §25.4 v13.1）。

`fc_command_configure_descriptor_internal` 的签名从 `index` 改为显式的 `(plane, cw)`——CW 号由调用方按 bitmap 枚举给出，不再由 desc 序号推导。（v17 后该函数即 `fc_command_configure_descriptor`，见下 §25.4 v17。）

#### v11：机制 / 策略拆分

上面的掩码展开是**策略**（"访问哪些 CW"），把 desc 分配 + buffer 分配 + 挂链那半截是**机制**。
v11 把机制抽成独立原语，公开给上层：

```c
/* 机制层：分配一个 desc + buffer、填 PAA/config/meta、挂到命令 desc 链尾。 */
fc_status_t fc_command_attach_cw_internal(fc_t *c, fc_command_t *cmd, uint8_t plane, uint8_t cw, nand_paa_t row_paa); /* fc.c */
fc_status_t fc_command_attach_cw(fc_command_t *cmd, uint8_t plane, uint8_t cw, nand_paa_t row_paa);                  /* 公共入口 */
```

拆开后，上层可以**自己算集合**（例如同页内只读 `{plane0:CW0, plane0:CW3, plane1:CW1}`）：

```c
fc_command_t *cmd = fc_command_alloc();
/* 填 config；各 plane 传自己的行地址（此处同页，故只有 plane 位不同） */
nand_paa_t row0 = paa_from_addr(&(nand_addr_t){.lun = lun, .block = block, .plane = 0, .page = page});
nand_paa_t row1 = paa_from_addr(&(nand_addr_t){.lun = lun, .block = block, .plane = 1, .page = page});
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

#### v12：per-plane 行地址（Policy B）

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

**行为中性论证**：现有全部调用者（策略层、FTL 的 `addr_to_cmd` / `ftl_cmd_setup_ppa`）传进来的
`row_paa` 都是"`command->paa` 的 page/block/lun + 目标 plane"，与 v11 的
`paa_make(paa_to_page(cmd->paa), plane, paa_to_block(cmd->paa), paa_to_lun(cmd->paa), cw)` 逐字节相同。
FTL 侧的 `ftl_cmd_setup_ppa_pair()`（`src/ftl/ftl_fc_interface.c`）是第一个真正传不同行地址的调用者。

**不改**：erase / SET/GET_FEATURES 分支仍读 `command->paa`（非数据命令无 desc 链）；
`fc_command_plane_span` 仍按 `command->paa` 定 plane 段（`multi_plane > 0` 时恒 `[0, multi_plane)`，
故 multi-plane 命令的行地址来源变化不影响 plane 段判定）。

> 详细落地见 `docs/changelog/fc_per_plane_row_address.md` 与 FTL 侧的 `ftl_policy_b_per_plane_block.md`。

#### v13：策略层上移 FTL

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

> 详细落地见 `docs/changelog/fc_policy_layer_uplift.md`。

#### v13.1：建链收敛（plane 段规则合并 + 删冗余断言）

v13 留下了两处可收敛的冗余，v13.1 一并处理，**行为中性**（产出给 NAND 的 `nand_dispatch_request_t` 逐字节不变）：

1. **plane 段规则合并为一份**。`fc_command_plane_span` 上移到 `include/fc/fc_command.h` 作 `static inline`
   （它只读 `command->config.multi_plane` / `command->paa`，不碰 `fc_t`，故不受"FTL 看不到 `fc_t`"约束），
   FC 的越段校验与 FTL 的掩码展开**共用同一份定义**；`ftl_cmd_plane_span`（`ftl_fc_interface.h`）删除。
   改 plane 语义（`PLANE_NUM` / BA6 含义 / 单 plane 起点）从此只需动一处。
   ⚠ FC 侧的越段校验（`fc_command_attach_cw_internal` 里 `plane` 不在 `[first, last)` 返回 `INVALID_ARGUMENT`）
   **保留**——它挡的是"desc 落到 `plane_paa[]` 覆盖范围之外"这个静默腐坏，不是重复实现。

2. **同源查重断言二选一**。"无重复 `n`"（掩码位数 == 链长）原先在两层各查一次：
   `fc_command_attach_cw_internal` 的 attach 侧（`fc_command_cw_attached`，覆盖单/多 plane 全部路径）
   与 `fc_build_request_data_planes` 单 plane 分支（`fc_cw_mask_popcount(mask) == fc_command_descriptor_count(...)`）。
   保留前者——它在**污染点**（attach）就炸，定位更准，且覆盖完整；后者连同 `fc_cw_mask_popcount` 删除。
   `fc_command_descriptor_count` 不受影响（`fc_command_release` 等仍在用）。

> 详细落地见 `docs/changelog/fc_plane_span_consolidation.md`。

> **v16 更正（理由，非结论）**：上面保留 `fc_command_cw_attached` 的**结论**不变，但
> 「掩码迭代可能产生重复」这个**理由不成立**——掩码是集合，枚举不会重复置位。
> 真正可达的重复路径是 `ftl_cmd_setup_ppa_pair`：plane 号取自 `pba_to_plane(pba[i])`
> （L2P 查表结果），两个 LBA 可能解析到同一 plane → 重复 `(plane, cw)`。
> ⚠ release 构建下该断言被裁空，此违规无守卫。详见 `docs/changelog/fc_chain_ownership_v16.md`。

#### v17：建链三层接力 → 一条公共入口

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
  （该路径由此硬绑单例 `g_fc_controller`——当前全仓只有一个 `fc_t` 实例，多实例时须改回 `controller` 参数化入口）。

详见 `docs/changelog/fc_build_chain_convergence_v17.md`。

#### v23：FTL 建链收敛为「单一掩码旋钮」（2026-10-08）

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

详见 `docs/changelog/ftl_cw_mask_single_knob_v23.md`。

#### v27：`cw_mask` → `cw_bitmap`，取消「0 = 段内全部」重载（2026-10-09）

**动机**：v23 的「`0` = 本命令 plane 段内全部 CW」是**重载**——同一个字段既有"字面集合"读法，
又有"未指定 → 展开"读法。它带来两个代价：

1. **两层 0 方向相反**（见上）：FC 侧 0 = 全部，NAND 侧 0 = 空集。这不是笔误，是重载的直接产物，
   只能靠文档解释，任何"顺手统一一下"的改动都会静默改变语义。
2. **名字与语义不符**：字段叫 mask 时"0 = 全部"勉强说得通（位掩码的常见重载），但它逐位表达的是
   **集合**，与 NAND 层的同名字段口径不一致。

**改动**：字段改名 **`cw_bitmap`**（FC 侧 `fc_command_t.param.cw_bitmap`、NAND 侧
`nand_dispatch_request_t.cw_bitmap`），语义统一为**字面位图**：置 1 的位 = 要访问的 CW，
**全 0 = 一个都不访问**。v23~v26 的重载取消。

**「整段访问」改为显式**——新增 `fc_command_plane_span_cw_bitmap(cmd)`（`fc_command.h` static inline），
返回本命令 plane 段内全部 CW 的 flat 位图（plane1 单 plane ⇒ `0xF0`，双 plane ⇒ `0xFF`）。
三个语义为"整段访问"的 FTL 入口各自调用它：

| 入口 | 调用者 | v27 起的位图来源 |
|---|---|---|
| `addr_to_cmd`（`ftl_fc_interface.h`） | `sync_page_prepare` → 四个 `ftl_sync_*` 页原语 | 入口自己填 `plane_span_cw_bitmap` |
| `ftl_cmd_setup_ppa` | `gc.c` | 同上（**在 `cmd->paa` 写好之后**算） |
| `ftl_cmd_setup_ppa_pair` | **host 读写**（`ftl.c`） | 同上；**调用者先前设的位图会被丢弃** |
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

详见 `docs/changelog/fc_cw_bitmap_literal_v27.md`。

#### v28：CW 集合退出命令结构体，改作建链形参（2026-10-09）

**动机**：v27 之后，`cmd->param.cw_bitmap` 这个字段在 FC 内**既无写入者也无读者**——
唯一读者 `ftl_cmd_resolve_cw_bitmap()` 就在建链那一刻把它取出来用掉。它是**建链的输入**，
不是命令的属性，没有留在命令里被回读的理由。同理 CQ 侧的失败位图是**派生量**：
上层本就能遍历 desc 链 OR 各 `desc->status` 自己算。

**改动**：

| 项 | v27 | v28 |
|---|---|---|
| SQ 请求集 | `cmd->param.cw_bitmap` 字段 | **形参** `ftl_cmd_configure_descriptors(cmd, cw_bitmap)` |
| CQ 失败集 | `completion.param.cw_bitmap`（`fc_complete_from_nand` 聚合） | **删除**；上层遍历 desc 链自取 |
| `fc_param_t` | `parameter0..3` / `bytes[4]` / `cw_bitmap` | `parameter0..3` / `bytes[4]`（仅 FEATURES 内联数据） |
| `fc_command_status_t.param` | 失败位图 / FEATURES 回读 | **保留**（仅 FEATURES 回读） |
| ERASE 请求位图 | 伪造 CW0 位（给 completion 一格写 status） | **合法空集**，由 `completion_all_cws` 空集分支写 `cw_status[0]` |

`ftl_cmd_resolve_cw_bitmap()` 的首参由 `const fc_command_t *cmd` 改为 `uint32_t bitmap`——
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

详见 `docs/changelog/fc_cw_bitmap_param_v28.md`。

### 25.5 读路径统一

FC 层命令**保持 `WITH_DATA_OUT` 语义**（desc 建链、`fc_is_page_read`、数据路径、完成项判定都用它），但下发给 NAND 的是对应的「只加载、不输出」op。转换点位于 `fc_build_dispatch_request` 末尾：

```c
if (request->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT)
    request->operation = OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT;
else if (request->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT)
    request->operation = OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT;
```

这样任意 CW 子集（含不含 CW0）走同一条路径。链推进逻辑（`nand_dispatch_task_cw_chain`）随之从「加载型 op 完成后 `cw_index++` 再发 `05h`」改成：

| 当前 op | 完成后动作 |
|---------|-----------|
| **加载型**（两种 `WITHOUT_DATA_OUT`） | **不推进槽位**，从槽位 0 起发取数 op；失败则链终止 |
| **取数型**（`OP_RANDOM_DATA_READ` / `_MULTI_PLANE`） | 推进一个槽位，直到链尾 |
| **旧语义**（`WITH_DATA_OUT` 普通读 / OTP 读） | CW0 已由 DATA_OUT 取回，其余逐 CW 随机读（保留给直接下发场景） |

取数 op 按 plane 选：单 plane → `OP_RANDOM_DATA_READ`（`05h-列-E0h`）；多 plane → `OP_RANDOM_DATA_READ_MULTI_PLANE`（`06h-列-行-E0h`，行地址带 plane 位，由 `paa_to_nand_row()` 编码）。

多 plane 的 CW 推进走**同一个 flat 位图**：`cw_index` 从 `nand_dispatch_cw_first(bitmap)` 起，
每取完一个用 `nand_dispatch_cw_next(bitmap, cw_index+1)` 跳到下一个置位，直到 `CW_TOTAL_MAX` 哨兵。
列地址 `(cw_index % CW_NUM) * CW_SIZE`，行地址 `paa_to_nand_row(plane_paa[cw_index / CW_NUM])`——
plane 与 CW 号都从这一个 `n` 算出，不再有"槽位 → (plane, ordinal)"的换算。

> **v9 起删除**：`nand_dispatch_task_total_slots()`（槽位总数 = Σ `cw_count[plane]`）与
> `nand_dispatch_slot_to_plane_cw()`（在 `cw_count[]` 上走减的槽位→plane/CW 换算）随两套坐标一起删除。

**cache 读族不动**（`OP_READ_PAGE_CACHE_SEQ` / `_RANDOM` / `_LAST` / `OP_READ_PAGE_FETCH`）——它们是 cache 管线机制，不是"加载进 cache"。

### 25.6 新增 op：`OP_RANDOM_DATA_READ_MULTI_PLANE`

`06h` 此前只**内联**在 `g_nand_multi_plane_read_with_data_out_uops` 里（多 plane 读后分 plane 取 cache），没有独立 OP 码。本次为逐 CW 取数把它提成独立 op：

```
70h轮询 → 06h → C1 → C2 → R1 → R2 → R3 → E0h → tR → Dout → 70h轮询
```

| 字段 | 值 |
|------|-----|
| OP 码 | 41（`OP_COUNT` 41 → 42） |
| `address_cycles` | 5（文档性字段，代码不消费） |
| `data_length` | `CW_SIZE` |
| `has_data` | 1 |
| uop 数组 | `g_nand_random_read_multi_plane_uops` |

与 `OP_RANDOM_DATA_READ`（`05h`）的唯一区别是命令字节 `06h` + E0h 前的 3 个行地址周期。**协议命令常量 `NAND_CMD_RANDOM_DATA_READ_TWO_PLANE`（`0x06U`）不改名**——那是 ONFI 协议命令自身的名字（Micron 文档里 06h 就叫 Random Data Read Two Plane）。

**前置约束（Micron MT29F4G08 datasheet）**：

1. `06h` 必须紧跟 TWO-PLANE PAGE READ（`00h-00h-30h`）之后 —— 即本项目的 `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT`（本流程正是如此）。
2. plane 由**行地址的 BA6** 选择（`paa_to_nand_row()` 把 plane 编码在 bit6，与 `PAGE_ADDR_WIDTH = 6` 对齐）。
3. two-plane 命令族（本 op、`OP_PROGRAM_PAGE_MULTI_PLANE`、`OP_READ_PAGE_MULTI_PLANE_*`、cache mode）要求**器件内部 on-die ECC 关闭**（datasheet **Note 4**——⚠ 该条**出处待重核**：MT29FxG08xAA Rev B 里 Table 6/7 的 Note 4 是 interleaved die，全文无 `on-die`；疑在 ABADA 专用 datasheet，见 §25.8 风险 2）。本项目从不写 ECC feature 寄存器（`0x90`），内部 ECC 保持默认关闭，故可用；启用 on-die ECC 会使全部多 plane 路径失效。

### 25.7 完成项

**NAND 侧**：新增 `completion_load` 策略，两种加载型 op 用它——**成功时不写任何 `cw_status`**（各槽位由后续逐 CW 取数逐个填入），失败时把失败状态刷满 8 槽（加载失败后取数链不会再启动，没有其它来源能反映这次失败）。

**FC 侧** `fc_complete_from_nand` 第 3 步：

- status 折叠走 desc 链全链 OR；首个失败 CW 的 desc_id（失败 = `FC_CW_STATUS_ERROR_MASK` 全集）。
  **v28 起不再聚合失败位图**——上层要定位失败 CW，遍历 desc 链 OR 各 `desc->status` 自取
  （desc 链保留到 `fc_command_release()`；位图编号仍与请求位图同空间，可直接求交集）。
- **CQ 不再携带 per-CW ECC**（`ecc_bit0..3` 已删除）。per-CW `ecc_bits` 只留在 descriptor 链；
  命令级聚合 ECC 走 `fc_get_cmd_ecc_max()`——**取全链最大值**（含两 plane）。旧实现只取 plane 0，
  在 plane 0 为空的 flat 位图下会恒为 0，是错的而非保守的。
- GET_FEATURES 的寄存器值经控制器级暂存 `g_fc_return_staging[command_id]` 拷进 `completion.param.bytes`；
  SQ 命令体提交后不再被写（见 `fc_command.h`「SQ 不可变」）。

### 25.8 风险

| # | 风险 | 说明 |
|---|------|------|
| 1 | **热路径改变（最高）** | 严格统一后，每次整页读多一轮 `70h + 05h-列-E0h + WAIT_READY`（原来 CW0 靠 `00h` 的 DATA_OUT 一次拿到）；多 plane 整页读从 1 条原子事务变成 1 次加载 + `2 × cw_count` 条事务。**必须重新上板验证**。若不可接受，退回方案是「含 CW0 时把 CW0 融进加载」——局部改动 |
| 2 | **`06h` 的 plane 语义** | 已按 Micron「4Gb, 8Gb, and 16Gb x8 NAND Flash Memory」（MT29FxG08xAA, Rev B）**逐字打开核对**（Table 7 Two-Plane Command Set / Table 6 注 / §Two-Plane Addressing）：`06h` 5 地址周期（2 列 + 3 行）✓、plane 由行地址 **BA6** 选择（"The least significant block address bit, BA6, must be different for the two addresses"）✓、`06h` 必须跟在 TWO-PLANE PAGE READ 后（Table 6 注 2："limited to use with the TWO-PLANE PAGE READ command"）✓。⚠ 第 4 项「two-plane 命令族要求 on-die ECC 关闭（**Note 4**）」**未在本 datasheet 对上**——该 PDF 里 Table 6/7 的 Note 4 是"valid during busy … interleaved die operations"，全文 `on-die` 出现 **0 次**；该条出处待重核（疑在 ABADA 专用 datasheet）。**上板未验，on-board 行为仍是最终判据** |
| 8 | **`multi-plane` 不是 ONFI 1.0 的东西**（2026-10-09 查证） | **ONFI 1.0（Rev 1.0, 2006-12-28）根本没有 multi-plane**：Table 14 命令集里 cache 只有单 plane 的 `Read Cache` 31h / `Read Cache End` 3Fh / `Read Cache Enhanced` 00h-31h / `Page Cache Program` 80h-15h，**全文 `plane` 一词出现 0 次**（1.0 唯一的「多设备」是 multiple LUN，走 `Interleaved` D1h / 11h）。multi-plane 全部来自器件侧（Micron datasheet 的 Table 7 Two-Plane Command Set）。⚠ 因此本文件里凡以「ONFI」泛称 multi-plane 的地方都要写成「器件 / Micron datasheet」，别让读者以为 1.0 里有。**未落地缺口**：本器件有 `TWO-PLANE PROGRAM PAGE CACHE MODE 80h-11h-80h-15h`，本项目 op 表**没有**这个 op（cache 编程只有单 plane 的 `OP_PROGRAM_PAGE_CACHE`）；器件**没有** two-plane cache 读 |
| 3 | **producer/consumer 索引不一致** | v6 把链槽位与 CW 号解耦后，消费点必须一次改齐；**v9 起两套坐标合一，本条风险随之消失**（见 §25.3 历史注 / §25.11） |
| 4 | **`LUN_NUM>1` 的 ISSUE/WAIT 拆分** | 转换后的两个加载 op 不再命中 Phase V splittable 拆分，多 LUN 重叠会失效。当前 `LUN_NUM=1` 本就不生效；启用多 LUN 前需重新对齐 |
| 5 | **`bitmap == 0` 的展开规则（v7 起为最高，v23 重定义，v27 取消重载）** | v7~v22：必须逐 plane 平移，照抄单 plane 的 `(1 << (config.cw+1)) - 1` 只会点亮 plane 0 → 所有 FTL 多 plane 命令当场退化。v23~v26：「0 = 本命令 plane 段内的全部 CW」，与 `config.cw` 解耦，该陷阱消失但引入"两层 0 方向相反"。**v27 起 0 = 空集**（字面位图），整段访问由 `fc_command_plane_span_cw_bitmap()` 显式给出；新风险是「忘填 ⇒ release 下静默空操作」（见 §25.4 v27） |
| 6 | **空 plane 的行地址（v7，flat 独有）** | 只读 plane 1（`0xF0`）时 plane 0 计数为 0，但多 plane 加载的行地址取自 `cw_paa[plane][0]`；FC 必须为命令 plane 集合内每个 plane 填槽位 0 的 PAA，否则行地址退化成 block0/page0 静默误操作。**uniform 命令下完全测不出来** |
| 7 | **链长度真值切换（v7）** | 把 `multi_plane*(config.cw+1)` 换成走链，漏改一处就在 flat 命令下算错长度；uniform 命令（全部 FTL 现状）必须逐字节等价 |

### 25.9 文件变更

| 文件 | 改动 |
|------|------|
| `include/fc/fc_command.h` | v6：`fc_command_t` 加 `cw_mask:4`；`fc_command_status_t` 加 `failed_cw_mask:4`。**v7**：删 `cw_mask:4` / `parameter0..3` → `fc_param_t param`；删 CQ 的 `ecc_bit0..3` / `failed_cw_mask` → `fc_param_t param`；新增 `fc_cw_mask_is_plane_symmetric()` |
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
| `docs/design/fc_api_usage.md`、本文件 §2 / §13 / §14 / §25 | **v7**：flat 掩码 + CQ 返回项同步 |

> **v9**（索引空间统一）的逐文件改动见 §25.11——它把上表里 v6/v7 的 `cws[plane][slot]` /
> `cw_count[PLANE_NUM]` / `cw_paa[][]` 全部删除，上表对应行只作历史记录。
> **v27**（`cw_mask` → `cw_bitmap`）的逐项改动见 §25.4 v27——上表里的 `cw_mask` / `fc_cw_mask_is_plane_symmetric`
> / `fc_cw_mask_popcount` 等标识符均为 v6/v7 当时名，只作历史记录。

### 25.10 验证状态

- v6：`make format` + `make debug` / `make tests` / `make release` 三档**零警告**通过（153412 / — / 87660 字节）。
- v7：见 `docs/changelog/fc_flat_cw_mask_cq_param.md` 的验证记录。
- v9：见 `docs/changelog/fc_cw_index_unification.md` 的验证记录。
- v27：仅 `make format-check` 通过（101 文件）。**未编译**——本轮处于测试代码重构期（Level 2/3 尚在
  引用旧夹具符号），编译验证留到调用点迁移完成之后。
- **上板未验证**（按平台规则由用户执行）。回归护栏：现有全部读用例必须通过。
- **新语义用例**：v7 补齐 flat 掩码三类用例（`0x29` 混合 / `0xF0` 只选 plane1 / `mask==0 + multi_plane=2` 建 8 desc）。
- **datasheet 复核未做**（风险 2）。

### 25.11 v9：索引空间统一（2026-10-06）

#### 动机

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

#### 设计：一个坐标

**统一为 flat 绝对 CW 号 `n = plane*CW_NUM + cw`（0..CW_TOTAL_MAX-1）**，与 `param.cw_mask`
的 bit 编号、CQ 回传的 `param.cw_mask` 完全同一空间。行地址从 CW 项里**拆出来**，改由
per-plane 的一维数组承载：

```c
uint32_t cw_mask;                    /* bit n = plane*CW_NUM + cw */
nand_paa_t plane_paa[PLANE_NUM];     /* 各 plane 的行地址承载 PAA（与掩码解耦） */
uint8_t   *cw_buffers[CW_TOTAL_MAX]; /* [n]，仅 n ∈ cw_mask 有效 */
```

为什么 `plane_paa[]` 不能省：`paa_to_nand_row()` 把 plane 编码进行地址（BA6），
所以 multi-plane 命令的两个行地址**必须**分别是两个 plane 的——一条行地址推不出另一条。
拆成显式数组后，"空 plane 也要填"从"槽位 0 无条件照拷"的隐式约定，变成一条按 plane 遍历的
直白赋值。

#### 改动

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

#### 新增断言

| 位置 | 断言 | 理由 |
|------|------|------|
| `fc_build_request_data_planes` | 同一 `n` 不被两条 desc 覆盖 | 掩码是集合，重复置位会静默丢数据 = desc 链构造错误 |
| `fc_build_request_data_planes` | `popcount(cw_mask) == 链上 desc 数`（单 plane） | 多一条或少一条都说明有 desc 落到了别的 plane |
| `fc_complete_from_nand` Step 1 | `n < CW_TOTAL_MAX` | 由 PAA 现算，越界即 PAA 构造错误 |

#### 验证

- 三档零警告：debug 152128B / release 87016B / `make format-check` 通过。
- `make -k tests`：仅卡在用户维护的 `test_fc_level1.c`（旧 scheduler API）与
  `test_fc_level3.c:48`（旧 `set_device_ready`），与本次改动无关。
- **上板未验证**（用户执行）。

#### 风险

| # | 风险 | 说明 |
|---|------|------|
| 1 | `completion_single` 的掩码分支 | 空掩码写 `[0]`、非空按 `n`。漏掉这个分支时 `OP_READ_PAGE_LOAD`（带掩码、走 `completion_single`）的失败状态会丢——已修，且只在稀疏掩码 + 失败路径才显形 |
| 2 | 三处 completion handler 的下标语义 | `completion_all_cws` / `per_cw` / `load` 现在写的都是 flat `n`，FC 也按 `n` 读——两侧必须同时是 v9，不能一新一旧 |

---

## 26. 与行业分层模型的对照（v13 的判据来源）

本节的作用是**防止 FC 接口再次长胖**：以后每加一个对外函数，先拿它去比对下面这张"硬件 FCC 寄存器表"。

### 26.1 行业里的分层

企业级 SSD 主控的固件分层大致是（术语各家不同，职责划分基本一致）：

```
   Host
    │  NVMe SQ/CQ + doorbell + MSI-X（或 eMMC CMDQ Task Descriptor List）
    ▼
 ┌─────────────┐
 │  HIL        │  Host Interface Layer：协议解析、队列管理、doorbell 响应
 ├─────────────┤
 │  FTL        │  逻辑→物理映射、GC、WL、坏块管理、掉电恢复、缓冲调度
 ├─────────────┤
 │  FCC        │  Flash Channel Controller：**硬件**。通道/CE/LUN/plane 时序、
 │             │  描述符链执行、ECC 编解码、加扰、DMA 搬数据
 ├─────────────┤
 │  DRAM       │  映射表 / 命令队列 / 数据缓冲（DRAM-less 时由 SRAM + NAND 兜底）
 └─────────────┘
    │  ONFI / toggle 总线
    ▼
  NAND die ── channel → chip → die → plane → block → page → CW
```

**主机接口的形态**（三种代表，本质相同）：

| 形态 | 提交 | 完成 | 描述符 |
|---|---|---|---|
| NVMe | SQ 写命令 + doorbell 寄存器 | CQ + MSI-X 中断 | PRP/SGL 描述数据缓冲 |
| eMMC CMDQ | Task Descriptor List（`CMD44/45`） | Task 完成 + 中断 | 描述符内含数据地址、方向、块数 |
| OCSSD / ZNS | 裸写指针 / zone 追加 | 完成队列 | 极简，设备侧几乎无 FTL |

三者的共同点：**设备侧固件拿到的是一条"已解析的请求"**（目标 LBA/地址 + 数据缓冲指针 + 操作类型），
"访问哪些物理单元""数据怎么搬"都是**固件**的事——硬件接口只负责"把请求排好队、按序执行、回一个完成"。

### 26.2 本项目的映射

| 行业部件 | 本项目 | 说明 |
|---|---|---|
| HIL | `src/main.c` + UART CLI + `ftl/` 的对外接口 | 命令入口、调度驱动 |
| FTL | `src/ftl/` | L2P、GC、WL、Journal、Checkpoint、BBT、缓冲调度 |
| FCC（硬件） | `src/fc/` + `src/nand/` | **两者合起来才等于行业的一个硬件 FCC** |
| DRAM | `FC_BUFFER_POOL` + FTL 的 SRAM 数组 | DRAM-less：缓冲池就是"片上页缓冲" |

`src/fc/` 与 `src/nand/` 的分界正是**行业 FCC 的内部再拆分**：

- `src/nand/`（微码引擎 + FMC 后端 + ONFI 命令表）= FCC 的**时序执行单元**（发命令、等 R/B#、搬数据）
- `src/fc/`（三池 + desc 链 + SQ/CQ + 数据路径）= FCC 的**前端**（描述符链、队列、ECC/加扰/RAID）

两者之间的 `nand_dispatch_request_t` 就是行业 FCC 内部的"已解析请求"寄存器组：

```c
/* include/nand/nand_dispatch.h —— 形状与行业 FCC 的请求寄存器一致 */
uint32_t  cw_bitmap;               /* 访问哪些 CW（flat 绝对号；v27 起字面位图，0 = 空集） */
nand_paa_t plane_paa[PLANE_NUM];   /* 每个 plane 的行地址 */
uint8_t  *cw_buffers[CW_TOTAL_MAX];/* 每个 CW 的数据缓冲指针 */
```

**下界**（`include/nand/nand_dispatch.h:22-24`）：`src/fc/` 必须在 submit 时把 desc 链解析完并交出
`cw_buffers[]`，`nand_dispatch` 之后**不再回调 FC**。故"造请求者必须拥有 buffer 池"——
FC 不能整体下沉到 FTL（这条约束排除了"FC 只留三池、其余全给 FTL"的方案）。

### 26.3 判据：硬件 FCC 寄存器表

> 拿 FC 的每一个对外函数去比对**硬件 FCC 的寄存器表**——**有对应项则保留，无对应项则剔除。**

按此判据过一遍 FC 的全部对外接口（v16 后的 21 个）：

| 对外函数 | 硬件 FCC 的对应物 | 判定 |
|---|---|---|
| `fc_init` | 复位 / 配置寄存器 | 保留 |
| `fc_command_alloc` / `fc_command_release` | 命令槽取 / 还 | 保留 |
| `fc_command_configure_descriptor` | 填一条描述符 | 保留（**唯一建链入口**；v17 前名 `fc_command_attach_cw`） |
| `fc_descriptor_chain_release_head` / `fc_descriptor_chain_release_all` | 描述符链释放（下界强制 FC 持有——链在 FC 的池里） | 保留（v16 新增，建链失败的回滚入口） |
| `fc_submit` | 写门铃 | 保留 |
| `fc_service` | 控制器自跑（软件里必须显式化） | 保留 |
| `fc_poll_completion` | 读 CQ | 保留 |
| `fc_buffer_alloc` / `release` / `id_to_real_buffer` / `get_base` / `get_meta` | 数据缓冲（下界强制 FC 持有） | 保留 |
| `fc_get_cmd_data_ptr` | 读第 n 条描述符取缓冲地址 | 保留 |
| `fc_get_cmd_cw_bitmap` | 读描述符链（v13 新增，仅供 RAID 断言；v13~v26 名 `fc_get_cmd_cw_mask`） | 保留 |
| `fc_get_cmd_ecc_max` | 完成聚合寄存器（ECC 纠错计数） | 保留 |
| `fc_command_id_to_command` / `fc_command_to_id` / `fc_descriptor_id_to_descriptor` | ID↔指针（软件模拟必需；ID 由指针派生，见 §2.2） | 保留 |
| `fc_get_resource_stats` | 状态寄存器 | 保留 |
| ~~`fc_command_configure_descriptors`~~ | **无此寄存器**（描述符链是固件填的） | **v13 上移 FTL** |
| ~~`fc_write_cmd_data` / `fc_read_cmd_data`~~ | **无此寄存器**（数据是固件 DMA 搬的） | **v13 上移 FTL** |

### 26.4 为什么这两件事在固件侧

硬件 FCC 拿到的是"已经决定好访问哪些 CW、数据已经在哪个缓冲里"的描述符链——
它**不解释掩码**（掩码是固件的策略产物），也**不搬运数据**（数据搬运是 DMA 引擎按描述符里的地址做的，
"往哪个缓冲拷、按什么顺序拷"由固件决定）。OCSSD / ZNS 这类"设备侧极简 FTL"的设计方向也印证同一点：
**越靠近硬件，接口越只谈机制；策略越往上放。**

### 26.5 反例：为什么不按"6 原语"重构

曾考虑过把 FC 收敛成"申请/释放命令、申请/释放 desc、申请/释放 buffer"六个裸池原语。
按 §26.3 判据这是**反向**的：行业 FCC 是"队列 + 门铃 + 完成"模型，
裸池原语会把 FC 变成内存分配器，反而**偏离**硬件形态。故不做。

---

## 附录 A：图 → 源码 grep 对照

| 图 | 内容 | 定位方法 |
|----|------|---------|
| 图 0 | 分层架构与数据流总览 | `grep -rn "nand_dispatch_submit" src/fc/fc.c` |
| 图 1 | 命令生命周期 | `grep -n "fc_command_alloc\|fc_submit\|fc_command_release" include/fc/fc.h` |
| 图 2 | fc_service() 主循环 | `grep -n "fc_poll_internal\|fc_poll_dispatch_\|fc_build_dispatch_request" src/fc/fc.c` |
| 图 3 | CW 布局与数据处理范围 | `grep -n "FC_BUFFER_USER_OFFSET\|fc_buffer_get_base" include/fc/fc.h src/fc/fc.c` |
| 图 4 | 写路径编码（Page Pack） | `grep -n "fc_data_path_encode" src/fc/fc_data_path.c` |
| 图 5 | 读路径解码（Page Unpack） | `grep -n "fc_data_path_process_read\|fc_data_path_raid_recover" src/fc/fc_data_path.c` |
| 图 6 | nand_engine 微码状态机 | `grep -n "nand_engine_step" src/nand/nand_engine.c` |
| 图 7 | 70h 状态轮询循环 | `grep -n "JUMP_IF_NOT_READY" src/nand/onfi_1_0.c` |
| 图 8 | FMC 后端 UOP 执行 | `grep -n "g_nand_fmc_handlers" src/nand/nand_fmc.c` |
| 图 9 | nand_dispatch poll 遍历模型（per-LUN FIFO） | `grep -n "nand_dispatch_poll\|nand_dispatch_lun_step" src/nand/nand_dispatch.c` |
| 图 10 | LUN 相位机（Phase V） | `grep -n "LUN_PHASE_" src/nand/nand_dispatch_internal.h src/nand/nand_dispatch.c` |
| 图 11 | 调度器填充 pass（fc_poll_fill） | `grep -n "fc_poll_fill\|nand_dispatch_lun_free" src/fc/fc.c src/nand/nand_dispatch.c` |
| 图 12 | Stripe RAID 写提交 | `grep -n "fc_stripe_submit" src/fc/fc.c` |
| 图 13 | 完成回写与 CQ 组装 | `grep -n "fc_complete_from_nand" src/fc/fc.c` |
| 图 14 | 上电初始化分层调用时序 | `grep -n "fc_init\|nand_init\|ssd_ftl_boot_all" src/main.c` |
| 图 15 | FC 控制面资源装配 | `grep -n "controller->command_pool\|controller->sq\|nand_dispatch_init" src/fc/fc.c src/nand/nand_dispatch.c` |
| 图 16 | NAND 芯片初始化 | `grep -n "HAL_NAND_Init\|nand_fmc_init\|nand_fc_reset\|nand_fc_read_id" src/nand/nand_mt29f4g08abadawp.c` |
| 图 17 | 一条命令的端到端生命周期 | `grep -n "fc_poll_dispatch_single\|nand_dispatch_submit\|fc_complete_from_nand" src/fc/fc.c src/nand/nand_dispatch.c` |
| 图 18 | 命令生命周期状态机 | `grep -n "FC_COMMAND_STATE_\|fc_command_alloc_internal\|fc_complete_internal" include/fc/fc_internal.h src/fc/fc.c` |
| 图 19 | fc_service() 单步全景 | `grep -n "fc_poll_internal\|fc_poll_fail_command" src/fc/fc.c` |
| 图 20 | nand_dispatch 相位机状态机 | `grep -n "LUN_PHASE_\|nand_dispatch_issued_advance\|nand_operation_is_issue" src/nand/nand_dispatch_internal.h src/nand/nand_dispatch.c` |
| 图 21 | 多 LUN ISSUE/WAIT/FETCH 时序波形 | `grep -n "wait_start_cycles\|min_busy_us\|delay_dwt_cycles_now" src/nand/nand_dispatch.c src/nand/nand_dispatch_tables.c` |
| 图 22 | 调度器填充 pass 决策 | `grep -n "fc_poll_fill\|fc_poll_chain_length\|chain_owner\|nand_dispatch_lun_free" src/fc/fc.c` |

## 附录 B：建议阅读顺序

```
第一步（整体）  图 0 → 图 1 → 图 2
第二步（数据）  图 3 → 图 4 → 图 5
第三步（硬件）  图 6 → 图 7 → 图 8
第四步（并发）  图 9 → 图 10 → 图 11
第五步（进阶）  图 12 → 图 13
第六步（初始化）图 14 → 图 15 → 图 16        ← 先看「系统怎么起来」（§22）
第七步（运行时）图 17 → 图 18 → 图 19 → 图 20 → 图 21 → 图 22   ← 再看「命令怎么跑完」（§23）
```
