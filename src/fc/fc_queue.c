/*
 * fc_queue.c — SQ/HSQ/CQ 环形队列实现
 *
 * 三种队列都是"存储由调用方提供、不分配内存"的固定容量 FIFO。
 * SQ/HSQ 存 Command Pool ID；CQ 存完成项结构。
 * 环形语义：head 出队、tail 入队，(i+1)%depth 推进，count 计数。
 *
 * 注意：这些队列单线程访问（fc_service 内），无锁；若将来多线程
 * 访问需要加互斥。
 */

#include "fc_queue.h"

#include <stddef.h>

/* --------------------------------------------------------------------
 * fc_submission_queue_init
 *   功能：初始化 SQ 环形队列（storage 由调用方提供）。
 *   参数：queue —— 队列；storage —— 槽位数组；depth —— 容量。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT（空指针或 depth=0）。
 * -------------------------------------------------------------------- */
fc_status_t fc_submission_queue_init(fc_submission_queue_t *queue, fc_command_id_t *storage, uint16_t depth)
{
    if ((queue == NULL) || (storage == NULL) || (depth == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    *queue = (fc_submission_queue_t){storage, depth, 0U, 0U, 0U};
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_completion_queue_init
 *   功能：初始化 CQ 环形队列（storage 由调用方提供）。
 *   参数：queue —— 队列；storage —— 完成项数组；depth —— 容量。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT。
 * -------------------------------------------------------------------- */
fc_status_t fc_completion_queue_init(fc_completion_queue_t *queue, fc_command_status_t *storage, uint16_t depth)
{
    if ((queue == NULL) || (storage == NULL) || (depth == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    *queue = (fc_completion_queue_t){storage, depth, 0U, 0U, 0U};
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_submission_queue_push
 *   功能：命令 ID 入队尾。
 *   参数：queue —— 队列；command_id —— Command Pool 槽位号。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / QUEUE_FULL。
 * -------------------------------------------------------------------- */
fc_status_t fc_submission_queue_push(fc_submission_queue_t *queue, fc_command_id_t command_id)
{
    if ((queue == NULL) || (command_id == FC_COMMAND_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (queue->count == queue->depth)
    {
        return FC_STATUS_QUEUE_FULL;
    }
    queue->entries[queue->tail] = command_id;
    queue->tail                 = (uint16_t)((queue->tail + 1U) % queue->depth);
    queue->count++;
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_submission_queue_pop
 *   功能：从队头弹出命令 ID。
 *   参数：queue —— 队列；command_id —— 输出。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / QUEUE_EMPTY。
 * -------------------------------------------------------------------- */
fc_status_t fc_submission_queue_pop(fc_submission_queue_t *queue, fc_command_id_t *command_id)
{
    if ((queue == NULL) || (command_id == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (queue->count == 0U)
    {
        return FC_STATUS_QUEUE_EMPTY;
    }
    *command_id = queue->entries[queue->head];
    queue->head = (uint16_t)((queue->head + 1U) % queue->depth);
    queue->count--;
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_completion_queue_push
 *   功能：完成项入队尾（整个 fc_command_status_t 拷贝）。
 *   参数：queue —— 队列；completion —— 待入队完成项。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / QUEUE_FULL。
 * -------------------------------------------------------------------- */
fc_status_t fc_completion_queue_push(fc_completion_queue_t *queue, const fc_command_status_t *completion)
{
    if ((queue == NULL) || (completion == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (queue->count == queue->depth)
    {
        return FC_STATUS_QUEUE_FULL;
    }
    queue->entries[queue->tail] = *completion;
    queue->tail                 = (uint16_t)((queue->tail + 1U) % queue->depth);
    queue->count++;
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_completion_queue_pop
 *   功能：从队头弹出完成项。
 *   参数：queue —— 队列；completion —— 输出。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / QUEUE_EMPTY。
 * -------------------------------------------------------------------- */
fc_status_t fc_completion_queue_pop(fc_completion_queue_t *queue, fc_command_status_t *completion)
{
    if ((queue == NULL) || (completion == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (queue->count == 0U)
    {
        return FC_STATUS_QUEUE_EMPTY;
    }
    *completion = queue->entries[queue->head];
    queue->head = (uint16_t)((queue->head + 1U) % queue->depth);
    queue->count--;
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_submission_queue_peek
 *   功能：查看队头起第 offset 条命令 ID（不消耗）。
 *   参数：queue —— 队列；offset —— 偏移（0=队头）。
 *   返回：命令 ID；越界返回 FC_COMMAND_ID_INVALID。
 * -------------------------------------------------------------------- */
fc_command_id_t fc_submission_queue_peek(const fc_submission_queue_t *queue, uint16_t offset)
{
    if ((queue == NULL) || (offset >= queue->count))
    {
        return FC_COMMAND_ID_INVALID;
    }
    uint16_t index = (uint16_t)((queue->head + offset) % queue->depth);
    return queue->entries[index];
}

/* --------------------------------------------------------------------
 * fc_submission_queue_pop_n
 *   功能：从队头一次弹出 count 条命令到 ids[0..count-1]。
 *   参数：queue —— 队列；ids —— 输出（可为 NULL 仅消耗）；count —— 条数。
 *   返回：无。
 *   实现要点：用于调度器整链弹出；ids 为 NULL 时仅推进队列。
 * -------------------------------------------------------------------- */
void fc_submission_queue_pop_n(fc_submission_queue_t *queue, fc_command_id_t *ids, uint16_t count)
{
    fc_command_id_t cmd;
    uint16_t        i;

    if (queue == NULL)
    {
        return;
    }
    for (i = 0U; i < count; i++)
    {
        (void)fc_submission_queue_pop(queue, (ids != NULL) ? &ids[i] : &cmd);
    }
}
