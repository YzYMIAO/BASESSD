/*
 * nand_fmc.c — STM32H743 FMC 控制器 NAND 后端
 *
 * 职责：将 nand_engine 发出的 UOP 微指令映射为 FMC NAND Bank3 的
 *       命令/地址/数据窗口访问，并负责 DMA / PIO 两条数据搬移路径。
 *
 * 本文件是 nand_engine 的一个 backend 实现：
 *   nand_engine_step() → g_nand_fmc_handlers[uop->type]() → FMC 窗口写
 *
 * 数据流（读/写）：
 *   nand_fc_* (mt29f4g08abadawp.c)
 *     → nand_engine (uop 解释器)
 *     → 本文件 handler（命令/地址走 PIO 窗口，DATA_IN/OUT 走 DMA 或 PIO）
 *
 * 涉及硬件外设：
 *   - FMC（Flexible Memory Controller）NAND Bank3 内存映射窗口
 *   - RB# GPIO（GPIOD PIN6，busy/ready 指示）
 *   - FMC 专用 DMA 通道（DMA_REQUEST_FMC）
 *
 * 时序来源：nand_fmc_timing_t（MT29 数据手册参数，见 nand_fmc.h / nand_mt29*.c）。
 */

#include "nand_fmc.h"

#include "common/assertion.h"
#include "common/log.h"
#include "delay.h"
#include "nand/nand_debug.h"
#include "nand/nand_mt29f4g08abadawp.h"
#include "onfi_1_0.h"
#include "platform/stm32h743/nand_dma.h"

#include <stddef.h>

/* --------------------------------------------------------------------
 * g_nand_fmc_handlers — UOP → handler 调度表（定义在本文件末尾）
 *   下标为 NAND_UOP_* 枚举；设计初始化器，与枚举顺序解耦。
 *   [NAND_UOP_JUMP_IF_NOT_READY] = NULL：该微指令由 nand_engine 直接
 *   处理（条件跳转不经过 FMC 后端），此处刻意留空；若误以为漏实现
 *   而补 handler，会导致引擎两条跳转路径并存。
 *   新增 NAND_UOP_* 时须同步本表长度（NAND_UOP_COUNT）。
 *   表定义放在各 handler 之后：handler 定义即声明，省掉 12 条长签名
 *   前向声明，也就无需 __attribute__((used))。
 * -------------------------------------------------------------------- */
static const nand_uop_handler_t g_nand_fmc_handlers[NAND_UOP_COUNT];

/* ── Phase V: per-(CH,CE,LUN) 实例数组 ──
 * 每 (通道, 片选, LUN) 一个独立 FMC 后端实例（config + handler 表），
 * 供多 LUN 并发时各 LUN 独立持有自己的时序/上下文。
 * 当前拓扑 CH_NUM=CE_NUM=LUN_NUM=1，实际只用到 [0][0][0]。 */
nand_fmc_instance_t g_nand_fmc_instances[CH_NUM][CE_NUM][LUN_NUM];

/*
 * 向后兼容接口。
 * nand_fmc_configure() 等价于 nand_fmc_init_instance(0,0,0, config)。
 * nand_fmc_backend_get() 返回 (0,0,0) 实例的 backend。
 * nand_init() 已改用 nand_fmc_init_instance()，这些接口仅供未迁移的外部代码使用。
 */
nand_result_t nand_fmc_configure(const nand_fmc_config_t *config)
{
    return nand_fmc_init_instance(0U, 0U, 0U, config);
}

/* 返回 (0,0,0) 实例的 backend 指针（旧式单 LUN 访问入口） */
const nand_engine_backend_t *nand_fmc_backend_get(void)
{
    return &g_nand_fmc_instances[0][0][0].backend;
}

nand_result_t nand_fmc_init_instance(uint8_t ch, uint8_t ce, uint8_t lun, const nand_fmc_config_t *config)
{
    nand_fmc_instance_t *inst;

    if ((config == NULL) || (ch >= CH_NUM) || (ce >= CE_NUM) || (lun >= LUN_NUM))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    inst                   = &g_nand_fmc_instances[ch][ce][lun];
    inst->config           = *config;
    inst->backend.handlers = g_nand_fmc_handlers;
    inst->backend.context  = &inst->config; /* context 指向实例内 config 副本，handler 据此转换合法 */
    return NAND_RESULT_OK;
}

const nand_engine_backend_t *nand_fmc_backend_get_ext(uint8_t ch, uint8_t ce, uint8_t lun)
{
    if ((ch >= CH_NUM) || (ce >= CE_NUM) || (lun >= LUN_NUM))
    {
        return NULL;
    }
    return &g_nand_fmc_instances[ch][ce][lun].backend;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_wait_ready
 *   功能：轮询 RB# GPIO 等待 NAND busy 结束（WAIT_READY 微指令）。
 *   参数：context —— nand_fmc_config_t*（本 handler 不使用）；
 *         uop/request/completion —— 不使用。
 *   返回：NAND_RESULT_OK（ready）/ NAND_RESULT_TIMEOUT（超时）。
 *   实现要点：tWB 已由上游 DELAY_US 保证，此处只做纯 spin-wait；
 *         轮询次数兼作超时微秒（每次 delay_us(1)，polls≥busy_timeout_us
 *         即超时）；首读即 HIGH 视为 ready（极短 busy 或无 busy）。
 *   注：与 config 层的 nand_fmc_wait_ready 回调功能重叠（另一套轮询）。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_wait_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
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
    if (HAL_GPIO_ReadPin(MT29F4G08ABADAWP_RB_GPIO_PORT, MT29F4G08ABADAWP_RB_GPIO_PIN) == 0U)
    {
        /* RB# LOW → NAND busy，spin-wait 直到 ready */
        uint32_t polls = 0U;
        FMC_LOGD("WAIT_READY start op=%u\r\n", (unsigned)request->operation);
        while (HAL_GPIO_ReadPin(MT29F4G08ABADAWP_RB_GPIO_PORT, MT29F4G08ABADAWP_RB_GPIO_PIN) == 0U)
        {
            polls++;
            if (polls >= timing->busy_timeout_us)
            {
                /* 轮询计数兼作超时微秒（每次迭代 delay_us(1)） */
                return NAND_RESULT_TIMEOUT;
            }
            delay_us(1U);
        }
        FMC_LOGD("WAIT_READY done op=%u polls=%u\r\n", (unsigned)request->operation, (unsigned)polls);
        return NAND_RESULT_OK;
    }

    /* RB# HIGH → NAND 已 ready（极短的 busy 或无 busy） */
    FMC_LOGD("WAIT_READY immediate op=%u\r\n", (unsigned)request->operation);
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_command
 *   功能：写命令字节（COMMAND 微指令）。
 *   参数：context —— nand_fmc_config_t*，取其 command 窗口指针。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：uop->operand 即命令字节，写入 FMC 命令窗口
 *         （FMC_BASE|CMD_OFFSET，A16=1 选命令口）。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_command(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)request;
    (void)timing;
    (void)completion;
    *config->command = (uint8_t)uop->operand;
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_column_0
 *   功能：写列地址低字节（COLUMN_0 微指令）。
 *   参数：context —— nand_fmc_config_t*，取其 address 窗口指针。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：ONFI 5 地址周期规则——列地址占 2 字节，COLUMN_0 写低字节。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_column_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)request->column;
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_column_1
 *   功能：写列地址高字节（COLUMN_1 微指令）。
 *   参数：context —— nand_fmc_config_t*，取其 address 窗口指针。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：ONFI 5 地址周期规则——列地址占 2 字节，COLUMN_1 写高字节。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_column_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->column >> 8U);
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_row_0
 *   功能：写行地址低字节（ROW_0 微指令）。
 *   参数：context —— nand_fmc_config_t*，取其 address 窗口指针。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：uop->operand 作 request->row[] 的 plane 下标
 *         （Multi-Plane 时第 0/1 个 plane 的行地址分两个 ROW 段写）。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_row_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)timing;
    (void)completion;
    *config->address = (uint8_t)request->row[uop->operand];
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_row_1
 *   功能：写行地址中间字节（ROW_1 微指令）。
 *   参数：context —— nand_fmc_config_t*，取其 address 窗口指针。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：uop->operand 作 plane 下标；取第 8~15 位。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_row_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->row[uop->operand] >> 8U);
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_row_2
 *   功能：写行地址高字节（ROW_2 微指令）。
 *   参数：context —— nand_fmc_config_t*，取其 address 窗口指针。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：uop->operand 作 plane 下标；取第 16~23 位。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_row_2(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->row[uop->operand] >> 16U);
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_data_in
 *   功能：写数据到 NAND（Program 数据阶段，DATA_IN 微指令）。
 *   参数：context —— nand_fmc_config_t*；request —— 源缓冲 + 长度。
 *   返回：NAND_RESULT_OK / NOT_READY（DMA 忙，稍后重试）/
 *         INVALID_ARGUMENT（buffer 空）。
 *   实现要点：
 *     - uop->operand 作 buffer 偏移（Multi-Plane Program 第二 plane 偏移）。
 *     - DMA 路径三态：查完成→取走；查忙→NOT_READY；首次→启动 DMA。
 *       DMA 启动失败回退 PIO（abort + fall through）。
 *     - PIO 路径逐字节写 FMC 数据窗口，每字节后 __DMB() 防重排
 *       （volatile 不保证连续写顺序，写缓冲/DMA 下必须 DMB）。
 *     - 非阻塞协议：本函数可被多次调用（引擎 step 循环驱动）。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_data_in(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    uint16_t                 index;
    uint8_t                  dma_ok;

    (void)timing;

    /* 请求由 nand_dispatch_tables.c 的 req_* 构建，DATA_IN/OUT 必带缓冲与长度 */
    ASSERT(request->buffer != NULL);
    ASSERT(request->data_length != 0U);

    /* uop->operand 作为 buffer 偏移量（用于 Multi-Plane Program 第二 Plane） */
    const uint8_t *src = request->buffer + uop->operand;

    /* ── DMA 路径（非阻塞中断模式） ── */
    if (g_nand_dma_enable != 0U)
    {
        /* 先查完成标志 */
        if (nand_dma_data_out_done() != 0U)
        {
            /* DMA 传输在之前 step 中已完成 */
            completion->transfer_status = NAND_RESULT_OK;
            return NAND_RESULT_OK;
        }

        /* 查 DMA 是否还在跑 */
        if (nand_dma_is_out_busy() != 0U)
        {
            return NAND_RESULT_NOT_READY;
        }

        /* 首次进入：启动 DMA（memory → FMC data reg） */
        dma_ok = nand_dma_memory_to_fmc_start_IT(config->data,
                                                 src,
                                                 request->data_length);
        if (dma_ok == 0U)
        {
            return NAND_RESULT_NOT_READY; /* DMA 启动中 */
        }

        /* DMA 启动失败，回退 PIO */
        nand_dma_abort_out();
        /* fall through to PIO */
    }

    /* ── PIO 路径（默认/回退） ── */
    for (index = 0U; index < request->data_length; index++)
    {
        *config->data = src[index];
        __DMB(); /* 数据屏障：保证对 FMC 数据寄存器的连续写不被重排 */
    }

    completion->transfer_status = NAND_RESULT_OK;
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_data_out
 *   功能：从 NAND 读数据（DATA_OUT 微指令）。
 *   参数：context —— nand_fmc_config_t*；request —— 目标缓冲 + 长度。
 *   返回：NAND_RESULT_OK / NOT_READY（DMA 忙）/ INVALID_ARGUMENT。
 *   实现要点：与 nand_fmc_exec_data_in 对称——
 *     - uop->operand 作 buffer 偏移（Multi-Plane Read 第二 Plane 偏移）。
 *     - DMA 完成后必须 Invalidate D-Cache，否则 CPU 读到脏缓存
 *       （STM32H7 D-Cache 一致性，关键点）。
 *     - DMA 启动失败回退 PIO。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_data_out(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    uint16_t                 index;
    uint8_t                  dma_ok;

    (void)timing;

    /* 请求由 nand_dispatch_tables.c 的 req_* 构建，DATA_IN/OUT 必带缓冲与长度 */
    ASSERT(request->buffer != NULL);
    ASSERT(request->data_length != 0U);

    /* uop->operand 作为 buffer 偏移量（用于 Multi-Plane Read 的第二 Plane） */
    uint8_t *dst = request->buffer + uop->operand;

    /* ── DMA 路径（非阻塞中断模式） ── */
    if (g_nand_dma_enable != 0U)
    {
        /* 查完成标志 */
        if (nand_dma_data_in_done() != 0U)
        {
            /* DMA 完成：Invalidate D-Cache 后取数据（CPU 才能看到 DMA 写入） */
            nand_dma_cache_invalidate(dst, request->data_length);
            if (request->operation == OP_READ_ID)
            {
                LOGI("nand: READ_ID data=%02X %02X %02X %02X %02X buffer=%p\r\n",
                     request->buffer[0],
                     request->buffer[1],
                     request->buffer[2],
                     request->buffer[3],
                     request->buffer[4],
                     (void *)request->buffer);
            }
            completion->transfer_status = NAND_RESULT_OK;
            return NAND_RESULT_OK;
        }

        /* 查 DMA 是否还在跑 */
        if (nand_dma_is_in_busy() != 0U)
        {
            return NAND_RESULT_NOT_READY;
        }

        /* 首次进入：启动 DMA（FMC data reg → memory） */
        dma_ok = nand_dma_fmc_to_memory_start_IT(config->data,
                                                 dst,
                                                 request->data_length);
        if (dma_ok == 0U)
        {
            return NAND_RESULT_NOT_READY;
        }

        /* DMA 启动失败，回退 PIO */
        nand_dma_abort_in();
        /* fall through to PIO */
    }

    /* ── PIO 路径（默认/回退） ── */
    for (index = 0U; index < request->data_length; index++)
    {
        dst[index] = *config->data;
    }

    if (request->operation == OP_READ_ID)
    {
        LOGI("nand: READ_ID data=%02X %02X %02X %02X %02X buffer=%p\r\n",
             request->buffer[0],
             request->buffer[1],
             request->buffer[2],
             request->buffer[3],
             request->buffer[4],
             (void *)request->buffer);
    }

    completion->transfer_status = NAND_RESULT_OK;
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_data_out_status
 *   功能：读 70h 状态字节（DATA_OUT_STATUS 微指令）。
 *   参数：context —— nand_fmc_config_t*；completion —— 回填 nand_status。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：直读 DQ 总线一次即得状态字节，不经 buffer；
 *         状态经 completion 链回传上层。状态位含义：
 *         bit6 = READY、bit0 = FAIL。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_data_out_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    uint8_t                  status;

    (void)uop;
    (void)timing;
    (void)request;
    /* 直读 DQ 总线 → completion->nand_status（状态值通过 completion 链→CQ，不写 buffer） */
    status                  = *config->data;
    completion->nand_status = status;

    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_check_fail
 *   功能：检查上一步读到的状态字节 FAIL 位（CHECK_FAIL 微指令）。
 *   参数：completion —— 含 nand_status（上一步 DATA_OUT_STATUS 填入）。
 *   返回：NAND_RESULT_OK（无 FAIL）/ NAND_RESULT_OPERATION_FAILED。
 *   实现要点：只查 FAIL 位（bit0），不查 READY 位——READY 由
 *         JUMP_IF_NOT_READY / WAIT_READY 阶段保证。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_check_fail(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)request;
    (void)timing;
    return ((completion->nand_status & NAND_STATUS_FAIL) == 0U) ? NAND_RESULT_OK : NAND_RESULT_OPERATION_FAILED;
}

/* --------------------------------------------------------------------
 * nand_fmc_exec_delay_us
 *   功能：固定微秒延时（DELAY_US 微指令）。
 *   参数：uop —— operand 即延时微秒数。
 *   返回：恒 NAND_RESULT_OK。
 *   实现要点：对应 ONFI 时序参数（tWB/tWHR 等）的软件延时。
 * -------------------------------------------------------------------- */
static nand_result_t nand_fmc_exec_delay_us(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
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
static const nand_uop_handler_t g_nand_fmc_handlers[NAND_UOP_COUNT] =
    {
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
        [NAND_UOP_DELAY_US]          = nand_fmc_exec_delay_us,
};
