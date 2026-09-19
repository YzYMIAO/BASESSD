#ifndef FC_RAID_ENGINE_H
#define FC_RAID_ENGINE_H

#include "fc_types.h"

#include <stdint.h>

typedef struct
{
    uint8_t data_plane_count;
    uint8_t parity_plane;
} fc_raid_layout_t;

fc_status_t fc_raid_xor(uint8_t *parity, const uint8_t *data, uint16_t length);

#endif /* FC_RAID_ENGINE_H */
