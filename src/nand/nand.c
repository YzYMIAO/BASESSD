#include "nand/nand.h"

#include <stddef.h>

int nand_device_init(nand_device_t         *device,
                     const nand_geometry_t *geometry,
                     const nand_ops_t      *ops,
                     void                  *backend_context)
{
    if ((device == NULL) || (geometry == NULL) || (ops == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    if ((nand_geometry_validate(geometry) != 0) || (ops->read_page == NULL) || (ops->program_page == NULL) ||
        (ops->erase_block == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    device->geometry        = *geometry;
    device->ops             = *ops;
    device->backend_context = backend_context;
    return SSD_STATUS_OK;
}

int nand_read_page(nand_device_t *device, ssd_ppa_t ppa, void *data, void *oob)
{
    if ((device == NULL) || (data == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    return device->ops.read_page(device->backend_context, ppa, data, oob);
}

int nand_program_page(nand_device_t *device, ssd_ppa_t ppa, const void *data, const void *oob)
{
    if ((device == NULL) || (data == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    return device->ops.program_page(device->backend_context, ppa, data, oob);
}

int nand_erase_block(nand_device_t *device, uint64_t block)
{
    if (device == NULL)
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    return device->ops.erase_block(device->backend_context, block);
}
