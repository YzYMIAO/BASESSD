# 软件架构

主数据路径：Host Protocol -> Controller -> FTL -> Flash Controller -> NAND。

核心模块只能依赖抽象接口，不能直接访问操作系统 API 或芯片寄存器。

平台代码分为两层：

- `platform/bsp`：目标芯片的板级初始化、启动加载器和外设驱动实现。
- `platform/stm32h743`（及 `platform/stm32h7r3`）：具体 MCU 的时钟、UART、DMA、CRC、NAND 接口实现。

SoC 相关代码通过 `platform/soc.h` 约定的接口接入，不能进入
Controller、FTL、NVMe 或 NAND 算法模块。
