#include "nand_dispatch.h"

#include "common/ssd_log.h"
#include "fc/fc.h"
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
    uint16_t descriptor_id;
    uint8_t descriptor_index;
    uint8_t operation;
    uint8_t started;
    uint8_t parameter[4];
    nand_fc_request_t request;
    nand_fc_transaction_t transaction;
    nand_dispatch_completion_t completion;
} nand_dispatch_task_t;

static nand_channel_queue_t g_nand_channel_queues[CH_NUM];
static uint8_t g_nand_channel_cursor;
static uint8_t g_nand_device_ready;
static nand_dispatch_task_t g_nand_dispatch_tasks[CH_NUM][CE_NUM][LUN_NUM];
static uint8_t g_nand_channel_page_buffer[CH_NUM][PAGE_SIZE];

static fc_command_t *nand_dispatch_command(uint16_t command_id)
{
    return fc_command_id_to_command(command_id);
}

static fc_descriptor_t *nand_dispatch_descriptor(uint16_t descriptor_id)
{
    return fc_descriptor_id_to_descriptor(descriptor_id);
}

static uint8_t nand_dispatch_target_is_valid(const fc_command_t *command)
{
    return (uint8_t)((command != NULL) && (command->config.ch < CH_NUM) &&
                     (command->config.ce < CE_NUM) && (command->config.lun < LUN_NUM));
}

static void nand_dispatch_completion_init(nand_dispatch_completion_t *completion, const fc_command_t *command)
{
    *completion = (nand_dispatch_completion_t){0};
    completion->command_id = (command != NULL) ? command->command_id : FC_COMMAND_HANDLE_INVALID;
    completion->failed_descriptor_id = FC_DESCRIPTOR_ID_INVALID;
    completion->cw_status[0] = FC_CW_STATUS_UNUSED;
    completion->cw_status[1] = FC_CW_STATUS_UNUSED;
    completion->cw_status[2] = FC_CW_STATUS_UNUSED;
    completion->cw_status[3] = FC_CW_STATUS_UNUSED;
}

static uint8_t nand_dispatch_execute_request(uint8_t operation, nand_paa_t paa, uint8_t *buffer, uint16_t length, uint16_t column, nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {column, paa_to_nand_row(paa), buffer, length};
    return nand_fc_execute(operation, &request, completion);
}

static uint8_t nand_dispatch_task_request_prepare(nand_dispatch_task_t *task, nand_fc_request_t *request)
{
    fc_command_t *command = nand_dispatch_command(task->command_id);
    fc_descriptor_t *descriptor = nand_dispatch_descriptor(task->descriptor_id);
    uint8_t *buffer = NULL;
    uint16_t length = 0U;
    uint16_t column = 0U;
    uint8_t index;

    if (command == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    *request = (nand_fc_request_t){0};
    if ((command->config.operation == OP_NORMAL_READ_PAGE) || (command->config.operation == OP_NORMAL_PROGRAM_PAGE))
    {
        if (descriptor == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        if (command->config.operation == OP_NORMAL_PROGRAM_PAGE)
        {
            if (task->started == 0U)
            {
                fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id;
                memset(g_nand_channel_page_buffer[command->config.ch], 0xFF, PAGE_SIZE);
                for (index = 0U; index <= command->config.cw; index++)
                {
                    descriptor = nand_dispatch_descriptor(descriptor_id);
                    if (descriptor == NULL)
                    {
                        return NAND_RESULT_INVALID_ARGUMENT;
                    }
                    buffer = fc_buffer_id_to_real_buffer((fc_buffer_id_t)descriptor->buffer_id);
                    if (buffer == NULL)
                    {
                        return NAND_RESULT_INVALID_ARGUMENT;
                    }
                    memcpy(&g_nand_channel_page_buffer[command->config.ch][descriptor->config.cw * (PAGE_SIZE / 4U)], buffer, PAGE_SIZE / 4U);
                    descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
                }
            }
            buffer = g_nand_channel_page_buffer[command->config.ch];
            length = PAGE_SIZE;
        }
        else
        {
            buffer = fc_buffer_id_to_real_buffer((fc_buffer_id_t)descriptor->buffer_id);
            column = (uint16_t)(descriptor->config.cw * (PAGE_SIZE / 4U));
            length = (uint16_t)(PAGE_SIZE / 4U);
        }
        if (buffer == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        request->column = column;
        request->row = paa_to_nand_row(descriptor->paa);
        request->data = buffer;
        request->data_length = length;
        return NAND_RESULT_OK;
    }
    if (command->config.operation == OP_READ_ID || command->config.operation == OP_READ_PARAMETER_PAGE)
    {
        if (descriptor == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        buffer = (descriptor != NULL) ? fc_buffer_id_to_real_buffer((fc_buffer_id_t)descriptor->buffer_id) : NULL;
        length = (command->config.operation == OP_READ_ID) ? 5U : 256U;
    }
    else if (command->config.operation == OP_READ_STATUS || command->config.operation == OP_GET_FEATURES || command->config.operation == OP_SET_FEATURES)
    {
        task->parameter[0] = (uint8_t)command->parameter0;
        task->parameter[1] = (uint8_t)command->parameter1;
        task->parameter[2] = (uint8_t)command->parameter2;
        task->parameter[3] = (uint8_t)command->parameter3;
        buffer = task->parameter;
        length = (command->config.operation == OP_READ_STATUS) ? 1U : 4U;
    }
    if (((command->config.operation == OP_READ_ID) || (command->config.operation == OP_READ_PARAMETER_PAGE)) && (buffer == NULL))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    request->row = paa_to_nand_row(command->paa);
    request->data = buffer;
    request->data_length = length;
    return NAND_RESULT_OK;
}

static void nand_dispatch_task_complete(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    fc_command_t *command = nand_dispatch_command(task->command_id);
    fc_descriptor_t *descriptor = nand_dispatch_descriptor(task->descriptor_id);
    uint8_t status = (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT : FC_CW_STATUS_NAND_FAIL;

    task->completion.execution_status = result;
    task->completion.nand_status = nand_completion->nand_status;
    if ((command != NULL) && (command->config.operation == OP_NORMAL_PROGRAM_PAGE) && (descriptor != NULL))
    {
        fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id;
        /* One FMC program transfer writes the complete Page, so it completes every submitted CW. */
        uint8_t cw;

        for (cw = 0U; cw <= command->config.cw; cw++)
        {
            descriptor = nand_dispatch_descriptor(descriptor_id);
            if (descriptor == NULL)
            {
                task->completion.execution_status     = NAND_RESULT_INVALID_ARGUMENT;
                task->completion.cw_status[cw]       = FC_CW_STATUS_NAND_FAIL;
                task->completion.failed_descriptor_id = descriptor_id;
                return;
            }
            descriptor->status = status;
            task->completion.cw_status[cw] = status;
            descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
        }
        if (result != NAND_RESULT_OK)
        {
            task->completion.failed_descriptor_id = (fc_descriptor_id_t)command->desc_id;
        }
    }
    else if ((command != NULL) && (command->config.operation == OP_NORMAL_READ_PAGE) && (descriptor != NULL))
    {
        descriptor->status = status;
        task->completion.cw_status[descriptor->config.cw] = status;
        if (result != NAND_RESULT_OK)
        {
            task->completion.failed_descriptor_id = task->descriptor_id;
        }
    }
    else
    {
        task->completion.cw_status[0] = status;
    }
}

static uint8_t nand_dispatch_execute_page(fc_command_t *command, nand_dispatch_completion_t *completion, uint8_t program)
{
    fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id;
    fc_descriptor_t *descriptor;
    nand_fc_completion_t nand_completion;
    uint8_t index;
    uint8_t result;
    uint8_t *page_buffer = g_nand_channel_page_buffer[command->config.ch];
    nand_paa_t page_paa;

    descriptor = nand_dispatch_descriptor(descriptor_id);
    if (descriptor == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    page_paa = descriptor->paa;
    if (program != 0U)
    {
        memset(page_buffer, 0xFF, PAGE_SIZE);
        for (index = 0U; index <= command->config.cw; index++)
        {
            descriptor = nand_dispatch_descriptor(descriptor_id);
            if ((descriptor == NULL) || (paa_to_nand_row(descriptor->paa) != paa_to_nand_row(page_paa)))
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            uint8_t *buffer = fc_buffer_id_to_real_buffer((fc_buffer_id_t)descriptor->buffer_id);
            if (buffer == NULL)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            memcpy(&page_buffer[descriptor->config.cw * (PAGE_SIZE / 4U)], buffer, PAGE_SIZE / 4U);
            descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
        }
        result = nand_dispatch_execute_request(OP_NORMAL_PROGRAM_PAGE, page_paa, page_buffer, PAGE_SIZE, 0U, &nand_completion);
    }
    else
    {
        for (index = 0U; index <= command->config.cw; index++)
        {
            descriptor = nand_dispatch_descriptor(descriptor_id);
            if ((descriptor == NULL) || (paa_to_nand_row(descriptor->paa) != paa_to_nand_row(page_paa)))
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            uint8_t *buffer = fc_buffer_id_to_real_buffer((fc_buffer_id_t)descriptor->buffer_id);
            if (buffer == NULL)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            result = nand_dispatch_execute_request((index == 0U) ? OP_NORMAL_READ_PAGE : OP_RANDOM_DATA_READ, descriptor->paa, buffer, PAGE_SIZE / 4U, descriptor->config.cw * (PAGE_SIZE / 4U), &nand_completion);
            completion->nand_status = nand_completion.nand_status;
            completion->cw_status[descriptor->config.cw] = (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : FC_CW_STATUS_NAND_FAIL;
            if (result != NAND_RESULT_OK)
            {
                completion->execution_status = result;
                completion->failed_descriptor_id = descriptor_id;
                return result;
            }
            descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
        }
        return NAND_RESULT_OK;
    }
    completion->execution_status = result;
    completion->nand_status = nand_completion.nand_status;
    for (index = 0U; index <= command->config.cw; index++)
    {
        completion->cw_status[index] = (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : FC_CW_STATUS_NAND_FAIL;
    }
    return result;
}

static uint8_t nand_dispatch_execute_command(fc_command_t *command, nand_dispatch_completion_t *completion)
{
    nand_fc_completion_t nand_completion;
    uint8_t operation = (uint8_t)command->config.operation;
    uint8_t *buffer = NULL;
    uint16_t length = 0U;
    uint8_t result;

    nand_dispatch_completion_init(completion, command);
    if (operation >= OP_COUNT)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    if (operation == OP_NORMAL_READ_PAGE)
    {
        return nand_dispatch_execute_page(command, completion, 0U);
    }
    if (operation == OP_NORMAL_PROGRAM_PAGE)
    {
        return nand_dispatch_execute_page(command, completion, 1U);
    }
    if ((operation == OP_READ_ID) || (operation == OP_READ_PARAMETER_PAGE))
    {
        fc_descriptor_t *descriptor = nand_dispatch_descriptor((fc_descriptor_id_t)command->desc_id);
        buffer = (descriptor != NULL) ? fc_buffer_id_to_real_buffer((fc_buffer_id_t)descriptor->buffer_id) : NULL;
        length = (operation == OP_READ_ID) ? 5U : 256U;
    }
    else if ((operation == OP_READ_STATUS) || (operation == OP_GET_FEATURES) || (operation == OP_SET_FEATURES))
    {
        buffer = NULL;
        length = (operation == OP_READ_STATUS) ? 1U : 4U;
    }
    uint8_t parameter[4] = {(uint8_t)command->parameter0, (uint8_t)command->parameter1, (uint8_t)command->parameter2, (uint8_t)command->parameter3};
    if (length != 0U && buffer == NULL)
    {
        buffer = parameter;
    }
    result = (buffer == NULL && length != 0U) ? NAND_RESULT_INVALID_ARGUMENT : nand_dispatch_execute_request(operation, command->paa, buffer, length, 0U, &nand_completion);
    completion->execution_status = result;
    completion->nand_status = nand_completion.nand_status;
    completion->cw_status[0] = (result == NAND_RESULT_OK) ? FC_CW_STATUS_SUCCESS : FC_CW_STATUS_NAND_FAIL;
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
                g_nand_dispatch_tasks[channel][ce][lun].command_id = FC_COMMAND_HANDLE_INVALID;
                g_nand_dispatch_tasks[channel][ce][lun].descriptor_id = FC_DESCRIPTOR_ID_INVALID;
            }
        }
    }
    g_nand_channel_cursor = 0U;
    g_nand_device_ready = 0U;
    return NAND_RESULT_OK;
}

void nand_dispatch_set_device_ready(uint8_t ready)
{
    g_nand_device_ready = (ready != 0U) ? 1U : 0U;
}

uint8_t nand_dispatch_submit(fc_command_t *command)
{
    nand_channel_queue_t *queue;
    if ((g_nand_device_ready == 0U) || (nand_dispatch_target_is_valid(command) == 0U))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    queue = &g_nand_channel_queues[command->config.ch];
    if (queue->count == NAND_CHANNEL_QUEUE_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }
    queue->entries[queue->tail] = (uint16_t)command->command_id;
    queue->tail = (uint16_t)((queue->tail + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
    queue->count++;
    return NAND_RESULT_OK;
}

uint8_t nand_dispatch_poll(nand_dispatch_completion_t *completion)
{
    uint8_t count;
    uint8_t result;
    if (completion == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    for (count = 0U; count < CH_NUM; count++)
    {
        uint8_t channel = (uint8_t)((g_nand_channel_cursor + count) % CH_NUM);
        nand_channel_queue_t *queue = &g_nand_channel_queues[channel];
        for (uint8_t ce = 0U; ce < CE_NUM; ce++)
        {
            for (uint8_t lun = 0U; lun < LUN_NUM; lun++)
            {
                nand_dispatch_task_t *task = &g_nand_dispatch_tasks[channel][ce][lun];
                fc_command_t *command;
                uint8_t result;
                nand_fc_completion_t nand_completion;
                if (task->command_id == FC_COMMAND_HANDLE_INVALID)
                {
                    continue;
                }
                result = nand_fc_step(&task->transaction, &nand_completion);
                if (result == NAND_RESULT_NOT_READY)
                {
                    continue;
                }
                command = nand_dispatch_command(task->command_id);
                for (;;)
                {
                    nand_dispatch_task_complete(task, result, &nand_completion);
                    if ((result != NAND_RESULT_OK) || (command == NULL) ||
                        (command->config.operation != OP_NORMAL_READ_PAGE) ||
                        (task->descriptor_index >= command->config.cw))
                    {
                        break;
                    }
                    fc_descriptor_t *descriptor = nand_dispatch_descriptor(task->descriptor_id);
                    task->descriptor_id = (descriptor != NULL) ? (uint16_t)descriptor->next_id : FC_DESCRIPTOR_ID_INVALID;
                    task->descriptor_index++;
                    task->operation = OP_RANDOM_DATA_READ;
                    task->started = 1U;
                    result = nand_dispatch_task_request_prepare(task, &task->request);
                    if (result != NAND_RESULT_OK)
                    {
                        nand_completion = (nand_fc_completion_t){result, 0U};
                        continue;
                    }
                    result = nand_fc_start(&task->transaction, task->operation, &task->request);
                    if (result == NAND_RESULT_NOT_READY)
                    {
                        break;
                    }
                    nand_completion.execution_status = result;
                    nand_completion.nand_status      = task->transaction.completion.nand_status;
                }
                if (result == NAND_RESULT_NOT_READY)
                {
                    continue;
                }
                *completion = task->completion;
                task->command_id = FC_COMMAND_HANDLE_INVALID;
                g_nand_channel_cursor = (uint8_t)((channel + 1U) % CH_NUM);
                return result;
            }
        }
        if (queue->count != 0U)
        {
            uint16_t command_id = queue->entries[queue->head];
            fc_command_t *command = nand_dispatch_command(command_id);
            nand_dispatch_task_t *task;
            nand_fc_completion_t nand_completion;
            if (command == NULL)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            task = &g_nand_dispatch_tasks[channel][command->config.ce][command->config.lun];
            if (task->command_id != FC_COMMAND_HANDLE_INVALID)
            {
                continue;
            }
            queue->head = (uint16_t)((queue->head + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
            queue->count--;
            nand_dispatch_completion_init(&task->completion, command);
            task->command_id = command_id;
            task->descriptor_id = (uint16_t)command->desc_id;
            task->descriptor_index = 0U;
            task->started = 0U;
            task->operation = (uint8_t)command->config.operation;
            if (nand_dispatch_task_request_prepare(task, &task->request) != NAND_RESULT_OK)
            {
                task->command_id = FC_COMMAND_HANDLE_INVALID;
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            SSD_LOGD("nand: FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n", task->operation, (void *)task->request.data, task->request.data_length, task->request.column, (unsigned long)task->request.row);
            result = nand_fc_start(&task->transaction, task->operation, &task->request);
            task->started = 1U;
            if (result == NAND_RESULT_NOT_READY)
            {
                continue;
            }
            nand_completion.execution_status = result;
            nand_completion.nand_status      = task->transaction.completion.nand_status;
            nand_dispatch_task_complete(task, result, &nand_completion);
            *completion = task->completion;
            task->command_id = FC_COMMAND_HANDLE_INVALID;
            return result;
        }
    }
    return NAND_RESULT_NOT_READY;
}
