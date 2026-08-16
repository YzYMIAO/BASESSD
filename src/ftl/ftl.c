#include <stddef.h>

#include "ftl/ftl.h"

int ftl_init(ftl_context_t *ftl, nand_device_t *nand,
             ssd_ppa_t *mapping_storage, size_t mapping_count)
{
    int status;

    if ((ftl == NULL) || (nand == NULL)) {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    status = ftl_l2p_init(&ftl->l2p, mapping_storage, mapping_count);
    if (status != SSD_STATUS_OK) {
        return status;
    }

    ftl->nand = nand;
    ftl->next_free_ppa = 0U;
    return SSD_STATUS_OK;
}

int ftl_read(ftl_context_t *ftl, ssd_lba_t lba, void *data)
{
    ssd_ppa_t ppa;
    int status;

    if ((ftl == NULL) || (data == NULL)) {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    status = ftl_l2p_get(&ftl->l2p, lba, &ppa);
    if ((status != SSD_STATUS_OK) || (ppa == FTL_PPA_INVALID)) {
        return SSD_STATUS_IO_ERROR;
    }

    return nand_read_page(ftl->nand, ppa, data, NULL);
}

int ftl_write(ftl_context_t *ftl, ssd_lba_t lba, const void *data)
{
    ssd_ppa_t ppa;
    int status;

    if ((ftl == NULL) || (data == NULL)) {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 第一版顺序分配物理页；GC 接入前不允许写指针回绕。 */
    ppa = ftl->next_free_ppa;
    if (ppa >= nand_geometry_total_pages(&ftl->nand->geometry)) {
        return SSD_STATUS_NO_SPACE;
    }

    status = nand_program_page(ftl->nand, ppa, data, NULL);
    if (status != SSD_STATUS_OK) {
        return status;
    }

    status = ftl_l2p_set(&ftl->l2p, lba, ppa);
    if (status == SSD_STATUS_OK) {
        ++ftl->next_free_ppa;
    }

    return status;
}

int ftl_trim(ftl_context_t *ftl, ssd_lba_t lba)
{
    if (ftl == NULL) {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    return ftl_l2p_trim(&ftl->l2p, lba);
}
