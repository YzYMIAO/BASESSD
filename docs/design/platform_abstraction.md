# 平台层抽象与移植规则（Platform Abstraction & Porting）

- **状态**：设计定稿（Stage 0）；**Stage 1 / Stage 2 已落地并上板验证**
  （2026-10-11，只做 `stm32h743` 一个 target，见 §五）。本文件**只描述规则与现状**，不改任何代码。
- **日期**：2026-10-11
- **范围**：`include/platform/`、`src/platform/`，以及所有依赖平台服务的核心模块
  （`src/{fc,ftl,nand,controller,common}`）。
- **关联**：`docs/architecture.md`、`docs/hardware_contract.md`、
  `docs/design/fc_nand_design.md §1`、`docs/boot_flow.md`。

---

## 〇、起因

在考虑把 soft_ssd 移植到别的 MCU 时提出：

> 「`src/platform/stm32h743/` 这些文件是底层硬件的。如果我有移植的想法，我应该把功能函数
> 抽象出来挂到 `src/platform/` 对应的文件中，而底层平台中应该只要初始化和配置、查询和设置
> 等等函数，各种功能应该抽象出来，对吧？」

**结论：方向对。但本仓的真实状态是「规则早已写在文档里，代码却漂移了」。**

- `docs/architecture.md:5` 已写明规则：「核心模块只能依赖抽象接口，不能直接访问操作系统 API
  或芯片寄存器」；`:12` 还点名了一个抽象契约头 `platform/soc.h` ——
  **该文件在全仓不存在**（`find` 核实）。
- `docs/hardware_contract.md:4-6` 断言「Core SSD modules never access MMIO registers directly」
  —— **今天不成立**：`src/nand/nand_fmc.c` 直接对 FMC 窗口做 MMIO 写，
  `src/nand/nand_mt29f4g08abadawp.c` 直接定义 `HAL_NAND_MspInit` / `HAL_NAND_Init` /
  `__HAL_RCC_*_CLK_ENABLE`。
- 真实存在的抽象 seam 是 `include/platform/platform.h` 的 `ssd_platform_ops_t`（4 个函数指针），
  但**只有 `time_us` 被填**；`include/platform/interrupt.h` 的 `ssd_irq_*` **零调用者**。

本文件把规则补成**可机械检查**的形式（§一），把现状逐文件审计出来（§二、§三），
给出目标布局（§四）与分阶段路线图（§五）。

---

## 一、规则

### 1.1 一句话规则

**接口可移植，实现按 target。**

核心模块只依赖 `include/platform/` 下的**抽象头**；每个 target 在 `src/platform/<soc>/`
提供实现；换 target = 换目录 + 换 `FIRMWARE_PLATFORM`。

### 1.2 规则**不是**「功能 vs 硬件」

常见误读是「凡是功能逻辑都要从 `<soc>/` 搬到 `platform/`」。**本仓不把这条当硬性判据**
（判据只有 §1.3 那五条）。Stage 2 之前，纯逻辑服务（调度器、状态机、环形缓冲）
**可以**留在 `<soc>/` 目录，只要满足两个条件：

1. 它的**头文件不暴露 HAL/CMSIS 类型**（判据 B）；
2. 它**能被任何 target 重新实现**，或已是一份 target 无关的实现（判据 C）。

「底层只留初始化 / 配置 / 查询 / 设置」是判据 A、B 的**推论**，不是额外要求。
Stage 2 之前，`btim.c`（16 槽任务表）、`watchdog.c`（健康域状态机）就属于「逻辑可移植、
但实现留在 SoC 目录」这一类——**它们本身不违规**；违规的是**核心模块绕过抽象头直接
include 它们**（§三 G4）。**Stage 2 起本仓改取更强立场**：纯逻辑**下沉到共享源文件**
（§1.5），`<soc>/` 只留 port 适配 —— 这样同一份逻辑不再在两个 SoC 目录里各存一份。

### 1.3 五条可机械检查的判据

| 判据 | 内容 | 检查命令 |
|---|---|---|
| **A（方向）** | 核心模块不得 include `platform/<soc>/…` 路径，也不得以裸名解析到该目录的头 | §七 · 命令 A |
| **B（纯净）** | `include/platform/*.h` 不得出现 `HAL_` / `__HAL_` / `HandleTypeDef` / `stm32h7xx` / CMSIS 符号（**含注释**，否则机械 grep 失效） | §七 · 命令 B |
| **C（对称）** | 每个抽象头在**每个** target 目录下都有实现；缺实现必须在 §三 G6 显式登记 | §七 · 命令 C |
| **D（共享源纯净）** | `src/platform/*.c` 不得出现 `HAL_` / `__HAL_` / `HandleTypeDef` / `stm32h7xx` / CMSIS 符号，也不得 include SoC 头（**含注释**） | §七 · 命令 D |
| **E（port 有定义）** | 抽象头里每个 `*_port_*` 声明，在 `src/platform/<soc>/*_port.c` 里**恰好有一个**定义 | §七 · 命令 E |

### 1.4 「编译期换 target」在本仓的真正机制（关键）

本仓选择**编译期换 target**（非运行期 vtable）：`makefile:16` 的 `FIRMWARE_PLATFORM`
（默认 `stm32h743`）→ `src/platform/makefile.mk` 只 include 对应 `<soc>/makefile.mk`
→ 只编译该目录的 `.c`。函数签名固定、链接期解析、**零间接调用开销**。

⚠ **但今天「能编译过」靠的是 include 路径的全局并集，而不是干净的目录边界：**

- `src/nand/makefile.mk:4` 把 `$(INC_ROOT)/platform/stm32h743` 加进 `MODULE_INCS`；
- `makefile:62` 的 `ALL_INCS := $(sort $(MODULE_INCS))` 把**所有模块**的 include 路径并成一份，
  `CFLAGS += $(addprefix -I,$(ALL_INCS))` 应用到**每一次**编译。

⇒ 于是 `src/fc/fc_crc_engine.c` 里一句 `#include "crc.h"`、`src/nand/nand_fmc.c` 里一句
`#include "delay.h"`，都会**静默**解析到 `include/platform/stm32h743/`——耦合因此不可见
（`grep '#include.*stm32'` 抓不到，§三 G4）。

**正确的编译期机制**：每个 target 只把自己的目录加进 `MODULE_INCS`，且**核心模块的
include 路径里不应出现 `<soc>` 目录**。这是 Stage 1 的构建侧改动（§五）。

### 1.5 共享源文件层（Stage 2 建立）

**三层概念，仍然只有两个目录**（§六 已排除第三层）：

```
include/platform/<svc>.h          ← 抽象头：零 HAL 类型（判据 B），声明公开 API + port 接口
src/platform/<svc>.c              ← 共享逻辑：跨 target 编译一次，零 SoC 头（判据 D）
src/platform/<soc>/<svc>_port.c   ← 适配：只碰 HAL/MMIO
```

- **绑定机制：同名 port 函数 + 链接期解析。** 抽象头声明 `xxx_port_*()`，共享 `.c` 直接调用，
  `src/platform/<soc>/<svc>_port.c` 提供定义。**不用 vtable、不用弱符号** —— 与 §1.4 的
  「编译期选目录」是同一套思路：签名固定、链接期解析、零间接调用开销。
- **零构建改动**：`src/platform/makefile.mk:2` 的 `$(wildcard $(SRC_ROOT)/platform/*.c)`
  **本来就**对每个 target 编译顶层 `src/platform/*.c`（Stage 2 前只有 `platform.c` 一个抽象源文件，
  现在有 13 个）；`<soc>/makefile.mk` 的平铺 wildcard 自动收新的 `<soc>/*_port.c`。
  两个 makefile 一行未改。
- **裸名解析**：`src/platform/makefile.mk:3` 在 include `<soc>/makefile.mk` **之前**加了
  `$(INC_ROOT)/platform`，`makefile:62` 的 `$(sort)` 后 `include/platform` 稳定排在前 ⇒ 裸名
  `"watchdog.h"` 落到抽象头。**前提是 `<soc>` 下的同名旧头必须删掉**（引号 include 先搜本文件
  所在目录，留着就会遮蔽抽象头）。
- **CMSIS 例外（有意保留的边界）**：判据 D 的原口径允许共享 `.c` 用 CMSIS **core**
  （`core_cm7.h`，非厂商 HAL）。**实测该头无法单独包含** —— 它依赖设备头提供 `IRQn_Type`
  与 `__NVIC_PRIO_BITS`，裸包含编译不过。⇒ 本仓更进一步：共享 `.c` **连 CMSIS core 也不用**，
  寄存器访问全部下沉为 port 函数。具体两例：
  `delay.c` 的 DWT/SysTick 寄存器拆成 7 个 `delay_port_*`；
  `syscalls.c` 的 `__get_MSP()` 换成内联汇编 `mrs %0, msp`。
  判据 D 因此按「**零 SoC 头**」执行，比原口径更严。
- **`static` 状态迁移规则**：每个变量只能落在一个 TU。`g_btimx_handle` / `g_storage_iwdg` /
  `g_storage_wwdg` / `g_fault_console_ready` → port；`g_btim_tick_ms` / `g_btim_tasks` /
  `g_btim_task_table` / `g_storage_wdg_*` → 共享。
- **目标文件不撞**：`patsubst` 按源路径映射 ⇒ `obj/platform/watchdog.o` 与
  `obj/platform/stm32h743/watchdog_port.o` 是两个文件。**符号不撞**的前提是旧 `<soc>` 源文件
  与 port 源文件**同一次改动内**完成增删，否则 `storage_wdg_init` 等会重复定义。
- **`-Wredundant-decls` 已开**（`config.mk:96-103`）⇒ 每个 port 函数**只在一个头里声明**
  （抽象头），`<soc>` 侧的 `*_port.h` 只放 HAL 宏，不重复声明 port 函数。

---

## 二、现状审计（逐文件）

分类记号：**(a)** 纯抽象 · **(b)** 纯硬件 · **(c)** 混合（接口/逻辑可移植，实现含硬件） ·
**(p)** port 适配层（只碰 HAL/MMIO，无可移植逻辑）· **(t)** 构建/工具。

### 2.1 `src/platform/` 顶层（`bsp/` 已删除）

| 文件 | 行数 | 分类 | 说明 | 违反 |
|---|---:|---|---|---|
| `platform.c` | 45 | a | `ssd_platform_ops_t` 注册表 + `stm32_platform_register()`（Stage 2.6 从 `<soc>/platform_ops.c` 搬来，只填 `time_us`），无硬件 | — |
| `crc.c` | 46 | **a** | CRC 自检策略（IDR 回环 `ASSERT`）+ 对齐契约（Stage 2.6 搬来） | — |
| `dma.c` | 19 | **a** | UART TX DMA 一行转发（Stage 2.6 搬来） | — |
| `mpu.c` | 13 | **a** | `mpu_set_protection` 一行转发（Stage 2.6 搬来） | — |
| `qspi.c` | 43 | **a** | 收发空参检查（`buf==NULL\|\|datalen==0`）后转发（Stage 2.6 搬来） | — |
| `rng.c` | 43 | **a** | DRDY 重试循环 + `rng_get_random_range` 取模映射（Stage 2.6 搬来） | — |
| `sys.c` | 83 | **a** | 上电初始化编排（顺序即契约）+ `sys_*` 一行转发（Stage 2.6 搬来） | — |
| `btim.c` | 202 | **a** | 16 槽任务表调度器（Stage 2 从 `<soc>` 搬来，只留 `btim_scheduler_tick()` 供 port 的 ISR 回调调） | — |
| `watchdog.c` | 230 | **a** | 健康域状态机 + 首故障锁存 + WWDG 窗口决策（Stage 2 搬来） | — |
| `fault.c` | 439 | **a** | Cortex-M 故障解码 / 栈扫描 / 崩溃记录打印（Stage 2 搬来） | 见下注 |
| `delay.c` | 279 | **a** | 毫秒/DWT 忙等逻辑（Stage 2 搬来） | — |
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

Stage 2 / Stage 2.6 后本目录**只剩 port**（薄 HAL 封装或向量表）。`btim.c` / `watchdog.c` /
`fault.c` / `delay.c` / `syscalls.c` **已迁出**（§2.1）；`usart.c` 拆成 port（`usart_port.c`）
+ 共享传输（`src/platform/usart.c`）+ CLI（`src/controller/cli.c`，§5.3）；`platform_ops.c` /
`crc.c` / `dma.c` / `mpu.c` / `rng.c` / `qspi.c` / `sys.c` **Stage 2.6 全部拆成共享源 + `*_port.c`**（§5.4）。

| 文件 | 行数 | 分类 | 说明 | 违反 |
|---|---:|---|---|---|
| `btim_port.c` | 61 | **p** | TIM6 句柄 + 中断使能 + `btim_port_irq_handler()`；回调转 `btim_scheduler_tick()`；向量入口 `TIM6_DAC_IRQHandler` | — |
| `watchdog_port.c` | 86 | **p** | IWDG/WWDG 句柄 + 6 个 port 函数 + 两个 HAL 回调转共享层；向量入口 `WWDG_IRQHandler` | — |
| `fault_port.c` | 220 | **p** | USART1/DMA1 轮询控制台 + 5 个 naked 异常入口 + 3 个内核异常空入口（SVC/DebugMon/PendSV）+ `fault_port_entry_c`（抓现场）+ `RCC->RSR` 复位原因解码 | — |
| `delay_port.c` | 61 | **p** | 7 个计数器/时钟查询函数（DWT、SysTick、HCLK/SYSCLK） | — |
| `usart_port.c` | 271 | **p** | USART1 句柄 + HAL 初始化 + GPIO/DMA + `usart_port_irq_handler()` + 向量入口 `USART1_IRQHandler` + 15 个 port 原语（寄存器/DMA/缓存/CMSIS 内建） | — |
| `crc_port.c` | 63 | **p** | CRC 句柄 + `HAL_CRC_Init` + `HAL_CRC_MspInit` + `__HAL_CRC_*` IDR + `HAL_CRC_Calculate` | — |
| `dma_port.c` | 54 | **p** | DMA1_Stream0 → USART1 TX 的 HAL 配置 + `HAL_DMA_IRQHandler` + 向量入口 `DMA1_Stream0_IRQHandler` | — |
| `mpu_port.c` | 71 | **p** | 中性参数填进 `MPU_Region_InitTypeDef` + `HAL_MPU_ConfigRegion` | — |
| `rng_port.c` | 52 | **p** | RNG 句柄 + `HAL_RNG_Init` + `HAL_RNG_MspInit` + `__HAL_RNG_GET_FLAG` + `HAL_RNG_GenerateRandomNumber` | — |
| `qspi_port.c` | 269 | **p** | QUADSPI（W25Q256）句柄 + 引脚/时钟 + 命令模式位域→HAL 枚举解码 + HAL 收发 | — |
| `sys_port.c` | 209 | **p** | MPU 区域表（SoC 内存映射）/ Cache / VTOR / WFI / PRIMASK / MSP 的 CMSIS 内建 + RCC-PLL 400 MHz + 向量入口 `SysTick_Handler` | — |
| `nand_dma.c` | 364 | b | DMA2 + FMC NAND 传输 + D-Cache 维护 + 向量入口 `DMA2_Stream0/1_IRQHandler`（`#if CONFIG_NAND_DMA` 内） | G3（Stage 3） |
| `bsp/sdram_w9825g6kh.c` | 207 | b | FMC SDRAM 初始化 | — |

> 分类记号 **p** = port 适配层（只碰 HAL/MMIO，不含可移植逻辑）。
> 另：`include/platform/stm32h743/bsp/` 还有 `led.h` / `norflash_w25q256.h` 两个**无实现**的头。
> **Stage 2.7 起本目录不再有总装 `stm32h7rsxx_it.c`**：每个 ISR 直接定义在拥有该外设的
> port 文件里（见 §5.5 的映射表）。

### 2.3 `src/platform/stm32h7r3/`

全部 **(b)/(c)**（`btim.c` / `watchdog.c` 同 H743 属 **(c)**）。
**缺失服务**（判据 C 不成立）：`platform_ops.c`、`crc.c`、`dma.c`、`fault.c`、`nand_dma.c`、
`qspi.c`、`rng.c`、`syscalls.c`。
⇒ 两个「移植」**并不对称**，H7R3 **不是** H743 的 drop-in 替换。

> **Stage 2.6 追加登记（`crc`/`dma`/`mpu`/`qspi`/`rng`/`sys`/`platform`）**：h743 侧这 7 个服务
> 已拆成共享源 + `*_port.c`；h7r3 侧仍是**单体** `crc.c`（若有）/ `sys.c` / `mpu.c`，且**没有**
> 对应的 `*_port.c`。共享源对所有 target 编译一次 ⇒ h7r3 解冻时会与它自己的单体实现**重复定义**
> （同 §2.3 的 `usart` 情形）。本阶段**不动** h7r3。

> **Stage 3 追加登记（`usart`）**：h743 侧 `usart.c` 已拆为 `usart_port.c` + 共享 `usart.c` +
> `controller/cli.c`；h7r3 侧仍是单体 `usart.c`（且自带 `_write` / `uart_log_write` 等定义）。
> 共享 `src/platform/usart.c` **对所有 target 编译一次** ⇒ 一旦 h7r3 解冻，会与它自己的
> `usart.c` **重复定义 `_write` 等符号**。这是 Stage 4（真换 target）必须先拆 h7r3 `usart.c`
> 的原因，本阶段**不动** h7r3。

> **Stage 2 登记：port 接口暂无第二 target 实现（有意冻结）。** h7r3 **今天编不过**，是抽象重构
> 之前的遗留脚手架（`src/main.c` 硬编码 h743 板级头、`delay.h` 原型 `uint32_t`/`uint16_t` 冲突）。
> 本次**不动**它，只在文档登记：Stage 2 的 5 个服务在 h7r3 侧**没有** `*_port.c`
> （`fault` 在 h7r3 连头都没有；`syscalls.c` 是 target 无关的，无需 port）。
> 因此**判据 E 目前只在 `stm32h743` 成立** —— 这是 §五 Stage 3 的输入，不是本阶段的缺口。

---

## 三、已核实缺口

| # | 缺口 | 证据 |
|---|---|---|
| **G1** | 文档点名的抽象头 `platform/soc.h` **不存在**；真实契约头是 `platform/platform.h`，且 4 个 op 只填 1 个 | `docs/architecture.md:12`；`include/platform/platform.h`；~~`src/platform/stm32h743/platform_ops.c`~~（Stage 2.6 已并入共享 `src/platform/platform.c`） |
| **G2** | ~~`src/platform/bsp/`（8 个 `.c`，含 124 行 `nand_loader.c`）**是死代码**，`bsp/makefile.mk` 从未被 include~~ **✅ 已删除（2026-10-12）** | `config.mk:225` `MODULES = common controller fc ftl nand platform`；`src/platform/makefile.mk` 只 include `<soc>/makefile.mk`；全仓无任何文件 include `bsp/makefile.mk` |
| **G3** | 核心模块**直接 include `<soc>` 路径** | `src/nand/nand_fmc.c:31`、`src/nand/nand_mt29f4g08abadawp.c:26` → `platform/stm32h743/nand_dma.h`；`src/main.c:43-45` → `fault.h` / `platform_ops.h` / `stm32h7xx_hal.h` |
| **G4** | 核心模块**以裸名 include `<soc>` 头**（靠全局 `ALL_INCS` 才编译过） | `src/fc/fc_crc_engine.c:32` → `crc.h`；`src/nand/{nand_fmc,nand_engine,nand_dispatch,nand_mt29f4g08abadawp}.c` → `delay.h`；`include/nand/nand_mt29f4g08abadawp.h:5` → `sys.h`（连带把 HAL/CMSIS 灌进所有 includer） |
| **G5** | 纯逻辑服务被埋在 SoC 目录（**本身不违规**，但使「服务」与「驱动」难以分开复用） | `btim.h` 的 `btim_scheduler_*`；`watchdog.h` 的 `storage_wdg_*`；~~`usart.c` 的环形缓冲~~（Stage 3 已迁出） |
| **G6** | 两个 target **不对称**，抽象层无法真正换 target | §2.3 缺失服务清单 |
| **G7** | **平台层反向依赖上层**（分层倒挂） | ~~`src/platform/stm32h743/usart.c:6` → `ftl/ftl.h`；`include/platform/stm32h743/usart.h:6` → `controller/controller.h`~~（Stage 3 已消除：CLI 移出平台层）；`nand_dma.{h,c}` → `nand/nand_config.h`（已断） |

> **本表是 Stage 0 基线。** 落地情况见 **§5.1 / §5.2**：
>
> - **G3 / G4 的 include 部分已收敛**（`delay.h` / `crc.h` / `dma.h` 走抽象头；
>   Stage 2 又把 `fault.h` 从 `main.c` 的 `<soc>` 路径改成裸名）⇒ **判据 A 的 grep 归零**：
>   Stage 2.6 把 `stm32_platform_register()` 声明从 `platform_ops.h` 搬进抽象 `platform.h`，
>   `main.c` 改 include `"platform.h"`，最后一处例外消失。
> - **G5 已闭合**：`btim` / `watchdog` / `fault` / `delay` / `syscalls` 五个服务的纯逻辑已搬到
>   `src/platform/*.c`，`<soc>/` 只留 `*_port.c`；Stage 3 又把 `usart` 的传输逻辑搬到
>   `src/platform/usart.c`；Stage 2.6 再把 `platform` / `crc` / `dma` / `mpu` / `qspi` /
>   `rng` / `sys` 七个服务同样拆开（含只做一行转发的 `dma` / `mpu`）。
> - **G7 的 nand 那一半已断**（`CONFIG_NAND_DMA` 已搬进 `include/platform/dma.h`），
>   `usart.c` 的两处倒挂（`ftl/ftl.h` / `controller/controller.h`）**Stage 3 已消除**——
>   CLI 整体移到 `src/controller/cli.c`，平台层不再 include 上层头。
> - ~~**G2**（`bsp/` 死代码）~~ **✅ 已删除（2026-10-12）**；**G6**（两 target 不对称）未动。

---

## 四、目标布局（三层概念，两个目录）+ 命名约定

```
include/platform/<svc>.h        ← 抽象接口：判据 B（零 HAL 类型）
    platform.h  interrupt.h
    delay.h  crc.h  dma.h  fault.h  usart.h
    btim.h  watchdog.h  mpu.h  rng.h  qspi.h  sys.h
src/platform/<svc>.c            ← 共享逻辑：判据 D（零 SoC 头），跨 target 编译一次
    platform.c  btim.c  watchdog.c  fault.c  delay.c  syscalls.c  usart.c
    crc.c  dma.c  mpu.c  qspi.c  rng.c  sys.c        ← Stage 2.6
src/platform/<soc>/             ← 唯一实现目录（判据 A 的唯一合法出口）
    <svc>_port.c                ← port 适配：只碰 HAL/MMIO
    btim_port.c  watchdog_port.c  fault_port.c  delay_port.c  usart_port.c
    crc_port.c  dma_port.c  mpu_port.c  qspi_port.c  rng_port.c  sys_port.c
    nand_dma.c  bsp/                                ← 本身即 port（Stage 3 处理）
include/platform/<soc>/         ← 实现头（含 HAL 类型），只有 <soc> 目录自己 include
    btim_port.h  usart_port.h  qspi_port.h  sys_port.h（原 HAL 伞头 sys.h 改名）
```

**`sys.h` 的伞头搬家（Stage 2.6）**：原 `include/platform/stm32h743/sys.h` **同时是 HAL 伞头**
（`stm32h7xx.h` + `stm32h7xx_hal.h` + `SYS_SUPPORT_OS`），被 h743 侧 8 个文件裸名 include（含核心模块
`include/nand/nand_mt29f4g08abadawp.h`）。拆 `sys.c` 需要一份零 HAL 的 `sys.h`，而该名字被占用 ⇒
**伞头改名 `include/platform/stm32h743/sys_port.h`**，8 个 includer 的 `"sys.h"` → `"sys_port.h"`；
`src/main.c` **不改**（它另有 `#include "stm32h7xx_hal.h"`，裸名 `"sys.h"` 自动落抽象头）。h7r3 侧 12 处
不动（冻结，且 h7r3 自带 `include/platform/stm32h7r3/sys.h`，quote-include 先搜 includer 目录 ⇒ 不受影响）。

**`dma.h` 是合并头**：一份 `include/platform/dma.h` 同时承载
① UART TX DMA（`uart_tx_dma_init` / `uart_tx_dma_irq_handler`，平台内部用）与
② NAND/FMC 数据搬移（原 `stm32h743/nand_dma.h` 整段，含 `CONFIG_NAND_DMA` 宏——
该宏已从 `include/nand/nand_config.h` 搬到本头，**反向依赖 G7 的 nand 那一半因此断掉**）。
两段签名都是裸类型 ⇒ 合并后仍满足判据 B。原 `stm32h743/dma.h`（含 HAL 类型
`DMA_HandleTypeDef`）与 `nand_dma.h` **已删除**。

**命名约定一句话**：抽象头放 `include/platform/`，共享逻辑放 `src/platform/`，
port 适配放 `src/platform/<soc>/`；核心模块只 include 抽象头。

### 4.1 port 接口总表（判据 B：签名里不得出现任何 HAL 类型）

```c
/* btim —— include/platform/btim.h */
void     btim_timx_int_init(uint16_t arr, uint16_t psc);   /* 公开入口，名字被 main.c 冻结 */
void     btim_port_irq_handler(void);
void     btim_scheduler_tick(void);                        /* 共享入口，由 port 的 ISR 回调调 */

/* watchdog —— include/platform/watchdog.h */
bool     watchdog_port_iwdg_init(uint32_t prescaler, uint32_t reload);
bool     watchdog_port_wwdg_init(uint32_t prescaler, uint32_t counter, uint32_t window);
void     watchdog_port_iwdg_refresh(void);
void     watchdog_port_wwdg_refresh(void);
void     watchdog_port_wwdg_irq_handler(void);
uint32_t watchdog_port_now_ms(void);
void     storage_wdg_wwdg_early_wakeup(void);              /* 共享入口 */

/* fault —— include/platform/fault.h */
void     fault_port_console_init(void);
void     fault_port_putc(char character);
void     fault_port_console_drain(void);
void     fault_port_enable_configurable_faults(void);
uint32_t fault_port_reset_flags(void);
void     fault_port_reset_reason_text(uint32_t flags, char *buffer, uint32_t capacity);
void     fault_port_reset_flags_clear(void);
void     fault_report_and_halt(const fault_ctx_t *ctx);    /* 共享入口 */

/* delay —— include/platform/delay.h */
uint32_t delay_port_hclk_hz(void);
uint32_t delay_port_core_clock_hz(void);
uint32_t delay_port_now_ms(void);
bool     delay_port_dwt_enable(void);
uint32_t delay_port_dwt_cycles(void);
uint32_t delay_port_systick_load(void);
uint32_t delay_port_systick_value(void);

/* platform —— include/platform/platform.h */
int      stm32_platform_register(void);                       /* 共享入口，名字被 main.c 冻结 */

/* crc —— include/platform/crc.h（Stage 2.6） */
void     crc_init(void);                                      /* 共享入口 */
uint32_t crc_compute(const uint8_t *data, uint32_t length);   /* 共享入口 */
void     crc_port_init(void);
void     crc_port_set_idr(uint32_t value);
uint32_t crc_port_get_idr(void);
uint32_t crc_port_compute(const uint8_t *data, uint32_t length);

/* dma —— include/platform/dma.h（Stage 2.6） */
uint8_t  uart_tx_dma_init(void);                              /* 共享入口 */
void     uart_tx_dma_irq_handler(void);                       /* 共享入口 */
uint8_t  dma_port_uart_tx_init(void);
void     dma_port_uart_tx_irq_handler(void);

/* mpu —— include/platform/mpu.h（Stage 2.6） */
void     mpu_set_protection(uint32_t baseaddr, uint32_t size, uint32_t rnum,
                            uint8_t de, uint8_t ap, uint8_t sen, uint8_t cen, uint8_t ben);
void     mpu_port_set_protection(uint32_t baseaddr, uint32_t size, uint32_t rnum,
                                 uint8_t de, uint8_t ap, uint8_t sen, uint8_t cen, uint8_t ben);

/* qspi —— include/platform/qspi.h（Stage 2.6） */
uint8_t  qspi_wait_flag(uint32_t flag, uint8_t sta, uint32_t wtime);   /* 共享入口 */
uint8_t  qspi_init(void);                                              /* 共享入口 */
uint8_t  qspi_send_cmd(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle); /* 共享入口 */
uint8_t  qspi_receive(uint8_t *buf, uint32_t datalen);                 /* 共享入口（空参检查） */
uint8_t  qspi_transmit(uint8_t *buf, uint32_t datalen);                /* 共享入口（空参检查） */
uint8_t  qspi_port_wait_flag(uint32_t flag, uint8_t sta, uint32_t wtime);
uint8_t  qspi_port_init(void);
uint8_t  qspi_port_send_cmd(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle);
uint8_t  qspi_port_receive(uint8_t *buf, uint32_t datalen);
uint8_t  qspi_port_transmit(uint8_t *buf, uint32_t datalen);

/* rng —— include/platform/rng.h（Stage 2.6） */
uint8_t  rng_init(void);                                      /* 共享入口（重试循环） */
uint32_t rng_get_random_num(void);                            /* 共享入口 */
int      rng_get_random_range(int min, int max);              /* 共享入口（取模映射） */
uint8_t  rng_port_init(void);
uint8_t  rng_port_data_ready(void);
uint32_t rng_port_get_random(void);

/* sys —— include/platform/sys.h（Stage 2.6） */
void     sys_init(void);                                      /* 共享入口，名字被 main.c 冻结 */
void     sys_mpu_config(void);
void     sys_cache_enable(void);
uint8_t  get_icache_sta(void);
uint8_t  get_dcache_sta(void);
void     sys_nvic_set_vector_table(uint32_t vtor, uint32_t offset);
void     sys_wfi_set(void);
void     sys_intx_disable(void);
void     sys_intx_enable(void);
void     sys_msr_msp(uint32_t addr);
uint8_t  sys_stm32_clock_init(uint32_t plln, uint32_t pllm, uint32_t pllp, uint32_t pllq);
/*   ↑ 共享入口；↓ port 接口（11 个） */
void     sys_port_mpu_config(void);
void     sys_port_cache_enable(void);
uint8_t  sys_port_icache_enabled(void);
uint8_t  sys_port_dcache_enabled(void);
void     sys_port_set_vector_table(uint32_t vtor, uint32_t offset);
void     sys_port_wfi(void);
void     sys_port_irq_disable(void);
void     sys_port_irq_enable(void);
void     sys_port_set_msp(uint32_t addr);
void     sys_port_hal_init(void);
uint8_t  sys_port_clock_init(uint32_t plln, uint32_t pllm, uint32_t pllp, uint32_t pllq);

/* syscalls —— 无 port（target 无关） */

/* usart —— include/platform/usart.h（Stage 3） */
void     usart_init(uint32_t baudrate);                       /* 共享入口，名字被 sys.c 冻结 */
int      uart_log_write(const char *data, uint32_t length);   /* 共享 */
int      uart_log_flush(uint32_t timeout_ms);                 /* 共享 */
void     uart_get_stats(uart_stats_t *stats);                 /* 共享 */
uint8_t  uart_rx_read(uint8_t *byte);                         /* 共享；cli.c 消费 */
/*   ↑ 共享入口；↓ port 接口（15 个） */
uint8_t  usart_port_hw_init(uint32_t baudrate);
void     usart_port_irq_handler(void);
int      usart_port_tx_dma_start(const uint8_t *buffer, uint32_t length);
void     usart_port_tx_dma_detach(void);
uint16_t usart_port_tx_dma_remaining(void);
void     usart_port_tx_irq_enable(void);
uint8_t  usart_port_tx_complete(void);
uint32_t usart_port_now_ms(void);
void     usart_port_memory_barrier(void);
void     usart_port_event_signal(void);
void     usart_port_event_wait(void);
uint8_t  usart_port_context_is_atomic(void);
uint32_t usart_port_irq_state(void);
void     usart_port_irq_disable(void);
void     usart_port_irq_restore(uint32_t state);
```

**只在 port、绝不出现在抽象头**（HAL 签名固定，写进抽象头会违反判据 B）：
`HAL_TIM_Base_MspInit` / `HAL_TIM_PeriodElapsedCallback` / `HAL_WWDG_MspInit` /
`HAL_WWDG_EarlyWakeupCallback` / `HAL_UART_MspInit` / `HAL_CRC_MspInit` / `HAL_QSPI_MspInit` /
`HAL_RNG_MspInit`，以及 `fault_trampoline` / `fault_port_entry_c` / 5 个
`FAULT_ENTRY(...)_Handler`（naked asm）、USART 的 2 个 `DMA_HandleTypeDef *` 签名回调、
QSPI 的 `g_qspi_handle` / `QSPI_HandleTypeDef *hqspi`、`MPU_Region_InitTypeDef` 字段
（`mpu_port_set_protection` 的形参是中性的 `uint32_t`/`uint8_t`，HAL 枚举只在 port 内部解码）。

---

## 五、分阶段路线图（本文件只写，不执行）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **Stage 0**（已交付） | 本设计文档 + 订正失实文档（§六） | 文档结论可复算 |
| **Stage 1**（**已部分落地**，见 §5.1） | ① 在 `include/platform/` 下为 `delay.h` / `crc.h` / `dma.h` 建抽象头（只搬声明，不搬实现；`nand_dma.h` 与 UART 的 `dma.h` 合并为一份 `dma.h`）；② `src/nand/*`、`src/fc/fc_crc_engine.c` 改 include 抽象头；③ ~~删 `src/platform/bsp/` 死代码（或接线）~~ **✅ 已删除（2026-10-12）**；④ 构建侧：核心模块的 `MODULE_INCS` 移除 `platform/<soc>` 目录 | 判据 A 的 grep 归零 + `make tests` 逐字不变 |
| **Stage 2**（**已落地**，见 §5.2） | **共享源文件层**：`btim` / `watchdog` / `fault` / `delay` / `syscalls` 五个服务的纯逻辑搬到 `src/platform/*.c`，`<soc>/` 只留 `*_port.c`（机制见 §1.5） | 判据 A/B/D/E 通过 + `make tests` 0 warning + 上板全套通过 |
| **Stage 2.5**（**已落地**，见 §5.3） | **usart 拆分**：`src/platform/stm32h743/usart.c`（1204 行）拆成共享传输 `src/platform/usart.c` + port `stm32h743/usart_port.c` + CLI `src/controller/cli.c`（消除 G7 倒挂） | 判据 A/B/D/E 通过 + 上板全套通过 |
| **Stage 2.6**（**已落地**，见 §5.4） | **剩余 7 服务补齐**：`platform_ops` / `crc` / `dma` / `mpu` / `qspi` / `rng` / `sys` 全部拆成共享源 + `*_port.c`（含只做一行转发的 `dma` / `mpu`），HAL 伞头 `sys.h` 改名 `sys_port.h` | 判据 A 归零、B/D 空、E 精确相等 + 0 warning + 上板全套通过 |
| **Stage 2.7**（**已落地**，见 §5.5） | **ISR 分散化**：删总装 `stm32h7rsxx_it.c`，每个 ISR 搬进拥有该外设的 `*_port.c`；EXTI 死代码删除 | 0 warning + `.isr_vector` 664 B 不变 + 上板全套通过 |
| **Stage 3**（需上板） | 把 `nand_mt29f4g08abadawp.c` 的 `HAL_NAND_MspInit` / `HAL_NAND_Init` / GPIO 与 `nand_fmc.c` 的 FMC MMIO 收进 `<soc>/`，NAND 层只留 `nand_device_t ops` 回调（`fc_nand_design.md §1` 已画出该 seam） | 上板全套通过 |
| **Stage 4**（真换 target 时才做） | `stm32h7r3` 补齐缺失服务与 6 个 `*_port.c`（btim/watchdog/fault/delay/usart 及各自的头），使判据 C / E 成立；并拆 h7r3 单体 `usart.c` 以消除 `_write` 重复定义（§2.3） | 判据 C、E 通过 |

### 5.1 Stage 1 进度（2026-10-11，只做 `stm32h743`）

- **① ② 已完成**：`include/platform/{delay,crc,dma}.h` 三个抽象头落地（零 HAL 类型），
  `include/platform/stm32h743/{delay,crc,dma,nand_dma}.h` 已删；`src/nand/*.c` 与
  `src/fc/fc_crc_engine.c` 的 include 全部走抽象头（裸名）。`src/platform/stm32h743/` 的
  `delay.c` / `dma.c` 补了各自的 `sys.h`（原本靠 `delay.h` / `dma.h` 传递 HAL 类型），
  `usart.c` 就地补 `extern DMA_HandleTypeDef g_dma_handle;`。
- **③ 已完成 / ④ 未做**：③（删 `src/platform/bsp/` 死代码）当时搁置、于 2026-10-12 补做（§2.1）；
  ④ 未做——`src/nand/makefile.mk:4` 仍把 `$(INC_ROOT)/platform/stm32h743` 加进
  `MODULE_INCS` —— 因为 `include/nand/nand_mt29f4g08abadawp.h:5` 仍裸 include `sys.h`
  （只为 `GPIOD` / `GPIO_PIN_6` 两个宏），删这一行会编译不过。**裸名解析仍靠 `-I` 排序**
  （§1.4 的隐式耦合机制本身尚未消除），R/B# 读下沉为平台服务后一并处理 ⇒ **Stage 2**。
- **`.bin` 240436 → 240444 B（+8）**，非「逐字不变」。原因已定位：`nand_engine.c` 不再经
  `delay.h → sys.h` 间接看到 CMSIS 头链，而 `system_stm32h7xx.h:88` 有一句
  `extern void ExitRun0Mode(void) __attribute__((optimize("Os")));` —— **这个只带声明的
  `optimize` 属性在本 GCC 版本会 TU 级泄漏优化标志**，`Os` ⇒ `-fmerge-constants` 打开
  ⇒ 19 条字符串字面量被放进按函数分节的 `.rodata.<func>.str1.4`；不再看到该头后合并关闭、
  字符串回到整块 `.rodata`，**仅差 7 字节对齐填充**（560 → 567）。全部指令与符号尺寸
  逐字节相同（`nm --print-size` 零差异、`objdump -d` 只差字符串指针的 `.word` 重定位）。
  实测复现：`-fmerge-constants` / `-O1` / 单独一句 `__attribute__((optimize("Os")))` 都能翻转，
  `optimize("O0")` 不能；10 万条合成宏不翻转 ⇒ 与 TU 规模无关。

### 5.2 Stage 2 进度（2026-10-11，只做 `stm32h743`）

**范围**：`btim` / `watchdog` / `fault` / `delay` / `syscalls` 五个服务（用户拍板）。
`usart` 本批**不做**（1204 行、头泄漏严重、带 `ftl/ftl.h` 反依赖）—— 已由 **Stage 2.5**（§5.3）接手。

**结果**：

| 服务 | 共享源（新） | port 源（新） | 抽象头 |
|---|---|---|---|
| btim | `src/platform/btim.c` | `stm32h743/btim_port.c` | `include/platform/btim.h` |
| watchdog | `src/platform/watchdog.c` | `stm32h743/watchdog_port.c` | `include/platform/watchdog.h` |
| fault | `src/platform/fault.c` | `stm32h743/fault_port.c` | `include/platform/fault.h` |
| delay | `src/platform/delay.c` | `stm32h743/delay_port.c` | `include/platform/delay.h` |
| syscalls | `src/platform/syscalls.c` | ——（target 无关） | ——（无公开头） |

删除：`src/platform/stm32h743/{btim,watchdog,fault,delay,syscalls}.c`、
`include/platform/stm32h743/{btim,watchdog,fault}.h`（后者必须删，否则遮蔽抽象头）。
`include/platform/stm32h743/btim_port.h` **新增**（只放 HAL 与 `BTIM_TIMX*` 宏）。

**关键实测发现：`core_cm7.h` 无法单独包含。** 它依赖设备头提供 `IRQn_Type` 与
`__NVIC_PRIO_BITS`，裸包含直接报 `unknown type name 'IRQn_Type'`。因此共享 `.c`
**零 CMSIS 且零 HAL**（比 §1.5 的原口径更严）：`delay.c` 的 DWT/SysTick 拆成 7 个
`delay_port_*`；`syscalls.c` 的 `__get_MSP()` 换成 `__asm__ volatile("mrs %0, msp" ...)`。
`delay.c` 的 `delay_enable_dwt()` 删除，改为 `delay_port_dwt_enable()`。

**唯一改动的非平台文件**：`src/main.c:43` 的 `#include "platform/stm32h743/fault.h"`
→ `#include "fault.h"`（用户拍板，不留转发 shim）。`src/platform/stm32h743/sys.c:4`
本来就写裸名，**零改动**。`stm32h7rsxx_it.c:189` 的 `HAL_TIM_IRQHandler(&g_btimx_handle)`
→ `btim_port_irq_handler()`（句柄改 static 后 ISR 够不着）。另**改写 4 处注释**
（`fc_crc_engine.c:24` 去掉 `<soc>` 路径字面量；`delay.h` / `delay.c` / `syscalls.c`
去掉注释里的 `CMSIS` / `core_cm7.h` 字样）—— 判据 A/B/D 是机械 grep，**注释也算**。

**验收（全部通过）**：

- **上板**：完整 `test` ⇒ **57 of 57 (100%)**、0 skipped、seed `0x5a4f21e4`、871.62 ms
  （2026-10-11）。开机 `[FAULT] reset reason: PIN SFT (rsr=0x01460000)` 正常打印 ⇒ port 侧
  `RCC->RSR` 解码路径活着。
- `make clean && make tests` ⇒ **0 warning / 0 error**。
- 判据 A：`grep` 只剩 `src/main.c:44`（已知例外）**1 处**。
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

**未做**：`usart` 拆分（已由 Stage 2.5 接手，见 §5.3）；`h7r3`（保持冻结，§2.3 已登记）；
`sys.c` / `mpu.c` / `crc.c` / `rng.c` / `qspi.c` / `dma.c` / `nand_dma.c` / `bsp/sdram_*` /
`stm32h7rsxx_it.c`（它们本身就是 port，无可搬逻辑）；`ssd_platform_ops_t` 未扩张；
`interrupt.h` 未动。

### 5.3 Stage 2.5 进度（2026-10-11，usart 拆分，只做 `stm32h743`）

**目标**：把平台目录里最后一块含可移植逻辑的大文件 `src/platform/stm32h743/usart.c`
（1204 行）按同一套三层模式拆开，并**消除唯一的平台层倒挂 G7**（`usart.c` → `ftl/ftl.h`、
`usart.h` → `controller/controller.h`）。用户拍板：**传输层 + CLI 全拆开**，CLI 落
`src/controller/cli.c`（controller 已在 `config.mk` 的 `MODULES` 里 ⇒ 零构建配置改动）。

**结果**：

| 层 | 文件 | 内容 |
|---|---|---|
| 抽象头 | `include/platform/usart.h`（新，147 行） | 传输 API + `uart_stats_t` + 8 个共享入口 + 15 个 `usart_port_*` 接口（零 HAL） |
| 共享传输 | `src/platform/usart.c`（新，413 行） | RX/TX 环形队列、`_write` newlib 钩子、log sink、TX 状态机、诊断计数 |
| port | `src/platform/stm32h743/usart_port.c`（新，251 行） | USART1 句柄 + HAL 初始化 + GPIO/DMA + `usart_port_irq_handler()` + 15 个原语 |
| port 头 | `include/platform/stm32h743/usart_port.h`（新，43 行） | `g_uart1_handle` + USART1/GPIO/时钟宏 |
| 应用层 CLI | `src/controller/cli.c`（新，728 行）+ `include/controller/cli.h` | 行编辑、命令表、help/status/version/log/wr/rd/tr/fl/info/test |

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
  （登记为已知耦合，将来 h7r3 可移植再抽成宏）。
- 其它非平台改动：`src/main.c` 加 `#include "controller/cli.h"` + `cli_init()`，
  `uart_service_poll()` → `cli_service_poll()`（2 处）；`stm32h7rsxx_it.c:170`
  `usart_irq_handler()` → `usart_port_irq_handler()`；`dma.c` 改从 `usart_port.h` 取
  `g_uart1_handle`；`include/platform/stm32h743/mpu.h` 清掉 vestigial `#include "usart.h"`。

**验收**：`make clean && make tests` ⇒ **0 warning / 0 error**（241300 B）；`make format-check`
通过；判据 A 只剩 `src/main.c:45`（已知例外，行号因新增 include 上移）；判据 B / D 均为空；
判据 E：15 个 `usart_port_*` 声明 ↔ 15 个定义，**精确相等**。**上板由用户跑**（完整 `test`
⇒ 57 of 57；CLI 命令逐一可用且输出逐字一致）。

**已知隐患（登记，本阶段不动）**：共享 `src/platform/usart.c` **对所有 target 编译一次**，
h7r3 单体 `usart.c` 也定义 `_write` / `uart_log_write` 等 ⇒ h7r3 解冻时会**重复符号**
（§2.3 已登记，Stage 4 处理）。

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
  （它另有 `#include "stm32h7xx_hal.h"`，裸名 `"sys.h"` 自动落抽象头）。h7r3 侧 12 处不动（冻结）。
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
不拆 `nand_dma.c`（Stage 3）、不动 h7r3、不扩 `ssd_platform_ops_t`、不碰 `stm32h7rsxx_it.c` 向量表。

### 5.5 Stage 2.7 进度（2026-10-11，ISR 分散化，只做 `stm32h743`）

**目标**：删掉总装文件 `src/platform/stm32h743/stm32h7rsxx_it.c`（220 行），把每个 ISR 搬进
拥有该外设的 `*_port.c`，与 `fault_port.c` 早已采用的「port 文件即中断入口」模式对齐。
ISR 名字由 `startup_stm32h743xx.s` 的向量表固定 ⇒ 任何 TU 定义同名符号都在链接期覆盖弱
`Default_Handler`，位置可自由迁移，**无需改启动文件**。

| ISR（向量表名） | 新归属 |
|---|---|
| `USART1_IRQHandler` | `usart_port.c` |
| `DMA1_Stream0_IRQHandler` | `dma_port.c` |
| `TIM6_DAC_IRQHandler` | `btim_port.c` |
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

**登记**：`CONFIG_NAND_DMA` 默认 **0**（`include/platform/dma.h:36`）⇒ 当前固件里 `nand_dma.c`
整体不参与编译，搬进 `#if` 后 `DMA2_Stream0/1` 向量**不再定义**、回落 `Default_Handler`。
行为安全：该配置下 DMA2 从不使能、无中断到来。

---

## 六、不做（本设计边界）

- **不**引入 `src/platform/service/` 第三层——Stage 2 的「三层」是**概念上的**（抽象头 /
  共享源 / port 源），落地**仍是两个目录**：共享源直接放 `src/platform/` 顶层。
- **不**用 vtable、**不**用弱符号做 port 绑定——一律「同名 port 函数 + 链接期解析」（§1.5）。
- **不**把运行期 vtable 作为主机制——`ssd_platform_ops_t` 保留为**已存在的小 seam**，不扩张。
- Stage 2 **不**拆 `usart`（1204 行、头泄漏、带 `ftl/ftl.h` 反依赖 G7）—— 已由 Stage 2.5（§5.3）
  完成：传输层留共享、CLI 移到 `src/controller/cli.c`，G7 倒挂消除。
- **不动** `src/ftl/**`、`tests/framework/**`、`ut_main.c`。

---

## 七、附录：取证命令（结论可复算）

```bash
# 命令 A —— 判据 A：核心模块直连 <soc> 路径
#   Stage 0 基线 5 处（G3 的 4 处 include + fc_crc_engine.c:24 注释）；
#   Stage 1 后剩 3 处；Stage 2 后剩 1 处：main.c:44（板级装配根 platform_ops.h）；
#   Stage 2.6 后归零（stm32_platform_register 声明搬进抽象 platform.h）
grep -rn 'platform/stm32h743' src --include=*.c --include=*.h | grep -v '^src/platform/'

# 命令 A' —— 判据 A 的补充：裸名 include（靠全局 ALL_INCS 解析，需人工判断落到哪个目录）
#   Stage 1 后：delay.h / crc.h / dma.h 已稳定解析到 include/platform/（抽象头）；
#   仍解析到 <soc> 目录的只剩 G4 的 sys.h 与 main.c 的板级头：
#   include/nand/nand_mt29f4g08abadawp.h:5 的 sys.h（Stage 3）、
#   src/main.c 的 btim/mpu/sys/usart/watchdog（板级装配根，不在核心模块范围）
#   Stage 2 起 btim.h/watchdog.h 已解析到 include/platform/ 抽象头
grep -rnE '#include "(delay|crc|sys|usart|btim|watchdog|mpu|rng|qspi|dma)\.h"' src include \
  --include=*.c --include=*.h | grep -vE '^src/platform/|^include/platform/'

# 命令 B —— 判据 B：抽象头零 HAL 类型（预期为空）
grep -nE 'HAL_|__HAL_|stm32h7xx|CMSIS|HandleTypeDef' include/platform/*.h

# 命令 C —— 判据 C：两 target 的实现对称性（对比文件清单）
#   预期：h743 独有 btim_port.h/qspi_port.h/sys_port.h/usart_port.h（port 头）；
#   h7r3 独有 btim.h/delay.h/delay_plus.h/spi_sdnand.h/watchdog.h/sys.h（Stage 4 待收敛）
ls include/platform/stm32h743/*.h | xargs -n1 basename | sort > /tmp/h743.txt
ls include/platform/stm32h7r3/*.h  | xargs -n1 basename | sort > /tmp/h7r3.txt
diff /tmp/h743.txt /tmp/h7r3.txt

# 命令 D —— 判据 D：共享源零 HAL / 零 SoC 头（预期为空，注释也算）
#   Stage 2 起口径比 §1.5 的原口径更严：连 CMSIS core 头都不许出现
#   Stage 2.6 起黑名单 sys\.h → sys_port\.h（旧 sys.h 是 HAL 伞头；抽象 sys.h 已零 HAL）
grep -nE 'HAL_|__HAL_|HandleTypeDef|stm32h7xx|CMSIS|core_cm7|sys_port\.h|ftl/|controller/|nand/' src/platform/*.c

# 命令 E —— 判据 E：抽象头里每个 *_port_* 声明恰好有一个定义
#   预期：62 个名字，每个 defs=1（无缺、无重复）
for f in $(grep -rhoE '[a-z_]+_port_[a-z_]+' include/platform/*.h | sort -u); do
  n=$(grep -rc "^[a-z_].*[ *]$f(" src/platform/*.c src/platform/stm32h743/*_port.c 2>/dev/null \
        | awk -F: '{s+=$2} END{print s+0}')
  printf '%-42s defs=%s\n' "$f" "$n"
done

# 命令 F —— G2 复核：顶层 bsp/ 是否参与编译
#   2026-10-12 前：只看到 src/platform/bsp/ 自身的 makefile.mk，无任何 include 它的片段
#   2026-10-12 后（目录已删）：应无输出（注意 <soc>/bsp 不含子串 "platform/bsp"）
grep -rn 'platform/bsp' --include=*.mk --include=makefile .
```

### 附：`__FILE__` 绝对路径陷阱（比对 `.bin` 时必读）

本仓以**绝对路径**编译（`Compiling the /home/.../src/main.c`），而 `ASSERT` 家族把
`__FILE__` 存进 `.rodata`。⇒ **同一份代码在不同目录下构建，`.bin` 尺寸会差数百字节**
（实测：Stage 2 的代码在 `/tmp/ssd-base` 下 240036 B，在本仓路径下 240780 B，**差 744 B**）。
跨目录比 `.bin` 会得到假结论；要比就**同路径比**，或改用 `nm --print-size` 逐符号 +
`.text` / `.rodata` 分节对比（`.rodata` 里的 `__FILE__` 字符串才是被污染的那部分）。

---

## 附：本次订正的失实文档

| 文件 | 原内容 | 订正 |
|---|---|---|
| `docs/architecture.md:12` | 「通过 `platform/soc.h` 约定的接口接入」 | 改为实际契约头 `platform/platform.h`，并注明 NAND 层尚未接入（本文件 §三 G3/G4） |
| `docs/hardware_contract.md:4-6` | 「Core SSD modules never access MMIO registers directly」 | 加现状注记：FTL/Controller/Common 成立，**NAND 层目前不成立** |
| `docs/boot_flow.md:6-7` | 引用 `soc_early_init()` / `soc_memory_init()` | 注明这两个入口尚未实现 |
