#include "watchdog.h"

#include "stm32h7xx_hal_iwdg.h"
#include "stm32h7xx_hal_wwdg.h"

#include <string.h>

/* 两个 HAL handle 仅由本文件持有，禁止业务模块绕过 Supervisor 直接刷新硬件。 */
static IWDG_HandleTypeDef          g_storage_iwdg;
static WWDG_HandleTypeDef          g_storage_wwdg;
static storage_wdg_config_t        g_storage_wdg_config;
static storage_wdg_status_t        g_storage_wdg_status;
static storage_wdg_recovery_hook_t g_storage_wdg_recovery_hook;
/* 上一次成功刷新 WWDG 的 HAL tick，用来判断当前是否已进入允许刷新窗口。 */
static uint32_t g_storage_wdg_last_wwdg_refresh_ms;

static bool storage_wdg_domain_is_valid(storage_wdg_domain_t domain)
{
    /* 防止调用方传入损坏的枚举值并越界访问 per-domain 数组。 */
    return ((uint32_t)domain < (uint32_t)STORAGE_WDG_DOMAIN_COUNT);
}

static bool storage_wdg_elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t limit_ms)
{
    /* HAL_GetTick() 约 49 天回绕一次；无符号减法可正确表示短时间间隔。 */
    return ((uint32_t)(now_ms - since_ms) > limit_ms);
}

static bool storage_wdg_all_required_domains_healthy(uint32_t now_ms, storage_wdg_domain_t *timed_out_domain)
{
    uint32_t index;

    for (index = 0U; index < (uint32_t)STORAGE_WDG_DOMAIN_COUNT; index++)
    {
        uint32_t mask       = STORAGE_WDG_DOMAIN_MASK(index);
        uint32_t timeout_ms = g_storage_wdg_config.domain_timeout_ms[index];

        /* 未列入 required_domains 的后台域不影响本次 IWDG 喂狗决定。 */
        if ((g_storage_wdg_config.required_domains & mask) == 0U)
        {
            continue;
        }
        if ((timeout_ms == 0U) || storage_wdg_elapsed(now_ms, g_storage_wdg_status.last_progress_ms[index], timeout_ms))
        {
            *timed_out_domain = (storage_wdg_domain_t)index;
            return false;
        }
    }

    return true;
}

static void storage_wdg_latch_fault(storage_wdg_domain_t domain, storage_wdg_fault_t fault)
{
    /*
     * 首故障优先：后续错误不能覆盖它，便于复位后日志/备份寄存器记录根因。
     * 更重要的是，不能由后来一次“正常心跳”把故障悄悄恢复为健康。
     */
    if (g_storage_wdg_status.fault != STORAGE_WDG_FAULT_NONE)
    {
        return;
    }

    g_storage_wdg_status.fault        = fault;
    g_storage_wdg_status.fault_domain = domain;
    /* 停止 IWDG 喂狗，让独立时钟在确定上限内完成最终全芯片复位。 */
    g_storage_wdg_status.iwdg_feeding = false;

    if ((fault == STORAGE_WDG_FAULT_DOMAIN_TIMEOUT) && (g_storage_wdg_recovery_hook != NULL))
    {
        g_storage_wdg_status.recovery_attempts[domain]++;
        /* 局部恢复仅用于隔离和采集；不会重新开始喂 IWDG。 */
        (void)g_storage_wdg_recovery_hook(domain, fault);
    }
}

bool storage_wdg_init(const storage_wdg_config_t *config)
{
    uint32_t now_ms;
    uint32_t index;

    /* WWDG 的软件窗口不合法时，拒绝启动，避免“看似启用、实际必然复位”。 */
    if (config == NULL)
    {
        return false;
    }
    if (config->enable_wwdg &&
        ((config->wwdg_refresh_min_ms == 0U) || (config->wwdg_refresh_max_ms <= config->wwdg_refresh_min_ms)))
    {
        return false;
    }

    /* 初始化时给每个域一次启动宽限期，避免还未启动的状态机立即超时。 */
    memset(&g_storage_wdg_status, 0, sizeof(g_storage_wdg_status));
    g_storage_wdg_config = *config;
    now_ms               = HAL_GetTick();
    for (index = 0U; index < (uint32_t)STORAGE_WDG_DOMAIN_COUNT; index++)
    {
        g_storage_wdg_status.last_progress_ms[index] = now_ms;
    }

    g_storage_iwdg.Instance       = IWDG1;
    g_storage_iwdg.Init.Prescaler = config->iwdg_prescaler;
    g_storage_iwdg.Init.Reload    = config->iwdg_reload;
    g_storage_iwdg.Init.Window    = IWDG_WINDOW_DISABLE;
    if (HAL_IWDG_Init(&g_storage_iwdg) != HAL_OK)
    {
        return false;
    }

    g_storage_wdg_status.iwdg_feeding = true;
    if (config->enable_wwdg)
    {
        g_storage_wwdg.Instance       = WWDG1;
        g_storage_wwdg.Init.Prescaler = config->wwdg_prescaler;
        g_storage_wwdg.Init.Counter   = config->wwdg_counter;
        g_storage_wwdg.Init.Window    = config->wwdg_window;
        /* EWI 只用于诊断：回调记录故障，绝不自动 refresh，否则会掩盖主循环死锁。 */
        g_storage_wwdg.Init.EWIMode   = WWDG_EWI_ENABLE;
        if (HAL_WWDG_Init(&g_storage_wwdg) != HAL_OK)
        {
            g_storage_wdg_status.iwdg_feeding = false;
            return false;
        }
        g_storage_wdg_status.wwdg_enabled  = true;
        g_storage_wdg_last_wwdg_refresh_ms = now_ms;
    }

    g_storage_wdg_status.initialized = true;
    return true;
}

void storage_wdg_set_recovery_hook(storage_wdg_recovery_hook_t hook)
{
    g_storage_wdg_recovery_hook = hook;
}

void storage_wdg_report_progress(storage_wdg_domain_t domain)
{
    if ((!g_storage_wdg_status.initialized) || !storage_wdg_domain_is_valid(domain))
    {
        return;
    }

    /* 故障锁存后心跳不再生效，必须走受控复位路径，不能静默恢复。 */
    if (g_storage_wdg_status.fault == STORAGE_WDG_FAULT_NONE)
    {
        g_storage_wdg_status.last_progress_ms[domain] = HAL_GetTick();
    }
}

void storage_wdg_service(void)
{
    storage_wdg_domain_t timed_out_domain = STORAGE_WDG_DOMAIN_COUNT;
    uint32_t             now_ms;
    uint32_t             since_wwdg_refresh_ms;

    /* 未初始化或已锁存故障时，一律不继续喂任何看门狗。 */
    if ((!g_storage_wdg_status.initialized) || (!g_storage_wdg_status.iwdg_feeding))
    {
        return;
    }

    now_ms = HAL_GetTick();
    if (!storage_wdg_all_required_domains_healthy(now_ms, &timed_out_domain))
    {
        storage_wdg_latch_fault(timed_out_domain, STORAGE_WDG_FAULT_DOMAIN_TIMEOUT);
        return;
    }

    if (g_storage_wdg_status.wwdg_enabled)
    {
        since_wwdg_refresh_ms = now_ms - g_storage_wdg_last_wwdg_refresh_ms;
        if (since_wwdg_refresh_ms > g_storage_wdg_config.wwdg_refresh_max_ms)
        {
            /* 超过最大窗口后绝不补喂；让 WWDG 自然复位，避免掩盖 Supervisor 被阻塞。 */
            storage_wdg_latch_fault(STORAGE_WDG_DOMAIN_SCHEDULER, STORAGE_WDG_FAULT_WWDG_WINDOW_MISSED);
            return;
        }
        if (since_wwdg_refresh_ms >= g_storage_wdg_config.wwdg_refresh_min_ms)
        {
            (void)HAL_WWDG_Refresh(&g_storage_wwdg);
            g_storage_wdg_last_wwdg_refresh_ms = now_ms;
        }
    }

    /* IWDG 刷新必须位于所有健康检查及 WWDG 窗口检查之后，保证它是最终裁决结果。 */
    (void)HAL_IWDG_Refresh(&g_storage_iwdg);
}

void storage_watchdog_task(void *context, uint32_t now_ms)
{
    (void)context;
    (void)now_ms;

    /*
     * 这是看门狗模块自己的调度器适配层。调度器只负责“到期调用”，
     * 健康域上报和硬件喂狗策略仍由本模块集中控制，避免业务入口绕过
     * Supervisor 直接操作 IWDG/WWDG。
     */
    storage_wdg_report_progress(STORAGE_WDG_DOMAIN_SCHEDULER);
    storage_wdg_service();
}

void storage_wdg_get_status(storage_wdg_status_t *status)
{
    if (status != NULL)
    {
        *status = g_storage_wdg_status;
    }
}

void storage_wdg_wwdg_irq_handler(void)
{
    /* HAL 会在此路径中清 EWI 标志并调用下方的 EarlyWakeupCallback。 */
    if (g_storage_wdg_status.wwdg_enabled)
    {
        HAL_WWDG_IRQHandler(&g_storage_wwdg);
    }
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
    if ((g_storage_wdg_status.wwdg_enabled) && (hwwdg == &g_storage_wwdg))
    {
        g_storage_wdg_status.wwdg_early_warning = true;
        /*
         * EWI 到来说明 WWDG 已接近下溢。这里只锁存最小诊断状态，不打印日志、
         * 不做 NAND 操作、不刷新 WWDG；这些动作可能超出剩余窗口并破坏复位时序。
         */
        storage_wdg_latch_fault(STORAGE_WDG_DOMAIN_SCHEDULER, STORAGE_WDG_FAULT_WWDG_EARLY_WAKEUP);
    }
}
