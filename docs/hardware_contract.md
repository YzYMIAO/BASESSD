# STM32H743 Hardware Contract

This project targets the STM32H743 (Cortex-M7) MCU with an MT29F4G08 NAND
device, accessed through the FMC bank 3 interface. Core SSD modules never
access MMIO registers directly; register access lives in
`src/platform/stm32h743` and `src/platform/bsp`.

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
