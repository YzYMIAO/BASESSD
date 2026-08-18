#ifndef SSD_LOG_H
#define SSD_LOG_H

/* 日志级别由平台层决定如何输出，例如终端或 UART。 */
typedef enum
{
    SSD_LOG_ERROR = 0,
    SSD_LOG_WARNING,
    SSD_LOG_INFO,
    SSD_LOG_DEBUG
} ssd_log_level_t;

void ssd_log_write(ssd_log_level_t level, const char *message);

#endif /* SSD_LOG_H */
