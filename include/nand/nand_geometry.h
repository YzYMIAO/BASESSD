#ifndef NAND_GEOMETRY_H
#define NAND_GEOMETRY_H

#include <stdint.h>

/* 描述模拟或真实 NAND 的层级组织和页大小。 */
typedef struct {
    uint32_t channel_count;
    uint32_t chips_per_channel;
    uint32_t dies_per_chip;
    uint32_t planes_per_die;
    uint32_t blocks_per_plane;
    uint32_t pages_per_block;
    uint32_t page_size;
    uint32_t oob_size;
} nand_geometry_t;

int nand_geometry_validate(const nand_geometry_t *geometry);
uint64_t nand_geometry_total_pages(const nand_geometry_t *geometry);

#endif /* NAND_GEOMETRY_H */
