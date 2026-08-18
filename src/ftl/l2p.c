#include "ftl/l2p.h"

int ftl_l2p_init(ftl_l2p_table_t *table, ssd_ppa_t *storage, size_t entry_count)
{
    size_t index;

    if ((table == NULL) || (storage == NULL) || (entry_count == 0U))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    table->entries     = storage;
    table->entry_count = entry_count;

    for (index = 0U; index < entry_count; ++index)
    {
        table->entries[index] = FTL_PPA_INVALID;
    }

    return SSD_STATUS_OK;
}

int ftl_l2p_get(const ftl_l2p_table_t *table, ssd_lba_t lba, ssd_ppa_t *ppa)
{
    if ((table == NULL) || (ppa == NULL) || (lba >= table->entry_count))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    *ppa = table->entries[lba];
    return SSD_STATUS_OK;
}

int ftl_l2p_set(ftl_l2p_table_t *table, ssd_lba_t lba, ssd_ppa_t ppa)
{
    if ((table == NULL) || (lba >= table->entry_count))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    table->entries[lba] = ppa;
    return SSD_STATUS_OK;
}

int ftl_l2p_trim(ftl_l2p_table_t *table, ssd_lba_t lba)
{
    return ftl_l2p_set(table, lba, FTL_PPA_INVALID);
}
