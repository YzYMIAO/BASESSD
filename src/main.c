#include "firmware/boot.h"
#include "platform/cpu_armv7r.h"
#include "platform/soc.h"

/*
 * Cortex-R5 主固件入口。
 *
 * Reset_Handler 已经完成处理器模式栈、VBAR、.data 搬运和 .bss 清零。
 * 从这里开始使用 C 代码按依赖顺序初始化平台、存储和主机接口。
 */
int main(void)
{
    firmware_boot_context_t boot_context;
    int                     status;

    firmware_boot_context_init(&boot_context);
    status = firmware_boot_execute(&boot_context);
    if (status != SSD_STATUS_OK)
    {
        /*
         * 当前参考平台没有真实 UART 和复位控制器，因此失败后保留现场并
         * 等待调试器。真实 BSP 可在这里记录崩溃信息并触发看门狗复位。
         */
        soc_uart_write("firmware boot failed\n");
        for (;;)
        {
            armv7r_wait_for_interrupt();
        }
    }

    if (boot_context.stage == FIRMWARE_BOOT_STAGE_PLACEHOLDER_READY)
    {
        soc_uart_write("reference boot flow completed with placeholders\n");
    }
    else
    {
        soc_uart_write("firmware entered ready state\n");
    }

    /*
     * 第一阶段采用裸机事件循环。后续接入控制器调度、后台 GC 或 RTOS
     * 后，可在 WFI 前处理软件队列和后台任务。
     */
    for (;;)
    {
        armv7r_wait_for_interrupt();
    }
}
