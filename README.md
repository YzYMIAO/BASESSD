# STM32H743 SSD Firmware

Build for the STM32H743 with the MT29F4G08 NAND device:

```sh
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 debug
```

See `docs/design/wsl_debug_environment.md` for the WSL↔JLink debug chain and
`docs/hardware_contract.md` for the target assumptions.
