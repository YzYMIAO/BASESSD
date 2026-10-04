#include "ftl/pending.h"

#include "common/assertion.h"
#include "ftl/ftl.h" /* ftl_context_t */

#include <string.h>

void pending_init(ftl_context_t *ftl)
{
    /* 层内接口：唯一调用者 ftl_init 已在入口判过 ftl != NULL */
    ASSERT(ftl != NULL);

    memset(ftl->pending, 0, sizeof(ftl->pending));
}

int pending_add(ftl_context_t *ftl, uint16_t cmd_id, const ftl_io_request_t *io, nand_pba_t old_pba, nand_pba_t new_pba, nand_pba_t old_pba1, uint8_t cache_intermediate)
{
    uint8_t i;

    /* 层内接口：调用者 ftl_submit_read/write 已在入口判过 ftl / req 非 NULL */
    ASSERT(ftl != NULL);
    ASSERT(io != NULL);

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (ftl->pending[i].used == 0U)
        {
            ftl->pending[i].used               = 1U;
            ftl->pending[i].command_id         = cmd_id;
            ftl->pending[i].io                 = *io; /* 按值拷贝整个 IO 请求描述 */
            ftl->pending[i].old_pba            = old_pba;
            ftl->pending[i].new_pba            = new_pba;
            ftl->pending[i].old_pba1           = old_pba1;
            ftl->pending[i].cache_intermediate = cache_intermediate;
            return FTL_OK;
        }
    }

    return FTL_ERROR; /* 表满 */
}

ftl_pending_req_t *pending_lookup(ftl_context_t *ftl, uint16_t cmd_id)
{
    uint8_t i;

    ASSERT(ftl != NULL);

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((ftl->pending[i].used != 0U) &&
            (ftl->pending[i].command_id == cmd_id))
        {
            return &ftl->pending[i];
        }
    }

    return NULL;
}

ftl_pending_req_t *pending_lookup_by_lba(ftl_context_t *ftl, lba_t lba)
{
    uint8_t i;

    ASSERT(ftl != NULL);

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((ftl->pending[i].used != 0U) &&
            (ftl->pending[i].io.lba == lba))
        {
            return &ftl->pending[i];
        }
    }

    return NULL;
}

void pending_remove(ftl_context_t *ftl, uint16_t cmd_id)
{
    ftl_pending_req_t *req;

    ASSERT(ftl != NULL);

    req = pending_lookup(ftl, cmd_id);
    if (req != NULL)
    {
        req->used = 0U;
    }
}

uint16_t pending_count(const ftl_context_t *ftl)
{
    uint16_t count = 0U;
    uint8_t  i;

    ASSERT(ftl != NULL);

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (ftl->pending[i].used != 0U)
        {
            count++;
        }
    }

    return count;
}
