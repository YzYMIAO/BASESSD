#include "nand_dispatch.h"

#include "common/ssd_log.h"
#include "fc/fc_command.h"
#include "nand_config.h"
#include "nand_mt29f4g08abadawp.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

typedef struct
{
    uint16_t entries[NAND_CHANNEL_QUEUE_DEPTH];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} nand_channel_queue_t;

typedef struct
{
    uint16_t command_id;
    uint8_t  operation;
    uint8_t  page_type   : 2;      /* Copy from dispatch request for LDPC engine. */
    uint8_t  reserved    : 6;
    uint8_t  cw_index;             /* Current CW being processed (0-based). */
    uint8_t  cw_count;
    uint8_t  started;
    uint8_t  parameter[4];
    uint8_t  feature_addr;

    /* Pre-resolved per-CW data; cw_count entries valid. */
    nand_paa_t  cw_paa[CW_NUM];
    uint8_t    *cw_buffers[CW_NUM];

    nand_fc_request_t        request;
    nand_fc_transaction_t    transaction;
    nand_dispatch_completion_t completion;
} nand_dispatch_task_t;

static nand_channel_queue_t g_nand_channel_queues[CH_NUM];
static uint8_t              g_nand_channel_cursor;
static uint8_t              g_nand_device_ready;
static nand_dispatch_task_t g_nand_dispatch_tasks[CH_NUM][CE_NUM][LUN_NUM];
static uint8_t              g_nand_channel_page_buffer[CH_NUM][PAGE_SIZE];

static void nand_dispatch_completion_init(nand_dispatch_completion_t *completion, uint16_t command_id)
{
    *completion                 = (nand_dispatch_completion_t){0};
    completion->command_id       = command_id;
    completion->failed_descriptor_id = FC_DESCRIPTOR_ID_INVALID;
    completion->cw_status[0]    = FC_CW_STATUS_UNUSED;
    completion->cw_status[1]    = FC_CW_STATUS_UNUSED;
    completion->cw_status[2]    = FC_CW_STATUS_UNUSED;
    completion->cw_status[3]    = FC_CW_STATUS_UNUSED;
}

static uint8_t nand_dispatch_execute_request(uint8_t operation, nand_paa_t paa, uint8_t *buffer,
                                              uint16_t length, uint16_t column,
                                              nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {column, paa_to_nand_row(paa), buffer, length};
    return nand_fc_execute(operation, &request, completion);
}

static void nand_dispatch_task_complete(nand_dispatch_task_t *task, uint8_t result,
                                         const nand_fc_completion_t *nand_completion)
{
    uint8_t status = (result == NAND_RESULT_OK)       ? FC_CW_STATUS_SUCCESS
                     : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT
                                                       : FC_CW_STATUS_NAND_FAIL;

    task->completion.execution_status = result;
    task->completion.nand_status      = nand_completion->nand_status;

    if ((task->operation == OP_NORMAL_PROGRAM_PAGE) ||
        (task->operation == OP_OTP_DATA_PROGRAM) ||
        (task->operation == OP_PROGRAM_PAGE_CACHE) ||
        (task->operation == OP_ERASE_BLOCK))
    {
        uint8_t cw;
        for (cw = 0U; cw < task->cw_count; cw++)
        {
            task->completion.cw_status[cw] = status;
        }
        if (result != NAND_RESULT_OK)
        {
            task->completion.failed_descriptor_id = 0U;
        }
    }
    else if ((task->operation == OP_NORMAL_READ_PAGE) ||
             (task->operation == OP_OTP_DATA_READ) ||
             (task->operation == OP_READ_PAGE_CACHE_SEQ) ||
             (task->operation == OP_READ_PAGE_CACHE_RANDOM) ||
             (task->operation == OP_READ_PAGE_CACHE_LAST))
    {
        task->completion.cw_status[task->cw_index] = status;
        if (result != NAND_RESULT_OK)
        {
            task->completion.failed_descriptor_id = task->cw_index;
        }
    }
    else
    {
        task->completion.cw_status[0] = status;
    }
}

static uint8_t nand_dispatch_execute_page(nand_dispatch_task_t *task, uint8_t program)
{
    nand_fc_completion_t nand_completion;
    uint8_t              index;
    uint8_t              result;
    uint8_t             *page_buffer = g_nand_channel_page_buffer[0U];
    nand_paa_t           page_paa    = task->cw_paa[0U];

    if (program != 0U)
    {
        memset(page_buffer, 0xFF, PAGE_SIZE);
        for (index = 0U; index < task->cw_count; index++)
        {
            if (paa_to_nand_row(task->cw_paa[index]) != paa_to_nand_row(page_paa))
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            if (task->cw_buffers[index] == NULL)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            memcpy(&page_buffer[index * (PAGE_SIZE / 4U)], task->cw_buffers[index], PAGE_SIZE / 4U);
        }
        result = nand_dispatch_execute_request(task->operation, page_paa, page_buffer, PAGE_SIZE, 0U,
                                                &nand_completion);
        for (index = 0U; index < task->cw_count; index++)
        {
            task->completion.cw_status[index] =
                (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : FC_CW_STATUS_NAND_FAIL;
        }
        task->completion.execution_status = result;
        task->completion.nand_status      = nand_completion.nand_status;
        return result;
    }

    /* READ: CW0 = (OTP_)NORMAL_READ_PAGE, CW1..N-1 = random data read per CW. */
    for (index = 0U; index < task->cw_count; index++)
    {
        uint8_t *cw_buffer = task->cw_buffers[index];
        uint16_t cw_column = (uint16_t)(index * (PAGE_SIZE / 4U));
        uint8_t  cw_op     = (index == 0U) ? task->operation : OP_RANDOM_DATA_READ;

        if (cw_buffer == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        result = nand_dispatch_execute_request(cw_op, task->cw_paa[index], cw_buffer,
                                                PAGE_SIZE / 4U, cw_column, &nand_completion);
        task->completion.nand_status                      = nand_completion.nand_status;
        task->completion.cw_status[index] =
            (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : FC_CW_STATUS_NAND_FAIL;
        if (result != NAND_RESULT_OK)
        {
            task->completion.execution_status     = result;
            task->completion.failed_descriptor_id = (uint16_t)index;
            return result;
        }
    }
    return NAND_RESULT_OK;
}

__attribute__((unused)) static uint8_t nand_dispatch_execute_command(nand_dispatch_task_t *task,
                                                                      nand_dispatch_completion_t *completion)
{
    nand_fc_completion_t nand_completion;
    uint8_t             *buffer     = NULL;
    uint16_t             length     = 0U;
    uint8_t              result;

    nand_dispatch_completion_init(completion, task->command_id);

    if (task->operation >= OP_COUNT)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    if ((task->operation == OP_NORMAL_READ_PAGE) || (task->operation == OP_OTP_DATA_READ))
    {
        return nand_dispatch_execute_page(task, 0U);
    }
    if ((task->operation == OP_NORMAL_PROGRAM_PAGE) ||
        (task->operation == OP_OTP_DATA_PROGRAM) ||
        (task->operation == OP_PROGRAM_PAGE_CACHE))
    {
        return nand_dispatch_execute_page(task, 1U);
    }
    if ((task->operation == OP_READ_PAGE_CACHE_SEQ) ||
        (task->operation == OP_READ_PAGE_CACHE_RANDOM) ||
        (task->operation == OP_READ_PAGE_CACHE_LAST))
    {
        /* Cache reads: single-shot full-page transfer. Use page_buffer,
         * then copy back to per-CW buffers. */
        uint8_t *page_buffer = g_nand_channel_page_buffer[0U];
        uint8_t  index;
        for (index = 0U; index < task->cw_count; index++)
        {
            if (task->cw_buffers[index] == NULL)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
        }
        memset(page_buffer, 0x00, PAGE_SIZE);
        result = nand_dispatch_execute_request(task->operation, task->cw_paa[0U],
                                                page_buffer, PAGE_SIZE, 0U, &nand_completion);
        for (index = 0U; index < task->cw_count; index++)
        {
            memcpy(task->cw_buffers[index], &page_buffer[index * (PAGE_SIZE / 4U)],
                   PAGE_SIZE / 4U);
            task->completion.cw_status[index] =
                (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : FC_CW_STATUS_NAND_FAIL;
        }
        task->completion.execution_status = result;
        task->completion.nand_status      = nand_completion.nand_status;
        return result;
    }
    if (task->operation == OP_ERASE_BLOCK)
    {
        nand_paa_t block_paa = task->cw_paa[0U];
        if (task->cw_count == 0U)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        result = nand_dispatch_execute_request(OP_ERASE_BLOCK, block_paa, NULL, 0U, 0U, &nand_completion);
        completion->execution_status = result;
        completion->nand_status      = nand_completion.nand_status;
        completion->cw_status[0] =
            (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : FC_CW_STATUS_NAND_FAIL;
        return result;
    }
    else if ((task->operation == OP_READ_ID) || (task->operation == OP_READ_PARAMETER_PAGE) ||
               (task->operation == OP_READ_UNIQUE_ID))
    {
        buffer = task->cw_buffers[0U];
        length = nand_operation_data_length(task->operation);
    }
    else if ((task->operation == OP_SET_FEATURES) || (task->operation == OP_GET_FEATURES))
    {
        buffer = task->cw_buffers[0U];
        length = 4U;
    }
    else if (nand_operation_has_data(task->operation))
    {
        buffer = task->cw_buffers[0U];
        length = nand_operation_data_length(task->operation);
    }
    if (length != 0U && buffer == NULL)
    {
        buffer = task->parameter;
    }
    nand_paa_t cmd_paa = (task->cw_count != 0U) ? task->cw_paa[0U] : 0U;
    uint16_t cmd_column = ((task->operation == OP_SET_FEATURES) || (task->operation == OP_GET_FEATURES))
                              ? (uint16_t)task->feature_addr
                              : 0U;
    result =
        (buffer == NULL && length != 0U)
            ? NAND_RESULT_INVALID_ARGUMENT
            : nand_dispatch_execute_request(task->operation, cmd_paa, buffer, length, cmd_column, &nand_completion);
    completion->execution_status = result;
    completion->nand_status      = nand_completion.nand_status;
    completion->cw_status[0] =
        (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : FC_CW_STATUS_NAND_FAIL;
    return result;
}

uint8_t nand_dispatch_init(void)
{
    uint8_t channel;
    uint8_t ce;
    uint8_t lun;

    memset(g_nand_channel_queues, 0, sizeof(g_nand_channel_queues));
    memset(g_nand_dispatch_tasks, 0, sizeof(g_nand_dispatch_tasks));
    for (channel = 0U; channel < CH_NUM; channel++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                g_nand_dispatch_tasks[channel][ce][lun].command_id = FC_COMMAND_ID_INVALID;
            }
        }
    }
    g_nand_channel_cursor = 0U;
    g_nand_device_ready   = 0U;
    return NAND_RESULT_OK;
}

void nand_dispatch_set_device_ready(uint8_t ready)
{
    g_nand_device_ready = (ready != 0U) ? 1U : 0U;
}

uint8_t nand_dispatch_submit(const nand_dispatch_request_t *request)
{
    nand_channel_queue_t *queue;
    uint8_t               cw;

    if ((g_nand_device_ready == 0U) || (request == NULL) || (request->ch >= CH_NUM) ||
        (request->ce >= CE_NUM) || (request->lun >= LUN_NUM) ||
        (request->cw_count > CW_NUM))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    queue = &g_nand_channel_queues[request->ch];
    if (queue->count == NAND_CHANNEL_QUEUE_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }

    /* Pre-load the task with resolved data so polling never calls back to FC. */
    nand_dispatch_task_t *task =
        &g_nand_dispatch_tasks[request->ch][request->ce][request->lun];
    if (task->command_id != FC_COMMAND_ID_INVALID)
    {
        return NAND_RESULT_NOT_READY;
    }

    nand_dispatch_completion_init(&task->completion, request->command_id);
    task->command_id   = request->command_id;
    task->operation    = request->operation;
    task->page_type    = request->page_type;
    task->cw_index     = 0U;
    task->cw_count     = request->cw_count;
    task->started      = 0U;
    task->parameter[0] = request->parameter[0];
    task->parameter[1] = request->parameter[1];
    task->parameter[2] = request->parameter[2];
    task->parameter[3] = request->parameter[3];
    task->feature_addr = request->feature_addr;
    for (cw = 0U; cw < request->cw_count; cw++)
    {
        task->cw_paa[cw]    = request->cws[cw].paa;
        task->cw_buffers[cw] = request->cws[cw].buffer;
    }

    queue->entries[queue->tail] = (uint16_t)request->command_id;
    queue->tail                 = (uint16_t)((queue->tail + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
    queue->count++;
    return NAND_RESULT_OK;
}

uint8_t nand_dispatch_poll(nand_dispatch_completion_t *completion)
{
    uint8_t count;
    uint8_t channel;

    if (completion == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    for (count = 0U; count < CH_NUM; count++)
    {
        channel = (uint8_t)((g_nand_channel_cursor + count) % CH_NUM);
        nand_channel_queue_t *queue = &g_nand_channel_queues[channel];
        uint8_t ce;
        uint8_t lun;

        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                nand_dispatch_task_t *task = &g_nand_dispatch_tasks[channel][ce][lun];
                nand_fc_completion_t  nand_completion;
                uint8_t               result;

                if (task->command_id == FC_COMMAND_ID_INVALID)
                {
                    continue;
                }
                result = nand_fc_step(&task->transaction, &nand_completion);
                if (result == NAND_RESULT_NOT_READY)
                {
                    continue;
                }

                /* Handle multi-CW READ chaining after each CW completes.
                 * Only page-read operations that span multiple CWs chain via RANDOM_DATA_READ. */
                for (;;)
                {
                    nand_dispatch_task_complete(task, result, &nand_completion);
                    if ((result != NAND_RESULT_OK) ||
                        ((task->operation != OP_NORMAL_READ_PAGE) && (task->operation != OP_OTP_DATA_READ)) ||
                        (task->cw_index + 1U >= task->cw_count))
                    {
                        break;
                    }
                    task->cw_index++;
                    task->operation = OP_RANDOM_DATA_READ;
                    task->started   = 1U;
                    /* Prepare the next CW request from pre-resolved data. */
                    {
                        nand_paa_t cw_paa      = task->cw_paa[task->cw_index];
                        uint8_t   *cw_buffer   = task->cw_buffers[task->cw_index];
                        uint16_t   cw_column   = (uint16_t)(task->cw_index * (PAGE_SIZE / 4U));
                        task->request = (nand_fc_request_t){cw_column, paa_to_nand_row(cw_paa),
                                                             cw_buffer, (uint16_t)(PAGE_SIZE / 4U)};
                    }
                    result = nand_fc_start(&task->transaction, task->operation, &task->request);
                    if (result == NAND_RESULT_NOT_READY)
                    {
                        break;
                    }
                    nand_completion.execution_status = result;
                    nand_completion.nand_status =
                        task->transaction.completion.nand_status;
                }
                if (result == NAND_RESULT_NOT_READY)
                {
                    continue;
                }

                /* Cache reads: copy page_buffer back to individual CW buffers. */
                if ((task->operation == OP_READ_PAGE_CACHE_SEQ) ||
                    (task->operation == OP_READ_PAGE_CACHE_RANDOM) ||
                    (task->operation == OP_READ_PAGE_CACHE_LAST))
                {
                    uint8_t cache_cw;
                    for (cache_cw = 0U; cache_cw < task->cw_count; cache_cw++)
                    {
                        memcpy(task->cw_buffers[cache_cw],
                               &g_nand_channel_page_buffer[channel][cache_cw * (PAGE_SIZE / 4U)],
                               PAGE_SIZE / 4U);
                    }
                }

                *completion       = task->completion;
                task->command_id = FC_COMMAND_ID_INVALID;
                g_nand_channel_cursor = (uint8_t)((channel + 1U) % CH_NUM);
                return result;
            }
        }

        if (queue->count != 0U)
        {
            uint16_t               command_id = queue->entries[queue->head];
            nand_dispatch_task_t  *task;
            nand_fc_completion_t   nand_completion;
            uint8_t                result;
            uint8_t                cw;

            task = &g_nand_dispatch_tasks[channel][0U][0U]; /* CE=0, LUN=0 for single-CE/LUN config. */
            if (task->command_id != FC_COMMAND_ID_INVALID)
            {
                continue;
            }
            queue->head = (uint16_t)((queue->head + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
            queue->count--;

            /* The task was pre-loaded at submit time — just validate it's still here. */
            if (task->command_id != command_id)
            {
                /* Task struct was already populated; command_id was cleared to
                 * FC_COMMAND_ID_INVALID at init time so this path won't hit. */
                continue;
            }

            /* Build the nand_fc request from pre-resolved task data. */
            if ((task->operation == OP_NORMAL_PROGRAM_PAGE) ||
                (task->operation == OP_OTP_DATA_PROGRAM) ||
                (task->operation == OP_PROGRAM_PAGE_CACHE))
            {
                /* PROGRAM: the FMC backend writes the full page buffer. */
                uint8_t *page_buffer = g_nand_channel_page_buffer[channel];
                memset(page_buffer, 0xFF, PAGE_SIZE);
                for (cw = 0U; cw < task->cw_count; cw++)
                {
                    if (task->cw_buffers[cw] == NULL)
                    {
                        task->command_id = FC_COMMAND_ID_INVALID;
                        return NAND_RESULT_INVALID_ARGUMENT;
                    }
                    memcpy(&page_buffer[cw * (PAGE_SIZE / 4U)], task->cw_buffers[cw],
                           PAGE_SIZE / 4U);
                }
                task->request = (nand_fc_request_t){0U, paa_to_nand_row(task->cw_paa[0U]),
                                                     page_buffer, PAGE_SIZE};
            }
            else if ((task->operation == OP_NORMAL_READ_PAGE) ||
                     (task->operation == OP_OTP_DATA_READ))
            {
                /* READ: start with CW0. */
                nand_paa_t cw_paa    = task->cw_paa[0U];
                uint8_t   *cw_buffer = task->cw_buffers[0U];
                if (cw_buffer == NULL)
                {
                    task->command_id = FC_COMMAND_ID_INVALID;
                    return NAND_RESULT_INVALID_ARGUMENT;
                }
                task->request = (nand_fc_request_t){0U, paa_to_nand_row(cw_paa),
                                                     cw_buffer, (uint16_t)(PAGE_SIZE / 4U)};
            }
            else if ((task->operation == OP_READ_PAGE_CACHE_SEQ) ||
                     (task->operation == OP_READ_PAGE_CACHE_RANDOM) ||
                     (task->operation == OP_READ_PAGE_CACHE_LAST))
            {
                /* Cache reads: single-shot full-page transfer into page_buffer.
                 * NAND outputs PAGE_SIZE bytes in one data phase — can't chain per-CW. */
                uint8_t *page_buffer = g_nand_channel_page_buffer[channel];
                for (cw = 0U; cw < task->cw_count; cw++)
                {
                    if (task->cw_buffers[cw] == NULL)
                    {
                        task->command_id = FC_COMMAND_ID_INVALID;
                        return NAND_RESULT_INVALID_ARGUMENT;
                    }
                }
                memset(page_buffer, 0x00, PAGE_SIZE);
                task->request = (nand_fc_request_t){0U, paa_to_nand_row(task->cw_paa[0U]),
                                                     page_buffer, PAGE_SIZE};
            }
            else if (task->operation == OP_ERASE_BLOCK)
            {
                task->request =
                    (nand_fc_request_t){0U, paa_to_nand_row(task->cw_paa[0U]), NULL, 0U};
            }
            else if ((task->operation == OP_READ_ID) || (task->operation == OP_READ_PARAMETER_PAGE))
            {
                task->request =
                    (nand_fc_request_t){0U,
                                        (task->cw_count != 0U) ? paa_to_nand_row(task->cw_paa[0U])
                                                                : 0U,
                                        task->cw_buffers[0U],
                                        (task->operation == OP_READ_ID) ? 5U : 256U};
            }
            else if ((task->operation == OP_SET_FEATURES) || (task->operation == OP_GET_FEATURES))
            {
                /* SET/GET_FEATURES: column = feature_addr (register address),
                 * data = task->parameter[0..3] (via cw_buffers[0] already redirected). */
                task->request =
                    (nand_fc_request_t){(uint16_t)task->feature_addr, 0U,
                                        task->cw_buffers[0U], 4U};
            }
            else if (nand_operation_has_data(task->operation))
            {
                /* SINGLE_IN / SINGLE_OUT: use pre-resolved CW0 buffer with exact length. */
                task->request =
                    (nand_fc_request_t){0U,
                                        (task->cw_count != 0U) ? paa_to_nand_row(task->cw_paa[0U]) : 0U,
                                        task->cw_buffers[0U],
                                        nand_operation_data_length(task->operation)};
            }
            else
            {
                /* NO_DATA (RESET, BLOCK_LOCK, READ_MODE, etc.): address-only, no buffer. */
                task->request = (nand_fc_request_t){
                    0U,
                    (task->cw_count != 0U) ? paa_to_nand_row(task->cw_paa[0U]) : 0U,
                    NULL,
                    0U};
            }

            SSD_LOGD("nand: FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n",
                     task->operation, (void *)task->request.data, task->request.data_length,
                     task->request.column, (unsigned long)task->request.row);
            result = nand_fc_start(&task->transaction, task->operation, &task->request);
            task->started = 1U;
            if (result == NAND_RESULT_NOT_READY)
            {
                continue;
            }
            nand_completion.execution_status = result;
            nand_completion.nand_status =
                task->transaction.completion.nand_status;
            nand_dispatch_task_complete(task, result, &nand_completion);
            *completion       = task->completion;
            task->command_id = FC_COMMAND_ID_INVALID;
            return result;
        }
    }
    return NAND_RESULT_NOT_READY;
}