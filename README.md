# STM32H743 SSD Firmware

Build for the STM32H743 with the MT29F4G08 NAND device:

```sh
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=stm32h743 debug
```

Start from `docs/README.md` (document map). Layering is in `docs/architecture.md`,
the WSL↔JLink debug chain in `docs/guides/wsl_debug_environment.md`, and the
target assumptions in `docs/hardware_contract.md`.

## License

Project code is licensed under Apache-2.0 — see `LICENSE`.
Third-party components (TinyUSB, STM32H743 HAL/CMSIS, μnit) follow their own
upstream terms — see `THIRD_PARTY_NOTICES.md`.
