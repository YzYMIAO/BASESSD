# STM32H743 Hardware Contract

This project targets the STM32H743 (Cortex-M7) MCU with an MT29F4G08 NAND
device, accessed through the FMC bank 3 interface. Core SSD modules never
access MMIO registers directly; register access lives in
`src/platform/stm32h743` and `src/platform/bsp`.

> **Status note (2026-10-11).** The rule above holds for FTL / Controller / Common
> (they use `ssd_platform_ops_t` only), but **not yet for the NAND layer**:
> `src/nand/nand_fmc.c` writes FMC windows via raw MMIO and
> `src/nand/nand_mt29f4g08abadawp.c` defines `HAL_NAND_MspInit` / `HAL_NAND_Init`
> directly. The NAND layer's **include coupling** did converge in Stage 1 — `delay` /
> `crc` / `dma` now come from the abstract headers under `include/platform/`, and no
> `platform/<soc>/` path remains in `src/nand/*.c` or `src/fc/fc_crc_engine.c`. **Stage 2**
> then moved the portable logic of five platform services (`btim` / `watchdog` / `fault` /
> `delay` / `syscalls`) into shared sources under `src/platform/*.c`, leaving only the
> hardware adaptation in `src/platform/stm32h743/*_port.c`; the
> raw MMIO and `HAL_NAND_*` usage above is what remains for Stage 3.
> Also note `src/platform/bsp` is currently **dead code** (its
> `makefile.mk` is not included by any build fragment). See
> `docs/design/platform_abstraction.md` for the full audit and staged plan.

## Memory layout (AXI_RAM)

| Region | Base | Size | Purpose |
| --- | ---: | ---: | --- |
| AXI_RAM | `0x24000000` | 384 KiB | FTL instances, BST `g_blocks`, L2P cache |
| SRAM2 | `0x30020000` | 128 KiB | free pool and closed SB list |
| DTCM | `0x20000000` | 128 KiB | stacks, hot data |

## Build selection

```sh
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 debug
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 tests
```
