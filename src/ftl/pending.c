#include "ftl/pending.h"

#include "ftl/ftl.h"   /* ftl_context_t */

#include <string.h>

static ftl_pending_req_t g_pending[FTL_PENDING_MAX];

void pending_init(void)
{
    memset(g_pending, 0, sizeof(g_pending));
}

int pending_add(uint16_t cmd_id, const ftl_io_request_t *io,
                nand_pba_t old_pba, nand_pba_t new_pba,
                nand_pba_t old_pba1, ftl_context_t *ftl,
                uint8_t cache_intermediate)
{
    uint8_t i;

    if (io == NULL)
    {
        return FTL_ERROR;
    }

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (g_pending[i].used == 0U)
        {
            g_pending[i].used               = 1U;
            g_pending[i].command_id         = cmd_id;
            g_pending[i].io                 = *io; /* 按值拷贝整个 IO 请求描述 */
            g_pending[i].old_pba            = old_pba;
            g_pending[i].new_pba            = new_pba;
            g_pending[i].old_pba1           = old_pba1;
            g_pending[i].ftl                = ftl;
            g_pending[i].cache_intermediate = cache_intermediate;
            return FTL_OK;
        }
    }

    return FTL_ERROR; /* 表满 */
}

ftl_pending_req_t *pending_lookup(uint16_t cmd_id)
{
    uint8_t i;

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((g_pending[i].used != 0U) &&
            (g_pending[i].command_id == cmd_id))
        {
            return &g_pending[i];
        }
    }

    return NULL;
}

ftl_pending_req_t *pending_lookup_by_lba(lba_t lba)
{
    uint8_t i;

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((g_pending[i].used != 0U) &&
            (g_pending[i].io.lba == lba))
        {
            return &g_pending[i];
        }
    }

    return NULL;
}

void pending_remove(uint16_t cmd_id)
{
    ftl_pending_req_t *req = pending_lookup(cmd_id);
    if (req != NULL)
    {
        req->used = 0U;
    }
}