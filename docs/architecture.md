# 软件架构

主数据路径：Host Protocol -> Controller -> FTL -> Flash Controller -> NAND。

核心模块只能依赖抽象接口，不能直接访问操作系统 API 或芯片寄存器。

平台代码分为两层：

- `platform/bsp`：目标芯片的板级初始化、启动加载器和外设驱动实现。
- `platform/stm32h743`（及 `platform/stm32h7r3`）：具体 MCU 的时钟、UART、DMA、CRC、NAND 接口实现。

SoC 相关代码通过 `platform/platform.h` 约定的接口（`ssd_platform_ops_t`）接入，
不能进入 Controller、FTL、NVMe 或 NAND 算法模块。

> ⚠ **现状偏差**：上述规则尚未完全落地——`platform/platform.h` 的 4 个 op 目前只填了
> `time_us`；NAND 层的 **include 耦合已在 Stage 1 收敛**（`delay` / `crc` / `dma` 三个服务
> 下沉为 `include/platform/` 下的抽象头，`src/nand/*.c` 与 `src/fc/fc_crc_engine.c` 里已无
> `platform/<soc>/` 路径），**Stage 2 又把五个平台服务的纯逻辑搬进共享源**
> （`btim` / `watchdog` / `fault` / `delay` / `syscalls` → `src/platform/*.c`，SoC 相关部分
> 下沉到 `src/platform/stm32h743/*_port.c`）；但 **FMC MMIO 直写与 `HAL_NAND_*` 直连仍在**，
> 留 Stage 3。完整审计与分阶段收敛计划见 `docs/design/platform_abstraction.md`。
