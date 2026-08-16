#include "flash_controller/flash_controller.h"

int flash_controller_init(flash_controller_t *controller, nand_device_t *nand)
{
    if ((controller == NULL) || (nand == NULL)) {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    controller->nand = nand;
    controller->active_channel = 0U;
    return SSD_STATUS_OK;
}

int flash_controller_poll(flash_controller_t *controller)
{
    if (controller == NULL) {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 同步版本暂时没有异步完成项，后续命令队列接入后在此推进状态。 */
    return SSD_STATUS_OK;
}
