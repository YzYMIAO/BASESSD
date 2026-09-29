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
    if ((controller == NULL) || (request == NULL) || (controller->state != SSD_CONTROLLER_READY))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 多块请求当前不支持 */
    if (request->block_count != 1U)
    {
        request->completion_status = SSD_STATUS_NOT_SUPPORTED;
        request->complete_flag     = 1U;
        return SSD_STATUS_NOT_SUPPORTED;
    }

    /* 清零完成标志 */
    request->complete_flag     = 0U;
    request->completion_status = SSD_STATUS_OK;

    switch (request->opcode)
    {
        case SSD_REQUEST_READ:
            /* 非阻塞提交读请求 */
            if (ftl_submit_read(controller->ftl,
                                request->start_lba,
                                request->buffer,
                                1U) != FTL_OK)
            {
                request->completion_status = SSD_STATUS_IO_ERROR;
                request->complete_flag     = 1U;
            }
            /* 成功时 complete_flag 由 FTL completion handler 置 1 */
            break;

        case SSD_REQUEST_WRITE:
            /* 非阻塞提交写请求（complete_flag 在 NAND PROGRAM 完成后置 1） */
            if (ftl_submit_write(controller->ftl,
                                 request->start_lba,
                                 request->buffer,
                                 1U,
                                 &request->complete_flag) != FTL_OK)
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

    return request->completion_status;
}