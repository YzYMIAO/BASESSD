#ifndef SSD_USB_MSC_H
#define SSD_USB_MSC_H

#include "ssd_types.h" /* BOOL */

#include <stdbool.h>
#include <stdint.h>

/*
 * USB MSC（Mass Storage Class, Bulk-Only Transport）块设备前端。
 *
 * 把 PC 的 SCSI READ(10)/WRITE(10) 映射到 controller 的 512B LBA 读写：
 *   1 LBA = 1 个 FTL 映射单元（FTL_IU_BYTES = 512B）；
 *   容量 = controller 的运行时 host_lba_max（≤ FTL_LBA_MAX，按实际坏块扣减）。
 * 另支持 SYNCHRONIZE CACHE(10/16) → FLUSH、UNMAP(0x42) → 逐 range TRIM、
 * WRITE(10) FUA（完成前不回执）。
 *
 * 协议栈（枚举、BOT 状态机、SCSI 子集）在 third_party/tinyusb；本模块只实现
 * class 回调与 controller 适配。回调运行在 usb_device_task()（主循环）上下文，
 * 因此采用 TinyUSB 的异步模型：回调提交请求后返回 TUD_MSC_RET_ASYNC，
 * 由 usb_msc_task() 在后续主循环里检查完成并调用 tud_msc_async_io_done()。
 * 回调内不自旋、也不用 TUD_MSC_RET_BUSY（后者会在 tud_task 内部立即重入，
 * 单线程主循环下会把 FTL 服务饿死）。
 */

/* SCSI logical block size（USB MSC sector）。与 FTL 的映射单元 512B 必须保持 1:1，
 * 由 usb_msc.c 的 _Static_assert 锁住。 */
#define USB_MSC_BLOCK_SIZE 512U

/**
 * @brief 清零在飞请求状态。
 *
 * 在 `usb_device_init()` 之前调用一次；host queue 由 `ssd_controller_init` 初始化。
 */
void usb_msc_init(void);

/**
 * @brief 在主循环调用（`usb_device_task()` 之前）：推进在飞的 MSC 请求。
 *
 * 两件事：① 请求因 CLI 占用 **host busy gate（本仓自拟：同一时刻只允许一个
 *         host 前端提交 controller）** 而排队时，gate 释放后提交；
 *         ② FTL 完成后把数据/状态交回 TinyUSB（tud_msc_async_io_done）。
 * 非阻塞，无在飞请求时立即返回。
 */
void usb_msc_task(void);

/**
 * @brief 打印 WRITE(10) 数据相分块直方图（诊断）。
 *
 * 只在 `stat` 命令里调用：运行期零输出。桶 = TinyUSB 每次回调的数据相长度
 * （≤ CFG_TUD_MSC_EP_BUFSIZE，当前 16 KB）。<=512B 占多数说明主机按单扇区
 * 下发；<=4K 说明仍是 4 KB 块；<=16K 说明 16 KB 端点缓冲生效。
 */
void usb_msc_io_stats_print(void);

#endif /* SSD_USB_MSC_H */
