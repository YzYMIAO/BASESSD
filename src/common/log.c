#include "log.h"

#include "platform.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* This filter applies before formatting, so suppressed logs consume no TX queue space. */
static log_level_e      g_log_level = LOG_INFO;
static log_sink_t       g_log_sink;
static log_flush_sink_t g_log_flush_sink;
static DWORD            g_log_truncated;
static DWORD            g_log_write_failures;

DWORD log_write(log_level_e level, const char *message)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();
    DWORD                     length;
    DWORD                     result;

    if ((message == NULL) || (level > g_log_level))
    {
        return 0U;
    }

    length = strlen(message);

    /* An explicitly registered sink lets a bare-metal BSP log before platform registration. */
    if (g_log_sink != NULL)
    {
        result = g_log_sink(message, length);
    }
    else if ((platform != NULL) && (platform->log_write != NULL))
    {
        platform->log_write(message);
        result = length;
    }
    else
    {
        return LOG_WRITE_INVALID;
    }

    if (result != length)
    {
        g_log_write_failures++;
        return LOG_WRITE_INVALID;
    }
    return result;
}

DWORD log_vprintf(log_level_e level, const char *format, va_list args)
{
    char message[192];
    int  length;

    if ((format == NULL) || (level > g_log_level))
    {
        return 0U;
    }

    length = vsnprintf(message, sizeof(message), format, args);
    if (length < 0)
    {
        return LOG_WRITE_INVALID;
    }
    if (length == 0)
    {
        return 0U;
    }
    if ((size_t)length >= sizeof(message))
    {
        static const char marker[]      = "...[truncated]";
        const size_t      marker_length = sizeof(marker) - 1U;
        const size_t      marker_offset = sizeof(message) - marker_length - 1U;

        g_log_truncated++;
        (void)memcpy(&message[marker_offset], marker, marker_length);
        message[sizeof(message) - 1U] = '\0';
        length                        = (int)(sizeof(message) - 1U);
    }

    /* Pass already-formatted text to the platform sink to preserve transport independence. */
    return log_write(level, message);
}

DWORD log_printf(log_level_e level, const char *format, ...)
{
    DWORD   length;
    va_list args;

    va_start(args, format);
    length = log_vprintf(level, format, args);
    va_end(args);
    return length;
}

void log_set_level(log_level_e level)
{
    if (level <= LOG_DEBUG)
    {
        g_log_level = level;
    }
}

log_level_e log_get_level(void)
{
    return g_log_level;
}

void log_set_sink(log_sink_t sink)
{
    g_log_sink = sink;
}

bool log_flush(DWORD timeout_ms)
{
    return (g_log_flush_sink != NULL) ? g_log_flush_sink(timeout_ms) : true;
}

void log_set_flush_sink(log_flush_sink_t sink)
{
    g_log_flush_sink = sink;
}

DWORD log_get_truncated_count(void)
{
    return g_log_truncated;
}

DWORD log_get_write_failure_count(void)
{
    return g_log_write_failures;
}
