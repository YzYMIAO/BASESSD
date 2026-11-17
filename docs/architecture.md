# 软件架构

主数据路径：Host Protocol → Controller（HIL）→ FTL → Flash Controller（FC-NAND）→ NAND。

## 分层

| 层 | 目录 | 职责 | 判据 |
|---|---|---|---|
| HIL | `src/hil/` | 主机协议（USB MSC / UART CLI）、请求拆分与排队 | 不碰 FTL 内部不变量 |
| FTL | `src/ftl/` | L2P 映射、GC、WL、Journal、Checkpoint、掉电恢复 | 只经 FC 命令接口访问介质 |
| FC-NAND | `src/fc/` + `src/nand/` | 命令/描述符/缓冲池、UOP 引擎、ECC/CRC、FMC 时序 | 只经 NAND 器件后端访问 PHY |
| DRAM | 无独立层 | 本仓 DRAM-less：元数据常驻 AXI_RAM / SRAM2 | 见 `docs/hardware_contract.md` 内存布局 |

核心模块只依赖抽象接口，不直接访问操作系统 API 或芯片寄存器。主机侧入口是
USB MSC 与 UART CLI（无 NVMe 主机层）。

## 平台抽象

平台代码分三层（概念上；落地仍是两个目录）：

- `include/platform/<svc>.h`：**抽象接口**（零 HAL 类型）——公开 API 与 `*_port_*` 接口声明。
- `src/platform/<svc>.c`：**共享逻辑**（零 SoC 头），跨 target 只编译一次。
- `src/platform/stm32h743/*_port.c`（及 `include/platform/stm32h743/*_port.h`）：**具体 MCU
  适配**——时钟、UART、DMA、CRC、QSPI、RNG、MPU、系统启动等，只碰 HAL/MMIO。

SoC 相关代码通过 `platform/platform.h` 的 `ssd_platform_ops_s` 接入，不进入
Controller、FTL 或 NAND 算法模块。

**当前例外**：NAND 层尚未完全走抽象接口——`src/nand/nand_fmc.c` 直接写 FMC 窗口，
`src/nand/devices/mt29f___g08a.c` 直接定义 `HAL_NAND_MspInit` / `HAL_NAND_Init`。
其余核心模块已无 `platform/stm32h743/` 路径依赖。完整审计与剩余项见
`docs/design/platform_design.md`。
