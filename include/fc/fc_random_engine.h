#ifndef FC_RANDOM_ENGINE_H
#define FC_RANDOM_ENGINE_H

#include "fc_types.h"

#include <stdint.h>

uint32_t    fc_random_seed_from_paa(nand_paa_t paa, uint32_t program_sequence);
fc_status_t fc_random_transform(uint8_t *data, uint16_t length, uint32_t seed);

#endif /* FC_RANDOM_ENGINE_H */
