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
 * ⚠ MSC 端点缓冲取 4096B：一次 SCSI READ/WRITE 在 USB 侧按该大小分块，
 *    每块对应 8 个 512B LBA；块越大，主循环里 tud_task() 的切换次数越少。
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
#define CFG_TUD_MSC_EP_BUFSIZE 4096

/* PA9 已用作 USART1_TX，不能同时做 OTG_FS_VBUS。 */
#define CFG_TUD_VBUS_DETECT_HW 0

/* DWC2 走 slave/PIO 模式，端点缓冲不需要 DMA 对齐；4B 对齐满足 FTL 的字访问。 */
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
