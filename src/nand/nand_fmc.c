#include "nand_fmc.h"

#include "common/ssd_log.h"
#include "delay.h"
#include "onfi_1_0.h"
#include "platform/stm32h743/nand_dma.h"
#include "platform/stm32h743/nand_rb_interrupt.h"

#include <stddef.h>

static nand_fmc_config_t g_nand_fmc_config;

static nand_result_t nand_fmc_exec_wait_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_command(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_command_primary(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_command_secondary(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_column_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_column_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_row_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_row_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_row_2(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_data_in(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_data_out(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_data_out_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_read_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_check_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static nand_result_t nand_fmc_exec_check_fail(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);

static const nand_uop_handler_t g_nand_fmc_handlers[NAND_UOP_COUNT] =
    {
        [NAND_UOP_WAIT_READY]        = nand_fmc_exec_wait_ready,
        [NAND_UOP_COMMAND]           = nand_fmc_exec_command,
        [NAND_UOP_COMMAND_PRIMARY]   = nand_fmc_exec_command_primary,
        [NAND_UOP_COMMAND_SECONDARY] = nand_fmc_exec_command_secondary,
        [NAND_UOP_COLUMN_0]          = nand_fmc_exec_column_0,
        [NAND_UOP_COLUMN_1]          = nand_fmc_exec_column_1,
        [NAND_UOP_ROW_0]             = nand_fmc_exec_row_0,
        [NAND_UOP_ROW_1]             = nand_fmc_exec_row_1,
        [NAND_UOP_ROW_2]             = nand_fmc_exec_row_2,
        [NAND_UOP_DATA_IN]           = nand_fmc_exec_data_in,
        [NAND_UOP_DATA_OUT]          = nand_fmc_exec_data_out,
        [NAND_UOP_DATA_OUT_STATUS]   = nand_fmc_exec_data_out_status,
        [NAND_UOP_READ_STATUS]       = nand_fmc_exec_read_status,
        [NAND_UOP_CHECK_READY]       = nand_fmc_exec_check_ready,
        [NAND_UOP_CHECK_FAIL]        = nand_fmc_exec_check_fail,
};

static const nand_engine_backend_t g_nand_fmc_backend =
    {
        .handlers = g_nand_fmc_handlers,
        .context  = &g_nand_fmc_config,
};

nand_result_t nand_fmc_configure(const nand_fmc_config_t *config)
{
    if ((config == NULL) || (config->data == NULL) || (config->command == NULL) || (config->address == NULL) || (config->wait_ready == NULL))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    g_nand_fmc_config = *config;
    return NAND_RESULT_OK;
}

const nand_engine_backend_t *nand_fmc_backend_get(void)
{
    return &g_nand_fmc_backend;
}

static nand_result_t nand_fmc_exec_wait_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)request;

    if (nand_rb_interrupt_armed() == 0U)
    {
        /* 首次进入 WAIT_READY: 配置 EXTI6 上升沿中断 + 超时 */
        SSD_LOGD("wait_ready op=%u FIRST arm timeout=%u\r\n",
                 (unsigned)request->operation, (unsigned)timing->busy_timeout_us);
        nand_rb_interrupt_arm(timing->busy_timeout_us);
        completion->nand_status = 0U;
        return NAND_RESULT_NOT_READY;
    }

    /* ── 中断已触发 → NAND 就绪 ── */
    if (nand_rb_interrupt_triggered() != 0U)
    {
        SSD_LOGD("wait_ready op=%u TRIGGERED -> OK\r\n", (unsigned)request->operation);
        nand_rb_interrupt_disarm();
        return NAND_RESULT_OK;
    }

    /* ── 超时 ── */
    if (nand_rb_interrupt_timed_out() != 0U)
    {
        SSD_LOGD("wait_ready op=%u TIMEOUT polling fallback\r\n", (unsigned)request->operation);
        nand_rb_interrupt_disarm();
        /* 回到轮询模式做最后一次确认（避免中断丢失） */
        return (config->wait_ready(config->context, 1U) == 0U) ? NAND_RESULT_OK : NAND_RESULT_TIMEOUT;
    }

    return NAND_RESULT_NOT_READY;
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

static nand_result_t nand_fmc_exec_command_primary(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    if (request->operation >= OP_COUNT)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    *config->command = g_nand_operation_map[request->operation].command_1;
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_command_secondary(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    if (request->operation >= OP_COUNT)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    *config->command = g_nand_operation_map[request->operation].command_2;
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

    (void)uop;
    (void)timing;

    if ((request->buffer == NULL) || (request->data_length == 0U))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    /*
     * 两步 DMA（非阻塞），与 WAIT_READY 一致：
     *   ① 首次进入 → 启动 DMA（中断模式）→ 返回 NOT_READY
     *   ② 后续 step → DMA 完成标志已置位 → 返回 OK
     *   DMA 启动失败 → 回退 PIO
     */
    if (nand_dma_data_out_done() == 0U)
    {
        if (nand_dma_memory_to_fmc_start_IT(config->data, request->buffer, request->data_length) == 0U)
        {
            return NAND_RESULT_NOT_READY;
        }
        /* DMA 启动失败，回退到 PIO */
    }
    else
    {
        /* DMA 传输完成，直接返回，不执行 PIO 回退 */
        completion->transfer_status = NAND_RESULT_OK;
        return NAND_RESULT_OK;
    }

    /* DMA 不可用 → PIO 退路 */
    {
        uint16_t index;
        for (index = 0U; index < request->data_length; index++)
        {
            *config->data = request->buffer[index];
        }
    }

    completion->transfer_status = NAND_RESULT_OK;
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_data_out(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;

    if ((request->buffer == NULL) || (request->data_length == 0U))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    /*
     * 两步 DMA（非阻塞），与 WAIT_READY / DATA_IN 一致：
     *   ① 首次进入 → 启动 DMA（中断模式）→ 返回 NOT_READY
     *   ② 后续 step → DMA 完成标志已置位 → Invalidate D-Cache → 返回 OK
     *   DMA 启动失败 → 回退 PIO
     */
    if (nand_dma_data_in_done() == 0U)
    {
        if (nand_dma_fmc_to_memory_start_IT(config->data, request->buffer, request->data_length) == 0U)
        {
            return NAND_RESULT_NOT_READY;
        }
        /* DMA 启动失败，回退到 PIO */
    }
    else
    {
        /* READ 方向：DMA 完成后 Invalidate D-Cache */
        nand_dma_cache_invalidate(request->buffer, request->data_length);

        if (request->operation == OP_READ_ID)
        {
            SSD_LOGI("nand: READ_ID data=%02X %02X %02X %02X %02X buffer=%p\r\n", request->buffer[0], request->buffer[1], request->buffer[2], request->buffer[3], request->buffer[4], (void *)request->buffer);
        }

        completion->transfer_status = NAND_RESULT_OK;
        return NAND_RESULT_OK;
    }

    /* DMA 不可用 → PIO 退路 */
    {
        uint16_t index;
        for (index = 0U; index < request->data_length; index++)
        {
            request->buffer[index] = *config->data;
        }
    }

    if (request->operation == OP_READ_ID)
    {
        SSD_LOGI("nand: READ_ID data=%02X %02X %02X %02X %02X buffer=%p\r\n", request->buffer[0], request->buffer[1], request->buffer[2], request->buffer[3], request->buffer[4], (void *)request->buffer);
    }

    completion->transfer_status = NAND_RESULT_OK;
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_data_out_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    uint8_t                  status;

    (void)uop;
    /* Command-primary has just issued 70h or 78h; wait until DQ drives status. */
    delay_us(timing->twh_us);
    status                  = *config->data;
    completion->nand_status = status;

    if ((request->buffer != NULL) && (request->data_length != 0U))
    {
        request->buffer[0]          = status;
        completion->transfer_status = NAND_RESULT_OK;
    }

    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_read_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)request;
    *config->command = NAND_CMD_READ_STATUS;
    /* tWHR is 60 ns minimum for this NAND. Use one full microsecond on the MCU. */
    delay_us(timing->twh_us);
    completion->nand_status = *config->data;
    return NAND_RESULT_OK;
}

static nand_result_t nand_fmc_exec_check_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)request;
    (void)timing;
    return ((completion->nand_status & NAND_STATUS_READY) != 0U) ? NAND_RESULT_OK : NAND_RESULT_NOT_READY;
}

static nand_result_t nand_fmc_exec_check_fail(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)request;
    (void)timing;
    return ((completion->nand_status & NAND_STATUS_FAIL) == 0U) ? NAND_RESULT_OK : NAND_RESULT_OPERATION_FAILED;
}
