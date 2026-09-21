#include "nand_fmc.h"

#include "common/ssd_log.h"
#include "delay.h"
#include "onfi_1_0.h"
#include "platform/stm32h743/nand_dma.h"

#include <stddef.h>

static nand_fmc_config_t g_nand_fmc_config;

static uint8_t nand_fmc_exec_wait_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_command(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_command_primary(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_command_secondary(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_column_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_column_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_row_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_row_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_row_2(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_data_in(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_data_out(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_data_out_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_read_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_check_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);
static uint8_t nand_fmc_exec_check_fail(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);

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

uint8_t nand_fmc_configure(const nand_fmc_config_t *config)
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

static uint8_t nand_fmc_exec_wait_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)request;
    (void)completion;
    (void)timing;
    return (config->wait_ready(config->context, 1U) == 0U) ? NAND_RESULT_OK : NAND_RESULT_NOT_READY;
}

static uint8_t nand_fmc_exec_command(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)request;
    (void)timing;
    (void)completion;
    *config->command = (uint8_t)uop->operand;
    return NAND_RESULT_OK;
}

static uint8_t nand_fmc_exec_command_primary(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
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

static uint8_t nand_fmc_exec_command_secondary(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
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

static uint8_t nand_fmc_exec_column_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)request->column;
    return NAND_RESULT_OK;
}

static uint8_t nand_fmc_exec_column_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->column >> 8U);
    return NAND_RESULT_OK;
}

static uint8_t nand_fmc_exec_row_0(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)request->row[uop->operand];
    return NAND_RESULT_OK;
}

static uint8_t nand_fmc_exec_row_1(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->row[uop->operand] >> 8U);
    return NAND_RESULT_OK;
}

static uint8_t nand_fmc_exec_row_2(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;

    (void)uop;
    (void)timing;
    (void)completion;
    *config->address = (uint8_t)(request->row[uop->operand] >> 16U);
    return NAND_RESULT_OK;
}

static uint8_t nand_fmc_exec_data_in(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    uint8_t                  result;

    (void)uop;
    (void)timing;

    if ((request->buffer == NULL) || (request->data_length == 0U))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    result = nand_dma_memory_to_fmc(config->data, request->buffer, request->data_length);
    if (result != 0U)
    {
        /* DMA 失败回退到 CPU 逐字节搬运 */
        uint16_t index;
        for (index = 0U; index < request->data_length; index++)
        {
            *config->data = request->buffer[index];
        }
    }

    completion->transfer_status = NAND_RESULT_OK;
    return NAND_RESULT_OK;
}

static uint8_t nand_fmc_exec_data_out(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    const nand_fmc_config_t *config = (const nand_fmc_config_t *)context;
    uint8_t                  result;

    (void)uop;
    (void)timing;

    if ((request->buffer == NULL) || (request->data_length == 0U))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    result = nand_dma_fmc_to_memory(config->data, request->buffer, request->data_length);
    if (result != 0U)
    {
        /* DMA 失败回退到 CPU 逐字节搬运 */
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

static uint8_t nand_fmc_exec_data_out_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
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

static uint8_t nand_fmc_exec_read_status(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
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

static uint8_t nand_fmc_exec_check_ready(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)request;
    (void)timing;
    return ((completion->nand_status & NAND_STATUS_READY) != 0U) ? NAND_RESULT_OK : NAND_RESULT_NOT_READY;
}

static uint8_t nand_fmc_exec_check_fail(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion)
{
    (void)context;
    (void)uop;
    (void)request;
    (void)timing;
    return ((completion->nand_status & NAND_STATUS_FAIL) == 0U) ? NAND_RESULT_OK : NAND_RESULT_OPERATION_FAILED;
}
