# v33：CRC 引擎改用 STM32H743 HAL 驱动（修「基址写成 ADC3」导致的静默失效）

- **日期**：2026-10-10
- **范围**：`src/fc/fc_crc_engine.{h,c}`、`src/platform/stm32h743/crc.{h,c}`（**新增**）、
  `third_party/stm32h743_vendor.mk`、`third_party/stm32h743/hal/inc/stm32h7xx_hal_conf.h`、文档
- **前置**：Phase L（CRC-32 引擎实现 + 数据路径集成）、v24（加扰范围 `[8,520)`，META/CRC 明文）
- **介质格式**：**变更**——此前写入的所有 CW 里 CRC 字段恒为 `0`，改后是真值。
  既有数据仍在，但 CRC 字段不可信；FTL 端到端须重跑一遍确认。
- **测试**：`make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  链接 **212180 B**；`make`（固件）0 warning。**上板已验**：`data_pathcrc_step_detect`
  4 个 case 全过（`user-1` / `crc-field-1` 必检出、`meta-1` / `parity-1` 必不检出），
  全套 FC UT 0 失败。

## 根因：`FC_CRC_BASE` 写的是 ADC3 的地址

`src/fc/fc_crc_engine.c` 里：

```c
#define FC_CRC_BASE (0x58026000UL)   /* ← 这是 ADC3，不是 CRC */
```

STM32H743 上 `D3_AHB1PERIPH_BASE = 0x40000000 + 0x18020000 = 0x58020000`，
而 `CRC_BASE = D3_AHB1PERIPH_BASE + 0x4C00 = 0x58024C00`。三处独立佐证：
本项目 `third_party/stm32h743/cmsis-device/stm32h743xx.h`、RM0433 §2.3.2、libopencm3。

**后果链**：IDR 写读回环打在 ADC3 上，读回 ≠ `0xA5` ⇒ `g_crc_ready = 0`
（设计上「失败静默继续」）⇒ `fc_crc_compute()` 恒返 `0` ⇒ `crc_write` 存 `0`、
`crc_check` 比 `0 == 0` ⇒ **CRC 保护自始从未生效，校验恒通过**，只剩 ECC 兜底。

**为什么长期没人发现**：整套 UT 里唯一能看见它的断言是 `test_fc_level2.c` 的
`crc_step_detect/user-1` 负例，而那条是 v30 才加的；其余 CRC 断言全是恒真
（`stored == fc_crc_compute(plain)` 是同函数算同段数据；`process_read_crc_error`
用 `0xDEADBEEF` 哨兵，与 `0` 天然不等）。

## 修法：不是改常量，而是整体改用 HAL 驱动

让地址由编译器（CMSIS 设备头的 `CRC` 宏）保证，同类笔误不可能再出现。
分层：**平台层持有外设，FC 层只留语义**。

| 文件 | 改动 |
|---|---|
| `include/platform/stm32h743/crc.h`（新） | 只暴露 `crc_init()` / `crc_compute(data, len_bytes)`。**刻意不 include `"sys.h"`**——唯一使用者 `src/fc/fc_crc_engine.c` 要保持零 HAL 依赖，故本头不泄漏任何 HAL 类型 |
| `src/platform/stm32h743/crc.c`（新） | 持有 `static CRC_HandleTypeDef g_crc_handle` + `HAL_CRC_MspInit`（`__HAL_RCC_CRC_CLK_ENABLE()`）+ IDR 写读回环自检 |
| `src/fc/fc_crc_engine.c` | 删 `FC_CRC_BASE` / `FC_CRC_REG` / 五个 offset 宏 / `FC_CRC_CR_RESET` / `g_crc_ready` / `0x580244E0UL`（RCC 裸地址）全部裸地址；`fc_crc_*` 只剩「对哪 512B 算、结果怎么判」 |
| `third_party/stm32h743_vendor.mk` | `STM32H743_SRCS` 加 `stm32h7xx_hal_crc.c` **和** `stm32h7xx_hal_crc_ex.c` |
| `third_party/stm32h743/hal/inc/stm32h7xx_hal_conf.h` | 取消注释 `#define HAL_CRC_MODULE_ENABLED` |

`src/fc/` 保持**零 HAL 依赖**：`fc_crc_init()` 是 FC 模块对外的初始化契约，
`src/fc/fc.c` 的调用点不必 include 平台头。

## 两个坑（都已处理，别再踩）

1. **`_ex.c` 必须一起加**——`HAL_CRC_Init` **无条件**调用 `HAL_CRCEx_Polynomial_Set`
   （`stm32h7xx_hal_crc.c:136`），`--gc-sections` 裁不掉，少加就 `undefined reference`。
2. **`ASSERT(HAL_CRC_Init(...) == HAL_OK)` 会把初始化整个裁掉**——`ASSERT` 在 release
   （`NDEBUG`）下展开为 `((void)0)`，表达式不求值。必须先 `status = HAL_CRC_Init(...)`
   再 `ASSERT(status == HAL_OK)`。（这是本项目 v18~v21 判据的既有性质，非本次引入。）

## 对齐

HAL 的 `WORDS` 分支直接按 `uint32_t *` 解引用，未对齐是硬伤。实测三个调用点全部满足：

- `g_fc_buffer_pool` 落在 `0x24060000`（段首 `ALIGN(32)`，步长 528 = 4×132）⇒ `cw+8` 恒 4 对齐；
- 测试夹具两块 `malloc` 缓冲（8 对齐，步长 512 / 528）同样满足。

MPU region 11 把该区设为 `TEX1 + C0 + B0`（Normal 非缓存），即便未对齐也不 fault。

## 体积

CRC 相关符号共 ~1.3 KB `.text`（`CRC_Handle_8` 288B / `HAL_CRCEx_Polynomial_Set` 284B /
`HAL_CRC_Init` 200B / `HAL_CRC_Calculate` 152B / `crc_init` 144B / `CRC_Handle_16` 116B /
`crc_compute` 108B / `HAL_CRC_MspInit` 56B）+ 36B `.bss`。`HAL_CRC_Accumulate` / `DeInit` /
`GetState` 零调用者，被 `--gc-sections` 裁掉。FLASH 2 MB、固件 ~210 KB，余量充足。

## 副作用

- `fc_crc_verify()` 全仓**零调用者**（只有定义 + 头声明），已被 `--gc-sections` 自动丢弃
  （`nm` 里查不到）。是否删源码待定。
- 正点原子阿波罗 V2 的 66 个标准例程 + 5 个扩展包**没有任何 CRC 例程**；其 Keil 工程
  `atk_h743.uvprojx` 里 `crc` 出现 **0 次**——`HAL_CRC_MODULE_ENABLED` 开着但
  `stm32h7xx_hal_crc.c` 从没被编进去过。这件事**没有现成样板可抄**。

## 上板

```
ssd> test /fc-nand/data_pathcrc_step_detect   # 4 case：user-1 / crc-field-1 必检出；
                                              #          meta-1 / parity-1 必不检出
ssd> test /fc-nand/                           # 全套 FC UT（CRC 首度生效，全量回归）
```

因属**介质行为变更**，另跑一轮 FTL 端到端：
`info → wr 0 2 → rd 0 2 c → tr 0 → fl → info`。
