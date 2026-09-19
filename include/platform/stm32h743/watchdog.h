#ifndef SSD_STM32H743_WATCHDOG_H
#define SSD_STM32H743_WATCHDOG_H

#include "sys.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * 存储主控的健康域。一个域只能在状态机发生“已验证的前进”后上报心跳，
 * 不能因为任务被调度过一次就上报。例如 NAND 域应在 R/B# 状态变化、DMA 完成
 * 或命令状态推进后上报，而不是在等待 Ready 的轮询循环中无条件上报。
 */
typedef enum
{
    STORAGE_WDG_DOMAIN_SCHEDULER = 0,
    STORAGE_WDG_DOMAIN_HOST,
    STORAGE_WDG_DOMAIN_NAND,
    STORAGE_WDG_DOMAIN_FTL,
    STORAGE_WDG_DOMAIN_GC,
    STORAGE_WDG_DOMAIN_DMA,
    STORAGE_WDG_DOMAIN_POWER,
    STORAGE_WDG_DOMAIN_COUNT
} storage_wdg_domain_t;

/* 将某个域转换成 required_domains 使用的位图，例如 HOST | NAND | FTL。 */
#define STORAGE_WDG_DOMAIN_MASK(domain) (1UL << (uint32_t)(domain))

typedef enum
{
    STORAGE_WDG_FAULT_NONE = 0,           /* 当前健康，允许继续喂 IWDG。 */
    STORAGE_WDG_FAULT_DOMAIN_TIMEOUT,     /* 一个必需域在 deadline 前没有前进。 */
    STORAGE_WDG_FAULT_WWDG_WINDOW_MISSED, /* Supervisor 太晚运行，错过 WWDG 窗口。 */
    STORAGE_WDG_FAULT_WWDG_EARLY_WAKEUP   /* WWDG 即将复位前产生了 EWI。 */
} storage_wdg_fault_t;

/*
 * 可选的局部恢复回调，例如重置 NAND channel 或重置 PCIe 控制器。
 * 回调必须是有时间上限的，不得在其中轮询等待或再次喂狗。返回 true 仅表示已接受
 * 恢复请求，不会清除全局 fault，也不会恢复 IWDG 喂狗；必需域超时后仍以硬复位和
 * FTL journal/checkpoint 恢复作为一致性保障。
 */
typedef bool (*storage_wdg_recovery_hook_t)(storage_wdg_domain_t domain, storage_wdg_fault_t fault);

typedef struct
{
    /*
     * IWDG 使用 LSI。超时 = (reload + 1) * prescaler / f_LSI；必须大于最长合法
     * 操作并留 LSI 频率误差余量，例如 NAND 擦除、读重试或受控掉电提交。
     */
    uint32_t iwdg_prescaler;
    uint32_t iwdg_reload;

    /*
     * 位图中置位的域是“本周期必需健康”的域。每个置位域必须在自己的 timeout 内
     * 调用 storage_wdg_report_progress()；timeout=0 被视为配置错误并立即判超时。
     */
    uint32_t required_domains;
    uint32_t domain_timeout_ms[STORAGE_WDG_DOMAIN_COUNT];

    /*
     * WWDG 依赖 PCLK 和严格调度节奏，因此可选。其作用是发现 Supervisor 过快或
     * 过慢运行；IWDG 仍是最终、独立时钟驱动的复位保障。
     */
    bool     enable_wwdg;
    uint32_t wwdg_prescaler;
    uint32_t wwdg_counter;
    uint32_t wwdg_window;

    /*
     * 根据 PCLK1、prescaler、counter、window 算出的软件合法窗口，单位 ms。
     * service() 只会在 min <= elapsed <= max 时刷新 WWDG。min/max 应比理论窗口
     * 略窄，为时钟误差、HAL tick 粒度和高优先级中断延迟预留余量。
     */
    uint32_t wwdg_refresh_min_ms;
    uint32_t wwdg_refresh_max_ms;
} storage_wdg_config_t;

typedef struct
{
    bool                 initialized;                                 /* 硬件和软件状态是否已经完成初始化。 */
    bool                 iwdg_feeding;                                /* false 表示已故障锁存，不会再刷新 IWDG。 */
    bool                 wwdg_enabled;                                /* 本次配置是否启用了 WWDG。 */
    bool                 wwdg_early_warning;                          /* 是否已经收到 WWDG 的 EWI。 */
    storage_wdg_fault_t  fault;                                       /* 首个故障原因，锁存后不被后续故障覆盖。 */
    storage_wdg_domain_t fault_domain;                                /* 首个超时或异常关联的健康域。 */
    uint32_t             last_progress_ms[STORAGE_WDG_DOMAIN_COUNT];  /* 各域最后前进时刻。 */
    uint32_t             recovery_attempts[STORAGE_WDG_DOMAIN_COUNT]; /* 每域本次启动的恢复请求次数。 */
} storage_wdg_status_t;

/* 配置 IWDG 与可选 WWDG，并以当前 HAL tick 作为所有域的初始心跳时刻。 */
bool storage_wdg_init(const storage_wdg_config_t *config);

/* 在正常 I/O 开始前注册有界的局部恢复回调。 */
void storage_wdg_set_recovery_hook(storage_wdg_recovery_hook_t hook);

/* 在指定状态机确认前进时调用，用当前 HAL tick 更新该域心跳。 */
void storage_wdg_report_progress(storage_wdg_domain_t domain);

/* 单一 Supervisor 任务或主循环周期调用，执行全部健康判定和唯一的硬件喂狗。 */
void storage_wdg_service(void);

/*
 * 调度器任务入口：由 btim_scheduler_poll() 在主循环上下文调用。
 * 任务实现属于看门狗模块，main.c 只负责把它装配进系统静态任务表。
 */
void storage_watchdog_task(void *context, uint32_t now_ms);

/* 获取状态快照，供 UART CLI、崩溃转储或遥测读取。 */
void storage_wdg_get_status(storage_wdg_status_t *status);

/* WWDG_IRQHandler 调用；它只分发 HAL 中断，不在 IRQ 上下文执行应用级恢复。 */
void storage_wdg_wwdg_irq_handler(void);

#endif /* SSD_STM32H743_WATCHDOG_H */
