/* 验证主机请求，并把多块请求拆分为 FTL 子请求。 */

#include "controller/controller.h"
#include "ftl/ftl.h"

/* ── 子请求完成回调 ──
 *
 * 由 ftl_handle_completion 在 pending 表处理后调用。
 * 递减 sub_remaining 计数器，归零时置父请求 complete_flag。
 */
static void on_sub_complete(void *ctx)
{
    ssd_request_t *req = (ssd_request_t *)ctx;

    if (req->sub_remaining > 0U)
    {
        req->sub_remaining--;
    }

    /* 所有子请求完成 → 通知 host */
    if (req->sub_remaining == 0U)
    {
        req->complete_flag = 1U;
    }
}

int ssd_request_split(ssd_controller_t *controller, ssd_request_t *request)
{
    uint32_t i;

    if ((controller == NULL) || (request == NULL) || (request->block_count <= 1U))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    request->sub_remaining = (request->block_count + 1U) / 2U;

    switch (request->opcode)
    {
        case SSD_REQUEST_READ:
            /*
             * 读拆分：两两连续 LBA 配对为 Multi-Plane 读（pages=2），
             * 剩余的奇数个走单 Plane 读（pages=1）。
             */
            for (i = 0U; i < request->block_count; )
            {
                uint8_t *buf = (uint8_t *)request->buffer + (i * FTL_READ_USER_SIZE);

                if ((i + 1U < request->block_count))
                {
                    /* 双 Plane Multi-Plane 读 */
                    if (ftl_submit_read(controller->ftl,
                                        request->start_lba + i,
                                        buf,
                                        2U,
                                        NULL,               /* 子请求用 on_complete 聚合 */
                                        &request->completion_status,
                                        on_sub_complete, request) != FTL_OK)
                    {
                        request->completion_status = SSD_STATUS_IO_ERROR;
                        request->complete_flag     = 1U;
                        return SSD_STATUS_IO_ERROR;
                    }
                    i += 2U;
                }
                else
                {
                    /* 单 Plane 读 */
                    if (ftl_submit_read(controller->ftl,
                                        request->start_lba + i,
                                        buf,
                                        1U,
                                        NULL,
                                        &request->completion_status,
                                        on_sub_complete, request) != FTL_OK)
                    {
                        request->completion_status = SSD_STATUS_IO_ERROR;
                        request->complete_flag     = 1U;
                        return SSD_STATUS_IO_ERROR;
                    }
                    i++;
                }
            }
            break;

        case SSD_REQUEST_WRITE:
            /*
             * 写拆分：两两连续 LBA 配对为 Multi-Plane 写（pages=2），
             * Super Block 跨 Plane 同步，写入性能翻倍。
             * 剩余的奇数个走单 Plane 写（pages=1）。
             */
            for (i = 0U; i < request->block_count; )
            {
                const uint8_t *data = (const uint8_t *)request->buffer + (i * FTL_READ_USER_SIZE);

                if ((i + 1U < request->block_count))
                {
                    /* 双 Plane Multi-Plane 写（pages=2，数据 4096 字节） */
                    if (ftl_submit_write(controller->ftl,
                                         request->start_lba + i,
                                         data,
                                         2U,
                                         NULL,               /* 子请求用 on_complete 聚合 */
                                         &request->completion_status,
                                         on_sub_complete, request) != FTL_OK)
                    {
                        request->completion_status = SSD_STATUS_NO_SPACE;
                        request->complete_flag     = 1U;
                        return SSD_STATUS_NO_SPACE;
                    }
                    i += 2U;
                }
                else
                {
                    /* 单 Plane 写 */
                    if (ftl_submit_write(controller->ftl,
                                         request->start_lba + i,
                                         data,
                                         1U,
                                         NULL,
                                         &request->completion_status,
                                         on_sub_complete, request) != FTL_OK)
                    {
                        request->completion_status = SSD_STATUS_NO_SPACE;
                        request->complete_flag     = 1U;
                        return SSD_STATUS_NO_SPACE;
                    }
                    i++;
                }
            }
            break;

        default:
            return SSD_STATUS_NOT_SUPPORTED;
    }

    return SSD_STATUS_OK;
}