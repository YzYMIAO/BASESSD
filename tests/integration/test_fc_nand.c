#include "test_fc_nand.h"

#include "common/ssd_log.h"
#include "delay.h"
#include "nand/nand_mt29f4g08abadawp.h"
#include "nand/onfi_1_0.h"

#include <stddef.h>

#define FC_NAND_TEST_MANUFACTURER_ID (0x2CU)
#define FC_NAND_TEST_DEVICE_ID       (0xDCU)
#define FC_NAND_TEST_SERVICE_LIMIT   (5000000U)
#define FC_NAND_TEST_PROGRESS_PAGES  (8U)
#define FC_NAND_TEST_PAGE_WINDOW     (24U)
#define FC_NAND_TEST_STATS_INTERVAL  (8U)

typedef struct
{
    fc_command_t *command;
    uint32_t      page;
    uint8_t       active;
} fc_nand_test_slot_t;

static void fc_nand_test_log_resource_stats(const char *phase, uint32_t completed,
                                            uint32_t next_page, uint32_t outstanding)
{
    fc_resource_stats_t stats;

    if (fc_get_resource_stats(&stats) != FC_STATUS_OK)
    {
        return;
    }
    SSD_LOGI("[FC TEST] pool %s done=%lu next=%lu inflight=%lu SQ=%u/%u CQ=%u/%u CMD=%u/%u DESC=%u/%u BUF=%u/%u\r\n",
             phase, (unsigned long)completed, (unsigned long)next_page,
             (unsigned long)outstanding, stats.sq_used, (unsigned int)FC_SQ_DEPTH,
             stats.cq_used, (unsigned int)FC_CQ_DEPTH,
             stats.command_used, (unsigned int)FC_COMMAND_POOL_DEPTH, stats.descriptor_used,
             (unsigned int)FC_DESCRIPTOR_POOL_DEPTH, stats.buffer_used,
             (unsigned int)FC_BUFFER_POOL_DEPTH);
}

static uint8_t fc_nand_test_pattern_byte(uint16_t index)
{
    (void)index;
    return 0x55U;
}

static fc_descriptor_t *fc_nand_test_descriptor(fc_descriptor_id_t descriptor_id)
{
    return fc_descriptor_id_to_descriptor(descriptor_id);
}

static uint8_t fc_nand_test_completion_is_success(const fc_command_status_t *completion, uint8_t cw_count)
{
    if ((completion == NULL) || (cw_count == 0U) || (cw_count > CW_NUM) ||
        (completion->status0 != FC_CW_STATUS_SUCCESS))
    {
        return 0U;
    }
    if ((cw_count > 1U) && (completion->status1 != FC_CW_STATUS_SUCCESS))
    {
        return 0U;
    }
    if ((cw_count > 2U) && (completion->status2 != FC_CW_STATUS_SUCCESS))
    {
        return 0U;
    }
    if ((cw_count > 3U) && (completion->status3 != FC_CW_STATUS_SUCCESS))
    {
        return 0U;
    }
    return 1U;
}

/* The caller selects the target LUN/Plane/Block; block tests always start at CW0 of each Page. */
static nand_paa_t fc_nand_test_page_paa(nand_paa_t block_paa, uint32_t page)
{
    return paa_make(page, paa_to_plane(block_paa), paa_to_block(block_paa), paa_to_lun(block_paa), 0U);
}

static fc_status_t fc_nand_test_wait_completion(fc_command_t *command)
{
    fc_command_status_t completion;
    fc_status_t         result;
    fc_status_t         service_result;
    uint32_t            service_count;

    result = fc_submit(command);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    for (service_count = 0U; service_count < FC_NAND_TEST_SERVICE_LIMIT; service_count++)
    {
        service_result = fc_service();
        result = fc_poll_completion(&completion);
        if ((service_count != 0U) && ((service_count % 100000U) == 0U))
        {
            SSD_LOGI("[FC TEST] wait service=%lu service_result=%d poll_result=%d\r\n",
                     (unsigned long)service_count, (int)service_result, (int)result);
        }
        if (result == FC_STATUS_OK)
        {
            SSD_LOGD("[FC TEST] completion cmd=0x%04X desc=0x%04X status0=0x%02X status1=0x%02X status2=0x%02X status3=0x%02X\r\n",
                     completion.command_id, completion.desc_id, completion.status0, completion.status1,
                     completion.status2, completion.status3);
            if ((completion.command_id != command->command_id) ||
                (fc_nand_test_completion_is_success(&completion, (uint8_t)(command->config.cw + 1U)) == 0U))
            {
                SSD_LOGE("[FC TEST] completion failed: status0=0x%02X status1=0x%02X desc=0x%04X\r\n",
                         completion.status0, completion.status1, completion.desc_id);
                return FC_STATUS_IO_ERROR;
            }
            return FC_STATUS_OK;
        }
        if ((service_result != FC_STATUS_OK) && (service_result != FC_STATUS_NOT_READY) &&
            (service_result != FC_STATUS_QUEUE_EMPTY))
        {
            SSD_LOGE("[FC TEST] service failed: result=%d cmd=0x%04X desc=0x%04X op=%u ch=%u ce=%u lun=%u cw=%u\r\n",
                     (int)service_result, (unsigned int)command->command_id, (unsigned int)command->desc_id,
                     (unsigned int)command->config.operation, (unsigned int)command->config.ch,
                     (unsigned int)command->config.ce, (unsigned int)command->config.lun,
                     (unsigned int)command->config.cw);
            return service_result;
        }
        if ((result != FC_STATUS_QUEUE_EMPTY) && (result != FC_STATUS_NOT_READY))
        {
            return result;
        }
        delay_us(1U);
    }
    return FC_STATUS_NOT_READY;
}

static void fc_nand_test_command_configure(fc_command_t *command, uint8_t operation, nand_paa_t paa, uint8_t descriptor_count)
{
    /* fc_command_alloc() returns a zeroed slot; an unlinked command must use the invalid ID. */
    command->desc_id       = FC_DESCRIPTOR_ID_INVALID;
    command->config.operation = operation;
    command->config.ch        = 0U;
    command->config.ce        = 0U;
    command->config.lun       = 0U;
    command->paa              = paa;
    command->config.cw        = (descriptor_count > 0U) ? (uint8_t)(descriptor_count - 1U) : 0U;
    command->config.crc       = 0U; /* CRC enabled; set to 1 only to disable it. */
    command->config.ldpc      = FC_LDPC_FULL; /* Full LDPC path enabled. */
    command->config.raid      = 0U; /* Normal RAID path; nonzero selects a RAID operation. */
    command->config.random    = 0U; /* Randomizer enabled; set to 1 only to disable it. */
    command->config.chain     = 0U; /* This test submits one standalone command. */
    command->config.page_type = 0U; /* LP page for the current SLC test device. */
    command->module           = FC_COMMAND_MODULE_FTL;
}

static fc_status_t fc_nand_test_release_command(fc_command_t *command)
{
    fc_status_t result;

    result = fc_command_release(command);
    return result;
}

static void fc_nand_test_release_command_resources(fc_command_t *command)
{
    fc_descriptor_id_t descriptor_id;

    if (command == NULL)
    {
        return;
    }
    descriptor_id = (fc_descriptor_id_t)command->desc_id;
    while (descriptor_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
        fc_descriptor_id_t next_id;

        if (descriptor == NULL)
        {
            break;
        }
        next_id = (fc_descriptor_id_t)descriptor->next_id;
        (void)fc_buffer_release((fc_buffer_id_t)descriptor->buffer_id);
        descriptor_id = next_id;
    }
    (void)fc_nand_test_release_command(command);
}

static fc_status_t fc_nand_test_prepare_page_command(uint8_t operation, nand_paa_t paa,
                                                     fc_nand_test_slot_t *slot)
{
    fc_command_t *command;
    fc_status_t   result;

    command = fc_command_alloc();
    if (command == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    fc_nand_test_command_configure(command, operation, paa, CW_NUM);
    result = fc_command_configure_descriptors(command);
    if (result != FC_STATUS_OK)
    {
        fc_nand_test_release_command_resources(command);
        return result;
    }
    if (operation == OP_NORMAL_PROGRAM_PAGE)
    {
        fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id;
        while (descriptor_id != FC_DESCRIPTOR_ID_INVALID)
        {
            fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
            uint8_t *buffer;
            uint16_t index;
            fc_descriptor_id_t next_id;

            if (descriptor == NULL)
            {
                fc_nand_test_release_command_resources(command);
                return FC_STATUS_IO_ERROR;
            }
            next_id = (fc_descriptor_id_t)descriptor->next_id;
            buffer = fc_buffer_id_to_real_buffer((fc_buffer_id_t)descriptor->buffer_id);
            if (buffer == NULL)
            {
                fc_nand_test_release_command_resources(command);
                return FC_STATUS_IO_ERROR;
            }
            for (index = 0U; index < FC_BUFFER_POOL_CW_SIZE; index++)
            {
                buffer[index] = fc_nand_test_pattern_byte(index);
            }
            descriptor_id = next_id;
        }
    }
    result = fc_submit(command);
    if (result != FC_STATUS_OK)
    {
        fc_nand_test_release_command_resources(command);
        return result;
    }
    slot->command = command;
    slot->active  = 1U;
    return FC_STATUS_OK;
}

static fc_nand_test_slot_t *fc_nand_test_find_slot(fc_nand_test_slot_t *slots,
                                                   uint32_t slot_count, uint16_t command_id)
{
    uint32_t index;

    for (index = 0U; index < slot_count; index++)
    {
        if ((slots[index].active != 0U) && (slots[index].command != NULL) &&
            (slots[index].command->command_id == command_id))
        {
            return &slots[index];
        }
    }
    return NULL;
}

static fc_status_t fc_nand_test_process_batch_completion(const fc_command_status_t *completion,
                                                          fc_nand_test_slot_t *slots,
                                                          uint32_t slot_count, uint8_t operation)
{
    fc_nand_test_slot_t *slot;
    fc_command_t        *command;
    fc_descriptor_id_t   descriptor_id;
    uint16_t              index;

    slot = fc_nand_test_find_slot(slots, slot_count, (uint16_t)completion->command_id);
    if (slot == NULL)
    {
        return FC_STATUS_IO_ERROR;
    }
    command = slot->command;
    if (fc_nand_test_completion_is_success(completion, (uint8_t)(command->config.cw + 1U)) == 0U)
    {
        fc_nand_test_release_command_resources(command);
        slot->command = NULL;
        slot->active  = 0U;
        return FC_STATUS_IO_ERROR;
    }
    if (operation == OP_NORMAL_READ_PAGE)
    {
        descriptor_id = (fc_descriptor_id_t)command->desc_id;
        while (descriptor_id != FC_DESCRIPTOR_ID_INVALID)
        {
            fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
            const uint32_t *words;
            fc_descriptor_id_t next_id;

            if (descriptor == NULL)
            {
                fc_nand_test_release_command_resources(command);
                slot->command = NULL;
                slot->active  = 0U;
                return FC_STATUS_IO_ERROR;
            }
            next_id = (fc_descriptor_id_t)descriptor->next_id;
            words = (const uint32_t *)(uintptr_t)fc_buffer_id_to_real_buffer((fc_buffer_id_t)descriptor->buffer_id);
            if (words == NULL)
            {
                fc_nand_test_release_command_resources(command);
                slot->command = NULL;
                slot->active  = 0U;
                return FC_STATUS_IO_ERROR;
            }
            for (index = 0U; index < (FC_BUFFER_POOL_CW_SIZE / sizeof(uint32_t)); index++)
            {
                if (words[index] != 0x55555555UL)
                {
                    SSD_LOGE("[FC TEST] READ mismatch: page=%lu cw=%u word=%u actual=%08lX expected=55555555\r\n",
                             (unsigned long)slot->page, descriptor->config.cw, index,
                             (unsigned long)words[index]);
                    fc_nand_test_release_command_resources(command);
                    slot->command = NULL;
                    slot->active  = 0U;
                    return FC_STATUS_IO_ERROR;
                }
            }
            descriptor_id = next_id;
        }
    }
    fc_nand_test_release_command_resources(command);
    slot->command = NULL;
    slot->active  = 0U;
    return FC_STATUS_OK;
}

static fc_status_t fc_nand_test_run_block_pipeline(nand_paa_t test_paa, uint8_t operation,
                                                    const char *name)
{
    fc_nand_test_slot_t slots[FC_NAND_TEST_PAGE_WINDOW] = {0};
    fc_command_status_t completion;
    uint32_t next_page = 0U;
    uint32_t completed = 0U;
    uint32_t outstanding = 0U;
    uint32_t service_count = 0U;
    fc_status_t result;

    SSD_LOGI("[FC TEST] %s begin, block=%lu plane=%lu lun=%lu pages=%u window=%u\r\n", name,
             (unsigned long)paa_to_block(test_paa), (unsigned long)paa_to_plane(test_paa),
             (unsigned long)paa_to_lun(test_paa), (unsigned int)PAGES_PER_BLOCK,
             (unsigned int)FC_NAND_TEST_PAGE_WINDOW);
    fc_nand_test_log_resource_stats("begin", completed, next_page, outstanding);
    while (completed < PAGES_PER_BLOCK)
    {
        uint32_t index;
        uint8_t submitted = 0U;

        for (index = 0U; index < FC_NAND_TEST_PAGE_WINDOW && next_page < PAGES_PER_BLOCK; index++)
        {
            if (slots[index].active == 0U)
            {
                result = fc_nand_test_prepare_page_command(operation,
                                                            fc_nand_test_page_paa(test_paa, next_page),
                                                            &slots[index]);
                if (result == FC_STATUS_OK)
                {
                    slots[index].page = next_page++;
                    outstanding++;
                    submitted = 1U;
                    SSD_LOGD("[FC TEST] %s page=%lu submitted inflight=%lu\r\n", name,
                             (unsigned long)slots[index].page, (unsigned long)outstanding);
                }
                else if (result != FC_STATUS_RESOURCE_EMPTY)
                {
                    SSD_LOGE("[FC TEST] %s submit failed: page=%lu result=%d\r\n", name,
                             (unsigned long)next_page, (int)result);
                    goto fail;
                }
            }
        }
        if ((submitted != 0U) && (next_page == FC_NAND_TEST_PAGE_WINDOW))
        {
            fc_nand_test_log_resource_stats("window", completed, next_page, outstanding);
        }
        result = fc_service();
        service_count++;
        if ((result != FC_STATUS_OK) && (result != FC_STATUS_NOT_READY) &&
            (result != FC_STATUS_QUEUE_EMPTY))
        {
            SSD_LOGE("[FC TEST] %s service failed: result=%d\r\n", name, (int)result);
            goto fail;
        }
        while (fc_poll_completion(&completion) == FC_STATUS_OK)
        {
            fc_nand_test_slot_t *completed_slot = fc_nand_test_find_slot(
                slots, FC_NAND_TEST_PAGE_WINDOW, (uint16_t)completion.command_id);
            uint32_t completed_page;

            if (completed_slot == NULL)
            {
                SSD_LOGE("[FC TEST] %s completion command not found: cmd=0x%04X\r\n", name,
                         (unsigned int)completion.command_id);
                result = FC_STATUS_IO_ERROR;
                goto fail;
            }
            completed_page = completed_slot->page;
            result = fc_nand_test_process_batch_completion(&completion, slots,
                                                            FC_NAND_TEST_PAGE_WINDOW, operation);
            if (result != FC_STATUS_OK)
            {
                SSD_LOGE("[FC TEST] %s completion failed: page result=%d\r\n", name, (int)result);
                goto fail;
            }
            completed++;
            outstanding--;
            SSD_LOGI("[FC TEST] %s page=%lu completed inflight=%lu\r\n", name,
                     (unsigned long)completed_page, (unsigned long)outstanding);
            if ((completed % FC_NAND_TEST_PROGRESS_PAGES) == 0U || completed == PAGES_PER_BLOCK)
            {
                SSD_LOGI("[FC TEST] %s progress: %lu/%u pages\r\n", name,
                         (unsigned long)completed, (unsigned int)PAGES_PER_BLOCK);
            }
            if ((completed % FC_NAND_TEST_STATS_INTERVAL) == 0U || completed == PAGES_PER_BLOCK)
            {
                fc_nand_test_log_resource_stats("reclaim", completed, next_page, outstanding);
            }
        }
        if ((submitted == 0U) && (outstanding == 0U) && (completed < PAGES_PER_BLOCK))
        {
            if (service_count >= FC_NAND_TEST_SERVICE_LIMIT)
            {
                result = FC_STATUS_NOT_READY;
                goto fail;
            }
            delay_us(1U);
        }
    }
    fc_nand_test_log_resource_stats("end", completed, next_page, outstanding);
    SSD_LOGI("[FC TEST] %s passed\r\n", name);
    return FC_STATUS_OK;

fail:
    for (uint32_t index = 0U; index < FC_NAND_TEST_PAGE_WINDOW; index++)
    {
        if (slots[index].active != 0U)
        {
            fc_nand_test_release_command_resources(slots[index].command);
            slots[index].active = 0U;
        }
    }
    return result;
}

fc_status_t fc_nand_test_read_id(void)
{
    const uint8_t *id;
    fc_command_t  *command;
    uint8_t       *buffer;
    fc_status_t    result;

    command = fc_command_alloc();
    if (command == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    SSD_LOGI("[FC TEST] READ_ID submit\r\n");
    fc_nand_test_command_configure(command, OP_READ_ID, 0U, 1U);
    result = fc_command_configure_descriptors(command);
    if (result != FC_STATUS_OK)
    {
        (void)fc_command_release(command);
        return result;
    }
    buffer = fc_buffer_id_to_real_buffer(fc_nand_test_descriptor((fc_descriptor_id_t)command->desc_id)->buffer_id);
    if (buffer == NULL)
    {
        return FC_STATUS_IO_ERROR;
    }
    result = fc_nand_test_wait_completion(command);
    if (result == FC_STATUS_OK)
    {
        id = buffer;
        SSD_LOGI("[FC TEST] READ_ID=%02X %02X %02X %02X %02X\r\n", id[0], id[1], id[2], id[3], id[4]);
        if ((id[0] != FC_NAND_TEST_MANUFACTURER_ID) || (id[1] != FC_NAND_TEST_DEVICE_ID))
        {
            result = FC_STATUS_IO_ERROR;
        }
    }
    for (fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id; descriptor_id != FC_DESCRIPTOR_ID_INVALID; )
    {
        fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
        descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
        if (fc_buffer_release(descriptor->buffer_id) != FC_STATUS_OK)
        {
            return FC_STATUS_IO_ERROR;
        }
    }
    if (fc_nand_test_release_command(command) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    return result;
}

fc_status_t fc_nand_test_erase_block(nand_paa_t test_paa)
{
    fc_command_t *command;
    fc_status_t   result;

    command = fc_command_alloc();
    if (command == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    SSD_LOGI("[FC TEST] ERASE submit, paa=0x%08lX\r\n", (unsigned long)test_paa);
    fc_nand_test_command_configure(command, OP_ERASE_BLOCK, test_paa, 1U);
    command->desc_id = FC_DESCRIPTOR_ID_INVALID;
    result              = fc_nand_test_wait_completion(command);
    if (fc_nand_test_release_command(command) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    return result;
}

static __attribute__((unused)) fc_status_t fc_nand_test_program_page(nand_paa_t test_paa)
{
    fc_command_t *command;
    uint8_t      *buffer;
    fc_status_t   result;
    uint16_t      index;

    command = fc_command_alloc();
    if (command == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    SSD_LOGD("[FC TEST] PROGRAM submit, paa=0x%08lX\r\n", (unsigned long)test_paa);
    fc_nand_test_command_configure(command, OP_NORMAL_PROGRAM_PAGE, test_paa, CW_NUM);
    result = fc_command_configure_descriptors(command);
    if (result != FC_STATUS_OK)
    {
        (void)fc_command_release(command);
        return result;
    }
    for (fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id; descriptor_id != FC_DESCRIPTOR_ID_INVALID; )
    {
        fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
        descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
        buffer = fc_buffer_id_to_real_buffer(descriptor->buffer_id);
        for (index = 0U; index < FC_BUFFER_POOL_CW_SIZE; index++)
        {
            buffer[index] = fc_nand_test_pattern_byte(index);
        }
    }
    result = fc_nand_test_wait_completion(command);
    SSD_LOGD("[FC TEST] PROGRAM completion result=%d\r\n", (int)result);
    for (fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id; descriptor_id != FC_DESCRIPTOR_ID_INVALID; )
    {
        fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
        descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
        if (fc_buffer_release(descriptor->buffer_id) != FC_STATUS_OK)
        {
            return FC_STATUS_IO_ERROR;
        }
    }
    if (fc_nand_test_release_command(command) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    return result;
}

fc_status_t fc_nand_test_program_block(nand_paa_t test_paa)
{
    return fc_nand_test_run_block_pipeline(test_paa, OP_NORMAL_PROGRAM_PAGE, "PROGRAM_BLOCK");
}

static __attribute__((unused)) fc_status_t fc_nand_test_read_page(nand_paa_t test_paa)
{
    fc_command_t *command;
    fc_status_t   result;
    uint16_t      index;

    command = fc_command_alloc();
    if (command == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    SSD_LOGD("[FC TEST] READ submit, paa=0x%08lX\r\n", (unsigned long)test_paa);
    fc_nand_test_command_configure(command, OP_NORMAL_READ_PAGE, test_paa, CW_NUM);
    result = fc_command_configure_descriptors(command);
    if (result != FC_STATUS_OK)
    {
        (void)fc_command_release(command);
        return result;
    }
    result = fc_nand_test_wait_completion(command);
    if (result == FC_STATUS_OK)
    {
        SSD_LOGD("[FC TEST] READ_DATA (0x55 pattern expected)\r\n");
        for (fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id; descriptor_id != FC_DESCRIPTOR_ID_INVALID; )
        {
            fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
            descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
            const uint32_t *words = (const uint32_t *)(uintptr_t)fc_buffer_id_to_real_buffer(descriptor->buffer_id);

            if (words == NULL)
            {
                SSD_LOGE("[FC TEST] READ buffer invalid: cw=%u id=%u\r\n", descriptor->config.cw, descriptor->buffer_id);
                result = FC_STATUS_IO_ERROR;
                break;
            }
            SSD_LOGD("[FC TEST] READ CW%u buffer_id=%u data=%08lX %08lX %08lX %08lX\r\n", descriptor->config.cw,
                     descriptor->buffer_id, (unsigned long)words[0], (unsigned long)words[1],
                     (unsigned long)words[2], (unsigned long)words[3]);
            for (index = 0U; index < (FC_BUFFER_POOL_CW_SIZE / sizeof(uint32_t)); index++)
            {
                if (words[index] != 0x55555555UL)
                {
                    SSD_LOGE("[FC TEST] READ mismatch: cw=%u word=%u actual=%08lX expected=55555555\r\n",
                             descriptor->config.cw, index, (unsigned long)words[index]);
                    result = FC_STATUS_IO_ERROR;
                }
            }
        }
    }
    for (fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id; descriptor_id != FC_DESCRIPTOR_ID_INVALID; )
    {
        fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
        descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
        if (fc_buffer_release(descriptor->buffer_id) != FC_STATUS_OK)
        {
            return FC_STATUS_IO_ERROR;
        }
    }
    if (fc_nand_test_release_command(command) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    return result;
}

fc_status_t fc_nand_test_read_block(nand_paa_t test_paa)
{
    return fc_nand_test_run_block_pipeline(test_paa, OP_NORMAL_READ_PAGE, "READ_BLOCK");
}

fc_status_t fc_nand_test_reset(void)
{
    fc_command_t *command;
    fc_status_t   result;

    command = fc_command_alloc();
    if (command == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    SSD_LOGI("[FC TEST] RESET submit\r\n");
    fc_nand_test_command_configure(command, OP_RESET, 0U, 0U);
    command->desc_id = FC_DESCRIPTOR_ID_INVALID;
    result           = fc_nand_test_wait_completion(command);
    if (fc_nand_test_release_command(command) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    return result;
}

fc_status_t fc_nand_test_read_status(void)
{
    fc_command_t *command;
    uint8_t      *buffer;
    fc_status_t   result;

    command = fc_command_alloc();
    if (command == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    SSD_LOGI("[FC TEST] READ_STATUS submit\r\n");
    fc_nand_test_command_configure(command, OP_READ_STATUS, 0U, 1U);
    result = fc_command_configure_descriptors(command);
    if (result != FC_STATUS_OK)
    {
        (void)fc_command_release(command);
        return result;
    }
    buffer = fc_buffer_id_to_real_buffer(fc_nand_test_descriptor((fc_descriptor_id_t)command->desc_id)->buffer_id);
    if (buffer == NULL)
    {
        return FC_STATUS_IO_ERROR;
    }
    result = fc_nand_test_wait_completion(command);
    if (result == FC_STATUS_OK)
    {
        SSD_LOGI("[FC TEST] READ_STATUS=0x%02X (READY=%u FAIL=%u)\r\n",
                 buffer[0], (unsigned int)((buffer[0] & NAND_STATUS_READY) != 0U),
                 (unsigned int)((buffer[0] & NAND_STATUS_FAIL) != 0U));
        if ((buffer[0] & NAND_STATUS_READY) == 0U)
        {
            result = FC_STATUS_IO_ERROR;
        }
    }
    for (fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id; descriptor_id != FC_DESCRIPTOR_ID_INVALID; )
    {
        fc_descriptor_t *descriptor = fc_nand_test_descriptor(descriptor_id);
        descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
        if (fc_buffer_release(descriptor->buffer_id) != FC_STATUS_OK)
        {
            return FC_STATUS_IO_ERROR;
        }
    }
    if (fc_nand_test_release_command(command) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    return result;
}

fc_status_t fc_nand_test_set_get_features(void)
{
    fc_command_t *set_cmd;
    fc_command_t *get_cmd;
    fc_status_t   result;

    /* SET_FEATURES: 设置 feature 地址 0x01 的值为 [0x40, 0x00, 0x00, 0x00]. */
    set_cmd = fc_command_alloc();
    if (set_cmd == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    SSD_LOGI("[FC TEST] SET_FEATURES submit\r\n");
    fc_nand_test_command_configure(set_cmd, OP_SET_FEATURES, 0U, 1U);
    /* 无 descriptor 分配 — 数据以内联方式传递 */
    set_cmd->feature_addr = 0x01U;
    set_cmd->parameter0   = 0x40U;  /* Enable ECC, enable "NORMAL" parameter page */
    set_cmd->parameter1   = 0x00U;
    set_cmd->parameter2   = 0x00U;
    set_cmd->parameter3   = 0x00U;
    result = fc_nand_test_wait_completion(set_cmd);
    if (result != FC_STATUS_OK)
    {
        SSD_LOGE("[FC TEST] SET_FEATURES failed: %d\r\n", (int)result);
        (void)fc_nand_test_release_command(set_cmd);
        return result;
    }
    SSD_LOGI("[FC TEST] SET_FEATURES passed\r\n");

    /* GET_FEATURES: 回读 feature 地址 0x01. */
    get_cmd = fc_command_alloc();
    if (get_cmd == NULL)
    {
        (void)fc_nand_test_release_command(set_cmd);
        return FC_STATUS_RESOURCE_EMPTY;
    }
    SSD_LOGI("[FC TEST] GET_FEATURES submit\r\n");
    fc_nand_test_command_configure(get_cmd, OP_GET_FEATURES, 0U, 1U);
    get_cmd->feature_addr = 0x01U;
    result = fc_nand_test_wait_completion(get_cmd);
    if (result == FC_STATUS_OK)
    {
        SSD_LOGI("[FC TEST] GET_FEATURES(0x01)=%02X %02X %02X %02X\r\n",
                 get_cmd->parameter0, get_cmd->parameter1,
                 get_cmd->parameter2, get_cmd->parameter3);
        if ((get_cmd->parameter0 != 0x40U) || (get_cmd->parameter1 != 0x00U) ||
            (get_cmd->parameter2 != 0x00U) || (get_cmd->parameter3 != 0x00U))
        {
            SSD_LOGE("[FC TEST] GET_FEATURES mismatch\r\n");
            result = FC_STATUS_IO_ERROR;
        }
    }
    else
    {
        SSD_LOGE("[FC TEST] GET_FEATURES failed: %d\r\n", (int)result);
    }

    (void)fc_nand_test_release_command(get_cmd);
    (void)fc_nand_test_release_command(set_cmd);
    return result;
}

fc_status_t fc_nand_test_run_all(nand_paa_t test_paa)
{
    fc_status_t result;

    result = fc_nand_test_reset();
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    SSD_LOGI("[FC TEST] RESET passed\r\n");
    result = fc_nand_test_read_id();
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    SSD_LOGI("[FC TEST] READ_ID passed\r\n");
    result = fc_nand_test_read_status();
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    SSD_LOGI("[FC TEST] READ_STATUS passed\r\n");
    result = fc_nand_test_set_get_features();
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    SSD_LOGI("[FC TEST] SET/GET_FEATURES passed\r\n");
    result = fc_nand_test_erase_block(test_paa);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    SSD_LOGI("[FC TEST] ERASE passed\r\n");
    result = fc_nand_test_program_block(test_paa);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    result = fc_nand_test_read_block(test_paa);
    if (result == FC_STATUS_OK)
    {
        SSD_LOGI("[FC TEST] READ_BLOCK passed\r\n");
    }
    return result;
}
