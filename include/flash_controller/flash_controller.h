#ifndef FLASH_CONTROLLER_H
#define FLASH_CONTROLLER_H

#include <stddef.h>

#include "nand/nand.h"

/* Flash Controller 负责命令排队、通道选择以及完成通知。 */
typedef struct {
    nand_device_t *nand;
    uint32_t active_channel;
} flash_controller_t;

int flash_controller_init(flash_controller_t *controller, nand_device_t *nand);
int flash_controller_poll(flash_controller_t *controller);

#endif /* FLASH_CONTROLLER_H */
