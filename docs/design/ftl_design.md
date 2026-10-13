# FTL 层完整设计方案

> 本文档记录 soft_ssd 项目中 FTL（Flash Translation Layer）模块的完整设计，包括架构选型、子模块设计、行业对比、接口定义和实现规划。
>
> 本文档与 `fc_nand_design.md`（FC + NAND 底层）平行，二者共同构成 SSD Firmware 的完整设计方案。

---

## 变更历史

| 版本 | 日期 | 变更内容 | 作者 |
|------|------|---------|------|
| v0.1 | 2026-09-21 | 初始框架：页级 L2P + 顺序分配 | Claude |
| v0.2 | 2026-09-23 | 完整设计：DRAM-less L2P cache、Region-based caching、CLOCK、Journal、Checkpoint | Claude |
| v0.3 | 2026-09-23 | 架构决策记录、行业对比、Block 分类、Block 生命周期图、FTL 接口说明 | Claude |
| v0.4 | 2026-09-23 | Write-back 四级触发条件 + 企业级 flush 对比 + 6 个补充设计要点 | Claude |
| v0.5 | 2026-09-23 | GC 多维度加权评分 + 六级触发 + 原子性保证（Journal/GC Bitmap）+ 企业级参考；Block 分类重写（拓扑/Super Block/温度/OP/Pre-conditioned）；Phase 规划扩展（P2.8~P2.11）；Allocator 接口增加温度参数 | Claude |
| v0.6 | 2026-09-23 | ftl_service 主循环调度设计：三级模式（IDLE/NORMAL/BUSY）+ 企业级后台调度器参考 + 主循环完整形态 | Claude |
| v0.7 | 2026-09-23 | Journal 自身可靠性设计（四层保护：ECC/CRC/entry_count/sequence）+ Journal Page 完整格式；BBT 完整设计（出厂扫描 + 运行时隔离 + Block Skip 替换 + 企业级参考） | Claude |
| v0.9 | 2026-09-24 | Super Block 坏块处理升级：Block Skip → Block Sparing + Spare Pool；"两层映射"方案废弃，回归行业标准物理 PAA 直映射（§3.8.3/ADR-11/§9/§15 联动更新） | Claude |
| v0.10 | 2026-09-24 | 新增静态磨损均衡（Static Wear Leveling）完整设计：三星企业级独立 Daemon 方案，三条件触发、冷 Block 选择算法、数据搬移流程、ftl_service 集成（§4.8 新增） | Claude |
| v0.11 | 2026-09-24 | Journal 回收触发条件重写：三级触发（水位线 85%/95% + 写入量 4096 + 定时 10s），基于企业级"预算推导"法，配套"无阻塞原则"（§7.5 重写） | Claude |
| v0.12 | 2026-09-24 | L2P 表磨损均衡定量分析（§14 U5 已解决+详细推导表）；§14 未解决问题列表更新（U2/U3/U4/U5 已解决，新增 §14.1 完整分析小节） | Claude |
| v0.13 | 2026-09-24 | 元数据区从固定 32 blocks 改为动态分配：新增 Metadata Directory + Anchor Block 设计（§3.3/§5.5）；Region 写回双 Plane 交错 Multi-Plane 加速（§5.6）；§14 U1/U6 已解决 | Claude |
| v0.14 | 2026-09-28 | FTL 接口层决策确认：LBA 粒度 512B、lba_t(32bit)/pba_t(32bit) 地址体系（LBA→PBA→PAA）、几何统一到 nand_config.h、异步提交+ftl_service 驱动完成、新增 ftl_fc_interface.h 命令构造 helper 层 | Claude |
| v0.15 | 2026-09-28 | 地址体系全面定稿：LAA/LBA/PBA/PAA 行业标准定义、cmd->meta 代替 cmd->faa、PBA=[CH][CE][PAA] bitfield、ftl_cmd_setup_ppa/ftl_cmd_write_data/ftl_cmd_read_data 三函数设计 | Claude |
| v0.16 | 2026-09-28 | Step 1~5 完整实现：PBA/PAA 全套 inline、ftl_fc_interface 桩、L2P Cache 纯 SRAM+CLOCK、Allocator+Super Block、Journal 固定 Block、GC+ftl_service 三级主循环 | Claude |
| v0.17 | 2026-09-29 | FTL 全面打通 NAND 路径：17 个核心桩全部替换为真实 FC 调用；新增同步 NAND 操作层（ftl_internal_read/write/erase）；新增 Pending Request 表；新增 ftl_read/write/trim 同步包装；GC 完整 7 步搬移（读 META→查 L2P→分配 Cold→写→L2P→Journal）；Metadata Directory 完整实现（Anchor Block 33-34 + 动态分配 + CRC 保护）；Allocator 全部桩消除（pre_cond/needs_gc/refill 同步）；controller 层 3 个隐式声明消除 | Claude |
| v0.18 | 2026-09-29 | FTL 全部剩余模块实现：Checkpoint、BBT、Read Recovery、Power Failure Recovery、Wear Leveling；代码审视修复4个逻辑断裂 | Claude |
| v0.19 | 2026-09-29 | Completion Table 统一完成路由：消除 ftl_service Phase2 和 ftl_internal_sync_wait 两个"泵"互相吞 completion 的 bug；新增 completion_table.h/c（16 项路由表【v0.31 扩为 32】+ 同步等待器 + 统一泵）；ftl_internal_sync_wait 改为注册 sync_waiter 后走 completion_table_drain，不再独占 fc_poll_completion；L2P Region 动态分配接入：cache 增加 alloc 指针，flush_one 中首次写 Region 改为真实调用 metadata_dir_alloc_region（取消回退固定位置） | Claude |
| v0.20 | 2026-09-29 | 返回值体系统一 + 历史死代码清理：新增 ftl_status_t{FTL_OK=0,FTL_ERROR=-1} 企业级风格枚举，~32 个操作函数从 int 改为 ftl_status_t，谓词/计数函数保持 int；修复 checkpoint_find_latest 因 FTL_OK=0 导致的调用反转 bug；ftl_cmd_setup_ppa 等 3 函数从 (int) 强转改为显式 FC_STATUS_OK 映射；删除历史遗留文件 nand.h/c、nand_geometry.h/c、config/nand_geometry.h、config/ftl_config.h、__patch*.txt；删除死代码 pba_t、wl_init()、allocator_refill_pre_cond()；wl_relocate 复用 gc_read_page_with_lba + gc_erase_and_free（~60 行重复代码消除）；ssd_types.h 清除 ssd_ppa_t；l2p_cache.h 显式 include allocator.h 消除隐式依赖 | Claude |
| v0.21 | 2026-09-29 | FTL 拓扑信息重构：删除 ftl_context_t/allocator_t/journal_t 中的 ch/ce/lun 散落字段，统一为 ftl_topology_t 结构体；ftl_init 新增 ftl_topology_t 参数替代原 ch/ce/lun 3 个参数；全部 pba_make 的 CH/CE 源改为从 topology 或 alloc->ch/ce 获取，消除硬编码 0,0；allocator/journal/metadata/checkpoint/bbt/power_failure_recovery/wear_leveling 各模块全部接入拓扑路由 | Claude |
| v0.22 | 2026-09-29 | FTL 对外接口异步化：删除 ftl_read/ftl_write 同步包装，FTL 对外只提供非阻塞接口；ftl_submit_write 新增 complete_flag 参数跟踪 NAND PROGRAM 完成；Controller 层改为纯异步提交 + complete_flag 轮询；新增 ssd_controller_is_complete()；ssd_request_t.completion_status 改为 ssd_status_t | Claude |
| v0.23 | 2026-09-29 | D4 多 LBA 拆分实现（方案 A — Controller 层拆分）：ftl_submit_read/write 去掉 count 参数（只支持单页）；pending 表新增 on_complete 回调+ctx 字段；request.c 实现 ssd_request_split 循环（block_count 次→N 个单页子请求）；on_sub_complete 递减 sub_remaining 归零时置父请求 complete_flag；单块读也传 complete_flag 正常发完成信号；PBA 无效路径也发回调/信号 | Claude |
| v0.24 | 2026-09-29 | D5 写失败回滚（modify-after-confirm + BBT 两阶段 + 自动回滚）：ftl_submit_write 中 journal_append/l2p_cache_write 后移到 completion handler 的成功确认路径；pending 表加 old_pba/new_pba/ftl 字段；写失败→bbt_mark_bad RAM only + background bbt_flush；L2P 不动=自动回滚到旧数据；Super Block 空洞由 GC 自然跳过 | Claude |
| v0.31 | 2026-09-30 | **Pending 表 + Completion Table 扩容**：`FTL_PENDING_MAX` 8→32、`COMPLETION_TABLE_SIZE` 16→32（多 LUN 并发软件前置）；纯常量改动，表为线性扫描数组无位域/mask 依赖；同步背压预算（§17.5）定量数字一并更新 | Claude |
| v0.25 | 2026-09-29 | D6 队列深度/背压确认（pending 表满即隐式背压，DRAM-less 消费级不需额外机制）：SQ 深度 64 >> 实际并发（pending=8→v0.31 扩为 32），pending 表满→FTL_ERROR→Controller→Host 重试，已自然构成流控 | Claude |
| v0.26 | 2026-09-29 | D7 多命令流水线实现：修正 D4——ftl_submit_read/write 加回 pages 参数（1=单 Plane, 2=Multi-Plane）；Super Block 双 Plane 同步写入（OP_PROGRAM_PAGE_MULTI_PLANE, multi_plane=2）；读路径区分顺序/随机（两两连续 LBA 配对 Multi-Plane）；Cache Program 安全中间页标志 cache_intermediate 跳过 L2P/Journal 确认 | Claude |
| v0.27 | 2026-09-29 | D8 错误通知粒度（消费级 DRAM-less）：pending 表加 status_out 指针，completion handler 写入 SSD_STATUS_OK/IO_ERROR；ftl_submit_read/write 签名加 status_out 参数；Controller 层传 &request->completion_status 接收异步完成状态 | Claude |
| v0.28 | 2026-09-30 | 地址体系双层化 + 形参优化 Step 1/2：新增 nand_addr_t（展开地址）与 pba_from_addr/addr_from_pba/addr_from_paa 双向转换，遍历路径不再 pba_make 打包→拆包；super_block_t 加 ftl_topology_t topo + super_block_page_addr；ftl_fc_interface.h 新增 addr_to_cmd 直填；删除 ftl_internal_read/write_page/erase_block/read_page_raw 四个包装（-232 行），各模块内联为 static sync 助手；新建 ftl_types.h（ftl_io_request_t/ftl_init_params_t 供 Step 3/4）；随机 IO 路径 ftl_cmd_setup_ppa 保留 | Claude |
| v0.29 | 2026-09-30 | 形参优化 Step 3：ftl_submit_read/write 改 (ftl, req) 结构体传参（8 形参→2，ftl_io_request_t）；ftl_pending_req_t 按值嵌入 ftl_io_request_t io（pending_add 12 形参→6）；pending_cb_t 从 pending.h 迁至 ftl_types.h 打破循环 include；ssd_request_t 增加 ftl_io_request_t io 复用模板（单块直填，拆分循环内只改 lba/buffer/pages） | Claude |
| v0.30 | 2026-09-30 | 形参优化 Step 4：ftl_init / allocator_init 改 ftl_init_params_t 结构体指针传参（7/8 形参→2）；ftl_init_params_t 定义于 allocator.h（依赖 super_block_t，避免 ftl_types.h↔allocator.h 循环 include）；ftl_types.h 收敛为 pending_cb_t + ftl_io_request_t | Claude |
| v0.31 | 2026-09-30 | **P-A 多 LUN 并发前置**：Pending 表 + Completion Table 扩容（8→32）；元数据 lun 泛化；ECC 返回链路打通（fc_ldpc_decode err_bits）；read recovery 接入（rr_block_track 空转待供电） | Claude |
| v0.32 | 2026-09-30 | **Block State Table（BST）统一 per-block 结构体数组**：新建 ftl/bst.h；合并 erase_counts/bad_blocks/g_bbt_bitmap/rr_block_track 四张分散表为 ftl_block_t[4096] 单数组（56B/项=224KB 全常驻 SRAM）；修复 3 处全局/per-plane 块号碰撞 bug（ftl_gblock 单点转换）；read recovery 由空转变为供电；逐页有效位图 valid_map + VPC 全真值维护（写/覆盖/GC迁移/擦除/trim/坏块/掉电七点闭环）；GC/WL 迁移扫位图免 NAND 读（P-C.5 落地）；行业全字段补全（状态机/温度/坏块原因/read&program 计数/last_pe_ts/gc_generation/sb_id 反向索引）；PFR 掉电 closed_sb_list 重建留二期 | Claude |

---

## 目录

1. [架构选型与行业背景](#1-架构选型与行业背景)
2. [NAND 几何与资源预算](#2-nand-几何与资源预算)
3. [NAND Block 分类](#3-nand-block-分类)
4. [Block 生命周期](#4-block-生命周期)
5. [L2P 映射表设计](#5-l2p-映射表设计)
6. [SRAM Cache 设计](#6-sram-cache-设计)
7. [Journal 与掉电恢复](#7-journal-与掉电恢复)
8. [Checkpoint 机制](#8-checkpoint-机制)
9. [架构决策记录](#9-架构决策记录)
10. [企业级实现对比](#10-企业级实现对比)
11. [FTL 对外接口](#11-ftl-对外接口)
12. [ftl_service 主循环调度](#12-ftl_service-主循环调度)
13. [与 FC 层接口约定](#13-与-fc-层接口约定)
14. [未解决问题](#14-未解决问题)
15. [设计总结](#15-设计总结)
16. [Phase 规划](#16-phase-规划)
17. [D6 队列深度/背压方案详细分析](#17-d6-队列深度背压方案详细分析)
18. [D7 多命令流水线方案详细分析](#18-d7-多命令流水线方案详细分析)
19. [Block State Table 产品化裁剪路径](#19-block-state-table-产品化裁剪路径档三sram-核心--nand-扩展)
20. [附录：核心数据结构原型](#20-附录核心数据结构原型)

---

## 1. 架构选型与行业背景

### 1.1 三种映射策略对比

| 维度 | 块级映射 (Block Mapping) | 混合映射 (Hybrid BAST/FAST) | **纯页级映射 (Page Mapping)** ✅ |
|------|-------------------------|----------------------------|-------------------------------|
| 原理 | LBA → Block# + 固定页偏移 | 日志块(页级) + 数据块(块级) | LBA → Page/CW# |
| 表大小 | ~16 KB / 512MiB | 几百 KB | **4 MB / 512MiB** |
| 写放大 | 5~10x ❌ | 2~3x | **1.0~1.3x** ✅ |
| 灵活性 | 最低 | 中 | **最高** |
| GC 复杂度 | 简单（整块操作） | 中（merge 策略复杂） | **高（逐 CW 搬移）** |
| 实现复杂度 | 最简单 | **最复杂** | 中等 |
| 主流应用 | IoT / 极低端控制器 | 已淘汰（2012 年前 U 盘） | **所有现代 SSD** ✅ |

### 1.2 行业现状

```
SSD 类型                      映射策略                原因
─────────────────────────────────────────────────────────────────
企业级（Samsung PM9A3、       ✅ 纯页级映射（4KB 粒度）   性能/WA/延迟，几 GB DRAM 不差表空间
 Intel DC P5800X）
消费级旗舰（990 PRO、         ✅ 纯页级映射（4KB 粒度）   1~2GB DRAM，全表常驻
 SN850X、KC3000）
消费级入门（SN580、           ✅ 纯页级映射（4KB 粒度）   HMB 用主机内存做 cache，还是页级
 NV2）
UFS 4.0（手机）               ✅ 纯页级映射（4KB 粒度）   L2P table on NAND + SRAM cache
─────────────────────────────────────────────────────────────────
块映射 / 混合映射              ❌ 已经不用                2010 年前技术
```

**结论**：纯页级映射是绝对主流。

### 1.3 混合映射（BAST/FAST）为何被淘汰

BAST（Block Associative Sector Translation）和 FAST（Fully Associative Sector Translation）是 2007~2012 年的技术，核心原因是：

| 变化 | 影响 |
|------|------|
| NAND Page 增大到 16~32 KB | 表大小相对缩小（同样容量的 entry 更少）|
| 控制器 SRAM 增大到 1~2 MB | 能放下更大的 cache |
| DRAM 成本暴跌 | 入门级也可配 256MB~1GB DRAM |
| 无 DRAM 方案用 HMB | 借用主机内存，空间管够 |
| QLC/TLC P/E 寿命有限 | 混合映射的 merge 产生大量额外写入 |

混合映射要解决的（表空间不足），已被硬件成本下降解决。块映射的 WA 在 QLC/TLC 时代更是致命。

### 1.4 当前设计选择

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 映射策略 | **纯 CW 级（512B）页映射** | 与 FC 完全对齐，行业标准做法 |
| L2P 存储 | **NAND 元数据 Block（DRAM-less）** | 学习嵌入式/UFS 主流方案 |
| L2P 缓存 | **SRAM region cache (64~256 KB)** | 1MB SRAM 可支持 |
| 替换策略 | **CLOCK** | LRU 命中率，O(1) 开销 |
| 写回策略 | **Write-back + Journal** | 性能与掉电安全的折中 |
| FTL → NAND | **通过 FC 接口** | 经过 CRC/ECC/Random 完整数据保护 |
| RAID | **不实现** | 复杂度高，当前 SLC 暂不需要 |
| Super Block | **跨双 Plane** | 学习企业级跨 Die 条带化，当前双 Plane 配对 |
| 温度分区 | **Hot / Cold 双 Open Super Block** | 提升 GC 效率，冷热分离 |
| OP | **10% 预留（可配置 7~28%）** | 性能保速 + GC 缓冲 |
| Free Pool | **一个已擦除池 + 三级水位**（v0.52 更正，见 §3.7） | 学习 Background GC 和前台 IO 隔离 |
| Bad Block | **Block Sparing（Spare Pool 保水位）** | 学习企业级坏块替换，不做额外映射层 |
| SLC Cache | **不实现** | 本身就是 SLC NAND |

---

## 2. NAND 几何与资源预算

### 2.1 NAND 几何

| 参数 | 值 |
|------|------|
| NAND 型号 | MT29F4G08ABADAWP（SLC） |
| Page main | 2048 B |
| Page spare | 64 B |
| CW 布局 | [META:4][CRC:4][USER:512][PARITY:8] = 528 B |
| CW/Page | 4 |
| Pages/Block | 64 |
| Blocks/Plane | 2048 |
| Planes | 2 |
| 总 Pages | 262,144 |
| 总用户容量 | 512 MiB |

### 2.2 映射粒度关系

```
你的 SLC 2048B page:                  TLC 16KB page:
┌──────┬──────┬──────┬──────┐         ┌──────────┬──────────┬──────────┬──────────┐
│ CW0  │ CW1  │ CW2  │ CW3  │         │ CW0      │ CW1      │ CW2      │ CW3      │
│ 512B │ 512B │ 512B │ 512B │         │ 4KB      │ 4KB      │ 4KB      │ 4KB      │
└──────┴──────┴──────┴──────┘         └──────────┴──────────┴──────────┴──────────┘
映射粒度 = 512B = 1 CW              映射粒度 = 4KB = 1 CW
1 Page = 4 LBA entries              1 Page = 4 LBA entries
总 LBA = 1,048,576                  总 LBA = (容量 / 4KB)
```

**本质**：结构一致，只是 CW USER 区大小不同。ECC 引擎的编解码单位（CW）决定了映射粒度。

### 2.3 SRAM 预算

> **v0.32 更新**：Block State Table（BST）统一 per-block 承载后，FTL 元数据从 ~45KB 增至 ~220KB。
> **v0.55/v0.56/v0.60 后**：BST 条目 **20B × 4096 = 80KB**（v0.55 把逐页有效位图搬出为独立表；
> v0.56 删 3 个死字段 24B → 20B；**v0.60 删掉独立位图表 64KB**——有效性切 **DRAM-less
> approach ③**：逐 CW 读**页内 META（=OOB 反向映射）**取回 LBA 反查 L2P，见 §4.9.7/§4.9.8 与
> `cw_level_mapping_design.md §八`）。⇒ BST 常驻 = **80KB**（无位图表）。
> DRAM-less 产品化裁剪路径（档三：核心 SRAM + 扩展 NAND）见 §19。
>
> ⚠ **下表已改为 AXI_RAM 实测**（真实约束是 AXI_RAM 384 KB，由 `src/main.c` 的编译期守卫
> `sizeof(g_ftl) + sizeof(g_blocks) + 20KB ≤ __AXI_RAM_SIZE(0x60000)` 把住）。
> 数字取自 **debug 构建的 `debug.map`（v0.63 工作树）**，按符号地址差算得（`g_ftl` = 0x26BD0
> = 154.95 KB）。**旧的「总计 1 MB」粗估表已废弃**——它把
> 「1 MB 全片 SRAM」当预算，而实际 FTL 元数据**只落 AXI_RAM**，其余大数组落 SRAM2。

| 占用方（AXI_RAM） | 大小 | 占 AXI_RAM |
|--------|------|------|
| `g_ftl`（L2P Cache 128KB + Journal/MD/Pending/allocator 结构） | 155.0 KB | 40.4% |
| `g_blocks`（BST 条目，20B × 4096） | 80 KB | 20.8% |
| FC 命令池/描述符池/缓冲池 + 其他 .bss/.data | ~19.7 KB | 5.1% |
| **小计** | **254.7 KB** | **66.3%** |
| 余量 | 129.3 KB | 33.7% |
| **AXI_RAM 容量** | **384 KB** | **100%** |

> 另有 4 张大数组刻意落 **SRAM2**（`.sram2_data`，NOLOAD）：`g_free_blocks` / `g_op_blocks` /
> `g_spare_blocks` / `g_closed_sb_list`（16 KB）——SRAM1 仅 128 KB 放不下，见 `src/main.c` 头注释。

### 2.4 SDRAM（8MB）用途

| 用途 | 大小 | 说明 |
|------|------|------|
| GC 搬运缓冲 | 256 KB | 读/写各 128 KB |
| Host NVMe 命令缓冲区 | ~1 MB | PRP/SGL 数据传输 |
| FTL 元数据暂存 | 32 KB | 持久化到 NAND 前暂存 |
| 调试日志、性能计数器 | ~6.5 MB | 剩余空间 |
| **总计** | **8 MB** | |

> SDRAM 不参与 L2P 缓存——DRAM-less 设计。

---

## 3. NAND Block 分类

### 3.1 拓扑结构

整个 FTL 设计以 `[ch][ce][lun]` 为拓扑维度，不假设任何维度为 1。

```c
/* nand_config.h 中定义的物理拓扑 */
#define CH_NUM      (1)     // 通道数
#define CE_NUM      (1)     // 每通道片选数
#define LUN_NUM     (1)     // 每片选 LUN 数
#define PLANE_NUM   (2)     // 每 LUN Plane 数
```

所有全局数据结构按此维度声明：

```c
ftl_lun_allocator_t g_allocator[CH_NUM][CE_NUM][LUN_NUM];

for (ch = 0; ch < CH_NUM; ch++)
    for (ce = 0; ce < CE_NUM; ce++)
        for (lun = 0; lun < LUN_NUM; lun++)
            process_lun(ch, ce, lun);
```

### 3.2 每 LUN Block 分类

```
每 LUN 的 NAND 物理空间（当前 2 Plane × 2048 Block = 4096 Block / LUN）

┌──────────────────────────────────────────────────────────────┐
│  Metadata Blocks（元数据区，固定块号）  每 LUN 独立一份       │
│  ├── L2P Table (16 blocks, 0..15)   哨兵层（页永不写入）      │
│  ├── Anchor (32 blocks, 16..47)     Metadata Directory 池    │
│  ├── Checkpoint (8 blocks, 48..55)  L2P 快照池               │
│  ├── BBT (2 blocks, 56..57)         坏块位图池               │
│  ├── Journal (4 blocks, 58..61)     掉电恢复日志（= CW_NUM）  │
│  └── Wear (1 block, 62)             擦除计数表（P0-1）        │
│      元数据区末尾 = 63 = FTL_METADATA_BLOCK_END              │
│                                                              │
│  User Data Area（数据区）      跨 Plane Super Block 管理    │
│  ├── OP Blocks (204 块 ≈ 5%)  预留空间（不映射给用户）       │
│  ├── Open Super Block         当前写入位置（跨双 Plane）     │
│  │     ├── Plane 0: Block N                                │
│  │     └── Plane 1: Block M                                │
│  ├── Closed Super Blocks      已写满，有效+无效页混合        │
│  └── Free Blocks              已擦除待分配（一个池，三级水位）│
│                                                              │
│  Bad Blocks（坏块）            BBT 标记                      │
└──────────────────────────────────────────────────────────────┘
```

### 3.3 元数据区（Metadata Blocks）— 动态分配

传统消费级设计使用固定元数据区，但企业级（Samsung/Solidigm）使用**动态分配**：
元数据和用户数据共享 Free Pool，通过 Metadata Directory 追踪元数据位置。

```
Anchor 池（固定块号 16..47，出厂好块，32 块）:
  一条 record = 8 页 = 一份完整 Metadata Directory（2048 条 × 7B）
  Directory 记录每份元数据（L2P region）当前的物理位置
  写满一块 ⇒ 擦池内下一块续写（块轮转，非"同块双份冗余"）

元数据（**只有 L2P region 日志块**从 Free Pool 动态取；其余是固定块池）:
  ┌──────────────────────────────────────────────────┐
  │  L2P Region 日志块 ── 动态（allocator_alloc_reserved_block）│
  │  Anchor            ── 固定池 16..47                │
  │  Checkpoint        ── 固定池 48..55                │
  │  BBT               ── 固定池 56..57                │
  │  Journal           ── 固定池 58..61（环形 + 擦除）  │
  │  Wear              ── 固定块 62（单副本）           │
  └──────────────────────────────────────────────────┘
```

固定方案 vs 动态方案对比：

| 维度 | 固定块池 + 环内轮转（**实际实现**）| 全动态分配（企业级）|
|------|-----------------------------------|--------------------|
| 磨损分布 | 池内块轮转，均匀摊到池内 N 块 | 均匀分布到所有好块 |
| 坏块影响 | 需要 Reserved 替换 | 自动跳过，Free Pool 自然替换 |
| 元数据 GC | ⚠️ **必须回收**：池是有界的，写满一块要擦池内下一块 | 需要（旧位置回收）|
| 启动时间 | ⚠️ 扫池内各块末条 + 读胜者 1 份（Anchor = 8 页）| 只需读 Anchor 1 份 |
| 复杂度 | 中：池几何 + 游标恢复 | ⚠️ 需维护 Directory |

> ⚠️ **本表原先把「动态分配」一列标成 ✅ 全胜**，与事实相反，已于 v0.50 订正：
> ①「元数据 GC 不需要」只对**无界日志**成立——本项目用的是**有界池**（Anchor 32 块 /
> Checkpoint 8 块 / BBT 2 块），池满必须擦旧块（`metadata_ring_prepare_write`），
> 回收是必需项而非可选项；
> ②「启动时间只需读 Anchor 2 pages」是旧 2 块固定布局的数字，现 Anchor 一份 = 8 页，
> 且开机要扫池内每块的末条 record 来定位游标；
> ③「磨损均匀分布到所有好块」对**固定池**不成立——Anchor 每块擦一次需 ≈65,536 次
> 主机写（8 份/块），是数据块的 8 倍。本项目选 N=32 是为了**主控形态**（固定根 +
> 专用元数据块池），不是磨损优化的结果；磨损在本仓不是约束。
> 只有 L2P region 日志块走真正的**动态分配**（从 Free Pool 取整块，
> `allocator_alloc_reserved_block`），见 §3.4。

#### Anchor Block

```c
/* ⚠ v0.50 起 Anchor 不再是"2 个固定 Block + 同块 3 份冗余"，而是**块池内环内轮转**
 *   （一条 record = ANCHOR_PAGE_COUNT 页，写满一块擦池内下一块）。以下为当前值。 */
#define ANCHOR_PAGE_COUNT    ((L2P_REGIONS_PER_INSTANCE + ANCHOR_ENTRIES_PER_PAGE - 1U) / ANCHOR_ENTRIES_PER_PAGE) /* = 8 */
#define ANCHOR_POOL_BLOCKS   (32U)  /* 池内块数（磨损旋钮）*/
#define ANCHOR_RECORDS_PER_BLOCK (PAGES_PER_BLOCK / ANCHOR_PAGE_COUNT) /* = 8 */

#define METADATA_TYPE_L2P_REGION    (0U)
#define METADATA_TYPE_JOURNAL       (1U)
#define METADATA_TYPE_CHECKPOINT    (2U)
#define METADATA_TYPE_BBT           (3U)
#define METADATA_TYPE_ERASE_COUNT   (4U)

typedef struct {
    uint8_t    type;             // 元数据类型
    uint16_t   id;               // L2P region_id / 0 for others
    nand_paa_t paa;              // 物理位置
} metadata_entry_t;              // 见 metadata.h（paa 为位域，逐字段赋值而非整体比较）

/* 一份 Anchor record = ANCHOR_PAGE_COUNT(8) 页，每页 = 一个 metadata_anchor_page_t：
 *   magic + sequence + chunk_index + chunk_count + entry_count + entries[PER_PAGE]
 *   （无独立 crc：完整性由下层 FC per-CW CRC + ECC 保证，v0.62）
 * 整份 = 全部 2048 条 entry。启动扫池取"整份校验通过"中 sequence 最大者。 */
```

- Anchor 池出厂时**不需要**预留固定好块——池内块由 Free Pool 提供（`RESERVED` 位标识），
  坏块由分配器自然跳过
- 冗余方式改为**池内多份**（32 块 × 8 份/块 = 256 份），不是同块 3 页
- 序列号单调递增，启动时取最大 sequence 的**整份有效**记录（不取半份）

#### 元数据分配 API

> ⚠ **订正**：`allocator_alloc_metadata` / `allocator_select_block` / `paa_make` 均已不存在
> （`paa_make` 于 v25 删除，改用 `*_from_addr`）。**只有 L2P region 日志块**走动态分配，
> 用 `allocator_alloc_reserved_block` 独占取整块；Anchor/Checkpoint/BBT/Journal/Wear
> 都是**固定块池**（见 §3.2），不走这里。

```c
/* 独占分配一个整块给元数据（当前唯一调用者：L2P region 日志块）*/
ftl_status_t allocator_alloc_reserved_block(ftl_allocator_t *alloc,
                                            uint8_t plane,
                                            uint16_t *out_gblock);
```

分配时优先选低擦除次数的块。Anchor 池每块装 8 份 record（`ANCHOR_RECORDS_PER_BLOCK`），
每份 = 8 页；故一个 Anchor 块要 8 份 × 2048 条变更才写满 ⇒ 磨损极低。

#### 介质布局版本与开机自动格式化（v0.61）

元数据区的**块位置、记录格式**会随设计演进变化（L2P 块位置 / Journal 块数 / Wear 块 /
Anchor 分页 / CW 级映射 …）。若固件**不能识别介质是旧格式**，直接烧板会在旧数据上启动，
而 Free Pool 里全是**未擦的旧块**——allocator **不做擦除**（"free pool 保证已擦除" 是隐式
前提）⇒ 写下去 = 新数据 AND 旧数据 = **静默损坏**。此前每次上板都得先手动 `bbt --erase`。

v0.61 起由固件兜底：

```
#define FTL_MEDIA_LAYOUT_VERSION (2U)   /* 任何改介质格式的改动都必须 +1（v0.62 删元数据 CRC 后 +1）*/
```

- **标记位置**：`l2p_checkpoint_header_t.layout_version`（Checkpoint 头）**与**
  `metadata_anchor_page_t.layout_version`（Anchor 页）**都写**。两处独立校验
  （`checkpoint_header_valid` / `anchor_page_valid`），任一版本不符即判该记录作废。
- **触发判据**（唯一、自终止）：本实例 `checkpoint_find_latest(&cp, topo) != FTL_OK`
  ⇒ 需格式化。因版本校验已含在 `checkpoint_header_valid` 内，该判据自动涵盖三种情况：
  ① 有当前版本 Checkpoint（否）② 有但版本不符（是）③ 完全没有（是）。
- **执行**（`ftl_media_format`）：① `bbt_erase_all_blocks` 擦本实例全部块（**擦前先读
  出厂坏块标记** ⇒ **保留**）→ ② `bbt_save` 重新持久化坏块图 → ③ `metadata_dir_init`
  复位 Directory → ④ `checkpoint_write_initial` 写当前版本标记（使判据 ① 命中 ⇒ 只发生一次）。
- **钩子位置**：`main.c ssd_ftl_boot()` 内、**`ftl_init()` 之后、free-pool 回填之前**。
  放 `ftl_init` 之后的关键理由：`bbt_init` 已先把出厂坏块标记读进 BST ⇒ 再擦全片不丢坏块信息。
- **开机日志**：`[FTL ] media layout mismatch -> format (erase all blocks)`。
- **代价**：出厂空片首启会多擦一遍全片（一次性，约十余秒）；`erase_count` 不重零（仅影响
  磨损均衡起点，无害）。手动 `bbt --erase` CLI 保留作兜底。

### 3.4 Super Block（跨 Plane 条带化）

Super Block 是跨双 Plane 的逻辑写入单元。当前硬件 PLANE_NUM=2，双 Plane 各取一个物理 Block 配对。

```
Open Super Block（每个温度等级一个）:
  Hot:  Plane 0: Block A,  Plane 1: Block B  ← 用户 IO 写入
  Cold: Plane 0: Block C,  Plane 1: Block D  ← GC 搬移/元数据写入

写入以 Super Block 为粒度，双 Plane 并行（Multi-Plane PROGRAM）：
  Page 0: Plane0-Page0 + Plane1-Page0
  ...
  Page 63: Plane0-Page63 + Plane1-Page63 → Super Block 写满

写满后从 Open → Closed，等待 GC 回收。
```

```c
#define FTL_TEMP_HOT    0
#define FTL_TEMP_COLD   1
#define FTL_TEMP_NUM    2

typedef struct {
    ftl_topology_t topo;                  // 本 Super Block 绑定的 NAND 芯片路由（ch/ce/lun）
    uint16_t plane_blocks[PLANE_NUM];     // Plane 0 和 Plane 1 的 Block 号
    uint8_t  next_page;                   // 当前写入页（0~63，双 Plane 同步）
    uint8_t  temperature;                 // FTL_TEMP_HOT / FTL_TEMP_COLD
    uint8_t  valid;                       // 1=正在使用（Open），0=已关闭
} super_block_t;

/* 返回 Super Block 中指定 Plane/Page 的展开地址（ch/ce/lun 取自 sb->topo）*/
static inline nand_addr_t super_block_page_addr(const super_block_t *sb,
                                                uint8_t plane, uint8_t page)
{
    nand_addr_t addr;
    addr.ch     = sb->topo.ch;
    addr.ce     = sb->topo.ce;
    addr.lun    = sb->topo.lun;
    addr.block  = sb->plane_blocks[plane];
    addr.plane  = plane;
    addr.page   = page;
    addr.cw     = 0; /* 0 = 整页（页级操作基址） */
    return addr;
}

/* 兼容旧调用点（内部经展开地址转换）*/
static inline nand_pba_t super_block_page_pba(uint8_t ch, uint8_t ce,
                                               const super_block_t *sb,
                                               uint8_t plane, uint8_t page)
{
    nand_addr_t addr = super_block_page_addr(sb, plane, page);
    addr.ch = ch; addr.ce = ce;
    return pba_from_addr(&addr);
}
```

> **v0.28**：Super Block 携带拓扑（`sb->topo`），遍历路径用 `super_block_page_addr` +
> `addr_to_cmd` 直填 FC 命令，不再 `pba_make` 打包 → 拆包。随机 IO 路径仍走
> `ftl_cmd_setup_ppa(cmd, pba)`（PBA 已在手）。

### 3.5 温度分区（Multi-Stream）

为提升 GC 效率，数据按更新频率分温度写入不同的 Open Super Block。

```
Hot Super Block:   ← 频繁写入的数据（用户 IO 热数据）
  数据特征：更新频繁，无效页比例增长快
  GC 效率：高（80~90% 无效页）

Cold Super Block:  ← 几乎不更新的数据（GC 搬移、元数据）
  数据特征：几乎不变，无效页比例极低
  GC 效率：低（0~5% 无效页），但几乎不用回收
```

温度归类策略：

| 数据来源 | 温度 | 理由 |
|---------|------|------|
| 用户写入（Host IO） | HOT | 可能频繁更新 |
| GC 搬移的有效页 | COLD | 已至少存活一轮 GC |
| GC 搬移的热数据 | HOT | 仍可能频繁更新 |
| L2P/Journal/元数据 | COLD | 几乎不更新 |

### 3.6 OP 区域（Over-Provisioning 10%）与 Spare Pool（替换池）

OP 是数据区中**不映射给用户**的好块，由 allocator 统一管理但 L2P 不覆盖。

```c
#define FTL_OP_PERCENT    (10)    // 10% 预留空间，可调 7~28，设为 0 禁用
```

Spare Pool 是**独立于 OP 的替换储备**，专用于运行时坏块替换后补充 free pool 水位：

```c
#define FTL_SPARE_POOL_SIZE   (50)    // 替换池大小，覆盖生命周期新增坏块
```

#### OP 与 Spare Pool 的分工

| 池 | 用途 | 大小 | 是否可以回收 |
|----|------|------|------------|
| OP 10% | GC 缓冲 + 性能保速 | 204 blocks（= 每 plane 块数 2048 × 10% ≈ 总量 5%） | ✅ GC 正常使用 |
| Spare Pool | 坏块替换专用 | ~50 blocks | ❌ 只出不进，耗尽后容量下降 |

#### OP 在 RMA 和性能恢复中的作用

```
RMA 场景：
  出厂时：总好块 4056，Spare Pool ~50，OP ~400，用户可见 ~3600 块
  使用中：新增 50 个坏块 → Spare Pool 补充 → 用户容量不变 ✅
  Spare Pool 耗尽后新增坏块 → 用户容量自然减少

性能恢复场景：
  写入压力大时：OP 池提供 400+ Block GC 缓冲
  GC 有充足空间后台搬移 → 前台写入速度稳定 ✅
  没有 OP：free pool 耗尽 → 前台 IO 等 GC → 写入断崖下降 ❌
```

#### 运行时坏块替换流程

```
Super Block {P0-BlockA, P1-BlockB} 中 P0-BlockA PROGRAM 失败：

  ① 关闭这个 Super Block（不再写入新数据）
  
  ② 数据拯救：
     P0-BlockA 的已写页 → 能读就搬，不能读就丢
     P1-BlockB 的已写页 → 全部读到 buffer → 写入新 Super Block
  
  ③ 更新 L2P：
     被搬移的数据 → L2P 指向新的物理地址
  
  ④ 标记坏块：
     BBT 标记 P0-BlockA = BAD
  
  ⑤ 处理好的 Block：
     P1-BlockB 擦除 → 成功则回 free pool，失败也标记坏块
  
  ⑥ Spare Pool 补充：
     从 Spare Pool 取一个好块 → 加入 free pool（保容量水位）
  
  ⑦ 分配新的 Super Block：
     从 free pool 取双 Plane Block 配对
```

**重要**：运行时 Multi-Plane PROGRAM 中一个 Plane 的 Block 损坏，
**不需要将两个 Block 的编号对齐**——ONFI Multi-Plane 命令中
每个 Plane 有独立的行地址（含自己的 Block#），异号不影响并行操作。

### 3.7 Free Pool 三级水位（v0.52 更正）

> **v0.52 更正**：本节原题为「Pre-conditioned Pool（预擦除池 — 三级 Free Pool）」，把三级画成
> 三个**池**。这是错的——本仓（以及真实主控）都只有**一个**已擦除池，三级是**水位**。
> 旧描述还引入了 `pre_cond_count`/`emergency_count` 两个计数与两个状态位，
> 它们恒等于 `free_count` 或零引用，已在 v0.52 删除。

**为什么不能分区**：`gc_erase_and_free` 是**先擦后归还**——victim 擦完才进 Free Pool。
所以池内**每个**块都已是擦除态，不存在「预擦除块」与「普通 free 块」的物理区分，
也就没有东西可以分区。

**真实主控的做法**：free pool 是**一个已擦除池 + 软/硬水位**，另外配**独立的保留池**
（本仓即 §3.6 的 OP 池 = 性能缓冲、Spare 池 = 坏块替换）。没有任何主流主控在 free 池
内部再切「预擦除 / 未擦除」两级——**擦除落在关键路径上本身就是要避免的事**，
那才是 OP 池与后台擦除存在的理由。前台"零等待"靠的是水位（池里始终有 ≥32 个已擦除块），
不是靠第二个池。

```
                 free_count
   ┌───────────────────────────────────────────────┐
   │                                               │
   ▼                                               ▼
 0        8 (EMERGENCY)          32 (TARGET)      N
 │◄──③ 紧急──►│◄────② 后台 GC 补池────►│◄──① 正常──►│
   强制 GC，              IDLE/NORMAL 里            不做后台 GC
   前台写不再等           每次回收 1 个 SB
```

| 水位 | 条件 | 动作 | 触发位（`gc.h`） |
|---|---|---|---|
| ① 正常 | `free_count ≥ FREE_POOL_TARGET (32)` | 不做后台 GC | — |
| ② 补池 | `FREE_POOL_EMERGENCY (8) < free_count < 32` | 后台 GC（IDLE 循环 / NORMAL 每轮 1 个 SB） | `GC_TRIGGER_WATERMARK` (T1) |
| ③ 紧急 | `free_count ≤ FREE_POOL_EMERGENCY (8)` | 强制 GC，前台写不再等 | `GC_TRIGGER_EMERGENCY` (T4) |

```c
/* include/ftl/allocator.h —— 水位阈值（同一池上的两个分界） */
#define FREE_POOL_TARGET    (32U) /* 水位①/②分界 */
#define FREE_POOL_EMERGENCY (8U)  /* 水位②/③分界 */

/* src/ftl/ftl.c ftl_service_idle —— 水位②：把池补回 TARGET */
if (ftl->alloc.free_count < FREE_POOL_TARGET) {
    uint32_t target = FREE_POOL_TARGET - ftl->alloc.free_count;  /* 单位：块 */
    while (target > 0U) {
        if (!gc_select_victim(&ftl->alloc, &victim)) break;
        gc_migrate_sb(...); gc_erase_and_free(...);
        target -= PLANE_NUM;   /* 一个 SB 归还 PLANE_NUM 个块 */
    }
}
```

**水位之外的两级保留池**（真池，各自独立数组与计数，见 §3.6）：

| 池 | 计数 | 消费方 | 作用 |
|---|---|---|---|
| OP（204 块） | `op_count` | `allocator_pull_from_op`——Free Pool 给不出某 plane 的块时补 | 前台不等 GC 的性能缓冲 |
| Spare（50 块） | `spare_count` | `bbt.c` 坏块替换 | 坏块替换，只出不进 |

### 3.8 坏块管理（BBT）

坏块管理分三个阶段：出厂扫描 → 运行时隔离 → 替换机制。

#### 3.8.1 出厂坏块扫描

```
NAND 出厂时，每个 Block 的 Page 0 的 OOB（spare）区域标记该 Block 是否出厂坏块。

标准规则（JEDEC/ONFI）：
  非坏块：OOB[0] = 0xFF ✅
  出厂坏块：OOB[0] ≠ 0xFF ❌

⚠ **本仓偏离（登记 P2-3）**：FC 层**无独立 OOB/spare 读取接口**，spare 区
（64B/页）全空置 ⇒ **无法按标准读 OOB 标记**。替代判据见下。
```

**扫描流程（真实实现）**：

> ⚠ **订正**：真实 `bbt_scan_factory` 只读 **Page 0 一次**（不是 Page 0~1），
> 走 `ftl_sync_read_page_raw`（**LDPC_RAW**，跳过 ECC），`ret0 != 0` ⇒ 标坏块；
> 跳过 Block 0（出厂总是好块）。**无** `fc_read_oob`、**无** `bad_blocks[]` 数组、
> **无** `METADATA_BLOCK_COUNT` / `bbt_replace_metadata_block`——坏块统一记进
> BST（`ftl_block_set_bad`）。

```c
int bbt_scan_factory(ftl_allocator_t *alloc) {
    uint16_t bad_idx = 0;

    for (uint16_t gblock = 0; gblock < BBT_MAX_BLOCKS; gblock++) {
        uint8_t raw_buf[PAGE_SIZE];
        nand_addr_t page0;
        int ret0;

        if (gblock == 0) continue;            /* Block 0 出厂总是好块 */

        /* 全局块号 → (plane, block) 物理地址，读 Page 0 */
        page0 = addr_from_paa(alloc->ch, alloc->ce, paa_from_addr(&(nand_addr_t){
                    .plane = gblock / BLOCKS_PER_PLANE,
                    .block = gblock % BLOCKS_PER_PLANE }));

        memset(raw_buf, 0xFF, sizeof(raw_buf));
        ret0 = ftl_sync_read_page_raw(page0, raw_buf, FC_COMMAND_MODULE_BBT);

        /* 保守判据：RAW 读失败 ⇒ 标坏块（记进 BST，非独立数组）*/
        if (ret0 != 0) {
            ftl_block_set_bad(&alloc->blocks[gblock]);
            ftl_block_state_clr(&alloc->blocks[gblock],
                FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED | FTL_BLOCK_STATE_FREE);
            ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
            bad_idx++;
        }
    }
    alloc->bad_count = bad_idx;
    return bad_idx;
}
```

> ⚠ RAW 模式下即使 NAND 物理失败，`fc_poll_completion` 仍返回 READY（raw 无 ECC
> 错误）⇒ 用返回码判坏块**不一定可靠**。全 FF 也可能只是空块特征。**更准确的检测
> 需要 FC 提供 spare/OOB 读取接口**（= P2-3 偏离，见 `ftl_optimization_plan.md`）。

**扫描性能估算**：

```
每个 Block 读 1 页（Page 0），每次 ~25μs
总 Block ≈ 4096
总时间 ≈ 4096 × 1 × 25μs ≈ 102ms（+ 数据传输 ≈ 20ms）≈ 120ms
→ 在启动过程中可接受（放在 firmware_boot_stage_storage）
```

**坏块替换策略**：

```
元数据区坏块：
  → 元数据走固定块池（Anchor/Checkpoint/BBT/Journal/Wear 见 §3.2）——
     池内块由 Free Pool 提供并带 RESERVED 位，坏块由分配器自然跳过
  → 无独立 Reserved 块、无 bbt_replace_metadata_block

数据区坏块：
  → 记进 BST（FTL_BLOCK_STATE_BAD），allocator 选块时跳过
  → 不替换（坏块消耗的是 OP 池容量）
```

**坏块表存储格式**：

```c
#define BBT_MAGIC       0x424254AB      // "BBT\xAB"
#define BBT_MAX_BLOCKS  (BLOCKS_PER_PLANE * PLANE_NUM)  // 4096

typedef struct {
    uint32_t magic;                 // 魔数验证
    uint32_t sequence;              // 单调递增（最新有效）
    uint32_t total_blocks;          // 总 Block 数
    uint32_t bad_count;             // 坏块数
    uint32_t reserved[3];           // 保留
    uint8_t  bad_map[BBT_MAX_BLOCKS / 8];  // 4096 bit = 512B
} __attribute__((packed)) bbt_header_t;    // 540B（v0.62 删 crc 字段）
```

- 存储在 **BBT 池（块 56..57，2 块）**，一条 record = **1 页**（540B header + padding）
- 池内**块轮转**：块满 ⇒ 擦池内下一块续写（**先擦后写**），非"同块 3 份冗余"
- 每份带单调 `sequence`；读取取"整条校验通过"中 sequence 最大者
  （记录完整性由下层 FC 的 per-CW CRC + ECC 保证，v0.62）
- ⚠ 旧实现把同一份 6 页**就地重写**——第二个坏块出现时 BBT 即损坏（见 `bbt.h` 头注释）

#### 3.8.2 运行时坏块隔离

运行时坏块的检测时机：

```
检测时机       触发条件                     处理方式
──────────    ──────────────────────────   ─────────────────
PROGRAM 完成    FC_CW_STATUS_NAND_FAIL      标记坏块 → 数据搬移
ERASE 完成      FC_CW_STATUS_NAND_FAIL      标记坏块
READ 重试超限   ECC 连续多次升高 + retry 失败  标记坏块 → 数据搬移
```

**隔离流程**：

> ⚠ **订正**：`allocator_mark_bad` 已删除（`allocator.h` 有墓碑注释），坏块统一走
> **`bbt_mark_bad(alloc, gblock)`**（两形参，v0.56 去掉了 `reason`）；**无** `bbt_update`、
> **无** `fc_command_id_to_command`（命令身份已去缓存化，v15 改指针派生）。
> 运行时的坏块标记点（4 处）：GC 擦除失败、`ftl.c` 读不可恢复 / 写失败、`bbt.c` 内部。

```c
/* 坏块标记唯一入口：记进 BST（FTL_BLOCK_STATE_BAD）并置 BBT dirty */
void bbt_mark_bad(ftl_allocator_t *alloc, uint16_t gblock);
/* 4 处调用点：gc.c 擦除失败 / ftl.c 读不可恢复 / ftl.c 写失败 / bbt.c 内部 */

/* 完成路径中的失败处理（ftl_handle_completion）：
 *   PROGRAM/ERASE 的 NAND_FAIL → bbt_mark_bad(&ftl->alloc, ftl_paa_gblock(cmd->paa))
 *   READ 不可恢复（ECC 超限）  → bbt_mark_bad + 慢死预测（rr 记录）*/
```

#### 3.8.3 替换机制

```
坏块替换策略：Block Sparing（替换池补充）

  Sparing 不是"坏块→替换块"的静态映射，而是在坏块发生后
  从 Spare Pool 取一个好块补充到 free pool，保容量水位。

  运行时坏块替换流程（以 Super Block PROGRAM 失败为例）:

    Super Block {P0-BlockA, P1-BlockB} 中 P0-BlockA PROGRAM 失败：

      ① 关闭该 Super Block（不再写入新数据）
      
      ② 数据拯救：
         P0-BlockA 的已写页 → 能读就搬，不能读就丢
         P1-BlockB 的已写页 → 全部读到 buffer → 写入新 Super Block
      
      ③ 更新 L2P：
         被搬移的数据 → L2P 指向新的物理地址
      
      ④ 标记坏块：
         BBT 标记 P0-BlockA = BAD
      
      ⑤ 处理好的 Block：
         P1-BlockB 擦除 → 成功则回 free pool，失败也标记坏块
      
      ⑥ Free Pool 补充：
         allocator_pull_from_op() → 从 OP 池取一个好块归还 free pool（保容量水位）
      
      ⑦ 分配新的 Super Block：
         从 free pool 取双 Plane Block 配对

  allocator 分配时跳过坏块 + 通过 Spare Pool 保水位：

  > ⚠ **订正**：`allocator_alloc` / `allocator_select_block` / `alloc->erase_counts[]` /
  > `alloc->spare_pool_count` / `ftl_handle_bad_block` 均已不存在。真实 API：
  > - 选块：`allocator_select_plane_block(alloc, plane)`（**逐 plane 独立**，Policy B）、
  >   分配一对：`allocator_alloc_pair(alloc, temp, out[PLANE_NUM])`；
  > - 擦除计数在 **BST**（`alloc->blocks[gblock].erase_count`），非独立数组；
  > - 坏块判据是 BST 状态位（`ftl_block_is_bad`），非 `bbt_is_bad` 查表；
  > - Spare Pool 计数是 `alloc->spare_count`（上限 `FTL_SPARE_POOL_SIZE=50`），
  >   由 `allocator_pull_from_op` / 补池逻辑消费，无 `ftl_handle_bad_block`。

```c
/* 真实签名 */
uint16_t allocator_select_plane_block(ftl_allocator_t *alloc, uint8_t plane);   /* 单 plane */
nand_pba_t allocator_alloc_pair(ftl_allocator_t *alloc, uint8_t temperature,
                                nand_pba_t out[PLANE_NUM]);                      /* 一对 plane */
```

**为什么用 Spare Pool 而不是 Block Remap Table**：

```
Block Remap Table（"两层映射"）                   Spare Pool（本方案）：
  维护 bad_block → replacement_block 的映射       只补充 free pool，不做映射
  读写需查 remap 表翻译                              PAA 直接物理，无需翻译
  企业级不用这层间接                                  企业级也用 spare + data migration
```

ONFI Multi-Plane 命令中每个 Plane 有独立的行地址，
坏块导致配对异号不影响 Multi-Plane 并行操作，
所以不需要 "Block Remap" 来对齐编号。

#### 3.8.4 企业级 BBT 参考

| 维度 | 本方案 | 消费级 | 企业级 |
|------|--------|--------|--------|
| 出厂扫描 | 全量扫描 4096 Block | 全量扫描 | 增量扫描（只查未查过的）|
| BBT 存储 | NAND BBT 池（2 块轮转，一条 record = 1 页 + sequence） | NAND ×3 冗余 | NAND + SRAM cache + RAID1 |
| 运行时检测 | PROGRAM/ERASE 完成后 | 同左 | 同左 + ECC 趋势预测 |
| 替换策略 | Block Sparing（Spare Pool 保水位） | Block Skip | Stripe Skip + Sparing |
| 新增坏块 | alloc 跳过 | 同左 | 动态映射表更新 |
| 坏块预测 | 无 | 无 | 根据 ECC 上升趋势预测即将坏的 Block |

**企业级的坏块预测（Predictive Failures）**：

```
企业级 SSD 不等到 PROGRAM 失败才标记坏块——太晚了：

  监控每个 Block 的 ECC 纠正比特数趋势
  ECC 连续 N 次上升 → "预测"为即将坏块
  → 主动搬移该 Block 的数据
  → 标记为"慢死"（Slow Death）Block
  → 不再分配新数据

好处：
  在数据彻底损坏之前搬移 ✅
  用户无感 ✅
  避免 Read Error 触发读恢复的性能抖动 ✅
```

#### 3.8.5 参数建议

| 参数 | 当前值 | 说明 |
|------|--------|------|
| BBT_POOL_BLOCKS | 2（块 56..57） | BBT 池块数，池内轮转 |
| SCAN_PAGES_PER_BLOCK | **1**（只读 Page 0） | 出厂扫描每 Block 读的页数 |
| MAX_BAD_BLOCKS | 200 | BBT bitmap 容量上限（历史值） |
| ECC_PREDICT_THRESHOLD | 8 次 | ECC 连续上升触发慢死预测（`rr`）|
| FTL_SPARE_POOL_SIZE | 50 | 替换池大小（覆盖生命周期坏块）|
| FTL_OP_PERCENT | 10（= 204 块） | OP 池预留百分比（每 plane 块数的 10%）|

### 3.9 与企业方案对比

| 维度 | 本方案 | 企业级（Samsung / Solidigm） |
|------|--------|-----------------------------|
| Super Block | 跨双 Plane Super Block | 跨 N Die Super Block |
| 温度分区 | Hot / Cold | 多级温度 |
| OP | 10%（可配置 0~28%） | 7~28% |
| Free Pool | 一个已擦除池 + 三级水位 | 多层池 + 动态调节 |
| GC | Background GC 持续补充 | 后台预测性 GC |
| SLC Cache | 不需要（本身就是 SLC） | 是（TLC/QLC） |
| 元数据分布 | 每 LUN 固定位置 | 跨 Die 动态分配 + RAID1 |

---

## 4. Block 生命周期

### 4.1 完整流程

当前生命周期以 Super Block 为粒度，Free Pool 三级水位管理，多维度触发 GC：

```
                ┌────────────────────────┐
                │      Free Pool          │
                │  一个已擦除池（三级水位）  │
                │  free_count ≥ 32 正常   │
                │  8 < free < 32 后台 GC  │ ← 提前擦好，前台零等待分配
                │  free ≤ 8      紧急 GC  │ ← 最后防线
                └───────────┬────────────┘
                            │ allocator: 双 Plane 各取一个配对 + 磨损均衡
                            ▼
                ┌──────────────────────────┐
                │  Open Super Block          │  ← 当前写入位置
                │  Plane0: Block A            │    每温度等级 1 个
                │  Plane1: Block B            │    Hot / Cold
                │  [已写 30/64 页]            │
                └──────────┬───────────────┘
                            │ 64 页写满
                            ▼
                ┌──────────────────────────┐
                │  Closed Super Block        │  ← 等待 GC
                │  Plane0: [VVI..IVV]        │    跨双 Plane
                │  Plane1: [IVV..VVI]        │    有效页 + 无效页
                └──────────┬───────────────┘
                            │ 多维度触发 GC
                            ▼
                ┌──────────────────────────────────────────┐
                │  GC 执行                                  │
                │  ① 多维度选 victim（有效页,擦除,温度,时间）│
                │  ② 遍历双 Plane，搬移有效页（通过 FC）     │
                │  ③ 原子性保证：journal + GC bitmap         │
                │  ④ 擦除双 Plane victim Block              │
                │  ⑤ 归还 Free Pool（已擦除态）             │
                └──────────────────────────────────────────┘
                            │
                            ▼
                  回到 Free Pool（已擦除）
```

### 4.2 关键说明

- 不存在"日志区→数据区"的 merge 操作（那是混合映射的概念）
- 纯页映射下，数据落地就是最终位置
- 生命周期是 Free → Open Super Block → Closed Super Block → GC Erase → Free 的循环
- GC 以 Super Block 为粒度，一次回收双 Plane 的两个物理 Block
- GC 的"搬移"是：读 Super Block 中所有有效页 → 写到新的 Open Super Block → 更新映射 → 擦除原双 Block
- Free Pool 由 Background GC 持续补充到水位①（`FREE_POOL_TARGET`），前台 IO 无需等待 GC

### 4.3 GC Victim 选择算法（多维度加权评分）

纯贪心（选有效页最少的 Block）在 SLC 场景下基本可用，但存在缺陷：

- **磨损均衡缺失**：可能选中擦除次数 5000 次快报废的 Block，而非 20 次几乎全新的
- **温度维度缺失**：COLD 数据有效页几乎不变，下次 GC 还得搬

**当前实现已是四维加权 cost 评分**（`gc_select_victim`，`gc.h`）：
`GC_WEIGHT_VALID=500 / ERASE=200 / TEMP=200 / AGE=100`，**cost 取小**者当选 victim。
（早期"简化贪心（选 closed 时写页最少）"的过渡实现已废弃。）

加权公式（`include/ftl/gc.h` 千分比整数，**cost 取小者当选**）：

```
cost = GC_WEIGHT_VALID·valid_ratio + GC_WEIGHT_ERASE·erase_ratio
     + GC_WEIGHT_TEMP·cold_penalty + GC_WEIGHT_AGE·youth_penalty
```

| 权重 | 值 | 说明 |
|------|----|------|
| `GC_WEIGHT_VALID` (有效 CW) | 500 | 核心指标，直接影响写放大 |
| `GC_WEIGHT_ERASE` (擦除次数) | 200 | 磨损均衡，避开临近寿命的 Block（**`+erase_ratio`**，非 `1-`） |
| `GC_WEIGHT_TEMP` (温度) | 200 | `cold_penalty`：HOT=0 / COLD=1000 ⇒ **HOT 优先当 victim** |
| `GC_WEIGHT_AGE` (时间) | 100 | `youth_penalty = 1000 − age_ratio`，保证每个 Block 都有机会被回收 |

> ⚠ 与本文件早期字面公式的三处偏离（擦除项取 `+`、温度项定义为 HOT 优先、时间项用
> `youth_penalty`）已在 `include/ftl/gc.h` 逐条登记；**以上述代码定义为准**。

### 4.4 GC 触发条件（多维度触发）

不只依赖 free pool 水位线，而是多维度综合判断：

| 触发 | 条件 | 说明 | 紧急度 |
|------|------|------|--------|
| T1: 水位线 | `free_count` < 32 | 核心触发，空闲块不足 | 🔴 紧急 |
| T2: 写入量 | `total_writes % GC_INTERVAL_WRITES(1024) == 0` | 写入量触发，分散 GC 压力 | 🟡 主动 |
| T3: 定时 | 500ms 定时器到期 | 空闲时有空就做一点 | 🟢 后台 |
| T4: 强制 | `free_count ≤ FREE_POOL_EMERGENCY(8)` | 无空闲块时的最后手段 | 🔴 紧急 |
| T5: 磨损均衡 | 最大/最小擦除比 > 10（max ≥ 64 地板、min > 0） | 防止局部 Block 磨损太快 | 🟡 均衡 |
| ~~T6: 碎片率~~ | ~~平均有效页 > 70%~~ | **不做**（v0.51 定案，见下方） | — |

**T6 为什么不做（v0.51 定案）**——原定义的指标方向与"碎片率"标签相反，且自激：

- **自激正反馈**：`gc_select_victim` 挑有效页**最少**的 victim；把它从 `closed_sb_list` 移走 ⇒
  剩余集合平均值**上升** ⇒ T6 更容易命中。越 GC 越触发。
- **收益为负**：搬一个 70% 有效的 SB 要写 ~90 页，净空间仅 `1 − 0.7 = 0.3` 个 SB，写放大 ~45×。
  行业 victim 选择定式（贪心取有效页最少；LFS cost-benefit 最大化 `(1−u)·age/(1+u)`）**一致偏向 u 小**，
  没有任何已发表策略是"u 高 → 优先清理"。
- **语义为空**：closed 块平均 70% 有效 ⇒ 盘近满 ⇒ 空闲池必然小 ⇒ **T1/T4 已命中**，T6 加不出新信息。
- **主流处置方向相反**：占用率高 ⇒ 写放大 ≈ `1/(1−u)` 高 ⇒ 行业杠杆是 **OP（预留空间压低 u）** 与
  **GC 限速**（§4.6 的 `GC_BANDWIDTH_*`），不是"因为块太满所以多 GC"。

⇒ `GC_FRAGMENT_THRESHOLD` 不定义；`gc.h` 的 `GC_TRIGGER_*` 位图**刻意留空 0x20**（不分配 T6 位）。

### 4.5 GC 搬移的原子性保证

GC 搬移（读 → 写新位置 → 更新 L2P → 擦除旧 Block）不是原子的。掉电可能发生在中间任何一步。

三层保护机制：

- **Layer 1: Journal** — 每个 L2P 更新先写 Journal 后改 cache，掉电后 Journal 回放重建 L2P。**实现约束（v0.43）**：Journal 是 write-back（`page_buf` 攒满才 flush），GC/WL 擦除 victim 前必须 `journal_flush_page` 强制落盘——否则掉电落在"搬移条目未落盘 + victim 已擦除"窗口，回放指向已擦除块（数据损坏）。`gc_migrate_sb`/`wl_relocate` 已按此接线。
- **Layer 2: GC Bitmap** — 在 NAND 元数据区记录搬移进度 bitmap，每搬 16 页更新一次，掉电恢复后从断点继续。**未实现**（设计有、代码无）；v0.43 用"擦除前 flush"闭合等价窗口，Layer 2 留作后续优化。
- **Layer 3: 双拷贝容忍** — 任何掉电场景最多产生一个"孤悬"Block，不会丢数据，下次 GC 回收。**依赖 Layer 1/2 正确性**：在"擦除先于 flush"窗口下不成立（v0.43 已闭合）。

### 4.6 企业级 GC 参考

企业级 GC 不是为了"回收空间"，而是"维持性能一致"。Idle Time GC 持续在后台做，前台 IO 永远不感知 GC。同时动态调节 GC 占用的 NAND 带宽（Host 活跃时 ≤ 10%，空闲时 100%）。

三星 GC 策略 = 多维度加权 × 动态优先级 × Soft/Hard 多级阈值。

### 4.7 GC 参数配置建议

| 参数 | 建议值 | 说明 |
|------|--------|------|
| FREE_POOL_TARGET | 32 blocks | 水位①/②分界（原 `PRE_COND_LOW_WATER`，重复常量已删） |
| GC_INTERVAL_WRITES | 1024 | 写入量触发间隔 |
| GC_TIMER_INTERVAL_MS | 500 ms | 定时触发间隔 |
| GC_WEAR_LEVEL_RATIO | 10 | 最大/最小擦除比阈值（T5） |
| GC_WEAR_LEVEL_MIN_ERASE | 64 | T5 地板：max 擦除次数低于此值不触发（防早期空跑） |
| ~~GC_FRAGMENT_THRESHOLD~~ | ~~0.7~~ | **不定义**——T6 已废弃，见 §4.4 |
| GC_WEIGHT_VALID | 0.50 | 有效页权重 |
| GC_WEIGHT_ERASE | 0.20 | 擦除次数权重 |
| GC_WEIGHT_TEMP | 0.20 | 温度权重 |
| GC_WEIGHT_AGE | 0.10 | 时间公平权重 |
| GC_BANDWIDTH_HOST_ACTIVE | 10% | Host 活跃时 GC 带宽上限 |
| GC_BANDWIDTH_HOST_IDLE | 100% | Host 空闲时 GC 带宽 |

### 4.8 静态磨损均衡（Static Wear Leveling）

消费级 SSD 大多不做静态 WL，依赖动态均衡就够了。但 TLC/QLC（3000~5000 P/E）下冷热不均会缩短设备寿命，所以企业级（Samsung）有独立的静态 WL Daemon。当前设计虽运行在 SLC 上，但以学习 TLC/QLC 企业级做法为目标，全量实现静态 WL。

#### 4.8.1 总体架构

静态 WL 作为 `ftl_service()` 中的独立后台任务，与 GC 并列：

```
ftl_service() Phase 3:
  ├── flush:        dirty cache 写回
  ├── GC:           回收有效页少的 Block（回收空间）
  ├── WL:           ✅ 静态磨损均衡（搬冷数据，均衡擦除）
  └── Checkpoint:   定期保存 L2P 快照
```

WL 与 GC 的分工：

```
           有效页少         有效页多
  ┌────────┬──────────────┬──────────────┐
  │擦除高   │ GC 选         │ 不管          │ ← 擦除次数已够
  ├────────┼──────────────┼──────────────┤
  │擦除低   │ GC/WL 都选    │ WL 选         │ ← WL 的目标
  └────────┴──────────────┴──────────────┘
```

#### 4.8.2 触发条件（三条件 OR）

> ⚠ **订正**：真实 `wl_should_trigger(const struct ftl_context *ftl)` 返回 `int`
> （`FTL_OK`=0 / `1`=触发）。间隔门是 `ftl->total_writes < WL_MIN_INTERVAL`（**非**
> `total_writes - wl_last_run`，无 `wl_last_run` 字段）；擦除统计走 `wl_max_erase` /
> `wl_min_erase` / `wl_avg_erase` / `wl_count_blocks_below`（**非** `allocator_*_erase`）；
> 条件 2 有 `avg_ec > 0` 保护。

```c
#define WL_DELTA_THRESHOLD    (5000U)   // max-min 差值
#define WL_MAX_AVG_RATIO      (3U)      // max/avg 比值
#define WL_LOW_ZONE_WIDTH     (1000U)   // 低擦除区宽度
#define WL_LOW_BLOCK_PCT      (5U)      // 低区 Block 占比阈值
#define WL_MIN_INTERVAL       (1000U)   // 最小间隔（写入次数）

int wl_should_trigger(const struct ftl_context *ftl) {
    if (ftl == NULL) return FTL_OK;
    if (ftl->total_writes < WL_MIN_INTERVAL) return FTL_OK;   // 最小间隔

    uint32_t max_ec = wl_max_erase(ftl);
    uint32_t min_ec = wl_min_erase(ftl);
    uint32_t avg_ec = wl_avg_erase(ftl);

    /* 条件 1：最大/最小差值（核心触发） */
    if ((max_ec - min_ec) >= WL_DELTA_THRESHOLD) return 1;

    /* 条件 2：最大/平均比值（防止少数 Block 过热） */
    if ((avg_ec > 0U) && (max_ec >= avg_ec * WL_MAX_AVG_RATIO)) return 1;

    /* 条件 3：低擦除区 Block 数量（大量 Block 未充分磨损） */
    uint32_t low_zone  = min_ec + WL_LOW_ZONE_WIDTH;
    uint32_t low_count = wl_count_blocks_below(ftl, low_zone);
    if (low_count > (uint32_t)ftl->alloc.total_blocks * WL_LOW_BLOCK_PCT / 100U) return 1;

    return FTL_OK;
}
```

三星企业级 SSD 使用此类多维度触发条件，确保在静负载等极端场景下也不会出现单个 Block 过度磨损。

#### 4.8.3 受害块选择算法

WL 选 victim 的目标与 GC 相反——**选有效 CW 最多 + 擦除次数最少的 Block**（最冷的冷数据）。

```c
typedef struct {
    uint8_t   found : 1;                 // 是否找到 victim
    uint16_t  plane_blocks[PLANE_NUM];   // 双 Plane Block 号
    uint32_t  avg_erase;                 // victim 平均擦除次数
    uint16_t  valid_cw;                  // 有效 CW 数（双 Plane valid_count 之和；CW 级映射）
    uint32_t  read_count;                // 双 Plane read disturb 计数之和（B5 冷块优先权重）
} wl_victim_t;
```

> ⚠ **订正**：真实 `wl_select_victim(const struct ftl_context *ftl, wl_victim_t *victim)`
> 返回 `int`（输出经 victim 指针）。用 `wl_min_erase(ftl)`（非 `allocator_min_erase`）、
> `super_block_has_plane`（退化单 plane 跳过）、BST 的 `erase_count`/`valid_count`/`read_count`
> （非 `reverse_bitmap_sb_valid_count`）。**无** `sb_is_open` / `super_block_avg_erase`。

```c
int wl_select_victim(const struct ftl_context *ftl, wl_victim_t *victim) {
    uint32_t min_ec = wl_min_erase(ftl);
    /* 门限单位 = 有效 CW（CW 级映射：1 页 = CW_NUM 个映射单元）*/
    uint16_t min_valid_cw = (PLANE_NUM * PAGES_PER_BLOCK * CW_NUM) * WL_MIN_VALID_RATIO / 100U;
    wl_victim_t best = {0};

    for (uint16_t i = 0; i < ftl->alloc.closed_sb_count; i++) {
        const super_block_t *sb = &ftl->alloc.closed_sb_list[i];
        if (sb->valid != 0U) continue;              /* 跳过仍为 Open 的 SB */

        /* 该 SB 平均擦除次数（按 BST 全局块号索引；退化单 plane 按实际 plane 数取平均）*/
        uint32_t sb_ec = /* sum(blocks[ftl_gblock(p, sb->plane_blocks[p])].erase_count) / n */;
        if (sb_ec > min_ec + WL_LOW_ZONE_WIDTH) continue;   /* 不在低擦除区 */

        /* 有效 CW = 双 Plane valid_count 之和；同有效 CW 数下 read_count 高的冷块优先（B5）*/
        uint16_t valid  = /* sum(blocks[...].valid_count) */;
        uint32_t rc_sum = /* sum(blocks[...].read_count) */;
        if (valid < min_valid_cw) continue;         /* 有效 CW 不够多 */

        /* 评分：有效 CW 越多越优先（越冷越值得搬）*/
        if (valid > best.valid_cw) {
            best.found = 1;
            best.avg_erase = sb_ec;
            best.valid_cw = valid;
            best.read_count = rc_sum;
            best.plane_blocks[0] = sb->plane_blocks[0];
            best.plane_blocks[1] = sb->plane_blocks[1];
        }
    }
    *victim = best;
    return FTL_OK;
}
```

关键区别：

| 维度 | GC victim | WL victim |
|------|-----------|-----------|
| 有效 CW | 越少越好（快速回收空间） | 越多越好（冷数据标志） |
| 擦除次数 | 越高越好（保护低擦除块） | 越低越好（需要被擦一次） |
| 筛选条件 | valid_ratio < 30% | valid_ratio > WL_MIN_VALID_RATIO |

#### 4.8.4 数据搬移流程

> ⚠ **订正**：真实 `wl_relocate(struct ftl_context *ftl, const wl_victim_t *victim)`
> 返回 `int`（搬移 **CW** 数），**逐页索引扫双 plane**（无位图可扫，见下），读走
> `gc_read_page_with_lba`、写走 `ftl_sync_write_page_cw`，擦除归还**复用 `gc_erase_and_free`**。
> 原伪代码的 `paa_make` / `ftl_fc_read_page` / `l2p_cache_remap_paa` /
> `journal_append_remap` / `ftl_erase_block` / `alloc->erase_counts[]` 均已不存在。
>
> ⚠ **v0.60 起无常驻位图**（DRAM-less approach ③）：有效性**唯一**真值 = 逐 CW 读
> **页内 META（=OOB 反向映射）**取回该 CW 的扇区 LBA → 反查 L2P 判活。`wl_select_victim`
> 已要求 `valid >= min_valid_cw` ⇒ 不会选中空块。

```c
int wl_relocate(struct ftl_context *ftl, const wl_victim_t *victim) {
    uint32_t migrated = 0;

    /* Step 1: 逐**页索引**扫双 plane（目标页须成对分配，见 gc_migrate_sb 同构） */
    for (uint16_t page = 0; page < PAGES_PER_BLOCK; page++) {
        uint32_t cw_bitmap[PLANE_NUM] = {0, 0};
        lba_t    cw_lba[PLANE_NUM][CW_NUM];   /* 逐 CW 自己的扇区 LBA */
        nand_pba_t src[PLANE_NUM], dst[PLANE_NUM];
        uint8_t  buf[PLANE_NUM][FTL_READ_USER_SIZE];

        for (uint8_t plane = 0; plane < PLANE_NUM; plane++) {
            if (victim->plane_blocks[plane] == FTL_BLOCK_NO_PLANE) continue;

            src[plane] = pba_from_addr(&(nand_addr_t){ .ch = ftl->topo.ch, .ce = ftl->topo.ce,
                .block = victim->plane_blocks[plane], .plane = plane, .page = page });

            /* 读整页 + 逐 CW 从 META 取回扇区 LBA（未写过 = FTL_LBA_INVALID）。
             * 读失败 / 全页无写过 CW ⇒ 返回 FTL_ERROR，跳过本页。 */
            if (gc_read_page_with_lba(src[plane], buf[plane], cw_lba[plane],
                                      &base[plane], &ecc, &rc) != 0) continue;

            /* 逐 CW 反查 L2P：用**该 CW 自己的** LBA 查，仍指向 (本页, 本 CW) 才算有效 */
            for (uint8_t cw = 0; cw < CW_NUM; cw++) {
                if (cw_lba[plane][cw] == FTL_LBA_INVALID) continue;
                if (l2p_cache_read(&ftl->cache, cw_lba[plane][cw]) == pba_with_cw(src[plane], cw))
                    cw_bitmap[plane] |= 1U << (plane * CW_NUM + cw);
            }
        }
        if (cw_bitmap[0] == 0U && cw_bitmap[1] == 0U) continue;

        /* 成对分配目标页（Cold 流）；池耗尽 → break，保留未搬源页（L2P 未动） */
        if (allocator_alloc_pair(&ftl->alloc, FTL_TEMP_COLD, dst) == NAND_PBA_INVALID) break;

        for (uint8_t plane = 0; plane < PLANE_NUM; plane++) {
            if (cw_bitmap[plane] == 0U) continue;
            /* 只编有效 CW 子集（页内允许空洞；META 由页基址 + CW 偏移推出） */
            if (ftl_sync_write_page_cw(addr_from_pba(dst[plane]), buf[plane], cw_bitmap[plane],
                                       FTL_PAGE_CFG_DATA(FC_COMMAND_MODULE_GC,
                                                         ftl_lba_page(base[plane]))) != 0) continue;
            for (uint8_t cw = 0; cw < CW_NUM; cw++) {
                if ((cw_bitmap[plane] & (1U << cw)) == 0U) continue;
                lba_t      l       = cw_lba[plane][cw];
                nand_pba_t old_pba = pba_with_cw(src[plane], cw);
                nand_pba_t new_pba = pba_with_cw(dst[plane], cw);
                l2p_cache_write(&ftl->cache, l, new_pba);
                journal_append(&ftl->journal, l, old_pba, new_pba);
                ftl_block_sub_valid(&ftl->alloc.blocks[gsrc[plane]]);       /* 源块 -1 */
                ftl_block_add_valid(&ftl->alloc.blocks[ftl_pba_gblock(new_pba)]); /* 新块 +1 */
                migrated++;
            }
        }
    }

    /* F10：搬移条目先落盘再擦除（掉电窗口内回放不会指向已擦除块）*/
    if (migrated > 0U) journal_flush_page(&ftl->journal);

    /* Step 2: 擦除 + 归还 Free Pool（复用 gc_erase_and_free）*/
    gc_erase_and_free(&ftl->alloc, victim, ftl->total_writes);
    return (int)migrated;
}
```

流程与 GC 搬移一致，区别在于：
- 搬移目标是 **Cold Super Block**（温度分区到 COLD）
- Journal 记录的是 **PAA 重映射**（不是 LBA→PAA 的变化）
- 搬完后归还 Free Pool（已擦除态）

#### 4.8.5 WL 在 ftl_service 中的调度

> ⚠ **以下按 `src/ftl/ftl.c` 的真实实现重写**（原伪代码用的 `gc_background` / `cp_check` /
> `l2p_cache_flush_background(ftl,…)` / `ftl->free_pool.emergency` / `gc_emergency` 均已不存在）。

```c
/* 真实实现 = ftl.c 的 ftl_service_idle / _normal / _busy（由 ftl_service 按 ftl->mode 分派）*/

static void ftl_service_idle(ftl_context_t *ftl) {
    l2p_cache_flush_all(&ftl->cache);
    /* 补 Free Pool 到水位① FREE_POOL_TARGET：逐 SB「选 victim → 搬移 → 擦除归还」 */
    while ((ftl->alloc.free_count < FREE_POOL_TARGET) &&
           gc_select_victim(&ftl->alloc, &victim)) {
        (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
        gc_erase_and_free(&ftl->alloc, victim, ftl->total_writes);
    }
    (void)wl_execute(ftl);                     /* WL 全力执行 */
    (void)wl_predictive_relocate(ftl);         /* 慢死巡检：1s 节流，一次一个 SB */
    journal_flush_page(&ftl->journal);
    (void)bbt_flush(&ftl->alloc);
    (void)wear_flush(&ftl->alloc, ftl->total_writes);   /* P0-1 磨损计数落盘（节拍门控）*/
    if (checkpoint_check_trigger(ftl, elapsed)) { (void)checkpoint_create(ftl); }
}

static void ftl_service_normal(ftl_context_t *ftl) {
    /* thrashing 降级（P1-4）：工作集 ≫ cache 时**停主动 flush**，脏行只由驱逐按需写回 */
    if (!l2p_cache_is_thrashing(&ftl->cache)) {
        /* 扫描 DIRTY 行，最多 flush BG_NORMAL_FLUSH_COUNT 条 */
    }
    /* GC 触发 = 位掩码（T1 水位 / T2 写入量 / T3 定时 / T4 紧急 / T5 磨损比）*/
    if (gc_check_triggers(&ftl->alloc, ftl->total_writes, elapsed) != 0U) {
        if (gc_select_victim(&ftl->alloc, &victim)) {
            (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
            gc_erase_and_free(&ftl->alloc, victim, ftl->total_writes);
        }
    }
    if (wl_should_trigger(ftl)) { (void)wl_execute(ftl); }
    (void)bbt_flush(&ftl->alloc);
    (void)wear_flush(&ftl->alloc, ftl->total_writes);
    if (checkpoint_check_trigger(ftl, elapsed)) { (void)checkpoint_create(ftl); }
}

static void ftl_service_busy(ftl_context_t *ftl) {
    /* 最少后台：只 flush 1 条防 evict 阻塞 */
    l2p_cache_flush_one(&ftl->cache, <首个 DIRTY 行>);
    /* 紧急 Checkpoint：Journal ≥ 95% 时 BUSY 也强制执行 */
    if (checkpoint_is_emergency(ftl)) { (void)checkpoint_create(ftl); }
    /* GC / WL 不执行，前台 IO 优先 */
}
```

#### 4.8.6 对写放大（WA）的影响

静态 WL 增加写放大，但在企业级场景中代价极低：

```
TLC（3000~5000 P/E，~400GB 容量）：
  每次 WL：搬移 ~80 有效页（~320KB 写入）
  生命周期内：~200 次 WL → ~64MB 额外写入
  总写入能力：~1.2PB（100k per 4KB × 各因子）
  WL 贡献的 WA：< 0.0001% → 可忽略

当前 SLC（100k P/E，512MiB）：
  触发次数更少，WA 更可忽略

结论：静态 WL 对 WA 的影响微乎其微。
```

#### 4.8.7 参数建议

| 参数 | 建议值 | 说明 |
|------|--------|------|
| WL_DELTA_THRESHOLD | 5000 | max-min 擦除差值触发 |
| WL_MAX_AVG_RATIO | 3 | max/avg 擦除比 ≥ 3x 触发 |
| WL_LOW_ZONE_WIDTH | 1000 | 低擦除区宽度 |
| WL_LOW_BLOCK_PCT | 5 | 低区 Block 占比 ≥ 5% 触发 |
| WL_MIN_INTERVAL | 1000 writes | 两次 WL 最小间隔 |
| WL_MIN_VALID_RATIO | 70% | 只搬有效页 ≥ 70% 的冷 Block |
| WL_BUDGET_SB | 1 SB/cycle | 每轮最多搬 1 个 Super Block |

---

## 4.9 Block State Table（BST）— 统一 per-block 结构体数组

> **v0.32 新增**。实现：`include/ftl/bst.h`，`ftl_block_t[4096]`（v0.32 为 56B/项 = 224KB；
> v0.48 瘦身为 40B/项；**v0.55 把逐页有效位图搬出为独立表** ⇒ 条目 24B/项；**v0.56 删 3 个
> 死字段** ⇒ 条目 **20B/项 = 80KB**；**v0.60 删掉该独立位图表 64KB** ⇒ **80KB，无位图表**，
> 见 §4.9.7/§4.9.8/§4.9.9）。条目全常驻 SRAM。

### 4.9.1 背景：分散 per-block 元数据的统一

v0.31 前 per-block 信息分散四张表，无统一承载，且存 3 处全局/per-plane 块号碰撞 bug：

| 现有信息 | 存放 | 粒度 | 问题 |
|---|---|---|---|
| 擦除次数 | `allocator->erase_counts[]` | 每 block | plane1 与 plane0 索引碰撞（gc.c:238 用 per-plane 号） |
| 坏块 RAM 列表 | `bad_blocks[]`+`bad_count` | 每 block | 与 bbt bitmap 双写可能漂移 |
| 坏块持久化位图 | `g_bbt_bitmap[512]` | 每 block 1bit | 独立 static 单例，非单数据源 |
| 有效页数 | 无——GC/WL 用 `sb->next_page` 近似 | 每 SB | 非真值，选 victim 不准 |
| ECC 趋势 | `rr_ecc_track_t[]`（未分配） | 每 block | read recovery 空转 |
| 温度 | 仅 `super_block_t.temperature` | 每 SB | block 关闭后 GC/WL 不读 |
| 块状态 | 散落（free_blocks/sb->valid） | — | 块不知道自己状态 |

**3 处块号碰撞 bug（已修复）**：
1. `gc_erase_and_free` 用 per-plane 号索引 4096 容量数组 → 2048..4095 从未被写
2. `allocator_select_block` 用 `free_blocks[i]`（per-plane 号）索引 erase_counts → 同样碰撞
3. `bbt_scan_factory` `block` 走到 4095 被 11bit 掩码截断 → 重复扫 plane0

修复：`ftl_gblock(plane, block) = plane * BLOCKS_PER_PLANE + block` 单点转换，全部索引统一全局块号。

### 4.9.2 结构（BIT 条目，**无位图表**）

```c
typedef struct {
    uint32_t       erase_count;   /* 4B  @0   擦除次数 */
    uint32_t       last_pe_ts;    /* 4B  @4   最近擦除时间戳（WL 冷热避聚） */
    uint16_t       read_count;    /* 2B  @8   read disturb 计数 */
    uint16_t       valid_count;   /* 2B  @10  双 Plane 合计有效 **CW** 0..512 */
    uint16_t       state;         /* 2B  @12  状态机+温度（bitmask 宏） */
    uint16_t       sb_id;         /* 2B  @14  block→SB 反向索引 */
    rr_ecc_track_t rr;            /* 3B  @16  ECC 健康（last_ecc/rising/health） */
} ftl_block_t;                    /* 19B → 20B（u32 对齐）→ 4096×20 = 80KB */
```

> **无位图表**（v0.60）：条目只留块级聚合 `valid_count`（GC/WL victim 选择用）；
> **逐 CW 有效性不常驻**——由「读页内 META（=OOB 反向映射）取回该 CW 的 LBA → 反查 L2P」
> 现查（**DRAM-less approach ③**，见 §4.9.7 与 `cw_level_mapping_design.md §八`）。
> v0.55 曾把逐页位图搬成独立表 `alloc->valid_map[gblock]`（64KB），v0.60 整表删除。

> **v0.56 删死字段**：`program_count`(2B)、`bad_reason`(1B)、`gc_generation`(1B) 三者
> **写而无人读**，已删——条目 24B → **20B**（96KB → 80KB）。`bad_reason` 的
> `FTL_BLOCK_BAD_*` 枚举与 `bbt_mark_bad()` 的 `reason` 形参一并撤除（BBT 持久化本就是
> bitmap，原因跨重启必丢，属纯 RAM 诊断残留）。`program_count` 在 Wear 表里的持久化槽
> 同步撤出（条目 12B → 8B）。见 §4.9.8。

**`state` 位域**（bitmask 宏 + 访问器，非 C 位域——嵌入式可移植）：

```
bit0  BAD        bit1  OPEN      bit2  CLOSED    bit3  FREE
bit4  RESERVED   bit5  TEMP_HOT  bit6  TEMP_COLD bit7-15 预留

（bit7/bit8 原为 PRE_COND / EMERGENCY 分级位，v0.52 删除——从无读者，见 §3.7）
```

### 4.9.3 与消费级/企业级对比

| 维度 | 本方案（v0.32） | 消费级 DRAM-less（Phison/SMI/Realtek） | 企业级（Samsung/Solidigm） |
|------|-----------------|--------------------------------------|---------------------------|
| 粒度 | 物理 Block 全局号索引 | 物理 Block BIT | 物理 Block Descriptor |
| 状态机 | BAD/OPEN/CLOSED/FREE + 分级 | FREE/OPEN/CLOSED/BAD | 多级 + 迁移代数 |
| VPC | `valid_count` 计数（**无位图**，v0.60 起逐 CW 现查 META+L2P） | VPC + bitmap（可选） | VPC + bitmap |
| 温度 | block 级 HOT/COLD 位 | block 级温区位 | 多级温度流 |
| read disturb | `read_count` 2B（预铺） | 部分有 | 有（读计数触发搬移） |
| 反向索引 | `sb_id` | 有（SB→block + block→SB） | 有 |
| ECC 健康 | `rr` 上次值 + 趋势计数（3B） | 健康度等级 | 全历史落 DRAM |
| 每项内存 | 20B（**无位图表**，v0.60） | 4~16B（压缩版） | 数百 B（DRAM 不心疼） |
| 存储 | 条目**全常驻 SRAM**（80KB）；有效性**不常驻**（逐 CW 现查页内 META） | 核心 SRAM + 扩展 NAND | 全 DRAM 常驻 |

> 表中**不列「坏块原因」**（v0.56）：本仓 `bad_reason` 已删（无读者），且主流侧
> 「位域 2-4bit / 位域 + 日志」一栏**无可信出处**（详见 v0.56 ADR 的撤回说明）。

**选择理由**：粒度物理块、VPC+bitmap 双承载；本方案条目全字段常驻，
条目 80KB 常驻 SRAM；**有效性不常驻**（v0.60 删位图表 64KB，逐 CW 现查页内 META）。
产品化裁剪路径见 §19（档三）。

### 4.9.4 维护点（全真值，缺一不可）

| 事件 | 动作 |
|---|---|
| 写成功 | VPC++（`ftl_block_add_valid`）；覆盖写 VPC--（旧 CW 由 L2P 反查自然落选，无需清位） |
| 读成功 | read_count++ + rr_record_ecc（ECC 历史） |
| GC/WL 迁移成功 | 源块 VPC--；新块 VPC++（有效性无需迁移——新页 META 随写落盘） |
| 擦除成功 | erase_count++ + last_pe_ts 写 + VPC=0（`ftl_block_clear_valid`）+ 状态 FREE |
| trim | VPC--（L2P 置 INVALID 后该 CW 自然失效，无需清位） |
| 坏块 | 置 BAD + clear_valid |
| 分配 | 置 OPEN + TEMP + sb_id |
| 写满关闭 | 置 CLOSED + sb_id → closed 索引 |
| 掉电恢复 | open SB 扫描置 OPEN（closed 重建 v0.40 已实现：`ftl_rebuild_closed_sb_list` 全扫 + 逐 CW L2P 反查回填 VPC） |

全部经 `ftl_block_add/sub_valid`、`ftl_block_state_set/clr` 等访问器单点更新。
⚠ **v0.60 起无位图可清**：有效性由「L2P 是否仍指向本物理 CW」唯一决定，覆盖/TRIM/迁移
**不需要**任何清位动作（这正是 approach ③ 相对位图的优势——不存在「位图与计数漂移」）。

### 4.9.5 为什么选"统一单数组"而非"分散表"

| 方案 | 优点 | 缺点 |
|---|---|---|
| A：统一 `ftl_block_t[]` 单数组（**选中**） | 单一数据源；GC/WL/read recovery/BBT/allocator 共享；一次骨架切换可回退 | 工程量大；BSS 224KB |
| B：保持分散 + 只补 rr 表 | 改动小 | 数据仍分散；GC 加速需第三张表 |
| C：按 Super Block 组织 | SB 级语义集中 | 擦除/坏块/ECC 是物理块级事实，需重映射全部索引；改动最大 |

**结论**：A 符合行业 BIT（物理块粒度）与 DRAM-less 单数据源纪律。

> ⚠ **订正（v0.57）**：C 的否决理由原含「两 plane 同号 ⇒ 按 SB 存可去重省 96KB」，该前提已被
> **v0.12 Policy B**（逐 plane 独立选块）删除 ⇒ 两 plane 元数据互不相同、**无可去重**，省下的是
> 分辨率而非重复。C 现在只剩**更硬**的一条理由：擦除/坏块/ECC 是**物理块级事实**。详见变更史 v0.57。

### 4.9.6 v0.48 瘦身（56B → 40B）

逐字段审计后只做两件事，收益 224KB → 160KB（AXI_RAM 98.2% → 81.5%）：

**① `rr` 16B → 3B**：8 深度环形缓冲的唯一用途是供给趋势比较的 `prev` 一个值，
改成单字节 `last_ecc` 即可。**顺带修复一个 bug**——原 `prev = ecc_history[history_idx]`
取的是环形缓冲里**将被覆盖的最老格**（8 次读之前的值），与注释「上一次值」不符；
`last_ecc` 语义即真正的上一次读数。同时清掉 `last_update`（4B，写点有、读点零）。

**② 字段重排**：`rr` 移到 `valid_map` **之前**（v0.48 时位图仍在条目内）。`valid_map` 是
`uint64_t[PLANE_NUM]`，需 8 字节对齐；`rr` 只有 3B，若留在数组之后会被尾部对齐从 40B 撑到
48B（白丢 8B/块 = 32KB）。⚠ 此对齐约束**已随 v0.55 位图搬出条目消失**（见 §4.9.7）。

**审计发现（本次保留，未处理）**：

| 类别 | 明细 |
|---|---|
| 死字段（写了无人读） | ~~`program_count`(2B)、`bad_reason`(1B)、`gc_generation`(1B)~~ ✅ **v0.56 已删**（见 §4.9.8）（~~`last_pe_ts`~~ **P1-1 已消费**：GC victim `youth_penalty` 读它，见 `gc.c:241/243/309/311`；本表原列其为死字段**已过期**） |
| 只写不读的状态位 | `OPEN`、`RESERVED`、`TEMP_HOT`、`TEMP_COLD`（~~`PRE_COND`/`EMERGENCY`~~ v0.52 已删除） |
| 跨重启不持久化 | ~~`erase_count` / `read_count` / `rr` 每次 `ftl_init` 被 `memset` 归零（`ftl.c:471`）~~ ✅ **已修（P0-1，2026-10-12）**：`erase_count`/`read_count` 由 Wear 表（`src/ftl/wear.c`）持久化并在 `ftl_init` 里 `wear_init` 回填（v0.56 起 `program_count` 已随字段删除撤出，Wear 条目 12B → 8B、快照 25 → 17 页）；`rr`（3B，驱动慢死预测 CRITICAL 分支）仍不持久化，登记为后续扩展。`METADATA_TYPE_ERASE_COUNT` 仍未引用——Wear 块位置固定，不走 Directory 登记（同 BBT）。 |

⚠ **订正（2026-10-12）**：~~砍掉 4 个死字段可再降 8B/块 → 32B → 128KB~~ —— **此算式不成立**。
`program_count`(2B)+`bad_reason`(1B)+`gc_generation`(1B) 共 4B 落在 `valid_map` 8B 对齐的
**padding 里**（`rr` 之后活字段 19B → 补到 24B → `valid_map` 16B = 40B）；删掉它们 `sizeof`
**仍是 40B（省 0 字节）**。要到 32B/128KB 必须再砍**活**数据 ≥3B（`rr` 3B 或 `last_pe_ts` 4B）。
真正省内存的着力点是 **`valid_map`（16B/块 = 64KB）的驻留策略**，见 §19。
✅ **该杠杆已于 v0.60 消解**：位图表**整表删除**（DRAM-less approach ③，见 §4.9.9）。

> ⚠ **上式的条件（v0.55 揭示）**：「删之省 0 字节」**只对 `valid_map = uint64_t[PLANE_NUM]`
> 成立**——它的 8 字节对齐把省下的 4B 又吃回 padding。v0.55 把位图搬出条目（§4.9.7）后，
> 8 字节对齐约束随之消失，条目从 40B 落到 24B（96KB）。**「对齐要求」才是那个隐藏前提。**
> v0.56 遂删这 4B → **20B（80KB）**，见 §4.9.8。

### 4.9.7 v0.55 位图搬出条目（位图与条目的分离）〔**v0.60 后此表已整表删除**，见 §4.9.9〕

> ⚠ **本节描述的是 v0.55 的中间形态**（位图从条目字段搬为独立表 `alloc->valid_map`）。
> **v0.60 已把该独立表整体删除**（DRAM-less approach ③）⇒ 本节仅存史，**当前无位图表**。

**动机**：主流 BIT 把「块级聚合」与「逐页有效位图」分开——位图是 per-block 元数据里
**唯一可能不常驻**的一项（1 bit/页，块数大时会超出 SRAM），主流把它当独立、可落盘
（OOB/spare 或专用区）、可 GC 时重建的弹性项，而非塞进每块一条的条目里。本仓此前把
位图当条目字段，导致 ① 条目大小被位图的 8 字节对齐绑架（删死字段省 0 字节）；
② 位图无法独立搬走。

**改法**（`include/ftl/bst.h` + `allocator.h` + `main.c`）：

| 项 | v0.48~v0.54 | v0.55 |
|---|---|---|
| BIT 条目 | `ftl_block_t` 40B（含 `valid_map[2]` 16B + `_pad` 1B） | `ftl_block_t` **24B**（无位图） |
| 位图 | 条目内字段 | **独立表** `alloc->valid_map[gblock]`（每块 `PLANE_NUM` 个 u64 = 16B） |
| 访问器 | `ftl_block_set_valid(b, plane, page)` | `ftl_block_set_valid(b, valid_map, plane, page)`（显式收本块切片） |
| 承载 | `g_blocks[4096]` 160KB | `g_blocks[4096]` 96KB + `g_valid_map[4096][2]` 64KB = 160KB |

**内存不变**（160KB）——本变更是**结构性**的：条目不再携带位图，两者可独立驻留/搬移。
`valid_count`（块级聚合）仍留在条目里且是**真值**，位图与它由访问器单点同步。

**验证**：`make clean && make all` 零告警（AXI_RAM 332.4KB/384KB）；`make clean && make tests` 零告警。

### 4.9.8 v0.56 删死字段（24B → 20B）

v0.55 搬走位图后，条目里已**不再有 8 字节对齐约束**，v0.48 记的「删死字段省 0 字节」
失效——此时删除真的省字节。

| 字段 | 大小 | 删除依据 |
|---|---|---|
| `program_count` | 2B | 写成功++，全仓**零读者**（v0.45 曾设想做健康模型，从未落地） |
| `bad_reason` | 1B | 仅 `bbt_mark_bad` 写入，零读者；BBT 持久化本就是 bitmap，原因跨重启必丢 |
| `gc_generation` | 1B | GC 迁移成功++，零读者 |

**连带撤除**：
- `FTL_BLOCK_BAD_*` 枚举（6 个常量，唯一用途是喂 `bad_reason`）
- `bbt_mark_bad(alloc, gblock, reason)` → `bbt_mark_bad(alloc, gblock)`（4 处调用点）
- Wear 表的 `program_count` 持久化槽 → 条目 12B → 8B（`_reserved` 对齐填充随之不再需要），
  全量快照 25 → 17 页；`wear_mark_dirty` 脏标记点 7 → 5（删 `ftl.c` 写完成的两点）

**收益**：条目 24B → **20B**（19B 活字段 + 1B 对齐），4096 项 96KB → **80KB**；
AXI_RAM 332.4KB → **316.4KB**（82.4%）。**无介质格式变更**（BST 从不落盘）。

**保留未动**：`OPEN` / `RESERVED` / `TEMP_HOT` / `TEMP_COLD` 状态位（见 §4.9.6 第 2 行）
——它们是**状态机语义**的一部分（非独立字段），且 `test_rebuild.c` 依赖，另案处理。

### 4.9.9 v0.60 删位图表（有效性切 DRAM-less approach ③）

**动机**：v0.55 的独立位图表（64KB）是本仓**唯一**非 DRAM-less 的残留——DRAM-less 盘
（eMMC/UFS）的主流做法是 **flash reverse mapping**：不常驻任何有效性位图，读**页内
OOB/P2L** 取回逻辑地址 → **反查 L2P** 判活（出处见 `cw_level_mapping_design.md §八`：
DFTL / arXiv 1504.01666 活页识别三法 / KIISE / LeaFTL·VPFTL）。本仓的「OOB」= 页内 **META**
（`[META:4][CRC:4][USER:512][PARITY:8]`，`include/fc/fc.h`），存的正是该 CW 的扇区 LBA。

**改法**：

| 项 | v0.55~v0.59 | v0.60 |
|---|---|---|
| 位图表 | 独立表 `alloc->valid_map[gblock]`（16B/块 = 64KB） | **删除**（字段/`g_valid_map`/`ftl_init_params_t.valid_map` 全撤） |
| 有效性真值 | 位图（单调「写过」提示）+ 逐 CW L2P 反查 | **唯一** = 逐 CW 读页内 META → 反查 L2P |
| 跳过未写页 | `ftl_block_is_written` 查位图 ⇒ 免 NAND 读 | `gc_read_page_with_lba` 读回全 FF ⇒ 返回 `FTL_ERROR` 自然跳过；**整块空洞**由 `gc_migrate_sb` 的 `valid_count == 0` 快路径覆盖 |
| `gc_read_page_with_lba` | 输出页基址（调用者 `base+cw` 推断） | **输出逐 CW 的扇区 LBA**（`lba_t cw_lba[CW_NUM]`，未写过 = `FTL_LBA_INVALID`） |

**收益**：AXI_RAM 319KB → **255.0KB**（66.4%），余量 65 → **129 KB**（为波 2 序 3/4 表所有权 /
多实例化腾空间）。同时消除「位图提示 vs `valid_count` 计数」两者漂移的隐患
（TRIM 只减计数不清位、裸 NAND 扫描只置位不计数）。**无介质格式变更**（位图纯 RAM 结构）。

**代价（诚实登记）**：散点空洞（同块内既满页写又 512B 写）的页仍会多读一次 NAND；
P2-6 的主要形态「整块空洞 plane 块」由快路径覆盖。详见 `ftl_optimization_plan.md §8.1 P2-6`。

**验证**：`make clean && make all` 零告警（AXI_RAM 255.0KB/384KB）。

---

## 5. L2P 映射表设计

### 5.1 映射粒度

```
LBA 0x00000 ──→ CW 0 of Page X ──→ PBA(page, plane, block, page, cw=0)
LBA 0x00001 ──→ CW 1 of Page X ──→ PBA(page, plane, block, page, cw=1)
LBA 0x00002 ──→ CW 2 of Page X ──→ PBA(page, plane, block, page, cw=2)
LBA 0x00003 ──→ CW 3 of Page X ──→ PBA(page, plane, block, page, cw=3)
LBA 0x00004 ──→ CW 0 of Page X+1 ──→ ...
```

- 1 entry = `nand_pba_t`（uint32_t），`[CH:2][CE:2][PAA]`，PAA=`[CW:2][PAGE:6][PLANE:1][BLOCK:11]`
- 每 1 个 LBA 对应 1 个 CW（512B user data）
- **必须整页写入**：SLC NAND 不支持 partial page program，FTL 写 512B 的物理操作始终是整页（4 CW）写入

### 5.2 Region 分片

| 概念 | 值 | 说明 |
|------|----|------|
| Entry 大小 | 4 B | `nand_pba_t` |
| Region 大小 | 2,048 B | 512 entries × 4B = 1 NAND page |
| Region 数 | 2,048 | 4 MB / 2 KB |
| 每块装 region | 128 | `L2P_REGION_PER_BLOCK = FTL_SLOTS_PER_BLOCK = PAGES_PER_BLOCK × PLANE_NUM` |
| 全表占用 NAND | **16 blocks** | ⌈2,048 ÷ 128⌉（`L2P_BLOCKS`） |

> ⚠ **订正**：早期文档写「32 blocks / 64 pages-per-block」。真实公式是
> `L2P_REGION_PER_BLOCK = PAGES_PER_BLOCK × PLANE_NUM = 128`（一块的两 plane 各 64 页都算 region 槽位）
> ⇒ 2,048 ÷ 128 = **16 块（Block 0..15）**。块号由 `ftl_l2p_region_placement` 算出：
> `block = L2P_BASE_BLOCK + region_id / 128`。

```
L2P 表（NAND 元数据区 Block 0..15，哨兵层——本区页永不写入）：
  Block 0:  Region 0000 ~ Region 0127 (128 region 槽 = 2 plane × 64 页)
  Block 1:  Region 0128 ~ Region 0255
  ...
  Block 15: Region 1920 ~ Region 2047

每 Region = 512 条 entry：
  entry[0]   → PBA for LBA = region_id × 512 + 0
  entry[1]   → PBA for LBA = region_id × 512 + 1
  ...
  entry[511] → PBA for LBA = region_id × 512 + 511
```

### 5.3 Region 物理地址计算（通过 Metadata Directory）

动态分配下，Region 的物理位置通过 Metadata Directory 查询，
不再使用固定公式：

```c
/* ⚠ v0.50 订正：本段为早期设计示意，与实现已不符 ——
 *   `metadata_dir_alloc_region` / `metadata_dir_add` / `paa_make` **均已删除**；
 *   region 的物理位置不再"首次访问时分配"，而是**每次 flush 追加**到日志块
 *   （`metadata_dir_append_slot`），Directory 条目在 `metadata_dir_fill_fixed`
 *   时全部预登记、由 `metadata_dir_commit_region` 就地覆盖。以下代码保留作历史。 */

/* 启动时加载 Metadata Directory */
metadata_directory_t g_metadata_dir;

/* 读取 LBA 所在 Region 的物理位置 */
static nand_paa_t l2p_region_ppa(uint16_t region_id) {
    /* ① 查 Directory：优先 SRAM 中的缓存 */
    metadata_entry_t *entry = metadata_dir_lookup(&g_metadata_dir,
                                                   METADATA_TYPE_L2P_REGION,
                                                   region_id);
    if (entry != NULL) {
        return entry->paa;   // 已有记录 → 直接返回物理 PAA
    }

    /* ② 首次访问（或 Directory 被 evict）：从 Free Pool 分配 */
    /*    偶数 region_id → Plane 0，奇数 → Plane 1（双 Plane 交错）*/
    uint8_t plane = (uint8_t)(region_id & 1U);

    nand_paa_t paa = metadata_dir_alloc_region(&g_metadata_dir,
                                                region_id, plane);
    return paa;
}

/* 分配新的元数据物理位置 */
nand_paa_t metadata_dir_alloc_region(metadata_directory_t *dir,
                                      uint16_t region_id,
                                      uint8_t plane) {
    /* 从 Free Pool 取块（元数据走 COLD 温度）*/
    uint16_t block = allocator_select_block(&g_alloc, FTL_TEMP_COLD, plane);

    /* 起始页 = 0（每次分配从 Page 0 开始顺序写入）*/
    nand_paa_t paa = paa_make(0, plane, block, 0, 0);

    /* 更新 Directory */
    metadata_dir_add(dir, METADATA_TYPE_L2P_REGION, region_id, paa);

    return paa;
}
```

### 5.4 LBA 分解

```c
uint16_t region_id  = lba >> 9;      // lba / 512 → 0..2047
uint16_t entry_idx  = lba & 0x1FF;   // lba % 512 → 0..511
```

### 5.5 Metadata Directory（元数据目录）

Metadata Directory 是动态元数据分配的核心数据结构，
SRAM 常驻（**≈ 18 KB**：2048 条 × 7B 条目 = 14 KB + `l2p_live[]` 等），
启动时从 Anchor 池加载（一份 record = 8 页）。

> ⚠ **v0.50 订正（本节代码为早期示意，多处已不符）**：
> - `METADATA_DIR_ENTRIES` = **2048**（`= L2P_REGIONS_PER_INSTANCE`），不是 128；
> - 条目结构里**没有 `sequence` 字段**（`metadata_entry_t` = `type` + `id` + `paa`）；
> - `metadata_dir_flush_to_anchor` 返回 `ftl_status_t`，且写的是**Anchor 池（32 块）内一条
>   8 页的 record**，不是"同块双页冗余"；
> - **没有** `anchor_page` 这个固定页游标——写点由 `metadata_ring_locate` 从池内容推导；
> - "Directory 变更 ≥ 64 次触发" 的节流改由 `dir->change_count` 承担（`uint32_t`，不再是
>   会回绕的 `uint8_t`）。

#### 数据结构

```c
#define METADATA_DIR_ENTRIES    (L2P_REGIONS_PER_INSTANCE)   /* = 2048，非 128 */

/* Entry：7 字节（packed），**无 sequence**——位置由 record 自身的 sequence 锚定 */
typedef struct {
    uint8_t    type;               // METADATA_TYPE_*
    uint16_t   id;                 // L2P region_id / 0 for others
    nand_paa_t paa;                // 物理 NAND 位置
} __attribute__((packed)) metadata_entry_t;   // 7 bytes

typedef struct {
    metadata_entry_t entries[METADATA_DIR_ENTRIES]; // 2048 × 7B = 14 KB
    uint16_t         count;                // 有效 entry 数
    uint32_t         anchor_sequence;      // 当前 Anchor sequence
    uint8_t          dirty;                // 是否有未写回 Anchor 的变更
    uint32_t         change_count;         // 自上次 Anchor flush 的变更数（节流用）
    uint8_t          have_dynamic;         // 是否已有 region 落到动态块
    uint16_t         l2p_live[FTL_BLOCK_TOTAL]; // 每个 L2P 日志块上的活 region 数
    /* …当前日志块游标 / Anchor 池游标 / topo（详见 metadata.h）… */
} metadata_dir_cache_t;             // SRAM 常驻 ≈ 18 KB，随 ftl_context_t 落 AXI_RAM
```

#### 核心操作

```c
/* 查找元数据位置 */
nand_paa_t metadata_dir_lookup(metadata_dir_cache_t *dir,
                                uint8_t type, uint16_t id) {
    for (int i = 0; i < dir->count; i++) {
        if (dir->entries[i].type == type && dir->entries[i].id == id)
            return dir->entries[i].paa;
    }
    return PAA_INVALID;   // 未找到
}

/* 添加/更新元数据位置 */
void metadata_dir_update(metadata_dir_cache_t *dir,
                          uint8_t type, uint16_t id, nand_paa_t new_paa) {
    /* 查找现有 entry（覆盖旧位置）*/
    for (int i = 0; i < dir->count; i++) {
        if (dir->entries[i].type == type && dir->entries[i].id == id) {
            dir->entries[i].paa = new_paa;
            dir->entries[i].sequence++;
            dir->dirty = true;
            return;
        }
    }
    /* 新增 entry */
    dir->entries[dir->count].type = type;
    dir->entries[dir->count].id = id;
    dir->entries[dir->count].paa = new_paa;
    dir->entries[dir->count].sequence = dir->anchor_sequence;
    dir->count++;
    dir->dirty = true;
}

/* 将目录写回 Anchor Block（满 64 变更或 Checkpoint 时调用）*/
void metadata_dir_flush_to_anchor(metadata_dir_cache_t *dir) {
    if (!dir->dirty) return;

    metadata_directory_t anchor;
    anchor.magic = 0x414E4449;    // "ANDIR"
    anchor.sequence = ++dir->anchor_sequence;
    anchor.entry_count = dir->count;
    for (int i = 0; i < dir->count; i++) {
        anchor.entries[i].type = dir->entries[i].type;
        anchor.entries[i].id = dir->entries[i].id;
        anchor.entries[i].paa = dir->entries[i].paa;
    }
    anchor.crc = crc32(&anchor, offsetof(typeof(anchor), crc));

    /* 写入 Anchor Block（Multi-Plane 双份冗余）*/
    fc_write_page(ANCHOR_BLOCK_PAA(anchor_page), &anchor, sizeof(anchor));
    fc_write_page(ANCHOR_BLOCK_PAA(anchor_page + 1), &anchor, sizeof(anchor));

    dir->dirty = false;
}
```

#### Anchor 同步触发条件

| 触发条件 | 说明 |
|---------|------|
| Directory 变更 ≥ 64 次 | 批次写入，减少 Anchor 擦写 |
| Checkpoint 时 | 与 L2P flush 同步 |
| ftl_service(IDLE) | 空闲时保证最新 |

#### 启动加载 Anchor

```c
int metadata_dir_load(metadata_dir_cache_t *dir) {
    /* 扫描两个 Anchor Block 各 3 个冗余页 */
    metadata_directory_t candidates[6];
    for (int i = 0; i < 6; i++) {
        fc_read_page(anchor_paa(i), &candidates[i], sizeof(metadata_directory_t));
    }

    /* 选 magic 正确 + sequence 最大的（完整性由 FC per-CW CRC + ECC 保证，v0.62） */
    metadata_directory_t *best = NULL;
    for (int i = 0; i < 6; i++) {
        if (candidates[i].magic != 0x414E4449) continue;
        if (!best || candidates[i].sequence > best->sequence)
            best = &candidates[i];
    }

    if (!best) return SSD_STATUS_IO_ERROR;  // 无有效 Anchor

    /* 重建 SRAM cache */
    dir->count = best->entry_count;
    dir->anchor_sequence = best->sequence;
    for (int i = 0; i < dir->count; i++) {
        dir->entries[i].type = best->entries[i].type;
        dir->entries[i].id = best->entries[i].id;
        dir->entries[i].paa = best->entries[i].paa;
    }
    dir->dirty = false;
    return SSD_STATUS_OK;
}
```

### 5.6 双 Plane Region 交错与 flush 加速

> ⚠ **本节为早期设计示意，未实现**。文中 `l2p_cache_flush_pair()` / `region_to_plane()` /
> `allocator_get_next_metadata_page()` / `flush_single_region()` 在代码中**都不存在**；
> 现实现是 region **逐条经 `metadata_dir_append_slot` 追加到日志块**（P0-2 统一写模型），
> region→plane 由 `ftl_l2p_region_placement`（`include/ftl/ftl_config.h`）决定，
> **没有 flush 配对**，也就没有文中所称的「翻倍 / 4×」。保留本节仅作历史思路参考。

#### 背景

传统的 L2P region 写回固定在 Plane 0 操作，浪费 Multi-Plane 能力：

```
传统（单 Plane）:
  Plane 0: [R0][R1][R2][R3]...   64 次 flush = 64 × 300μs = 19.2ms

企业级（双 Plane 交错）:
  Plane 0: [R0][R2][R4][R6]...   ┐  32 次 Multi-Plane
  Plane 1: [R1][R3][R5][R7]...   ┘  = 32 × 300μs = 9.6ms
  → 写回速度翻倍！
```

#### Region 到 Plane 的映射

```c
/* Region 0 → Plane 0, Region 1 → Plane 1, Region 2 → Plane 0, ... */
static uint8_t region_to_plane(uint16_t region_id) {
    return (uint8_t)(region_id & 1U);
}
```

偶数 Region 固定在 Plane 0，奇数在 Plane 1，保证任意相邻两次 flush 可配对。

#### Cache Flush 的 Multi-Plane 优化

```c
/* 在 §6.2 的 Cache evict 和 background flush 中使用 */
void l2p_cache_flush_pair(l2p_cache_t *cache) {
    /* 找两个 DIRTY region，分别在 Plane 0 和 Plane 1 */
    int p0_idx = -1, p1_idx = -1;

    for (int i = 0; i < L2P_CACHE_SIZE; i++) {
        if (cache->lines[i].state != L2P_CACHE_DIRTY) continue;
        if (region_to_plane(cache->lines[i].region_id) == 0)
            p0_idx = i;
        else
            p1_idx = i;
        if (p0_idx >= 0 && p1_idx >= 0) break;
    }

    if (p0_idx >= 0 && p1_idx >= 0) {
        /* 两页同 page#，不同 plane → Multi-Plane PROGRAM */
        nand_paa_t paa0 = metadata_dir_lookup(&g_metadata_dir,
                           METADATA_TYPE_L2P_REGION,
                           cache->lines[p0_idx].region_id);
        nand_paa_t paa1 = metadata_dir_lookup(&g_metadata_dir,
                           METADATA_TYPE_L2P_REGION,
                           cache->lines[p1_idx].region_id);

        /* 确保 page# 相同且分属不同 plane */
        uint8_t page = allocator_get_next_metadata_page(
                         &g_alloc, 0, 1);

        /* 构造 Multi-Plane 命令 */
        fc_command_t *cmd = fc_command_alloc();
        cmd->config.operation = OP_MULTI_PLANE_PROGRAM_PAGE;
        // desc[0]: Plane0 region data
        // desc[1]: Plane1 region data
        fc_submit(cmd);

        cache->lines[p0_idx].state = L2P_CACHE_CLEAN;
        cache->lines[p1_idx].state = L2P_CACHE_CLEAN;

    } else if (p0_idx >= 0) {
        /* 只有一个 Plane 有 DIRTY → 单 Plane fallback */
        flush_single_region(cache, p0_idx);
    } else if (p1_idx >= 0) {
        flush_single_region(cache, p1_idx);
    }
}
```

#### 性能对比

| flush 方式 | 每次耗时 | 64 次 flush | 加速比 |
|-----------|---------|------------|--------|
| 单 Plane 逐次 | 300μs | 19.2ms | 1x |
| 双 Plane 交错 Multi-Plane | 300μs | 9.6ms | **2x** |
| 批量 4 条（每次 2 对） | 4×300μs | 4.8ms | **4x** |

#### Directory 与双 Plane 的联动

```
启动时：
  ① 读 Anchor → Metadata Directory → 知道每 Region 的位置
  ② 偶 region → P0, 奇 region → P1（交错分配）
  
写入时：
  flush(a) → metadata_dir_update(region_a, new_paa)
  flush(b) → metadata_dir_update(region_b, new_paa)
  
读回时：
  cache_miss(region_x) → metadata_dir_lookup(region_x) → 直接读
```

Metadata Directory 是双 Plane 交错的基础——没有 Directory 就不知道
Region 当前在哪，也就无法利用双 Plane 并行。

---

## 6. SRAM Cache 设计

### 6.1 数据结构

```c
#define L2P_ENTRIES_PER_REGION   (512)                     // region 内 PBA 数（= PAGE_MAIN_SIZE/4）
#define L2P_REGION_SIZE          (L2P_ENTRIES_PER_REGION * sizeof(uint32_t))  // 2KB
#define L2P_REGIONS_PER_INSTANCE (2048)                     // 每 (ch,ce,lun) 表容量（分片：每实例一份完整表）
#define L2P_REGION_PER_BLOCK     (FTL_SLOTS_PER_BLOCK)      // 每块槽位 = PAGES_PER_BLOCK × PLANE_NUM = 128
#define L2P_BLOCKS               ((L2P_REGIONS_PER_INSTANCE + L2P_REGION_PER_BLOCK - 1U) / L2P_REGION_PER_BLOCK)  // = 16
#define L2P_CACHE_SIZE           (64)                       // 可配置：32/64/128
#define FTL_PBA_INVALID          (0xFFFFFFFFU)              // 无效 PBA，全 F 不可能为合法地址

typedef enum {
    L2P_CACHE_INVALID = 0,      // cache 行无效（冷启动或已驱逐），entries 全 FTL_PBA_INVALID
    L2P_CACHE_CLEAN,            // cache 行有效，未修改
    L2P_CACHE_DIRTY,            // cache 行有效，已修改（需写回）
    L2P_CACHE_FLUSHING,         // 正在写回 NAND（不可驱逐）
} l2p_cache_state_t;

typedef struct {
    nand_pba_t         entries[L2P_ENTRIES_PER_REGION];   // 512 条 PBA = 2KB
    uint16_t           region_id;                          // 0..2047
    uint8_t            state;                              // l2p_cache_state_t
    uint16_t           access_count;                       // CLOCK 访问计数（~0 时可被驱逐）
} l2p_cache_line_t;

typedef struct {
    l2p_cache_line_t lines[L2P_CACHE_SIZE];                // 64 × (2KB + 4B) ≈ 128 KB
    uint16_t         clock_hand;                           // CLOCK 替换指针
    uint32_t         hit_count, miss_count;                // 统计（开机累计，观测用）

    /* Thrashing 检测窗口（v0.53，见 §6.5）*/
    uint32_t         window_access;                        // 本窗口累计访问数
    uint32_t         window_miss;                          // 本窗口累计 miss 数
    uint8_t          miss_pct;                             // 最近一次窗口结算 miss 率
    uint8_t          thrashing;                            // 1 = 处于降级态（滞回保持）

    /* Region 级脏标记（F4）：自上次 checkpoint 以来该 region 是否被写过 */
    uint32_t         region_dirty_bitmap[L2P_REGIONS_PER_INSTANCE / 32U];

    /* 本 cache 绑定的拓扑与外部指针（由 l2p_cache_set_md_dir 注入）*/
    uint8_t          ch, ce, lun;
    metadata_dir_cache_t *md_dir;                          // Region 位置查找
    ftl_allocator_t      *alloc;                           // Region 动态分配
} l2p_cache_t;
```

### 6.2 查找流程

Cache 对外只暴露 `l2p_cache_read()` 和 `l2p_cache_write()` 两个入口，
evict 和 region load 是内部实现，不对外暴露接口。

```
l2p_cache_read(cache, lba):
    region_id = lba / 512
    entry_idx = lba % 512

    ═══ Cache hit ═══
    遍历 lines:
      line->region_id == region_id 且 line->state >= L2P_CACHE_CLEAN
        → line->access_count++
        → return line->entries[entry_idx]

    ═══ Cache miss ═══
    CLOCK 逐 line 检查（从 clock_hand 开始循环查找）:
      state == FLUSHING   → 跳过（写回中，不可驱逐）
      access_count > 0    → access_count--，检查下一个
      state == DIRTY      → 选中为 victim（后续需 flush）
      state == INVALID    → 直接占用

    选中 victim line 后:
      victim 是 DIRTY → l2p_cache_flush_one(victim_idx)
        走 metadata_dir_append_slot 取 L2P **日志块**下一空槽做整页追加（**只写擦除页**），
        成功后 metadata_dir_commit_region 登记；分配失败返回 FTL_ERROR，
        **绝不回退固定位置**（固定层是哨兵，永不写入）
      l2p_cache_find_or_load：从 NAND 加载 region
        （位置来自 metadata_dir_lookup：初始指向哨兵层固定页，刷新后指向日志块；
         固定位置回退仅作 md_dir 缺失兜底——正常路径不可达；sync_read_page）
      victim->region_id = region_id
      victim->state     = 加载成功 L2P_CACHE_CLEAN / 失败 L2P_CACHE_INVALID
      victim->access_count = 1
      return victim->entries[entry_idx]（加载失败时 entries 全 FTL_PBA_INVALID）
```

### 6.3 更新流程（Write-back）

```c
l2p_cache_write(cache, lba, pba) {
    // 查找或加载 region（同 l2p_cache_read 的 cache miss 逻辑）
    line = internal_find_or_load(cache, region_id);
    // 修改 entry
    line->entries[entry_idx] = pba;
    // 标记 dirty（延迟写回 NAND）
    line->state = L2P_CACHE_DIRTY;
}
```

### 6.4 CLOCK 替换算法

```
逐出流程：

  ① clock_hand 指向当前检查的 line
  ② line->state == FLUSHING → 跳过（写回中）
  ③ line->state == DIRTY    → 返回该 line（调用者 flush 后再用）
  ④ line->state == INVALID  → 直接占用
  ⑤ access_count > 0        → access_count--，检查下一个（给第二次机会）
  ⑥ access_count == 0       → 回收

CLOCK 是 LRU 的近似。嵌入式环境无动态内存分配，CLOCK 只需一个整数字段。
命中率接近 LRU，驱逐 O(1) amortized。

#### 未来升级方向（标记待定）

以下为 CLOCK 的优化方向，当前阶段不实现，留作后续 Phase 3 优化选项：

| 升级方向 | 说明 | 触发条件 | 预期收益 |
|---------|------|---------|---------|
| **CLOCK-pro** | access_count 从固定位宽改为动态阈值，热数据更易留在 cache 中 | Phase 3 性能优化 | 命中率 +3~5% |
| **自适应模式切换** | 检测到 sequential 访问时临时启用 prefetch（预加载后续 region）；随机访问时回到 CLOCK 保守模式 | Sequential 检测 | 顺序读场景命中率接近 100% |
| **Thrashing 降级** ✅ v0.53 已落地 | 窗口化 miss 率 ≥ 90% 时停 NORMAL 模式的**主动**后台 flush（**非** write-through，见 §6.5）| miss_rate > 90% | 避免性能断崖 |
```

### 6.5 Thrashing 检测（v0.53 落地）

工作集远大于 cache（64 行 × 512 entry = 16 MiB LBA 空间，而盘 512 MiB）时，全盘随机访问
几乎每次 miss ⇒ 「evict→flush→load」反复。设计初稿列了三条降级策略，**逐条对照本仓实现后
只保留一条**（另两条在本架构下不成立，见下）。

#### 检测：窗口化 miss 率 + 滞回

累计 `hit_count`/`miss_count` 随开机时长渐近，反映不了**当前**行为，降级判定必须看窗口：

```
每 L2P_THRASH_WINDOW (256) 次访问结算一次：miss_pct = window_miss × 100 / window_access
  进入：miss_pct ≥ L2P_THRASH_ENTER_PCT (90%)
  退出：miss_pct <  L2P_THRASH_EXIT_PCT  (70%)   ← 滞回，避免阈值附近反复翻转
```

命中/未命中在 `l2p_cache_find_or_load` 里各记一次（`l2p_cache_note_access`），状态**翻转**
时才打一行 `[L2P] thrash=… miss=…% hit=… miss_total=…`（抖动不刷屏）。

#### 降级：停 NORMAL 模式的**主动**后台 flush

`ftl_service_normal` 原本每轮主动 flush 最多 4 条脏行。thrashing 下这些主动 flush 大概率是
白写——被 flush 的行往往在下次访问前就被驱逐，届时还要再 flush 一次；同时挤占前台 IO 的
NAND 带宽。降级 ⇒ 停主动 flush，**脏行只由驱逐路径按需写回**。

> **持久性不受影响**：L2P 更新在写完成路径里**先入 Journal**（`journal_append` → 再
> `l2p_cache_write`），掉电可回放；`checkpoint_create` 也无条件 flush 全部脏 region
> （`checkpoint_flush_dirty_regions`）。故「脏行滞留 RAM」不比原设计多任何丢失窗口。

#### 刻意不实现的两条（初稿策略 1 / 2）

| 初稿策略 | 不实现的原因 |
|---|---|
| **write-through**（避免 dirty 写回） | 本仓 L2P 写模型是**整 region 页追加**：改 1 个 entry 也要写回整页 2KB ⇒ 无论 write-back 还是 write-through，都要先把 region **读进来**（RMW）再写一次。write-through 省不掉那次 load，反而放弃 write-back 的**合并收益**（同 region 多次写合并为一次 flush）⇒ 净亏。 |
| **增大 batch / 预取连续 region** | 预取依赖**空间局部性**，而 thrashing 的成因恰恰是**随机**访问（无局部性）⇒ 预取下一个 region 只会多读一页。预取属于**顺序**场景的优化，与 thrashing 正交。 |

#### 未做的更深一层（登记，非本轮）

DRAM-less 的行业解法之一是让读路径**先查 Journal overlay**（Journal 就是 L2P 的增量日志），
从而在 thrashing 下少 load region。本仓 `l2p_cache_read` 不查 Journal ⇒ 属独立改造，不在 P1-4。

---

## 7. Journal 与掉电恢复

### 7.1 为什么需要 Journal

Write-back cache 的致命问题：掉电时 dirty cache 丢失，L2P 映射无法恢复。

```
时序：
  ① FTL 写数据到 NAND ✅（数据在 NAND 上安全）
  ② l2p_cache_update(LBA, new_ppa) → cache dirty
  ③ [掉电] ❌（cache 内容未写回 NAND L2P 区域）

后果：数据在 NAND 上，但 L2P 映射丢失 → 数据永远不可达
```

### 7.2 Log-structured Journal

Journal 使用**固定位置**存储在元数据区，不通过 Metadata Directory 管理：

```
元数据区 Block 分配（固定，由 ftl_config.h 公式算出，非手写）：
  Block 0..15:  L2P Table（哨兵层，永不写入）
  Block 16..47: Anchor 池（32 块，Metadata Directory 的块轮转日志池）
  Block 48..55: Checkpoint 池（8 块）
  Block 56..57: BBT（2 块）
  Block 58..61: Journal（4 块 = CW_NUM，环形 + 擦除）
  Block 62:     Wear（1 块，软状态单副本）
  Block 63..:   用户数据（FTL_METADATA_BLOCK_END 之后）

Journal 块 = JOURNAL_BLOCK0 + b（JOURNAL_BLOCK0 = 58）；块数由 JOURNAL_BLOCK_COUNT
= CW_NUM 决定。启动时扫描这 4 块确定当前写入位置（无固定"J0/J1"双块概念）。
```

```
每次 L2P 修改前，先在 Journal 中追加一条记录：

┌──────────────────────────────────────────────────┐
│ Journal Entry（16B）                              │
│ ┌────────────────────────────────────────────┐   │
│ │ LBA (4B) | old_PBA (4B) | new_PBA (4B)    │   │
│ │         | sequence (4B)                    │   │
│ └────────────────────────────────────────────┘   │
│                                                   │
│ Journal 区域（4 块池 = CW_NUM，环形 + 擦除）：      │
│ Page 0: [hdr 12B][entry_0]...[entry_126]          │
│ Page 1: [hdr 12B][entry_127]...                   │
│                                                   │
│ 每个 NAND Page = 页头 12B + 127 entries × 16B      │
│   (2048 − 12) ÷ 16 = 127（写死 128 会写穿 page_buf）│
│ 写满一页才提交 NAND（批量化）                      │
│ Journal 满后配合 Checkpoint 截断回收               │
└──────────────────────────────────────────────────┘
```

### 7.3 写入流程（完整序列）

> ⚠ **订正**：真实写入分**两段**——提交路径（`ftl_submit_write`）只分配 PBA + 提交 FC
> 命令；Journal / L2P / 位图更新在**完成路径**（`ftl_handle_completion`）里、
> NAND 确认写成功之后才做（**modify-after-confirm**）。原伪代码把 journal_append /
> l2p_cache_write 写在提交路径，且 `allocator_alloc` / `ftl_cmd_setup_ppa` 等函数名均已过期。

```c
/* ── 提交路径：ftl_submit_write（非阻塞）── */
ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req) {
    nand_pba_t new_pba[PLANE_NUM], old_pba;

    /* ① 分配新 PBA（**逐 plane 独立选块**，Policy B）。Free Pool 耗尽 → 同步跑一轮
     *    紧急 GC（gc_select_victim → gc_migrate_sb → gc_erase_and_free）后重试一次 */
    if (allocator_alloc_pair(&ftl->alloc, FTL_TEMP_HOT, new_pba) == NAND_PBA_INVALID) {
        /* …紧急 GC 回收 1 个 SB 后重试；真无空间才返回 FTL_ERROR… */
    }

    /* ② 读旧 PBA（供 Journal 记录用）*/
    old_pba = l2p_cache_read(&ftl->cache, internal_lba);

    /* ③ 配置 FC 写命令（1 或 2 plane）+ 逐 plane PBA + desc 链 + 数据 */
    cmd->config.operation = (req->pages == 1U) ? OP_NORMAL_PROGRAM_PAGE
                                               : OP_PROGRAM_PAGE_MULTI_PLANE;
    cmd->config.crc    = FC_CRC_ENABLE;
    cmd->config.random = FC_RANDOM_ENABLE;
    cmd->config.ldpc   = FC_ECC_MODE_BCH;
    cmd->module        = FC_COMMAND_MODULE_FTL;
    cmd->meta          = (DWORD)(internal_lba << CW_WIDTH);   // 扇区 LBA（v24）
    ftl_cmd_setup_ppa_pair(cmd, new_pba);
    ftl_cmd_write_data(cmd, req->buffer, data_size);
    fc_submit(cmd);                                           // 非阻塞
    return FTL_OK;
}

/* ── 完成路径：ftl_handle_completion（NAND 确认写成功后，**逐 CW** 簿记）──
 * ⚠ pending **不存** old_pba：旧 PBA 在完成时按 LBA 重查 L2P（v0.58 起）。 */
for (k = 0U; k < pending->n_cw; k++) {
    lba_t      l    = pending->lba + k;
    nand_pba_t page = (ftl_lba_plane(l) == 0U) ? pending->new_pba : pending->new_pba1;
    nand_pba_t newp = pba_with_cw(page, ftl_lba_cw(l));
    nand_pba_t oldp = l2p_cache_read(&ftl->cache, l);

    journal_append(&ftl->journal, l, oldp, newp);
    l2p_cache_write(&ftl->cache, l, newp);
    ftl_block_add_valid(&ftl->alloc.blocks[ftl_pba_gblock(newp)]);      /* VPC：新 CW +1 */
    if ((oldp != NAND_PBA_INVALID) && (oldp != newp)) {
        ftl_block_sub_valid(&ftl->alloc.blocks[ftl_pba_gblock(oldp)]);  /* 覆盖写：旧 CW −1 */
    }
}
ftl->total_writes++;
/* ⚠ v0.60 后**无位图**：有效性唯一真值 = 逐 CW 的 valid_count（=OOB 反查 L2P）*/
```

### 7.4 启动恢复

```
冷启动（无 journal）:
  l2p_cache_init(cache, ch, ce, lun) → cache 全 INVALID（v0.35 起绑定拓扑）
  首次访问时从 NAND 加载 region

热启动（有 journal）:
  ① 扫描 Checkpoint 区域 → 加载最新 Checkpoint
  ② 回放 Journal（sequence > checkpoint 的所有条目）
  ③ Cache 中所有 line 标记 DIRTY
  ④ 后台 flush dirty regions 到 NAND L2P 表
```

### 7.5 Journal 回收（与 Checkpoint 协同）

#### 7.5.1 核心问题

Journal 是环形多 Block（初稿写双 Block J0 ↔ J1，**实现为 4 块**），写满后需要回切。
但旧 Block 中的条目在 Checkpoint 截断之前不能安全擦除：

```
时序：
  Phase 1: J0 写满 64 页（8192 条 entry，seq=1~8192）→ 切 J1
  Phase 2: J1 写满 → 需回切 J0
  Phase 3: J0 中还有 seq=1~8192 → 不能擦！
  Phase 4: 必须先 Checkpoint（sequence ≥ 8192）→ 截断 → 擦 J0 复用
```

**Checkpoint 的作用**：记录一个全局 sequence 号 checkpoint_seq，
Journal 中 sequence ≤ checkpoint_seq 的条目已写入 L2P table，可回收。

> ⚠️ **实现订正（v0.54）**：本仓 Journal 是 **4 块**环形（`JOURNAL_BLOCKS = CW_NUM = 4`，
> 块 58-61），非本节初稿的「双 Block J0↔J1」（设计自荐的「2→4」早已落地）。
> 环形块数见 `ftl/ftl_config.h`；下面的时序/容量推导按 2 块写，**数值**需 ×2。
>
> **环形块擦除（v0.54 P1-6）**：块的物理擦除有**唯一**一点——`journal_flush_page` 回切进入
> 新块时擦（`journal.c`）。`journal_truncate` 只**预擦除**环形最旧块并记账
> （`pre_erased_block`），使回切时跳过擦除 ⇒ 同一块每圈**只擦一次**。旧实现在 truncate 与
> flush_page 各擦一次 = **双倍磨损**，直击 §14.1 的 Journal 寿命瓶颈（凭空多 ~25%）。
> `up_to_sequence` 不再是死形参：水位不足以覆盖全部已写条目时**拒绝截断**（最旧块可能仍持有
> 回放需要的条目）。
>
> **更新合并（v0.54 P1-6）**：`journal_append` 在当前页缓冲里遇到**同一 LBA** 时就地更新
> （只改 new_pba，不新增条目、不消耗 sequence）⇒ 覆盖写工作集下减少 Journal 页数。

#### 7.5.2 触发条件（三级触发）

企业级 SSD（三星/铠侠）的 Journal 回收使用多级触发策略，
核心原则是**前台 IO 不能被后台任务阻塞**。

**条件 1：水位线触发（Journal 容量预警）**

由系统最坏情况 NAND 带宽预算推导：

```
已知：
  - Journal 容量：初稿 2 Block × 64 页 × 128 条 = 16,384 条
    （**实现** 4 Block × 64 页 × 127 条 = 32,512 条 ⇒ 下列余量只增不减）
  - NAND PROGRAM：~300μs/页（批量 flush 4 条 dirty cache 约 1.2ms）
  - Checkpoint 需要 flush 最多 64 条 dirty cache
  - 最坏情况：BUSY 模式下 Checkpoint 被延迟

推导 85%（JOURNAL_FULL_THRESHOLD）：
  剩余空间 = 15% × 256 页（4 块）≈ 38 页
  38 页 × 127 条 ≈ 4,826 条 entry 的写入时间
  2,432 次 ftl_write 期间 ftl_service 有足够多轮次分批完成 Checkpoint（4 块下余量翻倍）
  → 85% 安全

推导 95%（JOURNAL_CRITICAL）：
  剩余空间 = 5% × 256 页（4 块）≈ 13 页 ≈ 1,651 条 entry
  1,651 次写入 ≈ 1,651 × 300μs ≈ 495ms
  这是最后防线——230ms 内必须完成 Checkpoint
  触发后在 BUSY 模式下也强制执行
```

```c
#define JOURNAL_FULL_THRESHOLD   (85)      // 水位线触发（%）
#define JOURNAL_CRITICAL         (95)      // 紧急水位（阻塞触发）

/* 真实函数（checkpoint.h）：checkpoint_journal_watermark(l2p_journal_t *journal)
 * 在写完成路径调用，按占用率设 journal->emergency / ->checkpoint_needed */
void checkpoint_journal_watermark(l2p_journal_t *journal) {
    uint8_t used = (uint8_t)(((uint32_t)journal->current_block * PAGES_PER_BLOCK
                              + journal->current_page) * 100U
                             / (JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK));

    if (used >= JOURNAL_CRITICAL)
        journal->emergency = 1U;            // 强制 Checkpoint，前台可能等待
    else if (used >= JOURNAL_FULL_THRESHOLD)
        journal->checkpoint_needed = 1U;    // 设标志，后台执行
}
```

**条件 2：写入量触发（CP_WRITE_INTERVAL）**

由 Journal 容量与安全余量推导：

```
Journal 总容量 = 32,512 条（4 块 × 64 页 × 127 条）
CP_WRITE_INTERVAL = 1,024 次（当前值，见 ftl.h）
  ⇒ 两次 Checkpoint 之间 Journal 至多增长 ~1,024 条，远小于 32,512 条容量
  ⇒ Journal 有 ~30 个 CP 周期的余量（CP 连续失败多轮也不至于写满）

消费级 DRAM-less 典型值 1,024~4,096 writes。
企业级（有 DRAM Journal buffer）可达 100k~1M。
```

```c
#define CP_WRITE_INTERVAL        (1024U)   // 写入量触发
```

**条件 3：定时触发（CP_TIME_INTERVAL_MS）**

Host 写入量很小时，条件 2 不会触发，但 Journal 中可能有少量残留。
定时 Checkpoint 保证下次启动回放量有限：

```
场景：
  Host 每 30 秒写 1 条数据
  条件 2 永远不会触发（1024 条写入要 ~0.85 小时）
  Journal 可能积累几百条
    
定时触发（10 秒）：
  即使只写了几条，也会 Checkpoint
  下次启动 Journal 几乎为空
```

```c
#define CP_TIME_INTERVAL_MS      (10000)   // 定时触发（10 秒）
```

#### 7.5.3 Checkpoint 执行流程

> ⚠ **订正**：真实函数是 `checkpoint_create(struct ftl_context *ftl)`（返回
> `ftl_status_t`），**无** `ftl_checkpoint` / `checkpoint_write` / `timer_get_ms`。

```c
ftl_status_t checkpoint_create(struct ftl_context *ftl) {
    /* ① flush 所有 dirty cache → L2P 日志块（整页追加）*/
    l2p_cache_flush_all(&ftl->cache);

    /* ② 写 Checkpoint record（含 sequence + region_bitmap）到 Checkpoint 池下一槽 */
    /* ③ journal_truncate（预擦除环形最旧块 + 记账，见 §7.5.1 订正）*/
    /* ④ ftl->cp_sequence = cp.sequence */
}
```

#### 7.5.4 在 ftl_service 中的决策

> ⚠ **订正**：真实函数是 `checkpoint_check_trigger(ftl, elapsed_ms)`（返回触发起因，
> 0=不需要）与 `checkpoint_is_emergency(ftl)`；水位由 `checkpoint_journal_watermark(&ftl->journal)`
> 在写完成路径设置。**无** `ftl_cp_check` / `ftl->mode` / `timer_elapsed_ms`。

```c
/* IDLE / NORMAL 模式下（BUSY 只走紧急分支）：*/
int checkpoint_check_trigger(const struct ftl_context *ftl, uint32_t elapsed_ms) {
    /* T1 紧急：checkpoint_is_emergency（journal ≥ 95%）→ 强制
     * T2 写入量：total_writes % CP_WRITE_INTERVAL(1024) == 0
     * T3 定时：elapsed_ms ≥ CP_TIME_INTERVAL_MS(10000)（仅 IDLE/NORMAL）
     * 返回触发起因位掩码，0 = 不触发 */
}

/* BUSY 模式（ftl_service_busy）：仅紧急分支 */
if (checkpoint_is_emergency(ftl)) {
    (void)checkpoint_create(ftl);
}
```

企业级"无阻塞原则"总结：

| 条件 | 执行模式 | 是否阻塞前台 IO |
|------|---------|----------------|
| 水位线（85%） | NORMAL / IDLE | 否 |
| 写入量（1024） | NORMAL / IDLE | 否 |
| 定时（10s） | NORMAL / IDLE | 否 |
| 紧急（95%） | 任何模式 | ⚠️ 可能阻塞 |

#### 7.5.5 参数汇总

| 参数 | 值 | 推导依据 |
|------|----|---------|
| CP_WRITE_INTERVAL | 1,024 writes | 实现 4 块 × 64 页 × 127 条 = 32,512 条容量 ⇒ ~30 倍余量 |
| JOURNAL_FULL_THRESHOLD | 85% | 剩余 38 页（4 块）够分批做完 Checkpoint |
| JOURNAL_CRITICAL | 95% | 剩余 13 页，~495ms 最后防线 |
| CP_TIME_INTERVAL_MS | 10,000 ms | Host 静负载保底 |
| FLUSH_BUDGET (NORMAL) | 4 line/轮 | 4 × 300μs ≈ 1.2ms，不阻塞前台 |
| FLUSH_BUDGET (IDLE) | 全部 | NAND 空闲，全速 flush |

### 7.6 Journal 自身可靠性（掉电保护）

Journal 保护 L2P 映射不丢失，但 Journal 本身也写在 NAND 上。写 Journal 时掉电怎么办？这是 Journal 设计的"递归可靠性问题"。

#### 三种掉电场景

```
Journal Page PROGRAM 的三个阶段：

  ① SRAM buffer 中累积 JOURNAL_ENTRIES_PER_PAGE 条 entry（未写 NAND）
     掉电 → 最多丢失该条数（**实现 127 条**）                ← 掉电点 A

  ② FC PROGRAM 提交，NAND 开始烧录（~300μs）
     半途掉电 → 页面上数据不完整                           ← 掉电点 B

  ③ PROGRAM 成功，但返回途中掉电
     FTL 不知是否写入成功                                  ← 掉电点 C
```

#### 四层保护（不需要双写）

本方案的四层保护，充分利用 FC 层已有的 CRC + BCH ECC，不需要双写（2×写放大）：

```
Layer 1: FC per-CW CRC + BCH ECC（FC 层自带，零 FTL 侧开销）
  ────────────────────────────────────────────────────
  每次 FC PROGRAM/READ 都经过 BCH ECC（t=4）+ 逐 CW CRC-32。
  ECC 可纠正 ≤4 bit 的错误，超过时返回 UNC；CRC 覆盖整 CW（META+USER）且
  自身也在 ECC 码字内 ⇒ 消除"ECC 误纠后看似成功"的假阳性。
  ⚠ v0.62 起元数据页（Journal/Checkpoint/Anchor/BBT/Wear/L2P-Region）的
    FC CRC 由 FTL_PAGE_CFG_META 统一**开启**（此前为关闭）；FTL 记录内不再
    自算 CRC —— 校验和下沉到下层，与 DRAM-less 主流（dhara）一致。
  掉电点 B 导致的半烧录页面 → 少量 bit 错 → ECC 自动修 ✅
                              超过纠错能力 → UNC / CRC 不符 → 整页丢弃 ✅

Layer 2: Header magic + entry_count 精确定位
  ────────────────────────────────────────────────────
  Header 带 magic（"JOUR"）与 entry_count（本页有效 entry 数 0~127）：
    magic 不符 → 整页无效（含擦除态全 0xFF）✅
    恢复时只解析前 entry_count 条，之后的忽略 ✅

Layer 3: 单调递增 sequence
  ────────────────────────────────────────────────────
  每个 entry 有连续递增的 sequence 号
  恢复时检测连续性：gap → 截断于此
  即使整页 CRC 假阳性 → sequence 不连续也能检测 ✅

Layer 4: 多 Block 轮换（不是双写！）
  ────────────────────────────────────────────────────
  Block J0 写满 → 切 J1 → J0 擦除
  恢复时读各块最后一页，取 sequence 大的
  无需双写代价，只需多读一页 ✅
```

#### Journal Page 完整格式

```
每个 Journal Page（2048B = 1 NAND page = 4 CW × 512B）：

┌─────────────────────────────────────────────────────────┐
│ Header: 12B                                              │
│  ├── magic       (4B) = 0x4A4F5552 ("JOUR")            │
│  ├── page_seq    (4B) = 本页第一条 entry 的全局序号      │
│  ├── entry_count (2B) = 本页有效 entry 数（0~127）       │
│  └── block_seq   (2B) = 本 Journal Block 写入轮次       │
├─────────────────────────────────────────────────────────┤
│ Entries: N × 16B （N = entry_count，≤ 127）              │
│  ├── entry[0]: lba(4B) | old_pba(4B) | new_pba(4B)     │
│  │         : seq(4B) = 单调递增全局序号                  │
│  ├── entry[1]                                            │
│  ├── ...                                                 │
│  └── entry[N-1]                                          │
├─────────────────────────────────────────────────────────┤
│ Padding: (127 - N) × 16B = 0xFF（未使用区域）            │
└─────────────────────────────────────────────────────────┘
注意：Header + Entries 整页由 FC 的 per-CW CRC + BCH ECC 保护（写入 4 CW；v0.62 起 CRC 开启）。
      Entry 本身无独立 CRC（16B = lba + old_pba + new_pba + seq 全占满），
      完整性靠整页 CRC+ECC + page_seq 单调性判定。
```

#### entry_count 应对掉电点 B

```
掉电发生在 PROGRAM 中途（掉电点 B）：
  NAND 页面部分烧录 → entry_count 可能是正确值或 0xFF

恢复时读该页：
  FC READ → BCH ECC 解码
    ├── 成功  → entry_count 可信 → 只解析前 entry_count 条 ✅
    └── 失败  → 整页无效 → 只用上一页 ✅
```

#### 企业级做法参考

```
企业级 SSD 有硬件掉电保护方案：

三星/铠侠：
  Journal 有独立电源轨 + 大电容（~5ms）
  检测到掉电 → 电容支撑完成最后一次 PROGRAM
  → "掉电点 B" 在硬件层面消除

消费级 DRAM-less：
  无掉电电容 → 纯软件方案
  → 方案与本节设计一致：ECC + CRC + sequence + batch flush + 定时 flush

UFS/eMMC：
  硬件加速 Journal（Replay Protected Memory Block）
  硬件保证 write atomicity
```

#### 本方案总结

| 保护层 | 实现 | 成本 | 保护能力 |
|--------|------|------|---------|
| BCH ECC (t=4) | FC 层自带 | 0 | 纠正 ≤4 bit，检测更多 ✅ |
| FC per-CW CRC | FC 层自带（v0.62 起元数据页开启） | 0（4B/CW，FC 内部） | 校验整 CW，消除 ECC 误纠 ✅ |
| entry_count header | Header 2B | ~0 | 精确定位有效 entry ✅ |
| Monotonic sequence | 已有字段 | 4B/entry | 检测 gap 和乱序 ✅ |
| 多块轮换 | 4 Block 轮流写 | ~0 | 避免双写代价 ✅ |
| 双写 | ❌ 不需要 | — | ECC 四层已足够 |

---

## 8. Checkpoint 机制

### 8.1 为什么需要 Checkpoint

Journal 恢复的缺点：如果 journal 很长，启动回放时间长。Checkpoint 定期保存 L2P 快照，截断 journal。

### 8.2 设计

```c
typedef struct {
    uint32_t magic;                 // 0xCCCCCCCC
    uint32_t sequence;              // 单调递增（最新有效）
    uint32_t timestamp;             // 创建时间
    uint32_t region_count;          // 本 checkpoint 包含的 region 数
    uint32_t region_bitmap[64];     // 2048 bit，哪些 region 在 checkpoint 中
    uint32_t journal_sequence;      // 截断到此 sequence 之前的 journal
    uint32_t layout_version;        // FTL_MEDIA_LAYOUT_VERSION（版本不符 ⇒ 记录作废）
    // 无 crc：完整性由下层 FC 的 per-CW CRC + ECC 保证（v0.62）
} l2p_checkpoint_header_t;
```

**全量 Checkpoint**：flush 所有 dirty cache → 写 header → 截断 journal
**增量 Checkpoint**：只 flush 变化的 region → 写 header（含 region bitmap）→ 截断 journal

### 8.3 启动恢复完整流程

本文档的 Boot 框架（`firmware_boot_stage_storage`）提供了启动阶段入口，
FTL 的恢复在此阶段完成。

#### 8.3.1 核心矛盾

Checkpoint 保存的是 L2P 全表（2048 regions = 4MB），
而 SRAM cache 只有 64 个 line（~131KB）。
**不能预加载全表，必须按需加载**。

因此 Checkpoint 的作用不是"把全表搬到 SRAM"，
而是**标记 Journal 回放的起点**——Journal 中 sequence ≤ cp_seq 的条目
已通过后台 flush 写入了 NAND L2P 表，无需回放。

#### 8.3.2 关机类型判断

```c
/* 真实入口（power_failure_recovery.c）：返回 ftl_status_t，结果经 result 输出 */
ftl_status_t ftl_boot_recovery(struct ftl_context *ftl, ftl_boot_result_t *result) {
    l2p_checkpoint_header_t cp;

    /* ① 扫描 Checkpoint 池（8 块）→ 最新有效 header */
    if (checkpoint_find_latest(&cp, ftl->topo) != FTL_OK) {
        return ftl_boot_fresh_start(ftl);            // 出厂首次启动
    }

    /* ② 扫 Journal 4 块池各页 header，取最大 page_seq
     *    （不调 journal_replay——后者会改 journal 内部状态）*/
    uint32_t journal_last_seq = 0;
    for (b = 0; b < JOURNAL_BLOCK_COUNT; b++)
        for (page = 0; page < PAGES_PER_BLOCK; page++)
            if (页 magic == JOURNAL_MAGIC && page_seq > journal_last_seq)
                journal_last_seq = page_seq;

    /* ③ 判断关机类型 */
    if (journal_last_seq <= cp.journal_sequence) {
        return ftl_boot_clean_start(ftl, cp.sequence);           // 干净关机
    } else {
        return ftl_boot_power_failure(ftl, &cp, result);         // 异常掉电
    }
}
```

#### 8.3.3 出厂首次启动（Fresh Start）

```c
ftl_status_t ftl_boot_fresh_start(struct ftl_context *ftl) {
    /* ① L2P Cache 全 INVALID（v0.35 绑定拓扑）*/
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ② 全盘重建 Closed SB 列表（F2）：把 main.c 回填进 Free Pool 的「已编程块」
     *    逐出交给 GC——否则首次分配会把新数据 AND 到旧数据上 */
    ftl_rebuild_stats_t rebuild_stats;
    (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);

    /* ③ Allocator / Journal 已由 ftl_init 初始化，此处不重复；
     *    Wear 表亦由 ftl_init 回填（P0-1）*/
    ftl->cp_sequence    = 0U;
    ftl->cp_timer_start = 0U;
    return FTL_OK;
}
```

启动时间 ≈ BBT 扫描（250ms）+ Allocator（50ms）≈ **~300ms**。

#### 8.3.4 干净关机启动（Clean Start）

条件：Journal 中所有条目的 sequence ≤ Checkpoint 的 journal_sequence，
即 Journal 已全部回放到 L2P table，无需回放。

```c
ftl_status_t ftl_boot_clean_start(struct ftl_context *ftl, uint32_t cp_sequence) {
    /* ① Cache 全 INVALID（不预加载任何 region，首次 miss 才按需从 NAND 加载）*/
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ② 全盘重建 Closed SB 列表（F2）：掉电前已满的 SB 只在 RAM 列表里，
     *    重启后必须全扫 NAND 重建 */
    ftl_rebuild_stats_t rebuild_stats;
    (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);

    /* ③ 写指针：干净关机时由 allocator 内存跟踪，无需扫 NAND */
    /* ④ Allocator / BBT / Wear 表均已由 ftl_init 初始化 */
    ftl->cp_sequence    = cp_sequence;
    ftl->cp_timer_start = 0U;
    return FTL_OK;
}
```

**关键设计**：Cache 全空，首次 Host IO 触发的 cache miss 才从 NAND
按需加载对应的 Region。不需要预加载 Checkpoint 中的数据。

启动时间 ≈ BBT 扫描（250ms）+ Allocator 重建（50ms）+ 写指针恢复（2ms）
≈ **~350ms**。

#### 8.3.5 异常掉电恢复（Power Failure Recovery）

条件：Journal 有未回放到 L2P table 的条目（sequence > cp_seq）。

```c
ftl_status_t ftl_boot_power_failure(struct ftl_context            *ftl,
                                    const l2p_checkpoint_header_t *cp,
                                    ftl_boot_result_t             *result) {
    uint32_t cp_sequence = cp->journal_sequence;

    /* ① Cache 全 INVALID */
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ② Journal 回放：所有 page_seq > cp_sequence 的页（F10），写入 cache（DIRTY）*/
    (void)journal_replay(&ftl->journal, &ftl->cache, cp_sequence);

    /* ③ 按 Checkpoint region_bitmap 局部加载（F4）：
     *    跳过 DIRTY 回放行；bitmap 置位区预载 CLEAN；未置位区 INVALID（懒加载 CP 快照）*/
    ftl_boot_apply_region_bitmap(ftl, cp);

    /* ④ 全盘重建 Closed SB 列表（F2）：顺带替代原写指针扫描 */
    ftl_rebuild_stats_t rebuild_stats;
    (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);

    /* ⑤ Controller → Ready（cache 中的 DIRTY 行由后台 flush 逐步写回 NAND）*/
    ftl->cp_sequence    = cp_sequence;
    ftl->cp_timer_start = 0U;

    /* ⑥ 输出结果 */
    result->boot_type       = BOOT_POWER_FAILURE;
    result->cp_sequence     = cp_sequence;
    result->journal_entries = entry_count;
    return FTL_OK;
}
```

#### 8.3.6 Journal 回放实现（`journal_replay`）

> ⚠ **订正**：不存在 `journal_replay_batched` / `journal_read_entries_since` /
> `sort_by_region` / SDRAM batch buffer。真实实现是**单个 `journal_replay`**
> （`journal.c`），两遍扫描：

1. **第一遍（收集）**：扫 `JOURNAL_BLOCK_COUNT × PAGES_PER_BLOCK = 256` 页，
   收集所有 `page_seq > cp_sequence` 的页到栈上 `replay_pages[]`
   （每项 8B，2KB 栈上数组；⚠ 下标须 `uint16_t`——256 > 255，`uint8_t` 会死循环）。
2. **第二遍（按序应用）**：按 `(page_seq, entry_index)` **递增序**回放，写入 L2P Cache
   并标 DIRTY。

> **F10 关键**：必须回放**所有** `page_seq > cp_sequence` 的页——早期版本只回放
> 最大一页，掉电跨 ≥2 页时其余条目**静默丢失**。

#### 8.3.7 写指针恢复（Write Pointer Recovery）

> ⚠ **订正**：原 `allocator_scan_write_pointer` 已不在主路径上。掉电/干净启动的
> Open SB 写指针恢复**由 F2 的 `ftl_rebuild_closed_sb_list` 全扫 NAND 一并完成**
> （它重建 Closed SB 列表，顺带重建 open SB 的写位置）。
> 仓内仍存在 `ftl_boot_scan_write_pointer`（逐页 RAW 读找首个空白页），但主启动
> 路径不调用它。`allocator_recover_write_pointer` / `paa_make` / `fc_read_page_raw`
> 这些旧名均已不存在。

#### 8.3.8 启动时间总结

| 阶段 | 干净关机 | 异常掉电 | 说明 |
|------|---------|---------|------|
| FW Stage: PLATFORM | ~10ms | ~10ms | 平台初始化 |
| FW Stage: MEMORY | ~5ms | ~5ms | 内存初始化 |
| FW Stage: IMAGE | ~2ms | ~2ms | 镜像验证 |
| FC Init + NAND Probe | ~50ms | ~50ms | FC 层初始化 |
| BBT 扫描 | ~250ms | ~250ms | 全量 4096 Block |
| Checkpoint 扫描 | ~1ms | ~1ms | 读最新 header |
| Journal 扫描 | ~1ms | ~1ms | 读最后 sequence |
| Journal 回放 | 0ms | ~100ms | 仅异常掉电需要 |
| Allocator 重建 | ~50ms | ~50ms | Super Block 列表 |
| Write Pointer | ~2ms | ~5ms | 干净取内存值，异常需扫描 |
| FW Stage: HOST | ~5ms | ~5ms | Host 初始化 |
| **总计** | **~370ms** | **~480ms** | |

异常掉电恢复后，Cache 中的 DIRTY lines 由后台 flush 逐步写回，
不阻塞 Controller → Ready。

#### 8.3.9 参数建议

> ⚠ **订正**：下表原列的 `BOOT_CLEAN_TIME` / `BOOT_DIRTY_TIME` / `JOURNAL_BATCH_SIZE` /
> `WRITE_PTR_SCAN_GRANULE` 四个常量**代码里都不存在**。真实实现是 `journal_replay` 两遍扫描
> （§8.3.6），回放页收集在**栈上** `replay_pages[256]`（2KB），无 SDRAM 批量 buffer；
> 写指针恢复由 F2 全扫 NAND 完成（§8.3.7），无独立扫描粒度常量。

| 参数 | 建议值 | 说明 |
|------|--------|------|
| BOOT_CLEAN_TIME | ~370ms | 干净关机启动时间目标（观测值，非编译期常量）|
| BOOT_DIRTY_TIME | ~500ms | 异常掉电恢复时间目标（观测值，非编译期常量）|
| Journal 回放 buffer | `replay_pages[256]`（栈，2KB）| 收集 `page_seq > cp_sequence` 的页，见 `journal.c` |

---

## 9. 架构决策记录

### ADR-1：选择纯页映射而非块映射/混合映射

| 项目 | 决策 |
|------|------|
| 选型 | **纯页映射（CW 级 512B）** |
| 理由 | 写放大最小（1.0~1.3x）、行业标准、与 FC 对齐 |
| 拒绝方案 | 块映射（WA 5~10x，QLC/TLC 寿命不够）、混合映射（2012 年前技术，已被淘汰）|
| 代价 | 表最大（4MB），需 SRAM cache + NAND 存储 |

### ADR-2：选择 DRAM-less L2P Cache

| 项目 | 决策 |
|------|------|
| 选型 | **NAND 存全表 + SRAM region cache** |
| 理由 | 学习行业主流嵌入式/UFS/eMMC 方案；有 1MB SRAM 可支持 64~128 regions |
| 拒绝方案 | 全表放 SDRAM（不是真正的 DRAM-less，降低学习价值）、page 级映射全表塞 SRAM（粒度粗，WA 大）|
| 代价 | 全盘随机访问时性能下降（命中率 1.6~6.3%）|

### ADR-3：RAID 不实现

| 项目 | 决策 |
|------|------|
| 选型 | **不实现 RAID（CONFIG_FC_RAID = 0）** |
| 理由 | RAID 增加 GC 复杂度、parity block 管理复杂、SLC 的 BER 较低不需要 |
| 未来 | 如需数据保护增强，优先考虑加强 ECC |

### ADR-4：FTL 通过 FC 接口访问 NAND

| 项目 | 决策 |
|------|------|
| 选型 | **FTL → FC 接口 → NAND** |
| 理由 | 经过 CRC/ECC/Random 完整数据保护；与 FC 异步模型集成 |
| 代价 | 每次读写需 alloc/submit/poll/release 序列，代码量增加 |

### ADR-5：CLOCK 替换策略

| 项目 | 决策 |
|------|------|
| 选型 | **CLOCK 算法** |
| 理由 | 命中率接近 LRU，无需链表/内存分配，O(1) amortized |
| 拒绝方案 | LRU（需链表，嵌入式麻烦）、FIFO（命中率低）|

### ADR-6：Super Block（跨双 Plane 条带化）

| 项目 | 决策 |
|------|------|
| 选型 | **跨双 Plane Super Block** |
| 理由 | 学习企业级跨 Die 条带化逻辑；双 Plane 并行 Multi-Plane PROGRAM 提升写入带宽；FC 层已支持 Multi-Plane 操作 |
| 拒绝方案 | 单 Block 独立写入（无法利用双 Plane 并行）|
| 代价 | Super Block 配对增加 allocator 复杂度 |

### ADR-7：温度分区（Multi-Stream）

| 项目 | 决策 |
|------|------|
| 选型 | **Hot / Cold 双 Open Super Block** |
| 理由 | 冷热数据分离提升 GC 效率；HOT 数据无效页增长快、回收效率高；COLD 数据几乎不变、几乎不用回收；纯软件逻辑，无硬件依赖 |
| 拒绝方案 | 单温区混合写入（GC 效率差）、多级温度（512MiB 容量不需要）|
| 代价 | allocator 需维护双 Super Block；FTL 写操作需指定温度参数 |

### ADR-8：OP 10% 预留

| 项目 | 决策 |
|------|------|
| 选型 | **10% 预留（可配置 7~28%）** |
| 理由 | 消费级典型值；RMA 场景坏块替换后用户容量不变；写入压力大时提供 GC 缓冲、防止性能断崖 |
| 拒绝方案 | 0% OP（坏块替换直接减少用户容量，写入压力时 GC 阻塞前台 IO）|
| 代价 | 用户可见容量减少约 10%（512MiB → ~456MiB）|

### ADR-9：Free Pool 三级**水位**（v0.52 修正原「三级池」）

| 项目 | 决策 |
|------|------|
| 选型 | **一个已擦除池 + 三级水位**（`FREE_POOL_TARGET 32` / `FREE_POOL_EMERGENCY 8` / 0）+ **独立的 OP / Spare 保留池** |
| 理由 | 学习企业级 Background GC 和前台 IO 隔离：水位①以上不做事，②触发后台 GC 补池，③强制 GC。前台"零等待"由**水位**保证（池里始终有 ≥32 个已擦除块），不是由第二个池 |
| 拒绝方案 | ~~三池分区~~——**物理上不可实现**：`gc_erase_and_free` 先擦后归还 ⇒ 池内无未擦除块，没有东西可分区（v0.52 定案，见 §3.7） |
| 代价 | 原 `pre_cond_count`/`emergency_count` 与 PRE_COND/EMERGENCY 状态位是这一误解的产物，已删除 |
| 主流对照 | 真实主控同样是「一个已擦除 free pool + 软/硬水位」+ 独立保留池；擦除落在关键路径上本身就是要避免的事，那才是 OP 池存在的理由 |

### ADR-10：SLC Cache 不实现

| 项目 | 决策 |
|------|------|
| 选型 | **不实现 SLC Cache** |
| 理由 | 当前 NAND 本身就是 SLC，不存在 TLC/QLC 的慢速写入问题；SLC Cache 是 TLC/QLC SSD 的写加速技术 |
| 拒绝方案 | （不适用）|

### ADR-11：坏块管理（BBT）— Block Sparing

| 项目 | 决策 |
|------|------|
| 选型 | **出厂全量扫描 + 运行时隔离 + Block Sparing（Spare Pool 保水位）** |
| 理由 | 学习企业级坏块替换做法；Spare Pool 保证运行时坏块不减少用户容量；出厂坏块扫描 250ms 可接受；Sparing 不需要 block remap 表——只补充 free pool，不做映射 |
| 拒绝方案 | Block Skip（容量自然减少，不符合企业级学习目标）；Block Remap Table（行业不用两层映射）|
| 代价 | 出厂扫描增加启动时间 ~250ms；SPARE_POOL_SIZE × 1 Block 容量占用；需要 FC 支持 OOB/Spare 区读取 |

**与 §3.6 OP 的关系**：Spare Pool 替换坏块，OP 池负责 GC 缓冲和性能保速，二者职责分离。

---

## 10. 企业级实现对比

### 10.1 Block 管理对比

| 维度 | 本方案（当前设计） | 消费级（Phison/SMI） | 企业级（Samsung/Solidigm） |
|------|-----------------|---------------------|--------------------------|
| 数据块分区 | Hot / Cold 双温区 | 2~4 温区 | 多级温度 + SLC cache |
| 空闲池 | 一个已擦除池 + 三级水位 | 多层池（不同 P/E） | 水位 + 独立保留池 |
| GC 触发 | 五级（水位线/写入量/定时/紧急/磨损；T6 碎片率刻意不实现） | 多级阈值 | 后台预测性 GC |
| Over-provision | 10% 预留 | 7~15% | 7~28% |
| 写缓冲 | 无（SLC 不需要） | SLC mode buffer | SLC / pseudo-SLC cache |
| Super Block | 跨双 Plane | 跨 Die | 跨 N Die Super Block |

### 10.2 L2P 管理对比

| 维度 | 本方案 | 消费级（DRAM-less） | 企业级（有 DRAM） |
|------|--------|--------------------|-------------------|
| 表位置 | NAND | NAND | DRAM 全表 |
| SRAM cache | 64~128 regions | 32~256 regions | N/A |
| 替换策略 | CLOCK | CLOCK / 2Q | N/A（全表常驻）|
| 写策略 | Write-back + J | Write-back + J | 直写 DRAM |
| 启动时间 | ~秒级（checkpoint） | ~秒级 | 毫秒级 |

### 10.3 企业级的"多流写入"

本方案已实现 Hot / Cold 双 Open Super Block，与企业级多温区写入方向一致：

```
本方案（Hot / Cold 双 Open Super Block）:
  Hot  Super Block:  [A(热)][C(热)][E(热)]...   ← 用户 IO 数据
  Cold Super Block:  [B(冷)][D(冷)][F(冷)]...   ← GC 搬移的有效页、元数据
  GC Hot:  无效页率高 → 很快回收 ✅
  GC Cold: 无效页率低 → 几乎不用回收 ✅

企业级（多温区）:
  Hot Open Block:  [A][C][E][G]...   ← 频繁更新
  Cold Open Block: [B][D][F][H]...   ← 几乎不变
  GC Hot: 无效页率高 → 很快回收 ✅
  GC Cold: 无效页率低 → 很少需回收 ✅
```

扩展方案：

```c
#define FTL_TEMP_HOT  0
#define FTL_TEMP_COLD 1
#define FTL_TEMP_NUM  2

typedef struct {
    uint16_t open_block[PLANE_NUM][FTL_TEMP_NUM];  // 每 Plane 2 个 open block
    uint8_t  next_page[PLANE_NUM][FTL_TEMP_NUM];
} ftl_allocator_t;
```

---

## 11. FTL 对外接口

### 11.1 FTL 主上下文

```c
typedef struct ftl_context {
    /* 拓扑参数（v0.21 统一为结构体） */
    ftl_topology_t      topo;              /* CH/CE/LUN */

    /* L2P Cache */
    l2p_cache_t         cache;             /* SRAM L2P cache（~128 KB）*/

    /* Pending Request 表（F6：per-instance 异步请求追踪）*/
    ftl_pending_req_t   pending[FTL_PENDING_MAX];

    /* 完成组表（v0.58：一条 host 请求 = 一个组，通知只发一次）*/
    ftl_io_group_t      groups[FTL_GROUP_MAX];

    /* Allocator */
    ftl_allocator_t     alloc;             /* 空闲 block / Super Block 写指针 */

    /* Journal */
    l2p_journal_t       journal;           /* log-structured journal */

    /* Metadata Directory */
    metadata_dir_cache_t metadata_dir;     /* 元数据位置追踪 */

    /* Checkpoint 计时 */
    uint32_t            cp_timer_start;
    uint32_t            cp_sequence;

    /* GC 计时 */
    uint32_t            gc_last_run_writes;
    uint32_t            gc_timer_start;

    /* 统计 */
    uint32_t            total_writes;

    /* 后台写闸门（`bbt --erase` 置位）：置位后 Phase 3 三级后台整体跳过，
     * 杜绝把旧 RAM 状态写回已擦净的芯片；无清除路径，只能复位。*/
    uint8_t             bg_suspend;

    /* 慢死预测巡检节流（F6：per-instance）*/
    uint32_t            rr_last_scan_ms;

    /* Read Recovery 上下文（仅重试计数；ECC 趋势已并入 alloc.blocks[].rr）*/
    ftl_read_recovery_t rr;

    /* Read Recovery 整页暂存（CW 级映射：子操作可能只覆盖页内部分 CW，
     * 而 rr_recover_read 内部是整页读 ⇒ 恢复结果先落本镜像再按 CW 偏移搬回）*/
    uint8_t             rr_page_scratch[FTL_READ_USER_SIZE];
} ftl_context_t;
```

### 11.2 FTL API

> ⚠ **以下按 `include/ftl/ftl.h` 的真实签名重写**（原伪代码的 `ftl_read`/`ftl_write`/
> `ftl_background_flush`/`ftl_checkpoint`/`ftl_gc_execute` 均已不存在；读写是**非阻塞提交**，
> 完成经 completion table 路由到 pending 表）。
>
> **v0.58 订正（策略下沉 + 组完成）**：`ftl_io_request_t.pages` → `count`（长度交给 FTL，plane
> 配对由 FTL 自决）；新增 `ftl_flush`；`ftl_trim` 加 `count`；一次 `ftl_submit_*` = 一个**组**，
> 通知只发一次。详见变更史 v0.58 与 `ftl_host_interface.md`。

```c
/* ── 初始化（v0.30：ftl_init_params_t 结构体指针传参，7 形参 → 2）── */
ftl_status_t ftl_init(ftl_context_t *ftl, const ftl_init_params_t *params);

/* ── 读写（非阻塞提交，长度任意；返回 FTL_OK 只表示已受理）── */
ftl_status_t ftl_submit_read (ftl_context_t *ftl, const ftl_io_request_t *req);
ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req);
void         ftl_flush(ftl_context_t *ftl);                     /* v0.58 新增 */
void         ftl_trim(ftl_context_t *ftl, uint64_t lba, uint32_t count); /* v0.58 加 count */

/* ── 主循环任务（每次 main loop 调用一次）── */
void ftl_service(ftl_context_t *ftl);
// Phase 1:   fc_service()             → 推进 FC/NAND 调度
// Phase 2:   completion_table_drain() → 完成回调 → 路由 pending 表（读回填 / 写记 total_writes）
// Phase 2.5: ftl_drain_group()        → 补发各组待下发子操作（v0.58；host IO 优先于后台）
// Phase 3:   三级后台（ftl_service_idle / _normal / _busy，见 §4.8.5）
```

### 11.3 FTL 内部模块 API

**L2P Cache**（`l2p_cache.h`）：

```c
void         l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce, uint8_t lun); /* v0.35 绑定拓扑 */
nand_pba_t   l2p_cache_read(l2p_cache_t *cache, lba_t local_lba);   /* 收片内 local LBA */
void         l2p_cache_write(l2p_cache_t *cache, lba_t local_lba, nand_pba_t pba);
ftl_status_t l2p_cache_flush_one(l2p_cache_t *cache, int idx);      /* 返回 ftl_status_t */
ftl_status_t l2p_cache_flush_all(l2p_cache_t *cache);
/* ⚠ 无 l2p_cache_flush_background——后台 flush 已内联进 ftl.c 的
 *    ftl_service_normal / ftl_service_busy（按 BG_*_FLUSH_COUNT 条数）。 */
```

**Journal**（`journal.h`）：

```c
/* Journal 是固定块池，块号由 ftl_config.h 推导（当前 = 块 58..61）*/
#define JOURNAL_BLOCKS          (CW_NUM)     /* = 4（CW 级 4× 映射流量）*/
#define JOURNAL_BLOCK0          (58)         /* = BBT_BLOCK + BBT_POOL_BLOCKS */
#define JOURNAL_BLOCK_COUNT     (JOURNAL_BLOCKS)
/* ⚠ 无 JOURNAL_BLOCK1——旧"J0/J1 双块"概念已废弃 */

#define JOURNAL_PAGE_SIZE       (PAGE_MAIN_SIZE)  /* 2048 */
#define JOURNAL_HEADER_SIZE     (12U)             /* magic+page_seq+entry_count+block_seq */
#define JOURNAL_ENTRIES_PER_PAGE ((JOURNAL_PAGE_SIZE - JOURNAL_HEADER_SIZE) / sizeof(l2p_journal_entry_t)) /* = 127 */
#define JOURNAL_FULL_THRESHOLD  (85U)
#define JOURNAL_CRITICAL        (95U)

typedef struct {
    uint32_t    lba;            // 修改的 LBA
    nand_pba_t  old_pba;        // 修改前的 PBA（FTL_PBA_INVALID=新写入）
    nand_pba_t  new_pba;        // 修改后的 PBA
    uint32_t    sequence;       // 单调递增序号
} __attribute__((packed)) l2p_journal_entry_t;   // 16B

typedef struct {
    ftl_topology_t topo;                              // 目标 NAND 芯片路由
    nand_pba_t     block_pba[JOURNAL_BLOCK_COUNT];    // 4 块的 PBA 基址

    uint8_t     current_block;      // 0..3（当前写入的 journal block）
    uint8_t     current_page;       // 0..63
    uint8_t     current_entry;      // 0..126（127 条/页）
    uint32_t    sequence_counter;

    uint8_t     pre_erased_block;   // P1-6：预擦除待复用块号（JOURNAL_BLOCK_NONE=无）
    uint8_t     page_buf[JOURNAL_PAGE_SIZE];

    uint8_t     checkpoint_needed;  // 水位 ≥85%
    uint8_t     emergency;          // 水位 ≥95%
} l2p_journal_t;

void         journal_init(l2p_journal_t *journal, ftl_topology_t topo);
void         journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old, nand_pba_t new);
void         journal_flush_page(l2p_journal_t *journal);
/* F10：全量回放 page_seq > cp_sequence 的页，按 (page_seq, entry_index) 递增序应用 */
ftl_status_t journal_replay(l2p_journal_t *journal, l2p_cache_t *cache, uint32_t cp_sequence);
void         journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence);   /* P1-6 */
```

**Allocator**（`allocator.h`）：

```c
/* 温度等级 */
#define FTL_TEMP_HOT    0
#define FTL_TEMP_COLD   1
#define FTL_TEMP_NUM    2

/* Free Pool 三级水位（一个已擦除池，见 §3.7） */
#define FREE_POOL_TARGET      (32)     /* 水位①/②分界 */
#define FREE_POOL_EMERGENCY   (8)      /* 水位②/③分界 */
#define FTL_OP_PERCENT        (10)     /* OP 预留百分比 */
#define FTL_SPARE_POOL_SIZE   (50)     /* 坏块替换池大小 */

/* Super Block 列表容量：每 Plane 2048 Block，最多 1024 个 Super Block 配对 */
#define SUPER_BLOCK_LIST_MAX  (1024)

/* ⚠ 无 ftl_open_sb_t 类型——allocator 直接内嵌 hot_sb / cold_sb（见下）*/

/* Allocator 上下文 */
typedef struct {
    /* ── 拓扑参数 ── */
    uint8_t  ch, ce, lun;               /* 本 Allocator 绑定的 CH/CE/LUN */
    uint16_t total_blocks;              /* 总好 Block 数（出厂扫描后确定）*/

    /* ── Free Pool（一个已擦除池，三级水位建在 free_count 上）── */
    uint16_t *free_blocks;
    uint16_t  free_count;

    /* ── OP / Spare Pool（容量不动）── */
    uint16_t *op_blocks;    uint16_t op_count;    /* OP 预留池（GC 缓冲）*/
    uint16_t *spare_blocks; uint16_t spare_count; /* Spare Pool（坏块替换）*/

    /* ── Open Super Block（每温度一个）── */
    super_block_t hot_sb;
    super_block_t cold_sb;

    /* ── Closed Super Block 列表（等待 GC）── */
    super_block_t *closed_sb_list;
    uint16_t       closed_sb_count;

    /* ── Block State Table（统一 per-Block 状态）── */
    ftl_block_t *blocks;                            /* 每 Block 条目数组（BST）*/
    uint16_t bad_count;                             /* 坏 Block 数 */
    /* ⚠ v0.60 起无位图表：有效性逐 CW 由页内 META + L2P 反查（DRAM-less approach ③）*/
} ftl_allocator_t;

/* 真实接口 */
ftl_status_t allocator_init(ftl_allocator_t *alloc, const ftl_init_params_t *params);
uint16_t     allocator_select_plane_block(ftl_allocator_t *alloc, uint8_t plane);
nand_pba_t   allocator_alloc_pair(ftl_allocator_t *alloc, uint8_t temperature,
                                  nand_pba_t out[PLANE_NUM]);
ftl_status_t allocator_alloc_reserved_block(ftl_allocator_t *alloc, uint8_t plane,
                                            uint16_t *out_gblock);
/* ⚠ allocator_mark_bad 已删除——坏块统一走 bbt_mark_bad()；擦除计数在 BST */
```

### 11.4 性能估算

| 场景 | cache 命中率 | 平均读延迟 | 说明 |
|------|-------------|-----------|------|
| 64 reg, 全随机 | ~3.1% | ~50 μs | 最差情况 |
| 64 reg, 80/20 | ~60~70% | ~30 μs | 典型工作负载 |
| 128 reg, 80/20 | ~75~85% | ~27.6 μs | 接近理想（25 μs tR）|

Cache miss 代价 ≈ 26 μs（region 加载：一次 NAND read）
Write-back flush 代价 ≈ 200~300 μs（一次 NAND program）

---

## 12. ftl_service 主循环调度

`ftl_service()` 是整个 FTL 的"心脏"，负责协调前台 IO 和后台任务的执行时序。

### 12.1 调度模型概述

```
每次 ftl_service() 调用的执行顺序：

  ┌─────────────────────────────────────────────────────────────┐
  │  ftl_service()                                              │
  │                                                             │
  │  Phase 1 ── fc_service() 最多 NAND_SUBMIT_BUDGET 次         │
  │  ├── 推进 NAND 命令执行                                     │
  │  └── 计数 submitted（本轮新提交的 NAND 命令数）               │
  │                                                             │
  │  Phase 2 ── 处理完成回调                                    │
  │  ├── while (fc_poll_completion() == OK)                     │
  │  └── 根据 completion.module 路由到不同 handler               │
  │                                                             │
  │  Phase 3 ── 后台任务（三级模式切换）                          │
  │  ├── IDLE 模式：NAND 完全空闲 → 全力做后台                   │
  │  ├── NORMAL 模式：相对空闲 → 适度做后台                      │
  │  └── BUSY 模式：前台 IO 活跃 → 最小后台                      │
  └─────────────────────────────────────────────────────────────┘
```

**三级模式切换依据**：系统空闲度 = `pending_count(ftl) + completion_table_count()`
（**未决命令占用**，非 SQ 深度/本轮提交数——B3 起改；旧代理在 host 无 IO 时会被后台
命令推进误判成 NORMAL/BUSY）。

| 模式 | 条件 | 前台 IO 带宽 | 后台任务带宽 |
|------|------|------------|------------|
| IDLE | `inflight == 0`（pending + completion 都空） | 0% | 100% |
| NORMAL | `pending ≤ FTL_PENDING_MAX/2` | ~70% | ~30% |
| BUSY | `pending > FTL_PENDING_MAX/2` | ~95% | ~5% |

### 12.2 三种后台模式的行为

#### IDLE 模式（NAND 完全空闲）

```
触发条件：inflight == 0（pending + completion 都空）
行为：
  flush:    l2p_cache_flush_all()          → 清空所有 dirty
  GC:       free_count < FREE_POOL_TARGET 时循环
            gc_select_victim → gc_migrate_sb → gc_erase_and_free 补池到水位①
            （**无** refill_free_pool / free_pool 结构）
  Journal:  journal_flush_page()            → 强制写 journal buffer
  Checkpoint: 累计写入量够就 checkpoint_create()
设计思路：NAND 闲着也是闲着，把后台活做完，下次 Host IO 来时系统处于最佳状态
```

#### NORMAL 模式（系统相对空闲）

```
触发条件：pending ≤ FTL_PENDING_MAX/2
行为：
  flush:    最多 BG_NORMAL_FLUSH_COUNT(4) 条 DIRTY 行
            （⚠ P1-4：l2p_cache_is_thrashing 时停主动 flush，脏行只由驱逐按需写回）
  GC:       gc_check_triggers 位掩码（T1~T5）命中 → 搬 BG_NORMAL_GC_PAGES(4) 页
  Journal:  不单独 flush（让 page_buf 自然填满）
  Checkpoint: 检查但不一定执行
设计思路：留 NAND 带宽给突发 IO，后台任务"慢慢做"
```

#### BUSY 模式（前台 IO 活跃）

```
触发条件：pending > FTL_PENDING_MAX/2
行为：
  flush:    只 flush 1 条 DIRTY（防 evict 阻塞）
  GC:       跳过（BUSY 模式不做 GC）
  Journal:  跳过
  Checkpoint: 仅 Journal ≥ 95% 时强制执行（checkpoint_is_emergency）
设计思路：前台 IO 优先。唯一例外：紧急 Checkpoint（非 GC——旧文写"强制 GC"是错的）。
```

### 12.3 实现

> ⚠ **订正**：原代码原型里的 `fc_get_sq_usage` / `ftl_process_completion` /
> `ftl_gc_background` / `ftl_gc_emergency` / `ftl_cp_check` / `l2p_cache_flush_background`
> **均不存在**。以下按 `ftl.c` 真实实现重写。

```c
#define NAND_SUBMIT_BUDGET      (4U)     // 每轮最多提交 4 条
#define BG_NORMAL_FLUSH_COUNT   (4U)
#define BG_BUSY_FLUSH_COUNT     (1U)
#define BG_NORMAL_GC_PAGES      (4U)
#define BG_IDLE_GC_TARGET       (32U)
#define CP_WRITE_INTERVAL       (1024U)
#define CP_TIME_INTERVAL_MS     (10000U)

void ftl_service(ftl_context_t *ftl) {
    /* Phase 1: fc_service 推进 NAND（最多 NAND_SUBMIT_BUDGET 次）*/
    for (int i = 0; i < NAND_SUBMIT_BUDGET; i++) {
        if (fc_service() != FC_STATUS_OK) break;
    }

    /* Phase 2: 统一完成泵（替代 fc_poll_completion 循环）*/
    completion_table_drain();

    /* Phase 3: 三级后台（空闲度 = pending + completion 未决占用）*/
    uint16_t inflight = (uint16_t)(pending_count(ftl) + completion_table_count());
    if (inflight == 0U) {
        ftl_service_idle(ftl);
    } else if (pending_count(ftl) > FTL_PENDING_MAX / 2U) {
        ftl_service_busy(ftl);
    } else {
        ftl_service_normal(ftl);
    }
}

static void ftl_service_idle(ftl_context_t *ftl) {
    l2p_cache_flush_all(&ftl->cache);
    /* 补 Free Pool 到水位①：循环 gc_select_victim → gc_migrate_sb → gc_erase_and_free */
    while (ftl->alloc.free_count < FREE_POOL_TARGET) {
        super_block_t *victim;
        if (!gc_select_victim(&ftl->alloc, &victim)) break;
        (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
        gc_erase_and_free(&ftl->alloc, victim, ftl->total_writes);
    }
    journal_flush_page(&ftl->journal);
    /* checkpoint：累计写入量够则 checkpoint_create() */
}

static void ftl_service_normal(ftl_context_t *ftl) {
    /* flush 最多 4 条 DIRTY（P1-4：thrashing 时跳过主动 flush）*/
    if (!l2p_cache_is_thrashing(&ftl->cache)) {
        uint16_t flushed = 0U;
        for (uint16_t i = 0U; (i < L2P_CACHE_SIZE) && (flushed < BG_NORMAL_FLUSH_COUNT); i++) {
            if (ftl->cache.lines[i].state == L2P_CACHE_DIRTY) {
                (void)l2p_cache_flush_one(&ftl->cache, (int)i);
                flushed++;
            }
        }
    }
    /* GC：gc_check_triggers 位掩码（T1~T5）命中则搬 BG_NORMAL_GC_PAGES 页 */
    {
        uint32_t triggers = gc_check_triggers(&ftl->alloc, ftl->total_writes,
                                              ftl_now_ms() - ftl->gc_timer_start);
        if (triggers != 0U) { /* 按 trigger 分派 GC / 紧急 GC（见 gc.c）*/ }
    }
}

static void ftl_service_busy(ftl_context_t *ftl) {
    /* 只 flush 1 条 DIRTY（防 evict 阻塞）*/
    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++) {
        if (ftl->cache.lines[i].state == L2P_CACHE_DIRTY) {
            (void)l2p_cache_flush_one(&ftl->cache, (int)i);
            break;
        }
    }
    /* 紧急 Checkpoint（Journal ≥ 95%）：BUSY 模式不做 GC */
    if (checkpoint_is_emergency(ftl)) {
        (void)checkpoint_create(ftl);
    }
}
```

### 12.4 企业级主循环参考

```
企业级 SSD 主循环（三星/Solidigm）：

  while (1) {
      process_host_doorbell();      // NVMe 提交队列
      ftl_submit_host_io();         // 查 L2P、构造 FC 命令
      fc_service();                 // 推进 NAND
      process_fc_completions();     // 中断下半部
      background_scheduler();       // 独立后台任务调度器
  }
```

企业级后台调度器使用**独立时间预算**控制每个任务：

```c
typedef struct {
    void     (*task_func)(void);
    uint32_t  interval_ticks;        // 最小执行间隔（主循环次数）
    uint32_t  ticks_since_run;
    uint8_t   priority;              // 0=最高, 3=最低
    uint32_t  budget_us;             // 每轮最多花多少 μs
} background_task_t;

void background_scheduler(void) {
    uint32_t budget_remaining = BG_TOTAL_BUDGET_US;  // 每轮总预算
    for (int i = 0; i < BG_TASK_COUNT; i++) {
        background_task_t *task = &g_bg_tasks[i];
        task->ticks_since_run++;
        if (task->ticks_since_run < task->interval_ticks) continue;

        uint32_t start = timer_get_us();
        task->task_func();                              // 传预算执行
        uint32_t elapsed = timer_get_us() - start;
        budget_remaining -= MIN(elapsed, task->budget_us);
        task->ticks_since_run = 0;

        if (budget_remaining < MIN_TASK_BUDGET_US) break;
    }
}
```

**企业级 vs 本方案对比**：

| 维度 | 本方案（三级模式） | 企业级方案 |
|------|-----------------|-----------|
| 调度粒度 | 按 SQ 深度分三级 | 每个任务独立时间预算 + 间隔 |
| 后台抢占 | Phase 3 整体让路 | 高优任务可打断低优 |
| 时间控制 | 按"做几条"控制 | 精确到 μs 硬件定时器 |
| 前台保护 | SQ > 75% 停后台 | NAND 带宽比例动态调节 10~100% |
| 多核 | 无 | CPU0 = 前台 IO, CPU1 = 后台任务 |

**企业级反直觉做法——Inline mini-GC**：

```
ftl_write() → allocator_alloc()
  → 发现 free pool 低于水位①
  → 不等后台，inline 做一次 mini-GC（搬 1 页）
  → 继续写

原理：写操作要等 NAND program（~300μs），
      CPU 在等待间隙闲着也是闲着，顺便做后台
```

### 12.5 主循环完整形态

> ⚠ **订正**：早期草图用标量 `g_ftl` + `controller_init`/`host_io_pending`。真实 `main.c`
> 用**数组** `g_ftl[CH_NUM][CE_NUM][LUN_NUM]`（每 `(ch,ce,lun)` 一个独立实例），
> 逐实例 `ftl_init` 后由 `ssd_ftl_boot_all` 统一启动，主循环只 drain 实例 0；
> 主机侧入口是 **UART CLI**（`cli_service_poll`），NVMe host 层已于 v0.47 删除。
> `controller_init` / `host_process_requests` / `cpu_sleep` 均**不存在**。

```c
int main(void) {
    sys_init();                  /* 时钟、外设 */
    cli_init();
    fc_init();                   /* FC 层初始化 */
    nand_init();
    ssd_ftl_boot_all();          /* 逐 (ch,ce,lun) ftl_init + recovery + free pool 回填 */

    while (1) {
        cli_service_poll();      /* 收 UART 命令 → 入 pending 表 */
        ftl_service(&g_ftl[0][0][0]);  /* FTL 主循环（内部先跑 fc_service）*/
    }
}
```

---

## 13. 与 FC 层接口约定

### 13.1 FTL 使用 FC 的约束表

```
┌──────────────────────────────────────────────────────────────┐
│  FTL 通过 FC 访问 NAND 的接口约束                            │
├─────────────────────────────────────┬────────────────────────┤
│           用户数据读写               │ L2P / Journal 元数据   │
├─────────────────────────────────────┼────────────────────────┤
│  operation = NORMAL_PROGRAM_PAGE    │ operation = 同上       │
│             /NORMAL_READ_PAGE       │                       │
│  CRC    = ENABLE (数据保护)         │ CRC    = ENABLE (v0.62)│
│  Random = ENABLE (加扰)             │ Random = DISABLE      │
│  LDPC   = BCH                      │ LDPC   = BCH          │
│  module = FTL                      │ module = METADATA     │
│  lba    = lba (写入 META)           │ lba    = 不填          │
│  page_type = LP (SLC)              │ page_type = LP        │
└─────────────────────────────────────┴────────────────────────┘
```

### 13.2 FTL 写数据的标准序列

```c
nand_paa_t ftl_write_data(lba_t lba, const uint8_t *data, uint32_t len) {
    fc_command_t *cmd = fc_command_alloc();

    cmd->config.operation    = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.cw           = 3;                 // 4 CW per page
    cmd->config.crc          = FC_CRC_ENABLE;
    cmd->config.random       = FC_RANDOM_ENABLE;
    cmd->config.ldpc         = FC_ECC_MODE_BCH;
    cmd->config.page_type    = FC_PAGE_TYPE_LP;
    cmd->config.module       = FC_COMMAND_MODULE_FTL;
    cmd->paa                 = new_paa;           // allocator 分配
    cmd->lba                 = lba;               // 写入 META

    ftl_cmd_configure_descriptors(cmd);   // v13 前为 fc_command_configure_descriptors
    ftl_cmd_write_data(cmd, data, len);   // v13 前为 fc_write_cmd_data

    fc_submit(cmd);                               // → SQ
    // ftl_service() 中 fc_poll_completion 会拿到完成状态
    // 检查 completion.status0~3, ecc_bit0~3
    // 成功后 journal_append + l2p_cache_write

    fc_command_release(cmd);
}
```

### 13.3 FTL 应当避免的操作

```
❌ 直接调用 nand_read_page() / nand_program_page()
❌ 直接操作 NAND 引擎状态机
❌ 直接读写 NAND 寄存器（SET/GET_FEATURES 通过 FC 完成）
❌ 绕过 ftl_cmd_configure_descriptors 手动分配 desc

✅ 通过 CQ + completion 获取 per-CW 状态
✅ 通过 fc_data_path_raid_recover() 触发 RAID 恢复（未来）
✅ 遍历 desc 链获取每 CW 的详细错误信息
```

### 13.4 2026-09-28 接口层设计决策

本节记录 2026-09-28 讨论中确认的 FTL 接口层设计决策：

| # | 决策项 | 结论 | 说明 |
|---|--------|------|------|
| D1 | LBA 粒度 | **512B** | 一个 CW 的 USER 数据区（FC_BUFFER_USER_SIZE），CW 大小 = PAGE_SIZE / 4 |
| D2 | 几何模型 | **统一到 nand_config.h** | 废弃 `config/nand_geometry.h`，FTL 直接用 nand_config.h 的宏 |
| D3 | 地址类型 LBA | `lba_t` = **uint32_t** | Host 逻辑地址，传入 cmd->lba 用于 META 校验；替代 `ssd_lba_t`（uint64_t） |
| D4 | 地址类型 PBA | `nand_pba_t` = **uint32_t** | 完整物理地址，L2P 映射的目标值，含拓扑（CH/CE/LUN）+ 芯片内地址（PAA）。⚠ `pba_t` 已于 v0.20 删除 |
| D5 | 地址类型 PAA | `nand_paa_t` = **uint32_t** bitfield | 芯片内地址 [CW:2][PAGE:6][PLANE:1][BLOCK:11]；替代 `ssd_ppa_t`（uint64_t）|
| D5 | FTL 提交模型 | **非阻塞异步提交** | ftl_submit_*() 立即返回，ftl_service() 驱动完成 |
| D6 | 完成通知 | **module 路由** | fc_completion.module 区分来源，FTL 内部自行处理 |
| D7 | FTL→FC 访问 | **命令构造 helper**（不是隔离层） | 通过 `ftl_fc_interface.h` 的 `ftl_cmd_setup_*()`，不绕过 FC |
| D8 | 命名 | **ftl_fc_interface** | 头文件 `include/ftl/ftl_fc_interface.h`，源文件 `src/ftl/ftl_fc_interface.c` |
| D9 | RTOS | **暂不加** | 裸机超级循环 + 中断，存储主控标准做法 |
| D10 | Host 命令来源 | **UART Shell CLI** | 通过 read/write/trim 命令模拟 NVMe IO |
| D11 | Host 非阻塞 | **pending 请求表** | UART 来的命令入 pending 表，FTL 异步完成后再输出结果 |

#### 13.4.1 地址体系总结

##### 行业标准缩写定义

| 缩写 | 全称 | 阶段 | 用途 | 你的代码中对应 |
|------|------|------|------|--------------|
| LAA | Logical Application Address | Host → Controller | 应用程序下发的原始逻辑地址 | UART CLI 的 `lba` 参数（本项目 Host 为 UART CLI，无 NVMe）|
| LBA | Logical Block Address | L2P 索引 | 行业标准逻辑地址，`cmd->lba` 用于 META 校验 | `cmd->lba` |
| PBA | Physical Block Address | L2P 映射值 | 完整物理地址，含拓扑路由（CH/CE/LUN）+ 芯片内地址（PAA）| FTL 层 `nand_pba_t`（**只活在 FTL 内部**，v47 起不跨进 FC）|
| PAA | Physical Array Address | NAND 芯片寻址 | 芯片内地址（Block + Plane + Page + CW），不含拓扑 | `nand_paa_t` |
| FAA | （本项目曾用） | — | 原代码中 `cmd->faa` 曾用名，现统一为 LBA | 已废弃 |

##### 地址转换流程（行业标准 LBA → PBA → PAA）

```
Host（UART / NVMe）
  │ LAA = Application 下发的地址（= LBA，1:1）
  ▼
FTL
  │ LBA → L2P → PBA（完整物理地址）
  │ PBA(uint32_t) = [CH:?][CE:?][LUN:?][PAA]
  │                = [CH:?][CE:?][LUN:?][BLOCK:11][PLANE:1][PAGE:6][CW:2]
  │
  │ FTL 从 PBA 拆出（v47：转换全部发生在 **FTL 侧**，FC 层只认 PAA）：
  │   cmd->config.ch/ce/lun = PBA 高位（路由到哪个 NAND 芯片）
  │   cmd->paa              = pba_to_paa(PBA)（芯片内地址；**字段重排**，不是掩码截取）
  ▼
FC → NAND Dispatch → Engine → FMC
  │ cmd->config.ch/ce/lun → 选中目标 NAND 芯片
  │ cmd->paa(PAA)          → ONFI 行地址周期，发给芯片
  ▼
NAND Chip（由 CH/CE/LUN 选中的那颗芯片）
```

##### 地址编码关系

```
PBA(uint32_t):
  ┌──────────┬──────────┬──────────┬────────────────────────────┐
  │ CH(2b)   │ CE(2b)   │ LUN(2b)  │     PAA(20b)               │
  │ 路由信息  │ 路由信息  │ 路由信息  ├─────┬──────┬──────┬──────┤
  │          │          │          │ CW │ PAGE │PLANE│BLOCK │
  │          │          │          │ 2b │  6b  │ 1b  │ 11b  │
  └──────────┴──────────┴──────────┴────┴──────┴──────┴──────┘

cmd->lba = Host 逻辑地址（用于 META 校验，不参与地址路由）
cmd->paa = PAA（芯片内地址）= pba_to_paa(PBA)   ← v47：FC 层唯一的物理地址字段
```

#### 13.4.2 FTL 主循环形态

> ⚠ **订正**：`g_ftl` 是数组（见 §12.5），真实函数名是 `cli_service_poll`（非
> `uart_service_poll`）；完成通知由 `ftl_service` 内的 completion 路径处理，无
> `check_pending_done`。

```c
void main(void)
{
    sys_init();
    cli_init();
    fc_init();
    nand_init();
    ssd_ftl_boot_all();          /* 逐实例 ftl_init + recovery + free pool 回填 */

    while (1)
    {
        cli_service_poll();      /* 收 UART 命令 → 入 pending 表 */
        ftl_service(&g_ftl[0][0][0]);  /* 驱动 FTL + FC + 后台任务 */
    }
}
```

---

## 14. 未解决问题

| # | 问题 | 影响 | 状态 |
|---|------|------|------|
| 1 | Region 物理存储：固定 vs 动态元数据 block | 固定简单但磨损集中 | ⚠️ **部分解决（v0.50 订正）**：Anchor/Checkpoint/BBT 落**固定块池**（32/8/2 块，池内环内轮转 + 池满擦旧块），**不是**动态分配；只有 L2P region 日志块走动态分配（从 Free Pool 取整块）。原「✅ 已解决」不成立——`metadata_dir_alloc_region` 因 `fill_fixed` 预登记全部 region 而**不可达**，见 §3.3 订正注 |
| 2 | Journal 可靠性：journal 本身怎么防掉电 | journal page program 到一半掉电 | ✅ 四层保护（见 §7.6） |
| 3 | 启动加载策略：加载 checkpoint 全表还是按需 miss | 启动延迟 vs 首次访问延迟 | ✅ 已解决（见 §8.3）：干净关机按需 miss，异常掉电批量回放 |
| 4 | Journal 回收时机 | 需要确保 L2P 表已同步 | ✅ 已解决（见 §7.5）：三级触发（水位线 85%/95% + 写入量 4096 + 定时 10s） |
| 5 | L2P 表磨损：元数据 block 擦除是否超过数据 block | 寿命不均衡 | ⚠️ **订正（v0.50）**：Anchor 池每块擦一次需 ≈65,536 次主机写（8 份/块）⇒ 磨损是数据块的 **8×**（≈13 TB 全盘写），**不是**「几乎无磨损」。本仓磨损不是约束，选池大小是为**主控形态**而非磨损优化，详见 §14.1 |
| 6 | 多 Plane 下 region 分布：是否跨 plane 并行写入 | L2P 写回性能 | ✅ 已解决（见 §5.6）：偶/奇 Region 交错 Plane 0/1，Multi-Plane flush 并行，速度翻倍 |

### 14.1 L2P 表磨损均衡分析

> ⚠️ **本节推导的前提已过期（v0.50 订正，数字保留作历史推导）**：
> 本节按「L2P 表 = 固定 32 blocks，GC = 全部 32 块一起擦」建模。实际实现里
> **L2P region 写在日志块**（每块 64 槽，从 Free Pool 动态取整块，写满换新块、
> 旧块活 region 归零即归还），**没有**「32 块一起 GC」这一步；
> 「元数据 GC」现在是**池内单块轮转 + 逐块回收**（`metadata_ring_prepare_write` /
> `metadata_dir_reclaim`），代价按**单块**计而非 32 块。
> 另：Anchor 池每块擦一次 ≈65,536 次主机写（8 份/块）⇒ **8× 数据块磨损**，
> 与下面「Journal 是瓶颈」的结论不冲突但需并列看（Anchor 的绝对值仍远低于
> 100k P/E：全盘写 13 TB 量级，见 §3.3 订正注）。
> 下表「L2P 表（32 blocks）~5.68 TB」是旧模型的产物，**不再作为当前寿命结论**。

#### 负载模型

```
NAND 规格：SLC, 100k P/E, 512MiB
L2P 表：32 blocks × 64 pages = 2,048 页（日志结构写入）
Cache：64 line，每条 DIRTY 触发一次 Region flush（写 1 页）
全盘写 = 1,048,576 次 LBA 写入
```

#### L2P 表磨损推导

每次全盘写产生的 Region flush 次数：

```
Cache 64 line，全盘随机写：
  64 次 Write → Cache 满 → evict 1 个 DIRTY Region → flush 1 页
  1,048,576 / 64 = 16,384 次 evict flush
  加上 background flush / checkpoint flush ≈ 2,000 次
  合计 ≈ 18,000 次 Region flush / 全盘写
```

元数据区 GC 频率：

```
元数据区总页数 = 32 × 64 = 2,048 页
每次元数据 GC = 所有 32 blocks 擦除一次（压缩后重新分布）
GC 频率 = 18,000 / 2,048 ≈ 8.8 次 / 全盘写
每个 metadata block 擦除次数 = 8.8 次 / 全盘写
寿命内全盘写 = 100,000 / 8.8 ≈ 11,363 次
总写入量 = 11,363 × 512MiB ≈ 5.68 TB
```

#### 与数据区寿命对比

| 区域 | 寿命（TB 写入） | 说明 |
|------|---------------|------|
| User + OP | ~12.98 TB | 4056 block × 3.2GB/block |
| **L2P 表（32 blocks）** | **~5.68 TB** | 数据区的 44%，仍有余量 |
| **Journal（初稿 2 blocks）** | **~800 GB** | ⚠️ 最短的瓶颈；**实现已是 4 块 ⇒ ~1.6 TB**（v0.54）|

#### 每日写入 vs 实际寿命

| 每日写入 | L2P 表寿命 | Journal 寿命 |
|---------|-----------|-------------|
| 10 GB（极端） | 1.6 年 | 80 天 ❌ |
| 1 GB（较忙） | 15.6 年 | 2.2 年 ⚠️ |
| 100 MB（学习/开发） | 156 年 ✅ | 22 年 ✅ |

#### 结论

**L2P 表（32 blocks）在学习和开发场景下有余量。**
实际消耗比最坏模型更低——顺序写入的 cache 命中率高、批量 flush 合并更新、Journal 重复 LBA 会被截断。

**Journal 是真正的瓶颈**（初稿 2 blocks 仅 ~800GB），但容易解决：

```
JOURNAL_BLOCKS: 2 → 4
→ Journal 寿命从 800GB 翻倍到 1.6TB
→ 10GB/天场景下从 80 天到 160 天
```

> ✅ **已落地（v0.54）**：实现里 `JOURNAL_BLOCKS = CW_NUM = 4`（块 58-61），寿命 ~1.6 TB。
> 若还要加，方向是 `JOURNAL_BLOCKS → 8`（寿命 3.2 TB），代价是多占 4 个好块。

#### 缓解措施（v0.54 逐条核对）

| 措施 | 状态 |
|---|---|
| **增大 JOURNAL_BLOCKS**（初稿推荐，最简单）| ✅ 已落地（= 4 块）|
| **Region 更新合并**（同一 Region 多次写合并为一次 flush）| ✅ **本就固有**——region cache 把同 region 的多次写合并进一条脏行，只 flush 一次；无需改动。**额外**在 **Journal 条目**粒度实现了「同一 LBA 合并」（`journal_append` 就地更新）——覆盖写工作集下减少 Journal 页数 |
| **消除环形块双倍擦除**（v0.54 新增）| ✅ `journal_truncate` 改为**预擦除 + 记账**，`flush_page` 回切跳过 ⇒ 同一块每圈只擦一次（旧实现双擦，凭空多 ~25% 磨损）|
| **Checkpoint 批量写**（统一更新多个 Region，分摊元数据 GC 开销）| 未做（登记）——与 P0-2 的元数据写模型相关，非 P1 范围 |

---

## 15. 设计总结

### 15.1 设计定位

```
本方案定位：消费级 DRAM-less SSD（UFS/eMMC 主控档次）
对标产品：Phison E21T / WD SN580 / UFS 4.0 嵌入式方案
硬件平台：STM32H743（1MB SRAM + 8MB SDRAM）+ MT29F4G08 SLC NAND（512MiB）
```

### 15.2 设计决策总览

| 决策 | 选择 | 行业对标 | 硬件可行性 |
|------|------|---------|-----------|
| 映射策略 | 纯 CW 级（512B）页映射 | 所有现代 SSD 标准做法 | ✅ 与 FC 完全对齐 |
| L2P 存储 | NAND 元数据区（DRAM-less） | UFS/eMMC 主流 | ✅ 1MB SRAM 可做 cache |
| L2P 缓存 | SRAM region cache (64~128 regions) | Phison/SMI DRAM-less 方案 | ✅ 64KB~256KB |
| 替换策略 | CLOCK | 嵌入式主流，Phison/UFS 使用 | ✅ O(1)，无动态内存 |
| 写回策略 | Write-back + Journal | 消费级 DRAM-less 标配 | ✅ 纯软件 |
| Super Block | 跨双 Plane | 企业级跨 Die 的简化版本 | ✅ FC Multi-Plane 已支持 |
| 温度分区 | Hot / Cold 双 Open Super Block | 消费级 2~4 温区 | ✅ 纯软件 |
| OP 预留 | 10%（可配置 0~28%） | 消费级典型值 7~15% | ✅ 编译期配置 |
| Free Pool | 一个已擦除池 + 三级水位 | 企业级 Background GC | ✅ 纯软件 |
| Bad Block 管理 | 出厂全量扫描 + Block Sparing（Spare Pool） | 企业级标准做法 | ⚠️ 需要 FC OOB 接口 |
| GC 算法 | 四维加权 cost 评分 + 五级触发（T1~T5；T6 已废弃） | 与企业级一致 | ✅ 纯软件计算 |
| GC 原子性 | Journal + GC Bitmap + 双拷贝容忍 | 消费级/企业级做法 | ✅ 纯软件 |
| 主循环调度 | 三级模式（IDLE/NORMAL/BUSY） | 与消费级一致 | ✅ 纯软件 |
| RAID | 不实现（CONFIG_FC_RAID = 0） | — | SLC 不需要 |
| SLC Cache | 不实现 | — | 本身就是 SLC |

### 15.3 硬件依赖审查

所有设计功能中，除 BBT 出厂扫描需要 FC 支持 OOB/Spare 区读取外，其余功能均为纯软件逻辑，当前 FC 层和硬件平台已全部支持：

| 功能 | 硬件依赖 | 状态 |
|------|---------|------|
| Multi-Plane PROGRAM/READ/ERASE | FC 层操作码 | ✅ FC Phase E 已实现 |
| BCH ECC（t=4） | FC 数据路径 | ✅ FC Phase K 已实现 |
| CRC-32 | FC 数据路径 | ✅ FC Phase L 已实现 |
| Random 加扰 | FC 数据路径 | ✅ FC Phase J 已实现 |
| DMA 传输 | FC/NAND 层 | ✅ FC Phase F/G/H 已实现 |
| OOB/Spare 区读写 | FC 接口 | ⚠️ 需确认/添加 |

### 15.4 与企业/消费级的一致性

```
本方案   vs   消费级 DRAM-less (Phison E21T)  vs   企业级 (Samsung PM9A3)
─────────────────────────────────────────────────────────────────────────
纯页映射 ✅       纯页映射                        纯页映射（全表在 DRAM）
CLOCK ✅          CLOCK / 2Q                     不需要（全表常驻）
Write-back+J ✅   Write-back+J                   直写 DRAM
Hot/Cold 分区 ✅  2~4 温区                       多级 + SLC cache
三级 Free Pool 水位 ✅ 多层池                     水位 + OP/Spare 保留池
OP 10% ✅         7~15%                          7~28%
BBT Sparing ✅    Block Skip                     Stripe Skip + Sparing
无掉电电容 ✅     无掉电电容                      有掉电电容
三级主循环 ✅     类似                            硬件时间预算调度器
```

**结论**：本方案与消费级 DRAM-less SSD 主流做法一致，定位合理。没有"为了炫技而做的无用功能"，也没有"当前硬件无法支持的设计"。唯一的缺口是 FC 层的 OOB 读写接口。

### 15.5 实施顺序与当前状态

#### 已实现（2026-09-29 最终状态：FTL 全部模块完成 + Completion Table 统一路由 + Region 动态分配）

```
Step 1 — ftl_fc_interface 层 + 地址体系
  ├── include/ftl/ftl_fc_interface.h      ✅ addr_to_cmd（nand_addr_t 直填）/ setup_ppa / write_data / read_data
  ├── src/ftl/ftl_fc_interface.c          ✅ 仅保留 sync_wait（v0.28 内联化后 4 个同步包装删除）
  ├── include/ftl/ftl_type.h              ✅ lba_t / ftl_status_t 类型
  │                                       ✅ 企业级两值枚举 FTL_OK=0 / FTL_ERROR=-1
  │                                       ✅ 删除 pba_t（已由 nand_pba_t 替代）
  ├── include/nand/nand_config.h          ✅ PBA/PAA 全套 40+ inline 函数
  │                                       ✅ v0.28: nand_addr_t + pba_from_addr/addr_from_pba/addr_from_paa
  └── 废弃: firm_boot / l2p.h / garbage_collection / nand.h / nand_geometry.h 已清理

Step 2 — L2P Cache + CLOCK + 真实 NAND 读写
  ├── include/ftl/l2p_cache.h             ✅ 数据结构 + md_dir/alloc 指针 + l2p_region_pba()
  └── src/ftl/l2p_cache.c                 ✅ Region load（读 NAND）+ flush（写 NAND）
                                           ✅ 通过 Metadata Directory 定位（v0.50：日志块动态分配）
                                           ✅ l2p_cache_set_md_dir 增加 alloc 指针
                                           ✅ flush_one 每次 flush → metadata_dir_append_slot（追加写）

Step 3 — Allocator + Super Block
  ├── include/ftl/allocator.h             ✅ Free Pool 三级水位 + Super Block 配对
  └── src/ftl/allocator.c                 ✅ 磨损均衡 + Free Pool 三级水位
                                           ✅ allocator_close_sb → Closed 列表

Step 4 — Journal
  ├── include/ftl/journal.h               ✅ 固定池 Block 58~61（= CW_NUM）环形
  ├── src/ftl/journal.c                   ✅ flush_page（NAND 写）+ replay（扫描回放）
  │                                       ✅ Header [magic+page_seq+entry_count+block_seq]
  │                                       ✅ journal_truncate（擦除非当前 Block）
  └── 开放常量移至 .h                      ✅ JOURNAL_MAGIC / JOURNAL_HEADER_SIZE

Step 5 — GC + ftl_service 主循环
  ├── include/ftl/gc.h                    ✅ gc_migrate_sb 增加 journal 参数
  ├── src/ftl/gc.c                        ✅ 完整 7 步搬移（读 META→查 L2P→分配→写→L2P→Journal）
  │                                       ✅ gc_read_page_with_lba（保持 FC 命令存活读 META）
  │                                       ✅ gc_erase_and_free（真实擦除 + 失败标记坏块）
  ├── include/ftl/ftl.h                   ✅ ftl_context_t + 全部子模块字段
  └── src/ftl/ftl.c                       ✅ submit_read/write（异步）+ read/write/trim（同步包装）
                                           ✅ handle_completion 路由（FTL/METADATA/GC/BBT/CHECKPOINT）
                                           ✅ GC/WL/Checkpoint 三级后台集成

Step 13 — Completion Table 统一完成路由
  ├── include/ftl/completion_table.h      ✅ 32 项路由表 + 同步等待器 + 统一泵声明
  └── src/ftl/completion_table.c          ✅ completion_table_add/lookup/remove/drain
                                           ✅ ftl_sync_waiter_handler（带/不带 release）
                                           ✅ completion_table_set_default（= ftl_handle_completion）

Step 6 — Metadata Directory
  ├── include/ftl/metadata.h              ✅ Anchor 池 (16..47) + 2048 entry Directory（7B packed）
  └── src/ftl/metadata.c                  ✅ init/lookup/update/alloc/flush/load 完整实现
                                           ✅ 2048 条 Directory（日志块追加写，**无**固定位置回退）
                                           ✅ 完整性由下层 FC per-CW CRC + ECC 保证（v0.62）

Step 7 — Pending Request 表
  ├── include/ftl/pending.h               ✅ 32 项固定表 + lba lookup
  └── src/ftl/pending.c                   ✅ add/lookup/lookup_by_lba/remove

Step 8 — Checkpoint
  ├── include/ftl/checkpoint.h            ✅ 全量 + Journal 截断协同 + 三级触发
  └── src/ftl/checkpoint.c                ✅ flush_all → 池内轮转追加（一条 record）→ md flush → truncate
                                           ✅ 水位线/写入量/定时三级触发 + 紧急95%强制
                                           ✅ checkpoint_journal_watermark（ftl_submit_write 中调用）

Step 9 — BBT（Bad Block Table）
  ├── include/ftl/bbt.h                   ✅ Bitmap 4096 bit（一条 record = 1 页）
  └── src/ftl/bbt.c                       ✅ bbt_init（加载/出厂扫描）+ bbt_mark_bad + bbt_save
                                           ✅ 扫描策略：LDPC_RAW 读 Page 0 检测坏块
                                           ✅ BBT 池 56..57 轮转（块满擦下一块）

Step 10 — Read Recovery
  ├── include/ftl/read_recovery.h         ✅ Per-Block ECC 趋势追踪 + 4级健康状态
  └── src/ftl/read_recovery.c             ✅ Stage 1: Read Retry（SET_FEATURES 8次重试）
                                           ✅ Stage 2: 邻近页辅助（page±1 读验证）
                                           ✅ Stage 3+: XOR/RAID 预留
                                           ✅ ECC 预测性坏块检测（rr_check_predictive）
                                           ⚠️ rr_record_ecc 未接入读完成路径（需 timer + completion）

Step 11 — Power Failure Recovery
  ├── include/ftl/power_failure_recovery.h ✅ 三类启动类型枚举 + 结果结构体
  └── src/ftl/power_failure_recovery.c     ✅ ftl_boot_recovery（fresh/clean/PF 判断）
                                            ✅ ftl_boot_fresh_start / clean_start / power_failure
                                            ⚠️ ftl_boot_scan_write_pointer 存在但**主路径不调用**（写指针恢复由 F2 全扫 NAND 完成，见 §8.3.7）
                                            ✅ 扫描 Journal header 判断（不调 replay，不破坏状态）

Step 12 — Wear Leveling
  ├── include/ftl/wear_leveling.h         ✅ 动态（alloc 选低擦除）+ 静态（三星 Daemon）
  └── src/ftl/wear_leveling.c             ✅ 三条件触发（差值/比值/低区占比）
                                           ✅ wl_select_victim（选有效页多+擦除少的冷 Block）
                                           ✅ wl_relocate（读→L2P查→写→Journal→擦→归还）
                                           ✅ wl_execute 接入 ftl_service（IDLE全力/NORMAL按需）

#### 代码审视修复（v0.18）
  ├── l2p_cache_set_md_dir()              ✅ ftl_init 中连接 Cache → Directory
  ├── bbt_init()                          ✅ ftl_init 中加载 BBT
  ├── sync_wait "other" release           ✅ 删除（单 LUN 无并发）
  └── boot_recovery journal_replay        ✅ 改为直接读 header，不修改 journal 状态

#### 代码审视修复（v0.19 — Completion Table 统一完成路由）
  ├── ftl_internal_sync_wait              ✅ 改为 completion_table_add + sync_waiter
  │                                       ✅ 不再独占 fc_poll_completion
  │                                       ✅ completion_table_drain 自动路由非目标 completion 到 ftl_handle_completion
  ├── ftl_service Phase 2                 ✅ 改为 completion_table_drain() 统一泵
  │                                       ✅ ftl_handle_completion 注册为 default handler
  ├── ftl_read/ftl_write 同步循环         ✅ 改为 completion_table_drain()，两泵合并为一个
  ├── l2p_cache_flush_one Region 分配     ✅ v0.50 消除固定回退：每次 flush → metadata_dir_append_slot（日志块追加）
  └── ftl_init 连接 Cache → Allocator     ✅ l2p_cache_set_md_dir 增加 alloc 指针参数
```

#### 未实现（非 FTL 核心模块，后续方向）

| 模块 | 文件 | 说明 |
|------|------|------|
| Controller 层 | `state_machine.c` / `scheduler.c` | 状态机、优先级调度（`request.c` 已在 v0.23/v0.36 实现——拆分 + 跨实例路由，非桩）→ ✅ v0.47 已落地（最小状态机 + 调度钩子） |
| Platform BSP | `timer.c` / `uart.c` / `cache.c` / `dma.c` / `interrupt.c` | 硬件外设驱动（已有 soc.h + DWT 实现）|
| NVMe Host | ~~`nvme_admin.c` / `nvme_io.c` / `submission_queue.c` / `completion_queue.c`~~ | ~~NVMe 协议层~~ **已删除（v0.47）**：学习主控不实现（D10），Host 命令来源 = UART CLI（`usart.c` wr/rd/tr/fl/info） |
| 平台计时钩子 | `ssd_platform_ops_t.time_us`（platform.h 已定义，stm32h743 未注册） | Checkpoint T3 定时 + GC 定时 + cp.timestamp 接线（见 v0.37 遗留）|

**FTL 层 22 个源文件，0 个 `#warning` 核心桩。全部模块已实现并通过编译。**

#### 形参优化 4 步全部完成（v0.28~v0.30，2026-09-30）

> 详见 `ftl_revision_history.md` v0.28~v0.30。设计动机：形参数过多（`pending_add(12)`、
> `ftl_submit_read/write(8)`、`ftl_internal_*_page(7)`、`ftl_init(7)`）+ 遍历路径无意义
> 打包拆包（`pba_make` → `ftl_cmd_setup_ppa` 往返）+ `super_block_t` 不含拓扑。

| 步骤 | 内容 | 形参变化 | 关键文件 |
|------|------|---------|---------|
| **Step 1** (v0.28) | 地址双层化：`nand_addr_t`（展开）+ `pba_from_addr/addr_from_pba/addr_from_paa` 双向转换；`super_block_t` 加 `topo` + `super_block_page_addr`；`addr_to_cmd` 直填 | — | nand_config.h / allocator.h / ftl_fc_interface.h |
| **Step 2** (v0.28) | 删除 4 个 `ftl_internal_*` 同步包装（-232 行），各模块内联 static sync 助手 | 7 → 0 | ftl_fc_interface.c + 9 模块 |
| **Step 3** (v0.29) | `ftl_submit_read/write(ftl, req)`；`ftl_pending_req_t` 按值嵌入 `io`；`pending_cb_t` 迁至 ftl_types.h | 8→2 / 12→6 | ftl_types.h / pending.h / ftl.h / controller |
| **Step 4** (v0.30) | `ftl_init` / `allocator_init(ftl, params)`；`ftl_init_params_t` 定义于 allocator.h | 7→2 / 8→2 | ftl.h / allocator.h / ftl.c |

**地址体系（最终态）**：

```
nand_addr_t（展开字段，遍历路径）
   ↕ pba_from_addr / addr_from_pba
nand_pba_t（32-bit 紧凑，L2P 表存储）   = [CH][CE][PAA]
   ↕ addr_from_paa（元数据固定位置 / Directory 查找）
nand_paa_t（芯片内地址）                = [LUN][BLOCK][PLANE][PAGE][CW]
```

**遍历路径**：`super_block_page_addr(sb, plane, page)` / `addr_from_pba(pba)` / `addr_from_paa(ch,ce,paa)`
→ `addr_to_cmd(cmd, &addr)` 直填 FC 命令（省打包拆包）。
**随机 IO 路径**：从 L2P 拿 PBA → `ftl_cmd_setup_ppa(cmd, pba)` 保留（PBA 已在手）。

---

## 16. Phase 规划
### 已实现（截至 2026-09-29 v0.22）

| 任务 | 说明 | 状态 |
|------|------|------|
| L2P Cache + CLOCK | 64 row SRAM region cache，CLOCK 替换，Region NAND load/flush | ✅ |
| Allocator + Super Block | Free Pool 三级**水位**，跨双 Plane，磨损均衡，Hot/Cold 双温区 | ✅ |
| Journal | 固定 **Block 58-61** 四 Block 环形，SRAM buffer + flush_page，Header 4 字段 | ✅ |
| GC | 7 步搬移：读 META→查 L2P→分配 Cold→写→L2P→Journal，贪心 victim | ✅ |
| Checkpoint | 全量 L2P 快照 + **池 48-55（8 块，一条 1 页，环内轮转）** + 布局版本，Journal 截断协同，三级触发 | ✅ |
| BBT | 出厂扫描（LDPC_RAW）+ **池 56-57（2 块，一条 1 页，环内轮转）** + 运行时标记 | ✅ |
| Read Recovery | Read Retry（8 次）+ 邻近页辅助 + ECC 趋势预测 + 4 级健康状态 | ✅ |
| Power Failure Recovery | 三类启动判断 + 写指针 NAND 扫描 | ✅ |
| Wear Leveling | 动态（alloc 选低擦除）+ 静态三星 Daemon（三条件触发）| ✅ |
| Metadata Directory | Anchor **池 16-47（32 块，环内轮转）** + 布局版本，L2P Region 双 Plane 交错 | ✅ |
| Pending Request 表 | 32 项固定表，command_id + LBA lookup，支持 complete_flag 通知 | ✅ |
| ftl_service 主循环 | 三级模式（IDLE/NORMAL/BUSY），GC/WL/Checkpoint 集成 | ✅ |
| ftl_fc_interface | 3 PBA helper + 4 内部同步 NAND 操作（read/write/erase/sync_wait/read_raw）| ✅ |
| **Completion Table** | 统一完成路由，消除两泵冲突，sycn_waiter 不走独立 fc_poll_completion | ✅ |
| **L2P Region 日志块** | `l2p_cache_flush_one` 每次 flush 都 `metadata_dir_append_slot` 追加到日志块（从 Free Pool 取整块），旧块活 region 归零即归还 | ✅ |
| **FTL 对外接口异步化** | 删除 ftl_read/ftl_write 同步包装，纯异步 ftl_submit_read/write + complete_flag 跟踪 | ✅ |
| **ftl_submit_write pending** | 写请求注册 pending + complete_flag，NAND PROGRAM 真完成后才通知 caller | ✅ |
| **Controller 异步提交** | ssd_controller_submit 纯异步，ssd_controller_is_complete 轮询完成状态 | ✅ |

### 已解决

| 编号 | 问题 | 解决方案 |
|------|------|---------|
| D4 | **多 LBA 拆分** | ✅ **方案 A — Controller 层拆分**（D7 修正：ftl_submit_read/write 加回 pages 参数支持 Multi-Plane） |
| D5 | **写失败回滚** | ✅ **modify-after-confirm + BBT 两阶段 + 自动回滚**：`journal_append`/`l2p_cache_write` 移到 completion handler 确认路径；pending 表加 old_pba/new_pba/ftl 字段；写失败→`bbt_mark_bad` RAM only，后台 `bbt_flush` 批量持久化 BBT；L2P 不动=自动回滚 old_pba；Super Block 空洞由 GC 自然跳过 |
| D6 | **队列深度/背压** | ✅ **pending 表满即隐式背压（DRAM-less 消费级）**：见下方 §17 D6 详细分析 |
| D7 | **多命令流水线** | ✅ **Multi-Plane + Cache Program 安全中间页**：见下方 §18 D7 详细分析 |
| D8 | **错误通知粒度** | ✅ **status_out 指针（消费级 DRAM-less）**：见下方 §19 D8 详细分析 |

### 剩余待办（对外接口）

以下为 FTL 对外接口层面已识别的问题（D4~D8 均已实现，仅 D3 遗留）：

| 编号 | 问题 | 说明 |
|------|------|------|
| D3 | **LBA 地址体系统一** | ssd_lba_t(uint64_t) 已废弃删除（v0.20 死代码清理），当前 lba_t=uint32_t 统一；`ftl_io_request_t.lba` 保留 uint64_t（NVMe 协议宽度）。剩余问题：UART/NVMe host 层入参宽度与 lba_t 的边界 |
| D4 | **多 LBA 拆分** | ✅ 已实现 — Controller 层拆分，`request.c` → `on_sub_complete` 回调聚合，FTL 层保持单页原语 |
| D5 | **写失败回滚** | ✅ 已实现 — modify-after-confirm + BBT 两阶段 + 自动回滚 |
| D6 | **队列深度/背压** | ✅ 已实现 — pending 表满即隐式背压（DRAM-less 消费级），不需额外机制 |
| D7 | **多命令流水线** | ✅ 已实现 — Multi-Plane（写总是双 Plane）+ pages 参数修正 D4 + Cache Program 安全中间页 |
| D8 | **错误通知** | ✅ 已实现 — status_out 指针传递 SSD_STATUS_OK/IO_ERROR，消费级完成状态通知 |

## 17. D6 队列深度/背压方案详细分析

### 17.1 问题背景

D4（多 LBA 拆分）后，单次 host 请求（`block_count=N`）会产生 N 个 FC 命令同时入队。当前 FC 资源池深度的基础上，多个提交者（host IO / GC / Metadata / BBT）同时竞争，可能出现队列/池满的情况。

D5（modify-after-confirm）后，写完成回调中使用 `ftl_context_t *ftl` 访问 ftl 上下文写入 Journal + L2P。这引入了 ftl 与 fc 边界之间的上下文依赖，但不影响队列模型。

### 17.2 当前资源预算

| 资源 | 宏 | 深度 | 谁在用 |
|------|------|------|--------|
| Command Pool | `FC_COMMAND_POOL_DEPTH` | 64 | 所有 FC 命令（host IO + GC + metadata + BBT + Checkpoint）|
| Descriptor Pool | `FC_DESCRIPTOR_POOL_DEPTH` | 256 | 每个命令占用 (cw+1) 个 desc |
| Buffer Pool | `FC_BUFFER_POOL_DEPTH` | 128 | 数据缓冲区（读/写） |
| SQ（普通队列） | `FC_SQ_DEPTH` | 64 | 普通优先级命令（用户数据） |
| HSQ（高优队列）| `FC_HSQ_DEPTH` | 16 | 高优先级命令（元数据操作） |
| CQ（完成队列） | `FC_CQ_DEPTH` | 64 | 已完成命令等待消费 |
| Pending 表 | `FTL_PENDING_MAX` | 32 | 用户数据 IO 的异步追踪 |
| 每轮提交预算 | `NAND_SUBMIT_BUDGET` | 4 | ftl_service 每轮最多提交 |
| LUN 数 | `LUN_NUM` | 1 | 硬件并发单元数 |

### 17.3 四种背压方案分析

#### 方案 ①：直接返回错误（当前做法）

**原理**：队列/池满时 `fc_submit()` 返回 `FC_STATUS_QUEUE_FULL` → `ftl_submit_*()` 返回 `FTL_ERROR` → Controller 层通知 host 错误。

```c
// fc_submit → QUEUE_FULL 时的传播路径
fc_submit()
  → fc_submit_internal()
    → fc_submission_queue_push()  // count == depth → QUEUE_FULL
    → return FC_STATUS_QUEUE_FULL

ftl_submit_write()
  → pending_add()           // pending 满 → FTL_ERROR
  → 或 fc_submit()          // QUEUE_FULL → FTL_ERROR
  → fc_command_release(cmd) // 回滚资源
  → return FTL_ERROR

controller.c / request.c
  → 收到 FTL_ERROR
  → 设置 completion_status = error
  → 设置 complete_flag = 1
  → host 收到错误，自行决定重试
```

| 方面 | 评价 |
|------|------|
| **复杂度** | 零——已有完整路径 |
| **资源回滚** | ✅ 完整（fc_command_release + pending_remove 齐全） |
| **延迟** | 低——即时返回 |
| **公平性** | ❌ 无优先级，谁先碰到谁失败 |
| **适用场景** | **DRAM-less 消费级**、资源深度 > 实际消费的场景 |

**行业使用**：eMMC/UFS 消费级控制器，NVMe 消费级 SSD（SRAM 资源紧张时）。

#### 方案 ②：内部排队重试（FTL internal retry queue）

**原理**：`fc_submit` 失败时不放弃，而是将命令放入一个内部重试队列，在后续 `ftl_service` 的 fc_service 阶段重试。

```c
// FTL 侧
ftl_submit_write()
  → fc_submit() → QUEUE_FULL
  → retry_queue_push(cmd)   // 放入重试队列

ftl_service()
  → Phase 1: fc_service()  + 重试队列弹出
    → for each pending in retry_queue:
        if (fc_submit(retry_cmd) == OK) remove from queue
  → Phase 2: completion_table_drain()
```

| 方面 | 评价 |
|------|------|
| **复杂度** | 中——需要额外的 retry queue + 重试逻辑 |
| **资源占用** | 中——retry queue 占用 SRAM（~cmd_handle × 深度） |
| **延迟** | 高——提交被推迟到下一轮 ftl_service |
| **公平性** | ✅ 可做（按入队顺序或优先级排序） |
| **适用场景** | 企业级、对服务质量有要求但资源有限的场景 |

**行业使用**：极少独立实现——多数企业级选择方案③或④。

#### 方案 ③：背压（backpressure via NAND_SUBMIT_BUDGET 动态调节）

**原理**：根据 SQ 或 pending 表的占用率动态调整 `NAND_SUBMIT_BUDGET`，从源头控制 FTL → FC 的提交速率。

```c
// ftl_service 中动态调节
void ftl_service(ftl_context_t *ftl)
{
    uint8_t budget = NAND_SUBMIT_BUDGET;

    // SQ 占用 > 75% → 限流
    if (fc_sq_usage() > (FC_SQ_DEPTH * 3 / 4))
        budget = 1;

    // pending > 75% → 主机 IO 限流（但允许 GC/metadata 继续）
    if (pending_usage() > (FTL_PENDING_MAX * 3 / 4))
        budget = (budget > 1U) ? 1U : 0U;

    for (uint8_t i = 0; i < budget; i++)
        fc_service();
    ...
}
```

| 方面 | 评价 |
|------|------|
| **复杂度** | 低-中——只需修改 ftl_service 中 budget 的计算 |
| **资源占用** | 极低 |
| **延迟** | 中——提交速率被主动限制，但不会彻底拒绝 |
| **公平性** | ✅ 可按模块分别限流（host IO 受限但 GC/metadata 不受限） |
| **适用场景** | 企业级、Host 压力可能超过处理后端的场景 |

**行业使用**：Samsung、Phison 企业级主控中常见，通常结合 Per-command credit 系统。

#### 方案 ④：主机级流控（NVMe doorbell backpressure）

> **本项目不实现**（v0.47 删 NVMe，Host = UART CLI；UART CLI 天然两段式等待完成，无需 doorbell 背压）。留档作行业对照。

**原理**：在 Host 协议层控制入侵。NVMe 协议原生支持 SQ doorbell 管理——Controller 固件可以指示 host "暂停提交新命令"。

```c
// 控制器层
ssd_controller_submit()
  → pending_usage() > 阈值
  → 不接收新命令，返回 SSD_STATUS_BUSY
  → 或 NVMe 层暂停处理 doorbell 更新
```

| 方面 | 评价 |
|------|------|
| **复杂度** | 高——需要 NVMe 协议支持 |
| **资源占用** | 最低（在 host 侧阻塞） |
| **延迟** | 最低（host 直接停发，固件不需要保留重试资源） |
| **公平性** | ✅ NVMe Arbitation 机制原生支持 |
| **适用场景** | **所有带 NVMe 接口的 SSD**（企业级和消费级都使用） |

**行业使用**：每款 NVMe SSD 的标准做法——通过主机端的 Submission Queue Tail Doorbell 寄存器更新来控制入侵。

### 17.4 方案对比

| 维度 | ① 直接错误 | ② 内部重试 | ③ 动态 budget | ④ 主机流控 |
|------|-----------|-----------|-------------|-----------|
| 实现复杂度 | **0** | **中** | **低-中** | **高** |
| SRAM 额外开销 | 0 | retry queue | 0 | 0 |
| 命令丢弃 | 是 | 否 | 否 | 否 |
| 延迟影响 | 最低 | 最高（推迟） | 中（主动减速） | 最低（host 不发） |
| 优先级支持 | 无 | 可做 | 可做 | NVMe 原生 |
| **适用阶段** | **当前（DRAM-less 消费级）** | 少用 | 多 LUN 阶段 | NVMe 阶段 |

### 17.5 选择理由

本项目选择 **方案①（直接返回错误 = pending 表满即隐式背压）**，基于以下定量分析：

**资源预算推算**：

```
资源        深度    实际占用峰值
────────────────────────────────
SQ          64      NAND_SUBMIT_BUDGET=4 每轮提交 ≤4
                    同时最多 pending(32) + 同步操作 ≤8
                    峰值 ≈ 36 << 64  ✅

Command Pool 64     SQ(36) + HSQ(GC/Metadata ~6) ≈ 42 << 64 ✅

Pending 表   32     D4 拆分 block_count=8 时占用 8/32
                    → 新的 host 提交会 pending_add FAIL
                    → 自动拒绝过量提交 → 这就是背压
```

核心逻辑：

1. **SQ 深度 64**：当前 LUN_NUM=1、pending=32、budget=4，实际需要 SQ 中的命令最多 ≈ 36，仍远小于 SQ 64。`FC_STATUS_QUEUE_FULL` 几乎不会发生。

2. **真正瓶颈仍是 pending 表（32 槽）**：pending 表满时 `pending_add()` 返回 `FTL_ERROR`，FTL 在 fc_submit **之前**就拒绝了请求，不会浪费 FC 资源池。扩到 32 后，单 LUN 的原始并发仍未吃满——它为多 LUN 并发的 pending 深度预留了上限。

3. **SRAM 是 DRAM-less 最贵的资源**：维护一个 retry queue（方案②）需要额外的 SRAM，与 DRAM-less 设计目标（最小化 SRAM 占用）相悖。

4. **NVMe 阶段自然过渡到方案④**：当 NVMe 层实现后，host 级流控（方案④）会成为主流——那时方案①作为 fallback 保留。

### 17.6 结论

| 决策 | 值 |
|------|-----|
| **当前方案** | ① 直接返回错误（pending 表满即背压） |
| **适用阶段** | DRAM-less 消费级，LUN_NUM=1，pending=32 |
| **未来升级路径** | P-A pending 表扩展后 → 必要时加方案③（动态 budget） |
| **远期** | NVMe 层实现后 → 方案④（主机级流控）成为主流 |
| **不采用** | 方案②（内部重试）— SRAM 开销不合理 |

## 18. D7 多命令流水线方案详细分析

### 18.1 问题背景

D4 实现了多 LBA 拆分，但 FTL 接口的 `count` 参数被完全删除，导致 FTL 无法得知单次操作涉及几个 Plane。D4 的设计假设"FTL 只做单页原语"过于绝对——Multi-Plane 操作中，FTL 必须知道 pages 数才能配置正确的 OP 码和 multi_plane 字段。

同时存在三种可用的 NAND 并行技术：

| 技术 | NAND 命令 | 效果 | 现状 |
|------|----------|------|------|
| **Multi-Plane** | `80h-11h-80h-10h` / `30h` 双地址 | 双 Plane 并行 tPROG/tR → ~2x 吞吐 | FC 层完整支持，FTL 层全部 `multi_plane=0U` |
| **Cache Program** | `80h-address-data-15h` 流水线 | tPROG 与后续数据加载重叠 → ~1.3-1.5x | OP 码已定义，dispatch 表已注册 |
| **ISSUE/WAIT/FETCH** | Phase V 拆分 | LUN 间时间片轮转 | 已部分实现 |

### 18.2 当前资源预算（D7 相关）

| 资源 | 值 | 说明 |
|------|-----|------|
| `PLANE_NUM` | 2 | 每 LUN 双 Plane |
| `LUN_NUM` | 1 | 当前单 LUN |
| `PAGES_PER_BLOCK` | 64 | Super Block 容量 = 64 页 |
| Super Block 结构 | `plane_blocks[2]` | 两个 Plane 各一个 Block，配对写入 |
| FC `multi_plane` 字段 | 3 bit | 0=single, N=N-plane desc 数 × N |
| FC OP 码 | 已定义 `OP_PROGRAM_PAGE_MULTI_PLANE(31)`, `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT(30)` | FTL 层全部用 `OP_NORMAL_*` |
| `FC_SQ_DEPTH` | 64 | 命令队列 |

### 18.3 三种流水线技术分析

#### Multi-Plane

**原理**：ONFI 标准中，一个命令序列可以携带两个 Plane 的地址和数据，NAND 内部双 Plane 同时烧录/读取。

**写流程（OP_PROGRAM_PAGE_MULTI_PLANE）**：
```
80h-C1C2R1_P0R2_P0R3_P0-Din_P0-11h-C1C2R1_P1R2_P1R3_P1-Din_P1-10h ~~~~ tPROG ~~~~ 70h-Status
      |--- Plane 0 地址+数据---|    |--- Plane 1 地址+数据---|
```
11h 是 ONFI Multi-Plane 切换命令，两 Plane 数据加载完后 10h 同时烧录。

**读流程（OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT）**：
```
00h-C1C2R1_P0R2_P0R3_P0-00h-C1C2R1_P1R2_P1R3_P1-30h ~~~~ tR ~~~~ Dout(双页)
```
一次 tR 后输出两 Plane 数据。

**FC 层 desc 分配**：
```c
// multi_plane=2, cw=3 → total_desc = 2 × 4 = 8
desc[0~3]: plane=0, cw=0~3    ← command->paa 自动拆
desc[4~7]: plane=1, cw=0~3
```

**效率**：双 Plane PROGRAM 的 tPROG 与单 Plane 相同（~500μs），因此吞吐翻倍。

#### Cache Program

**原理**：NAND 内部有 cache register 和 data register（主寄存器）两级缓冲。`15h` 将 cache register 数据拷贝到 data register 后立即释放 cache register，copy 过程与 data register 的 tPROG 重叠。

```
时间 →
  [LOAD₀→15h]  tCBSY  [LOAD₁→15h]  tCBSY  [LOAD₂→10h]  tPROG
       │══tCBSY══│           │══tCBSY══│           │══tPROG══│
       ↑cache→data          ↑cache→data          ↑真正等待完成
```

**ONFI 规格说明**（MT29F4G08ABADAWP 数据手册）：

> PROGRAM PAGE CACHE (80h-15h) copies data from the cache register to the data register; then moves the data register contents to the specified block and page address.
>
> After the data is copied to the data register, the cache register is available for additional commands.
>
> When the LUN's status shows busy with a PROGRAM CACHE operation (RDY=1, ARDY=0), the host should check the status of the FAILC bit to see if a **previous** cache operation was successful.

**关键时序参数**：
| 参数 | 值 | 含义 |
|------|-----|------|
| tCBSY | ~3μs | cache→data register 拷贝 |
| tPROG | ~500μs | data register→NAND array 烧录 |
| Cache hit | ~1.3-1.5x | tPROG 被后续 15h 数据加载部分掩盖 |

**掉电安全性分析**：

ONFI 规格中，`15h` 的 WAIT_READY 等待的是 tCBSY（cache→data register 拷贝完成），**不是** tPROG（data→NAND array 烧录完成）。tPROG 在 WAIT_READY 返回时仍在后台运行。

```
uop 序列（g_nand_program_cache_uops）:
  ① 70h 轮询等 LUN ready
  ② 80h + 地址 + 数据 → cache register
  ③ 15h → 开始 tCBSY
  ④ WAIT_READY          ← 等 tCBSY 完成（cache register 空闲）
  ⑤ 70h + CHECK_FAIL    ← 检查的是上一个 cache op 的 FAILC
  ⑥ END → completion
        ↑ dispatch 在此处向上发 completion → FTL 收到后 D5 确认路径
```

这意味着：**中间页（15h）的 completion 到达时，数据可能还未烧入 NAND 阵列**。如果在 tPROG 中途掉电，数据丢失，但 D5 的 modify-after-confirm 已更新 L2P/Journal → 指向无效物理页。

#### ISSUE/WAIT/FETCH

Phase V 已实现：`OP_READ_PAGE_LOAD`(35) / `OP_READ_PAGE_FETCH`(36) / `OP_PROGRAM_PAGE_ISSUE`(37) / `OP_PROGRAM_PAGE_WAIT`(38) 等。
用于多 LUN 并发时时间片轮转，当前 LUN_NUM=1 未激活。

### 18.4 三种技术的互斥关系

| 组合 | 可行性 | 原因 |
|------|--------|------|
| **Multi-Plane + Cache Program** | ❌ | ONFI 不支持。Multi-Plane 用 11h 切换 Plane，Cache Program 用 15h 结束，命令序列不兼容 |
| **Multi-Plane + ISSUE/WAIT** | ✅ | Multi-Plane 可作为单个 ATOMIC 命令，被 ISSUE/WAIT 包装 |
| **Cache Program + ISSUE/WAIT** | ✅ | Cache 流水线中的每一页可作为独立的 ISSUE/WAIT |
| **ISSUE/WAIT 自身** | ✅ | Phase V 已实现，用于多 LUN |

**场景选择策略**：

| 场景 | 技术 | 原因 |
|------|------|------|
| **Super Block 写入（host IO / GC 连续写入）** | Multi-Plane | Super Block 双 Plane 天生配对，tPROG 翻倍收益 |
| **大块顺序写入（连续多页，单 LUN）** | Cache Program | 多页流水线，tPROG 重叠 |
| **多 LUN 并发** | ISSUE/WAIT + multi-plane per LUN | LUN 间时间片 + 单 LUN 内 Multi-Plane |
| **随机单 LBA** | 单 Plane | 没有配对机会 |

### 18.5 存储主控做法

企业级/消费级主控统一采用以下策略：

```
Controller 层:
  ┌── 识别连续 LBA 对 → 双 Plane Multi-Plane（pages=2）
  ├── 识别连续多页写序列 → Cache Program（pending 标记 cache_intermediate）
  └── 随机/奇数 LBA → 单 Plane（pages=1）

FTL 层:
  ┌── 接收 pages 参数，构造对应 OP 码 + multi_plane
  ├── completion handler 中检查 cache_intermediate
  │   ├── 0 = 正常确认（Journal + L2P）
  │   └── 1 = 中间页跳过确认（等最终 10h）
  └── PBA 无效自动回滚
```

### 18.6 实现方案

#### 修正 D4：加回 pages 参数

D4 将 `count` 完全删除不恰当。FTL 需要知道单次操作涉及的页数以配置正确的 FC 命令参数。

```c
// v0.29 签名（8 形参 → 2，ftl_io_request_t 结构体传参）
ftl_status_t ftl_submit_read(ftl_context_t *ftl, const ftl_io_request_t *req);
ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req);
```

| req->pages | 含义 | OP 码 | multi_plane | 数据量 |
|-------|------|-------|------------|--------|
| 1 | 单 Plane | `OP_NORMAL_*` | 0 | 2048 |
| 2 | 双 Plane Multi-Plane | `OP_*_MULTI_PLANE` | 2 | 4096 |

#### Multi-Plane 实现

写路径（`ftl_submit_write`）：
- pages=2 → `OP_PROGRAM_PAGE_MULTI_PLANE` + `multi_plane=2` + data size=4096
- Super Block 写入天生双 Plane → 写总是 Multi-Plane

读路径（`ftl_submit_read`）：
- pages=2 → `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` + `multi_plane=2`
- 顺序读配对：Controller 层两两连续 LBA 合并

#### Cache Program 安全机制

```c
typedef struct {
    ...
    uint16_t pages             : 2;   // 操作涉及的页数
    uint16_t cache_intermediate : 1;  // 1=Cache Program 中间页
} ftl_pending_req_t;

// ftl_handle_completion 中：
if (NAND_SUCCESS && buffer == NULL)   // 写完成
{
    if (pending->cache_intermediate == 0)
    {
        journal_append(...);            // 更新 Journal
        l2p_cache_write(...);           // 更新 L2P
    }
    // cache_intermediate=1 → 跳过，等最终页完成
}
```

**最终页（10h）**正常触发 L2P/Journal 更新，前面的中间页（15h）只发 complete_flag 和 on_complete 回调用于 Controller 聚合，但不更新 FTL 元数据。

如果中间页 NAND_FAIL：标记坏块 + 回滚（与 D5 机制一致，L2P 不动）。

### 18.7 Controller 层配对策略

`ssd_request_split` 中：

```
写拆分：两两配对 pages=2
  block_count=8 → [p0p1][p2p3][p4p5][p6p7] → 4 次 Multi-Plane

读拆分：两两配对 pages=2
  block_count=8 → 4 次 Multi-Plane（顺序读覆盖）
  block_count=3 → 1 次 Multi-Plane + 1 次单 Plane（奇数剩余）
```

`sub_remaining` 计数器修正为 `ceil(block_count/2)`，因为一次 Multi-Plane 命令覆盖两个 LBA。

### 18.8 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/ftl.h` | `ftl_submit_read/write` 加 `pages` 参数 |
| `include/ftl/pending.h` | 结构体加 `pages` + `cache_intermediate`；`pending_add` 签名加 2 参数 |
| `src/ftl/ftl.c` | `submit_read/write` pages 分支 + Multi-Plane OP 码；`handle_completion` cache 中间页跳过确认 |
| `src/ftl/pending.c` | `pending_add` 实现加 2 参数 |
| `src/controller/controller.c` | 单块直通传 pages=1 |
| `src/controller/request.c` | 配对拆分 pages=2，奇数 pages=1；`sub_remaining` 修正为 `ceil(N/2)` |

### 18.9 其他内部函数不受影响

| 函数 | 原因 |
|------|------|
| `sync_write_page/sync_read_page`（各模块内联 static） | 元数据操作不走 Super Block，保持 `multi_plane=0` |
| `sync_erase_block`（gc/journal 内联） | 擦除 Multi-Plane OP 码（OP_ERASE_BLOCK_MULTI_PLANE）暂不使用 |
| `gc.c` | GC 逐页搬移，`multi_plane=0` |
| `ftl_fc_interface.c` | v0.28 起仅保留 `ftl_internal_sync_wait`（统一完成路由） |

### 18.10 结论

| 技术 | 状态 | 实现 |
|------|------|------|
| **Multi-Plane 写** | ✅ 已实现 | Super Block 写入总是 pages=2 |
| **Multi-Plane 读** | ✅ 已实现 | Controller 层连续 LBA 配对 pages=2 |
| **Cache Program** | ✅ 基础设施就绪 | OP 码 + dispatch 表已注册；FTL 层用 `cache_intermediate` 确保掉电安全；实际使用预留 |
| **ISSUE/WAIT** | ⏳ Phase V 已部分实现 | 多 LUN 时启用 auto-split |

### 剩余待办（非对外接口，未实现待处理）

#### P-A：多 LUN 并发前置（依赖 Completion Table 已就绪）

| 任务 | 说明 | 优先级 |
|------|------|--------|
| P-A.1 | `fc_poll_completion` CQ 深度确认 — 确保 CQ 能同时容纳多 LUN 的完成事件 | 高 |
| P-A.2 | pending 表扩展 — `FTL_PENDING_MAX` 从 8 增大到 32 ✅ **（v0.31 已实现）** | 高 |
| P-A.3 | `ftl_write` 同步等待改为 per-cmd sync_waiter — 脱离全局 SQ/CQ 空判定，改用 `completion_table_add` + 等具体 cmd_id | 高 |
| P-A.4 | `rr_record_ecc` 接入读完成路径 — 当前为 ⚠️ 未接入，需 timer + completion 集成 | 中 |

#### P-B：Controller / Host 层

| 任务 | 说明 | 状态 |
|------|------|------|
| P-B.1 | Controller 请求拆分（`request.c`） | ✅ 已实现 — `ssd_request_split` + `on_sub_complete` 回调聚合 |
| P-B.2 | 优先级调度（`scheduler.c`） | ✅ v0.47 — `ssd_controller_schedule` 轻量钩子（host IO 优先，后台预算由 `ftl_service` B3 空闲度承担） |
| P-B.3 | 状态机管理（`state_machine.c`） | ✅ v0.47 — `ssd_controller_state_transition` 最小状态机（INITIALIZING→READY/FAILED） |
| P-B.4 | NVMe Admin 命令（`nvme_admin.c`） | ✅ v0.47 删除 — 学习主控不实现（D10：UART CLI 驱动 FTL） |
| P-B.5 | NVMe IO 命令（`nvme_io.c`） | ✅ v0.47 删除 — 同上 |
| P-B.6 | Submission/Completion Queue（`submission_queue.c` / `completion_queue.c`） | ✅ v0.47 删除 — 同上 |

#### P-C：优化

| 任务 | 说明 | 备注 |
|------|------|------|
| P-C.1 | Prefetch 顺序读预加载 | 后续 Phase |
| P-C.2 | GC L2P 批量更新优化 | 后续 Phase |
| P-C.3 | Warm boot 保留 cache | 后续 Phase |
| P-C.4 | 性能计数器 + 调试接口 | 后续 Phase |
| P-C.5 | 反向映射 bitmap（每 SB 64 bit 有效页 bitmap） | ⚠️ **已删除（v0.60）**——v0.32 实现 per-plane 64bit 位图，v0.55 搬为独立表，v0.60 整表删除改 **DRAM-less approach ③**（逐 CW 读页内 META 反查 L2P，见 §4.9.9）|
| P-C.6 | Thrashing 检测 + 降级 | ✅ **v0.53 已落地**（窗口 miss 率 ≥90% 停 NORMAL 主动 flush；**不做** write-through，见 §6.5）|

### 已废弃（旧 Phase 规划中的已完成项）

以下为旧 §16 Phase 规划中已实现的任务，不再追踪：

~~Phase 0~~ ✅ Phase 1 ~~掉电恢复~~ ✅ Phase 2 ~~完整 FTL~~ ✅

---

## 19. Block State Table 产品化裁剪路径（档三：SRAM 核心 + NAND 扩展）

> **v0.32 记录，本期不实现**。全常驻 SRAM（56B/224KB）能放下（剩 ~537KB），但违背 DRAM-less 纪律。
> 行业真实做法是分层。当 SRAM 压力真实出现 + 扩展字段有消费方时，按本路径裁剪。
>
> ⚠ **v0.60 更新**：本档三路径的**最后一块内容也已消失**——v0.55 把位图搬成独立表（结构前提落地），
> **v0.60 索性把该表整体删除**（DRAM-less approach ③，§4.9.9）。⇒ 档三已**无剩余可裁剪项**：
> 条目 80KB 全常驻且 7 个字段全有消费方；无位图表。故本档三**已无动作**，仅存裁剪方法论。

### 19.1 为什么需要

| 项 | 现状（全常驻） | 产品化（档三） |
|---|---|---|
| SRAM BST | **80KB**（条目 20B × 4096，**无位图表**，v0.60） | 无剩余可裁剪项 |
| 扩展字段 | SRAM 常驻 | NAND 持久化（v0.56 后已无冷字段可外置） |
| 适用 | 功能验证阶段 | 产品化裁剪 |

### 19.2 分层架构

```
SRAM 核心（全常驻，20B/块 = 80KB）：
    erase_count + last_pe_ts + read_count + valid_count + state + sb_id + rr
（v0.60：位图表已删 ⇒ 有效性逐 CW 现查页内 META，无常驻项）
NAND 扩展表（ftl_block_ext_t[]，SRAM 不占）：**当前为空**
    v0.56 删掉 program_count / gc_generation 后，条目里**再没有「写了少用」的冷字段**
    可以外置——剩下的 7 个字段全部有消费方，必须常驻。
```

> 原设计的 `ftl_block_ext_t { program_count, last_pe_ts, gc_generation }`（8B/块落 NAND）
> **已作废**：三个候选字段中两个（program/gc_gen）v0.56 删除，`last_pe_ts` 由 P1-1 的
> GC victim 打分消费、须常驻。扩展表在字段层面**已无内容**，保留本节仅作裁剪方法论记录。

### 19.3 加载流程（启动）

```
ftl_init
  → allocator_init（BST memset 0）
  → bbt_init（从 NAND BBT 加载 bad bit → state.BAD）
  → wear_init（从 Wear 表回填 erase_count / read_count，P0-1）
  → （扩展表当前为空，v0.56 起无字段可加载）
```

### 19.4 存储流程（周期落盘）

```
（v0.56 起无扩展字段可落盘）
擦除/读计数由 Wear 表持久化（src/ftl/wear.c，页式追加日志），见 §4.9.8 与 P0-1。
```

### 19.5 为什么本期不做

1. 扩展表持久化子系统（序列化 + CRC + 冗余 + 加载/落盘时机 + 脏跟踪）比 BBT 更复杂
2. 原候选冷字段 `program_count` / `gc_generation` **v0.56 已删**（无消费方）；
   剩下的字段全部有消费方，**无可外置者** ⇒ 扩展表的动机已消失
3. **功能优先全常驻 → 产品化裁剪**是行业流程；SRAM 压力真实出现 + 字段有消费方后再做，
   届时已知拆谁、有数据驱动

> **当前状态（v0.60）**：BST 全常驻 = **80KB**（条目 20B × 4096，无位图表），AXI_RAM 实用
> **~255 / 384 KB**（`src/main.c:132-138` 有 `_Static_assert` 兜底）。位图表 64KB（v0.55 独立表）
> 与冷字段（`program_count`/`gc_generation`）先后删除后，**本裁剪路径已无剩余动作**
> （详见 `ftl_optimization_plan.md` §8.1 P2-1）。

### 19.6 裁剪顺序（若需降内存）

| 裁剪 | 省内存 | 代价 |
|---|---|---|
| ~~砍 `program_count`~~ | ~~8KB~~ | ✅ v0.56 已删 |
| 砍 `read_count` | 8KB | read disturb 预铺失去 |
| ~~砍 `gc_generation`~~ | ~~4KB~~ | ✅ v0.56 已删 |
| `last_pe_ts` → NAND | 16KB | WL 冷热避聚失去（P1-1 GC victim 打分消费，须常驻） |
| ~~`valid_map` → GC 临时扫~~ | ~~64KB~~ | ✅ **v0.60 已删**（DRAM-less approach ③：逐 CW 读页内 META 反查 L2P，§4.9.9） |

---

## 20. 附录：核心数据结构原型

> ⚠️ **注意**：本附录为**旧 Phase 规划时期的历史原型**（v0.22 之前），其中的接口签名
> （`ftl_init(ftl)`、`allocator_init(alloc, ch, ce, lun, op_percent)`、`ftl_read/write` 同步包装等）
> **已被 v0.22~v0.30 的实际实现取代**。当前接口以头文件为准：
> - `ftl_init(ftl_context_t *ftl, const ftl_init_params_t *params)`（v0.30，8 形参 → 2）
> - `ftl_submit_read/write(ftl_context_t *ftl, const ftl_io_request_t *req)`（v0.29）
> - `ftl_read/ftl_write` 已删除（v0.22 异步化）
> 下表仅作设计演进追溯。

### L2P Cache 接口

```c
// l2p_cache.h — 完整接口

#define L2P_ENTRIES_PER_REGION  (512)
#define L2P_REGION_SIZE         (2048)      // 512 × 4B
#define L2P_CACHE_SIZE          (64)        // 可调: 32/64/128
#define L2P_REGIONS_TOTAL       (2048)      // LBA 空间总 region 数
#define L2P_REGIONS_PER_LUN     (L2P_REGIONS_TOTAL / LUN_NUM)  // v0.35: 每 (ch,ce,lun) 表容量
#define L2P_BLOCKS_PER_LUN      (L2P_REGIONS_PER_LUN / 64U)    // v0.35: 每 LUN 表块数

typedef struct {
    nand_pba_t entries[L2P_ENTRIES_PER_REGION];  // 512 PBA = 2KB
    uint16_t   region_id;                        // 0..L2P_REGIONS_PER_LUN-1（本表内编号，v0.35）
    uint8_t    state : 2;      // INVALID=0, CLEAN=1, DIRTY=2, FLUSHING=3
    uint16_t   access_count : 14;
} l2p_cache_line_t;

typedef struct {
    l2p_cache_line_t lines[L2P_CACHE_SIZE];
    uint16_t         clock_hand;
    uint32_t         hit_count, miss_count;
    uint8_t          ch, ce, lun;               // v0.35: 本 cache 绑定拓扑
} l2p_cache_t;

void       l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce, uint8_t lun);  // v0.35 绑定拓扑
nand_pba_t l2p_cache_read(l2p_cache_t *cache, lba_t local_lba);   // v0.35 收片内 local
void       l2p_cache_write(l2p_cache_t *cache, lba_t local_lba, nand_pba_t pba);
void       l2p_cache_flush_one(l2p_cache_t *cache, int idx);
void       l2p_cache_flush_all(l2p_cache_t *cache);
```

### Journal 接口

```c
// journal.h — 完整接口

#define JOURNAL_BLOCKS           (CW_NUM)  /* 实现值 = 4（初稿写 2，见 §14.1 订正）*/
#define JOURNAL_ENTRIES_PER_PAGE  ((JOURNAL_PAGE_SIZE - JOURNAL_HEADER_SIZE) / sizeof(l2p_journal_entry_t))  /* 推导值 = 127 */

typedef struct {
    uint32_t    lba;
    nand_pba_t  old_pba;
    nand_pba_t  new_pba;
    uint32_t    sequence;
    /* 16B，无独立 crc：整页完整性由 FC per-CW CRC + ECC 保证（v0.62）*/
} __attribute__((packed)) l2p_journal_entry_t;

typedef struct {
    uint32_t current_block;
    uint32_t current_page;
    uint32_t current_entry;
    uint32_t sequence_counter;
} l2p_journal_t;

void journal_init(l2p_journal_t *journal);
void journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old, nand_pba_t new);
int  journal_replay(l2p_journal_t *journal, l2p_cache_t *cache, uint32_t cp_sequence);
void journal_truncate(l2p_journal_t *journal, uint32_t seq);
void journal_flush_page(l2p_journal_t *journal);
```

### Allocator 接口

```c
// allocator.h — 完整接口

typedef struct {
    uint16_t  open_block[PLANE_NUM];      // 每 Plane 当前写入 Block
    uint8_t   next_page[PLANE_NUM];       // 每 Plane 当前写入页
    uint16_t *free_blocks;                // 空闲 Block 列表
    uint16_t  free_count;                 // 空闲 Block 数
    uint16_t *bad_blocks;                 // 坏 Block 列表
    uint16_t  bad_count;                  // 坏 Block 数
    uint32_t *erase_counts;               // 每 Block 擦除次数
    uint16_t  total_blocks;               // 总好 Block 数
} ftl_allocator_t;

int        allocator_init(ftl_allocator_t *alloc);   /* 旧原型，见顶部 ⚠️ 注 */
nand_paa_t allocator_alloc(ftl_allocator_t *alloc);
void       allocator_free_block(ftl_allocator_t *alloc, uint16_t block_no);
void       allocator_mark_bad(ftl_allocator_t *alloc, uint16_t block_no);
```

### FTL 读写接口

```c
// ftl.h — FTL 对外 API

typedef struct ftl_context {
    l2p_cache_t       cache;
    ftl_allocator_t   alloc;
    l2p_journal_t     journal;
    uint32_t          cp_sequence;
    uint32_t          cp_interval;
    uint64_t          total_writes;
    uint32_t          cache_hits, cache_misses;
} ftl_context_t;

int  ftl_init(ftl_context_t *ftl);   /* 旧原型，见顶部 ⚠️ 注 */
int  ftl_read(ftl_context_t *ftl, lba_t lba, void *data, uint32_t count);
int  ftl_write(ftl_context_t *ftl, lba_t lba, const void *data, uint32_t count);
void ftl_trim(ftl_context_t *ftl, lba_t lba, uint32_t count);
void ftl_service(ftl_context_t *ftl);
void ftl_background_flush(ftl_context_t *ftl);