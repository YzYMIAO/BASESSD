#include "controller.h"

#include "assertion.h"
#include "log.h"

#include <stddef.h>

/* 外部：request.c — 按实例边界切 run / 提交子请求 */
ssd_status_e ssd_request_split(ssd_controller_t *controller, ssd_request_t *request);
QWORD        ssd_controller_run_end(ssd_controller_t *controller, QWORD lba, QWORD lba_limit);

/*
 * ssd_controller_route - 按全局 LBA 路由到对应 FTL 实例
 *
 * 使用 l2p_cache.h 的 lba_route_ch/ce/lun（region 粒度取模）选实例；
 * 后续换哈希策略只改此处。CH_NUM=CE_NUM=LUN_NUM=1 时恒返回 ftl[0][0][0]。
 * 非 static：request.c 拆分循环共用（前向声明在 request.c），不暴露进公共头。
 *
 * @param controller Controller 上下文（调用者保证非 NULL）
 * @param lba        Host 全局 LBA（QWORD）
 * @return 路由到的 FTL 实例指针
 */
ftl_ctx_t *ssd_controller_route(const ssd_controller_t *controller, QWORD lba)
{
    BYTE ch;
    BYTE ce;
    BYTE lun;

    /* 唯一调用者 ssd_controller_submit / ssd_request_split 均已判过 controller */
    ASSERT(controller != NULL);

    ch  = lba_route_ch(lba);
    ce  = lba_route_ce(lba);
    lun = lba_route_lun(lba);

    /* 路由取模结果本应在 [0,N) 内；越界只可能是 CH_NUM/CE_NUM/LUN_NUM 宏配置错误 */
    ASSERT(ch < CH_NUM);
    ASSERT(ce < CE_NUM);
    ASSERT(lun < LUN_NUM);

    return controller->ftl[ch][ce][lun];
}

ssd_status_e ssd_controller_init(ssd_controller_t *controller, ftl_ctx_t *ftl[CH_NUM][CE_NUM][LUN_NUM])
{
    BYTE ch;
    BYTE ce;
    BYTE lun;

    if ((controller == NULL) || (ftl == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    controller->flush_req    = NULL;
    controller->trim_req     = NULL;
    controller->trim_lba     = 0U;
    controller->trim_started = false;
    controller->host_lba_max = (DWORD)FTL_LBA_MAX;

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
                    LOGE(LOG_TAG_CTRL "init failed: null FTL slot ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
                    ssd_controller_state_transition(controller, SSD_CONTROLLER_FAILED);
                    return SSD_STATUS_INVALID_ARGUMENT;
                }
                /* 槽位索引必须等于实例绑定拓扑：保证 LBA 路由与芯片定位语义一致 */
                if ((ftl[ch][ce][lun]->topo.ch != ch) || (ftl[ch][ce][lun]->topo.ce != ce) || (ftl[ch][ce][lun]->topo.lun != lun))
                {
                    LOGE(LOG_TAG_CTRL "init failed: topo mismatch slot ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
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

BYTE ssd_controller_is_complete(const ssd_request_t *request)
{
    if (request == NULL)
    {
        return 1U;
    }
    return request->complete_flag;
}

bool ssd_controller_lba_range_ok(const ssd_controller_t *controller, QWORD lba, DWORD count)
{
    /* 容量口径 = 运行时 host_lba_max（已扣实际坏块），不是编译期 FTL_LBA_MAX。
     * lba 与 count 的上界之和 < 2^33，`QWORD` 加法不会回绕；先判起点再判终点。 */
    const DWORD max = ssd_controller_host_lba_max(controller);

    if ((count == 0U) || (lba >= max))
    {
        return false;
    }

    return (lba + count) <= max;
}

ssd_status_e ssd_controller_submit(ssd_controller_t *controller, ssd_request_t *request)
{
    ssd_status_e ret;

    if ((controller == NULL) || (request == NULL) || (controller->state != SSD_CONTROLLER_READY))
    {
        if ((controller != NULL) && (controller->state != SSD_CONTROLLER_READY))
        {
            LOGW(LOG_TAG_CTRL "submit rejected: state=%d\r\n", (int)controller->state);
        }
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

    switch (request->opcode)
    {
        case SSD_REQUEST_READ:
        case SSD_REQUEST_WRITE:
            /* 缓冲容量校验：block_count 个映射单元 × 512B 必须不超过调用者给的
             * buffer_size（CW 级映射下 1 个 LBA = 1 个 CW = 512B 扇区，不是 2KB 页）。
             * 真实隐患：wr 0 64 会写爆 CLI 的 g_wr_buf。
             * ⚠ 保留 `(size_t)`：先加宽再相乘，避免 32 位运算溢出后绕过本检查
             *   （H44 的「刻意加宽避免溢出」例外）。 */
            if (request->buffer_size < ((size_t)request->block_count * FTL_IU_BYTES))
            {
                LOGE(LOG_TAG_CTRL "buffer too small count=%lu need=%zu have=%zu\r\n", request->block_count, (size_t)request->block_count * FTL_IU_BYTES, request->buffer_size);
                request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
                request->complete_flag     = 1U;
                return SSD_STATUS_INVALID_ARGUMENT;
            }

            /* 读/写统一走 run 拆分（含单块）：plane 配对在 FTL 内，HIL 只按实例切段 */
            ret = ssd_request_split(controller, request);
            if (ret != SSD_STATUS_OK)
            {
                request->completion_status = ret;
                request->complete_flag     = 1U;
                return ret;
            }
            break;

        case SSD_REQUEST_TRIM:
            /* TRIM 是**异步命令**：submit 只登记范围，ssd_controller_service 每轮
             * 按 run 调 ftl_trim_begin/step；完成后置 complete_flag。
             * 单飞行：同类请求已有一条在飞时返回 BUSY。 */
            if (controller->trim_req != NULL)
            {
                request->completion_status = SSD_STATUS_BUSY;
                request->complete_flag     = 1U;
                return SSD_STATUS_BUSY;
            }
            if (!ssd_controller_lba_range_ok(controller, request->start_lba, request->block_count))
            {
                request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
                request->complete_flag     = 1U;
                return SSD_STATUS_INVALID_ARGUMENT;
            }

            controller->trim_req       = request;
            controller->trim_lba       = request->start_lba;
            controller->trim_started   = false;
            request->completion_status = SSD_STATUS_OK;
            break;

        case SSD_REQUEST_FLUSH:
            /* FLUSH 是**异步命令**（RISK-USB-06）：提交只登记请求并启动各实例的
             * flush job，立即返回；完成由 ssd_controller_service 每轮推进后置
             * complete_flag。这样 host 回调（tud_task 内）不再自旋，USB 事件
             * 队列不会被饿死。单飞行：同类请求已有一条在飞时返回 BUSY。 */
            if (controller->flush_req != NULL)
            {
                request->completion_status = SSD_STATUS_BUSY;
                request->complete_flag     = 1U;
                return SSD_STATUS_BUSY;
            }

            {
                BYTE fch;
                BYTE fce;
                BYTE flun;

                for (fch = 0U; fch < CH_NUM; fch++)
                {
                    for (fce = 0U; fce < CE_NUM; fce++)
                    {
                        for (flun = 0U; flun < LUN_NUM; flun++)
                        {
                            ftl_ctx_t *fftl = controller->ftl[fch][fce][flun];

                            /* 槽位由 ssd_controller_init 绑满，恒非空 */
                            ASSERT(fftl != NULL);
                            ftl_flush_begin(fftl);
                        }
                    }
                }
            }

            controller->flush_req      = request;
            request->completion_status = SSD_STATUS_OK;
            break;

        default:
            request->completion_status = SSD_STATUS_NOT_SUPPORTED;
            request->complete_flag     = 1U;
            break;
    }

    return request->completion_status;
}

static void ssd_controller_flush_service(ssd_controller_t *controller)
{
    if (controller->flush_req == NULL)
    {
        return;
    }

    bool all_done = true;

    BYTE fch;
    BYTE fce;
    BYTE flun;

    /* 先推进**全部**实例再判完成：某一实例未完成时直接 return 会让后面的实例
     * 永远得不到推进（多实例下 FLUSH 卡死）。 */
    for (fch = 0U; fch < CH_NUM; fch++)
    {
        for (fce = 0U; fce < CE_NUM; fce++)
        {
            for (flun = 0U; flun < LUN_NUM; flun++)
            {
                ftl_ctx_t *fftl = controller->ftl[fch][fce][flun];

                ASSERT(fftl != NULL);

                if (!ftl_flush_step(fftl))
                {
                    all_done = false;
                }
            }
        }
    }

    if (!all_done)
    {
        return; /* 还有实例没落定：下一轮继续 */
    }

    /* 全部实例完成：按 HIL 完成通道回执（complete_flag / status_out）。 */
    controller->flush_req->completion_status = SSD_STATUS_OK;
    controller->flush_req->complete_flag     = 1U;
    controller->flush_req                    = NULL;
}

static void ssd_controller_trim_complete(ssd_controller_t *controller, ssd_status_e status)
{
    controller->trim_req->completion_status = status;
    controller->trim_req->complete_flag     = 1U;
    controller->trim_req                    = NULL;
    controller->trim_started                = false;
}

static void ssd_controller_trim_service(ssd_controller_t *controller)
{
    if (controller->trim_req == NULL)
    {
        return;
    }

    const QWORD end = controller->trim_req->start_lba + controller->trim_req->block_count;

    if (controller->trim_lba >= end)
    {
        ssd_controller_trim_complete(controller, SSD_STATUS_OK);
        return;
    }

    const QWORD run_end = ssd_controller_run_end(controller, controller->trim_lba, end);
    ftl_ctx_t  *tftl    = ssd_controller_route(controller, controller->trim_lba);

    /* 路由只可能因宏配置错误失败；正常配置下恒非 NULL */
    ASSERT(tftl != NULL);

    if (!controller->trim_started)
    {
        const DWORD        lba   = (DWORD)controller->trim_lba;
        const DWORD        count = (DWORD)(run_end - controller->trim_lba);
        const ftl_status_e bst   = ftl_trim_begin(tftl, lba, count);

        if (bst == FTL_AGAIN)
        {
            return; /* 实例上已有 job：下一轮重试 */
        }
        if (bst != FTL_OK)
        {
            ssd_controller_trim_complete(controller, SSD_STATUS_IO_ERROR);
            return;
        }
        controller->trim_started = true;
    }

    const ftl_status_e st = ftl_trim_step(tftl);

    if (st == FTL_AGAIN)
    {
        return; /* 本轮未完成：下一轮继续 */
    }
    if (st != FTL_OK)
    {
        ssd_controller_trim_complete(controller, SSD_STATUS_IO_ERROR);
        return;
    }

    controller->trim_started = false;
    controller->trim_lba     = run_end;

    if (controller->trim_lba >= end)
    {
        ssd_controller_trim_complete(controller, SSD_STATUS_OK);
    }
}

void ssd_controller_service(ssd_controller_t *controller)
{
    if (controller == NULL)
    {
        return;
    }

    ssd_controller_flush_service(controller);
    ssd_controller_trim_service(controller);
}

void ssd_controller_cancel_flush(ssd_controller_t *controller)
{
    if (controller == NULL)
    {
        return;
    }

    /* 摘回执指针 + 清各实例的 job 标志：避免旧 job 在请求对象被复用后才完成，
     * 把回执写到新请求上。在飞的数据操作有自己的服务路径，不需要回滚。 */
    controller->flush_req = NULL;

    BYTE ch;
    BYTE ce;
    BYTE lun;

    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                ftl_ctx_t *ftl = controller->ftl[ch][ce][lun];

                ASSERT(ftl != NULL);
                ftl_flush_cancel(ftl);
            }
        }
    }
}