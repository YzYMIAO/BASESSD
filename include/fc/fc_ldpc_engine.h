#ifndef FC_LDPC_ENGINE_H
#define FC_LDPC_ENGINE_H

#include "fc_types.h"

typedef struct
{
    uint16_t codeword_bytes;
    uint16_t parity_bytes;
    uint8_t  max_iterations;
} fc_ldpc_config_t;

fc_status_t fc_ldpc_encode(const fc_ldpc_config_t *config, uint8_t *data, uint16_t data_length);
fc_status_t fc_ldpc_decode(const fc_ldpc_config_t *config, uint8_t *data, uint16_t data_length);

#endif /* FC_LDPC_ENGINE_H */
