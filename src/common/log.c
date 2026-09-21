#include "common/ssd_log.h"
#include "platform/platform.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* This filter applies before formatting, so suppressed logs consume no TX queue space. */
static ssd_log_level_t      g_ssd_log_level = SSD_LOG_INFO;
static ssd_log_sink_t       g_ssd_log_sink;
static ssd_log_flush_sink_t g_ssd_log_flush_sink;
static uint32_t             g_ssd_log_truncated;
static uint32_t             g_ssd_log_write_failures;

int ssd_log_write(ssd_log_level_t level, const char *message)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();
    uint32_t                  length;
    int                       result;

    if ((message == NULL) || (level > g_ssd_log_level))
    {
        return 0;
    }

    length = (uint32_t)strlen(message);

    /* An explicitly registered sink lets a bare-metal BSP log before platform registration. */
    if (g_ssd_log_sink != NULL)
    {
        result = g_ssd_log_sink(message, length);
    }
    else if ((platform != NULL) && (platform->log_write != NULL))
    {
        platform->log_write(message);
        result = (int)length;
    }
    else
    {
        result = -1;
    }

    if (result != (int)length)
    {
        g_ssd_log_write_failures++;
        return (result < 0) ? result : -1;
    }
    return result;
}

int ssd_log_vprintf(ssd_log_level_t level, const char *format, va_list args)
{
    char message[192];
    int  length;

    if ((format == NULL) || (level > g_ssd_log_level))
    {
        return 0;
    }

    length = vsnprintf(message, sizeof(message), format, args);
    if (length <= 0)
    {
        return length;
    }
    if ((size_t)length >= sizeof(message))
    {
        static const char marker[]      = "...[truncated]";
        const size_t      marker_length = sizeof(marker) - 1U;
        const size_t      marker_offset = sizeof(message) - marker_length - 1U;

        g_ssd_log_truncated++;
        (void)memcpy(&message[marker_offset], marker, marker_length);
        message[sizeof(message) - 1U] = '\0';
        length                        = (int)(sizeof(message) - 1U);
    }

    /* Pass already-formatted text to the platform sink to preserve transport independence. */
    return ssd_log_write(level, message);
}

int ssd_log_printf(ssd_log_level_t level, const char *format, ...)
{
    int     length;
    va_list args;

    va_start(args, format);
    length = ssd_log_vprintf(level, format, args);
    va_end(args);
    return length;
}

void ssd_log_set_level(ssd_log_level_t level)
{
    if (level <= SSD_LOG_DEBUG)
    {
        g_ssd_log_level = level;
    }
}

ssd_log_level_t ssd_log_get_level(void)
{
    return g_ssd_log_level;
}

void ssd_log_set_sink(ssd_log_sink_t sink)
{
    g_ssd_log_sink = sink;
}

int ssd_log_flush(uint32_t timeout_ms)
{
    return (g_ssd_log_flush_sink != NULL) ? g_ssd_log_flush_sink(timeout_ms) : 0;
}

void ssd_log_set_flush_sink(ssd_log_flush_sink_t sink)
{
    g_ssd_log_flush_sink = sink;
}

uint32_t ssd_log_get_truncated_count(void)
{
    return g_ssd_log_truncated;
}

uint32_t ssd_log_get_write_failure_count(void)
{
    return g_ssd_log_write_failures;
}
