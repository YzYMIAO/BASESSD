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

/**
 * @brief 初始化提交队列（SQ / HSQ 共用）
 *
 * 存储由调用方提供，本模块不分配内存；单线程访问（fc_service 内），无锁。
 *
 * @param queue   队列
 * @param storage 条目存储（depth 项）
 * @param depth   容量（非 0）
 * @note 空指针 / depth=0 由 ASSERT 拦截。
 */
void fc_submission_queue_init(fc_sub_queue_s *queue, fc_cmd_id_t *storage, WORD depth)
{
    ASSERT(queue != NULL);
    ASSERT(storage != NULL);
    ASSERT(depth != 0U);
    queue->entries = storage;
    queue->depth   = depth;
    queue->head    = 0U;
    queue->tail    = 0U;
    queue->count   = 0U;
}
/**
 * @brief 初始化完成队列
 *
 * @param queue   队列
 * @param storage 条目存储（depth 项）
 * @param depth   容量（非 0）
 * @note 空指针 / depth=0 由 ASSERT 拦截。
 */
void fc_completion_queue_init(fc_comp_queue_s *queue, fc_cmd_status_s *storage, WORD depth)
{
    ASSERT(queue != NULL);
    ASSERT(storage != NULL);
    ASSERT(depth != 0U);
    queue->entries = storage;
    queue->depth   = depth;
    queue->head    = 0U;
    queue->tail    = 0U;
    queue->count   = 0U;
}

/**
 * @brief 命令 ID 入队
 *
 * 入队 ID 全由 FC 自铸，传入 INVALID 哨兵即调用者 bug；只有队列满是预期结果。
 *
 * @param queue      提交队列
 * @param cmd_id 命令 ID（≠ FC_CMD_ID_INVALID）
 * @return FC_STATUS_OK / FC_STATUS_QUEUE_FULL
 */
fc_status_e fc_submission_queue_push(fc_sub_queue_s *queue, fc_cmd_id_t cmd_id)
{
    if (queue->count == queue->depth)
    {
        return FC_STATUS_QUEUE_FULL;
    }
    queue->entries[queue->tail] = cmd_id;
    queue->tail                 = ((queue->tail + 1U) % queue->depth);
    queue->count++;
    return FC_STATUS_OK;
}
/**
 * @brief 命令 ID 出队
 *
 * @param queue      提交队列
 * @param cmd_id 输出：命令 ID
 * @return FC_STATUS_OK / FC_STATUS_QUEUE_EMPTY
 */
fc_status_e fc_submission_queue_pop(fc_sub_queue_s *queue, fc_cmd_id_t *cmd_id)
{
    ASSERT(cmd_id != NULL);
    if (queue->count == 0U)
    {
        return FC_STATUS_QUEUE_EMPTY;
    }
    *cmd_id     = queue->entries[queue->head];
    queue->head = ((queue->head + 1U) % queue->depth);
    queue->count--;
    return FC_STATUS_OK;
}
/**
 * @brief 完成项入队
 *
 * @param queue      完成队列
 * @param completion 完成项（按值复制）
 * @return FC_STATUS_OK / FC_STATUS_QUEUE_FULL
 */
fc_status_e fc_completion_queue_push(fc_comp_queue_s *queue, const fc_cmd_status_s *completion)
{
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
/**
 * @brief 完成项出队
 *
 * @param queue      完成队列
 * @param completion 输出：完成项
 * @return FC_STATUS_OK / FC_STATUS_QUEUE_EMPTY
 */
fc_status_e fc_completion_queue_pop(fc_comp_queue_s *queue, fc_cmd_status_s *completion)
{
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
/**
 * @brief 只读查看队列第 offset 项（不移动 head）
 *
 * @param queue  提交队列
 * @param offset 相对队首的偏移
 * @return 命令 ID；offset 越界返回 FC_CMD_ID_INVALID
 */
fc_cmd_id_t fc_submission_queue_peek(const fc_sub_queue_s *queue, WORD offset)
{
    // 值域校验保留返回码：offset 越界返回 INVALID 哨兵（调用方按哨兵判链尾）
    if (offset >= queue->count)
    {
        return FC_CMD_ID_INVALID;
    }
    WORD index = ((queue->head + offset) % queue->depth);
    return queue->entries[index];
}
