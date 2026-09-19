#include "fc.h"

#include "fc_queue.h"
#include "fc_registers.h"
#include "fc_scheduler.h"
#include "nand/nand_dispatch.h"

#include <stddef.h>
#include <string.h>

/* FC 硬件模型仅属于实现层；FTL 不能取得或修改该控制器上下文。 */
typedef enum
{
    FC_COMMAND_STATE_FREE = 0U,
    FC_COMMAND_STATE_ALLOCATED,
    FC_COMMAND_STATE_QUEUED,
    FC_COMMAND_STATE_ACTIVE,
    FC_COMMAND_STATE_COMPLETED
} fc_command_state_t;

typedef struct
{
    fc_registers_t        registers;
    fc_submission_queue_t sq;
    fc_submission_queue_t hsq;
    fc_completion_queue_t cq;
    fc_scheduler_t        scheduler;
    fc_command_t         *command_pool;
    fc_descriptor_t      *descriptor_pool;
    uint8_t (*buffer_pool)[FC_BUFFER_POOL_CW_SIZE];
    fc_command_handle *descriptor_owner;
    uint8_t           *command_state;
    fc_command_handle *command_free_next;
    uint16_t          *descriptor_free_next;
    uint16_t          *descriptor_owned_next;
    uint16_t          *buffer_free_next;
    uint8_t           *buffer_state;
    uint16_t          *command_descriptor_head;
    fc_command_handle  command_free_head;
    uint16_t           descriptor_free_head;
    uint16_t           buffer_free_head;
    fc_command_handle  active_handle;
    uint8_t            active_valid;
} fc_t;

/* fc_complete_from_nand() is defined before the internal completion implementation. */
static fc_status_t fc_complete_internal(fc_t *controller, fc_command_status_t *completion);

/*
 * FC 控制面固定资源区。当前位于默认 .bss（AXI SRAM）；不使用动态内存。
 * 后续根据 DMA/Cache 和 Buffer Pool 设计，可将各数组分别放入指定链接段。
 */
static fc_command_t        g_fc_command_pool[FC_COMMAND_POOL_DEPTH];
static fc_descriptor_t     g_fc_descriptor_pool[FC_DESCRIPTOR_POOL_DEPTH];
static uint8_t             g_fc_buffer_pool[FC_BUFFER_POOL_DEPTH][FC_BUFFER_POOL_CW_SIZE];
static fc_command_handle   g_fc_sq_storage[FC_SQ_DEPTH];
static fc_command_handle   g_fc_hsq_storage[FC_HSQ_DEPTH];
static fc_command_status_t g_fc_cq_storage[FC_CQ_DEPTH];
static fc_command_handle   g_fc_descriptor_owner[FC_DESCRIPTOR_POOL_DEPTH];
static uint8_t             g_fc_command_state[FC_COMMAND_POOL_DEPTH];
static fc_command_handle   g_fc_command_free_next[FC_COMMAND_POOL_DEPTH];
static uint16_t            g_fc_descriptor_free_next[FC_DESCRIPTOR_POOL_DEPTH];
static uint16_t            g_fc_descriptor_owned_next[FC_DESCRIPTOR_POOL_DEPTH];
static uint16_t            g_fc_buffer_free_next[FC_BUFFER_POOL_DEPTH];
static uint8_t             g_fc_buffer_state[FC_BUFFER_POOL_DEPTH];
static uint16_t            g_fc_command_descriptor_head[FC_COMMAND_POOL_DEPTH];
static fc_t                g_fc_controller;

static uint8_t fc_command_handle_is_valid(fc_command_handle command_handle)
{
    return (uint8_t)(command_handle < FC_COMMAND_POOL_DEPTH);
}

static uint8_t fc_buffer_id_is_valid(const fc_t *controller, fc_buffer_id_t buffer_id)
{
    return (uint8_t)((controller != NULL) && (buffer_id < FC_BUFFER_POOL_DEPTH) &&
                     (controller->buffer_state[buffer_id] != 0U));
}

static uint8_t *fc_buffer_id_to_real_buffer_internal(fc_t *controller, fc_buffer_id_t buffer_id)
{
    if (fc_buffer_id_is_valid(controller, buffer_id) == 0U)
    {
        return NULL;
    }
    return controller->buffer_pool[buffer_id];
}

static fc_buffer_id_t fc_real_buffer_to_buffer_id_internal(fc_t *controller, const uint8_t *buffer)
{
    uint16_t index;

    if ((controller == NULL) || (buffer == NULL))
    {
        return FC_BUFFER_ID_INVALID;
    }
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (buffer == controller->buffer_pool[index])
        {
            return (fc_buffer_id_t)index;
        }
    }
    return FC_BUFFER_ID_INVALID;
}

static fc_status_t fc_buffer_release_internal(fc_t *controller, fc_buffer_id_t buffer_id)
{
    if (fc_buffer_id_is_valid(controller, buffer_id) == 0U)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    controller->buffer_state[buffer_id]     = 0U;
    controller->buffer_free_next[buffer_id] = controller->buffer_free_head;
    controller->buffer_free_head            = buffer_id;
    return FC_STATUS_OK;
}

static uint8_t fc_command_to_handle(const fc_t *controller, const fc_command_t *command, fc_command_handle *command_handle)
{
    uint16_t index;

    if ((controller == NULL) || (command == NULL) || (command_handle == NULL))
    {
        return 0U;
    }
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (command == &controller->command_pool[index])
        {
            *command_handle = (fc_command_handle)index;
            return 1U;
        }
    }
    return 0U;
}

static uint8_t fc_command_descriptor_count(const fc_t *controller, const fc_command_t *command)
{
    return ((controller != NULL) && (command != NULL) && (command->desc_id != FC_DESCRIPTOR_ID_INVALID)) ? (uint8_t)(command->config.cw + 1U) : 0U;
}

static uint8_t fc_command_descriptors_owned(const fc_t *controller, fc_command_handle command_handle, const fc_command_t *command)
{
    fc_descriptor_id_t     descriptor_id = (fc_descriptor_id_t)command->desc_id;
    const fc_descriptor_t *descriptor;
    uint8_t                descriptor_count;
    uint16_t               pool_index;
    uint8_t                found;

    for (descriptor_count = 0U; descriptor_count < fc_command_descriptor_count(controller, command); descriptor_count++)
    {
        found = 0U;
        pool_index = descriptor_id;
        if (pool_index >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return 0U;
        }
        descriptor = &controller->descriptor_pool[pool_index];
        if (controller->descriptor_owner[pool_index] == command_handle)
        {
            found = 1U;
        }
        if (found == 0U)
        {
            return 0U;
        }
        descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
    }
    return 1U;
}

static uint8_t fc_cq_contains_command(const fc_t *controller, const fc_command_t *command)
{
    uint16_t index = controller->cq.head;
    uint16_t count;

    for (count = 0U; count < controller->cq.count; count++)
    {
        if ((command != NULL) && (controller->cq.entries[index].command_id == command->command_id))
        {
            return 1U;
        }
        index = (uint16_t)((index + 1U) % controller->cq.depth);
    }
    return 0U;
}

static void fc_register_queue_state(fc_t *controller)
{
    uint32_t status = controller->registers.status & ~(FC_STATUS_READY | FC_STATUS_BUSY | FC_STATUS_SQ_NOT_EMPTY | FC_STATUS_HSQ_NOT_EMPTY | FC_STATUS_CQ_NOT_EMPTY);
    uint16_t index;
    uint8_t  active_found = 0U;

    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (controller->command_state[index] == FC_COMMAND_STATE_ACTIVE)
        {
            active_found = 1U;
            break;
        }
    }
    if (active_found != 0U)
    {
        status |= FC_STATUS_BUSY;
    }
    else
    {
        status |= FC_STATUS_READY;
    }
    if (controller->sq.count != 0U)
    {
        status |= FC_STATUS_SQ_NOT_EMPTY;
    }
    if (controller->hsq.count != 0U)
    {
        status |= FC_STATUS_HSQ_NOT_EMPTY;
    }
    if (controller->cq.count != 0U)
    {
        status |= FC_STATUS_CQ_NOT_EMPTY;
    }

    controller->registers.status   = status;
    controller->registers.sq_head  = controller->sq.head;
    controller->registers.sq_tail  = controller->sq.tail;
    controller->registers.hsq_head = controller->hsq.head;
    controller->registers.hsq_tail = controller->hsq.tail;
    controller->registers.cq_head  = controller->cq.head;
    controller->registers.cq_tail  = controller->cq.tail;
}

static fc_status_t fc_init_internal(fc_t *controller)
{
    fc_status_t result;
    uint16_t    index;

    if (controller == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    memset(g_fc_command_pool, 0, sizeof(g_fc_command_pool));
    memset(g_fc_descriptor_pool, 0, sizeof(g_fc_descriptor_pool));
    memset(g_fc_buffer_pool, 0, sizeof(g_fc_buffer_pool));
    memset(g_fc_sq_storage, 0, sizeof(g_fc_sq_storage));
    memset(g_fc_hsq_storage, 0, sizeof(g_fc_hsq_storage));
    memset(g_fc_cq_storage, 0, sizeof(g_fc_cq_storage));
    memset(g_fc_command_state, 0, sizeof(g_fc_command_state));
    *controller                         = (fc_t){0};
    controller->command_pool            = g_fc_command_pool;
    controller->descriptor_pool         = g_fc_descriptor_pool;
    controller->buffer_pool             = g_fc_buffer_pool;
    controller->descriptor_owner        = g_fc_descriptor_owner;
    controller->command_state           = g_fc_command_state;
    controller->command_free_next       = g_fc_command_free_next;
    controller->descriptor_free_next    = g_fc_descriptor_free_next;
    controller->descriptor_owned_next   = g_fc_descriptor_owned_next;
    controller->buffer_free_next        = g_fc_buffer_free_next;
    controller->buffer_state            = g_fc_buffer_state;
    controller->command_descriptor_head = g_fc_command_descriptor_head;
    fc_registers_reset(&controller->registers);
    result = fc_submission_queue_init(&controller->sq, g_fc_sq_storage, FC_SQ_DEPTH);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    result = fc_submission_queue_init(&controller->hsq, g_fc_hsq_storage, FC_HSQ_DEPTH);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    result = fc_completion_queue_init(&controller->cq, g_fc_cq_storage, FC_CQ_DEPTH);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    result = fc_scheduler_init(&controller->scheduler, &controller->sq, &controller->hsq);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    if (nand_dispatch_init() != NAND_RESULT_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->command_free_next[index]       = ((index + 1U) < FC_COMMAND_POOL_DEPTH) ? (fc_command_handle)(index + 1U) : FC_COMMAND_HANDLE_INVALID;
        controller->command_descriptor_head[index] = UINT16_MAX;
        controller->command_pool[index].command_id = index;
    }
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        controller->descriptor_owner[index]      = FC_COMMAND_HANDLE_INVALID;
        controller->descriptor_free_next[index]  = ((index + 1U) < FC_DESCRIPTOR_POOL_DEPTH) ? (uint16_t)(index + 1U) : UINT16_MAX;
        controller->descriptor_owned_next[index] = UINT16_MAX;
    }
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        controller->buffer_free_next[index] = ((index + 1U) < FC_BUFFER_POOL_DEPTH) ? (uint16_t)(index + 1U) : UINT16_MAX;
        controller->buffer_state[index]     = 0U;
    }
    controller->command_free_head    = 0U;
    controller->descriptor_free_head = 0U;
    controller->buffer_free_head     = 0U;
    controller->active_handle        = FC_COMMAND_HANDLE_INVALID;
    fc_register_queue_state(controller);
    return FC_STATUS_OK;
}

static fc_command_t *fc_command_alloc_internal(fc_t *controller)
{
    fc_command_handle command_handle;

    if (controller == NULL)
    {
        return NULL;
    }
    command_handle = controller->command_free_head;
    if (command_handle == FC_COMMAND_HANDLE_INVALID)
    {
        return NULL;
    }
    controller->command_free_head             = controller->command_free_next[command_handle];
    controller->command_pool[command_handle]  = (fc_command_t){0};
    controller->command_pool[command_handle].command_id = command_handle;
    controller->command_pool[command_handle].desc_id = FC_DESCRIPTOR_ID_INVALID;
    controller->command_state[command_handle] = FC_COMMAND_STATE_ALLOCATED;
    return &controller->command_pool[command_handle];
}

static fc_descriptor_t *fc_descriptor_alloc_internal(fc_t *controller, fc_command_t *command)
{
    uint16_t          descriptor_index;
    fc_command_handle command_handle;

    if ((fc_command_to_handle(controller, command, &command_handle) == 0U) ||
        (controller->command_state[command_handle] != FC_COMMAND_STATE_ALLOCATED))
    {
        return NULL;
    }
    descriptor_index = controller->descriptor_free_head;
    if (descriptor_index == UINT16_MAX)
    {
        return NULL;
    }
    controller->descriptor_free_head                    = controller->descriptor_free_next[descriptor_index];
    controller->descriptor_pool[descriptor_index]       = (fc_descriptor_t){0};
    controller->descriptor_owner[descriptor_index]      = command_handle;
    controller->descriptor_owned_next[descriptor_index] = controller->command_descriptor_head[command_handle];
    controller->command_descriptor_head[command_handle] = descriptor_index;
    return &controller->descriptor_pool[descriptor_index];
}

static fc_buffer_id_t fc_buffer_alloc_internal(fc_t *controller)
{
    uint16_t buffer_index;

    if (controller == NULL)
    {
        return FC_BUFFER_ID_INVALID;
    }
    buffer_index = controller->buffer_free_head;
    if (buffer_index == UINT16_MAX)
    {
        return FC_BUFFER_ID_INVALID;
    }
    controller->buffer_free_head           = controller->buffer_free_next[buffer_index];
    controller->buffer_state[buffer_index] = 1U;
    return (fc_buffer_id_t)buffer_index;
}

static fc_status_t fc_buffer_release_internal_api(fc_t *controller, fc_buffer_id_t buffer_id)
{
    if (controller == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_buffer_release_internal(controller, buffer_id);
}

static void fc_command_resources_rollback(fc_t *controller, fc_command_handle command_handle)
{
    uint16_t descriptor_index = controller->command_descriptor_head[command_handle];

    while (descriptor_index != UINT16_MAX)
    {
        const uint16_t       next_descriptor_index = controller->descriptor_owned_next[descriptor_index];
        const fc_buffer_id_t buffer_id             = controller->descriptor_pool[descriptor_index].buffer_id;

        if (buffer_id != FC_BUFFER_ID_INVALID)
        {
            (void)fc_buffer_release_internal(controller, buffer_id);
        }
        controller->descriptor_pool[descriptor_index]       = (fc_descriptor_t){0};
        controller->descriptor_owner[descriptor_index]      = FC_COMMAND_HANDLE_INVALID;
        controller->descriptor_owned_next[descriptor_index] = UINT16_MAX;
        controller->descriptor_free_next[descriptor_index]  = controller->descriptor_free_head;
        controller->descriptor_free_head                    = descriptor_index;
        descriptor_index                                    = next_descriptor_index;
    }
    controller->command_descriptor_head[command_handle] = UINT16_MAX;
}

static fc_status_t fc_command_configure_descriptor_internal(fc_t *controller, fc_command_t *command, uint8_t index,
                                                             fc_descriptor_t **configured_descriptor)
{
    fc_command_handle command_handle;
    fc_descriptor_t  *descriptor;
    fc_buffer_id_t    buffer_id;
    uint8_t           cw;

    if ((configured_descriptor == NULL) || (fc_command_to_handle(controller, command, &command_handle) == 0U) ||
        (controller->command_state[command_handle] != FC_COMMAND_STATE_ALLOCATED) ||
        (index > command->config.cw))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    descriptor = fc_descriptor_alloc_internal(controller, command);
    buffer_id  = fc_buffer_alloc_internal(controller);
    if ((descriptor == NULL) || (buffer_id == FC_BUFFER_ID_INVALID))
    {
        if (buffer_id != FC_BUFFER_ID_INVALID)
        {
            (void)fc_buffer_release_internal(controller, buffer_id);
        }
        return FC_STATUS_RESOURCE_EMPTY;
    }

    cw = (uint8_t)(paa_to_cw(command->paa) + index);
    if (cw >= CW_NUM)
    {
        (void)fc_buffer_release_internal(controller, buffer_id);
        return FC_STATUS_INVALID_ARGUMENT;
    }
    descriptor->paa          = (command->paa & ~NAND_FIELD_MASK(CW_WIDTH)) | cw;
    descriptor->config      = command->config;
    descriptor->config.cw   = cw;
    descriptor->buffer_id    = buffer_id;
    descriptor->next_id      = FC_DESCRIPTOR_ID_INVALID;
    *configured_descriptor = descriptor;
    return FC_STATUS_OK;
}

static fc_status_t fc_command_configure_descriptors_internal(fc_t *controller, fc_command_t *command)
{
    fc_command_handle command_handle;
    fc_descriptor_t  *previous = NULL;
    uint8_t           index;

    if ((fc_command_to_handle(controller, command, &command_handle) == 0U) ||
        (controller->command_state[command_handle] != FC_COMMAND_STATE_ALLOCATED) ||
        (command->desc_id != FC_DESCRIPTOR_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index <= command->config.cw; index++)
    {
        fc_descriptor_t *descriptor = NULL;
        fc_status_t      result     = fc_command_configure_descriptor_internal(controller, command, index, &descriptor);

        if (result != FC_STATUS_OK)
        {
            fc_command_resources_rollback(controller, command_handle);
            command->desc_id = FC_DESCRIPTOR_ID_INVALID;
            return result;
        }
        if (previous == NULL)
        {
            command->desc_id = (fc_descriptor_id_t)(descriptor - controller->descriptor_pool);
        }
        else
        {
            previous->config.chain = 1U;
            previous->next_id = (fc_descriptor_id_t)(descriptor - controller->descriptor_pool);
        }
        previous = descriptor;
    }
    return FC_STATUS_OK;
}

static fc_status_t fc_submit_to_queue(fc_t *controller, fc_submission_queue_t *queue, fc_command_t *command)
{
    fc_status_t       result;
    fc_command_handle command_handle;

    if ((fc_command_to_handle(controller, command, &command_handle) == 0U) ||
        (controller->command_state[command_handle] != FC_COMMAND_STATE_ALLOCATED))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    result = fc_command_validate(command);
    if ((result == FC_STATUS_OK) && (fc_command_descriptors_owned(controller, command_handle, command) == 0U))
    {
        result = FC_STATUS_INVALID_ARGUMENT;
    }
    if (result == FC_STATUS_OK)
    {
        result = fc_submission_queue_push(queue, command_handle);
        if (result == FC_STATUS_OK)
        {
            controller->command_state[command_handle] = FC_COMMAND_STATE_QUEUED;
        }
    }
    return result;
}

static fc_status_t fc_submit_internal(fc_t *controller, fc_command_t *command)
{
    fc_status_t result;

    if (controller == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    result = fc_submit_to_queue(controller, (command != NULL) && (command->high_priority != 0U) ? &controller->hsq : &controller->sq, command);
    fc_register_queue_state(controller);
    return result;
}

static fc_status_t fc_complete_from_nand(fc_t *controller, const nand_dispatch_completion_t *nand_completion)
{
    fc_command_status_t completion;
    uint8_t             cw;

    if ((nand_completion == NULL) || (nand_completion->command_id >= FC_COMMAND_POOL_DEPTH))
    {
        return FC_STATUS_IO_ERROR;
    }

    fc_command_status_init(&completion, fc_command_id_to_command(nand_completion->command_id));
    completion.command_id = nand_completion->command_id;
    completion.desc_id = nand_completion->failed_descriptor_id;
    for (cw = 0U; cw < 4U; cw++)
    {
        fc_command_status_set_cw(&completion, cw, nand_completion->cw_status[cw], nand_completion->failed_descriptor_id);
    }
    if (fc_complete_internal(controller, &completion) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    return FC_STATUS_OK;
}

static fc_status_t fc_poll_internal(fc_t *controller)
{
    fc_status_t                result;
    fc_command_handle          command_handle;
    nand_dispatch_completion_t nand_completion;

    if (controller == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    const uint8_t nand_result = nand_dispatch_poll(&nand_completion);

    if (nand_result != NAND_RESULT_NOT_READY)
    {
        if (fc_complete_from_nand(controller, &nand_completion) != FC_STATUS_OK)
        {
            return FC_STATUS_IO_ERROR;
        }
        fc_register_queue_state(controller);
        return (nand_result == NAND_RESULT_OK) ? FC_STATUS_OK : FC_STATUS_IO_ERROR;
    }
    result = fc_scheduler_select(&controller->scheduler, &command_handle);
    if (result == FC_STATUS_OK)
    {
        if ((fc_command_handle_is_valid(command_handle) == 0U) || (controller->command_state[command_handle] != FC_COMMAND_STATE_QUEUED))
        {
            return FC_STATUS_IO_ERROR;
        }
        controller->command_state[command_handle] = FC_COMMAND_STATE_ACTIVE;

        if (nand_dispatch_submit(&controller->command_pool[command_handle]) != NAND_RESULT_OK)
        {
            nand_completion                  = (nand_dispatch_completion_t){0};
            nand_completion.command_id       = command_handle;
            nand_completion.execution_status = NAND_RESULT_NOT_READY;
            nand_completion.cw_status[0]     = FC_CW_STATUS_NAND_FAIL;
            (void)fc_complete_from_nand(controller, &nand_completion);
            fc_register_queue_state(controller);
            return FC_STATUS_IO_ERROR;
        }

        /* Submission only occupies the target LUN. Completion is advanced by a later fc_service() call. */
        result = FC_STATUS_OK;
    }
    fc_register_queue_state(controller);
    return result;
}

static fc_status_t fc_complete_internal(fc_t *controller, fc_command_status_t *completion)
{
    fc_status_t       result;
    fc_command_t     *command;
    fc_command_handle command_handle;

    if ((controller == NULL) || (completion == NULL) ||
        (completion->command_id >= FC_COMMAND_POOL_DEPTH) ||
        (fc_command_to_handle(controller, &controller->command_pool[completion->command_id], &command_handle) == 0U) ||
        (controller->command_state[command_handle] != FC_COMMAND_STATE_ACTIVE))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    command = &controller->command_pool[command_handle];
    result = fc_completion_queue_push(&controller->cq, completion);
    if (result == FC_STATUS_OK)
    {
        controller->command_state[command_handle] = FC_COMMAND_STATE_COMPLETED;
    }
    fc_register_queue_state(controller);
    return result;
}

static fc_status_t fc_poll_completion_internal(fc_t *controller, fc_command_status_t *completion)
{
    fc_status_t result;

    if ((controller == NULL) || (completion == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    result = fc_completion_queue_pop(&controller->cq, completion);
    fc_register_queue_state(controller);
    return result;
}

static fc_status_t fc_command_release_internal_api(fc_t *controller, fc_command_t *command)
{
    uint16_t          descriptor_index;
    uint16_t          next_descriptor_index;
    fc_command_handle command_handle;

    if ((fc_command_to_handle(controller, command, &command_handle) == 0U) ||
        ((controller->command_state[command_handle] != FC_COMMAND_STATE_ALLOCATED) &&
         (controller->command_state[command_handle] != FC_COMMAND_STATE_COMPLETED)))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (fc_cq_contains_command(controller, command) != 0U)
    {
        return FC_STATUS_NOT_READY;
    }
    descriptor_index = controller->command_descriptor_head[command_handle];
    while (descriptor_index != UINT16_MAX)
    {
        next_descriptor_index                               = controller->descriptor_owned_next[descriptor_index];
        controller->descriptor_pool[descriptor_index]       = (fc_descriptor_t){0};
        controller->descriptor_owner[descriptor_index]      = FC_COMMAND_HANDLE_INVALID;
        controller->descriptor_owned_next[descriptor_index] = UINT16_MAX;
        controller->descriptor_free_next[descriptor_index]  = controller->descriptor_free_head;
        controller->descriptor_free_head                    = descriptor_index;
        descriptor_index                                    = next_descriptor_index;
    }
    controller->command_descriptor_head[command_handle] = UINT16_MAX;
    controller->command_pool[command_handle]            = (fc_command_t){0};
    controller->command_state[command_handle]           = FC_COMMAND_STATE_FREE;
    controller->command_free_next[command_handle]       = controller->command_free_head;
    controller->command_free_head                       = command_handle;
    return FC_STATUS_OK;
}

fc_status_t fc_init(void)
{
    return fc_init_internal(&g_fc_controller);
}

fc_command_t *fc_command_alloc(void)
{
    return fc_command_alloc_internal(&g_fc_controller);
}

fc_command_t *fc_command_id_to_command(uint16_t command_id)
{
    if (command_id >= FC_COMMAND_POOL_DEPTH)
    {
        return NULL;
    }
    return &g_fc_controller.command_pool[command_id];
}

fc_descriptor_t *fc_descriptor_id_to_descriptor(uint16_t descriptor_id)
{
    if (descriptor_id >= FC_DESCRIPTOR_POOL_DEPTH)
    {
        return NULL;
    }
    return &g_fc_controller.descriptor_pool[descriptor_id];
}

fc_status_t fc_command_configure_descriptors(fc_command_t *command)
{
    return fc_command_configure_descriptors_internal(&g_fc_controller, command);
}

fc_buffer_id_t fc_buffer_alloc(void)
{
    return fc_buffer_alloc_internal(&g_fc_controller);
}

fc_status_t fc_buffer_release(fc_buffer_id_t buffer_id)
{
    return fc_buffer_release_internal_api(&g_fc_controller, buffer_id);
}

uint8_t *fc_buffer_id_to_real_buffer(fc_buffer_id_t buffer_id)
{
    return fc_buffer_id_to_real_buffer_internal(&g_fc_controller, buffer_id);
}

fc_buffer_id_t fc_real_buffer_to_buffer_id(const uint8_t *buffer)
{
    return fc_real_buffer_to_buffer_id_internal(&g_fc_controller, buffer);
}

fc_status_t fc_submit(fc_command_t *command)
{
    return fc_submit_internal(&g_fc_controller, command);
}

fc_status_t fc_service(void)
{
    return fc_poll_internal(&g_fc_controller);
}

fc_status_t fc_poll_completion(fc_command_status_t *completion)
{
    return fc_poll_completion_internal(&g_fc_controller, completion);
}

fc_status_t fc_command_release(fc_command_t *command)
{
    return fc_command_release_internal_api(&g_fc_controller, command);
}

fc_status_t fc_get_resource_stats(fc_resource_stats_t *stats)
{
    uint16_t index;

    if (stats == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    stats->sq_used = g_fc_controller.sq.count;
    stats->hsq_used = g_fc_controller.hsq.count;
    stats->cq_used = g_fc_controller.cq.count;
    stats->command_used = 0U;
    stats->descriptor_used = 0U;
    stats->buffer_used = 0U;
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (g_fc_controller.command_state[index] != FC_COMMAND_STATE_FREE)
        {
            stats->command_used++;
        }
    }
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        if (g_fc_controller.descriptor_owner[index] != FC_COMMAND_HANDLE_INVALID)
        {
            stats->descriptor_used++;
        }
    }
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (g_fc_controller.buffer_state[index] != 0U)
        {
            stats->buffer_used++;
        }
    }
    return FC_STATUS_OK;
}
