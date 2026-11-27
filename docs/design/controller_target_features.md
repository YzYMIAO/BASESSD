# 存储主控 SoC 介绍

> **状态：活跃设计文档（学习地图 + 选型判据），不是产品手册。**
> 读者：想通过一个端到端项目学习存储主控设计的人。
> 边界：先讲主流主控 SoC 的硬件与 FTL/固件功能块，再用本仓 STM32H743 平台做案例；
> 下一平台选择方法在 §7，候选数据在附录。具体芯片能力以数据手册 / 官方 BSP 为准。
>
> 依据：`docs/design/storage_controller_design.md` §一~§九，
> `docs/reference/README.md` A1（Cosmos+/NVMe）、A4（SPDK FTL）、D1/D2（DRAM-less / 小 FTL）；
> 当前平台见 `README.md` / `docs/hardware_contract.md`。

## 0. 摘要

本文是一份面向学习者的**存储主控 SoC 设计地图**：先讲主控硬件功能块
（主机接口、CPU、SRAM/DRAM、FC、NAND 接口、ECC、DMA、电源、调试、可靠性），
再讲 FTL/固件如何用这些硬件（映射、GC/WL、掉电恢复、OP/WAF、写读路径、TRIM、
调度、telemetry、安全、升级、E2E、数据放置、ZNS/FDP），最后用
STM32H743 + MT29F4G08 的现成开发板做案例，并给出下一平台的选择方法。
目标不是产品手册，而是建立「主流主控怎么做、为什么、本仓在哪、差在哪」的判断力。

**核心结论：**

1. 主控 = **主机接口 + HIL/FTL/FC + 介质**；分层、队列/门铃/完成、数据面轮询、
   通道并行、ECC/LDPC、掉电恢复是主线。
2. 消费级 vs 企业级不是功能清单，而是**约束强度**：稳态/P99.9、DWPD、PLP、QoS、
   可靠性；很多功能两边都有，差在要求。
3. 当前平台是「**现成板卡 + 低成本 + 不能自绘 PCB**」下的学习载体：DRAM-less、
   USB MSC、单 NAND/单 LUN/2 plane、PIO、BCH；很多硬件不符合主控，这是
   被迫约束 + 主动取舍的结果。
4. 这块板能学 HIL/FTL/软件 FC/掉电恢复/板端验证；学不到 PCIe/NVMe 队列、
   多通道、硬 FC/DMA、LDPC、PLP、ZNS/FDP。
5. 下一平台按**主控物理硬件块**倒推：硬条件 = 现成板卡、PCIe EP 已引出、
   NAND 可接；低端首选 Zynq-7000（需核对），只学 FTL/NVMe 选 i.MX 8M，
   完整多核 + FPGA 再上 UltraScale+/PolarFire。

**全文速览：**

| 节 | 内容 | 回答的问题 |
|---|---|---|
| §1 | 主控是什么、分几层 | 整体图 |
| §2 | 主机接口：链路 / 控制器 / 协议 | PCIe/NVMe/SATA/UFS 怎么配 |
| §3 | SoC 硬件功能块 | 主控里有哪些硬件、两边差在哪 |
| §4 | FTL/固件功能块 | 软件如何用这些硬件 |
| §5 | 三类市场与选型综合 | 需求如何变成架构分岔 |
| §6 | 当前平台案例 | 这块板怎么选、差距在哪、能学什么 |
| §7 | 下一平台选择 | 按硬件块怎么选、买前核对什么 |
| §8 | FTL 架构选择（ADR） | 为什么这样选、主要替代是什么 |
| §9 | FTL 模块主流对照 | 主流怎么做、本仓差在哪 |
| 附录 | 候选平台数据 | 各候选的原始对照与待核实项 |

**边界：**

- 不是产品手册；典型值 / 区间以数据手册和官方 BSP 为准。
- 当前平台证据对应 2026-11-20 镜像；此后工作区改动尚未重新上板。
- 候选平台的「待核实」项必须查原理图 / 数据手册，不能靠型号推断。

**阅读建议**：第一次读按 §1 → §2 → §3 → §4 → §5 → §6 → §7；
做选型可直接看 §5.6、§6.3、§7.2~§7.5。

## 1. 存储主控 SoC 是什么

### 1.1 一句话

主控 = **翻译机**（主机协议 ↔ 内部请求） + **调度器**（队列/门铃/完成） +
**可靠性引擎**（ECC/RAID/恢复） + **映射管理器**（FTL 把逻辑地址翻译成物理地址）。

### 1.2 分层图

```text
Host  ── PCIe / SATA / SAS / UFS / eMMC / USB ──┐
                                                 │ ① 主机接口（PHY + 控制器 + 协议 + 队列）
 ┌───────────────────────────────────────────────▼──────────────────────────────┐
 │ HIL  主机接口层    协议解析 · 队列管理 · 门铃响应 · 中断 · 描述符(PRP/SGL)      │
 ├──────────────────────────────────────────────────────────────────────────────┤
 │ FTL  闪存转换层    L2P 映射 · GC · WL · 坏块管理 · 掉电恢复 · 缓冲调度          │  ② 固件
 ├──────────────────────────────────────────────────────────────────────────────┤
 │ FC  闪存通道控制器 通道/CE/LUN/plane 时序 · 描述符链 · ECC · 加扰 · CRC · DMA │  ③ 硬件（或软 FC）
 ├──────────────────────────────────────────────────────────────────────────────┤
 │ DRAM/SRAM         映射表驻留 · 命令队列 · 数据缓冲（DRAM-less 时 SRAM+NAND 兜底）│
 └───────────────────────────────────────────────┬──────────────────────────────┘
        ONFI / Toggle 总线  ─────────────────────┘  ④ 器件总线
                    NAND die ── channel → CE → LUN/die → plane → block → page → CW
```

### 1.3 三条主线

| 主线 | 内容 | 一句话 |
|---|---|---|
| 数据面 | host 缓冲 → FC 缓冲 → 加扰 → ECC → NAND → 原路返回 | 数据怎么走 |
| 控制面 | 请求排队 → 调度 → 下发起步 → 完成回收 | 命令怎么排 |
| 可靠性面 | ECC 纠错 · CRC 校验 · RAID 恢复 · 坏块替换 · 掉电恢复 | 出错怎么救 |

### 1.4 主控固件 ≠ 应用/OS 编程

主控固件通常是**裸机 / RTOS + 固定向量表**：

- 中断向量编译期固定；
- 内存静态分配（池子 + 固定数组），没有动态堆；
- 并发靠状态机 + 轮询，不是线程/锁。

这决定了很多「往 Linux/OS 靠」的做法在主控语境里是反向的（例：运行期 `request_irq`、`malloc`）。
判据始终是硬件 FC 形态，不是 Linux 怎么写。

### 1.5 层职责

| 层 | 一句话职责 | 核心决策 |
|---|---|---|
| 器件 | 存数据、会磨损 | SLC/MLC/TLC/QLC、几何、可靠性 |
| NAND 接口 | 主控与 die 之间的语言 | ONFI/Toggle、命令集、时序、并行 |
| FC | 把「一条命令」变成「总线上的时序」 | 拓扑并行、描述符链、ECC/加扰/CRC/DMA |
| FTL | 把「裸 NAND」变成「逻辑块设备」 | 映射、GC、WL、坏块、掉电恢复 |
| HIL | 把「主机请求」变成「设备内部请求」 | 协议、队列、门铃、完成、描述符 |
| 调度/QoS | 决定「谁先走、谁等多久」 | 仲裁、读饥饿、尾延迟、后台协同 |
| 可靠性 | 保证端到端不丢不坏 | ECC/RAID/副本/端到端 CRC |

## 2. 主机接口：链路、控制器、协议

### 2.1 四层，不要混

| 层 | 例子 | 在 SSD 里的角色 |
|---|---|---|
| 物理/链路层（PHY/link） | PCIe、SATA、SAS、UFS（M-PHY + UniPro）、eMMC、USB | 电气、串行/并行、链路训练、速率/lane |
| 主机控制器 | PCIe controller、SATA controller（AHCI）、SAS controller、UFS controller、USB controller | 队列、命令搬运、中断、DMA |
| 命令/队列协议 | NVMe、AHCI + ATA、SCSI、UFS UTP + SCSI、MMC、USB MSC/UASP | 命令语义、队列、完成、描述符 |
| 描述符/DMA | PRP/SGL（NVMe）、PRD（AHCI）、UPIU（UFS） | 数据缓冲怎么指 |

### 2.2 配对关系

```text
PCIe  + NVMe        = NVMe SSD（消费级/企业级）
SATA  + AHCI + ATA  = SATA SSD（消费级/老平台）
SAS   + SCSI        = SAS SSD（企业级）
UFS   + UTP + SCSI  = UFS（移动）
eMMC  + MMC         = eMMC（移动/嵌入式）
USB   + MSC/UASP    = U 盘/移动硬盘
```

**NVMe 不是 PHY**，它是跑在 PCIe 上的命令/队列协议；SATA 不是命令集，AHCI 是主机控制器接口，
ATA/SCSI 才是命令；UFS 是一整套栈（M-PHY + UniPro + UTP + SCSI）。

### 2.3 消费级 / 企业级 / 移动

| 场景 | 链路 | 命令/队列 | 备注 |
|---|---|---|---|
| 消费级 SATA SSD | SATA | AHCI + ATA | 老平台、低成本 |
| 消费级 NVMe SSD | PCIe Gen3/4 x4 | NVMe | 当前主流 |
| 企业级 NVMe | PCIe Gen4/5 x4/x8，双端口 | NVMe | 稳态性能、QoS、PLP（power loss protection，掉电保护） |
| 企业级 SAS | SAS 12/24Gb/s | SCSI | 双端口、热插拔 |
| 移动 | UFS（M-PHY/UniPro） | UTP + SCSI | 低功耗、小尺寸 |
| 移动/嵌入式 | eMMC | MMC | 低成本 |
| 便携 | USB | MSC/UASP | U 盘/移动硬盘 |
| **本仓当前平台** | **USB FS** | **MSC BOT** | 不是 NVMe/PCIe/SATA/UFS |

### 2.4 选型含义

- 做 **NVMe**：SoC 必须有 **PCIe Endpoint + PCIe controller**；NVMe 队列/PRP 可以软件或 FPGA 实现；
  ⚠ SoC 有 PCIe 不等于能用：**板卡必须引出 lane/REFCLK/PERST#**，且控制器支持 EP 模式；
- 做 **SATA**：SoC 要有 SATA controller/AHCI，或 FPGA 自己写 SATA PHY/链路（很难）；
- 做 **UFS**：需要 M-PHY + UniPro + UTP，通用 SoC 基本不具备，不建议作为第一块板；
- 做 **USB**：当前板已经验证，但学习价值低于 NVMe/PCIe。

## 3. SoC 硬件功能块（逐个）

> 每块按「职责 / 消费级 / 企业级 / 当前平台 / 设计取舍 / 学习点」看。
> 「当前平台」= STM32H743 + MT29F4G08（阿波罗 V2 核心板）。

### 3.1 主机 PHY / 链路

| 项 | 内容 |
|---|---|
| 职责 | 物理层/链路：SerDes、CDR、均衡、lane、速率、编码、电气/信号完整性；**通常是硬 IP** |
| 消费级 | PCIe Gen3/4 x4；部分 SATA |
| 企业级 | PCIe Gen4/5 x4/x8；SAS；双端口；长背板/retimer |
| 当前平台 | **USB2_OTG_FS 全速 12 Mbps**；无 PCIe/SATA |
| 设计取舍 | 当前板只有 OTG_FS 引出；USB 可直连 PC，代价是没有 NVMe/PCIe 队列 |
| 学习点 | PHY ≠ 控制器 ≠ 协议；PHY 通常不可编程，能写的是控制器/NVMe/PL；SoC 需 EP 模式，板卡需引出 lane/REFCLK/PERST# |
| 细节 | `docs/design/hil_design.md`、`docs/design/usb_msc.md` |

### 3.2 主机控制器

| 项 | 内容 |
|---|---|
| 职责 | 链路/事务层、LTSSM、TLP/DLLP、配置空间、MSI-X、DMA、命令搬运 |
| 消费级 | PCIe controller 或 SATA/AHCI controller |
| 企业级 | 多端口、SR-IOV、更细粒度中断/队列；双端口是控制器/端口能力 |
| 当前平台 | USB OTG controller（MSC BOT）；无 PCIe 控制器 |
| 设计取舍 | 用 USB controller 换端到端可跑；代价是单命令、无门铃/多队列 |
| 学习点 | 控制器 vs PHY；队列/门铃/完成；EP/RC/SR-IOV 是控制器能力；FPGA 硬核/软核 |
| 细节 | `src/hil/usb_msc.c`、`docs/design/hil_design.md` |

#### 3.2.1 PCIe 控制器的三层

```text
Host 内存
  │  TLP（Memory Read/Write、Completion）
  ▼
[Transaction Layer]  ← TLP 组装/解析、tag、split transaction
  │
[Data Link Layer]    ← DLLP、ACK/NAK、CRC、流控 credit
  │
[Physical Logical]   ← LTSSM 链路训练、lane/速率协商
  │
[PHY]                ← SerDes/电气/均衡（硬 IP）
```

- **Transaction Layer**：把「读/写某个地址」变成 TLP；用 tag 配对 outstanding 请求与 Completion；
- **Data Link Layer**：ACK/NAK、CRC、重传、流控 credit，保证 TLP 可靠；
- **Physical Logical**：LTSSM 链路训练、速率/lane 协商；
- **PHY**：只负责电信号和 SerDes。

> 主机控制器 = 链路/事务层 + 配置空间 + DMA + 中断；不是 PHY，也不是 NVMe。

#### 3.2.2 RC vs EP：SSD 是 Endpoint

| 模式 | 角色 | 谁枚举谁 | SSD 能不能用 |
|---|---|---|---|
| Root Complex (RC) | 主机侧 | RC 枚举设备 | 不能当 SSD |
| Endpoint (EP) | 设备侧 | 被主机枚举 | **NVMe SSD 必须是 EP** |
| Dual-mode | 两者都行 | 按配置 | 可以，但要确认 |

SSD 作为 EP 需要：配置空间（Vendor/Device ID、Class Code、BAR）、BAR 映射门铃/寄存器、
MSI/MSI-X 中断、DMA 读写 host 内存里的 SQ/CQ 和数据缓冲。

**「有 PCIe PHY」不等于「能做 SSD」**：控制器必须支持 EP 模式。

#### 3.2.3 NVMe 控制器 vs PCIe 控制器

| 层 | 职责 |
|---|---|
| PCIe controller | TLP/DLLP、链路训练、配置空间、MSI-X、DMA |
| NVMe controller | SQ/CQ、doorbell、命令解析、PRP/SGL、namespace、完成 |

典型 NVMe 流程：

```text
Host 写 SQ entry（命令）
  → Host 写 doorbell（BAR 里）
  → NVMe controller 通过 DMA 读 SQ entry
  → 解析命令（读/写/FLUSH/TRIM…）
  → 用 PRP/SGL DMA 搬数据
  → 写 CQ entry（phase bit 翻转）
  → 发 MSI-X 中断
```

SQ/CQ 在 **host 内存**；doorbell 在 **控制器 BAR**；PCIe controller 负责搬运，
NVMe controller 负责命令语义。

#### 3.2.4 企业级：双端口 / SR-IOV / 多队列

| 能力 | 是什么 | 在哪一层 |
|---|---|---|
| 双端口 | 两个 PCIe 端口/控制器，两个主机可访问同一盘 | 控制器 + PHY |
| SR-IOV | PF/VF，多虚拟功能，各自 BAR/MSI-X | 控制器 |
| 多 namespace | 一个控制器多个逻辑命名空间 | NVMe controller |
| 多队列/MSI-X 向量 | 每队列独立中断，降低锁竞争 | 控制器 + NVMe |
| QoS/仲裁 | 读优先、尾延迟、租户隔离 | 控制器 + 固件 |

这些都不是 PHY 能力；选型看控制器规格，不是「有 PCIe 就行」。

#### 3.2.5 当前平台：USB OTG controller + HIL 软件队列

- 当前板是 **USB OTG controller + MSC BOT**；没有 PCIe 控制器。
- USB BOT 是**单命令**（CBW/CSW），没有 SQ/CQ/doorbell；
- HIL 用软件补上队列/完成职责：请求拆分、run 排队、完成聚合、host 回执；
- 代价：无多队列/中断聚合；好处：端到端可跑、PC 直连。

#### 3.2.6 选型检查清单

1. PCIe controller 是否支持 **EP**？
2. Gen 几、几 lane？x1 入门，x4 接近消费级 NVMe。
3. 是否支持 **MSI-X**？几个 function/vector？
4. 是否需要 **SR-IOV / 双端口**？
5. DMA/描述符能力：能否直接搬 SQ/CQ/PRP？
6. 板卡是否引出 PCIe lane/REFCLK/PERST#？
7. PCIe controller 是硬核还是 PL 软核？

### 3.3 命令 / 队列协议

| 项 | 内容 |
|---|---|
| 职责 | NVMe SQ/CQ、doorbell、PRP/SGL；AHCI+ATA；SCSI；UFS UTP；USB MSC/UASP |
| 消费级 | NVMe 或 AHCI+ATA |
| 企业级 | NVMe/SCSI，多 namespace、QoS |
| 当前平台 | **USB MSC BOT**（CBW/CSW，SCSI-like）；HIL 自己做请求拆分/排队/完成回执 |
| 设计取舍 | 不上 NVMe 队列，先把请求生命周期跑通；代价是无多队列/门铃 |
| 学习点 | 请求拆分、run、完成聚合、host 回执 |
| 细节 | `docs/design/hil_design.md`、`src/hil/ssd_request.c` |

#### 3.3.1 共同骨架：提交 / 完成 / 中断

```text
Host 提交命令 → 提交队列 → 设备取命令 → 执行 → 完成队列 → 中断
```

不同协议只是名字/结构不同：

| 协议 | 提交 | 完成 | 描述符 | 中断 |
|---|---|---|---|---|
| **NVMe** | SQ entry（64 B） | CQ entry（16 B）+ phase bit | PRP/SGL | MSI-X |
| **AHCI/ATA** | Command List + FIS | PxCI 清位 + 状态 | PRD | 中断 |
| **SCSI** | CDB | sense/status | 由传输层承载 | SAS/UFS 中断 |
| **UFS** | UPIU（UTP） | UPIU | UPIU | UFS 中断 |
| **USB BOT** | CBW + SCSI CDB | CSW | 无 | USB 中断 |
| **USB UASP** | 命令/数据/状态管道 | CSW/状态 | 流式队列 | USB 中断 |

#### 3.3.2 NVMe 的核心

- **Admin queue**（queue 0）+ **I/O queues**；
- **SQ/CQ 在 host 内存**，doorbell 在控制器 BAR；
- 命令 64 B、完成 16 B；**phase bit** 判断 CQ 新条目；
- **PRP/SGL** 指数据缓冲；
- 命令集：Read/Write/Flush/Trim/Identify/Get-Set Features…；
- 状态码 + 错误日志；
- 企业级追加：多 namespace、SR-IOV、QoS、telemetry。

#### 3.3.3 AHCI/ATA 与 SCSI/UFS

- **AHCI/ATA**：Command List（32 槽）+ Command Table（FIS + PRD）；主机写 PxCI 触发，
  设备完成后清位 + 中断；队列深度固定 32，比 NVMe 老一代。SATA SSD 常见
  AHCI + ATA，部分主机侧走 SAT（SCSI/ATA 转换）。
- **SCSI**：命令集（CDB + sense），跑在 SAS/UFS 上；SAS 强调双端口/热插拔。
- **UFS**：M-PHY + UniPro + UTP（UPIU）+ SCSI CDB；强调低功耗/小尺寸。

#### 3.3.4 USB BOT / UASP 与当前平台

- **BOT**：CBW（31 B，含 SCSI CDB）→ 数据 → CSW（13 B）；**单命令**；
- **UASP**：支持流式队列/多命令，比 BOT 快，但仍是 USB；
- 当前平台是 **BOT**，所以没有 SQ/CQ/doorbell；HIL 用软件补队列/完成职责：
  请求拆分（run）、排队、完成聚合、host 回执。

#### 3.3.5 消费级 / 企业级与选型含义

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 协议 | NVMe 或 AHCI/ATA | NVMe 或 SCSI/SAS |
| 队列 | 较少队列/深度 | 多队列、深队列、MSI-X 多向量 |
| 虚拟化 | 一般无 | SR-IOV、多 namespace |
| QoS | 基本 | 读优先、尾延迟、租户隔离 |
| 错误/telemetry | 基本 SMART | 完整 error log/telemetry |

选型含义：

- **NVMe**：需要 PCIe EP + NVMe controller；队列/PRP 可软件或 FPGA；
- **SATA**：需要 AHCI/SATA controller；
- **UFS**：需要 M-PHY/UniPro/UTP，通用 SoC 基本没有；
- **USB**：当前板路线，学习价值低于 NVMe。

### 3.4 CPU 子系统

| 项 | 内容 |
|---|---|
| 职责 | 跑 FTL、GC/WL、Journal/CP、调度、telemetry；处理中断/定时器 |
| 消费级 | 1~2 核 ARM/RISC-V |
| 企业级 | 多核 + 硬件加速（XOR/ECC/压缩/加密） |
| 当前平台 | Cortex-M7 400 MHz，单核；ICache/DCache on；MPU 12 区 |
| 设计取舍 | 单核足够跑通 FTL，但数据面 PIO + 后台 GC/CP 会 CPU-bound |
| 学习点 | 裸机/RTOS、静态分配、状态机、轮询 vs 中断 |
| 细节 | `docs/architecture.md`、`src/main.c` |

#### 3.4.1 CPU 在 SSD 里干什么

```text
Host 请求
  → HIL：解析/拆分/排队
  → FTL：L2P 查表/更新、GC/WL 调度
  → FC：建命令/描述符
  → NAND（硬件或 PIO）
  → 完成回收/telemetry
```

- HIL：命令解析、run 拆分、队列/完成管理；
- FTL：L2P 映射、cache、GC/WL、BBT；
- 元数据：Journal / Checkpoint / PFR（Power-Fail Recovery，掉电恢复）；
- 后台：GC、WL、read scrub、telemetry；
- 数据面：PIO 时 CPU 搬数据；DMA/硬 FC 时 CPU 只配描述符。

#### 3.4.2 CPU 子系统组成

| 部件 | 作用 | 对主控的影响 |
|---|---|---|
| Core(s) | ARM Cortex-M/R/A、RISC-V | 单核 vs 多核；SMP/AMP |
| Cache / TCM | I/D cache、ITCM/DTCM | 确定性 vs 吞吐；热代码/热数据 |
| SRAM | FTL 工作区、队列、缓冲 | L2P 表能否放下 |
| MPU / MMU | 内存保护/地址转换 | 裸机多用 MPU，无虚拟内存 |
| 中断控制器 | NVIC / GIC | ISR 延迟、优先级 |
| 定时器/看门狗 | SysTick、TIM、IWDG | 调度节拍、卡死检测 |
| DMA | 数据搬运 | 释放 CPU |
| 加速器 | XOR/RAID、ECC、CRC、压缩、加密 | 企业级差异大 |
| 调试 | SWD/JTAG、trace | 板端排障 |

#### 3.4.3 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 核数 | 1~2 核 | 4~16 核，或大小核 |
| 架构 | Cortex-M/R、RISC-V | Cortex-A/R、RISC-V |
| Cache/SRAM | 小 | 大，常配 DRAM |
| 加速器 | 少 | XOR/RAID、ECC、压缩、加密 |
| 并发 | 单核 superloop/RTOS | SMP/AMP、队列/核间通信 |
| QoS/telemetry | 基本 | 多队列、尾延迟、telemetry |

#### 3.4.4 当前平台：Cortex-M7 单核

- STM32H743IIT6：Cortex-M7 400 MHz，单核；2 MB Flash、1060 KB SRAM；
- ICache/DCache on，ITCM/DTCM，MPU 12 区；无 MMU；
- 裸机 superloop，HIL/FTL/FC/NAND 全在一个核上；
- 数据面 PIO，GC/CP/Journal 作为后台任务与 host I/O 竞争 CPU。

取舍：

- 好处：简单、确定、易调试；静态分配、状态机、轮询；
- 代价：CPU-bound；后台 GC/CP 会拖慢 host I/O；没有多核/硬件加速器；
- 学习点：小 MCU 上怎么跑完整 FTL；状态机/轮询/中断；cache/MPU/栈预算。

#### 3.4.5 多核 / FPGA 平台

- **AMP**：一个核跑 HIL/NVMe，一个核跑 FTL，一个核跑 GC/telemetry；共享内存 + mailbox；
  要处理 cache coherency、核间同步、队列归属。
- **SMP**：Linux/RTOS 多核；灵活但复杂，主控固件常用 AMP 而不是 SMP。
- **FPGA PS+PL**：PS 核跑固件，PL 写数据面/ECC/NVMe 队列；硬核 CPU 省资源，
  软核（MicroBlaze/RISC-V）灵活但占 PL。
- **硬件加速器**：XOR/RAID、ECC、CRC、压缩、加密；企业级把这些从 CPU 卸载。

#### 3.4.6 选型检查清单

1. 核数/架构：A9/A53/R5/RISC-V？单核够不够？
2. 主频、Cache、TCM、SRAM：L2P/队列放得下吗？
3. MMU/MPU、中断控制器、定时器/看门狗；
4. DMA/加速器：XOR/ECC/CRC/crypto 有没有？
5. 多核模型：AMP/SMP、共享内存/mailbox；
6. 工具链/RTOS/裸机支持；
7. FPGA 平台：PS 核 + PL，硬核/软核 CPU，PL 资源。

### 3.5 SRAM / DRAM / 缓冲管理

| 项 | 内容 |
|---|---|
| 职责 | L2P 表/缓存、命令队列、数据缓冲、FTL 工作区 |
| 消费级 | DRAM-less/HMB 或小 DRAM |
| 企业级 | DRAM 常驻 + ECC + 大缓冲 |
| 当前平台 | 板载 **32 MB SDRAM 未使用**；片上 1060 KB SRAM，约 685~706 KB 使用；DRAM-less |
| 设计取舍 | **主动选择 DRAM-less**：学 DRAM-less 分支 + 先做小再扩；代价是 SDRAM 闲置、元数据路径更复杂 |
| 学习点 | L2P 放哪、cache 命中/驱逐、journal/checkpoint；DRAM 型 vs DRAM-less |
| 细节 | `docs/design/ftl_design.md`、`docs/design/storage_controller_design.md` §5.2 |

#### 3.5.1 内存层次与 L2P 表大小

```text
CPU / FC
  │
[SRAM / TCM]        ← 快、小、贵；cache/队列/热数据
  │
[DRAM / SDRAM]      ← 大、慢、便宜；L2P 全表/大缓冲（可选）
  │
[NAND]              ← 最终存储；L2P 表也可以放在这里（DRAM-less）
```

先算账：

```text
L2P 表大小 ≈ 容量 / 映射粒度 × 表项大小

4 TB / 4 KB  × 4 B = 4 GB
4 TB / 512 B × 4 B = 32 GB
```

- 页级映射（4 KB）在 4 TB 盘上要 **4 GB** 表；
- CW 级映射（512 B）要 **32 GB**；
- 这就是企业级要 DRAM、消费级要 DRAM-less/HMB、CW 级映射必须配 cache 的原因。

#### 3.5.2 DRAM-based vs DRAM-less / HMB

| 方案 | L2P 表放哪 | 优点 | 代价 |
|---|---|---|---|
| DRAM-based | DRAM 常驻 | 查表简单、性能稳、队列深 | 需要 DRAM 控制器/ECC、成本/功耗 |
| DRAM-less | NAND + SRAM cache | 成本低、功耗低 | cache/thrashing/journal/CP 复杂 |
| HMB | 主机 DRAM（NVMe HMB） | 消费级 NVMe 常用 | 依赖主机支持；不是所有平台有 |

企业级通常 DRAM + ECC；消费级常见 DRAM-less/HMB；移动/嵌入式更极端
（小 SRAM + 块级/日志式 FTL）。

#### 3.5.3 缓冲管理：池 + 所有权

主控里的缓冲不是一块“大 malloc”，而是**固定池 + 所有权**：

| 池 | 放什么 | 关键约束 |
|---|---|---|
| command pool | 命令对象 | 深度固定；池空 = 背压 |
| descriptor pool | 描述符/链 | 链长有界；成环/越界是缺陷 |
| data buffer pool | 页/批数据 | DMA 对齐、cache 维护 |
| pending/request 表 | 在飞请求 | 完成配对、超时回收 |

当前平台：`cmd=64 / desc=256 / buf=128`；L2P cache + journal + checkpoint；
wbuf 写缓冲；所有池静态分配，没有动态堆。

#### 3.5.4 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| L2P 驻留 | DRAM-less/HMB 或小 DRAM | DRAM + ECC |
| 容量/成本 | 成本敏感 | 容量/性能优先 |
| 缓冲深度 | 较浅 | 更深，多队列 |
| 可靠性 | ECC | ECC + PLP + 端到端 |
| 功耗 | 低 | 高 |

#### 3.5.5 当前平台：DRAM-less + region cache

- 板载 **32 MB SDRAM 未使用**；片上 **1060 KB SRAM**，约 685~706 KB 使用；
- **DRAM-less**：CW 级全表按 4 B/CW 估算约 3.3 MiB，放不进 1 MB SRAM，
  所以用 **region cache + journal + checkpoint**；
- 数据缓冲/命令/描述符池静态分配；wbuf 做写合并；
- 取舍：学 DRAM-less 分支 + 先做小再扩；代价是 SDRAM 闲置、元数据路径复杂。

#### 3.5.6 选型检查清单

1. SRAM 容量/TCM：L2P cache、队列、池放得下吗？
2. 是否有 DRAM 控制器？DDR/LPDDR 几代、容量、ECC？
3. 是否支持 HMB（做消费级 NVMe）？
4. 内存带宽/延迟：PIO/DMA、cache 一致性；
5. 缓冲池深度：命令/描述符/数据/在飞请求；
6. FPGA 平台：BRAM/URAM、外部 DRAM、PS DRAM；
7. 是否需要 PLP/ECC 与 DRAM 配合。

### 3.6 NAND 通道控制器（FC）

| 项 | 内容 |
|---|---|
| 职责 | 通道/CE/LUN/plane 时序、命令/描述符/缓冲池、UOP 引擎、multi-plane/cache/copy-back |
| 消费级 | 4~8 通道 |
| 企业级 | 8~16 通道，interleaving/更复杂仲裁 |
| 当前平台 | **软件 FC-NAND**；1 ch × 1 CE × 1 LUN × 2 plane；cmd=64/desc=256/buf=128 池 |
| 设计取舍 | 不上硬 FC，用软件池 + UOP 学主控命令流；代价是 CPU 开销、无硬件并行 |
| 学习点 | 命令/描述符/缓冲池、UOP 建链、完成泵、资源上限 |
| 细节 | `docs/design/fc_nand_design.md`、`src/fc/` |

#### 3.6.1 FC 在数据路径中的位置

```text
FTL 命令（读/写/擦）
  → FC：命令/描述符/缓冲池
  → UOP/时序引擎：发 NAND 命令序列
  → NAND PHY（ONFI/Toggle 引脚）
  → NAND die
```

**FC ≠ NAND PHY**：FC 是数字控制逻辑（命令、描述符、UOP、通道仲裁、ECC/DMA 接口），
NAND PHY 是引脚级电气接口（ONFI/Toggle、时序、ODT、ready/busy）。

#### 3.6.2 FC 内部有什么

| 部件 | 作用 |
|---|---|
| 命令/描述符/缓冲池 | 接收 FTL/CPU 的命令，管理资源 |
| 描述符链 | 地址、长度、标志、next；链长有界 |
| UOP / 微码引擎 | 把命令展开成 NAND 命令序列（reset/read ID/read status/read/program/erase/multi-plane） |
| 通道/CE/LUN/plane 调度 | 多通道/多 CE/多 LUN/多 plane 的并行与仲裁 |
| 时序引擎 | tR/tPROG/tERASE、ready/busy、ONFI/Toggle 模式 |
| 数据面 | DMA/PIO、缓冲、ECC/加扰/CRC 流水线 |
| 完成/状态 | 命令完成、错误状态、超时 |

#### 3.6.3 典型命令流

```text
FTL: 读 page(PBA)
  → FC 分配 command/descriptor/buffer
  → UOP: 00h + 地址 + 30h
  → 等 R/B# / 读 status
  → 数据搬运 + ECC 解码
  → 完成回调
```

写/擦类似，只是命令序列和时序不同；multi-plane/cache program 会把多个命令合并到一轮。

#### 3.6.4 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 通道 | 4~8 | 8~16 |
| CE/LUN/plane | 较少 | 更多，interleaving |
| ECC | BCH/LDPC | LDPC + 软判决 + RAID |
| 并行 | 基本 multi-plane | 多通道/多 CE 仲裁、QoS |
| 数据面 | DMA | 多队列 DMA、描述符/SG |

#### 3.6.5 当前平台：软件 FC-NAND

- `src/fc/` 是**软件 FC**：命令/描述符/缓冲池（cmd=64/desc=256/buf=128）、UOP 引擎；
- `src/nand/nand_fmc.c` 负责 FMC 引脚级时序；数据面 **PIO**；
- 1 ch × 1 CE × 1 LUN × 2 plane；ONFI 1.0 Mode 5；
- BCH(13,4)、LFSR16 加扰、CRC 在 FC 数据面；
- 取舍：不上硬 FC，用软件池 + UOP 学命令流；代价是 CPU 开销、无硬件并行；
- 多 plane 有；cache program op44 正常路径已上板；copy-back 未实现。

#### 3.6.6 FPGA / 多核平台

- 硬 FC：FPGA/ASIC 里的专用 NAND 控制器，CPU 只配描述符；省 CPU、支持多通道；
- 软 FC：PL 里自己写 NAND 控制器/UOP/ECC；灵活但占 LUT/BRAM；
- NAND PHY：FPGA I/O bank + 电平/时序；不是 SerDes；
- 板卡必须引出 NAND 引脚（CE/ALE/CLE/RE/WE/RB/DQ），否则 FC 没用。

#### 3.6.7 选型检查清单

1. NAND 通道数、CE/LUN/plane 数；
2. ONFI/Toggle 版本、时序模式、ODT；
3. 是否支持 multi-plane / cache read/program / copy-back；
4. 数据面 DMA/PIO、描述符/SG；
5. ECC：BCH/LDPC、软判决、RAID；
6. FC 是硬核还是 PL 软核？
7. 板卡 NAND 连接器/引脚是否引出？

### 3.7 NAND 接口 / ONFI / 时序

| 项 | 内容 |
|---|---|
| 职责 | ONFI/Toggle 命令、SDR/NV-DDR 时序、地址周期、ODT、ready/busy |
| 消费级 | ONFI 3/4、NV-DDR |
| 企业级 | ONFI 4/5、NV-DDR3/4、高速信号完整性 |
| 当前平台 | **ONFI 1.0 SDR，Mode 5**（timing modes 0x003F）；MT29F4G08 |
| 设计取舍 | 受器件约束；先覆盖基础命令/时序，不做高速/多通道 |
| 学习点 | ONFI 命令、时序模式、multi-plane、tR/tPROG/tERASE |
| 细节 | `docs/design/fc_nand_design.md` §3.10~§3.14、`src/nand/` |

#### 3.7.1 引脚/信号

```text
FC
  │ CE# / CLE / ALE / RE# / WE# / R/B# / DQ[7:0] / DQS / ODT
  ▼
NAND die
```

| 信号 | 作用 |
|---|---|
| CE# | 片选 |
| CLE / ALE | 命令锁存 / 地址锁存 |
| RE# / WE# | 读使能 / 写使能 |
| R/B# | ready/busy |
| DQ[7:0] | 数据/命令/地址复用 |
| DQS | 源同步数据选通（NV-DDR/Toggle） |
| ODT | 片上终端（高速时） |
| WP# | 写保护 |

#### 3.7.2 ONFI vs Toggle

| 维度 | ONFI | Toggle |
|---|---|---|
| 标准 | ONFI 规范 | JEDEC 风格（厂商主导） |
| 参数页 | 有（signature/model/page/block/timing） | 有的有、有的没有 |
| 时序 | SDR / NV-DDR / NV-DDR2/3/4 | 类似，DQS 源同步 |
| 常见 | Micron/Intel/部分国产 | 三星/铠侠/部分国产 |
| 本仓 | **ONFI 1.0 SDR，Mode 5** | 不支持 |

#### 3.7.3 命令与参数页

典型命令（具体 opcode 以数据手册为准）：

- Reset、Read ID、Read Status；
- Read Page（00h + 地址 + 30h）；
- Program（80h + 地址 + 数据 + 10h）；
- Erase（60h + 地址 + D0h）；
- Multi-plane / cache read / cache program / copy-back（器件相关）。

**ONFI 参数页**是开机识别器件的关键：signature、revision、model、page size、block size、
LUN 数、ECC、支持的 timing modes。本仓启动日志里的 `ONFI ...` 就来自这里。

#### 3.7.4 时序与 ready/busy

- 关键时序：tR（读）、tPROG（写）、tERASE（擦）、tCCS、tADL、tWB、tRR 等；
- **Mode 5** 是当前器件/板卡的工作点；
- ready/busy 可以读 R/B# 引脚，也可以读 status；
- 超时要按数据手册留裕量（本仓 `tR`/`tPROG` 相关常量在 NAND 层）。

#### 3.7.5 multi-plane / cache / copy-back

- **multi-plane**：一次命令操作多个 plane，提高并行；
- **cache read/program**：用 cache 寄存器重叠下一命令；
- **copy-back**：片内搬页，省数据往返（是否支持看器件）；
- 本仓：2 plane；cache program op44 正常路径已上板；copy-back 未实现。

#### 3.7.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| ONFI 版本 | ONFI 1.0~3.x | ONFI 4/5，NV-DDR3/4 |
| 介质 | TLC/QLC | pSLC + TLC/QLC（固定 pSLC 为主，见 4.10） |
| 并行 | 基本 multi-plane | 多 plane/多 LUN/interleaving |
| 信号 | 板级短走线 | 长走线/ODT/retimer/信号完整性 |
| ECC | BCH/LDPC | LDPC + 软判决 |

#### 3.7.7 当前平台

- MT29F4G08：ONFI 1.0，SDR，Mode 5；512 MiB，page 2048+64，block 64 页，4096 块，1 LUN，2 plane；
- FMC bank 3；PIO 数据面；BCH(13,4)；
- 无 read-retry table，`0x89` 是保留地址、自始空转（本项目决定不处理）；
- 启动时读 ONFI 参数页并打印 device/model/timing modes。

#### 3.7.8 选型检查清单

1. ONFI 还是 Toggle？版本/时序模式？
2. 器件是否支持 multi-plane / cache read/program / copy-back？
3. 数据手册是否可得？（学习项目很重要）
4. 引脚/封装/板卡连接器是否匹配？
5. 时序参数与 FC 的 timing 引擎是否匹配？
6. ECC 要求：BCH 多少位 / LDPC？
7. 是否有 read-retry / ODT / 高速模式？

### 3.8 ECC / 加扰 / CRC / 端到端保护

| 项 | 内容 |
|---|---|
| 职责 | BCH/LDPC 编解码、LFSR 加扰、CRC、RAID/XOR、DIF/DIX |
| 消费级 | BCH/LDPC |
| 企业级 | LDPC + 软判决 + RAID + 端到端 |
| 当前平台 | **BCH(13,4)**、LFSR16 加扰、CRC（ISO-HDLC/MPEG-2）；无 LDPC；`CONFIG_FC_RAID=0` |
| 设计取舍 | 器件/算力约束；先 BCH + 加扰 + CRC，LDPC/RAID 不做 |
| 学习点 | ECC 在数据路径的位置、加扰、CRC、错误恢复 |
| 细节 | `docs/design/fc_nand_design.md` §3.7、`src/fc/` |

#### 3.8.1 数据路径中的位置

```text
写：host 数据 → 加扰 → ECC 编码 → NAND program
读：NAND read → ECC 解码 → 解扰 → CRC/端到端校验 → host
```

目标：把 NAND 的 **raw BER（1e-3 量级）** 压到主机可接受的 **1e-15 量级**。

#### 3.8.2 ECC：BCH vs LDPC

| 维度 | BCH | LDPC |
|---|---|---|
| 判决 | 硬判决 | 软判决（LLR） |
| 强度 | 中等 | 强，适合 TLC/QLC |
| 复杂度 | 低，门数少 | 高，迭代解码 |
| 读重试 | 少 | 常配 soft read/read retry |
| 典型 | SLC/MLC、低成本 | 企业级/大容量 TLC/QLC |

本仓：**BCH(13,4)**（m=13、t=4，4-bit 纠正），CW=512 B，4 CW/page，OOB 16 B/CW；
没有 LDPC/软判决。

#### 3.8.3 加扰（Scrambler）

- 用 LFSR（本仓 16 位）把数据“打散”，避免长 0/长 1、周期性图案；
- 减少 NAND 单元间耦合/噪声，**不是加密**；
- 种子通常来自 LBA/页地址，读时同种子解扰；
- 本仓：`LFSR 16 (x^16+x^14+x^13+x^11+1)`。

#### 3.8.4 CRC 与端到端保护

| 层 | 保护 | 作用 |
|---|---|---|
| NAND 页/CW | ECC | 纠正位翻转 |
| FC 页 | CRC | 检测 ECC 漏纠/元数据损坏 |
| 主机数据 | 端到端 CRC | 检测设备内部搬移/缓存错误 |
| 企业级 | DIF/DIX（T10 PI） | 每 sector 保护信息，端到端校验 |

本仓：FC CRC（ISO-HDLC/MPEG-2）；**没有**主机级端到端 CRC/DIF-DIX。

#### 3.8.5 RAID / XOR

- 跨 die/channel/plane 做 XOR，坏一颗 die 可恢复；
- 企业级常见；消费级一般没有；
- 本仓 `CONFIG_FC_RAID=0`，RAID 代码完成但未验证。

#### 3.8.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| ECC | BCH/LDPC 硬判决 | LDPC + 软判决 |
| read retry | 基本 | soft read/多档 |
| RAID | 一般无 | 常见 |
| 端到端 | 可选 CRC | DIF/DIX |
| PLP | 无 | 有 |

#### 3.8.7 当前平台

- BCH(13,4)、LFSR16 加扰、CRC（ISO-HDLC/MPEG-2）；
- 无 LDPC、无软判决、无 RAID、无 DIF/DIX；
- read recovery `0x89` 是保留地址，自始空转（本项目决定不处理）；
- 学习点：ECC 编解码、加扰种子、CRC 位置、错误路径。

#### 3.8.8 选型检查清单

1. NAND 需要多少位 ECC？BCH 还是 LDPC？
2. 是否需要软判决/read retry？SoC/FPGA 有没有对应 IP？
3. 是否需要 RAID/XOR？硬件加速器还是软件？
4. 是否需要端到端 CRC / DIF/DIX？
5. 加扰/CRC 是硬件还是软件？
6. FPGA 平台：ECC/CRC/RAID IP 占多少 LUT/BRAM？
7. 错误处理：ECC 计数、UNC、read recovery、日志。

### 3.9 DMA / 数据通路

| 项 | 内容 |
|---|---|
| 职责 | host↔device、memory↔NAND 数据搬运；描述符/SG；多队列 |
| 消费级 | DMA |
| 企业级 | 描述符/SG、多队列 DMA、QoS |
| 当前平台 | **PIO 逐字节 NAND 数据面**；UART 用 DMA；无 NAND DMA |
| 设计取舍 | PIO 先走通 FMC 时序/ECC；代价是 CPU-bound、单核吞吐 |
| 学习点 | PIO vs DMA、FMC 时序、总线/缓存一致性 |
| 细节 | `src/nand/nand_fmc.c`、`docs/design/fc_nand_design.md` §3.9 |

#### 3.9.1 数据路径三段

```text
Host 内存
  │  PCIe DMA / USB endpoint
  ▼
设备缓冲（SRAM/DRAM）
  │  FC DMA / PIO
  ▼
NAND 控制器 / NAND die
```

| 段 | 谁搬 | 典型机制 |
|---|---|---|
| Host ↔ 设备 | PCIe controller / USB controller | DMA、PRP/SGL、endpoint |
| 设备内部 | DMA / CPU | SRAM/DRAM ↔ FC 缓冲 |
| 设备 ↔ NAND | FC | NAND DMA 或 PIO |

#### 3.9.2 DMA vs PIO

| 维度 | DMA | PIO |
|---|---|---|
| CPU 占用 | 低 | 高，逐字节/逐字搬 |
| 复杂度 | 描述符/链/一致性问题 | 简单直接 |
| 吞吐 | 高 | 低，CPU-bound |
| 典型 | 主控标配 | 小 MCU/学习原型 |

本仓：**NAND 数据面 PIO**；UART 用 DMA；FC 池在 `.noncacheable_buffer` 段，
避免 D-Cache 一致性问题。

#### 3.9.3 描述符 / SG / cache

- DMA 不是“一个 memcpy”，而是**描述符链**：地址、长度、方向、next；
- SG（scatter-gather）支持不连续缓冲，适合 NVMe 的 PRP/SGL；
- DMA 与 cache 的交互：
  - CPU→设备：`SCB_CleanDCache_by_Addr`；
  - 设备→CPU：`SCB_InvalidateDCache_by_Addr`；
  - 地址/长度按 32 B 对齐（本仓 UART 范本）。

#### 3.9.4 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| Host DMA | PCIe DMA/USB | 多队列 DMA、SG |
| NAND DMA | 有 | 多通道 DMA |
| 描述符 | 基本 | 链式/SG、深队列 |
| 一致性 | 基本 | 多核/cache 一致性 |
| QoS | 基本 | 多队列仲裁 |

#### 3.9.5 当前平台

- NAND 数据面 **PIO**：CPU 通过 FMC 逐字节搬；ECC/加扰/CRC 在软件/FC；
- UART DMA：Clean + 32 B 对齐；
- FC 池放 `.noncacheable_buffer`，避免 cache 维护；
- 取舍：PIO 先走通 FMC 时序/ECC；代价是 CPU-bound、单核吞吐；
- 学习点：PIO vs DMA、FMC 时序、cache 一致性、总线竞争。

#### 3.9.6 FPGA / 多核平台

- FPGA：AXI DMA/HP 端口、BRAM/URAM、PL 自定义 DMA；PS↔PL 带宽要算；
- 多核：DMA 与 cache 一致性（ACE/CCI）、核间队列；
- 硬 FC 通常自带 DMA；软 FC 要自己写描述符/搬运；
- 板卡：DDR 带宽、PCIe DMA、NAND 引脚。

#### 3.9.7 选型检查清单

1. 是否有 DMA 引擎？几个通道？
2. 是否支持描述符/SG？
3. Host 侧 DMA：PCIe PRP/SGL 或 USB endpoint？
4. NAND 侧 DMA：FC 自带还是 CPU 搬？
5. Cache 一致性：是否有 noncacheable 区域/维护 API？
6. 带宽：DDR/PCIe/NAND 是否匹配？
7. FPGA：AXI DMA/HP 端口、BRAM/URAM 资源。

### 3.10 安全

| 项 | 内容 |
|---|---|
| 职责 | 安全启动、AES-XTS、SHA、密钥管理、TCG Opal、Sanitize |
| 消费级 | 可选 |
| 企业级 | 常见 |
| 当前平台 | 无 |
| 设计取舍 | 不在本仓范围；先聚焦 FTL/FC 骨架 |
| 学习点 | 静态数据保护、密钥生命周期、Sanitize |
| 细节 | — |

#### 3.10.1 安全子系统覆盖什么

安全不是 FTL/FC 的一部分，但会影响**启动链**和**数据路径**：

| 功能 | 作用 |
|---|---|
| 安全启动 | 校验固件签名，防止刷入未授权固件 |
| 数据加密 | AES-XTS 等，保护静态数据 |
| 密钥管理 | 密钥存在 OTP/安全存储，派生/包装/轮换 |
| 访问控制 | TCG Opal、锁定范围、密码/凭证 |
| Sanitize | 安全擦除/加密擦除，保证数据不可恢复 |
| 安全调试 | JTAG/SWD 锁定、认证调试 |
| 证明/审计 | 度量启动、事件日志（企业/数据中心） |

#### 3.10.2 数据加密与密钥

- **AES-XTS** 是 SSD 静态数据加密的主流模式：按 sector/LBA 调整 tweak，避免相同明文出现相同密文；
- 密钥通常在 **OTP/eFUSE/安全存储**，不要放在普通 Flash；
- 密钥生命周期：生成 → 注入 → 派生 → 轮换 → 销毁；
- **Crypto erase**：销毁密钥比擦全盘快，是企业级 Sanitize 的常用手段。

#### 3.10.3 安全启动与调试

```text
Boot ROM → 校验固件签名 → 加载固件 → 锁定调试口
```

- 公钥/根信任根在 OTP/eFUSE；
- 固件签名（RSA/ECDSA） + 哈希；
- 调试口（JTAG/SWD）在量产时锁定或认证解锁；
- FPGA 平台还要考虑 **bitstream 加密**、PL 配置安全。

#### 3.10.4 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 安全启动 | 可选 | 常见 |
| 数据加密 | 可选 SED/Opal | 常见，AES-XTS |
| 密钥管理 | 基本 | OTP/安全存储、轮换 |
| Sanitize | 基本 | Crypto erase/标准 Sanitize |
| 安全调试 | 一般无 | 锁定/认证 |
| 合规 | 一般无 | FIPS/CC 等（按产品） |

#### 3.10.5 当前平台

- **无**安全启动、AES、Opal、Sanitize；
- SWD 开放；板卡是学习原型；
- 取舍：先聚焦 FTL/FC 数据路径，安全不在本仓范围；
- 学习点：安全启动链、AES-XTS、密钥生命周期、Sanitize。

#### 3.10.6 FPGA / 多核平台

- SoC 安全启动（RSA/AES/eFUSE）可用；
- FPGA bitstream 加密/认证；
- PL 里的加密 IP 会占 LUT/BRAM/时钟；
- 调试口锁定要在板级/量产流程里做。

#### 3.10.7 选型检查清单

1. 是否需要安全启动？SoC/FPGA 支持吗？
2. 是否需要 AES-XTS/加密引擎？吞吐够吗？
3. 密钥存哪？OTP/eFUSE/安全存储？
4. 是否需要 TCG Opal / 锁定范围？
5. 是否需要 Sanitize / crypto erase？
6. 调试口能否锁定/认证？
7. 是否需要 FIPS/CC 等认证？

### 3.11 电源 / PLP

| 项 | 内容 |
|---|---|
| 职责 | PLP 电容、掉电保护、brown-out、电源时序 |
| 消费级 | 无 PLP |
| 企业级 | PLP 是硬指标 |
| 当前平台 | 无；IWDG 也关闭（待上板决策） |
| 设计取舍 | 不在本仓范围；掉电窗口用 Journal/CP 处理 |
| 学习点 | 掉电窗口、flush 屏障、PLP 为什么贵 |
| 细节 | `docs/design/ftl_design.md`、`src/main.c` |

#### 3.11.1 PLP 是什么

**PLP = Power Loss Protection（掉电保护）**：主控在掉电瞬间靠板上的储能（电容/超级电容）
撑住一小段时间，把易失数据（cache / journal / CP）落盘，再安全断电。

```text
正常供电 ──► PMIC/电源时序 ──► 主控 + NAND
掉电瞬间 ──► PLP 储能 ──► 主控检测到 PFail ──► 刷 cache/journal/CP ──► 安全断电
```

PLP 是**板级/电源**能力，不是 FTL 功能；但没有它，企业级掉电就会丢已回执写。

#### 3.11.2 PLP 关键量

| 量 | 含义 |
|---|---|
| Hold-up time | 掉电后还能撑多久 |
| Energy budget | 电容能量 = 1/2·C·V²，要覆盖 flush 时间 |
| PFail 检测 | 比较器/PMIC 提前通知主控 |
| Flush 时间 | cache/journal/CP 落盘需要多久 |
| 电容健康 | 电容老化/容值下降要监控 |
| 成本/体积 | 超级电容/钽电容贵、占板 |

#### 3.11.3 掉电流程

```text
PFail 中断/比较器
  → 停止接收新 host 命令
  → flush cache / journal / CP
  → 等 NAND program 完成
  → 安全断电
```

如果 PLP 不够，只能靠**下次开机的 Journal/CP/PFR** 恢复；这就是消费级/当前平台的做法。

#### 3.11.4 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| PLP | 一般无 | **必须有** |
| 掉电策略 | Journal/CP 恢复 | PLP + Journal/CP |
| 电容健康 | 无 | 监控 |
| 成本/体积 | 低 | 高 |
| 适用 | PC/便携 | 服务器/数据中心 |

#### 3.11.5 当前平台

- **无 PLP**；IWDG 也关闭（待上板决策）；
- 掉电靠 **Journal + Checkpoint + PFR** 在下次开机恢复；
- VWC（volatile write cache，易失写缓存；本仓缩写）早回执扩大了掉电窗口（已回执但未落盘的写）；
- 取舍：学习原型不做 PLP；代价是掉电可能丢一个窗口的已回执写；
- 学习点：掉电窗口、flush 屏障、Journal/CP、PLP 能量预算。

#### 3.11.6 FPGA / 多核平台

- PLP 仍是**板级**：PMIC/sequencer/电容；
- FPGA 配置/bitstream 也要考虑掉电；
- 多核平台要保证 PFail 后所有核都停止/flush；
- 企业级还要 telemetry 监控电容健康。

#### 3.11.7 选型检查清单

1. 是否需要 PLP？企业级通常必须；
2. Hold-up time/能量预算够不够 flush？
3. 是否有 PFail 检测（比较器/PMIC）？
4. 电源时序/PMIC 是否可控？
5. 电容健康监控？
6. 掉电恢复路径：Journal/CP/PFR 是否完整？
7. 板级空间/成本是否可接受？

### 3.12 热 / 功耗管理

| 项 | 内容 |
|---|---|
| 职责 | 温度传感器、throttle、功耗状态、telemetry |
| 消费级 | 基本 throttle |
| 企业级 | 更细粒度、集群 telemetry |
| 当前平台 | 无专门热管理；400 MHz；IWDG 关闭 |
| 设计取舍 | 不在本仓范围 |
| 学习点 | 持续性能 vs 峰值性能 |
| 细节 | — |

#### 3.12.1 热 / 功耗管理做什么

它保证主控在**持续负载**下不因过热降频/停机，并让功耗落在板卡/机箱预算内。

```text
温度传感器 / PMIC
  → 主控 throttle（降频/限队列/限 GC）
  → 风扇/机箱（板级）
  → telemetry
```

#### 3.12.2 关键机制

| 机制 | 作用 |
|---|---|
| 温度传感器 | 主控/板卡/NAND 温度 |
| Throttle | 降频、限队列、限后台 GC，保 host 可用 |
| DVFS | 动态调频/调压（SoC 支持时） |
| 电源状态 | idle/sleep、clock gating、power gating |
| 风扇/机箱 | 板级散热 |
| 温度感知调度 | 热数据/冷数据、GC 时机、WL |
| Telemetry | 温度/功耗日志（企业） |

#### 3.12.3 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 散热 | 被动/小风扇 | 机箱风道/风扇 |
| Throttle | 有，保不烫手 | 有，保稳态性能 |
| 温度感知 | 基本 | GC/WL/数据放置 |
| Telemetry | 基本 SMART | 完整温度/功耗 |
| 目标 | 峰值/成本 | **持续性能/P99.9** |

#### 3.12.4 当前平台

- 无专门热管理：没有温度传感器策略、没有风扇、没有 DVFS；
- 400 MHz 固定；IWDG 关闭（待上板决策）；
- FTL 有 **hot/cold 数据**分类，但那是数据放置/寿命策略，不是板级温度；
- 取舍：学习原型不追持续性能；代价是长时间满负载可能热/降频（板级未知）；
- 学习点：throttle、持续性能 vs 峰值、温度感知 GC/WL。

#### 3.12.5 FPGA / 多核平台

- FPGA 温度/功耗要算：PL 资源越多，功耗越高；
- 需要温度传感器/风扇/PMIC 监控；
- 多核 DVFS 复杂；企业级要 telemetry；
- 板级散热和机箱风道是硬约束。

#### 3.12.6 选型检查清单

1. 是否有温度传感器（主控/板卡/NAND）？
2. 是否支持 throttle/DVFS/电源状态？
3. 散热方案：被动/风扇/机箱？
4. 是否需要温度感知 GC/WL？
5. 是否需要 telemetry（温度/功耗）？
6. 持续负载下的功耗预算？
7. FPGA：PL 功耗/散热是否可接受？

### 3.13 调试 / telemetry

| 项 | 内容 |
|---|---|
| 职责 | UART/JTAG/SWD、SMART、telemetry log、error log、固件更新 |
| 消费级 | 基本 SMART |
| 企业级 | 完整 telemetry/error log |
| 当前平台 | UART CLI、SWD、`stats`/`smart`；无 telemetry log page |
| 设计取舍 | 用 `stats`/`smart` 做板端观测；不做主机 Log Page |
| 学习点 | 观测面设计、计数器口径、故障取证 |
| 细节 | `docs/design/common/statistics_design.md`、`docs/design/common/logging.md` |

#### 3.13.1 调试 / telemetry 覆盖什么

它是主控的**可观测面**：出问题时能回答“卡在哪、为什么、发生了多少次”。

| 层 | 手段 |
|---|---|
| 硬件调试 | JTAG/SWD、trace、ILA/SignalTap、逻辑分析仪 |
| 固件调试 | CLI、日志、断言、fault capture、栈哨兵 |
| 统计 | 计数器、直方图、`stats`、`--delta` |
| 主机侧 | SMART、telemetry log page、error log、固件更新 |
| 产测/诊断 | 自检、坏块扫描、read scrub、事件日志 |

#### 3.13.2 调试接口与故障捕获

- **JTAG/SWD**：下载、断点、寄存器/内存查看；
- **trace**：ETM/ITM、指令流、时间戳（SoC 支持时）；
- **assert/fault capture**：HardFault 现场（PC/LR/CFSR/HFSR）、栈哨兵、`.noinit` 记录；
- **CLI**：`help`、`info`、`stats`、`pc`、`ftl_ctx`、`dump`；
- **日志**：分级、限流、模块 tag、时间戳（可外部提供）。

#### 3.13.3 Telemetry / SMART / 日志

- **SMART**：温度、寿命、坏块、ECC、UNC、写放大、通电时间；
- **Telemetry log page**：主机可拉取的统计/事件（NVMe）；
- **Error log**：错误类型、LBA/PBA、状态、时间；
- **持久化**：终身计数（本仓 `smart` 走 CP Header，掉电保持）；
- **事件环**：`errlog` RAM 环（本仓待实现）。

#### 3.13.4 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| SMART | 基本 | 完整 |
| Telemetry log | 一般无 | 有 |
| Error log | 基本 | 完整 |
| 持久化计数 | 部分 | 常见 |
| 远程/集群 | 无 | 有 |
| 调试口 | 可开放 | 量产锁定 |

#### 3.13.5 当前平台

- UART CLI、SWD、`stats`/`smart`、日志、fault capture、栈哨兵、PC 采样；
- `stats` 统一注册/打印/`--delta`/`--clear`；直方图（L2P load/GC/CP/host）；
- `smart` 终身计数落 CP Header；无 NVMe telemetry log page；
- 日志级别/限流/`suppressed`；断言现场 dump；
- 取舍：板端可观测优先，不做主机 Log Page；
- 学习点：统计 vs 日志、限流、故障取证、板端判据。

#### 3.13.6 FPGA / 多核平台

- ILA/SignalTap 看 PL 内部信号；
- 多核日志/计数器要统一时间基与归属；
- trace 带宽/缓冲是资源；
- 企业级 telemetry 要跨核聚合。

#### 3.13.7 选型检查清单

1. 调试接口：JTAG/SWD、trace、ILA？
2. 是否有硬件性能计数器/时间戳？
3. 主机侧：SMART/telemetry/error log 要不要？
4. 持久化计数：放哪、怎么掉电保持？
5. 日志/统计：限流、分级、`--delta`？
6. 故障捕获：HardFault/栈哨兵/事件环？
7. FPGA/多核：trace 资源、跨核时间基？

### 3.14 可靠性 / 可用性

| 项 | 内容 |
|---|---|
| 职责 | ECC、RAID、journal/CP/PFR、BBT、WL、read scrub、双端口/热插拔 |
| 消费级 | ECC + 基本恢复 |
| 企业级 | ECC+RAID+PLP+双端口+热插拔 |
| 当前平台 | ECC、Journal/Checkpoint/PFR、BBT、动态/静态 WL；无 RAID/双端口 |
| 设计取舍 | 先做单 LUN 的完整恢复链；多通道/双端口后续 |
| 学习点 | 掉电恢复、坏块管理、WL、端到端保护 |
| 细节 | `docs/design/ftl_design.md`、`src/ftl/` |

#### 3.14.1 可靠性 vs 可用性

- **可靠性（reliability）**：不丢、不坏、可恢复；ECC、RAID、Journal/CP、BBT、WL；
- **可用性（availability）**：服务不中断；双端口、热插拔、failover、PLP。

#### 3.14.2 保护链

```text
Host 数据
  → HIL：请求/完成状态、超时、deferred error
  → FTL：L2P/Journal/CP/PFR、BBT、WL
  → FC：ECC/CRC/加扰、命令状态、超时
  → NAND：ECC、坏块、read retry
  → 端到端：CRC/DIF/DIX
```

每一环都可能失效；**不能只靠一环**。企业级要端到端保护，消费级至少 ECC + 恢复。

#### 3.14.3 失效模式与恢复

| 失效 | 手段 |
|---|---|
| 位翻转/读干扰/保持性 | ECC、read retry、read scrub |
| 坏块/磨损 | BBT、WL、备用块 |
| 掉电 | PLP、Journal/CP/PFR |
| die/通道失效 | RAID/XOR、双端口、failover |
| 固件 bug/总线错误 | 断言、CRC、deferred error、telemetry |
| 元数据损坏 | 双副本、CRC、rebuild |

#### 3.14.4 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| ECC | BCH/LDPC | LDPC + 软判决 |
| RAID | 一般无 | 常见 |
| PLP | 无 | 有 |
| 双端口/热插拔 | 无 | 有 |
| 掉电恢复 | Journal/CP | PLP + Journal/CP |
| telemetry | 基本 | 完整 |
| 目标 | 不丢用户数据 | 不丢 + 不停机 |

#### 3.14.5 当前平台

- ECC、Journal/Checkpoint/PFR、BBT、动态/静态 WL；
- 无 RAID、无 PLP、无双端口/热插拔；
- 单 NAND/单 LUN；掉电靠下次开机恢复；
- 取舍：先做单 LUN 的完整恢复链；多通道/双端口后续；
- 学习点：保护链、失效模式、掉电恢复、坏块/WL、端到端。

#### 3.14.6 FPGA / 多核平台

- RAID 可在 PL 做 XOR，也可软件；
- 双端口/热插拔需要板级/控制器支持；
- 多核 failover/telemetry 要设计；
- PLP 仍是板级。

#### 3.14.7 选型检查清单

1. ECC/RAID：需要哪一级？
2. 掉电恢复：Journal/CP/PFR 是否完整？
3. BBT/WL/read scrub 是否有？
4. PLP/双端口/热插拔要不要？
5. 端到端保护：CRC/DIF/DIX？
6. telemetry/error log 是否够？
7. FPGA/多核：RAID/failover 资源？

## 4. FTL / 固件功能块

> FTL 是软件；它使用 §3 的硬件。下表列消费级/企业级典型，以及本仓当前平台。

| 功能 | 消费级典型 | 企业级典型 | 当前平台 |
|---|---|---|---|
| 器件发现 / ONFI 参数页 | 有 | 有 | 有 |
| BBT / 备用块 | 有 | 更复杂策略 | 有 |
| L2P 映射粒度 | 页/CW 级 | 页级/混合 | CW 级（IU=512 B） |
| L2P 驻留 | DRAM-less/HMB | DRAM 常驻 | DRAM-less |
| L2P cache / thrashing | CLOCK 等 | 更复杂 | CLOCK + thrashing 降级 |
| GC victim / 水位 / 预擦除池 | 有 | 稳态性能、尾延迟 | 水位/紧急 + victim 评分；无预擦除池 |
| 磨损均衡 | 动态/静态 | 动态/静态 + 持久化 | 动态 + 静态 |
| 掉电恢复 | Journal/CP | Journal/CP + PLP | Journal + Checkpoint + PFR |
| OP（over-provisioning）/ WAF（write amplification factor） | ~7% OP | ~28% OP、DWPD | OP≈10%；WAF 无独立指标 |
| SLC cache | 常见 | 少用（固定 pSLC） | 不适用（SLC 介质） |
| 写路径 / 写缓冲 / 早回执 / FLUSH | 有 | 有 + 严格顺序 | VWC 早回执 + 读覆盖 |
| 读路径 / 读 cache / prefetch / read retry | 有 | 有 + 软判决 | prefetch 有；read retry 无（0x89 空转） |
| TRIM/UNMAP/discard | 有 | 有 | 有 |
| 调度/QoS | 基本 | P99.9、读优先、多租户 | 基本序列门控/背压 |
| Telemetry/SMART/日志 | 基本 | 完整 | `stats`/`smart` |
| 安全/加密/Opal/Sanitize | 可选 | 常见 | 无 |
| 固件更新/双镜像 | 有 | 有 + 回滚 | 无 |
| 端到端 CRC / DIF/DIX | 可选 | 常见 | 部分（FC CRC；无 DIF/DIX） |
| 数据放置/多流/温区 | 可选 | 常见 | 无多流；块温度参与 GC |
| namespace / ZNS / FDP | 单 namespace 常见 | 多 namespace、ZNS/FDP | 单 LUN（无） |

> 下面按 §4.1~§4.20 逐个展开：策略 + 取舍，不关注代码实现。
> §4.1~§4.20 已全部定稿，每节末尾标「状态：已定稿」。

### 4.1 器件发现 / ONFI 参数页 / 几何

#### 4.1.1 发现什么

启动时要知道：

- 器件身份：manufacturer/model/ID；
- 几何：page、block、LUN、plane、blocks/LUN；
- 接口：ONFI/Toggle、地址周期、timing modes、ECC 要求；
- 介质状态：present、ready/busy、BBT、format/version；
- 容量：可用块、坏块、OP。

#### 4.1.2 发现手段与取舍

| 手段 | 优点 | 代价/风险 |
|---|---|---|
| ONFI 参数页 | 标准、字段全（page/block/timing/ECC） | 不是所有 NAND 都有；坏页/CRC 失败 |
| Read ID + ID 表 | 简单，几乎都支持 | 只能识别型号，几何/时序靠表 |
| 硬编码几何 | 启动快、确定 | 换器件就错；不可移植 |
| 厂商数据手册 | 最准 | 人工维护、易过期 |

**策略**：优先 ONFI 参数页；没有/失败则 Read ID + ID 表；最后才硬编码/拒绝启动。
**取舍**：标准/可移植 vs 启动时间/复杂度；信任参数页 vs 交叉校验。

#### 4.1.3 几何校验

- 参数页/ID 表给出的 page/block/LUN/plane 要与驱动假设一致；
- 不一致时：拒绝启动（安全） vs 按参数动态配置（灵活） vs 回退默认（危险）；
- 本仓倾向**校验 + 拒绝**，不静默按错几何跑。

#### 4.1.4 timing / ECC 发现

- ONFI 参数页给 timing modes；选最高共同支持的模式，失败回退；
- ECC 要求（BCH 位数）与 FC 能力匹配；
- 本仓：ONFI 1.0，Mode 5；BCH(13,4)；参数页打印 device/model/timing modes。

#### 4.1.5 format / version / 坏块

- 介质上有 layout version / format 标志 / BBT；
- 空片：自动格式化 vs 拒绝（等 CLI）；
- 旧版本：迁移 vs 拒绝 vs 重建；
- 本仓：media layout version 12；format/auto-format；boot type 1/2；full rebuild fallback；
  `bbt` CLI 是探针，不是坏块图（坏块图在 allocator）。

#### 4.1.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 发现 | ONFI/ID 表 | ONFI/Toggle，更多参数 |
| 几何 | 单 LUN/少量 plane | 多 LUN/多 plane/多通道 |
| 校验 | 基本 | 严格，失败拒绝启动 |
| 版本/格式 | 自动/简单 | 版本化、可迁移/回滚 |

#### 4.1.7 当前平台

- 启动打印 `ONFI sig/model/page/block/LUN/timingmodes`；
- `[BLK] bad areas`、`[CAP]`、`[FTL] boot type`；
- ONFI 1.0、Mode 5、BCH(13,4)、512 MiB、1 LUN、2 plane；
- format/version、full rebuild fallback。

#### 4.1.8 选型 / 学习点

- 选型：NAND 是否有 ONFI 参数页？参数页是否可靠？几何/时序是否可动态配置？
- 学习点：启动链、ONFI、几何、format/version、BBT/坏块。

**状态：已定稿。**

### 4.2 BBT（bad block table）/ 备用块 / 工厂坏块 vs 运行期坏块

#### 4.2.1 坏块从哪来

| 来源 | 典型原因 | 处理 |
|---|---|---|
| 工厂坏块 | 出厂标记（OOB/BBT） | 信任/扫描后记入 BBT |
| 编程失败 | program fail / 超时 | 标记 BAD、替换 |
| 擦除失败 | erase fail / 超时 | 标记 BAD、替换 |
| 读失败 | UNC/ECC 不可纠 | 先 read retry/恢复，仍失败才标 BAD |
| 读干扰/保持性 | 长期读/放 | read scrub / 重写 / 退役 |
| 元数据损坏 | BBT/CP/Directory | 双副本/重建 |

**关键**：坏块标记是**永久状态**，一旦标 BAD 就不能再分配；误标会永久损失容量。

#### 4.2.2 BBT 存哪、怎么冗余

| 方案 | 优点 | 代价/风险 |
|---|---|---|
| 专用 BBT 块 | 简单、集中 | 占容量；块本身会坏 |
| 每块 OOB/备用区 | 分散、随块 | 需要扫描；OOB 空间/ECC 取舍 |
| 双副本 + 版本/CRC | 抗单点损坏 | 写放大、恢复逻辑 |
| 重建（scan） | 不依赖旧表 | 慢；可能误判已写块 |

**策略**：双副本 + sequence/CRC；坏一份用另一份；两份都坏则扫描重建。
**取舍**：冗余/容量 vs 恢复速度/复杂度。

#### 4.2.3 工厂坏块 vs 运行期坏块

- **工厂坏块**：出厂标记可信度高，但二手片/旧片可能丢失或不可信；
- **运行期坏块**：按失败类型和阈值标记，避免把瞬态错误永久标坏；
- **信任 vs 扫描**：信任工厂表快，扫描更可靠但慢；
- **阈值**：连续失败 N 次才标 BAD，还是首次失败就标？企业级更保守（避免误标），
  消费级更简单。

#### 4.2.4 备用块 / 替换策略

- 每个 plane 预留 spare（本仓 `spare=100`，OP 池另有预留）；
- 坏块替换从 spare 取，spare 不足从 free/OP 补；
- 策略：per-plane 平衡、避免某 plane 先耗尽；
- 代价：spare 越大，用户容量越小；越小，坏块多了会提前写失败。

#### 4.2.5 BBT 重建

- 触发：BBT 双副本都坏、元数据丢失、旧介质无有效 BBT；
- 做法：扫描所有块的标记/写入状态，重建 BBT；
- 风险：**把已写块误判为空块**（`bbt_medium_has_ftl_data` 的判据），
  或把瞬态读失败当坏块；
- 取舍：重建慢且危险，优先双副本/版本化。

#### 4.2.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| BBT | 基本、单/双副本 | 双副本 + 版本/CRC、telemetry |
| 工厂坏块 | 信任/简单扫描 | 严格扫描/校验 |
| 运行期坏块 | 简单阈值 | 分类、read scrub、预测退役 |
| spare | 较少 | 更多、per-plane |
| 重建 | 简单 | 可审计/可恢复 |

#### 4.2.7 当前平台

- BBT 在 allocator；启动 `[BLK] bad areas ... data=2 total=2`；`bad_count=2`；
- spare 池 100；OP 池 404/407；per-plane 预留；
- CLI `bbt` 是**探针/诊断**，不是坏块图（坏块图在 allocator）；
- `bbt_save` / `bbt_format_erase` / `bbt_factory_scan`；`FORMAT_IN_PROGRESS`；
- 运行期标坏：program/erase/read 失败；read recovery 走重试入口，但本器件对应 Feature
  地址保留 ⇒ 实际空转（见 4.12.4）；
- 元数据丢失走 full rebuild。

#### 4.2.8 选型 / 学习点

- 选型：NAND 是否有工厂坏块表？OOB 是否够？BBT 放哪？spare/OP 多少？
- 学习点：坏块生命周期、BBT 冗余、工厂 vs 运行期、spare 策略、重建风险。

**状态：已定稿。**

### 4.3 L2P（logical-to-physical）映射粒度与结构

#### 4.3.1 映射粒度谱系

| 粒度 | 表大小 | 写放大 | 适用 | 代价 |
|---|---|---|---|---|
| 块级 | 最小 | 高（整块重写） | 顺序/简单 FTL | 随机写差 |
| 页级 | 中 | 中 | 主流 SSD | 表较大 |
| CW/子页级 | 大 | 低（小写不放大） | 小写/ECC 粒度细 | 表最大、cache 压力 |
| 混合（FASTer） | 中 | 中 | 块级 + 页级 log | 复杂 |
| DFTL | 中（DRAM 小） | 中 | DRAM 小/大容量 | 查表要读 NAND |

**本仓：CW 级（IU=512 B）**，与 ECC CW 对齐，小写不放大，但表很大。

#### 4.3.2 映射表结构

典型结构：

- **L2P 表**：LBA/CW → PBA（页/块/plane/CW）；
- **P2L（physical-to-logical）/ 反向信息**：GC/重建时从物理页反查 LBA；可放页 META 或独立表；
- **valid bitmap/count**：块/页有效数据；
- **block descriptor**：状态、温度、擦除计数、SB（super block，多 plane 块组）归属；
- **region/segment**：把大表分片，便于 cache/持久化。

本仓：L2P region cache + 页 META（CW LBA）+ valid_count；没有独立全量 P2L 表。

#### 4.3.3 表大小与驻留

```text
表大小 ≈ 容量 / 粒度 × 表项大小

423 MiB / 512 B × 4 B ≈ 3.3 MiB
```

当前平台只有 1060 KB SRAM ⇒ **全表放不下** ⇒ region cache + journal + checkpoint。
企业级 4 TB / 4 KB × 4 B = 4 GB ⇒ 必须 DRAM；消费级 DRAM-less/HMB ⇒ cache/DFTL。

#### 4.3.4 更新与持久化

写入时：

```text
host 写 → L2P 更新（cache / CMT，cached mapping table）→ journal 记录映射 → 后续 checkpoint 落盘
```

- 不能每次写都原地更新 NAND 表（写放大/掉电）；
- 用 **journal/WAL + checkpoint** 批量持久化；
- 掉电时 replay journal，必要时 full rebuild；
- 取舍：更新频率/写放大 vs 掉电窗口/恢复时间。

#### 4.3.5 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 粒度 | 页/CW | 页级/混合 |
| 表驻留 | DRAM-less/HMB、region cache | DRAM 全表 |
| 查表 | cache/DFTL，可能读 NAND | DRAM 查表 |
| 持久化 | journal/CP | journal/CP + PLP |
| 随机写 | 可接受放大 | 稳态性能优先 |

#### 4.3.6 当前平台

- CW 级（IU=512 B）L2P；L2P region cache；
- 表放不进 1 MB SRAM ⇒ region cache + journal + checkpoint；
- 页 META 存 CW LBA，用于 GC/反查；
- CMT（dirty 映射）→ journal → checkpoint；
- 无独立全量 P2L 表；full rebuild 时扫页 META。

#### 4.3.7 选型 / 学习点

- 选型：容量/粒度决定表大小；SRAM/DRAM/HMB 决定驻留；NAND 是否支持页 META/反查。
- 学习点：映射粒度、表大小数学、region cache、journal/CP、P2L/反查、WAF。

**状态：已定稿。**

### 4.4 L2P 驻留（DRAM / DRAM-less / HMB）

#### 4.4.1 四种驻留方案

| 方案 | L2P 表放哪 | 优点 | 代价 |
|---|---|---|---|
| DRAM-based | DRAM 全表 | 查表快、实现简单、稳态性能好 | DRAM 控制器/ECC、成本/功耗 |
| DRAM-less | NAND + SRAM cache | 成本/功耗低 | cache/thrashing/journal/CP 复杂 |
| HMB | 主机 DRAM（NVMe HMB） | 消费级 NVMe 常用 | 依赖主机支持；延迟/一致性 |
| Hybrid | 小 DRAM + NAND/cache | 折中 | 设计复杂 |

#### 4.4.2 DRAM-based

- 全表常驻 DRAM；查表 O(1)，不用读 NAND；
- 适合大容量/企业级；需要 DRAM 控制器 + ECC；
- 代价：BOM/功耗/板面积；掉电要 PLP 或 journal/CP；
- 学习点：表大小、DRAM 带宽、ECC。

#### 4.4.3 DRAM-less

- 表主要放 NAND，SRAM 只放 **region/segment cache**；
- 查表：cache hit 直接返回；miss 读 NAND region；
- 难点：cache 命中率、thrashing、脏行写回、journal/checkpoint；
- 本仓：CW 级表约 3.3 MiB，放不进 1 MB SRAM ⇒ region cache + journal/CP；
- 代价：随机读延迟、cache 管理复杂；好处：成本/功耗低。

#### 4.4.4 HMB

- NVMe Host Memory Buffer：设备通过 PCIe 用主机 DRAM 存 L2P/缓存；
- 消费级 NVMe 常见，DRAM-less 的一种；
- 依赖主机分配 HMB、驱动支持；延迟/带宽受 PCIe；
- 不是所有主机/OS 都启用；不能当唯一方案。

#### 4.4.5 取舍

| 维度 | DRAM-based | DRAM-less | HMB |
|---|---|---|---|
| 成本 | 高 | 低 | 低（借主机） |
| 查表延迟 | 低 | 可能读 NAND | 中（PCIe） |
| 实现复杂度 | 低 | 高 | 中 |
| 稳态性能 | 好 | 依赖 cache | 依赖主机 |
| 掉电 | PLP/CP | journal/CP | journal/CP |
| 适用 | 企业/大容量 | 消费/移动/低成本 | 消费 NVMe |

#### 4.4.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 驻留 | DRAM-less/HMB 或小 DRAM | DRAM + ECC |
| 表大小 | 小~中 | 大 |
| 查表 | cache/DFTL | DRAM 全表 |
| 稳态 | 可接受抖动 | P99.9 |
| 成本/功耗 | 敏感 | 不敏感 |

#### 4.4.7 当前平台

- 板载 **32 MB SDRAM 未使用**；片上 1060 KB SRAM；
- **主动 DRAM-less**：学 DRAM-less 分支 + 先做小再扩；
- region cache + journal + checkpoint；CMT 管理脏映射；
- 代价：SDRAM 闲置、表放不下、随机读延迟；好处：成本/功耗/学习价值。

#### 4.4.8 选型 / 学习点

- 选型：是否有 DRAM 控制器/ECC？SRAM 多大？是否支持 HMB？
- 学习点：表大小数学、cache 命中/驱逐、thrashing、journal/CP、HMB 限制。

**状态：已定稿。**

### 4.5 L2P cache / thrashing

#### 4.5.1 缓存什么

L2P cache 缓存的是**映射表的一部分**（region/segment/page 的 L2P 条目），不是用户数据：

```text
读/写 LBA
  → L2P cache 查
      hit  → 直接拿到 PBA
      miss → 读 NAND 里的映射 region → 装入 cache → 驱逐旧行
```

- 缓存粒度：region/segment（本仓 region cache）；
- 行状态：INVALID / LOADING / CLEAN / DIRTY / FLUSHING；
- 脏行（dirty）要写回 NAND 或 journal/checkpoint；
- 缓存大小决定命中率、随机读延迟、SRAM 压力。

#### 4.5.2 命中/缺失/驱逐

| 事件 | 含义 | 代价 |
|---|---|---|
| hit | 条目在 cache | 低延迟 |
| miss | 读 NAND region | 一次 NAND 读 |
| eviction | 腾位置 | clean 直接丢；dirty 要写回 |
| thrashing | 反复驱逐刚用的行 | 命中率崩、延迟升 |

**关键指标**：hit rate、miss count、eviction count、dirty writeback、thrashing 降级。

#### 4.5.3 替换算法

| 算法 | 特点 | 代价 |
|---|---|---|
| LRU | 直观 | 扫描会污染；实现成本 |
| CLOCK | 近似 LRU、低开销 | 抗扫描一般 |
| CLOCK-pro | 区分 hot/cold | 复杂 |
| ARC/2Q | 抗扫描/自适应 | 复杂、参数多 |
| 自适应 | 按命中率调 cache 大小 | 需要统计/调参 |

**本仓：CLOCK + thrashing 降级**；命中率/驱逐/降级都有计数。

#### 4.5.4 thrashing 与抗扫描

- 顺序扫描大范围 LBA 会把 cache 里的热行全部冲掉；
- 策略：
  - 引用位/二次机会（CLOCK）；
  - 区分 scan-resistant（hot/cold）；
  - 顺序流 hint/预取，避免污染；
  - 降级：thrashing 时减少 cache 占用/走直达路径；
- 取舍：抗扫描复杂度 vs 顺序读性能；企业级更关注尾延迟，消费级更关注成本。

#### 4.5.5 写回与掉电

- dirty 行不能直接丢；写回 NAND 或 journal/checkpoint；
- 写回策略：write-back（延迟低，掉电窗口） vs write-through（安全，写放大）；
- 本仓：CMT dirty → journal → checkpoint；掉电 replay/rebuild；
- 取舍：写回频率/写放大 vs 掉电窗口/恢复时间。

#### 4.5.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 缓存大小 | 小（SRAM） | 大（DRAM） |
| 算法 | CLOCK/自适应 | 更复杂、QoS |
| thrashing | 需降级 | 稳态性能优先 |
| 写回 | journal/CP | journal/CP + PLP |
| 指标 | 基本 | telemetry/尾延迟 |

#### 4.5.7 当前平台

- L2P region cache；CLOCK + thrashing 降级；
- CMT 管 dirty 映射；journal/checkpoint 持久化；
- 统计：hit/miss、eviction、CMT hit/miss、wal_defer、hint hit/miss、prefetch；
- 表放不进 SRAM ⇒ cache 命中率直接决定随机读性能；
- 取舍：小 SRAM + DRAM-less，必须做 cache/降级。

#### 4.5.8 选型 / 学习点

- 选型：SRAM/DRAM 大小、cache 行数、算法复杂度、是否需要 anti-scan/telemetry。
- 学习点：hit/miss/eviction、CLOCK、thrashing、dirty writeback、journal/CP、指标口径。

**状态：已定稿。**

### 4.6 GC（垃圾回收）

#### 4.6.1 GC 做什么

NAND 不能原地覆写：旧数据变成 invalid 后仍占着块，必须把**部分有效块**里的有效页搬到新块，
再擦除 victim，把空块还给 free pool。

```text
选 victim（部分有效块）
  → 读有效页
  → 写新块
  → 更新 L2P / journal
  → 擦 victim
  → 归还 free pool
```

GC 的本质是**用写放大换空闲块**。没有 GC，free pool 只减不增，写满即卡死。

#### 4.6.2 触发条件

| 触发 | 含义 | 典型策略 |
|---|---|---|
| 水位（watermark） | free 低于目标水位 | 后台补池，平滑 WAF |
| 紧急（emergency） | free 接近耗尽 | 允许高 WAF，优先保命 |
| host 写 | 分配不到可用页对 | 前台/urgent GC，host 感知到停顿 |
| idle / periodic | 空闲期预补池 | 降低 host 抖动 |
| WL | 磨损均衡搬移 | 与空间 GC 是**不同目的**（见 4.7） |

**取舍**：触发太早 ⇒ 写放大、寿命损耗；太晚 ⇒ host 写被 GC 卡住、尾延迟飙升。
企业级通常保留较大 OP + 更早的后台 GC；消费级更愿意「用到快满再收」。

#### 4.6.3 victim 选择

| 指标 | 作用 |
|---|---|
| valid ratio（有效页占比） | **核心指标**：越低，搬得越少、回收越划算 |
| erase count | 避开接近寿命上限的高磨损块 |
| temperature | 热数据自然死亡快，优先回收 |
| age | 保证老块不被永久跳过（防饿死） |
| plane / die balance | 避免单 plane 先耗尽，保住并行度 |

常见策略：greedy（只挑 valid 最低）、加权评分、cost-benefit（收益/代价比）、
以及按 plane/通道分组的批量回收。

**取舍**：greedy 简单但容易抖动（刚写完的块又被选）；加权评分稳态更好，
但权重是经验值，换负载要重调。评分开销本身也要算进 GC 成本。

#### 4.6.4 搬移、预擦除与批处理

- **搬移**：读有效页 → 写新页 → 更新 L2P；期间掉电必须能回放/重建。
- **预擦除池（pre-erase pool）**：提前擦好一批空块，GC 只搬不擦，显著降低尾延迟；
  代价是后台擦除功耗与复杂度（企业级常见）。
- **批处理（batch GC）**：一次提交多页，摊薄命令/引擎启动开销。
- **copy-back**：器件支持时片内搬页，省数据往返；对 multi-plane/cache 时序有约束。
- **失败处理**：读校验失败/元数据损坏的 victim **不得擦除**，否则 L2P 悬空 = 静默丢数据。

#### 4.6.5 GC 与 host / 后台的协同

- **host 优先**：host 读写不能被 GC 饿死；GC 是「有空才做」的工作。
- **收益判据**：host 正在写时少筛/不筛；idle 时只回收值得回收的块，避免净收益为负的搬移。
- **no-gain 退避**：满盘且无可回收块时，反复扫描是纯空转 ⇒ 退避后重试。
- **序列门控/背压**：GC 与 host 序列共享命令池/SQ，需排队与背压，避免池耗尽。
- **QoS**：企业级要求 GC 不破坏 P99.9；消费级允许更大的延迟抖动。

#### 4.6.6 取舍 / WAF

| 选择 | 收益 | 代价 |
|---|---|---|
| 更大 OP | 降低 WAF、稳态更好 | 用户容量减少 |
| 更激进 GC | free 池稳定 | 写放大 / 寿命 |
| 更保守 GC | 写少、寿命长 | 尾延迟 / 卡写 |
| 预擦除池 | 尾延迟低 | 后台功耗 / 复杂度 |
| 复杂 victim 评分 | 稳态 WAF 低 | 调参 / CPU 开销 |
| 批处理 | 吞吐高 | 锁存/缓冲占用 |

WAF = 实际写入 NAND 的数据量 / host 写入量；GC 搬移是 WAF 的主要来源，
稳态 WAF 与 OP、victim 选择、负载随机性直接相关。

#### 4.6.7 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| OP | ~7% | ~28%（常见区间） |
| GC 抖动 | 可容忍 | P99.9 尾延迟硬约束 |
| 预擦除池 | 少见 | 常见 |
| 读优先 | 基本 | 强制（GC 不阻塞 host 读） |
| 目标 | 成本 / 峰值 | 稳态 / 寿命 / QoS |

#### 4.6.8 当前平台

- 触发：T1 水位 + T4 紧急，位掩码可同时命中；求值有节拍门（`GC_EVAL_INTERVAL_MS`）。
- victim：四项加权评分（valid / erase / temp / age），并带收益判据：
  idle 只收 valid 占比不高的块，emergency 要求可回收占比达到下限。
- host 写窗口：2 s 内视为 host 正在写 ⇒ 不筛收益；idle 才筛。
- 搬移：逐 plane 读→写，完成后逐 plane 擦；读/元数据损坏 ⇒ 中止且**不擦 victim**。
- 批处理：`MIG_BATCH_PAGES = 4`，有 batch job/page/abort 统计。
- no-gain 退避；GC 与静态 WL 分开；当前单 LUN、2 plane，无多通道并行 GC。
- **没有独立 pre-erase pool**：擦除发生在搬移完成后（ERASE 阶段），这是与主流企业级的一处差距。
- 统计：reclaimed / moved / noop / novictim / nogain、job start/end/abort、batch。

#### 4.6.9 选型 / 学习点

- 选型：OP 比例、触发水位、victim 评分、是否要预擦除池/批处理、QoS 目标、WAF 目标。
- 学习点：GC 生命周期、victim 选择、WAF 与 OP、稳态/写悬崖、GC 与 WL 的分工、
  预擦除为什么能降尾延迟。

**状态：已定稿。**

### 4.7 WL（磨损均衡）

#### 4.7.1 GC 与 WL 的分工

两者都走「搬移」，但目的不同：

| | GC | WL |
|---|---|---|
| 目的 | 回收空间 | 摊平磨损 |
| victim | 有效页**少**的块 | 擦除次数**少**的块 |
| 触发 | free 不足 | 磨损差值超阈值 |
| 代价 | 写放大 | 额外搬移（WAF 上升） |

主流实现里两者共用搬移引擎（migration job），按 kind 区分；本仓同样如此。

#### 4.7.2 磨损指标

| 指标 | 用途 |
|---|---|
| erase count / P/E cycles | 主指标 |
| max − min 擦除差值 | WL 触发判据 |
| 平均 / 最大擦除次数 | 寿命预测、telemetry |
| 坏块增长 | 磨损恶化信号 |
| plane / channel 分布 | 避免局部先耗尽 |

**取舍**：只看 max 会忽略分布；只看平均会掩盖热点。企业级通常同时报 max/mean/spread。

#### 4.7.3 动态 WL（Dynamic WL）

分配新块时**优先选擦除次数最少的空闲块**：

- 实现位置：allocator 的 free 分配路径；
- 本仓：每个 plane 各自选该 plane 擦除最少的块（各 plane 独立分配，不要求块同号）；
- 收益：新写入自然摊平，几乎零额外搬移；
- 代价：分配路径要维护/查询擦除计数；多 plane 下要按 plane 独立平衡。

**取舍**：动态 WL 便宜有效，但只影响「新分配」；长期冷数据所在块永远不被擦除 ⇒ 必须配静态 WL。

#### 4.7.4 静态 WL（Static WL）

主动把**低擦除块里的冷数据**搬走，让低擦除块空出来接收热数据：

```text
存在「高擦除空闲块」显著旧于「最冷 used 块」？
  → 搬冷数据到高擦除空闲块
  → 原低擦除块归还 free pool（之后被动态 WL 优先选中）
```

主流的 UBI 模型：`max_free_ec − min_used_ec ≥ threshold` 才触发；
threshold 是磨损差值，不是固定周期。

**取舍**：静态 WL 是纯成本（额外搬移、WAF 上升），不做则冷块永远不老；做了要有明确停止条件。

#### 4.7.5 冷热识别

| 手段 | 说明 |
|---|---|
| 温度标记（temp） | 写入时按流/热度打 HOT/COLD |
| 写计数 / 命中 | 统计 LBA 区域写频度 |
| read disturb 计数 | 冷块读多写少，搬移优先级 |
| host hint / stream | 多流、namespace 提示 |

本仓：块级温度 + GC 评分里的 temp 权重；WL victim 还参考 valid CW 与 read 计数。

#### 4.7.6 触发与节拍

- **启动门**：累计编程次数达到下限才开始求值（避免刚启动就搬）；
- **求值节拍**：WL 是慢过程，周期性扫描即可（本仓 5 s）；
- **停止条件**：差值低于阈值即停，不追求「绝对相等」；
- **与 GC 协同**：共用迁移引擎，任一时刻只跑一个 job。

#### 4.7.7 与 OP / 预留块 / 坏块的关系

- OP 越大，WL 可腾挪空间越大，磨损越平；
- 预留块（spare）用于坏块替换，不参与 WL 计算；
- 坏块 / 预测性退役块要从磨损统计中排除；
- 磨损表要持久化，否则掉电后 WL 失忆。

#### 4.7.8 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 重点 | 成本 / 寿命够用 | DWPD（drive writes per day）、稳态寿命 |
| OP | ~7% | ~28% |
| 静态 WL | 简单 / 低频 | 精细、常开 |
| 指标 | 基本磨损 | max/mean/spread、telemetry |
| 数据温度 | 少 | 多流 / 热区分层 |

#### 4.7.9 当前平台

- **动态 WL**：allocator 分配时按 plane 选擦除最少的空闲块；
- **静态 WL**：UBI 模型，`max_free_ec − min_used_ec ≥ WL_THRESHOLD(200)`；
  启动门 1000 次编程，valid CW ≥ 70%，5 s 求值；
- victim = 最冷 used SB（同擦除按 valid/read 排序）；搬移后源块归还 free pool；
- 磨损计数持久化（wear table）；CLI `wear` 与触发判据共用同一口径；
- GC victim 评分含 erase 权重，避免挑临近寿命块；
- 取舍：单 LUN / 2 plane，WL 无跨通道并行；不退役（退役只属预测性退役 / RR）。

#### 4.7.10 选型 / 学习点

- 选型：P/E 预算、OP、WL 阈值、静态 WL 频率、磨损表持久化、telemetry 口径。
- 学习点：动态 vs 静态 WL、UBI 差值模型、冷热识别、WL 与 GC 的 WAF 代价、
  磨损统计排除坏块 / 预留块。

**状态：已定稿。**

### 4.8 掉电恢复（PFR, Power-Fail Recovery）

#### 4.8.1 掉电窗口与失败模型

掉电可能发生在任意时刻，恢复必须覆盖：

| 失败形态 | 结果 |
|---|---|
| 页写一半（torn page） | 数据 / 元数据不完整 |
| 块擦一半 | 块状态未知 |
| L2P 已更新、数据未落盘 | 映射悬空 |
| 数据已落盘、L2P 未更新 | 数据不可见 |
| CP / journal 写一半 | 元数据损坏 |
| 多个写并发掉电 | 次序错乱 |

设计目标：**要么恢复到某个一致点，要么安全重建**，绝不带病启动。

#### 4.8.2 需要持久化什么

| 数据 | 粒度 | 掉电后果 |
|---|---|---|
| L2P 映射 | journal / checkpoint | 丢映射 = 丢数据 |
| CMT dirty 行 | journal | 同上 |
| block 状态（open/closed/free） | CP / 扫描 | 账本错 = 漏块 / 双分配 |
| write pointer | 页头 / 扫描 | 覆盖写 = 静默损坏 |
| bad block / wear | BBT / wear table | 坏块误用、WL 失忆 |
| 配置 / 版本 | 固定区 + 双副本 | 拒绝启动 / 误格式化 |

原则：**不可重建的必须持久化；可扫描重建的可以不持久化**（用恢复时间换写入成本）。

#### 4.8.3 写次序与原子性

- **WAL / journal 先行**：先记「要改成什么」，再改数据；回放幂等。
- **checkpoint**：定期把 journal 状态压实成快照，缩短回放长度。
- **双副本 / 交替写**：CP 或目录区写两份，坏一份还有一份。
- **序号（sequence / generation）**：判断新旧，避免回放旧 journal。
- **页级原子性**：依赖 ECC + 页头标记；torn page 必须能识别为无效。

**取舍**：journal 越频繁越安全，但写放大和延迟越高；CP 越频繁恢复越快，但写放大越大。

#### 4.8.4 恢复流程

```text
上电
  → 读配置 / 版本（双副本）
  → 找最新有效 CP
     有 → restore 快照
     无 → full metadata rebuild（全 NAND 扫描）
  → replay journal（只放未被 CP 覆盖的条目）
  → 重建 free / closed / open 账本
  → 校验不变量（无重复、无悬空）
  → 正常服务
```

- **增量恢复**（CP + journal）快；**全量重建**（扫描 P2L / page header）慢但兜底。
- 恢复路径必须能处理「CP 损坏」「journal 损坏」「扫描到 torn page」。

#### 4.8.5 幂等与部分写

- journal 条目必须**可重复回放**（同一条回放两次结果一致）；
- 部分写页要能判定为无效（ECC / 页头 / 序号）；
- 擦除中断的块不能假定为空，需重擦或标坏；
- 恢复过程中再次掉电：下次仍要能继续（恢复本身也要崩溃安全）。

#### 4.8.6 PLP 与无 PLP

| | 有 PLP（电容） | 无 PLP |
|---|---|---|
| 掉电窗口 | 可把 cache / 缓冲写完 | 只能靠 journal / CP |
| 成本 | 电容 + 电源管理 | 低 |
| 适用 | 企业级 | 消费级 / 本仓 |

本仓无 PLP；写确认依赖 VWC 早回执 + journal / CP 的撤销与回放。

#### 4.8.7 恢复时间与启动策略

- **fast boot**：只 replay 少量 journal，后台再做校验 / 重建；
- **lazy rebuild**：先服务、逐步补账本（风险：期间不能分配未确认块）；
- **CP 频率**：恢复时间 vs 写放大；
- **恢复上限**：企业级要求有界启动时间，消费级可接受更长。

#### 4.8.8 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| PLP | 通常无 | 有 |
| 恢复时间 | 可较长 | 有界、可预测 |
| journal / CP | 基本 | 更频繁、双副本 |
| 验证 | 正常掉电 | 故障注入、随机掉电 |

#### 4.8.9 当前平台

- 恢复计划：FRESH（无 CP → rebuild）/ SNAPSHOT（干净关机）/ REPLAY（有未回放 journal）/
  FORCE_REBUILD（运行期日志丢失）；决策集中在 `pfr_boot_plan`。
- journal 回放 + checkpoint 快照；CP / 目录双副本（介质版本 12）。
- CMT dirty 行走 journal；VWC 早回执，掉电时按 FAILC（cache program 失败状态位）撤销未落盘写。
- 全量重建兜底：扫 closed SB、写指针、块元数据；恢复后校验账本。
- 无 PLP；恢复时间受全 NAND 扫描影响（仅兜底路径）。
- 已知缺口（登记在 `known_open_items.md`）：USB 写过程中掉电恢复未验（ISS-USB-01）；
  CP 块快照 word 不含 `sb_id`，恢复后的 closed 条目可能单 plane（ISS-FTL-17 / RISK-ALLOC-09）；
  VWC 早回执的掉电暴露窗口未量化（RISK-CACHE-18）。
- 取舍：单 LUN / 软件 FC，恢复串行；已实现异常掉电 replay，但缺随机掉电注入的板端证据。

#### 4.8.10 选型 / 学习点

- 选型：是否需要 PLP、journal / CP 频率、恢复时间上限、双副本、故障注入能力。
- 学习点：WAL、checkpoint、幂等回放、torn page、全量重建兜底、
  「不可重建才持久化」的取舍。

**状态：已定稿。**

### 4.9 OP / WAF

#### 4.9.1 定义与口径

- **OP（over-provisioning）**：物理容量中**不暴露给 host** 的部分，用作 GC 腾挪、
  坏块替换、元数据、WL 余量。
- **WAF（write amplification factor）**：实际写入 NAND 的数据量 / host 写入量。

```text
OP  = (物理可用块 − 用户可见块) / 物理可用块
WAF = NAND 编程量 / host 写量
```

**口径**：OP 按块算还是按字节算要写明；WAF 是稳态值还是峰值要写明。

#### 4.9.2 OP 的来源

| 来源 | 用途 |
|---|---|
| 厂商预留 | 坏块、元数据、固件 |
| 用户配置 OP | GC 余量、延长寿命 |
| spare pool | 坏块替换 |
| 元数据区 | BBT / CP / journal / L2P log |

本仓：`FTL_OP_PERCENT=10%` 的 OP 池 + 每 plane 50 块 spare + 固定元数据区。

#### 4.9.3 WAF 的构成

| 来源 | 说明 |
|---|---|
| host 写 | 基线 |
| GC 搬移 | 主要放大源 |
| WL 搬移 | 静态 WL 的额外成本 |
| 元数据 | journal / CP / L2P log |
| ECC / RAID | 冗余字节（通常不计入 WAF，但要说明口径） |

WAF 随负载随机性、OP、GC 策略变化；**顺序写 WAF 接近 1，随机写稳态 WAF 明显大于 1**。

#### 4.9.4 OP 与稳态（write cliff）

- OP 越小，free pool 越紧，GC 越频繁 ⇒ WAF 上升；
- 到某个点后 WAF 急剧上升 = **write cliff**（写悬崖）；
- OP 足够大时，GC 有腾挪空间，稳态 WAF 可压到较低；
- OP 不是越大越好：直接减少用户容量。

**取舍**：容量 vs 寿命 / 稳态性能。

#### 4.9.5 与其他机制的交互

- **SLC cache**（见 4.10）：把 TLC/QLC 当 SLC 用会临时消耗空间，退出时集中搬移；
- **WL**（4.7）：静态 WL 是额外 WAF；
- **GC 触发**（4.6）：水位越保守，WAF 越低但尾延迟风险越高；
- **预留块耗尽**：坏块替换会吃掉 OP，稳态容量随之下降。

#### 4.9.6 测量

- 需要同时统计 **host 写字节** 与 **NAND 编程字节**；
- 用稳态区间（而非启动瞬间）计算 WAF；
- 区分 host 写、GC 搬移、WL 搬移、元数据四类来源；
- 企业级通常把 WAF 作为 telemetry 一等指标。

本仓：host 字节有统计；WAF 尚无独立一等指标（可由编程量 / 主机量估算），属于观测缺口。

#### 4.9.7 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| OP | ~7% | ~28% |
| 目标 | 容量 / 成本 | 稳态 WAF / DWPD |
| WAF 观测 | 少 | 一等指标 |
| write cliff | 可接受 | 必须避免 |
| 超额配置 | 少 | 常见（SLC cache、预留） |

#### 4.9.8 当前平台

- OP 池 = 每 plane 10%（`FTL_OP_PERCENT`），另有每 plane 50 块 spare + 固定元数据区；
- 用户容量由 `FTL_HOST_DATA_BLOCKS_PER_INSTANCE` 推导，编译期上界；
- GC 水位 `FREE_POOL_TARGET=32`、`FREE_POOL_EMERGENCY=8`；
- host 读写字节有统计（lifetime / stats）；WAF 未单独成指标；
- 取舍：DRAM-less + 10% OP，靠 region cache / GC 策略维持稳态；OP 偏小，满盘随机写更容易接近 write cliff。

#### 4.9.9 选型 / 学习点

- 选型：OP 比例、spare 数量、元数据开销、WAF 目标、是否需要 WAF telemetry。
- 学习点：OP 与 WAF / write cliff 的关系、GC/WL/元数据对 WAF 的贡献、
  稳态测量口径、容量 vs 寿命的取舍。

**状态：已定稿。**

### 4.10 SLC cache

#### 4.10.1 为什么需要

TLC/QLC 编程慢、寿命低、读延迟高；主控把一部分空间按 **SLC 模式（pSLC）** 使用：

- 吸收 host 突发写（burst），先快后慢；
- 降低稳态写延迟；
- 提高短时寿命（SLC P/E 远高于 TLC/QLC）。

```text
host 写 → SLC cache（快）→ ack
cache 满 / 空闲 → folding（搬进 TLC/QLC）→ 释放 cache
```

#### 4.10.2 形态

| 形态 | 说明 | 取舍 |
|---|---|---|
| 静态 SLC cache | 固定划出一块只做 SLC | 容量固定、可预测 |
| 动态 SLC cache | 从 TLC 空闲空间借用 | 容量弹性大，满盘时缩水 |
| 混合 | 静态 + 动态 | 复杂 |

**动态 cache 的容量随占用率下降**：盘越满，SLC cache 越小，写性能越早掉下来。

#### 4.10.3 写路径与退出策略

- host 写先落 SLC cache，映射指向 SLC 页；
- cache 低于低水位 → 停止接收新数据 / 直接写 TLC；
- 空闲时后台 **folding**：把 SLC 数据搬进 TLC，更新映射，回收 SLC 块；
- 顺序 / 流式写可直接绕过 cache（write-through），避免 folding 放大；
- cache 满且无法 folding 时，host 写要等 ⇒ 尾延迟尖峰。

#### 4.10.4 读路径

- 先查映射：指向 SLC 还是 TLC；
- SLC 读快、TLC 读慢；folding 后映射更新，旧 SLC 页失效；
- 读放大通常不增加，但 folding 会占用带宽。

#### 4.10.5 掉电与一致性

- SLC cache 的映射必须进 journal / CP，否则掉电丢映射；
- folding 必须是「写新 → 更新映射 → 释放旧」的原子次序；
- 掉电恢复可以：
  - 回放 journal 恢复到 folding 前后的一致点；
  - 或扫描识别 SLC/TLC 页并重建映射；
- SLC cache 不等于 PLP：它提高性能，不消除掉电窗口。

#### 4.10.6 与 OP / GC / WL 的交互

- 动态 SLC cache 吃掉 OP / 空闲空间 ⇒ 稳态 free pool 更紧；
- folding 是额外写放大，算进 WAF；
- SLC 块磨损快（写入集中），需要 WL 照顾；
- 盘满时 SLC cache 缩水 + GC 压力叠加 = 写性能悬崖。

#### 4.10.7 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| SLC cache | 常见（动态为主） | 少用；多用 pSLC / 固定分区 |
| 目标 | 峰值 / 突发性能 | 稳态、可预测 |
| folding | 后台集中搬 | 更严格 QoS / 写放大控制 |
| 满盘行为 | 性能下降明显 | 不允许不可预测的悬崖 |

企业级更倾向 **write-through / 固定 pSLC / 直接写 TLC**，避免「先快后慢」的不可预测性。

#### 4.10.8 当前平台

- MT29F4G08 是 **SLC NAND**，本身没有 TLC/QLC 的 folding 问题 ⇒ **不需要 SLC cache**；
- 本仓的 VWC（volatile write cache）是**易失写缓存**，不是 SLC cache：
  它提前回执、掉电可撤销，与 pSLC 的「非易失快速层」不是同一机制；
- 若将来换 TLC/QLC 平台，SLC cache 会成为写路径的一等模块（静态/动态、folding、水位）；
- 取舍：当前平台不引入 SLC cache 的复杂度和额外 WAF。

#### 4.10.9 选型 / 学习点

- 选型：介质类型（SLC/MLC/TLC/QLC）、是否需要 pSLC、静态/动态、folding 策略、满盘行为目标。
- 学习点：pSLC 原理、动态 cache 缩水、folding、write-through vs write-back、
  SLC cache 与 OP/WAF/掉电的关系。

**状态：已定稿。**

### 4.11 写路径（write path）

#### 4.11.1 端到端路径

```text
host 命令（HIL）
  → 解析 / 校验 / 拆 LBA
  → L2P 查旧映射
  → 分配新页（open SB / plane 平衡）
  → 数据传输（DMA / PIO）
  → NAND program
  → 更新 L2P（journal / CMT）
  → 完成回执（CQ / IRQ / 轮询）
```

每一段都可能掉电，次序决定一致性（见 4.8）。

#### 4.11.2 回执语义

| 语义 | 含义 | 掉电窗口 |
|---|---|---|
| write-back | 写进缓存即回执 | 有 |
| write-through | 落到介质才回执 | 小 |
| FUA（Force Unit Access） | 单命令强制落盘 | 无（该命令） |
| barrier / flush | 前序全部落盘再回执 | 无（前序） |
| 早回执（VWC） | 映射可撤销前提下先回执 | 有，靠撤销 / 回放 |

**取舍**：回执越早，host 性能越好，掉电暴露越大。企业级常用 PLP 支撑 write-back。

#### 4.11.3 写粒度与对齐

- host 写可以跨页、跨 block、跨 plane；
- CW 级映射（本仓 IU=512 B）会把一次 host 写拆成多个 CW；
- **部分页写**（只写半页）会导致整页编程浪费 ⇒ 页打包 / 写缓冲；
- 多 plane 配对写要求两 plane 地址满足器件规则。

#### 4.11.4 分配与顺序流

- 顺序流：连续 LBA 可走 cache program 序列，减少命令开销；
- 随机写：逐页分配，容易产生部分页与更早 GC；
- 分配要平衡 plane，避免单 plane 先耗尽；
- 顺序流检测通常由 FTL 做，不是 host 下发的开关。

#### 4.11.5 写放大点

| 来源 | 说明 |
|---|---|
| 部分页打包 | 不打包 ⇒ 每 CW 吃一整页 |
| padding | 序列结束补齐 |
| journal / CP | 映射持久化 |
| GC / WL | 后台搬移 |
| 元数据 | L2P log、BBT、wear |

#### 4.11.6 失败处理

- program fail：重试 → 换块（remap）→ 标坏；
- 写失败后映射不能指向坏页（否则读到坏数据）；
- 组完成语义：一个 host 写可能拆成多个子操作，全部成功才算成功；
- 超时 / 背压：不能无限等，要有超时结算（否则 host 命令挂死）。

#### 4.11.7 并发与排队

- 多 SQ / 多命令可并行，但同一 LBA 的写要有序；
- 命令池 / 描述符池 / 缓冲池是共享资源，写路径要背压；
- 完成顺序与提交顺序可以不同（CQ 按完成报）；
- 锁 / 门控：序列、GC、WL 与 host 写共享引擎时要让行。

#### 4.11.8 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 回执 | write-back / 早回执 | PLP + write-back |
| 队列深度 | 浅 | 深、多队列 |
| QoS | 尽力 | 尾延迟 / 公平性 |
| 掉电 | journal 兜底 | PLP + journal |

#### 4.11.9 当前平台

- HIL 拆分请求 → FTL 子操作 → 组完成；
- 顺序流检测 + cache program 序列（窗口 4 个 pair 页，可跨 block 续写）；
- `wbuf` 页打包：同 pair 窗口的多次写攒成一页，页内利用率回到 100%；
- VWC 早回执：映射可撤销，掉电按 FAILC 撤销；FUA 已删，`SYNCHRONIZE CACHE` 作持久性屏障；
- journal / CMT 记录映射，program 完成才确认映射；
- 取舍：单 LUN / 2 plane、软件 FC；无 PLP；写路径尾延迟主要来自 free 不足与 journal 配额。

#### 4.11.10 选型 / 学习点

- 选型：回执语义、是否需要 PLP、队列深度、cache program、页打包、QoS。
- 学习点：端到端次序、早回执与撤销、部分页、写放大点、失败重映射、
  序列与门控。

**状态：已定稿。**

### 4.12 读路径（read path）

#### 4.12.1 端到端路径

```text
host 读命令（HIL）
  → 解析 / 拆 LBA
  → L2P 查 PBA
      hit  → 直接读 NAND
      miss → 先读 L2P region（元数据读）→ 再读数据
  → NAND read（单页 / multi-plane / cache read）
  → ECC 校验 / 纠错（必要时重读）
  → 数据搬运（DMA / PIO）
  → 完成回执（CQ）
```

读路径的关键指标是**延迟**（不是吞吐），随机读尤其受 L2P miss 与 NAND tR 支配。

#### 4.12.2 L2P 查找与 miss 代价

| 情况 | 代价 |
|---|---|
| L2P cache hit | 一次 NAND 数据读 |
| L2P cache miss | 一次元数据读 + 一次数据读 |
| region 未加载 | 读 L2P log / 重建 region |
| 映射损坏 | 读失败 / 恢复流程 |

**读放大**：逻辑读 1 页，物理可能读 2 页（元数据 + 数据）。DRAM-less 架构下这是随机读性能的主要瓶颈。

#### 4.12.3 NAND 读类型

| 类型 | 说明 | 收益 |
|---|---|---|
| 单页读 | 基础 | 简单 |
| multi-plane 读 | 两 plane 并行 | 吞吐 |
| cache read（连续） | 器件内部流水 | 顺序读吞吐 |
| 顺序读序列 | 一次提交多页 | 命令开销低 |

顺序读要走 cache read / 序列；随机读几乎无法流水。

#### 4.12.4 ECC 与读恢复

- 正常：ECC 解码通过，直接返回；
- 可纠错：硬判决纠错；
- 纠错失败：重读（read retry，调读电压）；
- 软判决 / LDPC：更复杂，需要多次读；
- read disturb：读多的块要搬移 / 刷新；
- **读恢复策略（RR）**：重试电压、重读次数、标记坏块。

本仓：BCH 硬判决；read recovery 有重试入口，但本器件对应 Feature 地址为保留 ⇒ 实际空转（本项目决定不处理）。

#### 4.12.5 读放大与预取

- 元数据读是额外放大：region 越大，单次 miss 读得越多；
- 预取：顺序读时提前加载相邻 region / 页；
- 但预取会污染 cache（见 4.5 thrashing）；
- 读放大 vs 延迟：预取降低延迟但增加 NAND 读量。

#### 4.12.6 延迟构成

```text
总延迟 ≈ 队列等待 + L2P 查找(+miss) + NAND tR + 传输 + ECC + 完成
```

- 队列深度影响排队延迟；
- NAND tR 是物理下限；
- ECC 强度影响解码时间；
- 元数据 miss 可让延迟翻倍。

#### 4.12.7 QoS 与优先级

- 企业级：读优先于 GC/WL，P99.9 有界；
- 读饥饿：后台搬移占满引擎时，host 读必须能插队；
- 优先级 / 权重：多租户、多队列场景需要公平性。

#### 4.12.8 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 目标 | 吞吐 / 成本 | 尾延迟 / 一致性 |
| 预取 | 积极 | 受控 |
| ECC | 硬判决为主 | LDPC / 软判决 |
| 读优先 | 基本 | 强制 |
| 读放大 | 容忍 | 严格量化 |

#### 4.12.9 当前平台

- L2P region cache（CLOCK + thrashing 降级）；miss 时读映射 region；
- 顺序读检测 + cache read 序列（START_SEQ_MP / SEQ_MP / LAST_MP，可跨 block）；
- BCH ECC；随机读逐页；
- read recovery 模块存在，但器件 Feature 地址保留 ⇒ 实际不生效（已登记，不处理）；
- 读放大：DRAM-less 下随机读可能「元数据 + 数据」两次读；
- 取舍：单 LUN / 2 plane，无多通道并行；顺序读靠 cache read，随机读受 tR 与 L2P miss 支配。

#### 4.12.10 选型 / 学习点

- 选型：L2P 驻留方式、cache 大小、ECC 类型（BCH/LDPC）、cache read 支持、QoS 能力。
- 学习点：读放大、L2P miss 代价、cache read 流水、read retry / soft decode、
  延迟构成、读优先与 GC 协同。

**状态：已定稿。**

### 4.13 TRIM / UNMAP

#### 4.13.1 为什么需要

host 删除文件后，FTL 并不知道这些 LBA 已无效。TRIM/UNMAP 让 host 显式告诉设备：

- 这些 LBA 可以作废 ⇒ 减少 GC 搬移、降低 WAF；
- 避免恢复出旧数据（安全）；
- 让 OP 更有效。

不实现 TRIM 的设备，删除过的数据仍会被 GC 当作有效数据搬来搬去。

#### 4.13.2 接口形态

| 协议 | 命令 |
|---|---|
| ATA | TRIM |
| SCSI | UNMAP / WRITE SAME |
| NVMe | Dataset Management（DSM）deallocate |
| USB MSC | UNMAP（BOT 下的可选命令） |

语义相同：给出一个或多个 LBA range，设备把映射作废。
**读回语义**：多数实现返回全 0 或未定义；NVMe 可声明 deterministic read zeros after trim。

#### 4.13.3 粒度与描述符

- range 粒度：LBA 起止，通常要求对齐（如 4 KiB）；
- 一次可带多个 range（descriptor list）；
- 描述符数量 / 长度有上限 ⇒ 大范围 TRIM 要拆批；
- 未对齐 / 超范围的 range 要拒绝或截断（按协议）。

**取舍**：range 越多，解析 / 记账成本越高；大块 TRIM 需要限流，避免长时间占用。

#### 4.13.4 实现方式

```text
收到 TRIM range
  → 逐 LBA / 逐 region 查 L2P
  → 有效映射：清映射、valid_count--
  → 可选：写 journal 记录作废
  → 不立即擦除（块留待 GC）
```

- **惰性作废**：只改映射，不碰介质；由 GC 回收；
- **立即擦除**（少见）：直接擦块，延迟高；
- **批处理 / 限流**：TRIM 风暴会拖慢 host 命令；
- **异步推进**：controller 每轮推进一段（本仓方式）。

#### 4.13.5 与写 / 读的交互

- TRIM 与同 LBA 的在飞写要有序：后到的写不能被子操作打乱；
- TRIM 后读：返回 0 或未定义；若产品要求「不读回旧数据」，必须额外靠擦除 / crypto erase 保证，
  协议本身不保证；
- TRIM 不应破坏未覆盖的相邻 LBA；
- 大量 TRIM 期间，host 读写要有 QoS / 让行。

#### 4.13.6 收益与代价

| 收益 | 代价 |
|---|---|
| GC 少搬无效数据 | L2P 遍历 / 更新成本 |
| WAF 下降 | journal 条目增加 |
| 寿命延长 | TRIM 风暴可能拖慢 host |
| 安全（旧数据不可读） | 大范围操作要限流 |

#### 4.13.7 掉电语义

- 各协议 / 实现允许 TRIM 状态**不持久化**：掉电后未映射 LBA 可返回未定义数据
  （可能仍是旧数据）；若产品对外承诺安全擦除或 deterministic read zeros，则必须持久化；
- 需要 journal 记录 TRIM 的话，TRIM 也进入写放大；
- 无论是否持久化，**产品必须写明自己的语义**，不能默认「TRIM 一定持久」。

#### 4.13.8 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 支持 | 常见（NVMe DSM） | 常见（UNMAP） |
| 语义 | 尽力、性能优先 | 可预测、可审计 |
| TRIM 风暴 | 容忍 | 限流 / QoS |
| 安全擦除 | 可选 | 常见（crypto erase） |

#### 4.13.9 当前平台

- HIL 有 `SSD_REQUEST_TRIM`；USB MSC 支持 UNMAP(0x42) → 逐 range TRIM；
- Controller 把 TRIM 当**异步命令**：submit 只登记范围，`ssd_controller_service` 每轮推进
  `ftl_trim_begin/step`，全部完成才回执；
- FTL 逐 range 作废映射（`wbuf` 有 TRIGGER_DROP 处理窗口内数据）；
- 单飞行 TRIM（同一时刻只有一个 TRIM 在飞）；
- 取舍：TRIM 是惰性作废，不立即擦除；收益体现在后续 GC 的 WAF 上。

#### 4.13.10 选型 / 学习点

- 选型：协议支持（UNMAP/DSM）、range 上限、限流 / QoS、掉电语义、安全擦除。
- 学习点：TRIM 为什么降低 WAF、惰性作废 vs 立即擦除、TRIM 与 GC 的配合、
  TRIM 风暴、掉电后语义。

**状态：已定稿。**

### 4.14 调度 / QoS

#### 4.14.1 调度对象

一个主控里同时抢资源的有：

| 工作 | 类型 |
|---|---|
| host 读 | 前台、延迟敏感 |
| host 写 | 前台、吞吐 / 回执 |
| GC / WL | 后台、可延后 |
| journal / CP / metadata flush | 半后台、有截止期 |
| TRIM | 异步、可限流 |
| PFR / 扫描 | 启动期 |
| telemetry / SMART | 低优先级 |

调度要解决的问题：**谁先用 NAND 通道、命令池、缓冲、CPU**。

#### 4.14.2 队列与仲裁

- 单队列 vs 多队列（NVMe 多 SQ/CQ）；
- 仲裁：RR（轮询）、WRR（加权）、strict priority、credit-based；
- doorbell 通知 + 完成队列，本质是**生产者-消费者**；
- 队列深度决定可并行命令数，也放大乱序完成。

**取舍**：队列越深吞吐越好，但缓冲 / 命令池压力、尾延迟和公平性更难控。

#### 4.14.3 前台 / 后台仲裁

| 策略 | 说明 | 风险 |
|---|---|---|
| 前台优先 | host 永远先跑 | 后台饿死，free 耗尽后卡死 |
| 后台预留 | 给 GC 固定时间片 | host 尾延迟变差 |
| 时间预算 | 每轮给后台有限预算 | 需要节拍 / 让行 |
| 事件驱动 | free 低于水位才做 | 可能来不及 |

**本仓**：主循环 + 后台时间预算调度器，GC/WL 在预算内推进，host 写优先。

#### 4.14.4 QoS 指标

- 平均延迟、P99/P99.9 尾延迟；
- 吞吐（IOPS / 带宽）；
- 公平性（多 namespace / 多租户）；
- 有界启动 / 恢复时间；
- 抖动（jitter）。

企业级通常以 **P99.9 + 稳态** 为验收口径；消费级更看峰值和成本。

#### 4.14.5 背压与准入

- 资源池（命令 / 描述符 / 缓冲）满 ⇒ 返回 `AGAIN` / BUSY，而不是无限等；
- per-queue credit 防止单队列占满；
- 准入控制：超过能力就拒绝 / 延迟，避免雪崩；
- 背压要能传导到 host（doorbell 不再消费 / CQ 延迟）。

**取舍**：背压太硬 ⇒ host 看到错误 / 超时；太软 ⇒ 内部队列膨胀、延迟失控。

#### 4.14.6 顺序与依赖

- 同一 LBA 的写必须有序；
- barrier / flush 要等前序完成；
- 序列（cache program）不能被 GC/WL 劈开；
- 锁 / 门控：FC 链锁、LUN 锁、序列门控；
- 乱序完成时，host 依赖命令序号 / 完成顺序。

#### 4.14.7 饥饿与优先级反转

- 后台长期占引擎 ⇒ host 读饥饿；
- 低优先级持锁 ⇒ 高优先级等锁（优先级反转）；
- 解决：让行点、优先级继承、时间片、预留槽位；
- 企业级会把「GC 不阻塞 host 读」写成硬约束。

#### 4.14.8 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 队列 | 浅、单 / 少队列 | 深、多队列 |
| 目标 | 峰值吞吐 | P99.9、稳态、公平 |
| 后台 | 空闲做 | 预留 / 时间片 |
| 背压 | 简单 | credit / QoS |
| 多租户 | 无 | namespace / 租户隔离 |

#### 4.14.9 当前平台

- 纯轮询主循环（无数据面 ISR），FC 轮询完成；
- 后台时间预算调度器：GC/WL/元数据 job 在预算内推进；
- host 写优先；序列门控 / 让行；TRIM/FLUSH 单飞行；
- 资源池满 ⇒ `FTL_AGAIN` 背压，host 侧重试；
- 单 LUN、单队列（USB BOT），无多队列 / 多租户 QoS；
- 取舍：低端平台不做企业级 QoS，但保留了时间预算 / 让行 / 背压三个基本机制。

#### 4.14.10 选型 / 学习点

- 选型：队列数 / 深度、仲裁方式、是否需要 QoS / 多租户、后台预算、背压策略。
- 学习点：前台 / 后台仲裁、时间预算、背压与准入、乱序完成、饥饿 / 优先级反转、
  P99 与稳态口径。

**状态：已定稿。**

### 4.15 telemetry / SMART

#### 4.15.1 目的

telemetry 要能回答三类问题：

| 问题 | 需要的量 |
|---|---|
| 盘还健康吗？ | 坏块、UNC、ECC 纠正、program/erase 失败、温度 |
| 寿命用掉多少？ | host 字节、NAND 字节、擦除计数、WAF、OP 余量 |
| 出问题怎么定位？ | 事件计数、最近错误、GC/WL/PFR 次数、free pool 走势 |

#### 4.15.2 标准形态

| 标准 | 内容 |
|---|---|
| ATA SMART | 属性表（ID + raw value） |
| NVMe SMART / Health Log | percentage used、data units read/written、unsafe shutdowns、media errors |
| SCSI log pages | 错误计数、温度、寿命 |
| PCIe AER | 链路错误计数 |
| 厂商 telemetry log | 更细的内部计数 |

消费级通常只暴露简化 SMART；企业级还有 telemetry log、drive log、fleet 采集。

#### 4.15.3 计数分类

| 分类 | 示例 |
|---|---|
| 终身（lifetime） | 总读写字节、上电时长、启动次数、PFR 次数 |
| 健康 | UNC、ECC corrected、program/erase fail、坏块数 |
| 性能 | 命令数、队列深度、延迟分桶 |
| 后台 | GC/WL 次数、搬移页数、WAF |
| 状态 | free pool、open/closed 块、温度 |

**口径**：每个量要写清「终身 / 本次上电 / 当前窗口」，不能混。

#### 4.15.4 持久化

- 终身计数必须跨掉电保存（CP / 固定区 / 专用块）；
- 更新节拍：每次更新都落盘 ⇒ 写放大；只随 CP 落盘 ⇒ 掉电丢一个 CP 间隔；
- 多 die / 多实例：per-instance 字段各写各的，controller-wide 字段复制取 max；
- 计数块要带 magic/version/CRC，坏了不能影响主 CP。

#### 4.15.5 可观测性形态

- 计数器（counter）；
- 直方图 / 延迟分桶（latency histogram）；
- 事件日志（最近 N 条错误 / 状态转移）；
- 快照 / 转储（CLI、调试口）；
- trace（高频路径，通常只在 debug）。

**取舍**：越细越占 RAM/CPU/带宽；高频路径上的计数要保证低开销。

#### 4.15.6 开销控制

- 热路径计数用递增，不做格式化；
- 打印 / 格式化放低优先级或 host 请求时；
- 持久化只随 CP，避免每次计数写 NAND；
- 多字节共享计数要原子 / 临界区（H18）。

#### 4.15.7 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 暴露 | 简化 SMART | SMART + telemetry log |
| 采集 | 本地工具 | fleet / 远程 |
| 粒度 | 基本寿命 | 延迟分桶、WAF、温度历史 |
| 用途 | 保修 / 健康 | 运维 / 容量规划 |

#### 4.15.8 当前平台

- `stats`：分模块计数器（L2P/GC/WL/元数据/HIL 等），有窗口 delta 与吞吐基线；
- `lifetime`：掉电保持的终身计数（读写字节、ECC/UNC、program/erase fail、上电时长、
  启动/PFR 次数、host 命令数），随 CP 落盘，多 die 取 max；
- CLI：`stats`、`wear`、`ftl_ctx [module]`、`smart` 类查询；
- 有延迟直方图（L2P load / GC / CP / host）；无 NVMe SMART / telemetry log page（USB MSC 平台），
  以 CLI / 串口为主；
- 取舍：不做 fleet / 主机 Log Page；先保证计数口径正确、可持久化。

#### 4.15.9 选型 / 学习点

- 选型：需要哪些标准日志、持久化节拍、延迟分桶、fleet 采集、调试口带宽。
- 学习点：SMART / Health Log 字段含义、终身 vs 窗口口径、持久化与写放大、
  热路径计数开销、telemetry 如何指导 GC/WL 调参。

**状态：已定稿。**

### 4.16 安全（security）

#### 4.16.1 威胁模型

先定义防谁，否则安全设计没有边界：

| 威胁 | 说明 |
|---|---|
| 设备丢失 / 被盗 | 数据 at rest 泄露 |
| 未授权访问 | 无口令 / 无 key 读写 |
| 固件篡改 | 刷入恶意 / 旧固件 |
| 调试口滥用 | JTAG/SWD 读内存 / 密钥 |
| 供应链 | 生产 / 维修环节泄露 |
| 侧信道 | 功耗 / 时序推断（通常更高端） |

消费级常只做「丢失保护 + 安全擦除」；企业级还要密钥管理、认证、审计。

#### 4.16.2 访问控制

- ATA Security（旧，口令锁盘）；
- NVMe Security Send/Receive（协议命令）；
- TCG OPAL / Ruby（SED 标准，带 locking range）；
- 主机侧认证 + 设备侧策略；
- 失败重试锁定、防暴力。

#### 4.16.3 加密

| 方案 | 说明 |
|---|---|
| SED（self-encrypting drive） | 盘内 AES 引擎，host 无感 |
| AES-XTS | 扇区级加密标准模式 |
| 内部 key | 密钥不出盘，用户口令解封 |
| 外部 KMS | 企业密钥管理，支持远程吊销 |
| crypto erase | 销毁 key = 数据不可读（秒级安全擦除） |

**关键点**：加密密钥的保护比算法更重要；key 存哪、怎么解封、怎么销毁决定安全性。

#### 4.16.4 安全擦除（sanitize）

| 手段 | 效果 |
|---|---|
| 覆盖写 | 慢、对闪存不友好 |
| 块擦除 + 映射清空 | 仍可能有残留（坏块 / OP） |
| crypto erase | 快、彻底（销毁 key） |
| 厂商 sanitize 命令 | NVMe/SCSI 标准入口 |

TRIM 不是安全擦除：它只作废映射，数据可能仍在介质上。

#### 4.16.5 固件与调试安全

- Secure boot：验签后才运行；
- 签名固件 + 防回滚（anti-rollback）；
- 调试口：量产锁 JTAG/SWD，RMA 用签名解锁；
- 生产模式：密钥注入、测试后清密；
- 固件更新通道要认证（见 4.17）。

#### 4.16.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 加密 | 可选 / 整盘 | 默认 / 强制 |
| 标准 | 简单口令 | OPAL、KMS、FIPS |
| 安全擦除 | 基本 | sanitize + 审计 |
| 固件 | 基本验签 | secure boot + 防回滚 |
| 调试口 | 量产锁 | 严格生命周期管理 |

#### 4.16.7 当前平台

- **未实现**加密、安全启动、OPAL、sanitize；STM32H743 平台也没有接入 crypto 引擎；
- 调试口（SWD）开放；CLI 可读写介质、格式化 ⇒ 只适合学习 / 实验环境；
- 当前威胁模型未定义；若产品化，至少要补：调试口保护、固件验签、安全擦除路径；
- 取舍：学习项目优先数据路径 / FTL，安全作为明确的「未实现」项登记，不假装有能力。

#### 4.16.8 选型 / 学习点

- 选型：是否需要 SED、密钥管理（内部 / KMS）、OPAL、secure boot、防回滚、sanitize 认证。
- 学习点：威胁模型先行、XTS / crypto erase、密钥生命周期、TRIM ≠ 安全擦除、
  调试口与固件更新是常见攻击面。

**状态：已定稿。**

### 4.17 固件更新（firmware update）

#### 4.17.1 为什么需要

- 修 bug、补安全漏洞；
- 增加功能 / 算法；
- 适配新介质批次；
- 现场升级，不能返厂。

主控固件更新最难的不是「传镜像」，而是**升级失败不能变砖、数据不能丢**。

#### 4.17.2 升级通道

| 通道 | 说明 |
|---|---|
| NVMe Firmware Download/Commit | 标准 host 命令 |
| SCSI WRITE BUFFER / ATA DOWNLOAD MICROCODE | 传统盘命令 |
| USB DFU / MSC 专用命令 | 外置盘常见 |
| UART/CAN bootloader | 嵌入式板级 |
| SWD/JTAG | 工厂 / 调试 |
| OTA（带外） | 带网络的管理口 |

消费级多用 host 命令或厂商工具；企业级还支持带外 / 远程批量升级。

#### 4.17.3 镜像格式

```text
header:  magic | version | size | hash/CRC | signature | flags
payload: 固件本体（可压缩 / 加密）
```

- version + anti-rollback 计数；
- hash/CRC 校验完整性；
- 签名校验来源与完整性；
- 可选加密（防逆向 / 防篡改）。

#### 4.17.4 升级流程

```text
host 下发镜像
  → 写入 staging（DRAM 或 NAND 专用区）
  → 校验 hash/签名/版本
  → commit（标记待生效）
  → 复位
  → bootloader 选择新镜像 / 失败回滚
```

- **staging 区**不能覆盖运行中的固件；
- commit 要原子（掉电不能停在「半生效」）；
- 复位后由 bootloader 决定启动哪个 slot。

#### 4.17.5 原子性与掉电

| 机制 | 作用 |
|---|---|
| A/B 双槽 | 新固件坏了好回退 |
| bootloader 回滚 | 启动失败自动切旧槽 |
| 版本 / CRC 校验 | 拒绝坏镜像 |
| staging + commit | 下载与生效分离 |
| 看门狗 / 启动计数 | 连续失败回滚 |

目标：**任何时刻掉电都不变砖**。

#### 4.17.6 与数据 / 介质布局的兼容

- 固件升级可能改变 FTL 元数据布局 / 介质版本；
- 升级要处理旧介质：迁移、兼容读取、或要求格式化（会丢数据）；
- 向下兼容策略要写清：哪些版本可原地升级，哪些必须格式化；
- 回滚到旧固件时，新布局可能读不了 ⇒ 防回滚与兼容性要一起设计。

#### 4.17.7 安全

- 镜像必须验签（防恶意固件）；
- 防回滚（防止降级到有漏洞版本）；
- 调试口 / 解锁要有生命周期管理；
- 升级通道本身要防篡改（见 4.16）。

#### 4.17.8 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 通道 | 厂商工具 / host 命令 | host + 带外 / 批量 |
| 双槽 | 常见 | 必备 |
| 在线升级 | 少 | 支持（不中断业务） |
| 回滚 | 基本 | 严格版本 / 兼容管理 |
| 验证 | 基本校验 | 签名 + 审计 |

#### 4.17.9 当前平台

- STM32H743 内部 Flash；当前通过 **SWD/JLink 烧写**，没有 host 侧固件更新通道；
- `src/boot/` 有启动相关代码，但未接入 main；无 A/B 双槽、无 bootloader 回滚；
- USB MSC 没有固件更新命令；DFU 未集成；
- 介质版本常量存在（布局变更时靠版本 / 格式化处理），但没有「升级迁移」流程；
- 取舍：学习项目先保证数据路径；固件更新 / 防变砖作为明确的未实现项。

#### 4.17.10 选型 / 学习点

- 选型：升级通道、A/B 双槽、签名 / 防回滚、在线升级、旧介质兼容策略。
- 学习点：staging/commit、原子性、bootloader 回滚、镜像验签、介质版本与升级兼容、
  「升级失败不变砖」的设计。

**状态：已定稿。**

### 4.18 端到端数据保护（E2E / DIF / RAID）

#### 4.18.1 保护层次

| 层 | 机制 | 防什么 |
|---|---|---|
| 介质 | ECC（BCH/LDPC） | NAND 位翻转 |
| 页 / 元数据 | CRC | 页内 / 元数据损坏 |
| 链路 | 链路 CRC（DLLP 等） | 传输错误 |
| 端到端 | E2E / DIF / DIX | 控制器内部（DRAM / 总线）静默损坏 |
| 阵列 | RAID/XOR | 单 die / 页 / plane 失效 |
| 主机 | 应用校验和 | 全链路 |

ECC 只保护 NAND；**数据在 DRAM、总线、控制器内部被改坏，ECC 看不见** ⇒ 需要 E2E。

#### 4.18.2 DIF / DIX

T10 PI（Protection Information）每个逻辑块附 8 字节：

```text
guard (2B, CRC16) | application tag (2B) | reference tag (4B)
```

- **DIF**：PI 紧跟数据（如 512+8）；
- **DIX**：PI 放在独立缓冲区，由 host 提供 / 校验；
- Type 1/2/3：reference tag 校验方式不同；
- 读 / 写路径每段都要校验，不匹配 ⇒ 报错，不能静默返回。

#### 4.18.3 E2E 检查点

```text
host → HIL → DRAM/缓冲 → NAND → DRAM → HIL → host
        ↑        ↑          ↑        ↑
      校验     校验        校验     校验
```

- 写入时生成 / 附带校验，读出时逐段验证；
- 关键在**跨越易失内存**（DRAM 位翻转、DMA 错误）；
- 发现不匹配要能定位到段（host / DRAM / NAND）。

#### 4.18.4 RAID / XOR 恢复

| 粒度 | 说明 |
|---|---|
| die 级 | 多 die 组成 stripe，XOR 恢复单 die |
| page / plane 级 | 页 / plane 间 XOR |
| 板级 | 多通道 / 多芯片 |

- 代价：额外 parity 写（WAF 上升）、重建时间、写洞（write hole）；
- 适用：企业级高可靠；消费级通常不做；
- 与 ECC 分工：ECC 纠位翻转，RAID 恢复整页 / 整 die 失效。

#### 4.18.5 错误处理策略

- 可纠：ECC 纠正，计数；
- 不可纠（UNC）：返回读错误，不能返回坏数据；
- DIF 不匹配：报错并记录；
- 恢复：重读、RAID 重建、标记坏块；
- **静默错误是最大敌人**：宁可报错，不可返回错数据。

#### 4.18.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| ECC | BCH 为主 | LDPC + 软判决 |
| E2E / DIF | 少 | 常见 / 强制 |
| RAID | 少 | 常见（die / page） |
| 错误上报 | 基本 | 详细 telemetry |
| 静默错误容忍 | 低 | 极低 |

#### 4.18.7 当前平台

- NAND 侧：BCH ECC + CRC（页 / 元数据）；FC 有 CRC 引擎；
- DIF/DIX、host E2E：**未实现**；
- RAID：`CONFIG_FC_RAID=0`，功能代码存在但未验证，保持关闭；
- 数据路径的静默损坏检测主要靠 ECC/CRC，控制器内部（SRAM / 总线）无 E2E；
- 取舍：学习项目先把介质保护做对；E2E/RAID 作为明确未实现项。

#### 4.18.8 选型 / 学习点

- 选型：是否需要 DIF/DIX、RAID 粒度、ECC 类型、错误上报能力、静默错误预算。
- 学习点：ECC ≠ E2E、DIF 8 字节结构、检查点放置、RAID 的 WAF/写洞代价、
  「宁可报错不返回坏数据」。

**状态：已定稿。**

### 4.19 数据放置 / 多流 / 温度

#### 4.19.1 为什么重要

把冷数据和热数据混在同一个块里，会让 GC 很亏：

- 热数据很快失效，但冷数据仍有效；
- 回收该块要搬走大量冷数据 ⇒ WAF 上升；
- 分开放置后，热块很快全无效，回收几乎零搬移。

数据放置的目标：**按生命周期分组，让 GC 只收「整块都快死」的块**。

#### 4.19.2 温度分级

| 等级 | 判定 | 放置 |
|---|---|---|
| HOT | 写频高、短期反复覆盖 | 独立 open block |
| WARM | 中等 | 可混合 |
| COLD | 写一次长期不改 | 独立 open block |

判定依据：

- host 写频统计（LBA 区域）；
- 顺序 / 随机特征；
- host hint（stream、写生命周期提示）；
- 历史失效速度。

#### 4.19.3 多流与主机提示

| 机制 | 说明 |
|---|---|
| NVMe Streams | host 给写打 stream ID，设备按流分开放置 |
| FDP（Flexible Data Placement） | 按 Reclaim Unit 放置，GC 以 RU 为单位 |
| ZNS（Zoned Namespace） | host 管理 zone 写入顺序，设备不做随机 GC |
| 写生命周期提示 | host 提示数据预期寿命 |

这些机制把「冷热判断」部分交给 host，减少设备猜测。

#### 4.19.4 放置策略

- 每个温度 / stream 维护独立 open block；
- 块满 / 切换时关闭，GC 按组选 victim；
- 避免跨组混合写同一页（multi-plane 配对要注意组）；
- 元数据也要按组记录，否则恢复后分组丢失。

**取舍**：分组越多，free pool 碎片化、并行度下降、管理复杂度上升。

#### 4.19.5 收益与代价

| 收益 | 代价 |
|---|---|
| GC WAF 下降 | free pool 碎片化 |
| 寿命延长 | open block 数增加 |
| 稳态性能更好 | 元数据 / 恢复更复杂 |
| 可预测性 | host 提示依赖 |

#### 4.19.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| 温度分组 | 简单（hot/cold） | 多级 / 多流 |
| host 提示 | 少 | Streams / FDP / ZNS |
| 目标 | 成本 / WAF 平衡 | 稳态 WAF、可预测 |
| ZNS | 少 | 数据中心常见 |

#### 4.19.7 当前平台

- 块级温度：`FTL_TEMP_HOT/COLD`；GC victim 评分含 temp 权重（HOT 优先回收）；
- 没有 host multi-stream / FDP / ZNS 接口（USB MSC 平台）；
- 放置以 plane 为单位，温度是设备侧启发式，不是 host 提示；
- 取舍：低端平台用「块温度 + GC 评分」近似冷热分离，不做多流。

#### 4.19.8 选型 / 学习点

- 选型：是否需要 multi-stream / FDP / ZNS、温度分级数、host 提示能力、free pool 碎片容忍度。
- 学习点：冷热混合为什么抬高 WAF、温度判定、多流 / FDP / ZNS 的分工、
  分组与碎片化的取舍。

**状态：已定稿。**

### 4.20 namespace / ZNS / FDP

#### 4.20.1 namespace

- NVMe 用 **namespace** 划分逻辑块地址空间（类似分区，但在设备内）；
- 一个控制器可暴露多个 namespace，各自有容量 / 格式 / LBA 范围；
- 好处：隔离、独立格式化、独立 QoS / 权限；
- 代价：共享底层 FTL / 介质，namespace 之间仍会争 GC / 带宽。

#### 4.20.2 传统块接口的问题

传统块设备对 host 是「随机可写」，但 NAND 只能顺序写、按块擦：

- 设备必须做完整 GC，WAF 由设备承担；
- host 不知道数据冷热，设备只能猜；
- 结果是稳态 WAF 高、尾延迟不可控。

#### 4.20.3 ZNS（Zoned Namespace）

把地址空间划成 **zone**，每个 zone 内必须**顺序写**：

| 概念 | 说明 |
|---|---|
| zone | 一段连续 LBA，有固定容量 |
| write pointer | 设备维护，下次可写位置 |
| zone capacity ≤ zone size | 尾部可留空 |
| zone 状态 | Empty / Implicitly Open / Explicitly Open / Closed / Full / Read Only / Offline |
| 命令 | Write / Zone Append / Reset Zone / Zone Management |

- host 负责把数据按生命周期放进不同 zone；
- zone 内顺序写 ⇒ 设备几乎不需要 zone 内 GC；
- Reset Zone 一次性作废整个 zone；
- host 侧需要支持 zone 的软件栈（文件系统 / 驱动），改造成本高。

**取舍**：WAF 大幅下降、可预测性好；但 host 复杂度高、随机写受限、生态要求高。

#### 4.20.4 FDP（Flexible Data Placement）

介于传统块与 ZNS 之间：

- host 在写命令里带 **placement handle / reclaim unit** 提示；
- 设备按 RU 分组放置，GC 以 RU 为单位；
- host 仍是块接口，不需要重写文件系统；
- 可持久化 RU，减少分组丢失。

**取舍**：host 改动小、WAF 改善中等；控制粒度不如 ZNS。

#### 4.20.5 对比

| | 传统块 | ZNS | FDP |
|---|---|---|---|
| host 写入 | 随机 | zone 内顺序 | 随机 + 放置提示 |
| host 控制 | 无 | 高 | 中 |
| 设备 GC | 完整 | zone 内几乎无 | 按 RU 分组 |
| host 改造 | 无 | 大 | 小~中 |
| WAF | 较高 | 低 | 较低 |
| 典型场景 | 消费级 | 数据中心 | 数据中心 |

#### 4.20.6 消费级 vs 企业级

| 维度 | 消费级 | 企业级 |
|---|---|---|
| namespace | 少 / 单 | 多 namespace、隔离 |
| ZNS | 少见 | 数据中心常见 |
| FDP | 少见 | 逐渐常见 |
| 目标 | 兼容性 / 成本 | WAF / 可预测 / 多租户 |

#### 4.20.7 当前平台

- host 侧是 **USB MSC 单 LUN 块设备**，没有 NVMe namespace；
- 没有 ZNS/FDP 接口，也没有 zone write pointer；
- 内部多 die 按 FTL 实例分布，但 host 仍看到单一 LUN；
- 取舍：低端 / 学习平台优先「传统块接口 + 设备侧 GC」，ZNS/FDP 作为理解主控演进方向的知识点。

#### 4.20.8 选型 / 学习点

- 选型：是否需要多 namespace、是否上 ZNS/FDP、host 软件栈改造意愿、WAF 目标。
- 学习点：namespace 隔离、ZNS zone 状态机 / 写指针、FDP reclaim unit、
  「把冷热判断交给 host」的演进逻辑。

**状态：已定稿。**

## 5. 消费级 vs 企业级（综合与选型）

> §3/§4 的每个模块已分别给出两边的差异；本节只做**横向综合**：
> 市场需求 → 架构分岔 → 量化口径 → 同功能不同约束 → 当前平台定位 → 选型矩阵。
> 模块细节仍以各节为准，避免同一语义两处维护。
>
> 来源：`docs/design/storage_controller_design.md` §九 + 本文 §3/§4 各模块对照。
> 文中的 OP / DWPD / 延迟等是**典型区间或口径说明**，具体产品以数据手册为准。

### 5.1 三类市场

| 市场 | 典型形态 | 典型 workload | 寿命 / 可靠性目标 | 功耗 / 热 | 成本敏感度 | 典型接口 | 典型介质 |
|---|---|---|---|---|---|---|---|
| 消费级 client | PC / 笔记本 / 零售 SSD | 桌面混合；队列浅；读多写少但有突发 | TBW 覆盖保修期；基本 SMART | 笔记本受电池 / 热限制 | 很高 | PCIe NVMe（主流）/ SATA | TLC / QLC |
| 企业级 enterprise / data center | 服务器、阵列、云 | 高队列深度、随机混合、**稳态** | DWPD、P99.9、低 UBER、PLP | 高，但有风道 / 电源预算 | 较低 | PCIe NVMe / SAS | eTLC / MLC（部分 SLC） |
| 移动 / 嵌入式 mobile | 手机、平板、车载、工业 | 低功耗、长 idle、突发写 | 按设备寿命；基本可靠性 | 电池 / 热极敏感 | 高 | UFS / eMMC | TLC |

> 企业级内部还分 mission-critical（SAS/双端口/热插拔）与 cloud（NVMe 高密度）；
> 二者对可靠性和成本的要求不同，本表合并处理。

### 5.2 需求差异 → 架构分岔

| 需求 | 消费级做法 | 企业级做法 | 主要影响模块 |
|---|---|---|---|
| 稳态吞吐 / 尾延迟 | 峰值 / burst 可接受 | **P99.9 稳态**、读优先 | GC（4.6）、调度（4.14）、OP（4.9） |
| 寿命 | TBW 够用 | **DWPD**、更长保修 | OP、WL（4.7）、ECC、介质 |
| 掉电 | journal / checkpoint 兜底 | **PLP** + journal/CP | 电源（3.11）、PFR（4.8） |
| 数据完整性 | ECC 为主 | ECC + RAID + DIF/E2E | ECC（3.8）、E2E（4.18） |
| 多租户 / 隔离 | 基本没有 | SR-IOV、多 namespace、QoS | 主机控制器（3.2）、调度（4.14） |
| 功耗 / 热 | 电池、笔记本散热 | 风道、可预测功耗 | 热管理（3.12） |
| 成本 / 容量 | 极致压缩 | 为稳态/寿命付费 | DRAM/L2P（4.3/4.4）、OP、介质 |
| 形态 / 可用性 | 单盘 | 双端口、热插拔、冗余 | 板级 / 控制器（3.2、3.14） |

**关键：区分「架构分岔」与「调参」。**

| 类型 | 例子 | 含义 |
|---|---|---|
| 架构分岔 | DRAM vs DRAM-less、PLP、通道数、双端口 / SR-IOV、ECC 引擎、RAID、QoS 仲裁 | 由芯片 / 板卡能力决定，选型时必须先定 |
| 调参 | OP 大小、GC 水位、victim 权重、cache 大小、SLC cache 大小、telemetry 粒度 | 同一架构内可调，后期还能改 |

选型先看**真分岔**；调参可以后做。反过来（先调参、后补硬件）通常要重新设计。

### 5.3 量化口径

| 指标 | 定义 | 比较时要注意 |
|---|---|---|
| TBW（terabytes written） | 保修期内可写入的总字节数 | 与容量、保修年限绑定 |
| DWPD（drive writes per day） | 每天可写满整盘次数 = `TBW / (容量 × 365 × 保修年数)`（容量与 TBW 同单位） | 厂商口径不同；必须看容量/年限 |
| OP（over-provisioning） | `(物理可用 − 用户可见) / 物理可用` | 按块还是按字节要写清 |
| WAF（write amplification factor） | NAND 编程量 / host 写量 | 稳态值 vs 峰值；含不含元数据 |
| UBER（uncorrectable bit error rate） | 不可纠误码率 | 定义可能按 bit / 按 sector；比较前确认 |
| P99.9 latency | 99.9% 请求的延迟上界 | 必须同时给 workload、队列深度、块大小、是否稳态 |
| IOPS / 带宽 | 每秒 I/O 数 / 字节数 | burst 峰值 ≠ 稳态；顺序/随机要分开 |

**不可直接比较的典型情况**：

- 消费级 datasheet 的峰值 vs 企业级的稳态；
- 不同块大小（512 B / 4 KiB / 128 KiB）混比；
- 没有说明 preconditioning（预处理）和 steady-state 的测试；
- SLC cache 内的 burst 成绩 vs cache 耗尽后的成绩。

> 企业级验收通常以「稳态 + P99.9 + 规定 workload」为准；消费级更常给「峰值 + 典型 workload」。

### 5.4 同功能不同约束

| 模块 | 消费级 | 企业级 | 差异本质 |
|---|---|---|---|
| GC（4.6） | 抖动可容忍 | 预擦除池、读优先、P99.9 | 尾延迟约束 |
| ECC（3.8） | BCH 硬判决为主 | LDPC + 软判决 / read retry | 纠错强度与延迟 |
| QoS（4.14） | 尽力而为 | 仲裁、公平、多租户 | 稳态与隔离 |
| PLP（3.11） | 通常无 | 必备 | 掉电窗口 |
| OP（4.9） | ~7% | ~28%（常见区间） | 寿命 / 稳态 WAF |
| DRAM（4.4） | DRAM-less / HMB 常见 | DRAM 常驻 + ECC | 映射表驻留与随机读 |
| telemetry（4.15） | 基本 SMART | telemetry log、fleet | 运维与审计 |
| 安全（4.16） | 可选 | 默认 / OPAL / KMS | 威胁模型 |
| 固件更新（4.17） | 基本 | 双槽、在线、回滚 | 可用性 |
| 数据放置（4.19） | 设备侧启发式 | multi-stream / FDP / ZNS | 稳态 WAF |
| namespace（4.20） | 单 namespace 常见 | 多 namespace / 隔离 | 多租户 |

**注意**：这些不是「消费级没有、企业级才有」的简单清单；很多功能两边都有，
但**约束强度不同**（例如 GC 两边都有，企业级多了尾延迟和读优先的硬要求）。

### 5.5 当前平台的定位

**定位：消费级 / 学习原型，不是产品。**

主动选择：

- DRAM-less、单 NAND / 单 LUN / 2 plane、PIO 数据面、USB MSC；
- BCH 硬判决；无 PLP、无 RAID、无 DIF/E2E；
- 无加密 / 安全启动 / sanitize；
- 无 host 侧固件更新 / A/B 双槽；
- 无 multi-stream / FDP / ZNS / 多 namespace。

保留的「主控骨架」：

- HIL 请求生命周期（拆分 / 排队 / 完成回执）；
- Journal + Checkpoint + PFR；
- GC / WL、OP / spare、BBT；
- `stats` / `smart` / lifetime 持久化；
- VWC 早回执与失败撤销。

**产品化差距**（按优先级）：

1. 可靠性与掉电：PLP、完整故障注入、随机掉电验证；
2. 性能与介质效率：多通道 / 多 LUN、DMA、LDPC 软判决；
3. 主机接口：PCIe EP + NVMe 队列；
4. 企业能力：QoS、双端口 / SR-IOV、DIF/E2E、RAID；
5. 安全与可维护性：加密、secure boot、固件双槽。

> 证据边界见 §6.8 表下注：上板证据对应 2026-11-20 镜像，此后工作区改动尚未重新上板。

### 5.6 选型决策矩阵

| 目标档位 | 必须有 | 建议有 | 可以不做 | 平台含义 |
|---|---|---|---|---|
| C1 消费级 NVMe | PCIe EP + NVMe、NAND 控制器、L2P、GC/WL、PFR、TRIM | DRAM-less/HMB 或小 DRAM、SLC cache、热 throttle | PLP、RAID、双端口、多租户 | SoC 需 PCIe EP + NAND 接口；或 FPGA 自写 |
| E1 企业级 | PCIe EP 或 SAS、DRAM+ECC、PLP、多通道、LDPC、RAID/E2E、QoS、telemetry | 双端口 / SR-IOV、多 namespace、安全 | 低成本、小封装 | 通常 ASIC/高端 SoC/FPGA；板级电源与风道要配套 |
| R1 研究 / FPGA | FPGA PL、NAND PHY、ECC/DMA IP、可观测性 | PCIe EP、NVMe 队列 IP | 产品级认证、PLP | 先写 IP，再拼系统；OpenSSD 可作参考 |
| M1 移动 / 嵌入式 | UFS 或 eMMC 控制器、低功耗、小封装 | 热管理、基本 ECC | PLP、双端口、企业 QoS | SoC 需 UFS/eMMC 接口；功耗优先 |

**功能集分层**（在矩阵之上做裁剪）：

- **最小主控**：NAND 控制器 + BCH、L2P、GC/WL、BBT、掉电恢复、TRIM、基本 SMART；
- **消费级 NVMe 追加**：PCIe EP + NVMe SQ/CQ/doorbell/PRP、DRAM-less/HMB 或小 DRAM、
  SLC cache、热 throttle；
- **企业级追加**：DRAM+ECC、PLP、多通道、LDPC+软判决、RAID/端到端、QoS、telemetry、
  双端口/SR-IOV、安全、固件双槽；
- **移动追加**：UFS/eMMC、低功耗状态、热管理。

**决策顺序**：

1. 定市场与 workload（5.1）；
2. 从 5.2 勾出「必须有 / 建议有 / 可以不做」；
3. 映射到板卡/SoC 的硬条件（PCIe EP lane/REFCLK/PERST#、NAND 引脚、DRAM、电源）；
4. 逐项查数据手册 / BSP 核实（附录的「待核实」项）；
5. 定验证计划（稳态、掉电、故障注入），再接 §7 的选板步骤。

## 6. 当前平台案例：STM32H743 + MT29F4G08

> 这一节回答的是「在**现有条件**下如何做取舍」，不是「什么板子最像主控」。
> 结论先说：这块板是**学习载体**，不是主控硬件原型；很多硬件不符合主控，
> 这是「用现成板卡 + 低成本 + 不能自绘 PCB」的直接结果。

### 6.1 选择这块板的真实约束

> 板卡资料来源：正点原子 Apollo V2 资料页
> <https://www.openedv.com/docs/boards/stm32/zdyz_stm32h743_apolloV2.html>；
> 硬件参考手册 / 原理图 / 器件数据手册在页面所列「开发板资料 A 盘」。

先把板卡事实列清楚（只列与主控相关的）：

| 资源 | 板载 | 本项目用途 |
|---|---|---|
| MCU | STM32H743IIT6，LQFP176，2 MB Flash，1060 KB SRAM | 使用内部 Flash/SRAM |
| SDRAM | 32 MB（W9825G6KH / EM63A165TS-6G 等），挂 FMC | **未使用（主动 DRAM-less）** |
| NAND | 512 MB，支持 MT29F4G08 / FSNS8B004G / IS34ML04G081，挂 FMC | 使用 MT29F4G08 |
| QSPI NOR | W25Q256，32 MB | 未使用 |
| USB | Type-C，PA11/PA12 → OTG_FS，SLAVE/HOST | 使用 SLAVE（USB MSC） |
| 串口 / 调试 | USART1（PA9/PA10）/ SWD | 使用（CLI、日志、JLink） |
| EEPROM / 其他 | 24C02；RGB LCD、以太网、CAN、RS485、传感器 | 未使用 |

再列约束：

| 约束 | 事实 | 直接后果 |
|---|---|---|
| 已有平台 | 阿波罗 V2 核心板（STM32H743IIT6 + 512 MB NAND + 32 MB SDRAM + USB FS） | 外设 / 引脚固定，不能按主控需求重新选型 |
| 成本 | 开发板价格低，适合长期学习 | 不上高端 SoC / FPGA、多通道 NAND、PLP 电源 |
| 无 PCB 能力 | 本项目不具备自绘 / 打样 / 焊接 BGA 的条件 | 只能选**现成板卡 / 核心板**；难以加 PCIe EP / PLP / 多通道，且不可产品化 |
| 学习目标 | 端到端学习主控设计 | 优先「能跑通、能观察、能讲清」，不追求产品规格 |

**判断顺序**：

1. 先接受平台固定 —— 不把「换硬件」当作解题手段；
2. 再在固定平台上选**能学的分支**（DRAM-less、软件 FC、PIO、USB MSC）；
3. 最后明确哪些能力**这块板学不到**，留给下一块板或资料。

### 6.2 与真实主控的差距

| 维度 | 主流主控 | 本平台 | 影响 | 本仓怎么补 |
|---|---|---|---|---|
| 主机接口 | PCIe EP + NVMe SQ/CQ/doorbell | USB FS + MSC BOT | 无硬件队列、无 PRP/SGL、吞吐低 | HIL 软件请求拆分/排队/完成回执 |
| CPU | 多核 / 专用核（常见） | 单 Cortex-M7 | CPU-bound、无核间并行 | 轮询 + 后台时间预算 |
| NAND 通道 | 多 ch × CE × LUN | 1 ch × 1 CE × 1 LUN × 2 plane | 无通道并行、无多 LUN 仲裁 | 单 LUN 先把命令流/账本做对 |
| FC | 硬件 FC + DMA | 软件 FC + PIO | CPU 搬数据、命令开销大 | cmd/desc/buf 三池 + UOP |
| ECC | LDPC / 强 BCH + 软判决 | BCH(13,4) 硬判决 | 纠错强度低、无软判决 | BCH + CRC/加扰；LDPC 留作下一平台 |
| L2P 驻留 | DRAM 型（主流分支） | DRAM-less（32 MB SDRAM 未用） | 随机读多一次元数据读 | region cache + journal/CP |
| 掉电 | PLP + journal/CP | 无 PLP | 已回执写有掉电窗口 | VWC 撤销 + journal/CP + PFR |
| 介质 | TLC/QLC + SLC cache | SLC | 无 folding / SLC cache 问题 | 不实现 SLC cache（4.10） |
| 可靠性 | RAID / 双端口 / 热插拔 | 单盘 | 无阵列恢复 / 冗余路径 | 先做 ECC/BBT/WL/PFR |
| 调试 | trace / ILA / telemetry | SWD + UART | 可见性有限 | `stats`/`smart`/日志/fault capture |
| 安全 / 升级 | secure boot + A/B | SWD 烧写 | 无产品级安全 / 回滚 | 明确未实现（4.16/4.17） |

> 这张表就是「很多硬件不符合主控」的具体清单；它是平台边界，不是缺陷列表。
> 表中「主流主控」按消费级 / 企业级的主流形态列，具体档位差异见 §5。

### 6.3 被迫选择 vs 主动取舍

| 项 | 类别 | 原因 | 后果 |
|---|---|---|---|
| USB FS / MSC BOT | 被迫 | 板载只有 OTG_FS，无 PCIe | 学不到 NVMe 队列 / 门铃 |
| 单 NAND / 单 LUN / 2 plane | 被迫 | 板载一颗 NAND | 学不到多通道仲裁 |
| PIO 数据面 | 被迫 | 无硬 FC / NAND 专用 DMA 通路 | CPU 开销大、吞吐受限 |
| 无 PLP | 被迫 | 板级电源没有掉电保持 | 只能用 journal/CP 兜底 |
| BCH 硬判决 | 被迫 | 器件 / 算力约束 | 学不到 LDPC 软判决 |
| DRAM-less | **主动** | SDRAM 可用，但选择学 DRAM-less 分支 | 元数据路径更复杂，换到 cache/PFR 经验 |
| CW 级 L2P | **主动** | 学更细粒度映射与 ECC/GC 关系 | 表项多、cache 压力大 |
| 软件 FC | **主动** | 没有硬 FC，正好学命令流 / 池管理 | CPU 开销，但可观测 |
| Journal + CP + PFR | **主动** | 无 PLP，必须把恢复链做完整 | 写放大 / 恢复时间 |
| 单 LUN 先行 | **主动** | 先做对小系统，再谈并行 | 多 LUN 留作下一阶段 |
| RAID 关 / IWDG 关 | **主动（范围控制）** | 未验证分支不放开 | 能力缺口登记在案 |

**为什么区分**：被迫选择决定「这块板学不到什么」；主动选择决定「这块板重点学什么」。
两者混在一起，会把平台限制误当成设计结论。

### 6.4 这块板学得到 / 学不到

**学得到**：

- HIL 请求生命周期（软件队列也能讲清提交 / 完成 / 回执）；
- FTL 映射、GC/WL、OP、BBT、journal/CP/PFR；
- 软件 FC 的命令流、描述符链、池管理；
- ECC/CRC/加扰、VWC 早回执、失败撤销；
- 板端 UT、H2testw、`stats`/`smart` 的验证方法。

**学不到（只能看资料或换平台）**：

- PCIe/NVMe 的 SQ/CQ、doorbell、PRP/SGL、MSI-X；
- 多通道 / 多 die 并行与仲裁；
- 硬件 FC、NAND 数据面 DMA、LDPC 软判决；
- PLP 电源设计、双端口 / SR-IOV、热插拔；
- ZNS/FDP、multi-stream 的 host 侧生态；
- 高速信号完整性（PCIe / NAND 长走线）。

> 学习时先把「学得到」做深；「学不到」的部分用 §3/§4 的机制说明补，不假装板上验证过。

### 6.5 模块级策略

| 模块 | 选择 | 主流替代 | 代价 / 学习点 |
|---|---|---|---|
| HIL | USB MSC BOT + 软件请求拆分/排队/完成回执 | NVMe SQ/CQ + doorbell | 无多队列；学请求生命周期 |
| FTL 映射 | CW 级（IU=512 B）L2P | 页级/块级/DFTL | 表项多；学映射粒度与 ECC/GC 关系 |
| L2P 驻留 | DRAM-less + cache + journal/checkpoint | DRAM 型 L2P | 复杂；学 cache/CP/PFR |
| GC/WL | 水位/紧急 + victim 评分 + 静态 WL | Greedy/FASTer/DAC | 写放大/尾延迟；学 victim/OP/WL |
| 元数据/掉电 | Journal + Checkpoint + PFR | 纯 CP/纯日志 | 写放大/恢复时间；学 WAL/CP/replay |
| FC-NAND | cmd/desc/buf 三池 + UOP | 硬 FC 队列/门铃 | 软件开销；学命令流/资源管理 |
| ECC/CRC/加扰 | BCH(13,4) + LFSR + CRC | LDPC/硬件 ECC | 纠错强度；学数据路径保护 |
| VWC | 早回执 + 读覆盖 + FAILC 撤销 | write-through/严格 FLUSH | 掉电窗口；学写缓存/flush |
| 验证 | 板端 UT + H2testw + `stats` | host 模拟 | 回归慢；学板端判据 |
| 范围 | RAID 关、IWDG 关、多 LUN 未上板 | 全功能产品 | 分支未验证；学范围控制 |

### 6.6 一条 I/O 的旅程

```text
一笔 16 KiB host 写：
USB MSC WRITE(10) → HIL 拆 run → FTL 更新 L2P cache + journal + wbuf
→ FC 分配 cmd/desc/buf → UOP 建链 → FMC PIO + BCH/加扰 → NAND program
→ 完成回调 → host 回执

一笔读：
L2P lookup → FC read → ECC decode → host buffer

掉电：
Checkpoint + Journal replay / PFR
```

### 6.7 学习路线

1. 先承认平台差距：哪些是被迫、哪些是主动（6.1~6.4）；
2. 跟一笔写：HIL → FTL → FC-NAND → NAND；
3. 跟一笔读 + GC：L2P lookup / cache / victim；
4. 掉电恢复：Journal / Checkpoint / PFR；
5. 验证：板端 UT / H2testw / `stats`。

### 6.8 状态（截至 2026-11-20）

| 项目 | 状态 | 证据 |
|---|---|---|
| debug / release 构建 | 已验证 | 2026-11-20，零新告警 |
| 板端 UT（release 全量） | 已验证 | 2026-11-22，318/318，3 SKIP；seed `0x81a7c024`，697.30 s；日志 `docs/archive/test_logs/2026-11-22_release_ut_full.log` |
| 板端 H2testw | 已验证 | 2026-11-21，415/423 MB，938/866 KB/s，0 错误 |
| `CONFIG_FC_RAID` | 关闭，未验证 | `include/fc/fc_interface.h` |
| IWDG 看门狗 | 关闭，待上板决策 | `src/main.c` |
| read recovery `0x89` | 保留地址，空转 | `src/ftl/read_recovery.c` |

> 上表证据对应 2026-11-22 的镜像。

## 7. 下一平台选择

> 选平台按 §3 的**主控物理硬件块**倒推，不按开发板外设挑。
> 硬约束：**现成板卡 / 核心板**（本项目不能自绘 PCB）、**PCIe EP 已引出**、**NAND 接口可接**。
> 候选原始数据在附录 A；本节给筛选方法、结论和买前核对清单。

### 7.1 选择原则

1. 先定要学哪一层：**硬件 IP**（NAND 控制器 / ECC / DMA / NVMe 队列）、**FTL 软件**、还是**整机系统**；
2. 按主控硬件块优先级选：主机接口 → NAND 接口 → CPU / 内存 → ECC / DMA → 电源 / 调试；
3. 芯片 datasheet ≠ 板卡可用：必须看原理图、引脚、连接器；
4. 低端定位：一块板覆盖不了所有，接受**分阶段 / 两块板**。

### 7.2 主控硬件块 → 平台要求

| 硬件块（§3） | 最低要求 | 理想 | 板卡检查点 |
|---|---|---|---|
| 主机 PHY / 控制器（3.1/3.2） | PCIe EP，Gen2 x1 | Gen3/4 x4，多 lane | lane / REFCLK / PERST# 已引出；控制器支持 EP 模式 |
| 命令 / 队列（3.3） | NVMe SQ/CQ/doorbell 可在软件/PL 实现 | 多队列、MSI-X、多 namespace | 主机侧有 DMA 到 SQ/CQ；有足够 SRAM/DRAM |
| CPU（3.4） | ≥2 核 | 4+ 核 / 含 R5 或 MCU | 主频、cache、SMP、核间通信 |
| 内存（3.5） | DDR ≥512 MB | ≥2 GB、ECC、带宽匹配 | DDR 控制器、总线带宽、SRAM/BRAM |
| NAND 接口（3.6/3.7） | 1 通道 / 1 CE，ONFI 或 Toggle | 多通道、multi-plane、cache read/program | **板载 NAND 或引出连接器**；CE/RB/ALE/CLE/RE/WE/DQ；电压/时序 |
| ECC（3.8） | BCH | LDPC + 软判决 | 硬 ECC 引擎或 PL 资源（LUT/BRAM/DSP） |
| DMA（3.9） | host DMA + NAND DMA | 描述符/SG、多通道、cache 一致 | AXI HP 端口、DMA IP、总线带宽 |
| 电源 / PLP（3.11） | 外部稳定供电 | PFail / PLP | PMIC、掉电检测、电容空间、电源时序 |
| 调试（3.13） | JTAG/SWD + UART | trace / ILA | 调试口引出、工具链、ILA 资源 |
| 扩展 / 可靠性（3.14） | 单端口 | 双端口 / 热插拔 | 连接器、板级空间、背板支持 |

### 7.3 候选平台筛选

先按 7.2 的硬条件筛：**无 PCIe EP、无 NAND 可接、非现成板** 直接排除。
低端板卡资料常不完整，下表保留候选并标「待核实」，买前必须逐项确认。

| 候选 | CPU | PL/FPGA | PCIe EP | NAND | 内存 | 成本档 | 适合 | 关键待核实 |
|---|---|---|---|---|---|---|---|---|
| Zynq-7000（Zybo Z7 / Cora Z7 / PYNQ-Z2） | 2×A9 | 有 | 视板卡（Zybo Z7 需核实） | PS NAND / PL 自定义；板载或引出待核实 | DDR3 | 低 | FPGA IP 实验室、OpenSSD 参考 | PCIe lane/REFCLK/PERST#；NAND 接入 |
| Cyclone V SoC（SoCKit / DE10-Nano） | 2×A9 | 有 | SoCKit 有（待核实）；DE10-Nano 无 | HPS NAND / PL；板载待核实 | DDR3 | 低 | Intel 工具链、IP 实验 | NAND 接入；PCIe 引出 |
| Zynq UltraScale+（KV260 / ZCU104） | 4×A53 + PL | 有 | 有（Gen3/4 按型号） | PL 自定义；板载待核实 | DDR4 | 中高 | 完整多核 + FPGA 系统 | NAND 接入；价格 |
| i.MX 8M Mini/Plus EVK | 4×A53 + M7 | 无 | 需核实 EP 模式 | GPMI NAND + BCH（板载待核实） | DDR4 | 中 | NVMe + FTL 软件，不写 RTL | PCIe EP 模式；NAND 板载 |
| STM32MP25 DK | 2×A35 + M33 | 无 | Gen2 x1（EP 待核实） | FMC/NAND（待核实） | DDR4 | 低 | STM32 生态入门 | EP 模式；NAND 接入 |
| PolarFire SoC Icicle | 4×RISC-V + FPGA | 有 | 有（Gen2，待核实） | PL 自定义；板载待核实 | DDR4 | 中高 | RISC-V + FPGA | 文档/生态；NAND 接入 |

> 表中「待核实」是买前必须查数据手册/原理图/原理图引脚的项目；不要凭芯片型号推断板卡能力。

**低端 FPGA SoC 板最常见的两个缺口**：

1. **有 PCIe 槽，但只做 RC**（例如 M.2 存储槽）——不能当 NVMe 设备（EP）；
2. **SoC 有 NAND 控制器，但板卡只引 QSPI / SD / eMMC**，没有 NAND 芯片或连接器。

两者任一不满足，Zynq-7000 路线就不成立。此时：

- 只学 FTL / NVMe 软件 ⇒ 选 i.MX 8M Mini/Plus 或 STM32MP25（GPMI/FMC，待核实）；
- 坚持写硬件 IP ⇒ 找**带 NAND 接入**的 FPGA 板 / SoCKit（待核实），或接受「先无 NAND，只做 PCIe/队列 IP」。

### 7.4 三条路线与推荐

| 路线 | 目标 | 候选 | 代价 |
|---|---|---|---|
| A 硬件 IP 实验室 | 写 NAND 控制器 / ECC / DMA / NVMe 队列 IP | Zynq-7000 低成本板、Cyclone V SoC | PL 资源有限；NAND 接入要确认 |
| B NVMe + FTL 软件 | 跑通 PCIe EP + NVMe + FTL，不写 RTL | i.MX 8M Mini/Plus、STM32MP25 DK | 无 PL；EP 模式/NAND 要确认 |
| C 完整多核 + FPGA | 多核 + PL + 多通道 + 企业特性 | Zynq UltraScale+、PolarFire SoC | 贵；工具链/功耗门槛高 |

**推荐（低端、低成本、无 PCB 能力）**：

1. **首选下一平台：Zynq-7000 低成本板（XC7Z020 级别）**，前提是 7.5 核对通过。
   理由：2×A9 多核 + PL 可写 IP + PCIe EP 可能（视板卡）+ 成本低 + OpenSSD 参考生态；
   缺点是 PL 资源有限，一块板通常只能做单通道 NAND + ECC + DMA + 简化 NVMe 队列。
2. **备选：Cyclone V SoCKit**（Intel 工具链、PCIe Gen2 x4）或 **i.MX 8M Mini/Plus EVK**
   （只做 NVMe + FTL 软件，不写 RTL）。
3. **不建议第一块**：Versal / Arria 10 / Zynq UltraScale+ 高端板——贵，且与 OpenSSD 重叠。
4. **两块板分阶段**：先用 Zynq-7000 做 FPGA IP 实验室，再用 i.MX 8M / STM32MP25 做
   NVMe + FTL；不要指望一块低价板覆盖全部主控硬件。

### 7.5 买前核对清单

| 检查项 | 判据 | 不满足的后果 |
|---|---|---|
| PCIe EP | lane / REFCLK / PERST# 已引出；控制器支持 EP 模式 | 只能做 host，不能做 NVMe 设备 |
| 板卡形态 | 现成板卡，或现成核心板 + 现成载板；不需要自绘 PCB | 本项目无法落地 |
| NAND 接入 | 板载 NAND 或引出 CE/RB/ALE/CLE/RE/WE/DQ | 无法接 NAND，FC 层无从验证 |
| NAND 电压/时序 | 3.3 V / 1.8 V 可选；ONFI/Toggle 时序 | 器件不匹配，读写失败 |
| DDR | 容量/带宽满足 L2P 与缓冲 | L2P 放不下、性能瓶颈 |
| FPGA 资源 | LUT/BRAM/DSP/transceiver、AXI HP 端口 | 写不下 NAND 控制器/ECC/DMA |
| 调试 | JTAG/SWD、UART、ILA | 板上不可观测 |
| 电源 | 外部稳定供电；PFail/PLP 可行性 | 掉电实验无法做 |
| 文档 | 原理图、BSP、参考设计、Linux | 引脚/时序靠猜，返工 |
| 供货/成本 | 现货、低价、长期可买 | 学习计划中断 |

### 7.6 结论

- **下一平台不是「一块最像主控的板」，而是「能覆盖你要学的那一层、且买得到、接得上 NAND/PCIe 的现成板」**。
- 低端路线优先 **Zynq-7000 低成本板**；如果只学 FTL/NVMe 软件，选 **i.MX 8M Mini/Plus**。
- 任何候选在付款前，必须完成 7.5 的核对；「待核实」项不能靠型号推断。
- 平台选定后，按 §5.6 的矩阵定「必须有 / 建议有 / 可以不做」，再进入板级验证。

### 7.7 待核实

- [ ] Zynq-7000 候选板的 PCIe EP（lane/REFCLK/PERST#）与 NAND 接入方式；
- [ ] Cyclone V SoCKit 的 NAND 接入与 PCIe 引出；
- [ ] i.MX 8M Mini/Plus 的 PCIe **EP 模式**与 GPMI NAND 板载情况；
- [ ] STM32MP25 DK 的 PCIe EP 模式与 NAND 接入；
- [ ] Zynq-7010/7020 的 PL 资源能否容纳「NAND 控制器 + ECC + DMA + 简化 NVMe 队列」；
- [ ] 预算档位与工具链偏好（Vivado/Vitis、Quartus、STM32Cube）。

---

## 8. FTL 架构选择（ADR）

> 本节从 `ftl_design.md` 的 ADR 章节迁入，保留 ADR-1~17 编号；FTL 设计文档只保留索引。
> 下文未注明文档名的 `§x.y` 均指 `ftl_design.md`。

### ADR-1 映射粒度：纯 CW 级页映射

| 项 | 内容 |
|---|---|
| 背景 | NAND 只能异地更新；映射粒度决定表大小、写放大、GC 复杂度 |
| 选项 | 块映射 / 混合映射（BAST/FAST）/ 纯页映射（4 KiB 或 CW） |
| 选择 | **纯页映射，粒度 = 1 CW = 512 B** |
| 理由 | 写放大较小（模型值，实测见验证文档）；与 FC 的 ECC/CRC/加扰单位完全对齐，无粒度折算；GC 可只搬页内有效 CW |
| 主流 | 现代 SSD 普遍纯页映射；UFS/eMMC 也是页级（见 §1.6） |
| 代价 | 表最大（4 MiB）；GC 逐 CW 搬移，实现复杂 |
| 重估触发 | 若改用 4 KiB IU，表缩小 8×，但页内小写会放大写放大；当前器件 512 B CW，不换 |
| 出处 | `docs/reference/README.md` A4 SPDK FTL、D4 DFTL/AppL；本仓 WA 模型见验证文档 |

### ADR-2 L2P 存放：NAND 全表 + SRAM region cache

| 项 | 内容 |
|---|---|
| 背景 | 4 MiB 表装不进 SRAM；外置 DRAM 本板不可用 |
| 选项 | 全表 DRAM / 全表 SDRAM / 全表 SRAM / NAND + SRAM cache |
| 选择 | **NAND 存全表 + 64 行 SRAM region cache（DRAM-less）** |
| 理由 | 与 UFS/eMMC 的 DRAM-less 形态同构；SRAM 预算可支撑 64 行；掉电一致性由 Journal/CP 保证 |
| 主流 | UFS 4.0 / eMMC / 消费级 DRAM-less 均为“L2P on NAND + SRAM cache” |
| 代价 | 全盘随机访问命中率低（64 行 ≈ 3%）；需要 thrashing 降级 |
| 寿命核算 | 上界模型：一次全盘写 ≈ 1.8 万次 region flush；L2P 元数据区寿命约 5.7 TB 写入；**Journal（4 块）约 1.6 TB，是更短的瓶颈**。开发/学习负载（≤100 MB/天）有充足余量 |
| 重估触发 | 增加 DRAM/SDRAM；或 cache 行数显著增加（需重算 SRAM 预算） |
| 出处 | `docs/reference/README.md` A4 SPDK FTL、D4 DFTL；DRAM-less 对照 §1.6 |

### ADR-3 RAID 不实现

| 项 | 内容 |
|---|---|
| 背景 | 跨 die/plane 的 XOR 恢复可提高可靠性，但增加 GC/parity 管理复杂度 |
| 选项 | 单 plane 恢复 / 跨 die·plane XOR / 全盘 RAID |
| 选择 | **`CONFIG_FC_RAID = 0`，不实现** |
| 理由 | 当前 SLC 单盘 BER 较低；学习主控优先把分层/队列/映射做对；RAID 会显著增加 GC 与元数据复杂度 |
| 主流 | 企业级常用；消费级 DRAM-less 通常不做 |
| 代价 | 单 die/页不可纠错时只能标坏/丢数据 |
| 重估触发 | 换 TLC/QLC 或需要企业级可靠性 |
| 出处 | 本仓自定；企业级 RAID 对照见 `storage_controller_design.md` |

### ADR-4 FTL 通过 FC 接口访问 NAND

| 项 | 内容 |
|---|---|
| 背景 | NAND 时序、地址周期、ECC/CRC/加扰必须统一管理 |
| 选项 | FTL 直接操作 NAND / 经 FC 接口 |
| 选择 | **FTL 只调用 `ftl_fc_interface` 的 helper / 同步异步原语** |
| 理由 | 数据保护链完整；多平面/流水线能力复用；FTL 不感知 NAND 时序 |
| 主流 | 主控固件的标准分层（FTL / HAL / 介质） |
| 代价 | 每次读写有 alloc/submit/poll/release 开销 |
| 重估触发 | 不重估；这是分层底线 |
| 出处 | OpenSSD/SPDK 分层对照；本仓 `ftl_fc_interface` 契约 |

### ADR-5 Cache 替换：CLOCK

| 项 | 内容 |
|---|---|
| 背景 | 64 行 cache 需要近似 LRU，且不能有动态内存/链表开销 |
| 选项 | LRU（链表）/ FIFO / CLOCK / 2Q |
| 选择 | **CLOCK（二次机会）** |
| 理由 | 命中率接近 LRU；O(1) amortized；只需一个整数字段；无动态分配 |
| 主流 | 嵌入式/DRAM-less 常用 |
| 代价 | 不如 LRU 精确；顺序/随机混合负载下略差 |
| 重估触发 | 若命中率成为瓶颈，可评估 CLOCK-pro / 自适应预取（见 §7.2） |
| 出处 | SPDK FTL（A4）的 cache 形态；CLOCK 为通用替换算法 |

### ADR-6 Super Block：跨 plane 条带化

| 项 | 内容 |
|---|---|
| 背景 | 双 plane 可并行编程/擦除；按单块管理会浪费并行度 |
| 选项 | 单块管理 / 跨 plane Super Block |
| 选择 | **跨各 plane 的逻辑 SB；各 plane 独立选块（Policy B）** |
| 理由 | Multi-Plane 提升吞吐；擦除/GC 以 SB 为粒度；坏块导致两块编号不同不影响并行 |
| 主流 | 企业级跨 die、消费级跨 plane 的通用形态 |
| 代价 | allocator 配对/关闭逻辑更复杂；退化单 plane 条目需要哨兵 |
| 重估触发 | 换多 die 封装或 plane 数变化 |
| 出处 | `docs/reference/README.md` A1 Cosmos+、A4 SPDK FTL；本仓 Policy B 自定 |

### ADR-7 温度分区：Hot / Cold 双 Open SB

| 项 | 内容 |
|---|---|
| 背景 | 热数据无效页增长快，冷数据几乎不变；混写会降低 GC 效率 |
| 选项 | 单 Open SB / Hot + Cold 双 SB / 多温区 |
| 选择 | **两个 Open SB：HOT（host 写）/ COLD（GC 搬移）** |
| 理由 | HOT 块很快变成高无效比例，GC 回收便宜；COLD 块几乎不用回收；纯软件 |
| 主流 | 消费级 2~4 温区；企业级多级温度 |
| 代价 | allocator 维护两个 SB；写路径要指定温度 |
| 重估触发 | 容量/负载需要更多温区 |
| 出处 | 本仓自定；消费级 2~4 温区为公开经验值 |

### ADR-8 OP 预留 10%

| 项 | 内容 |
|---|---|
| 背景 | GC 需要空闲块缓冲；坏块替换需要储备；无 OP 时写压力会直接撞 GC |
| 选项 | 0% / 7~15% / 28% |
| 选择 | **10%（每 plane 204 块，共 408）** |
| 理由 | 给 GC 足够缓冲，前台写入不因 GC 断崖；坏块替换后用户容量不变 |
| 主流 | 消费级 7~15%，企业级 7~28% |
| 代价 | 用户可见容量 **424 MiB**（512 MiB − 固定元数据 24.5 − OP 51 − Spare 12.5） |
| 重估触发 | 容量/性能目标变化；`FTL_OP_PERCENT` 可配 0~28 |
| 出处 | 消费级/企业级 OP 区间为公开经验值；具体 10% 本仓自定 |

### ADR-9 Free Pool：一个已擦除池 + 三级水位 + 独立 OP/Spare

| 项 | 内容 |
|---|---|
| 背景 | 需要“前台零等待”和“后台补池”，但不能把擦除放到关键路径 |
| 选项 | 三个物理池（预擦/普通/紧急）/ 一个池 + 水位 + 独立保留池 |
| 选择 | **一个已擦除池 + 三级水位（32/8/0）+ 独立 OP/Spare 池** |
| 理由 | 搬移是“先擦后归还”⇒ 池内没有未擦块，无法按擦除状态分区；前台零等待靠水位，不靠第二个池 |
| 主流 | 真实主控同样是“一个 free pool + 软/硬水位 + 独立保留池” |
| 代价 | 水位阈值是本仓经验值，需要按容量/OP 调整 |
| 重估触发 | 换更大容量或不同 OP 比例 |
| 出处 | SPDK FTL（A4）的 free band 水位；具体阈值本仓自定 |

### ADR-10 不实现 SLC Cache

| 项 | 内容 |
|---|---|
| 背景 | SLC cache 是 TLC/QLC 的写加速技术 |
| 选项 | SLC cache / 不做 |
| 选择 | **不实现** |
| 理由 | 介质本身就是 SLC；没有“慢速 TLC 写”需要掩盖 |
| 主流 | SLC 介质不需要；TLC/QLC 才用 |
| 代价 | 无 |
| 重估触发 | 换 TLC/QLC 介质 |
| 出处 | 本仓自定（介质为 SLC）；TLC/QLC 对照见公开资料 |

### ADR-11 坏块管理：出厂扫描 + 运行时隔离 + Block Sparing

| 项 | 内容 |
|---|---|
| 背景 | 坏块不可用；出厂标记会被擦除毁掉；运行时还会新增坏块 |
| 选项 | Block Skip / Block Remap Table / Block Sparing |
| 选择 | **出厂扫描（仅空片）+ 运行时标坏 + Spare Pool 按 plane 补充** |
| 理由 | 不需要两层映射；PAA 直接物理；坏块配对异号不影响 Multi-Plane；Spare 保容量水位 |
| 主流 | 企业级也用 sparing + 数据迁移，不用 remap 表 |
| 代价 | 出厂扫描约 250 ms；Spare 池占用 100 块 |
| 重估触发 | 坏块率显著高于预期，或需要在线替换策略 |
| 出处 | Micron AN1819 / ONFI；候选池算法本仓自拟 |

### ADR-12 多 LUN：共享 L2P cache + 全链路全局 LBA

| 项 | 内容 |
|---|---|
| 背景 | 每个 `(ch,ce,lun)` 一个实例；若每实例一份 cache，SRAM 装不下；若数据通路用局部 LBA，跨实例语义复杂 |
| 选项 | 每实例独立 cache + 局部 LBA / 每 `(ch,ce)` 共享 cache + 全局 LBA |
| 选择 | **每 `(ch,ce)` 一份共享 cache；数据通路全链路全局 LBA；映射页落盘用 owner 的局部 region** |
| 理由 | cache 容量按 `(ch,ce)` 而不是按 LUN 增长；全局 LBA 让 host/Journal/META 只有一套坐标；owner 解析把“落盘位置”收在单一漏斗 |
| 主流 | 主流多 LUN 主控共享 L2P cache；全局 LBA + 分片是常见形态（具体实现本仓自定） |
| 代价 | owner 解析回调、全局脏位图、CP/recovery/flush 都要按 owner 过滤 |
| 重估触发 | 换多 die 芯片；或 cache 命中率需要 per-LUN 隔离 |
| 出处 | [`../archive/multi_lun_generalization.md`](../archive/multi_lun_generalization.md)；`l2p_cache.h` 的全局坐标注释 |

### ADR-13 实例描述符化 + 模块自持 workspace

| 项 | 内容 |
|---|---|
| 背景 | `ftl_ctx_s` 内嵌大数组时，单实例描述符很大；多实例线性增长，超出片上 RAM 预算 |
| 选项 | 继续内嵌 / 全部放外部存储 / 描述符 + 模块自持 workspace |
| 选择 | **描述符只留指针 + 热状态；每个模块自持 workspace 并选择落点；落点由平台决定** |
| 理由 | 多实例可装下；workspace 所有权归属清晰；落点可控但不写进 FTL 契约 |
| 主流 | 主控固件用 descriptor + 表指针，由平台链接/散列决定落点（OpenSSD/SPDK 同思路） |
| 代价 | 不清零 workspace 必须显式初始化；CPU-only 与 DMA-visible 的区分是设计约束；跨域访问需平台保证 |
| 重估触发 | 维度继续增加；或表结构显著变大 |
| 出处 | OpenSSD/SPDK 的 descriptor + workspace 形态；落点本仓自定 |

### ADR-14 主机背压：三态返回，不内部排队

| 项 | 内容 |
|---|---|
| 背景 | 多个提交者（host / GC / metadata）竞争有限的命令/描述符/缓冲/pending 资源 |
| 选项 | ① 直接返回错误 ② 内部重试队列 ③ 动态 budget ④ 主机级 doorbell 流控 |
| 选择 | **①：pending 表满/资源不足即返回 `FTL_AGAIN` 或 `FTL_ERROR`** |
| 理由 | 零额外 SRAM；资源回滚路径完整；本仓 host 为 USB MSC（BOT）与 UART CLI，提交者自带泵 / 同步等待，不需要 doorbell；内部重试队列与 DRAM-less 目标冲突 |
| 主流 | 消费级 DRAM-less 常见；企业级会用 credit/动态 budget |
| 代价 | 无优先级/公平性；极端竞争下 host 可能看到错误 |
| 重估触发 | 多 LUN 并发导致 pending 经常打满；或接入 NVMe host |
| 出处 | 旧 D6 分析（现并入本节）；`FTL_AGAIN` 语义见 `ftl_interface.h` |

### ADR-15 NAND 流水线：Multi-Plane + 受限 Cache Program

| 项 | 内容 |
|---|---|
| 背景 | 需要重叠 NAND 延迟；三种技术：Multi-Plane、Cache Program、ISSUE/WAIT/FETCH |
| 选项 | 只用单命令 / Multi-Plane / + Cache Program / + ISSUE/WAIT |
| 选择 | **写用 Multi-Plane；host 16 KiB 与 GC/WL 顺序搬移使用受限 Cache Program 批次；读在 LUN>1 时走 ISSUE/WAIT/FETCH** |
| 理由 | 器件支持 `80h-11h-80h-15h`（中间页）与 `80h-11h-80h-10h`（末页）；15h 完成只代表 cache 可复用、tPROG 仍在后台。**不提前确认**：中间页只释放命令，只有末页 10h 成功且 FAILC/FAIL 均无错误后才统一更新 L2P/Journal，因此不违反先编程后映射（program-before-map）。掉电时未确认页没有映射，恢复会关闭部分写块（不续写），安全。 |
| 主流 | 主流主控用 Multi-Plane + 流水线；cache program 属器件特性，批次策略是**本仓自拟**，不是行业标准形态 |
| 代价 | 需要 FAILC 状态检查；批次失败只能关闭目的 SB（不能 rewind）；GC 批次多占约 16 KiB SRAM |
| 重估触发 | 引入 HSQ / FC chain 或跨 LUN batch；或器件 datasheet 的 FAILC 语义变化 |
| 出处 | Micron MT29FxG08xAA datasheet Figure 74（Two-Plane Program Page Cache）与状态寄存器 FAILC 说明；op44/op48 的用法见 `fc_nand_design.md` §3.12 |

> ⚠ 15h 后状态寄存器 bit1 = `FAILC` 表示**上一页** cache program 的成败；只检查 bit0
> `FAIL` 会把中间页失败静默当成成功。op44 与末页 op48 都使用 `NAND_UOP_CHECK_FAIL_CACHE`
> （FAIL | FAILC）；首页用 op49 只查 FAIL，避免残留 FAILC 误判。
> host 16 KiB 正常路径的 cache program 收益与上板结果见验证文档。
> **FAILC 负例、失败注入、掉电注入仍未做**，不得据此声称 cache program 全部已验证。

### ADR-16 后台调度：工作量配额 + must-run

| 项 | 内容 |
|---|---|
| 背景 | 单线程 superloop，后台任务多；既要防饿死，又要防 host 被拖慢 |
| 选项 | 每轮全跑 / 固定顺序三级服务 / 每任务时间预算 + must-run / 每任务工作量配额 + must-run |
| 选择 | **10 项任务表：6 must-run（不 gate、不占配额）+ 4 预算任务；三级负载→固定配额/掩码** |
| 理由 | must-run 只放“不跑会死锁/挂起”的动作；预算任务按表序/credit 截断；每秒后台量由 token bucket（10 ms 补 grant_units）固定，单轮墙钟只做异常兜底——前台保护是确定性的（掩码/补充速率） |
| 主流 | 主控固件常见“poller + 后台调度器 + 每队列/每任务配额”；本仓用固定分档而非连续带宽调节 |
| 代价 | 分档粒度粗；时钟缺失时墙钟兜底失效（配额仍生效） |
| 重估触发 | 多核/RTOS；或需要更细的带宽比例控制 |
| 出处 | [`storage_controller_design.md`](storage_controller_design.md) 的调度形态；SPDK core poller 对照 |

### ADR-17 读写 run：FTL 构链，FC 执行 chain

| 项 | 内容 |
|---|---|
| 背景 | cache read / cache program 的 45h…47h / 49h…48h 必须在器件 cache 管线上连续执行；任一条被其他命令插入就会破坏序列 |
| 选项 | 依赖“全 FTL 命令走 SQ 且连续入队”的全局假设 / FTL 逐条提交并 defer 插入者 / **整条 run 标成 FC chain，控制器锁 LUN** |
| 选择 | **FTL 决策并构链**：L2P 前向扫描决定 run 边界，按位置选 45h/46h/47h（写 49h/44h/48h），末条 `FC_CHAIN_END`、其余 `FC_CHAIN_MORE`，批量提交原子连续入队；**FC 执行**：`fc_poll_chain_length` 每趟最多取 `FC_SCHEDULER_CHAIN_MAX`（= per-LUN FIFO 深度 4，**填充窗口**），批尾 MORE 时 `chain_owner` 跨轮次保留、另一队列回推，END 才解锁 |
| 理由 | 链总长 = run 页数，不受 FIFO 深度限制；跨轮次的 owner 状态解决“HSQ 插入劈开序列”；FC 侧复用既有 chain 机制，**无新增功能代码** |
| 主流 | 分层上 FTL 决定读写连续性与映射，控制器执行 NAND 序列；公开项目（Cosmos+ / Jasmine / SimpleSSD）未给出 cache read/program 的源码级序列实现，**具体 chain 机制是本仓自拟**（`fc_nand_dispatch.c`），不是行业标准形态 |
| 代价 | 链跨轮次会推迟同 LUN 另一队列的命令到链结束；每 4 条填充窗口之间 FIFO 排空，窗口边界暴露一次 tR（当前 USB FS 下可忽略） |
| 重估触发 | USB HS / DMA 使 host 传输 > 16 KiB；RR 改走 HSQ；或需要“失败即停”（当前 D7 = 失败页报错、其余成员继续执行） |
| 出处 | ONFI / MT29F4G08 Two-Plane Read Cache 45h/46h/47h、Cache Program 49h/44h/48h（`docs/reference/README.md`）；`fc_poll_fill_queue` / `fc_poll_chain_length` 见 `fc_nand_design.md` §3.5 |

> 链契约：一条 chain 的成员必须同优先级、末条 `FC_CHAIN_END`、经
> `fc_submit_batch` 原子入队；`fc_submit_batch` 在提交前拒绝违反者
> （debug `ASSERT` + release 返回 `FC_STATUS_INVALID_ARGUMENT`；见 `fc_nand_design.md` §3.12）。
>
> ⚠ chain 路径当前仅有 UT 覆盖；首启用必须上板验证跨轮次 owner / HSQ 回推 / 失败页状态（建链类改动）。
>
> ⚠ chain 写路径不处理 RAID stripe encode（RAID 关闭；启用前见 `known_open_items.md`）。


## 9. FTL 模块主流对照

> 本节从 `ftl_design.md` 各模块的「主流对照」迁入；FTL 设计文档只保留「设计取舍」摘要。
> 下文未注明文档名的 `§x.y` 均指 `ftl_design.md`。

### 9.1 引导与掉电恢复

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.14.8）。

DRAM-less 主控普遍采用「快照 + 增量日志 + 全盘扫描兜底」三段式：快照限定恢复上界，
日志补 post-snapshot 增量，扫描处理元数据损坏。**本仓自定**的部分是块元数据快照的
16-bit/块编码与 fresh/clean/PF 的判型口径；快照/日志/扫描的分工是 DRAM-less 的通用形态
（DFTL 等，见 `docs/reference/README.md`）。

---

### 9.2 分配器 / BST / Super Block

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.9.8）。

每块一个 metadata/descriptor、Free Pool + OP + Spare 的池化、Super Block 跨 plane 条带化，
是主流 SSD 控制器的共同形态（OpenSSD 全局表、SPDK FTL 内嵌表）。**本仓自定**的是
`valid_count` 的 CW 粒度与“无逐页位图”（DRAM-less approach ③，见 §3.12）。

---

### 9.3 坏块表

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.10.8）。

坏块表 + 预留替换块（sparing）而不是 block remap 表，是主流做法：ONFI Multi-Plane 命令
每个 plane 有独立行地址，坏块导致两块编号不同不影响并行操作，因此不需要“两层映射”。
固定元数据分区沿用同一原则：分区 = 候选块池（名义区间跳过 BAD + 共享
metadata spare 补缺），record 靠 sequence 扫描定位，不维护 logical→physical 表。
出厂标记“先读后擦”的判据来自 Micron AN1819（见 `bbt.c` 的引文）。
⚠ 本仓的具体候选池算法（4 块/plane shared spare、硬下界、重建顺序）是**本仓自拟实现**，
不冒充行业统一数值；机制本身是主流 BBT + sparing。

---

### 9.4 元数据目录

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.6.13）。

“固定根（root/目录检查点）+ 专用元数据块池 + 日志区 + Directory”是主控固件的常见形态：
固定根给启动一个确定的查找起点，日志区吸收高频更新，Directory 追踪最新位置。
日志区本身是**固定预留**（46..93/plane），与目录检查点/CP/BBT 同属固定元数据；
这是“元数据独立、有界、容量扣除”的主流原则（SPDK FTL：nvcache 独立 bdev + 每 band
尾部 P2L；spare 在 device formation 时从 surfaced capacity 扣除）。
具体块数/放置是本仓自定，不冒充行业统一数值。

固定池为什么必须环形轮转、池深如何影响单块磨损与启动扫描，见 §2.3.7。

DRAM-less 侧的关键同构是 **flash reverse-mapping**：不常驻逐页有效位图，
靠页内 META + L2P 反查判有效（§3.12）。这与 Directory 是两件事：Directory 管
“region 页在哪”，META 管“页内某 CW 是否仍被 L2P 指向”。

---

### 9.5 L2P Cache

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.5.9）。

DRAM-less 主控的 L2P cache 普遍采用“按需加载 + 近似 LRU（CLOCK/2Q）+ write-back”，
并用日志/快照保一致性；区别在 cache 行数、替换算法、是否共享与是否做 read-ahead。
**本仓自定**的是 64 行、CLOCK、thrashing 滞回 90/70、“加载失败按全未映射处理”，
以及顺序读触发的单 region 预取（阈值 2 个请求 / 提前 64 LBA / 最多 2 个在飞）。

---

### 9.6 Journal

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.7.9）。

“写前日志 + 定期快照截断”是数据库与闪存 FTL 共用的崩溃恢复形态；DRAM-less FTL
把 L2P 增量写进 Journal、用 Checkpoint 限定回放长度，是主流做法（DFTL 等）。
DFTL 的 mapping page 是 512 条 × 4 B、整页回写；本仓的 `JSEG` 页采用同一形态
（LBA 隐式、4 B/条），只是把它放进 Journal 环形页并由 `page_seq` 定序。
**本仓自定**的是 4 块环形、127 条通用页 / 508 条顺序页、85%/95% 水位与同 LBA 合并策略。

---

### 9.7 Checkpoint

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.8.9）。

“全量快照 + 日志截断 + 崩溃安全的提交顺序”是主流 FTL/数据库的通用恢复模型。
块元数据快照（per-block metadata snapshot）对应企业级 SSD 的 block descriptor/accounting
snapshot。**本仓自定**的是 16-bit/块 + 8 B/块的紧凑编码、21 页/record 与 T1/T2/T3 阈值。

---

### 9.8 Host 提交 / group / pending

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.1.11）。

“请求拆分 + 完成组 + 子操作表 + 背压/待入账”是主控固件数据面的标准形态：host 命令被拆成
介质命令，完成按命令 id 回收，组完成只通知一次。**本仓自定**的是 32 项固定表、
8 轮 stall 预算与页打包的 `owners` 位掩码。


---

### 9.9 完成派发面

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.2.8）。

NVMe/主控数据面的基本模型是「提交队列 + 完成队列 + 命令 ID 旁表」：完成项只带命令 ID
和状态，host/主控用 ID 找回请求上下文；FTL 再把“完成该由谁收尾”映射到服务函数。
本仓把 NVMe 的 SQ/CQ 简化为 FC 的队列 + 全局完成表：

- `command_id` 对应 NVMe 的 CID，用来连接 FC 完成项与 FTL request 表；
- `request.op` 是本仓的“服务函数选择器”，不是 NVMe 的命令类型字段；
- 实例路由按命令自身的 `(ch,ce,lun)` 解析，对应主控按 target/queue 归属完成；
- **本仓自定**：按 command_id 直接索引的 request 表、单一全局 CQ、`先摘表项再回调` 的防重入顺序，
 以及派发深度对嵌套完成泵的拦截。

---

### 9.10 页打包写缓冲

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.3.7）。

写缓冲/页打包是主控数据面的常规优化：消费级用 SLC cache/写缓冲，企业级用 write
coalescing。本仓没有 SLC cache（介质本身就是 SLC），只做**同窗口页打包**；
**本仓自定**的是 8-CW 窗口与 owners 位掩码的完成聚合方式。

---

### 9.11 FTL service 与请求调度

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.4.11）。

“主循环 + 请求调度器 + 独立后台策略/配额 + 每任务优先级”是主控固件的常见形态
（SPDK core poller、企业级后台核）。**本仓自定**的是 10 项任务表、三级负载→固定配额
（不是连续带宽调节）与 must-run 集合的划分。

### 9.12 GC 策略与迁移引擎

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.12.9）。

GC 触发看自由块水位、victim 按有效性比例选、后台异步回收，是主流 FTL 的共同形态
（SPDK 的 free band 水位、DRAM-less 的空间压力触发）。本仓额外加了磨损/温度/年龄三项，
**这是本仓自定**的评分权重；它们不冒充“行业标准公式”。与 SPDK 的一个刻意差异：
本仓写路径在分配不到页时会**就地起紧急 GC**，SPDK 的写路径直接返回 NULL、GC 全在
poller 里——这条差异已登记。

---

### 9.13 读恢复

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.13.8）。

read retry（soft-decode）、邻居页辅助、RAID/XOR 是企业级 SSD 读恢复的常见组成。
本仓**裁剪**为「单次读 + CRC 重读一次 + 块退役」是器件/配置约束（0x89 保留、SLC
无 retry table、RAID 关闭），不是对这些机制的否定。**本仓自定**的是 4 级健康状态
与 1000 次 read-disturb 阈值。

---

### 9.14 FTL↔FC 接口

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.15.9）。

“FTL 只表达逻辑请求、HAL/FC 负责介质命令与数据保护”是主控分层的通用形态；
描述符链 + 完成状态是 NVMe/ONFI 数据面的常见实现。**本仓自定**的是 CW 子集位图建链、
`cmd->meta` 携带扇区 LBA 的方式与 FC 池深度。

---

### 9.15 cache 序列

> 迁入自 `ftl_design.md` 模块主流对照（现 §3.16.9）。

设备 cache 序列（`0x31/0x3F`、`15h/10h`）的器件语义见 Linux MTD rawnand 与 MT29F4G08
datasheet（`docs/reference/README.md` §F1 / §C4；）。

主机侧写缓存语义（volatile write cache）与延迟错误（deferred error）来自 SCSI 的
写缓存模型：写命令在数据进入设备缓冲后即可完成，NAND 提交失败不在原命令上报。
⚠ **本仓出处层没有 SPC/SBC 原文**（`docs/reference/` 未收录），因此这里**不把具体
实现写成行业标准形态**：`padding END` 与"下一条能承载状态的命令直接失败一次"都是
**本仓自拟**。与公开 SCSI 语义的差异：TinyUSB MSC 不支持自定义 sense data，所以本仓
无法给出 CHECK CONDITION + deferred sense（具体 ASC 不作断言），只能用命令级失败近似
。

**早回执（volatile write cache）**：写命令在页编程完成（不是提交）后即可完成——与
DRAM 型/DRAM-less 主控都把「完成点」放在设备侧缓冲写完之后同型；差异是本仓**不做**
RAM 里的写合并缓存，未确认数据直接落在 NAND 页里，靠读覆盖层保证可见性。掉电窗口
因此是「已编程未证明的页 + 未落盘的 Journal 映射」，量化。

**FUA（不实现，用户裁决）**：WRITE(10) 的 CDB byte1 bit3 是 FUA（T10 SBC-3
§5.26：FUA=1 ⇒ 设备不得在数据真正写到介质前返回 GOOD，见 `docs/reference/README.md`
§外部规范）。本仓**忽略**该位，依赖主机 fsync 的后续 `SYNCHRONIZE CACHE` 作为持久性
屏障；取舍与恢复要点见 `known_open_items.md` 。

`SYNCHRONIZE CACHE` 在正常路径会先把序列 padding 收尾，并等 Journal 与 L2P 都落盘；
L2P 区域写回现在按 **WAL 次序**等覆盖其映射的 Journal 条目先落盘（§3.5 不变量 8），
所以正常完成的 flush 是「日志 + 数据」的完整屏障。只有**超时路径**仍是 best-effort
（`FTL_FLUSH_TIMEOUT_MS`，元数据真卡死时主机不能永不完成），那一路不是绝对屏障。

Journal 顺序映射页的形态出处（DFTL translation page）与差异见 §3.7.9 / §3.16.5。

## 附录：候选平台数据（§7 选型用）

> 主文先看功能块；本节是后续选板用，不是目标功能定义。
> 本项目偏好（2026-11-20）：FPGA 是加分项；尽量多写 IP；低成本、入行友好。

### A.1 多核 + FPGA 候选

| 候选 | 多核 CPU | 可编程逻辑 | PCIe EP | NAND/ECC | 文档/生态 | 适合档位 | 主要代价 |
|---|---|---|---|---|---|---|---|
| **Zynq-7000** | 2×Cortex-A9 | PL | Gen2（待核实） | PS NAND + PL 自定义 | **OpenSSD/Cosmos 参考** | R1 入门 | 性能低、工具链较老 |
| **Zynq UltraScale+ MPSoC** | 4×A53 + 2×R5 | PL | Gen3/4（按型号） | PS NAND（待核实）+ PL 自定义 | AMD 文档，学习曲线陡 | R1 / C1 | 贵、Vivado/Vitis 门槛高 |
| **PolarFire SoC** | 4×RISC-V U54 + E51 | FPGA | Gen2（待核实） | PL 自定义 | Microchip 文档，生态较小 | R1 | RISC-V + FPGA 资料少 |
| **Cyclone V SoC** | 2×A9 | FPGA | Gen2 | PL/硬 NAND（待核实） | Intel 文档，板卡便宜 | R1 入门 | 工具链/性能一般 |
| **Arria 10 SoC** | 2×A9 | FPGA | Gen3 | PL 自定义 | Intel 文档 | R1 / E1 | 贵、功耗高 |
| **Versal** | A72 + R5 + AI Engine | PL | Gen4/5（待核实） | PL 自定义 | AMD 新平台 | R1 / E1 | 很贵、工具链新 |
| *i.MX 8M（fallback）* | 4×A53 + M7 | 无 | EP（待核实） | GPMI + APBH-DMA + BCH-62 | NXP 文档好 | C1 | 无 PL |
| *RK3588（fallback）* | 4×A76 + 4×A55 | 无 | EP Gen3 x4（待核实） | NAND 控制器弱 | 社区文档 | C1 高性能 EP | 无 PL |
| *AM64x（fallback）* | 2×A53 + 4×R5F + PRU | 无 | EP（待核实） | GPMC + 系统 DMA | TI 文档好 | E1 工业 | 无 PL |

### A.2 低成本定位（补齐低端）

> 高端已有 OpenSSD/Cosmos；本项目补低成本、入行友好路线。
> 选型优先级：**板卡成本 > 工具链/文档 > PL 资源 > 性能**。
> 价格只是档位；硬条件有两条：**必须是现成板卡 / 核心板（不能自绘 PCB）**，
> **PCIe EP 的 lane / REFCLK / PERST# 已引出**。买前逐项核对 §7.5。

| 路线 | 代表板卡（需核实 PCIe 引出） | 多核 | FPGA | PCIe EP | 成本档 | 适合 |
|---|---|---|---|---|---|---|
| **Zynq-7000 低成本板** | Cora Z7 / PYNQ-Z2 / Zybo Z7 | 2×A9 | PL | 视板卡 | 百元~千元 | **FPGA IP 入门 + OpenSSD 参考** |
| **Cyclone V SoC** | DE10-Nano / SoCKit | 2×A9 | FPGA | 视板卡（SoCKit 有） | 百元~千元 | Intel 工具链 |
| **i.MX 8M Mini/Plus 低成本板** | 各家 SoM/EVK | 4×A53 + M7 | 无 | 有（EP 待核实） | 千元级 | **NVMe + FTL，不写 RTL** |
| **STM32MP25 DK** | ST 官方 DK | 2×A35 + M33 | 无 | Gen2 x1（EP 待核实） | 百元~千元 | STM32 生态入门 |
| **RK3588 板** | Orange Pi 5 / Rock 5B 等 | 8 核 | 无 | Gen3 x4（EP 待核实） | 百元~千元 | 高性能 NVMe |
| **Zynq UltraScale+ KV260** | KV260 | 4×A53 + PL | PL | 有（待核实） | 千元级 | 资源更多但更贵 |
| **PolarFire SoC Icicle** | Icicle Kit | 4×RISC-V + FPGA | FPGA | 有（待核实） | 千元级以上 | RISC-V + FPGA |

> 建议路线与待确认项已收口到 §7.4 / §7.7，避免两处维护。
