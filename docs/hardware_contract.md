# Reference Cortex-R5 Hardware Contract

This project does not target a publicly documented SSD controller SoC.  The
firmware therefore separates architecture code from the replaceable SoC BSP.

## Stable architecture assumptions

- ARMv7-R Cortex-R5, single core, little-endian.
- ARM exception vectors and separate IRQ, FIQ, abort, undefined and SVC stacks.
- Freestanding C11 firmware built with `arm-none-eabi-gcc`.
- Core SSD modules never access MMIO registers directly.

## Reference virtual SoC

The `reference_soc` platform is a software contract, not a real chip:

| Region | Base | Size | Purpose |
| --- | ---: | ---: | --- |
| Boot memory | `0x00000000` | 2 MiB | vectors, code, constants |
| DTCM | `0x20000000` | 64 KiB | processor mode stacks |
| SRAM | `0x21000000` | 512 KiB | initialized data and BSS |
| DRAM | `0x40000000` | 256 MiB | FTL tables and host buffers |

No physical UART, timer, interrupt controller, PCIe, NAND or DMA register
addresses are invented.  Their interfaces are placeholders for a future BSP.

## Boot assumption

The first firmware stage assumes a debugger, ROM model or simulator has loaded
the ELF image and initialized executable memory. NAND boot and DDR PHY training
remain outside the generic ARMv7-R startup path.

## Build selection

```sh
make BUILD_TARGET=host_sim all
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=reference_soc all
```
