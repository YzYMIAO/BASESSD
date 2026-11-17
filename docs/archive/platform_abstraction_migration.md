# 平台抽象迁移与审计记录（历史）

> 本文记录 2026-10 至 2026-11 的平台抽象迁移/审计过程，**不是当前设计**。
> 当前设计见 [`../design/platform_design.md`](../design/platform_design.md)；本文保留原貌，后续结论写进新文档。

## 〇、起因

在考虑把 soft_ssd 移植到别的 MCU 时提出：

> 「`src/platform/stm32h743/` 这些文件是底层硬件的。如果我有移植的想法，我应该把功能函数
> 抽象出来挂到 `src/platform/` 对应的文件中，而底层平台中应该只要初始化和配置、查询和设置
> 等等函数，各种功能应该抽象出来，对吧？」

**结论：方向对。但本仓的真实状态是「规则早已写在文档里，代码却漂移了」。**

- 审计当时 `docs/architecture.md` 已写明规则：「核心模块只能依赖抽象接口，不能直接访问
  操作系统 API 或芯片寄存器」；同文件还点名了一个抽象契约头 `platform/soc.h` ——
  **该文件在全仓不存在**（`find` 核实）。⚠ `architecture.md` 已于 2026-11-17 重写，
  当前架构说明以该文件现状为准。
- `docs/hardware_contract.md` 断言「Core SSD modules never access MMIO registers directly」
  —— **今天不成立**：`src/nand/nand_fmc.c` 直接对 FMC 窗口做 MMIO 写，
  `src/nand/devices/mt29f___g08a.c` 直接定义 `HAL_NAND_MspInit` / `HAL_NAND_Init` /
  `__HAL_RCC_*_CLK_ENABLE`。
- 真实存在的抽象 seam 是 `include/platform/platform.h` 的 `ssd_platform_ops_s`（4 个函数指针），
  但**只有 `time_us` 被填**。

本文件把规则补成**可机械检查**的形式（§一），把现状逐文件审计出来（§二、§三），
给出目标布局（§四）与分阶段路线图（§五）。

---


## 二、现状审计（逐文件）

分类记号：**(a)** 纯抽象 · **(b)** 纯硬件 · **(c)** 混合（接口/逻辑可移植，实现含硬件） ·
**(p)** port 适配层（只碰 HAL/MMIO，无可移植逻辑）· **(t)** 构建/工具。

### 2.1 `src/platform/` 顶层（`bsp/` 已删除）

| 文件 | 行数 | 分类 | 说明 | 违反 |
|---|---:|---|---|---|
| `platform.c` | 45 | a | `ssd_platform_ops_s` 注册表 + `stm32_platform_register()`（Stage 2.6 从 `<soc>/platform_ops.c` 搬来，只填 `time_us`），无硬件 | — |
| `crc.c` | 46 | **a** | CRC 自检策略（IDR 回环 `ASSERT`）+ 对齐契约（Stage 2.6 搬来） | — |
| `dma.c` | 19 | **a** | UART TX DMA 一行转发（Stage 2.6 搬来） | — |
| `mpu.c` | 13 | **a** | `mpu_set_protection` 一行转发（Stage 2.6 搬来） | — |
| `qspi.c` | 43 | **a** | 收发空参检查（`buf==NULL\|\|datalen==0`）后转发（Stage 2.6 搬来） | — |
| `rng.c` | 43 | **a** | DRDY 重试循环 + `rng_get_random_range` 取模映射（Stage 2.6 搬来） | — |
| `sys.c` | 83 | **a** | 上电初始化编排（顺序即契约）+ `sys_*` 一行转发（Stage 2.6 搬来） | — |
| `timer.c` | 239 | **a** | 64 槽任务表调度器；1 ms 心跳由 timer_port 提供，ISR 只 `tick++`，到期扫描在主循环 | — |
| `watchdog.c` | 230 | **a** | 健康域状态机 + 首故障锁存 + WWDG 窗口决策（Stage 2 搬来） | — |
| `fault.c` | 439 | **a** | Cortex-M 故障解码 / 栈扫描 / 崩溃记录打印（Stage 2 搬来） | 见下注 |
| `delay.c` | 256 | **a** | 毫秒/SysTick 忙等与 µs 时间戳逻辑（Stage 2 搬来） | — |
| `syscalls.c` | 96 | **a** | newlib `_sbrk` + 堆栈守卫（Stage 2 搬来；`mrs %0, msp` 内联汇编取 MSP） | — |
| `makefile.mk` | 14 | t | 按 `FIRMWARE_PLATFORM` 选 `<soc>` 目录 | — |

> **`bsp/` 已删除（2026-10-12）**：原 `src/platform/bsp/` 10 个文件（`nand_loader.c` 124 行、
> `system_init.c` 8 行空桩、`interrupt.c` 1 行 `#warning`、`drivers/{cache,dma,pcie,timer,uart}.c`
> 各 1 行 `#warning`、两份 `makefile.mk`）全是**死代码**（G2）——其 `makefile.mk` 从未被任何
> 构建片段 include，且 `MODULE_INCS` 指向的 `include/platform/bsp` 根本不存在。整目录删除。
> 注意与 **`<soc>/bsp/`**（`src/platform/stm32h743/bsp/sdram_w9825g6kh.c` 等，**活的**）区分。

> **注（有意保留的例外）**：共享 `fault.c` 的 `fault_dump_stack()` 仍带三个 stm32h743 内存映射
> 常量（`FAULT_FLASH_BEGIN/END`、`FAULT_DTCM_BEGIN`）与链接符号 `_estack`。换 target 时需下沉为
> port 接口（如 `fault_port_stack_top()` / `fault_port_is_code_address()`）。当前登记为**已知残留耦合**，
> 文件内已就地注明。

### 2.2 `src/platform/stm32h743/`（13 个 `.c` / 1905 行）

Stage 2 / Stage 2.6 后本目录**只剩 port**（薄 HAL 封装或向量表）。`timer.c` / `watchdog.c` /
`fault.c` / `delay.c` / `syscalls.c` **已迁出**（§2.1）；`usart.c` 拆成 port（`usart_port.c`）
+ 共享传输（`src/platform/usart.c`）+ CLI（`src/hil/cli.c`，§5.3）；`platform_ops.c` /
`crc.c` / `dma.c` / `mpu.c` / `rng.c` / `qspi.c` / `sys.c` **Stage 2.6 全部拆成共享源 + `*_port.c`**（§5.4）。

| 文件 | 行数 | 分类 | 说明 | 违反 |
|---|---:|---|---|---|
| `timer_port.c` | 61 | **p** | TIM6 句柄 + 1 ms 更新中断；ISR 只调 `timer_scheduler_tick()`；向量入口 `TIM6_DAC_IRQHandler` | — |
| `watchdog_port.c` | 86 | **p** | IWDG/WWDG 句柄 + 6 个 port 函数 + 两个 HAL 回调转共享层；域超时时间源读 `timer_scheduler_now_ms()`；向量入口 `WWDG_IRQHandler` | — |
| `fault_port.c` | 220 | **p** | USART1/DMA1 轮询控制台 + 5 个 naked 异常入口 + 3 个内核异常空入口（SVC/DebugMon/PendSV）+ `fault_port_entry_c`（抓现场）+ `RCC->RSR` 复位原因解码 | — |
| `delay_port.c` | 42 | **p** | 5 个计数器/时钟查询函数（SysTick LOAD/VAL、HCLK/SYSCLK、HAL tick）；无 DWT | — |
| `usart_port.c` | 271 | **p** | USART1 句柄 + HAL 初始化 + GPIO/DMA + `usart_port_irq_handler()` + 向量入口 `USART1_IRQHandler` + 15 个 port 原语（寄存器/DMA/缓存/CMSIS 内建）；`usart_port_now_ms()` 读 SysTick/HAL tick | — |
| `crc_port.c` | 63 | **p** | CRC 句柄 + `HAL_CRC_Init` + `HAL_CRC_MspInit` + `__HAL_CRC_*` IDR + `HAL_CRC_Calculate` | — |
| `dma_port.c` | 54 | **p** | DMA1_Stream0 → USART1 TX 的 HAL 配置 + `HAL_DMA_IRQHandler` + 向量入口 `DMA1_Stream0_IRQHandler` | — |
| `mpu_port.c` | 71 | **p** | 中性参数填进 `MPU_Region_InitTypeDef` + `HAL_MPU_ConfigRegion` | — |
| `rng_port.c` | 52 | **p** | RNG 句柄 + `HAL_RNG_Init` + `HAL_RNG_MspInit` + `__HAL_RNG_GET_FLAG` + `HAL_RNG_GenerateRandomNumber` | — |
| `qspi_port.c` | 269 | **p** | QUADSPI（W25Q256）句柄 + 引脚/时钟 + 命令模式位域→HAL 枚举解码 + HAL 收发 | — |
| `sys_port.c` | 209 | **p** | MPU 区域表（SoC 内存映射）/ Cache / VTOR / WFI / PRIMASK / MSP 的 CMSIS 内建 + RCC-PLL 400 MHz + 向量入口 `SysTick_Handler` | — |
| `nand_dma.c` | 364 | b | DMA2 + FMC NAND 传输 + D-Cache 维护 + 向量入口 `DMA2_Stream0/1_IRQHandler`（`#if CONFIG_NAND_DMA` 内） | G3（Stage 3） |
| `bsp/sdram_w9825g6kh.c` | 207 | b | FMC SDRAM 初始化 | — |

> 分类记号 **p** = port 适配层（只碰 HAL/MMIO，不含可移植逻辑）。
> **Stage 2.7 起本目录不再有总装 `stm32h7rsxx_it.c`**：每个 ISR 直接定义在拥有该外设的
> port 文件里（见 §5.5 的映射表）。

### 2.3 当前未启用的平台服务

以下服务已完成抽象分层，但产品路径**零调用者**，保留接口供将来接入；对应头文件已就地标注。

| 服务 | 状态 | 说明 |
|---|---|---|
| QSPI | 未启用 | 本板不带 QSPI NOR flash，`qspi.c` / `qspi_port.c` 随构建编译但不被调用；`include/platform/qspi.h` 注明 |
| RNG | 未启用 | 产品路径无消费者（UT 随机序列用软件 PRNG）；`include/platform/rng.h` 注明 |
| SDRAM（W9825G6KH） | 未启用 | FTL 为 DRAM-less，元数据常驻 AXI_RAM / SRAM2；`bsp/sdram_w9825g6kh.c` **未纳入构建**（`<soc>/makefile.mk` 只通配 `<soc>/*.c`）；`include/platform/stm32h743/bsp/sdram_w9825g6kh.h` 注明 |

---


## 三、已核实缺口

| # | 缺口 | 证据 |
|---|---|---|
| **G1** | 文档点名的抽象头 `platform/soc.h` **不存在**；真实契约头是 `platform/platform.h`，且 4 个 op 只填 1 个 | `docs/architecture.md`；`include/platform/platform.h`；~~`src/platform/stm32h743/platform_ops.c`~~（Stage 2.6 已并入共享 `src/platform/platform.c`） |
| **G2** | ~~`src/platform/bsp/`（8 个 `.c`，含 124 行 `nand_loader.c`）**是死代码**，`bsp/makefile.mk` 从未被 include~~ **✅ 已删除（2026-10-12）** | `config.mk:225` `MODULES = common controller fc ftl nand platform`；`src/platform/makefile.mk` 只 include `<soc>/makefile.mk`；全仓无任何文件 include `bsp/makefile.mk` |
| **G3** | 核心模块**直接 include `<soc>` 路径** | `src/nand/nand_fmc.c`、`src/nand/nand_mt29f4g08abadawp.c` → `platform/stm32h743/nand_dma.h`；`src/main.c` → `fault.h` / `platform_ops.h` / `stm32h7xx_hal.h` |
| **G4** | 核心模块**以裸名 include `<soc>` 头**（靠全局 `ALL_INCS` 才编译过） | `src/fc/fc_crc_engine.c` → `crc.h`；`src/nand/{nand_fmc,nand_engine,nand_dispatch,nand_mt29f4g08abadawp}.c` → `delay.h`；`include/nand/nand_mt29f4g08abadawp.h` → `sys.h`（连带把 HAL/CMSIS 灌进所有 includer） |
| **G5** | 纯逻辑服务被埋在 SoC 目录（**本身不违规**，但使「服务」与「驱动」难以分开复用） | `timer.h` 的 `timer_scheduler_*`；`watchdog.h` 的 `storage_wdg_*`；~~`usart.c` 的环形缓冲~~（Stage 3 已迁出） |
| **G6** | ~~两个 target **不对称**，抽象层无法真正换 target~~ **✅ 已关闭（2026-11-07 删除 `stm32h7r3`）** | 当前仅保留 `stm32h743`；新增 target 时重新评估 |
| **G7** | **平台层反向依赖上层**（分层倒挂） | ~~`src/platform/stm32h743/usart.c` → `ftl/ftl.h`；`include/platform/stm32h743/usart.h` → `controller/controller.h`~~（Stage 3 已消除：CLI 移出平台层）；`nand_dma.{h,c}` → `nand/nand_config.h`（已断） |

> **本表是 Stage 0 基线。** 落地情况见 **§5.1 / §5.2**：
>
> - **G3 / G4 的 include 部分已收敛**（`delay.h` / `crc.h` / `dma.h` 走抽象头；
>   Stage 2 又把 `fault.h` 从 `main.c` 的 `<soc>` 路径改成裸名）⇒ **判据 A 的 grep 归零**：
>   Stage 2.6 把 `stm32_platform_register()` 声明从 `platform_ops.h` 搬进抽象 `platform.h`，
>   `main.c` 改 include `"platform.h"`，最后一处例外消失。
> - **G5 已闭合**：`timer` / `watchdog` / `fault` / `delay` / `syscalls` 五个服务的纯逻辑已搬到
>   `src/platform/*.c`，`<soc>/` 只留 `*_port.c`；Stage 3 又把 `usart` 的传输逻辑搬到
>   `src/platform/usart.c`；Stage 2.6 再把 `platform` / `crc` / `dma` / `mpu` / `qspi` /
>   `rng` / `sys` 七个服务同样拆开（含只做一行转发的 `dma` / `mpu`）。
> - **G7 的 nand 那一半已断**（`CONFIG_NAND_DMA` 已搬进 `include/platform/dma.h`），
>   `usart.c` 的两处倒挂（`ftl/ftl.h` / `controller/controller.h`）**Stage 3 已消除**——
>   CLI 整体移到 `src/hil/cli.c`，平台层不再 include 上层头。
> - ~~**G2**（`bsp/` 死代码）~~ **✅ 已删除（2026-10-12）**；~~**G6**（两 target 不对称）~~ **✅ 已随 `stm32h7r3` 删除关闭（2026-11-07）**。

---


## 五、分阶段路线图（本文件只写，不执行）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **Stage 0**（已交付） | 本设计文档 + 订正失实文档（§六） | 文档结论可复算 |
| **Stage 1**（**已部分落地**，见 §5.1） | ① 在 `include/platform/` 下为 `delay.h` / `crc.h` / `dma.h` 建抽象头（只搬声明，不搬实现；`nand_dma.h` 与 UART 的 `dma.h` 合并为一份 `dma.h`）；② `src/nand/*`、`src/fc/fc_crc_engine.c` 改 include 抽象头；③ ~~删 `src/platform/bsp/` 死代码（或接线）~~ **✅ 已删除（2026-10-12）**；④ 构建侧：核心模块的 `MODULE_INCS` 移除 `platform/<soc>` 目录 | 判据 A 的 grep 归零 + `make tests` 逐字不变 |
| **Stage 2**（**已落地**，见 §5.2） | **共享源文件层**：`timer` / `watchdog` / `fault` / `delay` / `syscalls` 五个服务的纯逻辑搬到 `src/platform/*.c`，`<soc>/` 只留 `*_port.c`（机制见 §1.5） | 判据 A/B/D/E 通过 + `make tests` 0 warning + 上板全套通过 |
| **Stage 2.5**（**已落地**，见 §5.3） | **usart 拆分**：`src/platform/stm32h743/usart.c`（1204 行）拆成共享传输 `src/platform/usart.c` + port `stm32h743/usart_port.c` + CLI `src/hil/cli.c`（消除 G7 倒挂） | 判据 A/B/D/E 通过 + 上板全套通过 |
| **Stage 2.6**（**已落地**，见 §5.4） | **剩余 7 服务补齐**：`platform_ops` / `crc` / `dma` / `mpu` / `qspi` / `rng` / `sys` 全部拆成共享源 + `*_port.c`（含只做一行转发的 `dma` / `mpu`），HAL 伞头 `sys.h` 改名 `sys_port.h` | 判据 A 归零、B/D 空、E 精确相等 + 0 warning + 上板全套通过 |
| **Stage 2.7**（**已落地**，见 §5.5） | **ISR 分散化**：删总装 `stm32h7rsxx_it.c`，每个 ISR 搬进拥有该外设的 `*_port.c`；EXTI 死代码删除 | 0 warning + `.isr_vector` 664 B 不变 + 上板全套通过 |
| **Stage 3**（需上板） | 把 `mt29f___g08a.c` 的 `HAL_NAND_MspInit` / `HAL_NAND_Init` / GPIO 与 `nand_fmc.c` 的 FMC MMIO 收进 `<soc>/`，NAND 层只留 `nand_device_t ops` 回调（`fc_nand_design.md §1` 已画出该 seam） | 上板全套通过 |
| **Stage 4**（**已取消**，2026-11-07） | ~~`stm32h7r3` 补齐缺失服务与 6 个 `*_port.c`~~。用户裁决删除 `stm32h7r3` 平台，本仓只保留 `stm32h743`；未来新增 target 时按判据 C / E 重新规划 | — |

### 5.1 Stage 1 进度（2026-10-11，只做 `stm32h743`）

- **① ② 已完成**：`include/platform/{delay,crc,dma}.h` 三个抽象头落地（零 HAL 类型），
  `include/platform/stm32h743/{delay,crc,dma,nand_dma}.h` 已删；`src/nand/*.c` 与
  `src/fc/fc_crc_engine.c` 的 include 全部走抽象头（裸名）。`src/platform/stm32h743/` 的
  `delay.c` / `dma.c` 补了各自的 `sys.h`（原本靠 `delay.h` / `dma.h` 传递 HAL 类型），
  `usart.c` 就地补 `extern DMA_HandleTypeDef g_dma_handle;`。
- **③ 已完成 / ④ 未做**：③（删 `src/platform/bsp/` 死代码）当时搁置、于 2026-10-12 补做（§2.1）；
  ④ 未做——`src/nand/makefile.mk:4` 仍把 `$(INC_ROOT)/platform/stm32h743` 加进
  `MODULE_INCS` —— 因为 `include/nand/nand_mt29f4g08abadawp.h` 仍裸 include `sys.h`
  （只为 `GPIOD` / `GPIO_PIN_6` 两个宏），删这一行会编译不过。**裸名解析仍靠 `-I` 排序**
  （§1.4 的隐式耦合机制本身尚未消除），R/B# 读下沉为平台服务后一并处理 ⇒ **Stage 2**。
- **`.bin` 240436 → 240444 B（+8）**，非「逐字不变」。原因已定位：`nand_engine.c` 不再经
  `delay.h → sys.h` 间接看到 CMSIS 头链，而 `system_stm32h7xx.h` 有一句
  `extern void ExitRun0Mode(void) __attribute__((optimize("Os")));` —— **这个只带声明的
  `optimize` 属性在本 GCC 版本会 TU 级泄漏优化标志**，`Os` ⇒ `-fmerge-constants` 打开
  ⇒ 19 条字符串字面量被放进按函数分节的 `.rodata.<func>.str1.4`；不再看到该头后合并关闭、
  字符串回到整块 `.rodata`，**仅差 7 字节对齐填充**（560 → 567）。全部指令与符号尺寸
  逐字节相同（`nm --print-size` 零差异、`objdump -d` 只差字符串指针的 `.word` 重定位）。
  实测复现：`-fmerge-constants` / `-O1` / 单独一句 `__attribute__((optimize("Os")))` 都能翻转，
  `optimize("O0")` 不能；10 万条合成宏不翻转 ⇒ 与 TU 规模无关。

### 5.2 Stage 2 进度（2026-10-11，只做 `stm32h743`）

**范围**：`timer` / `watchdog` / `fault` / `delay` / `syscalls` 五个服务（用户拍板）。
`usart` 本批**不做**（1204 行、头泄漏严重、带 `ftl/ftl.h` 反依赖）—— 已由 **Stage 2.5**（§5.3）接手。

**结果**：

| 服务 | 共享源（新） | port 源（新） | 抽象头 |
|---|---|---|---|
| timer | `src/platform/timer.c` | `stm32h743/timer_port.c` | `include/platform/timer.h` |
| watchdog | `src/platform/watchdog.c` | `stm32h743/watchdog_port.c` | `include/platform/watchdog.h` |
| fault | `src/platform/fault.c` | `stm32h743/fault_port.c` | `include/platform/fault.h` |
| delay | `src/platform/delay.c` | `stm32h743/delay_port.c` | `include/platform/delay.h` |
| syscalls | `src/platform/syscalls.c` | ——（target 无关） | ——（无公开头） |

删除：`src/platform/stm32h743/{timer,watchdog,fault,delay,syscalls}.c`、
`include/platform/stm32h743/{timer,watchdog,fault}.h`（后者必须删，否则遮蔽抽象头）。
`include/platform/stm32h743/timer_port.h` 保留（只放 TIM6 句柄/宏）。

**关键实测发现：`core_cm7.h` 无法单独包含。** 它依赖设备头提供 `IRQn_Type` 与
`__NVIC_PRIO_BITS`，裸包含直接报 `unknown type name 'IRQn_Type'`。因此共享 `.c`
**零 CMSIS 且零 HAL**（比 §1.5 的原口径更严）：`delay.c` 的 SysTick 拆成 5 个
`delay_port_*`；`syscalls.c` 的 `__get_MSP()` 换成 `__asm__ volatile("mrs %0, msp" ...)`。
`delay.c` 的 `delay_enable_dwt()` 删除，改为 `delay_port_dwt_enable()`。

**唯一改动的非平台文件**：`src/main.c` 的 `#include "platform/stm32h743/fault.h"`
→ `#include "fault.h"`（用户拍板，不留转发 shim）。`src/platform/stm32h743/sys.c`
本来就写裸名，**零改动**。`stm32h7rsxx_it.c` 的 `HAL_TIM_IRQHandler(&g_timerx_handle)`
→ `timer_port_irq_handler()`（句柄改 static 后 ISR 够不着）。另**改写 4 处注释**
（`fc_crc_engine.c` 去掉 `<soc>` 路径字面量；`delay.h` / `delay.c` / `syscalls.c`
去掉注释里的 `CMSIS` / `core_cm7.h` 字样）—— 判据 A/B/D 是机械 grep，**注释也算**。

**验收（全部通过）**：

- **上板**：完整 `test` ⇒ **57 of 57 (100%)**、0 skipped、seed `0x5a4f21e4`、871.62 ms
  （2026-10-11）。开机 `[FAULT] reset reason: PIN SFT (rsr=0x01460000)` 正常打印 ⇒ port 侧
  `RCC->RSR` 解码路径活着。
- `make clean && make tests` ⇒ **0 warning / 0 error**。
- 判据 A：`grep` 只剩 `src/main.c`（已知例外）**1 处**。
- 判据 B / D：**均为空**（`include/platform/*.h` 与 `src/platform/*.c` 零 HAL、零 SoC 头）。
- 判据 E：抽象头里 21 个 `*_port_*` 声明，**每个恰好 1 个定义**。
- **`.bin` 不是判据**，但为了排除真回归做了同路径对比（把新代码 rsync 进 HEAD 的
  worktree 重建，消除 `__FILE__` 绝对路径长度差）：

  | | HEAD | Stage 2 | Δ |
  |---|---:|---:|---:|
  | `.bin` | 239708 | 240036 | **+328** |
  | `.text` | 200376 | 200688 | **+312** |
  | `.rodata` | 36844 | 36860 | +16 |
  | `.data` / `.bss` | 1784 / 318656 | 1784 / 318660 | 0 / +4 |

  `.rodata` **基本不动**（+16）⇒ 数据布局无变化；`+312` 全部来自 23 个新增函数
  （`fault_port_entry_c` 172 / `fault_port_reset_reason_text` 244 / `delay_port_*` 7 个 …）
  减去被搬走后缩小的原函数（`fault_report_boot` 420 → 188）。`nm --print-size` 逐符号核对：
  **没有非预期消失的符号**，`.isr_vector` 两份都是 664 B ⇒ 向量表未变。
  ⚠ 注意：**`__FILE__` 是绝对路径**（`ASSERT` 把它存进 `.rodata`）⇒ 同一份代码在不同目录下
  构建的 `.bin` 尺寸不同（实测 `/tmp/ssd-base` 240036 vs 本仓路径 240780，**差 744 B**）。
  跨目录比 `.bin` 尺寸会得到假结论。

**未做**：`usart` 拆分（已由 Stage 2.5 接手，见 §5.3）；`h7r3`（已于 2026-11-07 删除）；
`sys.c` / `mpu.c` / `crc.c` / `rng.c` / `qspi.c` / `dma.c` / `nand_dma.c` / `bsp/sdram_*` /
`stm32h7rsxx_it.c`（它们本身就是 port，无可搬逻辑）；`ssd_platform_ops_s` 未扩张。

### 5.3 Stage 2.5 进度（2026-10-11，usart 拆分，只做 `stm32h743`）

**目标**：把平台目录里最后一块含可移植逻辑的大文件 `src/platform/stm32h743/usart.c`
（1204 行）按同一套三层模式拆开，并**消除唯一的平台层倒挂 G7**（`usart.c` → `ftl/ftl.h`、
`usart.h` → `controller/controller.h`）。用户拍板：**传输层 + CLI 全拆开**，CLI 落
`src/hil/cli.c`（controller 已在 `config.mk` 的 `MODULES` 里 ⇒ 零构建配置改动）。

**结果**：

| 层 | 文件 | 内容 |
|---|---|---|
| 抽象头 | `include/platform/usart.h`（新，147 行） | 传输 API + `uart_stats_s` + 8 个共享入口 + 15 个 `usart_port_*` 接口（零 HAL） |
| 共享传输 | `src/platform/usart.c`（新，413 行） | RX/TX 环形队列、`_write` newlib 钩子、log sink、TX 状态机、诊断计数 |
| port | `src/platform/stm32h743/usart_port.c`（新，251 行） | USART1 句柄 + HAL 初始化 + GPIO/DMA + `usart_port_irq_handler()` + 15 个原语 |
| port 头 | `include/platform/stm32h743/usart_port.h`（新，43 行） | `g_uart1_handle` + USART1/GPIO/时钟宏 |
| 应用层 CLI | `src/hil/cli.c`（新，728 行）+ `include/hil/cli.h` | 行编辑、命令表、help/status/version/log/wr/rd/tr/fl/info/test |

删除：`src/platform/stm32h743/usart.c`、`include/platform/stm32h743/usart.h`
（后者必须删，否则遮蔽抽象头 —— 裸名 `usart.h` 的 quote-include 先搜 includer 目录）。

**TX 路径跨层 seam（本阶段最难点）**：TX 环形状态机**同时**被共享侧（`uart_log_write` 入队）
与 port 侧（DMA 回调、ISR）触碰。**环形结构 + 状态机策略留共享侧，寄存器/DMA/缓存/CMSIS
内建下沉 port**；两侧通过明确原语交互 —— 共享侧导出 `uart_tx_ring_pop` /
`uart_tx_ring_is_empty` / `uart_tx_dma_ready` / `uart_tx_dma_complete_handler` /
`uart_tx_dma_error_handler` / `uart_stats_count_rx_error` / `uart_rx_ring_push` 给 ISR，
port 侧导出 15 个 `usart_port_*` 给共享层。

**关键实测发现**：

- **`__ALIGNED(32)` 是 CMSIS 宏**（`cmsis_gcc.h`，非本仓定义）⇒ 共享 `usart.c` 的
  `g_tx_ring` 必须改写成裸 GCC 属性 `__attribute__((aligned(32)))`，否则零 CMSIS 的共享
  文件编不过。
- **`core_cm7.h` 无法单独包含**（Stage 2 已发现）⇒ 共享层零 CMSIS：`__DMB`/`__SEV`/`__WFE`/
  `__get_IPSR`/`__get_PRIMASK`/`__set_PRIMASK`/`SCB_CleanDCache_by_Addr` 全部下沉为
  `usart_port_*` 包装（与 Stage 2 的 `delay_port_*` 同策）。
- `g_wr_buf` 的 `__attribute__((section(".sram2_data")))` 是 h743 linker 段名，随 `cli.c` 走
  （登记为已知耦合；未来新增 target 时再抽成宏）。
- 其它非平台改动：`src/main.c` 加 `#include "controller/cli.h"` + `cli_init()`，
  `uart_service_poll()` → `cli_service_poll()`（2 处）；`stm32h7rsxx_it.c`
  `usart_irq_handler()` → `usart_port_irq_handler()`；`dma.c` 改从 `usart_port.h` 取
  `g_uart1_handle`；`include/platform/stm32h743/mpu.h` 清掉 vestigial `#include "usart.h"`。

**验收**：`make clean && make tests` ⇒ **0 warning / 0 error**（241300 B）；`make format-check`
通过；判据 A 只剩 `src/main.c`（已知例外，行号因新增 include 上移）；判据 B / D 均为空；
判据 E：15 个 `usart_port_*` 声明 ↔ 15 个定义，**精确相等**。**上板由用户跑**（完整 `test`
⇒ 57 of 57；CLI 命令逐一可用且输出逐字一致）。

**已知隐患（已随平台删除关闭）**：原 `h7r3` 单体 `usart.c` 也定义 `_write` / `uart_log_write`
等，曾存在与共享 `src/platform/usart.c` **重复符号**的风险；2026-11-07 删除 `stm32h7r3`
平台后该风险不再存在。

### 5.4 Stage 2.6 进度（2026-10-11，剩余 7 服务，只做 `stm32h743`）

**目标**：把 `src/platform/stm32h743/` 里**剩下的每个服务**都按同一套三层模式补齐
——用户拍板：**即使共享源里的函数只有一行、只转发给 port 接口也接受**，要的是形式完整对齐。
范围（用户点名）：`platform_ops.c` / `crc.c` / `dma.c` / `mpu.c` / `rng.c` / `qspi.c` / `sys.c`。

**结果**：

| 服务 | 抽象头 | 共享源（新） | port（新） | 逻辑上移内容 |
|---|---|---|---|---|
| platform | `platform.h`（补 `stm32_platform_register` 声明） | `platform.c`（扩展） | ——（复用 `delay_port_now_ms`） | `stm32_platform_register` + `time_us` 换算 |
| crc | `crc.h`（补 4 个 port 接口） | `crc.c` | `crc_port.c` | IDR 回环自检策略 + 对齐契约 |
| dma | `dma.h`（补 2 个 port 接口） | `dma.c` | `dma_port.c` | 一行转发 |
| mpu | `mpu.h`（新） | `mpu.c` | `mpu_port.c` | 一行转发 |
| qspi | `qspi.h`（新） | `qspi.c` | `qspi_port.c` + `qspi_port.h` | 收发空参检查 |
| rng | `rng.h`（新） | `rng.c` | `rng_port.c` | DRDY 重试循环 + 范围取模映射 |
| sys | `sys.h`（新，零 HAL） | `sys.c` | `sys_port.c` + `sys_port.h`（伞头搬家） | 上电初始化编排（顺序即契约） |

删除：`src/platform/stm32h743/{platform_ops,crc,dma,rng,qspi,mpu,sys}.c`、
`include/platform/stm32h743/{rng,qspi,mpu,sys}.h`（**后三者必删否则遮蔽抽象头**）、
`include/platform/stm32h743/platform_ops.h`。

**关键点 / 实测发现**：

- **HAL 伞头改名**：`include/platform/stm32h743/sys.h` 同时是 HAL 伞头，被 h743 侧 8 个文件裸名
  include（含核心模块 `include/nand/nand_mt29f4g08abadawp.h`）。拆 `sys.c` 要零 HAL 的 `sys.h`，
  名字冲突 ⇒ 伞头改名 `sys_port.h`，8 个 includer 的 include 行改字；`src/main.c` **不改**
  （它另有 `#include "stm32h7xx_hal.h"`，裸名 `"sys.h"` 自动落抽象头）。原 h7r3 侧的 12 处
  `"sys.h"` 已随平台删除，不再需要处理。
- **`SUCCESS`/`ERROR` 是 CMSIS 宏**（`SUCCESS=1` / `ERROR=0`，与 `FUNC_STATUS_SUCCESS=0` **相反**）。
  `sys_stm32_clock_init` 零外部调用者 ⇒ 统一改用 `FUNC_STATUS_SUCCESS`/`FUNC_STATUS_ERROR`（0/1），
  `sys_init` 与之比较；失败即死循环的**行为不变**。
- **零构建改动**：`src/platform/makefile.mk` glob `src/platform/*.c`；`stm32h743/makefile.mk`
  glob `stm32h743/*.c`（flat，非递归）。裸名解析靠 `-I include/platform` 先于 `<soc>` 目录。
- **判据 D 口径微调**：机械 grep 的黑名单里 `sys\.h` → `sys_port\.h`（旧 `sys.h` 当初入黑名单
  只因它是 HAL 伞头；抽象 `sys.h` 现在零 HAL）。另修 `include/platform/platform.h` 注释里
  `src/platform/<soc>/*_port.c` 的 `/*`（`-Wcomment` 警告）与 `src/platform/sys.c` 注释里的
  `CMSIS` 字样（机械 grep 命中注释）。

**验收（`stm32h743` 侧）**：

- `make format && make format-check` 通过；`make clean && make tests` ⇒ **0 warning / 0 error**
  （241584 B）。
- 判据 A：**归零**（`stm32_platform_register` 声明搬进抽象 `platform.h`，`main.c` 改 include）。
- 判据 B / D：**均为空**。
- 判据 E：抽象头里 62 个 `*_port_*` 声明，**每个恰好 1 个定义**（无缺、无重复）。
- **上板**：完整 `test` ⇒ **57 of 57 (100%)**、0 skipped、seed `0x5a4f21e4`、871.11 ms
  （2026-10-11，rev=24）。开机 `[FAULT] reset reason: PIN SFT (rsr=0x01460000)` 与 Stage 2
  逐字一致 ⇒ port 侧 `RCC->RSR` 解码路径与启动编排顺序未变。

**未做**：不接线死代码（`rng` / `qspi` / `mpu` 拆完**仍是零调用者**，本轮只做结构对齐）；
不拆 `nand_dma.c`（Stage 3）、不扩 `ssd_platform_ops_s`、不碰 `stm32h7rsxx_it.c` 向量表；
`h7r3` 平台已于 2026-11-07 删除。

### 5.5 Stage 2.7 进度（2026-10-11，ISR 分散化，只做 `stm32h743`）

**目标**：删掉总装文件 `src/platform/stm32h743/stm32h7rsxx_it.c`（220 行），把每个 ISR 搬进
拥有该外设的 `*_port.c`，与 `fault_port.c` 早已采用的「port 文件即中断入口」模式对齐。
ISR 名字由 `startup_stm32h743xx.s` 的向量表固定 ⇒ 任何 TU 定义同名符号都在链接期覆盖弱
`Default_Handler`，位置可自由迁移，**无需改启动文件**。

| ISR（向量表名） | 新归属 |
|---|---|
| `USART1_IRQHandler` | `usart_port.c` |
| `DMA1_Stream0_IRQHandler` | `dma_port.c` |
| `TIM6_DAC_IRQHandler` | `timer_port.c` |
| `WWDG_IRQHandler` | `watchdog_port.c` |
| `DMA2_Stream0/1_IRQHandler` | `nand_dma.c`（`#if CONFIG_NAND_DMA` 内） |
| `SysTick_Handler` | `sys_port.c` |
| `SVC_Handler` / `DebugMon_Handler` / `PendSV_Handler` | `fault_port.c`（空壳，`SYS_SUPPORT_OS==0` 永不触发） |
| `EXTI0/2/3/15_10_IRQHandler` | **删除** —— h743 上是死代码（无 `HAL_GPIO_EXTI_Callback`、无按键初始化、无 NVIC 使能），向量回落到 `Default_Handler` |

删除：`src/platform/stm32h743/stm32h7rsxx_it.c` + `include/platform/stm32h743/stm32h7rsxx_it.h`
（后者全仓只有前者 include，向量表在 `.s` 里按名字引用 ⇒ 可整删；`makefile.mk` 是 `*.c` glob
⇒ 无需改构建）。

**验收**：`make format && make format-check` 通过；`make clean && make tests` **0 warning / 0 error**；
`arm-none-eabi-nm` 逐个确认 handler 落在其模块 `.o`（`stm32h7rsxx_it.o` 已消失），
`.isr_vector` 仍 **664 B**（向量表尺寸未变）。**上板由用户跑**（完整 `test`）。

**登记**：`CONFIG_NAND_DMA` 默认 **0**（`include/platform/dma.h`）⇒ 当前固件里 `nand_dma.c`
整体不参与编译，搬进 `#if` 后 `DMA2_Stream0/1` 向量**不再定义**、回落 `Default_Handler`。
行为安全：该配置下 DMA2 从不使能、无中断到来。

---


## 附：本次订正的失实文档

| 文件 | 原内容 | 订正 |
|---|---|---|
| `docs/architecture.md` | 「通过 `platform/soc.h` 约定的接口接入」 | 改为实际契约头 `platform/platform.h`，并注明 NAND 层尚未接入（本文件 §三 G3/G4） |
| `docs/hardware_contract.md` | 「Core SSD modules never access MMIO registers directly」 | 加现状注记：FTL/Controller/Common 成立，**NAND 层目前不成立** |
| `docs/boot_flow.md` | 引用 `soc_early_init()` / `soc_memory_init()` | 注明这两个入口尚未实现 |

---
