# USB MSC U 盘设计

> 设计文档：目标、可行性、协议栈、架构、BOT 限制、与主控差距、实施阶段、风险。
> 上板验证矩阵/步骤/脚本/实测见 [`../guides/usb_msc_verify.md`](../guides/usb_msc_verify.md)。
> 历史来源：`docs/archive/usb_msc_udisk.md`（设计）与 `docs/archive/usb_msc_verification.md`（验证手册）。

---

## Part A — 设计

> 状态：**枚举 + 基础数据面已上板通过（rev30）；v12 全盘 H2testw 通过（release + 16 KB EP + `-O2` + cache program/read，2026-11-15：写 950 KB/s / 读 885 KB/s，415/423 MB 无错误；`cw`/`cr` 均已生效）；写中掉电/多分区待测**。
> 目标平台：阿波罗 V2 核心板（STM32H743IIT6 + MT29F4G08 + Type-C USB）。
> 编译：`make all` / `make release` / `make tests UT=FC` 零新告警。
> 上板证据（rev27）：Windows 识别 USB 磁盘 → MBR 分区 → FAT 格式化 →
> 写入 `notepad.exe`（360448B）与 16KB 非均匀 pattern → `Write-VolumeCache` →
> 拔插 USB 后 SHA256 与 2KB 块指纹一致。
> rev30 已确认：Windows `Get-Disk` = **449,839,104 B（429 MiB）**，FAT 格式化，
> `usb_verify write/check` 拔插后 `RESULT=PASS`（含 2KB 块指纹）。
> ⚠ 介质版本 12（2026-10-22）起 L2P 日志区扩为 96 块（每 region 主+备两份），
> 编译期上限 `FTL_LBA_MAX` 变为 **868,352（424 MiB）**；运行时按实际坏块取
> min(plane)/min(instance)，以 boot `[CAP]` / USB READ CAPACITY 为准。
> 上文 rev30 的 429 MiB 是历史证据，不重写。
> 大文件/GC 压力与顺序吞吐已由 2026-11-15 全盘 H2testw 覆盖；写过程中掉电恢复仍未验证（R1）。
> H2testw 吞吐与内部/器件上限的分层对照见 [`../guides/testing_guide.md`](../guides/testing_guide.md) §7.5 与 `known_open_items.md` RISK-PERF-04。

---

> 分层测试流程、工具、性能方法与实测结果：见 [`../guides/testing_guide.md`](../guides/testing_guide.md)。

### A.1 目标与硬件

#### A.1.1 现有核心板资源

| 资源 | 规格 |
|---|---|
| CPU | STM32H743IIT6，2 MB flash，1060 KB SRAM |
| NAND | MT29F4G08，512 MB，ONFI 1.0，page 2048+64，block 64 pages |
| SDRAM | 32 MB（W9825G6KH / EM63A165TS-6G 等） |
| USB | Type-C，USB SLAVE/HOST（OTG） |
| 串口 | USART1，PA9/PA10 |
| 调试 | SWD |

#### A.1.2 目标

让 PC 通过 Type-C 把本板识别成 **424 MiB 的 USB 大容量存储设备（U 盘）**，可分区、
格式化、读写文件；底层仍由本仓 FTL 管理 NAND。

#### A.1.3 v1 范围

- **USB FS 12 Mbps**（不是 HS 480 Mbps）
- **固定 424 MiB**、**512 B block**、**单 LUN**
- SCSI 最小命令集：`INQUIRY` / `TEST UNIT READY` / `REQUEST SENSE` /
  `READ CAPACITY(10)` / `READ(10)` / `WRITE(10)` / `SYNCHRONIZE CACHE(10/16)` /
  `UNMAP(0x42)` / `MODE SENSE(6)`
- **不做** removable eject、多 LUN、CD-ROM

---

### A.2 可行性结论

**能搭，但不是一个小 patch。** 属于中等规模的 HIL/传输层集成。

#### A.2.1 硬件侧

- STM32H743II 有 OTG_FS 与 OTG_HS 两个 USB 核（`third_party/stm32h743/cmsis-device/stm32h743xx.h`）。
- Type-C 的 D-/D+ 经 22R 接 **PA11/PA12（OTG_FS，AF10）**，已由原理图 2.2.10.1 确认；
  SBU1/SBU2 未接。
- **ULPI 高速不做**：板上没有 ULPI PHY，只有 FS 12 Mbps；HS 需要外部
  ULPI PHY（USB3300/3320 等），当前硬件没有路径。
- ⚠ **PA9 是 `OTG_FS_VBUS`，但本仓已用作 USART1_TX** ⇒ 必须
  `vbus_sensing_enable = DISABLE`，不能用 VBUS sensing。
- USB 需要 48 MHz 内核时钟；HSI48 已在 `src/platform/stm32h743/sys_port.c` 选为
  USB 内核时钟（`RCC_USBCLKSOURCE_HSI48`）。

#### A.2.2 仓库侧

| 项 | 现状 |
|---|---|
| USB HAL 源文件 | `stm32h7xx_hal_pcd.c` / `stm32h7xx_hal_pcd_ex.c` / `stm32h7xx_ll_usb.c` 存在，但**未编入** `third_party/stm32h743_vendor.mk` |
| USB Device middleware | **没有**（无 `usbd_*`、无 MSC 类） |
| USB 时钟/引脚/IRQ/MSP | **没有** |
| FTL 块设备接口 | **有**，且与 MSC 512B sector 天然对齐 |

#### A.2.3 FTL 后端匹配度

- `FTL_LBA_MAX = 868,352`（424 MiB / 512 B；已扣除固定元数据/L2P 日志区/OP/Spare）
- `FTL_IU_BYTES = 512`，1 LBA = 1 CW = 512 B
- `ftl_submit_read` / `ftl_submit_write` 是**异步**的：提交后由
  `complete_flag` / `on_complete` 通知完成
- 控制器层 `ssd_controller_submit` 已经提供 host 语义
  （`opcode` / `start_lba` / `block_count` / `buffer` / `complete_flag` / `status`）

⇒ **不需要改 FTL 的映射/GC/journal 逻辑**；只需要一个 USB → controller/FTL 的适配前端。

---

### A.3 USB 协议栈选择

**不重写 USB 协议栈。** 只写 MSC class 回调 + FTL adapter。

| 项目 | 许可 | 适配度 | 说明 |
|---|---|---|---|
| **TinyUSB** | MIT | **选定** | 有 STM32H7 DWC2 port、MSC device class、bare-metal `OPT_OS_NONE`；`tud_task()` 在主循环跑，MSC 回调**不在 ISR**，最适合本仓 |
| STM32Cube USBD | ST 免费许可 | 备选 | 官方 Core + MSC 类；但 MSC `Read/Write` 回调默认在 OTG 中断里执行，FTL 不能进 ISR ⇒ 必须改成主循环 deferred/pending |
| CherryUSB | Apache-2.0 | 备选 | 轻量、DWC2 port、MSC device/host，国内资料多；设备任务在主循环 |
| LUFA / libopencm3 | — | 不适用 | AVR / 非 H7 DWC2，移植成本高 |
| ChibiOS/NuttX/Zephyr USB | — | 不适用 | 需要 RTOS/大集成，和本仓 superloop 不匹配 |

#### A.3.1 为什么选 TinyUSB

- `tud_task()` 在**主循环**执行，MSC 回调不是 ISR 上下文 ⇒ 可以在回调里等待 FTL 完成；
- DWC2 是 STM32H7 的 USB 控制器，TinyUSB 已有 port；
- MIT 许可，代码量比 Cube 小，便于学习；
- 协议栈本身（枚举、控制传输、BOT、SCSI 子集）不用自己写。

#### A.3.2 FTL 适配方式：TinyUSB 异步回调

TinyUSB 的 MSC 回调支持 `TUD_MSC_RET_ASYNC` + `tud_msc_async_io_done()`。
本仓用它实现**非阻塞 adapter**，不在回调里自旋：

```text
tud_msc_read10_cb / write10_cb
  → 提交 ssd_controller_submit(...)，返回 TUD_MSC_RET_ASYNC
主循环下一轮：
  ftl_service_pump() + ftl_service()   （FTL 正常推进）
  usb_msc_task()                       （完成则回填 READ 数据 → async_io_done）
  usb_device_task()                    （tud_task_ext(0) 发出数据/CSW）
```

为什么不返回 `TUD_MSC_RET_BUSY`：TinyUSB 会在**同一个 `tud_task()` 调用**里
立即重入回调（伪造一次传输完成事件），单线程主循环下 `ftl_service` 永远轮不到，
表现为 `tud_task` 内死循环 + 看门狗复位。

为什么需要 bounce buffer：USB 端点缓冲在主机复位/拔出后会被协议栈立刻复用
（重新收 CBW）。FTL 只持有本模块的静态 bounce buffer，完成时再拷回端点缓冲。

---

### A.4 推荐架构

```mermaid
flowchart TD
    HOST["PC host"] -->|"USB Type-C / OTG_FS 12 Mbps"| PCD["TinyUSB DWC2 port + USBD core"]
    PCD -->|"tud_task() 在主循环；ISR 只做传输"| MSC["MSC class callbacks / usb_msc_task"]
    MSC -->|"SCSI: INQUIRY / READ CAPACITY / READ(10) / WRITE(10) / SYNC CACHE"| CTRL["ssd_controller_submit / ssd_req_s"]
    CTRL -->|"512B sector ↔ FTL LBA 1:1；稳定 bounce buffer"| FTL["现有 FTL / FC / NAND"]
```

#### A.4.1 分层职责

| 层 | 职责 |
|---|---|
| USB PCD/DWC2 | 端点、FIFO、中断；只做传输 |
| TinyUSB USBD core | 枚举、控制传输、BOT 状态机 |
| MSC class / `usb_msc.c` | SCSI 命令解析、数据阶段、CSW；调用块后端 |
| controller / FTL | 512B sector 读写、组完成、journal/CP |

#### A.4.2 关键约束

- **USB IRQ 只做传输**；SCSI/FTL 在主循环。
- 不在 ISR 调 `ftl_*`；不在 FTL completion callback 调 `ftl_flush` / `ftl_trim`
  （它们是同步自旋路径）。
- `ftl_service_pump()` 每轮只调一次，且在 `usb_msc_task()` / `usb_device_task()` 之前。
- **CLI 与 USB 共用 host busy 闸门**：同一时刻只允许一个 host 前端提交
  `ssd_controller_submit`（`cli_host_busy()` / `usb_msc_host_busy()`）。
- `SYNCHRONIZE CACHE` 只在前序写完成后调用 `ftl_flush`（主循环上下文）。
- 主循环顺序：`cli_service_poll` → `ftl_service_pump` + 逐实例 `ftl_service`
  → `usb_msc_task` → `usb_device_task` → `timer_scheduler_poll`。

#### A.4.3 缓冲与并发

- **单条 SCSI 命令在飞**（BOT 串行）；但同一条 WRITE(10) 的**两个 chunk** 可以同时在飞
  —— 写路径需要 1 chunk lookahead：缓存程序序列里 chunk k 的最后一页由 chunk k+1 的
  第一条命令的 `FAIL_CACHE` 确认（`docs/design/ftl_design.md` §3.16）。
- 三个槽（`usb_msc_slot_s`）：0/1 是数据槽，各带 16 KiB bounce（AXI `.bss` / DTCM，
  CPU-only）+ 一个 `ssd_req_s`；2 号是无 bounce 的控制槽（FLUSH/UNMAP）。
  bounce 必须活到该槽的 FTL 完成（FTL 可能因背压延后消费）。
- WRITE = **volatile write cache**：端点缓冲 → 拷进槽 bounce → 提交 → **立即回流控回执**
  （`tud_msc_async_io_done(length)`，**含一条命令的最后一段**）。主机因此不必等
  最后一页的 `FAIL_CACHE` 就能发下一条命令 ⇒ FTL 的设备 cache 序列可以跨 host 命令延续
  （`ftl_design.md` §3.16.5）。槽位保留到 FTL 完成（只回收缓冲，不再发 CSW）。
  FTL 侧对应地把**组完成点**提前到「页编程完成」（早回执，RISK-CACHE-27）：同一 LBA 在
  FTL 组完成后立刻读，读覆盖层直读刚编程的页。
  已回执的写段之后失败 ⇒ 置 **deferred error**，由**下一条** READ/WRITE/FLUSH 命令的 CSW
  失败一次。`usb_msc_take_deferred_error()` **先问各 FTL 实例**（`ftl_take_deferred_error`），
  再落回控制器自己的标志——早回执后 FAILC 的结论只存在于 FTL 的粘性标志里。
  （承载错误的那条命令会设置 MEDIUM ERROR / WRITE ERROR sense；TinyUSB 侧补丁保证
  应用设置的 sense 不被默认 NOT READY 覆盖，见 `known_open_items.md` RISK-USBC-01。）
  两槽都占用时写回调返回 `TUD_MSC_RET_BUSY`（数据留在端点缓冲重投）。
- 打开中的写序列会占用一个槽直到 FTL 收尾（空闲 1 s / 下一条命令 / flush 触发 padding
  END）⇒ `usb_msc_host_busy()`（CLI 用）在此期间为 true，CLI 命令需重试。
- READ：USB 端点缓冲 → 提交到槽 bounce → 完成 → 拷回端点缓冲 → USB IN（不需要 lookahead）。
- 主机复位/拔出时在飞槽标记 `discard`：完成后只收尾，不回写 USB 缓冲。
- 不支持多 LUN；支持 SCSI UNMAP（逐 range 走 controller 的异步 TRIM，全部 range
  完成后才回 CSW，保证与后续写的顺序）。

---

#### A.4.3 一次 SCSI 读写的路径

```mermaid
sequenceDiagram
    participant H as PC host
    participant U as TinyUSB
    participant M as MSC class
    participant C as controller / FTL
    H->>U: CBW（SCSI READ/WRITE）
    U->>M: tud_msc_read10_cb / write10_cb
    M->>C: ssd_controller_submit(...)
    C-->>M: 完成（主循环推进）
    M->>U: 数据阶段 + CSW
    U-->>H: CSW
```

### A.5 BOT 为什么不能多命令在飞

- USB MSC **Bulk-Only Transport（BOT）** 的状态机是：

```text
CBW (Command Block Wrapper) → data → CSW (Command Status Wrapper)
```

  同一个 interface 同时只有一条命令；下一条 CBW 必须等上一条 CSW。
- 这不是“没有命令集”：MSC 有完整的 SCSI 命令集，限制在**传输层**。
- 支持命令队列的是 **UASP（USB Attached SCSI Protocol）**，依赖 USB 3.0 / bulk streams；
  STM32H743 只有 USB 2.0（FS/HS），**本板不可用**。
- 因此：这块板上的 U 盘前端**无法让多条 host 命令在飞**。
- FTL 内部仍可把**一条**大的 READ/WRITE 拆成多个子操作并行（现有 group/pending 模型），
  但 host 侧是串行的。

> 协议出处：USB Mass Storage Class Bulk-Only Transport 1.0；USB Attached SCSI Protocol
> (UASP) 1.0。本仓 `docs/reference/` 未存档这两份规范原文，本文按**协议事实**引用，
> 不冒充本仓出处（R12）。

---

### A.6 与真实存储主控的差距

**U 盘 = USB-to-NAND bridge，不是 SSD 主控。**

| 维度 | 当前（TinyUSB U 盘） | 主流 SSD 主控 |
|---|---|---|
| Host 接口 | USB MSC BOT（CBW/CSW），单命令 | NVMe over PCIe：多 SQ/CQ、doorbell、MSI-X、PRP/SGL、Admin/IO 命令集 |
| 数据面 | STM32 FMC 单通道、PIO、512B | 多 channel / 多 die / 多 plane，ONFI/Toggle，DMA，硬件 ECC/LDPC 流水线 |
| FTL 映射 | L2P + journal/checkpoint，无 P2L | L2P + P2L（band/chunk），SLC cache，复杂 GC/WL/read-retention |
| DRAM | DRAM-less：SRAM + NAND 快照 | DRAM 表 + nvcache；或 DRAM-less 分支 |
| 掉电 | journal/CP，无超级电容 PLP | PLP/supercap、完整元数据一致性、原子写 |
| ECC | BCH 4-bit | LDPC、RAID/parity、端到端保护 |
| 性能 | USB FS ≈ 1 MB/s | PCIe Gen4/5，GB/s，10 万~百万 IOPS |
| 固件架构 | 裸机 superloop + 轮询 | 多核/协处理器、硬件调度器、RTOS/实时队列 |
| 特性 | 裸块设备；支持 UNMAP；无 SMART/sanitize | namespace、TRIM/UNMAP、SMART、sanitize、secure erase |

#### A.6.1 已经对齐的部分

- HIL / FTL / FCC / DRAM 分层（`docs/design/fc_nand_design.md §26`）
- 命令/完成模型（FTL op tag + completion table）
- L2P / GC / WL / journal / checkpoint / 掉电恢复
- DRAM-less 方向（SRAM + NAND 快照）
- 512B IU ↔ USB MSC 512B sector 的 1:1 映射

#### A.6.2 差距最大的部分

- **NVMe 队列/门铃/多命令在飞**：USB BOT 串行，UASP 需要 USB 3.0。
- **多通道并行**：本板单 FMC 通道、单 die。
- **硬件 ECC/LDPC、RAID/parity**：本仓是软件 BCH，无 RAID。
- **PLP**：无超级电容，只有 journal/CP。
- **P2L / DRAM 表**：本仓是 DRAM-less + checkpoint 快照；P2L 是另一条主流分支。
- **性能**：FS 12 Mbps 与 PCIe Gen4/5 差几个数量级。

#### A.6.3 学习定位

- U 盘适合当**测试夹具**：把 FTL 暴露给 PC，方便跑文件系统、格式化、掉电、性能测试。
- 如果目标是学习**存储主控**，下一步应做：
  - **NVMe 风格 SQ/CQ + doorbell**（本板无 PCIe，只能软件模型）；
  - 或 **P2L / 多通道模型**；
  - 而不是把 USB MSC 当主控核心。

---

### A.7 实施阶段

实施阶段与上板记录见 [`../archive/usb_msc_implementation_phases.md`](../archive/usb_msc_implementation_phases.md)；当前设计只保留架构与约束。

### A.8 关键文件（实现时）

| 文件 | 作用 |
|---|---|
| `third_party/tinyusb/`（新增，已裁剪） | MIT USB 协议栈；只保留 device + MSC + DWC2 编译闭包（见其 `README.soft_ssd.md`） |
| `third_party/tinyusb_vendor.mk`（新增） | 编入源文件与包含路径 |
| `include/tusb_config.h`（新增） | TinyUSB 编译期配置（H7 / OPT_OS_NONE / MSC / 16 KB EP） |
| `include/platform/usb.h`（新增） | `usb_device_init` / `usb_device_task` 抽象 |
| `src/platform/stm32h743/usb_port.c`（新增） | USB2_OTG_FS 时钟/引脚/IRQ、`tusb_time_millis_api` |
| `src/platform/stm32h743/sys_port.c` | USB kernel clock = HSI48 |
| `src/hil/usb_msc.c`（新增） | 描述符、MSC/SCSI 回调、异步请求状态机 |
| `include/hil/usb_msc.h`（新增） | `usb_msc_init` / `usb_msc_task` / `usb_msc_host_busy` |
| `src/main.c` | `usb_msc_init` + `usb_device_init`；主循环 `usb_msc_task` → `usb_device_task` |
| `src/hil/cli.c` | CLI 与 USB 共用 host busy（`cli_host_busy`） |
| `docs/hardware_contract.md` | 补 USB 引脚/时钟/VBUS |

---

### A.9 风险与未确认

| 风险 | 说明 | 缓解 |
|---|---|---|
| USB 只支持 FS 12 Mbps | 原理图 2.2.10.1：Type-C D-/D+ 经 22R 接 PA11/PA12，无 ULPI PHY | 不改 `OPT_MODE_HIGH_SPEED`；要 HS 只能换板 |
| TinyUSB MSC 异步回调 | 已改用 `TUD_MSC_RET_ASYNC` + `tud_msc_async_io_done`；主机复位时需丢弃在飞结果 | `discard` 标志 + `tud_ready()` 守卫；未上板 |
| `ftl_flush` 非 barrier | 只 best-effort，不能承诺绝对掉电持久 | SYNCHRONIZE CACHE 只在前序写完成后调用；文档写清 |
| WRITE(10) FUA（2026-11-05 用户改判实现） | FUA 写 chunk 不回早回执，等 FTL 组完成（program + 映射确认）后才回 CSW；失败直接 CSW failed | 非 FUA 写仍是 volatile write cache 早回执；上板验证 FUA 回执延迟与断电读回（RISK-USB-05） |
| `.noncacheable_buffer` 放置 | 链接器匹配模式漏前导点，`g_nand_channel_page_buffer` 落在可缓存 AXI RAM | **不在 USB 补丁内修**；已登记 `RISK-PLAT-01`（待决策，改地址必须上板） |
| CLI/USB 双前端 | 无全局锁 | 共用 host busy 闸门 |
| FS 12 Mbps | ≈1 MB/s；HS 需 ULPI，本板不可行 | v1 接受；性能另测 |
| UNMAP 验证 | 逐 range 的异步 TRIM；多 range/顺序/主机复位未上板 | 已实现（RISK-USB-14）；**未向主机广告**（无 VPD B0/B2 / LBPME），普通 `fstrim` 可能不下发，见 RISK-USB-15 |
| USB 中间件体积 | TinyUSB 代码/缓冲计入 flash/RAM | 实现前算预算 |

---

### A.10 非目标（v1）

- USB HS 480 Mbps / ULPI
- 多 LUN / CD-ROM / removable eject
- 文件系统解析（U 盘由 PC 格式化，固件只暴露块设备）
- 量产级 USB 认证 / 复合设备

---

> 上板验证步骤、判据与脚本见[`../guides/usb_msc_verify.md`](../guides/usb_msc_verify.md)（原 `docs/archive/usb_msc_verification.md`）。

### A.11 出处

- 分层：`docs/design/fc_nand_design.md §26`
- FTL / DRAM-less / 算法 / 取舍：`docs/design/storage_controller_design.md §九~§十一`
- SPDK FTL / P2L / band metadata：<https://spdk.io/doc/ftl.html>（索引见 `docs/reference/README.md` §A4）
- 论文清单：`docs/reference/README.md`
- USB BOT / UASP：USB Mass Storage Class Bulk-Only Transport 1.0；
  USB Attached SCSI Protocol (UASP) 1.0（本仓未存档原文，按协议事实引用）
- TinyUSB：https://github.com/hathach/tinyusb（MIT）
- CherryUSB：https://github.com/cherry-embedded/CherryUSB（Apache-2.0）
- STM32CubeH7：https://github.com/STMicroelectronics/STM32CubeH7

---


## Part B — 上板验证

上板验证矩阵、步骤、脚本与实测记录见 [`../guides/usb_msc_verify.md`](../guides/usb_msc_verify.md)。
