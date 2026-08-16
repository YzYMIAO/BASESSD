#include <stddef.h>

#include "common/ssd_log.h"
#include "platform/platform.h"

void ssd_log_write(ssd_log_level_t level, const char *message)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();

    (void)level;

    if ((message != NULL) && (platform != NULL) &&
        (platform->log_write != NULL)) {
        platform->log_write(message);
    }
}
