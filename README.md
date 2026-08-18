# Cortex-R5 SSD Firmware

## Host simulator

```sh
make BUILD_TARGET=host_sim all
```

## Reference Cortex-R5 firmware

```sh
make BUILD_TARGET=firmware FIRMWARE_PLATFORM=reference_soc all
```

The reference platform defines a virtual hardware contract. It does not use
register addresses from an undocumented SSD controller SoC. See
`docs/hardware_contract.md` for the current assumptions.
