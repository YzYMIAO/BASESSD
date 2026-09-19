#include "fc_command.h"

#include "onfi_1_0.h"

#include <stddef.h>

static uint8_t fc_command_is_page_data_operation(uint8_t operation)
{
    return (uint8_t)((operation == OP_NORMAL_READ_PAGE) || (operation == OP_NORMAL_PROGRAM_PAGE));
}

static uint8_t fc_command_cw_count(const fc_command_t *command)
{
    return (command == NULL) ? 0U : (uint8_t)(command->config.cw + 1U);
}

fc_status_t fc_command_validate(const fc_command_t *command)
{
    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if ((command->config.operation >= OP_COUNT) || (command->config.ldpc > FC_LDPC_RAW) ||
        (command->config.cw >= 4U) || (command->config.page_type >= 4U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if ((fc_command_is_page_data_operation((uint8_t)command->config.operation) != 0U) &&
        (command->desc_id == FC_DESCRIPTOR_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return FC_STATUS_OK;
}

void fc_command_status_init(fc_command_status_t *status, const fc_command_t *command)
{
    uint8_t count;

    if (status == NULL)
    {
        return;
    }
    *status = (fc_command_status_t){0};
    status->command_id = (command != NULL) ? command->command_id : FC_COMMAND_HANDLE_INVALID;
    status->desc_id    = (command != NULL) ? command->desc_id : FC_DESCRIPTOR_ID_INVALID;
    count              = fc_command_cw_count(command);
    status->status0    = FC_CW_STATUS_READY;
    status->status1    = (count > 1U) ? FC_CW_STATUS_READY : 0U;
    status->status2    = (count > 2U) ? FC_CW_STATUS_READY : 0U;
    status->status3    = (count > 3U) ? FC_CW_STATUS_READY : 0U;
}

void fc_command_status_set_cw(fc_command_status_t *status, uint8_t cw, uint8_t cw_status, fc_descriptor_id_t descriptor_id)
{
    if ((status == NULL) || (cw >= 4U))
    {
        return;
    }
    switch (cw)
    {
        case 0U:
            status->status0 = cw_status;
            break;
        case 1U:
            status->status1 = cw_status;
            break;
        case 2U:
            status->status2 = cw_status;
            break;
        default:
            status->status3 = cw_status;
            break;
    }
    if ((cw_status & (FC_CW_STATUS_NAND_FAIL | FC_CW_STATUS_TIMEOUT | FC_CW_STATUS_CRC_ERROR |
                      FC_CW_STATUS_UNC | FC_CW_STATUS_MATA_ERROR | FC_CW_STATUS_RAID_ERROR)) != 0U)
    {
        status->desc_id = descriptor_id;
    }
}
