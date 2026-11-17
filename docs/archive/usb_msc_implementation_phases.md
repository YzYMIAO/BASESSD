# USB MSC 实施阶段记录（历史）

> 从 `docs/design/usb_msc.md` §A.7 迁入（2026-11-17）；正文不改写。
> 当前设计见 [`../design/usb_msc.md`](../design/usb_msc.md)。

### A.7 实施阶段

#### Phase 0：确认硬件接线（**✅ rev30 已确认**）

- 查核心板原理图/万用表确认 Type-C 的 D+/D- 接的是 PA11/PA12 还是 PB14/PB15。
- 本文先按 PA11/PA12 实现；枚举失败再改 PB14/PB15（OTG_HS 内嵌 FS PHY，AF12）。
- 确认 VBUS sensing 必须 disable（PA9 是 USART1_TX）——已通过
  `CFG_TUD_VBUS_DETECT_HW = 0` 落实。

#### Phase 1：USB 传输 bring-up（**✅ rev30 已上板**）

- 引入 `third_party/tinyusb`（MIT），使用 DWC2 port。
- `tusb_config.h`：`CFG_TUD_MSC=1`、`CFG_TUSB_OS=OPT_OS_NONE`。
- `src/platform/stm32h743/sys_port.c`：`RCC_PERIPHCLK_USB` +
  `RCC_USBCLKSOURCE_HSI48`。
- 新增 `src/platform/stm32h743/usb_port.c`：
  - `tusb_init()`；
  - PA11/PA12 AF10（或 PB14/15 AF12）；
  - `OTG_FS_IRQn` / `OTG_FS_IRQHandler`；
  - VBUS sensing disable。
- 先只枚举出设备（不接存储），确认 PC 能识别。

#### Phase 2：MSC class + FTL adapter（**✅ rev30 已上板**）

- 新增 `src/hil/usb_msc.c` / `include/hil/usb_msc.h`：
  - `tud_msc_read10_cb` / `tud_msc_write10_cb`：登记到 `ssd_req_s`，
    提交后返回 `TUD_MSC_RET_ASYNC`；
  - `usb_msc_task()`：完成时回填 READ 数据并 `tud_msc_async_io_done()`；
  - `tud_msc_capacity_cb`：424 MiB / 512B block / `FTL_LBA_MAX`；
  - `tud_msc_inquiry_cb` / `tud_msc_start_stop_cb` / `tud_msc_test_unit_ready_cb`；
  - SCSI SYNCHRONIZE CACHE → `SSD_REQUEST_FLUSH`（`ftl_flush`，仅主循环上下文）；
  - SCSI UNMAP → 解析参数表，逐 range 提交 `SSD_REQUEST_TRIM`；全部完成才回 CSW；
  - `tud_event_hook_cb`：总线复位/拔出时把在飞请求标记 `discard`。
- 0/1 是 16 KiB bounce 数据槽；2 号是无 bounce 控制槽（FLUSH/UNMAP）。

#### Phase 3：主循环与仲裁（**✅ rev30 已上板**）

- `src/main.c` 主循环在 `ftl_service()` 之后调用 `tud_task()` / `usb_msc_task()`。
- CLI 与 USB 共用 host busy 闸门。
- 所有 `ftl_flush` / `ftl_trim` 只在主循环调用。

#### Phase 4：主机侧验证（**✅ 基础项已通过；掉电/吞吐/多分区待测**）

- Windows：设备管理器出现“USB 大容量存储设备”；磁盘管理显示 424 MiB；
  格式化 FAT32/exFAT；复制/读回文件。
- Linux：`lsusb`、`dmesg`、`lsblk`/`fdisk -l`；`dd` 读写；`sync`；`fsck`。
- 掉电：USB 写过程中断电，重启后 FTL 恢复，盘可重新挂载。
- 性能：记录 `dd` 吞吐，和 NAND/FTL 上限对照。

---

