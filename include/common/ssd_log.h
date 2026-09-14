#ifndef SSD_LOG_H
#define SSD_LOG_H

#include <stdarg.h>

/* 日志等级按严重程度从高到低排列，运行时过滤比较只需一次数值比较。 */
typedef enum
{
    SSD_LOG_ERROR = 0,
    SSD_LOG_WARNING,
    SSD_LOG_INFO,
    SSD_LOG_DEBUG
} ssd_log_level_t;

/* A sink owns transport policy; it may enqueue to UART, write stderr, or append a RAM trace. */
typedef void (*ssd_log_sink_t)(const char *message);

/*
 * 运行时日志接口。sink 由已注册的平台提供，例如 firmware 的 UART 或 host 的 stderr。
 * 业务模块只依赖本头文件，不应直接操作 UART、文件描述符或平台寄存器。
 */
void ssd_log_write(ssd_log_level_t level, const char *message);
int ssd_log_printf(ssd_log_level_t level, const char *format, ...);
int ssd_log_vprintf(ssd_log_level_t level, const char *format, va_list args);
void ssd_log_set_level(ssd_log_level_t level);
ssd_log_level_t ssd_log_get_level(void);
void ssd_log_set_sink(ssd_log_sink_t sink);

/*
 * 企业固件中的业务代码使用这些宏。DEBUG 日志可在 release 构建中完全裁剪，
 * 从而避免格式化参数的计算、栈占用和 UART 队列压力。
 */
#define SSD_LOGE(format, ...) ssd_log_printf(SSD_LOG_ERROR, "[ERR] " format, ##__VA_ARGS__)
#define SSD_LOGW(format, ...) ssd_log_printf(SSD_LOG_WARNING, "[WRN] " format, ##__VA_ARGS__)
#define SSD_LOGI(format, ...) ssd_log_printf(SSD_LOG_INFO, "[INF] " format, ##__VA_ARGS__)

#if defined(DEBUG)
#define SSD_LOGD(format, ...) ssd_log_printf(SSD_LOG_DEBUG, "[DBG] " format, ##__VA_ARGS__)
#else
#define SSD_LOGD(...) ((void)0)
#endif

#endif /* SSD_LOG_H */
