
#include "stm32h7rsxx_hal.h"
#include "sys.h"
#include "usart.h"
#include "delay.h"
#include "led.h"
#include "key.h"
#include "exti.h"
#include "btim.h"
#include "mpu.h"
#include "watchdog.h"
#include "common/ssd_log.h"

#include <string.h>

/*
 * 当前板级自检阶段只监督 Scheduler。产品启动 Host/NAND/FTL/GC/DMA 后，应把对应
 * bit 加入 required_domains，并在每个状态机的“确认前进”位置调用 report_progress()。
 * 当前板级阶段只启用 IWDG。按典型 LSI=32 kHz 计算，/256 与 Reload=3749 的
 * 名义超时约为 30 s：T=(3749+1)*256/32000。LSI 有频偏，产品量产前应根据
 * 数据手册的频率范围验证最短/最长复位时间，而不应把它当作精确的 30 s 定时器。
 */
static const storage_wdg_config_t g_storage_wdg_config = {
    .iwdg_prescaler = IWDG_PRESCALER_256,
    .iwdg_reload = 3749U,
    .required_domains = STORAGE_WDG_DOMAIN_MASK(STORAGE_WDG_DOMAIN_SCHEDULER),
    .domain_timeout_ms = {[STORAGE_WDG_DOMAIN_SCHEDULER] = 100U},
    .enable_wwdg = false,
    .wwdg_prescaler = 0U,
    .wwdg_counter = 0U,
    .wwdg_window = 0U,
    .wwdg_refresh_min_ms = 0U,
    .wwdg_refresh_max_ms = 0U,
};

/* Common logging stays transport-neutral; this BSP binds it to the non-blocking UART queue. */
static void stm32_uart_log_sink(const char *message)
{
    (void)uart_log_write(message, (uint32_t)strlen(message));
}
/* Newlib-nano calls this hook from __libc_init_array when no CRT provides it. */
void _init(void)
{
}

/*
 * STM32H7R3 烧录自检入口。
 * Reset_Handler 已完成 C 运行时初始化；HAL_Init() 配置 SysTick 为 1 ms 节拍。
 */
/*
void SysTick_Handler(void)
{
    HAL_IncTick();
}
*/



int main(void)
{
    sys_init();
    ssd_log_set_sink(stm32_uart_log_sink);
    led_init();                         /* 初始化LED */
    key_init();
    exti_init();                        /* 初始化外部中断 */
    /* TIM6 kernel clock is currently 300 MHz: divide to 1 MHz then count 1000 ticks = 1 ms. */
    btim_timx_int_init(1000 - 1, 300 - 1);
    SSD_LOGI("soft_ssd console ready\r\n");
    SSD_LOGI("type 'help' for commands\r\n");
    if (!storage_wdg_init(&g_storage_wdg_config))
        SSD_LOGE("watchdog supervisor init failed\r\n");
    else
    {
        /* 固定任务已在 btim.c 的 g_btim_tasks 中注册；这里只启动其运行态。 */
        btim_scheduler_init();
        SSD_LOGI("btim task table ready, IWDG nominal=30s, period=100ms\r\n");
    }
    uart_service_poll();
    while (1)
    {
        /* All command parsing and scheduled storage work remain outside ISR context. */
        uart_service_poll();
        btim_scheduler_poll();
    }
}

