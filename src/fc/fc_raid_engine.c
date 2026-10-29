#include "fc_raid_engine.h"

#include "assertion.h"
#include "fc.h"
#include "nand_config.h"

#include <stddef.h>
#include <string.h>

#if CONFIG_FC_RAID

// fc_raid_engine.c — RAID XOR 引擎
//
// 原理：parity = CW0 ^ CW1 ^ ... ^ CWn-1。
// 任一 CW 丢失（ECC 失败）时可用 recovered = 其余 CW ^ parity 重建
// （RAID-5 单盘容错的 XOR 性质，异或自逆）。
//
// 两套接口：
// - 上下文式（start/accumulate/end）：写路径跨命令累积 parity。
// - 静态恢复（recover）：读路径 ECC 失败后无上下文重建。
//
// 全局静态数组：
// g_raid_ctx[LUN_NUM]   — 每个 LUN 的 RAID 上下文
// g_raid_shadow[LUN_NUM] — 影子缓冲（HSQ 打断 save/restore）

static fc_raid_context_t g_raid_ctx[LUN_NUM];
static fc_raid_context_t g_raid_shadow[LUN_NUM];

// 清空上下文与影子缓冲，使该 LUN 回到空闲态。
void fc_raid_init_ctx(BYTE lun)
{
    // 前置条件由调用者保证（fc.c 按 LUN_NUM 循环传入）
    ASSERT(lun < LUN_NUM);

    memset(&g_raid_ctx[lun], 0, sizeof(fc_raid_context_t));
    memset(&g_raid_shadow[lun], 0, sizeof(fc_raid_context_t));
}

// 把第一个 CW 拷入累加器，并置序列为进行中。
fc_status_e fc_raid_start(BYTE lun, const BYTE *cw_buffer, BYTE total)
{
    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(cw_buffer != NULL);
    ASSERT(total != 0U);

    fc_raid_context_t *ctx = &g_raid_ctx[lun];

    if (ctx->active != 0U)
    {
        return FC_STATUS_NOT_READY;
    }

    memcpy(ctx->acc_buf, cw_buffer, FC_BUFFER_POOL_CW_SIZE);
    ctx->active    = 1U;
    ctx->processed = 1U;
    ctx->total     = total;
    return FC_STATUS_OK;
}

// 实现要点：按 32 位字对齐 XOR（528B / 4 = 132 字）。
fc_status_e fc_raid_accumulate(BYTE lun, const BYTE *cw_buffer)
{
    WORD         i;
    DWORD       *acc32;
    const DWORD *buf32;

    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(cw_buffer != NULL);

    fc_raid_context_t *ctx = &g_raid_ctx[lun];

    if (ctx->active == 0U)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    // 超累加 = 调用者计数 bug（total 由 fc_raid_start 给定）→ ASSERT
    ASSERT(ctx->processed < ctx->total);

    // 32 位对齐 XOR
    acc32 = (DWORD *)(void *)ctx->acc_buf;
    buf32 = (const DWORD *)(const void *)cw_buffer;
    for (i = 0; i < (FC_BUFFER_POOL_CW_SIZE / sizeof(DWORD)); i++)
    {
        acc32[i] ^= buf32[i];
    }

    ctx->processed++;
    return FC_STATUS_OK;
}

// 把累加器结果拷出到 parity buffer，并复位序列状态。
fc_status_e fc_raid_end(BYTE lun, BYTE *parity_buffer)
{
    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(parity_buffer != NULL);

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

// HSQ 打断保护：保存/恢复上下文（中断路径用）
void fc_raid_save(BYTE lun)
{
    if (lun < LUN_NUM)
    {
        g_raid_shadow[lun] = g_raid_ctx[lun];
    }
}

void fc_raid_restore(BYTE lun)
{
    if (lun < LUN_NUM)
    {
        g_raid_ctx[lun] = g_raid_shadow[lun];
    }
}

// 实现要点：recovered 初始为 0，异或跳过 failed_idx 的所有 buffer；
// 结果即 = 其余所有 buffer 的 XOR（RAID-5 重建公式）。
void fc_raid_recover(BYTE *recovered, BYTE *buffers[], BYTE count, BYTE failed_idx, WORD length)
{
    BYTE   i;
    DWORD *rec32    = (DWORD *)(void *)recovered;
    WORD   word_len = length / sizeof(DWORD);

    // 入参前置条件由调用者保证（fc_data_path 传入全 CW 收集结果）
    ASSERT(recovered != NULL);
    ASSERT(buffers != NULL);
    ASSERT(count != 0U);
    ASSERT(failed_idx < count);
    ASSERT(length != 0U);

    memset(recovered, 0, length);

    for (i = 0; i < count; i++)
    {
        if ((i == failed_idx) || (buffers[i] == NULL))
        {
            continue;
        }
        for (WORD w = 0U; w < word_len; w++)
        {
            rec32[w] ^= ((const DWORD *)(const void *)buffers[i])[w];
        }
    }
}

#endif /* CONFIG_FC_RAID */