#ifndef SSD_PLATFORM_H
#define SSD_PLATFORM_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stddef.h>
#include <stdint.h>

/*
 * 平台抽象接口。
 * 固件平台（stm32h743）提供一套实现，核心 FTL 不直接依赖操作系统或寄存器。
 */
typedef struct SSD_PLATFORM_OPS_S
{
    void (*log_write)(const char *message);
    QWORD(*time_us)
    (void);
    DWORD(*time_ms)
    (void); /* 系统毫秒时基（SysTick/HAL tick） */
    void *(*memory_allocate)(size_t size);
    void (*memory_free)(void *memory);
} ssd_platform_ops_s;

func_status_e             ssd_platform_register(const ssd_platform_ops_s *ops);
const ssd_platform_ops_s *ssd_platform_get(void);

/**
 * @brief 当前单调毫秒（平台毫秒时基的统一读取口）
 *
 * 优先注册的 `time_ms`；缺省时退回 `time_us / 1000`；两者都没有则返回 0。
 * 所有消费方都用差值（回绕安全，约 49.7 天一圈）。
 *
 * @return 当前毫秒；平台未注册 / 无时基时 0
 */
DWORD platform_now_ms(void);

/*
 * 注册本平台的 ssd_platform_ops_s 实现表。
 * 名字被 src/main.c 冻结；实现表本身在共享 src/platform/platform.c，
 * 其中的 time_us 由 port 侧毫秒计数换算（见 src/platform/<soc>/<svc>_port.c）。
 */
func_status_e stm32_platform_register(void);

#endif /* SSD_PLATFORM_H */
