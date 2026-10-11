/*
 * btim_port.c — 调度器心跳的 stm32h743 适配层
 *
 * 共享逻辑在 src/platform/btim.c（跨 target 编译一次，零 HAL）。本文件只负责
 * 那唯一一件共享层做不了的事：用 TIM6 产生毫秒周期中断，并在中断里把
 * 共享毫秒计数推进一步。
 */

#include "btim_port.h"

#include "btim.h"

/* TIM6 is used only as the scheduler's millisecond hardware heartbeat. */
static TIM_HandleTypeDef g_btimx_handle = {0};

void btim_timx_int_init(uint16_t arr, uint16_t psc)
{
    g_btimx_handle.Instance               = BTIM_TIMX;
    g_btimx_handle.Init.Prescaler         = psc;
    g_btimx_handle.Init.Period            = arr;
    g_btimx_handle.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_Base_Init(&g_btimx_handle);
    (void)HAL_TIM_Base_Start_IT(&g_btimx_handle);
}

void btim_port_irq_handler(void)
{
    /* 句柄是 static 的，由本文件的 TIM6_DAC_IRQHandler 转发进来。 */
    HAL_TIM_IRQHandler(&g_btimx_handle);
}

/*
 * TIM6 中断入口（向量表名由 startup_stm32h743xx.s 固定）。
 * 本文件即该向量的归属地：调度器毫秒心跳的中断从这里进来。
 */
void TIM6_DAC_IRQHandler(void)
{
    btim_port_irq_handler();
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != BTIM_TIMX)
    {
        return;
    }

    BTIM_TIMX_CLK_ENABLE();
    /* TIM6 only increments time; keep it below urgent data-plane interrupts. */
    HAL_NVIC_SetPriority(BTIM_TIMX_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(BTIM_TIMX_IRQn);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == BTIM_TIMX)
    {
        /* The ISR's entire scheduler role is advancing a monotonically increasing tick. */
        btim_scheduler_tick();
    }
}
