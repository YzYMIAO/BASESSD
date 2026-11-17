# 平台层设计（Platform Design）

> **目标**：核心模块只依赖通用接口；SoC / 板级差异全部落在 port 层。
> 换平台 = 新增 `src/platform/<soc>/` + 切换 `FIRMWARE_PLATFORM`，不改 FTL / FC / HIL 逻辑。
> 当前唯一 target：`stm32h743`。历史迁移与审计见
> [`../archive/platform_abstraction_migration.md`](../archive/platform_abstraction_migration.md)。

## 1. 分层与依赖方向

### 1.1 三层概念、两个目录

```
include/platform/<svc>.h          ← 抽象头：零 HAL 类型；公开 API + port 接口声明
src/platform/<svc>.c              ← 共享逻辑：跨 target 编译一次，零 SoC 头
src/platform/<soc>/<svc>_port.c   ← port 适配：只碰 HAL / MMIO
include/platform/<soc>/*_port.h   ← 实现头：允许 HAL 类型，只有 port 自己 include
```

依赖方向固定：**核心模块 → 抽象头**；port 由链接期提供实现。核心模块不 include
`platform/<soc>` 下的任何路径。

### 1.2 绑定机制

**同名 port 函数 + 链接期解析**：抽象头声明 `xxx_port_*()`，共享 `.c` 直接调用，
`src/platform/<soc>/<svc>_port.c` 提供唯一定义。不使用 vtable、不使用弱符号。

### 1.3 构建期选择 target

`FIRMWARE_PLATFORM`（默认 `stm32h743`）→ `src/platform/makefile.mk` 只 include 对应
`<soc>/makefile.mk` → 只编译该目录的 `.c`。函数签名固定、链接期解析、零间接调用开销。

核心模块的 `MODULE_INCS` **不得**包含 `platform/<soc>`；否则裸名 `#include "crc.h"` 会静默
落到 SoC 目录，形成不可见耦合。

## 2. 通用接口与服务映射

| 服务 | 抽象头 | 共享逻辑 | port | 主要消费者 | 状态 |
|---|---|---|---|---|---|
| 平台装配 | `platform.h` | `platform.c` | — | `main.c`、`log.c` | 启用 |
| 系统 | `sys.h` | `sys.c` | `sys_port.c` | 启动、Cache/MPU/VTOR | 启用 |
| 延时/时间戳 | `delay.h` | `delay.c` | `delay_port.c` | NAND、FC、日志、UT | 启用 |
| 定时调度器 | `timer.h` | `timer.c` | `timer_port.c` | watchdog、FTL 后台、观测 | 启用 |
| 看门狗 | `watchdog.h` | `watchdog.c` | `watchdog_port.c` | 长同步操作、CLI | 启用 |
| UART 传输 | `usart.h` | `usart.c` | `usart_port.c` | 日志 sink、CLI | 启用 |
| CRC | `crc.h` | `crc.c`（硬件后端选择） | `crc_port.c` | FC 数据面 | 启用（硬件；软件在 FC） |
| DMA | `dma.h` | `dma.c` | `dma_port.c` | UART TX（NAND 数据面当前 PIO） | 启用 |
| MPU | `mpu.h` | `mpu.c` | `mpu_port.c` | 启动内存保护 | 启用 |
| QSPI | `qspi.h` | `qspi.c` | `qspi_port.c` | — | 未启用（保留抽象） |
| RNG | `rng.h` | `rng.c` | `rng_port.c` | — | 未启用（UT 用软件 PRNG） |
| USB device | `usb.h` | —（抽象入口在 port 实现） | `usb_port.c` | HIL / TinyUSB | 启用 |
| 系统调用 | — | `syscalls.c` | —（target 无关） | newlib `_sbrk` / 堆栈守卫 | 启用 |

**例外与未启用**：

- NAND 层直接访问 FMC 与 HAL：`src/nand/nand_fmc.c` 写 FMC 窗口，
  `src/nand/devices/mt29f___g08a.c` 定义 `HAL_NAND_MspInit` / `HAL_NAND_Init`。
- QSPI / RNG 保留抽象，没有调用点；SDRAM 的 `.c` 未编入构建。
- `src/platform/stm32h743/bsp/` 是 SoC 内部目录，不属于核心模块可见接口。
- CRC 的软件实现放在 `src/fc/fc_crc_engine.c`（CRC-32/MPEG-2 与 ISO-HDLC）；平台层 `src/platform/crc.c` 只保留硬件后端选择。`PLATFORM_CRC_HW_SUPPORTED=1` 时 FC 优先调用 `crc_port.c` 的硬件实现，未定义/为 0 时使用 FC 软件实现。stm32h743 的 `makefile.mk` 默认置 1；仿真平台保持 0。两个后端必须在已知向量上 bit-exact。

## 3. Port 契约与判据

### 3.1 判据 A（方向）

核心模块不得 include `platform/<soc>/…` 路径，也不得以裸名解析到该目录的头。

```bash
grep -rn 'platform/stm32h743' src --include=*.c --include=*.h | grep -v '^src/platform/'

grep -rnE '#include "(delay|crc|sys|usart|timer|watchdog|mpu|rng|qspi|dma)\.h"' src include \
  --include=*.c --include=*.h | grep -vE '^src/platform/|^include/platform/'
```

### 3.2 判据 B（纯净）

`include/platform/*.h` 不得出现 `HAL_` / `__HAL_` / `HandleTypeDef` / `stm32h7xx` / CMSIS
符号（**含注释**，否则机械 grep 失效）。

```bash
grep -nE 'HAL_|__HAL_|stm32h7xx|CMSIS|HandleTypeDef' include/platform/*.h
```

### 3.3 判据 C（实现完整）

每个抽象头在当前 target 下都有实现；新增 target 时缺实现必须在
[`known_open_items.md`](known_open_items.md) 显式登记。

```bash
ls include/platform/*.h
ls src/platform/stm32h743/*_port.c
```

### 3.4 判据 D（共享源纯净）

`src/platform/*.c` 不得出现 HAL/CMSIS/SoC 头，也不得 include SoC 头（**含注释**）。
共享源连 CMSIS core 也不用：寄存器访问、`__get_MSP()` 等全部下沉为 port 函数。

```bash
grep -nE 'HAL_|__HAL_|HandleTypeDef|stm32h7xx|CMSIS|core_cm7|sys_port\.h|ftl/|controller/|nand/' src/platform/*.c
```

### 3.5 判据 E（port 有定义）

抽象头里每个 `*_port_*` 声明，在 `src/platform/<soc>/*_port.c` 里**恰好有一个**定义。

```bash
for f in $(grep -rhoE '[a-z_]+_port_[a-z_]+' include/platform/*.h | sort -u); do
  n=$(grep -rc "^[a-z_].*[ *]$f(" src/platform/*.c src/platform/stm32h743/*_port.c 2>/dev/null \
        | awk -F: '{s+=$2} END{print s+0}')
  printf '%-42s defs=%s\n' "$f" "$n"
done
```

### 3.6 签名、生命周期与状态

- **命名**：port 函数统一 `xxx_port_*`，只在一个抽象头里声明；`<soc>` 侧的 `*_port.h`
  不重复声明（`-Wredundant-decls` 会挡）。
- **类型**：抽象头与 port 签名只用裸类型（`BYTE/WORD/DWORD/QWORD`、`bool`、结构体指针），
  不出现 `*_HandleTypeDef`、HAL 枚举或 CMSIS 类型。
- **初始化顺序**：`sys_init()` 完成时钟/Cache/MPU；再初始化依赖时间的服务（timer/delay）；
  UART sink 可早绑定；USB 必须在 48 MHz 内核时钟就绪后初始化。
- **失败语义**：初始化失败返回 `bool` / `func_status_e` 并记录；不允许「返回 void 且静默失败」。
- **ISR 归属**：每个 ISR 定义在拥有该外设的 `*_port.c`；ISR 只做有界、可重入动作
  （置标志、递增计数、完成 DMA 握手），**禁止** `LOG*` / `ASSERT` / 阻塞等待 / FTL 事务。
- **DMA 与 Cache**：缓冲区按 32 字节对齐；CPU→设备 Clean，设备→CPU Invalidate。
  当前 NAND/FC 池在 `.noncacheable_buffer` 段。
- **静态状态**：每个 `static` 变量只落在一个 TU；HAL 句柄/寄存器影子 → port，
  任务表/环形缓冲/状态机 → 共享源。拆分时同一次改动内完成旧源文件与 port 的增删。

## 4. 边界与非目标

- 不引入运行期 vtable 作为主机制；`ssd_platform_ops_s` 只保留为已存在的小 seam，不扩张。
- 不增加 `src/platform/service/` 第三层；三层是概念划分，落地仍是两个目录。
- 抽象头只放声明与契约，不放实现、不放 HAL 类型。
- 共享源不 include SoC 头，不用 CMSIS core。
- port 不实现业务策略；策略留在 FC / FTL / HIL。
- 不为「看起来分层」拆一行转发函数；分层判据是接口与依赖方向。

## 附录 A：换平台步骤

1. 新建 `src/platform/<soc>/` 与 `makefile.mk`，在 `config.mk` 允许 `FIRMWARE_PLATFORM`。
2. 为每个抽象头实现 `*_port.c`；先让判据 C/E 通过，再接业务。
3. 提供启动文件、链接脚本、时钟/内存初始化、向量表；`sys_port.c` 完成 VTOR/Cache/MPU。
4. 实现 UART 传输与日志 sink；保证启动早期能出 banner。
5. 实现 timer/delay 时间源；确认 `timer_scheduler_tick` 与 `platform_now_ms` 一致。
6. 如启用 watchdog/USB/NAND，分别实现对应 port 并做板端 smoke。
7. 核心模块 `MODULE_INCS` 不加入 `platform/<soc>`；跑 §3 判据 A–E。
8. 跑构建三关 + 上板 smoke；在 `known_open_items.md` 登记未验证项。
