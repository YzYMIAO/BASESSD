#include "fc_command.h"

#include "nand/onfi_1_0.h"
#include "onfi_1_0.h"

#include <stddef.h>

fc_status_t fc_command_validate(const fc_command_t *command)
{
    const nand_operation_map_t *op;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (command->config.operation >= OP_COUNT)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    op = &g_nand_operation_map[command->config.operation];
    if ((command->config.operation != OP_SET_FEATURES) &&
        (command->config.operation != OP_GET_FEATURES) &&
        nand_operation_has_data((uint8_t)command->config.operation) &&
        (command->desc_id == FC_DESCRIPTOR_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    /* data_input_cycles > 0 is a subset of nand_operation_has_data() — the latter also
     * includes data-output operations (READ_STATUS, READ_ID, etc.) that need a buffer. */
    (void)op;
    return FC_STATUS_OK;
}

void fc_command_status_init(fc_command_status_t *status, const fc_command_t *command)
{
    uint8_t count;

    if (status == NULL)
    {
        return;
    }
    *status            = (fc_command_status_t){0};
    status->command_id = (command != NULL) ? command->command_id : FC_COMMAND_ID_INVALID;
    status->desc_id    = (command != NULL) ? command->desc_id : FC_DESCRIPTOR_ID_INVALID;
    status->module     = (command != NULL) ? command->module : 0U;
    count              = (command != NULL) ? (uint8_t)(command->config.cw + 1U) : 0U;
    status->ecc_bit0   = 0U;
    status->ecc_bit1   = (count > 1U) ? 0U : FC_CW_STATUS_UNUSED;
    status->ecc_bit2   = (count > 2U) ? 0U : FC_CW_STATUS_UNUSED;
    status->ecc_bit3   = (count > 3U) ? 0U : FC_CW_STATUS_UNUSED;
    status->status     = FC_CW_STATUS_READY;
}

void fc_command_status_set_cw(fc_command_status_t *status, uint8_t cw, uint8_t cw_status, fc_descriptor_id_t descriptor_id)
{
    if ((status == NULL) || (cw >= 4U))
    {
        return;
    }

    /* OR per-CW status bits into the aggregated command-level status. */
    status->status |= cw_status;

    /* Record the first-failing descriptor's ID for error-path lookup. */
    if ((cw_status & (FC_CW_STATUS_NAND_FAIL | FC_CW_STATUS_TIMEOUT | FC_CW_STATUS_CRC_ERROR |
                      FC_CW_STATUS_UNC | FC_CW_STATUS_MATA_ERROR | FC_CW_STATUS_RAID_ERROR)) != 0U)
    {
        /* Only overwrite desc_id on first error; later CW errors keep the first. */
        if ((status->status & (FC_CW_STATUS_NAND_FAIL | FC_CW_STATUS_TIMEOUT | FC_CW_STATUS_CRC_ERROR |
                               FC_CW_STATUS_UNC | FC_CW_STATUS_MATA_ERROR | FC_CW_STATUS_RAID_ERROR)) == cw_status)
        {
            status->desc_id = descriptor_id;
        }
    }
}
