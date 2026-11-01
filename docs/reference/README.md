# 资料区（docs/reference）

配套文档：**[docs/design/storage_controller_learning.md](../design/storage_controller_learning.md)**（学习路线）。
本目录是那份文档 §2「资源地图」与 §附录 的**离线备份**——把链接变成能直接翻的本地文件，
免得日后原文站失效/改版。

判据始终是本仓 **`docs/design/fc_nand_design.md` §26 分层**（HIL / FTL / FCC(硬件) / DRAM
+ 队列 + 门铃 + 完成），**不是 Linux、不是通用 OS 编程**。所以下面每份资料的「价值」一栏，
都是问同一句话：**它能不能帮我把 FC 的分层/队列/数据面做得更像真主控？**

---

## 目录结构

```
docs/reference/
├── README.md          ← 本文件
├── papers/            ← 论文 PDF（7 篇）
└── pages/             ← 网页存档（9 个 .html）
```

---

## 一、已备份清单

### 论文（`papers/`）

| 文件 | 标题 / 出处 |
|---|---|
| `FEMU_FAST18.pdf` | FEMU: A Firmware-based Emulator for Fast and Accurate SSD Emulation（FAST '18） |
| `MQSim_FAST18.pdf` | MQSim: A Framework for Enabling Realistic Studies of Modern Multi-Queue SSD Devices（FAST '18） |
| `SimpleSSD.pdf` | SimpleSSD: Modeling Solid State Drives for Holistic System Simulation（arXiv:1705.06419） |
| `Amber.pdf` | Amber: Enabling Precise Full-System Simulation with Detailed Modeling of All SSD Resources（arXiv:1811.01544） |
| `LeaFTL.pdf` | LeaFTL: A Learning-Based Flash Translation Layer for Solid-State Drives（arXiv:2301.00072） |
| `SoftSSD_FITEE2023.pdf` | 基于 SoftSSD 的快速固态硬盘固件原型开发（FITEE 2023，中文，7 页） |
| `zns-tools.pdf` | zns-tools 配套论文（ZNS 工具链） |

### 外部 datasheet（未本地存档）

| 来源 | 用途 |
|---|---|
| [Micron `MT29F4G08` 4Gb/8Gb/16Gb x8 NAND datasheet（Rev. N）](https://pdf.ttic.cc/pdfdet/MT29F4G08ABADAWP-IT:D_1288386_1288386_55.html#subpage) | Two-Plane Program Page Cache（Figure 74）与状态寄存器 **FAILC（bit1，上一页 cache program）** 语义；本仓 `include/nand/onfi_1_0.h` 的 op44/op48/op49 依据。datasheet 原文未随仓备份，引用处见 `docs/design/fc_nand_design.md` §17.5。 |
| 同上，"Read Cache Operations" / "Two-Plane Read Cache Operations" | **31h 可跨 block（同 die）、禁止跨 die/LUN**；two-plane 31h 每个 plane 各前进一页。原文摘录已本地存档：`docs/reference/datasheet_mt29f_cache_ops.md`（R12 判据出处）。 |

### 外部规范（未本地存档）

说「主控 / 主机协议怎么做」时从这里引，**不凭记忆**（R12）。

| 来源 | 原文要点 | 本仓用到的地方 |
|---|---|---|
| [T10 SBC-3 草案 03-388r1](https://t10.org/ftp/t10/document.03/03-388r1.pdf) §5.26 Table 6/7（WRITE(10)） | CDB **byte 1 位 3 = FUA**（位 7:5 WRPROTECT、位 4 DPO、位 1 FUA_NV）。**FUA=1** 时「设备不得在逻辑块真正写到介质之前返回 GOOD」；FUA=0 可写缓存即返回，后续错误走 deferred error | `known_risks.md` RISK-USB-05（本仓**有意忽略** FUA：靠主机 fsync 的 SYNCHRONIZE CACHE 做持久性屏障；此条只记录被忽略位的语义） |
| [NVM Express NVM Command Set Specification（1.0e / 1.1）](https://nvmexpress.org/wp-content/uploads/NVM-Express-NVM-Command-Set-Specification-1.0e-2024.07.29-Ratified.pdf) | ① Identify Namespace 的 **DLFEAT**（bits 2:0）规定**去分配 / 未写过 LBA** 的读值：`000b` = 全 0h 或全 FFh（实现自选）、`001b` = 全 0h、`010b` = 全 FFh；② Error Recovery feature（FID 05h）**CDW11 bit 16 = DULBE**（Deallocated or Unwritten Logical Block Error Enable，默认 0）：置 1 时读去分配区间**报错**。⇒ 主流是**命名空间级策略**，不是 per-LBA 持久丢失标记 | `docs/design/known_risks.md` RISK-CACHE-28（本仓重启后失败 LBA 读零 = DLFEAT `001b` 行为） |

### 网页存档（`pages/`）

| 文件 | 来源 |
|---|---|
| `spdk_ftl.html` | SPDK Flash Translation Layer 官方文档（spdk.io/doc/ftl.html） |
| `zonedstorage_ecosystem.html` | Linux Zoned Storage Ecosystem（zonedstorage.io） |
| `zonefs.html` | ZoneFS 内核文档（docs.kernel.org/filesystems/zonefs.html） |
| `opencores_nand_controller.html` | OpenCores NAND Controller 工程页（websvn listing） |
| `cosmos_openssd_index.html` | Cosmos+ OpenSSD 文档首页 |
| `cosmos_firmware_firmware.html` | Cosmos+ 固件代码页（薄，见下） |
| `cosmos_firmware_software.html` | Cosmos+ 主机端软件层页（薄） |
| `cosmos_hardware_hardware.html` | Cosmos+ 主机端硬件层页（薄） |
| `cosmos_hardware_nvme.html` | Cosmos+ 硬件代码页（薄） |

---

## 二、逐份分析

按五类分。**价值**一栏直接对标本仓北极星（FC 分层 + 队列/门铃/完成）。

### A. 开源主控 / 固件（最贴近「主控固件长什么样」）

#### A1. Cosmos+ OpenSSD（`cosmos_*.html`）

- **是什么**：韩国高校主导的开源 **NVMe SSD 控制器平台**——真实 FPGA 板 + 一套裸机固件。
  是「拿真硬件做 SSD 固件」最正统的开源样本。
- **对本项目的价值**：**分层形态的对照物**。它的固件里 host 接口 / FTL / NAND 三层的
  边界怎么切、命令怎么从主机下发到 NAND，可以直接比对本仓 `fc_nand_design.md` §26 的
  HIL/FTL/FCC 划分。**注意它是 NVMe 路径**，本项目是自研 FC 路径，接口不同、分层可借鉴。
- ⚠ **存档质量**：官方文档站是**空壳**——抓下来的 4 个子页正文各只有 ~850 字，基本是导航
  框架，实质内容全在 **GitHub 仓库代码**里（本目录未克隆仓库）。想读实质内容得看源码，
  不是看文档。
- **怎么读**：把它当「另一种分层切法」看，别当规范看。

#### A2. OpenSSD Jasmine（**未备份**，见 §三）

- **是什么**：OpenSSD 的**纯软件 FTL 参考实现**，GPLv3，含 Greedy / FASTer / DAC / Tutorial
  四套 FTL 算法。
- **对本项目的价值**：**FTL 算法侧的现成教材**——GC 策略、地址映射（页级/块级/混合）的实现
  范式。本仓 FTL 是 DRAM-less + CLOCK + journal 路线，Jasmine 是传统 DRAM 型，正好做**取舍
  对照**（它为什么敢常驻大表，你为什么不敢）。
- **为什么没备份**：文档站在 Vercel 机器人墙后面，curl 与 WebFetch 都取不到（见 §三）。

#### A3. SoftSSD（`SoftSSD_FITEE2023.pdf`）

- **是什么**：**软件定义的 SSD 固件原型**——把固件做成可快速迭代的原型框架（中文论文，
  7 页，好读）。
- **对本项目的价值**：**最像本仓的一份**。「用软件原型做固件、跑在真实 NAND 上」的动机与
  方法，和本仓 soft_ssd 的思路高度重合。看它怎么组织固件分层、怎么定义「原型」的边界。
- **怎么读**：先读这份，它最短、最贴题。

#### A4. SPDK FTL（`spdk_ftl.html`）——**已备份，内容最实**

- **是什么**：Intel **CSAL（Cloud Storage Acceleration Layer）** 的核心组件，一个
  **open-channel / 大 indirection unit 设备**上的 4K 块设备 FTL。术语表写得很清楚：
  - **L2P**（逻辑→物理映射）：0..surface 块数，**spare 块在 device formation 时从可用地址
    空间里扣掉**，专供 GC 缓冲；
  - 提到 **L2P 占 DRAM 量**（<16TiB 用 4B/LBA，更大用 8B/LBA）——这正是**本仓 DRAM-less
    路线要对抗的东西**；
  - **P2L / nvcache / chunk / recovery** 等机制。
- **对本项目的价值**：**L2P 表设计 + GC 缓冲的工业级参照**。本仓 FTL 的核心矛盾就是
  「L2P 表放不进 DRAM」，SPDK FTL 把 L2P 内存占用明码标价，是绝佳的反面对照——**看它花了
  多少 DRAM，就知道本仓省下的是什么**。
- **怎么读**：读术语表 + recovery 段，对照本仓 `docs/design/ftl_design.md`。

### B. 仿真器 / 模拟器（验证设计、跑实验，不碰真硬件）

> 这一类对**本仓当前阶段价值最低**——本仓已在真实 STM32H743 + MT29F4G08 上跑。
> 但它们是**「主控内部怎么建模」的知识源**：时序模型、并行度模型、队列模型。

#### B1. FEMU（`FEMU_FAST18.pdf`）
- **是什么**：**基于 QEMU 的固件级 SSD 仿真器**（FAST '18）。支持 blackbox / whitebox，
  NVMe、ZNS 等。ConfZNS 就是建在它上面的。
- **价值**：看它如何把 **NAND 时序、通道/way 并行、队列**抽象成可配置模型——本仓 NAND
  引擎的时序约束（tR/tPROG/tERASE）建模可以参考它的参数化方式。

#### B2. MQSim（`MQSim_FAST18.pdf`）
- **是什么**：CMU-SAFARI 的 **多队列 SSD 仿真框架**（FAST '18），C++/MIT，NAND 时序模型
  非常细。
- **价值**：**队列/调度**的建模参照。本仓有 SQ/HSQ/CQ + 门铃 + 完成，MQSim 的 multi-queue
  仲裁模型可对照——尤其「高优先级队列怎么抢低优先级」这块。

#### B3. SimpleSSD（`SimpleSSD.pdf`）
- **是什么**：**全系统 SSD 仿真**（arXiv:1705.06419），FTL + NAND 模型一体。
- **价值**：FTL 与 NAND 模型的分层接口设计。

#### B4. Amber（`Amber.pdf`）
- **是什么**：**全系统仿真中精确建模 SSD 全部资源**（arXiv:1811.01544）。
- **价值**：比 SimpleSSD 更细的资源建模——DRAM/控制器/后端。看它「SSD 内部资源」清单，
  可当本仓 FC 资源账（命令池/描述符池/buffer 池）的**穷举对照表**。

#### B5. ConfZNS（**未备份**，见 §三）
- **是什么**：建在 FEMU 上的 **ZNS SSD 仿真器**（SYSTOR '23），特色是 **zone→PU 的多种
  映射**（SU/MU/FU-zone），用多逻辑时钟建模 PU 争用，对真机 ~8% 误差。
- **价值**：ZNS 方向；对本仓（传统 NAND FTL）参考价值有限。

### C. 真实硬件 RTL（看主控「硬件那一半」怎么写）

#### C1. OpenCores ONFI NAND Controller（`opencores_nand_controller.html`）
- **是什么**：**VHDL 的 ONFI NAND 控制器 IP**（OpenCores 工程）。
- **价值**：**本仓 FCC（硬件）那一层的 RTL 对照**。§26 说 FC 要「有对应硬件寄存器表」——
  这份 RTL 就是「真硬件 FCC 长什么样」的样本：命令寄存器、状态寄存器、数据 FIFO 怎么摆。
  ⚠ 注意是 **ONFI 控制器**（管时序/ECC 的物理层），不含 FTL。
- **怎么读**：拿它的寄存器表比对本仓 FC 对外函数——正是 §26.3 的判据动作。

#### C2. NandFlashController（**未备份**，见 §三）
- **是什么**：`cjhonlyone/NandFlashController`，**Verilog + AXI** 的 NAND 控制器。
- **价值**：与 C1 同类（真硬件 FCC 的 RTL 样本），但**接口是 AXI**——看一个 NAND 控制器怎么
  挂上片上总线、寄存器/命令怎么经总线访问，可比对本仓 FC 与 CPU 的交互形态。

#### C3. BlueFlash（**未备份**，MIT/BlueDBM，见 §三）
- **是什么**：MIT CSAIL **BlueDBM**（分布式闪存存储、面向大数据分析）里的**可重构闪存控制器**。
  硬件：Xilinx **VC707** 载板（Virtex-7，走 PCIe）+ **FMC** 接口 + 闪存板上一颗**独立
  Artix-7** 专做闪存控制器（用 GTP 高速串行链路连载板，避免 NAND 总线直接上 FMC 的引脚/
  信号完整性问题）。NAND 组织成 **8 总线 × 8 芯片/总线 × 4 die/封装 = 512GB**，两级并行：
  **bus 并行**（独立总线并行寻址，带宽近线性）与 **chip 并行**（交错访问掩藏延迟）。用
  **Bluespec**（高层 HDL）开发。
- **对本项目的价值**：**分层哲学的正面教材，直击 §26 的 FCC/FTL 边界**。BlueFlash 明确把
  **总线/芯片调度、ECC 留在闪存控制器里**（= 本仓的 FCC 硬件层），把 **GC、磨损均衡上移到
  文件系统**（= FTL 层）——**而不是做成透明 FTL**。这正是 §26「哪些进硬件、哪些归固件」这个
  问题的一个已被真实系统选定的答案。另外它「bus 并行 vs chip 并行」的取舍（为什么选 8 总线）
  对本仓通道拓扑（`ch=1`）是很好的对照——**看它为什么敢上 8 条总线、本仓为什么只跑 1 条**。
- **怎么读**：重点读它的**功能划分**段落（谁在控制器、谁在软件），不是读它的 PCIe 细节。
- **为什么没备份**：MIT DSpace 的 PDF 被 **AWS WAF 人机验证**拦截（见 §三）。

#### C4. OCOWFC（Open-Channel Open-Way Flash Controller，**未备份**）

- **是什么**：开源 FPGA NAND 控制器（FPL 2021，`yhqiu16/OCOWFC`），支持 channel / way / plane
  交织、**cache-mode pipelining**、per-way 异步队列，以及**双级硬件命令调度器**（上层提高
  multi-plane / cache-mode 利用率，下层细粒度交错覆盖 busy 期）。
- **价值**：**本仓 FC 控制器侧的形态对照**——尤其「cache 序列怎么进队列、怎么和普通命令交错」。
  本仓的 `chain_owner` + 填充窗口是它在小 FIFO / 小池下的简化版。
- **URL**：<https://github.com/yhqiu16/OCOWFC>；论文
  <https://ieeexplore.ieee.org/abstract/document/9556350>
- ⚠ **未备份**：Verilog RTL，需按需 clone；本文分析基于公开 README / 论文摘要。

### D. 小型 / 嵌入式 FTL（**最贴近本仓 DRAM-less 路线**）

#### D1. LeaFTL（`LeaFTL.pdf`）
- **是什么**：**基于学习的 FTL**（arXiv:2301.00072）——用学习的方法管理映射/GC，目标之一是
  **降低 DRAM 占用**。
- **价值**：**DRAM-less / 小内存 FTL 的前沿思路**。本仓 FTL 表放不进 DRAM，LeaFTL 用「学」
  压缩映射开销——是「不靠大表怎么办」的一种答案，可作思路启发（未必能直接抄）。

#### D2. SPIFTL / dhara（**未备份**，见 §三）
- **是什么**：面向 **SPI-NOR / 小容量 raw flash** 的**极简 FTL**（dhara 是 `dlbeer/dhara`，
  块级映射、日志式；上游不活跃）。
- **价值**：**极简 FTL 的范式**——没有 DRAM、没有复杂 GC，只有磨损均衡 + 坏块 + 日志。
  本仓 FTL 虽大得多，但 dhara 的「小到极致」能帮你想清楚**哪些机制是必需的、哪些是奢侈的**。

#### D3. Linux MTD UBI 的静态磨损均衡（**未备份**，见 §三）
- **是什么**：Linux 内核 UBI 卷管理层（`drivers/mtd/ubi/`）。`wl.c` 的静态 WL：
  取**擦除最少的 used PEB**（e1），从 Free Pool 取擦除显著更高的 PEB（e2），当
  `e2->ec - e1->ec >= ubi->wl_threshold` 时把 e1 的数据搬到 e2；`Kconfig` 的
  `MTD_UBI_WL_THRESHOLD` 默认 **4096**（最高与最低擦除计数的最大差值）。
- **价值**：本仓静态 WL 的**判据出处**（`docs/design/ftl_design.md` §3.4 /
  `RISK-WL-08`）：磨损差触发 + 「低擦除 → 高擦除」方向。与 dhara 的完美 WL 属
  **不同分岔**：UBI 面向大容量映射 FTL，dhara 面向小容量 raw flash。
- **URL**：
  <https://raw.githubusercontent.com/torvalds/linux/master/drivers/mtd/ubi/wl.c>、
  <https://raw.githubusercontent.com/torvalds/linux/master/drivers/mtd/ubi/Kconfig>

### E. 协议 / 接口规范（判据依据）

#### E1. ONFI（**未备份**，见 §三）
- 器件命令集/时序的**权威来源**。⚠ 本仓记忆有教训：**别凭记忆说「协议要求」**
  （见记忆 `no-unverified-protocol-claims`、`onfi-1-0-no-multi-plane`）——查 ONFI 原文。

#### E2. Zoned Storage 生态（`zonedstorage_ecosystem.html` + `zonefs.html`）
- **是什么**：ZNS / SMR 的**主机侧软件栈**——内核 4.10 起支持，含驱动层/文件系统层/device
  mapper 层。zonefs 是把 zoned 设备当文件系统用的内核模块。
- **价值**：**主机侧接口的对照**。本仓是自研 HIL（主机接口层），ZNS 展示了「主机与设备之间
  的顺序写约束怎么用软件栈表达」——对本仓 HIL 的**约束表达**有参照意义（不是照抄）。

#### E3. zns-tools（`zns-tools.pdf`）
- ZNS 工具链论文，偏实验工具，参考价值有限。

### F. 命令序列 / 驱动实现（cache read / program 的展开）

#### F1. Linux MTD rawnand 的顺序 cache read（**未备份**）

- **是什么**：Linux 主线 `drivers/mtd/nand/raw` 的 sequential cache read——一次
  `00h-addr-30h` 之后，后续页用 `31h`（READ CACHE SEQUENTIAL），末页 `3Fh`（READ CACHE END）；
  提交 `003fe4b9545b83cca4f7a7633695d1f69a0b0011` 的 `nand_lp_exec_cont_read_page_op()`。
- **价值**：**cache read 序列的开源展开样本**。它的分工是「上层知道这次读多少页，再展开成一条
  序列」——与本仓「FTL 决定 run、FC 执行」一致。
- **不覆盖**：driver 层一次多页读，没有本仓的 per-LUN owner / 开放窗口（硬件控制器队列深）。
- **URL**：<https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=003fe4b9545b83cca4f7a7633695d1f69a0b0011>
  （镜像：<https://git.embedded.cherry.de/puma-linux.git/commit/drivers/mtd/nand/raw?id=003fe4b9545b83cca4f7a7633695d1f69a0b0011>）

#### F2. cache program 的开源空缺（如实登记）

- 公开结果只确认 Linux 定义了 `NAND_CMD_CACHEDPROG 0x15`；**没有完整的 49h/44h/48h 开源序列实现**。
- **权威依据**仍是 Micron MTG Table 18 / Figure 74 的 FAILC 语义（`E1` 与外部 datasheet）；
  本仓的增量确认（第 i+1 条确认第 i 页）是**本仓自拟**，不是行业标准形态。

---

## 三、未能下载（如实登记）

> **被拦截的只做分析，不强行抓取**（用户 2026-10-12 明确）。下表各项已在 §二 里有对应
> 的分析条目——分析基于**公开摘要 / 官方文档 / 检索结果**，而非一手 PDF；需要原文时按「备选」
> 自行获取。

| 目标 | 原因 | 备选 |
|---|---|---|
| `BlueFlash_MIT.pdf` | MIT DSpace 被 **AWS WAF 人机验证**拦截，curl 只拿到 HTML 验证页 | 需浏览器手动下载；或找会议论文集镜像 |
| `ConfZNS`（SYSTOR '23） | **ACM 付费墙**；作者实验室站 `embedded.dankook.ac.kr` 直连 **HTTP 000**（网络不通） | ACM DL 有账号时下；或用后继工作 **ConfZNS++**（atlarge-research.com，可访问） |
| `jasmine_software_components.html` | deepwiki 文档站在 **Vercel 机器人墙**后，curl 拿到 34K 的 "Vercel Security Checkpoint" 页、**真实内容 0 匹配**；WebFetch 报 "stream has been aborted" | 读 GitHub 仓库 `openssd/jasmine` 源码（GPLv3） |
| `NandFlashController`（Verilog/AXI） | 本轮未抓（GitHub 仓库，非「文档」） | 直接 clone `cjhonlyone/NandFlashController` |
| Linux UBI `wl.c` / `Kconfig` | 本轮未备份（单独源码文件，引用为主） | 按 D3 的 raw URL 直接读；或查本机内核源码 `drivers/mtd/ubi/` |

> **说明**：GitHub 代码仓库（Cosmos+/Jasmine/SoftSSD/FEMU/MQSim/ConfZNS/NandFlashController/
> SPIFTL/dhara/zns-tools）**未克隆**——克隆会把嵌套 `.git` 塞进本仓，污染版本库。
> 需要时按学习文档 §附录 的 URL 单独 clone 到仓外。

---

## 四、怎么用这些资料

别通读。按学习文档 §4 的三种读法取用：

1. **结构对照**：Cosmos+/SoftSSD/SPDK FTL 的**分层**，比对本仓 `fc_nand_design.md` §26。
2. **取舍对照**：Jasmine/SPDK FTL 的 **DRAM 型 L2P**，对照本仓 **DRAM-less** 路线——
   问「它敢花这块内存，我为什么不敢」。
3. **反例对照**：LeaFTL/SPIFTL/dhara 的**小内存 FTL**，问「哪些机制可以砍」。

判据回到学习文档 §5 的五条。**当前最大缺口是 HIL**（主机接口层）——本仓尚无，E2（Zoned
Storage）与 A1/A2（Cosmos+/Jasmine 的 host 接口）是补这块的主要参照。
