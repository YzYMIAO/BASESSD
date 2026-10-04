#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdint.h>

/* 日志等级按严重程度从高到低排列，运行时过滤比较只需一次数值比较。 */
typedef enum
{
    LOG_ERROR = 0,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG
} log_level_t;

/* A sink owns transport policy and returns the accepted byte count or a negative error. */
typedef int (*log_sink_t)(const char *message, uint32_t length);
typedef int (*log_flush_sink_t)(uint32_t timeout_ms);

/*
 * 运行时日志接口。sink 由已注册的平台提供，例如 firmware 的 UART 或 host 的 stderr。
 * 业务模块只依赖本头文件，不应直接操作 UART、文件描述符或平台寄存器。
 */
int         log_write(log_level_t level, const char *message);
int         log_printf(log_level_t level, const char *format, ...);
int         log_vprintf(log_level_t level, const char *format, va_list args);
void        log_set_level(log_level_t level);
log_level_t log_get_level(void);
void        log_set_sink(log_sink_t sink);
int         log_flush(uint32_t timeout_ms);
void        log_set_flush_sink(log_flush_sink_t sink);
uint32_t    log_get_truncated_count(void);
uint32_t    log_get_write_failure_count(void);

/*
 * 企业固件中的业务代码使用这些宏。DEBUG 日志可在 release 构建中完全裁剪，
 * 从而避免格式化参数的计算、栈占用和 UART 队列压力。
 */
#define LOGE(format, ...) log_printf(LOG_ERROR, "[ERR] " format, ##__VA_ARGS__)
#define LOGW(format, ...) log_printf(LOG_WARNING, "[WRN] " format, ##__VA_ARGS__)
#define LOGI(format, ...) log_printf(LOG_INFO, "[INF] " format, ##__VA_ARGS__)

#if defined(DEBUG)
#    define LOGD(format, ...) log_printf(LOG_DEBUG, "[DBG] " format, ##__VA_ARGS__)
#else
#    define LOGD(...) ((void)0)
#endif

#endif /* LOG_H */
