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
> hardware adaptation in `src/platform/stm32h743/*_port.c`. **Stage 2.5** split `usart` the
> same way (transport → `src/platform/usart.c` + `usart_port.c`, CLI → `src/controller/cli.c`),
> which also removed the platform layer's reverse dependency on FTL. **Stage 2.6** gave the
> remaining seven services the same treatment (`platform_ops` / `crc` / `dma` / `mpu` /
> `qspi` / `rng` / `sys` → shared `src/platform/<svc>.c` + `stm32h743/<svc>_port.c`), which
> **zeroed the include coupling** — no core module names a `platform/<soc>/` path any more
> (`src/main.c` now includes the abstract `"platform.h"`). The HAL umbrella header
> `include/platform/stm32h743/sys.h` was renamed to `sys_port.h` so the abstract `sys.h`
> can be HAL-free. The raw MMIO and `HAL_NAND_*` usage above is what remains for Stage 3.
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
