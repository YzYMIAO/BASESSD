#include "fc_random_engine.h"

#include <stddef.h>

uint32_t fc_random_seed_from_paa(nand_paa_t paa, uint32_t program_sequence)
{
    uint32_t seed;

    seed = program_sequence ^ (paa_to_lun(paa) << 16U) ^ paa;
    seed ^= (paa_to_plane(paa) << 14U) ^ (paa_to_block(paa) << 2U) ^ paa_to_page(paa);
    return (seed != 0U) ? seed : 0x1U;
}

fc_status_t fc_random_transform(uint8_t *data, uint16_t length, uint32_t seed)
{
    uint16_t index;

    if ((data == NULL) || (seed == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < length; index++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        data[index] ^= (uint8_t)seed;
    }
    return FC_STATUS_OK;
}
