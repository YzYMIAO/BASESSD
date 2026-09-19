#ifndef NAND_DISPATCH_H
#define NAND_DISPATCH_H

#include "fc/fc_command.h"
#include "nand_engine.h"

#include <stdint.h>

/* One fixed FIFO is attached to each physical NAND channel. */
#define NAND_CHANNEL_QUEUE_DEPTH (64U)

typedef struct
{
    uint16_t          command_id;
    uint8_t          execution_status;
    uint8_t          nand_status;
    uint8_t          cw_status[4];
    uint16_t          failed_descriptor_id;
} nand_dispatch_completion_t;

/* Initialize fixed Channel Dispatch Queues. This function does not initialize FMC hardware. */
uint8_t nand_dispatch_init(void);

/* NAND/FMC 初始化成功后置位；未就绪时不允许 FC 下发介质命令。 */
void nand_dispatch_set_device_ready(uint8_t ready);

/* Route one active FC command to the FIFO selected by command->ch. */
uint8_t nand_dispatch_submit(fc_command_t *command);

/* Execute one pending command from one Channel FIFO through ONFI -> NAND Engine -> FMC. */
uint8_t nand_dispatch_poll(nand_dispatch_completion_t *completion);

#endif /* NAND_DISPATCH_H */
