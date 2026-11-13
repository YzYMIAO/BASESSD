# 项目定位与水平评估（对照公开 SSD 项目）

> **性质**：评估文档，不是验证报告。仓内事实来自代码 / 设计文档 / 板端实测记录；
> 与公开项目的对照基于公开资料，**未在本仓做基准复现**。
> **日期**：2026-10-29
> **结论随代码演进会变**；若后续接入 NVMe/SATA、多通道或 LDPC 软判决，需重估。

---

## 〇、一句话结论

**作为「单人 MCU 软 SSD / 学习存储主控」项目，它明显高于一般嵌入式练手项目；
但作为「开源 SSD 控制器」去和 OpenSSD、SPDK FTL 比，它属于小规模、早期阶段——
不是一个量级的东西。**

---

## 一、项目坐标（仓内事实）

| 项 | 本仓 | 依据 |
|---|---|---|
| 平台 | STM32H743（Cortex-M7，无 MMU / OS） | `CLAUDE.md` §〇 / `docs/hardware_contract.md` |
| 介质 | MT29F4G08，SLC，512 MiB 裸容量 / 424 MiB 用户容量 | `include/nand/devices/mt29f___g08a.h`、`include/ftl/ftl.h:83` |
| 拓扑 | 1 CH × 1 CE × 1 LUN，2 plane，64 页/块，2048 块/plane | `mt29f___g08a.h:35-37`、`ftl_config.h:22-26` |
| 主机接口 | USB FS 12 Mbps，实测约 843/889 KB/s | `docs/design/usb_msc_udisk.md`、`docs/design/testing_guide.md` |
| ECC | BCH(m=13, t=4) 默认；QC-LDPC 硬判决实验性 | `include/fc/fc_ecc_engine.h:14-29` |
| RAID | `CONFIG_FC_RAID=0`，未启用 | `include/fc/fc.h:51-52` |
| 掉电保护 | Journal + Checkpoint + 版本化；**无电容**，acked 写容忍丢最后一页 | `docs/design/ftl_design.md` §7 |
| 代码量 | FTL + FC 约 4 万行（不含平台 / USB / 测试） | `wc -l src/ftl src/fc include/ftl include/fc` |
| 架构 | FC-NAND（队列 / 描述符链 / ECC / CRC / 加扰）+ FTL（DRAM-less、CW 级 L2P、GC/WL/BBT/PFR/RR） | `docs/design/fc_nand_design.md`、`docs/design/ftl_design.md` |

---

## 二、与公开项目的对照

对照对象是**公开、可查**的 SSD 相关项目，不是把本仓说成同类产品：

- **OpenSSD / Cosmos+ OpenSSD**：FPGA 控制器 + 固件的研究/教学平台，含 SATA/NVMe、多通道。
- **SPDK FTL**：用户态软件 FTL，跑在 NVMe SSD 上，DRAM 型、偏高吞吐。
- （其他如 OpenChannelSSD、Dhara、Linux MTD FTL / UBI 属于不同抽象层，不直接对标。）

| 维度 | 本仓 | OpenSSD 类 | SPDK FTL | 说明 |
|---|---|---|---|---|
| 形态 | MCU 固件 | FPGA 控制器 + 固件 | 用户态软件 FTL | 不是同一类产品 |
| 主机接口 | USB FS ~1 MB/s | SATA/NVMe | NVMe | 差 3 个数量级 |
| 并行度 | 1 通道 / 1 die | 多通道、多 die、多 plane | 多队列、多核 | 本仓基本没有通道级并行 |
| DRAM | 无（DRAM-less） | 有 | 有 | 本仓 L2P / 元数据全在 SRAM |
| FTL 深度 | CW 级映射 + Journal + CP + PFR + 双副本 Directory + GC/WL/BBT/RR | 完整 FTL，偏控制器固件骨架 | 成熟 L2P / GC，偏性能 | 本仓在恢复与 L2P 细节上不弱，但规模与验证远小 |
| ECC | BCH t=4；LDPC 硬判决实验 | 可换 ECC 方案 | 依赖底层盘 | 无 LDPC 软判决 / read retry 闭环 |
| 掉电 | Journal + CP + 版本化；无电容 | 通常有掉电保护设计 | 依赖盘 | 设计诚实，但不是企业级掉电保护 |
| 测试 | 板端 UT（85 次运行）、munit、风险登记、断言规范 | 社区/研究使用 | 成熟 CI / 压测 | 本仓无 CI、无覆盖率、无 fuzz / 长稳 |
| 文档 | 设计 / 风险 / 学习笔记齐全 | 论文 + 文档 | 完善 | 单人项目中文档投入很重 |
| 社区 / 工程化 | 单人、无 CI、无发布流程 | 有社区 / 论文 / 硬件 | 有社区 / CI | 不是同一种工程成熟度 |

> 表中 OpenSSD / SPDK FTL 的描述是公开项目的**一般形态**，不是逐项基准；
> 若要做严格对比，需要固定主机、介质、队列深度与负载后重跑。

### 2.1 对照来源

- Cosmos+ OpenSSD 文档首页：`docs/reference/pages/cosmos_openssd_index.html`
  （说明见 `docs/reference/README.md` §A1）
- SPDK FTL 官方文档：`docs/reference/pages/spdk_ftl.html`
  （说明见 `docs/reference/README.md` §A4）
- 分层 / 资源地图：`docs/design/storage_controller_learning.md` §2 与资源地图
- 本仓的分层判据：`docs/design/fc_nand_design.md` §26
  （HIL / FTL / FCC / DRAM + 队列·门铃·完成模型）

---

## 三、实事求是的强项

1. **端到端真机跑通**：NAND 时序 → FC → FTL → USB 主机，不是仿真。
2. **DRAM-less + CW 级映射 + 反向映射**（页内 META + L2P 反查），思路接近现代消费级
   DRAM-less 主控，而不是课本 FTL。
3. **恢复链路完整**：Journal 回放、Checkpoint、块元数据快照、双副本 Directory、
   掉电恢复、读恢复、坏块表、静态 WL——很多单人项目只做 L2P + GC。
4. **工程纪律**：风险登记（`known_risks.md`）、断言分级（`assertion_guidelines.md`）、
   板端 UT、变更史与设计文档。这部分高于绝大多数 hobby 项目。
5. **诚实的自我边界**：`usb_msc_udisk.md` 明确写了 USB FS 与 PCIe 的差距，
   没有把 424 MiB / 1 MB/s 说成 SSD 级。

---

## 四、实事求是的差距

1. **性能 / 并行**：1 通道、USB FS、无 DRAM、无多 die 交织——与开源 SSD 控制器差几个
   数量级。这是平台决定的，不是代码水平问题。
2. **ECC / 可靠性闭环**：BCH t=4、LDPC 硬判决实验性、RAID 关闭、无软判决 / read retry
   闭环；达不到企业级 NAND 管理。
3. **规模**：424 MiB、单 die；没有多通道调度、NVMe 队列、namespace、host-managed 接口。
4. **工程成熟度**：无 CI、无覆盖率、无 fuzz / 长稳 / 温度 / 寿命测试；单人维护。
5. **通用性**：绑定 STM32H743 + MT29F4G08；平台抽象存在，但未验证第二平台。

---

## 五、三个坐标下的水平

| 坐标 | 判断 |
|---|---|
| **学习 / 作品** | 单人 MCU 存储项目里的上游：独立完成 FC + FTL + 掉电恢复 + GC/WL + 板端 UT |
| **开源 SSD 控制器** | 早期 / 小规模：与 OpenSSD（FPGA + NVMe/SATA + 多通道）不在一个量级；与 SPDK FTL 差在性能、验证、社区 |
| **生产固件** | 远未达到：缺电容掉电保护、LDPC 软判决、RAID、多通道、长稳 / 寿命验证 |

一句话：**它不是能对标 OpenSSD / SPDK 的开源 SSD 项目，而是「把主控关键路径真正走通
并讲清楚」的高质量学习型项目。**

---

## 六、下一步瓶颈（若目标是开源影响力）

按投入产出排序，**不是**继续打磨注释或 bool/enum：

1. **主机接口**：USB HS 或 NVMe/SATA——当前 USB FS 是最大硬上限。
2. **并行度**：多通道 / 多 die 交织、队列深度，否则吞吐无法上来。
3. **ECC 闭环**：LDPC 软判决 + read retry + RAID 启用与验证。
4. **可复现工程**：CI、覆盖率、长稳 / 掉电循环 / 寿命测试、可复现基准脚本。

---

## 七、结论的可复核性

- **仓内事实**（平台、几何、容量、接口速率、ECC/RAID 开关、代码量）均给出文件定位，
  可直接复核。
- **对照结论**是定性评估；公开项目的具体性能数字未在本仓复现。
- 若后续改动触及接口 / 并行度 / ECC，本文的「坐标」表需同步更新（H6）。
