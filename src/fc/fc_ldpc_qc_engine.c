#include "fc_ldpc_qc_engine.h"

#include "assertion.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * fc_ldpc_qc_engine.c — QC-LDPC 引擎实现（硬判决 Bit-flipping）
 *
 * ── 参数 ──
 *   码长  n = 4224 bit (528B)
 *   信息  k = 4160 bit (520B)   ── META(4) + CRC(4) + USER(512)
 *   校验  m = 64  bit (8B)
 *   子块  Z = 16
 *   基矩阵 H_base: 4 × 264
 *   列重 dv = 3（固定）
 *
 * ── QC-LDPC 构造原理 ──
 *   校验矩阵 H 由 Z×Z 循环子矩阵组成（QC = Quasi-Cyclic）：
 *   基矩阵每个条目 (row,col)=shift 表示一个 Z×Z 循环移位单位阵
 *   （shift 为该子块内单位阵的循环移位量，-1 表示全零块）。
 *   码率 R = k/n = 4160/4224 ≈ 0.985，高码率下纠错能力约 t=1~2。
 *
 * ── 编码原理 ──
 *   校验位 p 满足 H·[data|p]^T = 0，即 p = H_p⁻¹ · H_d · data^T。
 *   校验部分选"阶梯结构"（H_p = diag 单位阵），使编码退化为
 *   逐数据位：data_bit=1 时取反其关联的所有校验位（列重 dv 次异或）。
 *
 * ── 解码原理（硬判决 Bit-flipping）──
 *   ① 综合式 s = H × y^T（逐校验行 XOR 变量节点）
 *   ② 对每个变量节点统计关联的失败校验数 fi
 *   ③ fi ≥ 阈值 → 翻转该比特（阈值随 syn_ones 自适应：min(3, syn_ones)）
 *   ④ 重复直至 s=0（成功）或达到 max_iter（失败）
 *
 * ── 存储设计 ──
 *   所有查表结构均使用全局静态数组，无动态分配。
 *   基矩阵   = g_h_base[4][264]       int8_t   1.0 KB
 *   列→校验  = g_col_to_check[4224][3] BYTE    12.0 KB
 *   行→变量  = g_row_to_var[64][198]  WORD   24.8 KB
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

/* 基矩阵：每项 = 循环移位量（-1 = 全零子块，0..Z-1 = 循环移位） */
static int8_t g_h_base[QC_BASE_R][QC_BASE_C];

/* 列→校验方程链接：col_to_check[col][0..col_weight-1] */
static BYTE g_col_to_check[QC_N][3];
static BYTE g_col_weight[QC_N]; /* 每列权重（固定为 3） */

/* 行→变量节点链接 */
static WORD g_row_to_var[QC_M][QC_N * 3U / QC_M + 1U]; /* 每行约 198 个 */
static WORD g_row_weight[QC_M];

static BYTE g_initialized = 0U;

/* ── 伪随机数生成器（固定种子，保证每次编译生成相同 H） ────── */

/* 线性同余 PRNG：state = state*1103515245 + 12345（标准 LCG 参数） */
static DWORD lcg_rand(DWORD *state)
{
    *state = *state * 1103515245U + 12345U;
    return *state;
}

/* ── 构造 ──────────────────────────────────────────── */

/* 构建基矩阵：数据列随机指派 3 行 + 随机移位；校验列阶梯结构 */
static void qc_build_base(void)
{
    DWORD seed = 0x20250921UL;
    DWORD r, c;

    memset(g_h_base, -1, sizeof(g_h_base));

    /* 数据部分（0..QC_DATA_C-1）：每列 3 个非 -1 条目
     * （列重 dv=3：随机选 3 个互不相同的行，各给随机循环移位量） */
    for (c = 0; c < QC_DATA_C; c++)
    {
        DWORD used_rows = 0U;
        DWORD i;

        for (i = 0; i < 3U; i++)
        {
            DWORD row;
            DWORD shift;
            DWORD attempt;

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

    /* 校验部分：阶梯结构，H_p = diag(恒等阵)
     * 使 H_p 可逆且编码退化（见文件头编码原理） */
    for (r = 0; r < QC_BASE_R; r++)
    {
        g_h_base[r][QC_DATA_C + r] = 0;
    }
}

/* 从基矩阵展开链接：每个子块条目展开成 Z 个变量↔校验连接 */
static void qc_expand_links(void)
{
    DWORD r, c, s;
    DWORD col_pos[QC_N];
    DWORD row_pos[QC_M];

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
                continue; /* 全零子块，无连接 */
            }
            DWORD shift = g_h_base[r][c];
            for (s = 0; s < QC_Z; s++)
            {
                /* 循环移位：变量 (c*Z+s) 连到校验 (r*Z + (s+shift)%Z) */
                DWORD var_idx = c * QC_Z + s;
                DWORD chk_idx = r * QC_Z + ((s + shift) % QC_Z);
                DWORD p;

                /* 列→校验 */
                p                          = col_pos[var_idx];
                g_col_to_check[var_idx][p] = chk_idx;
                g_col_weight[var_idx]      = (p + 1U);
                col_pos[var_idx]           = p + 1U;

                /* 行→变量 */
                p                        = row_pos[chk_idx];
                g_row_to_var[chk_idx][p] = var_idx;
                g_row_weight[chk_idx]    = (p + 1U);
                row_pos[chk_idx]         = p + 1U;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * 编码
 * ═══════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_ldpc_qc_encode
 *   功能：QC-LDPC 编码——520B 数据 → 528B 码字（520B + 8B parity）。
 *   参数：qc —— 引擎；data_in —— 输入 520B；data_out —— 输出 528B。
 *   返回：无（未初始化静默返回）。
 *   实现要点：阶梯结构编码——数据位为 1 时异或其关联的校验位
 *         （parity 初始为 0，逐数据位累加即得校验向量）。
 *         入参前置违反（qc/data_in/data_out 为空）一律 ASSERT（v21，与同文件
 *         fc_ldpc_qc_decode 的写法一致）；g_initialized 是运行时状态，保留静默返回。
 * -------------------------------------------------------------------- */
void fc_ldpc_qc_encode(const fc_ldpc_qc_t *qc, const BYTE *data_in, BYTE *data_out)
{
    BYTE  parity[8];
    DWORD bit_pos, i;

    ASSERT(qc != NULL);
    ASSERT(data_in != NULL);
    ASSERT(data_out != NULL);

    (void)qc; /* qc 本实现未用（校验矩阵为文件内静态表）；NDEBUG 下断言裁空后靠它消警告 */

    if (!g_initialized)
    {
        return;
    }

    memcpy(data_out, data_in, 520);
    memset(parity, 0, 8);

    for (bit_pos = 0; bit_pos < QC_K; bit_pos++)
    {
        BYTE data_bit = ((data_in[bit_pos / 8U] >> (bit_pos % 8U)) & 1U);
        if (data_bit == 0U)
        {
            continue;
        }

        /* 此数据位关联的所有校验位取反（列重固定为 3） */
        DWORD w = g_col_weight[bit_pos];
        for (i = 0; i < w; i++)
        {
            DWORD chk = g_col_to_check[bit_pos][i];
            parity[chk / 8U] ^= (1U << (chk % 8U));
        }
    }

    memcpy(data_out + 520, parity, 8);
}

/* ═══════════════════════════════════════════════════════════════
 * 解码（硬判决 Bit-flipping）
 * ═══════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_ldpc_qc_decode
 *   功能：QC-LDPC 硬判决解码——翻转失败校验关联多的比特直至综合式归零。
 *   参数：qc —— 引擎；data —— 528B 码字（原地修改返回纠正结果）。
 *   返回：FC_STATUS_OK（s=0）/ IO_ERROR（超迭代或无可翻转）/
 *         INVALID_ARGUMENT。
 *   实现要点（Bit-flipping 三阶段）：
 *     - 综合式：逐校验行 XOR 变量节点，统计失败校验数 syn_ones。
 *     - 失败计数：对每个失败校验，其关联变量 fi++。
 *     - 翻转：fi ≥ min(3, syn_ones) 的变量翻转；无翻转则提前退出。
 * -------------------------------------------------------------------- */
fc_status_e fc_ldpc_qc_decode(fc_ldpc_qc_t *qc, BYTE *data)
{
    BYTE  syndromes[QC_M];
    BYTE  col_failures[QC_N];
    DWORD iter;
    DWORD i, var, chk;
    DWORD threshold;
    DWORD max_iter;

    /* qc/data 为入参前置条件；g_initialized 是运行时初始化状态，保留返回码 */
    ASSERT(qc != NULL);
    ASSERT(data != NULL);
    if (!g_initialized)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    max_iter = qc->max_iter;

    for (iter = 0; iter < max_iter; iter++)
    {
        DWORD syn_ones = 0U;

        /* ── 综合式 s = H × y^T ── */
        memset(syndromes, 0, sizeof(syndromes));

        for (chk = 0; chk < QC_M; chk++)
        {
            DWORD sum = 0U;
            DWORD w   = g_row_weight[chk];

            for (i = 0; i < w; i++)
            {
                var = g_row_to_var[chk][i];
                sum ^= ((data[var / 8U] >> (var % 8U)) & 1U);
            }
            syndromes[chk] = sum;
            syn_ones += sum;
        }

        if (syn_ones == 0U)
        {
            return FC_STATUS_OK; /* 码字合法，解码成功 */
        }

        /* ── 统计每列失败校验数 ── */
        memset(col_failures, 0, sizeof(col_failures));

        for (chk = 0; chk < QC_M; chk++)
        {
            if (syndromes[chk] == 0U)
            {
                continue;
            }
            DWORD w = g_row_weight[chk];
            for (i = 0; i < w; i++)
            {
                var = g_row_to_var[chk][i];
                col_failures[var]++;
            }
        }

        /* ── 阈值递减：min(3, syn_ones)，至少为 1 ── */
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
            DWORD flipped = 0U;

            /* 翻转所有失败校验数 ≥ 阈值的变量 */
            for (var = 0; var < QC_N; var++)
            {
                if (col_failures[var] >= threshold)
                {
                    data[var / 8U] ^= (1U << (var % 8U));
                    flipped++;
                }
            }

            if (flipped == 0U)
            {
                /* 无翻转 → 陷入不动点，直接判失败 */
                break;
            }
        }
    }

    return FC_STATUS_IO_ERROR;
}

/* ═══════════════════════════════════════════════════════════════
 * 初始化
 * ═══════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_ldpc_qc_init
 *   功能：单例初始化 QC-LDPC 引擎（首次调用构造查表，全局复用）。
 *   参数：max_iter —— 最大解码迭代（0 时默认 15）。
 *   返回：引擎句柄（全局单例，静态）。
 * -------------------------------------------------------------------- */
fc_ldpc_qc_t *fc_ldpc_qc_init(BYTE max_iter)
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