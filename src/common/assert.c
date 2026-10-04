#include "common/ssd_assert.h"
#include "common/ssd_crash.h"
#include "common/ssd_log.h"

#include <stdlib.h>

#define SSD_ASSERT_FLUSH_TIMEOUT_MS 1000U

void ssd_assert_fail(const char *expression, const char *file, int line)
{
    ssd_crash_record_assert(expression, file, (uint32_t)line);
    SSD_LOGE("assertion failed: %s (%s:%d)\r\n", expression, file, line);
    /* A fault line is useful only if the asynchronous transport is drained before stopping. */
    (void)ssd_log_flush(SSD_ASSERT_FLUSH_TIMEOUT_MS);
    /*
     * 异步日志排空后再走平台轮询 dump（它会关掉 TX DMA/中断，独占 UART），
     * 顺序反了会把还没发出去的日志一起掐掉。
     */
    ssd_crash_platform_report(ssd_crash_record_get());

#if defined(SSD_FIRMWARE_BUILD)
    /* A running IWDG turns this terminal state into a bounded, diagnosable reset. */
    for (;;)
    {
    }
#else
    abort();
#endif
}
