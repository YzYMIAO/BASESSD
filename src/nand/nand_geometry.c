#include <stddef.h>

#include "nand/nand_geometry.h"

int nand_geometry_validate(const nand_geometry_t *geometry)
{
    if (geometry == NULL) {
        return -1;
    }

    /* 任一层级为零都会使 PPA 编解码和容量计算失去意义。 */
    if ((geometry->channel_count == 0U) ||
        (geometry->chips_per_channel == 0U) ||
        (geometry->dies_per_chip == 0U) ||
        (geometry->planes_per_die == 0U) ||
        (geometry->blocks_per_plane == 0U) ||
        (geometry->pages_per_block == 0U) ||
        (geometry->page_size == 0U)) {
        return -1;
    }

    return 0;
}

uint64_t nand_geometry_total_pages(const nand_geometry_t *geometry)
{
    if (nand_geometry_validate(geometry) != 0) {
        return 0U;
    }

    return (uint64_t)geometry->channel_count *
           geometry->chips_per_channel *
           geometry->dies_per_chip *
           geometry->planes_per_die *
           geometry->blocks_per_plane *
           geometry->pages_per_block;
}
