#ifndef SSD_REQUEST_H
#define SSD_REQUEST_H

#include <stddef.h>

#include "common/ssd_types.h"

typedef enum {
    SSD_REQUEST_READ = 0,
    SSD_REQUEST_WRITE,
    SSD_REQUEST_FLUSH,
    SSD_REQUEST_TRIM
} ssd_request_opcode_t;

typedef struct {
    ssd_request_opcode_t opcode;
    ssd_lba_t start_lba;
    uint32_t block_count;
    void *buffer;
    size_t buffer_size;
    int completion_status;
} ssd_request_t;

#endif /* SSD_REQUEST_H */
