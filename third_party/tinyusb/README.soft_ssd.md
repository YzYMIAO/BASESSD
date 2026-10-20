# TinyUSB — 本仓裁剪说明

上游：<https://github.com/hathach/tinyusb>（MIT，见 `LICENSE`）
版本：`TUSB_VERSION = 0.21.0`（master 快照，2026-10-20）

## 保留了哪些文件

只保留 **USB device + MSC (BOT) + STM32H7 DWC2** 这条路径的实际编译依赖
（由 `build/*/obj/third_party/tinyusb/**/*.d` 的 `-MMD` 结果反推，不是估计）：

```text
src/tusb.c
src/tusb.h
src/tusb_option.h
src/common/tusb_common.h
src/common/tusb_compiler.h
src/common/tusb_debug.h
src/common/tusb_fifo.c
src/common/tusb_fifo.h
src/common/tusb_mcu.h
src/common/tusb_private.h
src/common/tusb_types.h
src/common/tusb_verify.h
src/device/dcd.h
src/device/usbd.c
src/device/usbd.h
src/device/usbd_pvt.h
src/osal/osal.h
src/osal/osal_none.h
src/class/msc/msc.h
src/class/msc/msc_device.c
src/class/msc/msc_device.h
src/portable/synopsys/dwc2/dcd_dwc2.c
src/portable/synopsys/dwc2/dwc2_common.c
src/portable/synopsys/dwc2/dwc2_common.h
src/portable/synopsys/dwc2/dwc2_stm32.h
src/portable/synopsys/dwc2/dwc2_type.h
```

## 裁掉了什么

`docs/`、`examples/`、`hw/`、`test/`、`tools/`、`lib/`、CI/IDE 配置，以及：

- **host 栈**（`src/host/`）与 Typec（`src/typec/`）；
- 其它 class driver（CDC/HID/Audio/Video/MIDI/DFU/Net/…）；
- 其它 MCU 端口（`src/portable/` 下除 Synopsys DWC2 STM32 端口外的全部）。

需要新增 class 或端口时，按同一版本从上游重新拷入对应文件，不要手改保留的核心文件。

## 适配边界（核心文件零修改）

本仓**没有修改** `third_party/tinyusb/src/` 下的任何文件。适配全部走上游约定的扩展点：

| 适配点 | 位置 |
|---|---|
| 编译期配置 | `include/tusb_config.h`（上游以裸名 `"tusb_config.h"` 包含） |
| 源文件/包含路径 | `third_party/tinyusb_vendor.mk` |
| SoC 时钟/引脚/IRQ/毫秒时基 | `src/platform/stm32h743/usb_port.c` |
| 描述符、MSC/SCSI 回调 | `src/controller/usb_msc.c`（实现上游 weak 回调） |

升级 TinyUSB 时只需替换上述文件集合并重跑构建；本仓的适配代码不进入上游源码树。
