#include "fc_raid_engine.h"
#include "fc.h"
#include "nand/nand_config.h"

#include <stddef.h>
#include <string.h>

#if CONFIG_FC_RAID

/*
 * RAID XOR 引擎实现。
 *
 * 全局静态数组：
 *   g_raid_ctx[LUN_NUM]   — 每个 LUN 的 RAID 上下文
 *   g_raid_shadow[LUN_NUM] — 影子缓冲（HSQ 打断 save/restore）
 */

static fc_raid_context_t g_raid_ctx[LUN_NUM];
static fc_raid_context_t g_raid_shadow[LUN_NUM];

void fc_raid_init_ctx(uint8_t lun)
{
    if (lun < LUN_NUM)
    {
        memset(&g_raid_ctx[lun], 0, sizeof(fc_raid_context_t));
        memset(&g_raid_shadow[lun], 0, sizeof(fc_raid_context_t));
    }
}

fc_status_t fc_raid_start(uint8_t lun, const uint8_t *cw_buffer, uint8_t total)
{
    if ((lun >= LUN_NUM) || (cw_buffer == NULL) || (total == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    fc_raid_context_t *ctx = &g_raid_ctx[lun];

    if (ctx->active != 0U)
    {
        return FC_STATUS_NOT_READY;
    }

    memcpy(ctx->acc_buf, cw_buffer, FC_BUFFER_POOL_CW_SIZE);
    ctx->active   = 1U;
    ctx->processed = 1U;
    ctx->total     = total;
    return FC_STATUS_OK;
}

fc_status_t fc_raid_accumulate(uint8_t lun, const uint8_t *cw_buffer)
{
    uint16_t i;
    uint32_t *acc32;
    const uint32_t *buf32;

    if ((lun >= LUN_NUM) || (cw_buffer == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    fc_raid_context_t *ctx = &g_raid_ctx[lun];

    if (ctx->active == 0U)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    if (ctx->processed >= ctx->total)
    {
        return FC_STATUS_NOT_READY;
    }

    /* 32 位对齐 XOR */
    acc32 = (uint32_t *)(void *)ctx->acc_buf;
    buf32 = (const uint32_t *)(const void *)cw_buffer;
    for (i = 0; i < (FC_BUFFER_POOL_CW_SIZE / sizeof(uint32_t)); i++)
    {
        acc32[i] ^= buf32[i];
    }

    ctx->processed++;
    return FC_STATUS_OK;
}

fc_status_t fc_raid_end(uint8_t lun, uint8_t *parity_buffer)
{
    if ((lun >= LUN_NUM) || (parity_buffer == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    fc_raid_context_t *ctx = &g_raid_ctx[lun];

    if (ctx->active == 0U)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    memcpy(parity_buffer, ctx->acc_buf, FC_BUFFER_POOL_CW_SIZE);

    ctx->active    = 0U;
    ctx->processed = 0U;
    ctx->total     = 0U;
    return FC_STATUS_OK;
}

void fc_raid_save(uint8_t lun)
{
    if (lun < LUN_NUM)
    {
        g_raid_shadow[lun] = g_raid_ctx[lun];
    }
}

void fc_raid_restore(uint8_t lun)
{
    if (lun < LUN_NUM)
    {
        g_raid_ctx[lun] = g_raid_shadow[lun];
    }
}

void fc_raid_recover(uint8_t *recovered,
                     uint8_t *buffers[], uint8_t count,
                     uint8_t failed_idx, uint16_t length)
{
    uint8_t  i;
    uint32_t *rec32 = (uint32_t *)(void *)recovered;
    uint16_t word_len = length / sizeof(uint32_t);

    if ((recovered == NULL) || (buffers == NULL) || (count == 0U) ||
        (failed_idx >= count) || (length == 0U))
    {
        return;
    }

    memset(recovered, 0, length);

    for (i = 0; i < count; i++)
    {
        if ((i == failed_idx) || (buffers[i] == NULL))
        {
            continue;
        }
        for (uint16_t w = 0U; w < word_len; w++)
        {
            rec32[w] ^= ((const uint32_t *)(const void *)buffers[i])[w];
        }
    }
}

#endif /* CONFIG_FC_RAID */