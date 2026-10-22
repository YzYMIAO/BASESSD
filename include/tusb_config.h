/*
 * tusb_config.h — TinyUSB 编译期配置（本仓唯一一份）
 *
 * 本仓只使用 TinyUSB 的 **device + MSC (BOT)** 路径：
 *   - MCU 端口 = Synopsys DWC2（STM32H743 的 USB2_OTG_FS，全速 12 Mbps）；
 *   - 无 RTOS：TinyUSB 的 osal 走 OPT_OS_NONE，tud_task() 由主循环驱动；
 *   - 只有 MSC 一个 class driver，其它 class 全部关闭以减小体积。
 *
 * ⚠ VBUS sensing 必须关闭：本板 PA9 是 USART1_TX，而 PA9 同时是
 *    OTG_FS_VBUS。打开硬件 VBUS 检测会把引脚复用冲突变成“枚举偶发失败”。
 *    关闭后 DWC2 走 NOVBUSSENS，设备始终认为 VBUS 有效。
 *
 * ⚠ MSC 端点缓冲取 16384B：一次 SCSI READ/WRITE 在 USB 侧按该大小分块。
 *    4 KB → 16 KB 是为了摊薄「每块一次 FTL 异步完成往返」（BOT 单命令串行时
 *    每块都要等 async_io_done），不是总线带宽变化；16 KB 对应 32 个 512B LBA。
 *    该缓冲由 TinyUSB 放在 .bss（PIO 从 FIFO 搬运，无 DMA），无需非缓存段。
 */

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C"
{
#endif

#define CFG_TUSB_MCU OPT_MCU_STM32H7
#define CFG_TUSB_OS  OPT_OS_NONE

#define CFG_TUSB_DEBUG 0

/* RHPORT0 = USB2_OTG_FS；仅全速设备模式。 */
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MSC     1

#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_MSC_EP_BUFSIZE 16384

/* USB 事件 FIFO 深度（TinyUSB 默认 16）。
 *
 * 事件由 USB ISR 入队、由 tud_task() 在主循环出队。同步 FLUSH 在 SCSI 回调里
 * 自旋数十 ms，窗口内主循环不抽队列 ⇒ 16 会溢出。溢出后 TU_ASSERT 只在接了
 * 调试器时停机（TU_BREAKPOINT 读 DHCSR），**无调试器时静默丢事件**。
 * 64 是异步 FLUSH 落地前的余量，不是根治（RISK-USB-06）。 */
#define CFG_TUD_TASK_QUEUE_SZ 64

/* PA9 已用作 USART1_TX，不能同时做 OTG_FS_VBUS。 */
#define CFG_TUD_VBUS_DETECT_HW 0

/* DWC2 走 slave/PIO 模式，端点缓冲不需要 DMA 对齐；4B 对齐满足 FTL 的字访问。 */
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
