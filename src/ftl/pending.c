#include "ftl/pending.h"

#include "ftl/ftl.h"   /* ftl_context_t */

#include <string.h>

static ftl_pending_req_t g_pending[FTL_PENDING_MAX];

void pending_init(void)
{
    memset(g_pending, 0, sizeof(g_pending));
}

int pending_add(uint16_t cmd_id, lba_t lba, void *buffer,
                pending_cb_t on_complete, void *on_complete_ctx,
                volatile uint8_t *complete_flag,
                volatile ssd_status_t *status_out,
                nand_pba_t old_pba, nand_pba_t new_pba,
                ftl_context_t *ftl,
                uint16_t cache_intermediate,
                uint16_t pages)
{
    uint8_t i;

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (g_pending[i].used == 0U)
        {
            g_pending[i].used               = 1U;
            g_pending[i].command_id         = cmd_id;
            g_pending[i].lba                = lba;
            g_pending[i].buffer             = buffer;
            g_pending[i].on_complete        = on_complete;
            g_pending[i].on_complete_ctx    = on_complete_ctx;
            g_pending[i].complete_flag      = complete_flag;
            g_pending[i].status_out         = status_out;
            g_pending[i].old_pba            = old_pba;
            g_pending[i].new_pba            = new_pba;
            g_pending[i].ftl                = ftl;
            g_pending[i].cache_intermediate = cache_intermediate;
            g_pending[i].pages              = pages;
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
            (g_pending[i].lba == lba))
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