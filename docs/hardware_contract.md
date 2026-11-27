# STM32H743 Hardware Contract

This project targets the STM32H743 (Cortex-M7) MCU with an MT29F4G08 NAND
device, accessed through the FMC bank 3 interface. Core SSD modules never
access MMIO registers directly; register access lives in
`src/platform/soc/stm32h743`.

**NAND 层当前例外**：FTL / Controller / Common 只经 `ssd_platform_ops_s` 访问平台；
NAND 层仍在 `src/nand/nand_fmc.c` 直接写 FMC 窗口，并在 `src/nand/nand_board.c`
定义 `HAL_NAND_MspInit`（板级 FMC / GPIO 接线）；器件模型
`src/nand/devices/mt29f___g08a.c` 只调用 HAL 库的 `HAL_NAND_Init`。
平台服务分三层落地：抽象接口 `include/platform/<svc>.h`、共享逻辑
`src/platform/<svc>.c`、SoC 适配 `src/platform/soc/stm32h743/*_port.c`；核心模块不再引用
`platform/<soc>/` 路径。分层审计与剩余收敛项见 `docs/design/platform_design.md`。

## Board documentation (Apollo V2)

- 官方资料入口：<https://www.openedv.com/docs/boards/stm32/zdyz_stm32h743_apolloV2.html>
- 该页是**资料索引**；硬件参考手册、原理图、器件数据手册在页面所列「开发板资料 A 盘」中。
- 本文的板级资源（NAND / SDRAM / USB / 串口 / 调试）以该 A 盘资料为准。
- USB 引脚映射已由原理图 **2.2.10.1** 确认：Type-C 的 D-/D+ 经 22R 接
  `PA11/PA12`（OTG_FS），SBU1/SBU2 未接，无 ULPI PHY。

## NAND device model

组织层级：Channel → Chip → Die → Plane → Block → Page。

MT29F4G08 真硬件约束：先擦后写、页编程次数受限、坏块与操作延迟（详见
`docs/design/fc_nand_design.md` 的 §3.14（时序）与 §3.10~§3.12（器件与命令表））。

## Memory layout (AXI_RAM)

| Region | Base | Size | Purpose |
| --- | ---: | ---: | --- |
| AXI_RAM | `0x24000000` | 384 KiB | FTL instances `g_ftl` (157.6 KiB), BST `g_blocks` (80 KiB), L2P cache；`.bss` 合计 255.5 KiB |
| AXI_NOCACHE | `0x24060000` | 128 KiB | FC 命令/描述符/缓冲池（`.noncacheable_buffer`, 66 KiB, 非缓存 DMA/NAND） |
| SRAM2 | `0x30020000` | 128 KiB | free / op / spare pool 与 closed SB list（`.sram2_data`） |
| DTCM | `0x20000000` | 128 KiB | stacks, hot data |

> 实测值取自 `build/debug/bin/debug.map`（2026-10-13）。`src/main.c` 有
> `_Static_assert` 挡住 AXI_RAM 超预算——**提高 CH/CE/LUN 维度前必须先重新规划内存**。

## USB device (USB MSC U-disk)

| Item | Value |
| --- | --- |
| Controller | `USB2_OTG_FS`, device mode, full speed 12 Mbps (no ULPI/HS) |
| Pins | `PA11` = OTG_FS_DM, `PA12` = OTG_FS_DP, AF10（Type-C D-/D+ 经 22R；原理图 2.2.10.1） |
| Kernel clock | HSI48 = 48 MHz, `RCC_PERIPHCLK_USB` in `sys_port_clock_init` |
| VBUS sensing | **disabled** (`CFG_TUD_VBUS_DETECT_HW = 0`) — PA9 is USART1_TX |
| IRQ | `OTG_FS_IRQn`, preempt priority 5 (lower than USART1=3) |
| Class | MSC / SCSI transparent / BOT, single LUN, 512 B block, 424 MiB host capacity（0 坏块；实际按 `[CAP]` 动态值） |
| Stack | TinyUSB 0.21.0 (MIT), pruned to device+MSC+DWC2 (see `third_party/tinyusb/README.basessd.md`) |

> Pin mapping confirmed by schematic 2.2.10.1: Type-C D-/D+ → `PA11/PA12`,
> SBU1/SBU2 not connected, **no ULPI PHY** ⇒ only USB 2.0 Full Speed 12 Mbps.
> Do not switch to `OPT_MODE_HIGH_SPEED`; the board has no HS path.

## Build selection

```sh
make FIRMWARE_PLATFORM=stm32h743 debug
make FIRMWARE_PLATFORM=stm32h743 tests
```
