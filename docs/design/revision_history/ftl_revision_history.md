# FTL 设计变更历史

> **本文只回答「为什么变成现在这样」**；当前设计在 [`../ftl_design.md`](../ftl_design.md)。
> 历史事实不改写；后续版本的更正写在新条目里；验证记录只写当时结论，不因后续通过而追认。
> 被否方案见 §5；旧符号/旧文件名见 §6；逐提交 diff 以 `docs/archive/changelog/` 为准。

## 0. 文档定位与使用

| 问题 | 看哪里 |
|---|---|
| 当前 FTL 怎么设计 | [`../ftl_design.md`](../ftl_design.md) |
| 某个机制为什么这样、换过什么方案 | §2 / §3 |
| 已经否决过的方向 | §5 |
| 旧符号/旧文件名 | §6 |
| 缺陷 / 未实现项 | [`../known_open_items.md`](../known_open_items.md) |

**记录口径**：裸写 `§N` 指**当时**的 `../ftl_design.md` 章节；指向本文条目写 `vX 条目`。

## 1. 版本总览

### 1.1 功能阶段

| 阶段 | 版本范围 | 主题 |
|---|---|---|
| Phase 0 — 初始实现与 DRAM-less 定型 | v0.1–v0.2 | 从「绕过 FC 的同步页映射」转为完整 DRAM-less 设计；确定纯 CW 页映射、NAND 驻留 L2P、CLOCK、write-back + journal、跨 plane SB、10% OP 等 D1–D12。 |
| Phase 1 — 模块实现与基础设施 | v0.18–v0.30 | 完成 FTL 全模块、Completion Table、返回值统一、拓扑结构、地址双层化与形参优化（`ftl_submit_*` 改 `(ftl, req)`）。 |
| Phase 2 — 多 LUN / 拓扑 / 地址体系 | v0.31–v0.37 | 多 LUN 并发前置、BST 统一 per-block、数据平面多维化（L2P 分片 + ch/ce/lun）、Controller 三维路由、`main.c` FTL 上线。 |
| Phase 3 — 掉电恢复 / 元数据 / Checkpoint | v0.38–v0.50 | 写失败 SB 关闭、closed SB 重建、Checkpoint 增量、Journal 全量回放、Free Pool 三级、元数据写模型统一、Wear 持久化。 |
| Phase 4 — 算法档（GC / WL / L2P / Journal） | v0.51–v0.60 | GC victim 加权与触发补齐、L2P cache thrashing、Journal 寿命修复、BST 瘦身、CW 级映射、DRAM-less 有效性判定。 |
| Phase 5 — 介质布局 / 多实例 / 页打包 | v0.61–v0.74 | 介质布局版本、元数据 CRC 下沉、GC 触发对齐 DRAM-less、写→掉电恢复、多实例、OOB 出厂坏块、页打包 wbuf、地址 PBA 化、OP/Spare 按 plane。 |
| Phase 6 — 提交/完成解耦与后台调度 | v0.75–v0.82 | 提交与完成解耦（八阶段异步化）、时间预算后台调度器、allocator 账本收口、L2P 哨兵删除、元数据异步回收、固定元数据镜像、Wear 并入 Checkpoint。 |
| Phase 7 — 收口与上板稳定化 | v0.83–v1.10 | USB 数据面修复、L2P 日志固定区、状态码非负、元数据分区、GC 收益判据、region 主备、静态 WL UBI、cache read/program、跨块续读、volatile write cache。 |

### 1.2 版本索引

| 版本 | 日期 | 主题 |
|---|---|---|
| v0.1 | 2026-09-21 前 | 初始实现（Phase Unknown） |
| v0.2 | 2026-09-23 | 完整 DRAM-less 设计 |
| v0.18 | 2026-09-29 | FTL 全部模块实现 + 代码审视修复 |
| v0.19 | 2026-09-29 | Completion Table 统一完成路由 + L2P Region 动态分配 |
| v0.20 | 2026-09-29 | 返回值体系统一 + 历史死代码清理 + WL/GC 重复消除 |
| v0.21 | 2026-09-29 | FTL 拓扑信息重构（ftl_topology_t） |
| v0.23 | 2026-09-29 | D4 多 LBA 拆分（Controller 层方案 A） |
| v0.24 | 2026-09-29 | D5 写失败回滚（modify-after-confirm + BBT 两阶段 + 自动回滚） |
| v0.25 | 2026-09-29 | D6 队列深度/背压方案确认（pending 表满即隐式背压） |
| v0.26 | 2026-09-29 | D7 多命令流水线（Multi-Plane + Cache Program 安全中间页） |
| v0.27 | 2026-09-29 | D8 错误通知粒度（status_out 消费级完成状态） |
| v0.28 | 2026-09-30 | 地址体系双层化 + 形参优化 Step 1/2（nand_addr_s + addr_to_cmd + 同步操作内联化） |
| v0.29 | 2026-09-30 | 形参优化 Step 3（ftl_submit_read/write → (ftl, req) + pending 嵌入 io） |
| v0.30 | 2026-09-30 | 形参优化 Step 4（ftl_init / allocator_init → ftl_init_params_t 指针传参） |
| v0.31 | 2026-09-30 | P-A 多 LUN 并发前置 + read recovery 接入 |
| v0.32 | 2026-09-30 | Block State Table（BST）统一 per-block 结构体数组 |
| v0.33 | 2026-10-01 | FTL 内部边缘缺口闭环（write 路径紧急 GC + RR Stage 3 兜底） |
| v0.34 | 2026-10-01 | P-A 多 LUN 命令层机制启用（ISSUE/WAIT/FETCH + auto-split 活代码） |
| v0.35 | 2026-10-01 | 数据平面多维度化（L2P 分片 + ch/ce/lun 泛化 + 路由哈希） |
| v0.36 | 2026-10-01 | Controller FTL 实例三维路由接入（ftl[CH][CE][LUN] + 按 LBA 路由） |
| v0.37 | 2026-10-01 | main.c FTL 上线（✅ 已实现） |
| v0.38 | 2026-10-02 | F1/F3/F5 闭环（time_us 钩子 + predictive 搬移 + Multi-Plane 双页簿记） |
| v0.39 | 2026-10-02 | F7 写失败 Super Block 关闭（行业坏块替换流程） |
| v0.40 | 2026-10-02 | F2 掉电重建 closed_sb_list（全 NAND 扫描） |
| v0.41 | 2026-10-02 | F4 checkpoint 增量（region_bitmap） |
| v0.42 | 2026-10-02 | F6 pending/completion 多实例化（per-instance pending + 全局 completion 路由） |
| v0.43 | 2026-10-02 | F10 journal_replay 全量回放 + GC/WL 擦除前强制 flush |
| v0.44 | 2026-10-03 | FTL 未落地项体检 + A 档落地（OP 10% + Spare Pool + 三级 Free Pool 状态位） |
| v0.45 | 2026-10-03 | B 档落地（journal 水位修复 + 标志复位 + ftl_service 空闲度 + GC/WL 读记 ECC + read_count 消费） |
| v0.46 | 2026-10-03 | B 档复查修复（B1 水位分子口径 + B4 死变量清理） |
| v0.47 | 2026-10-03 | C 档落地（最小状态机 + host IO 优先调度 + FLUSH + UART CLI 驱动 FTL；NVMe 删除） |
| v0.48 | 2026-10-04 | Block State Table 瘦身（ftl_block_t 56B → 40B） |
| v0.49 | — | P0-1 计数持久化（Wear 表 load/save） |
| v0.50 | 2026-10-12 | P0-2 元数据写模型统一（追加日志 + 块池轮转） |
| v0.51 | 2026-10-12 | P1 算法档（GC victim 加权 + GC 触发补齐 + 读写判别修复） |
| v0.52 | 2026-10-12 | P1-3 Free Pool「三级水位」（原题「真三级化」的定案更正） |
| v0.53 | 2026-10-12 | P1-4 L2P cache thrashing 检测 + 降级（初稿三条降级只保留一条） |
| v0.54 | 2026-10-12 | P1-6 Journal 寿命瓶颈（环形块双倍擦除修复 + 条目合并） |
| v0.55 | 2026-10-12 | 逐页有效位图搬出 BST 条目（位图与条目的分离） |
| v0.56 | 2026-10-12 | 删 BST 死字段（条目 24B → 20B；Wear 条目 12B → 8B） |
| v0.57 | — | P2-2 结案（SB 索引粒度：前提被 Policy B 作废） |
| v0.58 | 2026-10-13 | FTL 对外接口对齐主流（plane 策略下沉 + 组完成语义） |
| v0.59 | 2026-10-13 | CW 级映射迁移（IU = 512 B；`ftl_optimization_plan.md §九` 波 2 序 2） |
| v0.60 | 2026-10-13 | 有效性切 DRAM-less approach ③（删常驻位图） |
| v0.61 | 2026-10-13 | 介质布局版本标记 + 开机自动格式化（顺带修 Anchor 页 CRC 覆盖面缺陷） |
| v0.62 | 2026-10-13 | 元数据校验和下沉：删 FTL 记录内 CRC，改由下层 FC per-CW CRC + ECC 承担 |
| v0.63 | 2026-10-13 | GC 触发对齐 DRAM-less 主流：删无出处触发（T2/T3）+ 统一求值点 + 修 IDLE 补池下溢 |
| v0.64 | 2026-10-13 | 修「写→掉电→重启丢数据」：干净关机路径从不恢复 Journal RAM 写态 |
| v0.65 | 2026-10-13 | 波 2 剩余 + 波 3：表自持 / 多实例 / OOB 出厂坏块 / 分片模型定案 / L2P 日志块压缩 / rr 持久化 |
| v0.66 | 2026-10-13 | P2-6 定案（器件/标准约束）+ 页打包写缓冲（§七 方案 A） |
| v0.67 | 2026-10-14 | 修 `allocator_close_sb` 的 valid 顺序缺陷：运行期关闭的 SB 对 GC/WL 永久不可见 |
| v0.68 | 2026-10-14 | GC 触发求值加节拍门（+ 触发日志走自己的节拍 + 进展计数） |
| v0.69 | 2026-10-14 | 修 `ftl_is_metadata_block` 的 plane 掩码：被掩的块落在所有池之外，L2P 悬空撞断言 |
| v0.70 | 2026-10-14 | 断言记录是陈旧的：加 `map <lba>` / `crash [clear]` 两条诊断 CLI |
| v0.71 | 2026-10-14 | `crash` 上板验证通过 + 一条未闭合的 HardFault 记录（纯文档） |
| v0.72 | 2026-10-14 | 结构重整：地址与布局的「归属归位」（纯文件组织，介质格式不变） |
| v0.73 | 2026-10-14 | FTL 层地址全面 PBA 化 + PBA 位域视图（介质格式变更，版本 3→4） |
| v0.74 | 2026-10-14 | OP / Spare 池改按 plane 预留（介质格式变更，版本 4→5） |
| v0.75 | 2026-10-14 | 命令「提交」与「完成」解耦（阶段 1 机制 + 阶段 2 Journal 刷页异步 + 阶段 3 Read Recovery 异步 + 阶段 4 GC / WL 搬移异步 + 阶段 5 L2P region 加载写回异步 + 阶段 6 Checkpoint 保存异步 + 阶段 7 BBT / Wear 持久化异步 + 阶段 8 metadata 目录压缩 / 换块异步） |
| v0.76 | 2026-10-15 | 时间预算后台调度器（三级后台升级：每任务 interval + 每轮总预算 + 优先级） |
| v0.77 | 2026-10-16 | Allocator 不变量收口（账本访问器 + 三池对称 push/pop；纯重构，无介质变更） |
| v0.78 | 2026-10-16 | 修 Closed SB 列表容量：`SUPER_BLOCK_LIST_MAX` 1024 → `FTL_BLOCK_TOTAL` |
| v0.79 | 2026-10-18 | 删除 L2P 哨兵层：未映射改由 Directory 条目 `NAND_PBA_INVALID` 表示 |
| v0.80 | 2026-10-18 | 元数据回收全面异步化：待回收位图 + job 拆分 + 观测收口 |
| v0.81 | 2026-10-18 | 固定元数据跨 plane 镜像（Anchor/Checkpoint/BBT）+ Journal multi-plane 擦除 |
| v0.82 | 2026-10-18 | 磨损计数并入 Checkpoint：删除独立 Wear 块 |
| v0.83 | 2026-10-20 | USB 数据面暴露的三处 FTL 修复（wbuf plane 镜像 / 回收前 Anchor / L2P 降级可见性） |
| v0.84 | 2026-10-20 | L2P 日志区改为固定预留（方案 A，RISK-MD-07 / RISK-CAP-02） |
| v0.85 | 2026-10-20 | 返回值状态码统一为非负（H46） |
| v0.86 | 2026-10-21 | 元数据分区改为 BBT + spare 候选池；容量/OP/写重试收口 |
| v0.87 | 2026-10-21 | 擦除页识别 + GC 按写指针截断 + 系统性读失败熔断修复 |
| v0.88 | 2026-10-22 | 开卡体检判据改为「擦除成功即好块」 |
| v0.89 | 2026-10-22 | GC 空间回收加收益判据（满盘只读不再空转） |
| v0.90 | 2026-10-22 | L2P region 主备副本 + 范围化恢复（单坏页不再全盘重建） |
| v0.91 | 2026-10-22 | 静态 WL 按 UBI 模型重写（idle 无限搬迁修复） |
| v0.92 | 2026-10-22 | 写确认 Journal 容量改为非阻塞预留（confirm 内不再同步刷页） |
| v0.93 | 2026-10-23 | L2P 顺序预取 + FTL two-plane cache read（未上板） |
| v0.94 | 2026-10-23 | FTL cache program（host 16 KiB + GC/WL 批次，host 正常路径已上板） |
| v0.95 | 2026-10-23 | Free Pool 水位回退 32 |
| v0.96 | 2026-10-23 | 移除 T5 磨损 GC，磨损均衡统一走 WL（已上板） |
| v0.97 | 2026-10-23 | DTCM/ITCM 热路径（正常路径已上板；压力/掉电未覆盖） |
| v0.98 | 2026-10-23 | 可读性重构（未上板） |
| v0.99 | 2026-10-24 | allocator 命名统一（未上板） |
| v1.00 | 2026-10-25 | cache read 判据修正（已上板） |
| v1.01 | 2026-10-29 | 注释规范统一（FTL 全量，未上板） |
| v1.02 | 2026-10-30 | cache read/write run 标成 FC chain（未上板） |
| v1.03 | 2026-10-30 | FC 状态字重构：descriptor / command 分开（未上板） |
| v1.04 | 2026-10-31 | cache 序列改由 FTL 顺序流检测器驱动 + Journal 顺序映射页（设计 A；2026-10-31 上板验证） |
| v1.05 | 2026-10-31 | 设备序列跨 host 命令：volatile write cache + padding END（2026-10-31 上板验证） |
| v1.06 | 2026-10-31 | 读写序列跨物理 block（器件原文判据；2026-10-31 上板验证） |
| v1.07 | 2026-10-31 | 低 free 成对分配失败：per-plane 水位 + victim 平衡 + 错误语义（2026-10-31 上板验证：生路；漂移由 v1.08 修） |
| v1.08 | 2026-10-31 | L2P 区域写回补 WAL 次序（journal 先落盘；2026-10-31 上板验证） |
| v1.09 | 2026-10-31 | debug 注入 seam + 失败侧 UT（2026-10-31 上板验证） |
| v1.10 | 2026-11-01 | FTL volatile write cache：早回执 + 读可见 + deferred error（未上板） |

## 2. 功能阶段

### Phase 0 — 初始实现与 DRAM-less 定型

**版本**：v0.1–v0.2

从「绕过 FC 的同步页映射」转为完整 DRAM-less 设计；确定纯 CW 页映射、NAND 驻留 L2P、CLOCK、write-back + journal、跨 plane SB、10% OP 等 D1–D12。

### Phase 1 — 模块实现与基础设施

**版本**：v0.18–v0.30

完成 FTL 全模块、Completion Table、返回值统一、拓扑结构、地址双层化与形参优化（`ftl_submit_*` 改 `(ftl, req)`）。

### Phase 2 — 多 LUN / 拓扑 / 地址体系

**版本**：v0.31–v0.37

多 LUN 并发前置、BST 统一 per-block、数据平面多维化（L2P 分片 + ch/ce/lun）、Controller 三维路由、`main.c` FTL 上线。

### Phase 3 — 掉电恢复 / 元数据 / Checkpoint

**版本**：v0.38–v0.50

写失败 SB 关闭、closed SB 重建、Checkpoint 增量、Journal 全量回放、Free Pool 三级、元数据写模型统一、Wear 持久化。

### Phase 4 — 算法档（GC / WL / L2P / Journal）

**版本**：v0.51–v0.60

GC victim 加权与触发补齐、L2P cache thrashing、Journal 寿命修复、BST 瘦身、CW 级映射、DRAM-less 有效性判定。

### Phase 5 — 介质布局 / 多实例 / 页打包

**版本**：v0.61–v0.74

介质布局版本、元数据 CRC 下沉、GC 触发对齐 DRAM-less、写→掉电恢复、多实例、OOB 出厂坏块、页打包 wbuf、地址 PBA 化、OP/Spare 按 plane。

### Phase 6 — 提交/完成解耦与后台调度

**版本**：v0.75–v0.82

提交与完成解耦（八阶段异步化）、时间预算后台调度器、allocator 账本收口、L2P 哨兵删除、元数据异步回收、固定元数据镜像、Wear 并入 Checkpoint。

### Phase 7 — 收口与上板稳定化

**版本**：v0.83–v1.10

USB 数据面修复、L2P 日志固定区、状态码非负、元数据分区、GC 收益判据、region 主备、静态 WL UBI、cache read/program、跨块续读、volatile write cache。

## 3. 版本条目

### v0.1 — 初始实现（Phase Unknown）

**日期**：2026-09-21 前

**内容**：
- 页级 L2P 映射（页面级 `ssd_ppa_t`，每 LBA 4096B）
- 顺序物理页分配（`next_free_ppa` 单调递增）
- 直接调用 `nand_read_page()` / `nand_program_page()` 绕过 FC
- Controller 路由：同步阻塞式 `ftl_read/write` → NAND

**问题**：
1. ❌ **绕过 FC 层**：没有 CRC/ECC/Random/RAID 数据保护
2. ❌ **地址编码不一致**：`ssd_ppa_t`（uint64_t 线性页号）与 `nand_paa_t`（uint32_t bitfield）不兼容
3. ❌ **LBA 不对齐**：LBA_SIZE=4096B vs NAND page=2048B，写 4KB 跨 2 页
4. ❌ **9 个桩模块**：allocator、GC、wear_leveling、BBT、metadata 等都是 `#warning` 占位
5. ❌ **同步阻塞模型**：未利用 FC 的异步非阻塞能力

**状态**：标记为需重写的初始实现。

---

### v0.2 — 完整 DRAM-less 设计

**日期**：2026-09-23

#### 讨论内容

基于 FC 层的完整设计文档 `../fc_nand_design.md` 和项目现有代码结构，以 FTL 层为蓝本进行完整设计。

#### 关键决策

| 序号 | 决策 | 选择 | 拒绝方案 | 理由 |
|------|------|------|---------|------|
| **D1** | 映射策略 | **纯 CW 级（512B）页映射** | 块映射、混合映射（BAST/FAST） | 写放大小、行业标准、FC 对齐 |
| **D2** | L2P 存储 | **NAND（DRAM-less）** | SDRAM 全表（8MB）、SRAM 全表 | 学习嵌入式/UFS 主流，有 1MB SRAM 可做 cache |
| **D3** | 替换策略 | **CLOCK** | LRU（链表复杂）、FIFO（命中率低） | 接近 LRU 命中率、无动态内存、O(1) |
| **D4** | 写回策略 | **Write-back + Journal** | Write-through（WA 高）、纯 Write-back（掉电风险） | 性能与掉电安全折中 |
| **D5** | 映射粒度 | **512B（1 CW）** | 2048B（页面级）、4096B | 与 FC CW USER 区完全对齐 |
| **D6** | FTL → NAND | **通过 FC 接口** | 直接 nand_* 调用（现状） | 完整数据保护 |
| **D7** | RAID | **不实现** | CONFIG_FC_RAID=1 开启 | 复杂度高，SLC 暂不需要 |
| **D8** | Super Block | **跨双 Plane Super Block** | 单 Block | 学习企业级跨 Die 条带化逻辑，当前双 Plane 做跨 Plane 配对 |
| **D9** | 温度分区 | **Hot / Cold 双 Super Block** | 单温区 | 提升 GC 效率，冷热数据分离 |
| **D10** | OP | **10% 预留（可配置 7~28%）** | 0% | 支持 RMA 坏块替换 + 性能恢复，消费级典型值 10% |
| **D11** | Free Pool | **三级结构**（Pre-conditioned / Normal / Emergency） | 单池 | 学习 Background GC 和前台 IO 隔离 |
| **D12** | SLC Cache | **不实现** | — | 本身就是 SLC NAND，无需模拟 SLC |

#### 地址编码决策

- FTL 使用 FC 的 `nand_paa_t`（uint32_t）格式
- 放弃原有 `ssd_ppa_t`（uint64_t 线性页号）
- PAA 布局：`[CW:2][PAGE:6][PLANE:1][BLOCK:11]` = 20 bit 有效
- `cmd->paa` 填 CW=0 基址，FC 内部自动生成 desc 链

#### 关于 RAID 的讨论

RAID 功能因实现复杂度过高，在当前 SLC 阶段不实现。具体不确定点：
- Stripe RAID 与 GC 的交互（parity block 如何参与 GC）
- 跨 Plane 错误的报告语义（数据命令成功、parity 失败时如何通知 FTL）
- parity block 的分配管理（allocator 能否分配 parity plane 的 block）

**结论**：RAID 延后，已有 ECC（BCH t=4）在 SLC 上可提供足够保护。

---

### v0.18 — FTL 全部模块实现 + 代码审视修复

**日期**：2026-09-29

#### 内容
FTL 层剩余 5 个模块全部实现，21 个源文件 0 个 `#warning` 核心桩。代码审视修复 4 个逻辑断裂。

#### 新增模块

| 模块 | 文件 | 内容 |
|------|------|------|
| **Checkpoint** | `checkpoint.h/c` | 全量 L2P 快照 + Journal 截断协同 + 三级触发（水位线/写入量/定时）+ 紧急 95% 强制 + Block 37-38 各 3 页冗余 |
| **BBT** | `bbt.h/c` | 出厂扫描（LDPC_RAW 读 Page 0）+ NAND 持久化（Block 36+39 各 3 页冗余，CRC 保护）+ 运行时标记 + Spare Pool 预留接口 |
| **Read Recovery** | `read_recovery.h/c` | Stage 1 SET_FEATURES Read Retry（8 次重试）+ Stage 2 邻近页辅助 + ECC 趋势预测（环形 buffer，连续上升告警）+ 4 级 Block 健康状态 |
| **Power Failure Recovery** | `power_failure_recovery.h/c` | 三类启动判断（fresh/clean/PF）+ 写指针 NAND 扫描 + ftl_boot_recovery 主入口 |
| **Wear Leveling** | `wear_leveling.h/c` | 动态（alloc 选低擦除）+ 静态三星 Daemon（三条件触发：差值 5000 / 比值 3x / 低区占比 5%）+ wl_relocate 搬移 + wl_execute 集成 ftl_service |

#### 代码审视修复（4 个逻辑断裂）

| # | 问题 | 影响 | 修复 |
|---|------|------|------|
| 1 | `l2p_cache_set_md_dir()` 从未调用 | `cache->md_dir` 永远 NULL，所有 Region 查询回退固定位置，Metadata Directory 完全无效 | `ftl_init()` 中加调用 |
| 2 | `bbt_init()` 从未调用 | 出厂坏块不加载，所有 4096 Block 视为好块 | `ftl_init()` 中加调用 |
| 3 | `ftl_internal_sync_wait()` 中 release "other" completion | 用户读的 pending 表项泄漏，8 次后所有读请求失败 | 删除 release 代码，注释说明单 LUN 无并发 |
| 4 | `ftl_boot_recovery()` 调 `journal_replay` 做判断 | 双重 replay 浪费 2× Journal 扫描时间 + 修改 journal 内部状态 | 改为直接读 header `page_seq` |

#### 编译状态
**零错误零警告**。FTL 层 21 个源文件全部通过。Binary size: 58368 bytes。

#### 剩余 #warning（非 FTL 层）
- Controller 层：`scheduler.c` `state_machine.c` — 主机请求调度/状态机（`request.c` 已在 v0.23/v0.36 实现，非桩）→ ✅ **v0.47 已落地**（最小状态机 + 调度钩子，见 v0.47 条目）
- NVMe 层：`nvme_admin.c` `nvme_io.c` `submission_queue.c` `completion_queue.c` — 当前用 UART CLI，5 个桩文件（含 nvme_controller.c 返回 NOT_SUPPORTED）→ ✅ **v0.47 已删除**（C2 砍掉，UART CLI 驱动 FTL，D10）
- Platform BSP：`timer.c` `uart.c` `cache.c` `dma.c` `pcie.c` `interrupt.c` — 已有 soc.h + DWT 实现

---

### v0.19 — Completion Table 统一完成路由 + L2P Region 动态分配

**日期**：2026-09-29

#### 背景

代码审视发现三个结构性隐患：
1. **两泵冲突**：`ftl_service` Phase 2 和 `ftl_internal_sync_wait` 各自调用 `fc_poll_completion` 消费 CQ entry。同步等待循环中的 `fc_poll_completion` 会"吞掉"用户 I/O 的完成事件——数据不拷贝到 caller buffer、pending 不释放、命令不 release。
2. **`ftl_write` 完成判定依赖全局 SQ/CQ 空**：在并发内部操作（GC、Region flush、Checkpoint）同时运行时可能提前返回或无限等待。
3. **L2P Region 首次写入回退固定位置**：`l2p_cache_flush_one` 中的 TODO 显示 Region 动态分配未接入，始终回退 Block 0~31。

#### 解决方案

新增 `Completion Table` 模块（`completion_table.h/c`）：

```
统一完成路由设计（企业级 Unified Completion Pump）：

每个 I/O 提交时可注册 completion handler：
  completion_table_add(cmd_id, handler, ctx);

fc_poll_completion() 消费一个 CQ entry 后：
  ① cmd_id → 查 completion_table → 找到 handler
  ② handler(cmd, ctx) — 可能触发 pending 回拷或设置 waiter->done
  ③ 无注册 → 走 default handler（ftl_handle_completion）

所有完成事件统一经过 completion_table_drain() 分发：
  ftl_service Phase 2:  completion_table_drain()
  ftl_internal_sync_wait:  completion_table_add → completion_table_drain()
  ftl_read/ftl_write 同步循环:  completion_table_drain()
```

#### 改动清单

| 文件 | 改动 | 说明 |
|------|------|------|
| `include/ftl/completion_table.h` | **新增** | 16 项路由表【v0.31 扩为 32】+ `ftl_sync_waiter_s` + 统一泵声明 |
| `src/ftl/completion_table.c` | **新增** | 路由表实现 + `ftl_sync_waiter_handler`（带/不带 release）|
| `src/ftl/ftl.c` | 重写 | `ftl_handle_completion` 改为 default handler 注册；Phase 2 改为 `completion_table_drain()`；同步循环统一泵 |
| `src/ftl/ftl_fc_interface.c` | 重写 | `ftl_internal_sync_wait` 改为 `completion_table_add` + `completion_table_drain()` 循环，不再独占 `fc_poll_completion` |
| `include/ftl/l2p_cache.h` | 修改 | 新增 `alloc` 指针；`l2p_cache_set_md_dir` 增加 `alloc` 参数 |
| `src/ftl/l2p_cache.c` | 修改 | `l2p_cache_flush_one` 中 TODO 消除：首次写 Region 调用 `metadata_dir_alloc_region` 动态分配 |
| `src/ftl/ftl.c` → `ftl_init()` | 修改 | 两次 `l2p_cache_set_md_dir`：md_dir 未就绪时设 NULL（首次），加载后重设（含 alloc）|

#### 三个隐患修复状态

| 隐患 | 状态 | 机制 |
|------|------|------|
| 同步等待吞 completion | ✅ **已修复** | `completion_table_drain` 自动路由非目标 completion 到 default handler |
| ftl_write 全局空判定 | ⚠️ **改善** | 同步循环已用统一泵，后续可改为 per-cmd sync_waiter |
| Region 回退固定位置 | ✅ **已修复** | 首次写通过 `metadata_dir_alloc_region` 从 Free Pool 分配 |

#### 多 LUN 并发前置条件

现在同步等待和用户 I/O 共用同一个泵，完成事件不被吞。
后续还需：
1. CQ 深度确认 — 确保 FC 层 CQ 能容纳多 LUN 完成事件
2. `FTL_PENDING_MAX` 扩展 — 从 8 增至 32 ✅ **（v0.31 已完成）**
3. `ftl_write` 同步等待改为 per-cmd sync_waiter

#### 编译状态

**零错误零警告**。新增 `completion_table.o` 398B text + 200B bss（32 项路由表，v0.31 扩）。FTL 层 22 个源文件。
Binary size: 58368 bytes。

---

### v0.20 — 返回值体系统一 + 历史死代码清理 + WL/GC 重复消除

**日期**：2026-09-29

#### 背景

FTL 层 ~30 个操作函数的返回值不统一：`int 0/-1`、`SSD_STATUS_OK/IO_ERROR`、`(int)fc_status_t` 混用，
调用者需要记住每个函数的成功/失败约定。同时项目积累了一批已退役的历史遗留文件。

#### 返回值统一方案（企业级风格）

参考存储主控行业做法，FTL 内部按职责分三类：

| 类别 | 返回值 | 函数数 | 说明 |
|------|--------|--------|------|
| **操作函数** | `ftl_status_t` | ~32 | `FTL_OK=0` / `FTL_ERROR=-1`，两值简洁枚举 |
| **谓词函数** | `int` | ~8 | 保持 `1=是` / `0=否`（check_trigger/needs_gc 等） |
| **计数函数** | `int` | ~3 | 保持非负=数量（gc_migrate_sb/wl_relocate 等） |
| **对外 API** | `ssd_status_t` | 3 | 保持 `SSD_STATUS_*`（ftl_read/write/trim） |

#### 新增类型

`include/ftl/ftl_type.h`：

```c
typedef enum {
    FTL_OK    = 0,   /* 成功 */
    FTL_ERROR = -1,  /* 通用失败 */
} ftl_status_t;
```

与 `ssd_status_t` 的边界：FTL 对外 API 依然返回 `ssd_status_t`，内部模块间调用返回 `ftl_status_t`。
在 `ftl_read/write` 包装中：`FTL_ERROR` → `SSD_STATUS_IO_ERROR`。

#### 改动清单

##### 头文件签名变更（13 个头文件）

| 模块 | 函数 | 旧 | 新 |
|------|------|----|----|
| `allocator.h` | `allocator_init` | `int` | `ftl_status_t` |
| `bbt.h` | `bbt_init/mark_bad/save/load/spare_pool_refill` | `int` | `ftl_status_t` |
| `checkpoint.h` | `checkpoint_create/find_latest` | `int` | `ftl_status_t` |
| `gc.h` | `gc_read_page_with_lba` | `int` | `ftl_status_t` |
| `journal.h` | `journal_replay` | `int` | `ftl_status_t` |
| `l2p_cache.h` | `l2p_cache_flush_one/flush_all` | `int` | `ftl_status_t` |
| `metadata.h` | `metadata_dir_flush_to_anchor/load` | `int` | `ftl_status_t` |
| `completion_table.h` | `completion_table_add` | `int` | `ftl_status_t` |
| `ftl_fc_interface.h` | 全部 7 个函数 | `int` | `ftl_status_t` |
| `power_failure_recovery.h` | 全部 4 个函数 | `int` | `ftl_status_t` |
| `wear_leveling.h` | `wl_init()` | 删除 | — |
| `ftl.h` | `ftl_init/submit_read/submit_write` | `int` | `ftl_status_t` |
| `ssd_types.h` | `ssd_ppa_t` | 删除 | — |

##### 审视修复（返回值切换中发现的 bug）

| # | 问题 | 文件 | 修法 |
|---|------|------|------|
| 1 | `checkpoint_find_latest` 改为 `ftl_status_t` 后调用处 `if (!cp_found)` 逻辑反转 | `power_failure_recovery.c` | `if (checkpoint_find_latest(&cp) != FTL_OK)` |
| 2 | `ftl_cmd_setup_ppa` 等 3 函数返回 `(int)fc_*()` 强转 | `ftl_fc_interface.c` | `(fc_*() == FC_STATUS_OK) ? FTL_OK : FTL_ERROR` |

##### 历史死代码删除

| 文件 | 理由 |
|------|------|
| `include/nand/nand.h` + `src/nand/nand.c` | 旧 NAND 后端抽象，0 调用，已被 FC 层取代 |
| `include/nand/nand_geometry.h` + `src/nand/nand_geometry.c` | 唯一用户是 nand.h（已死），几何参数已在 nand_config.h |
| `config/nand_geometry.h` | 设计文档已标废弃 |
| `config/ftl_config.h` | SSD_FTL_LBA_SIZE=4096 0 引用 |
| `include/ftl/ftl_type.h` → `typedef uint32_t pba_t` | 无人用，全用 nand_pba_t |
| `include/ftl/wear_leveling.h` → `wl_init()` | 5 行空实现 0 处调用 |
| `include/ftl/allocator.h` + `src/ftl/allocator.c` → `allocator_refill_pre_cond` | dead code（free_count 已在 allocator_free_block 中自同步）|
| `common/ssd_types.h` → `typedef uint64_t ssd_ppa_t` | 已被 nand_paa_t 替代 |
| `__patch_01.txt` + `__patch__.txt` | 根目录临时残留 |

##### WL/GC 重复代码消除

| 改动 | 效果 |
|------|------|
| `wl_relocate` 内联 FC 命令序列（~45 行）→ `gc_read_page_with_lba()` | `gc_read_page_with_lba` 从 `static` 升为 `gc.h` 公开接口 |
| `wl_relocate` 擦除循环（~15 行）→ `gc_erase_and_free()` | 构造临时 `super_block_s` 后复用 GC 擦除逻辑 |
| `gc.h` 的 `PRE_COND_LOW_WATER` → 删除（与 allocator.h 的 `FREE_POOL_TARGET` 重复）| 调用处改为 `FREE_POOL_TARGET` |

#### 编译状态

**零错误零 FTL 警告**。源文件数 105→103。Binary size: 58368 bytes（不变，死代码不链接）。

---

### v0.21 — FTL 拓扑信息重构（ftl_topology_t）

**日期**：2026-09-29

#### 背景

`ftl_context_t` 中散落 `ch/ce/lun` 三个字段，`ftl_init` 需要 8 个参数，
Controller 层被迫知道底层 NAND 拓扑。行业做法是 FTL 对外只暴露 LBA，
拓扑信息在初始化时一次性传入后由各子模块内部持有。

#### 改动

将分散在各结构体中的 `ch/ce/lun` 字段统一为 `ftl_topology_t`：

| 结构体 | 原字段 | 改为 |
|--------|--------|------|
| `ftl_context_t` | `ch / ce / lun` | `ftl_topology_t topo` |
| `ftl_allocator_t` | `ch / ce / lun`（曾被删除又恢复） | `ch / ce`（保留，从 topo 初始化） |
| `l2p_journal_t` | `ch / ce / lun` | `ftl_topology_t topo` |
| `metadata_dir_cache_t` | （无） | `ftl_topology_t topo` |

#### 新增类型

`ftl_type.h`：

```c
typedef struct {
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;
} ftl_topology_t;
```

#### 接口变化

```c
// 旧
ftl_init(ftl, ch, ce, lun, free_blocks, free_count, bad_blocks, erase_counts, closed_sb_list);

// 新
ftl_init(ftl, topo, free_blocks, free_count, bad_blocks, erase_counts, closed_sb_list);
```

所有元数据路径（Journal/Checkpoint/BBT/Anchor）使用 `topo.ch/ce` 构造 PBA。
用户数据路径（allocator_alloc）使用 `alloc->ch/ce` 构造 PBA。

#### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/ftl_type.h` | **新增** `ftl_topology_t` |
| `include/ftl/ftl.h` | `ftl_context_t` 增加 `topo` 字段；`ftl_init` 增加 `ftl_topology_t` 参数 |
| `include/ftl/allocator.h` | 恢复 `ch/ce` 字段；`allocator_init` 恢复 `ch/ce` 参数 |
| `include/ftl/journal.h` | `l2p_journal_t` 增加 `topo`；`journal_init` 增加 `ftl_topology_t` 参数 |
| `include/ftl/metadata.h` | `metadata_dir_cache_t` 增加 `topo` 字段 |
| `include/ftl/checkpoint.h` | `checkpoint_find_latest` 增加 `ftl_topology_t` 参数 |
| `include/ftl/bbt.h` | `bbt_load` 增加 `ftl_topology_t` 参数 |
| `src/ftl/ftl.c` | `ftl_init` 签名更新，子模块初始化传拓扑 |
| `src/ftl/journal.c` | 全部 `pba_make(0,0,...)` → `pba_make(topo.ch, topo.ce, ...)` |
| `src/ftl/allocator.c` | `allocator_init` 保存 `ch/ce`；`allocator_alloc` 用 `alloc->ch/ce` |
| `src/ftl/gc.c` | `pba_make(0,0,...)` → `pba_make(alloc->ch, alloc->ce, ...)` |
| `src/ftl/checkpoint.c` | `pba_make(0,0,...)` → `pba_make(topo.ch/ce)` 或 `ftl->topo.ch/ce` |
| `src/ftl/bbt.c` | `pba_make(0,0,...)` → `pba_make(alloc->ch/ce)` 或 `pba_make(topo.ch/ce)` |
| `src/ftl/metadata.c` | `pba_make(0,0,...)` → `pba_make(dir->topo.ch/ce)` |
| `src/ftl/wear_leveling.c` | `pba_make(0,0,...)` → `pba_make(ftl->topo.ch/ce)` |
| `src/ftl/power_failure_recovery.c` | `pba_make(0,0,...)` → `pba_make(ftl->topo.ch/ce)`；`checkpoint_find_latest` 传 `ftl->topo` |

#### 编译状态

**零错误零 FTL 警告**。Binary size: 58368 bytes。

### v0.23 — D4 多 LBA 拆分（Controller 层方案 A）

**日期**：2026-09-29

#### 背景

`block_count != 1` 的多 LBA 请求一直被 `count != 1U` 守卫拒绝。FTL 对外接口的 `count` 参数语义不清晰——它存在但从未真正被消费。

#### 决策：方案 A — Controller 层拆分

存储主控行业标准做法：Controller 层负责主机协议翻译（多 LBA → 单页子请求），FTL 层只处理单页原语。

#### 改动清单

##### FTL 接口净化

| 接口 | 旧签名 | 新签名 |
|------|--------|--------|
| `ftl_submit_read` | `(ftl, lba, buf, count)` | `(ftl, lba, buf, complete_flag, on_complete, ctx)` |
| `ftl_submit_write` | `(ftl, lba, data, count, complete_flag)` | `(ftl, lba, data, complete_flag, on_complete, ctx)` |

- 删除 `count` 参数（FTL 只支持单页）
- 新增 `pending_cb_fn_t on_complete` + `void *on_complete_ctx` 回调（pending 完成通知）
- 读请求新增 `complete_flag` 参数（此前读没有完成信号）

##### Pending 表扩展

`ftl_pending_req_t` 变化：
- 删除 `count` 字段（未使用）
- 新增 `on_complete` 回调指针 + `on_complete_ctx` 上下文
- `pending_add` 签名对应更新

##### Completion 路径

`ftl_handle_completion` 中新增回调调用：`complete_flag=1` 后 → `on_complete(ctx)`（pending 释放前）。

读请求 PBA 无效路径也发完成信号（memset 零后 → `complete_flag=1` → `on_complete(ctx)`），确保所有提交路径的完成语义一致。

##### Controller 拆分

`controller.c`：
- `block_count == 1`：直通 FTL，传 `complete_flag`
- `block_count > 1`：路由到 `ssd_request_split()`

`request.c`（核心拆分实现）：

```c
static void on_sub_complete(void *ctx) {
    ssd_req_s *req = (ssd_req_s *)ctx;
    if (req->sub_remaining > 0U) req->sub_remaining--;
    if (req->sub_remaining == 0U) req->complete_flag = 1U;
}
```

`ssd_req_s` 新增 `sub_remaining` 计数器。

#### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/ftl.h` | `ftl_submit_read/write` 签名：删 count + 加 complete_flag(读) + 加 on_complete/ctx |
| `include/ftl/pending.h` | 删 count + 加 on_complete/ctx；新增 `pending_cb_fn_t` 类型 |
| `src/ftl/pending.c` | `pending_add` 签名更新 |
| `src/ftl/ftl.c` | `ftl_submit_read/write` 实现对应调整；`ftl_handle_completion` 加 on_complete 调用；PBA 无效发完成信号 |
| `include/controller/ssd_request.h` | 新增 `sub_remaining` 字段 |
| `src/controller/controller.c` | block_count 拆分路由（1→直通，>1→ssd_request_split） |
| `src/controller/request.c` | 核心拆分实现 + on_sub_complete 回调聚合 |
| `src/controller/completion.c` | 注释更新 |

#### 编译状态

**零错误零警告**。Binary size: 58368 bytes（不变）。

---

### v0.24 — D5 写失败回滚（modify-after-confirm + BBT 两阶段 + 自动回滚）

**日期**：2026-09-29

#### 背景

`ftl_submit_write` 在 `fc_submit` 后立即执行 `journal_append` + `l2p_cache_write`，此时 NAND PROGRAM 尚未完成。如果 PROGRAM 失败：

1. **Journal 包含不可逆的映射** — 掉电后 replay 将 LBA 指向未写完成的新块 → 数据静默损坏
2. **L2P 指向垃圾数据** — 映射已更新但物理页无效
3. **坏块未标记** — 失败的 block 没有标记为坏，后续可能被重新分配

同时 `bbt_mark_bad` 在当前实现中同步写 NAND（`bbt_save` → 6 页 PROGRAM），如果在 completion handler 中调用会阻塞 completion 泵数毫秒。

#### 决策：modify-after-confirm + BBT 两阶段 + 自然回滚

##### 顺序修正

`journal_append` / `l2p_cache_write` / `checkpoint_journal_watermark` / `total_writes++` 从 `ftl_submit_write` 移入 `ftl_handle_completion` 的写成功确认路径，NAND PROGRAM 确认成功后执行（存储主控 industry standard）。

##### BBT 两阶段化

| 阶段 | 位置 | 操作 | NAND 访问 |
|------|------|------|-----------|
| 快速路径 | `ftl_handle_completion` | BBT bitmap 置位 + allocator 列表标记 | ❌ 无 |
| 后台持久化 | `ftl_service_idle/normal` → `bbt_flush()` | 攒批写 BBT payload 到 Block 36+39（6 页） | ✅ |

##### 回滚

写失败后 L2P 不动（自动指向 `old_pba`），无额外回滚逻辑。Super Block 空洞由 GC 扫描时读 NAND 失败自然跳过。

#### 改动清单

##### Pending 表扩展

`include/ftl/pending.h`：
- `ftl_pending_req_t` 新增 `old_pba` / `new_pba` / `ftl` 三个字段
- `pending_add` 签名新增 3 参数

`src/ftl/pending.c`：`pending_add` 实现对应更新

##### ftl_submit_write 顺序修正

`src/ftl/ftl.c` — `ftl_submit_write`：
- 删除 `journal_append()` / `l2p_cache_write()` / `checkpoint_journal_watermark()` / `total_writes++`
- `pending_add` 传 `old_pba`, `new_pba`, `ftl`
- 注释改为 `/* Journal + L2P 在 completion handler 中确认 NAND PROGRAM 成功后执行 */`

##### ftl_handle_completion 确认/回滚

`src/ftl/ftl.c` — `ftl_handle_completion` FC_COMMAND_MODULE_FTL 路径：

| 条件 | 动作 |
|------|------|
| NAND_SUCCESS + buffer != NULL（读成功） | `ftl_cmd_read_data`（不变） |
| NAND_SUCCESS + buffer == NULL（写成功） | `journal_append` + `l2p_cache_write` + `checkpoint_journal_watermark` + `total_writes++` |
| NAND_FAIL + buffer != NULL（读失败） | `memset` 零（不变） |
| NAND_FAIL + buffer == NULL（写失败） | `pba_to_block` → `bbt_mark_bad`（RAM only） |

##### BBT 两阶段化

`src/ftl/bbt.c` — `bbt_mark_bad`：删除 `bbt_save()` 调用
`src/ftl/bbt.c` — 新增 `bbt_flush()` 公开函数
`include/ftl/bbt.h` — 声明 `bbt_flush()`
`src/ftl/ftl.c` — `ftl_service_idle` + `ftl_service_normal` 中调用 `bbt_flush()`

#### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/pending.h` | pending 加 old_pba/new_pba/ftl 字段 + pending_add 签名扩展；恢复 FTL_PENDING_MAX 宏 |
| `src/ftl/pending.c` | pending_add 实现对应更新 |
| `src/ftl/ftl.c` — `ftl_submit_write` | 移出 journal/l2p/watermark/total_writes；pending_add 传 old_pba/new_pba/ftl |
| `src/ftl/ftl.c` — `ftl_handle_completion` | 写成功→确认 journal+l2p；写失败→坏块标记 |
| `src/ftl/ftl.c` — `ftl_submit_read` | pending_add 适配新签名 |
| `src/ftl/bbt.c` — `bbt_mark_bad` | 删除同步 bbt_save() |
| `src/ftl/bbt.c` | 新增 bbt_flush() |
| `include/ftl/bbt.h` | 声明 bbt_flush() |
| `src/ftl/ftl.c` — `ftl_service` | IDLE/NORMAL 模式加 bbt_flush() |

#### 编译状态

**零错误零警告**。Binary size: 58368 bytes（不变）。

---

### v0.25 — D6 队列深度/背压方案确认（pending 表满即隐式背压）
**日期**：2026-09-29

#### 背景

D4 多 LBA 拆分后，单次 host 请求（`block_count=N`）产生 N 个 FC 命令同时入 SQ。
SQ 深度 64、Command Pool 64、Pending 表 32 各层资源构成多层漏斗。需要确认背压策略。

#### 决策：方案① pending 表满即隐式背压

四种方案的原理、实现和行业使用情况在 `ftl_design.md §17` 中详细分析。

**选择理由（定量分析）**：

| 资源 | 深度 | 实际占用峰值 | 结论 |
|------|------|-------------|------|
| SQ | 64 | budget=4 + pending ≤32 + HSQ ≤6 ≈ 42 | << 64 ✅ |
| Command Pool | 64 | 同 SQ ≈ 42 | << 64 ✅ |
| Pending 表 | 32 | D4 拆分 block_count=8 时占用 8/32 ⚠️ | 并发上限（v0.31 扩） |

核心逻辑：
- SQ/池满几乎不会发生：实际并发 << 深度
- pending 表满时 `pending_add()` → `FTL_ERROR` → fc_submit 之前就拒绝
- SRAM 是 DRAM-less 最贵的资源，不设 retry queue（方案②）
- NVMe 实现后自然过渡到方案④（主机级流控）→ **v0.47 删 NVMe（学习主控 UART CLI 驱动）后不适用**；UART CLI 两段式天然免背压

**不采用**：
- 方案②（内部排队重试）：需额外 SRAM，与 DRAM-less 设计相悖
- 方案③（动态 budget）：当前资源深度足够，预算固定即可
- 方案④（主机级流控）：NVMe 层还没实现 → v0.47 后不适用（Host = UART CLI）

#### 波及的文件

| 文件 | 改动 |
|------|------|
| `docs/design/ftl_design.md` | 新增 §17 D6 详细分析（四种方案原理 + 对比表 + 定量推算 + 选择理由） |

**无代码改动**。

---

### v0.26 — D7 多命令流水线（Multi-Plane + Cache Program 安全中间页）

**日期**：2026-09-29

#### 背景

D4 的"FTL 只做单页原语"过于绝对——Multi-Plane 操作中 FTL 必须知道页数才能配置正确的 OP 码和 multi_plane 字段。同时 Super Block 设计（`plane_blocks[2]`）天生为双 Plane 配对，但 `allocator_alloc` 只返回单页 PBA。

#### 决策：修正 D4 + Multi-Plane + Cache Program 安全

##### 修正 D4：加回 pages 参数

```c
ftl_submit_read(ftl, lba, buf, pages, complete_flag, status_out, on_complete, ctx)
ftl_submit_write(ftl, lba, data, pages, complete_flag, status_out, on_complete, ctx)
```

| pages | 含义 | OP 码 | multi_plane | 数据量 |
|-------|------|-------|------------|--------|
| 1 | 单 Plane | `OP_NORMAL_*` | 0 | 2048 |
| 2 | 双 Plane Multi-Plane | `OP_*_MULTI_PLANE` | 2 | 4096 |

##### Multi-Plane

写总是 pages=2（Super Block 双 Plane 同步写入），读由 Controller 层两两连续 LBA 配对。

##### Cache Program 安全

`pending_req_t` 加 `cache_intermediate:1` 标志。中间页（15h）completion 时不更新 Journal/L2P，等最终页（10h）确认后一起更新。

#### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/ftl.h` | `ftl_submit_read/write` 签名加 `pages` 参数 |
| `include/ftl/pending.h` | 结构体加 `pages` + `cache_intermediate`；`pending_add` 签名加 2 参数 |
| `src/ftl/ftl.c` | `submit_read/write` pages 分支 + Multi-Plane OP 码；`handle_completion` cache 中间页跳过确认 |
| `src/ftl/pending.c` | `pending_add` 实现加 2 参数 |
| `src/controller/controller.c` | 单块直通传 pages=1 |
| `src/controller/request.c` | 配对拆分 pages=2，奇数 pages=1；`sub_remaining` 修正为 `ceil(N/2)` |

#### 编译状态

**零错误零警告**。Binary size: 58368 bytes（不变）。

---

### v0.27 — D8 错误通知粒度（status_out 消费级完成状态）

**日期**：2026-09-29

#### 背景

`complete_flag` 只能通知"完成"，不能区分"成功完成"和"失败完成"。消费级 DRAM-less 不需要企业级的 ECC 计数、failed descriptor ID、Error Log Entry——只需要知道"成功了还是失败了"。

#### 决策：status_out 指针

pending 表加 `volatile ssd_status_t *status_out` 指针（同 complete_flag 模式），completion handler 在置 `complete_flag` 的同时写入 `SSD_STATUS_OK` / `SSD_STATUS_IO_ERROR`。

```
ftl_handle_completion:
  NAND_SUCCESS + 读 → ftl_cmd_read_data → *status_out = SSD_STATUS_OK
  NAND_SUCCESS + 写(非cache中间页) → journal+l2p → *status_out = SSD_STATUS_OK
  NAND_FAIL + 读 → memset 零 → *status_out = SSD_STATUS_IO_ERROR
  NAND_FAIL + 写 → bbt_mark_bad → *status_out = SSD_STATUS_IO_ERROR
  cache_intermediate → 不设 status_out（由最终页决定）
```

#### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/pending.h` | pending 加 `status_out`；`pending_add` 签名加 1 参数；加 `#include "common/ssd_types.h"` |
| `src/ftl/pending.c` | `pending_add` 实现加 `status_out` |
| `include/ftl/ftl.h` | `ftl_submit_read/write` 签名加 `volatile ssd_status_t *status_out` |
| `src/ftl/ftl.c` — `ftl_handle_completion` | 各路径写 `*status_out = OK/IO_ERROR` |
| `src/ftl/ftl.c` — `ftl_submit_read/write` | `pending_add` 传 `status_out` |
| `src/controller/controller.c` | 单块传 `&request->completion_status` |
| `src/controller/request.c` | 拆分传 `&request->completion_status` |

#### 编译状态

**零错误零警告**。Binary size: 58368 bytes（不变）。

### v0.28 — 地址体系双层化 + 形参优化 Step 1/2（nand_addr_s + addr_to_cmd + 同步操作内联化）

**日期**：2026-09-30

#### 背景

- 形参数过多：`ftl_internal_read/write_page(7)`、`pending_add(12)`、`ftl_submit_read/write(8)`、`ftl_init(7)`
- 遍历路径无意义打包拆包：`pba_make` 打包 → `ftl_cmd_setup_ppa` 拆包（GC 搬移、元数据扫描、Checkpoint）
- `super_block_s` 不含拓扑，调用点从外部传 ch/ce

#### 决策：地址双层化

存储主控内部地址分两层：

| 层 | 类型 | 用途 | 布局 |
|----|------|------|------|
| **展开** | `nand_addr_s` | 遍历/迭代路径直填 FC 命令 | 独立字段（ch/ce/lun/block/plane/page/cw），无需 shift |
| **紧凑** | `nand_pba_t` (packed) | L2P 表存储接口 | 32-bit 位域（[CH][CE][PAA]） |

遍历路径不再打包→拆包，直接用展开地址：

```c
/* nand_config.h — 双向转换 */
nand_pba_t pba_from_addr(const nand_addr_s *addr);   // addr → PBA
nand_addr_s addr_from_pba(nand_pba_t pba);           // PBA → addr（随机路径）
nand_addr_s addr_from_paa(uint8_t ch, uint8_t ce, nand_paa_t paa);  // PAA+CH/CE → addr（元数据）

/* ftl_fc_interface.h — 地址直填 */
ftl_status_t addr_to_cmd(fc_cmd_s *cmd, const nand_addr_s *addr);
```

`super_block_s` 加 `ftl_topology_t topo` 字段，新增 `super_block_page_addr(sb, plane, page)`
（ch/ce/lun 取自 sb->topo）。旧 `super_block_page_pba(ch, ce, sb, ...)` 保留为兼容委托。

#### Step 1 内容（地址类型 + 结构体拓扑）

| 文件 | 改动 |
|------|------|
| `include/nand/nand_config.h` | `nand_addr_s` + `pba_from_addr` + `addr_from_pba` + `addr_from_paa`；修复头文件守卫破坏 |
| `include/ftl/allocator.h` | `super_block_s` 加 `topo`；新增 `super_block_page_addr`；旧 PBA helper 保留委托 |
| `include/ftl/ftl_fc_interface.h` | 新增 `addr_to_cmd` inline |
| `include/common/ftl_types.h` | 新建：`ftl_io_request_t` / `ftl_init_params_t`（Step 3/4 使用） |

#### Step 2 内容（删除 4 个同步包装，内联化）

`ftl_internal_read_page / write_page / erase_block / read_page_raw` 四个包装函数从
`ftl_fc_interface.c` 删除（-232 行），各模块内联为 `static` 同步助手
（`alloc → addr_to_cmd → submit → sync_wait → release`）：

| 模块 | 内联助手 | 地址来源 |
|------|---------|---------|
| `l2p_cache.c` | `sync_read_page` / `sync_write_page` | `addr_from_paa` / `addr_from_pba` |
| `journal.c` | `sync_read/write_page`、`sync_erase_block` | `journal_page_addr` / `journal_block_addr` |
| `checkpoint.c` | `sync_read/write_page` | `checkpoint_page_addr` |
| `metadata.c` | `sync_read/write_page` | `anchor_page_addr` |
| `bbt.c` | `sync_read/write_page`、`sync_read_page_raw` | `bbt_page_addr` |
| `gc.c` | `sync_write_page`、`sync_erase_block` | `addr_from_pba` / `super_block_page_addr` |
| `wear_leveling.c` | `sync_write_page` | `addr_from_pba` |
| `read_recovery.c` | `sync_read_page` | `addr_from_pba` / `addr_from_paa` |
| `power_failure_recovery.c` | `sync_read_page`、`sync_read_page_raw` | `journal_page_addr` |

仅保留 `ftl_internal_sync_wait`（completion_table 统一路由）。

#### 保留原则

- **随机 IO 路径**（`ftl.c` 从 L2P 拿 PBA 后 `ftl_cmd_setup_ppa`）**保留不动** —— PBA 已在手，无需展开
- `gc_read_page_with_lba`（GC 读页 + 提取 LBA）保留不动
- 元数据操作不走 Super Block，保持 `multi_plane=0`

#### 编译状态

**零错误零警告**。13 文件，+1379/-463（含 Step 1 未提交部分）。`ftl_fc_interface.c` 净 -227。

#### 下一步

- Step 3：`ftl_submit_read/write` 改 `(ftl, req)` 结构体传参；`pending_req_t` 嵌入 `io_request`
- Step 4：`ftl_init` / `allocator_init` 改指针传参（`ftl_init_params_t`）

### v0.29 — 形参优化 Step 3（ftl_submit_read/write → (ftl, req) + pending 嵌入 io）

**日期**：2026-09-30

#### 背景

`ftl_submit_read/write` 有 8 个形参（lba/buffer/pages/complete_flag/status_out/on_complete/on_complete_ctx），
`pending_add` 有 12 个形参，pending 表与 submit 签名重复描述同一请求。

#### 决策：ftl_io_request_t 结构体传参 + pending 按值嵌入

1. **`ftl_submit_read/write(ftl_context_t *ftl, const ftl_io_request_t *req)`** — 8 形参 → 2
2. **`ftl_pending_req_t` 嵌入 `ftl_io_request_t io`**（`g_pending[i].io = *req` 一次性拷贝 7 字段）
3. **`pending_add(cmd_id, io, old_pba, new_pba, ftl, cache_intermediate)`** — 12 形参 → 6
4. **位域 → 普通字段**：`used:1/command_id:15/pages:2/cache_intermediate:1` 展开（设计原则"非数组结构体不用位域"）

#### pending_cb_fn_t 迁移（打破循环 include）

- `pending.h` embed `ftl_io_request_t` 需 include `ftl_types.h`
- `ftl_types.h` 原本 include `pending.h` 取 `pending_cb_fn_t` → 循环
- **方案**：`pending_cb_fn_t` 迁至 `ftl_types.h`，`pending.h` 单向 include `ftl_types.h`
- 顺带修复遗留隐患：`ftl_init_params_t` 用 `super_block_s` 但未 include `allocator.h`（此前未编译进任何 .c）

#### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/common/ftl_types.h` | `pending_cb_fn_t` 迁入；补 `#include "ftl/allocator.h"` |
| `include/ftl/pending.h` | 单向 include ftl_types.h；`ftl_pending_req_t` 嵌 `io` + 位域展开；`pending_add` 6 形参 |
| `src/ftl/pending.c` | `pending_add` 实现 `g_pending[i].io = *io`（结构体拷贝）；`lookup_by_lba` 改 `io.lba` |
| `include/ftl/ftl.h` | 两个 submit 签名改 `(ftl, req)`；补 include ftl_types.h |
| `src/ftl/ftl.c` | submit 改读 `req->` 字段；`ftl_handle_completion` 改 `pending->io.` 前缀；old/new_pba/ftl/cache_intermediate 不变 |
| `include/controller/ssd_request.h` | 加 include ftl_types.h；结构体加 `ftl_io_request_t io` |
| `src/controller/controller.c` | 单块：填 `request->io`（lba/buffer/pages + 通知指针）后 `ftl_submit_*(ftl, &request->io)` |
| `src/controller/request.c` | 拆分：公共字段（on_complete/status_out）循环前设一次，循环内只改 lba/buffer/pages |

#### Controller 层调用形态

```c
/* 单块直通（controller.c） */
request->io.lba           = request->start_lba;
request->io.buffer        = request->buffer;
request->io.pages         = 1U;
request->io.complete_flag = &request->complete_flag;
request->io.status_out    = &request->completion_status;
ftl_submit_read(controller->ftl, &request->io);

/* 拆分（request.c）— 循环内复用 io 模板 */
request->io.complete_flag   = NULL;               /* 子请求用回调聚合 */
request->io.status_out      = &request->completion_status;
request->io.on_complete     = on_sub_complete;
request->io.on_complete_ctx = request;
for (i = 0; i < block_count; ) {
    request->io.lba    = start_lba + i;
    request->io.buffer = buf + i * FTL_READ_USER_SIZE;
    request->io.pages  = (i + 1 < block_count) ? 2U : 1U;
    ftl_submit_read(controller->ftl, &request->io);
    i += (request->io.pages == 2U) ? 2U : 1U;
}
```

> io 只需在 submit 调用期间有效（pending 表立即按值拷贝），循环内复用安全。

#### 语义不变项（回归防护）

- `ftl_handle_completion` 分支逻辑（读成功/写确认/读失败 memset/写失败标坏块/cache_intermediate 跳过）零改动，仅字段前缀
- `data_size = FTL_READ_USER_SIZE × req->pages` 与 0/1/2 校验不变
- PBA 无效快速路径（memset + 通知）行为不变
- `pending_lookup / lookup_by_lba / remove` 不变
- `ftl_init_params_t`（Step 4 用）不动

#### 编译状态

**零错误零警告**。23 文件，+1666/-697（含 Step 1/2 与文档）。`ftl.c` 净 -100、`pending.c` 净 -24、`request.c` 净 -48、`controller.c` 净 -26（均为形参聚合受益）。

#### 下一步

- Step 4：`ftl_init` / `allocator_init` 改指针传参（`ftl_init_params_t`）

### v0.30 — 形参优化 Step 4（ftl_init / allocator_init → ftl_init_params_t 指针传参）

**日期**：2026-09-30

#### 背景

`ftl_init` 7 个形参（topo/free_blocks/free_count/bad_blocks/erase_counts/closed_sb_list），
`allocator_init` 8 个形参（ch/ce + 同 6 个）。Step 1 已建 `ftl_init_params_t` 但未落地。

#### 决策：结构体指针传参

```c
ftl_status_t ftl_init(ftl_context_t *ftl, const ftl_init_params_t *params);          /* 7 → 2 */
ftl_status_t allocator_init(ftl_allocator_t *alloc, const ftl_init_params_t *params); /* 8 → 2 */
```

`ftl_init` 内部将 `params` 原样透传给 `allocator_init`（拓扑经 `ftl->topo` 保持一致）。

#### ftl_init_params_t 定义位置（打破循环 include）

- 初版放 `common/ftl_types.h`，但该结构体依赖 `super_block_s`（allocator.h 定义），
  而 allocator.h 需要 include ftl_types.h（拿 ftl_init_params_t）→ **循环 include 报错**
- **方案**：`ftl_init_params_t` 定义于 `include/ftl/allocator.h`（super_block_s 之后），
  `common/ftl_types.h` 收敛为 `pending_cb_fn_t` + `ftl_io_request_t`（无 allocator 依赖）

```
common/ftl_types.h  ── pending_cb_fn_t / ftl_io_request_t（无 allocator 依赖）
ftl/allocator.h     ── super_block_s → ftl_init_params_t（依赖闭合方向正确）
ftl.h               ── include common/ftl_types.h + ftl/allocator.h
```

#### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/common/ftl_types.h` | 删除 `ftl_init_params_t`（移往 allocator.h）；去掉 `#include "ftl/allocator.h"` |
| `include/ftl/allocator.h` | 新增 `ftl_init_params_t`（super_block_s 之后）；`allocator_init` 改 `(alloc, const params*)` |
| `src/ftl/allocator.c` | `allocator_init` 读 `params->topo/free_blocks/free_count/bad_blocks/erase_counts/closed_sb_list` |
| `include/ftl/ftl.h` | `ftl_init` 改 `(ftl, const params*)`；include 调整为 common/ftl_types.h + ftl/allocator.h |
| `src/ftl/ftl.c` | `ftl_init` 接收 params → `ftl->topo = params->topo` → `allocator_init(&ftl->alloc, params)` |

#### 启动示例（host 层）

```c
ftl_init_params_t params = {
    .topo           = {0, 0, 0},
    .free_blocks    = g_free_blocks,
    .free_count     = BLOCKS_PER_PLANE * PLANE_NUM,
    .bad_blocks     = g_bad_blocks,
    .erase_counts   = g_erase_counts,
    .closed_sb_list = g_closed_sb_list,
};
ftl_init(&ftl, &params);
```

#### 编译状态

**零错误零警告**。24 文件，+1848/-806（含 Step 1~3 与文档）。`allocator.c` 净 -14、`ftl.c` 净 -60（init 路径）。

#### 总结：形参优化 4 步全部完成

| 步骤 | 内容 | 形参变化 | 状态 |
|------|------|---------|------|
| Step 1 | nand_addr_s + addr_to_cmd + super_block_s topo + ftl_types.h | — | ✅ v0.28 |
| Step 2 | 删除 4 个 ftl_internal_* 包装，内联 sync 助手 | 7 → 0 | ✅ v0.28 |
| Step 3 | ftl_submit_read/write (ftl, req) + pending 嵌 io | 8 → 2 / 12 → 6 | ✅ v0.29 |
| Step 4 | ftl_init / allocator_init (params 指针) | 7→2 / 8→2 | ✅ v0.30 |

#### 下一步

- P-A：pending 表扩展 8 → 16/32（✅ v0.31 已做）
- P-B：scheduler / state_machine / NVMe 层（`#warning` 桩，6 文件）→ ✅ v0.47 落地/删除（最小状态机 + 调度钩子 + NVMe 删除，见 v0.47 条目）
- P-C.5：反向映射 bitmap（✅ v0.32 已做——`ftl_block_t.valid_map`）
- PFR closed_sb_list 掉电重建（v0.32 二期项）
- CP/GC 定时触发接线（`checkpoint_check_trigger(ftl, 0U)` / `gc_check_trigger(..., 500U)` 硬编码 elapsed；`cp_timestamp` 恒 0——需平台 `time_us` 钩子）

---

### v0.31 — P-A 多 LUN 并发前置 + read recovery 接入

**日期**：2026-09-30

**内容**：
- Pending 表 + Completion Table 扩容（8→32），多 LUN 并发软件前置
- 元数据 lun 泛化（bbt/checkpoint/metadata 地址构造器加 lun 形参）
- ECC 返回链路打通：`fc_ecc_decode` 加 `err_bits` out 参数
- read recovery 接入：`ftl_context` 加 `rr` + `rr_block_track`（**未分配，空转**）

**遗留**：rr_block_track 无存储（空转）→ v0.32 BST 供电解决。

---

### v0.32 — Block State Table（BST）统一 per-block 结构体数组

**日期**：2026-09-30

#### 背景

v0.31 read recovery 接入后，per-block 元数据分散四张表（erase_counts / bad_blocks /
g_bbt_bitmap / rr_block_track），无统一承载，且存 3 处全局/per-plane 块号碰撞 bug。

#### 关键决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 统一单数组 vs 分散表 | **A：统一 `ftl_block_t[4096]`** | 单一数据源，GC/WL/read recovery/BBT 共享；符合行业 BIT |
| 粒度 | 物理 Block 全局号 | 擦除/坏块/ECC 是物理级事实；SB 是逻辑层 |
| 存储位置 | 调用者经 `ftl_init_params_t.blocks` | 与 free_blocks/closed_sb_list 指针契约一致；多 LUN 每 LUN 一张 |
| VPC + bitmap | **全真值**（valid_count + valid_map） | P-C.5 落地；GC/WL 迁移扫位图免 NAND 读 |
| 全字段 | **56B/224KB 全常驻 SRAM** | 功能优先，剩 ~537KB；档三裁剪见 ftl_design §19 |
| state 表示 | **bitmask 宏 + 访问器**（非位域/联合体） | 嵌入式可移植 + 可位运算 |
| bad_reason | 独立 `uint8_t` | 可读性（用户拍板），6 种原因枚举 |

#### 实现

- 新建 `include/ftl/bst.h`：`ftl_block_t`（56B）+ 状态位 + 访问器 + 坏块原因枚举
- 修复 3 处全局/per-plane 块号碰撞：`ftl_gblock(plane, block)` 单点转换
- read recovery 由空转变为供电：ECC 历史入 `ftl_block_t.rr`
- VPC/bitmap 七点闭环：写成功（含覆盖清旧页）/ 读成功 / GC·WL 迁移 / 擦除 / trim / 坏块 / 掉电
- GC/WL 迁移改扫 `valid_map` 置位页（`__builtin_ctzll`），免无效页 NAND 读
- `bbt_mark_bad` 签名扩展带 bad_reason（3 处调用点）
- PFR open SB 扫描重建 bitmap + 置 OPEN
- **Closed 列表回收闭环**（审视补全）：新增 `allocator_remove_closed_sb`（swap-remove O(1)），
  GC 擦除后从 `closed_sb_list` 移除 victim——修 `closed_sb_count` 只增不减导致的列表爆炸 +
  已回收 SB 被再次选为 victim 的无限 GC 空转；`sb_id` 语义统一（OPEN 占位 NO_SB，关闭写 closed 索引，
  swap-remove 时同步修正）；`gc_erase_and_free` 加 `now` 参数传 `ftl->total_writes` 修 last_pe_ts
  非单调时间戳

#### 编译状态

**零错误零警告**（`make` + `make tests`）。16 文件，+520/-355。

#### 二期项

- **PFR closed_sb_list 掉电重建**：本期只做 open SB 状态机；closed 重建需扫全 NAND
  判有效页分组，涉及既有 PFR 缺口，独立立项
- **档三（SRAM 核心 + NAND 扩展）**：全常驻 224KB 的产品化裁剪路径，已写入 ftl_design §19，不实现

### v0.33 — FTL 内部边缘缺口闭环（write 路径紧急 GC + RR Stage 3 兜底）

**日期**：2026-10-01

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| Free Pool 耗尽 → write 路径 | **同步紧急 GC 兜底**（gc_select_victim + migrate + erase_and_free 1 个 SB 后重试一次） | 写请求在 Free Pool 枯竭瞬间不再丢弃；真无空间才返回 FTL_ERROR |
| RR Stage 3 | **显式坏块标记**（`bbt_mark_bad(FTL_BLOCK_BAD_READ)`） | 不做 XOR/RAID 恢复（FC raid 引擎孤儿代码）；页面损坏提升块级不可靠，预测搬移/GC 接手 |
| allocator.h #warning | 移除，改正式文档 | write 路径已闭环 |

#### 实现

- `ftl_submit_write` 第①步：`allocator_alloc` 失败 → 同步 GC → 重试（`src/ftl/ftl.c`）
- 读失败恢复全失败 → `bbt_mark_bad(READ)`（`src/ftl/ftl.c`）
- `read_recovery.c` Stage 3 注释改显式兜底语义；`allocator.h`/`read_recovery.h` 文档同步

#### 编译状态

零错误零新增警告（debug + tests）。commit `e727d29`。

### v0.34 — P-A 多 LUN 命令层机制启用（ISSUE/WAIT/FETCH + auto-split 活代码）

**日期**：2026-10-01

#### 背景

Phase V 已建好 ISSUE/WAIT/FETCH 三阶段骨架，但 `LUN_NUM=1/LUN_ADDR_WIDTH=0` 使机制全为死代码。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| LUN_NUM | 临时改 2（后续 v0.35 回滚至真实芯片 1） | 让门控 `(LUN_NUM>1U)` 变活、机制可验证 |
| 原子 OP pair_op | 3 个原子 OP 补 pair_op（READ→LOAD/PROGRAM→ISSUE/ERASE→ISSUE） | `nand_operation_is_splittable` 依赖 op_type==ATOMIC && pair_op!=0 |
| desc PAA lun | 修复 `paa_make(...,0U,...)` → 保留 `paa_to_lun(command->paa)` | 多 LUN desc META/随机种子/ECC 依赖 |
| 链式路径 | 补 auto-split（与单命令路径一致） | 单命令已有 fc.c，链式缺失 |
| sb->topo | allocator_alloc 分配时绑定真实 ch/ce/lun | 此前恒零，擦除/GC 地址全零 |

#### 编译状态

零错误零新增警告。commit `4e14c2a`。**注意：LUN_NUM=2 为临时验证值，v0.35 已回滚至 1。**

### v0.35 — 数据平面多维度化（L2P 分片 + ch/ce/lun 泛化 + 路由哈希）

**日期**：2026-10-01

#### 背景

v0.34 把 LUN_NUM 改成 2 与 MT29F4G08 真实结构（单 die，DIE_NUM=1）不符。用户决策：**配置回滚真实芯片（LUN_NUM=1），代码按多维度写，用 CH_NUM/CE_NUM/LUN_NUM 宏定大小**——换芯片只改宏。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| 配置 | LUN_NUM 2→1、LUN_ADDR_WIDTH 1→0 | MT29F4G08 单 die；P-A pair_op/auto-split 修复保留（换宏即活） |
| L2P 分片 | `L2P_REGIONS_PER_LUN = 2048/LUN_NUM`、`L2P_BLOCKS_PER_LUN`、`l2p_region_pba(ch,ce,lun,...)` | 行业标准：每 LUN 独立 region 表，总容量不变 |
| 路由 | `lba_route_ch/ce/lun` 哈希 + `lba_to_local` 交错 | 行业主控 LBA 哈希到三维；顺序写摊多 die |
| META 语义 | **存片内 local_lba** | GC/WL/journal replay/读恢复反查随 local 自洽（全链决策基准） |
| cache 拓扑 | `l2p_cache_s` 加 ch/ce/lun；`l2p_cache_init(cache,ch,ce,lun)` | 消固定位置寻址写死 0 |
| metadata_dir | 预填/alloc_region/anchor 按 `dir->topo.lun` 错开（ANCHOR_BLOCK_STRIDE + L2P_BLOCKS_PER_LUN） | 每 LUN 独立 anchor 区 |
| trim | 收全局 LBA → lba_to_local | controller 无维度知识，转换 ftl 内部完成 |

#### 实现

- `nand_config.h`：LUN_NUM/LUN_ADDR_WIDTH 回滚
- `l2p_cache.h/.c`：分片宏 + 路由 + local 转换 + cache 绑定 topo + 固定位置回退按 lun
- `metadata.h/.c`：anchor stride + 预填/alloc/load 按 topo.lun
- `ftl.c`：read/write/trim 全局→local；`cmd->meta=local`；l2p_cache_init 绑 topo
- `pfr.c`：3 处 l2p_cache_init 同步

#### 编译状态

零错误零新增警告（debug + tests）。commit `5f886e1`。LUN_NUM=1 折叠单 LUN，行为与现状一致。

#### 遗留（v0.36/v0.37 已闭环）

- **controller 三维 ftl 指针 + 路由**：✅ 已实现（v0.36，见下）
- **main.c FTL 上线**：✅ 已实现（v0.37 eeb2ace，见下）——host 大数组 + ftl_init 循环 + free pool 回填 + boot 恢复 + controller 接入，主循环切 ftl_service
- SRAM：每 LUN 352KB（BST 224KB + L2P 128KB）；v0.37 实测 AXI_RAM≈355.5KB + SRAM2≈24KB 不溢出；LUN_NUM=2 需跨区

### v0.36 — Controller FTL 实例三维路由接入（ftl[CH][CE][LUN] + 按 LBA 路由）

**日期**：2026-10-01

#### 背景

v0.35 数据平面多维化停在模块层（每 FTL 实例已绑 (ch,ce,lun)，但 controller 仍是单 `ftl_context_t *ftl` 指针，全仓无路由）。本版本把 controller 升级为三维指针数组 + 按 LBA 路由，把多维化推到系统层闭环。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| init 接口 | `ssd_controller_init(controller, ftl_context_t *ftl[CH_NUM][CE_NUM][LUN_NUM])` | 收三维数组，逐槽校验非空 + 槽位 topo 一致 |
| 路由函数 | `ssd_controller_route`（controller.c 内部函数，request.c 拆分循环共用，不暴露进公共头） | 按 `lba_route_ch/ce/lun`（region 粒度取模）选实例；后续换哈希策略只改此处 |
| Multi-Plane 配对 | `ssd_split_use_multi_plane`：连续 LBA 同实例才 pages=2，跨 region 边界切实例时降级单 Plane | FC Multi-Plane desc 从单个 `cmd->paa` 展开 plane0/1（fc.c），一次操作永远落在单个 (ch,ce,lun)；跨实例配 pages=2 会写错芯片导致静默数据丢失 |
| 配对判断基准 | 用「route 相等」而非「region 相等」 | route 相等才是同实例的充要条件；region 相等过严漏掉大量可配对场景；换哈希自动跟随 |
| 子请求计数 | `ssd_split_count` 预统计（替代硬公式 `ceil(block_count/2)`） | 跨实例降级使实际子请求数 > 公式值，欠计导致父请求 `complete_flag` 永不置位（host 悬挂）；且未映射读会同步完成，必须先计数后提交 |
| route helper 位置 | controller.c 内部函数 + request.c 前向声明 | controller.h 不依赖 l2p_cache.h（保持接口头纯净，业界做法） |

#### 实现

- `include/controller/controller.h`：`ssd_ctl_s.ftl` 单指针 → `ftl_context_t *ftl[CH_NUM][CE_NUM][LUN_NUM]`；init 签名收三维数组
- `src/controller/controller.c`：新增 `ssd_controller_route`（内部函数）；init 三重循环逐槽校验（非空 + topo 一致）；submit 单块分支 3 处 `controller->ftl` → 局部 `ftl`（route 后）
- `src/controller/request.c`：新增 `ssd_split_use_multi_plane` + `ssd_split_count`；拆分循环内每轮按 `io.lba` 路由 + 同实例配对判断；4 处 `controller->ftl` → 局部 `ftl`

#### 正确性要点

- 跨实例 Multi-Plane 必须降级：FC desc 从单 `cmd->paa` 展开，一次操作永远落在单 (ch,ce,lun)。跨实例配 pages=2 会把 512 数据写进 A 芯片 plane1，读路由到 B → B 的 L2P 查不到 → 静默读零（数据丢失）。
- 子请求计数必须预统计：跨实例降级使实际数 > `ceil(block_count/2)`，硬公式欠计 → host 悬挂；未映射读同步完成（ftl.c）须先计数后提交。
- LUN_NUM=1 折叠：`lba_route_*` 恒 0 → 配对恒真、计数 = ceil(block_count/2)，行为与现状完全一致（零回归）。

#### 编译状态

零告警（gcc -pedantic）+ 固件 stm32h743 debug/release/tests 通过。commit `86552ce`（已推送）。

#### 边界行为（用户确认）

拆分写失败保持 **partial write 语义**（已成功子请求不回滚，host 决定重试）——业界 NVMe 主流做法；批量回滚不在本次范围。

### v0.37 — main.c FTL 上线（✅ 已实现）

**日期**：2026-10-01

#### 背景

v0.36 把 controller 路由接入，但 **FTL 仍无生产调用入口**——`src/main.c` 只做 `fc_init` + `nand_init` + UART + watchdog，从不调 `ftl_init`/`ftl_service`。本版本把 main.c 接入 FTL 全链路，让多维化真正可运行、可上板。

#### 关键决策（已与用户确认）

| 项 | 决策 | 理由 |
|----|------|------|
| SRAM 布局（方案 B 修正） | BST `g_blocks[4096]`（224KB）**连续落 AXI_RAM**；`g_free_blocks` + `g_closed_sb_list` 落 **SRAM2**；l2p cache 随 ftl_context_t 在 AXI_RAM | SRAM1 仅 128KB（< BST 224KB），BST 不能落 SRAM1；拆两段会破坏 `params.blocks` 线性索引契约；AXI_RAM 空余 367KB，g_ftl(131.5)+g_blocks(224)=355.5KB 刚好放下 |
| BST 存储分层 | **档三分层记为后续独立任务，本次不动 BST 结构** | 大工程（读写路径全部改造 + NAND 布局 + 掉电一致性），塞进本次会爆炸 |
| free pool 回填 | 调用者（main.c）在 `ftl_init` 后遍历 BST 非坏块 `allocator_free_block` 逐个回填 | allocator_init 不填 free pool（free_count 硬清零）、bbt_scan_factory 只标坏块不产 free_blocks——**当前最大缺口，不回填则首次写失败** |
| boot 恢复 | `ftl_boot_recovery(&ftl, &result)` 由 main.c 调用（ftl_init 后、ftl_service 前） | ftl_init 内部不调用 boot；第一次上电 = BOOT_FRESH_START |
| 主循环 | `ftl_service(&g_ftl[0][0][0])` 替换裸 `fc_service()`（加 ftl_ready 守卫，失败降级 fc_service） | ftl_service Phase1 内部已含 fc_service（≤4 次） |
| 掉电重建 closed_sb_list | 本次不处理（v0.32 二期遗留） | power_failure 只重建 open SB；closed 列表为空时 GC 不回收已满 SB（空间泄漏但不丢数据） |
| controller 传参 | **显式 `g_ftl_ptrs` 三维指针表**（`ftl_context_t *[CH][CE][LUN]`） | C 数组退化规则：static 数组名退化为「指向 [CE][LUN] 子数组的指针」，与形参「指向指针的 [CE][LUN] 数组的指针」类型不匹配，GCC 10.3 起恒报 `-Wincompatible-pointer-types`（gcc/arm 双编译器复现）。指针表在 boot 后填 `&g_ftl[...]`，类型精确匹配 |

#### SRAM 核算（已验证，debug 实测）

| 对象 | 尺寸 | 落点 | 区域空余 |
|------|------|------|---------|
| `g_ftl`（含 l2p cache 128.5KB + journal/md ~3KB） | 131.5KB | AXI_RAM | 384-16.7=367KB |
| `g_blocks` BST | 224KB | AXI_RAM（连续） | 367-131.5-224=11.5KB 剩 |
| `g_free_blocks[4096]` uint16 | 8KB | SRAM2 | 128KB |
| `g_closed_sb_list[1024]` super_block_s | 16KB | SRAM2 | 128-24=104KB 剩 |
| 合计 | ~379.5KB | AXI 355.5 + SRAM2 24 | ✓ 不溢出 |

**链接实测**（`arm-none-eabi-nm` debug.elf）：`.sram2_data` = 20480B（= 8KB free + 16KB closed，0x30020000 起）；`.bss` = 383348B（AXI_RAM，g_ftl + g_blocks + 既有 16.7KB，末址 0x2405d9e8 < 0x24060000）✓ 不溢出。

#### 实现（src/main.c，唯一改动文件）

1. **include 区**新增：controller.h / ftl.h / allocator.h / bst.h / power_failure_recovery.h。
2. **声明区**：`g_ftl[CH][CE][LUN]`（AXI_RAM）、`g_blocks[CH][CE][LUN][FTL_BLOCK_TOTAL]`（AXI_RAM 连续）、`g_free_blocks[...]` + `g_closed_sb_list[...]`（`.sram2_data` NOLOAD）、`g_ctrl`、`g_ftl_ready`（主循环守卫）、`g_ftl_ptrs`（三维指针路由表）。
3. **`ssd_ftl_boot(ch,ce,lun)`**：构造 `ftl_init_params_t`（topo={ch,ce,lun}、blocks/free_blocks/closed_sb_list 指向多维子数组、free_count=0）→ `ftl_init`（内部含 bbt_init 标坏块）→ **遍历 BST 非坏块 `allocator_free_block` 回填 free pool** → `ftl_boot_recovery` → 日志 `[FTL] instance ... free blocks=N` + `[FTL] boot type=...`。
4. **`ssd_ftl_boot_all()`**：全实例三重循环 boot → 填 `g_ftl_ptrs` → `ssd_controller_init(&g_ctrl, g_ftl_ptrs)` → 成功置 `g_ftl_ready` + 日志 `[FTL] controller ready, lba_max=...`；任一失败降级（不置 ready，主循环退回裸 fc_service）。
5. **main()**：nand_init 成功分支末尾调 `ssd_ftl_boot_all()`；主循环 `if (g_ftl_ready) ftl_service(&g_ftl[0][0][0]); else fc_service();`。

#### 验证

- make debug / release / tests 均**零新警告**（tests 仅剩余既有 controller/host `#warning` 占位注释；release `-s` 剥离符号故 nm 汇总显示 0，debug 实测 `.sram2_data`=20KB、AXI .bss 末 0x2405d9e8 不溢出）
- 上板 UART 预期：`[FTL] instance ch=0 ce=0 lun=0 free blocks=N`（N≈4096−坏块）、`[FTL] boot type=0`（FRESH）、`[FTL] controller ready, lba_max=1048576`、主循环跑 ftl_service 不挂、首次写 allocator_alloc 成功

#### 待确认/遗留

- 掉电重建 closed_sb_list——v0.32 二期遗留

### v0.38 — F1/F3/F5 闭环（time_us 钩子 + predictive 搬移 + Multi-Plane 双页簿记）

**日期**：2026-10-02

#### 背景

FTL 层三个已知缺口在本版本闭环（对应「FTL 层未实现清单」F1/F3/F5）：
- **F1**：T3 定时触发从未生效（`checkpoint_check_trigger(ftl,0U)` 恒传 0、`gc_check_trigger(...,500U)` 硬编码、`cp.timestamp` 恒 0）
- **F3**：`rr_check_predictive` 无调用方 → CRITICAL/DYING 块不主动搬移
- **F5**：Multi-Plane 写（pages=2）只簿记 plane0，plane1 是"幽灵页"，且探索确认根因更深——**allocator 独立选异号块**导致 plane1 实际烧录到错误物理块

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| F1 时间源 | **平台抽象 `ssd_platform_ops_s.time_us`**（新增 `stm32_platform_time_us` = HAL_GetTick×1000，main.c boot 前注册） | FTL 核心不直接依赖 H743 平台实现（架构边界）；register 强制 time_us 非 NULL；us→ms `/1000` 换算防 10s 阈值变 10ms |
| F1 计时字段 | `ftl_now_ms()` helper（未注册返 0 → T3 恒不触发，行为等同接线前）；`cp_timer_start`/`gc_timer_start` 触发后回写 | 复用已有 trigger 签名（elapsed_ms 参数本就在），只填真实值 |
| F3 挂载 | **ftl_service_idle 巡检 + 1s 节流**（`RR_SCAN_INTERVAL_MS`），一次只搬一个 SB | 行业后台周期性巡检；read disturb 是慢变量，避免频繁全扫 4096 块 |
| F3 搬移粒度 | **整 Super Block**（`wl_relocate` 复用）+ 双 plane `rr_retire_block` 退役 | 行业 Predictive Failures：有效页整体搬走 + 慢死块退役（RESERVED，不再分配新数据），**不标坏**（BBT 不删、仍可读） |
| F3 归属 | `wl_predictive_relocate` **并入 wear_leveling.c**（删独立 predictive.c） | 搬移执行本来就归 WL，predictive 只是新触发源；RR 模块只留 scan/retire（健康判定） |
| F5 核心 | **allocator 同号双 plane**：`allocator_select_block` 按 block 号配对（双 plane 必须 FREE 且非坏，寿命取平均 EC），`allocator_alloc` 从 pool 移除两 gblock | 行业 super block = 每 plane 取同编号 block（NAND 行地址 plane 只差 BA6 位，block 必须同号）；修复 plane1 烧错块根因 |
| F5 簿记 | completion 对 plane1 页对称簿记（set_valid/program_count/L2P `io.lba+1`/Journal/覆盖清旧页）；pending 加 `old_pba1` | plane1 不再无映射幽灵页 → GC/WL 搬移不丢数据，运行/掉电恢复路径一致 |
| F5 语义 | pages=2 = `io.lba` 与 `io.lba+1` 两个连续 LBA 分别映射 plane0/plane1（**与奇偶无关**） | 放宽旧"必须偶数"假设；controller 拆分已产生连续 LBA 对，无需改 |

#### 实现

1. **F1**：`src/platform/stm32h743/platform_ops.c/h`（新增）+ `src/main.c`（注册）+ `src/ftl/ftl.c`（`ftl_now_ms` + 三处 trigger elapsed + 回写）+ `src/ftl/checkpoint.c`（`cp.timestamp` 填真值）。
2. **F3**：`src/ftl/read_recovery.c/h`（`rr_predictive_scan` 只扫 Closed health≥CRITICAL + `rr_retire_block` 置 RESERVED/健康复位）+ `src/ftl/wear_leveling.c/h`（`wl_predictive_relocate`）+ `src/ftl/ftl.c`（idle 巡检 1s 节流）。
3. **F5**：`src/ftl/allocator.c`（`allocator_pool_remove` 新增 + `allocator_select_block` 同号配对 + `allocator_alloc` 双 gblock 移除 + `plane_blocks[0]=plane_blocks[1]`）+ `src/ftl/pending.c/h`（`old_pba1`）+ `src/ftl/ftl.c`（submit 读 `old_pba1` + completion 双页簿记）+ `include/ftl/ftl.h`（pages=2 语义注释）。

#### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c 死代码警告 + controller/host `#warning` 桩）
- F5 关键正确性：allocator 同号配对（坏块单 plane 整对跳过）、completion 双页簿记对称性、读路径无需改（同号后 desc 派生自动正确）
- 上板验证（F1/F8/F9 前置，待 FTL 完工统一做）：T3 定时 checkpoint 10s 触发、GC 定时 500ms、pages=2 写读回一致、GC 搬移后 plane1 数据存活、predictive 巡检触发

### v0.39 — F7 写失败 Super Block 关闭（行业坏块替换流程）

**日期**：2026-10-02

#### 背景

F7（ftl_revision_history F 清单）：写失败路径原只 `bbt_mark_bad` 标坏块，但 **Super Block 仍保持 Open 继续写**——坏块还挂在 OPEN SB 里，与 allocator 状态矛盾（bbt_mark_bad 清 valid + 摘块，而 SB next_page 还推着写）。设计文档 §3.8「运行时坏块替换流程」已定义行业做法但未实现。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| 写失败处理 | **① 关闭当前 Open SB（不再写入）→ 进 closed_sb_list → GC 优先回收（VPC 低）；② bbt_mark_bad 标 block 号；③ L2P 不动自动回滚 old_pba** | 行业流程 §3.8：坏 SB 立即退役不再写，GC 回收时好 plane 擦除回 free pool、坏 plane 擦失败再标坏 |
| 标坏粒度 | 保留 `bbt_mark_bad`（标 block 号） | 同号双 plane 下两 plane 一起退役（同号语义固有粒度：一个 block 号横跨两 plane，物理上常同坏） |
| 空洞页 | 不置 valid（现状保留），GC 搬移跳过 | L2P 已回滚，读不会读空洞页 |

#### 实现

1. **`include/ftl/allocator.h` + `src/ftl/allocator.c`**：新增 `allocator_close_open_sb(alloc, temperature)` —— 运行时关闭指定温度的 Open SB（复用内部 `allocator_close_sb`：置 CLOSED + 进 closed_sb_list + valid=0）。
2. **`src/ftl/ftl.c`**：写失败分支（completion handler）—— 调 `allocator_close_open_sb(FTL_TEMP_HOT)` 关闭坏 SB，然后 `bbt_mark_bad` 标坏，L2P 不动自动回滚。

#### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c + controller/host `#warning` 桩）
- 逻辑闭环：写失败 → 坏 SB 关闭进 closed → GC 选 victim（VPC 低）→ gc_erase_and_free 擦双 plane → 好 plane 回 free pool、坏 plane 擦失败标坏
- 上板验证（待 FTL 完工统一做）：注入 PROGRAM 失败 → 观察 SB 关闭 + GC 回收 + 好 plane 复用

- pending/completion 全局单例多实例化——LUN_NUM>1 启用时必修
- controller 传参的 `g_ftl_ptrs` 显式指针表（避免 -Wincompatible-pointer-types）；若后续 controller 头改收数组退化形式（`ftl_context_t (*ftl)[CE][LUN]`）可去掉
- write 路径 pending 完成依赖 completion_table_drain 泵（非同步释放）——pending 槽位只在完成回调释放，submit 失败路径已配对 remove；无显式泄漏（UART 层尚未接 pending 通知，上板前关注）

### v0.40 — F2 掉电重建 closed_sb_list（全 NAND 扫描）

**日期**：2026-10-02

#### 背景

F2（ftl_revision_history F 清单）：`closed_sb_list` 是纯 RAM 态（SRAM2 NOLOAD），掉电即失；`allocator_init` 只清 `closed_sb_count`，不从 NAND 重建。掉电恢复 `ftl_boot_power_failure` 只扫 2 个 Open SB 写指针（且被 `hot_sb.valid!=0` 门控恒假 → 实际没扫），closed 列表完全没碰；BST 里已满块 state 停在 `allocator_init` 统一置的 FREE。结果：掉电重启后已满 SB 全部消失 → GC/WL/rr_predictive_scan 看不到 → 容量泄漏（不丢数据但衰减）；且这些块还被 free pool 回填，可能复用未擦除半满块。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| 重建时机 | **开机 recovery 阶段同步全扫**（Clean/Power Failure 两分支都重建） | fresh 全 NAND 未写白扫；clean 掉电前 closed 块同样在 free pool，同一根因；同步扫逻辑集中（用户确认） |
| 判定写过 | `sync_read_page_raw`（LDPC_RAW）+ 全 0xFF 检测（逐 plane） | FC 层无独立 OOB 接口，全库统一判据 |
| 判定有效 | `gc_read_page_with_lba` 取 LBA（META CW0）→ `l2p_cache_read` 反查 PBA==本页才置 valid | 覆盖/torn 页不置位 → GC victim 判据不虚高，迁移不搬幽灵页 |
| 部分写块 | 前 2 对恢复 Open SB（Hot/Cold），多余强制 Closed（GC 迁移自动清 torn） | 替代原被 valid!=0 门控恒假的 scan_write_pointer；torn 页靠 GC L2P 反查丢弃（不丢已确认写） |
| 固定元数据块排除 | `ftl_is_metadata_block` 统一谓词（固定区间 + md_dir 动态 region），rebuild 与 free pool 回填共用 | Journal/L2P 可写满 64 页，逐页判空会误判为满 SB；main.c 回填误纳元数据块问题同链修复（用户确认） |
| erase_count | F2 范围外不修（无持久化表） | WL 开机初期 max_ec-min_ec=0 退化为全平局，GC VPC 判据不受影响，写进文档 |

#### 实现

1. **`include/ftl/metadata.h` + `src/ftl/metadata.c`**：新增 `ftl_is_metadata_block(md_dir, block)` —— 固定块区间（L2P 0..L2P_BLOCKS_PER_LUN-1 / Anchor 33/34+stride / BBT 36/39 / Checkpoint 37/38 / Journal 40/41）+ md_dir 动态 L2P region 块（扫 entries）。
2. **`include/ftl/allocator.h` + `src/ftl/allocator.c`**：新增 `allocator_add_closed_sb(alloc, plane_blocks, temperature)`（仿 close_sb 后半段：置 CLOSED/清 FREE/温度/sb_id + 追加列表，valid=0）与 `allocator_remove_from_free(alloc, gblock)`（对 static allocator_pool_remove 的公共薄包装）。
3. **`src/ftl/power_failure_recovery.c`**：新增 `ftl_rebuild_closed_sb_list(ftl, stats)` + 3 个 static helper（`page_is_blank` / `scan_block_write_pointer` / `recover_open_sb` / `close_full_block`）；`ftl_boot_clean_start` 与 `ftl_boot_power_failure` 各加调用；顺带修 `ftl_boot_scan_write_pointer` 遗留的 buf[16] 传给 2048B 拷贝的栈溢出（新代码统一 `uint8_t raw_buf[PAGE_SIZE]`）。
4. **`src/main.c`**：free pool 回填加 `ftl_is_metadata_block` 谓词排除（时序安全：回填在前、boot_recovery 重建在后，中间无 allocator_alloc）。
5. **`docs/design/revision_history/ftl_revision_history.md`**：v0.40 条目 + F 清单 F2 标"✅ v0.40 已实现"。

#### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c 死代码 + controller/host `#warning` 桩）
- 上板验证（待 FTL 完工统一做）：写若干 SB（满/部分/覆盖留空洞）→ 断电重启 → 断言 closed_sb_count/valid_count、`gc_select_victim` 可选中满 SB、`gc_migrate_sb` 后 L2P 命中页全迁移、满块经 `gc_erase_and_free` 后 free pool 归还

- **已知代价**：Clean Start 重建的 L2P 反查触发按需 region 加载（cache 空，从 NAND 读 L2P 表），开机比 Power Failure 分支慢；另 erase_count 无持久化表，开机后 WL 短暂退化为全平局。

### v0.41 — F4 checkpoint 增量（region_bitmap）

**日期**：2026-10-02

#### 背景

F4（ftl_revision_history F 清单）：checkpoint 目前全量写 6 页 header（`region_bitmap` 恒全 1、`region_count` 恒全量），恢复侧完全不消费 bitmap。目标是把 checkpoint 改造成行业标准语义：**按 region 脏标记填 bitmap + 恢复侧按 bitmap 局部加载**。

关键事实（设计确认）：checkpoint 的「6 页」是 header 冗余（写 2048B header 到 Block 37/38 各 3 页），**不是 L2P 全表**；单 die 拓扑（LUN_NUM=1）下增量**没有 NAND 页数收益**（脏区数据本来就都要写，每区最多一页；evict 兜底保证任何脏区最终落 NAND）。用户确认**仍完整实现**——这是行业标准格式语义 + 未来多 LUN 的前置（多 die 时 checkpoint 不再覆盖全部芯片，bitmap 才有真正的增量收益）。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| 脏区追踪 | `l2p_cache_s` 加 `region_dirty_bitmap[L2P_REGIONS_PER_LUN/32]`（256B SRAM），`l2p_cache_write` 置位、`l2p_cache_flush_one` 成功清零 | 与 line 级 state 互补：evict 兜底 flush 后行 CLEAN/失效，但"本次周期内被改过"由 bitmap 记住；bit=1 ⟺ 该区 NAND 页可能有未落盘数据 |
| 快照先于 flush | `checkpoint_create` ①先 `memcpy` 快照 bitmap → ②按 bitmap 只 flush 脏区 → ③填 header → ④写 6 页 → ⑤anchor → ⑥truncate | flush 内 `flush_one` 会清 bit，先 flush 后快照则 bitmap 恒空、header 声称无区当前（功能为空）；truncate 必须最后（其擦对方块依赖 ≤cp_seq 已落表） |
| flush 失败中止 | `checkpoint_flush_dirty_regions` 任一失败 → `checkpoint_create` 立即 `return FTL_ERROR`，不写 header/不 flush anchor/不截断 journal | 失败区 NAND 页仍旧、bit 保持置位；若继续写 header 声称已落盘 → boot 按 bitmap 预载旧页 = 数据一致性破坏。不再忽略 flush 返回值（原 182 行 `l2p_cache_flush_all` 返回值被 `(void)` 吞掉，改后必须消费） |
| evict 失败保留行 | `l2p_cache_evict_one`：DIRTY 行 flush 失败 → **不复用该行**（视同 FLUSHING 跳过继续找；找不到返回 FTL_ERROR） | 原实现 flush 失败仍置 INVALID 复用 → RAM 脏数据丢弃 + NAND 页仍旧 + **bit 置位但无 resident DIRTY 行（孤儿 bit）** → checkpoint 按 bitmap 假声明 → boot 必坏读。F4 前置修复 |
| 恢复按 bitmap 局部加载 | `ftl_boot_power_failure` 签名追加 `const l2p_checkpoint_header_t *cp`；`journal_replay` 后新增 `ftl_boot_apply_region_bitmap`：DIRTY（回放）行跳过；bitmap 区显式读页置 CLEAN；非 bitmap 区置 INVALID（懒加载 CP 时刻快照） | 回放行若 CLEAN 声明"cache==NAND"是假（NAND 是 CP 时刻旧值）、INVALID 则直接丢回放数据；INVALID 行直接置 CLEAN 的 entries 仍是 init 全 0xFF，read 对有效映射返回假 INVALID。强制顺序必须在 replay 之后（init 后所有行 region_id=0，先强制误匹配全部 64 行）。`ftl_boot_clean_start` 不变（天然安全：全 INVALID + 懒加载） |
| 诚实标注 | 单 die 下选择性 flush 与 `flush_all` **覆盖完全相同**（都是所有 resident DIRTY 行）；`ftl_service_idle` 每 tick 的 `l2p_cache_flush_all`（ftl.c）使 IDLE 模式 CP 触发时 bitmap 近空 | **本改造不减少写页数**；真实收益只在多 die/multi-LUN 时 CP 不再覆盖全部芯片才出现。格式/语义前移是主要目的 |

#### 实现

1. **`include/ftl/l2p_cache.h` + `src/ftl/l2p_cache.c`**：`l2p_cache_s` 加 `region_dirty_bitmap[L2P_REGIONS_PER_LUN/32]`；`l2p_cache_init` memset 0；`l2p_cache_write` 置位；`l2p_cache_flush_one` 成功路径清位（失败保持）；`l2p_cache_evict_one` 失败保留行（见决策）。
2. **`src/ftl/checkpoint.c`**：新增 static `checkpoint_flush_dirty_regions`（只 flush state==DIRTY && bit 置位的行）；`checkpoint_create` 改「快照 bitmap → 选择性 flush（失败中止）→ header（region_count=置位 word 数，无消费方仅信息量）→ 6 页 → anchor → truncate」；`region_bitmap` 从 cache 快照填入。
3. **`src/ftl/power_failure_recovery.c`**：`ftl_boot_power_failure` 签名 `(ftl, cp, result)`（调用点 236 行同步改）；新增 static `ftl_boot_apply_region_bitmap`（跳过 DIRTY 回放行 / bitmap 区显式读页 CLEAN / 非 bitmap 区 INVALID）；`cp_sequence` 改取 `cp->journal_sequence`。
4. **`include/ftl/power_failure_recovery.h`**：签名 + include checkpoint.h。
5. **测试**：`tests/unit/test_f4_region_bitmap.c`（5 例，纯 RAM + FC 桩：bit 生命周期、evict 失败保留行、CP 中止）+ `tests/framework/munit_stub_fc.c/.h`（PC 本机 FC 桩 + `fc_stub_fail_next` 失败注入）+ `tests/unit/f4_stub_fail.c`（上板同名符号）+ `tests/Makefile` PC 本机 target + `src/fc/makefile.mk`/`ut_main.c` 上板注册。（v0.44 起：PC 桩/本机 target 与 F4 测试一并移除，测试统一走 `make tests` 上板。）
6. **`docs/design/revision_history/ftl_revision_history.md`**：v0.41 条目 + F 清单 F4 标"✅ v0.41 已实现"。

#### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c 死代码 + controller/host `#warning` 桩）
- PC 本机纯 RAM 验证 `make -C tests f4`：5/5 全过（v0.44 起移除）
- 上板验证（待 FTL 完工统一做）：连续写 → checkpoint → 断电重启 → 断言恢复读正确（bitmap 区预载 CLEAN 与懒加载无差别）、flush 失败注入路径（临时改 fc 桩）CP 中止不丢数据

### v0.42 — F6 pending/completion 多实例化（per-instance pending + 全局 completion 路由）

**日期**：2026-10-02

#### 背景

F6（ftl_revision_history F 清单）：`g_pending[32]`（src/ftl/pending.c）与 `g_table[32]`（src/ftl/completion_table.c）是全局单例，`ftl_init` 每次实例化都 `completion_table_init()` + `pending_init()` 把整表清零——**LUN_NUM>1 时第二个实例的 init 会把第一个实例的 pending 全部冲掉**。P-A 多 LUN 并发（LUN_NUM=2 已启用）的软件前置。

关键架构事实（v0.42 设计确认）：
- FC 层全局单一：`fc_service()` / `fc_submit()` / `fc_poll_completion()` / `fc_command_id_to_command()` 都操作全局 `g_fc_controller`；command_id 全局唯一（FC command pool 槽号）。
- **单线程顺序主循环下"实例 A 的 drain 遇到实例 B 的 completion"是必然事件**（A#cmd1 提交后 B#cmd2 异步在途，下一轮 A drain 时两者都在 CQ）。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| pending 表 | **per-instance**：`g_pending[32]` 移入 `ftl_context_t`（`ftl->pending[32]`），API 改 `(ftl, ...)` 指针风格（仿 l2p_cache_s） | 实例独立 32 槽背压；`ftl_pending_req_t.ftl` 字段删除（实例即上下文）；与既有 cache/alloc/journal 指针模式一致 |
| completion 表 | **保持全局**（不迁入 ftl_context_t） | FC completion 全局单一 CQ + cmd_id 全局唯一；per-instance 表会让 B 实例的 completion 走 A 的 default handler → `pending_lookup(A, cmd2)=NULL` → 读不回填 buffer、写不确认 Journal/L2P → **数据丢失**。路由必须依赖 completion 自身内容（cmd_id → command → ch/ce/lun → 实例）而非"谁在 drain" |
| 实例路由 | 新增 `g_route[CH_NUM][CE_NUM][LUN_NUM]`（ftl_context_t* 表）：default handler 里 `cmd_id → fc_command_id_to_command → cmd->config.ch/ce/lun → g_route → 实例 → pending_lookup(ftl, cmd_id)`；`completion_table_set_route`（main.c boot 后调用一次）+ `completion_table_route`（越界 NULL） | 两段无缝衔接：全局表 cmd_id 精确命中（sync waiter）→ 未命中走全局 default → route 到正确实例的 pending |
| `ftl_internal_sync_wait` | **签名不变，21 处调用点零改动** | waiter 注册在全局表；等待期间他实例的 completion 由全局 default handler 正确分发到各自实例，不互吞 |
| `g_rr_last_scan_ms` | 移入 `ftl_context_t`（`ftl->rr_last_scan_ms`） | 与 F6 同源（全局单例），一并收干净；慢死巡检节流变 per-instance |
| main.c | 一处：`ssd_ftl_boot_all` 填完 `g_ftl_ptrs` 后调 `completion_table_set_route(g_ftl_ptrs)` | `g_ftl[][][]` 已是实例数组；route 表全局，boot 后一次设置覆盖全部 |

**调用约定（正确性前提）**：提交实例 = 命令 ch/ce/lun 来源实例 = route 解析实例。由 ftl_submit_read/write 用本实例 topo 构造命令（cmd->config.ch/ce/lun 来自 allocator 分配/本实例 topo）保证。

#### 实现

1. **`include/ftl/pending.h` + `src/ftl/pending.c`**：全部 API 加 `ftl_context_t *ftl` 首参（`pending_init/add/lookup/lookup_by_lba/remove`）；删全局 `g_pending`，操作 `ftl->pending`；`ftl_pending_req_t` 删 `ftl` 字段、`pending_add` 删尾参。
2. **`include/ftl/completion_table.h` + `src/ftl/completion_table.c`**：`g_table/g_default_handler/g_default_ctx` 保持全局；新增 `completion_table_set_route(ftl_context_t *route[][CE_NUM][LUN_NUM])` + `completion_table_route(ch, ce, lun)`；`completion_table_init` 一并清 route。
3. **`include/ftl/ftl.h`**：`ftl_context_t` 加 `pending[FTL_PENDING_MAX]` + `rr_last_scan_ms`（不 include completion_table.h，无环）。
4. **`src/ftl/ftl.c`**：删全局 `g_rr_last_scan_ms`（22 行）；`ftl_handle_completion` FTL case 内 `completion_table_route(cmd->config.ch/ce/lun)`（位域显式 (uint8_t) 转换），NULL → release+break；全部 `pending->ftl->X` → `ftl->X`；`ftl_init` 改 `pending_init(ftl)` + `ftl->rr_last_scan_ms=0` + set_default 注释注明 ctx 仅 fallback；submit_read/write 的 `pending_add(ftl,...)/pending_remove(ftl,...)`。
5. **`src/main.c`**：include completion_table.h；`ssd_ftl_boot_all` 填完 `g_ftl_ptrs` 后 `completion_table_set_route(g_ftl_ptrs)`（boot 后、controller init 前）。
6. **测试**：`tests/unit/test_f6_pending.c`（4 例纯 RAM：实例隔离 / init 只清本实例 / 满 32 背压互不挤占 / route 越界 NULL）+ PC 本机 target `make -C tests f6`（链 pending.c + completion_table.c + munit + munit_stub_fc，桩补 `fc_poll_completion`=QUEUE_EMPTY + `fc_command_id_to_command`=NULL）+ 上板 `src/fc/makefile.mk`/`ut_main.c` 注册 `/f6/` 套件。（v0.44 起：PC 桩/本机 target 与 F6 测试一并移除，测试统一走 `make tests` 上板。）
7. **`docs/design/revision_history/ftl_revision_history.md`**：v0.42 条目 + F 清单 F6 标"✅ v0.42 已实现"。

#### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c 死代码 + controller/host `#warning` 桩）
- PC 本机纯 RAM 验证 `make -C tests f4`（5/5）+ `f6`（4/4）全过（v0.44 起移除）
- grep 复核：`pending_lookup/add/remove/init` 调用点全部带 ftl 首参；`g_pending`/`g_rr_last_scan_ms`/`pending->ftl` 归零；`ftl_internal_sync_wait` 21 处调用点无改动
- 上板验证（待 FTL 完工统一做）：LUN_NUM=2 双实例提交 → 各自 completion 正确路由（读回填各自 buffer、写确认各自 Journal）

#### FTL 层未实现清单（后续实现参考）

> 本清单汇总「FTL 层边界内」的真实缺口（与上文遗留对应，按优先级排序）。
> 非 FTL 层（NVMe/controller 调度/UART CLI/平台 BSP 计时）见 ftl_design §16「未实现」。

| # | 项 | 现状 | 影响 | 工作量 | 入口/依赖 |
|---|----|------|------|--------|-----------|
| F1 | **CP/GC 定时触发接线 + 平台 time_us 钩子** | ✅ **v0.38 已实现** — `stm32_platform_time_us`（HAL_GetTick×1000）注册 `ssd_platform_ops_s`；ftl.c `ftl_now_ms()` 计算真实 elapsed；`cp_timer_start`/`gc_timer_start` 触发后回写；`cp.timestamp` 填真实时间戳 | T3 定时 checkpoint（10s）/GC 定时（500ms）首次真正生效 | 小（半天内） | 见 v0.38 条目 |
| F2 | **掉电重建 closed_sb_list** | ✅ **v0.40 已实现** — `ftl_rebuild_closed_sb_list`（power_failure_recovery）：全 NAND 扫描，逐 plane LDPC_RAW 判空 + `gc_read_page_with_lba` 取 LBA + `l2p_cache_read` 反查回填 valid_map；满块进 closed_sb_list（`allocator_add_closed_sb`）、前 2 部分写对恢复 Open、其余强制 Closed；free pool 拉出写过块；clean/PF 两分支都接线 | 掉电重启后已满 SB 正确重建 → GC 回收 + 不丢数据；顺带修复 free pool 回填误纳元数据块 | 大（独立立项） | 见 v0.40 条目 |
| F3 | **predictive 搬移（read disturb / 慢死）** | ✅ **v0.38 已实现** — `wl_predictive_relocate`（并入 wear_leveling）：`rr_predictive_scan` 扫 Closed 块 health≥CRITICAL → sb_id 反查 SB → `wl_relocate` 整 SB 搬移 → 双 plane `rr_retire_block` 退役（RESERVED，不标坏）；ftl_service_idle 带 1s 节流接入 | CRITICAL/DYING 块主动搬移 + 退役，不再靠读到坏兜底 | 中 | 见 v0.38 条目 |
| F4 | **checkpoint 增量（region_bitmap）** | ✅ **v0.41 已实现** — 脏区追踪（`region_dirty_bitmap` 置位/成功清零）+ `checkpoint_create` 快照先于 flush + 选择性 flush 失败中止 + 恢复侧 `ftl_boot_apply_region_bitmap` 按 bitmap 局部加载（跳过回放行）；顺带修 evict flush 失败丢脏数据（孤儿 bit）。**诚实标注：单 die 拓扑下与全量写页数相同，真实收益在 multi-LUN** | 行业标准 bitmap 语义 + 多 LUN 前置（CP 不再覆盖全部芯片时才有增量页数收益） | 大 | 见 v0.41 条目 |
| F5 | **写 Multi-Plane 真双页（VPC/空洞侧）** | ✅ **v0.38 已实现** — allocator 同号双 plane（industry super block）+ completion 对 plane1 页对称簿记（set_valid/L2P io.lba+1/Journal/清旧页）；pending 加 `old_pba1` | 跨 plane 写 VPC/映射/GC 搬移完整闭环，修复"plane1 烧错块"根因 | 中 | 见 v0.38 条目 |
| F6 | **pending/completion 单例多实例化** | ✅ **v0.42 已实现** — pending 表 per-instance（`ftl->pending[32]`，API 加 ftl 首参，删 pending 项内 ftl 字段）；completion 表保持全局（FC CQ 全局 + cmd_id 唯一，per-instance 会让异实例 completion 走错 default handler → 数据丢失），新增 `g_route[CH][CE][LUN]` 按 cmd->config.ch/ce/lun 路由到实例；`g_rr_last_scan_ms` 一并移入 ftl_context；`ftl_internal_sync_wait` 签名不变（21 处调用点零改动） | LUN_NUM>1 多 FTL 实例安全共存（第二个实例 init 不再冲掉第一个的 pending）；多 LUN 并发前置 | 中 | 见 v0.42 条目 |
| F7 | **写失败 Super Block 空洞的显式回滚** | ✅ **v0.39 已实现** — `allocator_close_open_sb` 运行时关闭坏 SB（行业流程 §3.8 ①）+ `bbt_mark_bad` 标坏 block 号 + L2P 自动回滚；坏 SB 进 closed → GC 优先回收 | 修复"坏块还挂在 OPEN SB 里继续写"的状态矛盾；好 plane 经 GC 擦除回 free pool | 低 | 见 v0.39 条目 |
| F8 | **journal 空间不足的紧急 checkpoint 回切校验** | `journal_truncate` 简化实现（擦除非当前 block）；`checkpoint_is_emergency` 有但触发面未验证 | Journal 满时依赖 checkpoint 水位线，简化截断在极端写模式下的正确性未测 | 中（测试为主） | 上板高压写验证；`journal_flush_page` 满页回切逻辑已实现 |
| F10 | **journal_replay 只回放最新一页（掉电 ≥2 页条目丢失）** | ✅ **v0.43 已实现** — `journal_replay` 签名加 `cp_sequence`，两遍扫描：收集全部 `page_seq > cp_sequence` 页 → 按 `(page_seq, entry_index)` 递增序应用（per-LBA last-write-wins，同 LBA 旧 GC/WL 搬移条目被新写覆盖）；**顺带闭合 GC/WL 擦除先于 journal flush 的掉电损坏窗口**（Explore 调研发现：journal write-back 未 flush 就擦 victim，掉电回放指已擦除块 → `gc_migrate_sb`/`wl_relocate` 擦除前强制 `journal_flush_page`）；PC 本机单测 `/f10/` 5 例（多页回放/CP 过滤/last-write-wins/排序防御/GC flush 契约，内存 NAND 桩，v0.44 起随 PC 桩一并移除） | 掉电恢复丢写（post-CP 条目超过 128 条时）+ 回放指向已擦除块 | 中 | 见 v0.43 条目 |
| F9 | **CQ/SQ 深度与多实例并发实测** | FC 池深度（64/256/128/64/16/64）按单实例预算 | 多 LUN/多实例并发 + 后台（GC/CP/BBT）同时争池未实测 | 中（测试） | 上板多实例后观察；文档 §17 背压已分析 |

**上板验证（v0.37）是 F1/F8/F9 的前置**：FTL 首次真跑，UART 日志 + 高压写会暴露 allocator/pending/GC 的真实行为。

### v0.43 — F10 journal_replay 全量回放 + GC/WL 擦除前强制 flush

**日期**：2026-10-02

#### 背景

F10（F 清单，v0.41 F4 review 发现）：`journal_replay` 只回放 `page_seq` 最大那一页的 128 条 entry。而 checkpoint 窗口内 journal 条目量可达 **~13,900 条 ≈ 109 页**（CP 触发：每 1024 次写 / journal 85% 水位 ≈ 13,900 条 / 10s 定时，checkpoint.c）。掉电时 post-CP 条目跨 ≥2 页 → 只恢复最新 128 条，其余**静默丢失**。

Explore 调研另发现一个超出 F10 原描述的**真数据损坏窗口**：journal 是 write-back（RAM `page_buf` 攒 128 条才 `journal_flush_page`）；GC/WL 搬移 append 条目到 RAM 缓冲后**同步擦除 victim 块**，两者间无任何 flush。掉电落在窗口内 → NAND 上只有旧条目 `X→A`（指向已被擦除块），新条目 `X→B` 从未落盘 → 恢复回放后 L2P 指向已擦除块 = 数据损坏。此窗口 F10 全量回放不修复（新条目根本不在 NAND 上），必须一并闭合。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| `journal_replay` 签名 | 加 `cp_sequence`（`cp->journal_sequence`），由 `ftl_boot_power_failure` 传入 | v0.41 已把 `cp` 传进恢复侧，前置条件已具备；过滤 `page_seq > cp_sequence` 的页（≤ 的条目已在 checkpoint 时 flush 进 NAND L2P 表） |
| 回放顺序 | 两遍扫描：收集全部需回放页 → 按 `(page_seq, entry_index)` 插入排序后逐页应用 | per-LBA last-write-wins——同 LBA 的旧 GC/WL 搬移条目被新写覆盖，不指向已擦除块；双块 page_seq 区间不交叠（truncate 擦整非当前块），显式排序防御未来 truncate 变化 |
| GC/WL 擦除前 flush | `gc_migrate_sb` / `wl_relocate` 在 `gc_erase_and_free` 前调 `journal_flush_page` | 闭合"搬移条目未落盘 + victim 已擦除"掉电窗口；`journal_flush_page` 幂等（空缓冲 no-op），后台 idle flush 不受影响 |
| 栈上页表 | `replay_pages[128]`（2 块×64 页），每项 16B → 512B | 现有栈上已有 `page_data[2048]`；不缓存 entry（逐页重读），SRAM 余量充足 |

#### 实现

1. **`include/ftl/journal.h`**：`journal_replay` 声明加 `uint32_t cp_sequence` 第三参。
2. **`src/ftl/journal.c`**：`journal_replay` 重写中段——第一遍扫描收集 `page_seq > cp_sequence` 页（沿用 magic/count/上限校验），第二遍按 `(page_seq, entry_index)` 插入排序后逐页应用 entry；保留尾部写指针推进（`current_block/page/entry` + `sequence_counter`，`ftl_boot_power_failure` 依赖其估算 entry_count）。
3. **`src/ftl/power_failure_recovery.c`**：`ftl_boot_power_failure` 把 `cp_sequence` 传给 `journal_replay`。
4. **`src/ftl/gc.c` + `src/ftl/wear_leveling.c`**：搬移循环结束、擦除前 `journal_flush_page`。
5. **测试**：`tests/unit/test_f10_replay.c`（5 例，内存 NAND 桩）——多页回放（3 页全回放，旧实现只回放最大页）/ CP 过滤（`page_seq ≤ cp_sequence` 不回放）/ last-write-wins（同 LBA 旧 GC 条目被新写覆盖）/ 排序防御（跨块条目按 page_seq 序结果一致）/ GC flush 契约（擦除前 flush 后条目必落盘）+ `tests/Makefile` target `f10` + 上板 `src/fc/makefile.mk`/`ut_main.c` 注册 `/f10/` 套件。
6. **`tests/framework/munit_stub_fc.c`**：`ftl_cmd_write/read_data` 从 no-op 升级为内存 NAND 桩（静态数组按 `cmd->paa` 地址读写 + erase 清 0xFF），`journal_truncate` 改弱符号（F10 链真实 journal.c 强符号自动覆盖）。
7. **`docs/design/revision_history/ftl_revision_history.md`**：v0.43 条目 + F 清单 F10 标"✅ v0.43 已实现"。

#### 验证

- `make -C tests test`：f4（5/5）+ f6（4/4）+ **f10（5/5）** 全过，编译零警告（-Werror）
- `make` 默认固件零错误零新增警告
- `make FC_NAND_TEST_ENABLE=1` 上板测试固件零错误编译
- 上板验证（待 FTL 完工统一做）：高压写中随机掉电 + 重启，`/f10/` 套件回放多页/覆盖场景数据一致

#### FTL 层剩余清单

> 本清单汇总「FTL 层边界内」的真实缺口（与上文遗留对应，按优先级排序）。
> 非 FTL 层（NVMe/controller 调度/UART CLI/平台 BSP 计时）见 ftl_design §16「未实现」。

| # | 项 | 现状 | 影响 | 工作量 | 入口/依赖 |
|---|----|------|------|--------|-----------|

---

### v0.44 — FTL 未落地项体检 + A 档落地（OP 10% + Spare Pool + 三级 Free Pool 状态位）

**日期**：2026-10-03

#### 背景

对 FTL 层做全量体检（对照设计文档/变更历史/记忆点逐项 grep 验证），发现设计承诺但代码零消费的缺口：
- **A 档**：`FTL_OP_PERCENT(10)`/`FTL_SPARE_POOL_SIZE(50)` 只定义不消费；`spare_pool_count` 仅 init 赋 0；`bbt_spare_pool_refill` 是 `return FTL_ERROR` 桩；`FTL_BLOCK_STATE_PRE_COND/EMERGENCY` 全库从未 set/clr → 容量语义实际 = 全好块进 free pool（OP 0%）。
- **B 档**（下阶段）：journal 水位分母错、checkpoint/emergency 标志永不复位、ftl_service 模式代理、GC/WL 读不记录 ECC、read_count 只写不读。
- **C 档**（下阶段）：controller state_machine/scheduler + NVMe host 全是编译进来的 `#warning` 桩；`SSD_REQUEST_FLUSH` 返回 NOT_SUPPORTED。

#### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| OP/Spare 容量语义 | **容量不动**（512MiB 不变）：OP 10% + Spare 50 只做 GC 缓冲池 + 坏块替换池，不进用户分配路径 | 用户已确认；L2P region 数/metadata 布局/FTL_LBA_MAX 全部不动，广告容量不变 |
| Spare 池分配 | **前 50 好块入 Spare 池**（坏块替换池，只出不进，耗尽后容量自然下降） | ADR-11 Block Sparing：坏块替换专用 |
| OP 池分配 | **前 10% 好块入 OP 池**（GC 缓冲池），free pool 耗尽时回退 OP 取双 plane 块 | ADR-8：GC 缓冲 + 性能保速 |
| PRE_COND 状态位 | `allocator_free_block` 置（归还即预擦除态）、`allocator_alloc` 清（取走占用） | free pool 唯一归还入口，全块预擦除态，`pre_cond_count` 从 free_count 别名升为真实预擦除计数 |
| EMERGENCY 状态位 | 保留枚举定义，不 set（无物理 Emergency 池） | `FREE_POOL_EMERGENCY` 阈值由 `allocator_needs_gc` 的 `free_count==0` 兜底 |

#### 实现

1. **`include/ftl/allocator.h`**：`ftl_init_params_t` + `ftl_allocator_t` 增 `op_blocks/spare_blocks/op_count/spare_count`；删 `spare_pool_count` 别名。
2. **`src/main.c`**：`g_op_blocks`/`g_spare_blocks` 落 SRAM2；boot 回填三分流（前 50 → Spare → 前 10% → OP → 其余 → free pool）；日志 `free/op/spare`。
3. **`src/ftl/allocator.c`**：`allocator_init` 读新 params；新增 `allocator_pull_from_op`；`allocator_alloc` free 耗尽回退 OP 取块；`allocator_free_block` 置 `PRE_COND`、`allocator_alloc` 清 `PRE_COND`。
4. **`src/ftl/bbt.c`**：`bbt_spare_pool_refill` 桩改真实现（`spare_blocks` 取块补 free pool）；`bbt_mark_bad` 后调用。

#### 验证

- `make debug / release / FC_NAND_TEST_ENABLE=1` 全零错误零新增警告
- SRAM：AXI 374.7KB/384KB、SRAM2 28.1KB/128KB（新增 op+spare 数组，不溢出）
- 上板预期：`[FTL] instance ... free=N op=~400 spare=50`；PRE_COND 状态位随 alloc/free set/clr
- 上板高压写：OP 池耗尽回退分配、坏块触发 Spare 补池

#### 待办（B/C 档，见 docs/archive/ftl_full_landing_plan_20261003.md）

- **B 档**：B1 journal 水位分母（`PAGES_PER_BLOCK`→`JOURNAL_BLOCK_COUNT×PAGES_PER_BLOCK`）、B2 checkpoint/emergency 标志复位、B3 ftl_service 模式基于 pending 占用、B4 GC/WL 搬移读记录 ECC + read_count++、B5 read_count 消费（predictive scan/WL victim）。
- **C 档**：C1 controller state_machine 完整状态机、C1b scheduler 优先级调度、C2 NVMe host（nvme_io/admin/SQ/CQ/nvme_controller 全量）、C3 `SSD_REQUEST_FLUSH` 全实例 flush。（**已在 v0.47 落地**：C2 NVMe 按学习主控定位砍掉，改 UART CLI 驱动 FTL，见 v0.47 条目）
- **F8/F9**：依赖上板（紧急 CP 回切校验 / CQ·SQ 深度实测）。

---

### v0.45 — B 档落地（journal 水位修复 + 标志复位 + ftl_service 空闲度 + GC/WL 读记 ECC + read_count 消费）

**日期**：2026-10-03

#### 背景

v0.44 体检确认的 B 档 5 项语义缺口（FTL 层正确性）全部落地：

| # | 缺口 | 修复 |
|----|------|------|
| B1 | `checkpoint_journal_watermark` 分母用 `PAGES_PER_BLOCK(64)`（单块），容量实为双块 128 页 → 85%/95% 在 page≈55/61 触发，紧急水位离写满只剩 3 页≈384 条（设计预算 768 条） | 分母改 `JOURNAL_BLOCK_COUNT×PAGES_PER_BLOCK(128)`：85%→page≈108（留 19 页）、95%→page≈122（留 6 页≈768 条），与 §7.5.2 对齐 |
| B2 | `journal.checkpoint_needed/emergency` 只有 init 置 0、append 置 1，永不复位 → T1 永久命中（每圈必 CP）+ emergency 置位后 BUSY 每圈强制 CP | `checkpoint_create` 成功末尾 + `journal_truncate` 双处 `=0` 复位（设计 §7.5.4） |
| B3 | `ftl_service` 三级模式 = `submitted==0→IDLE/≤2→NORMAL/else→BUSY`（"本轮有无提交 NAND"代理），host 无 IO 时 fc_service 也可能非 0 → 误判 NORMAL/BUSY 推迟后台 | 新增 `pending_count()`/`completion_table_count()`，模式改 `inflight==0→IDLE / pending>16→BUSY / 其余→NORMAL`（基于未决命令占用近似空闲度） |
| B4 | GC/WL 搬移读 `gc_read_page_with_lba` 不记录 ECC/read_count（`rr_record_ecc` 只在 host 读 completion）→ 搬移读 read disturb 盲区 | `gc_read_page_with_lba` 加 `ecc_out`/`read_count_out`；`gc_migrate_sb`/`wl_relocate` 成功读 → `rr_record_ecc` + `read_count++` |
| B5 | `read_count/program_count` 只写不读 | `rr_predictive_scan` 加 `read_count≥RR_READ_DISTURB_THRESHOLD(1000)` 判据；`wl_victim_s` 加 `read_count` 字段，同有效页数下 read 多冷块优先 |

#### 实现

1. **`src/ftl/checkpoint.c`**（B1/B2）：`checkpoint_journal_watermark` 分母改 `(uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK`；`checkpoint_create` 末尾复位 `emergency/checkpoint_needed`。
2. **`src/ftl/journal.c`**（B2）：`journal_truncate` 擦除对方块后复位两标志（双保险）。
3. **`include/ftl/pending.h` + `src/ftl/pending.c`**（B3）：新增 `pending_count(ftl)`。
4. **`include/ftl/completion_table.h` + `src/ftl/completion_table.c`**（B3）：新增 `completion_table_count()`。
5. **`src/ftl/ftl.c`**（B3）：`ftl_service` Phase 3 改 `inflight==0→IDLE / pending>16→BUSY / 其余→NORMAL`。
6. **`include/ftl/gc.h` + `src/ftl/gc.c`**（B4）：`gc_read_page_with_lba` 签名加 `ecc_out`/`read_count_out`（4 CW 最大 ECC，与 host 读 completion 同口径）；`gc_migrate_sb` 搬移读后 `rr_record_ecc` + `read_count++`。
7. **`src/ftl/wear_leveling.c`**（B4/B5）：`wl_relocate` 搬移读后 `rr_record_ecc` + `read_count++`；`wl_select_victim` 加 read_count 权重（`wl_victim_s` 加字段）。
8. **`include/ftl/read_recovery.h` + `src/ftl/read_recovery.c`**（B5）：`RR_READ_DISTURB_THRESHOLD(1000)`；`rr_predictive_scan` 加 read_count 判据。
9. **`src/ftl/power_failure_recovery.c`**（B4 签名波及）：掉电重建反查传 `NULL,NULL`（重建非搬移读，不记录）。

#### 验证

- `make debug / release / FC_NAND_TEST_ENABLE=1` 全零错误零新增警告
- SRAM 无变化（纯逻辑改动）
- 上板预期：journal 85%/95% 水位按双块 128 页正确触发；T1 不永久命中；emergency 复位后 BUSY 不再每圈强制 CP；高压写/搬移后 read_count 与 ECC 历史真实增长，predictive scan 能命中 read disturb 慢死块

#### 待办（C 档，见 docs/archive/ftl_full_landing_plan_20261003.md）→ ✅ v0.47 已落地

- C1 controller state_machine 完整状态机、C1b scheduler 优先级调度、C2 NVMe host（nvme_io/admin/SQ/CQ/nvme_controller 全量）、C3 `SSD_REQUEST_FLUSH` 全实例 flush。（落地：C1 最小状态机 / C1b 调度钩子 / C3 FLUSH / C2 砍掉改 UART CLI，见 v0.47 条目）
- **F8/F9**：依赖上板（紧急 CP 回切校验 / CQ·SQ 深度实测）。

---

### v0.46 — B 档复查修复（B1 水位分子口径 + B4 死变量清理）

**日期**：2026-10-03

#### 背景

v0.45 B 档落地后逐 diff 复查，发现 2 处问题（编译验证盲区——逻辑错误 make 检不出）：

1. **B1 真 bug（分子口径错）**：v0.45 只把 `checkpoint_journal_watermark` 分母 `PAGES_PER_BLOCK(64)` 改成 `JOURNAL_BLOCK_COUNT×PAGES_PER_BLOCK(128)`，但**分子仍是 `current_page`（0..63）**。分母 128 + 分子最大 63 → `used` 最大 ≈ 49%，**85%/95% 永远不触发**（比旧实现更糟，水位线整体失效）。
2. **B4 死变量**：`gc_migrate_sb` 加了 `now_writes = 0U` 恒 0（本函数无 total_writes 入参），`rr_record_ecc` 传 0 使 `last_update` 无意义。

#### 修复

1. **`src/ftl/checkpoint.c`**：`checkpoint_journal_watermark` 分子分母都按双块 128 页全局口径：
   ```
   used = (current_block×64 + current_page)×100 / (2×64) + entry 项
   ```
   分子含 `current_block×PAGES_PER_BLOCK`（双块环形全局页偏移）。85% 在全局页≈108（留 19 页）、95% 在全局页≈122（留 6 页≈768 条），与 §7.5.2 对齐。
2. **`src/ftl/gc.c`**：删 `now_writes` 死变量，`rr_record_ecc(&src->rr, ecc, 0U)` + 注释"不追踪时间维度，rising_count/health 只用 ecc_bits 本身"。

#### 验证

- `make debug / release / FC_NAND_TEST_ENABLE=1` 全零错误零新增警告
- **上板验证（必要）**：`checkpoint_journal_watermark` 只在 host 读 completion 调用，编译无法验证 85%/95% 触发时序——上板高压写观察 journal 水位 / emergency 复位。这是编译验证盲区的直接教训。

#### 教训

`make` 只保证编译通过，不保证逻辑正确。纯逻辑改动（水位公式/标志复位）必须**上板验证触发时序**，不能以"编译零警告"作为完成标准。

---

### v0.47 — C 档落地（最小状态机 + host IO 优先调度 + FLUSH + UART CLI 驱动 FTL；NVMe 删除）

**日期**：2026-10-03

#### 背景

C 档按学习主控定位（stm32h743 + MT29F4G08，非真存储主控）重新规划（设计文档 §13.4 **D10：Host 命令来源 = UART Shell CLI**，§16 NVMe 标未实现）。NVMe 是 PCIe 协议壳、无硬件对手件，纯桩无验证价值 → **C2 砍掉**，UART CLI 作为 host 层完整驱动 FTL。

#### 落地

**C1 最小状态机**（`src/controller/state_machine.c` + `include/controller/controller.h`）
- `ssd_controller_state_transition`：RESET→INITIALIZING（boot 入口）→READY/FAILED（槽位校验结果），READY→FAILED 预留，FAILED 终态；非法转换忽略
- `ssd_controller_init`（controller.c）改走状态机：任一槽位缺失/拓扑不符 → `transition(FAILED)` + 返回 `INVALID_ARGUMENT`；全过 → `transition(READY)`

**C1b host IO 优先调度**（`src/controller/scheduler.c` + `controller.h`）
- `ssd_controller_schedule(controller, host_io_active)` 轻量钩子：`ssd_controller_submit` 每次提交调一次（host_io_active=1）返回 0=后台让路；空闲返回 1=全力后台
- 真正后台预算由 `ftl_service` B3 空闲度（pending+completion_table 占用 → IDLE/NORMAL/BUSY）承担，scheduler 不重复实现队列（学习主控单 LUN，全量优先级调度收益有限）

**C3 FLUSH**（`src/controller/controller.c`）
- `SSD_REQUEST_FLUSH`：对全部 `controller->ftl[ch][ce][lun]` 同步 `l2p_cache_flush_all(&ftl->cache)` + `journal_flush_page(&ftl->journal)`（两个原语内部 `ftl_internal_sync_wait` 阻塞泵完）→ `completion_status=OK` + `complete_flag=1`
- 多块 FLUSH（block_count≥2）仍走 `ssd_request_split` default → `NOT_SUPPORTED`（FLUSH 是全局操作，不按 LBA 路由，命令表 `fl` 用 block_count=1）

**UART CLI 驱动 FTL**（`src/platform/stm32h743/usart.c`，D10 核心，+367 行）
- `g_wr_buf[FTL_READ_USER_SIZE×CW_NUM×PLANE_NUM]=16KB` 落 SRAM2（`.sram2_data` NOLOAD → `usart_init` 主动预填）：plane0 区（前 8KB）=`0x55555555`、plane1 区（后 8KB）=`0xAAAAAAAA`
  - Multi-Plane 写（pages=2）时前 2048B 进 plane0（LBA n）、后 2048B 进 plane1（LBA n+1）——写后读回一眼可确认双页簿记正确
- 命令表 5 条（`wr <lba> <n>` / `rd <lba> <n> [c]` / `tr <lba>` / `fl` / `info`）：
  - `rd` 无 `c` → hex dump 前 64B；有 `c` → 与 pattern 按 4B 粒度比对，报 `PASS` / `FAIL at byte <off>: got.. expect..`
  - `tr`/`fl` 单块提交；`info` 打印 controller 状态机名 + 每 (ch,ce,lun) `free/op/spare/pre_cond/bad/closed`
- **两段式**（D11 非阻塞 pending 语义）：handler 只 `ssd_controller_submit` 立即返回（打印 `submit ok, waiting...`），`uart_service_poll` 每轮末尾 `uart_cli_pending_poll()` 检查 `ssd_controller_is_complete` 打印结果——主循环 `ftl_service` 异步完成，CLI 不阻塞主循环
- `g_ctrl` 去 static（main.c）+ `usart.h` `extern ssd_ctl_s g_ctrl;`

**NVMe 删除**（C2 砍掉落地）
- 删 `src/host/nvme/*.c`（5 桩）、`include/host/nvme/nvme.h`、`src/host/makefile.mk`；`config.mk` MODULES 移除 `host`
- 零引用零编译（wildcard 收集，无文件 include nvme.h）；`make debug/release/tests` 全零警告零错误（4 个 NVMe `#warning` 彻底消失）

#### 验证

- `make debug / release / tests(FC_NAND_TEST_ENABLE=1)` 全零错误零警告
- SRAM2 44.1KB/128KB（+16KB g_wr_buf，不溢出）；AXI_RAM 377KB/384KB
- 上板序列（真 NAND）：`info`（state=READY + free/op/spare 计数）→ `wr 0 2` → `rd 0 2 c`（PASS 确认双 plane pattern）→ `tr 0` → `fl` → `info`（free 回补）
- **F8/F9 仍依赖上板**（紧急 CP 回切校验 / CQ·SQ 深度实测），本轮不改

#### 备注

- `FC_COMMAND_MODULE_HOST_IO` 枚举保留零使用（F8/F9 上板后视需要再接线）
- stm32h7r3 平台 usart.c 未动

---

### v0.48 — Block State Table 瘦身（ftl_block_t 56B → 40B）

**日期**：2026-10-04

#### 背景

AXI_RAM 使用率已达 98.2%（377.1KB/384KB），内存压力真实出现。逐字段审计 `ftl_block_t` 12 个字段后确认：**问题不是"该搬到 NAND"，而是"胖 + 字段顺序浪费对齐"**。行业对标（设计文档 §4.9.3）显示本方案 56B/项 是消费级 DRAM-less 基准（4~16B）的 3~14 倍。

#### 落地

**① `rr` ECC 健康 16B → 3B**（`include/ftl/bst.h`、`src/ftl/read_recovery.c`）
- 删 `ecc_history[8]` + `history_idx` + `last_update`，改单字节 `last_ecc`
- 8 深度环形缓冲的**唯一用途**是供给趋势比较的 `prev` 一个值 → 单字节足够
- **顺带修复 bug**：原 `prev = ecc_history[history_idx]` 取的是环形缓冲里**将被覆盖的最老格**（8 次读之前的值），与注释「上一次值」不符；`last_ecc` 语义即真正的上一次读数
- 顺带清掉 `last_update`（4B，`gc.c` 写点有、读点零，且 `gc.c` 调用时已在传 `0U` 假值）
- `rr_record_ecc` 签名去掉 `total_writes` 形参，3 个调用点同步（`ftl.c` / `gc.c` / `wear_leveling.c`）

**② 字段重排**（`include/ftl/bst.h`）
- `rr` 从 `valid_map` **之后**移到**之前**
- 原因：`valid_map` 是 `uint64_t[PLANE_NUM]` 需 8 字节对齐；`rr` 只有 3B，留在数组之后会被尾部对齐从 40B 撑到 **48B**（白丢 8B/块 = 32KB）
- 新增 `_Static_assert(sizeof(ftl_block_t) == 40, ...)` 编译期守卫（此前无任何尺寸断言）

#### 审计发现（本次保留，未处理）

| 类别 | 明细 | 规模 |
|---|---|---|
| 死字段（写了无人读） | ~~`last_pe_ts`~~（**v0.51 已消费**，见下）、~~`program_count`~~、~~`bad_reason`~~、~~`gc_generation`~~（三者 **v0.56 已删**） | ~~8B/块 = 32KB~~（**订正：这 4B 落在 `valid_map` 对齐 padding 里，删之省 0 字节**，见 `ftl_design.md §4.9.6`） |
| 只写不读的状态位 | `OPEN`、`RESERVED`、`TEMP_HOT`、`TEMP_COLD`、~~`PRE_COND`~~；~~`EMERGENCY`~~ 全库零引用（两者 **v0.52 已删**） | — |
| 跨重启不持久化 | `erase_count` / `read_count` / `rr` 每次 `ftl_init` 被 `memset` 归零（`ftl.c`）；`METADATA_TYPE_ERASE_COUNT` 定义了但零引用 | — |

死字段成因：v0.32 建 BST 时按「行业 Block Info Table 标准形态」一次性把字段铺全（`c74f1ca`），标注「为后续预留」，但消费方一直未写。

**持久化缺口是独立的功能问题**——磨损均衡（`erase_count`）与 read disturb（`read_count`）、慢死预测（`rr`）跨重启全部失效，需单独排期，不属于结构体重构范畴。

#### 验证

- `make tests` 零告警零错误；`_Static_assert` 通过
- `.bss.g_blocks`：`0x38000`(224KB) → **`0x28000`(160KB)**，精确命中预期
- AXI_RAM：98.2% → **81.5%**（377.1KB → 313.1KB，-64KB）；总 SRAM 55.8% → 48.6%
- **上板验证未做**（按项目规则由用户执行）

#### 备注

- **SB 索引粒度已评估，本次不做**：`allocator.c` 的 `block1 = block0` 使 SB 永远配对同号物理块，两 plane 的 `erase_count`/`state`/`sb_id`/`valid_count` 天然相同，per-block 存是重复的 → 改按 SB 索引可再省 96KB（160KB → 64KB）。但 `ftl_gblock()` 是所有索引的单一入口，改动覆盖整个 FTL，且丢 per-plane 分辨率（`read_count`/`rr`）、与 BBT 的 per-physical-block 粒度不一致。设计文档 §4.9.5 选项 C 早已评估并否决，理由一致。⚠ **v0.57 结案**：本条前提 `block1 = block0` 已被 **v0.12 Policy B** 删除（逐 plane 独立选块）⇒ 两 plane 元数据互不相同、**无可去重**；且主流是**两级**结构（band 级 + per-block BIT）。见 **v0.57 条目**。
- **关联问题**：`block1 = block0` 本身是简化而非行业做法（真实主控允许 SB 内每 plane 用不同物理块号）。当前同号约束导致 plane0 的坏块连坐浪费 plane1 的同号好块。与索引粒度强相关，应合并评估。
- **rebuild 套件夹具超容**：`tests/unit/test_rebuild.c` 的 245KB 夹具超 128KB DTCM 堆，本次结构体缩小不解决该独立缺陷。→ **2026-10-04 后续处理**：已把该套件从板端构建摘除（`src/fc/makefile.mk` + `tests/unit/ut_main.c`），夹具落地区域另议，见 `common/fault_capture.md` §7 与 `docs/archive/debug_sbrk_heap_stack_20261004.md` §5。

---

### v0.49 — P0-1 计数持久化（Wear 表 load/save）

#### 背景

`ftl_optimization_plan.md` §P0-1（最高优先）。`ftl_init` 的
`memset(params->blocks, 0, sizeof(ftl_block_t) * FTL_BLOCK_TOTAL)` 每次开机把整张 BST 清零
⇒ 三个计数跨重启归零，依赖它们的三条机制**全部失效**：

| 消费方 | 位置 | 失效表现 |
|---|---|---|
| 动态 WL（选擦除最少块） | `allocator_select_plane_block` | 永远选到第一个 free 块 |
| 静态 WL（`max_ec - min_ec ≥ 5000`） | `wl_should_trigger` | 判定恒假 |
| read-disturb 预测（`read_count ≥ 1000`） | `rr_predictive_scan` | 判定恒假 |

#### 关键决策

**① 介质形态：块内**页式追加日志**，不是"每次整表重写"。**

块 `WEAR_BLOCK`（26）只有 64 页，全量快照 25 页 ⇒ 整表重写会让块 26 在 ~2.6 次落盘后
被擦一次，磨损远超数据块（器件寿命 ≈ 4096 块 × 10⁵ P/E ≈ 4.1×10⁸ 次块擦除；整表重写方案
块 26 寿命内约 12 万次自擦，超器件规格；追加日志 1–4 页/次 ⇒ 约 2–6 万次，在规格内）。

形态与 Anchor / Journal 同一套日志惯例：每页自包含 `magic + sequence + entry_count + CRC`，
**页序 == sequence 序**（块内只追加；块满则擦块 + 从头写全量快照）。
语义 = **绝对值 + 最新胜**（不是增量累加）⇒ 同一块多次出现只有最后一页生效，
重试重复写同一块是幂等的。

**② 持久化哪三个字段。**

只落**有消费方或为档三预留**的：`erase_count`（静态/动态 WL）、`read_count`
（read-disturb 阈值 + `wl_select_victim` 平手判定）、`program_count`（健康模型，
**当前无消费方**，为 `../ftl_design.md` §19 档三预留）。

不落 `last_pe_ts` / `gc_generation`（死字段，且 `last_pe_ts` 是时间戳、跨重启无意义）。
**`rr`（3B）不落** —— 它驱动 `rr_predictive_scan` 的 CRITICAL 分支，但把它加进来会把条目
从 12B 撑到 16B、快照 25→33 页、压缩 delta 余量；read-disturb 的主触发
（`read_count ≥ 1000`）已被覆盖。登记为后续扩展。

**③ 未复用 `METADATA_TYPE_ERASE_COUNT`（`metadata.h`，仍零引用）。**

Wear 块位置固定，不需要走 Metadata Directory 登记——与 BBT 同理（BBT 也是固定块，
不走 Directory）。`METADATA_TYPE_*` 是给"位置会变"的元数据用的。

**④ 状态放 file-scope static，不放 `ftl_context_t`。**

与最近的模板 `bbt.c`（`g_bbt_sequence` / `g_bbt_dirty`）一致。
⚠ 登记局限：**单实例**，与 `bbt.c` / `completion_table.c` 同类，是已登记的提维度阻断点之一，
等 dim ≥ 2 落地时一并搬进 `ftl_context_t`（per-instance 惯例见 `checkpoint.c`）。

**⑤ 节拍门限量用 `ftl->total_writes`（主机写成功数）作为"擦除活跃度"的代理。**

没有独立的擦除计数器可用，而主机写入是 GC 擦除的唯一驱动。
`WEAR_FLUSH_INTERVAL_MS`（120 s）**或** `WEAR_FLUSH_ERASE_DELTA`（1024 次写入）任一满足即落盘。
纯读负载下 `total_writes` 不增长 ⇒ 只有定时门限生效（这正是想要的：读负载只需保 `read_count`）。
代价：两次落盘之间掉电丢 ≤ 1024 次擦除 / ≤ 120 s 读计数。Wear 是**软状态**
（丢了只退化 WL 精度，不丢数据），静态 WL 阈值 5000 ⇒ 1024 的窗口 = 20%，可接受。

#### 改动清单

| 文件 | 改动 |
|---|---|
| `include/ftl/wear.h`（新增） | `WEAR_MAGIC` / `wear_entry_t`(12B) / `wear_page_t`(2048B) / 节拍常量 / 3 个原型 + 3 条 `_Static_assert` |
| `src/ftl/wear.c`（新增） | `wear_page_addr` / `wear_init` / `wear_flush` / `wear_mark_dirty` + 脏位图 |
| `include/ftl/ftl_config.h` | 新增 `FTL_WEAR_ENTRY_BYTES (12U)`；`FTL_WEAR_TABLE_BYTES` 由 `× sizeof(uint32_t)` 改 `× FTL_WEAR_ENTRY_BYTES`（16 KB → 48 KB，**仅逻辑表字节，不影响介质寻址**） |
| `src/ftl/ftl.c` | `ftl_init` 在 `bbt_init` 后加 `wear_init`；`ftl_service_idle`/`_normal` 在 `bbt_flush` 后加 `wear_flush`；3 处 `wear_mark_dirty` |
| `src/ftl/gc.c` | 2 处 `wear_mark_dirty`（`gc_erase_and_free` 的 `erase_count++`、`gc_migrate_sb` 的 `read_count++`） |
| `src/ftl/wear_leveling.c` | 2 处 `wear_mark_dirty`（`wl_relocate` 的 `read_count++`、`rr_retire_block` 调用点） |

**7 个脏标记点**（`erase_count`/`read_count`/`program_count` 的每一个变更点）：

| # | 位置 | 变更的计数 |
|---|---|---|
| 1 | `gc.c` `gc_erase_and_free` | `erase_count++` |
| 2 | `ftl.c` 主机读完成 | `read_count++` |
| 3 | `gc.c` `gc_migrate_sb` 迁移读 | `read_count++` |
| 4 | `wear_leveling.c` `wl_relocate` 迁移读 | `read_count++` |
| 5 | `ftl.c` plane0 新页 | `program_count++` |
| 6 | `ftl.c` plane1 新页（multi-plane） | `program_count++` |
| 7 | `wear_leveling.c` `rr_retire_block` 调用点 | `read_count = 0`（`rr_retire_block` 只拿到 `ftl_block_t*`，故标记放在其唯一调用者） |

#### 介质格式

**不新增格式变更**：块 26 早已在分区表 `g_ftl_partitions[]` 里预留（`"Wear"` 项，
`ftl_config.c`）且被 `ftl_is_metadata_block` 排除 ⇒ 本次只是**第一次往里写内容**。
块号/分区一个没动。上板前仍须擦除芯片——那是因为**上一次布局维度无关化**
（`c5cb8a0`：Anchor 改分页自包含 + L2P 块位置 + Journal 2→4 块），不是本次。

#### 开机可观测

`wear_init` 末尾一行横幅式 `LOGI`（这是判据「重启后计数一致」的观测口）：

```
[INF] [WEAR] seq=… pages=… max_ec=… min_ec=…
```

`pages` = 回放的有效页数（0 = 冷启动）；`max_ec`/`min_ec` = 非坏块的擦除计数极值。

#### 验证

- `make all` / `make tests` 零告警零错误；`build/*/obj/ftl/wear.o` 由 `wildcard` 自动纳入
  （**不需要改 Makefile**）
- `make format-check` 通过（`FORMAT_FILES` 用 `find`，新文件自动纳入）
- `_Static_assert`：`sizeof(wear_entry_t) == 12`、`sizeof(wear_page_t) == 2048`、
  `WEAR_SNAPSHOT_PAGES(25) <= PAGES_PER_BLOCK(64)`
- **本次不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

#### 登记不做

- **`program_count` 溢出**：u16 从不归零（`gc_erase_and_free` 只清 `erase_count`/`last_pe_ts`/valid）
  → **v0.56 已删该字段**，溢出问题随之消失
- **`last_pe_ts` / `gc_generation`** 死字段 → `last_pe_ts` v0.51 已消费；`gc_generation` **v0.56 已删**
- **`rr` 持久化**（见决策 ②）
- **多实例**（见决策 ④）
- **read 侧量化**：若窗口内 4096 块全脏（全盘随机读），日志退化成整表 25 页 ⇒ 与整表重写同价。
  要收紧就给 read 侧加"每 N 次读才标脏"的量化，或调大 `WEAR_FLUSH_ERASE_DELTA`。

---

### v0.50 — P0-2 元数据写模型统一（追加日志 + 块池轮转）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` / `make tests` 零告警零错误）

#### 背景

`ftl_optimization_plan.md` 里 P0-2 的原文是「`METADATA_DIR_ENTRIES=128` 但 region 有 2048」——
**这一条已过时**（常量早已改成 `= L2P_REGIONS_PER_INSTANCE`）。但顺着它往下查，挖出的是
比「容量不够」严重得多的两个**已生效**缺陷：

1. **Anchor 一次都没写过**：`metadata_dir_fill_fixed` 把 2048 个 region **全部预登记**成固定位置
   ⇒ `metadata_dir_lookup` 永远命中 ⇒ `dir->dirty` 恒 0 ⇒ `metadata_dir_flush_to_anchor` 早退
   ⇒ `metadata_dir_load` 永远失败 ⇒ 永远走 `metadata_dir_init`。整条动态路径死透。
2. **Checkpoint 第二次写即损坏**：`checkpoint_create` 每次重写**同 6 页**，NAND 只能 1→0 ⇒
   第二次起头 CRC 即失效 ⇒ `checkpoint_find_latest` 失败 ⇒ `ftl_boot_fresh_start` ⇒ **映射全丢**。
   T3 定时触发是「elapsed ≥ 10 s 且 IDLE/NORMAL」，`ftl_service_idle` 在 `inflight==0` 时必被调
   ⇒ **板子空闲跑过 10 s 就必中**——不是潜伏，只是从未被断言。

**真正的根缺陷在最下面一层**：`l2p_cache_flush_one` 把每个 L2P region 写回**固定地址，且从不擦除**。
NAND 只能 1→0，对已编程页二次编程 = 新数据 **AND** 旧数据 ⇒ **静默的映射损坏**。
同病遍布全部元数据：全仓 `ftl_sync_erase_block` 的调用者原先只有 `journal.c` / `gc.c` / `wear.c`
—— **没有任何 L2P / Anchor / BBT / Checkpoint 块被擦过**。
潜伏原因：顺序冒烟测试每个 region 只 flush 一次（64 行 × 512 项 = 32768 LBA，测试写不到）。

#### 关键决策

① **方向 = B 方案（追加日志 + Directory），按存储主控方向走**（用户定）。
   不是「加容量」，是**换写模型**——扩容只治了症状，且那次扩容反而把机制从半工作翻成全死。

② **修复范围 = 全部元数据统一**（用户定）：L2P / Anchor / BBT / Checkpoint 一套写模型。
   **Journal 已经是对的，不动**（4 块环 + 擦除 + 两遍重放是全仓唯一本来就对的元数据；
   本次只挪块号 40-41 → 58-61）。

③ **验收判据 = 两者都要**（用户定）：① 同一 region 连续两次 flush 后读回正确；
   ② `region_id > 128` 的 LBA 走动态分配。**追加 ③**：复位后数据仍在（因为当前必然失败）。

④ **Anchor 类元数据落多块池 N=32**（用户定）。理由不是磨损（本仓磨损不是约束），
   是**主控形态**：固定根 + 专用元数据块池。

#### 落地

**统一写模型（4 条规则 + 1 条校验）**：

| 规则 | 说明 |
|---|---|
| 只追加 | 元数据页只写**擦除态**页；对已编程页的二次编程 = 损坏，一律禁止 |
| 带 sequence | 每条 record 带单调递增 sequence；页序 == sequence 序 |
| 池内轮转 | 写满一块 → 擦**池内下一块** → 从页 0 续写 |
| 最新有效胜 | 读取 = 扫池内所有 record，取「**整条校验通过**」中 sequence 最大者 |
| 整条校验 | 一条 record 的多页必须**全部有效且 sequence 一致**，否则整条丢弃（不取半条）|

**共用原语**（`src/ftl/metadata.c`，Anchor / Checkpoint / BBT 三方复用）：
`metadata_ring_addr` / `metadata_ring_locate` / `metadata_ring_prepare_write`。

`metadata_ring_locate` 是本次最费心的算法（O(池块数) 快路径 + 仅损坏时全池慢路径）：
① 逐块廉价探针**该块末条** record（末条有效 = 该块写满）；
② 第一个末条无效的块 = **游标块**；若全部有效 ⇒ 池转满一圈 ⇒ 游标 = 最新满块之后，且该块整体擦掉重写；
③ 游标块内降序找最后一条有效 record；④ 写点 = 其 +1（块内最后一条则跨到下一块槽 0）；
⑤ 候选 = {全局最新满块末条, 游标块末条}，去重、按 seq 降序、**整条校验**；
⑥ 慢路径（仅损坏时）：全池探针取 max seq，整条校验，失败则排除该 seq 再找次新。

**新块布局（per-plane 块号）**：

| 区 | 块号 | 块数 | 说明 |
|---|---|---|---|
| L2P 基础层 | 0..15 | 16 | **哨兵层：永不写入**，页保持擦除态 ⇒ 读回 0xFF = 未映射 |
| Anchor 池 | 16..47 | 32 | 一条 8 页，256 槽 |
| Checkpoint 池 | 48..55 | 8 | 一条 1 页，512 槽 |
| BBT 池 | 56..57 | 2 | 一条 1 页，128 槽 |
| Journal | 58..61 | 4 | **只挪号，逻辑不动** |
| Wear | 62 | 1 | 只挪号 |
| `FTL_METADATA_BLOCK_END` | **63** | | 原 27 |

物理 126 / 4096 = **3.1%** 系统区。

**Checkpoint 写序重排**：旧序 ② flush 脏 region → ③ 写 header → ④ flush Anchor → ⑤ truncate
⇒ header 早于 Anchor ⇒ 两者之间掉电会留下「header 新 / Directory 旧」⇒ 已声称落盘的 region
在 Directory 里找不到 ⇒ **丢更新**。新序 **① 快照 bitmap → ② flush 脏 region → ③ flush Anchor
→ ④ 写 header → ⑤ truncate journal**：掉电在 ④ 之前 ⇒ 本次 CP 未记录，boot 用上一个 CP
并回放 journal（⑤ 还没跑）⇒ 一致。

**L2P 日志块**：`l2p_cache_flush_one` 每次 flush **无条件追加**到日志块（一块 64 槽，一个 region
= 一页）。块满 ⇒ 从 Free Pool 取新整块（`allocator_alloc_reserved_block`，**不是**与 GC 共享的
「冷 SB 的一页」）。游标**不跨重启持久化**（`metadata_dir_load` 置 `cur_block_valid = 0`），
首次追加时才分配 ⇒ 绝不复用可能已编程的旧页。旧块活 region 归零即归还
（`metadata_dir_commit_region` 里急切回收，把「已无引用却仍带旧内容」的窗口压到最短）。

#### 介质格式

**再次变更**（叠加在 `c5cb8a0` 布局维度无关化之上）⇒ 上板前**必须先擦除芯片**。

#### 开机可观测

`metadata_dir_load` 末尾一行（判据②「走动态分配」/ 判据③「Anchor 真的落盘」的唯一观测口）：

```
[INF] [META] anchor_seq=… dyn=… cur_blk=… live_blks=…
```

#### 验证

- `make clean && make all` / `make clean && make tests` 零告警零错误
  （⚠ `make all` 后直接 `make tests` 是空操作：共用 obj 目录、不跟踪 CFLAGS ⇒ **必须先 `make clean`**）
- 新增 `_Static_assert`：各池槽位数 ≤ `UINT16_MAX`；`PAGES_PER_BLOCK % ANCHOR_PAGE_COUNT == 0`
- **本次不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

#### 顺带修复（超出计划字面范围，登记）

- **`l2p_checkpoint_header_t` 尺寸 bug（2296 → 2048 B）**：`padding[2048 - 7*4 - 4]` 算式把
  `region_bitmap[64]`（256 B）算成 7 字 ⇒ 结构体 2296 B > 一页。旧写入被
  `ftl_cmd_write_data`（4 CW × 512 B）**静默截到 2048**，所以从未真正越界——但「一条 record
  = 一页」名不副实。现改为 `padding[2048 - 70*4]`（1768 B）+ `_Static_assert`，
  并把 `CHECKPOINT_CRC_LEN` 改写成 `offsetof(..., crc)`（算式写错时覆盖范围会跟着错）。
  （⚠ **v0.62 起该宏随记录内 CRC 一并删除**——元数据页完整性改由下层 FC per-CW CRC + ECC 承担。）
- **`ftl_boot_fresh_start` 补 `ftl_rebuild_closed_sb_list`**：`main.c` 的注释断言
  「用户数据 written 块仍回填（时序安全：boot_recovery 的 F2 重建会把它们拉出）」，
  但 fresh_start 路径此前跳过了它 ⇒ Free Pool 会信任未擦除的块。已闭合。

#### 登记不做

- **阶段 3 L2P 日志块压缩**（抗碎片）⇒ 已登记为 **`ftl_optimization_plan.md` 的 P0-3**
  （含完整设计：触发条件 / victim 选择 / 搬移顺序 / 重入切断 / 崩溃安全 / 判据 / 依赖）。
  一句话：若某日志块挂着「永不再写」的冷 region，它永远不归零 ⇒ 永久占块。
  **依赖本 ADR 的阶段 1+2 先上板验证通过**，**长时压测前必须补**。
- **多实例**：`checkpoint.c` / `bbt.c` / `wear.c` 的 file-scope static 游标/序号在多 LUN 下
  所有实例共享一份（局限登记在案）。
- **不做 SPDK FTL 式的 P2L 重建**（每 band 尾部存 P2L、开机顺序扫描重建 L2P）——那是替换整层
  持久化机制的大改，登记为后续方向；本仓 `ftl_rebuild_closed_sb_list` 已有雏形。
- **不顺手修**：~~`ftl_is_metadata_block` 的 per-plane 粒度~~（**✅ v0.69 已修**，见该版 ADR）、~~`program_count` 无消费方~~（**v0.56 已删该字段**）、`rr` 不持久化（**✅ v0.65 已落地**）。

### v0.51 — P1 算法档（GC victim 加权 + GC 触发补齐 + 读写判别修复）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 182480 B / `make clean && make tests`
260588 B，均零告警零错误；`make format-check` 通过）

#### 背景

`ftl_optimization_plan.md` 的 P1 档。开工前逐条核对源码，四条前提与现状不符（登记在下），
另挖出一个**既有真缺陷**。

#### ① P1-1 GC victim 多维加权评分

`gc_select_victim` 原为**单维贪心**（取 `valid_count` 之和最小的 SB），设计 §4.3 指出的
「磨损均衡缺失」「温度维度缺失」两条缺陷确实成立。

**本仓定案**（`cost` **取小** = 越优先回收，四项均为 0..1000 千分比整数，无浮点）：

```
cost = 500·valid_ratio + 200·erase_ratio + 200·cold_penalty + 100·youth_penalty
```

| 项 | 定义 | 方向 |
|---|---|---|
| `valid_ratio` | `valid*1000 / (present_planes*PAGES_PER_BLOCK)` | 有效页少 ⇒ 优先 |
| `erase_ratio` | `max_erase_of_sb*1000 / max_erase_all` | 磨损高 ⇒ 降权（避开临近寿命块） |
| `cold_penalty` | `HOT ? 0 : 1000` | **HOT 优先**当 victim（COLD 有效页几乎不变，下次还得搬） |
| `youth_penalty` | `1000 − (max_pe_ts − pe_min)*1000/max_pe_ts` | 越久没被擦 ⇒ 越优先 |

**与设计 §4.3 逐字公式的三处偏离**（文档公式符号自相矛盾：按「取大」读会奖励有效页**多**的块，
与同节「选有效页最少」相反；`temp_factor` 未定义；擦除项字面 `(1-erase_ratio)` 在取小约定下
会**偏向**磨损重的块）——三处均已写进 `gc.h` 的 `GC_WEIGHT_*` 块注释。

**`last_pe_ts` 不持久化**（P0-1 刻意未落盘）：上电后全表为 0 ⇒ 时间项对所有条目同值
（常量偏移，不影响排序）；开机后被 GC 擦过的块才有非零时间戳 ⇒ 「本上电周期内还没被回收过」
的块优先——正是公平性语义本身。

新增 `gc_table_extremes()`（一次全表扫描取 min/max 擦除 + max `last_pe_ts`），P1-1 的归一化分母
与 P1-2 的 T5 共用这一次扫描。

#### ② P1-2 GC 触发补齐（T4/T5 落地，**T6 废弃**）

原 `gc_check_trigger` 只有三条（水位 / 写入量 / 定时）。新增 `gc_check_triggers()` 返回**位掩码**
`GC_TRIGGER_*`，`gc_check_trigger()` 退化为 `!= 0` 薄封装（旧契约不变，调用点 `ftl.c` 改用位掩码
并打观测行 `[GC] trig=0x%02x pre=… free=… writes=…`）。

- **T4 强制**：`free_count <= FREE_POOL_EMERGENCY (8)`——终于消费掉 P1-3 清单里的死常量。
  与 T1（`free_count < FREE_POOL_TARGET 32`）**同源不同阈值**：T1 主动补池，T4 是池见底的最后手段。
  （v0.52 更正：原文写作 `pre_cond_count < 32`，该字段已随 P1-3 删除，见下。）
- **T5 磨损比**：`max_erase >= GC_WEAR_LEVEL_MIN_ERASE (64) && min_erase > 0 && max/min > 10`。
  用**除法**不用乘法（既避免 `10*min` 溢出，也逐字对应「比 > 10」）。两个 guard 的理由：
  `min_erase > 0` —— 对 0 求比值无意义，且「有块从未被擦过」是**静态 WL**（`wl_execute` 搬冷数据）
  的职责，T5 只管「所有块都在轮转、但轮得不均」；`max_erase` 地板 —— 防早期（max 才几次）
  满足比例后反复空跑。

**T6「碎片率」废弃**（原定义「平均有效页 > 70% 触发」，`GC_FRAGMENT_THRESHOLD 0.7`）：

1. **方向与标签相反**：碎片率高应对应有效页**少**；且 `gc_select_victim` 挑的是有效页**最少**的
   victim ⇒ 把它移出 `closed_sb_list` 后剩余平均值**上升** ⇒ T6 更易命中 ⇒ **自激正反馈**。
2. **收益为负**：搬一个 70% 有效的 SB 要写 ~90 页，净空间仅 `1 − 0.7 = 0.3` 个 SB，写放大 ~45×。
   行业 victim 选择定式（贪心取有效页最少；LFS cost-benefit 最大化 `(1−u)·age/(1+u)`）**一致偏向 u 小**。
3. **语义为空**：closed 块平均 70% 有效 ⇒ 盘近满 ⇒ 空闲池必然小 ⇒ **T1/T4 已命中**。
4. **主流处置方向相反**：占用率高 ⇒ 写放大 ≈ `1/(1−u)` 高 ⇒ 行业杠杆是 **OP（预留空间压低 u）**
   与 **GC 限速**（§4.6 的 `GC_BANDWIDTH_*`），不是「因为块太满所以多 GC」。

⇒ `GC_FRAGMENT_THRESHOLD` 不定义；`GC_TRIGGER_*` 位图**刻意留空 0x20**。完整论证见
`../ftl_design.md` §4.4 表下注；`gc.h` 对应位置有交叉引用。

#### ③ 顺带修复：`ftl_handle_completion` 读写判别缺陷（既有，非本批引入）

`ftl_handle_completion`（`ftl.c`）用 `pending->io.buffer != NULL` 判别读/写，但 `ftl_submit_write`
**拒绝 NULL buffer**（`ftl_io_request_t.buffer` 对写是「数据源」）⇒ 该条件对写**恒真** ⇒
写成功分支（`journal_append` / `l2p_cache_write` / `checkpoint_journal_watermark` / `total_writes++` /
VPC 簿记 / `wear_mark_dirty`）与写失败分支**都是死代码**，每次写完成反而走读分支
（`ftl_cmd_read_data` 把 NAND 数据读回源 buffer）。

**现象**：`wr` 报 `done status=0`，但 L2P/Journal 从不更新 ⇒ `rd` 未映射 ⇒ 返回全 0。
**爆炸半径**：`total_writes` 恒 0 ⇒ 连带静默关掉 checkpoint T2、`wear_flush` 的写入量节拍门、
`wl_should_trigger` 的 `WL_MIN_INTERVAL`、**P1-2 的 T2**、以及 **P1-1 的时间维度**（`last_pe_ts`
取 `total_writes` ⇒ 恒 0 ⇒ 时间项永久中性）。

**修法**：`ftl_pending_req_t` 新增显式 `op`（`ftl_pending_op_t`：`PENDING_OP_READ/WRITE`），
`pending_add` 加尾参，两处判别改按 `pending->op`；订正 `pending.h` 的文件头注释
（原文「写操作不需要 buffer」与代码相反）。

#### 前提核对：三条计划书描述已过期（登记）

- **P1-5（SB 独立块号）已完成**：`allocator_open_sb` 已逐 plane 独立选块
  （`allocator_select_plane_block`），`plane_blocks[PLANE_NUM]` 存各自块号。
  计划书「现状 block1 = block0」是过期描述 ⇒ **跳过**。
- **P1-6 前提部分过期**：Journal 已是 **4 块**（`JOURNAL_BLOCKS = CW_NUM`，块 58-61），
  非文档 §14.1 的「2 块」。剩余工作 = region 更新合并 + `journal_truncate` 尊重 `up_to_sequence`。
- **P1-3 前提含糊**：`pre_cond_count ≡ free_count` 的根因是 `gc_erase_and_free` **先擦后归还**
  ⇒ 每个空闲块本就已擦除，文档的三级池在本仓**物理上是同一批**。待定案。

#### 验证

- `make clean && make all` 182480 B（P1-1+P1-2 前 182240 B）；`make clean && make tests` 260588 B
- 零告警零错误；`make format-check` 通过
- **本次不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

#### 登记不做

- **T6 碎片率触发** —— 废弃，论证见上。
- **P1-3 三级池真三级化** —— 待定案（三池物理同批，需先定语义）。
- **P1-4 L2P cache thrashing 检测 + 降级**、**P1-6 journal 合并 + `truncate` 语义** —— 后续批次。
- **P1 各效果类判据本轮拿不到** —— 依赖 ③ 先上板验证（`total_writes` 才真正开始递增）。

### v0.52 — P1-3 Free Pool「三级水位」（原题「真三级化」的定案更正）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 182336 B / `make clean && make tests`
260452 B，均零告警零错误；`make format-check` 通过）

#### 结论：计划书的前提是错的，改的是**文档**不是**实现**

`ftl_optimization_plan.md` 的 P1-3 标题为「Pre-conditioned Pool 真三级化」，判据是
「`pre_cond_count` 与 `free_count` 解耦，前台 IO 不再等 GC」。逐条核对源码后判定**该判据不可达、
且不该达**：

1. **`pre_cond_count ≡ free_count` 不是缺陷，是正确推论**。`gc_erase_and_free`（`gc.c`）的语义是
   **先擦除、后归还**（`allocator_free_block` 的文件注释即写「本入口的契约是『调用者已擦除』」）⇒
   池内**每个**块都已擦除 ⇒ 不存在「已预擦除块」与「未擦除块」的物理区分 ⇒ **没有东西可以分区**。
   `pre_cond_count` 与 `free_count` 本就是同一个量，所谓「解耦」等于要求实现制造一个假的第二池。
2. **文档 §3.7 把「三级水位」画成了「三级池」**。设计原意（BG GC 预擦除池）在本仓的实现里
   体现为**一个已擦除池 + 三级软/硬水位**，不是三个池。
3. **主流主控正是这么做的**：一个已擦除 free pool + 软/硬水位（low watermark 触发后台 GC、
   critical watermark 阻塞/紧急 GC），另配**语义独立的保留池**（本仓 = OP 池 性能缓冲 / Spare 池
   坏块替换）。没有主流控制器在 free 池内部再切「预擦除 / 未擦除」两级——擦除落在关键路径上
   本身就是 OP 与后台擦除要消灭的东西。
4. **原判据的正确替代**：「前台 IO 不再等 GC」由**水位①**保证（池里常驻 ≥32 个已擦除块，
   `FREE_POOL_TARGET`），不由第二个池保证。

#### 定案：一个池 + 三级水位（代码侧只删不加）

| 水位 | 条件 | 语义 |
|---|---|---|
| ① 正常 | `free_count ≥ FREE_POOL_TARGET (32)` | 不动作 |
| ② 补池 | `FREE_POOL_EMERGENCY (8) < free_count < 32` | 后台 GC 主动补池（`ftl_service_idle` 循环补到水位①） |
| ③ 紧急 | `free_count ≤ FREE_POOL_EMERGENCY (8)` | 写路径同步兜底 + `GC_TRIGGER_EMERGENCY` |

**删除清单**（全部为死代码或冗余派生量，删除前逐个确认零读者）：

| 符号 | 位置 | 删除理由 |
|---|---|---|
| `pre_cond_count`（字段） | `ftl_allocator_t` | `≡ free_count`，唯一写入点是 `power_failure_recovery.c` 的直接赋值 |
| `emergency_count`（字段） | `ftl_allocator_t` | 无任何读写点 |
| `FTL_BLOCK_STATE_PRE_COND` | `bst.h` 状态位 | 只写不读（无 `ftl_block_state_is` 读者） |
| `FTL_BLOCK_STATE_EMERGENCY` | `bst.h` 状态位 | 无置位点、无读者 |
| `allocator_needs_gc()` | `allocator.c` | 零调用者，且重复了 `gc_check_triggers` 的 T1/T4 |

连带：`FTL_BLOCK_STATE_MASK` 由 `0x01FF` 收窄为 `0x007F`（bit7/bit8 释放）；
`cli.c` 状态行去掉 `pre_cond=%u`；`bst.h` 的结构体注释改为「BAD/OPEN/CLOSED/FREE/RESERVED」。

**GC 触发的真源收敛到 `ftl/gc.h`**：`allocator.h` 中原 `allocator_needs_gc` 的声明位置留一条
指向 `gc.h` 的删除注记（水位判定只此一份）。

#### 文档同步（本次改动的主体）

`../ftl_design.md` §3.7 整节重写为「Free Pool 三级水位（v0.52 更正）」，含水位图 + 三行水位表 +
「水位之外的两级保留池」表；ADR-9 由「三级池」改为「三级**水位**」；另修正约 15 处连带表述
（对比表 / 树 / 流程图 / 状态位清单 / 接口清单 / 伪码 / 文件清单）。
`../../archive/ftl_ut_munit.md` 的 allocator 套件行与不变量行、`ftl_api_usage.md` 的三处接口描述同步。

#### 验证

- `make clean && make all` 182336 B（v0.51 的 182480 B − 144 B）；`make clean && make tests` 260452 B
- 零告警零错误；`make format-check` 通过
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）；⚠ 该批改动**无介质格式变更**，无需重新擦除芯片

#### 登记不做

- **P1-4 L2P cache thrashing 检测 + 降级**、**P1-6 journal region 合并 + `journal_truncate` 尊重
  `up_to_sequence`** —— 后续批次。
- **P1 各效果类判据**仍依赖 v0.51 ③ 的读写判别修复先上板验证。

### v0.53 — P1-4 L2P cache thrashing 检测 + 降级（初稿三条降级只保留一条）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 182872 B / `make clean && make tests`
260988 B，均零告警零错误；`make format-check` 通过）

#### 前提核对

设计 §6.5 给了三条降级策略（write-through / 增大 batch 预取 / 停 background flush）。
`hit_count`/`miss_count` 确如计划书所述「只写不读」（无任何降级逻辑）——缺口成立。但三条策略
逐条对照**本仓实现**后**只有一条成立**。

#### 检测（新增）

累计 `hit_count`/`miss_count` 随开机时长渐近，反映不了**当前**行为 ⇒ 降级判定必须看**窗口**：

- `l2p_cache_s` 新增 `window_access` / `window_miss` / `miss_pct` / `thrashing`。
- `l2p_cache_note_access(cache, is_miss)`（static）在 `l2p_cache_find_or_load` 的 hit/miss 分支各调一次；
  每 `L2P_THRASH_WINDOW (256)` 次访问结算 `miss_pct`，带**滞回**：进入 `≥ L2P_THRASH_ENTER_PCT (90)`、
  退出 `< L2P_THRASH_EXIT_PCT (70)`，中间保持——避免阈值附近反复翻转。
- 公开只读访问器 `l2p_cache_is_thrashing()`（static inline）。
- 观测口：**状态翻转**时打一行 `[L2P] thrash=… miss=…% hit=… miss_total=…`（抖动不刷屏）；
  CLI 状态行 `ftl[...]` 追加 `thrash=%u miss=%u%%`。

#### 降级（只保留一条）

`ftl_service_normal` 每轮主动 flush 最多 4 条脏行的循环，**在 thrashing 时整体跳过**——脏行只由
**驱逐路径按需**写回。理由：thrashing 下这些主动 flush 大概率是白写（被 flush 的行往往在下次访问前
就被驱逐，届时还要再 flush 一次），且挤占前台 IO 的 NAND 带宽。

**持久性不受影响**（关键核对）：L2P 更新在写完成路径里**先 `journal_append` 再 `l2p_cache_write`**
⇒ 掉电可回放；`checkpoint_create` 也无条件 flush 全部脏 region（`checkpoint_flush_dirty_regions`）。
故「脏行滞留 RAM」不比原设计多任何丢失窗口。

#### 刻意不实现（初稿策略 1 / 2）

| 策略 | 不实现的原因 |
|---|---|
| **write-through** | 本仓 L2P 写模型是**整 region 页追加**（改 1 个 entry 也要写回整页 2KB）⇒ 无论 write-back 还是 write-through 都要先把 region **读进来**（RMW）再写一次，write-through **省不掉那次 load**，却放弃 write-back 的**合并收益**（同 region 多次写合并为一次 flush）⇒ 净亏。 |
| **增大 batch / 预取连续 region** | 预取依赖**空间局部性**，而 thrashing 的成因恰恰是**随机**访问（无局部性）⇒ 预取只多读一页。预取属于**顺序**场景优化，与 thrashing 正交。 |

#### 判据核对

原判据「随机写压测下不出现『evict→flush→load』活锁」：该活锁由**写路径紧急 GC**（v0.33）结构性
保证（分配不到块时同步 GC 兜底）；本轮降级是**降本**（少白写、让带宽）而非防活锁 ⇒ 该判据属上板
压力观测，**非纯 RAM UT 可判**。

#### 登记不做

- **读路径查 Journal overlay**（DRAM-less 行业解法之一：Journal 即 L2P 增量日志，thrashing 下少
  load region）——本仓 `l2p_cache_read` 不查 Journal，属独立改造。
- **L0 覆盖 thrashing 窗口**：结算+滞回是纯逻辑，但当前是 `l2p_cache.c` 的 `static` 内部函数；
  L0 要测需先把结算步导出成公开原语——属测试接线决策（实施时再定）。

#### 验证

- `make clean && make all` 182872 B（v0.52 的 182336 B + 536 B）；`make clean && make tests` 260988 B
  （v0.52 的 260452 B + 536 B）；零告警零错误；`make format-check` 通过
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）；无介质格式变更 ⇒ 无需重新擦除芯片

### v0.54 — P1-6 Journal 寿命瓶颈（环形块双倍擦除修复 + 条目合并）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 182992 B / `make clean && make tests`
261108 B，均零告警零错误；`make format-check` 通过）

#### 前提核对（两条过期，一条成立）

计划书 P1-6 的两条方向，逐条对照**本仓实现**后**两条都已不成立**：

| 计划书方向 | 核对结果 |
|---|---|
| **增 `JOURNAL_BLOCKS`（2→4/8）** | ❌ **过期**——实现早已是 4 块（`JOURNAL_BLOCKS = CW_NUM = 4`，块 58-61），设计自荐的「2→4」本就落地 ⇒ 寿命 ~1.6 TB。文档 §14.1 的「2 块 ≈ 800GB」为初稿值，本版一并更正。 |
| **region 更新合并**（同 region 多次写合并为一次 flush）| ❌ **本就固有**——region cache 把同一 region 的多次写合并进**一条脏行**，只 flush 一次；无需改动。 |

⇒ 真缺陷另在**环形块擦除路径**（见下）。

#### 真缺陷：环形块双倍擦除（P1-6 实修）

`journal_flush_page` 在写满一块回切下一块时**擦除新块**；而 `journal_truncate`
（由 `checkpoint_create` 调用）此前也擦除「环形下一块」。两者**擦的是同一个块**：

```
一圈内同一块被擦两次：
  truncate   擦 block N+1  ──┐
  flush_page 回切 block N+1 ──┴─ 又擦一次 ⇒ 双倍磨损
```

直击 §14.1 认定的 **Journal 寿命瓶颈**——Journal 磨损凭空 **+25%**（每圈多擦一块）。

**修法**：块的物理擦除**收敛到唯一一点**——`journal_flush_page` 回切进入新块时擦。

- `journal_truncate` 只做**预擦除 + 记账**：擦环形下一块，置 `pre_erased_block = next`。
- `journal_flush_page` 回切时，若 `pre_erased_block == current_block` ⇒ **跳过擦除**并清记账
  （`JOURNAL_BLOCK_NONE`），否则按原路径擦。
- 重复调用（两次 checkpoint 之间无写入）时 `next == pre_erased_block` ⇒ 跳过重擦。
- 新增哨兵 `JOURNAL_BLOCK_NONE (0xFFU)`；`l2p_journal_t` 新增字段 `uint8_t pre_erased_block`。

#### `up_to_sequence` 生效（不再是死形参）

旧 `journal_truncate` 忽略 `up_to_sequence`。新实现加**水位校验**：若
`sequence_counter == 0` 或 `(up_to_sequence + 1) < sequence_counter`（即水位**不足以**覆盖全部已写
条目），**直接返回不截断**——最旧块可能仍持有回放需要的条目，擦掉即丢数据。调用者
`checkpoint_create` 传的正是 `sequence_counter`（下一待分配序号），正常路径恒满足。

#### 条目合并（`journal_append`）

页缓冲里若已存在**同一 LBA** 的条目 ⇒ **就地更新** `new_pba`，不新增条目、**不消耗 sequence**、
**不推进 `current_entry`**。理由：回放按页序 + 页内下标应用（per-LBA last-write-wins），页内同 LBA
只保留最新 `new_pba` 即正确（`old_pba` 仅首条保留，回放不读它）。⚠ 必须**同时**不推进
`sequence_counter`——页头 `hdr_page_seq = sequence_counter − current_entry` 由二者反推，只推一个会
导致页序错乱、回放顺序错误。

#### 判据核对

- 原判据「寿命估算翻倍」：已由「4 块」满足（初稿 2 块 ⇒ 800 GB，4 块 ⇒ ~1.6 TB）。
- 原判据「写压测下 journal 水位不长期贴顶」：属**上板压力观测**，非纯 RAM UT 可判。
- ⚠ **无介质格式变更**（Journal 块位置/块数/页头/条目格式**均未变**）⇒ **无需重新擦除芯片**。

#### 验证

- `make clean && make all` 182992 B（v0.53 的 182872 B + 120 B）；`make clean && make tests`
  261108 B（v0.53 的 260988 B + 120 B）；零告警零错误；`make format-check` 通过
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

---

### v0.55 — 逐页有效位图搬出 BST 条目（位图与条目的分离）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 183368 B / `make clean && make tests`
261476 B，均零告警零错误）

#### 判据换向（本版的出发点）

用户明确：**判据换成「有出处的主流存储主控 FTL 设计」，不是本仓当前方案**——本仓
`ftl_design.md §19（档三）/§4.9.3（厂商对照表）` 是**待改对象**。故本版先核主流 BIT 设计，
再据其改本仓。已核到的**一手出处**：KIISE《Study of Effective Valid Page Tracking Methods
in Mobile Flash Storage without DRAM》(`jok.kiise.or.kr/pub-reader/1847`)，逐字：

- VPC = **每块 4 字节**，且「블록 당 4B만 사용되어 … 충분히 SRAM에서 관리할 수 있다」（小到能常驻）。
- 有效页位图 = **1 bit/4KB 页**；DRAM-less 下**留 SRAM 装不下** ⇒ **退到 flash 里管**
  （「유효 페이지 비트맵을 플래시 메모리에서 관리할 수 있지만」，代价是频繁 flash I/O）。

⇒ 主流的分界**不是「u64 还是 u8」**，而是**「位图是不是 per-block 条目里常驻的一个字段」**：
主流把**块级聚合（VPC）留常驻**、把**逐页位图当独立、可落盘、可 GC 重建的弹性项**。

⚠ **撤回**：本版讨论中一度引用的「主流把擦除次数/无效页/状态打包进 32-bit bitfield」**查不到一手出处**
（OFTL 的 20+10+2 位域布局全网不可检索），按项目规矩**不作依据**。KIISE 的一手值恰恰是
VPC = 4B（整 u32），**不是**打包字段。

#### 变更：`valid_map` 从 `ftl_block_t` 搬为独立表

| 项 | v0.48~v0.54 | v0.55 |
|---|---|---|
| BIT 条目 | `ftl_block_t` **40B**（含 `valid_map[PLANE_NUM]` 16B + `_pad` 1B） | `ftl_block_t` **24B**（无位图） |
| 位图 | 条目内字段 | **独立表** `alloc->valid_map[gblock]`（每块 `PLANE_NUM` 个 u64 = 16B） |
| 访问器 | `ftl_block_set_valid(b, plane, page)` | `ftl_block_set_valid(b, valid_map, plane, page)`（显式收本块切片） |
| 承载 | `g_blocks[4096]` 160KB | `g_blocks[4096]` 96KB + `g_valid_map[4096][2]` 64KB = 160KB |

**内存不变**（160KB）——本变更是**结构性**的：条目不再携带位图，两者可独立驻留/搬移；
`valid_count`（块级聚合）仍是条目里的**真值**，位图与它由访问器单点同步。

**顺带揭示的隐藏前提**：v0.48 记的「删死字段省 0 字节」（`ftl_design.md §4.9.6`）**只对
`valid_map = uint64_t[PLANE_NUM]` 成立**——它的 8 字节对齐把省下的 4B 又吃回 padding。
位图搬出后对齐约束消失，条目自然落到 24B（96KB）。⇒ **「对齐要求」才是那个隐藏前提**。
（死字段 `program_count`/`bad_reason`/`gc_generation` **本次未删**，仍按 v0.48 的「暂缓」保留；
**v0.56 已删**，见下一条。）

#### 改动文件

- `include/ftl/bst.h`：结构体去 `valid_map`/`_pad`；`_Static_assert` 40→**24**；四个访问器加
  位图切片形参；文件头加「位图与条目的分离」说明。
- `include/ftl/allocator.h`：`ftl_init_params_t` 与 `ftl_allocator_t` 各加
  `uint64_t (*valid_map)[PLANE_NUM]`。
- `src/main.c`：新增 `g_valid_map[CH][CE][LUN][FTL_BLOCK_TOTAL][PLANE_NUM]`（64KB → AXI_RAM）；
  AXI 预算 `_Static_assert` 计入它。
- `src/ftl/{ftl,allocator,gc,wear_leveling,bbt,power_failure_recovery}.c`：15 个访问器调用点
  + 2 个位图扫描循环（`gc.c`/`wear_leveling.c` 的 `__builtin_ctzll` 扫块）改取独立表；
  `ftl_init` 加位图表 memset + NULL 校验。
- `tests/unit/test_rebuild.c`：夹具加 `valid_map` 数组与 `params.valid_map`（结构体变更的**编译必需**
  适配，非新增测试）。
- 文档：`ftl_design.md §4.9`（§4.9.2 结构、新增 §4.9.7、§19 标注）、`ftl_optimization_plan.md`。

#### 验证

- `make clean && make all` 183368 B（v0.54 的 182992 B + 376 B）；`make clean && make tests`
  261476 B（v0.54 的 261108 B + 368 B）；零告警零错误
- AXI_RAM 332.4 KB / 384 KB（86.6%）
- ⚠ **无介质格式变更**（位图从不落盘，掉电由写指针扫描重建）⇒ **无需重新擦除芯片**
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

---

### v0.56 — 删 BST 死字段（条目 24B → 20B；Wear 条目 12B → 8B）

**日期**：2026-10-12
**性质**：结构体瘦身 + 接口收敛（**无 RAM 之外的介质格式变更**）

#### 动机

v0.55 把逐页位图搬出条目后，揭示了 v0.48 那条「删死字段省 0 字节」的**隐藏前提**——
省下的 4B 原本被 `valid_map = uint64_t[PLANE_NUM]` 的 8 字节对齐吃回 padding。
对齐约束消失后，删除**真的省字节**。用户据此拍板「删掉吧」。

#### 删除清单

| 字段 | 大小 | 依据 |
|---|---|---|
| `program_count` | 2B | 写成功++，全仓**零读者**（v0.45 曾设想做健康模型，从未落地） |
| `bad_reason` | 1B | 仅 `bbt_mark_bad` 写入，零读者；BBT 持久化本就是 bitmap，原因跨重启必丢 |
| `gc_generation` | 1B | GC 迁移成功++，零读者 |

#### 连带撤除

| 项 | 变化 |
|---|---|
| `FTL_BLOCK_BAD_*` 枚举 | 整删（6 常量，唯一用途是喂 `bad_reason`） |
| `bbt_mark_bad()` | `(alloc, gblock, reason)` → `(alloc, gblock)`；4 处调用点（`gc.c` ×1、`ftl.c` ×2、内部 ×3） |
| `ftl_block_bad_reason()` / `ftl_block_set_bad_reason()` | 整删 |
| Wear 条目 `wear_entry_t` | 去 `program_count` + `_reserved`（对齐填充随之不再需要）⇒ **12B → 8B**；`FTL_WEAR_ENTRY_BYTES` 12→8；全量快照 **25 → 17 页** |
| `wear_mark_dirty` 脏标记点 | **7 → 5**（删 `ftl.c` 写完成的两点，它们只为 `program_count` 服务） |

#### 保留未动

`OPEN` / `RESERVED` / `TEMP_HOT` / `TEMP_COLD` 状态位——它们是**状态机语义**的一部分
（非独立字段），且 `tests/unit/test_rebuild.c` 直接断言，另案处理。

#### 改动文件

- `include/ftl/bst.h`：结构体去 3 字段、`_Static_assert` 24→**20**、删 `FTL_BLOCK_BAD_*` 枚举
  与两个 `bad_reason` 访问器、文件头维护点注释同步
- `include/ftl/bbt.h` / `src/ftl/bbt.c`：`bbt_mark_bad` 去 `reason` 形参（3 处内部调用点同步）
- `src/ftl/{gc,ftl}.c`：`bbt_mark_bad` 调用点去实参；删 `gc_generation++`、
  `program_count++` ×2 及其 `wear_mark_dirty` ×2
- `include/ftl/wear.h` / `src/ftl/wear.c` / `include/ftl/ftl_config.h`：条目去两槽、常量 12→8
- 文档：`../ftl_design.md`（§4.9.2/§4.9.3/§4.9.4/§4.9.6、新增 **§4.9.8**、§19 全节）、
  `ftl_optimization_plan.md`、`cw_level_mapping_design.md`、`ftl_api_usage.md`、`../../archive/ftl_ut_munit.md`

#### 验证

- `make clean && make all` **182992 B**（v0.55 的 183368 B − 376）；`make clean && make tests`
  **261100 B**（v0.55 的 261476 B − 376）；零告警零错误；`make format-check` 通过
- AXI_RAM **316.4 KB** / 384 KB（82.4%；v0.55 为 332.4 KB）——**条目 96KB → 80KB（−16KB）**，
  位图表 64KB 不变
- ⚠ **Wear 表条目格式变更**（12B → 8B）。P0-1 尚未上板 ⇒ 芯片上无旧格式数据；
  且上板前本就须擦除芯片（P0-2 布局维度无关化）⇒ **无需额外处理**
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

---

### v0.57 — P2-2 结案（SB 索引粒度：前提被 Policy B 作废）

**性质**：**纯文档决策**，无代码改动（结论是「不实现」）。

#### 动机

`ftl_optimization_plan.md` §P2-2 登记着「按 SB 索引可再省 96KB」。复核发现该推导的
**前提已被删除**，且删除后**没有任何可省的空间**。

#### 依据

**① 前提已死（Policy B）。** 原推导的前提是 `allocator.c` 的 `block1 = block0`：SB 两 plane
永远配对**同号物理块** ⇒ 两 plane 的 `erase_count`/`state`/`sb_id`/`valid_count` 天然相同 ⇒
per-block 存是重复的。**v0.12 Policy B**（`cc09580`+`1927762`）已把前提删掉——`allocator_open_sb`
（`allocator.c`）**逐 plane 独立**调 `allocator_select_plane_block(alloc, plane)`。于是
`erase_count`/`last_pe_ts`/`read_count`/`valid_count`/`state`/`sb_id`/`rr` **各自属于不同物理块，
无一重复** ⇒ 没有东西可去重。

**② 数字双重过期。** 备注写「再省 96KB」；当前条目 20B（v0.56）× 4096 = **80KB**，且按 SB 索引
（2048 条）只是 80KB → 40KB——省下的是**分辨率**而非重复。

**③ 主流方向相反（两级结构）。** SPDK FTL（<https://spdk.io/doc/ftl.html>，本仓一手出处；索引见 `docs/reference/README.md` §A4）
是**两级**：band 级记 sequence ID / validity / write pointer / OPEN-FREE-CLOSE，底层仍是
**per-block BIT**（"Valid map - bitmask of all the valid physical addresses"）。本仓
`super_block_s`（SB 级）+ `ftl_block_t`（block 级）**逐项对应**，已对齐。改成按 SB 索引
= **删掉 block 级 BIT**，是往主流反方向走。

**④ 「删 `sb_id` 省 8KB」一并撤回。** 删掉 2B 的 `sb_id` 后 `ftl_block_t` **仍是 20B**
（4+4+2+2+2+3 = 17B → u32 对齐 20B）⇒ **省 0 字节**。这与 v0.48「删死字段省 0 字节」是**同一个
对齐陷阱**（设计 §4.9.6）。且主流 DRAM 控制器**有** block→band 反查表。要真省字节只能砍
**活**字段（`rr` 3B / `last_pe_ts` 4B），两者都有消费者（慢死预测 / GC youth penalty）⇒ 不砍。

#### 连带

- 新增登记 **P2-4**（`ftl_optimization_plan.md`）：band 级 **sequence ID + validity 聚合** 是本仓
  对 SPDK 的**唯一剩余偏离**。**不实现**的理由：SPDK 的 sequence ID 唯一消费者是「按 band 序号
  重放重建 L2P」，而本仓恢复走 **Journal/Checkpoint 的 `page_seq` 排序**
  （`power_failure_recovery.c`，粒度是日志页、比 band 级更精确）⇒ 消费者被取代，加了即死字段
  （v0.56 教训）。GC victim 侧 SPDK 用 **validity ratio**（"The lower the ratio, the higher the
  chance the band will be chosen"），本仓 `GC_WEIGHT_VALID` 已对齐。
- `../ftl_design.md` §4.9.5 选项 C 的否决理由中，「两 plane 同号 ⇒ 可去重」一条已失效，剩下
  「擦除/坏块/ECC 是物理块级事实」——**更硬**且独立成立。

#### 改动文件

- `docs/design/ftl_optimization_plan.md`：§P2-2 重写为结案 + 新增 §P2-4
- `docs/design/revision_history/ftl_revision_history.md`：v0.48 备注加结案指针 + 本条目
- `docs/design/ftl_design.md`：§4.9.5 选项 C 加订正注

#### 验证

- **无代码改动** ⇒ 无 `.bin` 变化、无介质变更、不新增测试源
- 结论以文档内可核对的事实为准（`allocator.c`、`bst.h` 的 `_Static_assert`）

---

### v0.58 — FTL 对外接口对齐主流（plane 策略下沉 + 组完成语义）

**日期**：2026-10-13

**性质**：**接口重构 + 文档**（策略下沉，行为变更：大请求从「失败」变「成功」）。设计依据见
新增文档 `docs/design/ftl_host_interface.md`（含 D1–D6 差异表与接口级 UT 设计）。

#### 动机

学**主流存储主控的 FTL 对外接口设计**（本仓北极星：`../fc_nand_design.md` §26 行业对照，非 Linux）。
问题：「FTL 对外接口是什么、外部传什么给 FTL、当前架构有何差异、如何设计 UT」。

#### 依据（判据 = 本仓 §26.1/§26.2）

**① 「访问哪些物理单元」是 FTL 的事。** §26.1 原文：设备侧固件拿到的是一条**已解析的请求**
（目标 LBA/地址 + 数据缓冲指针 + 操作类型），**访问哪些物理单元、数据怎么搬都是固件的事**。
**multi-plane 配对正是「访问哪些物理单元」** ⇒ 属 FTL，不属 HIL。而 v0.58 前，配对逻辑在
`controller/request.c` 的 `ssd_split_use_multi_plane`（HIL 侧）——**位置错**。

**② 主流传的是「长度」不是「plane 数」。** NVMe 的 NLB、eMMC CMDQ 的块数都是**长度**；FTL 自己
决定怎么拆。本仓旧 `ftl_io_request_t.pages`（1/2）把 FTL 的内部策略泄漏给了上层。

#### 关键决策

| # | 项 | 主流 | v0.58 前（本仓） | v0.58 处置 |
|---|---|---|---|---|
| **D1** | plane 配对 | FTL 内部策略 | HIL 的 `ssd_split_use_multi_plane` 决定 `pages=2` | **下沉到 FTL**（`ftl_issue_subop` 按 `count` 自拆） |
| **D2** | FLUSH | FTL 的一条 op | HIL 直接调 FTL 内部 `l2p_cache_flush_all`/`journal_flush_page` | **新增 `ftl_flush(ftl)`** |
| **D3** | TRIM 粒度 | 带长度的 deallocate | `ftl_trim(ftl,lba)` 单 LBA（忽略 `block_count`） | **改 `ftl_trim(ftl,lba,count)`** |
| **D4** | 请求 vs 通知 | 请求纯输入；完成走 CQ | 通知字段与请求混在一起 | **不改**（本仓无 CQ，轮询+回调是既定异步契约） |
| **D5** | Host 队列+doorbell+CQ | HIL 有 | HIL 无队列；队列机制只在 FC 内部 | **不改**（§26.2 已定位在 FC；HIL 侧属更大重构） |
| **D6** | LBA→实例路由 | 单一 FTL 拥有全 LBA | Controller 按 LBA 路由到 `ftl[ch][ce][lun]` 分片 | **保留**（分片是刻意模型；路由=后端选择，非 plane 策略） |

**③ 组完成语义（一次 submit = 一个组）。** 长度任意后，一次 `ftl_submit_*` 内部含多个 FC 子操作，
通知字段（`complete_flag`/`status_out`/`on_complete`）必须**只在全部子操作完成后触发一次**。
新增 `ftl_io_group_t`（`ftl->groups[FTL_GROUP_MAX]`，`FTL_GROUP_MAX = FTL_PENDING_MAX = 32`）承载
聚合；`ftl_pending_req_t` **不再内嵌 `ftl_io_request_t io`**，改显式子操作字段 + `group`/`planes`
（否则 `count`（host run 长）与 `planes`（子操作 plane 数）会共用一个字段、产生静默二义）。

**④ 三态返回是枢纽。** `ftl_issue_subop` 返回 `FTL_OK`（已下发/同步完成）/ `FTL_AGAIN`（瞬态背压：
命令/desc/buffer 池满、SQ 满、pending 表满）/ `FTL_ERROR`（永久）。旧实现把三类**全当致命**，
正是「大请求在第 N 个子操作崩」的根因。背压排队由 `ftl_service` **Phase 2.5** 补发
（完成先腾出 FC 资源 → 再补 host 子操作 → 后台吃剩下的）；连续无进展超
`FTL_DRAIN_STALL_MAX`（8 轮）才判卡死。

#### 顺带修掉的两个真缺陷

- **① 大请求提前完成**：旧 `ssd_request_split` 同步循环提交，`ftl_submit_*` 中途失败时置
  `complete_flag=1`，而 `sub_remaining` 已被预置为全量 ⇒ **父请求提前完成、余下计数永不归零**。
  真实容量瓶颈是 **FC buffer 池**（128 buffer ÷ 每多 plane 命令 8 buffer = **16** 并发子操作），
  `wr 0 40` 旧实现必失败。v0.58 后 FTL 自拆分 + 按需 drain，瞬态背压排队重试。
- **② CLI 谎报缓冲容量**：`cli.c` 把 `buffer_size` 设为 `n × FTL_READ_USER_SIZE`，而 `g_wr_buf`
  实为 16 KB ⇒ `wr 0 40` 会**写爆 16 KB 数组**（内存损坏）。改为上报真实 `sizeof(g_wr_buf)`，
  由 Controller 的容量校验拒绝（`block_count × FTL_IU_BYTES(512) > buffer_size` ⇒ `INVALID_ARGUMENT`）。
  （⚠ 此处原写 `× 2048`：CW 级迁移（v0.59）后步进单位由页 2048B 改为 IU 512B。）

#### 连带（登记，本轮不做）

- **D4/D5**：HIL 侧 host 队列 + doorbell + CQ（更大重构）。
- **D6**：LBA→实例路由上移到单一 FTL 前端（与本仓分片模型冲突，需单独立项）。
- `main.c` 目前只对 `g_ftl[0][0][0]` 调 `ftl_service` ⇒ Phase 2.5 只 drain 实例 0。当前几何
  （CH=CE=LUN=1）正确；维度扩展时须遍历全部实例（**既有结构缺口**，本步使其显形）。
- `ftl_trim` 大范围同步阻塞（超循环长停）——本轮只加 API，范围上界/分块后续定。
- `ftl_flush` 语义 = 「落已持久化映射」，**非** NVMe 式 barrier（不等待在飞/排队写）。
- 跨组 LBA 重叠时组间写序未定义（当前 HIL 串行，不成问题）。
- **保留未改**：`ftl_handle_completion` 读失败分支**无条件**报 `IO_ERROR`——即使 read recovery
  实际救回数据（S1 前既有行为，按「行为等价」原则原样保留 + ⚠ 注释，待单独立项）。

#### 改动文件

- `include/common/ftl_types.h`：`pages` → `count`
- `include/ftl/ftl_type.h`：新增 `FTL_AGAIN`
- `include/ftl/pending.h` / `src/ftl/pending.c`：`ftl_io_group_t` + 组管理；pending 改显式子操作字段
  + `group`/`planes`；删 `pending_lookup_by_lba`（零调用者死代码）
- `include/ftl/ftl.h`：`ftl_flush` / `ftl_trim(count)` / `FTL_DRAIN_STALL_MAX` / `groups[]` 进 context
- `src/ftl/ftl.c`：`ftl_issue_subop`（三态）/ `ftl_drain_group` / Phase 2.5 / 组完成；IDLE 判据改用未决数
- `src/ftl/ftl_fc_interface.c`：`ftl_cmd_setup_ppa_pair` desc 配置失败区分 `FTL_AGAIN`/`FTL_ERROR`
- `src/controller/request.c`：删 plane 配对，改按实例边界切 run 提交
- `src/controller/controller.c`：统一提交路径；FLUSH 走 `ftl_flush`；去 l2p/journal 直依赖；buffer_size 校验
- `src/controller/cli.c`：`buffer_size` 上报真实容量
- `docs/design/ftl_host_interface.md`（新增）/ `ftl_api_usage.md` / `../../archive/ftl_ut_munit.md`（§8.6 L1 接口套件）

#### 验证

- `make clean && make all` / `make clean && make tests`：**零错误零新增警告**（`184304 B` / `262420 B`）
- **等价性判据**：单实例、`count ≤ 2` 路径下提交的 FC 命令序列应与 v0.57 逐命令一致
- 上板（UART CLI）：`wr 0 5 / rd 0 5 c`（走 2+2+1 自拆）、`wr 0 2 / rd 0 2 c`、`tr 0 / fl`
- ⚠ CLI 缓冲仅 16 KB ⇒ `n ≤ 32`（CW 级迁移后步进 512B，`UART_CLI_MAX_SECTORS = 16KB/512`）；
  验证更大请求需先扩 `g_wr_buf`。（v0.58 当时为页级 2048B ⇒ `n ≤ 8`。）

---

### v0.59 — CW 级映射迁移（IU = 512 B；`ftl_optimization_plan.md §九` 波 2 序 2）

**日期**：2026-10-13

**性质**：**数据路径由页级改为 IU 级**——映射粒度从「页 2048 B」改到「CW 512 B」
（`FTL_IU_BYTES = FC_BUFFER_USER_SIZE = 512`，1 LBA = 1 CW = page/4）。**介质格式变更**。

> 本文档此前 v0.58 → v0.60 断档，本条目补齐（v0.60/v0.61 多处以此迁移为前提）。
> 完整设计见 `cw_level_mapping_design.md`（§八 有效位粒度、§十一 落地状态表）。

#### 动机

判据 = **有出处的主流存储主控 FTL 设计**（本仓北极星）。主流主控的 **indirection unit（IU）**
普遍小于物理页（SPDK FTL 的 IU 可配、Cosmos+ 亦然），IU = 映射粒度 = 有效位粒度。本仓此前
「页级映射 + 页级 2048 B/LBA」与主流不同构，且 **L2P 表声明空间（`FTL_LBA_MAX`）超容量 4×**
（伪缺陷的根因是步进仍按页）。

#### 改动（代码）

| 层 | 改动 | 文件 |
|---|---|---|
| 地址辅助 | `ftl_lba_cw/plane/page/pair_base`、`pba_page`、`ftl_cw_span_mask` | `include/ftl/ftl_config.h` §八（现 §三，见 v0.72） |
| 请求层 | 缓冲步进 = `FTL_IU_BYTES`（512 B/扇区）；`io.count` = 扇区数 | `src/controller/request.c`、`controller.c` |
| 写路径 | 1 子操作 → 逐 CW 一条 L2P entry + 一条 journal；`allocator_alloc_pair` 成对取页 | `src/ftl/ftl.c` |
| 读路径 | 逐 LBA 查 L2P，按物理 plane 分组；pair 窗口内至多两 plane 段 | `src/ftl/ftl.c` |
| GC / WL | 逐页索引成对分配目标页；逐 CW L2P 反查判有效；`ftl_sync_write_page_cw` 只编有效 CW | `src/ftl/gc.c`、`src/ftl/wear_leveling.c` |
| BST | `valid_count` 改 per-CW（0..512） | `include/ftl/bst.h` |
| 掉电重建 | `close_full_block` 逐 CW L2P 反查回填；`valid_pages`→`valid_cw` | `src/ftl/power_failure_recovery.c` |
| 页基址反推 | `gc_read_page_with_lba` 由**首个写过的 CW** 反推页基址（容忍页内空洞） | `src/ftl/gc.c` |
| CLI | `g_wr_buf` 16 KB ⇒ `wr/rd n ≤ 32` 扇区（旧页级上限 8） | `src/controller/cli.c` |

#### 关键决策

- **不设页缓冲**（`cw_level_mapping_design.md §七` 方案 A）：FC 层有 CW 位图能力（FC v27），
  512 B 写**直接编 CW 子集**，**页内允许空洞**（CW0 可能未写）。
- **不做 per-CW 位图**：`valid_count` 改 per-CW 但内存未增（v0.60 索性切 approach ③，见下）。
- **GC/WL 目标页必须成对同 plane**；**修真 bug**：页基址由首个写过 CW 反推（CW0 可能是空洞）。

#### 代价 / 登记

- **单 plane 子操作浪费另一 plane 同页** ⇒ 登记 `ftl_optimization_plan.md §8.1` **P2-6**
  （修法 = per-plane 独立写指针）。
- **上板前须擦芯片**（介质格式变更）——后被 **v0.61 的开机自动格式化**取代。

#### 验证

`make clean && make all` 0 warning；**未上板**。声明地址空间回到 `1,048,576 × 512 B = 512 MiB`。

---

### v0.60 — 有效性切 DRAM-less approach ③（删常驻位图）

**日期**：2026-10-13

**性质**：**结构删除 + 判据换源**（无介质格式变更；行为等价，仅少一次「跳过未写页」的优化）。
前置 = v0.59/v0.58 的 CW 级映射迁移（波 2 序 2）已落地。

#### 动机

用户决策（2026-10-13）：**有效位（validity）按 DRAM-less approach ③ = flash reverse mapping 实现**
——**读每个 CW 的 META 取逻辑地址**（该 META 即本仓的「OOB」），再**反查 L2P** 判该 CW 是否仍有效。

判据 = **有出处的主流存储主控 FTL 设计**（本仓北极星，见 `cw_level_mapping_design.md §八`）：

- **DFTL** — OOB 存 LBA；GC 读 OOB → 查 L2P。
- **arXiv 1504.01666**（*Garbage Collection for Flash-Resident Page-Mapping FTLs*）— 活页识别三法：
  ① 页映射扫描 ② PVB 逐页位图 ③ **flash reverse-mapping**。DRAM-less 选 **③**（② 的 RAM ∝ 页数：
  Micron P420m 要 **16 MB**）。
- **KIISE**（*Mobile Flash Storage without DRAM*）— 逐页位图放 flash 后，失效时清旧物理页的位
  会引发大量 flash I/O（旧页散落在任意块里）⇒ 主流**不做**常驻逐页位图。
- **LeaFTL / VPFTL** — P2L 存 OOB/spare，GC 读回并与 L2P **交叉验证**（匹配 = 活）。
- **SPDK FTL** — `valid_map` 按**物理地址**索引 = **有 DRAM** 的形态（1 bit / 映射单元）；
  其索引粒度佐证「有效位粒度 = 映射粒度」，但**驻留**前提是有 DRAM，本仓不具备。

#### 现状核查（改动前）

本仓有效性**已经**是 approach ③：`gc_read_page_with_lba` 读每个 CW 的 META（校验低位 plane/cw
自洽 + `0xFFFFFFFF` 哨兵）；`gc_migrate_sb` / `wl_relocate` / `close_full_block` **逐 CW 反查 L2P**
（`l2p_cache_read(l) == pba_with_cw(src, cw)`）判有效。META 就是本仓的 OOB 反向映射
（`[META:4][CRC:4][USER:512][PARITY:8]`，`include/fc/fc.h`）。

**唯一非 DRAM-less 的残留 = 常驻 64 KB 页级「写过」位图** `g_valid_map`：语义只是「本页被编程过」
（**单调、非真值**），**唯一用途** = 在 `gc_migrate_sb` 与 `wl_relocate` 跳过从未写过的页的那次 NAND 读。

#### 关键决策

| # | 项 | 决策 |
|---|---|---|
| **A** | 常驻位图 | **删**（`g_valid_map` 64KB、`ftl_init_params_t.valid_map`、`ftl_allocator_t.valid_map`） |
| **B** | 「写过」访问器 | **删** `ftl_block_mark_written` / `ftl_block_is_written`（含 5 处写调用点 + 2 处读调用点） |
| **C** | `ftl_block_clear_valid` | 去位图形参 ⇒ 单参（`gc.c` 擦除、`bbt.c` 标坏块两处同步） |
| **D** | GC/WL 跳过未写页 | 改为**快路径**：`gc_migrate_sb` 入口判「双 plane `valid_count` 之和 == 0」⇒ 直接返回（调用方仍会 `gc_erase_and_free`，语义不变）。这正是位图原先覆盖的主场景——**P2-6 整块空洞 plane**（2048B 写只落 plane0） |
| **E** | 逐 CW LBA 来源 | `gc_read_page_with_lba` 签名改为**输出逐 CW 的扇区 LBA**（`lba_t cw_lba[CW_NUM]`，未写过 = `FTL_LBA_INVALID`）⇒ GC/WL/掉电重建**直接用每个 CW 自己的 META**，不再做 `base + cw` 推断（消除对「同页 CW 的 META 恒为 B+c」不变量的隐式依赖） |

#### 代价（诚实登记）

- **散点空洞**（同块内既满页写又 512B 写）的页仍会多读一次 NAND；**整块空洞**由 D 的快路径覆盖。
  登记于 `ftl_optimization_plan.md §8.1 P2-6`。
- 收益：AXI_RAM **319 → 255.0 KB**（余 ~129 KB，为波 2 序 3/4 表所有权 / 多实例化腾空间）；
  同时消除「位图提示 vs `valid_count` 计数」两者漂移的隐患（TRIM 只减计数不清位、裸 NAND 扫描只置位不计数）。

#### 改动文件

- `include/ftl/bst.h`（删两访问器 + `clear_valid` 去参 + 文件头/结构注释改口径）
- `include/ftl/allocator.h` / `src/ftl/allocator.c`（删 `valid_map` 字段与赋值）
- `src/main.c`（删 `g_valid_map` + `_Static_assert` 项 + `params.valid_map`）
- `src/ftl/ftl.c`（删 `mark_written` 调用、`valid_map` NULL 检查与 `memset`）
- `src/ftl/gc.c` / `include/ftl/gc.h`（签名改逐 CW 输出 + 快路径 + 去 is_written 守卫）
- `src/ftl/wear_leveling.c`（同上）
- `src/ftl/bbt.c`（`clear_valid` 单参）
- `src/ftl/power_failure_recovery.c` / `include/ftl/power_failure_recovery.h`（去 `mark_written`，逐 CW LBA 回填）
- `docs/design/cw_level_mapping_design.md §八/§十一/§十二.2`、`ftl_optimization_plan.md §8.1/§8.2/§九 序 2.5`、`ftl_design.md §2.3/§4.9.*/§19`

#### 验证

- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%）
- 静态：`grep -rn "valid_map\|mark_written\|is_written" src/ include/` 零命中
- ⚠ `tests/unit/test_rebuild.c` 绑定了 `params.valid_map` ⇒ **删字段后该文件不再编译**
  （测试源文件由用户自建，本变更不代改，需用户更新后 `make tests` 方可运行）
- 上板：**先擦除芯片**（工作树含 CW 迁移的介质格式变更）→ 烧 `debug.bin` → UART `test` 全绿

---

### v0.61 — 介质布局版本标记 + 开机自动格式化（顺带修 Anchor 页 CRC 覆盖面缺陷）

**日期**：2026-10-13

**性质**：**新增持久化字段（介质格式变更）+ 真缺陷修复**。本改动本身即改介质格式 ⇒
首次上板必然触发一次自动格式化（符合预期，见下）。

#### 动机

CW 级映射迁移（波 2 序 2）与 v0.50~v0.60 的一连串元数据改造，**多次改变了 NAND 上的持久化
格式**（L2P 块位置、Journal 块数、Wear 块、Anchor 分页、CW 级映射）。固件**没有任何机制识别
"介质是旧格式"**：直接烧板会在旧数据上启动，而 **Free Pool 里全是未擦的旧块**——allocator
**不做擦除**（"free pool 保证已擦除" 是隐式前提）⇒ 写下去 = 新数据 AND 旧数据 = **静默损坏**。
用户每次上板都得先手动 `bbt --erase` 再断电重启。本版把这个手动步骤变成固件行为。

**用户决策（2026-10-13）**：
1. 做法 = **加版本标记 + 自动格式化**；
2. 版本标记放 **Checkpoint 头 + Anchor 页都加**（两处独立校验）；
3. 顺带查出的 **Anchor 页 CRC 覆盖面缺陷** = **本次一并修**；
4. 自动格式化**保留出厂坏块标记**。

#### 顺带修复的真缺陷：Anchor 页 CRC 覆盖面

`src/ftl/metadata.c` 的 `ANCHOR_PAGE_CRC_LEN` 原为
`sizeof(metadata_anchor_page_t) - sizeof(uint32_t)` = **2044**，但 `crc` 字段实际在
**offset 1808**（其后还有 236 B padding）⇒ CRC 覆盖面把 `crc` **自己**也算进去 ⇒
写入时（crc=0）算出的值与读回时（crc≠0）算出的值**必然不等** ⇒ **`anchor_page_valid`
恒返回 0**。

**证据**（Python ctypes 复现该结构体布局 + `zlib.crc32`）：
`sizeof = 2048`、`crc.offset = 1808`；`crc32(buf[:2044])` 写入后自校验 **False**。

⇒ `metadata_dir_load` 永远失败 ⇒ 每次开机都走 `metadata_dir_init`（全哨兵 Directory）⇒
**L2P 映射跨重启全丢**。这正是 `ftl_optimization_plan.md §8.2 P0-2 判据③`「复位后数据仍在
（`[META] anchor_seq` 非 0、`dyn` 非 0）——**当前必然失败**」与 `../../archive/ftl_ut_munit.md`「`anchor_seq=0`…
根因未闭合」的根因。缺陷在 `c5cb8a0` 引入：旧实现用显式 `offsetof` 常量
`METADATA_DIR_CRC_OFFSET`（=908，对的），重写时改成了 `sizeof - 4` 而字段顺序已变。

**修法**：改为 `((size_t)offsetof(metadata_anchor_page_t, crc))`（=1812），与
`checkpoint.h` 的 `CHECKPOINT_CRC_LEN` 同一写法（padding 不参与校验）。
（⚠ 该修法在 **v0.62 被取代**：记录内 CRC 连同 `CHECKPOINT_CRC_LEN` / `ANCHOR_PAGE_CRC_LEN`
一并删除——`anchor_page_valid` 改由下层 FC 的 per-CW CRC + ECC 保证，见 v0.62 ADR。）

#### 关键决策

| # | 项 | 决策 |
|---|---|---|
| **A** | 版本常量 | 新增 `FTL_MEDIA_LAYOUT_VERSION`（`ftl_config.h`）；**任何改介质格式的改动都必须 +1** |
| **B** | 标记位置 | `l2p_checkpoint_header_t` 插 `layout_version`（padding `2048-70*4`→`2048-71*4`，`CHECKPOINT_CRC_LEN` 276→280 自动）；`metadata_anchor_page_t` 插 `layout_version`（padding 236→232）。两者 `sizeof` 仍 2048 |
| **C** | 写侧 | `checkpoint_create` / `metadata_dir_flush_to_anchor` 各写当前版本（**在算 CRC 之前**） |
| **D** | 校验侧 | `checkpoint_header_valid` / `anchor_page_valid` 各加 `layout_version != 当前 ⇒ 无效`（旧格式记录字段处是旧 padding/旧 entries 字节，恒 ≠ 1） |
| **E** | 格式化触发判据 | **唯一**：本实例 `checkpoint_find_latest(&cp, topo) != FTL_OK` ⇒ 需格式化。因版本校验已含在 `checkpoint_header_valid` 内 ⇒ 该判据自动涵盖「有当前版本（否）/ 版本不符（是）/ 完全没有（是）」三种情况，且**自终止**（格式化后写入当前版本标记 ⇒ 下次开机判据 ① 命中） |
| **F** | 钩子位置 | `main.c ssd_ftl_boot()` 内、**`ftl_init()` 之后、free-pool 回填之前**。放 `ftl_init` 之后的理由：`ftl_init → bbt_init` 已**先把出厂坏块标记读进 BST**（`bbt_scan_factory` 读每块第 0 页列 2048）⇒ 之后再擦全片**不丢坏块信息**（满足用户「保留坏块标记」） |
| **G** | 擦除原语 | 新增 `bbt_erase_all_blocks(alloc)`：`plane × BLOCKS_PER_PLANE` 双循环，复用 `bbt_probe_block_bad(..., 1)`（擦前读出厂标记、擦除失败判坏）。**不走** `bbt_print_bad_map`（那是 CLI 打印用，会刷屏） |
| **H** | 标记写手 | 新增 `checkpoint_write_initial(topo)`：空 header（sequence=1、当前版本、journal_sequence=0）经 `metadata_ring_prepare_write` 取槽（自动擦目标块）后写入 |

**开机日志**（单行 `LOGW`）：`[FTL ] media layout mismatch -> format (erase all blocks)`

#### 代价 / 取舍（诚实登记）

- **出厂空片首启会多擦一遍全片**（约 4096 块 × ~3ms ≈ 十余秒，一次性）。换来的是判据无需
  "介质是否为空" 这种启发式判断。格式化后写标记 ⇒ 只发生一次。
- **BST 的 `erase_count` 仍是旧介质读来的值**（`wear_init` 在格式化前已加载）——格式化后不重零。
  仅影响磨损均衡起点，无害；本次不修（重零会连带清掉 `bbt_init` 标好的坏位，得不偿失）。
- **格式化后 Directory 复位为哨兵层**：旧 L2P 位置全丢——这正是 "格式化" 的语义。
- **保留** `bbt --erase` CLI 作为手动兜底；**不**引入 "介质是否为空" 启发式。

#### 改动文件

- `include/ftl/ftl_config.h`（`FTL_MEDIA_LAYOUT_VERSION` + 文件头「须先擦除」改「已自动兜底」）
- `include/ftl/checkpoint.h` / `src/ftl/checkpoint.c`（字段 + 写侧 + 校验侧 + `checkpoint_write_initial`）
- `include/ftl/metadata.h` / `src/ftl/metadata.c`（字段 + 写侧 + 校验侧 + **CRC 覆盖面修复**）
- `include/ftl/bbt.h` / `src/ftl/bbt.c`（`bbt_erase_all_blocks`）
- `include/ftl/power_failure_recovery.h` / `src/ftl/power_failure_recovery.c`（`ftl_media_needs_format` / `ftl_media_format`）
- `src/main.c`（`ssd_ftl_boot` 内钩子）
- `docs/design/ftl_design.md §3.3`、`ftl_optimization_plan.md §8.2`

#### 验证

- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%，本改动不增 RAM）
- `make clean && make tests`：**零告警**
- 静态：`grep -n "layout_version" include/ src/` 覆盖两结构体 + 两写侧 + 两校验侧
- 上板（测试一律上板）：
  - 烧 `debug.bin` 后**首次**开机打印 `media layout mismatch -> format`，随后正常 boot；
    断电重启 → **不再**打印格式化
  - **不再需要**手动 `bbt --erase`（本改动目的）
  - 写数据 → 断电重启 → 数据仍在（同时验证 **Anchor CRC 修复**：`[META] anchor_seq` 非 0、`dyn` 非 0）
  - `wr`/`rd`/`tr`/`fl`/`info` 冒烟；一轮 GC 后无 `IO_ERROR`

---

### v0.62 — 元数据校验和下沉：删 FTL 记录内 CRC，改由下层 FC per-CW CRC + ECC 承担

**日期**：2026-10-13

#### 背景

长期遗留的一处不一致：FTL 给自己的每一条元数据 record（Checkpoint / Anchor / BBT / Wear）
自算整记录 CRC，但**同时又**把元数据页的 FC 层 CRC **关闭**（`FTL_PAGE_CFG_META` = `FC_CRC_DISABLE`），
只剩 BCH ECC。于是「FTL 自算 CRC」与「下层已有 CRC 通道却被关掉」两件事并存——既重复又矛盾。

要判定该保留哪一种，需要**出处**（项目北极星：判据 = 有出处的主流存储主控设计，非 Linux）。

#### 出处调研（2026-10-13，逐源码核对）

| 出处 | 类型 | 元数据校验做法 |
|---|---|---|
| SPDK FTL（`lib/ftl/`） | **DRAM 型** | **自算 CRC32C**：superblock `crc` 字段；`ftl_band_md.p2l_map_checksum` / `ftl_nv_cache_chunk_md.p2l_map_checksum`（注释 "CRC32 checksum of the associated P2L map"）；`ftl_p2l.c` 用 `spdk_crc32c_update` 写校验、恢复时比对 |
| Cosmos+ OpenSSD GreedyFTL | **DRAM 型** | **只靠 ECC**：9 个 `.c` + `ftl_config.h` 全查零 crc/checksum（唯一 `Crc` = 硬件 ECC 引擎的 `V2FCrcValid`） |
| **dhara**（`dlbeer/dhara`，低内存嵌入式 NAND FTL） | **DRAM-less** | **校验和下沉到 NAND 层**：`journal.c`/`map.c` **零** crc/checksum；接口契约 `prog: program a NAND page, including ECC and **checksums**`、`nand.h`: `ECC must be handled by the NAND implementation`。dhara 明确：`Uncorrectable errors **must** be detected` / `may require the use of a checksum **in addition to ECC**` / `ensure that the checksum bytes are **also protected by ECC**!` |

**结论**：「FTL 有无自己的 CRC」不是行业定论，而是**按 DRAM 型 / DRAM-less 分岔**：

- DRAM 型（SPDK / Cosmos+）元数据是独立页、不假设底层兜底 ⇒ 自加 CRC（SPDK）或干脆只靠 ECC（Cosmos+）。
- **DRAM-less（本仓所属）** 元数据与用户数据走同一条 NAND 通道、下层就是自带 CRC+ECC 的 FC
  ⇒ 主流做法是**校验和下沉到下层**，FTL 只留结构判据（dhara 模型）。

本仓是 DRAM-less（`../ftl_design.md` 标题即 "设计 DRAM-less L2P/CLOCK/Journal/Allocator"），
故判据取 **dhara 模型**。

#### 三个候选与裁定

| 方案 | 内容 | 裁定 |
|---|---|---|
| **A** | 保留 FTL 整记录 CRC | SPDK 模型（DRAM 型）——与本仓 DRAM-less 定位不符 ❌ |
| **B** | 删 FTL CRC，只靠 ECC | **dhara 明确反对**（不可纠错误必须被检出，单靠 ECC 不够）❌ |
| **C** | 删 FTL 记录 CRC + 下层 FC 出 per-CW CRC + ECC | **dhara 模型 ✅ = 本版采纳** |

⚠ 关键事实核对：**本仓 v0.62 之前其实落在 B 不是 C**——`FTL_PAGE_CFG_META` = `FC_CRC_DISABLE`
⇒ 元数据页连 FC CRC 都没有、只剩 ECC。走 C 必须把该开关改成 `FC_CRC_ENABLE`。

#### 技术前提核对（均已读码确认）

- FC 的 ECC 码字 = `FC_BUFFER_DATA_SIZE`（520B = META 4 + CRC 4 + USER 512）
  ⇒ **CRC 字段本身在 ECC 覆盖内**（满足 dhara 要求的 "checksum bytes also protected by ECC"）。
- `ftl_sync_read_page` 的成败判据 = `(completion.status & FC_CW_STATUS_ERROR_MASK) != 0`，
  该掩码**已含 `FC_CW_STATUS_CRC_ERROR`** ⇒ 翻开关后读路径**零改动**即生效。
- `ftl_cmd_write_data` 对不满页部分 `memset 0xFF` 补齐、并写满 4 个 CW ⇒ CRC 覆盖整页。
- 环池探针（`checkpoint_record_probe` / `anchor_record_probe` / `bbt_record_probe` / wear replay）
  读失败即视为「该 record 无效」⇒ 与「CRC 不符」同一处置。

#### 关键决策

| # | 项 | 决策 |
|---|---|---|
| **A** | 元数据页 FC CRC | `FTL_PAGE_CFG_META`：`FC_CRC_DISABLE` → **`FC_CRC_ENABLE`**（单点开关；Checkpoint/Anchor/BBT/Wear/Journal/L2P-Region 六条路径共用它，一处即全部生效） |
| **B** | 删 FTL 记录 CRC | 四结构体删 `crc` 字段 + 全部算/校验点：`l2p_checkpoint_header_t`（padding `2048-71*4`→`2048-70*4`；删 `CHECKPOINT_CRC_LEN`）、`metadata_anchor_page_t`（padding 去掉 `-4U`；删 `ANCHOR_PAGE_CRC_LEN`）、`bbt_header_s`（packed，544→**540B**）、`wear_page_t`（`WEAR_ENTRY_MAX` 去掉 `-4U`：252→**253**，padding 12→8；`WEAR_SNAPSHOT_PAGES` 仍 17） |
| **C** | 布局版本 | `FTL_MEDIA_LAYOUT_VERSION` 1U → **2U**（四结构体尺寸/格式变化）⇒ 首次上板由 v0.61 自动格式化兜底 |
| **D** | 不动 | `boot_crc32` 保留（仍被 `boot_policy.c` 的开机镜像 CRC 使用）；`ftl_sync_read_page_raw` 家族（LDPC_RAW / 读列 / 读 META）保持 `FC_CRC_DISABLE` |

#### 代价 / 取舍（诚实登记）

- **每条元数据页多 4B/CW 的 CRC 字段与计算**：CPU 开销可忽略（STM32H743 硬件 CRC 外设），
  介质开销 = 每 CW 4B（已在既有 528B CW 布局内，不额外占用户区）。
- **读路径不变**：CRC 错误与既有 NAND/TIMEOUT/UNC 走同一 `FC_CW_STATUS_ERROR_MASK` 判据。
- **介质格式变更**：四结构体尺寸/内容变化 ⇒ 首次上板触发一次自动格式化（预期内）。

#### 改动文件

- `include/ftl/ftl_fc_interface.h`（`FTL_PAGE_CFG_META`：CRC 开关翻转）
- `include/ftl/checkpoint.h` / `src/ftl/checkpoint.c`（删字段 + 删 `CHECKPOINT_CRC_LEN` + 删写/校验）
- `include/ftl/metadata.h` / `src/ftl/metadata.c`（删字段 + 删 `ANCHOR_PAGE_CRC_LEN` + 删写/校验）
- `include/ftl/bbt.h` / `src/ftl/bbt.c`（删字段 544→540B + 删写/校验）
- `include/ftl/wear.h` / `src/ftl/wear.c`（删字段 + `WEAR_ENTRY_MAX` 252→253 + 删写/校验）
- `include/ftl/ftl_config.h`（`FTL_MEDIA_LAYOUT_VERSION` 1U→2U）
- 注释同步：`l2p_cache.c` / `journal.c` / `wear.c` / `metadata.h` / `bbt.h` / `ftl_fc_interface.h`
- `docs/design/ftl_design.md`（§7.6 四层保护重写、§3.8 BBT、§8.2 Checkpoint、§13.1 约束表、§Anchor、§Journal 接口）

#### 验证

- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%，本改动不增 RAM）
- `make clean && make tests`：**零告警**（`_Static_assert(sizeof == PAGE_MAIN_SIZE)` 全部通过
  ⇒ 四结构体尺寸回填正确）
- 静态：`grep -rni crc include/ftl src/ftl` 仅剩 FC 层数据路径引用，无 FTL 记录级 CRC 残留
- 上板（测试一律上板）：烧 `debug.bin` → 首次开机自动格式化一次 → 写数据 → 断电重启 → 数据仍在；
  人为翻转介质上一字节 → 该页读取报 `CRC_ERROR` 被丢弃（验证 CRC 真正生效）

---

### v0.63 — GC 触发对齐 DRAM-less 主流：删无出处触发（T2/T3）+ 统一求值点 + 修 IDLE 补池下溢

**日期**：2026-10-13

#### 背景（问题从哪来）

波 0 上板后跑了几百次 `wr 0 32`，串口**一行 `[GC]` 都没有**。排查结论不是「GC 坏了」，
而是**触发模型与求值点双错**，外加一个顺带查出的真缺陷。

#### 讨论内容

**1. 求值点不可达（结构性）**

`gc_check_triggers` 全仓**只有一个调用者**：`ftl_service_normal`（`ftl.c`）。
而 host 写全程 `queued != 0`（`pending.c` 的组 `used` 要到全部子操作完成才清）⇒ 模式三选一
走 `ftl_service_busy`（不 GC）；真空闲走 `ftl_service_idle`，它有**自己的**内联补池循环、
**不调**该函数。⇒ 五路触发里只有 T2（写入量整除 1024）能在实测中到达，且要 ≈256 次
`wr 0 32` 恰好命中整除点。

**2. T2/T3 在参考层查无出处（判据问题）**

逐字核过 `docs/reference/`（7 论文 + 9 网页）与 `../storage_controller_design.md`：
**T2（`total_writes % GC_INTERVAL_WRITES`）与 T3（`GC_TIMER_INTERVAL_MS`）零引用**。

| 出处 | 说的 GC 触发是什么 |
|------|-------------------|
| `spdk_ftl.html:180`（离线副本；原文 <https://spdk.io/doc/ftl.html>） | **按需**——「all of the bands contain some valid data and no band can be freed, so no writes can be executed anymore」 |
| 同上（另一处） | nvcache 压缩的 **free-chunk 阈值**（水位） |
| `reference/README.md`（dhara，DRAM-less） | 「没有复杂 GC，只有磨损均衡 + 坏块 + 日志」 |
| `cw_level_mapping_design.md`（DFTL/LeaFTL/VPFTL） | DRAM-less 侧只讨论**有效性判定**（OOB 反向映射），触发同样只认空间压力 |

对照：Journal 的 `CP_WRITE_INTERVAL` 在 `../ftl_design.md` 有完整推导；T2/T3 只有
「分散 GC 压力」「空闲时有空就做一点」两句断言，参数表/代码注释/变更史全无推导。
**T3 的注释还自称「host 不写也不擦时，让空闲期把池补回去」（`gc.c`），
实现却在只在 host IO 在场时才跑的 NORMAL 路径——注释与实现自相矛盾。**

**3. IDLE 补池循环 `uint32_t` 下溢（真缺陷，顺带修）**

`ftl.c` 的 IDLE 补池：

```c
uint32_t target = FREE_POOL_TARGET - ftl->alloc.free_count;   /* ∈ [1, 32] */
while (target > 0U) { …GC 一个 SB…; target -= 2U; }           /* 每轮减 2 */
```

`free_count` 按 **2** 递减（`allocator_alloc_pair` 一次一对），本板实测 `free=3713`
（**奇数**）⇒ 一旦跌破 32，`target` 也是奇数 ⇒ 最后一次迭代后 `target == 1`，
`1 - 2U` 回绕成 **`0xFFFFFFFF`** ⇒ 循环只在 `gc_select_victim` 返回 0 时退出，
即**把 closed 链上的 SB 全搬全擦一遍**（长时间阻塞 + 无谓写放大）。

#### 决策

| 序号 | 决策 | 选择 | 拒绝方案 | 理由 |
|------|------|------|---------|------|
| **D1** | 触发集合 | **删 T2/T3，留 T1/T4/T5** | ① 保留 T2/T3 只修求值点（T2 改有状态）② 连 T5 一起删 | 判据 = 有出处的主流；T2/T3 零引用且求值点不可达。T5（磨损比）是本仓 GC 的均衡维度，与独立静态 WL 分工不同，保留 |
| **D2** | 求值点 | **IDLE + NORMAL 共用 `ftl_gc_evaluate`** | ① 搬进 BUSY（三档全判）② 只留 IDLE | BUSY 是「host 饿着、后台让路」档（QoS 契约）；「无块可写」已由 `ftl_issue_subop` 的紧急 GC 兜底 = SPDK 的 demand 触发 |
| **D3** | 触发位值 | **不重排**（0x01/0x08/0x10；0x02/0x04 留空） | 重排成连续位 | 保持 `[GC] trig=` 日志与历史 ADR 里的位值可比；与 T6 的 0x20 留空同例 |
| **D4** | IDLE 突发规模 | **按「需回收的 SB 数」计数**（`⌈deficit/PLANE_NUM⌉`） | 保留 `target -= 2U` | 修下溢；deficit ≤ 0（仅 T5 命中）时取 1，即只回收一个 SB |
| **D5** | 顺带删 | `gc_check_trigger`（布尔版，零调用者）、`ftl_context_t.gc_last_run_writes`（零读者）、`gc_timer_start`（只服务 T3）、`BG_NORMAL_GC_PAGES` / `BG_IDLE_GC_TARGET`（死常量，注释描述已删模型） | — | 项目规矩：没有用处直接删掉 |
| **D6** | 不动 | `gc_select_victim` 四维评分（P1-1）、`gc_migrate_sb` 的 approach ③ 逐 CW 有效性判定、BUSY 档行为 | — | 均已有出处且已上板验证 |

#### 代价 / 取舍（诚实登记）

- **空闲板（`free ≈ 3713 ≫ 32`）按设计就不该跑 GC** —— 这不是缺陷，是水位驱动的正确行为。
  代价是 §7 的**手工验证需要一个入口**：把 `FREE_POOL_TARGET` 临时改成 `free + 2`
  （触发 1 个 SB 的搬移+擦除），验完改回 32。**未引入 CLI `gc` 命令**（用户已选改常量方案）。
- **一次 GC 迭代「取 2 块目标页 + 还 2 块 victim」⇒ `free` 净变化 ≈ 0**，所以 §7 的判据
  是 `[GC]` 两行 + `rd PASS` + 无 `IO_ERROR`，**不是**「`free` 上升」。
- **删 T2 后不再有「按写入量摊平 GC 压力」的机制**：GC 变成纯水位驱动 ⇒ 压力集中在水位线附近。
  这是主流形态（SPDK 即如此），代价由 **OP（预留空间压低 u）** 与后台档位承担。

#### 改动文件

- `include/ftl/gc.h`（删 `GC_INTERVAL_WRITES` / `GC_TIMER_INTERVAL_MS` / `GC_TRIGGER_WRITES` /
  `GC_TRIGGER_TIMER` / `gc_check_trigger`；`gc_check_triggers` 签名去两参；位注释改写）
- `src/ftl/gc.c`（删 T2/T3 分支与布尔包装；文件头 + 函数注释「五路」→「三路」）
- `include/ftl/ftl.h`（删 `gc_last_run_writes` / `gc_timer_start` / `BG_NORMAL_GC_PAGES` /
  `BG_IDLE_GC_TARGET`）
- `src/ftl/ftl.c`（新增 static `ftl_gc_reclaim_one` / `ftl_gc_evaluate`；`ftl_service_idle`
  与 `ftl_service_normal` 改用统一求值点；删两字段 init；修补池下溢）
- `docs/design/ftl_design.md`（§4.4 触发表重写 + T2/T3 删除论证、§4.6 带宽/后台模型重写并删
  无出处的「三星 GC 策略」句、§4.7 参数表、§12 服务模式伪码与实现原型、§4.9 对照表）
- `docs/design/ftl_api_usage.md`（上下文结构列举、Phase 3 流程叙述）
- `docs/archive/ftl_wave0_board_verify.md`（§7 配方重写 + §4 前提行）

#### 验证

- 静态：`grep -rn "GC_INTERVAL_WRITES\|GC_TIMER_INTERVAL_MS\|GC_TRIGGER_WRITES\|GC_TRIGGER_TIMER\|
  gc_timer_start\|gc_last_run_writes\|gc_check_trigger\b" src/ include/ tests/` ⇒ **零命中**
  （仅 `gc.h` 位注释里作为「已退役」的说明性引用）
- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%，本改动只减字段）；
  `Binary size: 189656 bytes`
- `make clean && make tests`：**零告警**；`Binary size: 267772 bytes`
- 上板（测试一律上板，见 `docs/archive/ftl_wave0_board_verify.md` §7 新配方）：
  临时 `FREE_POOL_TARGET = free + 2` ⇒ 开机后第一个 IDLE tick 应打
  `[GC] trig=0x01 free=… op=… writes=…` + `[GC] victim p0=… p1=… vr=… er=… cp=… yp=… cost=…`
  **各一行**（下溢修复的判据就是行数：旧代码会把 closed 链全搬全擦、连打几十行）；
  `rd 0 8 c` ⇒ `rd PASS`；断电重启 ⇒ `[WEAR] max_ec ≥ 1`

---

### v0.64 — 修「写→掉电→重启丢数据」：干净关机路径从不恢复 Journal RAM 写态

**日期**：2026-10-13

#### 背景（问题从哪来）

波 0 清单 §6（掉电重启数据仍在）**上板失败**：

```
[INF] [FTL ] boot type=2 cp_seq=624 journal_entries=3811
...
rd FAIL at byte 0: got 00000000 expect 55555555
```

`journal_entries=3811` 这个数字本身就是证据：3811 = 30×127 + 1 ⇒ 回放扫到的条目
落在**环形第 0 块第 30 页**（`JOURNAL_ENTRIES_PER_PAGE=127`、`PAGES_PER_BLOCK=64`）。
但一个刚格式化的环，写到第 30 页意味着 3811 条 L2P 更新——远超本轮测试实际写过的量。
⇒ 这是**之前几百次 `wr 0 32` 留在环里的旧页**被重新当成「本轮数据」回放/漏回放。

#### 讨论内容

**1. 根因：`journal_init` 无条件把 RAM 写态复位，而 NAND 上的环并没被擦**

`journal_init` 每次开机都把 `current_block/page/entry = 0`、`sequence_counter = 1`。
`ftl_boot_clean_start` 的注释据此写着「④ Journal 已由 ftl_init 初始化（空）」——**这个前提是错的**：

- `journal_truncate`（Checkpoint 的截断动作）**只预擦环形下一块**（P1-6 的单块单擦设计），
  它**不擦**环里其余已编程的页；
- 于是「Journal 是空的」只对**刚格式化过的介质**成立。正常运行的板子上，
  环里躺着一圈已编程的旧页。

两个后果，任一都足以毁掉 §6：

- **① 序号回退**。序号从 1 重来 ⇒ 本轮新条目的 `page_seq` **排在环内旧页之后**。
  下次开机 `journal_replay` 的过滤（`page_seq > cp_sequence`）把新页当「已进 L2P 表」整页丢弃，
  而旧页（序号大）反而通过过滤被应用 ⇒ L2P 被**旧映射覆盖**。
  这解释了 FAIL 那次为什么读到全 0：LBA 0 的映射被指回了从未写过数据的物理位置。
- **② 写指针回到 block0/page0** ⇒ 对**已编程**的页二次编程。NAND 编程只能 1→0，
  二次编程 = 按位 AND ⇒ 页头 `entry_count`/`magic` 被静默改坏，环从此不可解析。

**2. 顺带查出的第二个缺陷：`journal_replay` 的写指针落点**

旧实现的指针恢复是「最新页 + 页内下标 + 1」——即**继续填最新那一页**。
若最新页是部分页（掉电撕裂），续填会在页满时对该页**再 flush 一次**，同样二次编程页头。
正确落点是**最新页之后一页**（最新页已编程，不可再动）。

**3. 为什么「干净关机」也会中招**

干净关机（`type=1`）看似不经回放，但 §6 的失败恰恰在 `type=2`。两条路径共用一个错误前提：
**都假设 `journal_init` 之后 RAM 写态与 NAND 一致**。所以修复必须覆盖全部三条启动路径。

> 旁证：随后的「干净环重测」打印 `type=1 journal_entries=0` 并 `rd PASS`。
> 那次 PASS **没有覆盖回放路径**（数据是靠 Checkpoint 活下来的，`cp_seq` 1→4），
> 所以它**不能**用来判定缺陷已清 —— 两个日志是同一个机制的两面。

#### 决策

**Journal 层新增两个 API，把「扫环」与「接写态」从启动路径里收进 Journal 模块**：

- `journal_scan_latest(const l2p_journal_t *, journal_page_ref_t *out)` —— **只读**扫环取最新页
  （最大 `page_seq`），出参含 `{page_seq, block, page, entry_count}`。环空返回 0。
  与 `journal_replay` 共用同一份扫描内核 `journal_scan_ring`（旧实现是启动路径内联扫一遍，
  两份逻辑各写各的）。
- `journal_resume(l2p_journal_t *, const journal_page_ref_t *latest, uint32_t floor_sequence)` ——
  按 NAND 实际内容恢复写指针与序号，**不回放任何条目**：
  - 落点 = 最新页**之后一页**；跨块时先擦下一块（那里还留着上一圈的旧页）；
  - 序号 = `max(最新页 page_seq + 最新页 entry_count, floor_sequence)`，`floor_sequence` 由调用者传
    Checkpoint 的 `journal_sequence`（兜住「环空」与「环内最新页反而落后」两种情形）；
    **必须是 `+ entry_count` 而不是 `+1`** —— 一页占了 `entry_count` 个序号
    （`journal_append` 逐条 `sequence_counter++`，页头 `hdr_page_seq = sequence_counter −
    current_entry` 反推），只 +1 会让序号退回本页内部，下一页的 `page_seq` 落进本页区间，
    回放过滤 `page_seq + entry_count > cp_sequence` 便**重复回放已进 L2P 的页**
    （上板现象 = `journal_entries` 虚高：286、389 = 3×127+8，都带整页形状）。
  - 清空 `page_buf` 为全 FF、`current_entry = 0`。

**三条启动路径全部接上**：

| 路径 | 调用 |
|---|---|
| `ftl_boot_recovery` | 先 `journal_scan_latest(&ftl->journal, &ring)` 取快照（只读，供判型），再分派 |
| `ftl_boot_fresh_start` | 新增 ④ `journal_resume(&ftl->journal, ring, 1U)` |
| `ftl_boot_clean_start` | 新增 ② `journal_resume(&ftl->journal, ring, cp_journal_sequence)`（这是本缺陷的正面修复） |
| `ftl_boot_power_failure` | 走 `journal_replay`，其末尾已改为 `journal_resume` |

**回放过滤判据由「页首」改为「整页」**：`page_seq > cp_sequence`
⇒ `page_seq + entry_count > cp_sequence`。旧判据会把**跨水位的那一页整页跳过**，
丢掉它尾部那几条本应回放的新条目（水位落在页中间时必现）。

**删死代码**：`ftl_boot_recovery` 里的内联扫描被 `journal_scan_latest` 取代后，
`power_failure_recovery.c` 的 static `journal_page_addr` 变成孤儿 ⇒ 删。
扫描用的 2 KB `page_data` 缓冲随之从 `ftl_boot_recovery` 的栈帧移到 `journal_scan_ring`，
**栈占用不增**（`_Min_Stack_Size = 0x800`）。

#### 代价 / 取舍（诚实登记）

- **每次开机多一次只读扫环**（`JOURNAL_BLOCK_COUNT` 块 × 64 页的页头读，只读第一页头即可
  命中即跳过整块，但当前实现是逐页扫）。代价在开机时间，不在稳态路径。
  与「干净关机必须扫」的收益（否则写指针就是猜的）相比，这个代价是必需的。
- **`pre_erased_block` 是纯 RAM 记账，掉电即失**。重启后 `journal_resume` 跨块时会**再擦一次**
  下一块 ⇒ 相对「不丢记账」的情形多一次擦除。**这不是新引入的**（`journal_init` 本来就丢），
  但修复让这个路径真正可达了 ⇒ 登记为已知代价。P1-6 的「每圈只擦一次」在**单次开机内**成立。
- **`ftl_boot_power_failure` 仍走 `journal_replay` 的全量回放**，与 `resume` 的分工是
  「先回放（应用条目）→ 再 resume（接写态）」，没有合并成一次扫描的优化（可读性优先）。

#### 改动文件

- `include/ftl/journal.h`（新增 `journal_page_ref_t` + 两个 API 声明；`journal_replay` 的
  判据注释改为整页口径）
- `src/ftl/journal.c`（§4 重写：static `journal_scan_ring` 单一扫描内核 +
  `journal_scan_latest` + `journal_resume`；`journal_replay` 改用共用内核，
  过滤改整页口径，末尾接 `journal_resume`；文件头职责行同步）
- `include/ftl/power_failure_recovery.h`（两个 boot 函数签名加 `const journal_page_ref_t *ring`；
  文档注释补 `journal_resume` 步骤与「环形块并没被擦」的关键说明）
- `src/ftl/power_failure_recovery.c`（`ftl_boot_recovery` 改为「先取快照 → 判型 → 分派」；
  两条 boot 路径各加一步 `journal_resume`；删 static `journal_page_addr`）

#### 验证

- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%）；
  `Binary size: 189336 bytes`（改前 189656，**净减 320B** —— 内联扫描 + 孤儿函数删掉，
  扫描缓冲只留一份）
- `make clean && make tests`：**零告警**；`Binary size: 267460 bytes`（改前 267772）
- `grep -n journal_page_addr src/ftl/power_failure_recovery.c` ⇒ **零命中**
- 上板（待验，判据见 `docs/archive/ftl_wave0_board_verify.md` §6）：
  ① 复现配方 = 先把环弄脏（若干次 `wr 0 32`）→ 干净关机（`fl` 后断电）→ 再 `wr 0 8`
  → 直接断电 → 重启 `rd 0 8 c` 应 `rd PASS`；
  ② 修复前的签名是 `rd FAIL at byte 0: got 00000000`，修复后 `boot type` 仍按关机方式为 1 或 2，
  但 `journal_entries` 应回到**本轮实际写量**量级（不再是 30×127+1 这种整圈数）。
- **上板结果（2026-10-13）**：§6 主判据过 —— 脏环配方全程无 `rd FAIL`，最后一次
  `type=2 cp_seq=122 journal_entries=389` 下 `rd 1000 8 c` ⇒ `rd PASS`（该 LBA 本轮只写过 8 条，
  映射只能来自回放）。**但 `journal_entries` 虚高未消**（286、389 = 3×127+8，都带整页形状），
  据此反查 `journal_resume` 的序号落点，发现上述 `+1` / `+ entry_count` 缺陷并修正
  （`Binary size: 189344 bytes`，零告警）。**修正后需重跑 §6 复核 `journal_entries`。**
- **不变量（比具体数字更重要）**：`journal_resume` 保证 `sequence_counter > 环内任何 page_seq`
  ⇒ 新条目序号恒高于环内所有旧页 ⇒ 回放按 `page_seq` 升序 + per-LBA last-write-wins
  ⇒ **旧页即使被回放也必然先应用、被新条目覆盖**。这是 v0.64 真正提供的数据安全保证，
  「`journal_entries` 偏大」本身不构成数据风险，只浪费开机时间。

##### v0.64 追加修正 ③：判型判据的 off-by-one（**本批第三个、也是最要命的缺陷**）

**上板证据（擦净后的全新介质，2026-10-13）**：

```
（bbt --erase 后首次开机自动格式化）
[INF] [F2] rebuild blocks=0 partial=0 valid_cw=0
[INF] [FTL ] boot type=1 cp_seq=2 journal_entries=0
wr 1000 8                      ← status=0，写成功
（断电）
[INF] [F2] rebuild blocks=2 partial=2 valid_cw=0
[INF] [FTL ] boot type=1 cp_seq=3 journal_entries=0
rd 1000 8 c  ⇒  [ERR] rd FAIL at byte 0: got 00000000 expect 55555555
```

**数据页在 NAND 上（`blocks=2`），映射没了（`valid_cw=0`）⇒ 映射从未被恢复。**

**根因**：`ftl_boot_recovery` 的判型只看**页首**：

```c
if (ring.page_seq <= cp.journal_sequence)   /* ✗ 判成「干净关机」 */
```

而 `cp.journal_sequence` 的语义是「**下一个待分配**序号」（`checkpoint_create` 直接取
`sequence_counter`；`journal_truncate` 的注释亦如此声明：全部已写条目 `seq < up_to_sequence`）。
⇒ 被 CP 覆盖的条目是 `seq < cp.journal_sequence`。一页占
`[page_seq, page_seq+entry_count-1]`，**全被覆盖 ⟺ `page_seq + entry_count ≤ cp.journal_sequence`**。

**Checkpoint 之后写入的第一页，其 `page_seq` 恰好 == `cp.journal_sequence`**（因为页头由
`hdr_page_seq = sequence_counter − current_entry` 反推，CP 时刻 counter 就是那个值）。
于是：写 → 掉电 → 判 `type=1`（干净）→ **不回放** → L2P 从 NAND 表加载（没有这批映射）
→ 静默丢数据。

**修法**：判型改整页口径，与 `journal_replay` 的过滤同口径：

```c
if ((ring.page_seq + (uint32_t)ring.entry_count) <= cp.journal_sequence)   /* ✓ */
```

**为什么这个缺陷特别隐蔽**：① 它只在「CP 之后写入、且该页是环内最新页」时命中，而
`ftl_service_idle` 每轮都会 `journal_flush_page`，所以页确实落了盘、看起来一切正常；
② CLI 的 pattern 与 LBA 无关（前 8KB 全 `0x55555555`、后 8KB 全 `0xAAAAAAAA`，见
`cli.c`）⇒ 只要读到**任何**含该 pattern 的页就报 `rd PASS`，**旧介质上的
「PASS」因此可能是假绿**；③ 它同时解释了本批日志里 `type=1 journal_entries=0` 反复出现——
判型几乎总是偏一页地判成「干净」。

**验证**：`make clean && make all` 零告警，`Binary size: 189488 bytes`。
上板判据：擦净介质 → `wr 1000 8` → 断电（**不需要抢时间**：journal 页由 IDLE 每轮刷出）
→ 重启应 `type=2` + `journal_entries=8` + `rd 1000 8 c` ⇒ `rd PASS`。

**已改正的判据口径（三处必须一致）**：
| 位置 | 判据 |
|---|---|
| `ftl_boot_recovery` 判型（干净） | `page_seq + entry_count <= cp_sequence` |
| `journal_scan_ring` 回放过滤 | `page_seq + entry_count > cp_sequence` |
| `journal_resume` 序号落点 | `page_seq + entry_count` |

---

### v0.65 — 波 2 剩余 + 波 3：表自持 / 多实例 / OOB 出厂坏块 / 分片模型定案 / L2P 日志块压缩 / rr 持久化

**日期**：2026-10-13

#### 背景

按 `ftl_optimization_plan.md` §九「优化顺序」推进。波 0（v0.59~v0.64）收口后，
波 2 剩四项（序 3/4/5/6，均为**潜伏债**，`dims=1` 时不咬）、波 3 两项（序 7/8，
有效性缺口）、波 5 的最后一个死代码项，本批一并落地。

**本批全部未上板。**

---

#### 序 3 + 序 4 — FTL 表自持 + 多实例化（同一改动的两面）

**问题**：`checkpoint.c` / `bbt.c` / `wear.c` 各有一组 **file-scope static** 游标与序号。
单实例下无害；多实例（多 LUN，`g_ftl[ch][ce][lun]`）下**所有实例共享一份** ⇒ 各自往
同一个池槽写、`sequence` 互相踩、脏位图互相清。这与「每个 `(ch,ce,lun)` 是一个完整
独立的 FTL 实例」的分片模型（`cw_level_mapping_design.md` §3.1）直接冲突。

**做法**（把状态搬到「它天然属于谁」的地方，而不是一律塞进 `ftl_context_t`）：

| 原 static | 新家 | 为什么 |
|---|---|---|
| `bbt.c` 的 sequence / write_idx / cursor_valid / dirty | `ftl_allocator_t.bbt`（`bbt_state_s`） | `bbt.h` 的**全部**接口都以 `ftl_allocator_t *` 为首参；且 `bbt.h` 已 include `allocator.h`，反向包含成环 |
| `wear.c` 的 sequence / next_page / 节拍基准 / 脏位图 | `ftl_allocator_t.wear`（`wear_state_t`） | 同上 |
| `checkpoint.c` 的 sequence / write_idx / cursor_valid | `ftl_context_t.cp`（`l2p_cp_state_t`） | `checkpoint.h` 已前向声明 `struct ftl_context`，无环 |

`allocator_init` 把 `alloc->bbt` / `alloc->wear` **显式 memset 归零**——这两个结构里的
`cursor_valid` / `dirty[]` 决定首次落盘是「扫池定位」还是「从池首开始」，残留值会让
新实例写到别人的槽上。

**顺带删掉的死字段**：`ftl_context_t.cp_sequence`——4 个写点、**0 个读点**。boot 日志里
的 `cp_seq` 来自 `ftl_boot_result_t.cp_sequence`，真正的池序号是 `cp.sequence`，三者
语义各不相同，旧字段纯属混淆源。同理 `ftl_boot_clean_start` 的 `cp_sequence` 形参
（`checkpoint_find_latest` 已把序号写进 `ftl->cp.sequence`，形参成了第二真值）。

**泵/实例分离**（多实例的正确调度形态）：

```c
void ftl_service_pump(void);          /* 实例无关：Phase 1 推进 NAND + Phase 2 抽干完成表 */
void ftl_service(ftl_context_t *ftl); /* 单实例：Phase 2.5 补发子操作 + Phase 3 三级后台 */
```

`main.c` 每轮：`ftl_service_pump()` 一次 + 对**每一个** `g_ftl[ch][ce][lun]` 调
`ftl_service()`。Phase 1/2 的 NAND 引擎与完成表是**全局单份**，per-instance 调用会把
`fc_service` 预算重复消耗 N 遍；Phase 2.5/3 则必须逐实例，否则漏调的实例
journal / checkpoint / wear / bbt / GC **永不推进**。

**内存**：`g_ftl[1][1][1]` 单实例，AXI_RAM 283.1/384 KB（73.7%）。当前
`CH_NUM=CE_NUM=LUN_NUM=1` ⇒ 内存预算恰好只装一个实例；维度扩展须先重规划内存
（`main.c` 的 `_Static_assert` 会在编译期拦下）。

---

#### 序 5 — 出厂坏块检测改走 OOB 标记（P2-3）

**问题**：`bbt_scan_factory` 原来靠「裸读返回码 + 全 0xFF」猜坏块。两个判据都不可靠：
raw 模式对物理失败也返回 READY；全 0xFF 既可能是空块也可能是坏块，**分不开**。

**做法**：改读器件手册规定的**出厂坏块标记**——每块首页 spare 区首字节（列 2048），
经 `OP_READ_COLUMN`（`ftl_sync_read_column`，FC 槽位 42，**此前已实现**）够到：

- 标记非 `0xFF` ⇒ 出厂坏块；标记 == `0xFF` ⇒ 好块；**列读失败 ⇒ 坏块（保守）**。

判据逐字来自 Micron AN1819：*"the bad block information is erasable and cannot be
recovered once erased"* + *"System software should check the first spare area location on
the first page of each block prior to performing any PROGRAM or ERASE operations"*。
⇒ 扫描阶段**只读不擦**（`bbt_probe_block_bad(..., do_erase=0)`）——擦除会永久毁掉标记。

`bbt_load` 顺带去掉 `topo` 形参（从 `alloc` 派生，与其余 BBT 接口一致）。

---

#### 序 6 — D6「LBA→实例路由上移到单一 FTL 前端」：**登记理由，保留分片模型**

**动作是文档，不是代码**，理由是查证结论：

- 该主张的出处是**本仓自己的 D6 对照表**（`ftl_host_interface.md`、
  `v0.58 — FTL 对外接口对齐主流（plane 策略下沉 + 组完成语义） 条目`、`ftl_optimization_plan.md`），三处均标「主流」
  但**无任何外部引用**。
- 它唯一援引的 `fc_nand_design.md §26`（本项目锁定的判据）**通篇不谈** L2P 在
  通道/die 间如何分布——只定 HIL/FTL/FCC 分层边界。
- 仓内还存在**反方向**的无出处断言（`v0.35 — 数据平面多维度化（L2P 分片 + ch/ce/lun 泛化 + 路由哈希） 条目`「行业标准：每 LUN
  独立 region 表」）⇒ 两个方向都被断言过，都无证据。
- 唯一可能支持「单一 FTL 拥有全 LBA 空间」的**已备份**厂商文档（SPDK FTL，
  `spdk_ftl.html` 离线副本；原文 <https://spdk.io/doc/ftl.html>）从未被引用于此。

**裁决**（按 `ftl_optimization_plan.md` 明文允许的「**或**明确保留分片并登记理由」）：
**保留分片模型**，理由如下，且**不**把它记为「与主流偏离」——

1. 分片是**本仓的刻意模型**（`cw_level_mapping_design.md` §3.1/§3.2 有完整对比：
   模型 A 分片 vs 模型 B「LUN 作为地址维度」，后者已删）。
2. 判据要求「有出处」。「主流是单一前端」无出处 ⇒ 按无出处处理，**不得**据此改代码
   （否则就是以另一个无出处断言替换现有的刻意设计）。
3. 平台现状 `CH_NUM=CE_NUM=LUN_NUM=1` ⇒ 今天恰好只有一个 FTL 实例，
   「单一前端」性质**平凡成立**；真正的分歧只在维度扩展时才显现，属未排期。

⇒ `ftl_host_interface.md` 的 D6 行与 `ftl_optimization_plan.md` §九 序 6 据此订正
（「主流」列改为「无出处（本仓断言）」+ 保留理由）。

---

#### 序 7 — P0-3 L2P 日志块压缩（抗碎片）

**问题（活跃容量泄漏）**：`metadata_dir_reclaim` **只**回收 `l2p_live[g] == 0` 的日志块。
某个块只要还挂着一个**永不再写的冷 region**（如 63 冷 + 1 热），活计数就永不归零 ⇒
**永久占块**。稳态下日志块数单调增长，最坏吃掉近半个 die（`FTL_BLOCK_TOTAL/2` 块）。

**做法**：新增 `metadata_dir_compact(dir, alloc)`——`reclaim` 的补集。

- **victim** = `l2p_live` 最小（但 > 0）的**非当前**日志块，且必须是 `RESERVED`
  （与 `reclaim_block` 同一身份判据，避免误伤恰好活计数非零的用户块）。平手取块号最小者
  （结果确定，跨复位可复现）。
- **搬移** = 裸页拷贝：读 victim 上的 region 页（2KB = `L2P_REGION_SIZE`，region 页无头，
  就是 entries 数组）→ `metadata_dir_append_slot` 取当前块新槽 → 写 → `commit_region` 登记。
  **不经过 `l2p_cache`** ⇒ 不触发「写→evict→flush→又要块」的递归。
- **掉电安全顺序**（不可重排）：① 搬移 → ② 登记 → ③ `metadata_dir_flush_to_anchor`
  → ④ 擦 victim → ⑤ 归还 Free Pool。**③ 必须早于 ④**——否则掉电后 Anchor 仍指向
  已擦块，那些 region 的映射**永久丢失**（读回 0xFF = 未映射）。
- **触发点**：`metadata_dir_append_slot` **刚取到新日志块**之后，且
  `alloc->free_count <= FREE_POOL_TARGET`。此刻块内有整 `PAGES_PER_BLOCK` 个空槽
  ——这是唯一能「一次搬空任意 victim」的时机（一块最多 `PAGES_PER_BLOCK` 个 region）。
  低水位门槛保证池子宽裕时不做无谓 NAND 写；稳态下每 `PAGES_PER_BLOCK` 次 region 写
  最多压缩一块。

**新增 `dir->compacting` 标志，两个作用缺一不可**：

1. **抑制逐次立即回收**：搬移 victim 最后一个 region 时活计数归零，
   `commit_region` 的「归零即擦」分支会在 ③ 之前把 victim 擦掉（见上）；
2. **防重入**：搬移走 `append_slot`，其换块分支会再触发一次压缩 ⇒ `append_slot` 在
   `compacting != 0` 时直接返回 `NAND_PBA_INVALID` 让路（压缩方收到即中止，victim 不擦，
   已搬 region 的新位置已登记 ⇒ **无信息丢失**，下次重来）。

`compacting` 在 `metadata_dir_init` / `metadata_dir_load` 都显式归零（不跨重启）。

---

#### 序 8 — rr（慢死预测）持久化

**问题**：`ftl_block_t.rr`（`last_ecc` / `rising_count` / `health`，3B）是纯 RAM 态，
`ftl_init` 的 memset 跨重启清零 ⇒ `rr_predictive_scan` 的 ECC 趋势分支**跨重启归零**，
要重新攒 `RR_ECC_RISING_THRESHOLD` 次连续上升才再触发，期间那块一直在劣化。
（read-disturb 分支不受影响——`read_count` 已由 P0-1 的 Wear 表持久化。）

**做法**：把三字段并进 **Wear 条目**（该表已有完整的「页式追加日志 + 脏位图 + 全量
快照」机制，语义上也同属「per-Block 账本」）⇒ `wear_entry_t` 8 B → 12 B。
三字段**都必要**：

| 字段 | 丢了会怎样 |
|---|---|
| `last_ecc` | 「本次是否上升」的比较基准丢失 ⇒ 重启后首次读**必判上升**（虚高） |
| `rising_count` | 连续上升的进度丢失 ⇒ 趋势从头数 |
| `health` | 已判定的 WARNING/CRITICAL 白丢 |

`wear_init` 回放时对 `health` 做**范围钳制**（越界值 → `RR_BLOCK_HEALTHY`）：枚举只有
0..3，坏页读出的越界值会让 `rr_predictive_scan` 的 `>= RR_BLOCK_CRITICAL` 比较行为
未定义；钳到 HEALTHY 等于「这块重头数」，安全侧。

**介质格式变了 ⇒ `FTL_MEDIA_LAYOUT_VERSION` 2 → 3**。老格式 Wear 页的 `magic` **仍然
匹配**、条目布局却不同 ⇒ 不靠版本拦下就会把计数读成垃圾（这正是 v0.61 版本机制要防的）。

---

#### 波 5 — 删死代码 `ftl_boot_scan_write_pointer`

旧单-SB 写指针扫描路径，**零调用者**（写指针恢复由 F2 全扫 NAND 完成）。且它把
`uint8_t buf[16]` 传给按 `PAGE_MAIN_SIZE`（2048B）拷贝的 `ftl_sync_read_page_raw`
——栈溢出。按用户「无用直接删」原则删除（函数 + 头声明 + 文件结构注释）。
`ftl_sync_read_page_raw` 本身保留（F2 的 `page_is_blank` 仍在用）。

---

#### 验证

```bash
make clean && make all      # 0 warning；AXI_RAM 283.1/384 KB（73.7%）；Binary size: 190952 B
make clean && make tests    # 0 warning
```

**本批全部未上板。** 上板判据（并入 `docs/archive/ftl_wave0_board_verify.md`）：
多实例化与内存断言由编译期保证；OOB 出厂扫描的判据是「擦净介质后开机 `bbt` 读数与
器件标记一致」；P0-3 压缩的判据是「长写后 `free` 不单调下降 + `rd PASS`」；
rr 持久化的判据是「`[WEAR]` 行在重启后 `max_ec` 与关机前一致」。

#### 不做（本批）

`CONFIG_FC_RAID` 保持 0；`read_recovery.c` 保留地址 0x89 不碰；`journal.c`/`crash.c`
断言策略不变；不动 `include/nand/nand_config.h`；**P2-6（per-plane 独立写指针）**与
**波 4（GC Bitmap Layer 2 / Prefetch / 多流 hint）**保持登记未排期——前者是写指针模型的
结构性改动（multi-plane 要求两 plane 同页号，独立指针会拆掉 multi-plane 的前提），
后者的正确性窗口已由 v0.43「擦除前 flush」闭合（Layer 2 只是掉电后**断点续搬**的优化）。

---

### v0.66 — P2-6 定案（器件/标准约束）+ 页打包写缓冲（§七 方案 A）

**日期**：2026-10-13

**范围**：① P2-6 从「未排期（结构性改动）」改判为 **「不实现（器件/标准约束）」**（纯文档收口）；
② 实现 `cw_level_mapping_design.md §七 方案 A` 的**页打包写缓冲**——同一 pair 窗口内的多次
host 写攒成一页、窗口填满 / 切换 / flush / 空闲时**一次编程**。

#### 问题（P2-6）

`allocator_alloc_pair`（`src/ftl/allocator.c`）**每个写子操作**都给出一对 plane 页并推进写指针。
CW 级映射下子操作常只覆盖半个 pair 窗口 ⇒ 一次 512 B 写（1 CW）消耗一整对 4096 B 页
= **12.5 % 利用率**；`wr 0 4`（半窗口）= 50 %。器件**按页编程**：

- Micron `MT29F4G08ABADAWP` 参数页：Byte 86–89 数据字节/部分页 = `0x0200` = **512 B = 1 CW**；
  Byte 110 programs per page = **4**；时序表 Note 1：*"Four total partial-page programs to the
  same page. If ECC is enabled, then the device is limited to **one partial-page program per ECC
  user area**, not exceeding four partial-page programs per page."*（本仓每 CW 一个 ECC user area
  ⇒ 每 CW 只能编一次）。
- ONFI 3.2 §5.7.1.25 Byte 110 同义。

#### 关键决策：P2-6 的登记修法不可实现

登记修法 = **per-plane 独立写指针**。**否决**——两处一手判据都要求两 plane **同页地址**：

| 出处 | 逐字引文 |
|---|---|
| Micron `MT29F4G08ABADAWP` §Two-Plane Addressing | *"**The page address bits, PA[5:0], must be identical** for each issued address."*（`BA[6]` 必须不同；LUN 位必须相同） |
| ONFI 3.2 §3.1.1 Multi-plane Addressing | *"The plane address bit(s) shall be distinct… **The page address shall be the same** as any other multi-plane operations in the multi-plane command sequence."* |
| ONFI 3.2 §3.1.1.1 | *"**No restriction**: All block address bits may be different between two plane addresses."* ⇒ **Policy B（两 plane 不同块号）有标准级出处** |

独立写指针会让两 plane 的页号分叉 ⇒ 只能退化成两次单 plane 编程 ⇒ **拆掉 multi-plane 前提**。
⇒ 唯一能消除浪费又保留 multi-plane 的路 = **§七 方案 A 页打包写缓冲**（也正是主流做法）。

#### ONFI 版本时间线（对 ONFI 全集二分查证）

| ONFI 版本 | 全文 `plane` 出现次数 | 备注 |
|---|---|---|
| 1.0 / 2.0 / 2.1 / 2.2 | **0** | 1.0 的 `11h` 是 **Interleaved（多 LUN）**，不是 multi-plane |
| 2.3a | 首现 | `plane` 一词首次出现 |
| **3.2** | **首成标准** | §3.1.1 / §6.3 / §6.5 / §6.6 定义 multi-plane，并**强制同页地址** |

#### 方案 A 设计（`ftl_wbuf_t`）

`ftl_context_t` 新增 `ftl_wbuf_t wbuf`（4 KB 页镜像）：

```c
typedef struct {
    uint8_t  data[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 CW × 512B = 4096B 页镜像 */
    uint8_t  occupied;   /* bit i = 槽 i 有数据；i = plane*CW_NUM + cw（与 LBA 低 3 位同构） */
    uint8_t  open;       /* 1 = 正持有某窗口 */
    lba_t    base;       /* 持有页的 pair 窗口基址（低 3 位清零） */
    uint32_t owners;     /* bit gi = 组 gi 向本页贡献过 ≥1 槽（FTL_GROUP_MAX=32） */
} ftl_wbuf_t;
```

**写准入**（`ftl_issue_subop` 写分支 → `ftl_wbuf_append`）：窗口变化先 flush；把 n 个 CW 从 host
切片拷进 `wbuf.data[slot]`、置 `occupied` 位、`owners |= 1u<<gi`；满（`occupied==0xFF`）则尝试 flush。

**flush**（`ftl_wbuf_flush`）：`allocator_alloc_pair`（含 emergency GC）→ `fc_command_alloc` →
op select（两 plane 有数据 = `OP_PROGRAM_PAGE_MULTI_PLANE`，否则单 plane）→
`ftl_cmd_setup_ppa_pair_cw(cmd, new_pba, occupied)` → **`ftl_cmd_write_data_cw(cmd, wbuf.data,
occupied)`**（**按 CW 偏移从整页镜像散射**）→ `pending_add`（`lba = wbuf.base`，`cw_bitmap =
occupied`，`owners = wbuf.owners`，`buffer = NULL`）→ `fc_submit`。

> ⚠ **不能用 `ftl_cmd_write_data`（顺序打包）**：方案 A 合并多组后 `occupied` 可能**非连续**
> （组 A 写槽 {0,1}、组 B 写槽 {5,6} ⇒ `0x63`），顺序打包会把槽 0/1 的数据塞进 CW0/CW5。
> 稀疏位图是**固有需求**（`wr 0 1` 今天就是 `0x01` 的 512 B 部分页编程），此前位图恒是**连续段**
> 才恰好用顺序打包对；方案 A 起必须换散射。

**触发**：① 窗口变化 ② 满 ③ `ftl_flush` ④ **确定性 idle flush**（`ftl_service` Phase 2.5 之后
`ftl_wbuf_maybe_flush_idle`：若无任何 used 组的下一 LBA 落在 open 窗口内 ⇒ flush）。**不引入超时**。

#### 多组完成（`owners` 位掩码）

一页可能由**多个组**拼成（一个 FC 命令 = 一个 pending 槽）⇒ 完成时须对**每个**贡献组各递减一次
在飞数。`ftl_pending_req_t` 的 `uint16_t group` 换成 `uint32_t owners`；新增
`pending_group_complete_subop_multi(ftl, owners, sub_status)` 逐位置位调 `pending_group_complete_subop`
（`owners` 按值传入 = 局部副本，`on_complete` 重入复用槽不影响本循环）。

> ⚠ **`owner[8]` 逐槽写法是错的**：同一窗口内并发组覆盖同槽时旧组 `outstanding` 永不减 ⇒ 该 host
> 请求永久挂死。用 `owners` 位掩码（「哪些组向本页贡献过」）。

#### 读 / TRIM / 掉电语义

- **读命中**：`ftl_wbuf_has(ftl, lba)` ⇒ 直接从 `wbuf.data[slot]` 拷回 caller，不进 outstanding；
  L2P 循环内遇 buffered LBA 即 `break` 收缩。
- **TRIM**：命中 ⇒ 清该槽 `occupied` 位、跳过 L2P/journal；窗口变空 ⇒ 退掉 append（逐组完成 OK）。
- **掉电语义（无电容）**：**不做数据日志，改「推迟完成」**——buffered CW 只在 SRAM；
  `ftl_confirm_write`（journal + L2P + BST）**只**在 flush 编程成功的完成处理里调用（与既有顺序一致）。
  ⇒ 不变量「L2P/journal 只引用**已编程**页」由构造保证。掉电时这些 host 写**从未收到完成**，丢失
  即正确契约；重启回放旧映射，被覆盖的旧映射完好。

#### 顺带删死码

`allocator_alloc` **零外部调用者**（GC/WL v0.59 已全改 `allocator_alloc_pair`）⇒ 删函数体 + 声明 +
文档块 + 注释引用（`allocator.c` / `allocator.h`）。`grep -rn "allocator_alloc\b"` 只剩 `_pair`。

#### 验证

```bash
make clean && make all      # 0 warning；AXI_RAM 291.2/384 KB（75.8%）；Binary size: 193516 B
make clean && make tests    # 0 warning（不改测试源）
```

**本批未上板。** 板上回归（须全绿）：`wr 0 8`→`rd 0 8 c`；`wr 0 4`→`rd 0 4 c`；`wr 4 8`（跨窗口）；
`wr`→`fl`→断电→重启→`rd`；`tr`→`rd`。**合并可观测性**：新增 CLI 诊断命令 **`wp <lba> <n> <k>`**
（一次提交 k 组同窗口小写，绕过 busy 闸门）⇒ 判据 = `info` 的 `total_writes` 只 +1（对比逐组的 +k）、
`ftl_wbuf_flush` 打一行、`rd` 回读各段字节 = 0xA0+组号。

#### 局限（诚实登记）

收益需**多个组同时在飞**（pipelined host）。串行 host（CLI 现状）下「完成 = 持久」⇒ 两次写之间必然
flush ⇒ **不合并**；单个孤立的 4-LBA 写仍与今天相同（分配整对、编一个 plane）。本方案是**架构对齐
主流**（控制器形状）+ 为将来的流水线 HIL 铺路，**不是对串行 CLI 的吞吐改进**。

#### 不做（本批）

`CONFIG_FC_RAID` 保持 0；`read_recovery.c` 保留地址 0x89 不碰；`journal.c`/`crash.c` 断言策略不变；
不动 `include/nand/nand_config.h`；不引入 DRAM 常驻 L2P；不重排 `GC_TRIGGER_*` 位值；
不把 `ftl_trim` 改成异步（只登记在飞 flush 的既有竞争）；不把 CLI 改造成队列（用最小诊断命令替代）。

---

### v0.67 — 修 `allocator_close_sb` 的 valid 顺序缺陷：运行期关闭的 SB 对 GC/WL 永久不可见

**日期**：2026-10-14

**范围**：① 生产缺陷修复（`src/ftl/allocator.c`，一行换序）；② 测试基建修复
（`tests/unit/ut_main.c` 套件路径展开）。**上板跑 FTL L0 UT 顶出来的**。

#### 问题

板端 `test` 跑到第 4 例失败：

```
Error:test_ftl.c assertion failed: a->closed_sb_list[0].valid == 0U (1 == 0)
```

`include/ftl/allocator.h` 的字段语义是**字段级**的：`valid` = `1=正在使用（Open），0=已关闭`。
GC（`gc.c`）、WL（`wear_leveling.c`）、`gc_erase_and_free`（`gc.c`）三处遍历都以
`sb->valid != 0U → 跳过` 为闸门，注释写的是「跳过仍为 Open 的 SB」；重建路径
`allocator_add_closed_sb` 也显式 `valid = 0U` 并在文档里注明「**`gc_select_victim` 依赖**」。

但 `allocator_close_sb` **先拷贝、后清 valid**，清的是**源**（`hot_sb`/`cold_sb`）而非**副本**：

```c
alloc->closed_sb_list[alloc->closed_sb_count++] = *sb;   /* 拷进去的仍是 Open 态 valid=1 */
sb->valid = 0U;                                          /* 只失效了 Open 槽 */
```

⇒ **运行期关闭的每个 SB 都带 `valid == 1` 进列表 ⇒ GC/WL 一律跳过**；只有开机重建路径的条目可见。
`allocator_close_open_sb` 的文档注释本来就写着「进 closed_sb_list + **valid=0**」——
**意图正确，实现写错了对象**。

#### 影响（潜伏）

`free_count` 只出不进：水位跌破 `FREE_POOL_TARGET(32)` → `gc_check_triggers` 触发 T1 →
`gc_select_victim` 全表跳过 → 无 victim → 紧急 GC 无块可回收 → `allocator_alloc_pair` 返回
`NAND_PBA_INVALID` → **写永久失败**。WL 同理（一条候选都看不到，磨损均衡停摆）。

此前未暴露的原因：SB 需写满 64 页才关闭，GC 需 `free < 32` 才触发；板子开机后 `free=3710`
（`F2 rebuild blocks=2`），短测 `wr`/`rd` 触不到。

#### 修法

`allocator_close_sb` 把 `sb->valid = 0U;` 提到拷贝**之前**，与重建路径收敛到同一约定。
两处调用者（`allocator_alloc_pair` 写满关闭 / `allocator_close_open_sb` 显式关闭）语义不变。

#### 顺带：`test /ftl/` 恒「NO test run」

munit 的过滤（`tests/framework/munit.c`）要求过滤串 = **套件累积前缀 `pre` + 用例名前缀**。
用例挂在**叶子**套件下，其 `pre`（如 `/ftl/allocator/`）比 `/ftl/` 长 ⇒ 第一条 `strncmp` 即失配。
⇒ 只有叶子全路径（`/ftl/allocator/`）或全路径 + 用例名前缀能命中。

`ut_run_all`（`tests/unit/ut_main.c`）新增深度优先收集「持有用例的套件」全路径，把「以 `/` 结尾」
的实参展开为其下全部叶子套件路径。展开幂等，命中不到叶子的串原样透传。⇒ `test /ftl/` 按直觉工作。

#### 判据

`make tests UT=FTL` / `make tests` 均 0 warning；上板 `test /ftl/` → 53/53 PASS
（`close_open_sb` 此前是必 FAIL 的一例）。

#### 附：为什么 UT 值得先跑

本缺陷由 `test_alloc_close_open_sb` 一例直接顶出来；同一套件的 `test_gc_victim_skips_open`
（`test_ftl.c` 手工置 `valid = 1U` 模拟 Open 条目）**早已把「`valid != 0` = 跳过」的
正确约定写进了断言**。两例一个描述约定、一个验证生产者，合起来夹住这条潜伏缺陷——
v0.66 那轮 53 例纯静态审查没能发现它，因为缺陷在**生产代码**里，用例本身看不出矛盾。

---

### v0.68 — GC 触发求值加节拍门（+ 触发日志走自己的节拍 + 进展计数）

**日期**：2026-10-14

**范围**：`include/ftl/ftl.h`（4 字段 + 2 常量）、`src/ftl/ftl.c`（`ftl_gc_evaluate`
节拍门 + 日志节拍、`ftl_gc_reclaim_one` 进展计数、`ftl_init` 初始化）。
**纯后台调度口径与观测，不改任何判据语义**。

#### 问题（板端顶出来的）

跑完 FTL L0 UT 后，板子开始刷屏：

```
[INF] [GC] trig=0x09 free=0 op=0 writes=0
```

`trig=0x09` = `GC_TRIGGER_WATERMARK | GC_TRIGGER_EMERGENCY`，`free=0` 与之一致。
`free=0 / op=0 / writes=0` 是**破坏性夹具的残留**（`ftl_fixture_reset` 调 `allocator_init`
不回填 Free Pool、并清零 `total_writes`，见 `test_ftl.c` 的文件头声明）——
这一半是既定代价，复位即消。

**但刷屏本身是生产缺陷**：`ftl_gc_evaluate`（`ftl.c`）挂在 Phase 3 的 IDLE/NORMAL 两条
路径上，而主循环 `while(1)`（`src/main.c`）**没有任何节拍** ⇒ 求值**每轮一次**。而
`trig != 0` 不是异常——`free_count < FREE_POOL_TARGET(32)` 正是 GC 的**正常工作区间**
（T1 的定义就是「低于水位」）。⇒ 任何把空闲池压在 32 以下的**持续写负载**都会让 NORMAL
逐轮打一行；GC 选不出 victim 的卡死态下则是每轮一行 + 每轮一次全表扫描。

#### 根因：求值点没有节拍

`gc_check_triggers` → `gc_table_extremes`（`gc.c`）是**全表扫描**：

```c
for (uint16_t b = 0U; b < FTL_BLOCK_TOTAL; b++)   /* 2048 × PLANE_NUM(2) = 4096 块 ≈ 80KB */
```

T5 判据要全表磨损极值，所以这次扫描无法避免；但**它的频率不该等于主循环频率**。

⚠ **「主流按节拍求值」是错的**——本 ADR 初稿如此断言（"主流主控的后台 GC 跑在有节拍的
调度单元上，没有哪家每个 pass 无条件求值"），**已订正**：SPDK FTL 的 `ftl_core_poller`
注册周期就是 `0`（`lib/ftl/mngt/ftl_mngt_misc.c`：`SPDK_POLLER_REGISTER(ftl_core_poller,
dev, 0)`）＝连续轮询、**每轮求值**。它能这么做是因为判据是 **O(1)**：`ftl_needs_reloc()`
（`lib/ftl/ftl_core.c`）只做一次 `dev->num_free <= ftl_get_limit(SPDK_FTL_LIMIT_START)`
比较（默认 5 个自由 band，`lib/ftl/utils/ftl_conf.c`）。本仓 T5 是 O(4096) ⇒
**门控是成本补偿，不是向主流的节拍对齐**。

本仓既有同族做法：`rr_last_scan_ms`（1 s 巡检）、`wear_flush` 的节拍门、`cp_timer_start`（10 s）。

#### 修法

① **节拍门**：`ftl_gc_evaluate` 入口按 `GC_EVAL_INTERVAL_MS(10 ms)` 门控，per-instance
记 `ftl->gc_eval_last_ms`。`0` 作「尚未求值」哨兵——平台时钟缺失时 `ftl_now_ms()` 恒 0，
此时每轮都求值（**GC 不能因缺时钟而停摆**；与 T3 定时「恒不触发」的取舍方向相反）。

② **日志走自己的节拍**：`[GC] trig=` 按 `GC_LOG_INTERVAL_MS`（1 s，与求值同一套时基，
`ftl->gc_log_last_ms`）打。**求值节拍不能兼作日志节拍**——前者管「多久算一次」，10 ms 拍
仍是 100 行/秒；日志要的是「同一状态别重复报」。语义 = **进入触发态立即一行 + 触发态内
每 1 s 一行心跳**：退出触发态时归零该字段，故下次进入必然立即打（**不需要另存触发掩码**，
早期草案的 `gc_trig_last` 已废弃）。心跳是刻意保留的——稳态静默是对的（T1 是常态），
但「一行之后再无消息」会被误读成已恢复。

③ **心跳行带进展计数**：`ftl_gc_reclaim_one` 内自增 `gc_reclaimed_count`（成功回收一个 SB）
/ `gc_novictim_count`（触发但选不出 victim），两计数进 `[GC] trig=` 行。
**为什么**：`trig=` 常亮对「在推进」与「卡死」一视同仁，唯一的进展证据 `[GC] victim`
（`gc.c`）只在选到 victim 时才打——卡死时**根本不出现**，其"消失"又与"已收敛"**同形**
⇒ 只有计数能一行判读。⚠ 这不是假想：`allocator_close_sb` 的 valid 顺序缺陷（**cf604ba**）
正是"触发常亮且永不收敛"的**生产形态**——运行期关闭的 SB 对 GC/WL 永久不可见 ⇒
`gc_select_victim` 恒选不出 victim ⇒ 空闲池只出不进 ⇒ `free_count` 掉到 0 后永不回升、
**写最终永久失败**。

#### 代价（如实登记）

T1/T4 的空间压力响应也被门到同一周期 ⇒ IDLE 补池突发与 NORMAL 的「一轮回收一个 SB」
最坏延迟 10 ms。真卡死仍由写路径的按需 GC 兜底（`ftl_issue_subop`），且 10 ms 远小于
搬一个 SB（128 页）的 NAND 时间，实际不构成瓶颈。若日后要更快的空间响应，正确做法是
**把 T1/T4（O(1)）与 T5（O(N)）拆开**：便宜的每轮求值、贵的上节拍——本版未做。

#### 判据

`make all` / `make tests` 均 0 warning。⚠ 体积按 **v0.68 提交本身**实测：product
**192976 B** / 全套 **322980 B**（早先记的 193804 / 323968 含随后那轮排查临时加进树的
诊断探针——`[F2] judge/blk/skip`、`[L2P] r=`——那些探针已在 v0.69 删除，不属于本改动）。
上板判据：① 跑完 FTL L0 UT 后**不再刷屏**（卡死态是 1 行/秒心跳，改前是全速刷）；
② 正常写负载下进入水位立刻一行、之后每 1 s 一行、离开水位后静默；
③ **卡死可辨**——卡死态 `reclaimed` 不动而 `novictim` 持续上涨；正常回收则 `reclaimed` 每行都涨。

---

### v0.69 — 修 `ftl_is_metadata_block` 的 plane 掩码：被掩的块落在所有池之外，L2P 悬空撞断言

**日期**：2026-10-14

**范围**：`include/ftl/metadata.h` + `src/ftl/metadata.c`（判据加 `plane`）、`include/ftl/bst.h`
（`ftl_gblock_plane/block` 单点解码）、`src/main.c` / `tests/unit/test_ftl.c`（调用点）、
`src/ftl/power_failure_recovery.c`（`page_is_blank` 读失败判非空）、`include/ftl/journal.h` +
`src/ftl/journal.c`（`journal_replay` 回填实际回放条数）、`src/controller/cli.c`（usage 只在参数个数非法时打）。

#### 问题（板端顶出来的）

板端**重启后写一次**就撞断言：

```
[ERR] [FAULT] previous assert: b->valid_count > 0U (include/ftl/bst.h)
```

`ftl_block_sub_valid` 的断言 ⇒ 覆盖写时对**旧映射所在块**减有效计数，而该块的有效计数是 0。
即：**L2P 指向了一个 BST 认为「零有效」的块**。

#### 根因：元数据块判据只比 per-plane 块号

`ftl_is_metadata_block` 三分支里，① 固定分区**与 plane 无关**（分片模型下各 die 用同一份块号），
但 ② 当前日志块 / ③ 已登记日志块是**从用户区动态取的**，旧实现一律只比 per-plane 块号：

```c
/* 旧：cur_metadata_block 是 gblock，却只比了它的低 11 位 */
if ((md_dir->cur_block_valid != 0U) && (block == (uint16_t)(md_dir->cur_metadata_block % BLOCKS_PER_PLANE)))
...
if ((entries[i].type == METADATA_TYPE_L2P_REGION) && (paa_to_block(entries[i].paa) == block))
```

⇒ **一个 plane 的日志块会连带把另一 plane 的同号块一起排除**。被掩的块：

- 进不了 Free Pool（`main.c` 回填用同一判据）；
- 被 F2 重建跳过（`ftl_rebuild_closed_sb_list` 用同一判据）⇒ `valid_count` **恒 0**；
- 而日志块会**迁移**（`metadata_dir_reclaim` / `metadata_dir_compact`），掩码块号随之漂移
  ⇒ 历史上只要有一刻它没被掩住、被 allocator 当用户块发出去写过数据，L2P 就指向它。

⇒ 此后**每次**覆盖写该 LBA 都会撞断言，而重启本身看不出任何异常——这正是
「只在重启后、且必须写一次才炸」的形态。

⚠ **原判「偏保守、安全，不修」是错的**（`ftl_optimization_plan.md §七.7` / §8.3 已订正）：
被掩的块**落在所有池之外**（既不在 Free Pool、也不在 Closed），GC 永远不会碰它 ⇒ 数据确实
不会被擦掉；但 `valid_count` 恒 0 这件事**本身就是断言条件** ⇒ 不是保守，是定时炸弹。

#### 修法

① **判据加 `plane`**：`ftl_is_metadata_block(md_dir, plane, block)` —— ① 固定分区分支**不看**
plane，②③ 两条动态分支**必须**带 plane 比较。调用点三处：`main.c` free pool 回填、
`power_failure_recovery.c` F2 重建、`tests/unit/test_ftl.c` 夹具。
顺带在 `bst.h` 加 `ftl_gblock_plane/block`（`ftl_gblock` 的逆）做**单点解码**，
`metadata.c` 原有的 `gblock / BLOCKS_PER_PLANE` 内联算式改为调用它。

② **`page_is_blank` 读失败判「非空」**（同一类「F2 静默漏块」的另一支）：
`ftl_sync_read_page_raw` 刻意不判定 NAND 失败，返回 `FTL_ERROR` 只代表**命令**没走通
（提交失败 / 超时 / 无 buffer），此时契约规定 buffer 内容**未定义**。旧实现 memset 成 0xFF
后忽略返回码 ⇒ 读失败页被判空白 ⇒ `scan_block_write_pointer` 对该块返回 0 ⇒ 块被当成
"没写过"**留在 Free Pool** ⇒ 既可能被 allocator 拿去覆盖活数据，又让 L2P 指向 valid_count
恒 0 的块。判「非空」的代价只是真为空的块被拉出 Free Pool 多擦一次——不丢数据。

③ **`journal_replay` 回填实际回放条数**（`out_entries`）：旧实现用 journal 的 RAM 写指针
反推（`current_entry > 0` → `current_page × ENTRIES_PER_PAGE + current_entry`）——**恒假**：
`journal_replay` 末尾必调 `journal_resume`，后者把落点放到最新页之后并清 `current_entry`。
于是三条启动路径的 `journal_entries` 全是 0，该观测口不具判别力。改为在**应用循环**里逐页
累加 `entry_count`（读失败的页不计）。

④ **CLI 只在参数个数非法时打 usage**（`cli.c`）：旧写法把 `handler(...) != 0` 并进同一个
`||`，于是 handler 返回「执行失败」也被当成"参数错" ⇒ `wr 0 32` 在 busy 时会紧跟一条
`usage: wr <lba> <n>`，看着像语法错误、实际参数完全合法（wr/rd/tr/fl 四个命令的 busy 分支全中）。

#### 上板证据（定位用的临时探针，结论已入档，探针已删）

| 探针 | 结论 |
|---|---|
| `[F2] judge page_seq=169 entry_count=32 sum=201 cp_jseq=169` | `sum(201) > cp_jseq(169)` ⇒ 判型**正确**走掉电路径，不是 v0.64 那个 off-by-one 假绿 |
| `[L2P] r=0 dir=1 b=322 pl=0 pg=8 … e=00027a00..03` | 条目解出来是 plane0 block317 page0 cw0..3，与 `[F2] blk b=317 ok=4` 严丝合缝 ⇒ **L2P 不是垃圾** |
| 5 条 `[F2] skip … bad=0 meta=1`（块 16/48/49/58/62 + 322） | 全落在固定分区 / 活日志块 ⇒ **判据无假阳性**（且 5 条全是 `bad=0`，坏块支也干净） |
| `[F2] rebuild blocks=8 partial=8 valid_cw=39` | 4(b317)+16(b321)+3(p1 b63)+16(p1 b2043) = **39**，与各块 ok 计数一分不差 ⇒ 重建**完整自洽** |

**旁证**：`free=3715` 反证 plane 修复已在运行镜像里——4096 − 126（固定 63 块 × 2 plane）−
1（活日志块）= 3969 = 3715 + 204 + 50；若判据还是旧的只比块号，活日志块 322 会连 plane1
的同号块一起掩掉 ⇒ 3968 ⇒ free 会是 3714。

#### 保留的守卫（非临时探针，勿删）

`ftl_confirm_write` 里保留一条**常驻守卫**：覆盖写时若旧块 `valid_count == 0`，打一行
`[FTL] dangling lba=… old=… g=… st=… sb=… newg=… nst=…` 再进断言。它是这一类缺陷的
**唯一故障点**，且带旧块 `state` ⇒ 一次归因：

| `st` | 含义 |
|---|---|
| `0x01` BAD | 坏块，重建按判据跳过（数据本已不可读） |
| `0x08` FREE | 已归还空闲池 ⇒ 块还会被 allocator 复用，**丢数据** |
| RESERVED | 元数据日志块 ⇒ 判据过宽（v0.69 已修 plane 掩码） |
| `0x0000` | 重建从未认领（页 0 判空 / 未扫描到） |

代价 = 每次覆盖写多一次比较；收益 = 把「重启后写一次就死机」变成一条可读日志。

#### 判据

`make all` / `make tests` 均 0 warning（product 194196 B / 全套 324608 B）。
上板：① 重启后跑写负载，不再出现 `[FTL] dangling` 与 `bst.h` 断言；② `free` 比修复前**多 1**
（每个活日志块不再白掩一个同号用户块）；③ `boot type=N journal_entries=N` 的条数不再恒 0。

### v0.70 — 断言记录是陈旧的：加 `map <lba>` / `crash [clear]` 两条诊断 CLI

**日期**：2026-10-14

**范围**：`src/controller/cli.c`（两条命令 + 命令表）。**纯新增诊断口，不动任何算法路径。**

#### 问题：v0.69 上板后仍打印同一条断言

v0.69 修完 plane 掩码上板，重启后**仍然**看到：

```
[ERR] [FAULT] previous assert: b->valid_count > 0U (include/ftl/bst.h)
```

#### 结论：这条记录来自 v0.69 之前的镜像，是**陈旧记录**而非新崩溃

四条证据，任何一条都足以排除"当前代码还在崩"：

| # | 证据 | 含义 |
|---|---|---|
| ① | `.noinit` 是 `NOLOAD` 段（`linker/stm32h743xx_flash.ld`），启动**不清零** | 记录跨**软复位**保留；只有掉电才没 |
| ② | `crash_record_clear()`（`include/common/crash.h`）**无任何调用者** | 没有任何路径会清它 ⇒ 一次断言被此后**每次**复位反复重放 |
| ③ | 当前树里该断言在 **`bst.h`**（v0.69 新增的 `ftl_gblock_plane/block` 把它推下去 11 行）；`debug.lst` 里三份 `ftl_block_sub_valid` 全是 `mov.w r2, #272` | 记录里的旧行号 **只能**来自 v0.69 之前的镜像 |
| ④ | 断电重启日志（`reset reason: POR PIN BOR`）**没有** `previous assert` 行 | POR 清 `.noinit` ⇒ 那是干净基线；且 `free=3715` 与 plane 修复后的口径一致 |

⇒ `previous assert` 这一行的**语义**是「上次复位前记录里的东西」，不是「这次启动崩了」。
在没有 `crash clear` 之前，它会被无限重放，看着像"还在崩"。

#### 缺口：没有一条来自当前代码的确认崩溃

用户**不知道如何复现**，我们也拿不出一次当前代码的崩溃现场。与其空等复现，不如把
「复现」从必要条件降成可选项——加两条 CLI，把现场直接读出来。

#### 修法：两条诊断命令

**`map <lba>`**（只读）：打该 LBA 的 L2P 映射 + **目标块**的 BST 条目 + 末行 VERDICT。

```
map lba=100 pba=00027a00 ch=0 ce=0 lun=0 plane=0 blk=317 pg=0 cw=0 g=317
    block g=317 state=0x0004 valid=4 sb=3 ec=12
    VERDICT: ok —— 覆盖写安全
```

- 判据：**L2P 指向某块 ⇒ 至少该 CW 有效 ⇒ `valid_count ≥ 1` 是不变量**。
  `valid_count == 0` 即悬空，与 v0.69 的 `[FTL] dangling` 守卫**同一判据**。
- `state` 用于一次归因（BAD=本不可读 / FREE=会被复用**丢数据** / RESERVED=判据过宽 /
  `0x0000`=从未认领）。
- 关键性质：**不需要复现崩溃**——若某个 LBA 在写之前就已经指向 `valid_count == 0` 的块，
  直接就能看到现场，而且能**事先**指出哪些 LBA 一写就死。

**`crash [clear]`**：查看 / 清除 `.noinit` 记录（`crash_record_get` / `crash_record_clear`）。

- 给一个**显式归零点**：清完再复位，记录重新出现才是真的又崩了。
- ⚠ 掉电（POR）本来就清 `.noinit` ⇒ 断电重启后看到的记录一定是新的，无需 clear。
- 副作用：`crash_record_clear()` 从"有声明无调用者"变为 CLI 可达。
- 子参数**严格**校验（同 `bbt`）：拼错就报错返回，不静默降级。

#### 判据

`make all` **196236 B** / `make tests` **326648 B**，均 0 warning。
上板：① `crash` 打出 `bst.h` 的完整记录 ⇒ 证实陈旧；`crash clear` 后复位不再出现；
② `map <lba>` 对写过的 LBA 出 `valid=≥1` + `VERDICT: ok`；③ 若日后真复现，
`map` 与 `[FTL] dangling` 两条线应指向同一个 `g`。

### v0.71 — `crash` 上板验证通过 + 一条未闭合的 HardFault 记录（纯文档）

**日期**：2026-10-14

**范围**：`docs/design/common/fault_capture.md`（补 FPU 帧排布的出处）。**无代码改动。**

#### v0.70 上板验证：通过

```
crash clear
crash record cleared
→ 复位
=========== soft_ssd rev=24 built Oct 14 2026 13:51:19 ===========
[INF] [FAULT] reset reason: PIN (rsr=0x00460000)
（无 previous crash / previous assert 行）
```

清除前有记录、清除后复位不再出现 ⇒ **`crash clear` 按设计工作**，
且再次确认 `.noinit` 记录确实跨软复位保留。

#### ⚠ 但记录内容不是断言，是 HardFault

清除前那一行是：

```
[ERR] [FAULT] previous crash: HardFault PC=0x2400038c LR=0x24000b09 CFSR=0x00000001 HFSR=0x40000000
```

**与 v0.69/v0.70 讨论的 `bst.h` 断言是两个不同的签名**。记录是覆盖式的（后写的赢），
所以在那条 assert 之后，板子**真的又崩过一次**——这次是 CPU 故障，不是断言。

**解码**：

| 字段 | 值 | 含义 |
|---|---|---|
| `CFSR` | `0x00000001` | `MMFSR.IACCVIOL` —— **取指访问违例** |
| `HFSR` | `0x40000000` | `FORCED` —— MemManage 被升级成 HardFault |
| `PC` | `0x2400038c` | 落在 `.bss`（AXI_RAM）内 |
| `LR` | `0x24000b09` | 同样落在 `.bss` 内 |

⇒ CPU **试图从数据区取指**。两个独立寄存器同时指向数据区，典型是
**函数指针 / 返回地址被写坏**（控制流被破坏，不是数据被破坏）。
按当前镜像的符号表，两个地址都落在 `g_wp_buf`（`0x240002a0`–`0x240012a0`，4 KB，
`wp` 命令的写源缓冲）范围内。

**先决核实（否则上面全是假证据）**：故障处理函数从压栈帧取 PC/LR 的取法必须是
**基本帧在低地址、SP 指向 R0**。若 FPU 扩展帧的排布反过来，`frame[6]` 读到的是 `S6`
而不是 PC —— PC/LR 就成了浮点寄存器垃圾，`.bss` 之类的推论全部作废。
**已对照 Zephyr `arch_esf` 核实取法正确**（见 `common/fault_capture.md` §5.1），
⇒ 记录里的 PC/LR **是真值**，可以当线索用。

#### 未闭合的部分（登记在案，不再往下查）

- **不知道何时/如何触发**。用户不知道复现步骤；`cp_seq` 从 106 涨到 177 说明板子空转过
  十来分钟，崩在空转还是崩在负载里**未定**。
- **当场 dump 没留下**。HardFault 发生时固件会立刻打一整块 `=== CPU FAULT ===`
  （PC/LR/SP/xPSR/R0-R3/CFSR 位名/**栈扫描的候选返回地址**），那块才是定位的关键；
  本次没有保留。
- **操作顺序反了**：先 `crash clear` 才复位 ⇒ 详细记录（`expr`/`file` + 完整故障帧）
  已被抹掉，启动横幅只剩 PC/LR/CFSR/HFSR 四个字段。

#### 下次复现时的动作（按顺序）

1. **先打 `crash`**（看详细记录），**再** `crash clear` —— 顺序不能反。
2. 保留**当场**的 `=== CPU FAULT ===` 整块日志（含栈扫描）。
3. 用 `map <lba>` 与 `[FTL] dangling` 交叉对照：若两者指向同一个 `g`，
   则与 v0.69 的 plane 掩码属同一条线；若不相干，则是独立问题。

#### 可选加固（未做）

启动横幅的故障行只打 4 个字段，逼着人必须依赖"当场 dump"。可复用现成的
`fault_decode_cfsr` / `fault_decode_hfsr` 把 `SP/xPSR/MMFAR/BFAR` + CFSR/HFSR 的
**位名解码**一并打进开机日志，让单条启动日志自足。**未排期。**

---

### v0.72 — 结构重整：地址与布局的「归属归位」（纯文件组织，介质格式不变）

**日期**：2026-10-14

**范围**：`include/ftl/ftl_config.h`、`include/ftl/metadata.h`、`include/nand/nand_config.h`、
7 个 FTL 头的 include 行、`src/ftl/metadata.c`（并删 `src/ftl/ftl_config.c`）、
`src/fc/fc_interface.c` / `src/fc/fc_data_path.c` 各一行。
**不动算法路径、不动地址位序、不动介质格式 ⇒ `FTL_MEDIA_LAYOUT_VERSION` 保持 `3U`。**

#### 起因：三处「谁拥有这个事实」的错位

判据 = `fc_nand_design.md §26` 的分层（HIL / FTL / FCC / DRAM；`src/fc/` + `src/nand/`
合起来才是行业的一个硬件 FCC）。结构重整收的是**功能定义分布不合理**，两处已修：

| # | 错位 | 决定性证据 | 修法 |
|---|---|---|---|
| ① | **元数据区布局**（池尺寸 / 块基址 / 分区表 / L2P region 打包公式）写在 `ftl_config.h` —— 而它是 `include/ftl/` 下**唯一不对应任何 `.c` 模块**的头 | 布局的运行期唯一消费者 `ftl_is_metadata_block` 在 `metadata.c` | 整体搬进 `ftl/metadata.h`，分区表下沉 `metadata.c`，删 `ftl_config.c` |
| ② | **PBA**（`nand_pba_t` + `PBA_*` + `pba_*` 全套）写在 `nand/nand_config.h` —— 而它是 FTL 的地址货币 | `src/nand/`、`src/fc/` 的真实代码对 `nand_pba_t` **零引用**（只有注释）；且 `ftl_config.h` 已定义 `pba_page()` 而同族的 `pba_page_base()` 却留在 `nand_config.h` —— **同一访问器家族被劈成两半** | 整体搬进 `ftl/ftl_config.h` §三.2 |

#### ① 元数据区布局 → `metadata.h`（`998d59d`）

- 池尺寸宏、**全部基址宏**、逻辑表字节、`ftl_partition_t` / `g_ftl_partitions[]`
  声明、打包公式 `ftl_l2p_region_placement()`、池几何与不重叠 `_Static_assert`
  → `metadata.h`（「固定元数据区布局」）。
- **基址宏必须与打包公式一起搬**：`ftl_l2p_region_placement` 依赖 `L2P_BASE_BLOCK`；
  若基址留下，就得让 ftl_config.h include metadata.h，而成环。
- **唯一一处实质解耦**：`metadata.c` 原先 include `ftl/l2p_cache.h`，只为拿
  `ftl_l2p_region_placement` **一个符号** —— metadata 为一个**布局公式**依赖
  **L2P 缓存模块**，方向是反的。随搬动一并删掉。
- `ftl_config.h` 瘦身为「§一 几何 + §二 自检 + §三 地址编码」。
  ⚠ **内部编号未重排 ⇒ §三 就是旧 §八**（全文 `§八` 引用已改为 `§三`）。

#### ② PBA → `ftl_config.h` §三.2（`797c473`）

`ftl_config.h` §三 扩成「地址编码（LBA 侧 + PBA 侧）」：拓扑位宽、全部 `PBA_*_SHIFT` /
`_MASK`、`nand_pba_t`（**名字不变**）、`NAND_PBA_INVALID`、`pba_to_*` / `pba_from_addr` /
`paa_to_pba` / `pba_page_base` / `pba_with_*` / `pba_same_page` / `pba_is_valid` /
`addr_from_pba`，以及「三套地址位序为什么不同」的推导注释整段随迁。
`nand_config.h` 只剩**器件域**：几何 / ONFI 时序 / 字段位宽、PAA 与全部 `paa_*`、
`nand_addr_s`、`addr_from_paa`。

**唯一的非机械项**：`PBA_PLANE_SHIFT` 是 FC 与 FTL **共用**的低位编码（值恒等于
`CW_WIDTH`），FC 侧铸 `desc->meta` 也在用（`fc_interface.c`、`fc_data_path.c`）
⇒ 不能随 PBA 走。**改名 `PLANE_CW_SHIFT`**（中性名）留在 `nand_config.h` 新增的
「共享低位编码」小节，与 `PLANE_CW_MASK` / `lba_to_meta` 同处；`PBA_PAGE_SHIFT` 随之
改从 `PLANE_CW_SHIFT` 推导（数值不变，仍 3）；`PLANE_CW_MASK` 改写为自包含
（不再引 `PBA_*`）+ 一条 `_Static_assert` 钉住「plane 位紧邻 cw 位之上、中间不留空档」。
**这是本次唯一动 `src/fc/` 的地方（两行改名）。**

⚠ **代价（已知并接受）**：`ftl_config.h` include 了 `fc/fc.h` ⇒ 需要 `nand_pba_t` 的
FTL 头（`bst.h` / `allocator.h` / `gc.h` / `pending.h` / `read_recovery.h` / `journal.h` /
`ftl_fc_interface.h`）补 `ftl_config.h` 后，**连带把 FC 层拉进这些头**。
若将来要收紧，拆一个只依赖 `nand_config.h` 的叶子头即可。
顺带：`include/common/ftl_types.h` **直接删掉 `nand_config.h`**（只在注释里提地址类型，
函数体一个符号都不用）；`l2p_cache.h` 的 `nand_config.h` 换成 `ftl_config.h`。

#### 验证（两次搬动都是纯组织调整）

| 判据 | 结果 |
|---|---|
| `git stash` A/B（HEAD vs 改后） | debug `.data` `0x1e4` / `.bss` `0x48b44` / `.bss.g_ftl` `0x43968`，bin 196224 B；release `0x1e0` / `0x48b3c` / `0x43968` —— **逐项相同** |
| 搬走的 `_Static_assert` 仍生效 | 临时把 `ANCHOR_POOL_BLOCKS` 改成 1 ⇒ 编译期如期失败 |
| 改名 / 改写项逐值等价 | 离线比对旧参考实现：`PLANE_CW_SHIFT` / `PLANE_CW_MASK` / `PBA_PAGE_SHIFT` 全等；`lba_to_meta`（全 plane×cw×抽样 LBA）、`pba_from_addr∘addr_from_pba`、`pba_to_paa∘paa_to_pba` 互逆 —— 全 **0 mismatch** |
| 分层自检 | `src/fc/`、`src/nand/` 无存活 `PBA_` 前缀宏；`nand_config.h` 无 `nand_pba_t` / `pba_` |
| `make format-check` | 过；debug / release 0 error |

⚠ **上板验证未做**（留待用户）。⚠ `tests/unit/test_fc_level3.c` 两处
`PBA_PLANE_SHIFT` 需同步改成 `PLANE_CW_SHIFT`（测试源用户自建）。

#### 顺带订正

`wear.h` 的全量快照页数注释写「17 页」是 v0.65 条目 8B→12B **之前**的旧值，
实为 ⌈4096 ÷ 168⌉ = **25 页**（`wear.h` 一直是对的）。

---

### v0.73 — FTL 层地址全面 PBA 化 + PBA 位域视图（介质格式变更，版本 3→4）

**日期**：2026-10-14

**范围**：`include/ftl/{ftl_config,ftl_fc_interface,metadata,allocator,bst,l2p_cache}.h`、
`include/common/ftl_types.h`、`src/ftl/{ftl,ftl_fc_interface,metadata,l2p_cache,
power_failure_recovery,read_recovery,bbt,journal,checkpoint,gc,wear,wear_leveling}.c`。
**不改 `nand_pba_t` 的类型**（仍是 `uint32_t`）；**不动 `src/fc/**` / `src/nand/**`**
（PAA 与 `nand_addr_s` 在那边照旧）。

#### 起因：FTL 的地址货币是 PBA，代码却到处借 NAND 层的展开式

| 现象 | 证据 |
|---|---|
| FTL 层 78 行提到 `nand_addr_s`（展开式 7 字段结构） | `metadata.c` 11 行、`ftl_fc_interface.c` 9 行、`journal.c` 8 行…… |
| FTL 层 ~42 行用 `nand_paa_t` / `paa_*` | `metadata.c` 25 行、`l2p_cache.c` 9 行、`bst.h` 3 行 |
| **PAA 被写进介质** | `metadata_entry_t.paa` → Anchor 页落 NAND |
| 绕圈子的地址转换 | `addr_from_pba(pba)` → `nand_addr_s` → `pba_from_addr()` → `pba_to_paa()`：同一趟打包再拆包（`addr_to_cmd`、`sync_page_prepare_cw`） |
| FTL 模块直接读 FC 的 PAA 字段 | `ftl.c` `ftl_paa_gblock(cmd->paa)` |

判据同 v0.72 = `fc_nand_design.md §26` 的分层：PBA 是控制器地址货币，PAA 是器件行地址。
本次把 v47 的「PBA 不跨进 FC」在 **FTL 内部也贯彻到底**：`include/ftl/**` + `src/ftl/**`
的地址一律 PBA，PAA 只在 `pba_to_cmd` / `ftl_cmd_pba` 这一处边界转出。

#### 提交 A — 位域视图 + FTL 边界原语改 PBA（`42689ed`，介质不变）

新增 `nand_pba_fields_s`（4 字节位域，位序与 `nand_pba_t` 一致，可强转）承担原先
`nand_addr_s` 的「具名字段构造地址」职责（`ftl_config.h §三.2`）：

- `pba_fields()` / `pba_pack()` 值语义访问器，**按 `PBA_*_SHIFT` 宏编解码**，与位域布局无关；
- `pba_make()` 具名构造（把 `LUN_ADDR_WIDTH == 0` 时不能声明 `lun : 0` 的 `#if` 收敛在一处）；
- `pba_fields_layout_matches()` 开机自检（`ftl_init` 里 `ASSERT`）——见下「两条硬约束」。

删除 `pba_from_addr()` / `addr_from_pba()` / `pba_to_nand_row()`。保留 `pba_to_paa()`
（唯一转换器，内部仍用 `nand_addr_s`，**不改**——重写位重排无收益纯风险）。

边界原语全部改收 `nand_pba_t`：`ftl_sync_write_page` / `_cw` / `read_page` /
`read_page_raw` / `erase_block` / `read_meta` / `read_column`；`addr_to_cmd` 改名
`pba_to_cmd` 并**去掉 `addr_from_pba`→`pba_from_addr` 往返**；新增 `ftl_cmd_pba()` 反向，
使 `ftl.c` 不再直接读 `cmd->paa`。`bst.h` 删 `ftl_paa_gblock()`，`ftl_pba_gblock()`
直接解 PBA（原绕 `pba_to_paa()`）。

#### 提交 B — Metadata Directory 改存 PBA（`e9ee83c`，**介质格式变更**）

`metadata_entry_t.paa` → `.pba`；四个签名（`metadata_dir_lookup` 返回、
`metadata_dir_update` / `metadata_dir_commit_region` 的 `new_paa`、
`metadata_dir_append_slot` 返回）改 `nand_pba_t`；`metadata.c` 全面去 PAA
（`paa_from_addr({…})` → `pba_make(ch,ce,lun,block,plane,page,0)`）；
删 `l2p_cache.c` / `power_failure_recovery.c` 的 `paa_to_pba()` 桥接。

**实质改善**：`append_slot` / `lookup` 返回的地址**自带 ch/ce**，不再依赖「本实例的元数据
一定在本实例 ch/ce 上」这个隐含假设（原先靠调用方 `paa_to_pba(cache->ch, cache->ce, ·)`
补齐）。等价性依赖 `cache->ch/ce/lun == ftl->topo == dir->topo`（`ftl.c` +
`l2p_cache_init(&ftl->cache, ftl->topo.*)`，已核实）。

⚠ **介质格式变更**：4 字节字段语义由 PAA 改 PBA（字节数不变，`metadata_entry_t` 仍 7B，
Anchor 页 `_Static_assert` 不受影响）⇒ `FTL_MEDIA_LAYOUT_VERSION` **3U → 4U** ⇒
**上板首启自动全片格式化（预期行为，不是回归）**。

`paa_to_pba()` 保留在 `ftl_config.h`：它是 `pba_to_paa()` 的逆，是 `ftl_cmd_pba()` 用的
边界转换器（与 `pba_to_paa` 同属「唯一转换器」，不在清零范围内）。

#### 两条硬约束（v25 否决位域方案的正是它们，本轮逐条给应对而非推翻）

见 `docs/archive/assertion_rounds.md` 的「已跟进（v0.73）」段：
① `LUN_ADDR_WIDTH == 0` ⇒ C11 6.7.2.1p4 禁 0 宽具名位域 ⇒ 视图里 `lun` 用 `#if` 隐去；
② 位域布局 implementation-defined、`offsetof` 非法 ⇒ **无 `_Static_assert` 可证**（复合
字面量的 union 读不是 C11 整型常量表达式，GCC 实测报 `expression in static assertion is
not constant`）⇒ 退化为 `ftl_init` 里一次性 `pba_fields_layout_matches()` 自检：逐字段置
单比特、双关读出，证明 `*(nand_pba_t *)&fields == pba_pack(fields)`。**值语义路径走
`PBA_*_SHIFT` 宏，与位域布局无关**——自检只保证「强转可用」这一条性质。

#### 验证

| 判据 | 结果 |
|---|---|
| `make debug` / `make release` | 0 error 0 warning；**debug 192592 B / release 114924 B**（A 后 193520 / 115148） |
| `make format-check` | 过 |
| 离线逐值等价（PBA 全域抽样） | `pba_pack∘pba_fields` 恒等、`pba_fields(p).X == pba_to_X(p)`、`pba_make ≡ 旧 pba_from_addr`、`ftl_pba_gblock` 新旧同值、`paa_to_pba` 新旧同值、`ftl_cmd_pba∘pba_to_paa` 恒等、`pba_fields_layout_matches()==1` —— **全 0 mismatch** |
| 提交 B 放置等价 | 全 (block,plane,page) 组合（2048×2×64）新旧构造逐位相同，**0 失配** |
| 分层自检 | `include/ftl/**` + `src/ftl/**` 的 `nand_addr_s` / `paa_*` 只剩 `ftl_config.h` 转换器 + `ftl_fc_interface` 边界 + 注释；`src/ftl/` 无 `cmd->paa` 读 |
| `.bss` | 不变（无新增静态存储） |

⚠ **上板验证未做**（留待用户）。⚠ **首启会看到格式化横幅（版本 3→4，预期）**，
格式化后再跑一轮确认稳定。⚠ 测试源需同步：`tests/unit/test_ftl.c`
（Directory `paa` 变量改 `nand_pba_t`）、（`metadata_ring_addr` 现返回 PBA ⇒
`a.block` 等改 `pba_to_block(a)`）；另有 v0.72 遗留的 `tests/unit/test_fc_level3.c`
（`PBA_PLANE_SHIFT` → `PLANE_CW_SHIFT`）。

#### 补记（同日）— `include/common/ftl_types.h` 并入 `include/ftl/ftl_type.h`

上述「范围」里列的 `include/common/ftl_types.h` **已不存在**：该文件内容全是 FTL 概念
（`pending_cb_fn_t` / `ftl_io_request_t`），却放在依赖图底层的 `common/`——它是那里**唯一**
include 上层头的文件（`common/` 其余 6 个头只 include libc），且与 `include/ftl/ftl_type.h`
**只差一个 `s`**，两个「FTL 类型头」并存是明确的误用陷阱。

- 内容并入 `include/ftl/ftl_type.h`（补 `#include "common/ssd_types.h"` 取 `ssd_status_t`）；
- 删 `include/common/ftl_types.h`；
- 3 处 include 改路径：`ftl/pending.h`、`ftl/ftl.h`、`controller/ssd_request.h`
  （controller 是 HIL，HIL→FTL 本就是 §26 的正确方向，不需要靠 `common/` 中立化）；
- 文档同步：`ftl_api_usage.md`、`../../archive/ftl_ut_munit.md`、`nand_config.h` 注释。

注释里「打破循环 include」的理由**与目录无关**——那是把 `pending_cb_fn_t` 从 `pending.h`
搬出来；文件搬到 `ftl/` **不产生环**（`ftl_types.h` 不 include `pending.h`，反之才是）。
纯头文件归置：debug 192592 B / release 114924 B 与改前**逐字节相同**，0 新警告。

#### 补记（同日）② — 死 include 清理 + `FTL_PBA_INVALID` 归并（同上「同类体检」的其余四条）

同一轮体检发现同类的还有四处，一并处理（纯 include/宏，二进制不变）：

| # | 位置 | 问题 | 处置 |
|---|---|---|---|
| ① | `fc/fc_types.h` | 自称「FC 层最小依赖集」，却 include `common/common.h` + `nand/nand_config.h`，**两个都零使用**（连 `<stdint.h>` 都没直接 include） | 改为只 include `<stdint.h>` |
| ② | `nand/nand_dispatch.h` | include `fc/fc_types.h` **零使用**，且方向是 **nand(下) → fc(上)** 的假层反依赖 | 删除该 include |
| ③ | `ftl/read_recovery.h` | include `nand/nand_config.h` **零使用** | 删除 |
| ⑤ | `ftl/l2p_cache.h` | `FTL_PBA_INVALID` 与 `ftl_config.h` 的 `NAND_PBA_INVALID` **同值同义**，两名并存 | 删 `FTL_PBA_INVALID`，全用 `NAND_PBA_INVALID` |

**①的实质收益**：`fc_types.h` 是 `fc_command.h` / `fc.h` / `ftl_config.h` 的上游 ⇒ 那个死
include 把 `<stdio.h> <stdlib.h> <string.h> <time.h> <ctype.h>` 灌进了整条链。实测
`gcc -E -H src/ftl/ftl.c` 的依赖里出现 `/usr/include/stdio.h`。

**②的连带发现（不是纯删）**：`fc_command.h` 一直靠 `fc_types.h` **顺带**拿到
`DWORD`（定义在 `common/common.h`）与 `PLANE_NUM`/`CW_NUM`/`paa_to_plane`
（定义在 `nand_config.h`）——这正是「隐式传递依赖」的典型。故：
- `DWORD` / `uint8…` / `BYTE…` 一组历史别名从 `common/common.h` **下沉到
  `common/ssd_types.h`**（只依赖 `<stdint.h>`；`common.h` 反过来 include 它，向后兼容）；
- `fc_command.h` 显式 include `common/ssd_types.h`（`DWORD`）+ `nand/nand_config.h`；
- `fc_interface.h` / `fc_data_path.h` 显式 include `nand/nand_config.h`。

**验证**：`src/ftl/ftl.c` / `src/fc/fc.c` / `src/ftl/metadata.c` 的预处理依赖里
`stdio.h` 命中 **0**；`include/common/` 恢复零跨目录 include；层图里 **`nand → fc` 边消失**；
`FTL_PBA_INVALID` 全清。debug 192592 B / release 114924 B 与改前**逐字节相同**，
0 新警告（release 仍只有既有的 `ftl_fc_interface.c` 两条）。

---

### v0.74 — OP / Spare 池改按 plane 预留（介质格式变更，版本 4→5）

**日期**：2026-10-14

**范围**：`include/ftl/{allocator,bbt,metadata}.h`、
`src/ftl/{allocator,bbt,ftl}.c`、`src/main.c`、`src/controller/cli.c`、
`tests/unit/test_ftl.c`。

#### 起因：两个池是全局的，回填却是 plane-major 的升序单循环

`gblock = plane*BLOCKS_PER_PLANE + block`（`bst.h`，plane-major），而
`main.c` 的回填是**单次升序 gblock 遍历 + 全局计数**。前 254 个非元数据好块
因此全落 plane 0：

| 池 | plane 0 | plane 1 |
|---|---|---|
| Spare 50 | 63..112 | — |
| OP 204 | 113..316 | — |
| 可用数据块 | 1731 | 1985 |

两个消费点 `allocator_pull_from_op`（`allocator.c`）与 `bbt_spare_pool_refill`
（`bbt.c`）都是**无 plane 参数**的 LIFO 弹出，直接 `allocator_free_block` 丢回
通用 free pool。后果：

- **plane 1 坏块无替换能力**：`bbt_mark_bad(alloc, gblock)` 手上有 gblock
  （可算 plane）却没往下传，补进来的是 plane 0 的块 ⇒ 不恢复该 plane 的配对能力；
- **plane 1 无 OP 兜底**：其 free pool 见底时 `allocator_pull_from_op` 拿不到
  plane 1 的块；
- 两 plane 可用容量不对称（1731 vs 1985），末寿命时 plane 1 有块配不成对。

写入/擦除/GC 的最小单元是 **super block**（每 plane 一块），预留必须按 plane 对称。
**注意这不是崩溃型缺陷**（账本自洽、不会写坏数据），是容量/寿命末端的可用性缺陷。

**已定口径**：OP = 每 plane 块数的 10%（2048×10% = **204/plane = 408 总量**，
floor；不接受 205/204 的非对称取法——对称正是本改动的目的）；
Spare = **每 plane 50（总 100）**。

#### 改动

| 文件 | 内容 |
|---|---|
| `allocator.h` | `FTL_SPARE_POOL_SIZE(50)` → `FTL_SPARE_POOL_PER_PLANE(50)`；新增 `FTL_OP_BLOCKS_PER_PLANE`(204) 与 `FTL_OP_POOL_CAP`（维度钳 ≥1，兜住 `FTL_OP_PERCENT=0`）；两数组升 `[PLANE_NUM][…]`；指针改 pointer-to-array、计数改 `uint16_t[PLANE_NUM]` |
| `allocator.c` | `allocator_pull_from_op(alloc, plane)`；`allocator_open_sb` 按 plane 拉 OP（去掉 `tries<PLANE_NUM` 计数循环——每次拉进的就是本 plane 的块，一次即命中）；`allocator_init` 两处计数改 `memset`（`= 0U` 对数组不编译） |
| `bbt.c/h` | `bbt_spare_pool_refill(alloc, plane)`；`bbt_mark_bad` 传 `ftl_gblock_plane(gblock)`（**消费点 2 的实际 bug 修复**） |
| `main.c` | 回填循环按 `plane = ftl_gblock_plane(gblock)` 路由到 per-plane 配额；开机日志 `free=%u op=%u/%u spare=%u/%u` |
| `ftl.c` / `cli.c` | GC 心跳与 `info` 同样逐 plane 显示 |
| `metadata.h` | `FTL_MEDIA_LAYOUT_VERSION` **4U → 5U**（理由见下） |

选**二维数组**而非展平 + helper：本仓已有 per-plane 数组先例
（`super_block_s.plane_blocks[PLANE_NUM]`），`op_blocks[plane][idx]` 自解释、
不需要新索引函数、也不会让 plane/idx 混用静默通过。

#### ⚠ 为什么必须 bump 介质版本（不是"池不落盘"能豁免的）

预留块集合变了——旧固件把 plane 1 的 2111..2364 当**用户数据区**（写过用户数据、
被 L2P 引用），新固件把它们当 spare/OP 预留。就地升级不格式化时：

- 这些块仍被 L2P 引用，而预留池的契约是「块已擦除」，`pull_from_op` /
  `spare_refill` 会把它们直接丢回 free pool ⇒ 下次写即覆盖活数据；
- 且 F2 重建会把"写过的块"也建 Closed 条目 ⇒ 同一块同时进 spare/OP 池与
  closed 列表，账本错乱。

bump 版本让 `ftl_media_needs_format`（`power_failure_recovery.c`）在首启自动
全片格式化一次——这是本仓既有的、取代手动 `bbt --erase` 的机制
（`metadata.h`「介质布局版本」段）。**首启看到格式化横幅是预期行为，不是回归。**

#### 代价（明确记录）

free pool 水位**降 254 块**（预留 254 → 508）。用户容量由 L2P region 数
（`FTL_LBA_MAX`）固定不变，但 GC headroom 变少：开机 `free=` ≈ **3462**（旧 ≈3716）。
这是"每 plane 10%"的既定代价。

内存反而省：OP 由 `[4096]×2B`=8KB 缩为 `[2][204]×2B`=816B，Spare 由 100B 变
200B ⇒ **净省 ≈7.3 KB**（`.bss` 290500、`g_ftl` 263.2 KB）。

#### 验证

| 判据 | 结果 |
|---|---|
| `make tests UT=FTL` / `UT=FC` / `make tests` | 0 error 0 warning |
| `make debug` / `make release` | 0 error 0 warning；`.bin` 192956 / 115136 B |
| `_Static_assert(sizeof(g_ftl)+20KB <= 384KB)` | 过（AXI_RAM 284.1 KB / 74.0%） |
| `.bss` | 290500（`g_ftl` 0x41CF8 = 263.2 KB），净省 ≈7.3 KB，与预测吻合 |
| 新增用例 | `op_fallback_per_plane`（plane 1 free 空 ⇒ 从 plane 1 的 OP 补出块）、`spare_pool_plane_isolated`（plane 1 的坏块只动 plane 1 的池）；8 套件 53 → **55 例** |

⚠ **上板验证未做**（留待用户）。上板判据：
① 开机日志 `op=204/204 spare=50/50`（修复前 `op=204/0 spare=50/0`）；
② `info` 的 `op=`/`spare=` 两个 plane 分量都非零；
③ 写若干 LBA 后逐个 `map <lba>`，`plane=` 字段应同时出现 0 和 1（plane 1 不再饿死）；
④ 首启打一次 `media layout mismatch -> format`，其后不再打。

⚠ 工具坑：`release.elf` 被 strip（`nm: no symbols`）⇒ 构建脚本的 SRAM usage 报告
对 release 恒为 0.0 KB，是既有行为；判断 release 内存只看 `size` 表的 `.bss`。

---

### v0.75 — 命令「提交」与「完成」解耦（阶段 1 机制 + 阶段 2 Journal 刷页异步 + 阶段 3 Read Recovery 异步 + 阶段 4 GC / WL 搬移异步 + 阶段 5 L2P region 加载写回异步 + 阶段 6 Checkpoint 保存异步 + 阶段 7 BBT / Wear 持久化异步 + 阶段 8 metadata 目录压缩 / 换块异步）

> **本版八阶段至此全部落地**，`g_op_service[]` 的 12 个槽（`[1]`~`[11]`）**全部点亮**，
> 全仓不再有「在完成泵内同步等 NAND」的路径。同步原语 `ftl_sync_submit_wait` 只剩
> **结构性顺序**的调用者：开机初始化 / 全片格式化 / 出厂坏块扫描 / 开机回放 / CLI。

**日期**：2026-10-14

**范围**：`include/fc/fc_command.h`、`src/fc/fc.c`、新增 `include/ftl/ftl_op.h` +
`src/ftl/ftl_op.c`、新增 `include/ftl/migrate.h` + `src/ftl/migrate.c`、
`include/ftl/{completion_table,ftl,ftl_fc_interface,journal,metadata,bst,pending,
read_recovery,gc,wear_leveling,ftl_type,l2p_cache,checkpoint}.h`、`src/ftl/{ftl,ftl_op,
completion_table,ftl_fc_interface,journal,bbt,checkpoint,gc,migrate,l2p_cache,metadata,
pending,power_failure_recovery,read_recovery,wear,wear_leveling}.c`、
`src/controller/{controller,scheduler,cli}.c`、`tests/unit/*`。

#### 起因：FTL 里只有 host 读写是异步的，其余全是「提交后当场等」

约 38 处 NAND 访问走同一条阻塞原语：

```
ftl_sync_write_page / _cw / ftl_sync_read_page / _raw / ftl_sync_erase_block / ...
   → sync_page_submit_wait()
      → fc_submit(cmd)
      → ftl_internal_sync_wait(cmd_id, …)
         for (;;) { fc_service(); completion_table_drain(); if (waiter.done) break; }
```

`ftl_internal_sync_wait` 是**嵌套泵**——它在 completion 派发循环里**再调一次**
`completion_table_drain()`。于是出现这样的路径：

- host 写完成 → `ftl_confirm_write` → `journal_append` → 页满 → `journal_flush_page`
  → `ftl_sync_write_page` + 换块 `ftl_sync_erase_block` ⇒ **在完成回调内部又跑一遍完成泵**；
- host 写完成 → `ftl_confirm_write` → `l2p_cache_read` → region 未命中 →
  `l2p_cache_find_or_load` → `ftl_sync_read_page` ⇒ 同上重入（⚠ 写路径**下发时从不读 L2P**
  ⇒ 这是首次触碰的真 miss，不是罕见分支）；
- host 读路径每个未命中 region 阻塞整条流水线；Read Recovery 在读失败的完成回调里
  同步等待 SET_FEATURES / 重读。

结构性后果：调用者栈帧被钉住，GC / WL / L2P 回写 / BBT 持久化 / Checkpoint 都无法与
host I/O 重叠，`ftl_service` 的三级后台模式实际退化成「一次只做一件事」。

#### 目标（用户 2026-10-14 定）

1. **提交即返回**：`fc_submit` 成功即返回，不等命令回来。
2. **完成统一在回调里处理**：在 `ftl_service_pump` Phase 2 的完成泵里，按命令携带的
   **op ID** 查**服务函数数组**，执行对应服务函数。
3. **范围**：按主流存储主控的做法，**该异步的功能都异步**；只有结构性必须顺序执行的
   （开机初始化 / 全片格式化 / 出厂坏块扫描 / CLI）保留同步。

#### 阶段 1 —— 机制（**行为完全等价**，是后续所有阶段的基线）

##### ① FC 侧：`module : 4` → `op_tag : 7`

`fc_cmd_s` 的 `module : 4` 吸收紧随其后的 `reserved1 : 3`，成为
**不透明 7 位 `op_tag`**；`fc_cmd_status_s` 同步改名（`reserved : 20` → `17`）。
**不新增字段、不改结构体大小、不落介质**。FC **不解释、不分支**，只在
`fc_complete_from_nand` 里原样拷进 completion。删除 `fc_command_module_t` ——
它是 FTL 概念，不该由 FC 声明（全仓对它的唯一两处使用都只是拷贝；RAID parity 分支
`CONFIG_FC_RAID=0` 编译不进来，一并改对以免将来启用时静默漏字段）。

##### ② 新增 `ftl_op.h/c`：op tag + 服务函数数组

```c
typedef enum { FTL_OP_NONE, FTL_OP_SYNC_WAIT, FTL_OP_HOST_IO, FTL_OP_JOURNAL,
               FTL_OP_READ_RECOVERY, FTL_OP_GC, FTL_OP_WL, FTL_OP_L2P,
               FTL_OP_CHECKPOINT, FTL_OP_BBT, FTL_OP_WEAR, FTL_OP_METADATA,
               FTL_OP_COUNT } ftl_op_id_t;
_Static_assert((uint32_t)FTL_OP_COUNT <= 128U, "…必须装进 op_tag:7");
```

- `g_op_service[]`：**C99 designated initializer 的 `const` 数组**——编译期即可发现重复/
  越界，无 init-order 依赖。未列出的槽恒 NULL（= 孤儿 tag）。
- `ftl_op_submit(cmd, op, ctx)`：打 tag → 登记 ctx（**早于 `fc_submit`**，同 `pending_add`
  的先例）→ 提交；提交失败**回滚登记**并返回 `FTL_AGAIN`。`ctx == NULL` 时主动
  `completion_table_remove`（cmd_id 是池下标、会复用，清残留是廉价保险）。
- `ftl_op_dispatch(completion)`：按 `completion->op_tag` 查数组 → 越界/NULL ⇒ 孤儿
  （remove + release + LOGE）→ `completion_table_take(cmd_id)` 摘 ctx（**先摘链再回调**，
  容许服务函数重入登记）→ 按**命令自身**的 `config.ch/ce/lun` 经 `completion_table_route`
  解析实例（**不是「谁在 drain」**；单线程顺序主循环下「实例 A 的 drain 遇到实例 B 的
  completion」是必然事件；开机早期 `g_route` 未 set ⇒ NULL 是正常状态）。

##### ③ op tag 的语义：**完成服务函数选择器**，不是「这是哪个功能」

同步提交的命令一律带 `FTL_OP_SYNC_WAIT`，因为阻塞调用方的完成处理函数**就是**唤醒等待者；
Journal 的收尾（推进写指针、换块擦除）是**调用者在阻塞返回之后自己做的**。若给同步命令打上
`FTL_OP_JOURNAL`，服务函数与调用者会对同一份状态**各推进一次**。
⇒ 枚举与数组在阶段 1 **一次性建全**（满足「各功能一个 ID」），各 ID 随其站点改造逐个点亮。

##### ④ `completion_table` 瘦身

表项 `{used; cmd_id; handler; ctx}` ⇒ `{used; cmd_id; ctx}`（32 项 ⇒ **-128 B**）。
新增 `completion_table_take`；删 `lookup` / `set_default` / `g_default_handler` /
`ftl_sync_waiter_handler(_with_release)`。`drain` 的唯一出口 = `ftl_op_dispatch`。

##### ⑤ 派发点唯一化 + 同步原语改机制客户端

- `ftl_handle_completion` 的 `switch (completion->module)` 整体删除，FTL 分支原样搬成
  `ftl_op_host_io`，填进 `g_op_service[FTL_OP_HOST_IO]`。
- `ftl_internal_sync_wait` + `sync_page_submit_wait` 删除，换成公共
  `ftl_sync_submit_wait(cmd, &st, timeout_us)`（内部走 `FTL_OP_SYNC_WAIT`）。
- **命令 release 归属**写进 `ftl_op.h`（生命周期的唯一权威）：`SYNC_WAIT` **不** release
  （调用者读完 cmd 数据才释放，否则悬垂）；`HOST_IO` 与各 job **自己** release。
  `fc_command_release` 幂等，但漏则命令池泄漏 ⇒ `fc_command_alloc` 恒 NULL ⇒ 全系统
  `FTL_AGAIN`。

##### ⑥ `ftl_page_cfg_s.module` / `metadata_ring_t.module` 删除

同步路径上 `module` 只是诊断标签、**零读者**，且阶段 1 后必被 `SYNC_WAIT` 覆盖 ⇒
`FTL_PAGE_CFG_META()` / `_DATA(lba)` / `_DATA_ANY()` 去形参；`metadata_ring_t.module`
及其三处赋值一并删。

#### 阶段 2 —— Journal 刷页异步（本次）

**前提（已核实，本阶段成立的依据）**：Journal 是 **write-back**（`journal_append` 只写 RAM
`page_buf`，页满才落 NAND），且本仓**无电容** ⇒ 今天 acked 写就已经容忍「最多丢一页 journal
条目」。把刷页从「页满时立刻阻塞刷」改成「页满时交给 job 后台刷」**不改变掉电模型**。

##### job 状态机（per-instance，放进 `l2p_journal_t.job`）

```c
typedef enum { JRN_IDLE = 0, JRN_WRITE, JRN_ERASE } journal_phase_t;

typedef struct {
    uint8_t    phase;        /* journal_phase_t */
    uint8_t    need_erase;   /* 有**未起**的擦除：target_block 待擦 */
    uint8_t    target_block; /* 页写成功后 current_block 的落点 */
    uint8_t    target_page;  /* 页写成功后 current_page 的落点（已做块内回绕） */
    uint8_t    from_block;   /* 起写时的 current_block（防指针回退的守卫） */
    uint8_t    from_page;
    nand_pba_t waddr;        /* 在飞页地址（诊断 / 心跳） */
} journal_job_t;
```

##### 新增异步页级原语（`ftl_fc_interface.c`）

`sync_page_prepare` 拆成 `page_prepare_alloc(operation, cfg)`（只 alloc + config，
NULL = 池空）+ 地址/建链两步，供同步与异步共用模板：

```c
ftl_status_t ftl_async_write_page(fc_cmd_s **out_cmd, nand_pba_t pba,
                                  const void *data, uint32_t length, const ftl_page_cfg_s *cfg);
ftl_status_t ftl_async_erase_block(fc_cmd_s **out_cmd, nand_pba_t pba);
```

**只建链 + 搬数据，不提交**；命令交回调用者，由调用者 `ftl_op_submit(cmd, <功能 op>, ctx)`。
`FTL_AGAIN` = 池空（瞬态背压）。⚠ 调用者必须 submit-or-release，漏则命令池泄漏。

##### 流程

- `journal_append`：**先做同 LBA 合并扫描**（页满时同 LBA 合并仍须生效）→ 页满守卫 →
  写槽。页满 ⇒ `journal_freeze`（写页头、`ftl_async_write_page`、`ftl_op_submit(cmd,
  FTL_OP_JOURNAL, journal)`、清页缓冲）。**派发上下文里 freeze 失败不阻塞、不同步回退**，
  改丢弃该条目 + LOGE（见下）。
- `journal_kick`（IDLE 且有待办才起）：`need_erase` 优先 ⇒ `journal_start_erase`；
  否则 `current_entry != 0` ⇒ `journal_freeze`。
- `ftl_service_idle/normal` 调 `journal_kick`；`ftl_service_busy` 只在
  `phase == IDLE && need_erase` 时 kick（**不起新页写**——BUSY 档的页满由
  `journal_append` 自己起，这里再起就违背「BUSY 只做防饿死的最小后台」的档位约定）。
- `ftl_op_journal`（`g_op_service[FTL_OP_JOURNAL]`，`ctx = &ftl->journal`）按 `job.phase`：
  - **JRN_WRITE 成功** ⇒ 推进写指针（**带守卫**：只有 `current_block/page` 仍等于起写时的
    `from_block/from_page` 才推进，防「drain 超时 → 同步刷 → 异步完成回退指针」）⇒
    `need_erase` 且非 `pre_erased_block` ⇒ 起擦除（`JRN_ERASE`）；否则 `IDLE`。
    **失败 ⇒ 指针不推进 + LOGE**（比旧实现「`(void)` 丢弃仍推进」更正确）。
  - **JRN_ERASE** ⇒ 失败 LOGE；终态 `IDLE`。
  - 每条命令自己 `fc_command_release`。
- `journal_flush_page` 退化为**同步屏障**：`journal_drain`（自旋 `fc_service` +
  `completion_table_drain` 等 job 归位，上限 `JOURNAL_DRAIN_SPIN_MAX`）+ `journal_flush_now`
  （立即落盘）。**调用者只剩 F10（GC/WL 擦 victim 前）与 HIL `fl`**。

##### 与计划的偏离（有意）

计划书里的 **ping-pong `page_buf[2][2048]`（+2 KB）被丢弃**：`ftl_cmd_write_data` 在
**prepare 时**就把数据 `memcpy` 进 FC 的 buffer 池 ⇒ FTL 侧的源缓冲在 `ftl_async_write_page`
返回后即可释放，job 只需要记「写指针落点」。实测代价因此只有
**`g_ftl` +16 B**（`journal_job_t`），而非 +2 KB。

##### 顺带引入的跨阶段机制

- `ftl_op_in_dispatch()`（`ftl_op.c` 的深度计数）：给「服务函数内不得同步等待」这条约定
  一个**可判定**的依据。已落地用法 = `journal_append` 页满时若异步刷页起不来，
  **不在**派发上下文里同步回退（那正是要消除的嵌套泵）。
  ⚠ 已知唯一剩余的嵌套泵来源：host 写路径的紧急 GC
  （`ftl_wbuf_flush → gc_migrate_sb → journal_flush_page`），留待阶段 4。
- `journal_truncate` 与在飞 job 的**双擦竞争**已闭合：truncate 擦环形下一块时若正是
  `job.target_block`，两处（`ftl_op_journal` 起擦前、`journal_erase_pending_sync`）
  都先核对 `pre_erased_block` 再决定是否真擦。

##### 自审中发现并修掉的两个缺陷（均未流出）

1. `journal_freeze` 原来只守卫 `phase != JRN_IDLE`。若「回绕写完成但起擦失败」
   （`need_erase = 1` 而 `phase = IDLE`），后续 freeze 会**往没擦过的环形块里编程**。
   修：守卫扩为 `|| (journal->job.need_erase != 0U)`。
2. 页满守卫若放在合并扫描**之前**，同 LBA 合并在满页时失效；只放在尾部又会写出界
   （`12 + 127×16 = 2060 > 2048`，写进 `journal_job_t`）。修：**合并扫描 → 守卫 → 写槽**
   的顺序。

#### 阶段 3 —— Read Recovery 异步（本次）

**动机**：`ftl_op_host_io` 读失败 → `ftl_recover_read_subop` → `rr_recover_read` 把三级恢复
串成**一次阻塞调用**（每级 `ftl_sync_submit_wait`）⇒ 完成回调里又跑一遍完成泵（嵌套泵之二），
且一个坏页把整条 host 读流水线钉住数十毫秒。

##### 设计：读失败的子操作「托管」给 job，而不是当场恢复

```
ftl_op_host_io（读失败分支）
    pending->op         = PENDING_OP_READ_RECOVERY   ← 新 op 值（与 READ/WRITE 并列）
    pending->command_id = FTL_PENDING_CMD_NONE       ← 0xFFFF 哨兵：本项已与 FC 命令脱钩
    fc_command_release(cmd)                          ← 命令当场归还池
    rr_kick(ftl)                                     ← 交给 job，本函数**返回**
```

- pending 项**保留**（不 `pending_remove`、不递减组在飞数）⇒ host 读组继续 pending，
  直到 job 收尾统一 `pending_release_index` + `pending_group_complete_subop_multi(owners, …)`。
- **`FTL_PENDING_CMD_NONE` 是必需的**：命令一 release，其 `command_id`（FC 命令池下标）就会
  被后续 `fc_command_alloc` 复用，而 `pending_lookup` 按 id 扫**第一项** ⇒ 陈旧 id 会让一条
  新命令的完成撞上这个停泊项。
- 停泊区就是 pending 表本身（深度 32）⇒ **零额外内存**；配套新增
  `pending_find_op` / `pending_release_index`。

##### 状态机（per-instance 单 job，`ftl->rr`）

```c
typedef enum { RR_STAGE_IDLE, RR_STAGE_PLANE, RR_STAGE_SF_SET, RR_STAGE_READ,
               RR_STAGE_SF_RESTORE, RR_STAGE_NB_READ, RR_STAGE_NB_RETRY,
               RR_STAGE_PLANE_END, RR_STAGE_FINISH, RR_STAGE_ABORT } rr_stage_t;
```

**stage 语义约定**：stage 恒表示「当前该做 / 正在等的那一步」——
- **纯计算 stage**（IDLE / PLANE / PLANE_END / FINISH / ABORT）只从 `rr_step` 进入，跑完
  立刻转下一个 stage，**不提交**命令；
- **提交 stage**（SF_SET / READ / SF_RESTORE / NB_READ / NB_RETRY）由 `rr_step` 提交后
  **原地停留**，等完成服务函数改写。

判「有没有命令在飞」用 `waiting` 而**不是** stage：提交可能因背压失败（stage 不变但无命令）。

##### 逐条对照原同步实现（三级管线拆成分步，语义一字不改）

| 原实现 | 分步版 |
|---|---|
| Stage 1：`SET_FEATURES(attempt)` → 重试读 → 失败则 `SET_FEATURES(0)` → 下一 attempt | SF_SET → READ →（失败）SF_RESTORE → `rr_next_attempt` |
| `SET_FEATURES` 失败 ⇒ **直接**下一 attempt、**不**恢复模式（原 `rr_exec_retry` 在 Step 1 就 return -1，根本走不到 Step 3） | `RR_STAGE_SF_SET` 只认 `FC_CW_STATUS_NAND_FAIL`，命中即 `rr_next_attempt` |
| 重试读失败 ⇒ 恢复模式 0（原 Step 3 **无条件**执行，含最后一次 attempt） | READ 失败 ⇒ `SF_RESTORE`（不看其结果，一律 `rr_next_attempt`） |
| 重试读**成功** ⇒ 恢复成功，**不**恢复模式 0（原实现的既有行为） | READ 成功 ⇒ `ftl_cmd_read_data` + `recovered=1` + `PLANE_END` |
| Stage 2：邻居页 `page∓1`（块内回绕）读成功 ⇒ 再试目标页 | NB_READ →（成功）NB_RETRY；失败 ⇒ `rr_next_neighbor` |
| Stage 3：全失败 ⇒ 各 CW 清零 + `bbt_mark_bad` | `rr_settle_plane` 的 `recovered == 0` 分支 |

⚠ 邻居页用 `FTL_PAGE_CFG_DATA_ANY()`：邻居页的 LBA **本就未知**（辅助读只要「这一页读得
出来」这个事实），`DATA_ANY` 让命令 META = `FC_META_UNKNOWN` 跳过页内比对。用 `DATA(0)` 会把
「LBA 就是 0」当事实下传 ⇒ 恒置 `MATA_ERROR`。

##### 收尾

```c
static void rr_finish(ftl_context_t *ftl)
{
    const uint32_t owners = ftl->pending[ftl->rr.pending_idx].owners; /* 必须**先**取走：
                                          组完成通知可能重入并复用本槽 */
    pending_release_index(ftl, ftl->rr.pending_idx);
    pending_group_complete_subop_multi(ftl, owners, SSD_STATUS_IO_ERROR);
    ftl->rr.stage = (uint8_t)RR_STAGE_IDLE;
    ftl->rr.waiting = 0U;
}
```

⚠ **一律 `IO_ERROR`**：原代码在 `if/else` 之后**无条件**写 `IO_ERROR`，覆盖了
`recovered == 0` 分支的 OK，即「recovery 成功仍报错」。语义存疑、属既有缺陷；**本次只搬
位置，未顺手改**（同阶段 1「不顺手改语义」的约定）。

##### watchdog 只兜「提交侧」

`stall_rounds`（`uint16_t`，`RR_SUBMIT_STALL_MAX = 256`）累计「有 job 却没在飞」的轮数，
超限 ⇒ `rr_abort` 按恢复失败收尾。

- **在飞的命令不需要 watchdog**：NAND 层的 wait/jump 超时保证每条已提交命令**必有**完成
  （超时置 `FC_CW_STATUS_TIMEOUT`）；强拆在飞的 job 会释放一条仍在 FC 池里的命令 ⇒ 池损坏。
- **卡死也**不**标坏块**：提交侧卡死是控制器资源问题（命令池泄漏），与介质无关，标坏会白丢
  一个好块。
- ⚠ 字段是 `uint16_t` 而非 `uint8_t`：`uint8_t` 与 `RR_SUBMIT_STALL_MAX (256)` 比较**恒假**
  （255 就回绕），守卫永不触发 —— 编译期 `-Wtype-limits` 抓到。

##### 新增异步原语（`ftl_fc_interface.c`）

```c
ftl_status_t ftl_async_read_page(fc_cmd_s **out_cmd, nand_pba_t pba, const ftl_page_cfg_s *cfg);
ftl_status_t ftl_async_set_features(fc_cmd_s **out_cmd, uint8_t ch, uint8_t ce, uint8_t lun,
                                    uint8_t feature_addr, uint8_t mode);
```

- **读与写不对称**：写的数据在 prepare 阶段就 `memcpy` 进 FC buffer 池（源缓冲随即可复用）；
  **读不是**——数据留在 desc 链上，只有服务函数在 release **之前**调 `ftl_cmd_read_data`
  才拿得到 ⇒ 目标缓冲须活到完成。
- ⚠ **`ftl_async_set_features` 的 ch/ce/lun 是对原内联代码的有意偏离**：原
  `read_recovery.c` 建 SET_FEATURES 命令时 ch/ce/lun 留 0。它们不是地址，而是**完成派发的
  实例路由键**（`ftl_op_dispatch` 按命令自身 ch/ce/lun 经 `completion_table_route` 解析归属
  实例）；SET_FEATURES 无 PBA 可推 ⇒ 只能显式传。单实例下留 0 恰好等于 `topo`，多实例下会把
  本命令的完成派给实例 `[0][0][0]`。

##### 唯一残留的同步调用（有意留下）

`rr_select_plane` 里的 `l2p_cache_read`：region 未命中时它会内部同步加载。**它今天就在同一
位置**（原 `ftl_recover_read_subop` 的选页循环），且失败的读**刚刚**在 `ftl_issue_subop` 里
查过同一 LBA ⇒ 该 region 几乎必在 cache 中。彻底关闭留待阶段 5（L2P 加载 job 化）。

##### 自审中发现并修掉的一处（未流出）

`-Wredundant-decls`：`ftl_op_read_recovery` 同时在 `ftl_op.h` 与 `read_recovery.h` 声明。
修：只留 `ftl_op.h`（那是 `g_op_service[]` 各槽的**唯一权威列表**），`read_recovery.h` 处
换成指向它的注释。

#### 阶段 4 —— GC / WL 搬移异步（本次）

**动机**：单次搬移 = 最多 64 页 × 2 plane 的读 + 写 + 2 次擦，同步执行可阻塞数十毫秒；
而 `ftl_wbuf_flush`（host 写路径的"紧急 GC"）把这一整套搬到了 **host 写完成**的路径上
⇒ 完成泵被长时间占住，且是**最后一个嵌套泵**（`gc_migrate_sb → journal_flush_page`）。

##### 收敛：三个逐字同构的搬移主体合成**一台** job

原实现是三份近似复制：`gc.c` 的 `gc_migrate_sb` + `gc_erase_and_free`、
`wear_leveling.c` 的 `wl_relocate` + `wl_execute` + `wl_predictive_relocate`。
它们的搬移主体逐字同构，差别只有两处：

| 维度 | GC | 静态 WL | 慢死预测 |
|---|---|---|---|
| 谁当 victim | `gc_select_victim` | `wl_select_victim` | `rr_predictive_scan` |
| 擦完怎么处置 | 归还 Free Pool | 归还 + **退役** | 归还 + **退役** |

⇒ 收敛成**一个** `mig_job_s`（新增 `include/ftl/migrate.h` / `src/ftl/migrate.c`）：
`kind`（`MIG_KIND_GC/WL/PREDICT`）选 victim，`post`（`MIG_POST_FREE/RETIRE`）决定收尾。
`g_op_service[]` 的 `[FTL_OP_GC]` 与 `[FTL_OP_WL]` **两槽填同一个** `ftl_op_migrate`
（tag 由 `mig_op_tag(m)` 按 `post` 现算）——搬移引擎不关心是谁发起的。

`gc.c` / `wear_leveling.c` 现在只剩**选块策略**与共用原语，`ftl.c` 只剩**触发求值**与
`mig_begin_*` 启动；回收动作 `ftl_gc_reclaim_one` 删除。

##### 状态机

```
SELECT（在 mig_begin_* 里做，不是 job 阶段）
    → READ → WRITE → FLUSH_JRN → ERASE → DONE
```

- **READ** 逐页逐 plane：异步读 → 完成时解 META 得 `cw_lba`、逐 CW 反查 L2P 得
  `cw_bitmap`、记 `rr_record_ecc` / `read_count`（慢死预测闭环）。读失败 / 全页空洞 ⇒ 跳过该 plane。
- **WRITE** 本页两 plane 都读完且有有效 CW ⇒ `allocator_alloc_pair(FTL_TEMP_COLD)` 拿目标页，
  逐 plane 提交 CW 子集写；完成时逐 CW `l2p_cache_write` + `journal_append` + 有效计数迁移。
  写失败的 plane **不动 L2P**（源数据仍有效，下轮再搬）。
- **FLUSH_JRN** 等 Journal job 空闲——**F10 契约**：搬移条目先落盘再擦 victim。
  `journal_kick` 会冲刷半满页，故该屏障**必然可满足**（不会因"页没满"死等到 256 轮上限）。
- **ERASE** 逐 plane 异步擦：成功 `erase_count++` / `wear_mark_dirty` / 清 valid / 归还 Free Pool；
  失败 `bbt_mark_bad`。`post == MIG_POST_RETIRE` 时额外 `rr_retire_block`。
- **DONE** `allocator_remove_closed_sb` + `gc_reclaimed_count++`（进展计数）/+ 预测命中日志。

**stage 语义约定同 `rr_stage_t`**：纯计算 phase（`FLUSH_JRN` / `DONE`）只从 `mig_step` 进入、
跑完立刻转下一个（不提交命令）；提交 phase（`READ` / `WRITE` / `ERASE`）由 `mig_step` 提交后
**原地停留**，等 `ftl_op_migrate` 改写。判「有没有命令在飞」用 `waiting` 而**不是** `phase`：
提交可能因背压失败（`phase` 不变但无命令）。

##### 与同步版的**行为差异**（刻意，三条都登记在案）

1. **分配不到目标页 ⇒ 中止且不擦 victim**。原 `gc_migrate_sb` 在 `allocator_alloc_pair` 失败时
   `break` 出页循环，而其调用者 `ftl_gc_reclaim_one` **无条件**接着 `gc_erase_and_free`
   ⇒ **擦掉尚未搬移的有效页 = 静默丢数据**。本引擎改为中止 job（源数据仍在、L2P 未动、
   条目留在 `closed_sb_list`），下轮 GC 再来 —— 自愈且不丢数据。
2. **擦除失败时不再退役**。原 `wl_predictive_relocate` 在 `wl_relocate` 返回后**无条件**
   `rr_retire_block`（即使 `gc_erase_and_free` 已把该块标坏）。本引擎只在**擦除成功**时退役
   （`post == MIG_POST_RETIRE`）——擦不掉的块归 `bbt_mark_bad` 管，把它同时标成
   "RESERVED、仍可读"是自相矛盾的。擦除失败本身是极端分支（NAND 拒擦）。
3. **新增提交侧卡死计数** `MIG_SUBMIT_STALL_MAX`（256 轮）：原同步版没有这个概念
   （阻塞调用要么成功要么当场报错）。超限 ⇒ 中止 job（同 1：不擦 victim，数据无损）。

##### 写路径紧急 GC 改**背压**（用户可见的行为变化）

`ftl_wbuf_flush` 今天"分配不到块 → 同步 GC 重试一次"；改为 **分配不到 ⇒
`mig_begin_gc(ftl)`（若未在跑）+ 返回 `FTL_AGAIN`**（缓冲保留，`ftl_drain_group` 下轮重试）。
与既有 `FTL_AGAIN` 背压机制完全一致，也是主流做法（写路径不就地回收，取不到块就挂起主机命令）。

- ⚠ **不再返回 `FTL_ERROR`**：无 victim 可回收只是"暂时没空间"，下轮可能就有（另一条写完成
  归还了块）；报 IO_ERROR 会让 host 写**永久失败**。
- **代价**：池满时写延迟升高（等一个 job 走完），但 superloop 不再被 GC 卡死。
- **死锁边界**：GC 自身搬移需要 free block ⇒ 靠 **OP 预留池**兜底（v0.74 已把 OP 池改成
  按 plane 预留）；GC job 因池空而中止时（差异 1）不擦 victim、数据无损，下轮重试。

##### 自审中发现并修掉的两处（未流出）

1. **`m->gsrc[plane]` 在快路径下未初始化** ⇒ 擦除收尾会去改**块 0** 的元数据
   （`erase_count` / `clear_valid` / `allocator_free_block`）。快路径（`valid_total == 0`
   ⇒ 直接 ERASE）**从不经过 READ 阶段**，任何"读阶段填、擦阶段用"的缓存字段在快路径下
   都是 0。修：删掉该字段，改由 `mig_src_gblock(m, plane)` **每次现算**
   （`ftl_gblock(plane, victim.plane_blocks[plane])`）—— 与原 `gc_erase_and_free` 的口径一致。
2. **`stall_rounds = 0U` 置于 `ftl_op_submit` 之前** ⇒ 提交失败时每轮都清零，
   `MIG_SUBMIT_STALL_MAX` **永不触发**。修：三处 `mig_submit_*`（`read_recovery.c` 两处
   `rr_submit_*` 同）改为**提交成功后**才清零。

##### 顺带清理

- `gc_read_page_with_lba` 的两个出参 `ecc_out` / `read_count_out` 随搬移读走异步后**再无
  读者** ⇒ 删除（`gc.h` 签名、`power_failure_recovery.c` 调用点同步）。该函数现只剩
  一个调用者：掉电重建路径的 `ftl_rebuild_closed_sb_list`。
- `inflight` 公式切换（**贯穿机制 4**）：`ftl.c` 的三级模式判定改为
  `pending_count + pending_group_count`，**不再计入 `completion_table`**。后台 job
  （journal / RR / 搬移）的命令也登记在 completion_table 里，计进来会让"真空闲"永不成立
  ——一个后台 job 在飞就把模式从 IDLE 顶到 NORMAL，后台反而被自己的命令饿死。后台 job 的
  活跃度由各自的"单飞行门"（`journal.job.phase` / `rr.stage` / `mig.active`）单独管。

##### 唯一残留的同步调用

`mig_step` 的 READ 阶段在解 META 后逐 CW 反查 L2P 时调 `l2p_cache_read` / `l2p_cache_write`：
region 未命中会内部同步加载。**与阶段 3 的 `rr_select_plane` 不同，这里 region miss 是
现实可达的**——搬移读的是**整块**的页，其 LBA 来自页内 META，与刚刚查过的 host LBA 无必然
联系。**该残留已在阶段 5 关闭**（改在纯计算阶段 `MIG_READ_SETTLE` / `MIG_WRITE_SETTLE` 里
做就绪前置）。

#### 阶段 5 —— L2P region 加载 / 写回异步

**这是「该异步的都异步」里唯一需要 API 语义变更的阶段**，也是收益最直接的一处：host 读
随机访问的每个 region 未命中、host 写**首次触碰**每个 region，此前都会在完成泵内部同步
等一次 NAND 往返（阶段 1 的起因里列的头号重入源之一）。

##### ① 对外 API 三态化（`l2p_cache.h` / `l2p_cache.c`）

`l2p_cache_read` 由「返回 `nand_pba_t`」改为 **`ftl_status_t l2p_cache_read(cache, lba, *out)`**：

| 返回 | 含义 | `*out` |
|---|---|---|
| `FTL_OK` | 命中（CLEAN / DIRTY / **FLUSHING**） | 条目值；**可以是 `NAND_PBA_INVALID` = 未映射** |
| `FTL_AGAIN` | **还不知道**：region 未驻留（本次已起异步加载）或正在加载 | 未动 |
| `FTL_ERROR` | 参数非法 / 越界 | **`NAND_PBA_INVALID`**（见下） |

- ⚠ `FTL_OK` + `INVALID`（**未映射**）与 `FTL_AGAIN`（**还不知道**）**绝不可混同**——把后者
  当前者会让 host 读到零填假数据。所有调用点都按三态改写。
- ⚠ `FTL_ERROR` **也回填** `*out`：调用点的既有口径是「读得 INVALID 与未映射同义」
  （`ftl_trim` / `rr_select_plane`），不回填就是**未初始化变量**。旧单返回值实现此处返回
  `NAND_PBA_INVALID`，语义必须保持。这是自审中抓到的真缺陷（见下）。

新增 `int l2p_cache_ready(cache, lba)` —— **就绪前置探针**（命中 1 / 未就绪 0，未命中则起
加载）。供「必须先就绪、再一次性应用多条改动」的调用者用（写确认 / 搬移收尾）：
先对全部 LBA 过一遍，全 1 时后续 read/write 必不可能再返 `FTL_AGAIN`
⇒ 一批改动**全有或全无**，重试不会重复记账。

`l2p_cache_write` 同样三态（行在飞 ⇒ `FTL_AGAIN` 且**不改任何状态**）。

##### ② `fresh`：异步化的**观测口径补偿**（新增 1 字节，落在填充里 ⇒ 结构体不变）

异步后一次 miss 访问会**跨两轮**：第 1 轮起加载（计一次 miss），第 2 轮重试命中。若重试也
计一次 hit，则**同一次访问被记成 miss + hit** ⇒ 窗口 miss 率虚降一半（随机读满盘：真值
100% → 观测 50%，跌破 `L2P_THRASH_EXIT_PCT=70` ⇒ **P1-4 降级永不触发**）。

故每行加 `fresh`：加载完成时置 1，其后**第一次**命中消费它且**不计**命中。同时
`l2p_cache_ready` 定为**纯探针**（命中不记账、不消费 fresh、不动 `access_count`；未命中记
一次 miss——因为加载是它起的）。如此与旧同步实现**逐字等价**：

| 场景 | 旧 | 新 |
|---|---|---|
| host 读命中 | 1 hit | 1 hit |
| host 读 miss | 1 miss | ready/read 记 1 miss；重试命中被 `fresh` 吃掉 |
| host 写（首触） | `l2p_cache_read` 记 1 miss + `l2p_cache_write` 记 1 hit | `l2p_cache_ready` 记 1 miss + `l2p_cache_write` 记 1 hit |

⚠ **自审抓到**：`l2p_cache_write` 一度只 `hit_count++` 而**漏了** `l2p_cache_note_access(0)`
（旧实现是在 `find_or_load` 里一并做的）⇒ 窗口里只剩那一次 miss，随机写满盘从
「1 miss + 1 hit = 50%」变成「1 miss + 0 hit = 100%」⇒ 越过 `L2P_THRASH_ENTER_PCT=90`，
P1-4 降级在写密集下被**误触发**。已补回。

##### ③ 加载 / 写回的 job 化

- **加载**：`l2p_cache_load_start`（static）——CLOCK 选 victim（`DIRTY` 先起异步写回再跳过、
  `LOADING`/`FLUSHING` 跳过、全在飞 ⇒ 返回 0）→ `memset` 0xFF 占位 → 置 `LOADING` →
  `ftl_async_read_page` + `ftl_op_submit(tag=FTL_OP_L2P, ctx=该 cache line)`。任一步失败
  都回滚成 `INVALID` 并返回 0（调用者下轮重试）。
- **写回**：`l2p_cache_flush_kick(cache, line_idx)`——`metadata_dir_append_slot` 取槽 →
  `ftl_async_write_page` → 置 `FLUSHING` → 提交。**提交成功才置 `FLUSHING`**（失败必须留在
  `DIRTY`，否则脏数据永不再被写回）。背压闸门 `L2P_FLUSH_MAX_INFLIGHT = 4`：一次写回占 1 条
  命令 + 4 个 buffer（region 页 = 2KB = 4 CW），不设上限会把 FC 池吃光 ⇒ host I/O 拿不到
  命令、全系统 `FTL_AGAIN`。
- **完成服务 `ftl_op_l2p`**（`g_op_service[FTL_OP_L2P]`，ctx = 该 cache line）：
  - `LOADING` ⇒ 成功：`ftl_cmd_read_data` 拷进 `line->entries`（**必须早于 release**——读的
    数据在 desc 链上）；失败：**置 `CLEAN` 且 entries 保持全 INVALID**。
  - `FLUSHING` ⇒ 成功：`metadata_dir_commit_region` 登记新位置 + 置 `CLEAN` + 清 region 脏位；
    失败：置回 `DIRTY` 保留重试。
  - 两者都 `--load_inflight` / `--flush_inflight`，末尾 `fc_command_release`。

⚠ **刻意偏离**（登记在案）：加载**失败**时置 `CLEAN`（= 该 region 全部未映射），而**不是**
`INVALID`。旧实现失败后置 `INVALID` ⇒ **每次**访问都重试加载（坏 region 无限重读 NAND）；
异步化后重试语义会让 host 读**永久停在 `FTL_AGAIN`**（evict→加载→失败→再 evict 的活锁）。
观察行为一致：都返回「未映射」。

##### ④ 调用点改造（本阶段工作量所在）

| 调用点 | 改法 |
|---|---|
| `ftl.c` host 读下发（`ftl_issue_subop`） | 三态；`FTL_AGAIN` ⇒ 置 `ftl->l2p_defer` 并 `break`，复用既有 `g->stalls` 背压 |
| `ftl.c` `ftl_confirm_write`（host 写完成） | **两遍式** + **停泊**（见 ⑤） |
| `ftl.c` TRIM | 同步逃生口（CLI 上下文，契约是「取消映射同步完成」） |
| `ftl.c` `ftl_flush`（HIL `fl`） | 同步逃生口（显式持久化请求，非后台攒批） |
| `read_recovery.c` `rr_select_plane` | 三态 ⇒ 返回 -1（「还不知道」）与 0（「未映射」）分清，`RR_STAGE_PLANE` 原地等下轮 |
| `migrate.c` 搬移 | 新增**纯计算** phase `MIG_READ_SETTLE` / `MIG_WRITE_SETTLE`，就绪前置 + 一次性应用 |
| `journal.c` 回放 / `power_failure_recovery.c` F2 重建 / `checkpoint.c` / `cli.c map` | 同步逃生口 `*_sync`（结构性同步上下文，不在完成派发里） |

**同步逃生口**：`l2p_cache_read_sync` / `_write_sync` / `_flush_one_sync` / `_flush_all_sync`
——内部自旋 `ftl_service_pump` 直到就绪。它们就是阶段 1 的 `ftl_sync_submit_wait` 客户端，
**不是新机制**；`l2p_sync_pump` 里 `ASSERT(ftl_op_in_dispatch() == 0)` 把「服务函数禁用」
变成可测的。这些站点在阶段 6/8 逐个 job 化后即可删除。

##### ⑤ host 写完成的**停泊**（`PENDING_OP_WRITE_CONFIRM`）

`ftl_confirm_write` 在**完成回调里**被调，不能阻塞 ⇒ 拆两遍：
**第一遍只做就绪前置**（逐 CW `l2p_cache_ready`，任一未就绪即返 `FTL_AGAIN`，**一条都不动**），
**第二遍才动账**。两遍之间无泵（单线程顺序执行）⇒ 就绪判定与第二遍之间状态不可能变。
⚠ 绝不能边查边改：`l2p_cache_write` 对在飞的行返 `FTL_AGAIN`，中途放弃会让同一页里部分 CW
已记账、部分没记，重试时**重复记账**（`valid_count` 虚高、journal 重复条目、`total_writes`
多计）。

`ftl_op_host_io` 拿到 `FTL_AGAIN` ⇒ 把 pending 项**停泊**（新枚举
`PENDING_OP_WRITE_CONFIRM`）：`command_id` 置哨兵 `FTL_PENDING_CMD_NONE`（防 cmd_id 复用撞
车）、`fc_command_release`、**组在飞数不递减**（host 写组继续 pending），由
`ftl_confirm_kick` 每轮推进。与阶段 3 的托管同构，区别是**无外部引擎**——推进者就是
`ftl_service` Phase 2.5 的开头。

⚠ **顺序不可倒**：`ftl_confirm_kick` 必须早于 Phase 2.5 的补发循环——停泊意味着该 LBA 的
L2P 还是旧映射，先补发一条读同一 LBA 的子操作就会读到**旧物理页**（陈旧数据）。

⚠ **`ftl.c` 的不变量未破**：`ftl_confirm_write` 仍是**不存 old_pba、完成时按 LBA
重查**（同一 LBA 两次写在飞时，先完成的先改 L2P、后完成的重查即得前者）。停泊只是延后
几轮，顺序仍按完成序。

##### ⑥ 背压信号 `l2p_defer`（per-instance，新增 1 字节）

`ftl_issue_subop` 因 region 未就绪返 `FTL_AGAIN` 时置 1；`ftl_drain_group` 据此**不计**
`stalls`。理由：加载在飞 = **有确定完成时刻的 forward progress**，计进来会让一次正常的
region 加载吃掉 8 轮预算（`FTL_DRAIN_STALL_MAX`）里的一轮 ⇒ 随机读满盘时把好组**误判成卡死**
报 `IO_ERROR`。真卡死（命令丢失）由 NAND 层 wait/jump 超时兜底，不靠这个计数。

##### ⑦ 自审中发现并修掉的缺陷（均未流出）

1. **thrashing 窗口漏记账**（见 ②）——`l2p_cache_write` 少喂一次 `note_access`。
2. **`*out` 未回填**（见 ①）——`l2p_cache_read` 越界返回 `FTL_ERROR` 时不写 `*out`，
   `ftl_trim` 的 `nand_pba_t old_pba;` 会读到**未初始化值**并据此减某块的 `valid_count`。
3. **`fl` 漏等在飞行**——`l2p_cache_flush_all_sync` 原先只处理 `DIRTY`，会跳过「后台已 kick、
   尚未落盘」的 `FLUSHING` 行 ⇒ `fl` 返回时映射其实没落盘，与「落盘」语义不符。改为两者都等
   （`l2p_cache_flush_one_sync` 对 `FLUSHING` 是**等**而非重复起，不会发出第二条写命令）。

##### ⑧ 仍未拆的一处（有意留给阶段 8）

`l2p_cache_flush_kick` 内部仍会调 `metadata_dir_append_slot`——取空槽时**可能换块 ⇒ 擦除**，
那一次擦仍是瞬时阻塞。阶段 8 的 `md_job_t` 化会一并拆掉。

#### 阶段 6 —— Checkpoint 保存异步

`checkpoint_create` 原是一条**严格有序的同步流水线**（脏区落盘 → Anchor → Header → 截断），
每步都在 `ftl_service_*` 的栈上阻塞等 NAND 回来。单次 CP 可阻塞数十毫秒（脏 region 写回 +
8 页 Anchor + Header + 预擦），期间**完成泵停摆**——与阶段 4 的"紧急 GC 阻塞 host 写路径"
同源，只是触发频率低（T1 水位 / T2 写入量 / T3 定时）。

##### ① `cp_job_t` 状态机（`include/ftl/checkpoint.h`，per-instance 落 `ftl->cp_job`）

```
CP_IDLE → CP_FLUSH_REGIONS → CP_ANCHOR_PREP → [CP_ANCHOR_ERASE] → CP_ANCHOR_WRITE
        → CP_HDR_PREP → [CP_HDR_ERASE] → CP_HDR_WRITE → CP_TRUNC_PREP
        → [CP_TRUNC_ERASE] → CP_DONE
```

`checkpoint_begin(ftl)` **只装配 job 并推进第一步**（快照脏位图 + journal 水位），
返回 `FTL_OK` **只代表"已启动"，不代表 CP 成败**——这是与旧 `checkpoint_create` 的
调用契约差异，调用点注释已同步。此后每轮 `ftl_service` 调 `checkpoint_kick(ftl)` 续跑；
提交失败（背压）⇒ 原地返回下轮重试，连续 `CP_SUBMIT_STALL_MAX=256` 轮无进展 ⇒ 中止。

⚠ **`checkpoint_kick` 必须放在三档之外**（`ftl_service` 里 Phase 3 首，`bg_suspend` 判定
之后）：CP 可能在 **BUSY** 档被紧急水位（journal ≥95%）起，而它的后续阶段要跨多轮、跨档位
才能走完——只在"起它的那一档"推进会把 job 钉死在该档。

##### ② 三段式：把「擦除」从同步原语里剥出来

异步化的真正障碍不是"写"，而是**写之前的换块擦**：`metadata_ring_prepare_write` 和
`metadata_dir_flush_to_anchor` 都把"算地址 + 需要就擦"揉在一个同步函数里。拆成**纯算址** +
**调用者决定何时擦**：

| 新增（纯 RAM / 不动介质） | 原同步原语（保留，= 新函数 + 即刻擦） |
|---|---|
| `metadata_ring_prepare_addr(ring, topo, idx, *addr, *need_erase)` | `metadata_ring_prepare_write`（唯一剩余调用者：`bbt_flush` 与 `checkpoint_write_initial`，均结构性同步）|
| `metadata_dir_anchor_prepare(dir, *ring, *base, *idx, *seq, *need_erase)` | — |
| `metadata_dir_anchor_page(dir, seq, chunk, *page)`（组页，纯 RAM） | — |
| `metadata_dir_anchor_commit(dir, ring, idx, ok)`（推进游标，成功才清 dirty） | `metadata_dir_flush_to_anchor`（= prepare + 同步擦 + 逐页写 + commit）|
| `journal_truncate_plan(journal, up_to_seq, *addr, *block)` | — |
| `journal_truncate_commit(journal, erased_block)` | `journal_truncate`（= plan + 即刻擦 + commit）|

`metadata_dir_flush_to_anchor` 因此退化成**四行串行版**，被 `metadata_dir_compact`（阶段 8）
继续使用；`journal_truncate` 变成**零调用者**的同步逃生口（链接期被 gc 掉，源码保留）。
⚠ `metadata_dir_flush_to_anchor` 里显式保留了 `dir == NULL ⇒ FTL_OK` 的旧契约
（三段式内部把 NULL 判为 `-1`，更严）——`不顺手改语义`。

⚠ **阶段 8 追记**：两个"四行串行版"（`metadata_dir_flush_to_anchor`）与它的唯一调用者
`metadata_dir_compact` 一并删除；三段式 `metadata_dir_anchor_prepare` / `_page` / `_commit`
现由 **CP job 与 md job 两个异步状态机共用**（各自把 `need_erase` 变成"提交一条异步擦"）。

##### ③ 快照点前移（**与旧同步实现的刻意差异**，本阶段唯一的语义变更）

旧实现取 `journal.sequence_counter` 是在 **Anchor 之后**（步骤⑤）。异步 CP 跨多轮，若照旧
取"CP 末尾"的水位，则在 `CP_ANCHOR` 期间完成的 host 写会同时满足：

1. region 脏位在 RAM 里置了，但没进本次快照 ⇒ **不会被 flush**；
2. 其 journal 条目 `seq ≤ 末尾水位` ⇒ boot 视为"已进 L2P 表"而**跳过回放**。

⇒ **两边都不认，静默丢映射**。故 `region_bitmap` 与 `journal_seq` **一起在 job 起点取定**
（`checkpoint_begin` 里 memcpy + 取值）。此后任何 host 写都有 `seq > 快照水位`，boot 必然回放。

代价：`journal_truncate_plan` 的水位判据 `(up_to + 1) < sequence_counter` 比旧实现**更容易
不满足** ⇒ **更少预擦**。这是安全方向的取舍——预擦只是磨损优化（跳过它只是让
`journal_flush_page` 自己擦，每块每圈仍只擦一次）。

##### ④ 服务函数**不推进**状态机（与 `ftl_op_migrate` 的关键差别）

`CP_ANCHOR_PREP` / `CP_HDR_PREP` 里有**同步原语**：游标未建立时要 `metadata_ring_locate`
扫池定位（内含 `ftl_sync_read_page`）。在完成泵里跑就是**嵌套泵**——正是本次解耦要消除的
东西，且 `l2p_sync_pump` 有 `ASSERT(ftl_op_in_dispatch() == 0)` 挡。故 `ftl_op_checkpoint`
只做「记结果 + `release` + 改 phase」，推进一律留给下一轮 `ftl_service` 的 `checkpoint_kick`。
（`ftl_op_migrate` 可以当场推进，是因为搬移的阶段里已无同步原语——阶段 4 把
`MIG_READ_SETTLE` / `MIG_WRITE_SETTLE` 拆成纯计算 phase 换来的。）

##### ⑤ 失败分支逐条对照（`不顺手改语义`）

| 阶段 | 旧同步行为 | 异步版 |
|---|---|---|
| 脏区 flush 任一失败 | `checkpoint_flush_dirty_regions` 返回 `FTL_ERROR` ⇒ `checkpoint_create` 提前 return（不写 Header / 不 flush Anchor / 不截断）| `cp_flush_regions` 返回 -1 ⇒ `cp_abort`，`CP_DONE` 里**不** commit ⇒ 不写 Header / 不截断 |
| 脏行是 `LOADING`（不变量违例）| 旧实现不存在此状态 | **中止**（而非"当作已持久"）：此刻既没在写回、也无有效数据，写进 Header 会让 boot 预载该区 ⇒ 静默丢映射 |
| Anchor 换块擦失败 | `metadata_ring_prepare_write` 返回 `FTL_ERROR` ⇒ 提前 return，**不推进游标** | `CP_ANCHOR_ERASE` 失败 ⇒ `cp_abort`，不推进游标（dirty 保留，下次新槽重试整条）|
| Anchor 某分页写失败 | `ok = 0` ⇒ 整条残缺 ⇒ 推进游标 + 返回 `FTL_ERROR` | `anchor_ok = 0` + 照常 `anchor_chunk++` ⇒ 写完整条后 `commit` 推进游标 + `cp_abort` |
| Header 写失败 | 推进游标 + 提前 return（不截断 / 不复位水位）| `CP_HDR_WRITE` **无论成败都推进** `ftl->cp.write_idx` + `cp_abort` |
| Journal 预擦失败 | `(void)ftl_sync_erase_block(...)` **丢弃结果**，照样记账 `pre_erased_block` | 同（**保留旧语义**），仅加一行 `LOGW` 使其可观测 |
| CP 成功末尾 | 无条件复位 `emergency` / `checkpoint_needed`（第 ⑧ 步）| `CP_TRUNC_PREP` / `CP_TRUNC_ERASE` 调 `journal_truncate_commit` 复位（plan 返回 0 或 -1 时也走 `commit(NONE)`）|
| CP 中途失败 | 提前 return ⇒ 水位**不**复位 | `cp_abort` ⇒ `CP_DONE` 不 commit ⇒ 水位不复位 |

⚠ **`journal_truncate_plan` 的返回值语义**（自审中收紧）：`1` = 需擦 / `0` = 无需擦
（水位 OK，已预擦过）/ **`-1` = 不截断（水位不满足 / 参数非法）**。之所以要区分，是为了让
同步逃生口 `journal_truncate` **逐字保留旧行为**——旧函数在水位校验处直接 `return`，
复位语句在函数末尾 ⇒ **水位不满足时不复位水位标志**。CP job 则对 `<= 0` 统一 `commit(NONE)`
（对应旧 `checkpoint_create` 第 ⑧ 步的**无条件**复位）。

##### ⑥ 自审中发现并修掉的两处（均未流出）

1. **`CLEAN` 行被误判为不变量违例**：`cp_flush_regions` 起初把非 DIRTY / 非 FLUSHING 的
   脏位行一律 LOGW + 中止。但快照位图在 flush 成功后**仍然置位**（位由 `ftl_op_l2p` 的
   commit 清，不是由本函数清）⇒ 已落盘的 `CLEAN` 行每轮都触发假告警。补 `CLEAN ⇒ continue`。
2. **`default` 分支手工置 `failed`/`phase`** ⇒ 统一走 `cp_abort`，避免两处语义漂移。

另：`cp_job_t.region_bitmap` 的 memcpy 长度取自身字段 ⇒ 加 `_Static_assert` 钉死它与
`l2p_cache_s.region_dirty_bitmap` **同尺寸**（源小则读越界，源大则漏拷）。

##### ⑦ 栈深核实（结论：不需要改）

`cp_step` 的帧里有两个 2 KB 页缓冲（`CP_ANCHOR_WRITE` 的 `metadata_anchor_page_t page`、
`CP_HDR_WRITE` 的 `l2p_checkpoint_header_t cp`）。实测 `sub.w sp, sp, #2128` —— **GCC 已把
两者重叠**（2128 ≈ 2048 + 80 其它局部）。旧同步实现的嵌套是
`metadata_dir_flush_to_anchor`(2 KB) **套在** `checkpoint_create`(2 KB) 里 ⇒ 反而是 4 KB。
且 `linker/stm32h743xx_flash.ld` 的 `_Min_Stack_Size = 0x800` 只是 heap/stack 下限**校验**，
真实栈顶 `_estack = 0x20020000`（DTCM 末），而 `.dtcm_data` 实测 **0 字节** ⇒ 128 KB 全是栈余量。

#### 阶段 7 —— BBT / Wear 持久化异步（本次）

这两张表是后台档里**最后两个同步站点**。它们与阶段 6 的 CP 同族（"攒批 + 后台刷"），但阻塞
形态更极端：`wear_flush` 一次调用要写完 1~25 个 2 KB 日志页（compact 时是 25 页 + 一次整块
擦），全程同步；`bbt_flush` 一次要写 540 B 的坏块图 record（换块时再搭一次擦）。两者都在
`ftl_service_idle/normal` 里，每轮都可能各阻塞数毫秒。

##### ① BBT：两段式 job（`bbt_job_phase_t`，ctx = `&ftl->alloc`）

```
BBT_JOB_IDLE → [BBT_JOB_ERASE] → BBT_JOB_WRITE → BBT_JOB_IDLE
```

`bbt_kick(alloc)` **只提交第一条命令即返回**；完成由 `g_op_service[FTL_OP_BBT]`（`ftl_op_bbt`）
收尾并起写。job 状态与池游标同处（`alloc->bbt`，per-instance），`allocator_init` 的 memset
已零化 ⇒ 开机即 `BBT_JOB_IDLE`，`bbt_init` 再显式钉一次。

三处与旧 `bbt_save` 的**刻意对应**：
- **record 内容在提交写时从 BST 重建**（不是起步时快照）。换块擦的等待期间新标坏的块会被
  一并记进本条 record ⇒ 是严格的信息超集（坏块只增不减）。
- **header 放在栈上**（540 B）：`ftl_async_write_page` 在 prepare 时已把它拷进 FC buffer 池
  （同 `journal_freeze`）⇒ 异步 job **不需要**为它保留常驻快照缓冲。
- **地址按 `write_idx` 重算**（`metadata_ring_addr(ring, topo, write_idx, 0)`，与
  `metadata_ring_prepare_addr` 逐字同源）——因为 `write_idx` 写成功才推进，此刻仍指向本槽。
  故 job 里**不存** `addr`（CP 的 `cp_job_t` 存了，两种都行）。

##### ② Wear：三段式 job + `waiting` 标志（ctx = `&ftl->alloc`）

```
WEAR_JOB_IDLE → [WEAR_JOB_ERASE] → WEAR_JOB_WRITE（逐页，1~25 条命令）→ WEAR_JOB_IDLE
```

`wear_kick(alloc, total_writes)` 只**起步/续跑**：`wear_submit_next_page` 从 `job.cursor` 起
装**一页**日志（2 KB 栈缓冲 → prepare 时拷进 FC buffer 池）并提交，完成由
`g_op_service[FTL_OP_WEAR]`（`ftl_op_wear`）收尾 + `next_page++` + 续提交下一页。
节拍门（定时 `WEAR_FLUSH_INTERVAL_MS` **或** 写入增量 `WEAR_FLUSH_ERASE_DELTA`）与旧
`wear_flush` 逐字一致。

##### ③ 批跨多轮 ⇒ 两个必须钉死的东西

批不再是原子的（旧 `wear_flush` 在一次调用里写完，期间**没有任何** `wear_mark_dirty`
可能发生）。而 `wear_mark_dirty` 的两个调用点——`migrate.c` 的搬移擦完成（`MIG` job 的
服务函数）与 `ftl.c` 的 host 读错误——**都可能落在批中间**。于是：

| 风险 | 对策 |
|---|---|
| 批会写出超过 compact 判定所用 `pages_needed` 的页数 ⇒ `next_page` 越过块尾（页号 64 / 共 64 页）| **起点快照** `wear.dirty_snapshot[512]`：本批只写快照里的块，页数在起步时即定死 |
| 收尾清脏会丢掉「批期间新变」的块 | 见下条 |

**脏位图的清零式语义（本阶段唯一的刻意语义收紧）**：起步时
`snapshot = dirty; dirty = 0`，于是批期间 `wear_mark_dirty` 置的位**只表示「批开始后才变的
块」**。收尾（`wear_job_finish`）**什么都不清**——剩下的位正是"批期间变了、本批没写或写了
旧值"的块，留给下一批；失败（`wear_restore_dirty`）则 `dirty |= snapshot` 并回，得到
「批前脏 ∪ 批中新脏」。

对比"起步只拷快照、收尾 `dirty &= ~snapshot`"的写法：后者会丢掉「批前就脏 → 批中被写 →
**批中又变**」的块的最后一次变更（清位时只看快照，看不见批中的二次变更）。同步版
`wear_flush` 整批原子、根本没有这个窗口；清零式让异步版重新拿到同样的原子性。代价是
`dirty` 在批期间短暂为 0——但 job 未回 IDLE，`wear_kick` 不会另起一批，无副作用。

##### ④ 背压续跑：`waiting` 标志（**避免擦除放大**）

BBT 与 Wear 的 job 都带 `waiting`：`1` = 有一条本 job 的命令在飞；`0` = 上一步提交被
背压（`ftl_op_submit` 返回 `FTL_AGAIN`）而**停在当前 phase 待续跑**。`*_kick` 的
`phase == WRITE && !waiting` 分支只**续提交写**，**不重走换块判定**。

为什么必须有：BBT 的写点是块内槽 0 时需要先擦该块。若提交失败就回 `IDLE`，下一轮 kick 会
重算 `prepare_addr` ⇒ `need_erase` 仍为 1 ⇒ **再擦一次同一块**。命令池被长期占满时这会变成
"每轮擦一次同一个块"的**磨损放大**（该块可被擦成千上万次）。Wear 的 compact 路径同理。
⇒ 擦成功后**停在 WRITE/waiting=0**，续跑路径零介质访问。

##### ⑤ 失败分支逐条对照（`不顺手改语义`）

| 阶段 | 旧同步行为 | 异步版 |
|---|---|---|
| BBT 换块擦失败 | `metadata_ring_prepare_write` 返回 `FTL_ERROR` ⇒ `bbt_save` 提前 return，`write_idx` **不推进**、dirty 保留 | `BBT_JOB_ERASE` 失败 ⇒ 回 IDLE、`write_idx` 不动、dirty 保留，下轮 kick 从头再来（擦仍会重试——这是**对**的：擦没成功）|
| BBT record 写失败 | `write_idx` **照推**（半编程槽不可复用）+ dirty 保留 | 同（无论成败都 `write_idx++`），dirty 保留 |
| BBT record 写成功 | `write_idx++` + `dirty = 0` | 同 |
| Wear compact 擦失败 | `wear_compact` 首个 return ⇒ `next_page` **不动**、脏位保留 | 回 IDLE + `wear_restore_dirty`，下轮重试 |
| Wear compact 快照页写失败 | `memset(dirty, 0xFF)`（擦除已抹掉旧内容，起点时"干净"的块也成了未持久化）| 同 `memset(dirty, 0xFF)`，回 IDLE |
| Wear 增量页写失败 | `wear_write_page` return ⇒ `next_page` 不推进、脏位保留 | `next_page` 不推进 + `wear_restore_dirty`，回 IDLE |
| Wear 页写成功 | `next_page++`，继续下一页 | `next_page++`，续提交下一页 |

⚠ 两处**必须**保留的旧语义：① BBT 写失败**照推游标**（不是"不推进"）——已编程过的槽可能
半写坏，复用会读到残页；② Wear compact 失败**全标脏**（不是"并回快照"）——快照只覆盖批前
脏块，而擦除抹掉的是**整块**，批前干净的块也失去了持久化内容。

##### ⑥ 删掉的同步入口与保留的逃生口

- `bbt_flush` / `wear_flush`（及静态 `wear_write_page` / `wear_write_range` / `wear_compact`）
  全仓**只剩注释引用**。
- `bbt_save` **保留**（开机 `bbt_init` / 格式化路径的结构性同步站点，无人 drain CQ）；
  `wear_init` 本就只读（回放），不动。
- `bbt_kick` 里游标未建立时的 `metadata_ring_locate`（扫池逐页读）仍是**同步**的，但只在
  **冷启动后首次落盘**发生一次，且本函数由 `ftl_service` 的**后台档**调用（不在完成泵内）
  ⇒ 不是嵌套泵。异步化它需要把"扫池"也拆成 phase，收益极低，**有意不做**。
- CLI `bbt --erase` 走同步的 `bbt_print_bad_map`，且置 `bg_suspend` ⇒ `ftl_service` 在
  Phase 3 之前就返回，kick 不会被调用。`ftl_flush`（HIL FLUSH）只做 wbuf + L2P + journal，
  **不碰** BBT / Wear ⇒ 不需要同步逃生口。

##### ⑦ 无 watchdog（有意）

贯穿机制 1 要求的 watchdog 在本 job **不设**，理由是既有的 `[RR]` / `[MIG]` / `[JRN]` /
`[CP]` 也都没有：命令丢失的兜底在 NAND 层——`NAND_RESULT_TIMEOUT` 会被聚合成
`FC_CW_STATUS_TIMEOUT` 的**完成**（`nand_dispatch_tables.c`），故"在飞"必然有终态。
BBT / Wear 批只有 1~25 页、毫秒级，1 s 的心跳永远打不出来 ⇒ 失败可观测性靠 `LOGE`。

#### 判据与实测

| 判据 | 结果 |
|---|---|
| `make debug` / `make release` / `make tests UT=FTL` / `UT=FC` | 0 error；release 下 `ftl_fc_interface.c 'allowed'`、`:85 'plane_bits'`（ASSERT 被裁）、`crc_port.c 'status'`（HAL 状态位只写不读）三条 unused 为**既有**——均为**未改动**文件，与阶段 5 无关 |
| debug `.text` | 191688 → 191800（阶段 1）→ 193808（阶段 2）→ 195776（阶段 3）→ 195032（阶段 4）→ 198016（阶段 5，**+2984 B**）→ 201052（阶段 6，**+3036 B**）→ 203920（阶段 7，**+2868 B**）→ **206876**（阶段 8，**+2956 B**）|
| debug `.bss` | 381500 → 381372（阶段 1，**−128 B** = 旁表删 `handler`）→ 381404（阶段 2）→ 381404（阶段 3）→ 385596（阶段 4，**+4192 B**）→ 385596（阶段 5，**±0**）→ 385948（阶段 6，**+352 B**）→ 386460（阶段 7，**+512 B**）→ **388572**（阶段 8，**+2112 B**）|
| `g_ftl` | 269600 → 273784（阶段 4，**+4184 B** = `mig_job_s`；其中 4KB 页镜像原在 `gc_migrate_sb` / `wl_relocate` 的**栈**上，迁入 `.bss` 是真实新增）→ 273792（阶段 5，**+8 B**）→ 274128（阶段 6，**+336 B** = `cp_job_t`：256 B 脏位图快照 + 水位/Anchor/Header/截断 各阶段状态）→ 274656（阶段 7，**+528 B** = `wear_state_t.dirty_snapshot[512]` + 两个 job 状态 16 B）→ **276752**（阶段 8，**+2096 B** = `md_job_t`：2 KB `cp_buf[L2P_REGION_SIZE]` + 阶段/游标/Anchor/回收 状态）|
| release `.text` | 114612 → 114352 → 115776 → 116764 → 117108（阶段 4）→ 118440（阶段 5，**+1332 B**）→ 120332（阶段 6，**+1892 B**）→ 121928（阶段 7，**+1596 B**）→ **124152**（阶段 8，**+2224 B**）|
| release `.bss` | 381492 → 381364 → 381396 → 381428 → 385588（阶段 4）→ 385620（阶段 5，**+32 B**）→ 385940（阶段 6，**+320 B**）→ 386484（阶段 7，**+544 B**）→ **388564**（阶段 8，**+2080 B**）|
| debug `.bin` / release `.bin` | 196300 → 195556（阶段 4）/ 117288 → 117628（阶段 4）→ 198540 / 118960（阶段 5）→ 201580 / 120856（阶段 6）→ 204444 / 122448（阶段 7）→ **207404 / 124672**（阶段 8）|
| AXI_RAM 占用（构建脚本报告） | 284.0 KB（74.0%）→ 288.1 KB（75.0%）→ 288.1 KB（75.0%）（阶段 5 **±0**）→ 288.5 KB（75.1%）（阶段 6）→ 289.0 KB（75.3%）（阶段 7，**+0.5 KB**）→ **291.0 KB（75.8%）**（阶段 8，**+2.0 KB**），余量 ≈ 93.0 KB |
| `_Static_assert(sizeof(g_ftl)+FTL_AXI_RAM_RESERVED <= 384KB)`（`main.c`） | 过 |
| 二进制级核对 | `g_op_service` 48 B = 12 项；`[0]=0`、`[1]=ftl_op_sync_wait`、`[2]=ftl_op_host_io`、`[3]=ftl_op_journal`、`[4]=ftl_op_read_recovery`、`[5]=[6]=ftl_op_migrate`（GC / WL 共用同一引擎）、`[7]=ftl_op_l2p`（阶段 5 点亮）、`[8]=ftl_op_checkpoint`（阶段 6 点亮）、`[9]=ftl_op_bbt`、`[10]=ftl_op_wear`（阶段 7 点亮）、**`[11]=ftl_op_metadata`**（阶段 8 点亮 ⇒ **`[1]`~`[11]` 全部有主，`[0]` 恒 NULL**）（逐字节 dump `.rodata@0x0802ff50` 核对；`[5]`/`[6]` 同为 `0x0801a081`、`[9]=0x0800ee59`、`[10]=0x0801cc1d`、`[11]=0x080185d9`，各为对应符号 + thumb 位）|
| 旧同步 Checkpoint 入口 | `checkpoint_create` / `checkpoint_flush_dirty_regions` 从源码与符号表**双双消失**（`nm` 无命中，全仓只剩注释引用）；`metadata_dir_flush_to_anchor` / `metadata_ring_prepare_write` / `journal_truncate` 保留为同步逃生口，前者仍被 `metadata_dir_compact`（阶段 8）调用、中者被 `bbt_save` 与 `checkpoint_write_initial` 调用、后者**零调用者**（链接期 gc）<br>⚠ 阶段 8 后：`metadata_dir_compact` 与 `metadata_dir_flush_to_anchor` **双双消失**，`metadata_ring_prepare_write` 仍被 `bbt_save` / `checkpoint_write_initial` 调用（两处都是结构性同步）|
| 旧同步 BBT / Wear 入口 | `bbt_flush` / `wear_flush` / `wear_write_page` / `wear_write_range` / `wear_compact` / `wear_dirty_get` 从源码与符号表**双双消失**（`nm` 无命中）；`bbt_save` 保留为**开机 / 格式化**同步逃生口（`ftl_media_format`、CLI `bbt --erase` 用），`wear_init` 保留为只读回放 |
| 旧同步搬移入口 | `gc_migrate_sb` / `gc_erase_and_free` / `wl_relocate` / `wl_execute` / `wl_predictive_relocate` / `ftl_gc_reclaim_one` 全仓**只剩注释引用** |
| `gc_read_page_with_lba` | 形参 6 → 4（删死出参 `ecc_out` / `read_count_out`），唯一调用者是 `ftl_rebuild_closed_sb_list` |
| 旧 L2P API | `l2p_cache_find_or_load` / `l2p_cache_flush_one` / `l2p_cache_flush_all` 全仓**只剩注释引用**；`l2p_cache_read` 的 11 个调用点全部按三态改写 |
| 测试源 | 无引用需同步（`tests/` 对 L2P cache API **零引用**，`UT=FTL` / `UT=FC` 改名后仍绿） |

⚠ **阶段 5 的体积按等路径背靠背实测**：`git archive a6e0f2b` 与当前工作树各解到
**等长路径**（`/tmp/b_s4`、`/tmp/b_s5`）后完整构建，得 debug `.text` 194096 → 197056
（**+2960 B**）、release `.text` 117108 → 118440（**+1332 B**）、`.bss` **不变**。
debug 的 +2984（表内）与 +2960（等路径）差 24 B，即下条所述的路径伪影噪声。

⚠ **阶段 6 的体积同样按等路径背靠背实测**：`git archive 27addb8` 与当前工作树各解到
**等长路径**（`/tmp/b_s5`、`/tmp/b_s6`，均 9 字符）后完整构建：

| 等路径实测 | 阶段 5（`/tmp/b_s5`）| 阶段 6（`/tmp/b_s6`）| 增量 |
|---|---|---|---|
| debug `.text` | 197056 | 200068 | **+3012 B** |
| debug `.bss` | 385596 | 385948 | **+352 B** |
| debug `g_ftl` | 273792 | 274128 | **+336 B** |
| debug `.bin` | 197580 | 200596 | **+3016 B** |
| release `.text` | 118440 | 120332 | **+1892 B** |
| release `.bss` | 385620 | 385940 | **+320 B** |
| release `.bin` | 118960 | 120856 | **+1896 B** |
| 告警 | debug 0 / release 3（既有）| 同左 | — |

✅ 交叉校验：`/tmp/b_s5` 的 release `.text` **118440**、debug `.bss` **385596**、
`g_ftl` **273792** 与表内阶段 5 记录**逐字节吻合** ⇒ 本方法论可复现历史值。debug `.text`
的表内值（+3036）比等路径（+3012）多 24 B，即路径伪影噪声。

⚠ **debug `.text` 含构建路径伪影**：`ASSERT` 宏把 `__FILE__` 打进 debug 的 `.rodata`，
路径每长 1 字符 ≈ **+42 B**（≈ 40 个带断言的 TU）。故「同一提交在不同目录构建，
debug `.text` 不同」（实测 `/tmp/s4` 7 字符 vs `/tmp/b_s4` 9 字符差 84 B；仓库路径比
`/tmp/b_s5` 长 24 字符差 ≈ 960 B）。**release 不受影响**（ASSERT 编译期消失）——
故 release 行与历史值**逐字节吻合**，debug 行的历史值只在其自身环境下有效。
**判体积增量必须等路径背靠背**，否则读到的是路径差而不是代码差。

📏 **口径说明（易踩）**：本表的 `.text` / `.bss` 行是 **`arm-none-eabi-size`** 的
`text` / `bss` 列，**不是** `*.size` 文件里的 `.text` / `.bss` 段尺寸。两者不同：
`size` 的 `text` = `.text + .rodata + .data`（+ 少量段），`size` 的 `bss` 还含
链接脚本里约 **91 KB** 的 NOLOAD 预留（栈 / 堆 / `FTL_AXI_RAM_RESERVED`）。
以阶段 6 的 debug 为例：`size` 报 `text 201052 / bss 385948`，而 `.size` 文件报
`.text 172484 / .bss 294948`。**同一张表里只有 `g_ftl` / `.bin` / AXI_RAM 三行是直接读数**，
其余按 `size` 口径。⚠ 这也是为什么「路径伪影在 `.text` 里」与「路径伪影在 `.rodata` 里」
**并不矛盾**——前者是 `size` 口径（`.rodata` 被折进 `text`），后者是段口径。

⚠ **阶段 7 的体积按等路径背靠背实测**：`git stash` 前后各完整构建一次（**同一工作树、
同一路径**，`rm -rf build/{debug,release}` 后重建）：

| 实测（`size` 口径）| 阶段 6（`687bb73`）| 阶段 7 | 增量 |
|---|---|---|---|
| debug `text` | 201052 | 203920 | **+2868 B** |
| debug `bss` | 385948 | 386460 | **+512 B** |
| debug `.bin` | 201580 | 204444 | **+2864 B** |
| release `text` | 120332 | 121928 | **+1596 B** |
| release `bss` | 385940 | 386484 | **+544 B** |
| release `.bin` | 120856 | 122448 | **+1592 B** |
| `g_ftl` | 274128 | 274656 | **+528 B** |
| AXI_RAM | 288.5 KB（75.1%）| 289.0 KB（75.3%）| **+0.5 KB** |
| 告警 | debug 0 / release 3（既有）| 同左 | — |

✅ 交叉校验：同一路径重建阶段 6 得 `size` 的 **201052 / 385948**（debug）与
**120332 / 385940**（release），与表内阶段 6 记录**逐字节吻合** ⇒ 口径确认无误。
**+528 B 的 `g_ftl` 全部可解释**：`wear_state_t.dirty_snapshot[FTL_BLOCK_TOTAL/8]`
= 512 B，加 `bbt_job_t` / `wear_job_t` 两个 job 状态 16 B（`.text` 的 +2868 是
两套 job 状态机 + 收尾服务函数）。

⚠ **上板验证未做**（留待用户）。上板判据：
① `wr/rd/tr/fl/info/map` 冒烟、断电重启数据仍在；
② 开机**无**格式化横幅（`FTL_MEDIA_LAYOUT_VERSION` 未变，仍是 5U）、`info` 的
free/op/spare 与 v0.74 后一致；
③ **写满 Journal 一页时完成回调内不再出现嵌套泵**（阶段 2 的核心判据）；
④ **构造读失败（注入 ECC 错误）后 RR 不再阻塞完成泵，host 读最终返回**（阶段 3 的核心判据；
成功/失败都报 IO_ERROR 是既有语义，见上）；失败时可见
`[FTL] read unrecoverable lba=… gblock=… retry=…`，提交侧卡死可见 `[RR] submit stalled`；
⑤ Journal 刷页 job 的心跳可见（`[JOB]` 心跳尚未加，暂以 `[JRN]` / `[RR]` 失败日志观察）；
⑥ **搬移期间 host I/O 仍被服务**（**本阶段的核心判据**）：持续写触发 GC 时，
`wr`/`rd` 与后台搬移在 superloop 上重叠，不再出现"一次 `wr` 卡住数十毫秒"；
`[GC] … reclaimed=` 持续推进；
⑦ **池耗尽 + 持续写 ⇒ 写最终成功、不丢数据、不死锁**（**专项**，验证 OP 预留池兜底）：
写路径取不到块时返回 `FTL_AGAIN` 背压（不再是 `FTL_ERROR`），GC job 推进后写自动完成；
若 GC 因池空中止，可见 `[MIG] no free block page=… — abort, victim kept` 而**数据无损**；
⑧ 提交侧卡死可见 `[MIG] stall … rounds — abort`（不擦 victim）；
⑨ **host 读未命中 region 时不再有整条流水线停顿**（阶段 5 的核心判据）：随机读满盘
（`rd` 扫过大量 region）时 `ftl_service` 的每轮耗时应显著低于改动前，且**不出现**
`[FTL] drain stalled`（`l2p_defer` 保证加载在飞不计卡死）；
⑩ **host 写首次触碰 region 不再阻塞完成泵**：写满盘过程中不出现嵌套泵；若某 region 仍在
加载，可见该写组**多停一轮**（停泊）但**最终完成**，`info` 的 `total_writes` 与写次数相符；
⑪ **断电重启后 `map` 一致**（**专项**，验证 5a 的「完成时重查」不变量未破）：写入 → 断电 →
重启 → 逐 LBA `map` 比对；再对**同一 LBA 连写两次后立刻断电**，重启后 `map` 应指向**后一次**
的 PBA（若停泊把重查顺序搞乱，会指向前一次）；
⑫ `fl` 返回后**立即**断电重启，`map` 与断电前一致（验证 `l2p_cache_flush_all_sync` 连
`FLUSHING` 行一起等）；
⑬ `[L2P] thrash=` 只在**真随机**负载下出现（写密集随机 LBA 也应能触发，验证 ② 的记账口径
已修）；连续顺序读写**不应**触发；
⑭ **Checkpoint 落盘期间 host I/O 仍被服务**（**阶段 6 的核心判据**）：触发一次 CP
（等 10s 定时 / 写满 `CP_WRITE_INTERVAL=1024` 条 / journal ≥85%），期间持续 `wr`/`rd`
应照常完成，不再出现"一次 `fl` 或一次定时 CP 卡住数十毫秒"；卡在某个 phase 可见
`[CP] job phase=%u wait=%u fail=%u age=%lums` 心跳；
⑮ **`fl` 后立即断电重启，`map` 与断电前一致**（阶段 6 判据）：`fl` 走 `l2p_cache_flush_all_sync`
（含在飞 `FLUSHING` 行），**不**触发 CP；要判 CP 落盘需等一次真 CP 走完
（`[CP] done seq=… jrn_seq=… anchor=written|skip`，`log level debug` 才打）；
⑯ **CP 中途掉电不丢映射**（**专项**，验证 ③ 的快照点前移）：写一批 → 在 CP 进行中
（`[CP] job phase=…` 可见时）**立即断电** → 重启 → `map` 应与"断电前最后一次成功写完的
LBA"一致；`boot type=` 应为 `2`（JOURNAL_REPLAY）而非 `1`，且 `journal_entries` 覆盖
断电瞬间的那批写；
⑰ **CP 中止路径可观测**：构造 `journal_truncate_plan` 水位不满足（CP 期间再写 ≥2 条），
应**不**出现 `[CP] abort`（那是另一条路径），而是正常 `done`；构造脏行 flush 失败可见
`[CP] abort at phase=…`，且**无**新 Header 落盘（重启后 boot 用上一个 CP + 回放 journal）；
⑱ **`[CP] journal pre-erase failed blk=… — 仍记账（沿用旧语义）`** 若出现，说明沿用旧语义
的路径被踩到（不是新缺陷，但值得记录）；
⑲ **目录压缩期间 host I/O 仍被服务**（**阶段 8 的核心判据**）：持续写满盘触发日志块换块 +
池吃紧（`[META] compact start victim=…` 需 `log level debug`）时，`wr`/`rd` 照常完成，
不再出现"一次写卡住数十毫秒"；压缩走完可见 `[META] compact done victim=… moved=…`；
⑳ **断电落在压缩中途后重启能恢复**（**专项**）：在 `[META] compact start` 之后、
`compact done` 之前断电 → 重启 → `map` 与"断电前最后一次成功写完的 LBA"一致
（③④ 契约：Anchor 未落盘则 victim 不擦，已搬的 region 在 RAM 里指向新页但 Anchor 仍是旧的
⇒ 回放 journal / 用旧 Anchor 都应得到一致映射）；victim 擦失败的
`[META] reclaim erase failed blk=…` 与 `[META] compact: victim %u 仍有 %u 活 region — 不擦`
是**预期**的非致命日志；
㉑ 池吃紧 + 持续写时，`l2p_cache_flush_kick` 的 `FTL_AGAIN` 背压路径不产生
`[FTL] drain stalled` / 写组 `IO_ERROR`——写只是**多停几轮**，最终完成（与阶段 4 的
`FTL_AGAIN` 背压同族）。

⚠ 工具坑（沿用）：`release.elf` 被 strip ⇒ 构建脚本的 SRAM usage 报告对 release 恒 0.0 KB；
`BIN_DIR` 共享 ⇒ 跑 `make tests` 会覆盖 `debug.elf`，判体积须先重建再量。
⚠ 体积判定另有两坑：① **源无改动时 `make` 只跑 clang-format 不编译**，此时日志里没有编译
输出、也**不会**打 SRAM 报告；"0 warning"可能只是没编译 ⇒ 判体积/判告警前必须 `touch` 源。
② **`make tests` 之后直接 `make debug` 是空操作**（同一 `OBJ_DIR`/`BIN_DIR`，CFLAGS 不被跟踪
⇒ make 认为已是最新），`size` 读到的是**测试镜像**（`.text` ≈ 272 KB）；必须 `make clean`
后再 `make debug`。

#### 阶段 8 —— metadata 目录压缩 / 换块异步（本次，最后一阶段）

`metadata_dir_compact` 是最后一个同步循环，也是**唯一一个被完成泵直接踩到**的：
`ftl_op_l2p` 的 `FLUSHING` 分支 → `metadata_dir_commit_region` → 旧版内部的
`metadata_dir_reclaim_block` → `ftl_sync_erase_block` ⇒ **在完成服务函数里再跑一遍完成泵**。
它同时还是全仓最后一个"一次调用阻塞数十毫秒"的站点（最多 `METADATA_DIR_ENTRIES` 次
逐 region 读 + 写 + 一次 Anchor 八页落盘 + 一次擦）。本阶段把这条链拆成三件事。

##### ① 三件事分离（本阶段的结构性改动）

| 旧 | 新 |
|---|---|
| `metadata_dir_compact`（同步跑完整条：选 victim → 逐 region 读/写/登记 → Anchor 落盘 → 擦 victim）| `md_job_t` 状态机 + `metadata_dir_kick` 起步/续跑 + `ftl_op_metadata` 收尾 |
| `metadata_dir_commit_region` 内部**就地** `metadata_dir_reclaim_block`（擦） | 只**登记** `job.reclaim_target`（块号），擦由 job 的 `MD_JOB_RECLAIM` 阶段异步做 |
| `metadata_dir_append_slot` 换块时**就地**调 `metadata_dir_compact`（防重入靠 `dir->compacting`）| 只**登记** `job.compact_wanted` 并返回 `NAND_PBA_INVALID`；压缩由 job 做 |

```
MD_JOB_IDLE ─┬─ reclaim_target 有值 ──→ MD_JOB_RECLAIM ──→ IDLE
             └─ compact_wanted == 1 ──→ MD_JOB_CP_READ ──→ MD_JOB_CP_WRITE ──→（回 CP_READ，逐 region）
                                                          └─扫完→ MD_JOB_CP_ANCHOR_PREP
                                                                   ├─ dirty==0 → MD_JOB_CP_RECLAIM
                                                                   └─ [CP_ANCHOR_ERASE] → CP_ANCHOR_WRITE（逐页）
                                                                          → MD_JOB_CP_RECLAIM → MD_JOB_DONE → IDLE
```

`ftl_op_metadata`（`ctx = &ftl->metadata_dir.job`）**不推进状态机**——与 `ftl_op_checkpoint`
同族：`MD_JOB_CP_ANCHOR_PREP` 里游标未建立时会调 `metadata_ring_locate`（内含
`ftl_sync_read_page`），在完成泵里跑就是嵌套泵。故它只"记结果 + 改 phase + release"，
推进交给下一轮 `ftl_service` 的 `metadata_dir_kick`。

##### ② 关闭的不变量违例（本阶段最实质的收益）

旧版从 `ftl_op_l2p` 的完成服务函数里同步擦块，`l2p_sync_pump` 首行的
`ASSERT(ftl_op_in_dispatch() == 0)` 在**异步路径**上根本拦不到它（那条路径上 pump 根本没被调用）。
本阶段后：`metadata_dir_commit_region` **不碰 NAND**（函数头明写），
**全仓再无「在完成泵内同步等 NAND」的路径**（与阶段 5 的结论合并成立）。

##### ③ 背压与擦除放大（与阶段 7 同族的两个必须钉死的东西）

- **`job.slot` 跨背压保留**：`metadata_dir_slot_now` 取槽时**立即**推进 `cur_metadata_page`
  （写失败页可能半编程、不可复用）。故提交被背压拒时**只重试同一槽**，绝不重取——重取会白烧一页。
  对应 `md_compact_write` 的 `if (j->slot == NAND_PBA_INVALID)` 只取一次。
- **`stall_rounds` 只在提交成功后清零**（`md_submit_write` / `_erase` / `_read` 三处同构，
  与 `cp_submit_*` / `mig_submit_*` 逐字一致），超 `MD_SUBMIT_STALL_MAX = 256` 轮即
  `md_job_abort`。
- **不重走换块判定**：`MD_JOB_CP_ANCHOR_ERASE` 提交失败就 `md_stall` + `return`（phase 不动），
  下轮重试**同一次擦**——若回 `IDLE` 重算，`prepare_addr` 的 `need_erase` 仍为 1 ⇒ 每轮擦同一个
  Anchor 块 = 磨损放大。

##### ④ `metadata_dir_busy()`：让 CP 不把长压缩误判成卡死

一次 compact 最多 ~140 轮（2048 region 里命中 victim 的那些，每轮一读一写两条命令）。
`cp_flush_regions` 的 `CP_SUBMIT_STALL_MAX = 256` 会被它耗尽而**误中止 CP**。修法：把
`metadata_dir_busy(&ftl->metadata_dir)` 加进 stall 复位条件——

```c
if ((ftl->cache.flush_inflight > 0U) || (metadata_dir_busy(&ftl->metadata_dir) != 0))
{ j->stall_rounds = 0U; return 0; }   /* 在推进，不是卡死 */
```

`metadata_dir_busy()` 是 `metadata.h` 的 `static inline`（`job.phase != MD_JOB_IDLE`），
当前唯一读者就是这里。

##### ⑤ `l2p_cache_flush_kick` 的 `NAND_PBA_INVALID`：**背压不是失败**

`metadata_dir_append_slot` 现在会在三种情况下返回 `NAND_PBA_INVALID`：两 plane 池皆空、
整理 job 在飞（一律让路）、刚换块且池吃紧（登记 `compact_wanted`）。旧版 flush_kick 把它
当 `FTL_ERROR` ⇒ **`l2p_flush_kick` 跳过该行、`cp_flush_regions` 直接中止整条 CP**。
本阶段改为 **`FTL_AGAIN`**（下轮重试），理由：

- 前两种都是**瞬态**（job 走完、池回收后槽就有了），不是"这行脏数据写不出去"；
- `l2p_cache_flush_kick` 的调用者（`l2p_flush_kick` / `cp_flush_regions`）本来就有
  `FTL_AGAIN` 背压路径。

##### ⑥ CLI `fl` 死锁修复：`l2p_sync_pump` 现在会推进 md job

`l2p_sync_pump` 原先只跑 `ftl_service_pump()`（**实例无关**，不含各实例的 `*_kick`）⇒
CLI `fl` 若撞上"换块 + 池吃紧"，`append_slot` 返回 `NAND_PBA_INVALID`、`compact_wanted=1`，
而**没有任何人**推进 md job ⇒ 自旋到超时。改法：`l2p_sync_pump(l2p_cache_s *cache)` 多收
一个 cache，跑完 pump 后 `metadata_dir_kick(cache->md_dir, cache->alloc)`（三处调用点同步改）。
它仍以 `ASSERT(ftl_op_in_dispatch() == 0)` 开场，语义不变。

##### ⑦ 失败分支逐条对照（`不顺手改语义`）

| 阶段 | 旧同步行为 | 异步版 |
|---|---|---|
| region 读失败 | `st = FTL_ERROR; break` ⇒ 不 Anchor、不擦 victim | `md_job_abort`（victim 不擦）⇒ 一致 |
| 取槽失败 | `append_slot` 返回 INVALID ⇒ `st = FTL_ERROR; break` | 空槽预算在 `md_compact_begin` 一次核准（`l2p_live[victim] ≤ PAGES_PER_BLOCK − cur_page`），扫描期间无人发放槽位 ⇒ `metadata_dir_slot_now` 不应失败；万一失败则 `LOGW` + 中止 |
| region 写失败 | `st = FTL_ERROR; break`（槽已消耗、不登记） | `md_job_abort`，`job.slot` 随 reset 清掉 ⇒ 一致 |
| Anchor 写失败 | `flush_to_anchor != OK` ⇒ 不擦 victim；游标**照推**（半编程槽不可复用）、dirty 保留 | 逐页写时**无论成败都 `anchor_chunk++`**；整条写完 `anchor_commit(..., ok)` 照推游标，`ok==0` 则 `LOGW` + `md_job_abort` ⇒ 一致 |
| Anchor 换块擦失败 | `metadata_ring_prepare_write` 返回错误 ⇒ 不写、**不推进游标**、dirty 保留 | `MD_JOB_CP_ANCHOR_ERASE` 失败 ⇒ `md_job_abort`，游标不动 ⇒ 一致 |
| 扫描期间有 region 落回 victim | 末尾 `l2p_live[victim] == 0` 不成立 ⇒ 不 Anchor、不擦 | `MD_JOB_CP_RECLAIM` 消费前重查 `l2p_live[v] != 0` ⇒ 不擦（Anchor 多写一条，但目录**确实变了**，写是对的）⇒ 效果一致 |
| victim 擦失败 | `metadata_dir_reclaim_block` 返回 `FTL_ERROR`，被 `(void)` 丢弃；留 RESERVED + 活计数 0 | `LOGW` + 留 RESERVED（下次换块的全池 sweep 或新的登记再来）⇒ 一致但**可观测** |
| victim 擦成功 | `allocator_free_reserved_block` | 同 |

⚠ **`metadata_dir_reclaim`（append_slot 换块分支里的全池 sweep）保持同步**——这是本阶段
**有意保留的残留**：它只擦"活计数 0 且非当前块"的块，是 job 的 `reclaim_target` 被覆盖时的
**兜底**（单槽 target 覆盖不构成泄漏）。它跑的时机是 append_slot 换块分支，**不在完成泵内**，
且只在"刚换块"时跑一次。

⚠ **消费前重查（reclaim 与 victim 共用）**：`reclaim_target` 登记到消费之间可能隔了很多轮，
块号可能已被重新分配。故消费时三条重查——① `l2p_live[g] != 0`（又被引用了）；
② `g == cur_metadata_block`（正在被追加）；③ `md_block_is_reserved(alloc, g) == 0`
（已被别处归还/复用）。任一命中即放弃本次（target 清掉、回 IDLE），**绝不擦活块**。

##### ⑧ 删掉的同步入口与保留的逃生口

- `metadata_dir_compact` / `metadata_dir_flush_to_anchor` 从源码与符号表**双双消失**
  （`nm` 无命中，全仓只剩注释引用）。
- `metadata_dir_reclaim` / `metadata_dir_reclaim_block` **保留**（上面那条同步 sweep）；
  `metadata_dir_anchor_prepare` / `_page` / `_commit` 三段式保留（CP job 与 md job 共用）。
- `dir->compacting` 标志**删除**，两个作用都由 job 状态表达：① 抑制逐次立即回收 ⇒
  `md_compacting()`（phase ∈ [CP_READ, CP_RECLAIM]）；② 防重入 ⇒ `job.phase != IDLE`
  （`append_slot` 据此一律让路）。`metadata_dir_init` / `_load` 改为 `md_job_reset(dir)`
  （job 不跨重启——phase / 游标 / 请求位全是 RAM 态）。
- `md_job_t.cp_buf[L2P_REGION_SIZE]`（2 KB）**常驻 `.bss`**：读的数据留在 desc 链上，
  `ftl_cmd_read_data` 必须早于 release，缓冲必须活到完成 ⇒ 不能用栈缓冲（旧版是循环内的
  2 KB 栈帧）。

#### 后续

结构性必须顺序执行的路径（开机初始化 / 全片格式化 / 出厂坏块扫描 / 开机回放 / CLI）继续用
`ftl_sync_submit_wait`。**八阶段已全部落地，v0.75 收口。**

**v0.75 收口补丁（八阶段之后，未上板）**——计划书"贯穿各阶段的机制 1/2/5"的三条尾巴：

1. **`[JOB]` 心跳全量化**。新增 `include/ftl/ftl_job.h`（`ftl_job_now_ms` /
   `ftl_job_age_ms` / `ftl_job_heartbeat` / `ftl_job_heartbeat_reset` / `ftl_job_tick`），
   把原先只有 CP 一份的心跳抽成公共节拍：每满 `FTL_JOB_LOG_INTERVAL_MS`（1 s）打一行，
   **判据 = 两次心跳之间 phase 不动 ⇒ 卡死**（异步后没有 waiter 会超时，这是唯一观测口）。
   - 接入：`journal_kick` / `mig_service` / `rr_kick` / `bbt_kick` / `wear_kick` /
     `metadata_dir_kick` / `l2p_flush_kick`，加上原有的 `checkpoint_kick`。
   - 各 job 结构体（`journal_job_t` / `mig_job_s` / `ftl_read_recovery_t` / `bbt_job_t` /
     `wear_job_t` / `md_job_t` / `cp_job_t`）各增 `start_ms` / `log_ms` 两个基准字段
     （`l2p_cache_s` 只增 `job_log_ms`）。
   - **基准懒初始化**（首次见到在飞时记 start）⇒ 各 `*_begin` 无需显式赋值；只有 CP 保留
     显式起点（"job 起点"是快照时刻，不是首次心跳时刻）。
   - ⚠ **L2P 只报计数不报 age**：在飞的是若干 cache line，没有"单 job 的起点"可言 ⇒
     用 `ftl_job_tick` 单基准版本，打 `[L2P] job inflight load=/flush=`。
   - ⚠ 平台时钟缺失（`time_us` 未注册）时恒 0 ⇒ 心跳**永不触发**（纯观测，静默优于刷屏；
     与 GC 求值节拍的取舍相反——那边 0 是"尚未求值"哨兵，且它是**功能**）。
   - `CP_JOB_LOG_INTERVAL_MS` 删除（统一到 `FTL_JOB_LOG_INTERVAL_MS`）；
     `checkpoint_now_ms` 删除（改用 `ftl_job_now_ms`，`checkpoint.c` 不再直接依赖
     `platform/platform.h`）。
   - ⚠ `ftl.c` 的 `ftl_now_ms()` **刻意不动**（同一平台时钟，但归属"功能"节拍：GC 求值 /
     RR 巡检 / CP 定时触发）；两处等价实现在 `ftl_job.h` 顶部登记在案。

2. **`ftl_sync_submit_wait` 入口断言**（贯穿机制 2 的落地）：
   `ASSERT(ftl_op_in_dispatch() == 0)`——在完成派发里再开一个完成泵（嵌套泵）是本次异步化
   要消灭的形态。**目标终态**：本函数只被开机初始化 / 全片格式化 / 出厂坏块扫描 / CLI
   调用。新增调用点若触发断言 ⇒ 该功能尚未 job 化，应改走 `ftl_async_* + ftl_op_submit`。

3. **清死代码**（零调用点，逐条 grep 确认）：
   - `journal_truncate`（同步包装 = plan + 即刻擦 + commit）——阶段 6 把 Checkpoint job 化后
     唯一调用者消失；异步 CP 走 `journal_truncate_plan` → 异步擦 → `journal_truncate_commit`；
   - `ftl_sync_read_meta`——页内 META 统一经 `ftl_async_read_page` 的 desc 链完成数据读取，
     不需要单独的裸 META 原语。⚠ **判坏块看的是 OOB 标记**，那是 `ftl_sync_read_column`
     （`OP_READ_COLUMN`），**仍在用**，勿一并删。

**阶段 8 关闭的欠账**：
- ✅ `metadata_dir_compact` / `metadata_dir_flush_to_anchor` 同步版删除；
- ✅ `metadata_dir_commit_region` **不再擦 NAND** ⇒ 完成泵内嵌套泵的最后一条路径消失；
- ✅ `l2p_cache_flush_kick` 内部的 `metadata_dir_append_slot`（取空槽可能换块 ⇒ 擦）不再
  在取槽时同步跑完整条压缩——这正是阶段 5 记为"待阶段 8 再拆"的那一条；
- ✅ CP 的 `CP_FLUSH_REGIONS` 不再可能因压缩而瞬时阻塞（压缩是 job，且 `metadata_dir_busy`
  让它不被误判为卡死）。

仍待闭合（阶段 8 相关）：
- `metadata_dir_reclaim` 的全池 sweep **仍是同步**（见 ⑦ 末），且 `ftl_sync_erase_block`
  仍在 `metadata_dir_reclaim_block` 里——但该路径只在 `append_slot` 换块分支、且 job 空闲时跑；
- `MD_JOB_CP_ANCHOR_PREP` 里游标未建立时的 `metadata_ring_locate` 扫池定位仍是**同步**的
  （与 CP 的 `CP_ANCHOR_PREP` / BBT 的 `bbt_kick` 同族的**一次性冷启动**同步点），
  收益极低，**有意不做**；
- md job **无 watchdog**（与 journal / mig / RR / CP / BBT / Wear 一致）：NAND 层超时由
  `nand_dispatch_tables.c` 聚合成 `FC_CW_STATUS_TIMEOUT` 的**完成**送达，故"在飞"必有终态；
- per-job 的 `[JOB]` 心跳日志：✅ **已补齐**（见上文"v0.75 收口补丁"）——`[MD] job phase=…`
  每满 1 s 一行。原先只能靠 `log level debug` 下的 `[META] compact start/done` /
  `log-block:` 观察。

**阶段 7 关闭的欠账**：
- ✅ BBT / Wear 两处"攒批 + 后台刷"已 job 化：`bbt_flush` / `wear_flush` 及其内部
  `wear_write_page` / `wear_write_range` / `wear_compact` 全部删除，改由
  `bbt_kick` / `wear_kick` 起步、`ftl_op_bbt` / `ftl_op_wear` 收尾；
- ✅ 提交被背压拒时**不重走换块判定**（`bbt_job_t.waiting` / `wear_job_t.waiting`），
  否则每轮重擦同一块 = 擦除放大；
- ✅ Wear 的跨轮批用「起点快照 + 起点整体清零脏位图」拿到与原同步整批等价的原子性。

仍待闭合（阶段 7 相关）：
- `bbt_kick` 在游标未建立时会调 `metadata_ring_locate` 扫池定位（内含
  `ftl_sync_read_page`）——与 CP 的 `CP_ANCHOR_PREP` 同族的**一次性冷启动**同步点，
  收益极低，**有意不做**；
- BBT / Wear job **无 watchdog**（与 journal / mig / RR / CP 一致）：NAND 层超时由
  `nand_dispatch_tables.c` 聚合成 `FC_CW_STATUS_TIMEOUT` 的**完成**送达，故"在飞"必有终态。
  两批都只有 1~25 页、毫秒级，1 s 心跳永远打不出来 ⇒ 可观测性靠 `LOGE`；
- per-job 的 `[JOB]` 心跳日志：✅ **已补齐**（见上文"v0.75 收口补丁"）——`[BBT] job phase=…`
  / `[WEAR] job phase=…`。⚠ 两批都只有 1~25 页、毫秒级，1 s 心跳实际上打不出来，
  可观测性仍主要靠 `LOGE`；心跳的价值在于"万一真卡住"。**无 watchdog** 的结论不变。

**阶段 6 关闭的欠账**：
- ✅ 同步逃生口 `l2p_cache_*_sync` 的四个调用族里，**checkpoint 一族消失**——
  `CP_FLUSH_REGIONS` 改用 `l2p_cache_flush_kick`（起异步写回 + 等 `flush_inflight` 归零），
  不再在服务循环里逐条自旋。剩下三族（开机回放 / F2 重建 / CLI `map` 与 TRIM）是**结构性
  同步**（在 `main` 进 superloop 前执行，无人 drain CQ）或 CLI 上下文，**会长期保留**。

**阶段 5 关闭的欠账**（原「已知仍待闭合」前两条）：
- ✅ `ftl_confirm_write` 的 L2P 重查已 job 化（停泊 + 两遍式），**「完成时重查」不变量保留**；
- ✅ `rr_select_plane`（阶段 3）与 `mig_step`（阶段 4）里的 `l2p_cache_read` 已三态化——
  前者在 `RR_STAGE_PLANE` 原地等，后者拆出 `MIG_READ_SETTLE` / `MIG_WRITE_SETTLE`
  两个纯计算 phase。**全仓至此不再有「在完成泵内同步等 NAND」的路径**。

仍待闭合：
- `l2p_cache_flush_kick` 内部仍会调 `metadata_dir_append_slot`（取空槽可能换块 ⇒ 擦），
  阶段 8 再拆。⚠ 这条同样意味着 **CP 的 `CP_FLUSH_REGIONS` 仍可能瞬时阻塞一次擦除**——
  但只在"取空槽恰逢换块"时，且是**同步站点当场擦**而非整条 CP 阻塞；
- `CP_ANCHOR_PREP` / `CP_HDR_PREP` 里游标未建立时的 `metadata_ring_locate` 扫池定位仍是
  **同步**的（内含 `ftl_sync_read_page`）——这是本 job 唯一剩余的同步点，且**只在冷启动后
  首次 CP** 出现（开机 `checkpoint_find_latest` 已建立游标时零代价）。异步化它需要把
  "扫池"也拆成 phase，收益极低（一次性），**有意不做**；
- per-job 的 `[JOB]` 心跳日志：✅ **已全量化**（见上文"v0.75 收口补丁"）——CP 的
  `[CP] job phase=…` 之外，journal / mig / RR / BBT / Wear / MD / L2P 各自一行。

---

### v0.76 — 时间预算后台调度器（三级后台升级：每任务 interval + 每轮总预算 + 优先级）

> **RAM 侧调度器重构，无介质变更**（`FTL_MEDIA_LAYOUT_VERSION` 不变），仿 v0.63/v0.68
> 先例**同批上板**。`ftl_service` Phase 3 的三级 static 服务函数被 `bg_scheduler` 引擎 +
> 14 项任务表 + 三级负载→预算映射取代；三级行为与 v0.75 **等价**。

**日期**：2026-10-15

**范围**：新增 `include/ftl/bg_scheduler.h` + `src/ftl/bg_scheduler.c`（策略无关引擎，
只依赖 `platform/platform.h` 的 `time_us`）、新增 `tests/unit/test_ftl_bg.c`（`/ftl/bg/`
套件 6 用例）、`include/ftl/ftl.h`（`bg_sched_s bg` 字段 + `ftl_bg_init` /
`ftl_bg_level_select` 声明）、`src/ftl/ftl.c`（删三个 static 三级服务函数 → 14 个
`bg_a_*` 适配器 + `g_bg_level_cfg[3]` + `ftl_bg_init` / `ftl_bg_level_select` + `ftl_service`
Phase 3 改写）、`tests/unit/ut_main.c` + `src/ftl/makefile.mk`（套件接线）、三份文档
（`ftl_design.md §12.4` 对比表修正 + `ftl_optimization_plan.md §九` 波 4.5 + 本文件）。

#### 起因：计数式三级 + 节拍门散落 + 前台保护一刀切

v0.75 的三级后台按「未决命令数」分档（IDLE/NORMAL/BUSY），档内按「做几条」计数控制
（`BG_NORMAL_FLUSH_COUNT=4` / `BG_BUSY_FLUSH_COUNT=1` / `NAND_SUBMIT_BUDGET=4`），
节拍门散落在各处（GC 求值 10ms 哨兵、CP 定时 10s、RR 巡检 1s、wear 内部 120s 双门），
前台保护是「档位一刀切」而非随负载缩放——离主流主控 FTL 的
`background_task_t`（每任务 interval + 优先级 + 预算）形态有距离。

#### 目标（用户 2026-10-15 定）

1. **对齐主流形态**：每任务独立 `interval_ms` + 每轮总预算 + 优先级（§12.4 `background_task_t`）。
2. **前台保护升级为「负载→预算缩放」**：三级档位各配总预算 + l2p 条数上界 + 任务掩码。
3. **节拍门统一进任务表**（唯一例外 = wear 内部双触发门，见下）。
4. **正确性关键集显式化**为 `must_run`：无条件、不 gate、不占预算、每轮最先跑。

#### 阶段

1. **引擎**（`bg_scheduler.h/c`）：`bg_task_s` / `bg_sched_s` / `bg_level_cfg_s` +
   `bg_scheduler_init` / `bg_scheduler_run`。两遍 Pass：Pass 0 跑 must-run 集
   （`checkpoint_kick` 排第一），Pass 1 按掩码 + interval 门 + 优先级线性扫、`charge =
   MIN(elapsed, budget_us)`、`remaining < MIN_TASK_BUDGET_US`(250) 截断。
2. **14 项任务表**（`ftl_bg_init`）：0~5 = must-run（checkpoint_kick / rr_kick /
   mig_service / metadata_dir_kick / journal need_erase / 紧急 CP），6~13 = 预算任务
   （GC 求值 10ms/400 / WL 300 / l2p flush 1500 / journal flush 700 / bbt 700 /
   wear 700·interval=0 / CP 触发 100 / RR 巡检 1s·300）。掩码位下标 == 表下标。
3. **三级负载→预算映射**（`g_bg_level_cfg[3]`，锚定 `onfi_1_0.c` 时序）：IDLE/NORMAL
   2000µs（≈2~3 页写 + GC 全表求值）、BUSY 700µs（仅 must-run + 1 l2p flush）；
   l2p 条数上界 IDLE=64 / NORMAL=4 / BUSY=1 —— **确定性前台保护由档位→条数/掩码承担**
   （kick 全部提交即返回，elapsed 预算受 1ms 时钟量化影响，只做 CPU 时间上界）。
4. **时钟缺失降级**：`time_us` 恒 0 ⇒ interval 门失效（`last_run_ms` 恒 0 = "未运行"哨兵）、
   elapsed=0 永不截断 ⇒ 退化「无门控」，靠各任务内部单飞行/事件门防刷屏（GC 哨兵
   「每轮求值」等既有语义不变）。
5. **wear 例外**：调度器 interval 设 0，内部 OR 双门
   （`WEAR_FLUSH_INTERVAL_MS` 120s OR 写增量≥1024）为准——写增量触发是掉电窗口上界。

#### 决策记录

- **`bg_scheduler.c` 策略无关**：不 include 任何 kick，只 include `platform/platform.h`；
  `ftl_context_t` 前向声明（同 `pending.h` 手法）⇒ 可被单测自定义 cfg 驱动。
- **`ftl_bg_level_select` 纯函数**：`pending_count==0 → IDLE`；`> FTL_PENDING_MAX/2 ||
  queued!=0 → BUSY`；否则 NORMAL——逐字复刻 v0.75 的档位判定。
- **表位 5（紧急 CP）移入 must-run**：`checkpoint_is_emergency` 每轮检查（原仅 BUSY）；
  `checkpoint_begin` 单飞行（`cp_job.active → FTL_AGAIN`）幂等安全，属**刻意的行为放宽**
  ——紧急水位（journal≥95%）必须随时响应。
- **表位 4（journal need_erase）是表位 9 的子集**：IDLE/NORMAL 双跑安全
  （`journal_kick` 单飞行）。

#### 收口

- `make all` / `make tests UT=FTL` / `make tests UT=FC` 三锚点全绿；`.bin` 214084B
  （v0.75 栈哨兵后 212024B，+2060B）。
- `test /ftl/bg/` 6 用例：任务表接线 / interval 门控 / 预算截断 / 时钟缺失 / BUSY
  must-run / level select。
- ⚠ **未上板**：判据 = 三级行为与 v0.75 等价回归（IO 下 BUSY 无后台刷屏 / 空闲 IDLE
  正常补池）+ `test /ftl/bg/` 板上过 + 断电重启数据仍在。
- ⚠ 生产适配器（bg_a_* 起 journal 擦 / mig 读 / checkpoint cp_step）的真完成链归异步套件
  （`../../archive/ftl_ut_munit.md` §4.9 计划）职责，`/ftl/bg/` 用全 14 计数桩覆盖引擎语义。

### v0.77 — Allocator 不变量收口（账本访问器 + 三池对称 push/pop；纯重构，无介质变更）

> **RAM 侧重构，无介质变更**（`FTL_MEDIA_LAYOUT_VERSION` 仍 5U）。目标不是改行为，
> 而是把 allocator 的不变量从「注释纪律」搬进 API。

**日期**：2026-10-16

**起因**：用户读 `allocator.c` 反馈「阅读起来有点难」。只读审计结论——难点不在单个函数，
而在**同一不变量有大量外部模块手工维护**（`free_count` 5 读 / op·spare 池 4 裸写 /
`closed_sb_list` 12 读 / 手工地址转换 4 处 / `sb->valid` 2 读），正确性寄托在注释和调用方
纪律上，无机器强制。

**改动**（`allocator.h` + `allocator.c` + `main.c` + `bbt/gc/wear_leveling/migrate/metadata/
power_failure_recovery/ftl/cli`）：

1. **谓词** `super_block_is_open/is_closed()`。`valid` 语义是反的（1=Open/0=Closed），
   调用点裸比看不出含义；读反 ⇒ 条目对 GC/WL 永久不可见 ⇒ 空闲池只出不进（v0.67 同形态）。
2. **只读访问器** `allocator_free_count()` / `op_count(plane)` / `spare_count(plane)` /
   `closed_count()` / `closed_at(i)`。越界读当场 ASSERT。`closed_at` 形参收 const、返回
   非 const（`closed_sb_list` 是 `super_block_s *` 成员，const 容器下天然非 const 左值）
   ⇒ 无需动 `gc_select_victim` 签名。
3. **地址解码单点**：`gblock /`、`% BLOCKS_PER_PLANE` → `ftl_gblock_plane/block`（与
   `ftl_gblock` 严格互逆）。
4. **三池对称 push/pop** `allocator_{op,spare}_try_push/try_pop()`。满/空返回 0 而非
   ASSERT（判据 3：`FTL_OP_BLOCKS_PER_PLANE` 可为 0，池满在合法配置下可达）。
   `main.c` 回填的 4 处裸写（v0.77 前唯一绕过全部 API 的地方）与 `bbt_spare_pool_refill`、
   `allocator_pull_from_op` 的手写记账一并收口。push/pop 只动数组+计数，不碰 BST state。
5. **`allocator.h` 文件头新增「不变量」四条**：① 四本账各有唯一闸门；② `FTL_BLOCK_STATE_FREE`
   **会说谎**（`allocator_init` 早于元数据目录加载，把全部非坏块无条件置 FREE，而元数据块
   不进池）⇒ 选块一律以 Free Pool **数组**为准；③ `valid` 反语义；④ `closed_sb_list` 是
   swap-remove 活列表，借出指针不跨调用有效。

**收口**：`make all` / `make release` / `make tests UT=FTL` 三锚点全绿、零新告警（release 仅剩
3 条既有告警）。`.bin` 212772B → 215036B（+2264B，访问器 ASSERT 内联展开，release 裁空）。
UT 零改动（纯新增 API，结构体字段仍公开）。⚠ **未上板**：判据 = 冷启动 → 顺序写 → GC 冒烟 +
`ftl info` 计数与改动前一致。

**顺带发现（已在本轮修，见 v0.78）**：`allocator_close_sb` 在 `closed_sb_count >=
SUPER_BLOCK_LIST_MAX` 时**静默丢弃**条目，而 `allocator_add_closed_sb` 同条件 **ASSERT**
——两者不对称，且根因是容量取值本身错了。

**范围外**：closed 列表稳定槽位（要改 GC victim 遍历语义）；删 `FTL_BLOCK_STATE_FREE` 位
（生产零读者，但删前要先决定元数据块置 `RESERVED` 还是 `state=0`，且 `bbt_mark_bad` 需补一次
free-pool 驱逐 ⇒ 改 `free_count` 可观测值，**需上板**）。

### v0.78 — 修 Closed SB 列表容量：`SUPER_BLOCK_LIST_MAX` 1024 → `FTL_BLOCK_TOTAL`

> **RAM 侧变更（AXI_RAM +36 KB），无介质变更**（`FTL_MEDIA_LAYOUT_VERSION` 仍 5U）。

**日期**：2026-10-16

**缺陷**：容量 1024 是**估值**不是上界，而列表的真实需求就是「所有不在 Free Pool 里、
又不属于 Open SB 的块」：
- 掉电重建**每写过一个物理块建一条**退化单 plane 条目 ⇒ 满盘重启要 ~3960 条；
- 运行时每条 SB 占 2 个块，**盘越满、GC 越搬不动，列表就越长** ⇒ 写满 ~264 MiB
  （1024 条 × 2 块 × 132 KiB/块）后就溢出。

溢出后两条路径的失败形态都不可接受：重建路径**开机即 ASSERT**（debug 下 HardFault 开不了机），
运行时路径 `allocator_close_sb` **静默丢条目**——块的 BST 已置 CLOSED、`sb_id` 已写成越界索引，
却不在列表里 ⇒ 既不在 Free Pool、GC 也永不可见 = **永久漏账**，表现为可用容量无声流失、
写最终永久 `FTL_AGAIN`。

**修法**：容量取 **`FTL_BLOCK_TOTAL`**（每条条目至少占 1 个块 ⇒ 满在结构上不可能），
于是两处守卫都成了真不变量：`allocator_close_sb` 的静默丢弃改为 **ASSERT**（与
`allocator_add_closed_sb` 对称），`allocator_add_closed_sb` 的 ASSERT 保留并更新注释
（其返回值 `UINT16_MAX` 从此只表示参数非法）。

**代价**：`super_block_s` 12 B × 4096 = 48 KB（原 12 KB）。AXI_RAM 291.3 → 327.3 KB
（75.9% → 85.2%），`_Static_assert(sizeof(g_ftl) + FTL_AXI_RAM_RESERVED <= BOARD_AXI_RAM_SIZE)`
仍通过。**可选后续**：`super_block_s.topo` 的 `ch`/`ce` 是**只写不读**（`super_block_page_pba`
4 个调用点全部自带宽高、只读 `topo.lun`），去掉后可压到 8 B/条 ⇒ 只 +20 KB。

**收口**：三锚点全绿、零新告警；`.bin` 215036 → 215068B。⚠ **未上板**：判据 = 写满 >300 MiB
后 `ftl info` 的 `closed` 不再被封顶、`free` 不再无声下滑，断电重启仍能开机。

### v0.79 — 删除 L2P 哨兵层：未映射改由 Directory 条目 `NAND_PBA_INVALID` 表示

**日期**：2026-10-18

**动机（主流对齐）**：SPDK FTL（DRAM 型）把 L2P 表放 DRAM，"未映射/已 trim"是映射条目的
状态，不占闪存；DFTL 路线（DRAM-less）同理——未分配的翻译页不存在，查表命中"无效"即可
返回未映射。主流的共同点：**未映射不产生 NAND 读**。本仓原设计为每个 region 预登记一个
指向"永不写入的擦除页"（固定块 0..15，共 4 MiB），读该 region 时**真的发一次 NAND 读**，
再以读回全 `0xFF` 判定未映射。这是本仓自拟的表示法，不是主流形态。

**修法**：
1. `metadata_dir_fill_fixed()` 把全部 region 预登记为 `NAND_PBA_INVALID`（不再生成固定 PBA）；
2. `l2p_cache_load_start()` 查到 `NAND_PBA_INVALID` ⇒ entries 已是全 INVALID，直接置
   `CLEAN` / `fresh=1` 返回，**不提交读命令、不占 `load_inflight`**；
3. 删除固定分区 `L2P Table`（原块号 0..15）。固定元数据整体前移：Anchor 0..31、
   Checkpoint 32..39、BBT 40..41、Journal 42..45、Wear 46，`FTL_METADATA_BLOCK_END`
   **63 → 47**；释放的 16 个块号（4 MiB）归入用户数据/OP 池；
4. 删除 `l2p_region_pba()` / `ftl_l2p_region_placement()` / `L2P_BASE_BLOCK` /
   `L2P_BLOCKS` / `L2P_REGION_PER_BLOCK` / `FTL_L2P_TABLE_BYTES` / `FTL_SLOTS_PER_BLOCK`
   （再无消费者）。

**必须同步的两处 INVALID 判定**（编译期完全无声，写错会在开机/回填时把最后一个用户块
误当元数据块，或把 gblock 4095 记成活 region）：
- `metadata_dir_rebuild_live()`：先挡 `NAND_PBA_INVALID` 再解 PBA 字段；
- `ftl_is_metadata_block()`：动态块扫描跳过 INVALID 条目。

**介质格式**：块布局变了 ⇒ `FTL_MEDIA_LAYOUT_VERSION_BASE` **6 → 7**（DIM>1 为 8）。
⚠ **上板首启会自动全片格式化**，这是预期行为。

**代价/收益**：省 4 MiB（0.78% die）；每次首次访问未写 region（含首次写该 region 的
read-modify-write）省一次 NAND 读（tR ~25 µs）。L2P cache 仍为 64 region / 2048。

**验证状态**：⚠ **未上板**。已过 `make all`（零新告警）、`make release`（仅剩 2 条既有
unused-variable 告警）、`make tests UT=FTL` 仍因 v0.77~v0.78 重构遗留的测试符号
（`super_block_s.topo/valid`、旧块状态 API）编译失败，与本轮改动无关（UT 冻结）。
上板判据：首启自动格式化 → 冷启动 `free` 计数比旧布局多 16 块号对应量 → 顺序写/随机读
回归 → 读一个从未写过的 LBA 返回未映射且设备不产生额外 NAND 读（可对比改动前后同负载
的读耗时/FC 命令计数）。

### v0.80 — 元数据回收全面异步化：待回收位图 + job 拆分 + 观测收口

**日期**：2026-10-18

**动机**：v0.75 已把压缩/回收做成后台 job，但 `metadata_dir_append_slot` 换块时仍保留
一条**同步**全池 sweep（`metadata_dir_reclaim`）作为兜底——每次换块（每 64 次 region
刷盘）可能同步擦 0..N 个块，单次 tERASE ~3 ms，直接击穿 700 µs / 2000 µs 的服务预算。
同步 sweep 存在的原因是 `md_job_t.reclaim_target` 只有**一个槽**：同一轮多个块归零时
早的会被覆盖，只能靠 sweep 补。

**修法**：
1. `metadata_dir_cache_t` 新增 `reclaim_pending[FTL_BLOCK_TOTAL/32]` 位图（4096 bit =
   512 B，RAM-only）+ `reclaim_pending_count`；`md_job_t.reclaim_target` 删除。
2. 登记点：`commit_region` 旧位置归零、`append_slot` 换走零活旧块、换块时
   `md_reclaim_mark_all` 只登记不擦、压缩/擦除失败重试。登记**永不**碰 NAND。
3. `metadata_dir_kick` 的 IDLE 先看 `reclaim_pending_count`；RECLAIM 从位图取块、
   重查三条资格判据后提交异步擦除。**成功才清位**并归还 Free Pool；失败保留位，下轮重试。
4. 当前日志块已满且 `l2p_live == 0` 时，换块前先摘掉其"当前"身份并登记，否则
   Free Pool 空时会永远换不了块。
5. 拆分：job 状态机（约 650 行）移到 `src/ftl/metadata_job.c`；共用原语
   （`metadata_dir_slot_now` / `md_job_reset` / 位图操作）放
   `include/ftl/metadata_job.h`；`metadata.c` 只留 Directory/布局/Anchor/追加/登记。
6. 薄封装收口：删 `allocator_free_log_block`（纯转发 ⇒ 直接用 `allocator_free_block`）、
   `md_block_is_log`（纯表达式 ⇒ 两处内联）、`ftl_job_heartbeat_reset`
   （纯清零 ⇒ 调用点直接写）。
7. 观测：`[MD] job` 心跳 `LOGI → LOGD`（默认档静默）；CLI `info` 在 job 在跑或
   `pending != 0` 时追加一行 phase/moved/stall/pending/victim。

**介质格式**：**无变更**（位图是 RAM 态），`FTL_MEDIA_LAYOUT_VERSION` 不动，上板首启
不格式化。

**验证状态**：⚠ **未上板**。`make all` 零告警；`make release` 仅剩 2 条既有
unused-variable 告警；`make tests UT=FTL` 仍因既有测试符号失败，另新增 1 条
`md_job_t.reclaim_target` 不存在（UT 冻结，未改测试源）。
上板判据：冷启动不格式化；顺序写跨越多个日志块轮转时 `info` 的 `pending` 能归零、
`free` 不持续下滑；压缩中途 / 回收在飞时断电，重启后 Journal/Anchor 恢复且
`[FTL] dangling` 不出现；写路径不再出现 ~3 ms 同步擦除停顿。

### v0.81 — 固定元数据跨 plane 镜像（Anchor/Checkpoint/BBT）+ Journal multi-plane 擦除

**日期**：2026-10-18

**动机**：v0.80 后固定池仍只落 P0，P1 的同号块被 `ftl_is_metadata_block()` 排除却从未
写入（43 块 ≈ 5.375 MiB 闲置）。按"预留必用"对齐主流：要么把 P1 的块用起来，要么
不预留。选择**镜像**：关键元数据（Anchor/Checkpoint/BBT）用 multi-plane program 把
同一页写两份，P1 成为真实副本；Wear 是软状态，保持 P0 单副本，P1 的块 46 归还用户区。

**修法**：
1. `ftl_partition_t` 新增 `plane_mask`；Anchor/Checkpoint/BBT/Journal = `0x03`，
   Wear = `0x01`。`ftl_is_metadata_block()` 改为 `(plane & plane_mask)` + 块区间判断。
2. 新增 `ftl_async_write_page_mirror` / `ftl_sync_write_page_mirror`：
   `OP_PROGRAM_PAGE_MULTI_PLANE`，同一页按 CW 复制到两个 plane 的 desc；短页
   （BBT 540 B）用 0xFF 补到一页。**运行期只用 async**；sync 仅开机/格式化
   （`checkpoint_write_initial` / `bbt_save`，没有服务泵可等）。
3. 读回退：Anchor/Checkpoint/BBT 的 probe / locate / load 逐 plane 尝试；整条 record
   或整条快照从**同一个 plane** 读，某副本读失败/CRC 不符才换另一个 plane。
4. 换块擦除：镜像池（Anchor/CP/BBT）与 Journal 全部改
   `OP_ERASE_BLOCK_MULTI_PLANE`。这同时修掉 Journal 的**既有隐患**：
   `journal_page_addr` 按页号奇偶写 P0/P1，但旧擦除只擦 P0 ⇒ 回绕后对 P1 未擦页二次编程。
5. `metadata_ring_t.plane` → `plane_mask`；新增 `metadata_ring_addr_plane()`；
   `metadata_ring_prepare_write()`（仅开机/格式化）在镜像池上用 multi-plane 同步擦。
6. 分类修正：P1 的 0..41/46.. 现在是用户区，动态日志块可能落在
   `FTL_METADATA_BLOCK_END` 之前 ⇒ `metadata_dir_rebuild_live` / `commit_region` /
   `recovery_is_fixed_block` 从"块号 ≥ END"改为"非固定元数据块"（按分区表）。
7. `bbt_medium_has_ftl_data()` 只扫分区表实际使用的 plane。

**介质格式**：`FTL_MEDIA_LAYOUT_VERSION_BASE` **7 → 8**（块的写语义/副本布局变了）
⇒ **上板首启自动全片格式化**。

**验证状态**：⚠ **未上板**。`make all` 零告警；`make release` 仅剩 2 条既有
unused-variable 告警；`make tests UT=FTL` 仍因既有测试符号 + `reclaim_target` 引用失败
（UT 冻结，未改测试源）。
上板判据：首启自动格式化；`info` 显示 `cp_seq`/`free` 正常；顺序写/随机读回归；**故意
损坏 P0 副本后仍能从 P1 读回 Anchor/CP/BBT**（镜像生效的唯一证据）；Journal 回绕多圈
无 `PROGRAM_FAIL`；掉电恢复与 `[FTL] dangling` 不出现。

### v0.82 — 磨损计数并入 Checkpoint：删除独立 Wear 块

**日期**：2026-10-18

**动机**：独立 Wear 块（块 46，P0 单副本 + 页式追加日志）是一块**固定单副本热点**：
它自身的擦除计数不被 WL 管理，也不随 Anchor/CP/BBT 镜像。按主流"per-block 元数据随
块账/checkpoint 一起落盘"的思路，把 `erase_count / read_count / last_ecc /
rising_count / health` 并入 Checkpoint 记录：与块账**原子提交**，随 CP 池轮转，
不再占独立块。

**修法**：
1. Checkpoint 记录从 5 页扩到 **21 页**：Header 1 + 块账 4 + **磨损快照 16**
   （每块 8 B：erase 4 / read 2 / last_ecc 1 / flags 1；256 条/页）。
   每块 record 数 12 → **3**；池 8 块 ⇒ 24 条。
2. Header 新增 `wear_entry_count / wear_entry_bytes / wear_page_count /
   wear_page_crc[16]`；`record_pages = 1 + 块账页 + 磨损页`。
3. CP job 新增 `CP_WEAR_WRITE` 阶段：在块账页之后、Header 之前逐页从 BST 组页写入；
   每页 CRC 存 Header；Header 仍是 commit 页。运行期全 async。
4. 开机 `checkpoint_load_wear_table`：逐页从 P0/P1 任一副本读、校验 per-page CRC、
   回填 BST；失败只当"计数不可用"（软状态），不触发格式化。
5. **删除** `include/ftl/wear.h` / `src/ftl/wear.c`、`wear_state_t`、`FTL_OP_WEAR`、
   `ftl_op_wear`、后台 Wear 任务（任务表 14 → 13，掩码 IDLE 0x3FC0→0x1FC0、
   NORMAL 0x1FC0→0x0FC0）、`wear_mark_dirty` 调用点。
6. 块布局：`FTL_METADATA_BLOCK_END` 47 → **46**；块 46 起在两个 plane 都是用户数据。
   `g_ftl_partitions[]` 删 Wear 项。

**介质格式**：`FTL_MEDIA_LAYOUT_VERSION_BASE` **8 → 9** ⇒ **上板首启自动全片格式化**。

**代价/收益**：CP 每条 record 的写页数 5 → 21（CP 仍由水位/定时触发，非每写必跑）；
磨损计数不再有固定热点，且与块账原子。软状态语义不变：两次 CP 之间掉电只丢一个 CP
窗口的计数。

**验证状态**：⚠ **未上板**。`make all` 零告警；`make release` 仅剩 2 条既有
unused-variable 告警；`make tests UT=FTL` 仍因既有测试符号 + `ring.plane` /
`reclaim_target` 引用失败（UT 冻结，未改测试源）。
上板判据：首启自动格式化；`checkpoint_load_wear_table` 成功回填（`[WEAR]` 无告警）；
顺序写/GC 后 `erase_count` 非零且跨重启保持；损坏磨损页 CRC 后计数从 0 且不触发格式化；
CP 记录 21 页读写与回绕正常。

### v0.83 — USB 数据面暴露的三处 FTL 修复（wbuf plane 镜像 / 回收前 Anchor / L2P 降级可见性）

**日期**：2026-10-20

**动机**：把板子接到 Windows 当 U 盘用，暴露了三类此前被“均匀 pattern / 旧测试口径”掩盖的问题：

1. **host 写数据损坏**：4KB 写入的后 2KB 重复前 2KB，Windows 格式化报 “track 0 bad”、
   大文件重插后 SHA256 不一致；
2. **回收旧日志块不等 Anchor 落盘**：掉电后 on-media Anchor 指向已擦块，重启出现
   `[L2P] region 0 load failed`；
3. **L2P 加载降级被误报成“合法未映射”**：boot 把“读失败降级”报成 `restored`，不触发
   full metadata rebuild。

**修法**：

1. **wbuf / pair 镜像**（`RISK-WB-06`）：`wbuf_program_page` 的源是 plane-major pair 镜像
   （`slot = plane*CW_NUM + cw`），却调用单页语义的 `ftl_cmd_write_data_cw`（只按
   `cw*512` 取数）⇒ plane1 复制 plane0。新增 `ftl_cmd_write_data_pair_cw`（同一 core，
   `pair_major = true`），wbuf 改用它；`ftl_cmd_write_data_cw` 保持单页语义给
   mirror / GC / WL。
2. **Metadata 回收前先落 Anchor**（`RISK-MD-06`）：
   - `MD_JOB_RECLAIM` 擦前要求 `dir->dirty == false`；脏则先走 `CP_ANCHOR_*`
     写 Anchor（`anchor_for_reclaim`），成功后才回 RECLAIM；
   - `dirty_gen / anchor_gen`：Anchor 8 页异步写期间若有新 commit，commit 不清 dirty，
     下一槽重写整条；
   - Anchor 成功且未过期后才 `md_reclaim_mark_all()`。
3. **L2P 降级可见性**（`RISK-L2P-04`）：`l2p_cache_line_s` 增 `degraded`（与
   `fresh / load_fail_count` 共用对齐填充，行尺寸不变）；新增
   `l2p_cache_region_degraded()`；boot probe 与 journal replay 检测到降级时分别计入
   `l2p_read_errors` / 置 `block_ctx->ok=0`，回退 full metadata rebuild。
4. **BAD 块后的减计数守卫**（`RISK-MIG-03`）：`bbt_mark_bad()` 清 `valid_count` 后，
   L2P 仍可能指向该块（predictive 读失败标坏 / 重建漏认）。写确认与 TRIM 直接
   `ftl_block_sub_valid()` 会在 debug 撞 `valid_count > 0U` 断言、release 回绕成
   65535。新增 `ftl_block_sub_valid_if_live()`（BAD 或 count==0 跳过），写确认 /
   TRIM / migrate 源块统一使用。
5. **主机容量修正**（`RISK-CAP-01`）：`FTL_LBA_MAX` 原取 `FTL_IU_TOTAL`（全部物理 CW）
   = 512 MiB，未扣固定元数据（92 块）、OP（408 块）、Spare（100 块），超卖 75 MiB。
   改为 `FTL_HOST_DATA_BLOCKS_PER_INSTANCE = 总块数 − (元数据 + OP + Spare) × PLANE_NUM` = 3496 块，
   `FTL_LBA_MAX` = **894,976（437 MiB）**；`FTL_USER_CAPACITY_BYTES` 更名
   `FTL_MAPPING_CAPACITY_BYTES`，新增 `FTL_HOST_CAPACITY_BYTES`。L2P 表/Anchor/
   Directory 布局不变；旧介质的越界映射需一次 CLI `format` 清掉。
6. **full metadata rebuild 快速判空**（`RISK-BOOT-14`）：`ftl_recovery_scan_write_pointer`
   原来对每个块逐页扫满 64 页，空闲块也要 64 次读（4096×64 ≈ 26 万次）。
   改为 **page 0 的 4 个 CW META 全 FF ⇒ 整块空白**（1 次读），写过的块才从 page 1
   继续找写指针并检测 hole。假设写模型顺序追加且 page 0 失败即关闭 SB；若开放续写
   需恢复全块扫描或补读 page 63。

**介质格式**：无变化（`FTL_MEDIA_LAYOUT_VERSION` 仍为 9）；新增字段均为 RAM 态。

**验证状态**：
- ✅ `make all` / `make release` / `make tests UT=FC` 零新告警；
- ✅ **`RISK-WB-06` 已上板验证**（rev27）：Windows FAT 格式化成功；`notepad.exe`
  写入 → `Write-VolumeCache` → 拔插 USB → SHA256 一致；16KB 非均匀 pattern 的 2KB
  块 0..7 内容分别为 0..7；
- ⚠ `RISK-MD-06` / `RISK-L2P-04` / `RISK-MIG-03` **未做专项板端复现/注入**，保持
  `known_open_items` 的“已修未上板”口径；
- ✅ `RISK-CAP-01` 容量参数已改（512 → 437 MiB）并 **rev30 上板确认**：启动日志
  `lba_max=894976 (437MiB)`；Windows `Get-Disk = 458,227,712 B`；FAT 格式化；
  `usb_verify write/check` 拔插后 `RESULT=PASS`；旧介质已由 CLI `format` 清理；
- ⚠ `RISK-BOOT-14` rebuild 快速判空已改，**待上板**用 clean/PF/fresh + 部分写块验证
  `partial`/`valid_cw` 与优化前一致（不能只测“启动变快”）；
- ⚠ 同期观察到 predictive 迁移活锁 `RISK-MIG-02`（`kind=2 victim read failed` 反复重选），
  **本轮未修**，单列；rev27 上板还触发了它导致的二级崩溃（见 `RISK-MIG-03`）。

**回归判据（写进 `docs/design/usb_msc_verification.md`）**：
- 不能用 CLI `wr/rd` 的固定 pattern 验证 pair 写（每 2KB 相同，缺陷不可见）；
- 必须用每 2KB 互不相同的数据 + 拔插后读回；
- 回收/Anchor 专项需要填满 64 页元数据日志块后冷启动，不能用“写两次 LBA 0”代替。

---

### v0.84 — L2P 日志区改为固定预留（方案 A，RISK-MD-07 / RISK-CAP-02）

**日期**：2026-10-20

**起因**：rev32 H2testw 写 343 MiB（约 79%）时报 Code 34。账本显示 L2P 日志块碎片到
1076 块（134 MiB），叠加数据块后超过 4096；压缩只在 `free≤32` 时触发且一次只搬一个
victim，运行期大部分时间不压缩。

**改动**：
- `include/ftl/metadata.h`：新增 `L2P_LOG_BLOCK0` / `L2P_LOG_BLOCKS_PER_PLANE` /
  `FTL_METADATA_AREA_END` / `L2P_LOG_COMPACT_TRIGGER`；分区表加 `L2P log [46,78)`；
  **介质版本 9 → 10**。
- 容量：`FTL_HOST_DATA_BLOCKS_PER_INSTANCE` 改扣 `FTL_METADATA_AREA_END` ⇒ 数据块
  **3432 = 429 MiB**；`FTL_LBA_MAX = 878,592`；`FTL_HOST_CAPACITY_BYTES = 449,839,104`。
- `src/ftl/metadata.c`：`metadata_log_alloc` / `metadata_log_release` /
  `metadata_log_area_restore` / `metadata_dir_live_block_count`；换块只从固定区取；
  压缩触发改为 `live_log_blocks > 40`（理想 32 + 25%）。
- `src/ftl/metadata_job.c`：`md_note_block_erased` 归还固定区（不回 Free Pool）；
  `md_compact_next` 一次装满新块、连续搬多个稀疏 victim。
- `ftl_fc_interface`：`ftl_page_is_blank_raw` 收敛为唯一实现（PFR + metadata 共用）。
- CLI `info` 增加 `live=`（活日志块数）。
- 删除无调用者的 `allocator_alloc_log_block`。

**依据**：SPDK FTL 把元数据放在独立区/独立介质（nvcache + 每 band 尾部 P2L），spare 在
device formation 时从 surfaced capacity 扣除（<https://spdk.io/doc/ftl.html>；索引见
`docs/reference/README.md` §A4）。

**状态**：编译零告警；**未上板**。介质版本 9→10 ⇒ 首启自动格式化是预期（用户已同意）。

---

### v0.85 — 返回值状态码统一为非负（H46）

**日期**：2026-10-20

**起因**：用户要求「本仓所有返回值状态位改为无符号值，不用负值」。

**改动**：
- `ftl_status_t`：0/1/2/3（原 0/-1/-2/-3）。
- `ssd_status_t`：0..4（原 0/-1..-4）。
- `fc_status_t`：0..7（原 0/-1..-7）。
- `boot_policy_status_t`：0..6。
- 5 个步进枚举只把原负值成员改为下一个未用正数，**保持已有 0 语义不重排**：
  `MD_COMPACT_FAILED=3`、`MD_ANCHOR_FAILED=2`、`CP_FLUSH_FAILED=2`、
  `JRN_TRUNC_NO_TRUNCATE=2`、`RR_PLANE_WAIT=2`。
- `ssd_controller_init/submit`、`ssd_request_split`、`ssd_ftl_boot` 的返回类型由
  `int` 改为对应状态枚举；`ssd_platform_register` 的 `-1` 改为 `FUNC_STATUS_ERROR`。
- 第二轮（用户要求「非第三方代码同一风格」）：
  - `uart_log_write` / `log_write` / `log_vprintf` / `log_printf` → `uint32_t`，
    错误用 `UART_LOG_INVALID` / `LOG_WRITE_INVALID`（`UINT32_MAX`）显式哨兵；
  - `uart_log_flush` / `log_flush` / `usart_port_tx_dma_start` → `bool`；
  - `fc_bch_decode` → `bool` + `*out_nerr`（原「错误数 or -1」）；BCH 内部
    `compute_error_locator_polynomial` / `build_gf_tables` / `build_deg2_base` 同步；
  - CLI 数值解析 → `bool`；tokenizer → `uint32_t` + `UART_CLI_ARGC_INVALID`；
    `uart_cli_log` 的 `-1` → `FUNC_STATUS_ERROR`；
  - `strStr` / `canCompleteCircuit` → `uint32_t` + `UINT32_MAX`。
  - 明确例外：newlib `_write()`（libc ABI 要求 int/-1）、比较器（排序契约）、
    BCH `deg(0)` 与局部下标算术、`third_party/` / CMSIS/HAL / munit /
    `src/platform/stm32h7r3/`（未构建的备用平台）。
- 全仓无 `status < 0` 类判据（已核对）。

**验证**：`make all` / `make release` / `make tests UT=FC` 零新告警；debug DWARF 已核对
`FTL_ERROR=1 / FTL_AGAIN=2 / FTL_TIMEOUT=3`；UT=FTL 仍因冻结测试源编不过（RISK-MD-03）。

---

### v0.86 — 元数据分区改为 BBT + spare 候选池；容量/OP/写重试收口

**日期**：2026-10-21

**起因**：用户要求「按主流做」：固定元数据分区不能再用编译期块号硬碰 BAD；
容量要反映真正可写容量；OP 要守卫运行期坏块；数据写失败要透明重试。

**判据**：本仓 `docs/design/ftl_design.md §3.3.8` 已写明主流 = **BBT + 预留替换块
（sparing），不是 block remap 表**（ONFI multi-plane 每 plane 独立行地址）。因此不引入
持久化 logical→physical 重映射表。

**改动**：
- 布局 v11：`78..81/plane` 为 shared metadata spare；`FTL_METADATA_AREA_END` 78 → 82；
  现有 0..77 块号不动（v11 首启仍能读回 v10 BBT，282 data BAD 不丢）。
- `md_ring_s` 增加候选块表 `blocks[]`；Anchor/CP/BBT/Journal 的地址漏斗改查候选表。
  候选 = 名义区间内 plane_mask 所有 plane 非 BAD 的块；不足硬下界时从 shared spare 补
  （分配顺序 BBT → CP → Journal；Anchor/L2P 日志只跳过 BAD）。
- BBT 自举：直接物理扫描「名义块 + 全部 spare」，record 自带 magic/sequence。
- 格式化：保留当前 BBT 候选集、verify 跳过它；verify 后用最终 BAD 集重建候选池并
  在新池上重新定位 BBT 游标再落盘。
- 运行期元数据 BAD：`bbt_mark_bad` 只置 `pools->dirty`（不弹用户 Spare）；`md_pool_service`
  在元数据 job 空闲时重建候选池、重定位游标；Journal 候选表变化时用
  `jrn_scan_latest + jrn_resume` 接回实际写点。Journal/L2P 日志失块置 BBT
  `REBUILD_REQUIRED`，下次开机强制 full rebuild，新 CP 落盘后清除。
- 数据 BAD：Spare 为第一道替换，Spare 空时从 OP 取块（`alc_refill_free_from_op`）。
- 容量：按 `min(plane)`、`min(instance)` 计好块并扣 OP/Spare；新增 `[CAP]` 日志。
- 写失败：从失败命令的 desc 链读回本页数据，在新块上重建命令重发一次
  （`FTL_WRITE_RETRY_MAX=1`）；组保持 pending，host 看不到第一次失败。
- `format --verify` 改为**从头识别**：清空非 BBT 池的旧 BAD 集，测试所有非 BBT 池块
  （含原先 BAD 的块），打印 `old_bad/recovered/new_bad`；BBT 池保留本轮 record。
  `bbt_save` 的 sequence 改为只取 max，避免候选池缩小后被移出池的旧 record 在
  下次开机 bootstrap 扫描中胜出。
- 诊断：CLI `pool` 打印候选池；debug `bbt fault <plane> <block>` 注入元数据 BAD 做负例。
- 日志/观测（`docs/design/logging_design.md`）：`[STAT]` 5 s 摘要；dangling 按旧 gblock
  聚合；RR 已退役块每 gblock 一次、好块首故障全字段 + `st=` 解码；CP done/GC trig 降
  DEBUG；MIG 单块失败降 WARN；64 条写轨迹飞行记录器（首次 UNC/program-fail dump）；
  GC 系统性读失败熔断（连续 8 个 victim）；NAND timing mode 可通过
  `nand_get_timing_mode()` 观测。

**验证**：`make all` / `make release` 零新告警（release 仅有 gc.c 既有 4 条
`best_*` 未用告警）；`make tests UT=FTL` 仍因冻结测试源编不过（461 error，R4）。
**未上板**；上板判据见 `docs/design/known_open_items.md` RISK-BBT-12/13/14。

---

### v0.87 — 擦除页识别 + GC 按写指针截断 + 系统性读失败熔断修复

**日期**：2026-10-21

**起因**：rev33 板端 279 MB 分区测试复现 `RISK-MIG-07`/`RISK-GC-10`：Free Pool 触底后
GC 在 250 s 内把 **264 个不同 gblock** 标 BAD（`bad` 156→420），`dng=55`、
`op=204/204→11/0`、`spare=50/50→0/29`，写入冻结。`rdpaa 0x2DF00` 拿到直接证据：
gblock 2415（plane1/blk367）page0 **原始读全 FF、normal 读 FAIL(UNC)**，与日志首条
`[MIG] abort ... plane=1` → `gblk=2415 mark BAD` 逐字对应；同块 page63 正常 ⇒ 半写块的
空洞页。

**根因（两项）**：
1. **FC 没有擦除页识别**：全 0xFF 不是合法 BCH 码字，解码返回 UNC；`migrate.c` 却假设
   「空页 = 无错误 + META 全 FF」⇒ 空洞页永远走不到空页分支。
2. **熔断计数结构性不可达**：`mig_start` 的 `memset(m, 0, ...)` 不恢复
   `read_fail_streak`（单 job 最多 +2）；且 `mig_complete_erase` 在单个 plane 擦成功时
   就清零。两个都让「连续 8 个 victim」永远达不到。

**改动**：
- FC：新增 `FC_CW_STATUS_BLANK`（bit 8，**不属于** `FC_CW_STATUS_ERROR_MASK`）；Pass 1
  解码失败后判整条 CW 全 0xFF ⇒ 置 BLANK；Pass 2/3/4 对 BLANK 跳过解扰/CRC/META 自洽；
  已知期望 LBA 的 host 读仍报 `MATA_ERROR`（映射指向未编程页 = 真实损坏）。
  `desc->status` / `fc_cmd_status_s.status` 位宽 8 → 9（RAM 内位域，尺寸不变）。
- FTL GC：`mig_step_read` 扫描区间截断到 `victim.next_page`；WL/predict 的 victim 构造
  一并带 `next_page`（`wl_victim_s.next_page` 新增）；重建条目仍记满 64，由 BLANK 兜底。
- 熔断：`mig_start` 保存/恢复 `read_fail_streak` 与 `paused`；删除 `mig_complete_erase`
  的逐 plane 清零；只在 `mig_finish` 的「整块迁移成功」分支清零。
- **第二轮（同日上板暴露）**：`format --verify` 恢复 860 个误标块后，Windows 初始化 U 盘
  立即出现 `[RR] unc lba=4 ... st=MATA|READY|BLANK` 并连锁退役 9 个好块。`rdpaa` 证明
  这些页数据与 META 都正确 ⇒ 根因是**稀疏页的空兄弟 CW**：单扇区写只编程 1 个 CW，整页读
  对其余空 CW 做 META 自洽校验 → `MATA_ERROR` → 整条命令判失败。修法：Pass 4 对
  `BLANK` **一律**跳过 META 校验（不再区分 `command->meta` 是否已知），见 `RISK-GC-12`。

**验证**：`make all` / `make release` 零新告警；`make tests UT=FTL` 仍因冻结测试源
编不过（R4，错误全在 `tests/unit/test_ftl.c` / `ftl_test_common.h`）。**未上板**。
上板判据见 `docs/design/known_open_items.md` RISK-GC-10 / RISK-GC-11。

**介质布局**：无变化（未抬 `FTL_MEDIA_LAYOUT_VERSION`）。

---

### v0.88 — 开卡体检判据改为「擦除成功即好块」

**日期**：2026-10-22

**起因**：用户要求简化开卡流程——不再用「写图样 → 读回比对 → 擦」判定好块，
擦除命令成功即可。

**判据**：open-card 需要的是**不依赖数据通路的稳定判据**。写/读体检只覆盖
**单 plane 顺序写**，测不到运行期的 pair 写路径（`RISK-NAND-06` 已证明它「通过」
不能证明运行期无问题），收益与耗时不成比例；而擦除失败是开卡阶段能稳定观测的物理判据。

**改动**：`ftl_format_verify_blocks` 去掉逐页写/读/比对，只对每个非 BBT 池块发一次
擦除命令：
- 擦除成功 ⇒ 判为好块（`ftl_format_reset_bad_map` 已先清空非 BBT 池的旧 BAD 集，
  所以原先 BAD 的块只要擦得动就 recovered）；
- 擦除失败 ⇒ `bbt_mark_bad` 并落盘。
编程 / 读失败由运行期兜底：host 写失败 → `ftl_write_retry` 换块重发一次 → 仍失败则
`ftl_host_io_write_fail` 标坏；读失败 → RR 标坏。
日志文案与 CLI/makefile 帮助同步改为 erase check。

**附带修复（CLI 测试坑）**：`rd` 把数据读进 `g_wr_buf`，而 `wr` 的写源也是它；pattern 只在
`cli_init` 预填一次 ⇒ `rd → wr` 相邻时 `wr` 会写回上次读到的数据。`uart_cli_wr()` 现在每次
先 `uart_cli_pattern_init()`，耗时 ~75 µs（相对一次 4 KB 写的 ~15 ms 可忽略），且不影响
USB/H2testw 路径。

**验证**：`make all` 零新告警（315,352 B）；上板（2026-10-22 00:13）：`format --verify`
erase-check 约 10 s、`old_bad=9 recovered=9 new_bad=0`；`wr 4 1`/`rd 4 1 c` PASS、
`wr 0 8`/`rd 0 8 c` PASS，`bad=0`。**全盘 H2testw 未测**。风险见 `RISK-BBT-16`。

**介质布局**：无变化。

---

### v0.89 — GC 空间回收加收益判据（满盘只读不再空转）

**日期**：2026-10-22

**起因**：全盘 H2testw 写满 425 MiB 后 `free=10`（< 水位 32），每个 victim 512 CW 中
693–504（96–98%）有效。GC 只看水位、选块无收益下限 ⇒ 只读校验期间仍每 1.8–1.9 s 搬一个
98% 有效的块，净回收 ~2%，`free` 卡在 10–12；NAND 带宽被占，校验从 ~297 KB/s 降到
~192 KB/s（`RISK-GC-13`）。

**改动**：
- `gc_select_victim` 改为 `gc_select_e gc_select_victim(alloc, mode, out)`：在 cost 评分
  **之前**按收益判据逐候选过滤，返回 `OK / NO_CANDIDATE / NOT_WORTHWHILE`，并输出
  `best_valid_pct / scanned / eligible` 供日志归因。
- 三个模式：`OFF`（host 在写 / T5 磨损）、`IDLE`（host 不写且 `free > 8`：valid ≤ 80%）、
  `EMERGENCY`（`free ≤ 8`：可回收 ≥ 1%）。80% / 1% / 500 ms / 2 s 均为本仓自拟。
- `mig_handle_s` 增加 host-writing 2 s 窗口、`gc_no_gain` 结论 + 快照、`nogain_count`、
  `gc_last_mode`；`mig_start` 的 memset 后显式恢复这些字段。
- 无收益结论按 `GC_NO_GAIN_BACKOFF_MS`（500 ms）退避；新建写、进入紧急水位、时钟缺失时
  立即重扫。紧急水位下 `mig_abort("no free block")` 同样置 no-gain，避免
  「选中 → 目的分配失败 → abort」死循环。
- 写路径不在 `wbuf` 里塞新的错误路径（`wb_reset` 会误结算其它组）；改为依赖既有
  `ftl_drain_group` 失速预算：GC 停止后连续无进展 ⇒ `SSD_STATUS_IO_ERROR`，并打
  `[FTL] no space: GC no-gain` 归因行。
- 观测：`[GC]` 日志加 `nogain/mode/hw/bestvr`；CLI `diag` 加 `gcm/ng`；
  `mig_status_t` 加 `nogain/gc_mode/gc_no_gain`。

**验证**：`make all` 零新告警（317,216 B）；`make release` 仅 `gc.c` 既有 4 条
`best_*` 告警（179,800 B）；`make tests UT=FTL` 仍因冻结测试源编不过（R2/R4）。
**未上板**。风险见 `RISK-GC-13` / `RISK-GC-14`。

**介质布局**：无变化（未抬 `FTL_MEDIA_LAYOUT_VERSION`）。

---

### v0.90 — L2P region 主备副本 + 范围化恢复（单坏页不再全盘重建）

**日期**：2026-10-22

**起因**：开机 `[L2P] region 1200 load failed 3 times — degrade to unmapped` ⇒ Journal 回放
命中降级基线 ⇒ 全局 `block_ctx.ok=0` ⇒ 整份块元数据快照被丢弃 ⇒ full metadata rebuild，
实测 **~15 分钟**（`boot type=2 ... bit=fallback`）。快照本身完好（`plane=0 crc=ok`），
问题在于「快照 + Journal 增量」的组合被判全局不可信（`RISK-BOOT-18`）。

**判据**：主流分层恢复 = 读重试 → 元数据冗余 → 范围化降级/局部重建 → 全量重建。
本仓器件无 read-retry（0x89 保留）、RAID 关闭，因此补 **L2 副本 + L3 范围化**。

**改动**：
- **L3 范围化**：`jrn_replay` 对降级 region 不再清 `block_ctx.ok`；probe 的降级 CW 单独
  计入 `l2p_degraded_cw`，只有真正读失败才回退全量重建；启动结果新增 `bit=scoped`。
  host 读丢失映射返回 `IO_ERROR`（`l2p_cache_s.region_degraded_bitmap` 让行驱逐后仍识别；
  `l2p_load_start` 命中降级位图直接合成全 INVALID 行，不再反复读坏页）。
- **L2 副本**：每个 region 写主/备两份副本——`md_append_slot_pair` 一次预留两槽，
  `l2p_flush_kick` 串行写（主成功后写备），两份都成功才 `md_commit_region_pair`；
  读失败自动回退备份并交换主备（`md_commit_region_pair` 原子交换）。
- **Directory/Anchor**：`md_entry_s` 仍是 7 B 主 PBA，新增平行 `backup_pba[2048]`（8 KiB）；
  Anchor 记录 8→16 页（主 8 + 备 8），**主半必须完整同 sequence 才算有效**、备份半
  best-effort；`md_rebuild_live` 两份都计入 `l2p_live`。
- **压缩**：副本感知——victim 上可能只有主/只有备/两份都有，逐副本搬移，两份都离开前
  不推进 region 游标；预留槽位 1→2。
- **布局 v12**：L2P 日志区 32→48 块/plane（96 块 = 12 MiB），`FTL_METADATA_AREA_END`
  82→98，用户块 3424→**3392**（428→**424 MiB**，`lba_max=868,352`）；介质版本 11→12，
  **首启自动格式化**。

**验证**：`make all` 零新告警（319,656 B）；`make release` 181,276 B；`make tests UT=FTL`
仍因冻结测试源编不过（466 处，全在 `tests/unit/*`）；`UT=FC` 的失败是既有
`fc_status_t` 重命名遗留（测试源）。**未上板**。风险见 `RISK-BOOT-18/19/20`、
`RISK-MD-14/15`、`RISK-CAP-04`。

**介质布局**：**v12**（见下表）。

---

### v0.91 — 静态 WL 按 UBI 模型重写（idle 无限搬迁修复）

**日期**：2026-10-22

**起因**：板端 host 空闲时每 2.4 s 出现 `[MIG] done kind=wl vcw=512 post=free`，
`free`/`wr` 不变 ⇒ 纯内部拷贝（~107 KB/s、~9 GiB/天）。根因见 `RISK-WL-08`。

**判据**：Linux MTD UBI 的静态 WL（`drivers/mtd/ubi/wl.c` + `Kconfig`
`MTD_UBI_WL_THRESHOLD`）：只有当 **Free Pool 中的高擦除块**比**最冷的 used SB**
高出阈值时，才把冷数据搬过去；每次搬一个单位；方向是低擦除 → 高擦除。
（dhara 的完美 WL 是小容量 raw flash 分支，本仓不采用。）

**改动**：
- `wl_should_trigger`：三条件（max-min / max-avg / 低区占比）→ 单一 UBI 判据
  `max_free_ec − min_used_ec ≥ WL_THRESHOLD(200)`；`min_used_ec` 只统计有效 CW≥70%、
  无坏 plane 的 Closed SB（与选块同门限）。
- `wl_select_victim`：去掉"低擦除区"过滤，选平均擦除次数最小的 SB。
- 目标方向：新增 `alc_alloc_pair_high_wear`（每 plane 取擦除最多的空闲块、无 OP 兜底）
  与 `alc_prepare_wl_destination`（启动前摘掉 cold SB）；`mig_handle.dst_high_wear`
  只在 `mig_begin_wl` 置位。
- 删除 `WL_DELTA_THRESHOLD` / `WL_MAX_AVG_RATIO` / `WL_LOW_ZONE_WIDTH` /
  `WL_LOW_BLOCK_PCT` / `wl_get_min_erase` / `alc_count_blocks_below`（零消费者）。
- 顺带修 release 档 `gc.c` 的 4 处 `set but not used`（LOGD 被裁空，H30 显式丢弃）。

**验证**：`make all` / `make release` 零新告警；**未上板**；无介质格式变化
（不需要重新开卡）。上板判据见 `RISK-WL-08`。

---

### v0.92 — 写确认 Journal 容量改为非阻塞预留（confirm 内不再同步刷页）

**日期**：2026-10-22

**起因**：`ftl_confirm_write` 的「两遍之间无泵」假设对停泊确认不成立。正常完成路径在
`ftl_op_dispatch` 内，`jrn_append` 页满时不会同步刷页；但 `pend_confirm_service` 在主循环
（dispatch 深度 0）调 confirm，`jrn_append` 页满且 `jrn_freeze` 失败时会走
`jrn_flush_page` → `ftl_sync_write_page` → `ct_drain`，**重入 host 完成回调**。后果：
批量化在 Journal 循环前一次读完的 `oldp[]` 可能变陈旧（Journal old_pba 链断、
valid_count 重复扣减）；重入的 `l2p_is_ready`/`l2p_load_start` 还可能把本行 kick 成
FLUSHING，使 `l2p_write_entries` 返 `FTL_AGAIN`，release 下静默跳过 L2P 更新
（`RISK-HOST-07`）。

**判据**：主流存储主控的 completion/service handler 不阻塞、不重入完成派发；资源不足
用背压（不 post completion / 返回可重试状态），Journal 页由后台 job 异步落盘。

**改动**：
- 新增 `jrn_reserve(journal, entries)`：非阻塞预留当前页槽位；不足时只调 `jrn_freeze`
  起异步页写并返 false，**不**调 `jrn_flush_page` / `ftl_sync_submit_wait` / `ct_drain`。
  预留按最坏情况（mask 置位数）并留 1 个空槽 ⇒ 追加结束后不会触发 `jrn_append` 页满分支。
- `ftl_confirm_write`：在 ready 之后、读 `oldp`/追加之前调用 `jrn_reserve`；失败直接返
  `FTL_AGAIN` 停泊，由 `jrn_handle` 后续轮推进。`ring_full` 时不再静默成功——停泊超时
  按 IO_ERROR 结算。
- `jrn_append` **移除所有上下文中的同步回退**：页满且 `jrn_freeze` 起不来时只丢弃后续
  条目 + LOGE，整页由 `jrn_handle` 后续轮刷；需要不丢条目的调用者先 `jrn_reserve`。
- GC/WL 的 `MIG_WRITE_SETTLE` 在改 L2P/Journal 前 `jrn_reserve(popcount(cw_bitmap))`；
  不足时只起异步刷页并 `mig_wait` 挂起，不再在 migrate 里同步刷 Journal。
  ⚠ `wait_ms` 只在 L2P 与 Journal 容量都就绪后清零（原先每轮 L2P ready 都清零，
  使 journal capacity timeout 永不触发）；GC/WL 追加后补 `jrn_update_watermark`，
  否则大量搬移只靠 T2/T3 触发 CP，可能撞 ring_full。
- TRIM 改为异步 job：`ftl_trim_begin/step`（step 每次最多 8 条）；L2P region 未就绪或
  Journal 容量不足时返 `FTL_AGAIN`，游标不动；Controller 的 TRIM 请求改为 submit 登记 +
  `ssd_controller_service` 每轮按 run 推进，完成走 `complete_flag`。不再调用
  `l2p_read_sync`/`l2p_write_sync`/`jrn_flush_page`。
- `jrn_flush_page` 及其专用 helper（`jrn_flush_now` / `jrn_drain` /
  `jrn_erase_pending_sync`）与零调用者的 `l2p_flush_one_sync` 一并删除：Journal 模块
  不再提供同步刷页出口。TRIM 增加 1 s 无进展超时，Journal ring_full / L2P 加载卡死时
  host 请求按 IO_ERROR 收尾，不会永久 BUSY。
- `l2p_read_entries` 的 miss 分支与逐条 `l2p_read` 对齐（记 miss + 起异步加载），
  避免未来调用点永久重试（`RISK-L2P-06`）。
- 删除批量 L2P API 里重复的 NULL/页对齐/同 region 运行期断言（H2/C7）；同 region
  改为编译期 `_Static_assert`。

**验证**：`make all` / `make release` 零新告警；`make tests UT=FTL` 仍因冻结测试源
编不过（475 处，全在 `tests/unit/*`）。**TRIM 基本路径已上板**（banner 18:43:34）：
`wr 100 1` → `n=1 status=0`，`tr 100` → `status=0`，`map 100` 写后新 PBA / 擦后
INVALID；断电回放（`boot type=2`）后仍 INVALID。写确认/GC 的 Journal 背压与全盘
H2testw 仍待测。风险见 `RISK-HOST-07/08/09/10`、`RISK-L2P-06`。

**介质布局**：无变化（未抬 `FTL_MEDIA_LAYOUT_VERSION`）。

---

### v0.93 — L2P 顺序预取 + FTL two-plane cache read（未上板）

**日期**：2026-10-23

**内容**：

- **L2P region 顺序预取**：每实例记录连续 host 读请求；连续 2 个请求且距 region
  末尾 64 LBA 内时，预取 `last_region + FTL_DIM_TOTAL` 的下一个 region。
  - 新增 `l2p_prefetch_region()`；复用 `l2p_load_begin()`；
  - 预取只驱逐 INVALID/CLEAN，遇 DIRTY 跳过、不 kick 写回；
  - `thrashing` / `flush_active` / `trim_active` / `bg_suspend` / 资源不足 /
    在飞 load 达 2 时停；
  - 不计 miss/hit；预取行首次被 host 命中时计 hit（`prefetch` 行标志）。
- **FTL cache read 批次**：一次 host 读请求内，最多 4 个物理连续 pair 用
  `START_SEQ_MP + SEQ_MP×(N-2) + LAST_MP` 提交；任一条件不满足回退普通读。
  - 新增 `include/ftl/cache_read.h` / `src/ftl/cache_read.c`；
  - `PENDING_OP_READ_CACHE`：失败不启动 RR（避免向同 LUN 插命令破坏 sequence）；
  - 不使用 FC chain，依赖当前 FTL 全 SQ、命令连续提交的顺序；
  - `info` 输出 `seq=` / `pf=` / `cr=` 计数。
- 不改 wbuf / migrate / journal / checkpoint；不做 cache program（写）。
- **日志收敛**：GC 会话只在开始打一行 `[GC] start reason/trig/mode/free`；
  逐 victim 的 `[MIG] move` 降为 DEBUG，且只保留变化字段；INFO 只保留
  `[GC] start/progress/end` 与 10 s `[STAT]`。

**验证**：`make all` / `make release` 零新告警；`make tests UT=FC` 通过；
`make tests UT=FTL` 仍因冻结测试源编不过。**未上板**。

**介质布局**：无变化。

---

### v0.94 — FTL cache program（host 16 KiB + GC/WL 批次，host 正常路径已上板）

**日期**：2026-10-23

**内容**：

- **NAND/FC 状态语义**：新增 `NAND_STATUS_FAILC`（bit1，上一页 cache program 的结果）
  与 `CHECK_FAIL_CACHE`；op44 中间页检查 FAIL|FAILC。新增
  `OP_TWO_PLANE_PROGRAM_CACHE_END`（op48，末页 10h）与
  `OP_TWO_PLANE_PROGRAM_CACHE_FIRST`（op49，首页只查 FAIL，避免残留 FAILC 误判）；
  末页开始前先查上一页 FAILC，结束后同时查 FAIL/FAILC。普通 op31 不用于 cache 批次末页。
- **FC 批量提交**：`fc_submit_batch()` + `ftl_op_submit_batch()`，保证 N 条命令全进
  SQ 或全不进；不依赖 FC chain。
- **host 16 KiB batch**：新增 `cache_write.c/.h`。条件：wbuf 空闲、hot SB 的 page0 已成功
  编程且余量 ≥4 页、32 LBA 对齐。一次提交 op49（首页，只查 FAIL）+ 2×op44 +
  op48（末页）；中间页不确认映射、不递减组；
  末页成功后逐页 confirm（region 未就绪则停泊）。失败关闭 hot SB，已完成的成员立即
  报 IO_ERROR，在飞成员等各自完成回调结算。
- **GC/WL batch**：读仍逐页 READ/READ_SETTLE；满足 cold SB page0 已成功、余量 ≥4 页时
  攒最多 4 页，进入 `MIG_WRITE_BATCH` 一次提交 N 条 pair 命令（N≥2 用 15h+10h，N=1 用
  op31）；全部成功后 `MIG_WRITE_SETTLE_BATCH` 两遍式统一确认。失败关闭 cold SB、
  victim 不擦。静态 WL high-wear 路径暂不批。
- **page0 异步写屏障**：`super_block_s.page0_programmed`；page0 写成功前
  `alc_alloc_pair` 拒绝同 SB 的 page≥1。恢复扫描在 page0 空白时补探 page1..3。
- **观测**：`info` 增加 `cw=batches/pages/declines/aborts` 与 `mb=jobs/pages/aborts`。
- ADR-15 由“不启用”改为“受限启用”。

**review 收口（同日）**：host batch 失败按 PBA 归属关闭目标 SB；GC batch 对
`FTL_AGAIN` 重试、失败 drain 加硬 deadline；`info` 拆两行避免 192 B 截断；
`ftl_async_write_pair_cmd` 收敛 wbuf 的 pair 构造；Journal→L2P 提交循环由
`mig_commit_plane_meta` 共用；资源检查收敛到 `ftl_cache_budget_ok`；删除
`cache_wb.gi` / `source_page` 死字段；page0 屏障改为 wbuf page0 补全写 + GC dummy page0，不再在 `alc_alloc_pair` 直接拒绝 page≥1。

**验证**：`make all` / `make release` / `make tests UT=FC` 通过；`make tests UT=FTL`
仍因冻结测试源编不过。

**板端（2026-10-23，release 08:23:13）**：H2testw 415/423 MB 写+校验通过
（`Test finished without errors`），写 **843 KB/s**、读 **889 KB/s**；`info`：
`cw=24906/99624/7891/0`（host 16 KiB batch 生效，0 abort）、`mb=0/0/0`
（GC batch 未触发）。**FAILC 负例、失败注入、掉电注入仍未做**，host 正常路径
通过不等于 cache program 全部验证。

**介质布局**：无变化。

---

### v0.95 — Free Pool 水位回退 32

**日期**：2026-10-23

**内容**：

- `FREE_POOL_TARGET` 由 200 回退到 **32**；`FREE_POOL_EMERGENCY` 保持 8；
- GC T1 / BUSY 档水位 GC / WL 让路判据仍统一使用 `FREE_POOL_TARGET`，不新增第二套水位变量；
- 同步更新 `allocator.h`、`../ftl_design.md`、`ftl_api_usage.md`、`../testing_guide.md`、
  `logging_design.md`、`../known_open_items.md` 中的当前取值。
- 历史版本里的 `32→200` 记录保留不改写（H37）。

**验证**：`make all` / `make release` / `make tests UT=FC` 通过。

**板端（2026-10-23）**：H2testw 全盘写后 `free` 从 3392 降到 64 后稳定；
`free=64 > FREE_POOL_TARGET=32`，**T1 未被触发**，符合新阈值；未做 `free<32` 的
强制触发验证。

**介质布局**：无变化。

---

### v0.96 — 移除 T5 磨损 GC，磨损均衡统一走 WL（已上板）

**日期**：2026-10-23

**内容**：

- **问题**：T5 用全表 `max_erase/min_erase` 比较；`min_erase` 可能来自从未使用的空闲块，
  比值在数据搬移后仍不下降。板端 H2testw 期间 GC 持续运行，`free` 从 3388 被搬到 64
  后仍不停止，读写吞吐下降。
- **修复**：从 `gc_should_trigger()` 移除 T5（`GC_TRIGGER_WEAR` / `GC_WEAR_LEVEL_*` 删除）。
  磨损维护统一由 `wear_leveling` 的 UBI 判据
  （`max_free_ec − min_used_ec ≥ WL_THRESHOLD`）在 host 空闲时独立启动。
- `mig_begin_wl` 增加 `[WL] start free/writes` 日志；GC 日志不再出现 `trig=0x10`。

**验证**：`make all` / `make release` / `make tests UT=FC` 通过。

**板端（2026-10-23，release 08:23:13）**：H2testw 读写期间**无 `[GC] start` / 无 `trig=0x10`**，
`mig=0/0`、`mv=0`；写 **843 KB/s**、读 **889 KB/s**，恢复到/超过无 cache 基线
（765/884 KB/s）。`free` 下降由 host 写满盘造成，不是 T5。**空闲态 `[WL] start` 未观测**
（本次 free=64 且 UBI 阈值未满足）。

**介质布局**：无变化。

---

### v0.97 — DTCM/ITCM 热路径（正常路径已上板；压力/掉电未覆盖）

**日期**：2026-10-23

**内容**：

- **DTCM P1（整目标文件放置，无逐变量修饰）**：`ftl.o` 的
  `g_ftl`/`g_ftl_wbuf`/`g_ftl_journal`、`fc.o` 的命令池/描述符池/SQ/HSQ/CQ/簿记数组、
  `nand_dispatch.o` 的 dispatch 队列、`fc_bch_engine.o` 的 BCH 静态表移入
  `.dtcm_data`（交换版 release 67,832 B）；`metadata.o` 的 `g_ftl_md`（33,752 B）
  回到 **SRAM4**（TCM 之前的基线域）。NAND 页缓冲固定 **AXI_NOCACHE**
  （放 DTCM 会破坏 multi-plane program，见 RISK-TCM-08）；`ftl_init` 入口新增
  `memset(ftl,0,sizeof(*ftl))`（DTCM 是 NOLOAD）。
- **ITCM P1（整目标文件放置，无逐函数修饰）**：H743 `Reset_Handler` 新增
  `.ssd_code_high` 从 FLASH 到 ITCM 的复制；链接脚本按目标文件收
  `ftl.o` / `l2p_cache.o` / `wbuf.o` / `pending.o` / `cache_read.o` /
  `cache_write.o` / `fc.o` / `fc_data_path.o` / `fc_bch_engine.o` /
  `nand_dispatch.o` / `nand_fmc.o`（公共集：debug 61,992 B，94.6%）。
- **release 额外 ITCM 集**：链接脚本另按对象路径 `*release/obj/...` 收
  `allocator.o` / `journal.o` / `metadata.o` / `metadata_job.o` / `migrate.o` /
  `gc.o` / `wear_leveling.o` / `bg_scheduler.o` / `bg_wiring.o` /
  `completion_table.o` / `ftl_op.o` / `fc_interface.o` / `fc_queue.o` /
  `fc_command.o` / `nand_engine.o`；debug 对象在 `debug/obj/`，不匹配。
  release `.ssd_code_high` 61,384 B（余量 4,152 B），DTCM 布局不变。
- linker 新增加 DTCM（预留 20 KB stack；历史实测 5,448 B，当前待复测）与 ITCM 容量 ASSERT；
  `rules_link.mk` 的 SRAM 汇总增加 ITCM 行。
- DMA/NAND 对象：`g_fc_buffer_pool` / `g_fc_return_staging` / `g_nand_channel_page_buffer`
  固定留 `.noncacheable_buffer`。`g_l2p_cache`（133.7 KB）与 `g_ftl_blocks`（82 KB）暂不移动。
- **顺带修复** `.noncacheable_buffer` 段通配：原脚本只有无点形式，
  `nand_dispatch.o` 的页缓冲漏匹配、作为 orphan 落进 AXI_RAM（cacheable），
  与“DMA 直访、D-Cache 关闭”的注释不符；补上带点通配（RISK-PLAT-01）。
- **BCH 去堆**：`fc_bch_init(13,4)` 原先需要 41 KB 表 + 32 KB `roots`，TCM 版静态
  数据后 DTCM 堆只剩约 67 KB，疑似初始化失败并静默跳过 ECC；改为 42,656 B 静态结构体
  （roots 改 bitmap），与 `g_ftl_md` 交换域后放 **DTCM**，并打
  `BCH ready` / `BCH init FAILED` 日志（RISK-TCM-06/09）。
- **口径更正**：归档文档的“栈峰值 55 KB”无实测依据；唯一有串口证据的是 v0.75
  `peak=5448 B`（`docs/archive/ftl_v075_board_verify.md`）。栈预留从 64 KB 改为
  20 KB，`stack` 增加 `heap_used`（RISK-TCM-07）。
- **页缓冲域定位（2026-10-23 板端 A/B）**：页缓冲放 DTCM 时，multi-plane program 的
  handler 收到正确 `bitmap=0xff` 与源 buffer，但 NAND 读回全 FF，8 个 CP 候选块全失败；
  基线（AXI_NOCACHE）`format` 成功，页缓冲放回 AXI_NOCACHE 后当前 TCM 版 `format` 也成功。
  ⇒ 页缓冲固定 AXI_NOCACHE，不得再放 DTCM（RISK-TCM-08）。
- **局部大对象**：`cp_step` 的 Anchor 2 KB、`cp_write_initial` 的 4 KB 栈帧、
  `ftl_boot_recovery` 的 2 KB `cp_header_s`、`jrn_raw_page_blank` / `cp_probe_media_status` /
  `bbt_medium_has_ftl_data` / PFR 扫描的 2 KB raw 缓冲全部移入 handle 或文件级 static。
  实测栈帧：`cp_step` 2,128→168、`ftl_boot_recovery` 2,116→68、`cp_write_initial`
  4,288→212、`cp_probe_media_status` 2,256→152、`bbt_medium_has_ftl_data` 2,192→80。

**验证**：`make all`（零告警）/ `make release`（零告警）/ `make tests UT=FC` 通过；
DTCM/ITCM 放置与 linker ASSERT 通过。`make tests UT=FTL` **未过，原因是冻结的测试源**
（引用已删除的字段），未回退生产代码。**已上板（正常路径）**：页缓冲 AXI 版 `format` 通过，
H2testw 415/423 MB **840/885 KB/s、0 错误**（基线 843/889）；`info avg=4209us`、
`cw=24906/99624/7822/0`、`bad=0`；`stack peak=2,964 B`、`heap_used=0`、`alarm=0`。
**BCH↔g_ftl_md 交换版 + release 额外 ITCM 集**（BCH DTCM / md SRAM4 / ITCM 61,384 B）
同日上板：boot `BCH ready`、恢复通过，H2testw 415/423 MB **843/887 KB/s、0 错误**，
`info avg=4171us`、`cw=24906/99624/7891/0`、`bad=0`。相对 840/885 差 +3/+2 KB/s，
属测量抖动；本次 `mig=0/0`、`gcm=0`，未覆盖 GC/迁移路径 ⇒ 额外 ITCM 集收益未证实，
按 RISK-TCM-05 保留（无回归）并待 GC/后台压力 A/B。压力/掉电场景仍未覆盖。

**介质布局**：无变化。

---

### v0.98 — 可读性重构（未上板）

**日期**：2026-10-23

**内容**：
- BBT 候选池判断去重：新增 `bbt_block_is_candidate()`，`bbt.c` 与
  `power_failure_recovery.c` 三处手写表达式统一；`borrowed_log` 拆具名 bool。
- allocator 收薄：`alc_refill_free_from_op` 合并为返回 gblock 的单一函数；
  `alc_close_sb` 与列表追加合并为唯一 live setter，并上抛 `FTL_BLOCK_NO_SB`。
  分配策略收敛为 `alc_alloc_mode_e`（HOT / COLD / COLD_HIGH_WEAR），删除
  `alc_alloc_pair_impl` 与 `alc_alloc_pair_high_wear`。
- 磨损快照：新增 `bst_wear_s`，`alc_block_wear_get/restore_wear` 由 7 参数收为 3 参数。
- `alc_is_bbt_format_in_progress/rebuild_required` 移到 `allocator.h` `static inline`。
- cache 可读性：`ftl_cache_write_try` 条件拆具名 bool；`cache_read.c` 拆
  probe/complete/contiguous/build/release/submit helper，内部用 `cache_read_req_s`；
  `cache_wb_build` 拆 alloc/cmd/pending 三步。读/写合并为 `cache_batch.c/.h`
  （各自保留独立小节），删除 `cache_read.*` / `cache_write.*`；随后按“单调用纯转发”收敛，
  cache 内部函数由 16 个降为 8 个（read 5 + write 3）。
- 后台调度：删除 `bg_wiring.c` 重复的 `ftl_now_ms`，改用 `ftl_job_now_ms`；
  `bg_scheduler_run` 拆 must-run / budget 两 pass。

**验证**：`make all` / `make release` / `make tests UT=FC` 通过，零新告警；
`make tests UT=FTL` 仍因冻结测试源不可用（未改测试、未回退生产代码）。
ITCM：release **61,296 B**、debug **62,064 B**；`.dtcm_data` 67,832 B、
`.sram4_data` 33,760 B 不变。**未上板**；需 `format` + H2testw 回归。

**介质布局**：无变化。

---

### v0.99 — allocator 命名统一（未上板）

**日期**：2026-10-24

**内容**：
- 函数名统一为 `alc_<resource>[_<state>]_<action>[_<detail>]`：
  `free` / `op` / `spare` / `super` / `bst` / `bbt` / `pool`；
  `super` 状态用 `open` / `closed`。
- 删除旧名混用：`alc_free_block`、`alc_remove_from_free`、`alc_alloc_pair`、
  `alc_close_open_sb`、`alc_get_sb_stats`、`alc_block_wear_get` 等改为资源前缀。
- 公开枚举/类型同步：`alc_super_alloc_mode_e`、`alc_super_pba_state_e`、
  `alc_super_stats_s`、`alc_bst_erase_stats_s`。
- `allocator.h` 的 pointer-to-array 字段改为数组 typedef，避免
  `WORD(*op_blocks)` 被 clang-format 拆行。
- BBT：`bbt_scan_factory` → `bbt_factory_scan`、`bbt_spare_pool_refill` →
  `bbt_spare_refill`；`bbt.c` / `bbt.h` 按池几何→初始化/扫描→格式化→标坏/持久化→
  异步 job→Spare→诊断重排。

**验证**：`make all` / `make release` / `make tests UT=FC` 通过，零新告警；
`UT=FTL` 仍因冻结测试源失败。**未上板**；纯命名/声明整理，不改行为。

**介质布局**：无变化。

---

### v1.00 — cache read 判据修正（已上板）

**日期**：2026-10-25

**问题**：FTL two-plane cache read 自 v0.93 实现以来**从未生效**（`cache_read_batches` 恒 0，
每个 ≥16 LBA 的读在组首 decline 一次后回退普通读）。速度用例 `/speed/ftl_rw` 的探针定位到
`cache_read_collect` 的两处判据错用：

- `complete` 跨 plane 比较 `pba_page()`。`pba_page()` 只清 CW 位、**保留 plane/block 位**
  ⇒ 两 plane 的同一页号永远不等（实测 plane0 `0x00045000` vs plane1 `0x000CDC04`），
  收集结果恒为 0 个 pair。
- 相邻 pair 的连续性用 `pba_page()+1`，那是给 **CW 位**加 1，不是页号 +1。

**修正**：跨 plane 比较与连续性判断改用**页号字段** `pba_to_page()`；连续性仍要求
同 plane 同块。跨 plane 不要求块号相同（Policy B 两 plane 独立选块），符合多平面 cache
命令「两 plane 页号一致」的语义。

**验证**：`make all` / `make release` / `make tests UT=FC` / `make tests` / `make tests UT=FTL`
通过，零新告警。**2026-10-25 已上板验证**：`/speed/ftl_rw` 2/2 通过，cache 档
`rd_path=cache`、`rdc(b/d/f)=1600/0/0`、`err=0`（详见 RISK-HOST-13）。

**介质布局**：无变化。

---

### v1.01 — 注释规范统一（FTL 全量，未上板）

**日期**：2026-10-29

**内容**：FTL 19 个 `.c` + 24 个 `.h` 的函数契约统一为 `/** */`
（`@brief` / `@param` / `@return` / `@note`），实现细节统一为 `//`；对外函数契约只写在
`.h`，`.c` 侧不重复参数表与返回值；多值 `@return` 一行一值。删除旧式
`/* ---- 功能/参数/返回 ---- */` 横幅，并清理 H37 变更史叙述（版本号 / 日期 /
提交号 / 「原先·已删除」等）。

**验证**：`make all` / `make release` / `make tests UT=FTL` 通过，零新告警；
代码 token 与改动前逐文件比对一致（纯注释）。**未上板**（无行为变化）。

**介质布局**：无变化。

---

### v1.02 — cache read/write run 标成 FC chain（未上板）

**日期**：2026-10-30

**内容**：
- cache read 的 run 长度改由 FC 资源决定（`FTL_CACHE_READ_MAX_PAIRS =
  FC_BUFFER_POOL_DEPTH / FTL_LBA_PER_PAIR`，实际批深再按预算收缩），不再等于
  `NAND_DISPATCH_CHAIN_DEPTH`；尝试节拍独立为 `FTL_CACHE_READ_TRIAL_LBAS`（32 LBA）。
- 整条 run 标成一条 FC chain：末条 `FC_CHAIN_END`，其余 `FC_CHAIN_MORE`；
  cache read 由逐条 `ftl_op_submit` 改为 `ftl_op_submit_batch` 原子入队（失败整批回滚）。
- cache program（host 16 KiB）与 GC/WL batch 同样标 chain。
- FC 侧复用既有 `chain_owner`：链跨填充 pass 时锁住该 LUN，另一队列回推；
  **无 FC 功能改动**，只同步“生产路径恒 END / 仅单测覆盖”相关注释。
- `FTL_CACHE_READ_ERROR` 保留：原子入队后它不再表示“部分提交”，改为表示永久提交
  失败（如链契约违反）；`ftl_op_submit_batch` 把非 `QUEUE_FULL` 失败映射为 `FTL_ERROR`。
- `fc_submit_batch` 增加链契约守卫：批内含 `FC_CHAIN_MORE` 时要求全批同优先级
  **且**末条 `FC_CHAIN_END`，否则 debug `ASSERT` + release 返回
  `FC_STATUS_INVALID_ARGUMENT`（防链被拆到 SQ/HSQ、防 owner 永不解锁）。

**验证**：`make all` / `make release` / `make tests UT=FTL` / `make tests UT=FC` 通过，
零新告警。板端（2026-10-30，用户）：H2testw **415/423 MB、无错误、843/887 KB/s** ⇒
host 4 页链（读 45h/46h×3/47h、写 49h/44h/44h/48h）正常。**未覆盖**：跨 pass owner
（链长 >4 页）、HSQ 回推、失败页状态、守卫拒绝路径；这些仍需 FC UT / 大请求上板。

**介质布局**：无变化。

---

### v1.03 — FC 状态字重构：descriptor / command 分开（未上板）

**日期**：2026-10-30

**内容**：
- 状态拆成两套 8 位坐标：descriptor（per-CW：FAIL/RAID/MATA/CRC/UNC/EMPTY）与
  command（命令级：NAND_FAIL/FAIL_CACHE/TIMEOUT/DATA_ERROR/MATA/AREADY/RDY/WP）。
  命令状态不再复制数据路径类别；具体类别读 `desc_id` 指向的 descriptor。
- 明确 NAND 一个 OP 的**前后两次读状态**：`pre_status`（RDY/ARDY/WP）与
  `nand_status`（FAIL/FAILC）。命令成功 = 后一次读状态 FAIL/FAILC 全 0，不是 RDY。
  引擎在第一个 `JUMP_IF_NOT_READY` 循环里捕获并保留 `pre_status`（busy 超时也保留）。
- `FC_CW_STATUS_BLANK` → `FC_CW_STATUS_EMPTY`；成功 = `FC_CW_STATUS_UNUSED`(0)，
  不再有 READY 成功位。
- FTL 一律改用 `fc_cmd_status_has_error()` / `FC_CMD_STATUS_ERROR_MASK`；
  RR 的 CRC-only 判据改为回看 desc 链（命令级 `DATA_ERROR` 合并了 UNC/CRC/RAID）。
- 修 RISK-IF-08：desc 合并按 flat CW 把 NAND per-CW 结果并进 **所有带 desc 的命令**
  （program 也并）；page-read 的 TIMEOUT 现在一定进命令状态，不再可能被数据路径的
  READY 掩盖。program 失败平面归因因此不再回退 plane0（`ftl.c` 的 bad_plane_mask）。

**验证**：`make all` / `make release` / `make tests UT=FTL` / `make tests UT=FC` 通过，
零新告警。DWARF 核实 `fc_cmd_status_s` 仍 12 B、`fc_desc_s` 仍 20 B（C6）。
**未上板**：FC UT / H2testw / 超时与 CRC 负例仍未跑。

**介质布局**：无变化。

---

### v1.04 — cache 序列改由 FTL 顺序流检测器驱动 + Journal 顺序映射页（设计 A；2026-10-31 上板验证）

**日期**：2026-10-31

**起因**：v1.00 的"流式"实际仍按 host 传输段截断——`ftl_cache_read_try/write_try` 以
CBW 的 `total_lbas/offset_lbas` 作命令上下文，序列每 16 KiB 就重开；H2testw 的
`cr≈3.93 pair/batch` 就是证据。用户定调：**顺序读写是 FTL 的判断，host 传输段不是 FTL
的语义边界**；顺序写占用 plane cache 寄存器期间元数据写不下来，所以 FTL 要**主动按配额
截断**，落完元数据再开新段。

**关键决策（设计 A）**：

| 决策 | 选择 | 拒绝方案 | 理由 |
|---|---|---|---|
| 序列开启 | FTL 顺序流检测器（连续 LBA ≥ 64） | host 下发开关（CBW 总长/偏移） | 传输段是 USB 的交付粒度，不是 FTL 语义 |
| 序列延续 | `cache_seq.next_lba == 请求起始 LBA` | 同 SCSI 命令上下文 | 同一 LBA 流跨段延续；命令身份不是判据 |
| 命令末尾 | `cmd_remaining`（只给 END 落点） | 用命令上下文判续接 | 器件要求 END/LAST 落在最后一个数据页 |
| Journal 条目 | 顺序写用 JSEG 页（4B/条，LBA 隐式） | 全部 16B 通用页 | 504 条/页 ⇒ 一段可覆盖 252 KiB；DFTL translation page 同形态 |
| 主动截断 | 到 JSEG 页配额（63 pair 页）END，START 时刷旧页 | 每传输段 END | 元数据写的必要代价；掉电窗口从 16 KiB 放宽到 252 KiB |
| 门控 | 后台元数据（Journal/L2P/GC/WL/CP/metadata/BBT）被挡但**不**置 `break_requested` | 所有后台提交都请求断点 | 否则后台每轮重试都把序列切回逐窗口（v1.00 的 `cr≈3.93` 主因） |

**实现落点**：
- `ftl_io_req_s.total_lbas/offset_lbas` → `cmd_remaining`（单个 DWORD）；`pend_group_s`
  同步替换（`cmd_start/cmd_total` 删除，结构体仍 56 B）。
- `ftl.h` 新增 `ftl_stream_det_s stream_rd/stream_wr` 与常量 `FTL_STREAM_MIN_LBAS` /
  `FTL_CACHE_WRITE_STREAM_ENTRIES`；`ftl_submit_common` 更新检测器，
  续接判据改为 `next_lba`。
- `cache_batch.c`：开启条件 = 检测器 + 请求起点 pair 对齐 + 无普通写确认在飞；
  写序列加配额与 `cmd_remaining` 断点；读序列 `run_limit` → 命令末尾。
- `journal.h/.c`：页头 12 B → 16 B（+`base_lba`）；新增 `JRN_MAGIC_SEG` / `JRN_PAGE_SEG`、
  `jrn_seq_begin` / `jrn_seq_reserve` / `jrn_seq_ready` / `jrn_append_seq`；
  `current_entry` BYTE → WORD；回放按页型解析，JSEG 的 `old_pba` 从 L2P 基线现读。
- `ftl_op.c`：后台元数据（Journal/L2P/GC/WL/CP/metadata/BBT）被门控但不置
  `break_requested`；只有 host / 同步 / RR 提交请求断点。
- `migrate.c`：`mig_job_wait` 在 cache 序列打开时不计 `FTL_WAIT_TIMEOUT_MS`——长段
  （~300 ms 墙钟）下否则 GC/WL 每 50 ms 中止一次，host 持续写时拿不到 Journal 容量。
- 观测：`info` 的 `cw=` 行新增 `sw=段数/配额断点数`；`cr=` 行新增读段数
  （`batches/pairs/starts/declines/fallbacks`）。判据 = 读 `batches/starts`、写
  `pages/starts` 明显 > 4（写应接近 63）。

**验证**：`make all` / `make release` / `make tests UT=FTL` / `make tests UT=FC` /
`make tests` 通过，零新告警。**未上板**：顺序写 `pages/starts`、跨段 START/SEQ/END、
JSEG 页回放（掉电重启）、配额断点后的元数据落盘都需板端证据（R1）。

**介质布局**：**v13**（Journal 页头与页型变化；首启自动格式化，旧数据不保留）。

---

### v1.05 — 设备序列跨 host 命令：volatile write cache + padding END（2026-10-31 上板验证）

**日期**：2026-10-31

**起因**：v1.04 的序列仍以 host 命令为边界（`cmd_remaining==0` 即 END），实测每条
SCSI WRITE 只有 ~57 KiB（3.58 × 16 KiB），63 pair 的顺序页配额永远触发不了
（`sw=7455/0`）。用户定调：**这是配套决定，按主流做**——把 host 回执与最后一页的
确认解耦，让设备序列真正跨命令。

**关键决策**：

| 决策 | 选择 | 拒绝方案 | 理由 |
|---|---|---|---|
| 写段回执 | **所有写段（含末段）提交即回执** | 末段等 FTL 确认 | BOT 下主机等 CSW 才发下一条命令；末段不提前回执就无法跨命令（会 1 s 超时） |
| 写失败上报 | **deferred error**：下一条 READ/WRITE/FLUSH 失败一次 | 改写已发 CSW（不可能） | 主流写缓存语义；TinyUSB 无自定义 sense，用"下一条命令失败"近似（RISK-USBC-01，本仓自拟近似） |
| 无数据 END | **padding END**：不映射零页（META=UNKNOWN）作为 48h | abort（丢已回执数据） | 器件没有不带数据的 END；padding 每段最多一次（≈0.4% 页写） |
| 主动断点 | Journal 顺序页写满（504 条 = 63 pair ≈252 KiB） | host 命令边界 | 顺序序列占用 plane cache 寄存器，元数据只在断点落盘；页可跨命令续用 |
| 读序列 | 同样跨命令；LAST 只在物理/region 断点 / 命令尾 / D4 | 每条命令 LAST | lookahead 改为按"路由/键空间 + region 驻留"判续，不再用命令末尾 |

**实现落点**：
- `usb_msc.c`：写段（含末段）提交即 `tud_msc_async_io_done`；槽位保留到 FTL 完成只回收
  缓冲；`s_deferred_write_error` 在下一命令数据入口上报一次；写延迟改在**回执点**测量。
- `ftl.c`：`close_requested` / `closing` 状态；`ftl_cache_seq_close_step`（Phase 2.5）
  提交 padding END；`ftl_cache_pad_submit` / `ftl_cache_pad_callback`（`FTL_OP_CACHE_PAD`）；
  padding 页落在 page0 成功时记 `page0_programmed`、失败关 hot SB；flush 入口置
  `close_requested`；idle 守卫与 `ftl_submit_common` 不再 abort 写序列。
- `cache_batch.c`：写/读的 `end_seq` 去掉 `at_cmd_end`；写窗口按 `jrn_seq_room()` 收缩，
  **页满**才是主动断点；LBA 不连续 / 低空间 / 前台等待 → `close_requested` + AGAIN
  （不计 stall）；读 lookahead 用 `lba_routes_to` + 键空间上界替代命令末尾。
- `journal.h/.c`：`jrn_seq_begin(lba, need)` 跨命令续用（LBA 连续 + 空间够）；新增
  `jrn_seq_room()` / `jrn_page_writes()`（`info` 的 `jf=`）。
- 观测：`cw=` 行新增 `sw=starts/page_ends pc=pad_closes jf=page_writes`。
- ITCM：padding/close 路径与 Journal 开机路径（scan/resume/replay）改放 `.ssd_code_fast`
  （Flash，boot/冷路径），release `.ssd_code_high` 62,696 B（余量 2,840 B，RISK-TCM-11）。
- **cache 路径自开 block page0**（`alc_super_open_get_room_seq` + `cache_seq.own_page0` +
  成功结算时记 `page0_programmed`）：消除每个 block 边界回退 wbuf 的整段（≈4 页）及其
  引起的 Journal 顺序页冻结；page0 屏障改由**有序管线**自证（page0 FAIL_CACHE 失败中止
  其后所有页，不击穿 rebuild 捷径，RISK-CACHE-19）。剩余：63 pair 的 JSEG 配额与 64 页
  block 差 1 页，每块最后 1 页仍走 wbuf（1 页/块）。
- **顺序流跨 region 预加载**（写/读 START 时用 `lba_next_region_base()` 额外加载本实例的
  下一个 L2P region；`ftl_seq_note_read` 的预取目标也收敛到同一推导）：L2P region 边界
  不再把序列切成两半，段长从平均 ~32 页升到接近 63（JSEG 页容量）。用
  `l2p_region_prefetch`（best-effort：thrashing / 在飞上限 / FC 预算节流，不踢脏行、
  不挤 host load），失败/被驱逐时 lookahead 照常 END（安全降级）。

**验证**：`make all` / `make release` / `make tests UT=FTL` / `make tests UT=FC` /
`make tests` 通过，零新告警。**未上板**：deferred error 注入、跨命令 START/END 计数、
`pages/starts` ≈ 63、padding 收尾、断电窗口都需板端证据（RISK-CACHE-15/16/18、RISK-USBC-01）。

**介质布局**：无变化（v13）。

---

### v1.06 — 读写序列跨物理 block（器件原文判据；2026-10-31 上板验证）

**日期**：2026-10-31

**起因**：用户贴出 MT29F4G08 datasheet 原文，纠正了两处判断：① `READ PAGE CACHE
SEQUENTIAL (31h)` **可以跨 block**（同一 die；禁止跨 die/LUN）；② `PROGRAM PAGE CACHE
(80h-15h)` 每条命令**自带地址**，写侧本来就没有物理连续性要求。原文见 datasheet 的
"Read Cache Operations" / "Two-Plane Read Cache Operations" 两节（链接见
`docs/reference/README.md`；R12 判据出处）。此前把"同 block"当成器件
语义（RISK-DOC-02）是错的。

**关键决策**：

| 决策 | 选择 | 拒绝方案 | 理由 |
|---|---|---|---|
| 读连续性判据 | 同 block page+1 **或两 plane 都 block+1/page0** | 放宽到任意物理相邻 | 31h 由器件自增，只能表达"下一页"；两 plane 必须同时成立（two-plane 31h 每 plane 各前进一页） |
| 读单段上限 | `FTL_CACHE_READ_MAX_RUN_PAIRS` = 128 pair | 不限长 | 读序列期间该 die 的 cache 寄存器被占用（ARDY=0），程序命令发不进去 ⇒ 断点是后台元数据的让步点（与写侧 Journal 配额同性质） |
| 写 block 断点 | **取消**：窗口吃干 SB 后由 `alloc_pair` 关旧块开新块，序列继续 | 保持 block 到头 END | 44/48h 每条自带地址；窗口仍 clamp 到 SB 剩余（一次构建不跨 SB，保 `rewind_pair` 回滚前提） |
| 写开序列阈值 | 允许 1 页窗口（原 ≥4） | 保持 ≥4 | 63 pair 的 JSEG 配额与 64 页 block 差 1 页：块尾页不再回退 wbuf |
| 跨块准入 | 收尾块时 `alc_super_can_open` 为 false ⇒ 该窗口 END | 无条件跨块 | 否则序列停在「设备序列打开、无页可提交」：门控挡 GC、padding END 也无页可分配（RISK-CACHE-15 盘满例） |

**实现落点**：
- `cache_batch.c`：`cache_read_pair_contiguous()`（收集与 lookahead 共用）；读 `run_pairs`
  计数 + 上限强制 LAST；写 `end_seq` 去掉 `block_end`；`min_pages` 起始改 1；
  `jrn_seq_begin` 预留量改 `min(4, room)`；`alc_super_can_open` 准入。
- `allocator.c/.h`：新增只读 `alc_super_can_open()`。
- `ftl.h`：`cache_seq.run_pairs`；`own_page0` 注释更新（跨块后由新块分配重新置位）。
- 文档：§3.16.4/§3.16.5、§3.16.10 限制表与验收、`../known_open_items.md` RISK-CACHE-20/21、
  RISK-DOC-02（已改）。

**验证**：五个构建关（`make all` / `make release` / `make tests UT=FTL` / `make tests UT=FC` /
`make tests`）通过，零新告警。**未上板**：`cr` 的 `pairs/starts`（~32 → 65~128）、
`wr − cw pages`（wbuf 页数）趋近 0、`jf`、长读/长写期间 GC 进展都需板端证据。

**板端首跑（2026-10-31，debug 镜像）**：写阶段跑到 405.6 MiB 时 `[ERR] drain stalled gi=0 next=0/32`，该 chunk 已提前回执 ⇒ deferred error ⇒ 主机判「卷被外部更改」并反复重挂，此后 `free` 冻结在 135 块、小写也全失败。**写出逻辑本身判读正常**：`sw=1622/1575`、`pc=45`、`d4=2` ⇒ 1622 次 START 全部有序收尾（1575 page_end + 45 padding + 2 D4），`cw aborts=0`、`pages/starts`=62.9；`jf=3362`；`wr=103,892` 对 `cw pages=101,947`（差 ≈1,900 = wbuf 回退页，仍 ≈1 页/块）。**故障已归因（第二跑 dump）**：`free=1489 sb_open=1 np=1 p0=01 wb_busy=1 pend=0` ⇒ padding END 页落在新 SB 的 page0 时，回调用 `pba_with_plane(plane0,1)` 推导 plane1 地址，Policy B 下块号不同 ⇒ plane1 的 page0 屏障漏记 ⇒ wbuf 永久 `FTL_AGAIN`（RISK-CACHE-24）。修复：`cache_seq.pad_pba[PLANE_NUM]` 记录 submit 时真实两 plane PBA，回调逐 plane 记账。新增的 `ftl_drain_stall_dump()` 与 `bg_progress` 两条确定性等待（Journal 在飞 / 停泊确认）保留。同时观察到 CP 长期停在 `phase=8`（RISK-CACHE-23），且因 CP 未推进，失败后掉电重启时上次数据不可见（anchor 仍为初始 seq=1）。**修复后上板验证（2026-10-31，17:55 镜像）**：H2testw **415/415 MB 0 错误**，写 894 KB/s、读 888 KB/s；`pc=51` 无 stall、`sw=1663/1610`、`aborts=0`、`pages/starts`=62.8；读 `cr=27435/105840/2583/430/0` ⇒ `pairs/starts`=41.0（预取 100% 命中；段长受分配器物理块不连续限制，非 128 上限）。Injection 类（掉电回放/deferred error/page0 失败）仍未做。读阶段未跑，读侧跨 block 仍未验证。

### v1.07 — 低 free 成对分配失败：per-plane 水位 + victim 平衡 + 错误语义（2026-10-31 上板验证：生路；漂移由 v1.08 修）

**日期**：2026-10-31

**起因**：GC 压力测试（写满 → 删文件 → 再写）在 `free=264` 时 `drain stalled`：`sb_open=0`、`nc=64`、`pend=0`/`wb_busy=0`/FC 池全空 ⇒ 唯一阻塞是**打不开新 SB（每 plane 各需一块）**。根因：恢复路径把 closed 列表建成**单 plane 条目**（CP 快照 word 无 `sb_id`），GC 逐块回收空块（`nop` 54→671、`mv=0`）使 free 池 plane 失衡；`cache_wr_build` 又把永久失败映射成 `FTL_AGAIN`，重试到 stall（RISK-ALLOC-09）。

**改动**：
- `allocator`：新增 `free_plane_count[PLANE_NUM]`（唯一闸门 push/remove 维护）+ `alc_free_get_count` / `alc_free_min_plane_count`；恢复重置点同步清零。
- `gc`：T1 增加「任一 plane 的 free < `FREE_POOL_PLANE_LOW`(4)」；victim 选择在饥饿时优先「成对条目 + 饥饿 plane 的单条目」，偏好组空才退回原贪心；两 plane 健康时行为不变。
- 错误语义（H38）：`cache_wr_build` / `wb_flush` 把 alloc 的 `FTL_ERROR` 原样上报，不再伪装成 `FTL_AGAIN`。
- 诊断：stall dump 与 `info` 的 diag 行增加 `fpmin`（最饥渴 plane 的空闲块数）。

**未做**：持久化 block→SB 归属（恢复期成对重建）——用户 2026-10-31 决定不做打折版，登记为 `../known_open_items.md` ISS-FTL-17（需介质格式升级 + 首启格式化 + 掉电恢复重验）。

**验证**：五关构建零新告警；release `.ssd_code_high` 63,664 B（余量 1,872 B）。**未上板**：验收 = 同一 GC 压力场景下`free` 跌破 32 后写不中断、`fpmin` 不长期贴 0、`mig mv` 有增长。

### v1.08 — L2P 区域写回补 WAL 次序（journal 先落盘；2026-10-31 上板验证）

**日期**：2026-10-31

**起因**：GC 压力测试出现 `dng=10`/`rrab=1215`、读 LBA 0 命中 META 不符页并被 RR 退役（data lost）、H2testw Code 34。静态分析定位：确认路径是「Journal 追加到 RAM 页缓冲 → L2P 行原地更新」，两者落盘无次序；`l2p_flush_kick` 不等 Journal 页落盘就写回区域 ⇒ 掉电后「映射在、valid_count 缺」⇒ 漂移/丢数据（RISK-L2P-09）。

**改动**：
- `journal`：`jrn_handle_s` 增加 `persisted_seq` / `write_page_seq` / `write_entry_count`；`jrn_freeze` 记录在飞页的序号区间，页写成功时推进 `persisted_seq`；新增 `jrn_persisted_seq()`；`jrn_init`/`jrn_resume` 初始化水位。
- `l2p_cache`：行增加 `dirty_seq`（映射更新后记录 `sequence_counter`）；`l2p_flush_kick` 增加 WAL 闸门（`persisted_seq < dirty_seq` ⇒ `wal_defer++` + `FTL_AGAIN`）；行加载/失效清零；`wal_defer_count` 经 `info` 的 `wd=` 观测。
- `main.c`：L2P owner 端口增加 `journal` 指针（前向声明 `struct l2p_jrn_s`，避免 journal.h ↔ l2p_cache.h 环）。

**验证**：五关构建零新告警；release `.ssd_code_high` 63,856 B（余量 1,680 B）。**上板验证（2026-10-31 20:38 镜像）**：format 后基线全盘写+校验 `dng=0`/`rrab=0`（`wd` 计数证明闸门生效）；删文件重写压到 `free<32`（GC 启动）时拔 USB 掉电 → `boot type=2`、`journal_entries=4608`、`closed=3354 free=28 anomalies=0`、**`dng=0`/`rrab=0`**；chkdsk 修主机 FS 后 verify **127 MB 0 错误**。对比修复前同场景 `dng=10`/`rrab=1215`/数据丢失 ⇒ RISK-L2P-09 闭合。判据仍成立：`dng=0`、`rrab=0`、verify 0 错误、`wd>0`（闸门确实生效）。

**介质布局**：无变化（v13）。

---

### v1.09 — debug 注入 seam + 失败侧 UT（2026-10-31 上板验证）

**日期**：2026-10-31

**起因**：`ISS-FTL-16` —— 最贵的几条「静默失效」验证（cache FAILC 中止、page0 失败、
Journal 写失败）只能等真 NAND 自发失败，无法重复执行。

**改动**：
- 新增 `include/common/fault_inject.h` + `src/common/fault_inject.c`：debug-only 注入点
  （`NDEBUG` 下退化为 `static inline` 空实现，release 零代码）。`fi_cache_fail_arm(page_lba)`
  在指定 cache program 命令的完成上伪造 `FAIL_CACHE`（一次性；器件语义 = 命令 K 结算
  page K-1）；`fi_journal_fail_arm(n)` 让接下来 n 次 Journal 页写按失败结算；`fi_reset()`
  由 pure/media 两个测试夹具在 setup 调用。
- `l2p_cache`：WAL 闸门在 `jrn_has_write_failure()` 时**放行**（RISK-L2P-10）。
- `pending`：新增 `pend_confirm_get_handler()`（仅供测试临时替换并原样还原停泊 handler）。
- UT：新增 `l2p_wal_gate`、`confirm_park_timeout`、`cache_failc_abort`（arm=1/2/last）、
  `journal_write_fail_wal_stall`、`cache_seq_cross_block`、`bbt_ring_reload`、
  `boot_restore_reconcile`；扩展 `free_pool_invariants`（两本账恒等）、`gc_trigger`
  （单 plane 水位）、`gc_select_victim`（饥饿 plane 偏好/回退）、`journal_barrier_cp_progress`
  （`persisted_seq ≥ 写入序号`）、`trim_no_resurrection`（不 request CP 的 journal-only 段）。

**RISK-L2P-10（本批修复）**：Journal 页写失败后，失败页条目已随 `page_buf` 清空而丢失，
`persisted_seq` 永远覆盖不到那段序号 ⇒ WAL 闸门把 Checkpoint 的区域落盘永久挡住，
而清除失败标记（`write_fail_pending`）的唯一途径就是「CP 完成」⇒ 死锁。修复 =
失败窗口内闸门放行（CP 本身就是恢复机制：提交后快照全部 L2P 并截断 Journal）。
该缺陷由新增用例在首次上板时暴露。

**验证**：五关构建零新告警。**上板（2026-10-31 22:08 镜像）**：`test /ftl/media/`
**36/36 通过**（108.5 s），含 `journal_write_fail_wal_stall`（1.11 s）、`cache_failc_abort`
arm=1/2/last（6.83 / 10.37 / 10.73 s）、`cache_seq_cross_block`、`bbt_ring_reload`、
`boot_restore_reconcile`；纯 RAM 套件在同日上一轮全量运行中全绿（含 `l2p_wal_gate`、
`confirm_park_timeout`、两本账/水位/victim 三例）。

**介质布局**：无变化（v13）。

---

### v1.10 — FTL volatile write cache：早回执 + 读可见 + deferred error（未上板）

**日期**：2026-11-01

**起因**：`RISK-CACHE-25` —— 同步主机（`ftl_submit_write` + 等 `complete_flag`，一条命令在飞）
在组完成前不发下一段 ⇒ 尾页只能等 `FTL_CACHE_SEQ_IDLE_MS`（1 s）padding 收尾，
`/speed/ftl_rw chunk=32` 实测 142 KB/s（`wr_ms=180001`）。根因不是断点策略，而是
**完成点**：设备 cache program 的结论滞后一页，组等到结论才完成。

**改动**（用户裁决：不推翻 v1.05 的跨命令序列，改完成点）：
- `cache_seq.wr` 从「待结算 FIFO」升级为**已编程页表**（`lba/pba0/pba1/cw_bitmap/state/proven`
  + 结论游标 `pend_idx`）。页在**自身命令完成**时入表并立即结算子操作（早回执）。
- 确认（Journal→L2P→BST）与组完成解耦：`ftl_cache_wr_confirm_step()` 每轮 service 按提交序推进。
- 读覆盖层：`wb_read` 之后、`l2p_read` 之前查页表，命中直读刚编程的 PBA（RISK-CACHE-27）；
  cache 读批量对含未确认 LBA 的 pair 判不可批量。
- FAILC 撤销：Journal + L2P 写 `NAND_PBA_INVALID`、region **立即** degraded、粘性 deferred error；
  失败页覆盖的 LBA 读直接 IO_ERROR（`ftl_cache_wr_failed_covers`，RISK-CACHE-28）。
- 序列中止语义保留 RISK-CACHE-19：任一错误（含 FAILC）⇒ 其后未证明页一律不确认；器件已证明的页保留。
- flush 屏障：等页表排空（`wr.count==0`）**且没有在飞成员**（`wr_inflight==0`）+ Journal/L2P/wbuf 空闲（RISK-CACHE-29）。
- 序列表号 `cache_seq.gen` 只在**中止**时递增（迟到完成守卫）；窗口构建的环/顺序页余量
  计入 `wr_inflight`（否则完成后集中入表即溢出）。
- 控制器：`usb_msc_take_deferred_error()` 先问各 FTL 实例（早回执后 FAILC 的结论只存在于
  FTL 的粘性标志里）。FUA **不实现**（用户裁决，RISK-USB-05 / ISS-USB-03）。

**UT / 登记同步**：`cache_failc_abort` 的判据改为「失败页读 IO_ERROR、同批未确认页读旧
数据、重启后失败 LBA 读零（降级位不持久）」，并在失败写后先断言 `ftl_take_deferred_error`
只报一次（否则粘性错误会挡掉后续读）；RISK-HOST-05 / CP-04 / IF-13 的提交 deadline 判据
统一为 `ftl_op_bg_submit_deferred()`（序列打开或主机组在飞）；`ut_run_all()` 每条命令复位
media 会话状态（RISK-UT-08）。

**验证**：五关构建零新告警（`all` / `release` / `tests UT=FTL` / `tests UT=FC` / `tests`）。

**2026-11-01 板端（release -O2 全量 UT，镜像 `04:31:58`）**：`test` **156/156**（3 skipped，
237.9 s），含之前只在隔离路径验过的项：
- `/ftl/pure/` 58/58、`/ftl/media/` 36/36（`cache_failc_abort` **arm=1/2/last 全过**：失败页读
  IO_ERROR、同批未确认页读旧数据、重启后读零、deferred 只报一次）；
- `/speed/ftl_rw`：chunk=8/16/32 写 **3329/3179/3208 KB/s**、读 3379/3330/3386 KB/s，
  `rd_path=cache`、`err=0`；FC 上下限同镜像：写 3639 normal / 4281 cache、读 3500/3557。
- debug -O0 同用例 chunk=32 = 1222 KB/s（旧值 142 KB/s、`wr_ms=180001`）⇒ 早回执 + 读覆盖层
  在真实主机形态下成立，release 档相对 debug 提速约 2.6×。

⚠ 隔离运行时的 `arm=last`「重启后读零」失败已定位并修复：media 套件因 `s_media_ready`
（进程级 static）**跳过格式化**，介质被先跑的 `/fc-nand/lifecycle/` 原始块测试改写。
已改为每条 `test` 命令复位一次（`RISK-UT-08`）；本次全量运行的 `format_and_roundtrip`
10.28 s（release 格式化量级）证明重格式化生效，156/156 通过。

**仍未验**：H2testw 全盘、USB 端到端 deferred 注入、断电窗口量化（`RISK-CACHE-18/29`）。

**介质布局**：无变化（v13）。

---


## 4. 专题深挖

### 4.1 行业对标讨论摘要

#### 映射策略的行业主流

| 时期 | 主流策略 | 代表产品 | 原因 |
|------|---------|---------|------|
| ~2007 | 块映射 | 早期 U 盘、低端控制器 | SRAM 极度有限（几 KB~几十 KB）|
| 2007~2012 | 混合映射（BAST/FAST） | 中低端 SSD、U 盘主控 | SRAM 增大（64~256KB），但 DRAM 仍罕见 |
| 2015~至今 | 纯页映射 | **所有现代 SSD**（企业/消费/UFS） | SRAM/DRAM 降价，硬件条件满足 |

纯页映射是绝对主流。块映射和混合映射已被淘汰，核心原因：
- QLC/TLC 的有限 P/E 寿命无法承受 BAST/FAST 的 merge 开销
- DRAM/HMB 普及，表空间不再是限制因素

#### DRAM-less vs 有 DRAM

| 类型 | 做法 | 经典产品 |
|------|------|---------|
| 有 DRAM 企业级 | 全表常驻 DRAM | Samsung PM9A3、Solidigm D7 |
| 有 DRAM 消费级 | 全表常驻 DRAM | Samsung 990 PRO、WD SN850X |
| DRAM-less 消费级 | HMB 借用主机内存 | WD SN580、Phison E21T |
| DRAM-less 嵌入式 | NAND + SRAM cache（本方案）| UFS 4.0、eMMC 5.1 |

**本方案定位**：DRAM-less 嵌入式，与 UFS 旗舰主控架构一致。

---

### 4.2 CLOCK 替换策略升级方向

当前选择 CLOCK 算法作为替换策略。以下为已识别的优化方向，标记为**未来升级**，当前不实现：

| 升级方向 | 说明 | 触发条件 | 预期收益 |
|---------|------|---------|---------|
| **CLOCK-pro** | access_count 从固定位宽改为动态阈值，热数据更易留在 cache 中 | Phase 3 性能优化 | 命中率 +3~5% |
| **自适应模式切换** | 顺序访问时 prefetch 预加载后续 region，随机访问回到 CLOCK | Sequential 检测 | 顺序读场景命中率接近 100% |
| **Thrashing 降级** | 命中率低于 10% 时切换 write-through + 停 background flush | miss_rate > 90% | 避免性能断崖 |

---

### 4.3 各版本对比

| 维度 | v0.1（初始实现） | v0.2（当前设计） |
|------|-----------------|-----------------|
| 映射粒度 | 页面级（2048B） | CW 级（512B） |
| 地址编码 | `ssd_ppa_t`（uint64_t） | `nand_paa_t`（uint32_t） |
| 表存储 | SDRAM | **NAND（DRAM-less）** |
| L2P 缓存 | 无（全表在 SDRAM） | SRAM region cache（64~128 reg） |
| 替换策略 | 无 | CLOCK |
| FTL → NAND | 直接 nand_* | **通过 FC 接口** |
| 数据保护 | 无 | CRC + Random + ECC |
| 掉电恢复 | 无 | Journal + Checkpoint |
| GC | 桩（#warning） | 贪心 + 同步搬移 |
| Allocator | `next_free_ppa++` | Free list + 磨损均衡 |
| Block 类型 | 未区分 | 元数据/数据/空闲/坏块 |
| Modules | 9 个桩 | 完整接口定义 |
| RAID | 无（CONFIG=0） | 无（CONFIG=0） |

---

### 4.4 介质布局版本（`FTL_MEDIA_LAYOUT_VERSION`）演进表

当前值 = `FTL_MEDIA_LAYOUT_VERSION_BASE`（13）+ `(FTL_DIM_TOTAL > 1 ? 1 : 0)`。
任何改变**介质格式或块布局**的改动（结构体字段 / 块号 / 记录格式 / 字段位序语义）
都必须把 BASE +1；版本不符或缺失 ⇒ 首启自动全片格式化。

| 版本 | 变更 | 为什么必须 +1 |
|---:|---|---|
| v0.62 | 元数据页完整性改由 FC 的 per-CW CRC + ECC 承担，FTL 记录内 CRC 字段全部删除 | Checkpoint / Anchor / BBT / Wear 结构体尺寸变化 |
| v0.65 | Wear 条目增加 rr 健康字段（`last_ecc` / `rising_count` / `health`，8 B → 12 B） | 老格式 Wear 页 magic 仍匹配、条目布局不同，不拦会把计数读成垃圾 |
| v0.73 | Directory 条目 4 字节位置字段由 PAA 改存 PBA（字节数不变） | 字段位序语义变化；老介质读回的 PAA 会被当 PBA 解出错误块/页 |
| v0.74 | OP / Spare 池改按 plane 预留（每 plane 204 OP + 50 Spare） | 预留块集合变化；老固件把 plane 1 同号块当用户数据，新固件当已擦除预留池 ⇒ 账本与活数据冲突 |
| v6 | Checkpoint record 1 页 → 5 页（Header + 块元数据快照 4 页），Header 增加 `bit_*` 字段 | 记录格式与块元数据恢复方式变化 |
| v7 | 删除 L2P 哨兵层（原固定分区 0..15）；未映射改由 Directory 条目 `NAND_PBA_INVALID` 表示；固定分区整体前移，`FTL_METADATA_BLOCK_END` 63 → 47 | 块布局变化 |
| v8 | Anchor / Checkpoint / BBT 改 P0+P1 跨 plane 镜像；Journal 保持页号奇偶交错；换块擦除统一 multi-plane | 块的写语义与恢复路径变化 |
| v9 | 删除独立 Wear 块（块 46）；erase/read/ECC 健康并入 Checkpoint（块元数据 4 页 + 磨损快照 16 页），record 5 页 → 21 页 | 块布局与记录格式变化 |
| v10 | 新增固定预留的 L2P 日志区（46..77/plane，32 块/plane）；`FTL_METADATA_AREA_END` 46 → 78；主机容量 437 → 429 MiB | 块布局变化；老固件会把 46..77 当用户数据/动态日志块 |
| v11 | 新增 shared metadata spare（78..81/plane）；固定元数据分区改为「候选块池（跳过 BAD + spare 补缺）」；`FTL_METADATA_AREA_END` 78 → 82；主机容量上限 429 → 428 MiB；新增 `REBUILD_REQUIRED` BBT 标志（reserved[1]） | 块布局变化；老固件会把 78..81 当用户数据 |
| v12 | L2P region 主备双副本：日志区 32→48 块/plane（46..93），shared spare 94..97，`FTL_METADATA_AREA_END` 82 → 98；Anchor 记录 8→16 页（新增备份 PBA 半）；主机容量 428 → **424 MiB**；`lba_max=868,352` | 块布局与 Anchor 记录格式变化；老固件会把 78..93 当用户数据、且读不懂 16 页 Anchor |
| v13 | Journal 页头 12 → 16 B（新增 `base_lba`），新增顺序流映射页（`JSEG`，4 B 条目）与页型 magic | 老固件的 12 B 页头会把新页的条目区解析错位（magic 相同、条目起点不同）⇒ 回放错误映射 |
| v14 | Anchor 页头 20 → 24 B（新增 `dirty_gen`）：整条 record 的 16 页必须同 `sequence` 且同代数，否则整条作废（RISK-MD-06 混代记录）。**同时**：L2P region 主/备改为**每 plane 各一个当前块**（主→plane0、备→plane1），不再"同块连续两页"——冗余不变、每 plane 容量利用率翻倍 | Anchor 页头布局变化（老固件解不出新页头，新固件也读不懂旧页头）⇒ 首启自动格式化；日志区的块内布局变化对 Directory 不可见（仍按 PBA 读） |

## 5. 被否方案与已删除设计

> 只列当时明确比较过、但没有采用的方向；避免重新提议。

| 被否方案 | 当时选择 | 理由 | 记录 |
|---|---|---|---|
| 块映射 / 混合映射（BAST/FAST） | 纯 CW 页映射 | 写放大更小、与 FC CW 对齐；现代 SSD 主流 | v0.2 D1 |
| SDRAM / SRAM 全表驻留 | NAND 驻留（DRAM-less） | 学习嵌入式/UFS 主流，SRAM 预算不足 | v0.2 D2 |
| LRU / FIFO | CLOCK | 接近 LRU 命中率、无动态内存、O(1) | v0.2 D3 |
| Write-through / 纯 write-back | write-back + journal | 性能与掉电安全的折中 | v0.2 D4 |
| 2048B 页级 / 4096B 映射 | 512B（1 CW） | 与 FC CW USER 区完全对齐 | v0.2 D5 |
| FTL 直接调 `nand_*` | 经 FC 接口 | 完整 CRC/ECC/加扰保护 | v0.2 D6 |
| RAID | 不实现（`CONFIG_FC_RAID=0`） | 复杂度高，SLC + BCH 已足够 | v0.2 D7 |
| 单 block Super Block / 单温区 / 单 Free Pool / 0% OP | 跨 plane SB + 冷热 + 三级池 + 10% OP | GC 效率与坏块替换 | v0.2 D8–D11 |
| SLC cache | 不实现 | 本身就是 SLC NAND | v0.2 D12 |
| T2/T3 GC 触发 | 统一 `ftl_gc_evaluate` | 无出处触发、难以对照主流 | v0.63 |
| L2P 哨兵层 | Directory 条目 `NAND_PBA_INVALID` | 减少一层表示、避免两套坐标 | v0.79 |
| 独立 Wear 块 | Checkpoint 磨损快照 | 减少固定热点与寿命瓶颈 | v0.82 |

## 6. 旧名 → 现行名对照

> 历史条目按**当时**的名字记录，不回改；本节只做导航。

| 旧名 | 现行名 | 版本 |
|---|---|---|
| `ssd_ppa_t` | `nand_paa_t` | v0.2 |
| `cw_count[]` / `cws[][]` | `cw_bitmap`（flat 绝对编号） | v0.27 |
| `ftl_submit_read` / `ftl_submit_write` | `ftl_submit_*((ftl, req))` | v0.29 |
| 常驻 `valid_map` | 逐 CW 反查 META（approach ③） | v0.60 |
| `T2` / `T3` GC 触发 | `ftl_gc_evaluate` 统一求值 | v0.63 |
| `nand_addr_t` | PBA（`nand_pba_t`） | v0.73 |
| L2P 哨兵层 | Directory 条目 `NAND_PBA_INVALID` | v0.79 |
| 独立 Wear 块 | Checkpoint 磨损快照 | v0.82 |

## 7. 验证记录索引

> 只列标题或条目前部明确记录「上板验证」的版本；完整证据在各版本条目。

| 版本 | 日期 | 上板记录 |
|---|---|---|
| v0.39 | 2026-10-02 | F7 写失败 Super Block 关闭（行业坏块替换流程） |
| v0.46 | 2026-10-03 | B 档复查修复（B1 水位分子口径 + B4 死变量清理） |
| v0.50 | 2026-10-12 | P0-2 元数据写模型统一（追加日志 + 块池轮转） |
| v0.51 | 2026-10-12 | P1 算法档（GC victim 加权 + GC 触发补齐 + 读写判别修复） |
| v0.52 | 2026-10-12 | P1-3 Free Pool「三级水位」（原题「真三级化」的定案更正） |
| v0.53 | 2026-10-12 | P1-4 L2P cache thrashing 检测 + 降级（初稿三条降级只保留一条） |
| v0.54 | 2026-10-12 | P1-6 Journal 寿命瓶颈（环形块双倍擦除修复 + 条目合并） |
| v0.55 | 2026-10-12 | 逐页有效位图搬出 BST 条目（位图与条目的分离） |
| v0.71 | 2026-10-14 | `crash` 上板验证通过 + 一条未闭合的 HardFault 记录（纯文档） |
| v1.00 | 2026-10-25 | cache read 判据修正（已上板） |
| v1.04 | 2026-10-31 | cache 序列改由 FTL 顺序流检测器驱动 + Journal 顺序映射页（设计 A；2026-10-31 上板验证） |
| v1.05 | 2026-10-31 | 设备序列跨 host 命令：volatile write cache + padding END（2026-10-31 上板验证） |
| v1.06 | 2026-10-31 | 读写序列跨物理 block（器件原文判据；2026-10-31 上板验证） |
| v1.07 | 2026-10-31 | 低 free 成对分配失败：per-plane 水位 + victim 平衡 + 错误语义（2026-10-31 上板验证：生路；漂移由 v1.08 修） |
| v1.08 | 2026-10-31 | L2P 区域写回补 WAL 次序（journal 先落盘；2026-10-31 上板验证） |
| v1.09 | 2026-10-31 | debug 注入 seam + 失败侧 UT（2026-10-31 上板验证） |

## 8. 维护规则

- **当前设计进 `../ftl_design.md`，演进只进本文**（H37）。设计文档不写版本号、日期、变更叙事。
- 历史事实不改写；后续更正写在新条目，并注明推翻了哪一条。
- 验证记录只写当时结论与证据位置；未上板写「未上板」，不因后续通过而追认。
- 新条目必须包含：内容 / 决策 / 拒绝方案 / 验证；被否方案同步进 §5。
- 旧符号按当时名字保留；新增旧名→现行名映射进 §6。
