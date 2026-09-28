#include "nand_fmc.h"

#include "common/ssd_log.h"
#include "delay.h"
#include "nand/nand_debug.h"
#include "nand/nand_mt29f4g08abadawp.h"
#include "onfi_1_0.h"
#include "platform/stm32h743/nand_dma.h"

#include <stddef.h>

static __attribute__((used)) nand_result_t nand_fmc_exec_wait_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_command(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_column_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_column_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_row_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_row_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_row_2(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_data_in(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_data_out(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_data_out_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_check_fail(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static __attribute__((used)) nand_result_t nand_fmc_exec_delay_us(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);

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

/* ── Phase V: per-(CH,CE,LUN) 实例数组 ── */
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
    inst->backend.context  = &inst->config;
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

static nand_result_t nand_fmc_exec_command(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)request;
    (void)timing;
    (void)completion;
    *config->command = (uint8_t)uop->operand;
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_column_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)request->column;
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_column_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->column >> 8U);
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_row_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)request->row[uop->operand];
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_row_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->row[uop->operand] >> 8U);
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_row_2(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->row[uop->operand] >> 16U);
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_data_in(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    uint16_t                 index;
    uint8_t                  dma_ok;

    (void)timing;

    if ((request->buffer == NULL) || (request->data_length == 0U))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    /* uop->operand 作为 buffer 偏移量（用于 Multi-Plane Program） */
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
        __DMB();
    }

    completion->transfer_status = NAND_RESULT_OK;
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_data_out(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    uint16_t                 index;
    uint8_t                  dma_ok;

    (void)timing;

    if ((request->buffer == NULL) || (request->data_length == 0U))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    /* uop->operand 作为 buffer 偏移量（用于 Multi-Plane Read 的第二 Plane） */
    uint8_t *dst = request->buffer + uop->operand;

    /* ── DMA 路径（非阻塞中断模式） ── */
    if (g_nand_dma_enable != 0U)
    {
        /* 查完成标志 */
        if (nand_dma_data_in_done() != 0U)
        {
            /* DMA 完成：Invalidate D-Cache 后取数据 */
            nand_dma_cache_invalidate(dst, request->data_length);
            if (request->operation == OP_READ_ID)
            {
                SSD_LOGI("nand: READ_ID data=%02X %02X %02X %02X %02X buffer=%p\r\n",
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
        SSD_LOGI("nand: READ_ID data=%02X %02X %02X %02X %02X buffer=%p\r\n",
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

static nand_result_t nand_fmc_exec_check_fail(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)request;
    (void)timing;
    return ((completion->nand_status & NAND_STATUS_FAIL) == 0U) ? NAND_RESULT_OK : NAND_RESULT_OPERATION_FAILED;
}

static nand_result_t nand_fmc_exec_delay_us(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)request;
    (void)timing;
    (void)completion;
    delay_us(uop->operand);
    return NAND_RESULT_OK;
}