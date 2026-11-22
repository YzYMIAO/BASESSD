# 软件架构

主数据路径：Host Protocol → Controller（HIL）→ FTL → Flash Controller（FC-NAND）→ NAND。

## 分层

| 层 | 职责 | 判据 |
|---|---|---|
| HIL | 主机协议（USB MSC / UART CLI）、请求拆分与排队 | 不碰 FTL 内部不变量 |
| FTL | L2P 映射、GC、WL、Journal、Checkpoint、掉电恢复 | 只经 FC 命令接口访问介质 |
| FC-NAND | 命令/描述符/缓冲池、UOP 引擎、ECC/CRC、NAND 时序 | 只经 NAND 器件后端访问 PHY |
| DRAM | DRAM-less：无独立层，元数据常驻片上 SRAM | 容量与布局见平台文档 |
| Boot policy | A/B 镜像头、启动元数据与 slot 选择策略 | 不访问 Flash；CRC 走平台服务 |

核心模块只依赖抽象接口，不直接访问操作系统 API 或芯片寄存器。主机侧入口是
USB MSC 与 UART CLI（无 NVMe 主机层）。

## 平台抽象

平台代码按**概念三层**组织：

1. **抽象接口**：零 HAL 类型的公开 API 与端口接口声明。
2. **共享逻辑**：零 SoC 头，跨 target 只编译一次。
3. **具体 MCU 适配**：时钟、UART、DMA、CRC、QSPI、RNG、MPU、系统启动等，只碰 HAL/MMIO。

SoC 相关代码通过平台操作表接入，不进入 Controller、FTL 或 NAND 算法模块。

**当前例外**：NAND 层尚未完全走抽象接口——时序执行与器件模型仍直接访问 SoC 外设，
并包含厂商 HAL 初始化。其余核心模块已无 SoC 路径依赖。

目录划分、绑定机制、换平台步骤与剩余项见
[`design/platform_design.md`](design/platform_design.md)。
