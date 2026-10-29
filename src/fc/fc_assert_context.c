/*
 * fc_assert_context.c — 断言停机前的 FC 现场补充
 *
 * 覆盖 assertion.c 里的弱钩子 assertion_platform_context()，在断言终端路径
 * （flush 之前）追加打印 FC 资源池占用，便于定位「资源泄漏导致的断言」。
 *
 * 依赖方向 FC → assertion：本文件认识断言设施，断言设施不认识 FC。
 */

#include "assertion.h"
#include "fc.h"
#include "log.h"

/**
 * @brief 断言终端路径的 FC 现场补充：把各资源池占用与队列长度追加到日志。
 *
 * @note 覆盖 assertion.c 中的同名弱钩子（强符号），由 assertion_terminate 在
 * flush 之前调用；fc_get_resource_stats 失败时静默跳过，不干扰停机路径。
 */
void assertion_platform_context(void)
{
    fc_resource_stats_t stats;

    if (fc_get_resource_stats(&stats) != FC_STATUS_OK)
    {
        return;
    }

    LOGE(LOG_TAG_RES "cmd=%hu/%u desc=%hu/%u buf=%hu/%u sq=%hu hsq=%hu cq=%hu\r\n", stats.command_used, FC_COMMAND_POOL_DEPTH, stats.descriptor_used, FC_DESCRIPTOR_POOL_DEPTH, stats.buffer_used, FC_BUFFER_POOL_DEPTH, stats.sq_used, stats.hsq_used, stats.cq_used);
}
