# 固件启动流程

1. 复位向量进入 `Reset_Handler`。
2. 为 IRQ、FIQ、Abort、Undefined 和 SVC 模式分别建立栈。
3. 设置 ARMv7-R `VBAR`，安装异常向量。
4. 调用所选 BSP 的 `soc_early_init()`。
5. 初始化 C 数据段和 BSS，并调用 `soc_memory_init()`。
6. 初始化中断控制器并进入 `main()`。

参考 SoC 假定调试器、ROM 模型或模拟器已经装载 ELF。NAND Boot、DDR
PHY 训练、时钟树和 TCM 配置都依赖真实芯片，不属于通用 ARMv7-R
启动代码。

## 当前 C 启动状态机

`src/main.c` 调用 `firmware_boot_execute()`，依次经过：

1. Platform：UART、Timer、平台接口和中断控制器。
2. Memory：DDR、MPU、Cache 和 ECC。
3. Image：镜像头、CRC/Hash、签名和 A/B 槽选择。
4. Storage：NAND 探测、坏块表和 FTL 元数据恢复。
5. Host：PCIe、NVMe 队列和 Controller Ready。

参考 SoC 目前只有流程和接口，以下能力仍为占位：

- 复位原因寄存器读取。
- DDR PHY Training、DDR ECC 和内存测试。
- MPU 区域配置与 Cache 一致性策略。
- 固件镜像格式、完整性校验和安全启动。
- NAND Controller、DMA、ECC 和坏块扫描。
- FTL Checkpoint 加载、Journal 回放和掉电恢复。
- PCIe PHY、BAR、MSI/MSI-X、Doorbell 和 NVMe Ready。

存在任何占位能力时，状态机进入 `FIRMWARE_BOOT_STAGE_PLACEHOLDER_READY`，
不会把参考流程完成误认为真实 SSD 已经可以接收主机 IO。

## A/B 镜像策略模块

`include/firmware/boot_policy.h` 与 `src/firmware/boot_policy.c` 提供不依赖具体
Flash 控制器的启动策略实现，包含：

- 固件镜像头 CRC、芯片/硬件版本、加载区间和防回滚检查。
- SHA-256 摘要比对以及可注入的签名验证接口。
- 双副本启动元数据的 CRC 与 sequence 选择。
- pending 槽启动前扣减重试次数、失败回退和成功确认。

真实 BSP 的启动顺序应为：读取两份元数据，选择最新合法记录；读取并计算
A/B 镜像摘要，调用 `boot_image_validate()`；调用 `boot_policy_select_slot()`；
若元数据发生变化，先把新记录写入非当前副本并读回验证，再跳转固件。主固件
完成最低可用自检后调用 `boot_policy_confirm_success()` 并原子持久化记录。

策略模块不会自行访问 Flash，也没有内置示例密钥。SHA-256、签名验签和掉电
安全写入必须由目标 SoC 的安全引擎、ROM 服务或经过审计的密码库实现。
