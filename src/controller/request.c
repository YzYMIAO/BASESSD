/* 主机请求校验 + 按 FTL 实例边界切连续 run 提交。
 *
 * 策略下沉（对齐主流）：**plane 配对（访问哪些物理单元）由 FTL 决定**——上层只给
 * 「起始 LBA + 长度 + 缓冲」，FTL 自行决定单/双 plane 拆分。Controller 只做两件事：
 *   ① 按 LBA 路由到 FTL 实例（分片模型）
 *   ② 在**实例边界**处把请求切成若干连续 run，每个 run 一次 ftl_submit_*（FTL 一次
 *      提交只作用一个实例）
 * 出处：fc_nand_design.md §26.1/§26.2 —— HIL 拿到的是一条已解析的请求，访问哪些
 * 物理单元、数据怎么搬都是固件（FTL）的事。
 */

#include "assertion.h"
#include "controller.h"
#include "ftl.h"
#include "log.h"

/* 外部：controller.c — 按 LBA 路由到 FTL 实例（内部函数，拆分循环共用） */
ftl_ctx_t *ssd_controller_route(const ssd_controller_t *controller, QWORD lba);

/* ── 子请求完成回调 ──
 *
 * 由组的完成通知（pend_group_maybe_complete）调用，一次 host run 恰好一次。
 * 递减 sub_remaining 计数器，归零时置父请求 complete_flag。
 */
static void on_sub_complete(void *ctx)
{
    ssd_request_t *req = (ssd_request_t *)ctx;

    if (req->sub_remaining > 0U)
    {
        req->sub_remaining--;
    }

    /* 所有 run 完成 → 通知 host */
    if (req->sub_remaining == 0U)
    {
        req->complete_flag = 1U;
    }
}

/*
 * ssd_controller_run_end - 从 lba 起、路由到同一 FTL 实例的最长连续 run 的结束 LBA（开区间）
 *
 * 路由按 region（L2P_REGION_SHIFT 个 LBA）粒度分片 ⇒ run 只可能在 region 边界处结束，
 * 逐 region 前进即可，无需逐 LBA 试探。
 *
 * @param controller Controller 上下文
 * @param lba        起点（全局 LBA）
 * @param lba_limit  上界（开区间）
 * @return 本 run 的结束 LBA（> lba，<= lba_limit）
 */
QWORD ssd_controller_run_end(ssd_controller_t *controller, QWORD lba, QWORD lba_limit)
{
    ftl_ctx_t *ftl = ssd_controller_route(controller, lba);
    QWORD      end = lba + 1U;

    /* 路由只可能因宏配置错误失败；正常配置下恒非 NULL */
    ASSERT(ftl != NULL);

    while (end < lba_limit)
    {
        QWORD boundary;

        if (ssd_controller_route(controller, end) != ftl)
        {
            break; /* 进入另一实例 */
        }

        boundary = (((end >> L2P_REGION_SHIFT) + 1U) << L2P_REGION_SHIFT);
        if (boundary > lba_limit)
        {
            boundary = lba_limit;
        }
        end = boundary;
    }

    return end;
}

/*
 * ssd_split_count - 预统计拆分出的 run 段数
 *
 * 必须在提交前精确置位：run 的完成可能**同步**触发 on_sub_complete（如整段未映射读），
 * 计数为 0 时同步回调会把父请求提前置完成。
 */
static DWORD ssd_split_count(ssd_controller_t *controller, ssd_request_t *request)
{
    QWORD lba     = request->start_lba;
    QWORD lba_end = request->start_lba + request->block_count;
    DWORD count   = 0U;

    while (lba < lba_end)
    {
        count++;
        lba = ssd_controller_run_end(controller, lba, lba_end);
    }

    return count;
}

ssd_status_e ssd_request_split(ssd_controller_t *controller, ssd_request_t *request)
{
    QWORD        lba;
    QWORD        lba_end;
    DWORD        submitted = 0U;
    ssd_status_e err_status;

    if ((controller == NULL) || (request == NULL) || (request->block_count == 0U))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    if ((request->opcode != SSD_REQUEST_READ) && (request->opcode != SSD_REQUEST_WRITE))
    {
        return SSD_STATUS_NOT_SUPPORTED;
    }

    err_status = (request->opcode == SSD_REQUEST_WRITE) ? SSD_STATUS_NO_SPACE : SSD_STATUS_IO_ERROR;

    /* 先精确统计 run 段数（子请求可能同步完成、立即递减该计数），置位后再提交。 */
    request->sub_remaining = ssd_split_count(controller, request);
    if (request->sub_remaining == 0U)
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 子请求公共字段（循环内复用 io 模板，仅改 lba/buffer/count） */
    request->io.complete_flag   = NULL; /* 子请求用 on_complete 聚合 */
    request->io.status_out      = &request->completion_status;
    request->io.on_complete     = on_sub_complete;
    request->io.on_complete_ctx = request;

    lba     = request->start_lba;
    lba_end = request->start_lba + request->block_count;

    while (lba < lba_end)
    {
        QWORD        run_end = ssd_controller_run_end(controller, lba, lba_end);
        DWORD        run_len = (run_end - lba);
        ftl_ctx_t   *ftl     = ssd_controller_route(controller, lba);
        ftl_status_e st;

        /* 路由只可能因宏配置错误失败；正常配置下恒非 NULL */
        ASSERT(ftl != NULL);

        request->io.lba    = lba;
        /* 缓冲步进 = 映射单元（FTL_IU_BYTES = 512B）——CW 级映射下 1 LBA = 1 个 CW
         * （= 512B 扇区），不是 2KB 页。 */
        request->io.buffer = (BYTE *)request->buffer + ((size_t)(lba - request->start_lba) * FTL_IU_BYTES);
        request->io.count  = run_len; /* ★ 长度交给 FTL，plane 拆分由 FTL 决定 */

        st = (request->opcode == SSD_REQUEST_WRITE) ? ftl_submit_write(ftl, &request->io) : ftl_submit_read(ftl, &request->io);
        if (st != FTL_OK)
        {
            LOGE(LOG_TAG_CTRL "split submit failed lba=%llu count=%lu\r\n", lba, run_len);
            request->completion_status = err_status;
            /* 已提交的 run 仍会回调 on_sub_complete → 把计数收敛到"实际在飞数"，
             * 避免父请求提前完成（旧实现在此处直接置 complete_flag 即此缺陷）。 */
            request->sub_remaining     = submitted;
            if (submitted == 0U)
            {
                request->complete_flag = 1U;
            }
            return err_status;
        }
        submitted++;
        lba = run_end;
    }

    return SSD_STATUS_OK;
}
