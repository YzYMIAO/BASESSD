/*
 * fc_assert_context.c — 断言停机前的 FC 现场补充
 *
 * 覆盖 assertion.c 里的弱钩子 assertion_platform_context()，在断言终端路径
 * （flush 之前）追加打印 FC 资源池占用，便于定位「资源泄漏导致的断言」。
 *
 * 依赖方向 FC → assertion：本文件认识断言设施，断言设施不认识 FC。
 * （它最初内联在 munit.c 里，属于非上游移植件，随断言系统重建迁出。）
 */

#include "assertion.h"
#include "fc.h"
#include "log.h"

void assertion_platform_context(void)
{
    fc_resource_stats_t stats;

    if (fc_get_resource_stats(&stats) != FC_STATUS_OK)
    {
        return;
    }

    LOGE(LOG_TAG_RES "cmd=%u/%u desc=%u/%u buf=%u/%u sq=%u hsq=%u cq=%u\r\n",
         (unsigned)stats.command_used,
         (unsigned)FC_COMMAND_POOL_DEPTH,
         (unsigned)stats.descriptor_used,
         (unsigned)FC_DESCRIPTOR_POOL_DEPTH,
         (unsigned)stats.buffer_used,
         (unsigned)FC_BUFFER_POOL_DEPTH,
         (unsigned)stats.sq_used,
         (unsigned)stats.hsq_used,
         (unsigned)stats.cq_used);
}
