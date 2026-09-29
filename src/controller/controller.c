#include "controller/controller.h"

#include <stddef.h>

/* 外部：request.c — 拆分多块请求 */
int ssd_request_split(ssd_controller_t *controller, ssd_request_t *request);

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

uint8_t ssd_controller_is_complete(const ssd_request_t *request)
{
    if (request == NULL)
    {
        return 1U;
    }
    return request->complete_flag;
}

int ssd_controller_submit(ssd_controller_t *controller, ssd_request_t *request)
{
    int ret;

    if ((controller == NULL) || (request == NULL) || (controller->state != SSD_CONTROLLER_READY))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 清零完成标志 */
    request->complete_flag     = 0U;
    request->completion_status = SSD_STATUS_OK;

    if (request->block_count == 1U)
    {
        /* 单块直接提交 */
        switch (request->opcode)
        {
            case SSD_REQUEST_READ:
                if (ftl_submit_read(controller->ftl,
                                    request->start_lba,
                                    request->buffer,
                                    1U,                 /* pages=1: 单页读 */
                                    &request->complete_flag,
                                    &request->completion_status,
                                    NULL, NULL) != FTL_OK)
                {
                    request->completion_status = SSD_STATUS_IO_ERROR;
                    request->complete_flag     = 1U;
                }
                break;

            case SSD_REQUEST_WRITE:
                if (ftl_submit_write(controller->ftl,
                                     request->start_lba,
                                     request->buffer,
                                     1U,                 /* pages=1: 单页写 */
                                     &request->complete_flag,
                                     &request->completion_status,
                                     NULL, NULL) != FTL_OK)
                {
                    request->completion_status = SSD_STATUS_NO_SPACE;
                    request->complete_flag     = 1U;
                }
                break;

            case SSD_REQUEST_TRIM:
                ftl_trim(controller->ftl, request->start_lba);
                request->completion_status = SSD_STATUS_OK;
                request->complete_flag     = 1U;
                break;

            case SSD_REQUEST_FLUSH:
            default:
                request->completion_status = SSD_STATUS_NOT_SUPPORTED;
                request->complete_flag     = 1U;
                break;
        }
    }
    else
    {
        /* 多块 → 拆分提交 */
        ret = ssd_request_split(controller, request);
        if (ret != SSD_STATUS_OK)
        {
            request->completion_status = (ssd_status_t)ret;
            request->complete_flag     = 1U;
            return ret;
        }
    }

    return request->completion_status;
}