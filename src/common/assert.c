#include "common/ssd_assert.h"
#include "common/ssd_log.h"

#include <stdlib.h>

void ssd_assert_fail(const char *expression, const char *file, int line)
{
    SSD_LOGE("assertion failed: %s (%s:%d)\n", expression, file, line);

#if defined(SSD_FIRMWARE_BUILD)
    /* A running IWDG turns this terminal state into a bounded, diagnosable reset. */
    for (;;)
    {
    }
#else
    abort();
#endif
}
