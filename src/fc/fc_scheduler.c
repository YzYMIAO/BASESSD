#include "fc_scheduler.h"

#include <stddef.h>

fc_status_t fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq)
{
    if ((scheduler == NULL) || (sq == NULL) || (hsq == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    scheduler->sq  = sq;
    scheduler->hsq = hsq;
    return FC_STATUS_OK;
}

fc_status_t fc_scheduler_select(fc_scheduler_t *scheduler, fc_command_handle *command_handle)
{
    if ((scheduler == NULL) || (command_handle == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (scheduler->hsq->count != 0U)
    {
        /* Read retry、恢复等高优先级控制命令可以抢占普通 SQ。 */
        return fc_submission_queue_pop(scheduler->hsq, command_handle);
    }
    return fc_submission_queue_pop(scheduler->sq, command_handle);
}
