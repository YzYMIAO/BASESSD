#ifndef FTL_L2P_H
#define FTL_L2P_H

#include "common/ssd_types.h"

#include <stddef.h>

#define FTL_PPA_INVALID UINT64_MAX

/* 最初采用完整页级映射，后续可以替换为分级映射或缓存映射。 */
typedef struct
{
    ssd_ppa_t *entries;
    size_t     entry_count;
} ftl_l2p_table_t;

int ftl_l2p_init(ftl_l2p_table_t *table, ssd_ppa_t *storage, size_t entry_count);
int ftl_l2p_get(const ftl_l2p_table_t *table, ssd_lba_t lba, ssd_ppa_t *ppa);
int ftl_l2p_set(ftl_l2p_table_t *table, ssd_lba_t lba, ssd_ppa_t ppa);
int ftl_l2p_trim(ftl_l2p_table_t *table, ssd_lba_t lba);

#endif /* FTL_L2P_H */
