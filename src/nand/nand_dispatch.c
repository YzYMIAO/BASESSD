/*
 * nand_dispatch.c — NAND 命令分发器
 *
 * 职责：接收 FC 侧预解析的多命令请求（nand_dispatch_request_t），
 *       送入通道 FIFO，由 poll 驱动 NAND 引擎执行：
 *         FC 预解析 → submit/submit_chain 入队 → poll 出队 + 执行 + 回 completion
 *
 * 层次关系：
 *   上层 FC（fc_command.h）→ 本文件（dispatch）
 *     → nand_fc_start/step（nand_mt29*.c）→ nand_engine（UOP 解释器）→ FMC 后端
 *
 * 执行模型：
 *   - 单线程 poll 推进模型，无锁；依赖外部串行调用 poll。
 *   - submit/submit_chain 仅"登记"命令（入队 + 预加载 task），
 *     实际执行发生在后续 poll。
 *   - 每 LUN 一个 chain buffer（Phase V），支持链式命令 + ISSUE/WAIT/FETCH 相位机。
 *
 * 对外接口：nand_dispatch_init / submit / submit_chain / poll / set_device_ready。
 */

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
 * 通道队列及全局状态
 * ====================================================================== */

/* 通道 FIFO 元素：仅供 poll Phase 2 出队路由到具体 chain。 */
typedef struct
{
    uint16_t command_id; /* FC 完成池关联键 */
    uint8_t  ce;         /* 出队路由键：CE */
    uint8_t  lun;        /* 出队路由键：LUN */
} nand_dispatch_queue_entry_t;

/* 每通道一个环形 FIFO（深度 NAND_CHANNEL_QUEUE_DEPTH）。 */
typedef struct
{
    nand_dispatch_queue_entry_t entries[NAND_CHANNEL_QUEUE_DEPTH];
    uint16_t                    head;  /* 出队游标 */
    uint16_t                    tail;  /* 入队游标 */
    uint16_t                    count; /* 当前元素数（满时 = DEPTH） */
} nand_channel_queue_t;

/* 通道 FIFO 数组（CH_NUM 个），submit 入队、poll Phase 2 出队 */
static nand_channel_queue_t g_nand_channel_queues[CH_NUM];
/* round-robin 游标：poll 从上次返回通道的下一个通道续走，保证各通道公平 */
static uint8_t g_nand_channel_cursor;
/* 设备就绪门控：仅 submit 准入检查，poll 不检查 */
static uint8_t g_nand_device_ready;

/* 每 LUN 链状态：原 g_nand_dispatch_tasks[ch][ce][lun] 的一维扩展。
 * 单命令：count=1，tasks[0] 预加载。
 * 链式命令：count=N，tasks[0..N-1] 预加载，poll 从 cursor=0 推进到 N-1。
 * count 双语义：既是链深又是"LUN 忙"标志（count!=0 即忙）。 */
static nand_dispatch_lun_chain_t g_nand_dispatch_chains[CH_NUM][CE_NUM][LUN_NUM];
/* 通道页缓冲：置于 noncacheable_buffer 段（DMA/FMC 直访，D-Cache 关闭）。
 * 多 plane 时拼接双页（plane stride = PAGE_SIZE），poll 三处 memcpy 拆页依赖它。 */
uint8_t g_nand_channel_page_buffer[CH_NUM][PAGE_BUFFER_SIZE] __attribute__((section(".noncacheable_buffer")));

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
 *   功能：把 FC 预解析的 request 搬运到 chain 的 task slot。
 *   参数：task —— 目标 slot；request —— 预解析请求。
 *   返回：无。
 *   实现要点：
 *     - 复位 cw_index=0 / started=0（poll 的启动判断依赖 started==0）。
 *     - Phase V：绑定 per-LUN backend，并一次性绑定引擎上下文三指针——
 *       backend/request/completion 随 transaction 固定（指向 transaction 自己的
 *       存储，地址不变），nand_engine_start 只收 uops/timing。
 *     - 双层数组 cw_paa/cw_buffers[plane][cw] 按 request->cws 逐 CW 搬运；
 *       plane 下标写死 0/1（双 plane 上限）。
 * -------------------------------------------------------------------- */
static void nand_dispatch_task_load(nand_dispatch_task_t *task, const nand_dispatch_request_t *request)
{
    uint8_t cw;

    nand_dispatch_completion_init(&task->completion, request->command_id);
    task->command_id   = request->command_id;
    task->ch           = request->ch;
    task->ce           = request->ce;
    task->lun          = request->lun;
    task->operation    = request->operation;
    task->page_type    = request->page_type;
    task->cw_index     = 0U;
    task->cw_count     = request->cw_count;
    task->plane_count  = request->plane_count;
    task->started      = 0U;
    task->parameter[0] = request->parameter[0];
    task->parameter[1] = request->parameter[1];
    task->parameter[2] = request->parameter[2];
    task->parameter[3] = request->parameter[3];
    task->feature_addr = request->feature_addr;
    /* Phase V: 绑定 per-LUN backend + 引擎上下文三指针（地址随事务固定，只在此绑一次） */
    task->backend                       = nand_fmc_backend_get_ext(request->ch, request->ce, request->lun);
    task->transaction.engine.backend    = task->backend;
    task->transaction.engine.request    = &task->transaction.request;
    task->transaction.engine.completion = &task->transaction.completion;
    for (cw = 0U; cw < request->cw_count; cw++)
    {
        task->cw_paa[0][cw]     = request->cws[0][cw].paa;
        task->cw_buffers[0][cw] = request->cws[0][cw].buffer;
        task->cw_paa[1][cw]     = request->cws[1][cw].paa;
        task->cw_buffers[1][cw] = request->cws[1][cw].buffer;
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
    memset(g_nand_channel_queues, 0, sizeof(g_nand_channel_queues));
    memset(g_nand_dispatch_chains, 0, sizeof(g_nand_dispatch_chains));
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

/* ======================================================================
 * submit — 单命令入队
 * ====================================================================== */

/* --------------------------------------------------------------------
 * nand_dispatch_submit
 *   功能：单命令入队（非阻塞登记）。
 *   参数：request —— 预解析的单个命令请求。
 *   返回：NAND_RESULT_OK / INVALID_ARGUMENT / NOT_READY（LUN 忙或队列满）。
 *   实现要点：
 *   - LUN 忙判定 = chain->count!=0（count 兼作忙标志）。
 *   - 预加载到 tasks[0]，count=1/cursor=0/phase=IDLE。
 *   - 入队 command_id+ce/lun，环形尾指针 %DEPTH 推进。
 *   - 本函数只是"登记"，实际执行在后续 poll。
 * -------------------------------------------------------------------- */
nand_result_t nand_dispatch_submit(const nand_dispatch_request_t *request)
{
    nand_channel_queue_t      *queue;
    nand_dispatch_lun_chain_t *chain;

    if ((g_nand_device_ready == 0U) || (request == NULL) || (request->ch >= CH_NUM) ||
        (request->ce >= CE_NUM) || (request->lun >= LUN_NUM) ||
        (request->cw_count > CW_NUM))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    chain = &g_nand_dispatch_chains[request->ch][request->ce][request->lun];
    if (chain->count != 0U)
    {
        return NAND_RESULT_NOT_READY; /* LUN 忙（count 兼作忙标志） */
    }

    queue = &g_nand_channel_queues[request->ch];
    if (queue->count == NAND_CHANNEL_QUEUE_DEPTH)
    {
        return NAND_RESULT_NOT_READY; /* 通道 FIFO 满 */
    }

    /* 预加载到 tasks[0] */
    nand_dispatch_task_load(&chain->tasks[0U], request);
    chain->count  = 1U;
    chain->cursor = 0U;
    chain->phase  = LUN_PHASE_IDLE;

    /* 入队 command_id + ce/lun */
    queue->entries[queue->tail] = (nand_dispatch_queue_entry_t){
        .command_id = (uint16_t)request->command_id,
        .ce         = request->ce,
        .lun        = request->lun,
    };
    queue->tail = (uint16_t)((queue->tail + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
    queue->count++;
    return NAND_RESULT_OK;
}

/* ======================================================================
 * submit_chain — 链式命令原子入队
 *
 * 数据完整性保证：
 *   1. 队列剩余空间不足整链时返回 NOT_READY，无副作用
 *   2. LUN chain buffer 非空闲时返回 NOT_READY，无副作用
 *   3. 入队后链中命令按序执行，HSQ 无法插入
 * ====================================================================== */

/* --------------------------------------------------------------------
 * nand_dispatch_submit_chain
 *   功能：整链原子入队（多命令按序执行）。
 *   参数：requests —— 命令数组；count —— 链长（1..NAND_DISPATCH_CHAIN_DEPTH）。
 *   返回：NAND_RESULT_OK / INVALID_ARGUMENT / NOT_READY（忙或队列不足）。
 *   实现要点：
 *     - 只校验 requests[0] 的 ch/ce/lun——假定整链同 ch/ce/lun（调用方保证）。
 *     - 空间原子检查用 (uint16_t)(count+queue->count) 无符号饱和比较；
 *       count≤4、queue->count≤64，无溢出风险。
 *     - 先全部预加载 task + 入队，最后才挂链（count/cursor/phase）；
 *       保证 poll 不会看到半挂载的链（"准原子"）。
 * -------------------------------------------------------------------- */
nand_result_t nand_dispatch_submit_chain(const nand_dispatch_request_t *requests, uint8_t count)
{
    nand_channel_queue_t      *queue;
    nand_dispatch_lun_chain_t *chain;
    uint8_t                    i;

    if ((requests == NULL) || (count == 0U) || (count > NAND_DISPATCH_CHAIN_DEPTH))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    if ((requests[0U].ch >= CH_NUM) || (requests[0U].ce >= CE_NUM) || (requests[0U].lun >= LUN_NUM))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    chain = &g_nand_dispatch_chains[requests[0U].ch][requests[0U].ce][requests[0U].lun];
    if (chain->count != 0U)
    {
        return NAND_RESULT_NOT_READY; /* LUN 链正忙 */
    }

    queue = &g_nand_channel_queues[requests[0U].ch];

    /* 原子性检查：空间必须容纳整链 */
    if ((uint16_t)(queue->count + count) > NAND_CHANNEL_QUEUE_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }

    /* 预加载整链到 tasks[0..count-1] */
    for (i = 0U; i < count; i++)
    {
        nand_dispatch_task_load(&chain->tasks[i], &requests[i]);
        queue->entries[queue->tail] = (nand_dispatch_queue_entry_t){
            .command_id = requests[i].command_id,
            .ce         = requests[i].ce,
            .lun        = requests[i].lun,
        };
        queue->tail = (uint16_t)((queue->tail + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
        queue->count++;
    }

    /* 全部入队后才挂链：poll 不会看到半挂载链 */
    chain->count  = count;
    chain->cursor = 0U;
    chain->phase  = LUN_PHASE_IDLE;
    return NAND_RESULT_OK;
}

/* ======================================================================
 * poll — 推进各 LUN 活跃事务 + 从通道队列出队新命令
 *
 * 两阶段设计（对应企业主控 Flash 后端的两个模块）：
 *   Phase 1 — Per-die FSM：遍历各 Channel/CE/LUN 的 chain buffer，
 *             按相位机推进活跃事务。当前命令完成后自动推进 cursor，
 *             不清 LUN busy；整链结束（cursor+1==count）才返回 completion。
 *   Phase 2 — Arbiter：从通道队列出队 command_id，启动 LUN 的第一个命令。
 *
 * 相位机（Phase V）：IDLE → ISSUE → ISSUED → FETCH → IDLE
 *   ISSUE：发完命令即返回，LUN 进入 tR/tPROG/tBERS
 *   ISSUED：DWT 计时最短忙期，到点后启动配对 WAIT/FETCH
 *   FETCH：执行 WAIT_READY + 数据 + 状态检查
 *
 * 每个相位的写点/读点总表见 nand_dispatch_internal.h 的 nand_dispatch_lun_chain_t
 * 注释；下文函数体内的 `── 相位 X → Y ──` 标记指出该分支属于哪次转场。
 * 完整状态机图见 docs/design/fc_nand_design.md §23（图 20）。
 *
 * 本段函数按企业后端分层组织：
 *   Command issue  —— task_build_request / task_fc_start / task_start
 *   Arbiter        —— queue_consume_match / phase2_dequeue
 *   Per-die FSM    —— issued_advance / lun_drive / chain_advance / phase1_lun
 *   Data path      —— task_cw_chain / scatter_page_buffer
 * ====================================================================== */

/* ── Per-die FSM 单步推进结果 ── */
typedef enum
{
    NAND_DISPATCH_LUN_WAIT = 0, /* 本轮无进展，跳到下一 LUN（对应原 6 处 break/continue） */
    NAND_DISPATCH_LUN_CONTINUE, /* 链已推进到下一命令，继续内层 while */
    NAND_DISPATCH_LUN_DONE,     /* 拿到可返回的 completion */
    NAND_DISPATCH_LUN_ERROR     /* req 构建失败，调用方须 abort poll */
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

/* ── 单 LUN 上下文（仅 Phase 1 用；Phase 2 是直线代码，用显式参数）。
 *    不缓存 task 指针——chain->cursor++ 后旧指针失效，一律由
 *    &chain->tasks[chain->cursor] 现取。 ── */
typedef struct
{
    nand_dispatch_completion_t *completion; /* 链尾输出目标 */
    nand_channel_queue_t       *queue;
    nand_dispatch_lun_chain_t  *chain;
    nand_fc_completion_t        nand_completion; /* 读前必写（active==0 分支依赖同轮先写） */
    uint8_t                     channel;
    uint8_t                     ce;
    uint8_t                     lun;
    uint8_t                     result;
} nand_dispatch_lun_ctx_t;

/* --------------------------------------------------------------------
 * nand_dispatch_task_build_request
 *   功能：调 req_table 把命令参数构建成 nand_fc_request_t。
 *   参数：task —— 目标任务。
 *   返回：req handler 的返回码。
 *   实现要点：单独成函数是为了保住 Phase 2 的日志点位置——
 *         LOGD 必须夹在"构建 request"与"nand_fc_start"之间。
 * -------------------------------------------------------------------- */
static uint8_t nand_dispatch_task_build_request(nand_dispatch_task_t *task)
{
    return g_nand_dispatch_req_table[task->operation](task);
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_fc_start
 *   功能：发起异步 ONFI 事务并置初始相位。
 *   参数：chain —— 所属 LUN 链；task —— 目标任务；fc_result —— 输出 nand_fc_start 返回码。
 *   返回：NAND_DISPATCH_START_ASYNC / NAND_DISPATCH_START_SYNC。
 *   实现要点：
 *     - 相位按操作类型判定：ISSUE 型置 LUN_PHASE_ISSUE，其余（WAIT/FETCH/原子）置 FETCH。
 *       ISSUED 块的 pair_op 目标必为 WAIT/FETCH 型，故此处统一判定与"固定置 FETCH"等价。
 *     - 相位在 NOT_READY 判定之前提交——下一轮路由依赖它。
 * -------------------------------------------------------------------- */
static nand_dispatch_start_outcome_t nand_dispatch_task_fc_start(nand_dispatch_lun_chain_t *chain,
                                                                 nand_dispatch_task_t      *task,
                                                                 uint8_t                   *fc_result)
{
    uint8_t result;

    result        = nand_fc_start(&task->transaction, task->operation, &task->request);
    task->started = 1U;
    if (nand_operation_is_issue(task->operation) != 0U)
    {
        chain->phase = LUN_PHASE_ISSUE;
    }
    else
    {
        chain->phase = LUN_PHASE_FETCH;
    }
    *fc_result = result;
    return (result == NAND_RESULT_NOT_READY) ? NAND_DISPATCH_START_ASYNC : NAND_DISPATCH_START_SYNC;
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_start
 *   功能：构建 request + 发起事务（Phase 1 三个无日志调用点的统一入口）。
 *   参数：chain —— 所属 LUN 链；task —— 目标任务；fc_result —— 输出 nand_fc_start 返回码。
 *   返回：REQ_ERROR（调用方须 abort）/ ASYNC / SYNC。
 * -------------------------------------------------------------------- */
static nand_dispatch_start_outcome_t nand_dispatch_task_start(nand_dispatch_lun_chain_t *chain,
                                                              nand_dispatch_task_t      *task,
                                                              uint8_t                   *fc_result)
{
    if (nand_dispatch_task_build_request(task) != NAND_RESULT_OK)
    {
        return NAND_DISPATCH_START_REQ_ERROR;
    }
    return nand_dispatch_task_fc_start(chain, task, fc_result);
}

/* --------------------------------------------------------------------
 * nand_dispatch_queue_consume_match
 *   功能：同步完成时消费通道队列的 head entry。
 *   参数：queue —— 通道 FIFO；ce/lun —— 当前 LUN。
 *   返回：无。
 *   实现要点：环形 FIFO 先进先出，head entry 的 ce/lun 与当前 LUN 相符
 *         才出队——靠轮转命中隐式配对，无显式标记。
 * -------------------------------------------------------------------- */
static void nand_dispatch_queue_consume_match(nand_channel_queue_t *queue, uint8_t ce, uint8_t lun)
{
    if ((queue->count != 0U) && (queue->entries[queue->head].ce == ce) && (queue->entries[queue->head].lun == lun))
    {
        queue->head = (uint16_t)((queue->head + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
        queue->count--;
    }
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
    nand_dispatch_task_t *task   = &ctx->chain->tasks[ctx->chain->cursor];
    uint16_t              min_us = g_nand_operation_map[task->operation].min_busy_us;
    uint8_t               pair_op;
    uint8_t               result;

    if ((min_us != 0U) && !delay_dwt_elapsed_us(ctx->chain->wait_start_cycles, min_us))
    {
        return NAND_DISPATCH_LUN_WAIT; /* 最短等待未到，下一轮再检查 */
    }

    pair_op = nand_operation_get_pair(task->operation);
    if ((pair_op != 0U) && (pair_op != task->operation))
    {
        task->operation = pair_op;
    }

    switch (nand_dispatch_task_start(ctx->chain, task, &result))
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
 *   功能：Per-die FSM 单步——启动 pre-loaded 命令或推进已启动事务，
 *         并处理 ISSUE 段完成 → ISSUED 的相位切换。
 *   参数：ctx —— 单 LUN 上下文（result / nand_completion 就地更新）。
 *   返回：WAIT（本轮无进展）/ DONE（已产出 result）/ ERROR（req 构建失败）。
 *   实现要点：
 *     - started==0：submit 已预加载但未启动，走 task_start；同步完成才消费
 *       队列 entry（异步时 entry 留待下轮）。
 *     - started!=0：transaction.engine.active 为 0 说明命令已在 start() 中同步
 *       完成（如 FETCH WAIT_READY 已 ready），直接取已存的 execution_status，
 *       不再 step（nand_fc_step 对 active==0 会返回 INVALID_ARGUMENT）。
 *     - ISSUE 段完成 → task_complete + 转 ISSUED + 记 DWT 起点，不清 LUN busy。
 * -------------------------------------------------------------------- */
static nand_dispatch_lun_outcome_t nand_dispatch_lun_drive(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t *task = &ctx->chain->tasks[ctx->chain->cursor];
    uint8_t               result;

    if (task->started == 0U)
    {
        switch (nand_dispatch_task_start(ctx->chain, task, &result))
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
        nand_dispatch_queue_consume_match(ctx->queue, ctx->ce, ctx->lun);
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
    if (ctx->chain->phase == LUN_PHASE_ISSUE)
    {
        nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        ctx->chain->phase = LUN_PHASE_ISSUED;
        /* 记录 tR/tPROG/tBERS 起点，供 ISSUED 块最短等待用 */
        delay_dwt_cycles_now(&ctx->chain->wait_start_cycles);
        return NAND_DISPATCH_LUN_WAIT; /* 不清 LUN busy，下一轮由 ISSUED 块接管 */
    }
    return NAND_DISPATCH_LUN_DONE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_cw_chain
 *   功能：CW 级 chaining——多 CW READ 的 RANDOM_DATA_READ 链式续读。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：CW_ASYNC（下一 CW 异步启动，本轮到此为止）/ CW_DONE。
 *   实现要点：
 *     - 每 CW 先 task_complete，再判断是否可续读。
 *     - chaining 条件：整页读入页缓冲型操作 + 还有下一 CW
 *       （仅普通读/OTP 读/随机读这几类可按 CW 链式续读）。
 *     - 链中后续 CW 统一改用 OP_RANDOM_DATA_READ，列地址 = cw_index × 528B
 *       （PAGE_SIZE/4，页缓冲按 CW 等分，4=CW_NUM）。
 *     - 链内 start 的错误码仍走完成路径，只有 NOT_READY 才中止本轮。
 * -------------------------------------------------------------------- */
static nand_dispatch_cw_outcome_t nand_dispatch_task_cw_chain(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t *task   = &ctx->chain->tasks[ctx->chain->cursor];
    uint8_t               result = ctx->result;

    for (;;)
    {
        nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        if ((result != NAND_RESULT_OK) ||
            ((task->operation != OP_NORMAL_READ_PAGE_WITH_DATA_OUT) &&
             (task->operation != OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) &&
             (task->operation != OP_OTP_DATA_READ) &&
             (task->operation != OP_RANDOM_DATA_READ)) ||
            (task->cw_index + 1U >= task->cw_count))
        {
            break;
        }
        task->cw_index++;
        task->operation = OP_RANDOM_DATA_READ; /* 链中后续 CW 统一改用随机读 */
        task->started   = 1U;                  /* 复用 start 路径但标记已启动 */
        {
            nand_paa_t cw_paa    = task->cw_paa[0][task->cw_index];
            uint8_t   *cw_buffer = task->cw_buffers[0][task->cw_index];
            /* 列地址 = CW 偏移 × 528B（PAGE_SIZE/4，页缓冲按 CW 等分，4=CW_NUM） */
            uint16_t cw_column = (uint16_t)(task->cw_index * (PAGE_SIZE / 4U));
            task->request      = (nand_fc_request_t){.column = cw_column, .row = {paa_to_nand_row(cw_paa)}, .data = cw_buffer, .data_length = (uint16_t)(PAGE_SIZE / 4U)};
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
 *   功能：把通道页缓冲的整页数据拆回各 CW buffer。
 *   参数：task —— 已完成任务；channel —— 通道号（定位页缓冲）。
 *   返回：无。
 *   实现要点：三种操作的数据落在通道页缓冲而非各 CW buffer（FMC 数据路径约定）：
 *     - Cache 读（SEQ/RANDOM/LAST）与 OP_READ_PAGE_FETCH：单 plane，
 *       offset = cw × (PAGE_SIZE/CW_NUM)。
 *     - Multi-Plane WITH_DATA_OUT：双 plane 数据按 plane 顺序拼接，
 *       offset = plane × PAGE_SIZE + cw × (PAGE_SIZE/CW_NUM)。
 * -------------------------------------------------------------------- */
static void nand_dispatch_scatter_page_buffer(const nand_dispatch_task_t *task, uint8_t channel)
{
    uint8_t cw;
    uint8_t plane;

    if ((task->operation == OP_READ_PAGE_CACHE_SEQ) || (task->operation == OP_READ_PAGE_CACHE_RANDOM) ||
        (task->operation == OP_READ_PAGE_CACHE_LAST) || (task->operation == OP_READ_PAGE_FETCH))
    {
        for (cw = 0U; cw < task->cw_count; cw++)
        {
            memcpy(task->cw_buffers[0][cw],
                   &g_nand_channel_page_buffer[channel][cw * (PAGE_SIZE / 4U)],
                   PAGE_SIZE / 4U);
        }
    }

    if (task->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT)
    {
        for (plane = 0U; plane < task->plane_count; plane++)
        {
            for (cw = 0U; cw < task->cw_count; cw++)
            {
                uint8_t *src = &g_nand_channel_page_buffer[channel][plane * PAGE_SIZE + cw * (PAGE_SIZE / CW_NUM)];
                memcpy(task->cw_buffers[plane][cw], src, PAGE_SIZE / CW_NUM);
            }
        }
    }
}

/* --------------------------------------------------------------------
 * nand_dispatch_chain_advance
 *   功能：链推进——cursor++ 并启动下一命令（链尾收尾不在这里）。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：CONTINUE（已启动下一命令，调用方回到驱动循环顶部）/
 *         WAIT（启动异步）/ ERROR（req 构建失败）。
 *   实现要点：CONTINUE 与 WAIT 必须区分——前者继续内层 while，后者离开。
 * -------------------------------------------------------------------- */
static nand_dispatch_lun_outcome_t nand_dispatch_chain_advance(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t *task;
    uint8_t               result;

    ctx->chain->cursor++;
    task = &ctx->chain->tasks[ctx->chain->cursor];

    switch (nand_dispatch_task_start(ctx->chain, task, &result))
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
    return NAND_DISPATCH_LUN_CONTINUE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_phase1_lun
 *   功能：Per-die FSM 主循环——推进单个 LUN 的活跃 chain 直到本轮无进展。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：WAIT（本轮无进展，调用方看下一个 LUN）/ DONE（整链结束，*completion
 *         已回填）/ ERROR（req 构建失败，chain 已清）。
 *   实现要点：
 *     - ISSUED 块只在进入驱动循环前判一次（与原实现一致）。
 *     - 返回 WAIT 的出口共 6 处，返回前必须已提交全部持久状态：
 *       ISSUED 忙期未到 / 配对启动异步 / started==0 首次启动异步（entry 不消费）/
 *       step 异步 / ISSUE 段完成转 ISSUED / 链推进后异步。
 *     - 整链结束才清 count 并重置 phase，同时推进通道游标。
 * -------------------------------------------------------------------- */
static nand_dispatch_lun_outcome_t nand_dispatch_phase1_lun(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_lun_outcome_t out;
    nand_dispatch_task_t       *task;

    /* ── 相位 ISSUED 预检（只判一次）──
     * ISSUED 由 lun_drive / phase2_dequeue 的 ISSUE→ISSUED 转场置入。
     * 只在进入驱动循环前判一次（与原实现一致）：本轮先进 issued_advance 等最短忙期。 */
    if (ctx->chain->phase == LUN_PHASE_ISSUED)
    {
        out = nand_dispatch_issued_advance(ctx);
        if (out == NAND_DISPATCH_LUN_ERROR)
        {
            ctx->chain->count = 0U;
            return out;
        }
        if (out == NAND_DISPATCH_LUN_WAIT)
        {
            return out;
        }
    }

    /* ── 驱动循环（FETCH / 链推进）──
     * 逐个 task 推进，直到本轮无进展（WAIT）或整链结束（DONE）。
     * 每个出口返回前必须已提交全部持久状态，见上方实现要点。 */
    while (1)
    {
        task = &ctx->chain->tasks[ctx->chain->cursor];
        out  = nand_dispatch_lun_drive(ctx);
        if (out == NAND_DISPATCH_LUN_ERROR)
        {
            ctx->chain->count = 0U;
            return out;
        }
        if (out == NAND_DISPATCH_LUN_WAIT)
        {
            break;
        }

        if (nand_dispatch_task_cw_chain(ctx) == NAND_DISPATCH_CW_ASYNC)
        {
            break;
        }

        nand_dispatch_scatter_page_buffer(task, ctx->channel);

        if ((uint8_t)(ctx->chain->cursor + 1U) < ctx->chain->count)
        {
            out = nand_dispatch_chain_advance(ctx);
            if (out == NAND_DISPATCH_LUN_ERROR)
            {
                ctx->chain->count = 0U;
                return out;
            }
            if (out == NAND_DISPATCH_LUN_WAIT)
            {
                break;
            }
            continue; /* 链未结束，回到驱动循环顶部 */
        }

        /* ── 相位 FETCH 尾段结束 → 整链收尾，phase 复位 IDLE，通道游标前移 ── */
        *ctx->completion      = task->completion;
        ctx->chain->count     = 0U;
        ctx->chain->phase     = LUN_PHASE_IDLE;
        g_nand_channel_cursor = (uint8_t)((ctx->channel + 1U) % CH_NUM);
        return NAND_DISPATCH_LUN_DONE;
    }
    return NAND_DISPATCH_LUN_WAIT;
}

/* --------------------------------------------------------------------
 * nand_dispatch_phase2_dequeue
 *   功能：Arbiter——从通道 FIFO 出队一条命令并在目标 LUN 上启动。
 *   参数：channel —— 通道号；queue —— 通道 FIFO；completion —— 完成输出；
 *         result_out —— DONE 时带回执行结果。
 *   返回：WAIT（本轮不产出 completion，调用方看下一通道）/ DONE（单命令立即
 *         完成，*completion 有效）/ ERROR（req 构建失败）。
 *   实现要点：
 *     - 目标 LUN 仍忙时 entry 留队，等下一轮。
 *     - 日志点必须夹在"构建 request"与"nand_fc_start"之间。
 *     - 同步完成且 phase==ISSUE → 转 ISSUED，不返回 completion；
 *       链（count>1）交给 Phase 1；只有单命令立即完成才 DONE。
 *     - DONE 收尾**不重置 phase、不推进通道游标**（与 Phase 1 链尾不对称，
 *       这是既有行为）。
 *     - 异步启动（START_ASYNC）时 entry 已消费、命令却还没完成：此时命令不在
 *       任何队列里，仅由 chain->count>0 + tasks[0].started 表示，靠 Phase 1 下
 *       一轮接管。详见 nand_dispatch_internal.h 中 nand_dispatch_lun_chain_t
 *       的 count 双语义说明——"队列空 + count>0"是正常中间态，不是命令丢失。
 * -------------------------------------------------------------------- */
static nand_dispatch_lun_outcome_t nand_dispatch_phase2_dequeue(uint8_t                     channel,
                                                                nand_channel_queue_t       *queue,
                                                                nand_dispatch_completion_t *completion,
                                                                uint8_t                    *result_out)
{
    nand_dispatch_queue_entry_t entry = queue->entries[queue->head];
    nand_dispatch_lun_chain_t  *chain = &g_nand_dispatch_chains[channel][entry.ce][entry.lun];
    nand_dispatch_task_t       *task;
    nand_fc_completion_t        nand_completion;
    uint8_t                     result;

    if (chain->count != 0U)
    {
        return NAND_DISPATCH_LUN_WAIT; /* 目标 LUN 仍忙，entry 留队等下一轮 */
    }

    queue->head = (uint16_t)((queue->head + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
    queue->count--;

    task = &chain->tasks[0U]; /* task 已由 submit/submit_chain 预加载 */

    result = nand_dispatch_task_build_request(task);
    if (result != NAND_RESULT_OK)
    {
        chain->count = 0U;
        return NAND_DISPATCH_LUN_ERROR;
    }

    LOGD("nand: FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n",
         task->operation,
         (void *)task->request.data,
         task->request.data_length,
         task->request.column,
         (unsigned long)task->request.row[0]);

    switch (nand_dispatch_task_fc_start(chain, task, &result))
    {
        case NAND_DISPATCH_START_ASYNC:
            return NAND_DISPATCH_LUN_WAIT; /* entry 已出队，命令留在 task，由 Phase 1 下轮接管 */
        default:
            break; /* START_SYNC；REQ_ERROR 不会出现（上面已构建成功） */
    }

    nand_completion.execution_status = result;
    nand_completion.nand_status      = task->transaction.completion.nand_status;
    nand_dispatch_task_complete(task, result, &nand_completion);

    /* ── 相位 ISSUE → ISSUED（同步完成路径）──
     * Phase V: ISSUE 段同步完成 → 切 ISSUED，不返回 completion。
     * 相位由上方 task_fc_start 置入（ISSUE 型 OP → ISSUE）；task_complete 已在上
     * 面无条件调用、task->completion 已填好，但 LUN 还在 tR/tPROG/tBERS，这里必须
     * 拦下，否则会把"只发完前半段"的结果当命令完成报给上层。 */
    if (chain->phase == LUN_PHASE_ISSUE)
    {
        chain->phase = LUN_PHASE_ISSUED;
        delay_dwt_cycles_now(&chain->wait_start_cycles);
        return NAND_DISPATCH_LUN_WAIT;
    }

    /* 单命令立即完成（count=1 表示单命令；count>1 为链，由 Phase 1 推进） */
    if (chain->count > 1U)
    {
        return NAND_DISPATCH_LUN_WAIT;
    }

    *completion  = task->completion;
    chain->count = 0U;
    *result_out  = result;
    return NAND_DISPATCH_LUN_DONE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_poll
 *   功能：推进各通道各 LUN 的活跃事务，并出队启动新命令。
 *   参数：completion —— 有命令完成时回填（整链一次调用返回）。
 *   返回：NAND_RESULT_OK（有完成，completion 有效）/
 *         NOT_READY（无命令可推进或完成，调用方应轮询重试）。
 *   实现要点：
 *     - 外层 round-robin 遍历通道（从上次游标处续走）。
 *     - Phase 1 交给 phase1_lun（Per-die FSM），Phase 2 交给 phase2_dequeue（Arbiter）。
 *     - completion 有两条返回路径：Phase 1 整链结束（含单命令）、
 *       Phase 2 单命令立即完成。
 * -------------------------------------------------------------------- */
nand_result_t nand_dispatch_poll(nand_dispatch_completion_t *completion)
{
    uint8_t count;

    if (completion == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    for (count = 0U; count < CH_NUM; count++)
    {
        /* round-robin：从上次返回通道的下一个续走，各通道公平 */
        uint8_t               channel = (uint8_t)((g_nand_channel_cursor + count) % CH_NUM);
        nand_channel_queue_t *queue   = &g_nand_channel_queues[channel];
        uint8_t               ce;
        uint8_t               lun_idx;

        /* ── Phase 1: Per-die FSM —— 推进各 LUN 活跃事务 ── */
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun_idx = 0U; lun_idx < LUN_NUM; lun_idx++)
            {
                nand_dispatch_lun_chain_t  *chain = &g_nand_dispatch_chains[channel][ce][lun_idx];
                nand_dispatch_lun_ctx_t     ctx;
                nand_dispatch_lun_outcome_t out;

                if (chain->count == 0U)
                {
                    continue; /* 该 LUN 无在途任务，跳过 */
                }

                ctx.completion      = completion;
                ctx.queue           = queue;
                ctx.chain           = chain;
                ctx.channel         = channel;
                ctx.ce              = ce;
                ctx.lun             = lun_idx;
                ctx.nand_completion = (nand_fc_completion_t){0};
                ctx.result          = NAND_RESULT_OK;

                out = nand_dispatch_phase1_lun(&ctx);
                if (out == NAND_DISPATCH_LUN_ERROR)
                {
                    return NAND_RESULT_INVALID_ARGUMENT;
                }
                if (out == NAND_DISPATCH_LUN_DONE)
                {
                    return ctx.result;
                }
            }
        }

        /* ── Phase 2: Arbiter —— 从通道队列出队新命令 ── */
        if (queue->count != 0U)
        {
            uint8_t                     result;
            nand_dispatch_lun_outcome_t out = nand_dispatch_phase2_dequeue(channel, queue, completion, &result);

            if (out == NAND_DISPATCH_LUN_ERROR)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            if (out == NAND_DISPATCH_LUN_DONE)
            {
                return result;
            }
            /* WAIT → 目标 LUN 忙 / 启动异步 / 转 ISSUED / 链中还有后续，继续下一通道 */
        }
    }
    return NAND_RESULT_NOT_READY;
}