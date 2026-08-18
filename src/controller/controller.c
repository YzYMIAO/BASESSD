#include "controller/controller.h"

#include <stddef.h>

int ssd_controller_init(ssd_controller_t *controller, ftl_context_t *ftl)
{
    if ((controller == NULL) || (ftl == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    controller->state = SSD_CONTROLLER_INITIALIZING;
    controller->ftl   = ftl;
    controller->state = SSD_CONTROLLER_READY;
    return SSD_STATUS_OK;
}

int ssd_controller_submit(ssd_controller_t *controller, ssd_request_t *request)
{
    if ((controller == NULL) || (request == NULL) || (controller->state != SSD_CONTROLLER_READY))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 第一版仅处理单 LBA，多块请求后续由调度器拆分。 */
    if (request->block_count != 1U)
    {
        return SSD_STATUS_NOT_SUPPORTED;
    }

    switch (request->opcode)
    {
        case SSD_REQUEST_READ:
            request->completion_status = ftl_read(controller->ftl, request->start_lba, request->buffer);
            break;
        case SSD_REQUEST_WRITE:
            request->completion_status = ftl_write(controller->ftl, request->start_lba, request->buffer);
            break;
        case SSD_REQUEST_TRIM:
            request->completion_status = ftl_trim(controller->ftl, request->start_lba);
            break;
        case SSD_REQUEST_FLUSH:
        default:
            request->completion_status = SSD_STATUS_NOT_SUPPORTED;
            break;
    }

    return request->completion_status;
}
