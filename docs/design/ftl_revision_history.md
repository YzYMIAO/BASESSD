# FTL 设计变更历史

> 本文档记录 FTL 模块设计过程中的关键讨论、决策反转、方案对比和选型理由。
> 本文档与 `fc_nand_design.md` 顶部的变更历史（Phase A~N）风格一致，用于追溯 FTL 设计的演进过程。

---

## v0.1 — 初始实现（Phase Unknown）

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

## v0.2 — 完整 DRAM-less 设计

**日期**：2026-09-23

### 讨论内容

基于 FC 层的完整设计文档 `fc_nand_design.md` 和项目现有代码结构，以 FTL 层为蓝本进行完整设计。

### 关键决策

| 序号 | 决策 | 选择 | 拒绝方案 | 理由 |
|------|------|------|---------|------|
| **D1** | 映射策略 | **纯 CW 级（512B）页映射** | 块映射、混合映射（BAST/FAST） | 写放大小、行业标准、FC 对齐 |
| **D2** | L2P 存储 | **NAND（DRAM-less）** | SDRAM 全表（8MB）、SRAM 全表 | 学习嵌入式/UFS 主流，有 1MB SRAM 可做 cache |
| **D3** | 替换策略 | **CLOCK** | LRU（链表复杂）、FIFO（命中率低） | 接近 LRU 命中率、无动态内存、O(1) |
| **D4** | 写回策略 | **Write-back + Journal** | Write-through（WA 高）、纯 Write-back（掉电风险） | 性能与掉电安全折中 |
| **D5** | 映射粒度 | **512B（1 CW）** | 2048B（页面级）、4096B | 与 FC CW USER 区完全对齐 |
| **D6** | FTL → NAND | **通过 FC 接口** | 直接 nand_* 调用（现状） | 完整数据保护 |
| **D7** | RAID | **不实现** | CONFIG_FC_RAID=1 开启 | 复杂度高，SLC 暂不需要 |

### 地址编码决策

- FTL 使用 FC 的 `nand_paa_t`（uint32_t）格式
- 放弃原有 `ssd_ppa_t`（uint64_t 线性页号）
- PAA 布局：`[CW:2][PAGE:6][PLANE:1][BLOCK:11]` = 20 bit 有效
- `cmd->paa` 填 CW=0 基址，FC 内部自动生成 desc 链

### 关于 RAID 的讨论

RAID 功能因实现复杂度过高，在当前 SLC 阶段不实现。具体不确定点：
- Stripe RAID 与 GC 的交互（parity block 如何参与 GC）
- 跨 Plane 错误的报告语义（数据命令成功、parity 失败时如何通知 FTL）
- parity block 的分配管理（allocator 能否分配 parity plane 的 block）

**结论**：RAID 延后，已有 ECC（BCH t=4）在 SLC 上可提供足够保护。

---

## 行业对标讨论摘要

### 映射策略的行业主流

| 时期 | 主流策略 | 代表产品 | 原因 |
|------|---------|---------|------|
| ~2007 | 块映射 | 早期 U 盘、低端控制器 | SRAM 极度有限（几 KB~几十 KB）|
| 2007~2012 | 混合映射（BAST/FAST） | 中低端 SSD、U 盘主控 | SRAM 增大（64~256KB），但 DRAM 仍罕见 |
| 2015~至今 | 纯页映射 | **所有现代 SSD**（企业/消费/UFS） | SRAM/DRAM 降价，硬件条件满足 |

纯页映射是绝对主流。块映射和混合映射已被淘汰，核心原因：
- QLC/TLC 的有限 P/E 寿命无法承受 BAST/FAST 的 merge 开销
- DRAM/HMB 普及，表空间不再是限制因素

### DRAM-less vs 有 DRAM

| 类型 | 做法 | 经典产品 |
|------|------|---------|
| 有 DRAM 企业级 | 全表常驻 DRAM | Samsung PM9A3、Solidigm D7 |
| 有 DRAM 消费级 | 全表常驻 DRAM | Samsung 990 PRO、WD SN850X |
| DRAM-less 消费级 | HMB 借用主机内存 | WD SN580、Phison E21T |
| DRAM-less 嵌入式 | NAND + SRAM cache（本方案）| UFS 4.0、eMMC 5.1 |

**本方案定位**：DRAM-less 嵌入式，与 UFS 旗舰主控架构一致。

---

## 未解决问题记录

以下问题在设计讨论中已识别但未最终决定，留待后续 Phase 实施时确定：

| # | 问题 | 影响 | 当前倾向 | 最终决策 |
|---|------|------|---------|---------|
| U1 | Region 固定 vs 动态 block | 元数据 block 磨损 | 固定 32 blocks | ⏳ |
| U2 | Journal 自身可靠性 | 掉电时 journal 损坏 | 双页 + CRC | ⏳ |
| U3 | 启动加载策略 | 启动时间 | 按需 miss | ⏳ |
| U4 | L2P 表磨损均衡 | 元数据 block 寿命 | region 更新合并 | ⏳ |
| U5 | 多温区支持 | GC 效率 | 暂不实现，预留接口 | ⏳ |

---

## CLOCK 替换策略升级方向

当前选择 CLOCK 算法作为替换策略。以下为已识别的优化方向，标记为**未来升级**，当前不实现：

| 升级方向 | 说明 | 触发条件 | 预期收益 |
|---------|------|---------|---------|
| **CLOCK-pro** | access_count 从固定位宽改为动态阈值，热数据更易留在 cache 中 | Phase 3 性能优化 | 命中率 +3~5% |
| **自适应模式切换** | 顺序访问时 prefetch 预加载后续 region，随机访问回到 CLOCK | Sequential 检测 | 顺序读场景命中率接近 100% |
| **Thrashing 降级** | 命中率低于 10% 时切换 write-through + 停 background flush | miss_rate > 90% | 避免性能断崖 |

---

## 各版本对比

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