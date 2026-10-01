/* 验证主机请求，并把多块请求拆分为 FTL 子请求。 */

#include "controller/controller.h"
#include "ftl/ftl.h"

/* 外部：controller.c — 按 LBA 路由到 FTL 实例（内部函数，request.c 拆分循环共用） */
ftl_context_t *ssd_controller_route(const ssd_controller_t *controller, uint64_t lba);

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

/* ── 拆分辅助 ── */

/*
 * ssd_split_use_multi_plane - 判断位置 i 的两个连续 LBA 是否可用 Multi-Plane 双页
 *
 * Multi-Plane（pages=2）的 FC desc 双 Plane 属于同一 (ch,ce,lun)：
 * 连续两 LBA 必须路由到同一 FTL 实例才有效。跨 region 边界切实例
 *（(lba>>9) 进位，如 LBA 511/512）必须降级为两个单 Plane 子请求。
 * 用"路由相等"而非"region 相等"判断——换哈希策略后自动跟随。
 *
 * @param controller Controller 上下文
 * @param lba_i      当前块 LBA（全局）
 * @param has_next   1=存在下一块（i+1 < block_count）
 * @return 1=可用双页 Multi-Plane，0=降级单 Plane
 */
static uint8_t ssd_split_use_multi_plane(ssd_controller_t *controller,
                                         uint64_t          lba_i,
                                         uint32_t          has_next)
{
    ftl_context_t *ftl;
    ftl_context_t *ftl_next;

    if (has_next == 0U)
    {
        return 0U;
    }

    ftl      = ssd_controller_route(controller, lba_i);
    ftl_next = ssd_controller_route(controller, lba_i + 1U);

    /* 任一路由失败（防御性，正常不可达）→ 不配对，由 submit 循环单独报错 */
    if ((ftl == NULL) || (ftl_next == NULL))
    {
        return 0U;
    }
    return (uint8_t)(ftl == ftl_next);
}

/*
 * ssd_split_count - 预统计拆分出的子请求数
 *
 * 必须与 submit 循环使用相同的配对条件：跨实例降级会使子请求数
 * 多于 ceil(block_count/2)（如 2 块跨 region 边界 → 2 个子请求），
 * 不能用公式硬算。且 ftl_submit_read 对未映射 LBA 会同步完成
 *（on_sub_complete 立即触发），sub_remaining 必须先精确置位再提交，
 * 否则计数为 0 时同步回调会把父请求提前置完成。
 *
 * @param controller Controller 上下文
 * @param request    父请求（block_count >= 2）
 * @return 子请求总数
 */
static uint32_t ssd_split_count(ssd_controller_t *controller, ssd_request_t *request)
{
    uint32_t i;
    uint32_t count = 0U;

    for (i = 0U; i < request->block_count; )
    {
        count++;
        if (ssd_split_use_multi_plane(controller, request->start_lba + i,
                                      (uint32_t)(i + 1U < request->block_count)) != 0U)
        {
            i += 2U;
        }
        else
        {
            i++;
        }
    }
    return count;
}

int ssd_request_split(ssd_controller_t *controller, ssd_request_t *request)
{
    uint32_t i;

    if ((controller == NULL) || (request == NULL) || (request->block_count <= 1U))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 先精确统计子请求数（跨实例降级会使数量 > ceil(block_count/2)），
     * 置位后再提交；未映射读的同步完成会立即递减该计数。 */
    request->sub_remaining = ssd_split_count(controller, request);
    if (request->sub_remaining == 0U)
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 子请求公共字段（循环内复用 io 模板，仅改 lba/buffer/pages） */
    request->io.complete_flag   = NULL;               /* 子请求用 on_complete 聚合 */
    request->io.status_out      = &request->completion_status;
    request->io.on_complete     = on_sub_complete;
    request->io.on_complete_ctx = request;

    switch (request->opcode)
    {
        case SSD_REQUEST_READ:
            /*
             * 读拆分：两两连续 LBA 配对为 Multi-Plane 读（pages=2），
             * 剩余的奇数个走单 Plane 读（pages=1）。
             * 连续 LBA 路由到同一 FTL 实例才配对，跨实例（region 边界）
             * 降级为两个单 Plane 子请求，各路由各的实例。
             */
            for (i = 0U; i < request->block_count; )
            {
                uint8_t      *buf = (uint8_t *)request->buffer + (i * FTL_READ_USER_SIZE);
                ftl_context_t *ftl;

                /* 路由前先赋值 io.lba：路由即按本次子请求的 LBA 选实例 */
                request->io.lba    = request->start_lba + i;
                request->io.buffer = buf;

                ftl = ssd_controller_route(controller, request->io.lba);
                if (ftl == NULL)
                {
                    request->completion_status = SSD_STATUS_IO_ERROR;
                    request->complete_flag     = 1U;
                    return SSD_STATUS_IO_ERROR;
                }

                if (ssd_split_use_multi_plane(controller, request->io.lba,
                                              (uint32_t)(i + 1U < request->block_count)) != 0U)
                {
                    /* 双 Plane Multi-Plane 读（连续 LBA 同实例） */
                    request->io.pages = 2U;
                    if (ftl_submit_read(ftl, &request->io) != FTL_OK)
                    {
                        request->completion_status = SSD_STATUS_IO_ERROR;
                        request->complete_flag     = 1U;
                        return SSD_STATUS_IO_ERROR;
                    }
                    i += 2U;
                }
                else
                {
                    /* 单 Plane 读（奇数块 / 跨实例降级） */
                    request->io.pages = 1U;
                    if (ftl_submit_read(ftl, &request->io) != FTL_OK)
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
             * 与读拆分同构：连续 LBA 同实例才配对，跨实例降级单 Plane。
             */
            for (i = 0U; i < request->block_count; )
            {
                const uint8_t *data = (const uint8_t *)request->buffer + (i * FTL_READ_USER_SIZE);
                ftl_context_t  *ftl;

                request->io.lba    = request->start_lba + i;
                request->io.buffer = (void *)data;

                ftl = ssd_controller_route(controller, request->io.lba);
                if (ftl == NULL)
                {
                    request->completion_status = SSD_STATUS_NO_SPACE;
                    request->complete_flag     = 1U;
                    return SSD_STATUS_NO_SPACE;
                }

                if (ssd_split_use_multi_plane(controller, request->io.lba,
                                              (uint32_t)(i + 1U < request->block_count)) != 0U)
                {
                    /* 双 Plane Multi-Plane 写（连续 LBA 同实例） */
                    request->io.pages = 2U;
                    if (ftl_submit_write(ftl, &request->io) != FTL_OK)
                    {
                        request->completion_status = SSD_STATUS_NO_SPACE;
                        request->complete_flag     = 1U;
                        return SSD_STATUS_NO_SPACE;
                    }
                    i += 2U;
                }
                else
                {
                    /* 单 Plane 写（奇数块 / 跨实例降级） */
                    request->io.pages = 1U;
                    if (ftl_submit_write(ftl, &request->io) != FTL_OK)
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