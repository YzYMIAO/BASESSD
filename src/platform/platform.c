#include "platform/platform.h"

static const ssd_platform_ops_t *g_platform_ops;

int ssd_platform_register(const ssd_platform_ops_t *ops)
{
    if ((ops == NULL) || (ops->time_us == NULL)) {
        return -1;
    }

    g_platform_ops = ops;
    return 0;
}

const ssd_platform_ops_t *ssd_platform_get(void)
{
    return g_platform_ops;
}
