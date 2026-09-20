#include "fc_data_path.h"
#include "fc.h"
#include "fc_random_engine.h"

#include <stddef.h>

fc_status_t fc_data_path_encode(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *buffer;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (command->config.random != 0U)
    {
        return FC_STATUS_OK;
    }
    for (desc_id = (fc_descriptor_id_t)command->desc_id;
         desc_id != FC_DESCRIPTOR_ID_INVALID;)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
        if (buffer != NULL)
        {
            uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
            (void)fc_random_transform(buffer, FC_BUFFER_POOL_CW_SIZE, seed);
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
    /* TODO: LDPC encode, RAID encode */
    return FC_STATUS_OK;
}

fc_status_t fc_data_path_decode(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *buffer;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (command->config.random != 0U)
    {
        return FC_STATUS_OK;
    }
    /* TODO: RAID decode, LDPC decode */
    for (desc_id = (fc_descriptor_id_t)command->desc_id;
         desc_id != FC_DESCRIPTOR_ID_INVALID;)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
        if (buffer != NULL)
        {
            uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
            (void)fc_random_transform(buffer, FC_BUFFER_POOL_CW_SIZE, seed);
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
    return FC_STATUS_OK;
}