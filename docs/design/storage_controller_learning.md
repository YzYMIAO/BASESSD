# 如何学习存储主控设计

> 本文承接「有哪些有价值的 FTL / 存储主控开源项目」那一问：把那份调研**重新按「学到哪一层」归类**，
> 再给出**分阶段学习路线**、**用开源项目的对照方法**、以及**「学会没有」的判据**。
>
> 锚点是本仓 [`fc_nand_design.md`](fc_nand_design.md) §26 的行业分层判据（HIL / FTL / FCC(硬件) / DRAM
> + 队列 + 门铃 + 完成）。**读别人的代码之前，先有自己的一套判据** —— 否则只会抄到形，抄不到为什么。

---

## 0. 怎么用这篇文档

| 你现在的状态 | 从这里看 |
|---|---|
| 想知道「主控设计到底要学什么」 | §1 知识地图 |
| 想知道「读哪个开源项目」 | §2 资源地图（按层归类，不是按项目类型） |
| 想知道「按什么顺序学」 | §3 分阶段路线 |
| 已经在读别人的代码了 | §4 对照方法（怎么读才有用） |
| 想自检进度 | §5 判据 + §6 你现在的坐标 |

**一句话原则**：主控设计的知识 = **分层边界** + **层间接口** + **每层的经典取舍**。
学任何东西，都问「它属于哪一层 / 层间接口是什么 / 它做了什么取舍」。

---

## 1. 知识地图：主控设计在学什么

### 1.1 按 §26 的分层拆

```
   Host
    │  NVMe SQ/CQ + doorbell + MSI-X（或 eMMC CMDQ / UFS）
    ▼
 ┌─────────────┐  ── 学什么 ──────────────────────────────────────
 │  HIL        │  协议解析 · 队列管理 · 门铃响应 · 中断 · 描述符(PRP/SGL)
 ├─────────────┤
 │  FTL        │  L2P 映射 · GC · 磨损均衡 · 坏块管理 · 掉电恢复 · 缓冲调度
 ├─────────────┤
 │  FCC（硬件）│  通道/CE/LUN/plane 时序 · 描述符链执行 · ECC · 加扰 · DMA
 ├─────────────┤
 │  DRAM       │  映射表驻留 · 数据缓冲 · cache 算法（CLOCK/LRU）· DRAM-less 兜底
 └─────────────┘
    │  ONFI / toggle 总线
    ▼
  NAND die ── channel → chip → die → plane → block → page → CW
```

**每一层的核心问题**（自测：能不能不看资料答出来）

| 层 | 核心问题 |
|---|---|
| 器件 | die/plane/block/page/CW 的层级与容量怎么算？SLC/TLC/QLC 差在哪？读干扰/保持性是什么？ |
| ONFI | 命令集（读/写/擦/状态/feature）、地址周期、时序模式、R/B# 语义、multi-plane 从哪来？ |
| FCC | 一条命令怎么变成总线上的时序？描述符链怎么执行？ECC 什么时候编/解？加扰解决什么问题？ |
| FTL | L2P 怎么组织（页级/块级/混合/DFTL）？GC 选谁当 victim？掉电后怎么重建映射？ |
| HIL | 主机怎么把请求交给设备？门铃/完成/中断三者怎么配合？描述符怎么指数据缓冲？ |
| 系统 | 中断还是轮询？裸机还是 RTOS？队列深度怎么定？QoS 怎么保证？ |

### 1.2 三条主线（贯穿所有层）

| 主线 | 内容 | 本仓对应 |
|---|---|---|
| **数据面** | 一次读写的数据从 host 缓冲 → FC 缓冲 → 加扰 → ECC → NAND，再原路返回 | `src/fc/fc_data_path.c` |
| **控制面** | 请求排队 → 调度 → 下发起步 → 完成回收 | `src/fc/fc.c` 的 `fc_service()` / SQ/CQ / 三池 |
| **可靠性面** | ECC 纠错、CRC 校验、RAID 恢复、坏块替换、掉电恢复 | `fc_ecc_engine` / `ftl/` journal+checkpoint |

**学会的标准**：能把任意一个功能**归到「哪条主线的哪一层」**，并说清它跟上下层的接口。

### 1.3 主控固件 ≠ 应用/OS 编程（先破一个误区）

主控固件是**裸机 / RTOS + 固定向量表**，没有 `malloc`、没有 `request_irq`、没有运行期注册：

- 中断向量**编译期固定**（向量表在 startup 里），不能运行期挂 handler
- 内存**静态分配**（池子 + 固定数组），没有动态堆
- 并发靠**状态机 + 轮询**，不是线程/锁

> ⚠ 这条决定了很多「往 OS 靠」的做法在主控语境里是**反向**的。判据始终是 §26，不是 Linux 怎么写。

---

## 2. 资源地图（按「学到哪一层」归类）

### 2.1 真控制器固件平台 —— 学「完整分层长什么样」

| 项目 | 覆盖层 | 学到什么 | 状态 |
|---|---|---|---|
| **Cosmos+ OpenSSD** | HIL+FTL+FCC+DRAM 全栈 | `host_lld.c`=HIL（NVMe 队列/门铃/DMA）· `address_translation.c`=FTL · `request_schedule/allocation.c`=调度 · `garbage_collection.c`=GC。**唯一能对照你 §26 完整分层的代码** | 活跃 |
| **OpenSSD Jasmine** | FTL 为主 | 四种 FTL 参考实现：**Greedy**（页级+Greedy GC，GC 祖师爷）· **FASTer**（混合）· **DAC**（冷热分离）· Tutorial | 老（2011，GPLv3）但经典 |
| **SoftSSD** | HIL+FCC（软件化） | 大部分组件在 ARM 上跑软件、只留 PCIe PHY+NAND 接口在 FPGA；事件驱动模型，可用调试器看内部状态 | 同名项目，值得读论文 |
| **OpenExpress / BlueFlash** | FCC 硬件化 | 全硬件自动化 NVMe 框架 / 可重构 flash 控制器（MIT） | 研究平台 |

### 2.2 生产级用户态 FTL —— 学「现代工程取舍」

**SPDK `spdk_ftl`**（CSAL 的核心，真在生产里跑）。它和你正好构成对照：

| 维度 | SPDK FTL | 本仓 |
|---|---|---|
| L2P 驻留 | 只缓存最近用的 2 GiB，其余换出到 cache 设备 | **DRAM-less**：SRAM 数组 + NAND 兜底 |
| 元数据 | **P2L 存在每个 band 尾部**，按 sequence ID 顺序读回重建 L2P | journal + checkpoint + BBT |
| 写组织 | 1 GiB band，顺序写（默认 1 MiB 单元） | 块级/plane 级分配 |
| 状态机 | band `FREE → OPEN → CLOSING → CLOSED` | FTL 状态机 |

**这是最值得精读的现代 FTL**：它的每一个选择都能照出你 DRAM-less 路线的差异。

### 2.3 仿真 / 模拟器 —— 学「分层模型 + 设计空间」

| 项目 | 定位 | 对你的价值 |
|---|---|---|
| **SimpleSSD / Amber** | gem5 全系统，**HIL / FTL / PAL 分层** | **你的分层几乎就是它的翻版**（你有 HIL/FTL/FCC，它把硬件并行抽象成 PAL）——用它验证你的分层划分是否「标准」 |
| **FEMU** | QEMU 里模拟 NVMe SSD（真延时），含 FTL，支持 OpenChannel/ZNS | 想在 Linux 里端到端跑一遍 FTL 逻辑时的试验台 |
| **MQSim** | 多队列 NVMe 精确建模（误差 8–18%） | 队列/门铃/多流调度建模参考 |
| **ConfZNS / ConfZNS++** | 基于 FEMU 的 ZNS 模拟器，对真机验证过 | 往 ZNS 方向看时 |

### 2.4 NAND 控制器 RTL —— 学「硬 FCC 替你做了什么」

⚠ **「ONFI + Verilog + 开源 + 活跃」四者同时满足的极少**。

| 项目 | 语言/许可 | 备注 |
|---|---|---|
| **OpenCores NAND Controller** | VHDL / LGPL | 唯一明确 **ONFI compliant** 的完整开源控制器，有规格文档 + Avalon-MM |
| `cjhonlyone/NandFlashController` | Verilog / GPL-3.0 | AXI 接口、同步模式 |
| Lattice RD1055 / Xilinx `nandpsu` | 参考设计 / 驱动 | ⚠ Xilinx 的 RTL 专有，GitHub 上只有**裸机驱动** |

**你在 STM32 上做的是「软 FCC」**（FMC + 引擎状态机）——对比硬 FCC 能看清哪些事硬件替你做了、哪些是你的软件在补。

### 2.5 嵌入式小规模 FTL + 上层生态

- **`earlephilhower/SPIFTL`** —— MCU 级 SPI flash 静态磨损均衡。**省略了坏块管理/并行/保持性扫描** ⇒ 可当「最小 FTL 能省到什么程度」的反面教材
- **`dhara`**（`dlbeer/dhara`，MIT）—— 低内存系统的 NAND FTL。设计点很干净：**完美磨损均衡**（任意两块擦除次数差 ≤ 1）、`write()`/`trim()` **原子 + 掉电回滚到最后同步点**、**不占用 OOB**（全部留给 ECC）、启动与所有操作 **O(log n) 最坏**。⚠ 上游已停维护（最后提交 4 年多前）；`espressif/dhara` 是活跃的封装
- **F2FS** —— 文件系统层不是 FTL，但**分段 + GC + ZNS 感知**思路对 Allocator 有参考价值
- **ZNS 生态**：`libzbd` / `zonefs` / `zns-tools` / BTRFS / XFS
- ⚠ **Open-Channel SSD / LightNVM 已从 Linux 移除（5.15）**，被 ZNS 取代 —— 别再按 OCSSD 方向投入

### 2.6 规范与论文（一手资料，优先级最高）

| 类型 | 来源 |
|---|---|
| **器件规范** | ONFI 各版本（onfi.org）· JEDEC JESD230 · 具体型号 datasheet（如 Micron MT29F4G08） |
| **经典论文** | FAST / MSST / HotStorage / SYSTOR 的 SSD 相关论文；**DFTL**、**Greedy FTL** 原始论文 |
| **研究前沿** | `LeaFTL`（学习式 L2P，用线性回归预测映射）· FDP（Flexible Data Placement）· ZNS+ |

> **一手规范 > 论文 > 别人代码 > 二手博客**。任何「协议要求……」的说法，先回去查规范原文（本仓曾吃过凭记忆断言协议的亏）。

---

## 3. 分阶段学习路线

每一阶段给：**目标 / 读什么 / 动手 / 判据（怎么算过关）**。

### 阶段 0：地基 —— 器件与协议

- **目标**：彻底搞懂一块 NAND 芯片能做什么、不能做什么
- **读**：ONFI 规范（命令集 + 地址 + 时序）· 你手上型号的 datasheet（Table：命令集/地址/时序）
- **动手**：手算你的器件地址——channel/CE/LUN/plane/block/page 到总线地址的映射
- **判据**：能不看资料写出一次 page read 的完整命令序列（命令码 + 地址周期 + 等待 + 读数据）

### 阶段 1：把一块 NAND 驱动起来 —— FCC 的时序执行单元

- **目标**：能发命令、等 R/B#、搬数据，跑通 reset / read ID / read status
- **读**：OpenCores 的 ONFI 控制器（看硬件怎么排时序）· 你的 `src/nand/`
- **动手**：微码/状态机式驱动 —— 一条命令 = 一串微操作（发命令 → 发地址 → 等 → 读）
- **判据**：能解释「为什么等 R/B# 要分 issue/wait/fetch 三段」，能读出 ONFI 参数页

### 阶段 2：控制面 —— 队列 + 门铃 + 完成

- **目标**：把「一次操作」升级成「一个命令系统」
- **读**：Cosmos+ 的 `host_lld.c`（NVMe 队列/门铃）· §26 的三种主机接口形态
- **动手**：命令池 + 描述符链 + SQ/CQ + 调度器；**先做对，再谈快**
- **判据**：能画出「一条命令从提交到完成的端到端生命周期」，说清每一步谁改哪个状态

### 阶段 3：数据面 —— ECC / 加扰 / 冗余 / 并发

- **目标**：数据在 NAND 上可靠地进、可靠地出
- **读**：ECC（BCH vs LDPC 的取舍）· 加扰为何能保证统计独立 · RAID/冗余
- **动手**：编解码路径（pack/unpack）· multi-plane · 多 LUN 并发（issue/wait/fetch）
- **判据**：能说清「数据在哪一步被加扰、哪一步被 ECC 保护、读回来怎么按逆序还原」

### 阶段 4：FTL —— 映射、回收、恢复

- **目标**：把裸 NAND 变成「逻辑块设备」
- **读**：SPDK FTL（现代取舍）· Jasmine Greedy FTL（GC 经典）· FTL 综述论文
- **动手**：L2P 映射 · GC · 磨损均衡 · 坏块管理 · **掉电恢复**（journal + checkpoint）
- **判据**：能说清「掉电后从哪几个来源重建完整映射表」，能设计一个 GC victim 选择策略并解释

### 阶段 5：向真主控靠 —— 形态、规模、性能

- **目标**：从「能跑」到「像真主控」
- **读**：Cosmos+ 全栈 · SimpleSSD 的 PAL · 多通道/多 CE 的架构
- **动手**：多通道准备（哪怕硬件不支持，软件架构先留好）· QoS/读饥饿保护 · 性能剖析
- **判据**：能对一个陌生主控设计**指出它缺了哪一层、哪条主线**

---

## 4. 怎么用开源项目（不是读代码，是做对照）

**不要从第一行读到最后一行。** 用三种对照动作，每个项目只取你要的那一点：

### 4.1 结构对照 —— 拿别人的分层图跟你的比

> 例：SimpleSSD 的 `HIL / FTL / PAL` vs 你的 `HIL / FTL / FCC`。
> PAL（Parallelism Abstraction Layer）在你这里是什么？——是 `nand_dispatch` 的多 LUN 相位机。
> **别人把它单独成层，说明它值得单独成层** —— 这正是你 §26 里 `nand_dispatch_request_t` 的位置。

### 4.2 取舍对照 —— 同一问题的不同选择

> 例：L2P 映射表放哪？
> - SPDK FTL：DRAM 缓存 2 GiB + cache 设备换页
> - 你：**DRAM-less**，SRAM 数组 + NAND 兜底 + CLOCK 淘汰
> - Jasmine Greedy：全页级映射（简单但吃内存）
>
> **没有对错，只有约束下的取舍**。问：我的约束是什么？我的选择代价是什么？

### 4.3 反例对照 —— 别人的省略与限制

> 例：SPIFTL 明确**不做**坏块管理/并行/保持性扫描。
> 问：它为什么能省？因为 MCU + 小容量场景。**你的场景为什么不能省？** ——答不上来，说明你还没想清楚自己的约束。

---

## 5. 判据：怎么算「学会了」

| # | 判据 | 说明 |
|---|---|---|
| 1 | 能画出主控分层图并**说清每层边界** | 尤其能解释「为什么某功能在这一层」 |
| 2 | 能对一个接口做 §26 式审查 | 拿功能去比硬件寄存器表，有对应项则留、无则剔 |
| 3 | 能说出任意功能的**层间接口**（数据结构 + 方向） | 例：`nand_dispatch_request_t` 是 FC→NAND 的「已解析请求寄存器」 |
| 4 | 能对陌生设计**指出缺失** | 「这个设计没有独立的 GC 层」「这里把策略混进了机制」 |
| 5 | 能解释**硬件限制**下的退让 | 哪些是「不可逾越」，哪些是「退而求其次」 |

---

## 6. 你现在的坐标（用 §26 判据定位）

| 层 | 本仓现状 | 缺口 |
|---|---|---|
| 器件 / ONFI | ✅ 驱动跑通、ONFI 参数页解码、时序模式 | 更多器件特性（on-die ECC、多 plane cache op） |
| FCC | ✅ 三池 + desc 链 + 数据面（ECC/加扰/CRC）+ per-LUN FIFO | 多通道受硬件限制；DMA 数据路径 |
| FTL | ✅ L2P/GC/WL/Journal/Checkpoint/BBT（DRAM-less） | — |
| HIL | ⚠ UART CLI 为主 | **没有真主机协议**（NVMe/eMMC/UFS）——这是离真主控最远的一层 |
| 规模 | ⚠ 单通道单 CE 单 LUN | 多通道架构（软件已留口，硬件不支持） |

**下一步的三个候选**（按「向主控靠」的价值排序）：

1. **补 HIL 形态** —— 哪怕做一个最小的命令队列 + 门铃 + 完成的抽象（不必真 NVMe），把「主机如何提交请求」这一层显式化
2. **多通道软件架构落地** —— 把 `nand_dispatch` 的 per-LUN 相位机推广到 per-channel，验证软件是否真的准备好了
3. **性能与 QoS** —— 剖析数据面瓶颈，做读饥饿保护/调度公平性

---

### 6.1 项目定位（2026-10-29 评估，已归档）

> 结论：作为单人 MCU 软 SSD / 学习主控，明显高于一般嵌入式练手项目；作为开源 SSD
> 控制器与 OpenSSD / SPDK FTL 比，属于小规模、早期阶段。完整评估（仓内坐标、对照表、
> 强项/差距、三个坐标下的水平、下一步瓶颈）见
> `docs/archive/project_positioning_20261029.md`。

---

## 附录：资源清单（速查）

**真固件平台**
- Cosmos+ OpenSSD — https://github.com/Cosmos-OpenSSD/Cosmos-plus-OpenSSD （[文档](https://cosmos-plus-openssd-doc.readthedocs.io/en/latest/OpenSSD/OpenSSD.html)）
- OpenSSD Jasmine — https://github.com/openssd/jasmine （GPLv3，含 Greedy/FASTer/DAC/Tutorial FTL）
- SoftSSD — https://github.com/Jimx-/softssd （[论文 FITEE 2023](https://jzus.zju.edu.cn/article.php?doi=10.1631%2FFITEE.2200456)）

**生产级 FTL**
- SPDK FTL — https://spdk.io/doc/ftl.html

**仿真器**
- FEMU — https://github.com/vtess/FEMU （[FAST'18](https://www.usenix.org/system/files/conference/fast18/fast18-li.pdf)）
- MQSim — https://github.com/CMU-SAFARI/MQSim （C++/MIT，[FAST'18](https://www.usenix.org/system/files/conference/fast18/fast18-tavakkol.pdf)）
- SimpleSSD / Amber — [论文](https://arxiv.org/pdf/1705.06419) / [Amber](https://arxiv.org/pdf/1811.01544)
- ConfZNS — https://github.com/DKUStarLab/ConfZNS

**NAND 控制器 RTL**
- OpenCores ONFI NAND Controller — https://opencores.org/projects/nand_controller （VHDL）
- NandFlashController — https://github.com/cjhonlyone/NandFlashController （Verilog/AXI）
- OCOWFC（Open-Channel Open-Way Flash Controller）— https://github.com/yhqiu16/OCOWFC
  （FPL 2021；cache-mode pipelining + 双级调度器；论文 https://ieeexplore.ieee.org/abstract/document/9556350）

**NAND 命令序列（cache read/program 的展开）**
- Linux MTD rawnand sequential cache read（`31h`/`3Fh`，commit `003fe4b`）—
  https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=003fe4b9545b83cca4f7a7633695d1f69a0b0011
  （镜像 https://git.embedded.cherry.de/puma-linux.git/commit/drivers/mtd/nand/raw?id=003fe4b9545b83cca4f7a7633695d1f69a0b0011）
- ⚠ cache program（49h/44h/48h）**没有**找到完整开源实现；权威依据是 Micron MTG Table 18 /
  Figure 74 的 FAILC（见 `docs/reference/README.md` §F2）

**嵌入式 FTL**
- SPIFTL — https://github.com/earlephilhower/SPIFTL
- dhara — https://github.com/dlbeer/dhara

**规范 / 生态**
- ONFI — https://onfi.org/specifications/
- Zoned Storage — https://zonedstorage.io/docs/introduction/linux-ecosystem
- zns-tools — https://github.com/stonet-research/zns-tools

**研究前沿**
- LeaFTL — https://arxiv.org/pdf/2301.00072v1.pdf

---

*本文是学习路线图，不替代 [`fc_nand_design.md`](fc_nand_design.md)（本项目设计）与 [`ftl_design.md`](ftl_design.md)（FTL 设计）。*
