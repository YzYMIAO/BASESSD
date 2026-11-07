/*
 * host_queue.c — HIL 主机请求队列 / tag 表（见 host_queue.h）
 *
 * 这不是 NVMe SQ/CQ 的软件模型：BOT 串行下没有 doorbell/多队列。它只负责
 * 「请求归属 + tag 分配 + 单一生命周期」，让 USB MSC 和 UART CLI 不再各自持有
 * 一份 ssd_req_s。接 NVMe 时在本模块上扩展环形队列/完成队列。
 */

#include "host_queue.h"

#include "assertion.h"

#include <string.h>

/**
 * @brief 清零整张 host queue 表。
 *
 * @param queue 队列（由 ssd_ctl_s 持有）
 */
void ssd_host_queue_init(ssd_host_queue_s *queue)
{
    ASSERT(queue != NULL);
    memset(queue, 0, sizeof(*queue));
}

/**
 * @brief 取一个空闲 entry（tag = 数组下标）。
 *
 * request 字段不清零——调用者按 H47 先 memset / 逐字段赋值。
 *
 * @param queue 队列
 * @param owner 占用前端（USB / CLI；NONE 非法）
 * @return 空闲 entry；表满返回 NULL
 */
ssd_host_entry_s *ssd_host_queue_alloc(ssd_host_queue_s *queue, ssd_host_owner_e owner)
{
    ASSERT(queue != NULL);
    ASSERT((owner > SSD_HOST_OWNER_NONE) && (owner < SSD_HOST_OWNER_COUNT));

    for (BYTE i = 0U; i < SSD_HOST_QUEUE_DEPTH; i++)
    {
        ssd_host_entry_s *entry = &queue->entries[i];

        if (entry->in_use == false)
        {
            entry->in_use = true;
            entry->tag    = i;
            entry->owner  = (BYTE)owner;
            queue->used++;
            return entry;
        }
    }

    return NULL;
}

/**
 * @brief 归还一个已分配的 entry，并清零其 request。
 *
 * @param queue 队列
 * @param entry 必须是 in_use 的 entry
 */
void ssd_host_queue_free(ssd_host_queue_s *queue, ssd_host_entry_s *entry)
{
    ASSERT(queue != NULL);
    ASSERT(entry != NULL);
    ASSERT(entry->in_use == true);
    ASSERT(queue->used > 0U);

    memset(entry, 0, sizeof(*entry));
    queue->used--;
}
