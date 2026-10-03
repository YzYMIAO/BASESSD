#include "controller/controller.h"

#include "ftl/journal.h"
#include "ftl/l2p_cache.h"

#include <stddef.h>

/* 外部：request.c — 拆分多块请求 */
int ssd_request_split(ssd_controller_t *controller, ssd_request_t *request);

/*
 * ssd_controller_route - 按全局 LBA 路由到对应 FTL 实例
 *
 * 使用 l2p_cache.h 的 lba_route_ch/ce/lun（region 粒度取模）选实例；
 * 后续换哈希策略只改此处。CH_NUM=CE_NUM=LUN_NUM=1 时恒返回 ftl[0][0][0]。
 * 非 static：request.c 拆分循环共用（前向声明在 request.c），不暴露进公共头。
 *
 * @param controller Controller 上下文（可为 NULL，返回 NULL）
 * @param lba        Host 全局 LBA（uint64_t）
 * @return 路由到的 FTL 实例指针；入参非法或索引越界返回 NULL
 */
ftl_context_t *ssd_controller_route(const ssd_controller_t *controller, uint64_t lba)
{
    uint32_t ch;
    uint32_t ce;
    uint32_t lun;

    if (controller == NULL)
    {
        return NULL;
    }

    ch  = lba_route_ch(lba);
    ce  = lba_route_ce(lba);
    lun = lba_route_lun(lba);

    /* 防御性索引越界：路由取模结果本应在 [0,N) 内，宏配置错误时兜底返回 NULL */
    if ((ch >= CH_NUM) || (ce >= CE_NUM) || (lun >= LUN_NUM))
    {
        return NULL;
    }

    return controller->ftl[ch][ce][lun];
}

int ssd_controller_init(ssd_controller_t *controller,
                        ftl_context_t    *ftl[CH_NUM][CE_NUM][LUN_NUM])
{
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;

    if ((controller == NULL) || (ftl == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 逐槽校验并绑定 FTL 实例：任一槽位缺失或槽位与实例 topo 不符即失败。
     * 状态机（C1）：入口 RESET → INITIALIZING，校验结果驱动 READY/FAILED。 */
    ssd_controller_state_transition(controller, SSD_CONTROLLER_INITIALIZING);

    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                if (ftl[ch][ce][lun] == NULL)
                {
                    ssd_controller_state_transition(controller, SSD_CONTROLLER_FAILED);
                    return SSD_STATUS_INVALID_ARGUMENT;
                }
                /* 槽位索引必须等于实例绑定拓扑：保证 LBA 路由与芯片定位语义一致 */
                if ((ftl[ch][ce][lun]->topo.ch != ch) ||
                    (ftl[ch][ce][lun]->topo.ce != ce) ||
                    (ftl[ch][ce][lun]->topo.lun != lun))
                {
                    ssd_controller_state_transition(controller, SSD_CONTROLLER_FAILED);
                    return SSD_STATUS_INVALID_ARGUMENT;
                }
                controller->ftl[ch][ce][lun] = ftl[ch][ce][lun];
            }
        }
    }

    ssd_controller_state_transition(controller, SSD_CONTROLLER_READY);
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
    int            ret;
    ftl_context_t *ftl;

    if ((controller == NULL) || (request == NULL) || (controller->state != SSD_CONTROLLER_READY))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* C1b：host IO 到来 → 后台让路（scheduler 钩子；实际预算由 ftl_service B3 判定） */
    (void)ssd_controller_schedule(controller, 1);

    /* 清零完成标志 */
    request->complete_flag     = 0U;
    request->completion_status = SSD_STATUS_OK;

    /* 组装 IO 请求模板（lba/buffer 由各路径覆盖） */
    request->io.complete_flag   = &request->complete_flag;
    request->io.status_out      = &request->completion_status;
    request->io.on_complete     = NULL;
    request->io.on_complete_ctx = NULL;

    if (request->block_count == 1U)
    {
        /* 单块直接提交：按块 LBA 路由到对应 FTL 实例 */
        request->io.lba    = request->start_lba;
        request->io.buffer = request->buffer;
        request->io.pages  = 1U;

        ftl = ssd_controller_route(controller, request->io.lba);
        if (ftl == NULL)
        {
            request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
            request->complete_flag     = 1U;
            return SSD_STATUS_INVALID_ARGUMENT;
        }

        switch (request->opcode)
        {
            case SSD_REQUEST_READ:
                if (ftl_submit_read(ftl, &request->io) != FTL_OK)
                {
                    request->completion_status = SSD_STATUS_IO_ERROR;
                    request->complete_flag     = 1U;
                }
                break;

            case SSD_REQUEST_WRITE:
                if (ftl_submit_write(ftl, &request->io) != FTL_OK)
                {
                    request->completion_status = SSD_STATUS_NO_SPACE;
                    request->complete_flag     = 1U;
                }
                break;

            case SSD_REQUEST_TRIM:
                ftl_trim(ftl, request->start_lba);
                request->completion_status = SSD_STATUS_OK;
                request->complete_flag     = 1U;
                break;

            case SSD_REQUEST_FLUSH:
                /* C3：FLUSH 落地 — 对全部 (ch,ce,lun) 实例刷 dirty L2P cache 到
                 * NAND + 强制 Journal buffer 落盘。两个原语均同步阻塞（内部
                 * ftl_internal_sync_wait 泵完），完成标志可直接同步置 1，
                 * 无需走 pending 异步。 */
                {
                    uint8_t fch;
                    uint8_t fce;
                    uint8_t flun;

                    for (fch = 0U; fch < CH_NUM; fch++)
                    {
                        for (fce = 0U; fce < CE_NUM; fce++)
                        {
                            for (flun = 0U; flun < LUN_NUM; flun++)
                            {
                                ftl_context_t *fftl = controller->ftl[fch][fce][flun];

                                if (fftl == NULL)
                                {
                                    continue; /* 防御：槽位绑定后恒非空 */
                                }
                                (void)l2p_cache_flush_all(&fftl->cache);
                                journal_flush_page(&fftl->journal);
                            }
                        }
                    }
                    request->completion_status = SSD_STATUS_OK;
                    request->complete_flag     = 1U;
                }
                break;

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