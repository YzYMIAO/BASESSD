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

#include "assertion.h"

#include <stddef.h>

// 判据（见 docs/design/fc_nand_design.md §24）：
// 指针判空 → ASSERT —— 传空指针是调用者的编程错误，不变量破坏就该留下现场；
// 值域校验 → 返回码 —— QUEUE_FULL / QUEUE_EMPTY / 越界 offset 是预期内的运行结果，
// 必须返回给上层（fc.c:709 就靠 QUEUE_FULL 做背压判断）；
// INVALID 哨兵入队 → ASSERT（见 docs/design/assertion_guidelines.md §十五）
// —— 入队 ID 由 FC 自铸，传哨兵是调用者 bug，非运行结果。

fc_status_e fc_submission_queue_init(fc_submission_queue_t *queue, fc_command_id_t *storage, WORD depth)
{
    ASSERT(queue != NULL);
    ASSERT(storage != NULL);
    ASSERT(depth != 0U);
    queue->entries = storage;
    queue->depth   = depth;
    queue->head    = 0U;
    queue->tail    = 0U;
    queue->count   = 0U;
    return FC_STATUS_OK;
}

fc_status_e fc_completion_queue_init(fc_completion_queue_t *queue, fc_command_status_t *storage, WORD depth)
{
    ASSERT(queue != NULL);
    ASSERT(storage != NULL);
    ASSERT(depth != 0U);
    queue->entries = storage;
    queue->depth   = depth;
    queue->head    = 0U;
    queue->tail    = 0U;
    queue->count   = 0U;
    return FC_STATUS_OK;
}

// 实现要点：INVALID 哨兵入队 = 调用者 bug（入队 ID 全由 FC 自铸，见 fc.c），
// 按判据 2 转 ASSERT；只有「队列满」才是预期内的运行结果。
fc_status_e fc_submission_queue_push(fc_submission_queue_t *queue, fc_command_id_t command_id)
{
    ASSERT(queue != NULL);
    ASSERT(command_id != FC_COMMAND_ID_INVALID);
    if (queue->count == queue->depth)
    {
        return FC_STATUS_QUEUE_FULL;
    }
    queue->entries[queue->tail] = command_id;
    queue->tail                 = ((queue->tail + 1U) % queue->depth);
    queue->count++;
    return FC_STATUS_OK;
}

fc_status_e fc_submission_queue_pop(fc_submission_queue_t *queue, fc_command_id_t *command_id)
{
    ASSERT(queue != NULL);
    ASSERT(command_id != NULL);
    if (queue->count == 0U)
    {
        return FC_STATUS_QUEUE_EMPTY;
    }
    *command_id = queue->entries[queue->head];
    queue->head = ((queue->head + 1U) % queue->depth);
    queue->count--;
    return FC_STATUS_OK;
}

fc_status_e fc_completion_queue_push(fc_completion_queue_t *queue, const fc_command_status_t *completion)
{
    ASSERT(queue != NULL);
    ASSERT(completion != NULL);
    if (queue->count == queue->depth)
    {
        return FC_STATUS_QUEUE_FULL;
    }
    queue->entries[queue->tail] = *completion;
    queue->tail                 = ((queue->tail + 1U) % queue->depth);
    queue->count++;
    return FC_STATUS_OK;
}

fc_status_e fc_completion_queue_pop(fc_completion_queue_t *queue, fc_command_status_t *completion)
{
    ASSERT(queue != NULL);
    ASSERT(completion != NULL);
    if (queue->count == 0U)
    {
        return FC_STATUS_QUEUE_EMPTY;
    }
    *completion = queue->entries[queue->head];
    queue->head = ((queue->head + 1U) % queue->depth);
    queue->count--;
    return FC_STATUS_OK;
}

fc_command_id_t fc_submission_queue_peek(const fc_submission_queue_t *queue, WORD offset)
{
    ASSERT(queue != NULL);
    // 值域校验保留返回码：offset 越界返回 INVALID 哨兵（调用方按哨兵判链尾）
    if (offset >= queue->count)
    {
        return FC_COMMAND_ID_INVALID;
    }
    WORD index = ((queue->head + offset) % queue->depth);
    return queue->entries[index];
}
