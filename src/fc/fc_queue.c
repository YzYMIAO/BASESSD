#include "fc_queue.h"

#include <stddef.h>

fc_status_t fc_submission_queue_init(fc_submission_queue_t *queue, fc_command_id_t *storage, uint16_t depth)
{
    if ((queue == NULL) || (storage == NULL) || (depth == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    *queue = (fc_submission_queue_t){storage, depth, 0U, 0U, 0U};
    return FC_STATUS_OK;
}

fc_status_t fc_completion_queue_init(fc_completion_queue_t *queue, fc_command_status_t *storage, uint16_t depth)
{
    if ((queue == NULL) || (storage == NULL) || (depth == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    *queue = (fc_completion_queue_t){storage, depth, 0U, 0U, 0U};
    return FC_STATUS_OK;
}

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
    queue->head     = (uint16_t)((queue->head + 1U) % queue->depth);
    queue->count--;
    return FC_STATUS_OK;
}

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
