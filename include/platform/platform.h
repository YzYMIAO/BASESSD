#ifndef SSD_PLATFORM_H
#define SSD_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/*
 * 平台抽象接口。
 * 固件平台（stm32h743/stm32h7r3）提供一套实现，核心 FTL 不直接依赖操作系统或寄存器。
 */
typedef struct
{
    void (*log_write)(const char *message);
    uint64_t (*time_us)(void);
    void *(*memory_allocate)(size_t size);
    void (*memory_free)(void *memory);
} ssd_platform_ops_t;

int                       ssd_platform_register(const ssd_platform_ops_t *ops);
const ssd_platform_ops_t *ssd_platform_get(void);

#endif /* SSD_PLATFORM_H */
