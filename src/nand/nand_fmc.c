/*
 * nand_fmc.c — STM32H743 FMC 控制器 NAND 后端
 *
 * 职责：将 fc 的 UOP 引擎 发出的 UOP 微指令映射为 FMC NAND Bank3 的
 *       命令/地址/数据窗口访问；数据阶段固定走 PIO（FMC NAND 无 DMA 请求线）。
 *
 * 本文件是 fc 的 UOP 引擎 的一个 backend 实现：
 *   fc_uop_engine_step() → g_nand_fmc_handlers[uop->type]() → FMC 窗口写
 *
 * 数据流（读/写）：
 *   fc_nand_* (src/fc/fc_nand_ops.c)
 *     → fc 的 UOP 引擎 (uop 解释器)
 *     → 本文件 handler（命令/地址/数据全部走 PIO 窗口）
 *
 * 涉及硬件外设：
 *   - FMC（Flexible Memory Controller）NAND Bank3 内存映射窗口
 *   - RB# GPIO（GPIOD PIN6，busy/ready 指示）
 *
 * 时序来源：nand_fmc_timing_t（MT29 数据手册参数，见 nand_fmc.h / devices/mt29f___g08a.c）。
 */

#include "nand_fmc.h"

#include "assertion.h"
#include "delay.h"
#include "log.h"
#include "nand_debug.h"
#include "nand_device.h"
#include "onfi_1_0.h"

#include <stddef.h>

/* --------------------------------------------------------------------
 * g_nand_fmc_handlers — UOP → handler 调度表（定义在本文件末尾）
 *   下标为 NAND_UOP_* 枚举；设计初始化器，与枚举顺序解耦。
 *   [NAND_UOP_JUMP_IF_NOT_READY] = NULL：该微指令由 fc 的 UOP 引擎 直接
 *   处理（条件跳转不经过 FMC 后端），此处刻意留空；若误以为漏实现
 *   而补 handler，会导致引擎两条跳转路径并存。
 *   新增 NAND_UOP_* 时须同步本表长度（NAND_UOP_COUNT）。
 *   表定义放在各 handler 之后：handler 定义即声明，省掉 12 条长签名
 *   前向声明，也就无需 __attribute__((used))。
 * -------------------------------------------------------------------- */
static const nand_uop_handler_t g_nand_fmc_handlers[NAND_UOP_COUNT];

/* ── FMC 后端实例（单例）──
 * 数据/命令/地址窗口 + 时序对全部 LUN 一致：FMC 时序寄存器（PMEM/PATT）
 * 是 bank 级，同 bank 各 LUN 共享窗口，硬件层面无法按 LUN 分化。
 * backend 只读共享——handlers 是 const 表，context 指向本实例 config；
 * 每事务可变状态在 fc_uop_engine_state_t，不在 backend，故多 LUN 并发安全。 */
static nand_fmc_instance_t g_nand_fmc_instance;

/**
 * @brief 取 FMC 后端单例
 *
 * 同步路径与 fc_nand_dispatch_task_load() 共用同一后端（单 bank 拓扑下与坐标无关）。
 *
 * @return 后端接口（handlers 为 const 表，context 指向实例内 config）
 */
const nand_phy_backend_t *nand_fmc_backend_get(void)
{
    return &g_nand_fmc_instance.backend;
}

const nand_phy_backend_t *nand_phy_backend_get(void)
{
    return nand_fmc_backend_get();
}
/**
 * @brief 装配 FMC 后端单例
 *
 * @param config FMC 窗口指针 + 时序（按值复制进实例，故生命周期只需到调用结束）
 * @return NAND_RESULT_OK
 */
nand_result_e nand_fmc_init(const nand_fmc_config_t *config)
{
    /* 前置条件由调用者保证（mt29f_hw_init 传入模块内 const config） */
    ASSERT(config != NULL);

    g_nand_fmc_instance.config           = *config;
    g_nand_fmc_instance.backend.handlers = g_nand_fmc_handlers;
    g_nand_fmc_instance.backend.context  = &g_nand_fmc_instance.config; /* 指向实例内持久副本，handler 据此转换合法 */
    return NAND_RESULT_OK;
}

/**
 * @brief 轮询 RB# 等待 NAND ready（WAIT_READY）
 *
 * tWB 已由上游 DELAY_US 保证，此处只做纯 spin-wait；首读即 HIGH 视为 ready。
 *
 * @param timing 取其 busy_timeout_us 作超时（轮询计次兼作微秒）
 * @return NAND_RESULT_OK / NAND_RESULT_TIMEOUT
 */
static nand_result_e nand_fmc_exec_wait_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)request;
    (void)completion;

    /*
     * RB# GPIO 轮询：NAND 已由前面的 DELAY_US 保证过 tWB 窗口，
     * handler 不再包含 tWB 保护，仅做纯 spin-wait。
     *
     * 如果第一次读到 RB#=HIGH，说明 NAND 已经 ready 或根本没 busy，
     * 直接返回 OK（极短 busy 或无 busy 均视为 ready）。
     */
    if (HAL_GPIO_ReadPin(NAND_RB_GPIO_PORT, NAND_RB_GPIO_PIN) == 0U)
    {
        /* RB# LOW → NAND busy，spin-wait 直到 ready */
        DWORD polls = 0U;
        FMC_LOGD("WAIT_READY start op=%hhu\r\n", request->operation);
        while (HAL_GPIO_ReadPin(NAND_RB_GPIO_PORT, NAND_RB_GPIO_PIN) == 0U)
        {
            polls++;
            if (polls >= timing->busy_timeout_us)
            {
                /* 轮询计数兼作超时微秒（每次迭代 delay_us(1)） */
                return NAND_RESULT_TIMEOUT;
            }
            delay_us(1U);
        }
        FMC_LOGD("WAIT_READY done op=%hhu polls=%lu\r\n", request->operation, polls);
        return NAND_RESULT_OK;
    }

    /* RB# HIGH → NAND 已 ready（极短的 busy 或无 busy） */
    FMC_LOGD("WAIT_READY immediate op=%hhu\r\n", request->operation);
    return NAND_RESULT_OK;
}

/**
 * @brief 写命令字节（COMMAND）
 *
 * @param uop operand 即命令字节
 * @return 恒 NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_command(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)request;
    (void)timing;
    (void)completion;
    *config->command = uop->operand;
    return NAND_RESULT_OK;
}

/**
 * @brief 写列地址低字节（COLUMN_0）
 *
 * ONFI 5 地址周期：列地址 2 字节，本微指令写低字节。
 *
 * @return 恒 NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_column_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = request->column;
    return NAND_RESULT_OK;
}

/**
 * @brief 写列地址高字节（COLUMN_1）
 *
 * @return 恒 NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_column_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (request->column >> 8U);
    return NAND_RESULT_OK;
}

/**
 * @brief 写行地址低字节（ROW_0）
 *
 * @param uop operand 作 row[] 的 plane 下标（multi-plane 时两个 plane 分两段写）
 * @return 恒 NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_row_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)timing;
    (void)completion;
    *config->address = request->row[uop->operand];
    return NAND_RESULT_OK;
}

/**
 * @brief 写行地址中间字节（ROW_1，第 8~15 位）
 *
 * @param uop operand 作 row[] 的 plane 下标
 * @return 恒 NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_row_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)timing;
    (void)completion;
    *config->address = (request->row[uop->operand] >> 8U);
    return NAND_RESULT_OK;
}

/**
 * @brief 写行地址高字节（ROW_2，第 16~23 位）
 *
 * @param uop operand 作 row[] 的 plane 下标
 * @return 恒 NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_row_2(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)timing;
    (void)completion;
    *config->address = (request->row[uop->operand] >> 16U);
    return NAND_RESULT_OK;
}

/**
 * @brief 向 NAND 写数据（DATA_IN）
 *
 * PIO 每字节后 __DMB()，因为 volatile 不保证连续写 FMC 数据窗口的顺序。
 *
 * @param uop operand 作 buffer 偏移（multi-plane program 第二 plane）
 * @return NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_data_in(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    WORD                     index;

    (void)timing;

    /* 请求由 fc_nand_dispatch_tables.c 的 req_* 构建，DATA_IN/OUT 必带缓冲与长度 */
    ASSERT(request->buffer != NULL);
    ASSERT(request->data_length != 0U);

    /* uop->operand 作为 buffer 偏移量（用于 Multi-Plane Program 第二 Plane） */
    const BYTE *src = request->buffer + uop->operand;

    /* PIO：FMC NAND 无 DMA 请求线，数据阶段逐字节写数据窗口。
     * volatile 不保证连续写顺序，必须逐字节 DMB（H16）。 */
    for (index = 0U; index < request->data_length; index++)
    {
        *config->data = src[index];
        __DMB(); /* 数据屏障：保证对 FMC 数据寄存器的连续写不被重排 */
    }

    completion->transfer_status = NAND_RESULT_OK;
    return NAND_RESULT_OK;
}

/**
 * @brief 从 NAND 读数据（DATA_OUT）
 *
 * 与 DATA_IN 对称，逐字节从 FMC 数据窗口读入缓冲。
 *
 * @param uop operand 作 buffer 偏移（multi-plane read 第二 plane）
 * @return NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_data_out(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    WORD                     index;

    (void)timing;

    /* 请求由 fc_nand_dispatch_tables.c 的 req_* 构建，DATA_IN/OUT 必带缓冲与长度 */
    ASSERT(request->buffer != NULL);
    ASSERT(request->data_length != 0U);

    /* uop->operand 作为 buffer 偏移量（用于 Multi-Plane Read 的第二 Plane） */
    BYTE *dst = request->buffer + uop->operand;

    /* PIO：逐字节从 FMC 数据窗口读入缓冲。 */
    for (index = 0U; index < request->data_length; index++)
    {
        dst[index] = *config->data;
    }

    if (request->operation == OP_READ_ID)
    {
        /* DEBUG 档，默认 INFO 档不打印。 */
        LOGD(LOG_TAG_NAND "READ_ID data=%02X %02X %02X %02X %02X buffer=%p\r\n", request->buffer[0], request->buffer[1], request->buffer[2], request->buffer[3], request->buffer[4], (void *)request->buffer);
    }

    completion->transfer_status = NAND_RESULT_OK;
    return NAND_RESULT_OK;
}

/**
 * @brief 读 70h 状态字节（DATA_OUT_STATUS）
 *
 * 直读 DQ 总线写入 completion->nand_status，不经 buffer。
 *
 * @return 恒 NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_data_out_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    BYTE                     status;

    (void)uop;
    (void)timing;
    (void)request;
    /* 直读 DQ 总线 → completion->nand_status（状态值通过 completion 链→CQ，不写 buffer） */
    status                  = *config->data;
    completion->nand_status = status;

    return NAND_RESULT_OK;
}

/**
 * @brief 检查 nand_status 的 FAIL 位（CHECK_FAIL）
 *
 * 只查 bit0 FAIL；READY 由 WAIT_READY / JUMP_IF_NOT_READY 保证。
 *
 * @return NAND_RESULT_OK / NAND_RESULT_OPERATION_FAILED
 */
static nand_result_e nand_fmc_exec_check_fail(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)request;
    (void)timing;
    return ((completion->nand_status & NAND_STATUS_FAIL) == 0U) ? NAND_RESULT_OK : NAND_RESULT_OPERATION_FAILED;
}

/**
 * @brief cache program 状态检查：FAIL 与上一页 FAILC 同查（CHECK_FAIL_CACHE）
 *
 * 只查 FAIL 会把中间页失败当成功，末页 10h 再确认映射 = 静默坏数据。
 * 首页（CACHE_FIRST）无本批上一页，FAILC 可能是上一批残留，故只查 FAIL。
 *
 * @return NAND_RESULT_OK / NAND_RESULT_OPERATION_FAILED
 */
static nand_result_e nand_fmc_exec_check_fail_cache(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)timing;

    /* 首页之前没有本批的 cache 页：FAILC 可能残留上一批结果，只查 FAIL。
     * 其余 cache 页必须同时查 FAILC（上一页 cache program 的成败）。 */
    const BYTE bad = (request->operation == OP_TWO_PLANE_PROGRAM_CACHE_FIRST) ? NAND_STATUS_FAIL : (BYTE)(NAND_STATUS_FAIL | NAND_STATUS_FAILC);

    return ((completion->nand_status & bad) == 0U) ? NAND_RESULT_OK : NAND_RESULT_OPERATION_FAILED;
}

/**
 * @brief 固定微秒延时（DELAY_US，用于 tWB / tWHR 等 ONFI 时序）
 *
 * @param uop operand 即延时微秒数
 * @return 恒 NAND_RESULT_OK
 */
static nand_result_e nand_fmc_exec_delay_us(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)request;
    (void)timing;
    (void)completion;
    delay_us(uop->operand);
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * g_nand_fmc_handlers — UOP → handler 调度表
 *   说明与注意事项见文件开头的前向声明处。
 * -------------------------------------------------------------------- */
static const nand_uop_handler_t g_nand_fmc_handlers[NAND_UOP_COUNT] = {
    [NAND_UOP_WAIT_READY]        = nand_fmc_exec_wait_ready,
    [NAND_UOP_COMMAND]           = nand_fmc_exec_command,
    [NAND_UOP_COLUMN_0]          = nand_fmc_exec_column_0,
    [NAND_UOP_COLUMN_1]          = nand_fmc_exec_column_1,
    [NAND_UOP_ROW_0]             = nand_fmc_exec_row_0,
    [NAND_UOP_ROW_1]             = nand_fmc_exec_row_1,
    [NAND_UOP_ROW_2]             = nand_fmc_exec_row_2,
    [NAND_UOP_DATA_IN]           = nand_fmc_exec_data_in,
    [NAND_UOP_DATA_OUT]          = nand_fmc_exec_data_out,
    [NAND_UOP_DATA_OUT_STATUS]   = nand_fmc_exec_data_out_status,
    [NAND_UOP_JUMP_IF_NOT_READY] = NULL,
    [NAND_UOP_CHECK_FAIL]        = nand_fmc_exec_check_fail,
    [NAND_UOP_CHECK_FAIL_CACHE]  = nand_fmc_exec_check_fail_cache,
    [NAND_UOP_DELAY_US]          = nand_fmc_exec_delay_us,
};
