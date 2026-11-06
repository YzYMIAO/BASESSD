#include "fc_raid_engine.h"

#include "assertion.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════
 * Stripe RAID 引擎
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

// RAID XOR 引擎
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

static fc_raid_ctx_s g_raid_ctx[LUN_NUM];
static fc_raid_ctx_s g_raid_shadow[LUN_NUM];

/**
 * @brief 清空该 LUN 的 RAID 上下文与影子缓冲
 *
 * @param lun LUN 号（< LUN_NUM）
 */
void fc_raid_init_ctx(BYTE lun)
{
    // 前置条件由调用者保证（fc.c 按 LUN_NUM 循环传入）
    ASSERT(lun < LUN_NUM);

    memset(&g_raid_ctx[lun], 0, sizeof(fc_raid_ctx_s));
    memset(&g_raid_shadow[lun], 0, sizeof(fc_raid_ctx_s));
}

/**
 * @brief 以第一个 CW 作为 XOR 累加器初值，启动一次 parity 累积
 *
 * @param lun       LUN 号（< LUN_NUM）
 * @param cw_buffer 首个 CW（528B）
 * @param total     本组 CW 总数（含首个）
 * @return FC_STATUS_OK / FC_STATUS_NOT_READY（该 LUN 已有活跃序列）
 */
fc_status_e fc_raid_start(BYTE lun, const BYTE *cw_buffer, BYTE total)
{
    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(cw_buffer != NULL);
    ASSERT(total != 0U);

    fc_raid_ctx_s *ctx = &g_raid_ctx[lun];

    if (ctx->active == true)
    {
        return FC_STATUS_NOT_READY;
    }

    memcpy(ctx->acc_buf, cw_buffer, FC_BUFFER_POOL_CW_SIZE);
    ctx->active    = true;
    ctx->processed = 1U;
    ctx->total     = total;
    return FC_STATUS_OK;
}

/**
 * @brief 把一个 CW 按 32 位字异或进累加器
 *
 * @param lun       LUN 号（< LUN_NUM）
 * @param cw_buffer CW（528B）
 * @return FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（无活跃序列）
 */
fc_status_e fc_raid_accumulate(BYTE lun, const BYTE *cw_buffer)
{
    WORD         i;
    DWORD       *acc32;
    const DWORD *buf32;

    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(cw_buffer != NULL);

    fc_raid_ctx_s *ctx = &g_raid_ctx[lun];

    if (ctx->active == false)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    // 超累加 = 调用者计数 bug（total 由 fc_raid_start 给定）→ ASSERT + release 拒绝
    ASSERT(ctx->processed < ctx->total);
    if (ctx->processed >= ctx->total)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

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

/**
 * @brief 把累加器结果拷出为 parity CW 并复位序列状态
 *
 * @param lun           LUN 号（< LUN_NUM）
 * @param parity_buffer 输出：528B parity
 * @return FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（无活跃序列）
 */
fc_status_e fc_raid_end(BYTE lun, BYTE *parity_buffer)
{
    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(parity_buffer != NULL);

    fc_raid_ctx_s *ctx = &g_raid_ctx[lun];

    if (ctx->active == false)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    ASSERT(ctx->processed == ctx->total);
    if (ctx->processed != ctx->total)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    memcpy(parity_buffer, ctx->acc_buf, FC_BUFFER_POOL_CW_SIZE);

    ctx->active    = false;
    ctx->processed = 0U;
    ctx->total     = 0U;
    return FC_STATUS_OK;
}

/**
 * @brief 保存该 LUN 的 RAID 上下文到影子缓冲（HSQ 打断保护）
 *
 * @param lun LUN 号；越界时静默跳过
 */
void fc_raid_save(BYTE lun)
{
    if (lun < LUN_NUM)
    {
        g_raid_shadow[lun] = g_raid_ctx[lun];
    }
}
/**
 * @brief 从影子缓冲恢复该 LUN 的 RAID 上下文
 *
 * @param lun LUN 号；越界时静默跳过
 */
void fc_raid_restore(BYTE lun)
{
    if (lun < LUN_NUM)
    {
        g_raid_ctx[lun] = g_raid_shadow[lun];
    }
}

/**
 * @brief RAID-5 重建：recovered = 除 failed_idx 外所有 CW 的 XOR
 *
 * 异或自逆，结果与被恢复 CW 处于同一 scrambled domain。
 *
 * @param recovered  输出：重建结果（length 字节）
 * @param buffers    CW 数组（NULL 项跳过）
 * @param count      数组长度（非 0）
 * @param failed_idx 失败 CW 下标（< count）
 * @param length     重建长度（字节）
 */
void fc_raid_recover(BYTE *recovered, BYTE *buffers[], BYTE count, BYTE failed_idx, WORD length)
{
    BYTE   i;
    DWORD *rec32    = (DWORD *)(void *)recovered;
    WORD   word_len = length / sizeof(DWORD);

    // 入参前置条件由调用者保证（fc_data_path_raid_recover 传入全 CW 收集结果）
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
