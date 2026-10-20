# STM32H743 Hardware Contract

This project targets the STM32H743 (Cortex-M7) MCU with an MT29F4G08 NAND
device, accessed through the FMC bank 3 interface. Core SSD modules never
access MMIO registers directly; register access lives in
`src/platform/stm32h743`.

> **Status note (2026-10-11).** The rule above holds for FTL / Controller / Common
> (they use `ssd_platform_ops_t` only), but **not yet for the NAND layer**:
> `src/nand/nand_fmc.c` writes FMC windows via raw MMIO and
> `src/nand/nand_mt29f4g08abadawp.c` defines `HAL_NAND_MspInit` / `HAL_NAND_Init`
> directly. The NAND layer's **include coupling** did converge in Stage 1 — `delay` /
> `crc` / `dma` now come from the abstract headers under `include/platform/`, and no
> `platform/<soc>/` path remains in `src/nand/*.c` or `src/fc/fc_crc_engine.c`. **Stage 2**
> then moved the portable logic of five platform services (`btim` / `watchdog` / `fault` /
> `delay` / `syscalls`) into shared sources under `src/platform/*.c`, leaving only the
> hardware adaptation in `src/platform/stm32h743/*_port.c`. **Stage 2.5** split `usart` the
> same way (transport → `src/platform/usart.c` + `usart_port.c`, CLI → `src/controller/cli.c`),
> which also removed the platform layer's reverse dependency on FTL. **Stage 2.6** gave the
> remaining seven services the same treatment (`platform_ops` / `crc` / `dma` / `mpu` /
> `qspi` / `rng` / `sys` → shared `src/platform/<svc>.c` + `stm32h743/<svc>_port.c`), which
> **zeroed the include coupling** — no core module names a `platform/<soc>/` path any more
> (`src/main.c` now includes the abstract `"platform.h"`). The HAL umbrella header
> `include/platform/stm32h743/sys.h` was renamed to `sys_port.h` so the abstract `sys.h`
> can be HAL-free. The raw MMIO and `HAL_NAND_*` usage above is what remains for Stage 3.
> Also note `src/platform/bsp` — long registered as **dead code** (its
> `makefile.mk` was never included by any build fragment) — was **deleted** on
> 2026-10-12. See `docs/design/platform_abstraction.md` for the full audit and
> staged plan.

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
| Class | MSC / SCSI transparent / BOT, single LUN, 512 B block, 429 MiB host capacity |
| Stack | TinyUSB 0.21.0 (MIT), pruned to device+MSC+DWC2 (see `third_party/tinyusb/README.soft_ssd.md`) |

> ⚠ Pin mapping is **not confirmed by schematic** (board: Apollo V2). If enumeration
> fails, check `PB14/PB15` (OTG_HS with internal FS PHY, AF12) before changing anything else.

## Build selection

```sh
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 debug
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 tests
```
