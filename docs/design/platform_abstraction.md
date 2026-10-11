# 平台层抽象与移植规则（Platform Abstraction & Porting）

- **状态**：设计定稿（Stage 0）；**Stage 1 已部分落地**（2026-10-11，只做 `stm32h743`
  一个 target，见 §五）。本文件**只描述规则与现状**，不改任何代码。
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

常见误读是「凡是功能逻辑都要从 `<soc>/` 搬到 `platform/`」。**本仓不采纳这条。**
纯逻辑服务（调度器、状态机、环形缓冲）**可以**留在 `<soc>/` 目录，只要满足两个条件：

1. 它的**头文件不暴露 HAL/CMSIS 类型**（判据 B）；
2. 它**能被任何 target 重新实现**，或已是一份 target 无关的实现（判据 C）。

「底层只留初始化 / 配置 / 查询 / 设置」是判据 A、B 的**推论**，不是额外要求。
本仓的 `btim.c`（16 槽任务表）、`watchdog.c`（健康域状态机）、`usart.c`（环形缓冲）
就属于「逻辑可移植、但实现留在 SoC 目录」这一类——**它们本身不违规**；
违规的是**核心模块绕过抽象头直接 include 它们**（§三 G4）。

### 1.3 三条可机械检查的判据

| 判据 | 内容 | 检查命令 |
|---|---|---|
| **A（方向）** | 核心模块不得 include `platform/<soc>/…` 路径，也不得以裸名解析到该目录的头 | §七 · 命令 A |
| **B（纯净）** | `include/platform/*.h` 不得出现 `HAL_` / `__HAL_` / `HandleTypeDef` / `stm32h7xx` / CMSIS 符号 | §七 · 命令 B |
| **C（对称）** | 每个抽象头在**每个** target 目录下都有实现；缺实现必须在 §三 G6 显式登记 | §七 · 命令 C |

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

---

## 二、现状审计（逐文件）

分类记号：**(a)** 纯抽象 · **(b)** 纯硬件 · **(c)** 混合（接口/逻辑可移植，实现含硬件） ·
**(t)** 构建/工具。

### 2.1 `src/platform/` 顶层与 `bsp/`

| 文件 | 行数 | 分类 | 说明 | 违反 |
|---|---:|---|---|---|
| `platform.c` | 21 | a | `ssd_platform_ops_t` 注册表，无硬件 | — |
| `makefile.mk` | 14 | t | 按 `FIRMWARE_PLATFORM` 选 `<soc>` 目录 | — |
| `bsp/nand_loader.c` | 124 | b | 启动期 NAND 固件加载 + ITCM 初始化（硬编码 MMIO `0x80000000`） | **死代码**（G2）；`TCM_Init` / `NAND_Load_Firmware` 零调用者 |
| `bsp/system_init.c` | 8 | b | `firmware_system_init()` 空桩 | **死代码**（G2）；零调用者 |
| `bsp/interrupt.c` | 1 | b | 单行 `#warning` 占位 | **死代码**（G2） |
| `bsp/drivers/{cache,dma,pcie,timer,uart}.c` | 各 1 | b | 各一行 `#warning` 占位 | **死代码**（G2） |

### 2.2 `src/platform/stm32h743/`（15 个 `.c` / 3937 行）

| 文件 | 行数 | 分类 | 说明 | 违反 |
|---|---:|---|---|---|
| `platform_ops.c` | 27 | c | 抽象 seam 的实现；**只填 `time_us`**（`HAL_GetTick()*1000`），`log_write` / `memory_*` 为 NULL | G1 |
| `btim.c` | 220 | **c** | **16 槽任务表调度器是纯逻辑**，只有 TIM6 tick 源是硬件 | G5 |
| `watchdog.c` | 245 | **c** | **健康域状态机（超时/锁存/窗口计算）是纯逻辑**，只有 IWDG/WWDG 刷新是硬件 | G5 |
| `usart.c` | 1201 | **c** | **RX/TX 环形缓冲 + CLI + log sink 是纯逻辑**，寄存器部分是硬件 | G5、G7（反向依赖 FTL） |
| `crc.c` | 78 | b | HAL CRC-32 外设封装 | G4（被 `fc_crc_engine.c` 裸名 include） |
| `delay.c` | 291 | b | Cortex-M7 DWT / SysTick 忙等 + `HAL_GetTick` | G4（被 nand 四处裸名 include） |
| `nand_dma.c` | 345 | b | DMA2 + FMC NAND 传输 + D-Cache 维护 | G3 |
| `fault.c` | 585 | b | Cortex-M7 故障捕获（naked handler + 直接 poke USART1/DMA 寄存器） | G3（`main.c` 直连） |
| `sys.c` | 197 | b | MPU 表 / Cache / VTOR / RCC-PLL 400 MHz | — |
| `mpu.c` | 64 | b | `HAL_MPU_ConfigRegion` 封装 | — |
| `rng.c` | 78 | b | HAL RNG 封装 | — |
| `qspi.c` | 268 | b | QUADSPI（W25Q256）封装 | — |
| `dma.c` | 38 | b | DMA1_Stream0 → USART1 TX | — |
| `stm32h7rsxx_it.c` | 220 | b | 向量表 ISR | — |
| `syscalls.c` | 80 | b | newlib `_sbrk` + 堆栈守卫 | — |
| `bsp/sdram_w9825g6kh.c` | 207 | b | FMC SDRAM 初始化 | — |

> 另：`include/platform/stm32h743/bsp/` 还有 `led.h` / `norflash_w25q256.h` 两个**无实现**的头。

### 2.3 `src/platform/stm32h7r3/`

全部 **(b)/(c)**（`btim.c` / `watchdog.c` 同 H743 属 **(c)**）。
**缺失服务**（判据 C 不成立）：`platform_ops.c`、`crc.c`、`dma.c`、`fault.c`、`nand_dma.c`、
`qspi.c`、`rng.c`、`syscalls.c`。
⇒ 两个「移植」**并不对称**，H7R3 **不是** H743 的 drop-in 替换。

---

## 三、已核实缺口

| # | 缺口 | 证据 |
|---|---|---|
| **G1** | 文档点名的抽象头 `platform/soc.h` **不存在**；真实契约头是 `platform/platform.h`，且 4 个 op 只填 1 个 | `docs/architecture.md:12`；`include/platform/platform.h`；`src/platform/stm32h743/platform_ops.c` |
| **G2** | `src/platform/bsp/`（8 个 `.c`，含 124 行 `nand_loader.c`）**是死代码**，`bsp/makefile.mk` 从未被 include | `config.mk:225` `MODULES = common controller fc ftl nand platform`；`src/platform/makefile.mk` 只 include `<soc>/makefile.mk`；全仓无任何文件 include `bsp/makefile.mk` |
| **G3** | 核心模块**直接 include `<soc>` 路径** | `src/nand/nand_fmc.c:31`、`src/nand/nand_mt29f4g08abadawp.c:26` → `platform/stm32h743/nand_dma.h`；`src/main.c:43-45` → `fault.h` / `platform_ops.h` / `stm32h7xx_hal.h` |
| **G4** | 核心模块**以裸名 include `<soc>` 头**（靠全局 `ALL_INCS` 才编译过） | `src/fc/fc_crc_engine.c:32` → `crc.h`；`src/nand/{nand_fmc,nand_engine,nand_dispatch,nand_mt29f4g08abadawp}.c` → `delay.h`；`include/nand/nand_mt29f4g08abadawp.h:5` → `sys.h`（连带把 HAL/CMSIS 灌进所有 includer） |
| **G5** | 纯逻辑服务被埋在 SoC 目录（**本身不违规**，但使「服务」与「驱动」难以分开复用） | `btim.h` 的 `btim_scheduler_*`；`watchdog.h` 的 `storage_wdg_*`；`usart.c` 的环形缓冲 |
| **G6** | 两个 target **不对称**，抽象层无法真正换 target | §2.3 缺失服务清单 |
| **G7** | **平台层反向依赖上层**（分层倒挂） | `src/platform/stm32h743/usart.c:6` → `ftl/ftl.h`；`include/platform/stm32h743/usart.h:6` → `controller/controller.h`；`nand_dma.{h,c}` → `nand/nand_config.h` |

> **本表是 Stage 0 基线。** Stage 1 的落地情况见 **§5.1**：G3 / G4 的 **include 部分已收敛**
> （`delay.h` / `crc.h` / `dma.h` 走抽象头，`platform/<soc>/` 路径只剩 `main.c` 与 1 处注释）；
> G7 的 **nand 那一半已断**（`CONFIG_NAND_DMA` 已搬进 `include/platform/dma.h`），
> `usart.c` 的两处倒挂未动。G2（`bsp/` 死代码）、G5、G6 未动。

---

## 四、目标布局（两层）+ 命名约定

```
include/platform/          ← 抽象接口：判据 B（零 HAL 类型）
    platform.h  interrupt.h
    delay.h  crc.h  dma.h  fault.h  usart.h
    btim.h  watchdog.h  mpu.h  rng.h  qspi.h  sys.h
include/platform/<soc>/    ← 实现头（含 HAL 类型），只有 <soc> 目录自己 include
src/platform/platform.c    ← 与 target 无关的注册/胶水
src/platform/<soc>/        ← 唯一实现目录（判据 A 的唯一合法出口）
```

**`dma.h` 是合并头**：一份 `include/platform/dma.h` 同时承载
① UART TX DMA（`uart_tx_dma_init` / `uart_tx_dma_irq_handler`，平台内部用）与
② NAND/FMC 数据搬移（原 `stm32h743/nand_dma.h` 整段，含 `CONFIG_NAND_DMA` 宏——
该宏已从 `include/nand/nand_config.h` 搬到本头，**反向依赖 G7 的 nand 那一半因此断掉**）。
两段签名都是裸类型 ⇒ 合并后仍满足判据 B。原 `stm32h743/dma.h`（含 HAL 类型
`DMA_HandleTypeDef`）与 `nand_dma.h` **已删除**。

**命名约定一句话**：抽象头放 `include/platform/`，实现头（含 HAL 类型）放
`include/platform/<soc>/`；核心模块只 include 前者。

---

## 五、分阶段路线图（本文件只写，不执行）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **Stage 0**（本次交付） | 本设计文档 + 订正失实文档（§六） | 文档结论可复算 |
| **Stage 1**（低风险、可编译验证） | ① 在 `include/platform/` 下为 `delay.h` / `crc.h` / `dma.h` 建抽象头（只搬声明，不搬实现；`nand_dma.h` 与 UART 的 `dma.h` 合并为一份 `dma.h`）；② `src/nand/*`、`src/fc/fc_crc_engine.c` 改 include 抽象头；③ 删 `src/platform/bsp/` 死代码（或接线）；④ 构建侧：核心模块的 `MODULE_INCS` 移除 `platform/<soc>` 目录 | 判据 A 的 grep 归零 + `make tests` 逐字不变 |
| **Stage 2**（需上板） | 把 `nand_mt29f4g08abadawp.c` 的 `HAL_NAND_MspInit` / `HAL_NAND_Init` / GPIO 与 `nand_fmc.c` 的 FMC MMIO 收进 `<soc>/`，NAND 层只留 `nand_device_t ops` 回调（`fc_nand_design.md §1` 已画出该 seam） | 上板全套通过 |
| **Stage 3**（真换 target 时才做） | `stm32h7r3` 补齐缺失服务，使判据 C 成立 | 判据 C 通过 |

### 5.1 Stage 1 进度（2026-10-11，只做 `stm32h743`）

- **① ② 已完成**：`include/platform/{delay,crc,dma}.h` 三个抽象头落地（零 HAL 类型），
  `include/platform/stm32h743/{delay,crc,dma,nand_dma}.h` 已删；`src/nand/*.c` 与
  `src/fc/fc_crc_engine.c` 的 include 全部走抽象头（裸名）。`src/platform/stm32h743/` 的
  `delay.c` / `dma.c` 补了各自的 `sys.h`（原本靠 `delay.h` / `dma.h` 传递 HAL 类型），
  `usart.c` 就地补 `extern DMA_HandleTypeDef g_dma_handle;`。
- **③ ④ 未做**：`src/nand/makefile.mk:4` 仍把 `$(INC_ROOT)/platform/stm32h743` 加进
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

---

## 六、不做（本设计边界）

- **不**引入 `src/platform/service/` 第三层——维持**两层**布局。
- **不**把运行期 vtable 作为主机制——`ssd_platform_ops_t` 保留为**已存在的小 seam**，不扩张。
- **不动** `src/ftl/**`。

---

## 七、附录：取证命令（结论可复算）

```bash
# 命令 A —— 判据 A：核心模块直连 <soc> 路径
#   Stage 0 基线 5 处（G3 的 4 处 include + fc_crc_engine.c:24 注释）；
#   Stage 1 后剩 3 处：fc_crc_engine.c:24（注释）+ main.c:43/44（板级装配根，本次不动）
grep -rn 'platform/stm32h743' src --include=*.c --include=*.h | grep -v '^src/platform/'

# 命令 A' —— 判据 A 的补充：裸名 include（靠全局 ALL_INCS 解析，需人工判断落到哪个目录）
#   Stage 1 后：delay.h / crc.h / dma.h 已稳定解析到 include/platform/（抽象头）；
#   仍解析到 <soc> 目录的只剩 G4 的 sys.h 与 main.c 的板级头：
#   include/nand/nand_mt29f4g08abadawp.h:5 的 sys.h（Stage 2）、
#   src/main.c 的 btim/mpu/sys/usart/watchdog（板级装配根，不在核心模块范围）
grep -rnE '#include "(delay|crc|sys|usart|btim|watchdog|mpu|rng|qspi|dma)\.h"' src include \
  --include=*.c --include=*.h | grep -vE '^src/platform/|^include/platform/'

# 命令 B —— 判据 B：抽象头零 HAL 类型（预期为空）
grep -nE 'HAL_|__HAL_|stm32h7xx|CMSIS|HandleTypeDef' include/platform/*.h

# 命令 C —— 判据 C：两 target 的实现对称性（对比文件清单）
#   预期：h743 独有 crc.h/dma.h/fault.h/nand_dma.h/platform_ops.h/qspi.h/rng.h
ls include/platform/stm32h743/*.h | xargs -n1 basename | sort > /tmp/h743.txt
ls include/platform/stm32h7r3/*.h  | xargs -n1 basename | sort > /tmp/h7r3.txt
diff /tmp/h743.txt /tmp/h7r3.txt

# 命令 D —— G2 复核：bsp/ 是否参与编译
#   预期：只看到 src/platform/bsp/ 自身的 makefile.mk，无任何 include 它的片段
grep -rn 'platform/bsp' --include=*.mk --include=makefile .
```

---

## 附：本次订正的失实文档

| 文件 | 原内容 | 订正 |
|---|---|---|
| `docs/architecture.md:12` | 「通过 `platform/soc.h` 约定的接口接入」 | 改为实际契约头 `platform/platform.h`，并注明 NAND 层尚未接入（本文件 §三 G3/G4） |
| `docs/hardware_contract.md:4-6` | 「Core SSD modules never access MMIO registers directly」 | 加现状注记：FTL/Controller/Common 成立，**NAND 层目前不成立** |
| `docs/boot_flow.md:6-7` | 引用 `soc_early_init()` / `soc_memory_init()` | 注明这两个入口尚未实现 |
