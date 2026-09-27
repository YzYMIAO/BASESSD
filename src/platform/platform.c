#include "platform/platform.h"

#include "common/common.h"

static const ssd_platform_ops_t *g_platform_ops;

int ssd_platform_register(const ssd_platform_ops_t *ops)
{
    if ((ops == NULL) || (ops->time_us == NULL))
    {
        return -1;
    }

    g_platform_ops = ops;
    return FUNC_STATUS_SUCCESS;
}

const ssd_platform_ops_t *ssd_platform_get(void)
{
    return g_platform_ops;
}
