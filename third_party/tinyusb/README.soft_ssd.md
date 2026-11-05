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

## 适配边界

适配优先走上游约定的扩展点：

| 适配点 | 位置 |
|---|---|
| 编译期配置 | `include/tusb_config.h`（上游以裸名 `"tusb_config.h"` 包含） |
| 源文件/包含路径 | `third_party/tinyusb_vendor.mk` |
| SoC 时钟/引脚/IRQ/毫秒时基 | `src/platform/stm32h743/usb_port.c` |
| 描述符、MSC/SCSI 回调 | `src/hil/usb_msc.c`（实现上游 weak 回调） |

## 本仓补丁（**唯一的 src/ 修改**，RISK-USB-06 / RISK-USB-11）

本仓在 `src/class/msc/` 下的修改共 **6 组**，全部带 `【本仓补丁】` 注释：

| # | 位置（函数 / 文件） | 修改 | 用途 |
|---|---|---|---|
| 1 | `tud_msc_async_io_done` | 仅 READ_10/WRITE_10 把 `bytes_io == 0` 当错误；无数据命令允许 0 = 成功 | FLUSH 异步完成 |
| 2 | `proc_async_io_done` | `default` 分支：非 READ/WRITE 命令完成 ⇒ 进入 `MSC_STAGE_STATUS`（失败则 `fail_scsi_op`） | FLUSH / UNMAP 异步完成 |
| 3 | `mscd_xfer_cb`（SCSI CMD 阶段） | `resplen == TUD_MSC_RET_ASYNC && total_bytes == 0` ⇒ 置 `pending_io` 并返回，等 `tud_msc_async_io_done` | FLUSH 异步提交 |
| 4 | `tud_msc_cbw_info_cb`（`msc_device.c` 声明/weak 实现/READ/WRITE 命令起点调用；`msc_device.h` 声明） | 命令起点把 CBW 的总 block_count / 起始 LBA / FUA 位交给应用 | FTL cache 序列 `cmd_remaining` 的唯一来源（RISK-USB-11）+ FUA（RISK-USB-05 改判） |
| 5 | `mscd_xfer_cb`（OUT data 阶段） | 回调返回 `TUD_MSC_RET_ASYNC` ⇒ 置 `pending_io` 并返回，等 `tud_msc_async_io_done` | UNMAP（带 data-out 的 SCSI 命令）异步完成 |
| 6 | `proc_async_io_done` default / `proc_read_io_data` ERROR / `proc_write_io_data` ERROR | 仅当 `sense_key == 0` 才用默认 NOT READY 覆盖 | 保留应用设置的 deferred MEDIUM ERROR sense |

上游 MSC 的异步完成只覆盖 READ_10 / WRITE_10；无数据 SCSI 命令（`SYNCHRONIZE
CACHE(10)`）的 `TUD_MSC_RET_ASYNC` 会被当成失败。本仓的 FLUSH 必须异步（否则
host 回调里自旋会饿死 USB 事件队列），故有 #1~#3。

升级 TinyUSB 时必须：

1. `grep -rn "【本仓补丁】" third_party/tinyusb/src` 对照本表**全部 6 组**检查是否已重套；
2. 回归「H2testw 跑到结束不 halt / `[FTL] flush took` 正常」；
3. 回归「连续大顺序写跨 chunk」：`tud_msc_cbw_info_cb` 缺失会让 `cmd_remaining` 恒 0，
   序列提前收尾（功能不坏，吞吐/序列行为退化，编译期完全无声）；
4. 回归 UNMAP（`blkdiscard`/`fstrim`）、FUA 写（回执延迟/断电读回）与 deferred write error 的 sense key。

其余适配代码不进入上游源码树。
