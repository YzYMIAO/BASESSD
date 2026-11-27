#ifndef PLATFORM_USB_H
#define PLATFORM_USB_H

#include "ssd_types.h" /* BOOL */

#include <stdbool.h>

/*
 * USB device 控制器平台抽象（SoC 适配在 src/platform/<soc>/usb_port.c）。
 *
 * 本头零 HAL/TinyUSB 类型：上层只关心「初始化控制器」「在主循环推进协议栈」。
 * 当前只支持设备模式 + 全速；host 模式、高速 PHY 不在本仓范围内。
 */

/*
 * 初始化 USB device 控制器并使能协议栈。
 *
 * 必须在系统时钟（含 USB 48 MHz 内核时钟）配置完成后调用；只允许调用一次。
 * 返回 false 表示控制器初始化失败，此后 usb_device_task() 仍可安全调用
 * （TinyUSB 未初始化时直接返回）。
 */
BOOL usb_device_init(void);

/*
 * 在主循环周期调用，处理 TinyUSB 设备事件。
 *
 * ⚠ 必须是非 ISR 上下文；本函数不阻塞（事件队列空时立即返回）。
 * ⚠ MSC class 的 READ/WRITE 回调在这里执行，采用 TUD_MSC_RET_ASYNC：
 *   回调只登记请求，完成与数据交付由 usb_msc_task() 在主循环推进。
 */
void usb_device_task(void);

#endif /* PLATFORM_USB_H */
