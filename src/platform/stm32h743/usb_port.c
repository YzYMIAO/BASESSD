/*
 * usb_port.c — stm32h743 USB device 控制器适配层
 *
 * 只做 SoC 相关的事：USB2_OTG_FS 外设时钟、PA11/PA12 复用（AF10）、
 * NVIC 优先级、TinyUSB 端口初始化，以及向量表入口。
 * 协议栈与 MSC class 全部在 third_party/tinyusb；块设备映射在 src/hil/usb_msc.c。
 *
 * 关键约束：
 *   - USB 内核时钟由 sys_port_clock_init 的 RCC_PERIPHCLK_USB 统一选择
 *     （HSI48 = 48 MHz）；本文件只初始化控制器，不改时钟树。
 *   - VBUS sensing 关闭（include/tusb_config.h）：PA9 同时是 USART1_TX。
 *   - DWC2 用 slave/PIO 模式（CFG_TUD_DWC2_DMA_ENABLE=0），不碰 D-Cache；
 *     端点 FIFO 由 CPU 搬运，缓冲仍在普通 .bss。
 */

#include "log.h"
#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */
#include "stm32h7xx_hal.h"
#include "tusb.h"
#include "usb.h"

#include <string.h>

/* RHPORT0 = USB2_OTG_FS（TinyUSB 的控制器表按 FS/HS 顺序编号）。 */
#define USB_PORT_RHPORT 0U

/* 低于 USART1 的 3：USB 只有端点传输中断，短且不阻塞。 */
#define USB_PORT_IRQ_PRIORITY 5U

/* TinyUSB 在 OPT_OS_NONE 下要求应用提供毫秒时基（dwc2 PHY/复位延时用）。 */
DWORD tusb_time_millis_api(void)
{
    return HAL_GetTick();
}

BOOL usb_device_init(void)
{
    /* dwc2_clock_init() 在 STM32 端口里是空实现，外设时钟必须由应用打开。 */
    __HAL_RCC_USB2_OTG_FS_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA11 = OTG_FS_DM, PA12 = OTG_FS_DP, AF10（USB2_OTG_FS）。 */
    GPIO_InitTypeDef gpio;

    memset(&gpio, 0, sizeof(gpio));
    gpio.Pin       = GPIO_PIN_11 | GPIO_PIN_12;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF10_OTG2_FS;
    HAL_GPIO_Init(GPIOA, &gpio);

    HAL_NVIC_SetPriority(OTG_FS_IRQn, USB_PORT_IRQ_PRIORITY, 0U);

    /* 只支持全速设备；VBUS sensing 由 CFG_TUD_VBUS_DETECT_HW=0 关闭。 */
    tusb_rhport_init_t rh_init;
    memset(&rh_init, 0, sizeof(rh_init));

    rh_init.role  = TUSB_ROLE_DEVICE;
    rh_init.speed = TUSB_SPEED_FULL;

    /* dcd_init 内部会打开 NVIC 并使能全局中断。 */
    const BOOL ok = tusb_rhport_init(USB_PORT_RHPORT, &rh_init);

    /* GHWCFG2.arch[2:0]：0 = slave-only，1 = external DMA，2 = internal DMA。
     * 本仓走 slave/PIO；这一行用于在板端确认该芯片的 OTG_FS 到底有没有
     * 内部 DMA（TinyUSB 的 DMA 路径要求 arch == INTERNAL_DMA）。 */
    const DWORD ghwcfg2 = USB2_OTG_FS->GHWCFG2;
    const DWORD arch    = ghwcfg2 & 0x7U;
    const char *arch_s  = (arch == 0U) ? "slave-only" : (arch == 1U) ? "external-dma"
                                                    : (arch == 2U)   ? "internal-dma"
                                                                     : "unknown";

    (void)LOGI("[USB ] GHWCFG2=0x%08lx arch=%lu (%s)\r\n", ghwcfg2, arch, arch_s);

    return ok;
}

void usb_device_task(void)
{
    /* 超时 0：主循环不能被 USB 事件队列阻塞。 */
    tud_task_ext(0U, false);
}

/* ─────────────────────────── 向量表入口 ─────────────────────────── */

/* OTG_FS 中断入口（startup_stm32h743xx.s 固定名称）；ISR 只做端点搬运。 */
void OTG_FS_IRQHandler(void)
{
    tud_int_handler(USB_PORT_RHPORT);
}
