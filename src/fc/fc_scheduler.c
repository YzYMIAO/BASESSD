#include "fc_scheduler.h"

#include "fc.h"

#include <stddef.h>
#include <string.h>

fc_status_t fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq)
{
    if ((scheduler == NULL) || (sq == NULL) || (hsq == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    scheduler->sq  = sq;
    scheduler->hsq = hsq;
    memset(scheduler->busy, 0, sizeof(scheduler->busy));
    return FC_STATUS_OK;
}

static uint8_t fc_scheduler_target_is_busy(const fc_scheduler_t *scheduler, fc_command_id_t command_id)
{
    const fc_command_t *command;

    command = fc_command_id_to_command(command_id);
    if (command == NULL)
    {
        return 1U;
    }
    return scheduler->busy[command->config.ch][command->config.ce][command->config.lun];
}

fc_status_t fc_scheduler_select(fc_scheduler_t *scheduler, fc_command_id_t *command_id)
{
    uint16_t index;

    if ((scheduler == NULL) || (command_id == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    /* HSQ 优先 */
    if (scheduler->hsq->count != 0U)
    {
        for (index = 0U; index < scheduler->hsq->count; index++)
        {
            fc_command_id_t head_id = scheduler->hsq->entries[scheduler->hsq->head];
            if (fc_scheduler_target_is_busy(scheduler, head_id) == 0U)
            {
                return fc_submission_queue_pop(scheduler->hsq, command_id);
            }
            /* 目标忙，移到队尾继续找 */
            fc_command_id_t skipped;
            (void)fc_submission_queue_pop(scheduler->hsq, &skipped);
            (void)fc_submission_queue_push(scheduler->hsq, skipped);
        }
        return FC_STATUS_NOT_READY;
    }
    /* 普通 SQ */
    for (index = 0U; index < scheduler->sq->count; index++)
    {
        fc_command_id_t head_id = scheduler->sq->entries[scheduler->sq->head];
        if (fc_scheduler_target_is_busy(scheduler, head_id) == 0U)
        {
            return fc_submission_queue_pop(scheduler->sq, command_id);
        }
        fc_command_id_t skipped;
        (void)fc_submission_queue_pop(scheduler->sq, &skipped);
        (void)fc_submission_queue_push(scheduler->sq, skipped);
    }
    return FC_STATUS_QUEUE_EMPTY;
}

void fc_scheduler_set_busy(fc_scheduler_t *scheduler, uint8_t ch, uint8_t ce, uint8_t lun)
{
    if ((scheduler != NULL) && (ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM))
    {
        scheduler->busy[ch][ce][lun] = 1U;
    }
}

void fc_scheduler_clear_busy(fc_scheduler_t *scheduler, uint8_t ch, uint8_t ce, uint8_t lun)
{
    if ((scheduler != NULL) && (ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM))
    {
        scheduler->busy[ch][ce][lun] = 0U;
    }
}
