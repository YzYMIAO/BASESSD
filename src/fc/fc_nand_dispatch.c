/*
 * fc_nand_dispatch.c — NAND 命令分发器
 *
 * 职责：接收 FC 侧预解析的多命令请求（fc_nand_dispatch_request_t），
 *       送入目标 LUN 的待处理 FIFO，由 poll 驱动 NAND 引擎执行：
 *         FC 预解析 → submit/submit_chain 入队 → poll 出队 + 执行 + 回 completion
 *
 * 层次关系：
 *   上层 FC（fc_command.h）→ 本文件（dispatch）
 *     → fc_nand_start/step（devices/mt29f___g08a.c）→ nand_engine（UOP 解释器）→ FMC 后端
 *
 * 执行模型：
 *   - 单线程 poll 推进模型，无锁；依赖外部串行调用 poll。
 *   - submit/submit_chain 仅"登记"命令（按 room 追加 + 预加载 task），
 *     实际执行发生在后续 poll。
 *   - **每 LUN 一个待处理 FIFO（深 NAND_DISPATCH_CHAIN_DEPTH）**，每槽 = 一条命令；
 *     NAND 侧看不到"链"——一条链 = 连续 N 槽（FC 原子批量追加）。
 *     poll 遍历每 LUN FIFO 先进先出，每轮每 LUN 推进一条，完成一条即回一份 completion。
 *   - 相位机（Phase V）：IDLE → ISSUE → ISSUED → FETCH → IDLE（per-LUN，作用于队头槽）。
 *
 * 对外接口：fc_nand_dispatch_init / submit / submit_chain / poll / set_armed /
 *           is_armed / lun_free。
 */

#include "assertion.h"
#include "delay.h"
#include "fc_command.h"
#include "fc_nand_dispatch_internal.h"
#include "log.h"
#include "nand_config.h"
#include "nand_debug.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/* ======================================================================
 * 全局状态
 * ====================================================================== */

/* 每 LUN 待处理 FIFO：submit 按 room 追加，poll 逐条排空。
 * 每槽 = 一条命令；链 = 连续槽（由 submit_chain 原子批量追加保证）。 */
static fc_nand_dispatch_lun_queue_t g_fc_nand_dispatch_queues[CH_NUM][CE_NUM][LUN_NUM];
/* 异步路径放行闩锁的查询/置位在 NAND 层（nand_set_armed / nand_is_armed）：
 * 它同时被器件驱动的同步入口用作契约守卫，放在 NAND 才不会让 NAND 反向依赖 FC。
 * 本文件只在 fc_nand_dispatch_enqueue() 一处检查（submit 与 submit_chain 共用的漏斗），
 * poll 不检查。 */

/* 页缓冲固定在 AXI_NOCACHE（与基线一致）。
 * 板端实验：放到 DTCM 时 multi-plane program 的 handler 收到了正确的
 * bitmap/data，但 NAND 读回全 FF；回到 AXI_NOCACHE 后 format 正常。
 * DMA=1 时同样要求 noncacheable（DMA1/2 不能访问 DTCM）。
 * 按 (ch, ce, lun) 各持一块——同通道各 LUN 由 poll 轮流驱动，且 request.data
 * 指针会跨多次 poll 存活（如 program ISSUE 卡在 70h 忙轮询），共享一块会被后驱动的
 * LUN 覆盖（详见 fc_nand_dispatch_tables.c 顶部注释）。
 * 多 plane 时拼接双页（plane stride = PAGE_SIZE），scatter 拆页依赖它。 */
#define NAND_PAGE_BUFFER_ATTR __attribute__((section(".noncacheable_buffer")))
BYTE g_nand_channel_page_buffer[CH_NUM][CE_NUM][LUN_NUM][PAGE_BUFFER_SIZE] NAND_PAGE_BUFFER_ATTR;

/* ======================================================================
 * 辅助函数
 * ====================================================================== */

/**
 * @brief 初始化一个 completion 结构（全清零 + 哨兵）
 *
 * cw_status 全量预置 FC_CW_STATUS_UNUSED，避免脏数据。
 *
 * @param completion 目标
 * @param command_id FC 关联键
 */
static void fc_nand_dispatch_completion_init(fc_nand_dispatch_completion_t *completion, WORD command_id)
{
    BYTE i;

    memset(completion, 0, sizeof(*completion));
    completion->command_id           = command_id;
    completion->failed_descriptor_id = FC_DESCRIPTOR_ID_INVALID;
    for (i = 0U; i < CW_TOTAL_MAX; i++)
    {
        completion->cw_status[i] = FC_CW_STATUS_UNUSED;
    }
}

/**
 * @brief 统一收尾：写状态后按 operation 分派到 completion 策略 handler
 *
 * @param task           完成任务
 * @param result         执行结果
 * @param nand_completion NAND 完成
 */
static void fc_nand_dispatch_task_complete(fc_nand_dispatch_task_t *task, BYTE result, const fc_nand_completion_t *nand_completion)
{
    task->completion.execution_status = result;
    task->completion.nand_status      = nand_completion->nand_status;
    task->completion.pre_status       = nand_completion->pre_status;

    g_fc_nand_dispatch_completion_table[task->operation](task, result, nand_completion);
}

/**
 * @brief 把 FC 预解析 request 搬到 LUN FIFO 的 task slot
 *
 * 只搬掩码内的 cw_buffers；掩码外保持 slot 残留（消费点保证不读）。
 *
 * @param task    目标 slot
 * @param request 预解析请求
 */
static void fc_nand_dispatch_task_load(fc_nand_dispatch_task_t *task, const fc_nand_dispatch_request_t *request)
{
    BYTE n;
    BYTE plane;

    fc_nand_dispatch_completion_init(&task->completion, request->command_id);
    task->command_id                    = request->command_id;
    task->ch                            = request->ch;
    task->ce                            = request->ce;
    task->lun                           = request->lun;
    task->operation                     = request->operation;
    task->page_type                     = request->page_type;
    task->cw_bitmap                     = request->cw_bitmap;
    task->plane_count                   = request->plane_count;
    task->plane                         = request->plane;
    task->started                       = 0U;
    task->feature_addr                  = request->feature_addr;
    task->column                        = request->column;
    /* 绑定 FMC 后端（单 bank 全 LUN 共享）+ 引擎上下文三指针（地址随事务固定，只在此绑一次） */
    task->backend                       = nand_phy_backend_get();
    task->transaction.engine.backend    = task->backend;
    task->transaction.engine.request    = &task->transaction.request;
    task->transaction.engine.completion = &task->transaction.completion;
    /* 当前 CW = 掩码最低位：直接下发的取数 op（OP_RANDOM_DATA_READ）靠它定位自己那一位，
     * 加载型 op 靠它定首次取数的起点。空掩码（退化命令）退化为 0，保证
     * completion_per_cw 的下标不越界。 */
    task->cw_index                      = fc_nand_dispatch_cw_first(request->cw_bitmap);
    /* 逐格搬掩码内的 CW buffer；不在掩码里的格保持零值。 */
    for (n = fc_nand_dispatch_cw_next(request->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(request->cw_bitmap, (n + 1U)))
    {
        task->cw_buffers[n] = request->cw_buffers[n];
    }
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->plane_paa[plane] = request->plane_paa[plane];
    }
    /* 单 plane 有效行的归属由生产者保证，此处不再复核：掩码位 n 由 desc 的 PAA 本身
     * 算出（见 fc_build_request_data_planes），"在 n 位写了 buffer"与
     * "该 desc 的 plane/cw 位是 n"是同一条表达式。真正可能错位的情形（desc plane 位
     * 与 command->paa 不一致）在造链时就被 fc_command_configure_descriptor 的 plane 段
     * 校验（越段 → ASSERT）拦下，不会走到这里。
     * ⚠ 擦除位图为空 ⇒ 上面这个循环一格都不搬，cw_buffers[] 保持上一次占用
     * 该 slot 的残留值。擦除路径不读 cw_buffers[]（req_erase 只用 plane_paa），故安全；
     * 但"空掩码 = 无 buffer 可读"这条契约**依赖消费点真的不读**——新增无数据 op 时注意。 */
}

/* ======================================================================
 * 初始化 / 就绪
 * ====================================================================== */

/**
 * @brief 清零软件调度状态（不碰 FMC 硬件）
 *
 * @return 恒 NAND_RESULT_OK
 */
nand_result_e fc_nand_dispatch_init(void)
{
    memset(g_fc_nand_dispatch_queues, 0, sizeof(g_fc_nand_dispatch_queues));
    /* DTCM/非缓存段都不由启动代码清零；各 builder 虽会先 memset，这里仍显式清零，
     * 避免任何未来路径读到上电残留。 */
    memset(g_nand_channel_page_buffer, 0, sizeof(g_nand_channel_page_buffer));
    nand_set_armed(0U);
    return NAND_RESULT_OK;
}

/**
 * @brief 置/清异步路径放行闩锁（归一化到 0/1）
 *
 * @param armed 非 0 放行
 */
void fc_nand_dispatch_set_armed(BYTE armed)
{
    nand_set_armed(armed);
}

/**
 * @brief 目标 LUN FIFO 的剩余槽数
 *
 * @param ch  目标 ch
 * @param ce  目标 ce
 * @param lun 目标 lun
 * @return 剩余槽数
 */
BYTE fc_nand_dispatch_lun_free(BYTE ch, BYTE ce, BYTE lun)
{
    /* 唯一调用者是 fc_poll_fill，且调用前已 ASSERT 过 ch/ce/lun（同源值）。
     * 保留防御会退化成静默活锁（越界 → 恒 0 → 该 LUN 永久挂起），断言停机更好定位。 */
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    return (NAND_DISPATCH_CHAIN_DEPTH - g_fc_nand_dispatch_queues[ch][ce][lun].count);
}

/**
 * @brief 目标 LUN FIFO 的占用槽数
 *
 * @param ch  目标 ch
 * @param ce  目标 ce
 * @param lun 目标 lun
 * @return 占用槽数
 */
BYTE fc_nand_dispatch_lun_used(BYTE ch, BYTE ce, BYTE lun)
{
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    return (BYTE)g_fc_nand_dispatch_queues[ch][ce][lun].count;
}

/**
 * @brief 目标 LUN 的相位机当前相位
 *
 * @param ch  目标 ch
 * @param ce  目标 ce
 * @param lun 目标 lun
 * @return 相位（LUN_PHASE_*）
 */
BYTE fc_nand_dispatch_lun_phase(BYTE ch, BYTE ce, BYTE lun)
{
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    return g_fc_nand_dispatch_queues[ch][ce][lun].phase;
}

/* ======================================================================
 * submit — 入队（单命令 / 整链）
 *
 * 语义：按 room 追加到目标 LUN FIFO 的尾部。
 *   - 单命令 = count 1 的批。
 *   - 整链 = 原子批量追加：剩余深度不足整链则整体拒收，无副作用。
 *   - 追加后链占据连续槽位、按 FIFO 顺序执行，中间不可被后续命令插入。
 * ====================================================================== */

/**
 * @brief 把一批命令按 room 原子追加到 LUN FIFO
 *
 * 放行闩锁在此检查；剩余深度不足整批则整体拒收、无副作用。
 *
 * @param requests 命令数组（同 ch/ce/lun）
 * @param count    批大小
 * @return OK / INVALID_ARGUMENT 未放行 / NOT_READY 深度不足
 */
static nand_result_e fc_nand_dispatch_enqueue(const fc_nand_dispatch_request_t *requests, BYTE count)
{
    fc_nand_dispatch_lun_queue_t *queue;
    BYTE                          i;

    /* 异步路径放行闩锁：单命令与整链两个入口共用同一道门，结构上不会再分叉。
     * 未放行是运行时状态（非调用者编程错误）→ 保留返回码，不转 ASSERT。 */
    if (nand_is_armed() == 0U)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    /* 入参前置条件由调用者（fc.c 提交路径）保证 */
    ASSERT(requests != NULL);
    ASSERT((count != 0U) && (count <= NAND_DISPATCH_CHAIN_DEPTH));
    /* 整批逐条校验（不只 [0]）：队列按 requests[0] 的 LUN 选取，链成员必须同 LUN；
     * 掩码上界是 completion_* 写 cw_status[n] 的前置。 */
    for (i = 0U; i < count; i++)
    {
        ASSERT(requests[i].ch < CH_NUM);
        ASSERT(requests[i].ce < CE_NUM);
        ASSERT(requests[i].lun < LUN_NUM);
        ASSERT((requests[i].ch == requests[0U].ch) && (requests[i].ce == requests[0U].ce) && (requests[i].lun == requests[0U].lun));
        ASSERT((requests[i].cw_bitmap & ~(((1UL << CW_TOTAL_MAX) - 1UL))) == 0UL);
    }

    queue = &g_fc_nand_dispatch_queues[requests[0U].ch][requests[0U].ce][requests[0U].lun];

    /* 原子性检查：剩余深度必须容纳整批 */
    if ((queue->count + count) > NAND_DISPATCH_CHAIN_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }

    for (i = 0U; i < count; i++)
    {
        BYTE slot = ((queue->head + queue->count + i) % NAND_DISPATCH_CHAIN_DEPTH);
        fc_nand_dispatch_task_load(&queue->tasks[slot], &requests[i]);
    }
    /* 全部预加载后才提交 count：poll 不会看到半挂载的批 */
    queue->count = (queue->count + count);
    return NAND_RESULT_OK;
}

/**
 * @brief 单命令入队（非阻塞登记）
 *
 * @param request 预解析请求
 * @return OK / INVALID_ARGUMENT / NOT_READY
 */
nand_result_e fc_nand_dispatch_submit(const fc_nand_dispatch_request_t *request)
{
    return fc_nand_dispatch_enqueue(request, 1U);
}

/**
 * @brief 整链原子入队
 *
 * @param requests 命令数组
 * @param count    链长
 * @return OK / INVALID_ARGUMENT / NOT_READY
 */
nand_result_e fc_nand_dispatch_submit_chain(const fc_nand_dispatch_request_t *requests, BYTE count)
{
    return fc_nand_dispatch_enqueue(requests, count);
}

/* ======================================================================
 * poll — 推进各 LUN FIFO 队头事务
 *
 * 遍历模型（每 LUN 一个 FIFO，先进先出）：
 *   - 外层 round-robin 遍历通道（从上次返回完成的通道的下一个续走）。
 *   - 每 LUN 每轮只推进一次（fc_nand_dispatch_lun_step）：未启动则启动，已启动则 step；
 *     完成一条命令即回一份 completion 并出队（head++/count--）。
 *   - 队列空（count==0）的 LUN 直接跳过。
 *
 * 相位机（Phase V，作用于队头槽）：IDLE → ISSUE → ISSUED → FETCH → IDLE
 *   ISSUE：发完命令即返回，LUN 进入 tR/tPROG/tBERS
 *   ISSUED：SysTick 计时最短忙期，到点后启动配对 WAIT/FETCH
 *   FETCH：执行 WAIT_READY + 数据 + 状态检查
 *
 * 完整状态机图见 docs/design/fc_nand_design.md §23（图 20）。
 *
 * 本段函数按职责组织：
 *   Command issue  —— task_fc_start / task_start
 *   Per-LUN FSM    —— issued_advance / lun_drive / lun_step
 *   Data path      —— task_cw_chain / scatter_page_buffer
 * ====================================================================== */

/* ── 单 LUN 单步推进结果 ── */
typedef enum
{
    NAND_DISPATCH_LUN_WAIT = 0, /* 本轮无进展，调用方看下一 LUN */
    NAND_DISPATCH_LUN_DONE      /* 队头命令完成，*completion 已回填并出队 */
} fc_nand_dispatch_lun_outcome_t;

/* ── 命令发起结果：req_table 构建无失败路径（见 fc_nand_dispatch_req_handler_t），
 *    故只剩"异步待下轮 step"与"同步完成/带错完成"两种。注意 fc_nand_start 返回的
 *    INVALID_ARGUMENT/TIMEOUT/OPERATION_FAILED 不是失败出口，而是"带错误状态的完成"，
 *    必须继续走完成处理链。 ── */
typedef enum
{
    NAND_DISPATCH_START_ASYNC = 0, /* fc_nand_start 返回 NOT_READY，待下轮 step */
    NAND_DISPATCH_START_SYNC       /* 同步完成或返回错误码，*fc_result 有效 */
} fc_nand_dispatch_start_outcome_t;

/* ── CW 链式续读结果（无 ERROR 出口：链内 start 错误仍走完成路径） ── */
typedef enum
{
    NAND_DISPATCH_CW_ASYNC = 0, /* 下一 CW 异步启动，本轮到此为止 */
    NAND_DISPATCH_CW_DONE       /* 链式续读结束 */
} fc_nand_dispatch_cw_outcome_t;

/* ── 单 LUN 上下文。不缓存 task 指针——出队后旧指针失效，
 *    一律由 &queue->tasks[queue->head] 现取（head 在单步内不变）。
 *    也不缓存 ch/ce/lun：底层 task 自带这三项（见 fc_nand_dispatch_task_t），
 *    需要时经 task 取即可，本层不重复。 ── */
typedef struct
{
    fc_nand_dispatch_completion_t *completion; /* 完成输出目标 */
    fc_nand_dispatch_lun_queue_t  *queue;
    fc_nand_completion_t           nand_completion; /* 读前必写（active==0 分支依赖同轮先写） */
    BYTE                           result;
} fc_nand_dispatch_lun_ctx_t;

/**
 * @brief 发起异步 ONFI 事务并置初始相位
 *
 * @param queue     所属 LUN FIFO
 * @param task      目标任务
 * @param fc_result 输出：fc_nand_start 返回码
 * @return ASYNC / SYNC
 */
static fc_nand_dispatch_start_outcome_t fc_nand_dispatch_task_fc_start(fc_nand_dispatch_lun_queue_t *queue, fc_nand_dispatch_task_t *task, BYTE *fc_result)
{
    BYTE result;

    result        = fc_nand_start(&task->transaction, task->operation, &task->request);
    task->started = 1U;
    if (nand_operation_is_issue(task->operation) != 0U)
    {
        queue->phase = LUN_PHASE_ISSUE;
    }
    else
    {
        queue->phase = LUN_PHASE_FETCH;
    }
    *fc_result = result;
    return (result == NAND_RESULT_NOT_READY) ? NAND_DISPATCH_START_ASYNC : NAND_DISPATCH_START_SYNC;
}

/**
 * @brief 构建 request 并发起事务（命令启动唯一入口，含日志点）
 *
 * @param queue     所属 LUN FIFO
 * @param task      目标任务
 * @param fc_result 输出：fc_nand_start 返回码
 * @return ASYNC / SYNC
 */
static fc_nand_dispatch_start_outcome_t fc_nand_dispatch_task_start(fc_nand_dispatch_lun_queue_t *queue, fc_nand_dispatch_task_t *task, BYTE *fc_result)
{
    g_fc_nand_dispatch_req_table[task->operation](task);

    DISP_LOGD("FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n", task->operation, (void *)task->request.data, task->request.data_length, task->request.column, task->request.row[0]);

    return fc_nand_dispatch_task_fc_start(queue, task, fc_result);
}

/**
 * @brief ISSUED 相位推进：等最短忙期到点，启动配对的 WAIT/FETCH
 *
 * @param ctx 单 LUN 上下文
 * @return WAIT / DONE（nand_completion 有效）
 */
static fc_nand_dispatch_lun_outcome_t fc_nand_dispatch_issued_advance(fc_nand_dispatch_lun_ctx_t *ctx)
{
    fc_nand_dispatch_task_t *task   = &ctx->queue->tasks[ctx->queue->head];
    WORD                     min_us = g_nand_operation_map[task->operation].min_busy_us;
    BYTE                     pair_op;
    BYTE                     result;

    /* ISSUE 段已完成但失败（如起始 70h 状态 FAIL）⇒ 别再发 WAIT/取数。
     * 否则后续逐 CW 取数可能把已标失败的状态覆盖成成功。 */
    if (task->completion.execution_status != NAND_RESULT_OK)
    {
        ctx->nand_completion.execution_status = task->completion.execution_status;
        ctx->nand_completion.nand_status      = task->completion.nand_status;
        ctx->nand_completion.pre_status       = task->completion.pre_status;
        ctx->result                           = task->completion.execution_status;
        return NAND_DISPATCH_LUN_DONE;
    }

    if ((min_us != 0U) && !delay_time_elapsed_us(ctx->queue->wait_start_us, min_us))
    {
        return NAND_DISPATCH_LUN_WAIT; /* 最短等待未到，下一轮再检查 */
    }

    pair_op = nand_operation_get_pair(task->operation);
    if ((pair_op != 0U) && (pair_op != task->operation))
    {
        task->operation = pair_op;
    }

    if (fc_nand_dispatch_task_start(ctx->queue, task, &result) == NAND_DISPATCH_START_ASYNC)
    {
        return NAND_DISPATCH_LUN_WAIT;
    }

    ctx->nand_completion.execution_status = result;
    ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
    ctx->nand_completion.pre_status       = task->transaction.completion.pre_status;
    ctx->result                           = result;
    return NAND_DISPATCH_LUN_DONE;
}

/**
 * @brief Per-LUN FSM 单步：启动或推进队头事务
 *
 * @param ctx 单 LUN 上下文（result / nand_completion 就地更新）
 * @return WAIT / DONE
 */
static fc_nand_dispatch_lun_outcome_t fc_nand_dispatch_lun_drive(fc_nand_dispatch_lun_ctx_t *ctx)
{
    fc_nand_dispatch_task_t *task = &ctx->queue->tasks[ctx->queue->head];
    BYTE                     result;

    if (task->started == 0U)
    {
        if (fc_nand_dispatch_task_start(ctx->queue, task, &result) == NAND_DISPATCH_START_ASYNC)
        {
            return NAND_DISPATCH_LUN_WAIT;
        }
        ctx->nand_completion.execution_status = result;
        ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
        ctx->nand_completion.pre_status       = task->transaction.completion.pre_status;
    }
    else if (task->transaction.engine.active != 0U)
    {
        result = fc_nand_step(&task->transaction, &ctx->nand_completion);
        if (result == NAND_RESULT_NOT_READY)
        {
            return NAND_DISPATCH_LUN_WAIT;
        }
    }
    else
    {
        result = ctx->nand_completion.execution_status;
    }

    ctx->result = result;

    /* ── 相位 ISSUE → ISSUED ──
     * 相位由上方 task_fc_start 置入（ISSUE 型 OP → ISSUE，其余 → FETCH）。
     * 命中即"命令只发完了前半段"：task_complete 已调用、task->completion 已填好，
     * 但 LUN 还在 tR/tPROG/tBERS，绝不能往下走 DONE 把结果当命令完成报出去。 */
    if (ctx->queue->phase == LUN_PHASE_ISSUE)
    {
        fc_nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        ctx->queue->phase = LUN_PHASE_ISSUED;
        /* 记录 tR/tPROG/tBERS 起点，供 ISSUED 块最短等待用 */
        (void)delay_time_now_us(&ctx->queue->wait_start_us);
        return NAND_DISPATCH_LUN_WAIT; /* 不报完成，下一轮由 ISSUED 块接管 */
    }
    return NAND_DISPATCH_LUN_DONE;
}

/**
 * @brief 是否为「页已加载进页缓冲」的 op
 *
 * @param operation OP_*
 * @return 1 是；0 否
 */
static BYTE fc_nand_dispatch_is_load_complete_op(BYTE operation)
{
    return ((operation == OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) || (operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT) || (operation == OP_READ_PAGE_WAIT)) ? 1U : 0U;
}

/**
 * @brief 是否为「从页缓冲逐 CW 取数」的 op
 *
 * @param operation OP_*
 * @return 1 是；0 否
 */
static BYTE fc_nand_dispatch_is_fetch_op(BYTE operation)
{
    return ((operation == OP_RANDOM_DATA_READ) || (operation == OP_RANDOM_DATA_READ_MULTI_PLANE)) ? 1U : 0U;
}

/**
 * @brief CW 级链式推进：加载进页缓冲后逐 CW 取数
 *
 * @param ctx 单 LUN 上下文
 * @return CW_ASYNC / CW_DONE
 */
static fc_nand_dispatch_cw_outcome_t fc_nand_dispatch_task_cw_chain(fc_nand_dispatch_lun_ctx_t *ctx)
{
    fc_nand_dispatch_task_t *task   = &ctx->queue->tasks[ctx->queue->head];
    BYTE                     result = ctx->result;

    for (;;)
    {
        BYTE n;

        fc_nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        if (result != NAND_RESULT_OK)
        {
            break;
        }

        if (fc_nand_dispatch_is_load_complete_op(task->operation) != 0U)
        {
            /* 加载完成：数据已在页缓冲，从掩码最低位起取 */
            n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U);
            if (n == CW_TOTAL_MAX)
            {
                break;
            }
            task->cw_index = n;
        }
        else if (fc_nand_dispatch_is_fetch_op(task->operation) != 0U)
        {
            /* 取数：推进到下一个置位，链尾则结束 */
            n = fc_nand_dispatch_cw_next(task->cw_bitmap, (task->cw_index + 1U));
            if (n == CW_TOTAL_MAX)
            {
                break;
            }
            task->cw_index = n;
        }
        else if (((task->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT) || (task->operation == OP_OTP_DATA_READ)) && ((n = fc_nand_dispatch_cw_next(task->cw_bitmap, (task->cw_index + 1U))) != CW_TOTAL_MAX))
        {
            /* 旧语义：最低位那个 CW 已由加载 op 的 DATA_OUT 取回，其余逐 CW 随机读 */
            task->cw_index = n;
        }
        else
        {
            break;
        }

        task->operation = (task->plane_count > 0U) ? OP_RANDOM_DATA_READ_MULTI_PLANE : OP_RANDOM_DATA_READ;
        task->started   = 1U; /* 复用 start 路径但标记已启动 */
        {
            BYTE plane     = (task->cw_index / CW_NUM);
            BYTE cw        = (task->cw_index % CW_NUM);
            WORD cw_column = (cw * CW_SIZE);

            memset(&task->request, 0, sizeof(task->request));
            task->request.column      = cw_column;
            task->request.row[0]      = paa_to_nand_row(task->plane_paa[plane]);
            task->request.data        = task->cw_buffers[task->cw_index];
            task->request.data_length = CW_SIZE;
        }
        result = fc_nand_start(&task->transaction, task->operation, &task->request);
        if (result == NAND_RESULT_NOT_READY)
        {
            break;
        }
        ctx->nand_completion.execution_status = result;
        ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
        ctx->nand_completion.pre_status       = task->transaction.completion.pre_status;
    }

    ctx->result = result;
    return (result == NAND_RESULT_NOT_READY) ? NAND_DISPATCH_CW_ASYNC : NAND_DISPATCH_CW_DONE;
}

/**
 * @brief 把页缓冲整页数据拆回各 CW buffer
 *
 * 只遍历掩码内的置位；单 plane / 双 plane 的 offset 规则不同。
 *
 * @param task 已完成任务
 */
static void fc_nand_dispatch_scatter_page_buffer(const fc_nand_dispatch_task_t *task)
{
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];
    BYTE  n;

    if ((task->operation == OP_READ_PAGE_CACHE_START_SEQ) || (task->operation == OP_READ_PAGE_CACHE_SEQ) || (task->operation == OP_READ_PAGE_CACHE_RANDOM) || (task->operation == OP_READ_PAGE_CACHE_LAST))
    {
        for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
        {
            BYTE cw_num = (n % CW_NUM);
            memcpy(task->cw_buffers[n], &page_buffer[cw_num * CW_SIZE], CW_SIZE);
        }
    }

    if ((task->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT) || (task->operation == OP_READ_PAGE_CACHE_START_SEQ_MP) || (task->operation == OP_READ_PAGE_CACHE_SEQ_MP) || (task->operation == OP_READ_PAGE_CACHE_LAST_MP))
    {
        for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
        {
            BYTE  plane  = (n / CW_NUM);
            BYTE  cw_num = (n % CW_NUM);
            BYTE *src    = &page_buffer[plane * PAGE_SIZE + cw_num * CW_SIZE];
            memcpy(task->cw_buffers[n], src, CW_SIZE);
        }
    }
}

/**
 * @brief Per-LUN FSM 单步：推进队头命令，完成即回填并出队
 *
 * @param ctx 单 LUN 上下文
 * @return WAIT / DONE
 */
static fc_nand_dispatch_lun_outcome_t fc_nand_dispatch_lun_step(fc_nand_dispatch_lun_ctx_t *ctx)
{
    fc_nand_dispatch_task_t       *task;
    fc_nand_dispatch_lun_outcome_t out;

    /* ── 相位 ISSUED 预检（只判一次）── */
    if (ctx->queue->phase == LUN_PHASE_ISSUED)
    {
        out = fc_nand_dispatch_issued_advance(ctx);
        if (out != NAND_DISPATCH_LUN_DONE)
        {
            return out; /* WAIT（DONE 落到下方收尾） */
        }
        /* DONE：同步完成，落到下方驱动路径收尾 */
    }

    task = &ctx->queue->tasks[ctx->queue->head];
    out  = fc_nand_dispatch_lun_drive(ctx);
    if (out != NAND_DISPATCH_LUN_DONE)
    {
        return out; /* WAIT（DONE 落到下方收尾） */
    }

    if (fc_nand_dispatch_task_cw_chain(ctx) == NAND_DISPATCH_CW_ASYNC)
    {
        return NAND_DISPATCH_LUN_WAIT;
    }

    fc_nand_dispatch_scatter_page_buffer(task);

    /* ── 队头命令完成：回填 completion、出队、复位相位 ── */
    *ctx->completion = task->completion;
    ctx->queue->head = ((ctx->queue->head + 1U) % NAND_DISPATCH_CHAIN_DEPTH);
    ctx->queue->count--;
    ctx->queue->phase = LUN_PHASE_IDLE;
    return NAND_DISPATCH_LUN_DONE;
}

/**
 * @brief 遍历全部 LUN，每 LUN 推进一条；完成经 sink 逐条交出
 *
 * 全遍历不早退，保证公平；返回本轮产出条数。
 *
 * @param sink 完成回调
 * @param ctx  透传给 sink 的上下文
 * @return 本轮完成条数
 */
BYTE fc_nand_dispatch_poll(fc_nand_dispatch_sink_t sink, void *ctx)
{
    fc_nand_dispatch_completion_t completion;
    BYTE                          delivered = 0U;
    BYTE                          channel;
    BYTE                          ce;
    BYTE                          lun_idx;

    /* 回调前置条件由调用者保证（fc.c 轮询路径恒传有效 sink） */
    ASSERT(sink != NULL);

    for (channel = 0U; channel < CH_NUM; channel++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun_idx = 0U; lun_idx < LUN_NUM; lun_idx++)
            {
                fc_nand_dispatch_lun_queue_t  *queue = &g_fc_nand_dispatch_queues[channel][ce][lun_idx];
                fc_nand_dispatch_lun_ctx_t     lun_ctx;
                fc_nand_dispatch_lun_outcome_t out;

                if (queue->count == 0U)
                {
                    continue; /* 该 LUN 无在途任务，跳过 */
                }

                /* completion 只在 lun_step 返回 DONE 时被写入，此处复用同一槽 */
                lun_ctx.completion = &completion;
                lun_ctx.queue      = queue;
                memset(&lun_ctx.nand_completion, 0, sizeof(lun_ctx.nand_completion));
                lun_ctx.result = NAND_RESULT_OK;

                out = fc_nand_dispatch_lun_step(&lun_ctx);
                if (out == NAND_DISPATCH_LUN_DONE)
                {
                    sink(&completion, ctx);
                    delivered++;
                }
            }
        }
    }
    return delivered;
}
