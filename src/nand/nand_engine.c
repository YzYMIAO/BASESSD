#include "nand_engine.h"

#include "common/ssd_log.h"
#include "delay.h"
#include "nand/nand_debug.h"
#include "onfi_1_0.h"

#include <stddef.h>

#define NAND_ENGINE_MAX_UOPS 64U

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
                /* NAND ready → 推进到下一 UOP */
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
                SSD_LOGE("nand: JUMP_IF_NOT_READY timeout op=%u polls=%u timeout_us=%u\r\n",
                         state->request->operation,
                         state->wait_polls,
                         state->timing->busy_timeout_us);
                state->active = 0U;
                return NAND_RESULT_TIMEOUT;
            }

            /* 未超时：跳回 operand 步重新执行 70h 轮询序列 */
            state->uop_index -= (uint16_t)uop->operand;
            return NAND_RESULT_NOT_READY;
        }

        handler = state->backend->handlers[uop->type];

        if (handler == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }

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
            continue;
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
    return NAND_RESULT_INVALID_ARGUMENT;
}