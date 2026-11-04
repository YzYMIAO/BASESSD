#include "controller.h"

#include "assertion.h"
#include "log.h"

#include <stddef.h>
#include <string.h>

/* 外部：request.c — 按实例边界切 run / 提交子请求 */
ssd_status_e ssd_request_split(ssd_controller_t *controller, ssd_request_t *request);
QWORD        ssd_controller_run_end(ssd_controller_t *controller, QWORD lba, QWORD lba_limit);

/*
 * ssd_controller_route - 按全局 LBA 路由到对应 FTL 实例
 *
 * 使用 l2p_cache.h 的 lba_2_die（region 粒度取模）选实例；后续换哈希策略
 * 只改该函数。非 static：request.c 拆分循环共用（前向声明在 request.c），
 * 不暴露进公共头。
 *
 * @param controller Controller 上下文（调用者保证非 NULL）
 * @param lba        Host 全局 LBA（QWORD）
 * @return 路由到的 FTL 实例指针
 */
ftl_ctx_t *ssd_controller_route(const ssd_controller_t *controller, QWORD lba)
{
    /* 唯一调用者 ssd_controller_submit / ssd_request_split 均已判过 controller */
    ASSERT(controller != NULL);

    const BYTE die = lba_2_die(lba);

    /* 路由取模结果本应在 [0,DIE_MAX) 内 */
    ASSERT(die < DIE_MAX);

    return controller->ftl[die];
}

/**
 * @brief Controller 最小状态机（C1）
 *
 * 管理 RESET → INITIALIZING → READY / FAILED 转换：
 *   · ssd_controller_init 的槽位校验结果驱动转换：全部通过 → READY，
 *     任一槽位缺失/拓扑不符 → FAILED（终态）；
 *   · READY → FAILED 预留给运行时致命错误（当前无调用点）；
 *   · 非法转换直接忽略（保持现态）。
 *
 * SHUTDOWN 不在枚举内（学习主控无热移除/优雅掉电退出路径）；若后续加枚举值，
 * 只扩展本函数的转换表，不动调用方。
 *
 * @param controller Controller 上下文（NULL 时安全返回）
 * @param next_state 目标状态
 */
void ssd_controller_state_transition(ssd_controller_t *controller, ssd_ctrl_state_e next_state)
{
    ssd_ctrl_state_e prev_state;

    if (controller == NULL)
    {
        return;
    }

    prev_state = controller->state;

    switch (controller->state)
    {
        case SSD_CONTROLLER_RESET:
            /* boot 起点：RESET → INITIALIZING（ssd_controller_init 入口） */
            if (next_state == SSD_CONTROLLER_INITIALIZING)
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_INITIALIZING:
            /* 槽位校验结束：全部通过 → READY；任一失败 → FAILED */
            if ((next_state == SSD_CONTROLLER_READY) || (next_state == SSD_CONTROLLER_FAILED))
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_READY:
            /* 运行时致命错误 → FAILED（当前无调用点，保留语义） */
            if (next_state == SSD_CONTROLLER_FAILED)
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_FAILED:
        default:
            /* FAILED 为终态，不响应任何转换 */
            break;
    }

    /* FAILED 是终态，进入即宣告 controller 不可用；在此单点留痕 */
    LOGE_IF((prev_state != SSD_CONTROLLER_FAILED) && (controller->state == SSD_CONTROLLER_FAILED), LOG_TAG_CTRL "state -> FAILED\r\n");
}

ssd_status_e ssd_controller_init(ssd_controller_t *controller, ftl_ctx_t *const *ftl_by_die)
{
    if ((controller == NULL) || (ftl_by_die == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    controller->flush_req    = NULL;
    controller->trim_req     = NULL;
    controller->trim_lba     = 0U;
    controller->trim_started = false;
    controller->host_lba_max = (DWORD)FTL_LBA_MAX;

    /* 稀疏拓扑 / 重复 init：先清空 die 槽，缺失项保持 NULL。 */
    memset(controller->ftl, 0, sizeof(controller->ftl));

    /* 逐 present die 校验并绑定 FTL 实例：稀疏拓扑允许部分 die 槽为 NULL。
     * 状态机（C1）：入口 RESET → INITIALIZING，校验结果驱动 READY/FAILED。 */
    ssd_controller_state_transition(controller, SSD_CONTROLLER_INITIALIZING);

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const BYTE ch   = die_2_ch(die);
        const BYTE ce   = die_2_ce(die);
        const BYTE lun  = die_2_lun(die);
        ftl_ctx_t *slot = ftl_by_die[die];

        if (slot == NULL)
        {
            LOGE(LOG_TAG_CTRL "init failed: null FTL slot die=%hhu ch=%u ce=%u lun=%u\r\n", die, ch, ce, lun);
            ssd_controller_state_transition(controller, SSD_CONTROLLER_FAILED);
            return SSD_STATUS_INVALID_ARGUMENT;
        }
        /* die 索引必须等于实例绑定拓扑：保证 LBA 路由与芯片定位语义一致 */
        if ((slot->topo.ch != ch) || (slot->topo.ce != ce) || (slot->topo.lun != lun))
        {
            LOGE(LOG_TAG_CTRL "init failed: topo mismatch die=%hhu slot ch=%u ce=%u lun=%u\r\n", die, ch, ce, lun);
            ssd_controller_state_transition(controller, SSD_CONTROLLER_FAILED);
            return SSD_STATUS_INVALID_ARGUMENT;
        }
        controller->ftl[die] = slot;
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

    /* 主机 IO 到来后的后台让路由 ftl_service 的档位判定（pending/groups → IDLE/
     * NORMAL/BUSY）承担；Controller 只负责受理与拆分，不再维护调度钩子。 */

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
                for (BYTE die = 0U; die < DIE_MAX; die++)
                {
                    ftl_ctx_t *fftl = controller->ftl[die];

                    /* present die 由 ssd_controller_init 绑满，恒非空 */
                    ASSERT(fftl != NULL);
                    ftl_flush_begin(fftl);
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

    /* 先推进**全部** present die 再判完成：某一实例未完成时直接 return 会让后面的实例
     * 永远得不到推进（多实例下 FLUSH 卡死）。 */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_t *fftl = controller->ftl[die];

        ASSERT(fftl != NULL);

        if (!ftl_flush_step(fftl))
        {
            all_done = false;
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

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_t *ftl = controller->ftl[die];

        ASSERT(ftl != NULL);
        ftl_flush_cancel(ftl);
    }
}