
#include "btim.h"
#include "common/ssd_crash.h"
#include "common/ssd_log.h"
#include "delay.h"
#include "fc/fc.h"
// #include "led.h"
#include "mpu.h"
#include "nand/nand_dispatch.h"
#include "nand/nand_mt29f4g08abadawp.h"
#if defined(FC_NAND_TEST_ENABLE)
/* TODO: 待重构为 munit UT 框架后移除 */
/* #include "test_fc_nand.h" */
#endif
#include "stm32h7xx_hal.h"
#include "sys.h"
#include "usart.h"
#include "watchdog.h"

#include <string.h>

/* 每次需要确认板上 ELF 是否更新时递增；仅用于开发阶段 UART 诊断。 */
#define SSD_FIRMWARE_DEBUG_REVISION 23U
#define SSD_WATCHDOG_ENABLE         0U

#if (SSD_WATCHDOG_ENABLE != 0U)
/*
 * 当前板级自检阶段只监督 Scheduler。产品启动 Host/NAND/FTL/GC/DMA 后，应把对应
 * bit 加入 required_domains，并在每个状态机的“确认前进”位置调用 report_progress()。
 * 当前板级阶段只启用 IWDG。按典型 LSI=32 kHz 计算，/256 与 Reload=3749 的
 * 名义超时约为 30 s：T=(3749+1)*256/32000。LSI 有频偏，产品量产前应根据
 * 数据手册的频率范围验证最短/最长复位时间，而不应把它当作精确的 30 s 定时器。
 */
static const storage_wdg_config_t g_storage_wdg_config = {
    .iwdg_prescaler      = IWDG_PRESCALER_256,
    .iwdg_reload         = 3749U,
    .required_domains    = STORAGE_WDG_DOMAIN_MASK(STORAGE_WDG_DOMAIN_SCHEDULER),
    .domain_timeout_ms   = {[STORAGE_WDG_DOMAIN_SCHEDULER] = 100U},
    .enable_wwdg         = false,
    .wwdg_prescaler      = 0U,
    .wwdg_counter        = 0U,
    .wwdg_window         = 0U,
    .wwdg_refresh_min_ms = 0U,
    .wwdg_refresh_max_ms = 0U,
};
#endif

/* Common logging stays transport-neutral; this BSP binds it to the non-blocking UART queue. */
static int stm32_uart_log_sink(const char *message, uint32_t length)
{
    return uart_log_write(message, length);
}

static int stm32_uart_log_flush_sink(uint32_t timeout_ms)
{
    return uart_log_flush(timeout_ms);
}

#if 0
static void stm32_report_previous_crash(void)
{
    const ssd_crash_record_t *record = ssd_crash_record_get();

    if (record == NULL)
    {
        return;
    }

    SSD_LOGE("previous assert: %s (%s:%lu)\r\n",
             record->expression,
             record->file,
             (unsigned long)record->line);
    if (ssd_log_flush(1000U) == 0)
    {
        ssd_crash_record_clear();
    }
}
#endif
/* Newlib-nano calls this hook from __libc_init_array when no CRT provides it. */
void _init(void)
{
}

/*
 * STM32H743 烧录自检入口。
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
    uint8_t nand_init_result;

    sys_init();
    ssd_log_set_sink(stm32_uart_log_sink);
    ssd_log_set_flush_sink(stm32_uart_log_flush_sink);
    ssd_log_set_level(SSD_LOG_INFO);
    SSD_LOGD("debug logging enabled\r\n");
    /* VBAT 未供电时 BKPSRAM 无法保持；调试阶段暂不读取 Crash Record。 */
    /* stm32_report_previous_crash(); */
    SSD_LOGI("soft_ssd debug revision=%lu\r\n", (unsigned long)SSD_FIRMWARE_DEBUG_REVISION);
    if (fc_init() != FC_STATUS_OK)
    {
        SSD_LOGE("fc init failed\r\n");
    }
    else if ((nand_init_result = nand_init()) != NAND_RESULT_OK)
    {
        SSD_LOGE("nand init failed: 0x%02X\r\n", (unsigned int)nand_init_result);
    }
    else
    {
        nand_dispatch_set_device_ready(1U);
        SSD_LOGI("fc and nand raw path ready\r\n");
    }
    /* APB1=100 MHz and APB1 prescaler=2, so TIM6 kernel clock is 200 MHz. */
    btim_timx_int_init(1000 - 1, 200 - 1);
    SSD_LOGI("soft_ssd console ready\r\n");
    SSD_LOGI("type 'help' for commands\r\n");
#if (SSD_WATCHDOG_ENABLE != 0U)
    if (!storage_wdg_init(&g_storage_wdg_config))
    {
        SSD_LOGE("watchdog supervisor init failed\r\n");
    }
    else
    {
        /* 固定任务已在 btim.c 的 g_btim_tasks 中注册；这里只启动其运行态。 */
        btim_scheduler_init();
        SSD_LOGI("btim task table ready, IWDG nominal=30s, period=100ms\r\n");
    }
#else
    btim_scheduler_init();
    SSD_LOGI("btim task table ready, IWDG disabled for debug\r\n");
#endif
    uart_service_poll();
    while (1)
    {
        /* All command parsing and scheduled storage work remain outside ISR context. */
        uart_service_poll();
        (void)fc_service();
        btim_scheduler_poll();
    }
}
