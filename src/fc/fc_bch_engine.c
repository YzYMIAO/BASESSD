#include "fc_bch_engine.h"

#include "assertion.h"
#include "ssd_compiler.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// fc_bch_engine.c — BCH ECC 引擎（clean-room 实现）
//
// 码参数：GF(2^13)，生成根 α^1 / α^3 / α^5 / α^7（t=4）。全码 n=8191 bit；
// 本固件用缩短码：520B 数据（4160 bit）+ 52 bit parity = 4212 bit。
// parity 以 MSB-first 存 7 字节；调用者的 8B ECC 槽第 8 字节保持 0。
//
// 位序：data[0] 的 bit7 是最高次系数；32-bit 字内按大端字节序处理。
// decode 输出沿用 API 约定：bit0 = data[0] 的 LSB，调用方用
// `data[pos / 8] ^= 1U << (pos % 8)` 纠正。
//
// 算法：独立实现 GF(2^13) 对数/反对数表、生成多项式、LFSR 编码、
// Berlekamp–Massey 与根搜索；不含 Linux lib/bch.c 代码。项目许可 Apache-2.0。
// 编码一次处理 32 bit；解码由「数据重编码 parity」与「接收 parity」的差值
// 算奇数 syndrome S1/S3/S5/S7（O(1)），偶数由平方导出；根搜索 degree 1/2
// 用闭式；3/4 先做次数专用变换（三次乘线性因子、四次消一次项后取倒数）
// 化成 affine 四次式 x^4+a·x^2+b·x+c，再用 GF(2) 线性方程组求根，
// 避免 O(nbits) 的 Chien 全扫描。
// parity、错误位置与增量编码的兼容性由 host 差分测试覆盖。

#define BCH_M              (13U)
#define BCH_N              ((1U << BCH_M) - 1U)
#define BCH_T              (4U)
#define BCH_ECC_BITS       (BCH_M * BCH_T) /* 52 */
#define BCH_ECC_BYTES      ((BCH_ECC_BITS + 7U) / 8U)
#define BCH_PRIM_POLY      (0x201BU)
#define BCH_MASK52         ((UINT64_C(1) << BCH_ECC_BITS) - UINT64_C(1))
#define BCH_MAX_DATA_LEN   ((BCH_N - BCH_ECC_BITS) / 8U)
#define BCH_ENC_WORD_BYTES (4U) /* 32-bit 编码字 = 4 byte */
/* affine 四次式最多 4 个根；零空间维数 k 对应 2^k 个根 ⇒ k ≤ 2。 */
#define BCH_AFFINE_MAX_ROOTS   BCH_T
#define BCH_AFFINE_MAX_NULLITY (2U)

typedef struct FC_BCH_CONTROL_S
{
    uint16_t exp[BCH_N];
    uint16_t log[BCH_N + 1U];
    uint64_t gen_lo; /* 生成多项式 g(x) 的 bit51..0；bit52 为首一系数，隐含 */
    /* 编码查表：4 个 byte 位置各 256 项；一次处理 32 bit。 */
    uint64_t enc_word[BCH_ENC_WORD_BYTES][256];
    BYTE     swap_bits;
    BOOL     ready;
} fc_bch_control_s;

static fc_bch_control_s s_bch;

_Static_assert(sizeof(fc_bch_control_s) <= (50U * 1024U), "BCH static storage exceeds 50 KiB");

/* ── GF(2^13) ─────────────────────────────────────────────────────── */

/**
 * @brief 对数域乘法 a·b。
 * @param bch 引擎句柄
 * @param a 操作数；0 单独处理
 * @param b 操作数；0 单独处理
 * @return a·b；任一操作数为 0 时返回 0
 */
static inline uint16_t gf_mul(const fc_bch_control_s *bch, uint16_t a, uint16_t b)
{
    uint32_t sum;

    if ((a == 0U) || (b == 0U))
    {
        return 0U;
    }
    sum = (uint32_t)bch->log[a] + (uint32_t)bch->log[b];
    if (sum >= BCH_N)
    {
        sum -= BCH_N;
    }
    return bch->exp[sum];
}

/**
 * @brief 有限域逆元 a^-1。
 * @param bch 引擎句柄
 * @param a 非零域元素
 * @return a^-1
 * @note 调用者必须保证 a != 0。
 */
static inline uint16_t gf_inv(const fc_bch_control_s *bch, uint16_t a)
{
    ASSERT(a != 0U);
    return (bch->log[a] == 0U) ? 1U : bch->exp[BCH_N - bch->log[a]];
}

/**
 * @brief 计算 α^exponent。
 * @param bch 引擎句柄
 * @param exponent 任意非负指数；入口取模到 [0, n)
 * @return 域元素 α^exponent
 */
static inline uint16_t gf_alpha(const fc_bch_control_s *bch, uint32_t exponent)
{
    return bch->exp[exponent % BCH_N];
}

/**
 * @brief 有限域平方 a^2。
 * @param bch 引擎句柄
 * @param a 域元素
 * @return a^2
 * @note 2*log < 2n，用一次条件减法代替 %n；Cortex-M 上整数取模要走软件除法。
 */
static inline uint16_t gf_sqr(const fc_bch_control_s *bch, uint16_t a)
{
    uint32_t exponent;

    if (a == 0U)
    {
        return 0U;
    }
    exponent = 2U * bch->log[a];
    if (exponent >= BCH_N)
    {
        exponent -= BCH_N;
    }
    return bch->exp[exponent];
}

/**
 * @brief 有限域平方根（GF(2^m) 中唯一）。
 * @param bch 引擎句柄
 * @param a 域元素
 * @return 满足 r^2 = a 的唯一 r
 * @note log 为奇数时先加 n 再除 2。
 */
static inline uint16_t gf_sqrt(const fc_bch_control_s *bch, uint16_t a)
{
    uint32_t exponent;

    if (a == 0U)
    {
        return 0U;
    }
    exponent = bch->log[a];
    if ((exponent & 1U) != 0U)
    {
        exponent += BCH_N;
    }
    return bch->exp[exponent >> 1U];
}

/**
 * @brief GF(2^m) 的迹 Tr(a) = a + a^2 + ... + a^(2^(m-1))。
 * @param bch 引擎句柄
 * @param a 域元素
 * @return 0 或 1
 */
static uint16_t gf_trace(const fc_bch_control_s *bch, uint16_t a)
{
    uint16_t sum = 0U;
    uint16_t v   = a;

    for (uint32_t i = 0U; i < BCH_M; i++)
    {
        sum ^= v;
        v = gf_sqr(bch, v);
    }
    return sum;
}

/**
 * @brief GF(2^m) 的半迹；m 为奇数时返回 y^2+y=a 的一个根。
 * @param bch 引擎句柄
 * @param a 域元素
 * @return y，满足 y^2+y=a
 */
static uint16_t gf_half_trace(const fc_bch_control_s *bch, uint16_t a)
{
    uint16_t sum = 0U;
    uint16_t v   = a;

    for (uint32_t i = 0U; i < ((BCH_M + 1U) / 2U); i++)
    {
        sum ^= v;
        v = gf_sqr(bch, gf_sqr(bch, v));
    }
    return sum;
}

/* ── 初始化：域表 + 生成多项式 ─────────────────────────────────────── */

/**
 * @brief 生成 α^0..α^(n-1) 与 log 表。
 * @param bch 引擎句柄
 * @param prim_poly 13 次本原多项式
 * @return true 成功；false 多项式次数/本原性不合格
 * @note x 必须在 n 步后回到 1，否则周期小于 n。
 */
static BOOL build_field_tables(fc_bch_control_s *bch, uint32_t prim_poly)
{
    uint32_t x = 1U;
    uint32_t i;

    if (((prim_poly & (1U << BCH_M)) == 0U) || ((prim_poly >> (BCH_M + 1U)) != 0U))
    {
        return false; /* 必须是 13 次多项式 */
    }

    memset(bch->exp, 0, sizeof(bch->exp));
    memset(bch->log, 0, sizeof(bch->log));

    for (i = 0U; i < BCH_N; i++)
    {
        if ((i != 0U) && (x == 1U))
        {
            return false; /* 非本原：周期小于 n */
        }
        bch->exp[i] = (uint16_t)x;
        bch->log[x] = (uint16_t)i;
        x <<= 1U;
        if ((x & (1U << BCH_M)) != 0U)
        {
            x ^= prim_poly;
        }
    }
    bch->log[0] = 0U;
    return (x == 1U);
}

/**
 * @brief 构造生成多项式 g(x) = ∏(x+α^r)。
 * @param bch 引擎句柄
 * @return true 成功；false 度不为 m*t 或系数不是 0/1
 * @note r 遍历 1/3/5/7 的 cyclotomic coset；g(x) 的 bit51..0 存入 gen_lo。
 */
static BOOL build_generator(fc_bch_control_s *bch)
{
    uint16_t g[BCH_ECC_BITS + 1U];
    uint16_t next[BCH_ECC_BITS + 1U];
    uint32_t deg = 0U;
    uint16_t base;
    uint32_t r;

    memset(g, 0, sizeof(g));
    g[0] = 1U;

    for (base = 1U; base <= (uint16_t)(2U * BCH_T); base += 2U)
    {
        r = base;
        for (uint32_t j = 0U; j < BCH_M; j++)
        {
            const uint16_t a = bch->exp[r];

            memset(next, 0, sizeof(next));
            for (uint32_t k = 0U; k <= deg; k++)
            {
                next[k] ^= gf_mul(bch, g[k], a);
                next[k + 1U] ^= g[k];
            }
            deg++;
            memcpy(g, next, (deg + 1U) * sizeof(g[0]));
            r = (2U * r) % BCH_N;
        }
    }

    if (deg != BCH_ECC_BITS)
    {
        return false;
    }

    {
        uint64_t bits = 0U;

        for (uint32_t i = 0U; i <= BCH_ECC_BITS; i++)
        {
            if (g[i] > 1U)
            {
                return false; /* GF(2) 上的生成多项式系数只能是 0/1 */
            }
            bits |= (uint64_t)g[i] << i;
        }
        if (g[0] != 1U)
        {
            return false;
        }
        bch->gen_lo = bits & BCH_MASK52;
    }
    return true;
}

/**
 * @brief 从任意余式状态处理一个 byte（MSB-first）。
 * @param bch 引擎句柄
 * @param rem 当前 52-bit 余式
 * @param value 输入 byte
 * @return 处理后的 52-bit 余式
 * @note 用于建表以及不足 4B 的尾部。
 */
static uint64_t encode_byte_raw(const fc_bch_control_s *bch, uint64_t rem, BYTE value)
{
    for (uint32_t k = 0U; k < 8U; k++)
    {
        const uint32_t bit  = ((uint32_t)value >> (7U - k)) & 1U;
        const uint32_t feed = (uint32_t)((rem >> (BCH_ECC_BITS - 1U)) & 1U) ^ bit;

        rem = (rem << 1U) & BCH_MASK52;
        if (feed != 0U)
        {
            rem ^= bch->gen_lo;
        }
    }
    return rem;
}

/**
 * @brief 建 4×256 编码查表。
 * @param bch 引擎句柄
 * @note 表由当前生成多项式现算，不依赖第三方代码。
 */
static void build_lookup_tables(fc_bch_control_s *bch)
{
    for (uint32_t k = 0U; k < BCH_ENC_WORD_BYTES; k++)
    {
        for (uint32_t b = 0U; b < 256U; b++)
        {
            const uint32_t word = b << (((BCH_ENC_WORD_BYTES - 1U) - k) * 8U);
            uint64_t       rem  = 0U;

            for (uint32_t j = 0U; j < BCH_ENC_WORD_BYTES; j++)
            {
                rem = encode_byte_raw(bch, rem, (BYTE)(word >> (((BCH_ENC_WORD_BYTES - 1U) - j) * 8U)));
            }
            bch->enc_word[k][b] = rem;
        }
    }
}

/**
 * @brief 初始化 BCH 引擎（本工程固定 m=13 / t=4）。
 * @param m 域阶；本实现只支持 13
 * @param t 纠错 bit 数；本实现只支持 4
 * @param prim_poly 本原多项式；0 使用 0x201B
 * @param swap_bits 非 0 时对数据与 ECC 字节做位序反转
 * @return 引擎句柄；参数不支持或建表失败返回 NULL
 * @note 表存于静态结构体（约 40 KiB），无堆分配；重复调用会重建。
 */
SSD_CODE_FAST fc_bch_control_s *fc_bch_init(int m, int t, unsigned int prim_poly, BYTE swap_bits)
{
    if ((m != (int)BCH_M) || (t != (int)BCH_T))
    {
        ASSERT_MSG(false, "unsupported BCH params m=%d t=%d", m, t);
        return NULL;
    }

    memset(&s_bch, 0, sizeof(s_bch));
    s_bch.swap_bits = swap_bits;

    if ((build_field_tables(&s_bch, (prim_poly == 0U) ? BCH_PRIM_POLY : prim_poly) == false) ||
        (build_generator(&s_bch) == false))
    {
        memset(&s_bch, 0, sizeof(s_bch));
        return NULL;
    }

    build_lookup_tables(&s_bch);
    s_bch.ready = true;
    return &s_bch;
}

/**
 * @brief 释放 BCH 引擎。
 * @param bch 引擎句柄（可为 NULL）
 * @note 存储为静态分配；本实现不释放资源，保留 API 语义。
 */
void fc_bch_free(fc_bch_control_s *bch)
{
    (void)bch;
}

/* ── 编码 ─────────────────────────────────────────────────────────── */

/**
 * @brief 反转 byte 内的 bit 顺序。
 * @param value 输入 byte
 * @return bit7..bit0 反转后的 byte
 */
static inline BYTE reverse_byte(BYTE value)
{
    value = (BYTE)((value & 0xF0U) >> 4U) | (BYTE)((value & 0x0FU) << 4U);
    value = (BYTE)((value & 0xCCU) >> 2U) | (BYTE)((value & 0x33U) << 2U);
    value = (BYTE)((value & 0xAAU) >> 1U) | (BYTE)((value & 0x55U) << 1U);
    return value;
}

/**
 * @brief 按 swap_bits 处理输入/输出 byte。
 * @param bch 引擎句柄
 * @param value 输入 byte
 * @return swap_bits 非 0 时返回位序反转结果，否则原值
 */
static inline BYTE input_byte(const fc_bch_control_s *bch, BYTE value)
{
    return (bch->swap_bits != 0U) ? reverse_byte(value) : value;
}

/**
 * @brief 从 ECC 缓冲区读取 52-bit 余式。
 * @param bch 引擎句柄
 * @param ecc 7B parity（外部字节表示，按 swap_bits 处理）
 * @return 低 52 位为余式
 */
static uint64_t load_remainder(const fc_bch_control_s *bch, const BYTE *ecc)
{
    uint64_t rem = 0U;

    for (uint32_t i = 0U; i < BCH_ECC_BITS; i++)
    {
        const uint32_t bit = ((uint32_t)input_byte(bch, ecc[i >> 3U]) >> (7U - (i & 7U))) & 1U;
        rem                = (rem << 1U) | bit;
    }
    return rem;
}

/**
 * @brief 把 52-bit 余式写入 7B ECC。
 * @param bch 引擎句柄
 * @param ecc 输出缓冲区；只写前 7B
 * @param rem 余式（只使用 bit51..0）
 * @note bit51..0 依次写 MSB-first；第 8 字节保持原值。
 */
static void store_remainder(const fc_bch_control_s *bch, BYTE *ecc, uint64_t rem)
{
    for (uint32_t i = 0U; i < BCH_ECC_BYTES; i++)
    {
        ecc[i] = 0U;
    }
    for (uint32_t i = 0U; i < BCH_ECC_BITS; i++)
    {
        const uint32_t bit = (uint32_t)((rem >> (BCH_ECC_BITS - 1U - i)) & 1U);
        ecc[i >> 3U] |= (BYTE)(bit << (7U - (i & 7U)));
    }
    for (uint32_t i = 0U; i < BCH_ECC_BYTES; i++)
    {
        ecc[i] = input_byte(bch, ecc[i]);
    }
}

/**
 * @brief 计算数据段的 BCH parity。
 * @param bch 引擎句柄
 * @param data 输入数据
 * @param len 数据字节数；须满足 8*len ≤ n - ecc_bits
 * @param ecc 7B parity 输出；非 NULL 时以其为初值做增量编码，NULL 时按全零初值计算并丢弃结果
 * @note 增量编码：首次调用前 ecc 必须清零；后续传入上一次输出即可继续。
 * @note parity 为 shortening 后的余式，MSB-first；第 8 字节保持调用者原值。
 * @note bch/data 为空或 len 超长违反前置条件，一律 ASSERT。
 */
SSD_CODE_FAST void fc_bch_encode(fc_bch_control_s *bch, const BYTE *data, unsigned int len, BYTE *ecc)
{
    uint64_t rem = 0U;

    ASSERT(bch != NULL);
    ASSERT(bch->ready == true);
    ASSERT(data != NULL);
    ASSERT(len <= BCH_MAX_DATA_LEN);

    if (ecc != NULL)
    {
        rem = load_remainder(bch, ecc);
    }

    uint32_t i = 0U;

    // 32 bit 批处理：4 个 byte 位置的查表线性组合；每 4 byte 处理一次。
    while ((len - i) >= BCH_ENC_WORD_BYTES)
    {
        const uint32_t word = ((uint32_t)input_byte(bch, data[i]) << 24U) |
                              ((uint32_t)input_byte(bch, data[i + 1U]) << 16U) |
                              ((uint32_t)input_byte(bch, data[i + 2U]) << 8U) |
                              (uint32_t)input_byte(bch, data[i + 3U]);
        const uint32_t x = (uint32_t)(rem >> 20U) ^ word;

        rem = ((rem << 32U) & BCH_MASK52) ^
              bch->enc_word[0][(x >> 24U) & 0xFFU] ^
              bch->enc_word[1][(x >> 16U) & 0xFFU] ^
              bch->enc_word[2][(x >> 8U) & 0xFFU] ^
              bch->enc_word[3][x & 0xFFU];
        i += BCH_ENC_WORD_BYTES;
    }

    // 尾部不足 4 byte：最多 3 byte，逐 byte 处理。
    for (; i < len; i++)
    {
        rem = encode_byte_raw(bch, rem, input_byte(bch, data[i]));
    }

    if (ecc != NULL)
    {
        store_remainder(bch, ecc, rem);
    }
}

/* ── 解码 ─────────────────────────────────────────────────────────── */

/**
 * @brief 由 52-bit parity 差值直接计算 syndrome。
 * @param bch 引擎句柄
 * @param diff 接收 parity XOR 预期 parity（外部字节表示）
 * @param syn 输出 2t 个 syndrome
 * @note BCH 是线性码，错误多项式的 syndrome 等于 parity 差值的 syndrome；
 *       复杂度只看 ecc_bits，与数据长度无关。
 */
static void compute_syndromes_diff(fc_bch_control_s *bch, const BYTE *diff, uint16_t syn[2U * BCH_T])
{
    uint16_t odd[BCH_T];

    for (uint32_t idx = 0U; idx < BCH_T; idx++)
    {
        const uint32_t j   = (2U * idx) + 1U;
        uint16_t       sum = 0U;

        for (uint32_t i = 0U; i < BCH_ECC_BITS; i++)
        {
            const uint32_t bit = ((uint32_t)input_byte(bch, diff[i >> 3U]) >>
                                  (7U - (i & 7U))) &
                                 1U;
            if (bit != 0U)
            {
                const uint32_t exponent = (j * (BCH_ECC_BITS - 1U - i)) % BCH_N;

                sum ^= gf_alpha(bch, exponent);
            }
        }
        odd[idx] = sum;
    }

    syn[0] = odd[0];
    syn[1] = gf_sqr(bch, syn[0]);
    syn[2] = odd[1];
    syn[3] = gf_sqr(bch, syn[1]);
    syn[4] = odd[2];
    syn[5] = gf_sqr(bch, syn[2]);
    syn[6] = odd[3];
    syn[7] = gf_sqr(bch, syn[3]);
}

/**
 * @brief Berlekamp–Massey：由 2t 个 syndrome 递推最小 locator σ(x)。
 * @param bch 引擎句柄
 * @param syn 2t 个 syndrome
 * @param sigma 输出 locator 系数（低次在前）
 * @return σ 的次数；次数超过 t 时返回 -1（不可纠）
 */
static int berlekamp_massey(fc_bch_control_s *bch, const uint16_t syn[2U * BCH_T], uint16_t sigma[BCH_T + 1U])
{
    uint16_t C[2U * BCH_T + 1U];
    uint16_t B[2U * BCH_T + 1U];
    uint16_t T[2U * BCH_T + 1U];
    int      L = 0;
    int      m = 1;
    uint16_t b = 1U;

    memset(C, 0, sizeof(C));
    memset(B, 0, sizeof(B));
    C[0] = 1U;
    B[0] = 1U;

    for (int n = 1; n <= (int)(2U * BCH_T); n++)
    {
        uint16_t d = syn[n - 1U];

        for (int i = 1; i <= L; i++)
        {
            d ^= gf_mul(bch, C[i], syn[n - i - 1U]);
        }

        if (d == 0U)
        {
            m++;
            continue;
        }

        memcpy(T, C, sizeof(T));
        {
            const uint16_t coef = gf_mul(bch, d, gf_inv(bch, b));

            for (int i = 0; i <= (int)(2U * BCH_T) - m; i++)
            {
                C[i + m] ^= gf_mul(bch, coef, B[i]);
            }
        }

        if ((2 * L) <= (n - 1))
        {
            L = n - L;
            memcpy(B, T, sizeof(B));
            b = d;
            m = 1;
        }
        else
        {
            m++;
        }
    }

    if (L > (int)BCH_T)
    {
        return -1;
    }
    memcpy(sigma, C, (BCH_T + 1U) * sizeof(sigma[0]));
    return L;
}

/**
 * @brief 把 locator 根 y=α^log_y 换成 API 错误 bit 位置。
 * @param bch 引擎句柄
 * @param root locator 根
 * @param nbits 缩短码总 bit 数
 * @param errloc 输出位置数组
 * @param count 输入/输出：已记录根数
 * @return true 根在缩短码范围内（或为重复根）；false 越界/重复计数已满
 */
static BOOL root_to_position(const fc_bch_control_s *bch, uint16_t root, uint32_t nbits, unsigned int *errloc, uint32_t *count)
{
    uint32_t exponent;
    uint32_t q;

    if (root == 0U)
    {
        return false;
    }
    exponent = (BCH_N - (uint32_t)bch->log[root]) % BCH_N;
    if (exponent >= nbits)
    {
        return false;
    }
    q = nbits - 1U - exponent;
    if (bch->swap_bits == 0U)
    {
        q = (q & ~7U) | (7U - (q & 7U));
    }
    for (uint32_t i = 0U; i < *count; i++)
    {
        if (errloc[i] == q)
        {
            return true; /* 重复根不重复计数 */
        }
    }
    if (*count >= BCH_T)
    {
        return false;
    }
    errloc[(*count)++] = q;
    return true;
}

/* ── locator 根搜索 ──────────────────────────────────────────────── */

/**
 * @brief 解 affine 四次方程 x^4 + a·x^2 + b·x + c = 0。
 * @param bch 引擎句柄
 * @param a x^2 系数
 * @param b x 系数
 * @param c 常数项
 * @param roots 输出根数组（容量 ≥4）
 * @return 根的个数（0/1/2/4）
 * @note x^4、x^2、x 都是 GF(2)-线性映射 ⇒ 13 个基向量列构成 GF(2) 线性方程组；
 *       用列基消元同时得到特解与零空间基，零空间维数 ≤2 时最多 4 个解。
 */
static int affine4_roots(const fc_bch_control_s *bch, uint16_t a, uint16_t b, uint16_t c, uint16_t roots[BCH_AFFINE_MAX_ROOTS])
{
    uint16_t basis[BCH_M];
    uint16_t combo[BCH_M];
    uint16_t null_vec[BCH_AFFINE_MAX_NULLITY];
    uint32_t null_cnt = 0U;
    uint16_t solution = 0U;
    int      count    = 0;

    for (uint32_t i = 0U; i < BCH_M; i++)
    {
        basis[i] = 0U;
        combo[i] = 0U;
    }

    // 逐列插入：basis[bit] 是首个最高位为 bit 的列，combo 记录它由哪些原列组成。
    // 插入失败的列给出一组和为 0 的原列组合，即零空间基。
    for (uint32_t i = 0U; i < BCH_M; i++)
    {
        const uint16_t e        = (uint16_t)(1U << i);
        const uint16_t e2       = gf_sqr(bch, e);
        uint16_t       v        = gf_sqr(bch, e2) ^ gf_mul(bch, a, e2) ^ gf_mul(bch, b, e);
        uint16_t       m        = (uint16_t)(1U << i);
        BOOL           inserted = false;

        for (int bit = (int)BCH_M - 1; bit >= 0; bit--)
        {
            if ((v & (uint16_t)(1U << bit)) != 0U)
            {
                if (basis[bit] == 0U)
                {
                    basis[bit] = v;
                    combo[bit] = m;
                    inserted   = true;
                    break;
                }
                v ^= basis[bit];
                m ^= combo[bit];
            }
        }
        if (!inserted)
        {
            if (null_cnt >= BCH_AFFINE_MAX_NULLITY)
            {
                return 0; /* 次数 4 的多项式不可能有 >4 个不同根 */
            }
            null_vec[null_cnt++] = m;
        }
    }

    // 用同一组基求特解。
    {
        uint16_t v = c;

        for (int bit = (int)BCH_M - 1; bit >= 0; bit--)
        {
            if ((v & (uint16_t)(1U << bit)) != 0U)
            {
                if (basis[bit] == 0U)
                {
                    return 0; /* 无解 */
                }
                v ^= basis[bit];
                solution ^= combo[bit];
            }
        }
        if (v != 0U)
        {
            return 0;
        }
    }

    for (uint32_t mask = 0U; mask < (1U << null_cnt); mask++)
    {
        uint16_t root = solution;

        for (uint32_t k = 0U; k < null_cnt; k++)
        {
            if ((mask & (1U << k)) != 0U)
            {
                root ^= null_vec[k];
            }
        }
        roots[count++] = root;
    }
    return count;
}

// 三次/四次 locator 的变换都是代数恒等式；affine 求解只利用
// x^4/x^2/x 在 GF(2^m) 上的 GF(2)-线性性，不依赖第三方实现。

/**
 * @brief 求三次 locator 的根。
 * @param bch 引擎句柄
 * @param sigma locator 系数（低次在前）
 * @param nbits 缩短码总 bit 数
 * @param errloc 输出错误位置
 * @param count 输入/输出：已记录根数
 * @return false 表示首项为 0；true 表示参数有效（根数可能小于 3）
 * @note σ(x)=x^3+a2x^2+b2x+c2 乘 (x+a2) 得 affine 四次式，解后去掉额外根 a2。
 */
static BOOL solve_cubic(const fc_bch_control_s *bch, const uint16_t *sigma, uint32_t nbits, unsigned int *errloc, uint32_t *count)
{
    uint16_t lead;
    uint16_t a2;
    uint16_t b2;
    uint16_t c2;
    uint16_t roots[BCH_AFFINE_MAX_ROOTS];

    if (sigma[3] == 0U)
    {
        return false;
    }
    lead = gf_inv(bch, sigma[3]);
    a2   = gf_mul(bch, sigma[2], lead);
    b2   = gf_mul(bch, sigma[1], lead);
    c2   = gf_mul(bch, sigma[0], lead);

    {
        // (x+a2)(x^3+a2x^2+b2x+c2) = x^4 + (a2^2+b2)x^2 + (a2b2+c2)x + a2c2
        const uint16_t a = gf_sqr(bch, a2) ^ b2;
        const uint16_t b = gf_mul(bch, a2, b2) ^ c2;
        const uint16_t c = gf_mul(bch, a2, c2);
        const int      n = affine4_roots(bch, a, b, c, roots);

        for (int i = 0; i < n; i++)
        {
            if (roots[i] != a2)
            {
                (void)root_to_position(bch, roots[i], nbits, errloc, count);
            }
        }
    }
    return true;
}

/**
 * @brief 求四次 locator 的根。
 * @param bch 引擎句柄
 * @param sigma locator 系数（低次在前）
 * @param nbits 缩短码总 bit 数
 * @param errloc 输出错误位置
 * @param count 输入/输出：已记录根数
 * @return false 表示首项为 0；true 表示参数有效（根数可能小于 4）
 * @note a=0 时已是 affine 形式；否则先平移消去一次项，再用 Y=1/z 变 affine。
 */
static BOOL solve_quartic(const fc_bch_control_s *bch, const uint16_t *sigma, uint32_t nbits, unsigned int *errloc, uint32_t *count)
{
    uint16_t lead;
    uint16_t a;
    uint16_t b;
    uint16_t c;
    uint16_t d;
    uint16_t roots[BCH_AFFINE_MAX_ROOTS];

    if (sigma[4] == 0U)
    {
        return false;
    }
    lead = gf_inv(bch, sigma[4]);
    a    = gf_mul(bch, sigma[3], lead);
    b    = gf_mul(bch, sigma[2], lead);
    c    = gf_mul(bch, sigma[1], lead);
    d    = gf_mul(bch, sigma[0], lead);

    if (a == 0U)
    {
        const int n = affine4_roots(bch, b, c, d, roots);

        for (int i = 0; i < n; i++)
        {
            (void)root_to_position(bch, roots[i], nbits, errloc, count);
        }
        return true;
    }

    {
        uint16_t e;

        if (c == 0U)
        {
            e = 0U;
        }
        else
        {
            const uint16_t ca = gf_mul(bch, c, gf_inv(bch, a));

            e = gf_sqrt(bch, ca);
        }
        {
            const uint16_t e2 = gf_sqr(bch, e);
            const uint16_t bp = gf_mul(bch, a, e) ^ b;
            const uint16_t dp = gf_sqr(bch, e2) ^ gf_mul(bch, b, e2) ^ d;

            if (dp == 0U)
            {
                // 平移后 z=0 是重根 ⇒ 原 locator 有重根，按不可纠处理。
                return true;
            }
            {
                const uint16_t inv_dp = gf_inv(bch, dp);
                const uint16_t aff_a  = gf_mul(bch, bp, inv_dp);
                const uint16_t aff_b  = gf_mul(bch, a, inv_dp);
                const uint16_t aff_c  = inv_dp;
                const int      n      = affine4_roots(bch, aff_a, aff_b, aff_c, roots);

                for (int i = 0; i < n; i++)
                {
                    if (roots[i] != 0U)
                    {
                        const uint16_t z = gf_inv(bch, roots[i]);

                        (void)root_to_position(bch, (uint16_t)(e ^ z), nbits, errloc, count);
                    }
                }
            }
        }
    }
    return true;
}

/**
 * @brief 求二次 locator 的闭式根。
 * @param bch 引擎句柄
 * @param sigma locator 系数（低次在前）
 * @param nbits 缩短码总 bit 数
 * @param errloc 输出错误位置
 * @param count 输入/输出：已记录根数
 * @return false 表示系数非法（首项/a 为 0）；true 表示参数有效（根数可能小于 2）
 * @note σ(x)=x^2+a·x+b；a=0 时只有重根，按不可纠处理。
 */
static BOOL solve_quadratic(const fc_bch_control_s *bch, const uint16_t *sigma, uint32_t nbits, unsigned int *errloc, uint32_t *count)
{
    uint16_t lead;
    uint16_t a;
    uint16_t b;
    uint16_t c;
    uint16_t y;
    uint16_t r0;
    uint16_t r1;

    if ((sigma[2] == 0U) || (sigma[1] == 0U))
    {
        return false;
    }
    lead = gf_inv(bch, sigma[2]);
    a    = gf_mul(bch, sigma[1], lead);
    b    = gf_mul(bch, sigma[0], lead);
    c    = gf_mul(bch, b, gf_inv(bch, gf_sqr(bch, a)));

    if (gf_trace(bch, c) != 0U)
    {
        return true; /* 无根（不是参数错误） */
    }
    y  = gf_half_trace(bch, c);
    r0 = gf_mul(bch, a, y);
    r1 = gf_mul(bch, a, (uint16_t)(y ^ 1U));
    (void)root_to_position(bch, r0, nbits, errloc, count);
    (void)root_to_position(bch, r1, nbits, errloc, count);
    return true;
}

/**
 * @brief 求 locator 的根。
 * @param bch 引擎句柄
 * @param sigma locator 系数（低次在前）
 * @param degree locator 次数（1..t）
 * @param nbits 缩短码总 bit 数
 * @param errloc 输出错误位置
 * @return 找到的根数；-1 表示参数非法
 * @note degree 1/2 用闭式；3/4 用次数专用的 affine 线性解。
 */
static int find_roots(const fc_bch_control_s *bch, const uint16_t sigma[BCH_T + 1U], int degree, uint32_t nbits, unsigned int *errloc)
{
    uint32_t count = 0U;

    if ((degree <= 0) || (degree > (int)BCH_T))
    {
        return -1;
    }

    if (degree == 1)
    {
        // σ(x)=x+a ⇒ x=a（BM 的 locator 已归一化；仍按 lead 归一保险）。
        if (sigma[1] == 0U)
        {
            return -1;
        }
        {
            const uint16_t a = gf_mul(bch, sigma[0], gf_inv(bch, sigma[1]));

            (void)root_to_position(bch, a, nbits, errloc, &count);
        }
        return (int)count;
    }

    if (degree == 2)
    {
        if (!solve_quadratic(bch, sigma, nbits, errloc, &count))
        {
            return -1;
        }
        return (int)count;
    }

    // degree 3/4：次数专用变换 → affine 四次式 → GF(2) 线性方程组。
    if (sigma[degree] == 0U)
    {
        return -1;
    }
    {
        BOOL valid;

        if (degree == 3)
        {
            valid = solve_cubic(bch, sigma, nbits, errloc, &count);
        }
        else
        {
            valid = solve_quartic(bch, sigma, nbits, errloc, &count);
        }
        if (!valid)
        {
            return -1;
        }
    }
    return (int)count;
}

/**
 * @brief 检测并定位 data + parity 中的错误。
 * @param bch 引擎句柄
 * @param data 数据区
 * @param len 数据字节数；须满足 8*len ≤ n - ecc_bits
 * @param recv_ecc 接收 parity；现场重编码 data 并与其比对
 * @param errloc 输出错误 bit 位置（API 约定：bit0 = LSB）
 * @param out_nerr 输出错误数（0..t）
 * @return true 可纠（0..t）；false 不可纠或根数与 locator 次数不符
 * @note errloc[i] < 8*len 为数据区，>= 8*len 为 parity 区；调用方负责翻转。
 * @note swap_bits==0 时位置按字节内 bit 反转；swap_bits!=0 直接返回 q。
 * @note 入参前置违反（len 超长 / data、recv_ecc、errloc、out_nerr 为空）一律 ASSERT。
 */
SSD_CODE_FAST BOOL fc_bch_decode(fc_bch_control_s *bch, const BYTE *data, unsigned int len, const BYTE *recv_ecc, unsigned int *errloc, unsigned int *out_nerr)
{
    uint16_t s[2U * BCH_T];
    uint16_t sigma[BCH_T + 1U];
    int      degree;

    ASSERT(bch != NULL);
    ASSERT(bch->ready == true);
    ASSERT(data != NULL);
    ASSERT(recv_ecc != NULL);
    ASSERT(errloc != NULL);
    ASSERT(out_nerr != NULL);
    ASSERT(len <= BCH_MAX_DATA_LEN);

    const uint32_t nbits = len * 8U + BCH_ECC_BITS;

    {
        BYTE diff[BCH_ECC_BYTES];
        BYTE expected[BCH_ECC_BYTES] = {0};

        fc_bch_encode(bch, data, len, expected);
        for (uint32_t i = 0U; i < BCH_ECC_BYTES; i++)
        {
            diff[i] = (BYTE)(expected[i] ^ recv_ecc[i]);
        }
        compute_syndromes_diff(bch, diff, s);
    }

    {
        uint16_t sum = 0U;

        for (uint32_t i = 0U; i < (2U * BCH_T); i++)
        {
            sum |= s[i];
        }
        if (sum == 0U)
        {
            *out_nerr = 0U;
            return true;
        }
    }

    degree = berlekamp_massey(bch, s, sigma);
    if (degree < 0)
    {
        *out_nerr = 0U;
        return false;
    }
    if (degree == 0)
    {
        *out_nerr = 0U;
        return false;
    }

    // 根搜索：e 为多项式指数；q=nbits-1-e 再按 API 位序换算。
    {
        const int found = find_roots(bch, sigma, degree, nbits, errloc);

        if ((found < 0) || (found != degree))
        {
            *out_nerr = 0U;
            return false;
        }
        *out_nerr = (unsigned int)found;
    }
    return true;
}
