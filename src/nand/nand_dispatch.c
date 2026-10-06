/*
 * nand_dispatch.c — NAND 命令分发器
 *
 * 职责：接收 FC 侧预解析的多命令请求（nand_dispatch_request_t），
 *       送入目标 LUN 的待处理 FIFO，由 poll 驱动 NAND 引擎执行：
 *         FC 预解析 → submit/submit_chain 入队 → poll 出队 + 执行 + 回 completion
 *
 * 层次关系：
 *   上层 FC（fc_command.h）→ 本文件（dispatch）
 *     → nand_fc_start/step（nand_mt29*.c）→ nand_engine（UOP 解释器）→ FMC 后端
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
 * 对外接口：nand_dispatch_init / submit / submit_chain / poll / set_device_ready /
 *           lun_free。
 */

#include "common/assertion.h"
#include "common/log.h"
#include "delay.h"
#include "fc/fc_command.h"
#include "nand/nand_debug.h"
#include "nand_config.h"
#include "nand_dispatch_internal.h"
#include "nand_mt29f4g08abadawp.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/* ======================================================================
 * 全局状态
 * ====================================================================== */

/* 每 LUN 待处理 FIFO：submit 按 room 追加，poll 逐条排空。
 * 每槽 = 一条命令；链 = 连续槽（由 submit_chain 原子批量追加保证）。 */
static nand_dispatch_lun_queue_t g_nand_dispatch_queues[CH_NUM][CE_NUM][LUN_NUM];
/* round-robin 游标：poll 从上次返回完成的通道的下一个通道续走，保证各通道公平 */
static uint8_t g_nand_channel_cursor;
/* 设备就绪门控：仅 submit 准入检查，poll 不检查 */
static uint8_t g_nand_device_ready;

/* 页缓冲：置于 noncacheable_buffer 段（DMA/FMC 直访，D-Cache 关闭）。
 * 按 (ch, ce, lun) 各持一块——同通道各 LUN 由 poll 轮流驱动，且 request.data
 * 指针会跨多次 poll 存活（如 program ISSUE 卡在 70h 忙轮询），共享一块会被后驱动的
 * LUN 覆盖（详见 nand_dispatch_tables.c 顶部注释）。
 * 多 plane 时拼接双页（plane stride = PAGE_SIZE），scatter 拆页依赖它。 */
uint8_t g_nand_channel_page_buffer[CH_NUM][CE_NUM][LUN_NUM][PAGE_BUFFER_SIZE] __attribute__((section(".noncacheable_buffer")));

/* ======================================================================
 * 辅助函数
 * ====================================================================== */

/* --------------------------------------------------------------------
 * nand_dispatch_completion_init
 *   功能：初始化一个 completion 结构（全清零 + 哨兵值）。
 *   参数：completion —— 目标；command_id —— FC 关联键。
 *   返回：无。
 *   实现要点：
 *     - *completion = {0} 复合字面量整体清零。
 *     - failed_descriptor_id 置 FC_DESCRIPTOR_ID_INVALID。
 *     - cw_status 用 CW_TOTAL_MAX（=8，单/多 plane 最大值）全量预置
 *       FC_CW_STATUS_UNUSED，避免脏数据（此时 cw_count 尚未拷贝）。
 * -------------------------------------------------------------------- */
static void nand_dispatch_completion_init(nand_dispatch_completion_t *completion, uint16_t command_id)
{
    uint8_t i;

    *completion                      = (nand_dispatch_completion_t){0};
    completion->command_id           = command_id;
    completion->failed_descriptor_id = FC_DESCRIPTOR_ID_INVALID;
    for (i = 0U; i < CW_TOTAL_MAX; i++)
    {
        completion->cw_status[i] = FC_CW_STATUS_UNUSED;
    }
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_complete
 *   功能：统一收尾路径——写 execution_status/nand_status 后按 operation
 *         分派到对应 completion 策略 handler。
 *   参数：task —— 完成任务；result —— 执行结果；nand_completion —— NAND 完成。
 *   返回：无。
 *   实现要点：经 g_nand_dispatch_completion_table[operation] 分派
 *        （completion_per_cw / all_cws / multi_plane，见 nand_dispatch_tables.c）。
 *         注意 poll 中多处先改写 task->operation 再 complete，handler 按
 *         最新 operation 匹配（如 LOAD→FETCH 切换）。
 * -------------------------------------------------------------------- */
static void nand_dispatch_task_complete(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    task->completion.execution_status = result;
    task->completion.nand_status      = nand_completion->nand_status;

    g_nand_dispatch_completion_table[task->operation](task, result, nand_completion);
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_load
 *   功能：把 FC 预解析的 request 搬运到 LUN FIFO 的 task slot。
 *   参数：task —— 目标 slot；request —— 预解析请求。
 *   返回：无。
 *   实现要点：
 *     - 复位 cw_index=0 / started=0（poll 的启动判断依赖 started==0）。
 *     - Phase V：绑定 per-LUN backend，并一次性绑定引擎上下文三指针——
 *       backend/request/completion 随 transaction 固定（指向 transaction 自己的
 *       存储，地址不变），nand_engine_start 只收 uops/timing。
 *     - 双层数组 cw_paa/cw_buffers[plane][cw] 按 request->cws 逐 plane 逐 CW 搬运，
 *       plane 维恒为物理 plane 索引（不再写死 0/1）。plane 字段必须无条件赋值——
 *       task slot 会被 FIFO 复用，本函数不做整体 memset。
 *     - 每 plane 的 CW 数取自 request->cw_count[plane]（flat 掩码下各 plane 可不同）；
 *       槽位 0 无条件拷（承载行地址，空 plane 也要带对 BA6），槽位 1..count-1 按界拷。
 * -------------------------------------------------------------------- */
static void nand_dispatch_task_load(nand_dispatch_task_t *task, const nand_dispatch_request_t *request)
{
    uint8_t cw;
    uint8_t plane;

    nand_dispatch_completion_init(&task->completion, request->command_id);
    task->command_id                    = request->command_id;
    task->ch                            = request->ch;
    task->ce                            = request->ce;
    task->lun                           = request->lun;
    task->operation                     = request->operation;
    task->page_type                     = request->page_type;
    task->cw_index                      = 0U;
    task->plane_count                   = request->plane_count;
    task->plane                         = request->plane;
    task->started                       = 0U;
    task->feature_addr                  = request->feature_addr;
    /* 绑定 FMC 后端（单 bank 全 LUN 共享）+ 引擎上下文三指针（地址随事务固定，只在此绑一次） */
    task->backend                       = nand_fmc_backend_get();
    task->transaction.engine.backend    = task->backend;
    task->transaction.engine.request    = &task->transaction.request;
    task->transaction.engine.completion = &task->transaction.completion;
    /* 无条件照拷 PLANE_NUM 行、每行**槽位 0 无条件拷**：request 已 {0} 初始化，FC 只为
     * 命令 plane 集合内的 plane 填 cws[plane][0].paa（承载行地址，即使该 plane 的 CW 数
     * 为 0——flat 掩码下 plane0 可为空，见 fc_build_dispatch_request）。槽位 1..count-1
     * 按每-plane 计数拷；未写行确定性为零（PAA=0 / buffer=NULL）。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint8_t count = request->cw_count[plane];

        task->cw_count[plane]       = count;
        task->cw_paa[plane][0U]     = request->cws[plane][0U].paa;
        task->cw_buffers[plane][0U] = request->cws[plane][0U].buffer;
        for (cw = 1U; cw < count; cw++)
        {
            task->cw_paa[plane][cw]     = request->cws[plane][cw].paa;
            task->cw_buffers[plane][cw] = request->cws[plane][cw].buffer;
        }
    }
    /* 单 plane 命令的有效行必须确实属于 plane 字段指向的 plane——producer（FC 按
     * command->paa 落位）与 consumer（req_* 读 cw_paa[task->plane][*]）错位时立即断言，
     * 而不是静默读到全零行、把行地址算成 0 去误操作 plane0/block0/page0。
     * 排除无 PAA 语义的命令：SET/GET_FEATURES 只填 buffer 不填 paa（cws[plane][0].paa 恒 0），
     * 本检查不适用。 */
    if ((request->plane_count == 0U) && (request->cw_count[request->plane] > 0U) && (request->operation != OP_SET_FEATURES) &&
        (request->operation != OP_GET_FEATURES))
    {
        ASSERT(paa_to_plane(request->cws[request->plane][0U].paa) == request->plane);
    }
}

/* ======================================================================
 * 初始化 / 就绪
 * ====================================================================== */

/* --------------------------------------------------------------------
 * nand_dispatch_init
 *   功能：清零软件调度状态。
 *   参数：无。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：仅清软件状态，不碰 FMC 硬件（见 nand_dispatch.h）。
 *   调用链：fc_init_internal() → 本函数。因为纯 memset，FC 可以先于
 *          nand_init()（碰 FMC 硬件）执行，见 main.c 的初始化级联。
 * -------------------------------------------------------------------- */
nand_result_t nand_dispatch_init(void)
{
    memset(g_nand_dispatch_queues, 0, sizeof(g_nand_dispatch_queues));
    g_nand_channel_cursor = 0U;
    g_nand_device_ready   = 0U;
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_dispatch_set_device_ready
 *   功能：置/清设备就绪门控位。
 *   参数：ready —— 非 0 置就绪。
 *   返回：无。
 *   实现要点：归一化到 0/1；device_ready 仅 submit 检查，poll 不检查。
 * -------------------------------------------------------------------- */
void nand_dispatch_set_device_ready(uint8_t ready)
{
    g_nand_device_ready = (ready != 0U) ? 1U : 0U;
}

/* --------------------------------------------------------------------
 * nand_dispatch_is_device_ready
 *   功能：查询设备就绪门控位。
 *   返回：1 = 已就绪（异步路径放行）；0 = 未就绪（初始化阶段）。
 *   实现要点：与 set_device_ready 归一化语义一致，恒返回 0/1；
 *         供 nand_fc_execute 的"仅初始化可调"契约断言使用。
 * -------------------------------------------------------------------- */
uint8_t nand_dispatch_is_device_ready(void)
{
    return g_nand_device_ready;
}

/* --------------------------------------------------------------------
 * nand_dispatch_lun_free
 *   功能：查询目标 LUN FIFO 的剩余槽数。
 *   参数：ch/ce/lun —— 目标。
 *   返回：剩余槽数（0 .. NAND_DISPATCH_CHAIN_DEPTH）；越界返回 0。
 *   实现要点：FC 填充 pass 靠它判断能否放下整条链（need ≤ free 才下发）。
 * -------------------------------------------------------------------- */
uint8_t nand_dispatch_lun_free(uint8_t ch, uint8_t ce, uint8_t lun)
{
    /* 值域校验保留返回码：越界是运行时参数，返回 0（放不下任何命令） */
    if ((ch >= CH_NUM) || (ce >= CE_NUM) || (lun >= LUN_NUM))
    {
        return 0U;
    }
    return (uint8_t)(NAND_DISPATCH_CHAIN_DEPTH - g_nand_dispatch_queues[ch][ce][lun].count);
}

/* ======================================================================
 * submit — 入队（单命令 / 整链）
 *
 * 语义：按 room 追加到目标 LUN FIFO 的尾部。
 *   - 单命令 = count 1 的批。
 *   - 整链 = 原子批量追加：剩余深度不足整链则整体拒收，无副作用。
 *   - 追加后链占据连续槽位、按 FIFO 顺序执行，中间不可被后续命令插入。
 * ====================================================================== */

/* --------------------------------------------------------------------
 * nand_dispatch_enqueue
 *   功能：把一批（1..NAND_DISPATCH_CHAIN_DEPTH）命令按 room 追加到 LUN FIFO。
 *   参数：requests —— 命令数组（同一 ch/ce/lun）；count —— 批大小。
 *   返回：NAND_RESULT_OK / NOT_READY（剩余深度不足整批，无副作用）。
 *   实现要点：
 *     - 空间原子检查用 (uint16_t)(count+queue->count) 无符号比较；
 *       count≤4、queue->count≤4，无溢出风险。
 *     - 逐槽 nand_dispatch_task_load() 到 tasks[(head+count+i)%DEPTH]；
 *       全部预加载完成后才提交 count（poll 不会看到半挂载的批）。
 * -------------------------------------------------------------------- */
static nand_result_t nand_dispatch_enqueue(const nand_dispatch_request_t *requests, uint8_t count)
{
    nand_dispatch_lun_queue_t *queue;
    uint8_t                    i;

    /* 入参前置条件由调用者（fc.c 提交路径）保证 */
    ASSERT(requests != NULL);
    ASSERT((count != 0U) && (count <= NAND_DISPATCH_CHAIN_DEPTH));
    ASSERT(requests[0U].ch < CH_NUM);
    ASSERT(requests[0U].ce < CE_NUM);
    ASSERT(requests[0U].lun < LUN_NUM);
    for (i = 0U; i < PLANE_NUM; i++)
    {
        ASSERT(requests[0U].cw_count[i] <= CW_NUM);
    }

    queue = &g_nand_dispatch_queues[requests[0U].ch][requests[0U].ce][requests[0U].lun];

    /* 原子性检查：剩余深度必须容纳整批 */
    if ((uint16_t)(queue->count + count) > NAND_DISPATCH_CHAIN_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }

    for (i = 0U; i < count; i++)
    {
        uint8_t slot = (uint8_t)((queue->head + queue->count + i) % NAND_DISPATCH_CHAIN_DEPTH);
        nand_dispatch_task_load(&queue->tasks[slot], &requests[i]);
    }
    /* 全部预加载后才提交 count：poll 不会看到半挂载的批 */
    queue->count = (uint8_t)(queue->count + count);
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_dispatch_submit
 *   功能：单命令入队（非阻塞登记）。
 *   参数：request —— 预解析的单个命令请求。
 *   返回：NAND_RESULT_OK / INVALID_ARGUMENT（设备未就绪）/ NOT_READY（FIFO 满）。
 *   实现要点：设备未就绪是运行时状态（非调用者编程错误），保留返回码；
 *         实际执行在后续 poll。
 * -------------------------------------------------------------------- */
nand_result_t nand_dispatch_submit(const nand_dispatch_request_t *request)
{
    if (g_nand_device_ready == 0U)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    return nand_dispatch_enqueue(request, 1U);
}

/* --------------------------------------------------------------------
 * nand_dispatch_submit_chain
 *   功能：整链原子入队（多命令按序执行）。
 *   参数：requests —— 命令数组；count —— 链长（1..NAND_DISPATCH_CHAIN_DEPTH）。
 *   返回：NAND_RESULT_OK / NOT_READY（剩余深度不足整链，无副作用）。
 * -------------------------------------------------------------------- */
nand_result_t nand_dispatch_submit_chain(const nand_dispatch_request_t *requests, uint8_t count)
{
    return nand_dispatch_enqueue(requests, count);
}

/* ======================================================================
 * poll — 推进各 LUN FIFO 队头事务
 *
 * 遍历模型（每 LUN 一个 FIFO，先进先出）：
 *   - 外层 round-robin 遍历通道（从上次返回完成的通道的下一个续走）。
 *   - 每 LUN 每轮只推进一次（nand_dispatch_lun_step）：未启动则启动，已启动则 step；
 *     完成一条命令即回一份 completion 并出队（head++/count--）。
 *   - 队列空（count==0）的 LUN 直接跳过。
 *
 * 相位机（Phase V，作用于队头槽）：IDLE → ISSUE → ISSUED → FETCH → IDLE
 *   ISSUE：发完命令即返回，LUN 进入 tR/tPROG/tBERS
 *   ISSUED：DWT 计时最短忙期，到点后启动配对 WAIT/FETCH
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
    NAND_DISPATCH_LUN_DONE,     /* 队头命令完成，*completion 已回填并出队 */
    NAND_DISPATCH_LUN_ERROR     /* req 构建失败，调用方须丢弃该 LUN 队列 */
} nand_dispatch_lun_outcome_t;

/* ── 命令发起结果：必须把 req_table 失败与 nand_fc_start 自身错误码分开。
 *    req_table 失败是致命的；nand_fc_start 返回的 INVALID_ARGUMENT/TIMEOUT/
 *    OPERATION_FAILED 只是"带错误状态的完成"，要继续走完成处理链。 ── */
typedef enum
{
    NAND_DISPATCH_START_REQ_ERROR = 0, /* req_table 构建失败 → 调用方 abort */
    NAND_DISPATCH_START_ASYNC,         /* nand_fc_start 返回 NOT_READY，待下轮 step */
    NAND_DISPATCH_START_SYNC           /* 同步完成或返回错误码，*fc_result 有效 */
} nand_dispatch_start_outcome_t;

/* ── CW 链式续读结果（无 ERROR 出口：链内 start 错误仍走完成路径） ── */
typedef enum
{
    NAND_DISPATCH_CW_ASYNC = 0, /* 下一 CW 异步启动，本轮到此为止 */
    NAND_DISPATCH_CW_DONE       /* 链式续读结束 */
} nand_dispatch_cw_outcome_t;

/* ── 单 LUN 上下文。不缓存 task 指针——出队后旧指针失效，
 *    一律由 &queue->tasks[queue->head] 现取（head 在单步内不变）。 ── */
typedef struct
{
    nand_dispatch_completion_t *completion; /* 完成输出目标 */
    nand_dispatch_lun_queue_t  *queue;
    nand_fc_completion_t        nand_completion; /* 读前必写（active==0 分支依赖同轮先写） */
    uint8_t                     channel;
    uint8_t                     ce;
    uint8_t                     lun;
    uint8_t                     result;
} nand_dispatch_lun_ctx_t;

/* --------------------------------------------------------------------
 * nand_dispatch_task_fc_start
 *   功能：发起异步 ONFI 事务并置初始相位。
 *   参数：queue —— 所属 LUN FIFO；task —— 目标任务；fc_result —— 输出 nand_fc_start 返回码。
 *   返回：NAND_DISPATCH_START_ASYNC / NAND_DISPATCH_START_SYNC。
 *   实现要点：
 *     - 相位按操作类型判定：ISSUE 型置 LUN_PHASE_ISSUE，其余（WAIT/FETCH/原子）置 FETCH。
 *       ISSUED 块的 pair_op 目标必为 WAIT/FETCH 型，故此处统一判定与"固定置 FETCH"等价。
 *     - 相位在 NOT_READY 判定之前提交——下一轮路由依赖它。
 * -------------------------------------------------------------------- */
static nand_dispatch_start_outcome_t nand_dispatch_task_fc_start(nand_dispatch_lun_queue_t *queue,
                                                                 nand_dispatch_task_t      *task,
                                                                 uint8_t                   *fc_result)
{
    uint8_t result;

    result        = nand_fc_start(&task->transaction, task->operation, &task->request);
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

/* --------------------------------------------------------------------
 * nand_dispatch_task_start
 *   功能：构建 request + 发起事务（命令启动的唯一入口，含日志点）。
 *   参数：queue —— 所属 LUN FIFO；task —— 目标任务；fc_result —— 输出 nand_fc_start 返回码。
 *   返回：REQ_ERROR（调用方须 abort）/ ASYNC / SYNC。
 *   实现要点：日志点必须夹在"构建 request"与"nand_fc_start"之间，
 *         故 build_request 与 fc_start 在同一函数内串联，不拆开。
 * -------------------------------------------------------------------- */
static nand_dispatch_start_outcome_t nand_dispatch_task_start(nand_dispatch_lun_queue_t *queue,
                                                              nand_dispatch_task_t      *task,
                                                              uint8_t                   *fc_result)
{
    if (g_nand_dispatch_req_table[task->operation](task) != NAND_RESULT_OK)
    {
        return NAND_DISPATCH_START_REQ_ERROR;
    }

    LOGD("nand: FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n",
         task->operation,
         (void *)task->request.data,
         task->request.data_length,
         task->request.column,
         (unsigned long)task->request.row[0]);

    return nand_dispatch_task_fc_start(queue, task, fc_result);
}

/* --------------------------------------------------------------------
 * nand_dispatch_issued_advance
 *   功能：LUN_PHASE_ISSUED 推进——等最短忙期到点，启动配对的 WAIT/FETCH。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：WAIT（忙期未到或启动异步）/ DONE（已同步完成，nand_completion 有效）/
 *         ERROR（req 构建失败）。
 *   实现要点：
 *     - min_busy_us = tR(38)/tPROG(700)/tBERS(5000)，带余量的典型值。
 *     - 到点后切配对 OP（LOAD↔FETCH / ISSUE↔WAIT）；原子 OP 的 pair_op==自身，
 *       跳过不切换。
 *     - 同步完成时相位已是 FETCH，调用方直接进入完成处理链。
 * -------------------------------------------------------------------- */
static nand_dispatch_lun_outcome_t nand_dispatch_issued_advance(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t *task   = &ctx->queue->tasks[ctx->queue->head];
    uint16_t              min_us = g_nand_operation_map[task->operation].min_busy_us;
    uint8_t               pair_op;
    uint8_t               result;

    if ((min_us != 0U) && !delay_dwt_elapsed_us(ctx->queue->wait_start_cycles, min_us))
    {
        return NAND_DISPATCH_LUN_WAIT; /* 最短等待未到，下一轮再检查 */
    }

    pair_op = nand_operation_get_pair(task->operation);
    if ((pair_op != 0U) && (pair_op != task->operation))
    {
        task->operation = pair_op;
    }

    switch (nand_dispatch_task_start(ctx->queue, task, &result))
    {
        case NAND_DISPATCH_START_REQ_ERROR:
            return NAND_DISPATCH_LUN_ERROR;
        case NAND_DISPATCH_START_ASYNC:
            return NAND_DISPATCH_LUN_WAIT;
        default:
            break;
    }

    ctx->nand_completion.execution_status = result;
    ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
    ctx->result                           = result;
    return NAND_DISPATCH_LUN_DONE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_lun_drive
 *   功能：Per-LUN FSM 单步——启动队头 pre-loaded 命令或推进已启动事务，
 *         并处理 ISSUE 段完成 → ISSUED 的相位切换。
 *   参数：ctx —— 单 LUN 上下文（result / nand_completion 就地更新）。
 *   返回：WAIT（本轮无进展）/ DONE（已产出 result）/ ERROR（req 构建失败）。
 *   实现要点：
 *     - started==0：submit 已预加载但未启动，走 task_start。
 *     - started!=0：transaction.engine.active 为 0 说明命令已在 start() 中同步
 *       完成（如 FETCH WAIT_READY 已 ready），直接取已存的 execution_status，
 *       不再 step（nand_fc_step 对 active==0 会返回 INVALID_ARGUMENT）。
 *     - ISSUE 段完成 → task_complete + 转 ISSUED + 记 DWT 起点，不报完成。
 * -------------------------------------------------------------------- */
static nand_dispatch_lun_outcome_t nand_dispatch_lun_drive(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t *task = &ctx->queue->tasks[ctx->queue->head];
    uint8_t               result;

    if (task->started == 0U)
    {
        switch (nand_dispatch_task_start(ctx->queue, task, &result))
        {
            case NAND_DISPATCH_START_REQ_ERROR:
                return NAND_DISPATCH_LUN_ERROR;
            case NAND_DISPATCH_START_ASYNC:
                return NAND_DISPATCH_LUN_WAIT;
            default:
                break;
        }
        ctx->nand_completion.execution_status = result;
        ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
    }
    else if (task->transaction.engine.active != 0U)
    {
        result = nand_fc_step(&task->transaction, &ctx->nand_completion);
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
        nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        ctx->queue->phase = LUN_PHASE_ISSUED;
        /* 记录 tR/tPROG/tBERS 起点，供 ISSUED 块最短等待用 */
        delay_dwt_cycles_now(&ctx->queue->wait_start_cycles);
        return NAND_DISPATCH_LUN_WAIT; /* 不报完成，下一轮由 ISSUED 块接管 */
    }
    return NAND_DISPATCH_LUN_DONE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_is_load_op / nand_dispatch_is_fetch_op
 *   功能：CW 链的 op 分类（决定链如何推进）。
 *   参数：operation —— OP_*。
 *   返回：1 = 是；0 = 否。
 *   实现要点：
 *     - load：只把整页搬进 NAND 页缓冲、自身不输出数据；完成后从槽位 0 起取数。
 *     - fetch：从页缓冲按列地址取单个 CW；每完成一个推进一个槽位。
 * -------------------------------------------------------------------- */
static uint8_t nand_dispatch_is_load_op(uint8_t operation)
{
    return ((operation == OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) ||
            (operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT))
               ? 1U
               : 0U;
}

static uint8_t nand_dispatch_is_fetch_op(uint8_t operation)
{
    return ((operation == OP_RANDOM_DATA_READ) || (operation == OP_RANDOM_DATA_READ_MULTI_PLANE)) ? 1U : 0U;
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_total_slots
 *   功能：本任务的 CW 链槽位总数。
 *   参数：task —— 任务。
 *   返回：槽位总数（单 plane = cw_count[plane]；多 plane = Σ cw_count[plane]）。
 *   实现要点：槽位按 plane-major 展开（plane0 的 CW 在前、plane1 在后），
 *         与 FC 的 desc 链序、completion.cw_status[] 下标三者一致。
 *         不能再用 plane_count × cw_count 的乘积——flat 掩码下各 plane 的
 *         CW 数可以不同。
 * -------------------------------------------------------------------- */
static uint8_t nand_dispatch_task_total_slots(const nand_dispatch_task_t *task)
{
    uint8_t plane;
    uint8_t total = 0U;

    if (task->plane_count == 0U)
    {
        return task->cw_count[task->plane];
    }
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        total = (uint8_t)(total + task->cw_count[plane]);
    }
    return total;
}

/* --------------------------------------------------------------------
 * nand_dispatch_slot_to_plane_cw
 *   功能：plane-major 链槽位 → (plane, plane 内 CW 槽位)。
 *   参数：task —— 任务；slot —— 链槽位（0..total_slots-1）；
 *         plane_out / ordinal_out —— 输出。
 *   返回：无。
 *   实现要点：在 cw_count[] 上走减定位（替代旧的 slot/cw_count 除法/取模）。
 *         **CW 号一律从 cw_paa[plane][ordinal] 经 paa_to_cw 取**，本函数给的是
 *         槽位号，两者在 flat 掩码下解耦（见 fc_command_t.cw_mask）。
 * -------------------------------------------------------------------- */
static void nand_dispatch_slot_to_plane_cw(const nand_dispatch_task_t *task, uint8_t slot, uint8_t *plane_out, uint8_t *ordinal_out)
{
    uint8_t plane = 0U;

    if (task->plane_count == 0U)
    {
        *plane_out   = task->plane;
        *ordinal_out = slot;
        return;
    }
    while ((plane + 1U < PLANE_NUM) && (slot >= task->cw_count[plane]))
    {
        slot -= task->cw_count[plane];
        plane++;
    }
    *plane_out   = plane;
    *ordinal_out = slot;
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_cw_chain
 *   功能：CW 级 chaining——「加载进页缓冲 + 逐 CW 取数」的链式推进。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：CW_ASYNC（下一 CW 异步启动，本轮到此为止）/ CW_DONE。
 *   实现要点：
 *     - 每步先 task_complete，再判断是否可续读。
 *     - **加载型 op**（WITHOUT_DATA_OUT 两种）完成后不推进槽位，直接从槽位 0 起发取数 op；
 *       失败则链在此终止（completion_load 已把失败刷满全部槽位）。
 *     - **取数 op** 每完成一个推进一个槽位，直到链尾。
 *     - **旧语义 op**（WITH_DATA_OUT 普通读 / OTP 读）：CW0 已由 DATA_OUT 取回，
 *       其余 CW 逐 CW 随机读——保留原行为（FC 已把普通读转成加载型，此路只剩直接下发场景）。
 *     - 取数 op 按 plane 选：单 plane → OP_RANDOM_DATA_READ（05h-列-E0h）；
 *       多 plane → OP_RANDOM_DATA_READ_MULTI_PLANE（06h-列-行-E0h，行地址带 plane 位）。
 *     - 多 plane 槽位按 plane-major 展开（plane0 的 CW 在前），与 FC 的 desc 链序、
 *       cw_status[] 下标三者一致；槽位→(plane, ordinal) 由 slot_to_plane_cw helper 走减得出
 *       （不再用 slot / cw_count 除法——flat 掩码下各 plane 的 CW 数可以不同，甚至为 0）。
 *     - 列地址 = **paa_to_cw(PAA) × CW_SIZE**：CW 号一律从 PAA 取，不用槽位号
 *       （fc_command_t.cw_mask 使两者解耦）。
 *     - 链内 start 的错误码仍走完成路径，只有 NOT_READY 才中止本轮。
 *
 * 注意：本函数是**命令内**多 CW 的推进，与 FC 侧"命令链"（多条命令）无关；
 *       命令链在 NAND 侧只是 FIFO 里的连续槽。
 * -------------------------------------------------------------------- */
static nand_dispatch_cw_outcome_t nand_dispatch_task_cw_chain(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t *task   = &ctx->queue->tasks[ctx->queue->head];
    uint8_t               result = ctx->result;
    /* 链槽位总数：单 plane = cw_count[plane]；多 plane = Σ cw_count[plane]（plane-major） */
    uint8_t total_slots          = nand_dispatch_task_total_slots(task);

    for (;;)
    {
        uint8_t plane;
        uint8_t cw;

        nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        if (result != NAND_RESULT_OK)
        {
            break;
        }

        if (nand_dispatch_is_load_op(task->operation) != 0U)
        {
            /* 加载完成：数据已在页缓冲，从槽位 0 起取（不改 cw_index，恒为 0） */
            if (total_slots == 0U)
            {
                break;
            }
            nand_dispatch_slot_to_plane_cw(task, 0U, &plane, &cw);
        }
        else if (nand_dispatch_is_fetch_op(task->operation) != 0U)
        {
            /* 取数：推进到下一槽位，链尾则结束 */
            if ((uint8_t)(task->cw_index + 1U) >= total_slots)
            {
                break;
            }
            task->cw_index++;
            nand_dispatch_slot_to_plane_cw(task, task->cw_index, &plane, &cw);
        }
        else if (((task->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT) ||
                  (task->operation == OP_OTP_DATA_READ)) &&
                 ((uint8_t)(task->cw_index + 1U) < task->cw_count[task->plane]))
        {
            /* 旧语义：CW0 已由加载 op 的 DATA_OUT 取回，其余逐 CW 随机读 */
            task->cw_index++;
            plane = task->plane;
            cw    = task->cw_index;
        }
        else
        {
            break;
        }

        task->operation = (task->plane_count > 0U) ? (uint8_t)OP_RANDOM_DATA_READ_MULTI_PLANE : (uint8_t)OP_RANDOM_DATA_READ;
        task->started   = 1U; /* 复用 start 路径但标记已启动 */
        {
            nand_paa_t cw_paa    = task->cw_paa[plane][cw];
            uint8_t   *cw_buffer = task->cw_buffers[plane][cw];
            /* 列地址 = CW 号 × CW_SIZE（CW 号从 PAA 取，槽位与 CW 号已解耦） */
            uint16_t cw_column   = (uint16_t)(paa_to_cw(cw_paa) * CW_SIZE);
            task->request        = (nand_fc_request_t){.column = cw_column, .row = {paa_to_nand_row(cw_paa)}, .data = cw_buffer, .data_length = (uint16_t)CW_SIZE};
        }
        result = nand_fc_start(&task->transaction, task->operation, &task->request);
        if (result == NAND_RESULT_NOT_READY)
        {
            break;
        }
        ctx->nand_completion.execution_status = result;
        ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
    }

    ctx->result = result;
    return (result == NAND_RESULT_NOT_READY) ? NAND_DISPATCH_CW_ASYNC : NAND_DISPATCH_CW_DONE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_scatter_page_buffer
 *   功能：把页缓冲的整页数据拆回各 CW buffer。
 *   参数：task —— 已完成任务（自带 ch/ce/lun，定位页缓冲）。
 *   返回：无。
 *   实现要点：三种操作的数据落在页缓冲而非各 CW buffer（FMC 数据路径约定）：
 *     - Cache 读（SEQ/RANDOM/LAST）与 OP_READ_PAGE_FETCH：单 plane，
 *       offset = CW 号 × CW_SIZE。
 *     - Multi-Plane WITH_DATA_OUT：双 plane 数据按 plane 顺序拼接，
 *       offset = plane × PAGE_SIZE + CW 号 × CW_SIZE。
 *     **页缓冲内的偏移一律用 paa_to_cw(PAA) 取 CW 号，不能用槽位号**：cw_mask 使
 *     链槽位与 CW 号解耦（见 nand_dispatch_task_cw_chain）。
 *     页缓冲按 (ch, ce, lun) 各持一块，故索引一律取自 task 自身。
 * -------------------------------------------------------------------- */
static void nand_dispatch_scatter_page_buffer(const nand_dispatch_task_t *task)
{
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];
    uint8_t  slot;
    uint8_t  plane;

    if ((task->operation == OP_READ_PAGE_CACHE_SEQ) || (task->operation == OP_READ_PAGE_CACHE_RANDOM) ||
        (task->operation == OP_READ_PAGE_CACHE_LAST) || (task->operation == OP_READ_PAGE_FETCH))
    {
        for (slot = 0U; slot < task->cw_count[task->plane]; slot++)
        {
            uint8_t cw_num = (uint8_t)paa_to_cw(task->cw_paa[task->plane][slot]);
            memcpy(task->cw_buffers[task->plane][slot], &page_buffer[cw_num * CW_SIZE], CW_SIZE);
        }
    }

    if (task->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT)
    {
        for (plane = 0U; plane < task->plane_count; plane++)
        {
            for (slot = 0U; slot < task->cw_count[plane]; slot++)
            {
                uint8_t  cw_num = (uint8_t)paa_to_cw(task->cw_paa[plane][slot]);
                uint8_t *src    = &page_buffer[plane * PAGE_SIZE + cw_num * CW_SIZE];
                memcpy(task->cw_buffers[plane][slot], src, CW_SIZE);
            }
        }
    }
}

/* --------------------------------------------------------------------
 * nand_dispatch_lun_step
 *   功能：Per-LUN FSM 单步——推进队头命令，完成即回填 completion 并出队。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：WAIT（本轮无进展，调用方看下一 LUN）/ DONE（队头命令完成，*completion
 *         已回填且已出队）/ ERROR（req 构建失败，调用方须丢弃该 LUN 队列）。
 *   实现要点：
 *     - ISSUED 块只在进入驱动前判一次（与原实现一致）：先等最短忙期。
 *       issued_advance 返回 DONE 时相位已是 FETCH，落到下方驱动路径收尾。
 *     - 返回 WAIT 的出口必须已提交全部持久状态：
 *       ISSUED 忙期未到 / 配对启动异步 / started==0 首次启动异步 / step 异步 /
 *       ISSUE 段完成转 ISSUED / CW 链续读异步。
 *     - 完成即出队（head 前移、count 递减、相位复位 IDLE）并推进通道游标。
 *       每槽 = 一条命令 = 一份 completion——链中间命令的状态不再被吞掉。
 * -------------------------------------------------------------------- */
static nand_dispatch_lun_outcome_t nand_dispatch_lun_step(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t       *task;
    nand_dispatch_lun_outcome_t out;

    /* ── 相位 ISSUED 预检（只判一次）── */
    if (ctx->queue->phase == LUN_PHASE_ISSUED)
    {
        out = nand_dispatch_issued_advance(ctx);
        if (out != NAND_DISPATCH_LUN_DONE)
        {
            return out; /* WAIT / ERROR */
        }
        /* DONE：同步完成，落到下方驱动路径收尾 */
    }

    task = &ctx->queue->tasks[ctx->queue->head];
    out  = nand_dispatch_lun_drive(ctx);
    if (out != NAND_DISPATCH_LUN_DONE)
    {
        return out; /* WAIT / ERROR */
    }

    if (nand_dispatch_task_cw_chain(ctx) == NAND_DISPATCH_CW_ASYNC)
    {
        return NAND_DISPATCH_LUN_WAIT;
    }

    nand_dispatch_scatter_page_buffer(task);

    /* ── 队头命令完成：回填 completion、出队、复位相位、推进通道游标 ── */
    *ctx->completion = task->completion;
    ctx->queue->head = (uint8_t)((ctx->queue->head + 1U) % NAND_DISPATCH_CHAIN_DEPTH);
    ctx->queue->count--;
    ctx->queue->phase     = LUN_PHASE_IDLE;
    g_nand_channel_cursor = (uint8_t)((ctx->channel + 1U) % CH_NUM);
    return NAND_DISPATCH_LUN_DONE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_poll
 *   功能：遍历各通道各 LUN 的待处理 FIFO，每 LUN 推进一条命令。
 *   参数：completion —— 有命令完成时回填（一次调用返回一条）。
 *   返回：NAND_RESULT_OK（有完成，completion 有效）/
 *         INVALID_ARGUMENT（req 构建失败，该 LUN 队列已丢弃）/
 *         NOT_READY（无命令可推进或完成，调用方应轮询重试）。
 *   实现要点：
 *     - 外层 round-robin 遍历通道（从上次返回完成的通道的下一个续走）。
 *     - 每 LUN 交给 lun_step 单步推进；完成一条即返回（每槽 = 一条命令）。
 *     - req 构建失败是编程错误：丢弃该 LUN 队列避免死循环，返回 INVALID_ARGUMENT。
 * -------------------------------------------------------------------- */
nand_result_t nand_dispatch_poll(nand_dispatch_completion_t *completion)
{
    uint8_t count;

    /* 出参前置条件由调用者保证（fc.c 轮询路径恒传有效 completion） */
    ASSERT(completion != NULL);

    for (count = 0U; count < CH_NUM; count++)
    {
        /* round-robin：从上次返回完成的通道的下一个续走，各通道公平 */
        uint8_t channel = (uint8_t)((g_nand_channel_cursor + count) % CH_NUM);
        uint8_t ce;
        uint8_t lun_idx;

        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun_idx = 0U; lun_idx < LUN_NUM; lun_idx++)
            {
                nand_dispatch_lun_queue_t  *queue = &g_nand_dispatch_queues[channel][ce][lun_idx];
                nand_dispatch_lun_ctx_t     ctx;
                nand_dispatch_lun_outcome_t out;

                if (queue->count == 0U)
                {
                    continue; /* 该 LUN 无在途任务，跳过 */
                }

                ctx.completion      = completion;
                ctx.queue           = queue;
                ctx.channel         = channel;
                ctx.ce              = ce;
                ctx.lun             = lun_idx;
                ctx.nand_completion = (nand_fc_completion_t){0};
                ctx.result          = NAND_RESULT_OK;

                out = nand_dispatch_lun_step(&ctx);
                if (out == NAND_DISPATCH_LUN_ERROR)
                {
                    /* req 构建失败（编程错误）：丢弃该 LUN 队列，避免下一轮原地重试死循环 */
                    queue->count = 0U;
                    queue->phase = LUN_PHASE_IDLE;
                    return NAND_RESULT_INVALID_ARGUMENT;
                }
                if (out == NAND_DISPATCH_LUN_DONE)
                {
                    return ctx.result;
                }
            }
        }
    }
    return NAND_RESULT_NOT_READY;
}
