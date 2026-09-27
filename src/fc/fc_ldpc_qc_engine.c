#include "fc_ldpc_qc_engine.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * QC-LDPC 引擎实现（硬判决 Bit-flipping）。
 *
 * ── 参数 ──
 *   码长  n = 4224 bit (528B)
 *   信息  k = 4160 bit (520B)   ── META(4) + CRC(4) + USER(512)
 *   校验  m = 64  bit (8B)
 *   子块  Z = 16
 *   基矩阵 H_base: 4 × 264
 *   列重 dv = 3（固定）
 *
 * ── 存储设计 ──
 *   所有查表结构均使用全局静态数组，无动态分配。
 *   基矩阵   = g_h_base[4][264]       int8_t   1.0 KB
 *   列→校验  = g_col_to_check[4224][3] uint8_t    12.0 KB
 *   行→变量  = g_row_to_var[64][198]  uint16_t   24.8 KB
 *   （行重统计随列重计算得出）
 */

#define QC_Z      (16U)
#define QC_N      (4224U)
#define QC_K      (4160U)
#define QC_M      (64U)
#define QC_BASE_R (4U)
#define QC_BASE_C (264U)
#define QC_DATA_C (260U)

/* ── 全局查表结构 ──────────────────────────────────────────── */

/* 基矩阵 */
static int8_t g_h_base[QC_BASE_R][QC_BASE_C];

/* 列→校验方程链接：col_to_check[col][0..col_weight-1] */
static uint8_t g_col_to_check[QC_N][3];
static uint8_t g_col_weight[QC_N]; /* 每列权重（固定为 3） */

/* 行→变量节点链接 */
static uint16_t g_row_to_var[QC_M][QC_N * 3U / QC_M + 1U]; /* 每行约 198 个 */
static uint16_t g_row_weight[QC_M];

static uint8_t g_initialized = 0U;

/* ── 伪随机数生成器（固定种子） ────────────────────────────── */

static uint32_t lcg_rand(uint32_t *state)
{
    *state = *state * 1103515245U + 12345U;
    return *state;
}

/* ── 构造 ──────────────────────────────────────────── */

static void qc_build_base(void)
{
    uint32_t seed = 0x20250921UL;
    uint32_t r, c;

    memset(g_h_base, -1, sizeof(g_h_base));

    /* 数据部分（0..QC_DATA_C-1）：每列 3 个非 -1 条目 */
    for (c = 0; c < QC_DATA_C; c++)
    {
        uint32_t used_rows = 0U;
        uint32_t i;

        for (i = 0; i < 3U; i++)
        {
            uint32_t row;
            uint32_t shift;
            uint32_t attempt;

            for (attempt = 0; attempt < 100U; attempt++)
            {
                row = lcg_rand(&seed) % QC_BASE_R;
                if (((used_rows >> row) & 1U) == 0U)
                {
                    break;
                }
            }
            used_rows |= (1U << row);
            shift            = lcg_rand(&seed) % QC_Z;
            g_h_base[row][c] = (int8_t)shift;
        }
    }

    /* 校验部分：阶梯结构，H_p = diag(恒等阵) */
    for (r = 0; r < QC_BASE_R; r++)
    {
        g_h_base[r][QC_DATA_C + r] = 0;
    }
}

static void qc_expand_links(void)
{
    uint32_t r, c, s;
    uint32_t col_pos[QC_N];
    uint32_t row_pos[QC_M];

    /* 清零 */
    memset(g_col_weight, 0, sizeof(g_col_weight));
    memset(g_row_weight, 0, sizeof(g_row_weight));
    memset(col_pos, 0, sizeof(col_pos));
    memset(row_pos, 0, sizeof(row_pos));

    for (r = 0; r < QC_BASE_R; r++)
    {
        for (c = 0; c < QC_BASE_C; c++)
        {
            if (g_h_base[r][c] < 0)
            {
                continue;
            }
            uint32_t shift = (uint32_t)g_h_base[r][c];
            for (s = 0; s < QC_Z; s++)
            {
                uint32_t var_idx = c * QC_Z + s;
                uint32_t chk_idx = r * QC_Z + ((s + shift) % QC_Z);
                uint32_t p;

                /* 列→校验 */
                p                          = col_pos[var_idx];
                g_col_to_check[var_idx][p] = chk_idx;
                g_col_weight[var_idx]      = (uint8_t)(p + 1U);
                col_pos[var_idx]           = p + 1U;

                /* 行→变量 */
                p                        = row_pos[chk_idx];
                g_row_to_var[chk_idx][p] = (uint16_t)var_idx;
                g_row_weight[chk_idx]    = (uint16_t)(p + 1U);
                row_pos[chk_idx]         = p + 1U;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * 编码
 * ═══════════════════════════════════════════════════════════════ */

void fc_ldpc_qc_encode(const fc_ldpc_qc_t *qc,
                       const uint8_t      *data_in,
                       uint8_t            *data_out)
{
    uint8_t  parity[8];
    uint32_t bit_pos, i;

    (void)qc;

    if (!g_initialized)
    {
        return;
    }

    memcpy(data_out, data_in, 520);
    memset(parity, 0, 8);

    for (bit_pos = 0; bit_pos < QC_K; bit_pos++)
    {
        uint8_t data_bit = (uint8_t)((data_in[bit_pos / 8U] >> (bit_pos % 8U)) & 1U);
        if (data_bit == 0U)
        {
            continue;
        }

        /* 此数据位关联的所有校验位取反（列重固定为 3） */
        uint32_t w = (uint32_t)g_col_weight[bit_pos];
        for (i = 0; i < w; i++)
        {
            uint32_t chk = g_col_to_check[bit_pos][i];
            parity[chk / 8U] ^= (uint8_t)(1U << (chk % 8U));
        }
    }

    memcpy(data_out + 520, parity, 8);
}

/* ═══════════════════════════════════════════════════════════════
 * 解码（硬判决 Bit-flipping）
 * ═══════════════════════════════════════════════════════════════ */

fc_status_t fc_ldpc_qc_decode(fc_ldpc_qc_t *qc, uint8_t *data)
{
    uint8_t  syndromes[QC_M];
    uint8_t  col_failures[QC_N];
    uint32_t iter;
    uint32_t i, var, chk;
    uint32_t threshold;
    uint32_t max_iter;

    if (!qc || !data || !g_initialized)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    max_iter = (uint32_t)qc->max_iter;

    for (iter = 0; iter < max_iter; iter++)
    {
        uint32_t syn_ones = 0U;

        /* ── 综合式 s = H × y^T ── */
        memset(syndromes, 0, sizeof(syndromes));

        for (chk = 0; chk < QC_M; chk++)
        {
            uint32_t sum = 0U;
            uint32_t w   = (uint32_t)g_row_weight[chk];

            for (i = 0; i < w; i++)
            {
                var = (uint32_t)g_row_to_var[chk][i];
                sum ^= (uint32_t)((data[var / 8U] >> (var % 8U)) & 1U);
            }
            syndromes[chk] = sum;
            syn_ones += sum;
        }

        if (syn_ones == 0U)
        {
            return FC_STATUS_OK;
        }

        /* ── 统计每列失败校验数 ── */
        memset(col_failures, 0, sizeof(col_failures));

        for (chk = 0; chk < QC_M; chk++)
        {
            if (syndromes[chk] == 0U)
            {
                continue;
            }
            uint32_t w = (uint32_t)g_row_weight[chk];
            for (i = 0; i < w; i++)
            {
                var = (uint32_t)g_row_to_var[chk][i];
                col_failures[var]++;
            }
        }

        /* ── 阈值递减 ── */
        threshold = 3U;
        if (threshold > syn_ones)
        {
            threshold = syn_ones;
        }
        if (threshold == 0U)
        {
            threshold = 1U;
        }

        {
            uint32_t flipped = 0U;

            for (var = 0; var < QC_N; var++)
            {
                if (col_failures[var] >= threshold)
                {
                    data[var / 8U] ^= (uint8_t)(1U << (var % 8U));
                    flipped++;
                }
            }

            if (flipped == 0U)
            {
                if (threshold > 1U)
                {
                    /* 下次迭代降阈，但旧阈无翻转，直返 IO_ERROR */
                }
                break;
            }
        }
    }

    return FC_STATUS_IO_ERROR;
}

/* ═══════════════════════════════════════════════════════════════
 * 初始化
 * ═══════════════════════════════════════════════════════════════ */

fc_ldpc_qc_t *fc_ldpc_qc_init(uint8_t max_iter)
{
    static fc_ldpc_qc_t g_qc;

    if (g_initialized)
    {
        return &g_qc;
    }

    g_qc.initialized = 1U;
    g_qc.max_iter    = (max_iter > 0U) ? max_iter : 15U;

    qc_build_base();
    qc_expand_links();

    g_initialized = 1U;
    return &g_qc;
}

void fc_ldpc_qc_free(fc_ldpc_qc_t *qc)
{
    (void)qc;
    /* 全局静态数组，无需释放 */
}