#include "fc_data_path.h"

#include <stddef.h>

fc_status_t fc_data_path_encode(const fc_command_t *command)
{
    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    /* ??????? random -> LDPC -> RAID?????? OOB metadata? */
    return FC_STATUS_OK;
}

fc_status_t fc_data_path_decode(const fc_command_t *command)
{
    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    /* ??????? LDPC -> random?????? OOB metadata? */
    return FC_STATUS_OK;
}
