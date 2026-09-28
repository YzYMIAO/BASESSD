#include "ftl/pending.h"

#include <string.h>

static ftl_pending_req_t g_pending[FTL_PENDING_MAX];

void pending_init(void)
{
    memset(g_pending, 0, sizeof(g_pending));
}

int pending_add(uint16_t cmd_id, lba_t lba, void *buffer, uint32_t count)
{
    uint8_t i;

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (g_pending[i].used == 0U)
        {
            g_pending[i].used       = 1U;
            g_pending[i].command_id = cmd_id;
            g_pending[i].lba        = lba;
            g_pending[i].buffer     = buffer;
            g_pending[i].count      = count;
            return 0;
        }
    }

    return -1; /* 表满 */
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