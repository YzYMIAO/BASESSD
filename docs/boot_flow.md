# 固件启动流程

## 复位到 `main()`

1. 复位向量取 `__initial_sp`（MSP）与 `Reset_Handler`。目标是 **Cortex-M7**（STM32H743）：
   没有 IRQ/FIQ/Abort/Undefined/SVC 模式、也没有 `VBAR`（那是 Cortex-A/R 的概念）。
2. `Reset_Handler`（`src/platform/stm32h743/startup_stm32h743xx.s`）初始化 `.data`
   （从 Flash 拷到 RAM）、清零 `.bss`，调用 `__libc_init_array`（含 `_init`）。
3. 跳转 `main()`。中断向量表由 startup 的 `.isr_vector` 段提供；STM32H743 从片内 Flash 启动。

> 早期文档写的 `soc_early_init()` / `soc_memory_init()` **从未实现**；时钟/Cache/MPU 的初始化
> 现在都在 `sys_init()` 内（见下）。平台分层现状见 `docs/design/platform_design.md`。

## `main()` 启动顺序（`src/main.c`）

严格 `if / else-if` 级联——前一步失败会整段跳过后面（详见 `docs/design/fc_nand_design.md §22`）。

1. **Platform / 系统**：`sys_init()`（时钟树、Cache、MPU）→ `cli_init()`（CLI pattern buffer，
   落 `.sram2_data`）→ 绑定日志 sink → 打印开机横幅 + `fault_report_boot()`（复位原因 +
   上次崩溃记录，走 `.noinit`）→ `[SYS ]` 身份行与时钟树。
2. **FC 控制面**：`fc_init()` —— 三池（命令/描述符/缓冲）+ SQ/HSQ/CQ + 调度器 + CRC。
   内部的 `fc_nand_dispatch_init()` 是纯 memset，故 FC 可以先于 NAND 硬件初始化。
3. **NAND 芯片**：`nand_init()` —— HAL/FMC + R/B# 轮询 + per-(ch,ce,lun) 后端 + RESET +
   READ_ID 校验（boot 命令直接走 `nand_engine_execute`，绕过命令队列）。
4. **放行命令队列**：`fc_nand_engine_set_armed(1U)`（此前 `submit`/`submit_chain` 一律拒绝）。
5. **Controller 汇总**：`fc_boot_report()`。
6. **平台 time_us 注册**：`stm32_platform_register()` —— FTL 的 T3 定时触发（CP 10 s / GC 500 ms）依赖它。
7. **FTL**：`ssd_ftl_boot_all()` —— 全实例 `ftl_init` → 介质布局版本校验（不符则
   `ftl_media_format` 自动全片格式化）→ free pool 回填 → `ftl_boot_recovery`
   （Checkpoint 加载 + Journal 回放）→ `ssd_controller_init` 绑定路由 → `g_ftl_ready = 1`。
8. **控制台**：TIM6 中断 + `[SYS ] console ready`。
9. **主循环**：`cli_service_poll()`；`g_ftl_ready == 0` 时跑 `fc_service()`，否则跑
   `ssd_runtime_pump()`（逐个 die 推进 FTL 实例；FC 侧由 `fc_service` / `ftl_service_pump` 驱动）。

## 仍未实现（占位）

- 复位原因寄存器读取（部分：`fault_report_boot` 已读 `.noinit` 崩溃记录）。
- DDR PHY Training、DDR ECC 和内存测试。
- 固件镜像格式、完整性校验和安全启动（`boot_policy` 模块存在，但 `main()` 未调用）。

> 已从「占位」移出（均已落地）：NAND 探测/坏块表（`nand_init` + `bbt_init`）、
> FTL Checkpoint 加载 / Journal 回放 / 掉电恢复（`ftl_boot_recovery`）。
> **NVMe**（Doorbell / MSI-X / Controller Ready）已在 v0.47 整体删除，不再是待办。

## A/B 镜像策略模块

`include/common/boot_policy.h` 与 `src/common/boot_policy.c` 提供不依赖具体
Flash 控制器的启动策略实现，包含：

- 固件镜像头 CRC、芯片/硬件版本、加载区间和防回滚检查。
- SHA-256 摘要比对以及可注入的签名验证接口。
- 双副本启动元数据的 CRC 与 sequence 选择。
- pending 槽启动前扣减重试次数、失败回退和成功确认。

> ⚠ 该模块**当前未被 `main()` 调用**（无 A/B 槽 / 安全启动流程）；FTL 元数据侧另有独立的
> 完整性判据（介质布局版本 + 下层 FC 的 per-CW CRC/ECC，见 `docs/design/ftl_design.md §2.3`）。

真实 BSP 的启动顺序应为：读取两份元数据，选择最新合法记录；读取并计算
A/B 镜像摘要，调用 `boot_image_validate()`；调用 `boot_policy_select_slot()`；
若元数据发生变化，先把新记录写入非当前副本并读回验证，再跳转固件。主固件
完成最低可用自检后调用 `boot_policy_confirm_success()` 并原子持久化记录。

策略模块不会自行访问 Flash，也没有内置示例密钥。SHA-256、签名验签和掉电
安全写入必须由目标 SoC 的安全引擎、ROM 服务或经过审计的密码库实现。
