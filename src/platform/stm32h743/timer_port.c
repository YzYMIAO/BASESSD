/*
 * timer_port.c — 调度器心跳的 stm32h743 适配层
 *
 * 共享逻辑在 src/platform/timer.c（跨 target 编译一次，零 HAL）。本文件只负责
 * 那唯一一件共享层做不了的事：用 TIM6 产生毫秒周期中断，并在中断里把
 * 共享毫秒计数推进一步。
 */

#include "timer_port.h"

#include "assertion.h"
#include "timer.h"

/* TIM6 is used only as the scheduler's millisecond hardware heartbeat. */
static TIM_HandleTypeDef g_timerx_handle = {0};

void timer_timx_int_init(WORD arr, WORD psc)
{
    HAL_StatusTypeDef status;

    g_timerx_handle.Instance               = TIMER_TIMX;
    g_timerx_handle.Init.Prescaler         = psc;
    g_timerx_handle.Init.Period            = arr;
    g_timerx_handle.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    /* 这是全系统唯一的毫秒时基：初始化/启动失败会让所有 ms 超时与调度节拍冻结。
     * 两个 HAL 调用都有副作用，必须先执行再断言（H14：不能写进 ASSERT）。 */
    status                                 = HAL_TIM_Base_Init(&g_timerx_handle);
    ASSERT(status == HAL_OK);
    status = HAL_TIM_Base_Start_IT(&g_timerx_handle);
    ASSERT(status == HAL_OK);
    (void)status; /* release 下 ASSERT 裁空；显式丢弃避免 unused 告警（H30） */
}

void timer_port_irq_handler(void)
{
    /* 句柄是 static 的，由本文件的 TIM6_DAC_IRQHandler 转发进来。 */
    HAL_TIM_IRQHandler(&g_timerx_handle);
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
    /* TIM6 only increments time; keep it below urgent data-plane interrupts. */
    HAL_NVIC_SetPriority(TIMER_TIMX_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIMER_TIMX_IRQn);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIMER_TIMX)
    {
        /* The ISR's entire scheduler role is advancing a monotonically increasing tick. */
        timer_scheduler_tick();
    }
}
