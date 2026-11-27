/*
 * host_queue.c — HIL 主机请求队列 / tag 表（见 host_queue.h）
 *
 * 这不是 NVMe SQ/CQ 的软件模型：BOT 串行下没有 doorbell/多队列。它只负责
 * 「请求归属 + tag 分配 + 单一生命周期」，让 USB MSC 和 UART CLI 共用同一张表。
 * 接 NVMe 时在本模块上扩展环形队列/完成队列。
 */

#include "host_queue.h"

#include "assertion.h"

#include <string.h>

/**
 * @brief 占用计数 +1（调用者已确认 allows / 同 owner）
 *
 * @param queue 队列
 * @param owner 前端
 */
static void ssd_host_queue_gate_take(ssd_host_queue_s *queue, ssd_host_owner_e owner)
{
    ASSERT(queue->gate_count < 0xFFU);

    if (queue->gate_owner == SSD_HOST_OWNER_NONE)
    {
        queue->gate_owner = owner;
    }
    queue->gate_count++;
}

/**
 * @brief 当前前端能否占用 host gate（NONE 或 owner 自己持有）。
 *
 * @param queue 队列
 * @param owner 前端（USB / CLI；NONE 非法）
 * @return true = 可占用；false = 另一前端持有
 */
BOOL ssd_host_queue_gate_allows(const ssd_host_queue_s *queue, ssd_host_owner_e owner)
{
    ASSERT(queue != NULL);
    ASSERT((owner > SSD_HOST_OWNER_NONE) && (owner < SSD_HOST_OWNER_COUNT));

    return (queue->gate_owner == SSD_HOST_OWNER_NONE) || (queue->gate_owner == owner);
}

/**
 * @brief 是否有任一前端占用了 host gate。
 *
 * @param queue 队列
 * @return true = 有主机命令在飞
 */
BOOL ssd_host_queue_gate_busy(const ssd_host_queue_s *queue)
{
    ASSERT(queue != NULL);

    return (queue->gate_owner != SSD_HOST_OWNER_NONE) ? true : false;
}

/**
 * @brief 占用 host gate（同一前端可重复占用，计数递增）。
 *
 * @param queue 队列
 * @param owner 前端（USB / CLI）
 * @return true = 已占用；false = 另一前端持有
 */
BOOL ssd_host_queue_gate_acquire(ssd_host_queue_s *queue, ssd_host_owner_e owner)
{
    if (ssd_host_queue_gate_allows(queue, owner) == false)
    {
        return false;
    }

    ssd_host_queue_gate_take(queue, owner);
    return true;
}

/**
 * @brief 释放 host gate（与 acquire 一一配对）。
 *
 * @param queue 队列
 * @param owner 必须与当前 gate_owner 一致
 */
void ssd_host_queue_gate_release(ssd_host_queue_s *queue, ssd_host_owner_e owner)
{
    ASSERT(queue != NULL);
    ASSERT(queue->gate_count > 0U);
    /* 错主释放会静默改写 gate 会计：release 档也必须拒绝（判据 9），
     * 因此这里用 ASSERT_REJECT_VOID 而非仅 debug 的 ASSERT。 */
    ASSERT_REJECT_VOID(queue->gate_owner == owner, "host gate owner mismatch on release\r\n");

    queue->gate_count--;
    if (queue->gate_count == 0U)
    {
        queue->gate_owner = SSD_HOST_OWNER_NONE;
    }
}

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
