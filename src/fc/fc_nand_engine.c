/*
 * fc_nand_engine.c — FC 的 NAND 命令引擎：UOP 解释器 + 命令编排
 *
 * 职责：顺序执行一条命令的 UOP 序列（nand_uop_s 数组），驱动后端
 *       硬件（本工程为 FMC）完成 NAND 操作。支持：
 *         - 顺序推进（每 step 可消费多个 UOP）
 *         - 条件跳转（JUMP_IF_NOT_READY：70h 状态轮询循环）
 *         - 协作式让出（NOT_READY：外设忙时返回，上层稍后再 step）
 *         - 超时检测（SysTick µs 时间 + 轮询计数双保险）
 *
 * 调用链：
 *   fc_nand_execute/start/step（本文件）
 *     → fc_uop_engine_*（本文件）
 *     → 后端 handler 表（nand_fmc.c 的 g_nand_fmc_handlers）
 *
 * 关键约定：
 *   - NOT_READY 是"让出点"：引擎不阻塞，调用方循环 step 推进。
 *   - JUMP_IF_NOT_READY 由引擎直接处理，不经后端 handler。
 *   - 单条命令 UOP 数上限 NAND_ENGINE_MAX_UOPS（64），防死循环。
 */

#include "fc_nand_engine.h"

#include "assertion.h"
#include "delay.h"
#include "log.h"
#include "nand_config.h"
#include "nand_debug.h"
#include "nand_device.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

#define NAND_ENGINE_MAX_UOPS 64U

/* 无器件/无响应时 DQ 总线读到全 1（浮空）；78h 状态字节用它判 LUN 不存在。 */
#define NAND_STATUS_NO_RESPONSE (0xFFU)

/* 异步路径放行闩锁：0 = 初始化/同步命令阶段；1 = 已移交异步 dispatch。
 * 放在引擎侧：它表达的是「引擎当前允许阻塞执行还是异步推进」的执行模式，
 * 调度器只查询它。这样 dispatch 单向依赖 engine，engine 不再反向 include dispatch。 */
static BYTE s_engine_armed;

/**
 * @brief 置/清异步路径放行闩锁（归一化到 0/1）
 *
 * @param armed 非 0 放行。
 */
void fc_nand_engine_set_armed(BYTE armed)
{
    s_engine_armed = (armed != 0U) ? 1U : 0U;
}

/**
 * @brief 查询异步路径放行闩锁
 *
 * @return 1 = 已放行；0 = 同步初始化阶段。
 */
BYTE fc_nand_engine_is_armed(void)
{
    return s_engine_armed;
}

/**
 * @brief 阻塞执行整条 UOP 序列，直到完成或出错
 *
 * 把协作式 step 循环压成 while(NOT_READY) 阻塞调用。仅供初始化阶段
 * （nand_init，armed==0）使用；运行期走 fc_uop_engine_start/step。
 *
 * @param backend    后端 handler 表 + context
 * @param uops       UOP 序列
 * @param timing     时序参数
 * @param request    命令数据
 * @param completion 输出：执行状态 + NAND 状态字节
 * @return NAND_RESULT_OK / 错误码
 */
static nand_result_e fc_uop_engine_execute(const nand_phy_backend_s *backend, const nand_uop_s *uops, const nand_op_timing_s *timing, const nand_cmd_req_s *request, nand_cmd_comp_s *completion)
{
    /* 引擎上下文的 backend/request/completion 三指针随 state 固定，先绑定再 start */
    fc_uop_engine_state_s state;
    memset(&state, 0, sizeof(state));
    state.backend    = backend;
    state.request    = request;
    state.completion = completion;
    BYTE result;

    result = fc_uop_engine_start(&state, uops, timing);
    while (result == NAND_RESULT_NOT_READY)
    {
        result = fc_uop_engine_step(&state);
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
nand_result_e fc_uop_engine_start(fc_uop_engine_state_s *state, const nand_uop_s *uops, const nand_op_timing_s *timing)
{
    /* 前置条件由调用者保证（见 fc_nand_engine.h）：state->backend/request/completion 三指针
     * 随事务固定（fc_nand_dispatch_task_load / fc_nand_start 绑定），uops/timing 取自
     * g_nand_operation_map[operation]（调用者已判 map->uops != NULL）。 */
    ASSERT(state->backend != NULL);
    ASSERT(state->backend->handlers != NULL);
    ASSERT(state->request != NULL);
    ASSERT(state->completion != NULL);
    /* busy_timeout_us 为 0 会让忙等双条件首次即判超时（elapsed>=0 恒真），属时序表配置错误 */
    ASSERT(timing->busy_timeout_us > 0U);

    state->uops                         = uops;
    state->timing                       = timing;
    state->uop_index                    = 0U;
    state->debug_polls                  = 0U;
    state->active                       = 1U;
    state->wait.started                 = 0U;
    state->wait.polls                   = 0U;
    state->jump.started                 = 0U;
    state->jump.polls                   = 0U;
    state->pre_jump_index               = 0xFFFFU;
    state->completion->nand_status      = 0U;
    state->completion->pre_status       = 0U;
    state->completion->execution_status = NAND_RESULT_OK;
    return fc_uop_engine_step(state);
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
static BYTE fc_uop_engine_wait_latch(fc_uop_engine_state_s *state, fc_uop_engine_wait_s *w)
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
static BYTE fc_uop_engine_wait_timeout(fc_uop_engine_state_s *state, fc_uop_engine_wait_s *w, const char *op_name)
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
static void fc_uop_engine_uop_log_ok(const fc_uop_engine_state_s *state)
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
nand_result_e fc_uop_engine_step(fc_uop_engine_state_s *state)
{
    const nand_uop_s     *uop;
    nand_uop_handler_fn_t handler;
    BYTE                  result;

    /* 前置条件由调用者保证（见 fc_nand_engine.h）：active==0 后不得再调；游标字段由
     * fc_uop_engine_start 填好。fc_nand_step 已先判 active，fc_uop_engine_execute 的
     * 循环与 start 内部调用也都在 active==1 时进入。 */
    // 三指针/表由 fc_uop_engine_start 的入口断言保证；step 只守“仍 active”这个每步都可能变的状态。
    ASSERT(state->active != 0U);

    for (; state->uop_index < NAND_ENGINE_MAX_UOPS;)
    {
        uop = &state->uops[state->uop_index];

        if (uop->type == NAND_UOP_END)
        {
            state->active = 0U;
            return state->completion->execution_status;
        }

        /* UOP 流来自 fc_nand_dispatch.c 的静态表，type 必然 < NAND_UOP_COUNT */
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
            if (fc_uop_engine_wait_latch(state, &state->jump) != NAND_RESULT_OK)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            if (fc_uop_engine_wait_timeout(state, &state->jump, "JUMP_IF_NOT_READY") != 0U)
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
            if (fc_uop_engine_wait_latch(state, &state->wait) != NAND_RESULT_OK)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
        }

        result = handler(state->backend->context, uop, state->request, state->timing, state->completion);

        if (result == NAND_RESULT_OK)
        {
            fc_uop_engine_uop_log_ok(state);
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
                if (fc_uop_engine_wait_timeout(state, &state->wait, "WAIT_READY") != 0U)
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

/* ══════════════════════════════════════════════════════════
 * NAND 命令编排 / 初始化 / 拓扑扫描
 * ══════════════════════════════════════════════════════════ */

static nand_result_e fc_nand_execute(BYTE ch, BYTE ce, BYTE operation, const nand_cmd_req_s *request, nand_cmd_comp_s *completion)
{
    const nand_op_map_s      *map;
    const nand_phy_backend_s *backend;
    BYTE                      status;

    ASSERT(operation < OP_COUNT);
    ASSERT(request != NULL);
    ASSERT(completion != NULL);
    /* 同步入口契约：仅初始化阶段（armed==0）可调。放行后必须走
     * fc_nand_start/step 异步路径——此处阻塞式执行会卡住整个调度器。 */
    ASSERT(fc_nand_engine_is_armed() == 0U);

    map = &g_nand_operation_map[operation];
    if (map->uops == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    const nand_result_e target_result = nand_target_get(ch, ce, &backend);

    if (target_result != NAND_RESULT_OK)
    {
        return target_result;
    }
    status = fc_uop_engine_execute(backend, map->uops, &map->timing, request, completion);
    return status;
}

/**
 * @brief 发起异步 ONFI 事务（非阻塞，只推进一步）
 *
 * @param transaction 事务状态（backend 已绑定）
 * @param operation   OP_*
 * @param request     命令数据
 * @return OK / NOT_READY（需 fc_nand_step）/ 错误码
 */
nand_result_e fc_nand_start(fc_nand_txn_s *transaction, BYTE operation, const nand_cmd_req_s *request)
{
    const nand_op_map_s *map;
    BYTE                 result;

    ASSERT(transaction != NULL);
    ASSERT(operation < OP_COUNT);
    ASSERT(request != NULL);

    map = &g_nand_operation_map[operation];
    if (map->uops == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    ENG_LOGD("fc_nand_start: op=%hhu col=%hu row=0x%06lX data=%p len=%hu\r\n", operation, request->column, request->row[0], request->buffer, request->data_length);

    transaction->request           = *request;
    transaction->request.operation = operation;
    memset(&transaction->completion, 0, sizeof(transaction->completion));
    /* backend 由 dispatch 的任务装载处绑定；engine_start 统一校验。 */
    result = fc_uop_engine_start(&transaction->engine, map->uops, &map->timing);
    if (result != NAND_RESULT_NOT_READY)
    {
        transaction->engine.active = 0U;
        LOGE_IF(result != NAND_RESULT_OK, LOG_TAG_NAND "start failed, op=%u result=0x%02X status=0x%02X\r\n", operation, result, transaction->completion.nand_status);
    }
    return result;
}

/**
 * @brief 推进异步事务一个引擎步
 *
 * @param transaction 发起中的事务
 * @param completion  输出：完成状态
 * @return NOT_READY（继续 poll）/ 完成结果
 */
nand_result_e fc_nand_step(fc_nand_txn_s *transaction, nand_cmd_comp_s *completion)
{
    BYTE result;

    ASSERT(transaction != NULL);
    ASSERT(completion != NULL);

    result = fc_uop_engine_step(&transaction->engine);
    if (result != NAND_RESULT_NOT_READY)
    {
        completion->execution_status = result;
        completion->nand_status      = transaction->completion.nand_status;
        completion->pre_status       = transaction->completion.pre_status;
        ENG_LOGD("complete, op=%u result=0x%02X status=0x%02X\r\n", transaction->request.operation, result, completion->nand_status);
        transaction->engine.active = 0U;
    }
    return result;
}

/**
 * @brief NAND 复位（同步）
 *
 * @param completion 输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
static nand_result_e fc_nand_reset(BYTE ch, BYTE ce, nand_cmd_comp_s *completion)
{
    nand_cmd_req_s request;

    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.buffer      = NULL;
    request.data_length = 0U;
    return fc_nand_execute(ch, ce, OP_RESET, &request, completion);
}

/**
 * @brief 读取芯片 ID（5 字节，同步）
 *
 * @param id         输出：5 字节
 * @param completion 输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
static nand_result_e fc_nand_read_id(BYTE ch, BYTE ce, BYTE id[5], nand_cmd_comp_s *completion)
{
    nand_cmd_req_s request;

    ASSERT(id != NULL);
    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.buffer      = id;
    request.data_length = 5U;
    return fc_nand_execute(ch, ce, OP_READ_ID, &request, completion);
}

/**
 * @brief 读 ONFI 参数页（ECh，256 字节，同步）
 *
 * @param page       输出：参数页
 * @param completion 输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
static nand_result_e fc_nand_read_parameter_page(BYTE ch, BYTE ce, BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], nand_cmd_comp_s *completion)
{
    nand_cmd_req_s request;

    ASSERT(page != NULL);
    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.buffer      = page;
    request.data_length = NAND_ONFI_PARAM_PAGE_SIZE;
    return fc_nand_execute(ch, ce, OP_READ_PARAMETER_PAGE, &request, completion);
}

/**
 * @brief 78h READ STATUS ENHANCED（同步）
 *
 * @param row        行地址（含 LUN/plane 位）
 * @param completion 输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
static nand_result_e fc_nand_read_status_enhanced(BYTE ch, BYTE ce, DWORD row, nand_cmd_comp_s *completion)
{
    nand_cmd_req_s request;

    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.row[0]      = row;
    request.buffer      = NULL;
    request.data_length = 1U;
    return fc_nand_execute(ch, ce, OP_READ_STATUS_ENHANCED, &request, completion);
}

/**
 * @brief 把解码后的 ONFI 参数页打三行开机日志
 *
 * @param info 已解码的参数页
 */
static void fc_nand_onfi_param_print(const nand_onfi_param_info_s *info)
{
    /* 器件总容量（MiB）= 页主区 × 页/块 × 块/单元 × 逻辑单元。
     * 先加宽到 QWORD 再乘：32-bit 中间积会在大容量器件上回绕（H3）。 */
    const DWORD capacity_mib = (DWORD)(((QWORD)info->data_bytes_per_page * info->pages_per_block * info->blocks_per_unit * info->logical_units) / (1024ULL * 1024ULL));

    LOGI(LOG_TAG_NAND "ONFI sig=%c%c%c%c rev=0x%04hX model=%s\r\n", info->signature[0], info->signature[1], info->signature[2], info->signature[3], info->revision, info->model);
    LOGI(LOG_TAG_NAND "ONFI %luMiB page=%lu+%hu partial=%lu+%hu block=%lup blocks=%lu lun=%hhu addrcyc=0x%02hhX(C%hhu/R%hhu) bitcell=%hhu\r\n", capacity_mib, info->data_bytes_per_page, info->spare_bytes_per_page, info->data_bytes_per_partial_page, info->spare_bytes_per_partial_page, info->pages_per_block, info->blocks_per_unit, info->logical_units, info->address_cycles, info->column_cycles, info->row_cycles, info->bits_per_cell);
    LOGI(LOG_TAG_NAND "ONFI ecc=%hhubit prog/pg=%hhu tprog=%hu terase=%hu tr=%hu tccs=%hu timingmodes=0x%04hX\r\n", info->ecc_bits, info->programs_per_page, info->tprog_max, info->tbers_max, info->tr_max, info->tccs_min, info->timing_mode_support);
}

/**
 * @brief READ ID 是否为无器件（全 00 / 全 FF）
 *
 * @param id 5 字节 ID
 * @return 1 无器件；0 有器件
 */
static BYTE fc_nand_id_is_blank(const BYTE id[5])
{
    BYTE all_00 = 1U;
    BYTE all_ff = 1U;

    for (BYTE i = 0U; i < 5U; i++)
    {
        if (id[i] != 0x00U)
        {
            all_00 = 0U;
        }
        if (id[i] != 0xFFU)
        {
            all_ff = 0U;
        }
    }
    return ((all_00 != 0U) || (all_ff != 0U)) ? 1U : 0U;
}

/**
 * @brief 单 target 探测：选择通路 → RESET → READ_ID → ONFI 参数页 → 逐 LUN 78h
 *
 * @param ch   目标 ch
 * @param ce   目标 ce
 * @param info 输出：target 信息
 * @return OK / NOT_READY 无器件 / INVALID_ARGUMENT / OPERATION_FAILED
 */
static nand_result_e fc_nand_target_probe(BYTE ch, BYTE ce, nand_target_info_s *info)
{
    const nand_device_desc_s *dev = nand_device_get();
    nand_cmd_comp_s           completion;
    BYTE                      id[5];
    nand_result_e             result;
    BYTE                      lun_count;

    ASSERT(dev != NULL);
    ASSERT(info != NULL);
    memset(info, 0, sizeof(*info));

    delay_ms(1U);
    result = fc_nand_reset(ch, ce, &completion);
    if (result == NAND_RESULT_INVALID_ARGUMENT)
    {
        return result;
    }
    if (result != NAND_RESULT_OK)
    {
        return NAND_RESULT_NOT_READY;
    }

    result = fc_nand_read_id(ch, ce, id, &completion);
    if (result != NAND_RESULT_OK)
    {
        return NAND_RESULT_NOT_READY;
    }
    if (fc_nand_id_is_blank(id) != 0U)
    {
        return NAND_RESULT_NOT_READY;
    }
    LOGI(LOG_TAG_NAND "ID=%02hhX %02hhX %02hhX %02hhX %02hhX\r\n", id[0], id[1], id[2], id[3], id[4]);

    const DWORD id_value = ((DWORD)id[1] << 24U) | ((DWORD)id[2] << 16U) | ((DWORD)id[3] << 8U) | id[4];

    if (id_value != dev->expected_id)
    {
        LOGE(LOG_TAG_NAND "unexpected ID\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }

    {
        BYTE                   page[NAND_ONFI_PARAM_PAGE_SIZE];
        nand_onfi_param_info_s onfi;

        result = fc_nand_read_parameter_page(ch, ce, page, &completion);
        if (result != NAND_RESULT_OK)
        {
            LOGE(LOG_TAG_NAND "read parameter page failed, result=0x%02hhX status=0x%02hhX\r\n", result, completion.nand_status);
            return NAND_RESULT_OPERATION_FAILED;
        }
        if (nand_onfi_parameter_page_decode(page, &onfi) == 0U)
        {
            LOGE(LOG_TAG_NAND "parameter page signature mismatch (got %02hhX %02hhX %02hhX %02hhX)\r\n", page[0], page[1], page[2], page[3]);
            return NAND_RESULT_OPERATION_FAILED;
        }

        fc_nand_onfi_param_print(&onfi);
        if ((onfi.column_cycles != NAND_COLUMN_ADDR_CYCLES) || (onfi.row_cycles != NAND_ROW_ADDR_CYCLES))
        {
            LOGE(LOG_TAG_NAND "addr cycle mismatch: device C%hhu/R%hhu, driver C%u/R%u\r\n", onfi.column_cycles, onfi.row_cycles, NAND_COLUMN_ADDR_CYCLES, NAND_ROW_ADDR_CYCLES);
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        if ((onfi.logical_units == 0U) || (onfi.logical_units > LUN_NUM))
        {
            LOGE(LOG_TAG_NAND "ONFI logical_units=%hhu out of range (LUN_NUM=%u)\r\n", onfi.logical_units, LUN_NUM);
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        lun_count = onfi.logical_units;
    }

    for (BYTE lun = 0U; lun < lun_count; lun++)
    {
        nand_addr_s addr;
        DWORD       row;
        BYTE        status;

        memset(&addr, 0, sizeof(addr));
        addr.lun = lun;
        row      = paa_to_nand_row(paa_from_addr(&addr));

        result = fc_nand_read_status_enhanced(ch, ce, row, &completion);
        if (result != NAND_RESULT_OK)
        {
            LOGE(LOG_TAG_NAND "lun %hhu status probe failed, result=0x%02hhX\r\n", lun, result);
            return NAND_RESULT_OPERATION_FAILED;
        }

        status                 = completion.nand_status;
        info->lun_present[lun] = (status == NAND_STATUS_NO_RESPONSE) ? 0U : 1U;
        LOGI(LOG_TAG_NAND "lun=%hhu status=0x%02hhX present=%hhu\r\n", lun, status, info->lun_present[lun]);
    }

    if (info->lun_present[0] == 0U)
    {
        LOGE(LOG_TAG_NAND "target ch=%hhu ce=%hhu: LUN0 no response\r\n", ch, ce);
        return NAND_RESULT_NOT_READY;
    }

    info->present   = 1U;
    info->lun_count = lun_count;
    return NAND_RESULT_OK;
}

/**
 * @brief 遍历 CH_NUM × CE_NUM 探测 target
 *
 * @return OK / NOT_READY 无 target / 错误码
 */
static nand_result_e fc_nand_scan_targets(void)
{
    BYTE present = 0U;

    memset(g_nand_targets, 0, sizeof(g_nand_targets));

    for (BYTE ch = 0U; ch < CH_NUM; ch++)
    {
        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            nand_target_info_s  info;
            const nand_result_e result = fc_nand_target_probe(ch, ce, &info);

            if (result == NAND_RESULT_OK)
            {
                g_nand_targets[ch][ce] = info;
                present                = 1U;
                LOGI(LOG_TAG_NAND "scan ch=%hhu ce=%hhu present luns=%hhu\r\n", ch, ce, info.lun_count);
            }
            else if (result != NAND_RESULT_NOT_READY)
            {
                LOGE(LOG_TAG_NAND "scan ch=%hhu ce=%hhu failed 0x%02hhX\r\n", ch, ce, result);
                return result;
            }
        }
    }

    if (present == 0U)
    {
        LOGE(LOG_TAG_NAND "scan: no target present\r\n");
        return NAND_RESULT_NOT_READY;
    }
    return NAND_RESULT_OK;
}

/**
 * @brief FC 侧 NAND 初始化：器件硬件初始化 + 拓扑扫描
 *
 * @return OK / 错误码
 */
nand_result_e fc_nand_init(void)
{
    const nand_device_desc_s *dev = nand_device_get();

    ASSERT(dev != NULL);
    ASSERT(dev->hw_init != NULL);

    LOGI(LOG_TAG_NAND "device=%s\r\n", dev->name);

    nand_result_e result = dev->hw_init();

    if (result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "device hw init failed: 0x%02hhX\r\n", result);
        return result;
    }

    result = fc_nand_scan_targets();
    if (result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "target scan failed: 0x%02hhX\r\n", result);
        return result;
    }

    LOGI(LOG_TAG_NAND "init complete (low speed)\r\n");
    return NAND_RESULT_OK;
}

/**
 * @brief boot 完成后切到 CONFIG_NAND_TIMING_MODE 并更新 FMC 时序
 *
 * armed 已置位，用 fc_nand_start/step 推进 SET_FEATURES；失败保持低速。
 *
 * @param fmc_hz FMC 内核时钟
 * @return OK / 错误码
 */
static nand_result_e fc_nand_enable_high_speed_target(BYTE ch, BYTE ce, DWORD fmc_hz)
{
#if (CONFIG_NAND_TIMING_MODE != 0U)
    const BYTE                mode[4] = {(BYTE)CONFIG_NAND_TIMING_MODE, 0U, 0U, 0U};
    nand_cmd_req_s            request;
    nand_cmd_comp_s           completion;
    fc_nand_txn_s             transaction;
    nand_result_e             result;
    const nand_device_desc_s *dev     = nand_device_get();
    const nand_phy_backend_s *backend = NULL;

    ASSERT(dev != NULL);
    ASSERT(dev->set_timing != NULL);

    if (fmc_hz == 0U)
    {
        LOGE(LOG_TAG_NAND "FMC clock unknown, fallback to 200MHz\r\n");
        fmc_hz = 200000000U;
    }

    memset(&request, 0, sizeof(request));
    request.column      = 0x01U; /* ONFI timing mode feature address */
    request.buffer      = (BYTE *)mode;
    request.data_length = 4U;

    memset(&completion, 0, sizeof(completion));
    result = nand_target_get(ch, ce, &backend);
    if (result != NAND_RESULT_OK)
    {
        return result;
    }

    memset(&transaction, 0, sizeof(transaction));
    transaction.engine.backend    = backend;
    transaction.engine.request    = &transaction.request;
    transaction.engine.completion = &transaction.completion;

    result = fc_nand_start(&transaction, OP_SET_FEATURES, &request);
    while (result == NAND_RESULT_NOT_READY)
    {
        result = fc_nand_step(&transaction, &completion);
    }
    if (result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "set feature 01h (Mode %u) failed, result=0x%02hhX status=0x%02hhX\r\n", CONFIG_NAND_TIMING_MODE, result, transaction.completion.nand_status);
        return result;
    }

    result = dev->set_timing(ch, ce, (BYTE)CONFIG_NAND_TIMING_MODE, fmc_hz);
    if (result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "set timing Mode %u failed: 0x%02hhX\r\n", CONFIG_NAND_TIMING_MODE, result);
        return result;
    }

    LOGI(LOG_TAG_NAND "switched NAND to ONFI Mode %u\r\n", CONFIG_NAND_TIMING_MODE);
    return NAND_RESULT_OK;
#else
    LOGI(LOG_TAG_NAND "high-speed mode disabled (CONFIG_NAND_TIMING_MODE=0)\r\n");
    return NAND_RESULT_OK;
#endif /* CONFIG_NAND_TIMING_MODE */
}

nand_result_e fc_nand_enable_high_speed(DWORD fmc_hz)
{
    nand_result_e last = NAND_RESULT_OK;

    for (BYTE ch = 0U; ch < CH_NUM; ch++)
    {
        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            if (g_nand_targets[ch][ce].present == 0U)
            {
                continue;
            }
            last = fc_nand_enable_high_speed_target(ch, ce, fmc_hz);
            if (last != NAND_RESULT_OK)
            {
                return last;
            }
        }
    }
    return last;
}
