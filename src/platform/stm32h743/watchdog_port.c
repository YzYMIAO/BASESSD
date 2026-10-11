/*
 * watchdog_port.c — 看门狗 Supervisor 的 stm32h743 适配层
 *
 * 共享逻辑在 src/platform/watchdog.c（跨 target 编译一次，零 HAL）。本文件只
 * 持有 IWDG/WWDG 句柄、执行 HAL 调用，并把两个固定签名的 HAL 回调转回共享层。
 *
 * 句柄仅由本文件持有，禁止业务模块绕过 Supervisor 直接刷新硬件。
 */

#include "stm32h7xx_hal.h"
#include "watchdog.h"

static IWDG_HandleTypeDef g_storage_iwdg;
static WWDG_HandleTypeDef g_storage_wwdg;

bool watchdog_port_iwdg_init(uint32_t prescaler, uint32_t reload)
{
    g_storage_iwdg.Instance       = IWDG1;
    g_storage_iwdg.Init.Prescaler = prescaler;
    g_storage_iwdg.Init.Reload    = reload;
    g_storage_iwdg.Init.Window    = IWDG_WINDOW_DISABLE;
    return HAL_IWDG_Init(&g_storage_iwdg) == HAL_OK;
}

bool watchdog_port_wwdg_init(uint32_t prescaler, uint32_t counter, uint32_t window)
{
    g_storage_wwdg.Instance       = WWDG1;
    g_storage_wwdg.Init.Prescaler = prescaler;
    g_storage_wwdg.Init.Counter   = counter;
    g_storage_wwdg.Init.Window    = window;
    /* EWI 只用于诊断：回调记录故障，绝不自动 refresh，否则会掩盖主循环死锁。 */
    g_storage_wwdg.Init.EWIMode   = WWDG_EWI_ENABLE;
    return HAL_WWDG_Init(&g_storage_wwdg) == HAL_OK;
}

void watchdog_port_iwdg_refresh(void)
{
    (void)HAL_IWDG_Refresh(&g_storage_iwdg);
}

void watchdog_port_wwdg_refresh(void)
{
    (void)HAL_WWDG_Refresh(&g_storage_wwdg);
}

void watchdog_port_wwdg_irq_handler(void)
{
    /* HAL 会在此路径中清 EWI 标志并调用下方的 EarlyWakeupCallback。 */
    HAL_WWDG_IRQHandler(&g_storage_wwdg);
}

/*
 * WWDG 提前唤醒中断入口（向量表名由 startup_stm32h743xx.s 固定）。
 * 本文件即该向量的归属地。
 * 不在这里刷新 WWDG；Supervisor 记录故障并让硬件完成复位。
 */
void WWDG_IRQHandler(void)
{
    storage_wdg_wwdg_irq_handler();
}

uint32_t watchdog_port_now_ms(void)
{
    return HAL_GetTick();
}

/*
 * HAL_WWDG_Init() 调用此板级回调。它必须与统一 Supervisor 放在同一模块，
 * 确保没有旧驱动抢占 WWDG 时钟、NVIC 或 IRQ 所有权。
 */
void HAL_WWDG_MspInit(WWDG_HandleTypeDef *hwwdg)
{
    (void)hwwdg;
    __HAL_RCC_WWDG_CLK_ENABLE();
    HAL_NVIC_SetPriority(WWDG_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(WWDG_IRQn);
}

void HAL_WWDG_EarlyWakeupCallback(WWDG_HandleTypeDef *hwwdg)
{
    /* 句柄比对留在这里（共享层看不到句柄），业务判断转回共享层。 */
    if (hwwdg == &g_storage_wwdg)
    {
        storage_wdg_wwdg_early_wakeup();
    }
}
