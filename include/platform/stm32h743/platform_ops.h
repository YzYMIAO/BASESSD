#ifndef PLATFORM_STM32H743_PLATFORM_OPS_H
#define PLATFORM_STM32H743_PLATFORM_OPS_H

/*
 * stm32_platform_register - 注册 stm32h743 平台操作表（ssd_platform_ops_t）。
 *
 * 当前只提供 time_us（HAL tick 换算微秒）；log_write / memory_allocate /
 * memory_free 未提供（register 仅强制 time_us 非 NULL）。须在 sys_init()
 * 之后调用——HAL_GetTick 依赖 SysTick 已由 HAL_Init() 启动。
 *
 * @return ssd_platform_register 的结果（0 成功，负值失败）
 */
int stm32_platform_register(void);

#endif /* PLATFORM_STM32H743_PLATFORM_OPS_H */
