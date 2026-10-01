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

#include "common/ssd_log.h"
#include "delay.h"
#include "nand/nand_debug.h"
#include "onfi_1_0.h"

#include <stddef.h>

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
nand_result_t nand_engine_execute(const nand_engine_backend_t *backend, const nand_uop_t *uops, const nand_operation_timing_t *timing, const nand_command_request_t *request, nand_command_completion_t *completion)
{
    nand_engine_state_t state;
    uint8_t             result;

    result = nand_engine_start(&state, backend, uops, timing, request, completion);
    while (result == NAND_RESULT_NOT_READY)
    {
        result = nand_engine_step(&state);
    }
    return result;
}

/* --------------------------------------------------------------------
 * nand_engine_start
 *   功能：初始化引擎状态并立即推进一步。
 *   参数：state —— 引擎游标（调用方提供）；其余同 execute。
 *   返回：NAND_RESULT_OK / NOT_READY / 错误码。
 *   实现要点：
 *     - *state 整体清零，active=1，预置 completion 初值。
 *     - 末尾直接调一次 step：无 busy 阶段命令（RESET 等）在 start
 *       内即完成（上层 nand_fc_start 据此置 transaction->active=0）。
 *     - 返回值可为 OK 或 NOT_READY，调用方须两者都接受。
 * -------------------------------------------------------------------- */
nand_result_t nand_engine_start(nand_engine_state_t *state, const nand_engine_backend_t *backend, const nand_uop_t *uops, const nand_operation_timing_t *timing, const nand_command_request_t *request, nand_command_completion_t *completion)
{
    if ((state == NULL) || (backend == NULL) || (backend->handlers == NULL) || (uops == NULL) ||
        (timing == NULL) || (request == NULL) || (completion == NULL))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    *state                      = (nand_engine_state_t){0};
    state->backend              = backend;
    state->uops                 = uops;
    state->timing               = timing;
    state->request              = request;
    state->completion           = completion;
    state->active               = 1U;
    completion->nand_status     = 0U;
    completion->transfer_status = NAND_RESULT_OK;
    return nand_engine_step(state);
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
 * -------------------------------------------------------------------- */
nand_result_t nand_engine_step(nand_engine_state_t *state)
{
    const nand_uop_t  *uop;
    nand_uop_handler_t handler;
    uint8_t            result;

    if ((state == NULL) || (state->active == 0U) || (state->backend == NULL) ||
        (state->backend->handlers == NULL) || (state->uops == NULL) || (state->timing == NULL) ||
        (state->request == NULL) || (state->completion == NULL))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    for (; state->uop_index < NAND_ENGINE_MAX_UOPS;)
    {
        uop = &state->uops[state->uop_index];

        if (uop->type == NAND_UOP_END)
        {
            state->active = 0U;
            return state->completion->transfer_status;
        }

        if (uop->type >= NAND_UOP_COUNT)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }

        /* ── NAND_UOP_JUMP_IF_NOT_READY：引擎直接处理的条件跳转微指令 ── */
        if (uop->type == NAND_UOP_JUMP_IF_NOT_READY)
        {
            if ((state->completion->nand_status & NAND_STATUS_READY) != 0U)
            {
                /* NAND ready（状态字节 bit6）→ 推进到下一 UOP */
                state->uop_index++;
                continue;
            }

            /* NAND busy → 超时检测 */
            if (state->wait_started == 0U)
            {
                if (!delay_dwt_cycles_now(&state->wait_start_cycles))
                {
                    state->active = 0U;
                    return NAND_RESULT_INVALID_ARGUMENT;
                }
                state->wait_started = 1U;
                state->wait_polls   = 0U;
            }
            if (state->wait_polls < UINT16_MAX)
            {
                state->wait_polls++;
            }

            if (delay_dwt_elapsed_us(state->wait_start_cycles, state->timing->busy_timeout_us) ||
                (state->wait_polls >= state->timing->busy_timeout_us))
            {
                /* 双超时：DWT 时钟周期预算 或 轮询计数，任一触发即超时 */
                SSD_LOGE("nand: JUMP_IF_NOT_READY timeout op=%u polls=%u timeout_us=%u\r\n",
                         state->request->operation,
                         state->wait_polls,
                         state->timing->busy_timeout_us);
                state->active = 0U;
                return NAND_RESULT_TIMEOUT;
            }

            /* 未超时：回跳 operand 步，重执行 70h 轮询序列（状态读→延时→再读） */
            state->uop_index -= (uint16_t)uop->operand;
            return NAND_RESULT_NOT_READY;
        }

        handler = state->backend->handlers[uop->type];

        if (handler == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }

        /* WAIT_READY 首次进入：记录 DWT 起点（与 JUMP 共用 wait_start_cycles） */
        if ((uop->type == NAND_UOP_WAIT_READY) &&
            (state->wait_started == 0U))
        {
            if (!delay_dwt_cycles_now(&state->wait_start_cycles))
            {
                state->active = 0U;
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            state->wait_started = 1U;
            state->wait_polls   = 0U;
        }

        result = handler(state->backend->context, uop, state->request, state->timing, state->completion);

        if (result == NAND_RESULT_OK)
        {
            if ((uop->type == NAND_UOP_DATA_IN) || (uop->type == NAND_UOP_DATA_OUT) ||
                (uop->type == NAND_UOP_WAIT_READY) ||
                (uop->type == NAND_UOP_CHECK_FAIL) || (uop->type == NAND_UOP_END))
            {
                SSD_LOGD("nand: uop[%u]=%u OK op=%u nand_st=0x%02X polls=%u\r\n",
                         (unsigned)state->uop_index,
                         (unsigned)uop->type,
                         (unsigned)state->request->operation,
                         (unsigned)state->completion->nand_status,
                         (unsigned)state->debug_polls);
            }
            state->uop_index++;
            state->wait_started = 0U;
            state->wait_polls   = 0U;
            continue; /* OK 后可继续消费下一 UOP（一步多推进） */
        }

        if (result == NAND_RESULT_NOT_READY)
        {
            if (state->debug_polls < UINT16_MAX)
            {
                state->debug_polls++;
            }
            if ((state->request->operation == OP_NORMAL_PROGRAM_PAGE) &&
                ((state->debug_polls == 1U) || ((state->debug_polls % 1000U) == 0U)))
            {
                SSD_LOGD("nand: program pending uop=%u status=0x%02X wait=%u\r\n",
                         state->uop_index,
                         state->completion->nand_status,
                         state->debug_polls);
            }
            if (uop->type == NAND_UOP_WAIT_READY)
            {
                if (state->wait_polls < UINT16_MAX)
                {
                    state->wait_polls++;
                }

                if (delay_dwt_elapsed_us(state->wait_start_cycles, state->timing->busy_timeout_us) ||
                    (state->wait_polls >= state->timing->busy_timeout_us))
                {
                    /* WAIT_READY 专用超时：DWT + 轮询计数双条件 */
                    SSD_LOGE("nand: WAIT_READY timeout op=%u polls=%u timeout_us=%u\r\n",
                             state->request->operation,
                             state->wait_polls,
                             state->timing->busy_timeout_us);
                    state->active = 0U;
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