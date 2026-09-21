#include "common/ssd_log.h"
#include "fc/fc_command.h"
#include "nand_config.h"
#include "nand_dispatch_internal.h"
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

static nand_channel_queue_t g_nand_channel_queues[CH_NUM];
static uint8_t              g_nand_channel_cursor;
static uint8_t              g_nand_device_ready;
static nand_dispatch_task_t g_nand_dispatch_tasks[CH_NUM][CE_NUM][LUN_NUM];
static uint8_t              g_nand_channel_page_buffer[CH_NUM][PAGE_SIZE];

static void nand_dispatch_completion_init(nand_dispatch_completion_t *completion, uint16_t command_id)
{
    *completion                      = (nand_dispatch_completion_t){0};
    completion->command_id           = command_id;
    completion->failed_descriptor_id = FC_DESCRIPTOR_ID_INVALID;
    completion->cw_status[0]         = FC_CW_STATUS_UNUSED;
    completion->cw_status[1]         = FC_CW_STATUS_UNUSED;
    completion->cw_status[2]         = FC_CW_STATUS_UNUSED;
    completion->cw_status[3]         = FC_CW_STATUS_UNUSED;
}

/* ------------------------------------------------------------------
 * 这些函数已被重构为函数指针表，保留此处供参考：
 * - nand_dispatch_execute_request   → 由 nand_fc_start/step 替代
 * - nand_dispatch_execute_page      → nand_dispatch_req_table
 * - nand_dispatch_execute_command   → nand_dispatch_req_table + completion_table
 * 删除前请确认所有外部引用已清理。
 * ------------------------------------------------------------------ */

static void nand_dispatch_task_complete(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    task->completion.execution_status = result;
    task->completion.nand_status      = nand_completion->nand_status;

    g_nand_dispatch_completion_table[task->operation](task, result, nand_completion);
}

/* ------------------------------------------------------------------
 * 这些函数已被重构为函数指针表，保留此处供参考：
 * - nand_dispatch_execute_page      → nand_dispatch_req_table
 * - nand_dispatch_execute_command   → nand_dispatch_req_table + completion_table
 * 删除前请确认所有外部引用已清理。
 * ------------------------------------------------------------------ */

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
    task->ch           = request->ch;
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
        task->cw_paa[cw]     = request->cws[cw].paa;
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
        channel                     = (uint8_t)((g_nand_channel_cursor + count) % CH_NUM);
        nand_channel_queue_t *queue = &g_nand_channel_queues[channel];
        uint8_t               ce;
        uint8_t               lun;

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
                        nand_paa_t cw_paa    = task->cw_paa[task->cw_index];
                        uint8_t   *cw_buffer = task->cw_buffers[task->cw_index];
                        uint16_t   cw_column = (uint16_t)(task->cw_index * (PAGE_SIZE / 4U));
                        task->request        = (nand_fc_request_t){cw_column, paa_to_nand_row(cw_paa), cw_buffer, (uint16_t)(PAGE_SIZE / 4U)};
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

                *completion           = task->completion;
                task->command_id      = FC_COMMAND_ID_INVALID;
                g_nand_channel_cursor = (uint8_t)((channel + 1U) % CH_NUM);
                return result;
            }
        }

        if (queue->count != 0U)
        {
            uint16_t              command_id = queue->entries[queue->head];
            nand_dispatch_task_t *task;
            nand_fc_completion_t  nand_completion;
            uint8_t               result;

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

            /* Build the nand_fc request from pre-resolved task data via function pointer table.
             * O(1) indirect jump — no if-else chain. */
            result = g_nand_dispatch_req_table[task->operation](task);
            if (result != NAND_RESULT_OK)
            {
                task->command_id = FC_COMMAND_ID_INVALID;
                return NAND_RESULT_INVALID_ARGUMENT;
            }

            SSD_LOGD("nand: FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n",
                     task->operation,
                     (void *)task->request.data,
                     task->request.data_length,
                     task->request.column,
                     (unsigned long)task->request.row);
            result        = nand_fc_start(&task->transaction, task->operation, &task->request);
            task->started = 1U;
            if (result == NAND_RESULT_NOT_READY)
            {
                continue;
            }
            nand_completion.execution_status = result;
            nand_completion.nand_status =
                task->transaction.completion.nand_status;
            nand_dispatch_task_complete(task, result, &nand_completion);
            *completion      = task->completion;
            task->command_id = FC_COMMAND_ID_INVALID;
            return result;
        }
    }
    return NAND_RESULT_NOT_READY;
}