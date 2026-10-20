#ifndef SSD_PLATFORM_H
#define SSD_PLATFORM_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stddef.h>
#include <stdint.h>

/*
 * 平台抽象接口。
 * 固件平台（stm32h743/stm32h7r3）提供一套实现，核心 FTL 不直接依赖操作系统或寄存器。
 */
typedef struct
{
    void (*log_write)(const char *message);
    QWORD(*time_us)
    (void);
    void *(*memory_allocate)(size_t size);
    void (*memory_free)(void *memory);
} ssd_platform_ops_t;

int                       ssd_platform_register(const ssd_platform_ops_t *ops);
const ssd_platform_ops_t *ssd_platform_get(void);

/*
 * 注册本平台的 ssd_platform_ops_t 实现表。
 * 名字被 src/main.c 冻结；实现表本身在共享 src/platform/platform.c，
 * 其中的 time_us 由 port 侧毫秒计数换算（见 src/platform/<soc>/<svc>_port.c）。
 */
int stm32_platform_register(void);

#endif /* SSD_PLATFORM_H */
