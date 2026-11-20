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
#include "delay.h" /* delay_time_now_us（命令延迟直方图） */
#include "ftl_interface.h"
#include "lifetime.h" /* 终身计数：host 命令成功计数 */
#include "log.h"
#include "stats.h"

#include <string.h>

/* host 失败回执的限流日志（无命令池/队列依赖，统计关闭也保留）。 */
static DWORD s_bad_status_log_ms;
static DWORD s_bad_status_log_n;

/**
 * @brief 请求 opcode 的短名（日志用）
 *
 * @param opcode 请求类型
 * @return 稳定短名
 */
static const char *ssd_request_opcode_name(ssd_req_opcode_e opcode)
{
    switch (opcode)
    {
        case SSD_REQUEST_READ:
            return "read";
        case SSD_REQUEST_WRITE:
            return "write";
        case SSD_REQUEST_FLUSH:
            return "flush";
        case SSD_REQUEST_TRIM:
            return "trim";
        default:
            return "unknown";
    }
}

/**
 * @brief 完成状态的短名（日志用）
 *
 * @param status 聚合状态
 * @return 稳定短名
 */
static const char *ssd_request_status_name(ssd_status_e status)
{
    switch (status)
    {
        case SSD_STATUS_OK:
            return "ok";
        case SSD_STATUS_INVALID_ARGUMENT:
            return "invalid_argument";
        case SSD_STATUS_NO_SPACE:
            return "no_space";
        case SSD_STATUS_IO_ERROR:
            return "io_error";
        case SSD_STATUS_NOT_SUPPORTED:
            return "not_supported";
        case SSD_STATUS_BUSY:
            return "busy";
        case SSD_STATUS_TIMEOUT:
            return "timeout";
        default:
            return "unknown";
    }
}

/* ── HIL 请求统计（模块自持；docs/design/common/statistics_design.md）──
 * 统计专用对象/表/函数在 SSD_STATS_ENABLE=0 时整体不编译。 */
#if SSD_STATS_ENABLE
typedef struct HIL_REQUEST_STATISTICS_S
{
    DWORD        cmd_read;
    DWORD        cmd_write;
    DWORD        cmd_flush;
    DWORD        cmd_trim;
    DWORD        timeout;
    DWORD        bad_status;
    stats_hist_s cmd_us; /* host 命令提交 → 完成 */
} hil_request_stats_s;

static hil_request_stats_s s_stats_hil;

#    define HIL_STATS_LIST(X)                  \
        X(cmd_read, STATS_U32, STATS_COUNTER)  \
        X(cmd_write, STATS_U32, STATS_COUNTER) \
        X(cmd_flush, STATS_U32, STATS_COUNTER) \
        X(cmd_trim, STATS_U32, STATS_COUNTER)  \
        X(timeout, STATS_U32, STATS_COUNTER)   \
        X(bad_status, STATS_U32, STATS_COUNTER)

#    define HIL_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(hil_request_stats_s, field), width, kind},
static const stats_name_s s_stats_hil_names[] = {
    HIL_STATS_LIST(HIL_STATS_NAME)};
#    undef HIL_STATS_NAME
#    define HIL_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(hil_request_stats_s, field, width);
HIL_STATS_LIST(HIL_STATS_CHECK)
#    undef HIL_STATS_CHECK

#    define HIL_STATS_HIST_LIST(X)     X(cmd_us)
#    define HIL_STATS_HIST_NAME(field) {STATS_NAME_STR(field), STATS_OFFSET(hil_request_stats_s, field)},
static const stats_hist_name_s s_stats_hil_hist_names[] = {
    HIL_STATS_HIST_LIST(HIL_STATS_HIST_NAME)};
#    undef HIL_STATS_HIST_NAME
STATS_CHECK_HIST_FIELD(hil_request_stats_s, cmd_us);

/**
 * @brief HIL 请求统计注册（controller init 调用一次；幂等，重入会重置计数）
 */
void ssd_request_stats_register(void)
{
    memset(&s_stats_hil, 0, sizeof(s_stats_hil));
    (void)stats_register("hil", STATS_INST_NONE, &s_stats_hil, s_stats_hil_names, STATS_ENTRY_COUNT(s_stats_hil_names));
    (void)stats_register_hist("hil", STATS_INST_NONE, &s_stats_hil, s_stats_hil_hist_names, STATS_ENTRY_COUNT(s_stats_hil_hist_names));
}

/**
 * @brief 已受理的主机命令计数
 *
 * @param opcode 请求类型（READ / WRITE / FLUSH / TRIM）
 */
void ssd_request_note_cmd(ssd_req_opcode_e opcode)
{
    switch (opcode)
    {
        case SSD_REQUEST_READ:
            STATS_INC(s_stats_hil.cmd_read);
            break;
        case SSD_REQUEST_WRITE:
            STATS_INC(s_stats_hil.cmd_write);
            break;
        case SSD_REQUEST_FLUSH:
            STATS_INC(s_stats_hil.cmd_flush);
            break;
        case SSD_REQUEST_TRIM:
            STATS_INC(s_stats_hil.cmd_trim);
            break;
        default:
            ASSERT(false);
            break;
    }
}

/** @brief host 超时回执计数（ssd_request_mark_timeout 调用） */
void ssd_request_note_timeout(void)
{
    STATS_INC(s_stats_hil.timeout);
}

/** @brief 异步完成回执为失败（非 OK、非 TIMEOUT）的计数 */
void ssd_request_note_bad_status(void)
{
    STATS_INC(s_stats_hil.bad_status);
}

/**
 * @brief 记录一次请求的完成延迟（只记一次）
 *
 * @param request 请求（start_timed 为 false 时直接返回）
 */
void ssd_request_note_latency(ssd_req_s *request)
{
    if (request->start_timed == false)
    {
        return;
    }

    DWORD now_us = 0U;

    if (delay_time_now_us(&now_us) != false)
    {
        stats_hist_observe(&s_stats_hil.cmd_us, now_us - request->start_us);
    }
    request->start_timed = false;
}
#endif /* SSD_STATS_ENABLE */

/* 外部：controller.c — 按 LBA 路由到 FTL 实例（内部函数，拆分循环共用） */
ftl_ctx_s *ssd_controller_route(const ssd_ctl_s *controller, QWORD lba);

/**
 * @brief 子请求完成回调：聚合状态并递减未完成 run 数。
 *
 * 由组的完成通知（pend_group_maybe_complete）调用，一次 host run 恰好一次；
 * 归零时把 agg_status 写回 completion_status 并置 complete_flag。
 *
 * @param ctx 父请求（ssd_req_s *）
 */
static void on_sub_complete(void *ctx)
{
    ssd_req_s *req = (ssd_req_s *)ctx;

    /* 已超时：host 回执已发出；只递减在飞 run 数，供前端判断何时可释放。
     * 不得覆盖 TIMEOUT，也不得再置 complete_flag。 */
    if (req->timed_out != false)
    {
        if (req->sub_remaining > 0U)
        {
            req->sub_remaining--;
        }
#if SSD_STATS_ENABLE
        if (req->sub_remaining == 0U)
        {
            ssd_request_note_latency(req);
        }
#endif
        return;
    }

    /* 聚合状态：首个非 OK 胜出。FTL 在回调前已把本 run 的状态写进 sub_status；
     * 后续成功 run 会覆盖它，所以必须在这里锁存。completion_status 是 host 可见
     * 状态，只由本函数在全部 run 完成后写。 */
    if ((req->sub_status != SSD_STATUS_OK) && (req->agg_status == SSD_STATUS_OK))
    {
        req->agg_status = req->sub_status;
    }

    if (req->sub_remaining > 0U)
    {
        req->sub_remaining--;
    }

    /* 所有 run 完成 → 用聚合状态通知 host（不是最后一个 run 的状态） */
    if (req->sub_remaining == 0U)
    {
        req->completion_status = req->agg_status;
        req->complete_flag     = 1U;

        if (req->agg_status != SSD_STATUS_OK)
        {
            // 失败只在这里留一行现场：LBA/长度/状态，超时路径由 USB/FLUSH/TRIM
            // 自己的日志承担（它们在 timed_out 分支早退，不会走到这里）。
            LOGE_RATE(log_rate_due(&s_bad_status_log_ms, &s_bad_status_log_n, LOG_RATE_SLOW_INTERVAL_MS), s_bad_status_log_n, LOG_TAG_CTRL "host op=%s lba=%llu n=%lu status=%s", ssd_request_opcode_name(req->opcode), req->start_lba, req->block_count, ssd_request_status_name(req->agg_status));
        }
#if SSD_STATS_ENABLE
        ssd_request_note_latency(req);
        if (req->agg_status != SSD_STATUS_OK)
        {
            ssd_request_note_bad_status();
        }
#endif
#if SSD_STATS_PERSIST_ENABLE
        if (req->agg_status == SSD_STATUS_OK)
        {
            switch (req->opcode)
            {
                case SSD_REQUEST_READ:
                    lifetime_note_cmd(LIFETIME_CMD_READ);
                    break;
                case SSD_REQUEST_WRITE:
                    lifetime_note_cmd(LIFETIME_CMD_WRITE);
                    break;
                case SSD_REQUEST_FLUSH:
                    lifetime_note_cmd(LIFETIME_CMD_FLUSH);
                    break;
                case SSD_REQUEST_TRIM:
                    lifetime_note_cmd(LIFETIME_CMD_TRIM);
                    break;
                default:
                    ASSERT(false);
                    break;
            }
        }
#endif
    }
}

/**
 * @brief 从 lba 起、路由到同一 FTL 实例的最长连续 run 的结束 LBA（开区间）。
 *
 * 路由按 region（L2P_REGION_SHIFT 个 LBA）粒度分片 ⇒ run 只可能在 region 边界处结束，
 * 逐 region 前进即可，无需逐 LBA 试探。
 *
 * @param controller Controller 上下文
 * @param lba        起点（全局 LBA）
 * @param lba_limit  上界（开区间）
 * @return 本 run 的结束 LBA（> lba，<= lba_limit）
 */
QWORD ssd_controller_run_end(ssd_ctl_s *controller, QWORD lba, QWORD lba_limit)
{
    ftl_ctx_s *ftl = ssd_controller_route(controller, lba);
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

/**
 * @brief 预统计拆分出的 run 段数。
 *
 * 必须在提交前精确置位：run 的完成可能**同步**触发 on_sub_complete（如整段未映射读），
 * 计数为 0 时同步回调会把父请求提前置完成。
 *
 * @param controller Controller 上下文
 * @param request    主机请求
 * @return run 段数（>0）
 */
static DWORD ssd_split_count(ssd_ctl_s *controller, ssd_req_s *request)
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

/**
 * @brief 按 FTL 实例边界把 host 请求拆成连续 run 并逐段提交。
 *
 * 部分 run 提交失败时保留已提交 run 的在飞计数，错误锁存到 agg_status；
 * 返回值只表示「是否受理」，最终状态由 complete_flag/completion_status 回执。
 *
 * @param controller Controller 上下文
 * @param request    主机请求（READ/WRITE）
 * @return SSD_STATUS_OK               全部/部分 run 已受理
 *         SSD_STATUS_INVALID_ARGUMENT 入参或 block_count 非法
 *         SSD_STATUS_NOT_SUPPORTED    opcode 不是 READ/WRITE
 *         SSD_STATUS_NO_SPACE / IO_ERROR 第一个 run 就提交失败（无在飞）
 */
ssd_status_e ssd_request_split(ssd_ctl_s *controller, ssd_req_s *request)
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
    const DWORD total_runs = ssd_split_count(controller, request);

    request->sub_remaining = total_runs;
    if (total_runs == 0U)
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 子请求公共字段（循环内复用 io 模板，仅改 lba/buffer/count） */
    request->io.complete_flag   = NULL; /* 子请求用 on_complete 聚合 */
    request->io.status_out      = &request->sub_status;
    request->io.on_complete     = on_sub_complete;
    request->io.on_complete_ctx = request;

    /* 「本 chunk 之后同一 host 命令还剩多少 LBA」只在本次 split 内有效：
     * request->io 是跨命令复用的模板（USB 每次 memset、CLI 是 static），把 per-run
     * 值留在里面会让下一次请求继承上一条命令的剩余长度。 */
    const DWORD chunk_remaining = request->io.cmd_remaining; /* 0 = 调用者未给（CLI / 命令末尾） */

    lba     = request->start_lba;
    lba_end = request->start_lba + request->block_count;

    while (lba < lba_end)
    {
        QWORD        run_end = ssd_controller_run_end(controller, lba, lba_end);
        DWORD        run_len = (run_end - lba);
        ftl_ctx_s   *ftl     = ssd_controller_route(controller, lba);
        ftl_status_e st;

        /* 路由只可能因宏配置错误失败；正常配置下恒非 NULL */
        ASSERT(ftl != NULL);

        request->io.lba           = lba;
        /* 缓冲步进 = 映射单元（FTL_IU_BYTES = 512B）——CW 级映射下 1 LBA = 1 个 CW
         * （= 512B 扇区），不是 2KB 页。 */
        request->io.buffer        = (BYTE *)request->buffer + ((size_t)(lba - request->start_lba) * FTL_IU_BYTES);
        request->io.count         = run_len; /* ★ 长度交给 FTL，plane 拆分由 FTL 决定 */
        /* 本 run 之后同一 host 命令还剩的 LBA = 本 chunk 余量 + chunk 内后续 run。 */
        request->io.cmd_remaining = chunk_remaining + (DWORD)(lba_end - run_end);

        st = (request->opcode == SSD_REQUEST_WRITE) ? ftl_submit_write(ftl, &request->io) : ftl_submit_read(ftl, &request->io);
        if (st != FTL_OK)
        {
            LOGE(LOG_TAG_CTRL "split submit failed lba=%llu count=%lu\r\n", lba, run_len);

            /* 本 run 及其后所有 run 都不会提交 ⇒ 从计数里扣掉「未提交数」。
             * 不能把它覆盖成 submitted：已经**同步完成**的 run 也计在 submitted 里，
             * 覆盖会让父请求永久等待一个不会到来的回调。 */
            const DWORD not_submitted = total_runs - submitted;

            ASSERT(not_submitted <= request->sub_remaining);
            if (not_submitted <= request->sub_remaining)
            {
                request->sub_remaining -= not_submitted;
            }
            else
            {
                /* 不应发生；release 下宁可收敛到 0（立即回执错误）也不要挂死。 */
                request->sub_remaining = 0U;
            }

            /* 首个非 OK 胜出：后续成功 run 不得覆盖这次失败。 */
            if (request->agg_status == SSD_STATUS_OK)
            {
                request->agg_status = err_status;
            }

            if (request->sub_remaining == 0U)
            {
                request->completion_status = request->agg_status;
                request->complete_flag     = 1U;
            }
            request->io.cmd_remaining = chunk_remaining; /* 还原模板：见上方注释 */
            return err_status;
        }
        submitted++;
        lba = run_end;
    }

    request->io.cmd_remaining = chunk_remaining; /* 还原模板：见上方注释 */

    return SSD_STATUS_OK;
}
