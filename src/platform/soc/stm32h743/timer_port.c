/*
 * timer_port.c — 调度器心跳的 stm32h743 适配层
 *
 * 共享逻辑在 src/platform/timer.c（跨 target 编译一次，零 HAL）。本文件只负责
 * 那唯一一件共享层做不了的事：用 TIM6 产生毫秒周期中断，并在中断里把
 * 共享毫秒计数推进一步。
 */

#include "timer_port.h"

#include "timer.h"

/* TIM6 只作为调度器的毫秒硬件心跳。 */
static TIM_HandleTypeDef s_timerx_handle = {0};

BOOL timer_port_hw_init(WORD arr, WORD psc)
{
    s_timerx_handle.Instance               = TIMER_TIMX;
    s_timerx_handle.Init.Prescaler         = psc;
    s_timerx_handle.Init.Period            = arr;
    s_timerx_handle.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    /* 这是全系统唯一的毫秒时基：初始化/启动失败会让所有 ms 超时与调度节拍冻结，
     * 因此失败必须回传给调用者停机，不能只靠 debug 档 ASSERT（release 会裁空）。 */
    if (HAL_TIM_Base_Init(&s_timerx_handle) != HAL_OK)
    {
        return false;
    }
    if (HAL_TIM_Base_Start_IT(&s_timerx_handle) != HAL_OK)
    {
        return false;
    }
    return true;
}

void timer_port_irq_handler(void)
{
    /* 句柄是 static 的，由本文件的 TIM6_DAC_IRQHandler 转发进来。 */
    HAL_TIM_IRQHandler(&s_timerx_handle);
}

/*
 * TIM6 中断入口（向量表名由 startup_stm32h743xx.s 固定）。
 * 本文件即该向量的归属地：调度器毫秒心跳的中断从这里进来。
 */
void TIM6_DAC_IRQHandler(void)
{
    timer_port_irq_handler();
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIMER_TIMX)
    {
        return;
    }

    TIMER_TIMX_CLK_ENABLE();
    /* TIM6 只推进时基，优先级放在紧急数据面中断之下。 */
    HAL_NVIC_SetPriority(TIMER_TIMX_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIMER_TIMX_IRQn);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIMER_TIMX)
    {
        /* ISR 对调度器的全部职责就是推进单调递增的毫秒计数。 */
        timer_scheduler_tick();
    }
}
