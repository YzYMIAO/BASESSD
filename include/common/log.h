#ifndef LOG_H
#define LOG_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* 日志等级按严重程度从高到低排列，运行时过滤比较只需一次数值比较。 */
typedef enum
{
    LOG_ERROR = 0,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG
} log_level_e;

/* A sink owns transport policy and returns the accepted byte count or LOG_WRITE_INVALID. */
#define LOG_WRITE_INVALID (UINT32_MAX)
typedef DWORD (*log_sink_t)(const char *message, DWORD length);
typedef bool (*log_flush_sink_t)(DWORD timeout_ms);

/*
 * 运行时日志接口。sink 由已注册的平台提供，例如 firmware 的 UART 或 host 的 stderr。
 * 业务模块只依赖本头文件，不应直接操作 UART、文件描述符或平台寄存器。
 */
DWORD log_write(log_level_e level, const char *message);
/* 格式与实参类型由编译器按 printf 语义检查（GCC -Wformat）。 */
__attribute__((format(printf, 2, 3))) DWORD log_printf(log_level_e level, const char *format, ...);
DWORD                                       log_vprintf(log_level_e level, const char *format, va_list args);
void                                        log_set_level(log_level_e level);
log_level_e                                 log_get_level(void);
void                                        log_set_sink(log_sink_t sink);
bool                                        log_flush(DWORD timeout_ms);
void                                        log_set_flush_sink(log_flush_sink_t sink);
DWORD                                       log_get_truncated_count(void);
DWORD                                       log_get_write_failure_count(void);

/*
 * 企业固件中的业务代码使用这些宏。DEBUG 日志可在 release 构建中完全裁剪，
 * 从而避免格式化参数的计算、栈占用和 UART 队列压力。
 */
/*
 * 模块标签固定 4 字符宽，右括号对齐；不足补空格。级别前缀 [ERR]/[WRN]/[INF]/[DBG]
 * 固定 3 字符，本身已对齐。格式串直接拼接这些宏：编译期完成，无运行期格式化开销。
 * 例外：[FAULT] / [BOUND] 是断言/故障类别，不是模块标签，不参与对齐。
 */
#define LOG_TAG_GC   "[GC  ] "
#define LOG_TAG_MIG  "[MIG ] "
#define LOG_TAG_L2P  "[L2P ] "
#define LOG_TAG_JRN  "[JRN ] "
#define LOG_TAG_CP   "[CP  ] "
#define LOG_TAG_RR   "[RR  ] "
#define LOG_TAG_WL   "[WL  ] "
#define LOG_TAG_MD   "[MD  ] "
#define LOG_TAG_BBT  "[BBT ] "
#define LOG_TAG_USB  "[USB ] "
#define LOG_TAG_FTL  "[FTL ] "
#define LOG_TAG_BLK  "[BLK ] "
#define LOG_TAG_TRC  "[TRC ] "
#define LOG_TAG_CAP  "[CAP ] "
#define LOG_TAG_FLT  "[FLT ] "
#define LOG_TAG_STAT "[STAT] "
#define LOG_TAG_TRIM "[TRIM] "
#define LOG_TAG_PRED "[PRED] "
#define LOG_TAG_NAND "[NAND] "
#define LOG_TAG_BOOT "[BOOT] "
#define LOG_TAG_CTRL "[CTRL] "
#define LOG_TAG_FC   "[FC  ] "
#define LOG_TAG_SCH  "[SCH ] "
#define LOG_TAG_DSP  "[DSP ] "
#define LOG_TAG_ENG  "[ENG ] "
#define LOG_TAG_FMC  "[FMC ] "
#define LOG_TAG_DAT  "[DAT ] "
#define LOG_TAG_TIM  "[TIM ] "
#define LOG_TAG_RES  "[RES ] "
#define LOG_TAG_META "[META] "
#define LOG_TAG_WEAR "[WEAR] "
#define LOG_TAG_POOL "[POOL] "

#define LOGE(format, ...) log_printf(LOG_ERROR, "[ERR] " format, ##__VA_ARGS__)
#define LOGW(format, ...) log_printf(LOG_WARNING, "[WRN] " format, ##__VA_ARGS__)
#define LOGI(format, ...) log_printf(LOG_INFO, "[INF] " format, ##__VA_ARGS__)

#if defined(DEBUG)
#    define LOGD(format, ...) log_printf(LOG_DEBUG, "[DBG] " format, ##__VA_ARGS__)
#else
#    define LOGD(...) ((void)0)
#endif

/*
 * 条件日志：仅当 cond 为真时输出。格式参数写在 if 内部，cond 为假时完全不求值，
 * 成本模型与 LOGx(...) 一致（无多余格式化）。
 * 用途：分支体**只有日志**的场合（如"失败才打"），免去显式 if 块。
 * 注意：若分支体还有 return / 回退等语句，显式 if 必须保留，只把日志那句换成本宏。
 */
#define LOG_IF(level, cond, format, ...)              \
    do                                                \
    {                                                 \
        if (cond)                                     \
        {                                             \
            log_printf(level, format, ##__VA_ARGS__); \
        }                                             \
    } while (0)

#define LOGE_IF(cond, format, ...) LOG_IF(LOG_ERROR, cond, "[ERR] " format, ##__VA_ARGS__)
#define LOGW_IF(cond, format, ...) LOG_IF(LOG_WARNING, cond, "[WRN] " format, ##__VA_ARGS__)
#define LOGI_IF(cond, format, ...) LOG_IF(LOG_INFO, cond, "[INF] " format, ##__VA_ARGS__)
#if defined(DEBUG)
#    define LOGD_IF(cond, format, ...) LOG_IF(LOG_DEBUG, cond, "[DBG] " format, ##__VA_ARGS__)
#else
#    define LOGD_IF(cond, ...) ((void)0)
#endif

#endif /* LOG_H */
