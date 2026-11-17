# STM32H743 Hardware Contract

This project targets the STM32H743 (Cortex-M7) MCU with an MT29F4G08 NAND
device, accessed through the FMC bank 3 interface. Core SSD modules never
access MMIO registers directly; register access lives in
`src/platform/stm32h743`.

**NAND 层当前例外**：FTL / Controller / Common 只经 `ssd_platform_ops_s` 访问平台；
NAND 层仍在 `src/nand/nand_fmc.c` 直接写 FMC 窗口，并在
`src/nand/nand_mt29f4g08abadawp.c` 直接定义 `HAL_NAND_MspInit` / `HAL_NAND_Init`。
平台服务分三层落地：抽象接口 `include/platform/<svc>.h`、共享逻辑
`src/platform/<svc>.c`、SoC 适配 `src/platform/stm32h743/*_port.c`；核心模块不再引用
`platform/<soc>/` 路径。分层审计与剩余收敛项见 `docs/design/platform_abstraction.md`。

## NAND device model

组织层级：Channel → Chip → Die → Plane → Block → Page。

MT29F4G08 真硬件约束：先擦后写、页编程次数受限、坏块与操作延迟（详见
`docs/design/fc_nand_design.md` 的时序与器件章节）。

## Memory layout (AXI_RAM)

| Region | Base | Size | Purpose |
| --- | ---: | ---: | --- |
| AXI_RAM | `0x24000000` | 384 KiB | FTL instances `g_ftl` (157.6 KiB), BST `g_blocks` (80 KiB), L2P cache；`.bss` 合计 255.5 KiB |
| AXI_NOCACHE | `0x24060000` | 128 KiB | FC 命令/描述符/缓冲池（`.noncacheable_buffer`, 66 KiB, 非缓存 DMA/NAND） |
| SRAM2 | `0x30020000` | 128 KiB | free / op / spare pool 与 closed SB list（`.sram2_data`） |
| DTCM | `0x20000000` | 128 KiB | stacks, hot data |

> 实测值取自 `build/firmware/debug/bin/debug.map`（2026-10-13）。`src/main.c:138` 有
> `_Static_assert` 挡住 AXI_RAM 超预算——**提高 CH/CE/LUN 维度前必须先重新规划内存**。

## USB device (USB MSC U-disk)

| Item | Value |
| --- | --- |
| Controller | `USB2_OTG_FS`, device mode, full speed 12 Mbps (no ULPI/HS) |
| Pins | `PA11` = OTG_FS_DM, `PA12` = OTG_FS_DP, AF10 |
| Kernel clock | HSI48 = 48 MHz, `RCC_PERIPHCLK_USB` in `sys_port_clock_init` |
| VBUS sensing | **disabled** (`CFG_TUD_VBUS_DETECT_HW = 0`) — PA9 is USART1_TX |
| IRQ | `OTG_FS_IRQn`, preempt priority 5 (lower than USART1=3) |
| Class | MSC / SCSI transparent / BOT, single LUN, 512 B block, 424 MiB host capacity（0 坏块；实际按 `[CAP]` 动态值） |
| Stack | TinyUSB 0.21.0 (MIT), pruned to device+MSC+DWC2 (see `third_party/tinyusb/README.soft_ssd.md`) |

> ⚠ Pin mapping is **not confirmed by schematic** (board: Apollo V2). If enumeration
> fails, check `PB14/PB15` (OTG_HS with internal FS PHY, AF12) before changing anything else.

## Build selection

```sh
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 debug
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 tests
```
