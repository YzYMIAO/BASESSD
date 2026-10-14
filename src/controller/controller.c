#include "controller/controller.h"

#include "common/assertion.h"
#include "common/log.h"

#include <stddef.h>

/* 外部：request.c — 按实例边界切 run / 提交子请求 */
int      ssd_request_split(ssd_controller_t *controller, ssd_request_t *request);
uint64_t ssd_controller_run_end(ssd_controller_t *controller, uint64_t lba, uint64_t lba_limit);

/*
 * ssd_controller_route - 按全局 LBA 路由到对应 FTL 实例
 *
 * 使用 l2p_cache.h 的 lba_route_ch/ce/lun（region 粒度取模）选实例；
 * 后续换哈希策略只改此处。CH_NUM=CE_NUM=LUN_NUM=1 时恒返回 ftl[0][0][0]。
 * 非 static：request.c 拆分循环共用（前向声明在 request.c），不暴露进公共头。
 *
 * @param controller Controller 上下文（调用者保证非 NULL）
 * @param lba        Host 全局 LBA（uint64_t）
 * @return 路由到的 FTL 实例指针
 */
ftl_context_t *ssd_controller_route(const ssd_controller_t *controller, uint64_t lba)
{
    uint32_t ch;
    uint32_t ce;
    uint32_t lun;

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
                    LOGE("[CTRL] init failed: null FTL slot ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
                    ssd_controller_state_transition(controller, SSD_CONTROLLER_FAILED);
                    return SSD_STATUS_INVALID_ARGUMENT;
                }
                /* 槽位索引必须等于实例绑定拓扑：保证 LBA 路由与芯片定位语义一致 */
                if ((ftl[ch][ce][lun]->topo.ch != ch) ||
                    (ftl[ch][ce][lun]->topo.ce != ce) ||
                    (ftl[ch][ce][lun]->topo.lun != lun))
                {
                    LOGE("[CTRL] init failed: topo mismatch slot ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
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
    int ret;

    if ((controller == NULL) || (request == NULL) || (controller->state != SSD_CONTROLLER_READY))
    {
        if ((controller != NULL) && (controller->state != SSD_CONTROLLER_READY))
        {
            LOGW("[CTRL] submit rejected: state=%d\r\n", (int)controller->state);
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
             * 真实隐患：wr 0 64 会写爆 CLI 的 g_wr_buf。 */
            if (request->buffer_size < ((size_t)request->block_count * FTL_IU_BYTES))
            {
                LOGE("[CTRL] buffer too small count=%lu need=%lu have=%lu\r\n",
                     (unsigned long)request->block_count,
                     (unsigned long)((size_t)request->block_count * FTL_IU_BYTES),
                     (unsigned long)request->buffer_size);
                request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
                request->complete_flag     = 1U;
                return SSD_STATUS_INVALID_ARGUMENT;
            }

            /* 读/写统一走 run 拆分（含单块）：plane 配对在 FTL 内，HIL 只按实例切段 */
            ret = ssd_request_split(controller, request);
            if (ret != SSD_STATUS_OK)
            {
                request->completion_status = (ssd_status_t)ret;
                request->complete_flag     = 1U;
                return ret;
            }
            break;

        case SSD_REQUEST_TRIM:
        {
            uint64_t lba = request->start_lba;
            uint64_t end = request->start_lba + request->block_count;

            /* TRIM 同步完成（无异步回调）；跨实例时逐 run 提交 */
            while (lba < end)
            {
                uint64_t       run_end = ssd_controller_run_end(controller, lba, end);
                ftl_context_t *tftl    = ssd_controller_route(controller, lba);

                /* 路由只可能因宏配置错误失败；正常配置下恒非 NULL */
                ASSERT(tftl != NULL);
                ftl_trim(tftl, lba, (uint32_t)(run_end - lba));
                lba = run_end;
            }

            request->completion_status = SSD_STATUS_OK;
            request->complete_flag     = 1U;
            break;
        }

        case SSD_REQUEST_FLUSH:
            /* C3：FLUSH 落地 — 对全部 (ch,ce,lun) 实例调 ftl_flush（L2P cache 写回 +
             * journal 尾页刷出）。同步阻塞（内部 ftl_sync_submit_wait 泵完），完成
             * 标志可直接同步置 1，无需走 pending 异步。
             * HIL 只调 FTL 对外接口，不再伸手进 FTL 内部原语（对齐主流）。 */
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

                            /* 槽位由 ssd_controller_init 绑满，恒非空 */
                            ASSERT(fftl != NULL);
                            ftl_flush(fftl);
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

    return request->completion_status;
}