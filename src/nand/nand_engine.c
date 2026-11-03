/*
 * nand_engine.c — UOP 微指令解释器 / 状态机
 *
 * 职责：顺序执行一条命令的 UOP 序列（nand_uop_t 数组），驱动后端
 *       硬件（本工程为 FMC）完成 NAND 操作。支持：
 *         - 顺序推进（每 step 可消费多个 UOP）
 *         - 条件跳转（JUMP_IF_NOT_READY：70h 状态轮询循环）
 *         - 协作式让出（NOT_READY：外设忙时返回，上层稍后再 step）
 *         - 超时检测（SysTick µs 时间 + 轮询计数双保险）
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

/**
 * @brief 阻塞执行整条 UOP 序列，直到完成或出错
 *
 * 把协作式 step 循环压成 while(NOT_READY) 阻塞调用。仅供初始化阶段
 * （nand_init，armed==0）使用；运行期走 nand_engine_start/step。
 *
 * @param backend    后端 handler 表 + context
 * @param uops       UOP 序列
 * @param timing     时序参数
 * @param request    命令数据
 * @param completion 输出：执行状态 + NAND 状态字节
 * @return NAND_RESULT_OK / 错误码
 */
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

/**
 * @brief 复位引擎游标并立即推进一步
 *
 * 只复位游标/超时/active，不整体清零：backend/request/completion 由调用方
 * 绑定并跨 start 保留。末尾先 step 一次，无 busy 阶段的命令在 start 内即完成。
 *
 * @param state 引擎状态（已绑定 backend/request/completion）
 * @param uops  UOP 序列
 * @param timing 时序参数（busy_timeout_us 必须非 0）
 * @return NAND_RESULT_OK / NAND_RESULT_NOT_READY / 错误码
 */
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
    state->pre_jump_index              = 0xFFFFU;
    state->completion->nand_status     = 0U;
    state->completion->pre_status      = 0U;
    state->completion->transfer_status = NAND_RESULT_OK;
    return nand_engine_step(state);
}

/**
 * @brief 一次性锁存忙等起点（µs 时间戳 + 轮询计数清零）
 *
 * WAIT_READY 与 JUMP_IF_NOT_READY 各持一组状态、走同一入口。
 *
 * @param state 引擎状态
 * @param w     该忙等所属的超时状态
 * @return NAND_RESULT_OK（本次或此前已锁存）/ NAND_RESULT_INVALID_ARGUMENT（µs 时钟不可用，active 已清）
 */
static BYTE nand_engine_wait_latch(nand_engine_state_t *state, nand_engine_wait_t *w)
{
    if (w->started != 0U)
    {
        return NAND_RESULT_OK;
    }
    if (!delay_time_now_us(&w->start_us))
    {
        state->active = 0U;
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    w->started = 1U;
    w->polls   = 0U;
    return NAND_RESULT_OK;
}

/**
 * @brief 忙等超时检测：轮询计数 +1 后判双条件
 *
 * µs 预算与轮询计数任一超限即超时：时间戳防计数单位误解，计数防 µs 时钟
 * 不可用/漂移。polls 饱和递增防回绕。
 *
 * @param state   引擎状态
 * @param w       该忙等所属的超时状态
 * @param op_name 日志用的微指令名
 * @return 1 已超时（active 已清）；0 未超时
 */
static BYTE nand_engine_wait_timeout(nand_engine_state_t *state, nand_engine_wait_t *w, const char *op_name)
{
    if (w->polls < UINT16_MAX)
    {
        w->polls++;
    }

    if (delay_time_elapsed_us(w->start_us, state->timing->busy_timeout_us) || (w->polls >= state->timing->busy_timeout_us))
    {
        LOGE(LOG_TAG_NAND "%s timeout op=%u polls=%u timeout_us=%u\r\n", op_name, state->request->operation, w->polls, state->timing->busy_timeout_us);
        state->active = 0U;
        return 1U;
    }
    return 0U;
}

/**
 * @brief 按类型白名单打印 UOP 成功日志（只打关键节点，避免刷屏）
 *
 * 白名单是「会改变 NAND 状态或需要观察」的微指令；纯地址/命令字节写入不打。
 *
 * @param state 引擎状态（uop_index / request / completion 供打印）
 */
static void nand_engine_uop_log_ok(const nand_engine_state_t *state)
{
    BYTE type = state->uops[state->uop_index].type;

    if ((type == NAND_UOP_DATA_IN) || (type == NAND_UOP_DATA_OUT) || (type == NAND_UOP_WAIT_READY) || (type == NAND_UOP_CHECK_FAIL))
    {
        ENG_LOGD("uop[%hu]=%hhu OK op=%hhu nand_st=0x%02hhX polls=%hu\r\n", state->uop_index, type, state->request->operation, state->completion->nand_status, state->debug_polls);
    }
}

/**
 * @brief 推进 UOP 游标一次（可消费多个 UOP），驱动后端
 *
 * END 置 inactive 并返回命令结果；JUMP_IF_NOT_READY 由引擎直接处理
 * （ready 前进，busy 双条件超时后回跳 operand 步）；其余类型查后端
 * handler 表执行。循环跑满 NAND_ENGINE_MAX_UOPS 无 END 视为非法序列。
 *
 * @param state 引擎状态（游标 + 超时字段）
 * @return NAND_RESULT_OK / NOT_READY（让出）/ TIMEOUT / INVALID_ARGUMENT / OPERATION_FAILED
 */
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
            // OP 前的第一次读状态是“命令下发时的器件状态”（RDY/ARDY/WP）；
            // OP 后的第二次读状态是操作结果（FAIL/FAILC）。只跟随**第一个** JUMP
            // 的循环更新 pre_status（busy 时保留最后一次读到的 busy 状态）。
            if (state->pre_jump_index == 0xFFFFU)
            {
                state->pre_jump_index = state->uop_index;
            }
            if (state->uop_index == state->pre_jump_index)
            {
                state->completion->pre_status = state->completion->nand_status;
            }
            if ((state->completion->nand_status & NAND_STATUS_READY) != 0U)
            {
                /* NAND ready（状态字节 bit6）→ 复位本组忙等状态，推进到下一 UOP。
                 * 复位是必需的：否则同一序列中后续 JUMP 会复用上一轮的陈旧 µs 起点。 */
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

        /* WAIT_READY 首次进入：记录 µs 起点（用独立 wait 状态） */
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
