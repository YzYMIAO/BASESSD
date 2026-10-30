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
 * 对外接口：nand_dispatch_init / submit / submit_chain / poll / set_armed /
 *           is_armed / lun_free。
 */

#include "assertion.h"
#include "delay.h"
#include "fc_command.h"
#include "log.h"
#include "nand_config.h"
#include "nand_debug.h"
#include "nand_dispatch_internal.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/* ======================================================================
 * 全局状态
 * ====================================================================== */

/* 每 LUN 待处理 FIFO：submit 按 room 追加，poll 逐条排空。
 * 每槽 = 一条命令；链 = 连续槽（由 submit_chain 原子批量追加保证）。 */
static nand_dispatch_lun_queue_t g_nand_dispatch_queues[CH_NUM][CE_NUM][LUN_NUM];
/* 异步路径放行闩锁（boot 期一次性置位，运行期不清）：
 * 0 = FMC 仍归同步初始化路径独占（nand_init 的 boot 命令走 nand_engine_execute）；
 * 1 = 已移交异步 dispatch，submit/submit_chain 才准入。
 * ⚠ 这不是 ONFI 的 "device ready"（R/B# / status bit 6，per-die 且瞬态）——
 *   那由引擎的 WAIT_READY（70h 状态轮询）现查，不在此缓存。
 * 仅在 nand_dispatch_enqueue() 一处检查（submit 与 submit_chain 共用的漏斗），poll 不检查。 */
static BYTE g_nand_dispatch_armed;

/* 页缓冲固定在 AXI_NOCACHE（与基线一致）。
 * 板端实验：放到 DTCM 时 multi-plane program 的 handler 收到了正确的
 * bitmap/data，但 NAND 读回全 FF；回到 AXI_NOCACHE 后 format 正常。
 * DMA=1 时同样要求 noncacheable（DMA1/2 不能访问 DTCM）。
 * 按 (ch, ce, lun) 各持一块——同通道各 LUN 由 poll 轮流驱动，且 request.data
 * 指针会跨多次 poll 存活（如 program ISSUE 卡在 70h 忙轮询），共享一块会被后驱动的
 * LUN 覆盖（详见 nand_dispatch_tables.c 顶部注释）。
 * 多 plane 时拼接双页（plane stride = PAGE_SIZE），scatter 拆页依赖它。 */
#define NAND_PAGE_BUFFER_ATTR __attribute__((section(".noncacheable_buffer")))
BYTE g_nand_channel_page_buffer[CH_NUM][CE_NUM][LUN_NUM][PAGE_BUFFER_SIZE] NAND_PAGE_BUFFER_ATTR;

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
 *       FC_CW_STATUS_UNUSED，避免脏数据（掩码内的格稍后由 completion handler 覆写）。
 * -------------------------------------------------------------------- */
static void nand_dispatch_completion_init(nand_dispatch_completion_t *completion, WORD command_id)
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

/* --------------------------------------------------------------------
 * nand_dispatch_task_complete
 *   功能：统一收尾路径——写 execution_status/nand_status 后按 operation
 *         分派到对应 completion 策略 handler。
 *   参数：task —— 完成任务；result —— 执行结果；nand_completion —— NAND 完成。
 *   返回：无。
 *   实现要点：经 g_nand_dispatch_completion_table[operation] 分派
 *        （completion_per_cw / all_cws / multi_plane，见 nand_dispatch_tables.c）。
 *         注意 poll 中多处先改写 task->operation 再 complete，handler 按
 *         最新 operation 匹配（如 LOAD→WAIT 切换）。
 * -------------------------------------------------------------------- */
static void nand_dispatch_task_complete(nand_dispatch_task_t *task, BYTE result, const nand_fc_completion_t *nand_completion)
{
    task->completion.execution_status = result;
    task->completion.nand_status      = nand_completion->nand_status;
    task->completion.pre_status       = nand_completion->pre_status;

    g_nand_dispatch_completion_table[task->operation](task, result, nand_completion);
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_load
 *   功能：把 FC 预解析的 request 搬运到 LUN FIFO 的 task slot。
 *   参数：task —— 目标 slot；request —— 预解析请求。
 *   返回：无。
 *   实现要点：
 *     - 复位 started=0（poll 的启动判断依赖 started==0）；cw_index 置为掩码最低位。
 *     - Phase V：绑定 per-LUN backend，并一次性绑定引擎上下文三指针——
 *       backend/request/completion 随 transaction 固定（指向 transaction 自己的
 *       存储，地址不变），nand_engine_start 只收 uops/timing。
 *     - cw_buffers 按 **flat 绝对 CW 号 n** 逐格搬运：只搬 cw_bitmap 里的位。
 *       ⚠ 掩码外的格**不清零**（本函数不做整体 memset，task slot 会被 FIFO 复用）——
 *         它们保持上一次占用该 slot 的残留指针。"掩码外无有效 buffer"由**消费点**保证：
 *         所有取 buffer 的循环都按 cw_bitmap 走，空掩码（擦除）的消费点不读 cw_buffers。
 *       plane_paa 无条件照拷 PLANE_NUM 格。plane 字段必须无条件赋值。
 * -------------------------------------------------------------------- */
static void nand_dispatch_task_load(nand_dispatch_task_t *task, const nand_dispatch_request_t *request)
{
    BYTE n;
    BYTE plane;

    nand_dispatch_completion_init(&task->completion, request->command_id);
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
    task->backend                       = nand_fmc_backend_get();
    task->transaction.engine.backend    = task->backend;
    task->transaction.engine.request    = &task->transaction.request;
    task->transaction.engine.completion = &task->transaction.completion;
    /* 当前 CW = 掩码最低位：直接下发的取数 op（OP_RANDOM_DATA_READ）靠它定位自己那一位，
     * 加载型 op 靠它定首次取数的起点。空掩码（退化命令）退化为 0，保证
     * completion_per_cw 的下标不越界。 */
    task->cw_index                      = nand_dispatch_cw_first(request->cw_bitmap);
    /* 逐格搬掩码内的 CW buffer；不在掩码里的格保持零值。 */
    for (n = nand_dispatch_cw_next(request->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = nand_dispatch_cw_next(request->cw_bitmap, (n + 1U)))
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
     * ⚠ 擦除（v28 起）位图为空 ⇒ 上面这个循环一格都不搬，cw_buffers[] 保持上一次占用
     * 该 slot 的残留值。擦除路径不读 cw_buffers[]（req_erase 只用 plane_paa），故安全；
     * 但"空掩码 = 无 buffer 可读"这条契约**依赖消费点真的不读**——新增无数据 op 时注意。 */
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
nand_result_e nand_dispatch_init(void)
{
    memset(g_nand_dispatch_queues, 0, sizeof(g_nand_dispatch_queues));
    /* DTCM/非缓存段都不由启动代码清零；各 builder 虽会先 memset，这里仍显式清零，
     * 避免任何未来路径读到上电残留。 */
    memset(g_nand_channel_page_buffer, 0, sizeof(g_nand_channel_page_buffer));
    g_nand_dispatch_armed = 0U;
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_dispatch_set_armed
 *   功能：置/清异步路径放行闩锁。
 *   参数：armed —— 非 0 放行。
 *   返回：无。
 *   实现要点：归一化到 0/1；仅在 nand_dispatch_enqueue() 一处检查，poll 不检查。
 * -------------------------------------------------------------------- */
void nand_dispatch_set_armed(BYTE armed)
{
    g_nand_dispatch_armed = (armed != 0U) ? 1U : 0U;
}

/* --------------------------------------------------------------------
 * nand_dispatch_is_armed
 *   功能：查询异步路径放行闩锁。
 *   返回：1 = 已放行；0 = 未放行（初始化阶段）。
 *   实现要点：与 set_armed 归一化语义一致，恒返回 0/1；
 *         供 nand_fc_execute 的"仅初始化可调"契约断言使用。
 * -------------------------------------------------------------------- */
BYTE nand_dispatch_is_armed(void)
{
    return g_nand_dispatch_armed;
}

/* --------------------------------------------------------------------
 * nand_dispatch_lun_free
 *   功能：查询目标 LUN FIFO 的剩余槽数。
 *   参数：ch/ce/lun —— 目标。
 *   返回：剩余槽数（0 .. NAND_DISPATCH_CHAIN_DEPTH）。
 *   实现要点：FC 填充 pass 靠它判断能否放下整条链（need ≤ free 才下发）。
 *         ch/ce/lun 由调用者保证在界内（唯一调用者 fc_poll_fill 调用前已断言）。
 * -------------------------------------------------------------------- */
BYTE nand_dispatch_lun_free(BYTE ch, BYTE ce, BYTE lun)
{
    /* 唯一调用者是 fc_poll_fill，且调用前已 ASSERT 过 ch/ce/lun（同源值）。
     * 保留防御会退化成静默活锁（越界 → 恒 0 → 该 LUN 永久挂起），断言停机更好定位。 */
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    return (NAND_DISPATCH_CHAIN_DEPTH - g_nand_dispatch_queues[ch][ce][lun].count);
}

BYTE nand_dispatch_lun_used(BYTE ch, BYTE ce, BYTE lun)
{
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    return (BYTE)g_nand_dispatch_queues[ch][ce][lun].count;
}

BYTE nand_dispatch_lun_phase(BYTE ch, BYTE ce, BYTE lun)
{
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    return g_nand_dispatch_queues[ch][ce][lun].phase;
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
 *   返回：NAND_RESULT_OK / INVALID_ARGUMENT（异步路径未放行）/ NOT_READY（剩余深度不足整批，无副作用）。
 *   实现要点：
 *     - 放行闩锁在此检查：单命令与整链共用本漏斗，避免两入口再分叉。
 *     - 空间原子检查用 (count+queue->count) 无符号比较；
 *       count≤4、queue->count≤4，无溢出风险。
 *     - 逐槽 nand_dispatch_task_load() 到 tasks[(head+count+i)%DEPTH]；
 *       全部预加载完成后才提交 count（poll 不会看到半挂载的批）。
 * -------------------------------------------------------------------- */
static nand_result_e nand_dispatch_enqueue(const nand_dispatch_request_t *requests, BYTE count)
{
    nand_dispatch_lun_queue_t *queue;
    BYTE                       i;

    /* 异步路径放行闩锁：单命令与整链两个入口共用同一道门，结构上不会再分叉。
     * 未放行是运行时状态（非调用者编程错误）→ 保留返回码，不转 ASSERT。 */
    if (g_nand_dispatch_armed == 0U)
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

    queue = &g_nand_dispatch_queues[requests[0U].ch][requests[0U].ce][requests[0U].lun];

    /* 原子性检查：剩余深度必须容纳整批 */
    if ((queue->count + count) > NAND_DISPATCH_CHAIN_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }

    for (i = 0U; i < count; i++)
    {
        BYTE slot = ((queue->head + queue->count + i) % NAND_DISPATCH_CHAIN_DEPTH);
        nand_dispatch_task_load(&queue->tasks[slot], &requests[i]);
    }
    /* 全部预加载后才提交 count：poll 不会看到半挂载的批 */
    queue->count = (queue->count + count);
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_dispatch_submit
 *   功能：单命令入队（非阻塞登记）。
 *   参数：request —— 预解析的单个命令请求。
 *   返回：NAND_RESULT_OK / INVALID_ARGUMENT（异步路径未放行）/ NOT_READY（FIFO 满）。
 *   实现要点：仅转发 nand_dispatch_enqueue（放行闩锁与 room 检查都在那里）；
 *         实际执行在后续 poll。
 * -------------------------------------------------------------------- */
nand_result_e nand_dispatch_submit(const nand_dispatch_request_t *request)
{
    return nand_dispatch_enqueue(request, 1U);
}

/* --------------------------------------------------------------------
 * nand_dispatch_submit_chain
 *   功能：整链原子入队（多命令按序执行）。
 *   参数：requests —— 命令数组；count —— 链长（1..NAND_DISPATCH_CHAIN_DEPTH）。
 *   返回：NAND_RESULT_OK / INVALID_ARGUMENT（异步路径未放行）/ NOT_READY（剩余深度不足整链，无副作用）。
 * -------------------------------------------------------------------- */
nand_result_e nand_dispatch_submit_chain(const nand_dispatch_request_t *requests, BYTE count)
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
    NAND_DISPATCH_LUN_DONE      /* 队头命令完成，*completion 已回填并出队 */
} nand_dispatch_lun_outcome_t;

/* ── 命令发起结果：req_table 构建无失败路径（见 nand_dispatch_req_handler_t），
 *    故只剩"异步待下轮 step"与"同步完成/带错完成"两种。注意 nand_fc_start 返回的
 *    INVALID_ARGUMENT/TIMEOUT/OPERATION_FAILED 不是失败出口，而是"带错误状态的完成"，
 *    必须继续走完成处理链。 ── */
typedef enum
{
    NAND_DISPATCH_START_ASYNC = 0, /* nand_fc_start 返回 NOT_READY，待下轮 step */
    NAND_DISPATCH_START_SYNC       /* 同步完成或返回错误码，*fc_result 有效 */
} nand_dispatch_start_outcome_t;

/* ── CW 链式续读结果（无 ERROR 出口：链内 start 错误仍走完成路径） ── */
typedef enum
{
    NAND_DISPATCH_CW_ASYNC = 0, /* 下一 CW 异步启动，本轮到此为止 */
    NAND_DISPATCH_CW_DONE       /* 链式续读结束 */
} nand_dispatch_cw_outcome_t;

/* ── 单 LUN 上下文。不缓存 task 指针——出队后旧指针失效，
 *    一律由 &queue->tasks[queue->head] 现取（head 在单步内不变）。
 *    也不缓存 ch/ce/lun：底层 task 自带这三项（见 nand_dispatch_task_t），
 *    需要时经 task 取即可，本层不重复。 ── */
typedef struct
{
    nand_dispatch_completion_t *completion; /* 完成输出目标 */
    nand_dispatch_lun_queue_t  *queue;
    nand_fc_completion_t        nand_completion; /* 读前必写（active==0 分支依赖同轮先写） */
    BYTE                        result;
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
static nand_dispatch_start_outcome_t nand_dispatch_task_fc_start(nand_dispatch_lun_queue_t *queue, nand_dispatch_task_t *task, BYTE *fc_result)
{
    BYTE result;

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
 *   返回：ASYNC / SYNC。
 *   实现要点：日志点必须夹在"构建 request"与"nand_fc_start"之间，
 *         故 build_request 与 fc_start 在同一函数内串联，不拆开。
 *         req_table handler 无返回码（构建不存在失败路径），故此处直接接续。
 * -------------------------------------------------------------------- */
static nand_dispatch_start_outcome_t nand_dispatch_task_start(nand_dispatch_lun_queue_t *queue, nand_dispatch_task_t *task, BYTE *fc_result)
{
    g_nand_dispatch_req_table[task->operation](task);

    DISP_LOGD("FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n", task->operation, (void *)task->request.data, task->request.data_length, task->request.column, task->request.row[0]);

    return nand_dispatch_task_fc_start(queue, task, fc_result);
}

/* --------------------------------------------------------------------
 * nand_dispatch_issued_advance
 *   功能：LUN_PHASE_ISSUED 推进——等最短忙期到点，启动配对的 WAIT/FETCH。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：WAIT（忙期未到或启动异步）/ DONE（已同步完成，nand_completion 有效）。
 *   实现要点：
 *     - min_busy_us = tR(38)/tPROG(700)/tBERS(5000)，带余量的典型值。
 *     - 到点后切配对 OP（LOAD↔WAIT / ISSUE↔WAIT）；原子 OP 的 pair_op==自身，
 *       跳过不切换。
 *     - **ISSUE 段失败立即短路**：不再发配对 WAIT，直接把失败结果交回驱动路径，
 *       让 cw_chain 以非 OK 收尾。
 *     - 同步完成时相位已是 FETCH，调用方直接进入完成处理链。
 * -------------------------------------------------------------------- */
static nand_dispatch_lun_outcome_t nand_dispatch_issued_advance(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t *task   = &ctx->queue->tasks[ctx->queue->head];
    WORD                  min_us = g_nand_operation_map[task->operation].min_busy_us;
    BYTE                  pair_op;
    BYTE                  result;

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

    if ((min_us != 0U) && !delay_dwt_elapsed_us(ctx->queue->wait_start_cycles, min_us))
    {
        return NAND_DISPATCH_LUN_WAIT; /* 最短等待未到，下一轮再检查 */
    }

    pair_op = nand_operation_get_pair(task->operation);
    if ((pair_op != 0U) && (pair_op != task->operation))
    {
        task->operation = pair_op;
    }

    if (nand_dispatch_task_start(ctx->queue, task, &result) == NAND_DISPATCH_START_ASYNC)
    {
        return NAND_DISPATCH_LUN_WAIT;
    }

    ctx->nand_completion.execution_status = result;
    ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
    ctx->nand_completion.pre_status       = task->transaction.completion.pre_status;
    ctx->result                           = result;
    return NAND_DISPATCH_LUN_DONE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_lun_drive
 *   功能：Per-LUN FSM 单步——启动队头 pre-loaded 命令或推进已启动事务，
 *         并处理 ISSUE 段完成 → ISSUED 的相位切换。
 *   参数：ctx —— 单 LUN 上下文（result / nand_completion 就地更新）。
 *   返回：WAIT（本轮无进展）/ DONE（已产出 result）。
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
    BYTE                  result;

    if (task->started == 0U)
    {
        if (nand_dispatch_task_start(ctx->queue, task, &result) == NAND_DISPATCH_START_ASYNC)
        {
            return NAND_DISPATCH_LUN_WAIT;
        }
        ctx->nand_completion.execution_status = result;
        ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
        ctx->nand_completion.pre_status       = task->transaction.completion.pre_status;
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
 * nand_dispatch_is_load_complete_op / nand_dispatch_is_fetch_op
 *   功能：CW 链的 op 分类（决定链如何推进）。
 *   参数：operation —— OP_*。
 *   返回：1 = 是；0 = 否。
 *   实现要点：
 *     - load-complete：页已加载进 NAND 页缓冲、自身不输出数据；完成后从掩码
 *       最低位开始逐 CW 取数。命中两类：
 *         · WITHOUT_DATA_OUT（LUN=1 的原子加载路径，含等待 tR）
 *         · OP_READ_PAGE_WAIT（LUN>1 的 Phase V：LOAD ISSUE 发完后由 WAIT 收尾）
 *     - fetch：从页缓冲按列地址取单个 CW；每完成一个推进一个置位。
 * -------------------------------------------------------------------- */
static BYTE nand_dispatch_is_load_complete_op(BYTE operation)
{
    return ((operation == OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) || (operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT) || (operation == OP_READ_PAGE_WAIT)) ? 1U : 0U;
}

static BYTE nand_dispatch_is_fetch_op(BYTE operation)
{
    return ((operation == OP_RANDOM_DATA_READ) || (operation == OP_RANDOM_DATA_READ_MULTI_PLANE)) ? 1U : 0U;
}

/* --------------------------------------------------------------------
 * nand_dispatch_task_cw_chain
 *   功能：CW 级 chaining——「加载进页缓冲 + 逐 CW 取数」的链式推进。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：CW_ASYNC（下一 CW 异步启动，本轮到此为止）/ CW_DONE。
 *   实现要点：
 *     - 每步先 task_complete，再判断是否可续读。
 *     - **页已加载 op**（WITHOUT_DATA_OUT 两种 / Phase V 的 READ_PAGE_WAIT）
 *       完成后从掩码最低位起发取数 op；失败则链在此终止
 *       （completion_load 已把失败刷满全部槽位）。
 *     - **取数 op** 每完成一个推进到掩码的下一个置位，直到链尾。
 *     - **旧语义 op**（WITH_DATA_OUT 普通读 / OTP 读）：掩码最低位那个 CW 已由
 *       DATA_OUT 取回，其余逐 CW 随机读——保留原行为（FC 已把普通读转成加载型，
 *       此路只剩直接下发场景）。
 *     - 取数 op 按 plane 选：单 plane → OP_RANDOM_DATA_READ（05h-列-E0h）；
 *       多 plane → OP_RANDOM_DATA_READ_MULTI_PLANE（06h-列-行-E0h，行地址带 plane 位）。
 *     - 推进完全在 **flat CW 掩码**上进行（nand_dispatch_cw_next），不再有 plane-major
 *       槽位、也不再有槽位→(plane, CW) 的换算：n / CW_NUM = plane，n % CW_NUM = CW 号。
 *     - 列地址 = CW 号 × CW_SIZE；行地址取 plane_paa[n / CW_NUM]。
 *     - 链内 start 的错误码仍走完成路径，只有 NOT_READY 才中止本轮。
 *
 * 注意：本函数是**命令内**多 CW 的推进，与 FC 侧"命令链"（多条命令）无关；
 *       命令链在 NAND 侧只是 FIFO 里的连续槽。
 * -------------------------------------------------------------------- */
static nand_dispatch_cw_outcome_t nand_dispatch_task_cw_chain(nand_dispatch_lun_ctx_t *ctx)
{
    nand_dispatch_task_t *task   = &ctx->queue->tasks[ctx->queue->head];
    BYTE                  result = ctx->result;

    for (;;)
    {
        BYTE n;

        nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        if (result != NAND_RESULT_OK)
        {
            break;
        }

        if (nand_dispatch_is_load_complete_op(task->operation) != 0U)
        {
            /* 加载完成：数据已在页缓冲，从掩码最低位起取 */
            n = nand_dispatch_cw_next(task->cw_bitmap, 0U);
            if (n == CW_TOTAL_MAX)
            {
                break;
            }
            task->cw_index = n;
        }
        else if (nand_dispatch_is_fetch_op(task->operation) != 0U)
        {
            /* 取数：推进到下一个置位，链尾则结束 */
            n = nand_dispatch_cw_next(task->cw_bitmap, (task->cw_index + 1U));
            if (n == CW_TOTAL_MAX)
            {
                break;
            }
            task->cw_index = n;
        }
        else if (((task->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT) || (task->operation == OP_OTP_DATA_READ)) && ((n = nand_dispatch_cw_next(task->cw_bitmap, (task->cw_index + 1U))) != CW_TOTAL_MAX))
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
        result = nand_fc_start(&task->transaction, task->operation, &task->request);
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

/* --------------------------------------------------------------------
 * nand_dispatch_scatter_page_buffer
 *   功能：把页缓冲的整页数据拆回各 CW buffer。
 *   参数：task —— 已完成任务（自带 ch/ce/lun，定位页缓冲）。
 *   返回：无。
 *   实现要点：两类操作的数据落在页缓冲而非各 CW buffer（FMC 数据路径约定）：
 *     - 单 plane Cache 读（START_SEQ/SEQ/RANDOM/LAST）：offset = CW 号 × CW_SIZE。
 *     - 双 plane cache 读（START_SEQ_MP/SEQ_MP/LAST_MP）与 Multi-Plane
 *       WITH_DATA_OUT：offset = plane × PAGE_SIZE + CW 号 × CW_SIZE。
 *     **遍历只走掩码里的置位**：plane = n / CW_NUM、CW 号 = n % CW_NUM，
 *     不再有独立的槽位号或 PAA 里的 CW 位。
 *     页缓冲按 (ch, ce, lun) 各持一块，故索引一律取自 task 自身。
 * -------------------------------------------------------------------- */
static void nand_dispatch_scatter_page_buffer(const nand_dispatch_task_t *task)
{
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];
    BYTE  n;

    if ((task->operation == OP_READ_PAGE_CACHE_START_SEQ) || (task->operation == OP_READ_PAGE_CACHE_SEQ) || (task->operation == OP_READ_PAGE_CACHE_RANDOM) || (task->operation == OP_READ_PAGE_CACHE_LAST))
    {
        for (n = nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
        {
            BYTE cw_num = (n % CW_NUM);
            memcpy(task->cw_buffers[n], &page_buffer[cw_num * CW_SIZE], CW_SIZE);
        }
    }

    if ((task->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT) || (task->operation == OP_READ_PAGE_CACHE_START_SEQ_MP) || (task->operation == OP_READ_PAGE_CACHE_SEQ_MP) || (task->operation == OP_READ_PAGE_CACHE_LAST_MP))
    {
        for (n = nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
        {
            BYTE  plane  = (n / CW_NUM);
            BYTE  cw_num = (n % CW_NUM);
            BYTE *src    = &page_buffer[plane * PAGE_SIZE + cw_num * CW_SIZE];
            memcpy(task->cw_buffers[n], src, CW_SIZE);
        }
    }
}

/* --------------------------------------------------------------------
 * nand_dispatch_lun_step
 *   功能：Per-LUN FSM 单步——推进队头命令，完成即回填 completion 并出队。
 *   参数：ctx —— 单 LUN 上下文。
 *   返回：WAIT（本轮无进展，调用方看下一 LUN）/ DONE（队头命令完成，*completion
 *         已回填且已出队）。
 *   实现要点：
 *     - ISSUED 块只在进入驱动前判一次（与原实现一致）：先等最短忙期。
 *       issued_advance 返回 DONE 时相位已是 FETCH，落到下方驱动路径收尾。
 *     - 返回 WAIT 的出口必须已提交全部持久状态：
 *       ISSUED 忙期未到 / 配对启动异步 / started==0 首次启动异步 / step 异步 /
 *       ISSUE 段完成转 ISSUED / CW 链续读异步。
 *     - 完成即出队（head 前移、count 递减、相位复位 IDLE）。
 *       每槽 = 一条命令 = 一份 completion——链中间命令的状态不再被吞掉。
 *     - 本函数只管 LUN 自身的执行/簿记；跨通道的扫描游标由调用方 poll 维护。
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
            return out; /* WAIT（DONE 落到下方收尾） */
        }
        /* DONE：同步完成，落到下方驱动路径收尾 */
    }

    task = &ctx->queue->tasks[ctx->queue->head];
    out  = nand_dispatch_lun_drive(ctx);
    if (out != NAND_DISPATCH_LUN_DONE)
    {
        return out; /* WAIT（DONE 落到下方收尾） */
    }

    if (nand_dispatch_task_cw_chain(ctx) == NAND_DISPATCH_CW_ASYNC)
    {
        return NAND_DISPATCH_LUN_WAIT;
    }

    nand_dispatch_scatter_page_buffer(task);

    /* ── 队头命令完成：回填 completion、出队、复位相位 ── */
    *ctx->completion = task->completion;
    ctx->queue->head = ((ctx->queue->head + 1U) % NAND_DISPATCH_CHAIN_DEPTH);
    ctx->queue->count--;
    ctx->queue->phase = LUN_PHASE_IDLE;
    return NAND_DISPATCH_LUN_DONE;
}

/* --------------------------------------------------------------------
 * nand_dispatch_poll
 *   功能：遍历全部 (ch, ce, lun)，每 LUN 推进一条命令；完成一条就当场交给 sink。
 *   参数：sink —— 完成回调（每条完成调用一次，调用方在其中消费，可不缓存）；
 *         ctx  —— 透传给 sink 的调用方上下文。
 *   返回：本轮产出的完成条数（0 = 无完成；上限 CH_NUM×CE_NUM×LUN_NUM）。
 *   实现要点：
 *     - **全遍历、不早退**：每个 LUN 每轮恰好 step 一次，无论前面是否有人完成。
 *       这是公平性的来源——若"碰到第一条完成就返回"，索引靠前的 LUN 会系统性
 *       抢占，靠后的 LUN 步数递减、可被饿死（旧 g_nand_channel_cursor 只轮转通道，
 *       ce/lun 恒从 0 起，挡不住这个偏斜）。
 *     - 完成经 sink 逐条交出，故本函数只用一个 completion 槽复用，无需批量数组。
 *     - 命令执行状态不体现在返回值里：它已由 completion.cw_status 承载
 *       （completion handler 把 NAND_RESULT_* 映射成 FC_CW_STATUS_*）。
 * -------------------------------------------------------------------- */
BYTE nand_dispatch_poll(nand_dispatch_sink_t sink, void *ctx)
{
    nand_dispatch_completion_t completion;
    BYTE                       delivered = 0U;
    BYTE                       channel;
    BYTE                       ce;
    BYTE                       lun_idx;

    /* 回调前置条件由调用者保证（fc.c 轮询路径恒传有效 sink） */
    ASSERT(sink != NULL);

    for (channel = 0U; channel < CH_NUM; channel++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun_idx = 0U; lun_idx < LUN_NUM; lun_idx++)
            {
                nand_dispatch_lun_queue_t  *queue = &g_nand_dispatch_queues[channel][ce][lun_idx];
                nand_dispatch_lun_ctx_t     lun_ctx;
                nand_dispatch_lun_outcome_t out;

                if (queue->count == 0U)
                {
                    continue; /* 该 LUN 无在途任务，跳过 */
                }

                /* completion 只在 lun_step 返回 DONE 时被写入，此处复用同一槽 */
                lun_ctx.completion = &completion;
                lun_ctx.queue      = queue;
                memset(&lun_ctx.nand_completion, 0, sizeof(lun_ctx.nand_completion));
                lun_ctx.result = NAND_RESULT_OK;

                out = nand_dispatch_lun_step(&lun_ctx);
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
