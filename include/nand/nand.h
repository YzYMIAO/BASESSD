#ifndef NAND_H
#define NAND_H

#include <stdint.h>

#include "common/ssd_types.h"
#include "nand/nand_geometry.h"

/* NAND 后端统一接口，供模拟器和真实 Flash Controller 共同实现。 */
typedef struct {
    int (*read_page)(void *context, ssd_ppa_t ppa, void *data, void *oob);
    int (*program_page)(void *context, ssd_ppa_t ppa,
                        const void *data, const void *oob);
    int (*erase_block)(void *context, uint64_t block);
    int (*is_bad_block)(void *context, uint64_t block);
} nand_ops_t;

typedef struct {
    nand_geometry_t geometry;
    nand_ops_t ops;
    void *backend_context;
} nand_device_t;

int nand_device_init(nand_device_t *device, const nand_geometry_t *geometry,
                     const nand_ops_t *ops, void *backend_context);
int nand_read_page(nand_device_t *device, ssd_ppa_t ppa, void *data, void *oob);
int nand_program_page(nand_device_t *device, ssd_ppa_t ppa,
                      const void *data, const void *oob);
int nand_erase_block(nand_device_t *device, uint64_t block);

#endif /* NAND_H */
