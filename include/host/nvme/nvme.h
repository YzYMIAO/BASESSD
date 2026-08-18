#ifndef SSD_NVME_H
#define SSD_NVME_H

#include "controller/controller.h"

#include <stdint.h>

/* 教学模型只保留命令处理所需字段，暂不模拟完整 PCIe 寄存器空间。 */
typedef struct
{
    uint8_t  opcode;
    uint16_t command_id;
    uint32_t namespace_id;
    uint64_t start_lba;
    uint16_t block_count;
} nvme_command_t;

typedef struct
{
    uint16_t command_id;
    uint16_t status;
} nvme_completion_t;

int nvme_process_command(ssd_controller_t *controller, const nvme_command_t *command, nvme_completion_t *completion);

#endif /* SSD_NVME_H */
