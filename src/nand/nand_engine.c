/*
 * nand_engine.c — UOP 微指令解释器 / 状态机
 *
 * 职责：顺序执行一条命令的 UOP 序列（nand_uop_t 数组），驱动后端
 *       硬件（本工程为 FMC）完成 NAND 操作。支持：
 *         - 顺序推进（每 step 可消费多个 UOP）
 *         - 条件跳转（JUMP_IF_NOT_READY：70h 状态轮询循环）
 *         - 协作式让出（NOT_READY：外设忙时返回，上层稍后再 step）
 *         - 超时检测（DWT 时钟周期 + 轮询计数双保险）
 *
 * 调用链：
 *   nand_fc_execute/start/step（nand_mt29*.c）
 *     → nand_engine_*（本文件）
 *     → 后端 handler 表（nand_fmc.c 的 g_nand_fmc_handlers）
 *
 * 关键约定：
 *   - NOT_READY 是"让出点"：引擎不阻塞，调用方循环 step 推进。
 *   - JUMP_IF_NOT_READY 由引擎直接处理，不经后端 handler。
 *   - 单条命令 UOP 数上限 NAND_ENGINE_MAX_UOPS（64），防死循环。
 */

#include "nand_engine.h"

#include "assertion.h"
#include "delay.h"
#include "log.h"
#include "nand_debug.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

#define NAND_ENGINE_MAX_UOPS 64U

/* --------------------------------------------------------------------
 * nand_engine_execute
 *   功能：阻塞式执行整条 UOP 序列，直到完成或出错。
 *   参数：backend —— 后端 handler 表 + context；uops —— UOP 序列；
 *         timing —— 时序参数；request/completion —— 命令数据。
 *   返回：NAND_RESULT_OK 或错误码。
 *   实现要点：@deprecated 仅初始化阶段同步调用（nand_init）；
 *         把协作式 step 循环压成 while(NOT_READY) 阻塞调用。
 * -------------------------------------------------------------------- */
nand_result_e nand_engine_execute(const nand_engine_backend_t *backend, const nand_uop_t *uops, const nand_operation_timing_t *timing, const nand_command_request_t *request, nand_command_completion_t *completion)
{
    /* 引擎上下文的 backend/request/completion 三指针随 state 固定，先绑定再 start */
    nand_engine_state_t state;
    memset(&state, 0, sizeof(state));
    state.backend    = backend;
    state.request    = request;
    state.completion = completion;
    BYTE result;

    result = nand_engine_start(&state, uops, timing);
    while (result == NAND_RESULT_NOT_READY)
    {
        result = nand_engine_step(&state);
    }
    return result;
}

/* --------------------------------------------------------------------
 * nand_engine_start
 *   功能：复位引擎游标并立即推进一步。
 *   参数：state —— 引擎状态（调用方提供，须已绑定 backend/request/completion）；
 *         uops —— UOP 序列；timing —— 时序参数。
 *   返回：NAND_RESULT_OK / NOT_READY / 错误码。
 *   实现要点：
 *     - 只复位游标/超时/active，不整体清零：backend/request/completion 由
 *       调用方绑定、跨 start 保留（原实现整体清零，故此处逐个显式复位）。
 *     - 末尾直接调一次 step：无 busy 阶段命令（RESET 等）在 start
 *       内即完成（上层 nand_fc_start 据此清 active）。
 *     - 返回值可为 OK 或 NOT_READY，调用方须两者都接受。
 * -------------------------------------------------------------------- */
nand_result_e nand_engine_start(nand_engine_state_t *state, const nand_uop_t *uops, const nand_operation_timing_t *timing)
{
    /* 前置条件由调用者保证（见 nand_engine.h）：state->backend/request/completion 三指针
     * 随事务固定（nand_dispatch_task_load / nand_fc_start 绑定），uops/timing 取自
     * g_nand_operation_map[operation]（调用者已判 map->uops != NULL）。 */
    ASSERT(state != NULL);
    ASSERT(state->backend != NULL);
    ASSERT(state->backend->handlers != NULL);
    ASSERT(state->request != NULL);
    ASSERT(state->completion != NULL);
    ASSERT(uops != NULL);
    ASSERT(timing != NULL);
    /* busy_timeout_us 为 0 会让忙等双条件首次即判超时（elapsed>=0 恒真），属时序表配置错误 */
    ASSERT(timing->busy_timeout_us > 0U);

    state->uops                        = uops;
    state->timing                      = timing;
    state->uop_index                   = 0U;
    state->debug_polls                 = 0U;
    state->active                      = 1U;
    state->wait.started                = 0U;
    state->wait.polls                  = 0U;
    state->jump.started                = 0U;
    state->jump.polls                  = 0U;
    state->completion->nand_status     = 0U;
    state->completion->transfer_status = NAND_RESULT_OK;
    return nand_engine_step(state);
}

/* --------------------------------------------------------------------
 * nand_engine_wait_latch
 *   功能：一次性锁存忙等起点（DWT 时间戳 + 轮询计数清零）。
 *   参数：state —— 引擎状态；w —— 该忙等所属的超时状态（wait / jump）。
 *   返回：NAND_RESULT_OK（本次锁存成功，或之前已锁存）/ INVALID_ARGUMENT（DWT 不可用）。
 *   实现要点：WAIT_READY 与 JUMP_IF_NOT_READY 各持一组状态、走同一入口；
 *         失败时清 active，调用方直接返回。
 * -------------------------------------------------------------------- */
static BYTE nand_engine_wait_latch(nand_engine_state_t *state, nand_engine_wait_t *w)
{
    if (w->started != 0U)
    {
        return NAND_RESULT_OK;
    }
    if (!delay_dwt_cycles_now(&w->start_cycles))
    {
        state->active = 0U;
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    w->started = 1U;
    w->polls   = 0U;
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_engine_wait_timeout
 *   功能：忙等超时检测——轮询计数 +1 后判双条件。
 *   参数：state —— 引擎状态；w —— 该忙等所属的超时状态；op_name —— 日志用的微指令名。
 *   返回：1 = 已超时（active 已清）；0 = 未超时。
 *   实现要点：DWT 周期预算与轮询计数任一超限即判超时（DWT 防计数单位误解，
 *         计数防 DWT 不可用/漂移）；polls 饱和递增防回绕。
 * -------------------------------------------------------------------- */
static BYTE nand_engine_wait_timeout(nand_engine_state_t *state, nand_engine_wait_t *w, const char *op_name)
{
    if (w->polls < UINT16_MAX)
    {
        w->polls++;
    }

    if (delay_dwt_elapsed_us(w->start_cycles, state->timing->busy_timeout_us) || (w->polls >= state->timing->busy_timeout_us))
    {
        LOGE(LOG_TAG_NAND "%s timeout op=%u polls=%u timeout_us=%u\r\n", op_name, state->request->operation, w->polls, state->timing->busy_timeout_us);
        state->active = 0U;
        return 1U;
    }
    return 0U;
}

/* --------------------------------------------------------------------
 * nand_engine_uop_log_ok
 *   功能：按类型白名单打印 UOP 成功日志（只打关键节点，避免刷屏）。
 *   参数：state —— 引擎状态（uop_index / request / completion 供打印）。
 *   返回：无。
 *   实现要点：白名单是"会改变 NAND 状态或需要观察"的微指令；纯地址/命令
 *         字节写入不打。NAND_UOP_END 不在此列——它在 step 里提前返回，
 *         永远到不了 handler 成功分支。
 * -------------------------------------------------------------------- */
static void nand_engine_uop_log_ok(const nand_engine_state_t *state)
{
    BYTE type = state->uops[state->uop_index].type;

    if ((type == NAND_UOP_DATA_IN) || (type == NAND_UOP_DATA_OUT) || (type == NAND_UOP_WAIT_READY) || (type == NAND_UOP_CHECK_FAIL))
    {
        ENG_LOGD("uop[%u]=%u OK op=%u nand_st=0x%02X polls=%u\r\n", (unsigned)state->uop_index, (unsigned)type, (unsigned)state->request->operation, (unsigned)state->completion->nand_status, (unsigned)state->debug_polls);
    }
}

/* --------------------------------------------------------------------
 * nand_engine_step
 *   功能：推进 UOP 游标一次（可消费多个 UOP），驱动后端。
 *   参数：state —— 引擎状态（游标 + 超时字段）。
 *   返回：NAND_RESULT_OK（本步完成，可继续）/ NOT_READY（外设忙，让出）/
 *         TIMEOUT / INVALID_ARGUMENT / OPERATION_FAILED。
 *   实现要点（核心状态机）：
 *     - END → 置 inactive，返回 transfer_status（命令结果）。
 *     - JUMP_IF_NOT_READY → 引擎直接处理：ready 则前进；busy 则
 *       DWT+计数双超时，未超时回跳 operand 步（70h 轮询循环）。
 *     - 其余 UOP → 查后端 handler 表执行；OK 则前进并清零 wait 状态；
 *       NOT_READY 则让出（WAIT_READY 同时做超时检测）。
 *     - 循环跑满 NAND_ENGINE_MAX_UOPS 无 END → 非法序列，报错。
 *   忙等/日志的类型特化装饰抽成三个 static helper（见上），本函数只留
 *   解释器骨架：END / JUMP 是控制流（不查表），其余一律走 handler 表。
 * -------------------------------------------------------------------- */
nand_result_e nand_engine_step(nand_engine_state_t *state)
{
    const nand_uop_t  *uop;
    nand_uop_handler_t handler;
    BYTE               result;

    /* 前置条件由调用者保证（见 nand_engine.h）：active==0 后不得再调；游标字段由
     * nand_engine_start 填好。nand_fc_step 已先判 active，nand_engine_execute 的
     * 循环与 start 内部调用也都在 active==1 时进入。 */
    ASSERT(state != NULL);
    ASSERT(state->active != 0U);
    ASSERT(state->backend != NULL);
    ASSERT(state->backend->handlers != NULL);
    ASSERT(state->uops != NULL);
    ASSERT(state->timing != NULL);
    ASSERT(state->request != NULL);
    ASSERT(state->completion != NULL);

    for (; state->uop_index < NAND_ENGINE_MAX_UOPS;)
    {
        uop = &state->uops[state->uop_index];

        if (uop->type == NAND_UOP_END)
        {
            state->active = 0U;
            return state->completion->transfer_status;
        }

        /* UOP 流来自 nand_dispatch_tables.c 的静态表，type 必然 < NAND_UOP_COUNT */
        ASSERT(uop->type < NAND_UOP_COUNT);

        /* ── NAND_UOP_JUMP_IF_NOT_READY：引擎直接处理的条件跳转微指令 ── */
        if (uop->type == NAND_UOP_JUMP_IF_NOT_READY)
        {
            if ((state->completion->nand_status & NAND_STATUS_READY) != 0U)
            {
                /* NAND ready（状态字节 bit6）→ 复位本组忙等状态，推进到下一 UOP。
                 * 复位是必需的：否则同一序列中后续 JUMP 会复用上一轮的陈旧 DWT 起点。 */
                state->jump.started = 0U;
                state->jump.polls   = 0U;
                state->uop_index++;
                continue;
            }

            /* NAND busy → 锁存起点 + 双条件超时检测（用独立 jump 状态，
             * 不被回跳体内返回 OK 的 UOP 复位） */
            if (nand_engine_wait_latch(state, &state->jump) != NAND_RESULT_OK)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            if (nand_engine_wait_timeout(state, &state->jump, "JUMP_IF_NOT_READY") != 0U)
            {
                return NAND_RESULT_TIMEOUT;
            }

            /* 未超时：回跳 operand 步，重执行 70h 轮询序列（状态读→延时→再读） */
            state->uop_index -= uop->operand;
            return NAND_RESULT_NOT_READY;
        }

        handler = state->backend->handlers[uop->type];

        /* 控制流 UOP（JUMP_IF_NOT_READY / END）已在上方拦截，不会走到查表；
         * 其余类型的 handler 表覆盖枚举全部非控制流项，查得 NULL 即表与枚举
         * 失配的编程错误（返回错误码会把固件 bug 伪装成 NAND 命令失败）。 */
        ASSERT(handler != NULL);

        /* WAIT_READY 首次进入：记录 DWT 起点（用独立 wait 状态） */
        if (uop->type == NAND_UOP_WAIT_READY)
        {
            if (nand_engine_wait_latch(state, &state->wait) != NAND_RESULT_OK)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
        }

        result = handler(state->backend->context, uop, state->request, state->timing, state->completion);

        if (result == NAND_RESULT_OK)
        {
            nand_engine_uop_log_ok(state);
            state->uop_index++;
            state->wait.started = 0U; /* 复位 WAIT_READY 忙等，供下一次等待重新锁存 */
            state->wait.polls   = 0U;
            continue; /* OK 后可继续消费下一 UOP（一步多推进） */
        }

        if (result == NAND_RESULT_NOT_READY)
        {
            if (state->debug_polls < UINT16_MAX)
            {
                state->debug_polls++;
            }
            if ((state->request->operation == OP_NORMAL_PROGRAM_PAGE) && ((state->debug_polls == 1U) || ((state->debug_polls % 1000U) == 0U)))
            {
                ENG_LOGD("program pending uop=%u status=0x%02X wait=%u\r\n", state->uop_index, state->completion->nand_status, state->debug_polls);
            }
            if (uop->type == NAND_UOP_WAIT_READY)
            {
                if (nand_engine_wait_timeout(state, &state->wait, "WAIT_READY") != 0U)
                {
                    return NAND_RESULT_TIMEOUT;
                }
            }
            return NAND_RESULT_NOT_READY;
        }

        if (result != NAND_RESULT_OK)
        {
            state->active = 0U;
            return result;
        }
    }

    state->active = 0U;
    return NAND_RESULT_INVALID_ARGUMENT; /* 循环跑满无 END：视为非法序列 */
}