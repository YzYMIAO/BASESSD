
#include "fc_ecc_engine.h"

#include "assertion.h"
#include "fc_bch_engine.h"
#include "fc_interface.h" /* FC_BUFFER_DATA_SIZE / FC_BUFFER_POOL_CW_SIZE（入口上界与 LDPC 分支用） */
#include "log.h"

#include <stddef.h>
#include <string.h>
// 全局 BCH / QC-LDPC 引擎实例（单例，首次使用惰性初始化）
static fc_bch_control_s *g_bch = NULL;

// 惰性初始化的单次日志标志：成功/失败各只打印一次，避免每次编解码刷屏。
static BOOL s_bch_init_failed = false;

#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)

#    define QC_Z      (16U)
#    define QC_N      (4224U)
#    define QC_K      (4160U)
#    define QC_M      (64U)
#    define QC_BASE_R (4U)
#    define QC_BASE_C (264U)
#    define QC_DATA_C (260U)

/* ── 全局查表结构 ──────────────────────────────────────────── */

// 基矩阵：每项 = 循环移位量（-1 = 全零子块，0..Z-1 = 循环移位）
static int8_t g_h_base[QC_BASE_R][QC_BASE_C];

// 列→校验方程链接：col_to_check[col][0..col_weight-1]
static BYTE g_col_to_check[QC_N][3];
static BYTE g_col_weight[QC_N]; /* 每列权重（固定为 3） */

// 行→变量节点链接
static WORD g_row_to_var[QC_M][QC_N * 3U / QC_M + 1U]; /* 每行约 198 个 */
static WORD g_row_weight[QC_M];

static BOOL g_initialized = false;

typedef struct FC_LDPC_QC_S
{
    BYTE initialized;
    BYTE max_iter;
} fc_ldpc_qc_s;

static fc_ldpc_qc_s *s_qc = NULL;

static fc_ldpc_qc_s *fc_ldpc_qc_init(BYTE max_iter);
static void          fc_ldpc_qc_encode(const BYTE *data_in, BYTE *data_out);
static fc_status_e   fc_ldpc_qc_decode(fc_ldpc_qc_s *qc, BYTE *data);

#endif

// fc_ecc_engine.c — ECC 统一入口（BCH / QC-LDPC 按编译宏切换）+ QC-LDPC 实现
//
// FC_ECC_ALGO 控制编译使用 BCH 或 LDPC。
//
// BCH（默认）：
// CW 布局：[META:4][CRC:4][USER:512][PARITY:8] = 528B
// DATA = META+CRC+USER = 520B, PARITY = 8B
// BCH(m=13, t=4)：52bit parity → 7B，8B 够用
// 编码：520B data → 8B parity
// 解码：520B data + 8B recv_parity → 检测/纠正
//
// LDPC（可选，FC_ECC_ALGO_LDPC 编译）：
// 同上 CW 布局，但不使用 BCH 算法，而是使用 QC-LDPC 硬判决 Bit-flipping。
// 注意：码率 0.985 下 LDPC 纠错能力约 t=1~2，低于 BCH t=4。
// LDPC 的真正优势在低码率（r<0.9）和软判决场景。

#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)
/**
 * @brief 惰性初始化 QC-LDPC 引擎（最大迭代 15）
 *
 * @return 1 = 引擎可用
 *         0 = 初始化失败
 */
static BYTE fc_ecc_init_ldpc(void)
{
    if (g_qc == NULL)
    {
        g_qc = fc_ldpc_qc_init(15U);
    }
    return (g_qc != NULL) ? 1U : 0U;
}
#endif

#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)

/* ── 伪随机数生成器（固定种子，保证每次编译生成相同 H） ────── */

/**
 * @brief 线性同余 PRNG：state = state*1103515245 + 12345（标准 LCG 参数）
 *
 * @param state 随机数状态（就地更新）
 * @return 本次生成的 32 位随机值（等于更新后的 state）
 */
static DWORD lcg_rand(DWORD *state)
{
    *state = *state * 1103515245U + 12345U;
    return *state;
}

/* ── 构造 ──────────────────────────────────────────── */

/**
 * @brief 构建基矩阵：数据列随机指派 3 行 + 随机移位；校验列阶梯结构
 */
static void qc_build_base(void)
{
    DWORD seed = 0x20250921UL;
    DWORD r, c;

    memset(g_h_base, -1, sizeof(g_h_base));

    // 数据部分（0..QC_DATA_C-1）：每列 3 个非 -1 条目
    // （列重 dv=3：随机选 3 个互不相同的行，各给随机循环移位量）
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

    // 校验部分：阶梯结构，H_p = diag(恒等阵)
    // 使 H_p 可逆且编码退化（见文件头编码原理）
    for (r = 0; r < QC_BASE_R; r++)
    {
        g_h_base[r][QC_DATA_C + r] = 0;
    }
}

/**
 * @brief 从基矩阵展开链接：每个子块条目展开成 Z 个变量↔校验连接
 */
static void qc_expand_links(void)
{
    DWORD r, c, s;
    DWORD col_pos[QC_N];
    DWORD row_pos[QC_M];

    // 清零
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
                // 循环移位：变量 (c*Z+s) 连到校验 (r*Z + (s+shift)%Z)
                DWORD var_idx = c * QC_Z + s;
                DWORD chk_idx = r * QC_Z + ((s + shift) % QC_Z);
                DWORD p;

                // 列→校验
                p                          = col_pos[var_idx];
                g_col_to_check[var_idx][p] = chk_idx;
                g_col_weight[var_idx]      = (p + 1U);
                col_pos[var_idx]           = p + 1U;

                // 行→变量
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

/**
 * @brief QC-LDPC 编码：拷 520B 数据，阶梯结构逐位累加出 8B parity
 *
 * 引擎未初始化时静默返回不写 data_out——调用方须保证先 init。
 *
 * @param data_in 528B 码字缓冲（前 520B 为数据）
 * @param data_out 输出：528B 码字（数据 + parity）
 *
 * @note 校验矩阵为文件内静态表，不需要引擎句柄。
 */
static void fc_ldpc_qc_encode(const BYTE *data_in, BYTE *data_out)
{
    BYTE  parity[8];
    DWORD bit_pos, i;

    ASSERT(data_in != NULL);
    ASSERT(data_out != NULL);

    if (g_initialized == false)
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

        // 此数据位关联的所有校验位取反（列重固定为 3）
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

/**
 * @brief QC-LDPC 硬判决解码（Bit-flipping）
 *
 * 每轮算综合式，全零即成功；失败校验数达自适应阈值 min(3, syn_ones) 的
 * 变量位翻转；无位可翻说明陷入不动点，判失败。
 *
 * @param qc   引擎句柄（取其 max_iter）
 * @param data 528B 码字缓冲（原地纠正）
 * @return FC_STATUS_OK / FC_STATUS_IO_ERROR（不可纠）/ FC_STATUS_INVALID_ARGUMENT（未初始化）
 */
static fc_status_e fc_ldpc_qc_decode(fc_ldpc_qc_s *qc, BYTE *data)
{
    BYTE  syndromes[QC_M];
    BYTE  col_failures[QC_N];
    DWORD iter;
    DWORD i, var, chk;
    DWORD threshold;
    DWORD max_iter;

    // qc/data 为入参前置条件；g_initialized 是运行时初始化状态，保留返回码
    ASSERT(qc != NULL);
    ASSERT(data != NULL);
    if (g_initialized == false)
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

            // 翻转所有失败校验数 ≥ 阈值的变量
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
                // 无翻转 → 陷入不动点，直接判失败
                break;
            }
        }
    }

    return FC_STATUS_IO_ERROR;
}

/* ═══════════════════════════════════════════════════════════════
 * 初始化
 * ═══════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化 QC-LDPC 引擎（首次调用建基矩阵并展开链接）
 *
 * 后续调用直接返回同一静态单例。
 *
 * @param max_iter 最大迭代轮数，0 表示取默认 15
 * @return 引擎句柄
 */
static fc_ldpc_qc_s *fc_ldpc_qc_init(BYTE max_iter)
{
    static fc_ldpc_qc_s g_qc;

    if (g_initialized)
    {
        return &g_qc;
    }

    g_qc.initialized = 1U;
    g_qc.max_iter    = (max_iter > 0U) ? max_iter : 15U;

    qc_build_base();
    qc_expand_links();

    g_initialized = true;
    return &g_qc;
}

/**
 * @brief 释放 QC-LDPC 引擎
 *
 * 查表为全局静态数组、句柄为静态单例，无动态资源可回收，本函数为空操作；
 * 保留该接口以对齐其他 ECC 引擎的 init/free 生命周期。
 *
 * @param qc 引擎句柄（未使用）
 */

#endif /* FC_ECC_ALGO_LDPC */

/**
 * @brief 惰性初始化 BCH 引擎（m=13, t=4）
 *
 * 初始化失败必须可见：编码/解码不得“成功跳过”，否则等于静默关闭 ECC（C8/§3.2）。
 * 表为静态存储，失败只可能是参数/断言错误。
 *
 * @return 1 = 引擎可用
 *         0 = 初始化失败（失败已 LOG 一次，此后 ECC 不可用）
 */
static BYTE fc_ecc_init_bch(void)
{
    if (g_bch == NULL)
    {
        g_bch = fc_bch_init(13, 4, 0U, 0U);
        if (g_bch == NULL)
        {
            if (s_bch_init_failed == false)
            {
                s_bch_init_failed = true;
                LOGE("[FC  ] BCH init FAILED: ECC is disabled (check static storage/size)\r\n");
            }
        }
    }
    return (g_bch != NULL) ? 1U : 0U;
}

/**
 * @brief 当前编译出的 ECC 算法名
 *
 * @return "BCH(13,4)" 或 "LDPC"（随 FC_ECC_ALGO 切换）
 */
const char *fc_ecc_algo_name(void)
{
#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)
    return "LDPC";
#else
    return "BCH(13,4)";
#endif
}

/**
 * @brief ECC 编码统一入口：对 data[0..data_length) 编码，parity 写在其后 8B
 *
 * BCH 分支由 fc_bch_encode 生成 8B parity；LDPC 分支以 528B 完整码字为输入输出。
 * 引擎不可用时返回 NOT_SUPPORTED；调用方必须让命令失败，不得静默跳过 ECC。
 *
 * @param data        CW 缓冲（528B）
 * @param data_length 数据区字节数（≤ FC_BUFFER_DATA_SIZE = 520）
 * @return FC_STATUS_OK / FC_STATUS_NOT_SUPPORTED
 */
fc_status_e fc_ecc_encode(BYTE *data, WORD data_length)
{
    // 入参前置条件由调用者保证（fc_data_path 恒传 CW 缓冲 + FC_BUFFER_DATA_SIZE）
    ASSERT(data != NULL);
    ASSERT(data_length != 0U);
    // 上界：parity 尾区占 data[data_length..data_length+7]，CW 缓冲恒 528B
    ASSERT(data_length <= FC_BUFFER_DATA_SIZE);

#if (FC_ECC_ALGO == FC_ECC_ALGO_BCH)

    /* ── BCH 编码 ── */
    if (fc_ecc_init_bch() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        BYTE ecc_buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        // data_length 恒 ≥ 8（520），parity 尾区足够
        ASSERT(data_length >= 8U);

        fc_bch_encode(g_bch, data, data_length, ecc_buf);
        memcpy(data + data_length, ecc_buf, 8U);
    }

    return FC_STATUS_OK;

#elif (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)

    /* ── QC-LDPC 编码 ── */
    if (fc_ecc_init_ldpc() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        // data_length = 520（数据区），后 8B = parity 输出区
        WORD full_len = (data_length + 8U);
        BYTE tmp[FC_BUFFER_POOL_CW_SIZE];

        memcpy(tmp, data, full_len);
        fc_ldpc_qc_encode(tmp, data);
    }

    return FC_STATUS_OK;

#else
#    error "Unknown FC_ECC_ALGO value"
#endif
}

/**
 * @brief ECC 解码统一入口：校验 data + 其后 8B parity，并原地纠正数据区
 *
 * @param data        CW 缓冲（528B）
 * @param data_length 数据区字节数（≤ FC_BUFFER_DATA_SIZE = 520）
 * @param err_bits    输出：纠正的 bit 数（可 NULL）
 * @return FC_STATUS_OK / FC_STATUS_NOT_SUPPORTED / FC_STATUS_IO_ERROR（不可纠）
 */
fc_status_e fc_ecc_decode(BYTE *data, WORD data_length, BYTE *err_bits)
{
    if (err_bits != NULL)
    {
        *err_bits = 0U;
    }

    // 入参前置条件由调用者保证（fc_data_path 恒传 CW 缓冲 + FC_BUFFER_DATA_SIZE）；
    // err_bits 可为 NULL（辨析 B），保留上方语义分支
    ASSERT(data != NULL);
    ASSERT(data_length != 0U);
    // 上界：recv_ecc = data + data_length 起读 8B，CW 缓冲恒 528B
    ASSERT(data_length <= FC_BUFFER_DATA_SIZE);

#if (FC_ECC_ALGO == FC_ECC_ALGO_BCH)

    /* ── BCH 解码 ── */
    if (fc_ecc_init_bch() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        BYTE        *recv_ecc = data + data_length;
        unsigned int errloc[4];
        unsigned int nerr;
        WORD         i;

        // data_length 恒 ≥ 8（520），parity 尾区足够
        ASSERT(data_length >= 8U);

        if (fc_bch_decode(g_bch, data, data_length, recv_ecc, errloc, &nerr) == false)
        {
            return FC_STATUS_IO_ERROR;
        }

        // BCH 成功解码：报告纠正的 bit 数（0..t）
        if (err_bits != NULL)
        {
            *err_bits = nerr;
        }

        for (i = 0U; i < nerr; i++)
        {
            unsigned int bit_pos = errloc[i];
            if (bit_pos < (unsigned int)(data_length * 8U))
            {
                data[bit_pos / 8U] ^= (1U << (bit_pos % 8U));
            }
        }

        return FC_STATUS_OK;
    }

#elif (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)

    /* ── QC-LDPC 硬判决解码 ── */
    if (fc_ecc_init_ldpc() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    // data_length = 520（来自调用者），但 QC-LDPC 解码器硬码
    // 码长 n=4224bit=528B（520B 数据 + 8B parity），内部直接
    // 按 528B 操作 buffer。buffer 实际分配为 FC_BUFFER_POOL_CW_SIZE(528)，
    // 访问安全。此处显式抑制未使用参数警告。
    (void)data_length;
    return fc_ldpc_qc_decode(g_qc, data);

#else
#    error "Unknown FC_ECC_ALGO value"
#endif
}
