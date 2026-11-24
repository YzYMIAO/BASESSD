/*
 * watchdog.c — 存储主控看门狗 Supervisor（跨 target 共享逻辑）
 *
 * 本文件不 include 任何 SoC 头、不出现任何 HAL 类型：健康域状态机、首故障锁存与
 * WWDG 窗口决策全在这里；IWDG/WWDG 句柄与硬件调用全部下沉到
 * src/platform/<soc>/watchdog_port.c。
 *
 * 设计要点：硬件喂狗是最终裁决，因此 service() 里所有健康检查与窗口检查都排在
 * IWDG 刷新之前；一旦锁存故障就再也不喂狗，让独立时钟完成全芯片复位。
 */

#include "watchdog.h"

#include <string.h>

/* 长操作显式喂狗的节流间隔：远小于 scheduler 域超时（100 ms），又避免同步泵的
 * 每个自旋轮次都写一次 IWDG 寄存器。 */
#define STORAGE_WDG_KICK_INTERVAL_MS (10U)

static storage_wdg_cfg_s g_storage_wdg_config;

static storage_wdg_status_s g_storage_wdg_status;

static storage_wdg_recovery_hook_fn_t g_storage_wdg_recovery_hook;

/* 上一次成功刷新 WWDG 的时刻，用来判断当前是否已进入允许刷新窗口。 */
static DWORD g_storage_wdg_last_wwdg_refresh_ms;

static DWORD s_last_kick_ms;

static BOOL storage_wdg_domain_is_valid(wdg_domain_e domain)
{
    /* 防止调用方传入损坏的枚举值并越界访问 per-domain 数组。 */
    return (domain < STORAGE_WDG_DOMAIN_COUNT);
}

static BOOL storage_wdg_elapsed(DWORD now_ms, DWORD since_ms, DWORD limit_ms)
{
    /* 毫秒计数约 49 天回绕一次；无符号减法可正确表示短时间间隔。 */
    return ((now_ms - since_ms) > limit_ms);
}

static BOOL storage_wdg_all_required_domains_healthy(DWORD now_ms, wdg_domain_e *timed_out_domain)
{
    DWORD index;

    for (index = 0U; index < STORAGE_WDG_DOMAIN_COUNT; index++)
    {
        DWORD mask       = STORAGE_WDG_DOMAIN_MASK(index);
        DWORD timeout_ms = g_storage_wdg_config.domain_timeout_ms[index];

        /* 未列入 required_domains 的后台域不影响本次 IWDG 喂狗决定。 */
        if ((g_storage_wdg_config.required_domains & mask) == 0U)
        {
            continue;
        }
        if ((timeout_ms == 0U) || storage_wdg_elapsed(now_ms, g_storage_wdg_status.last_progress_ms[index], timeout_ms))
        {
            *timed_out_domain = (wdg_domain_e)index;
            return false;
        }
    }

    return true;
}

static void storage_wdg_latch_fault(wdg_domain_e domain, wdg_fault_e fault)
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

BOOL storage_wdg_init(const storage_wdg_cfg_s *config)
{
    DWORD now_ms;
    DWORD index;

    /* WWDG 的软件窗口不合法时，拒绝启动，避免“看似启用、实际必然复位”。 */
    if (config == NULL)
    {
        return false;
    }
    if (config->enable_wwdg && ((config->wwdg_refresh_min_ms == 0U) || (config->wwdg_refresh_max_ms <= config->wwdg_refresh_min_ms)))
    {
        return false;
    }

    /* 初始化时给每个域一次启动宽限期，避免还未启动的状态机立即超时。 */
    memset(&g_storage_wdg_status, 0, sizeof(g_storage_wdg_status));
    g_storage_wdg_config = *config;
    now_ms               = watchdog_port_now_ms();
    for (index = 0U; index < STORAGE_WDG_DOMAIN_COUNT; index++)
    {
        g_storage_wdg_status.last_progress_ms[index] = now_ms;
    }

    if (!watchdog_port_iwdg_init(config->iwdg_prescaler, config->iwdg_reload))
    {
        return false;
    }

    g_storage_wdg_status.iwdg_feeding = true;
    if (config->enable_wwdg)
    {
        if (!watchdog_port_wwdg_init(config->wwdg_prescaler, config->wwdg_counter, config->wwdg_window))
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

void storage_wdg_set_recovery_hook(storage_wdg_recovery_hook_fn_t hook)
{
    g_storage_wdg_recovery_hook = hook;
}

void storage_wdg_report_progress(wdg_domain_e domain)
{
    if ((!g_storage_wdg_status.initialized) || !storage_wdg_domain_is_valid(domain))
    {
        return;
    }

    /* 故障锁存后心跳不再生效，必须走受控复位路径，不能静默恢复。 */
    if (g_storage_wdg_status.fault == STORAGE_WDG_FAULT_NONE)
    {
        g_storage_wdg_status.last_progress_ms[domain] = watchdog_port_now_ms();
    }
}

void storage_wdg_service(void)
{
    wdg_domain_e timed_out_domain = STORAGE_WDG_DOMAIN_COUNT;
    DWORD        now_ms;
    DWORD        since_wwdg_refresh_ms;

    /* 未初始化或已锁存故障时，一律不继续喂任何看门狗。 */
    if ((!g_storage_wdg_status.initialized) || (!g_storage_wdg_status.iwdg_feeding))
    {
        return;
    }

    now_ms = watchdog_port_now_ms();
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
            watchdog_port_wwdg_refresh();
            g_storage_wdg_last_wwdg_refresh_ms = now_ms;
        }
    }

    /* IWDG 刷新必须位于所有健康检查及 WWDG 窗口检查之后，保证它是最终裁决结果。 */
    watchdog_port_iwdg_refresh();
}

void storage_wdg_kick(void)
{
    const DWORD now_ms = watchdog_port_now_ms();

    if ((now_ms - s_last_kick_ms) < STORAGE_WDG_KICK_INTERVAL_MS)
    {
        return;
    }
    s_last_kick_ms = now_ms;

    // 顺序承重：先上报再 service。长操作把主循环占住，service 的域健康检查会看到
    // 「scheduler 很久没上报」；反过来会在第一次 kick 就 latch 超时故障并停喂。
    storage_wdg_report_progress(STORAGE_WDG_DOMAIN_SCHEDULER);
    storage_wdg_service();
}

void storage_watchdog_task(void *context, DWORD now_ms)
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

void storage_wdg_get_status(storage_wdg_status_s *status)
{
    if (status != NULL)
    {
        *status = g_storage_wdg_status;
    }
}

void storage_wdg_wwdg_irq_handler(void)
{
    /* 具体的中断分发（清 EWI 标志）由 port 侧完成。 */
    if (g_storage_wdg_status.wwdg_enabled)
    {
        watchdog_port_wwdg_irq_handler();
    }
}

void storage_wdg_wwdg_early_wakeup(void)
{
    if (!g_storage_wdg_status.wwdg_enabled)
    {
        return;
    }

    g_storage_wdg_status.wwdg_early_warning = true;
    /*
     * EWI 到来说明 WWDG 已接近下溢。这里只锁存最小诊断状态，不打印日志、
     * 不做 NAND 操作、不刷新 WWDG；这些动作可能超出剩余窗口并破坏复位时序。
     */
    storage_wdg_latch_fault(STORAGE_WDG_DOMAIN_SCHEDULER, STORAGE_WDG_FAULT_WWDG_EARLY_WAKEUP);
}
