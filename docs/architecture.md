# 软件架构

主数据路径：Host Protocol -> Controller -> FTL -> Flash Controller -> NAND。

核心模块只能依赖抽象接口，不能直接访问操作系统 API 或芯片寄存器。

平台代码分为三层（概念上；落地仍是两个目录）：

- `include/platform/<svc>.h`：**抽象接口**（零 HAL 类型）——声明公开 API 与 `*_port_*` 接口。
- `src/platform/<svc>.c`：**共享逻辑**（零 SoC 头），跨 target 只编译一次。
- `src/platform/<soc>/*_port.c`（及 `include/platform/<soc>/*_port.h`）：**具体 MCU 适配**
  ——时钟、UART、DMA、CRC、QSPI、RNG、MPU、系统启动等，只碰 HAL/MMIO。

SoC 相关代码通过 `platform/platform.h` 约定的接口（`ssd_platform_ops_s`）接入，
不能进入 Controller、FTL 或 NAND 算法模块。（NVMe 主机层已于 v0.47 删除；主机侧入口是 UART CLI。）

> ⚠ **现状偏差**：上述规则尚未完全落地——`platform/platform.h` 的 4 个 op 目前只填了
> `time_us`；NAND 层的 **include 耦合已在 Stage 1 收敛**（`delay` / `crc` / `dma` 三个服务
> 下沉为 `include/platform/` 下的抽象头，`src/nand/*.c` 与 `src/fc/fc_crc_engine.c` 里已无
> `platform/<soc>/` 路径），**Stage 2 又把五个平台服务的纯逻辑搬进共享源**
> （`timer` / `watchdog` / `fault` / `delay` / `syscalls` → `src/platform/*.c`，SoC 相关部分
> 下沉到 `src/platform/stm32h743/*_port.c`），**Stage 2.5 再把 `usart` 拆开**（传输层 →
> `src/platform/usart.c` + `usart_port.c`，CLI → `src/hil/cli.c`，消除平台层反向依赖
> FTL 的倒挂），**Stage 2.6 把剩余的 `platform_ops` / `crc` / `dma` / `mpu` / `qspi` / `rng` /
> `sys` 七个服务同样补齐**（`src/platform/<svc>.c` + `stm32h743/<svc>_port.c`，HAL 伞头
> `sys.h` 改名 `sys_port.h`）⇒ **判据 A 的 include 耦合归零**；但 **FMC MMIO 直写与
> `HAL_NAND_*` 直连仍在**，留 Stage 3。完整审计与分阶段收敛计划见
> `docs/design/platform_abstraction.md`。
