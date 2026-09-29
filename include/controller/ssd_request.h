#ifndef SSD_REQUEST_H
#define SSD_REQUEST_H

#include "common/ftl_types.h" /* ftl_io_request_t */
#include "common/ssd_types.h"

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    SSD_REQUEST_READ = 0,
    SSD_REQUEST_WRITE,
    SSD_REQUEST_FLUSH,
    SSD_REQUEST_TRIM
} ssd_request_opcode_t;

typedef struct
{
    ssd_request_opcode_t opcode;
    uint64_t             start_lba;
    uint32_t             block_count;
    void                *buffer;
    size_t               buffer_size;
    ssd_status_t         completion_status;
    volatile uint8_t     complete_flag;      /* FTL completion handler 置 1 */
    volatile uint32_t    sub_remaining;      /* 未完成的子请求数（Controller 拆分用） */
    ftl_io_request_t     io;                 /* 子请求复用模板（lba/buffer/pages 循环内改写） */
} ssd_request_t;

#endif /* SSD_REQUEST_H */
