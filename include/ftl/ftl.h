#ifndef FTL_H
#define FTL_H

#include "common/ssd_types.h"
#include "ftl/l2p.h"
#include "nand/nand.h"

typedef struct {
    nand_device_t *nand;
    ftl_l2p_table_t l2p;
    ssd_ppa_t next_free_ppa;
} ftl_context_t;

int ftl_init(ftl_context_t *ftl, nand_device_t *nand,
             ssd_ppa_t *mapping_storage, size_t mapping_count);
int ftl_read(ftl_context_t *ftl, ssd_lba_t lba, void *data);
int ftl_write(ftl_context_t *ftl, ssd_lba_t lba, const void *data);
int ftl_trim(ftl_context_t *ftl, ssd_lba_t lba);

#endif /* FTL_H */
