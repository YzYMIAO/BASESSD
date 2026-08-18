# 软件架构

主数据路径：Host Protocol -> Controller -> FTL -> Flash Controller -> NAND。

核心模块只能依赖抽象接口，不能直接访问操作系统 API 或芯片寄存器。

平台代码分为四层：

- `platform/armv7r`：Cortex-R5/ARMv7-R 通用异常和运行时。
- `platform/bsp`：目标芯片的板级初始化、启动加载器和外设驱动实现。
- `platform/reference_soc`：仓库维护的虚拟 SoC 契约，不代表真实芯片。
- `platform/host_sim`：PC 端功能和故障注入验证环境。

`src/firmware` 保存产品级启动编排与 A/B 固件策略；`src/platform/bsp`
只保存依赖芯片寄存器的硬件适配。未来获得目标 SoC 资料后，由 BSP 实现
`platform/soc.h` 约定的接口并替换 `reference_soc`。

SoC 相关代码通过 `platform/soc.h` 和设备抽象接口接入，不能进入
Controller、FTL、NVMe 或 NAND 算法模块。
