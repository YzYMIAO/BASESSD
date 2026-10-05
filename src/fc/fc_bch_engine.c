#include "fc_bch_engine.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * fc_bch_engine.c — BCH ECC 引擎（纠错码）
 *
 * 移植自 Linux 内核 lib/bch.c（GPL v2），作者 Ivan Djelic。
 * 将内核依赖替换为标准 C（malloc/free、__builtin_clz/bswap）。
 *
 * ── 算法（BCH 码 = 有限域上的循环码）──
 *   编码：4 查找表并行 32bit 输入（生成多项式 g(x) 的 mod 查表）
 *   解码：综合式 S(x) → Berlekamp–Massey（错误位置多项式 σ(x)）
 *         → 求根（BTZ factoring：degree 1..4 解析 + 高次因数分解）
 *         → 按错误位置翻转 bit 纠正。
 *
 * ── 关键公式出处 ──
 *   · 域 GF(2^m)：本原多项式 prim_poly，元素 a_pow/a_log 查表（对数域乘法）
 *   · 生成多项式 g(x) = lcm{ m1(x), m3(x), ..., m(2t-1)(x) }，
 *     根 = 连续 2t 个 α^i 的极小多项式之积（BCH 码构造定理）
 *   · 综合式 S_j = r(α^j)，j=1..2t（接收多项式在根处的取值 = 错误分量）
 *   · Berlekamp–Massey：由 S 递推最小 σ(x)，σ(α^-i)=0 的位置即错误位置
 *   · Chien 搜索等价物：本实现用 BTZ 因子分解（度≤4 解析求根 + 递归分解）
 *
 * ── 本工程参数 ──
 *   m=13, t=4：ecc_bits = 13×4 = 52bit → ecc_bytes=7，8B 槽够用
 *   码长 n = 2^13 - 1 = 8191；数据位 ≤ n - 52（520B×8 = 4160 < 8139 ✓）
 */

/* ── 辅助宏 ─────────────────────────────────────────────────────── */

#define DIV_ROUND_UP(n, d) (((n) + (d)-1U) / (d))

#ifndef MIN
#    define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* fls — find last set bit（最高位位置+1；x=0 → 0） */
static inline int fls(unsigned int x)
{
    if (x == 0U)
    {
        return 0;
    }
    return (int)(sizeof(unsigned int) * 8U - __builtin_clz(x));
}

/* 8-bit 位序反转（swap_bits 模式：NAND 数据位序与算法假设相反时用） */
static inline uint8_t bitrev8(uint8_t x)
{
    x = (uint8_t)((x & 0xF0U) >> 4U) | (uint8_t)((x & 0x0FU) << 4U);
    x = (uint8_t)((x & 0xCCU) >> 2U) | (uint8_t)((x & 0x33U) << 2U);
    x = (uint8_t)((x & 0xAAU) >> 1U) | (uint8_t)((x & 0x55U) << 1U);
    return x;
}

/* 大端序转换（数据按 BE 语义输入 BCH 引擎） */
static inline uint32_t cpu_to_be32(uint32_t x)
{
    return __builtin_bswap32(x);
}

/* ── 参数与数据结构 ──────────────────────────────────────────────── */

#define BCH_MAX_M 15 /* 域阶上限（GF(2^15)） */
#define BCH_MAX_T 64 /* 纠错能力上限 */

/*
 * BCH 引擎上下文：
 *   a_pow_tab[a_log_tab]  —— GF(2^m) 对数/反对数表（乘法→加法）
 *   mod8_tab               —— 编码查找表（每字节偏移，4 组 256 项）
 *   ecc_buf/ecc_buf2       —— 校验字工作区
 *   xi_tab                 —— 度 2 多项式求根用的迹映射表
 *   syn                    —— 综合式数组（2t 项）
 *   cache                  —— 多项式 mod 用对数表示缓存
 *   elp / poly_2t[]        —— 错误位置多项式 + 求根工作多项式
 *   swap_bits              —— 输入/输出位序是否交换
 */
struct fc_bch_control
{
    unsigned int    m;
    unsigned int    n;
    unsigned int    t;
    unsigned int    ecc_bits;
    unsigned int    ecc_bytes;
    uint16_t       *a_pow_tab;
    uint16_t       *a_log_tab;
    uint32_t       *mod8_tab;
    uint32_t       *ecc_buf;
    uint32_t       *ecc_buf2;
    unsigned int   *xi_tab;
    unsigned int   *syn;
    int            *cache;
    struct gf_poly *elp;
    struct gf_poly *poly_2t[4];
    uint8_t         swap_bits;
};

#define GF_M(_p)          ((_p)->m)
#define GF_T(_p)          ((_p)->t)
#define GF_N(_p)          ((_p)->n)
#define BCH_ECC_WORDS(_p) DIV_ROUND_UP(GF_M(_p) * GF_T(_p), 32)
#define BCH_ECC_BYTES(_p) ((_p)->ecc_bytes)
#define BCH_ECC_MAX_WORDS DIV_ROUND_UP(BCH_MAX_M *BCH_MAX_T, 32)

/* GF(2^m) 上的多项式：deg = 次数，c[] = 系数（低位优先） */
struct gf_poly
{
    unsigned int deg;
    unsigned int c[];
};

#define GF_POLY_SZ(_d) (sizeof(struct gf_poly) + ((_d) + 1U) * sizeof(unsigned int))

struct gf_poly_deg1
{
    struct gf_poly poly;
    unsigned int   c[2];
};

/* ── 前向声明 ────────────────────────────────────────────────────── */

static uint8_t      swap_bits(struct fc_bch_control *bch, uint8_t in);
static void         load_ecc8(struct fc_bch_control *bch, uint32_t *dst, const uint8_t *src);
static void         store_ecc8(struct fc_bch_control *bch, uint8_t *dst, const uint32_t *src);
static int          modulo(struct fc_bch_control *bch, unsigned int v);
static int          mod_s(struct fc_bch_control *bch, unsigned int v);
static int          deg(unsigned int poly);
static unsigned int gf_div(struct fc_bch_control *bch, unsigned int a, unsigned int b);
static unsigned int gf_mul(struct fc_bch_control *bch, unsigned int a, unsigned int b);
static unsigned int gf_sqr(struct fc_bch_control *bch, unsigned int a);
static unsigned int gf_inv(struct fc_bch_control *bch, unsigned int a);
static unsigned int a_pow(struct fc_bch_control *bch, int i);
static int          a_log(struct fc_bch_control *bch, unsigned int x);
static int          a_ilog(struct fc_bch_control *bch, unsigned int x);
static void         compute_syndromes(struct fc_bch_control *bch, uint32_t *ecc, unsigned int *syn);
static int          compute_error_locator_polynomial(struct fc_bch_control *bch, const unsigned int *syn);
static int          find_poly_roots(struct fc_bch_control *bch, unsigned int k, struct gf_poly *poly, unsigned int *roots);
static int          build_gf_tables(struct fc_bch_control *bch, unsigned int poly);
static void         build_mod8_tables(struct fc_bch_control *bch, const uint32_t *g);
static int          build_deg2_base(struct fc_bch_control *bch);
static uint32_t    *compute_generator_polynomial(struct fc_bch_control *bch);

/* ═══════════════════════════════════════════════════════════════════
 * 辅助函数
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t swap_bits(struct fc_bch_control *bch, uint8_t in)
{
    return bch->swap_bits ? bitrev8(in) : in;
}

/* ═══════════════════════════════════════════════════════════════════
 * 编码
 * ═══════════════════════════════════════════════════════════════════ */

/* 非对齐数据段的逐字节编码（mod8 查表推进校验寄存器） */
static void bch_encode_unaligned(struct fc_bch_control *bch,
                                 const unsigned char   *data,
                                 unsigned int           len,
                                 uint32_t              *ecc)
{
    int             i;
    const uint32_t *p;
    const int       l = (int)BCH_ECC_WORDS(bch) - 1;

    while (len--)
    {
        uint8_t tmp = swap_bits(bch, *data++);
        /* 最高字节 ⊕ 输入字节 → 查表，表项与其余校验字异或移位 */
        p           = bch->mod8_tab + (unsigned int)(l + 1) * (((ecc[0] >> 24) ^ (uint32_t)tmp) & 0xFFU);
        for (i = 0; i < l; i++)
        {
            ecc[i] = ((ecc[i] << 8) | (ecc[i + 1] >> 24)) ^ (*p++);
        }
        ecc[l] = (ecc[l] << 8) ^ (*p);
    }
}

/* 把 ECC 字节（可选位序交换）载入工作区（32bit 字） */
static void load_ecc8(struct fc_bch_control *bch, uint32_t *dst, const uint8_t *src)
{
    uint8_t      pad[4] = {0, 0, 0, 0};
    unsigned int i, nwords = BCH_ECC_WORDS(bch) - 1;

    for (i = 0; i < nwords; i++, src += 4)
    {
        dst[i] = ((uint32_t)swap_bits(bch, src[0]) << 24) |
                 ((uint32_t)swap_bits(bch, src[1]) << 16) |
                 ((uint32_t)swap_bits(bch, src[2]) << 8) |
                 (uint32_t)swap_bits(bch, src[3]);
    }
    memcpy(pad, src, BCH_ECC_BYTES(bch) - 4 * nwords);
    dst[nwords] = ((uint32_t)swap_bits(bch, pad[0]) << 24) |
                  ((uint32_t)swap_bits(bch, pad[1]) << 16) |
                  ((uint32_t)swap_bits(bch, pad[2]) << 8) |
                  (uint32_t)swap_bits(bch, pad[3]);
}

/* 把工作区写回 ECC 字节（与 load 对称） */
static void store_ecc8(struct fc_bch_control *bch, uint8_t *dst, const uint32_t *src)
{
    uint8_t      pad[4];
    unsigned int i, nwords = BCH_ECC_WORDS(bch) - 1;

    for (i = 0; i < nwords; i++)
    {
        *dst++ = swap_bits(bch, (uint8_t)(src[i] >> 24));
        *dst++ = swap_bits(bch, (uint8_t)(src[i] >> 16));
        *dst++ = swap_bits(bch, (uint8_t)(src[i] >> 8));
        *dst++ = swap_bits(bch, (uint8_t)(src[i]));
    }
    pad[0] = swap_bits(bch, (uint8_t)(src[nwords] >> 24));
    pad[1] = swap_bits(bch, (uint8_t)(src[nwords] >> 16));
    pad[2] = swap_bits(bch, (uint8_t)(src[nwords] >> 8));
    pad[3] = swap_bits(bch, (uint8_t)(src[nwords]));
    memcpy(dst, pad, BCH_ECC_BYTES(bch) - 4 * nwords);
}

/* --------------------------------------------------------------------
 * fc_bch_encode
 *   功能：计算 data 的 BCH 校验码（增量支持）。
 *   参数：bch —— 引擎；data —— 输入数据；len —— 字节数；ecc —— 输出校验。
 *   返回：无。
 *   实现要点：4 查找表并行（每输入 32bit 分 4 字节查表异或），
 *         首尾非对齐段用 bch_encode_unaligned 兜底。
 * -------------------------------------------------------------------- */
void fc_bch_encode(struct fc_bch_control *bch, const uint8_t *data, unsigned int len, uint8_t *ecc)
{
    const unsigned int    l = BCH_ECC_WORDS(bch) - 1;
    unsigned int          i, mlen;
    unsigned long         m;
    uint32_t              w, r[BCH_ECC_MAX_WORDS];
    const size_t          r_bytes = BCH_ECC_WORDS(bch) * sizeof(*r);
    const uint32_t *const tab0    = bch->mod8_tab;
    const uint32_t *const tab1    = tab0 + 256 * (l + 1);
    const uint32_t *const tab2    = tab1 + 256 * (l + 1);
    const uint32_t *const tab3    = tab2 + 256 * (l + 1);
    const uint32_t       *pdata, *p0, *p1, *p2, *p3;

    if (ecc)
    {
        load_ecc8(bch, bch->ecc_buf, ecc);
    }
    else
    {
        memset(bch->ecc_buf, 0, r_bytes);
    }

    m = (unsigned long)data & 3UL;
    if (m)
    {
        mlen = (len < (unsigned int)(4U - m)) ? len : (unsigned int)(4U - m);
        bch_encode_unaligned(bch, data, mlen, bch->ecc_buf);
        data += mlen;
        len -= mlen;
    }

    pdata = (const uint32_t *)(const void *)data;
    mlen  = len / 4U;
    data += 4U * mlen;
    len -= 4U * mlen;
    memcpy(r, bch->ecc_buf, r_bytes);

    while (mlen--)
    {
        w = cpu_to_be32(*pdata++);
        if (bch->swap_bits)
        {
            w = (uint32_t)swap_bits(bch, (uint8_t)(w >> 0)) |
                ((uint32_t)swap_bits(bch, (uint8_t)(w >> 8)) << 8) |
                ((uint32_t)swap_bits(bch, (uint8_t)(w >> 16)) << 16) |
                ((uint32_t)swap_bits(bch, (uint8_t)(w >> 24)) << 24);
        }
        w ^= r[0];
        p0 = tab0 + (l + 1) * ((w >> 0) & 0xFFU);
        p1 = tab1 + (l + 1) * ((w >> 8) & 0xFFU);
        p2 = tab2 + (l + 1) * ((w >> 16) & 0xFFU);
        p3 = tab3 + (l + 1) * ((w >> 24) & 0xFFU);
        for (i = 0; i < l; i++)
        {
            r[i] = r[i + 1] ^ p0[i] ^ p1[i] ^ p2[i] ^ p3[i];
        }
        r[l] = p0[l] ^ p1[l] ^ p2[l] ^ p3[l];
    }
    memcpy(bch->ecc_buf, r, r_bytes);

    if (len)
    {
        bch_encode_unaligned(bch, data, len, bch->ecc_buf);
    }
    if (ecc)
    {
        store_ecc8(bch, ecc, bch->ecc_buf);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * GF(2^m) 算术
 *
 * GF(2^m) 元素用本原元 α 的幂表示：a_pow[i] = α^i、a_log[x] = i。
 * 乘法 → 对数相加后查幂表；除法 → 对数相减；平方/逆同理。
 * 模 n（n=2^m-1）归约：α^n = 1。
 * ═══════════════════════════════════════════════════════════════════ */

/* 对 v 做 mod n 归约（利用 2^m ≡ 1 的循环性质） */
static inline int modulo(struct fc_bch_control *bch, unsigned int v)
{
    const unsigned int n = GF_N(bch);
    while (v >= n)
    {
        v -= n;
        v = (v & n) + (v >> GF_M(bch));
    }
    return (int)v;
}

/* 快速 mod：v < n 原样返回，否则减一次 n（v 保证 < 2n） */
static inline int mod_s(struct fc_bch_control *bch, unsigned int v)
{
    const unsigned int n = GF_N(bch);
    return (v < n) ? (int)v : (int)(v - n);
}

/* 多项式最高次数（x=0 → -1） */
static inline int deg(unsigned int poly)
{
    if (poly == 0)
    {
        return -1;
    }
    return (int)(sizeof(unsigned int) * 8U - __builtin_clz(poly)) - 1;
}

/* GF 除法 a/b = a_log[a] - a_log[b] 再查幂表 */
static inline unsigned int gf_div(struct fc_bch_control *bch, unsigned int a, unsigned int b)
{
    if ((a == 0U) || (b == 0U))
    {
        return 0U;
    }
    return bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)((int)bch->a_log_tab[a] + (int)((int)GF_N(bch) - (int)bch->a_log_tab[b])))];
}

/* GF 乘法 a·b = α^(log a + log b) */
static inline unsigned int gf_mul(struct fc_bch_control *bch, unsigned int a, unsigned int b)
{
    return (a && b) ? bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)((int)bch->a_log_tab[a] + (int)bch->a_log_tab[b]))] : 0;
}

/* GF 平方：a² = α^(2·log a) */
static inline unsigned int gf_sqr(struct fc_bch_control *bch, unsigned int a)
{
    return a ? bch->a_pow_tab[(unsigned int)mod_s(bch, 2 * (int)bch->a_log_tab[a])] : 0;
}

/* GF 逆：a^-1 = α^(n - log a)（n = 2^m-1 时 α^(n-1) = α^-1） */
static inline unsigned int gf_inv(struct fc_bch_control *bch, unsigned int a)
{
    return bch->a_pow_tab[GF_N(bch) - (unsigned int)bch->a_log_tab[a]];
}

/* α^i（i 可为负，经 modulo 归约） */
static inline unsigned int a_pow(struct fc_bch_control *bch, int i)
{
    return bch->a_pow_tab[(unsigned int)modulo(bch, (unsigned int)i)];
}

/* 离散对数：a_log[x] = i 使 α^i = x */
static inline int a_log(struct fc_bch_control *bch, unsigned int x)
{
    return (int)bch->a_log_tab[x];
}

/* 逆对数：a_ilog[x] = 指数 i 使 α^i = x（等价 gf_inv 的对数形式） */
static inline int a_ilog(struct fc_bch_control *bch, unsigned int x)
{
    return mod_s(bch, GF_N(bch) - (unsigned int)bch->a_log_tab[x]);
}

/* ═══════════════════════════════════════════════════════════════════
 * 综合式计算
 *
 * S_j = r(α^j) = Σ_i e_i·(α^j)^pos_i，j=1..2t。
 * 逐 bit 扫描接收码字，遇置位 bit 累加 a_pow((j+1)·bitpos)。
 * 校验字部分（ecc 字）参与其中；最终 S_(2j+1) = S_j²（GF 平方，域特性）。
 * ═══════════════════════════════════════════════════════════════════ */

static void compute_syndromes(struct fc_bch_control *bch, uint32_t *ecc, unsigned int *syn)
{
    int          i, j, s;
    unsigned int m;
    uint32_t     poly;
    const int    t = (int)GF_T(bch);

    s = (int)bch->ecc_bits;
    m = (unsigned int)s & 31U;
    if (m)
    {
        ecc[(unsigned int)s / 32U] &= ~((1u << (32U - m)) - 1U);
    }
    memset(syn, 0, (unsigned int)(2 * t) * sizeof(*syn));
    do
    {
        poly = *ecc++;
        s -= 32;
        while (poly)
        {
            i = (int)deg(poly);
            for (j = 0; j < 2 * t; j += 2)
            {
                syn[j] ^= a_pow(bch, (j + 1) * (i + s));
            }
            poly ^= (1u << (unsigned int)i);
        }
    } while (s > 0);
    for (j = 0; j < t; j++)
    {
        syn[2 * j + 1] = gf_sqr(bch, syn[j]);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Berlekamp–Massey
 *
 * 由综合式 S 递推最小错误位置多项式 σ(x)（σ(α^-i)=0 的 i 即错误位置）。
 * 迭代：当前偏差 d → 修正多项式 → 必要时更新上次成功位置 pp/pd。
 * ═══════════════════════════════════════════════════════════════════ */

static void gf_poly_copy(struct gf_poly *dst, struct gf_poly *src)
{
    memcpy(dst, src, GF_POLY_SZ(src->deg));
}

/* BM 算法：返回 σ 的次数（= 错误数），>t 表示不可纠返回 -1 */
static int compute_error_locator_polynomial(struct fc_bch_control *bch, const unsigned int *syn)
{
    const unsigned int t = GF_T(bch), n = GF_N(bch);
    unsigned int       i, j, tmp, l, pd = 1, d = syn[0];
    struct gf_poly    *elp = bch->elp, *pelp = bch->poly_2t[0], *elp_copy = bch->poly_2t[1];
    int                k, pp = -1;

    memset(pelp, 0, GF_POLY_SZ(2 * t));
    memset(elp, 0, GF_POLY_SZ(2 * t));
    pelp->deg  = 0;
    pelp->c[0] = 1;
    elp->deg   = 0;
    elp->c[0]  = 1;

    for (i = 0; (i < t) && ((int)elp->deg <= (int)t); i++)
    {
        if (d)
        {
            k = (int)(2U * i) - pp;
            gf_poly_copy(elp_copy, elp);
            tmp = (unsigned int)((int)a_log(bch, d) + (int)n - a_log(bch, pd));
            for (j = 0; j <= pelp->deg; j++)
            {
                if (pelp->c[j])
                {
                    l = a_log(bch, pelp->c[j]);
                    elp->c[(unsigned int)(j + (unsigned int)k)] ^= a_pow(bch, (int)(tmp + (int)l));
                }
            }
            tmp = (unsigned int)((int)pelp->deg + k);
            if ((int)tmp > (int)elp->deg)
            {
                elp->deg = (int)tmp;
                gf_poly_copy(pelp, elp_copy);
                pd = d;
                pp = (int)(2U * i);
            }
        }
        if (i < t - 1U)
        {
            d = syn[2U * i + 2U];
            for (j = 1; j <= (unsigned int)elp->deg; j++)
            {
                d ^= gf_mul(bch, elp->c[j], syn[2U * i + 2U - j]);
            }
        }
    }
    return ((int)elp->deg > (int)t) ? -1 : (int)elp->deg;
}

/* ═══════════════════════════════════════════════════════════════════
 * 求根（BTZ factoring）
 *
 * 目标：找 σ(x) 在 GF(2^m) 中所有根（错误位置）。
 *   - 度 1..4：解析求根（线性 / 二次 / 三次 / 四次，见各 find_poly_deg*_roots）。
 *   - 高次：递归 BTZ 因子分解——用迹映射 T_k 与 σ 的 GCD 拆分因子。
 * 关键辅助：
 *   solve_linear_system —— 解 GF(2) 上的 m×m 线性方程组（求二次根）。
 *   compute_trace_bk_mod —— 计算迹多项式 T_k(x) = Σ_i x^(2^i) mod f。
 * ═══════════════════════════════════════════════════════════════════ */

/* 解 GF(2) 线性方程组（高斯消元 + 回代）；nsol = 2^k2 个解枚举 */
static int solve_linear_system(struct fc_bch_control *bch, unsigned int *rows, unsigned int *sol, int nsol)
{
    const int    m = (int)GF_M(bch);
    unsigned int tmp, mask;
    int          rem, c, r, p, k2, param[BCH_MAX_M];

    k2   = 0;
    mask = 1U << (unsigned int)m;
    for (c = 0; c < m; c++)
    {
        rem = 0;
        p   = c - k2;
        for (r = p; r < m; r++)
        {
            if (rows[r] & mask)
            {
                if (r != p)
                {
                    tmp     = rows[r];
                    rows[r] = rows[p];
                    rows[p] = tmp;
                }
                rem = r + 1;
                break;
            }
        }
        if (rem)
        {
            tmp = rows[p];
            for (r = rem; r < m; r++)
            {
                if (rows[r] & mask)
                {
                    rows[r] ^= tmp;
                }
            }
        }
        else
        {
            param[k2++] = c;
        }
        mask >>= 1;
    }
    if (k2 > 0)
    {
        p = k2;
        for (r = m - 1; r >= 0; r--)
        {
            if ((r > m - 1 - k2) && rows[r])
            {
                return 0;
            }
            rows[r] = (p && (r == param[p - 1])) ? (unsigned int)(p--, 1U << (unsigned int)(m - r)) : (r >= p) ? rows[r - p]
                                                                                                               : 0;
        }
    }
    if (nsol != (1 << k2))
    {
        return 0;
    }
    for (p = 0; p < nsol; p++)
    {
        for (c = 0; c < k2; c++)
        {
            rows[param[c]] = (rows[param[c]] & ~1U) | ((unsigned int)((p >> c) & 1));
        }
        tmp = 0;
        for (r = m - 1; r >= 0; r--)
        {
            unsigned int x = rows[r] & (tmp | 1U);
            x ^= x >> 1;
            x ^= x >> 2;
            x   = (x & 0x11111111U) * 0x11111111U;
            tmp = (tmp << 1) | ((x >> 28) & 1U);
        }
        sol[p] = tmp >> 1;
    }
    return nsol;
}

static int find_affine4_roots(struct fc_bch_control *bch, unsigned int a, unsigned int b, unsigned int c, unsigned int *roots)
{
    int          i, j, k3;
    const int    m    = (int)GF_M(bch);
    unsigned int mask = 0xFFU, t, rows[16] = {0};

    j       = a_log(bch, b);
    k3      = a_log(bch, a);
    rows[0] = c;
    for (i = 0; i < m; i++)
    {
        rows[i + 1] = bch->a_pow_tab[(unsigned int)(4 * i)] ^
                      (a ? bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)k3)] : 0) ^
                      (b ? bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)j)] : 0);
        j++;
        k3 += 2;
    }
    for (j = 8; j != 0; j >>= 1, mask ^= (mask << j))
    {
        for (k3 = 0; k3 < 16; k3 = (k3 + j + 1) & ~j)
        {
            t = ((rows[k3] >> j) ^ rows[k3 + j]) & mask;
            rows[k3] ^= (t << j);
            rows[k3 + j] ^= t;
        }
    }
    return solve_linear_system(bch, rows, roots, 4);
}

static int find_poly_deg1_roots(struct fc_bch_control *bch, struct gf_poly *poly, unsigned int *roots)
{
    if (!poly->c[0])
    {
        return 0;
    }
    roots[0] = (unsigned int)mod_s(bch, GF_N(bch) - (unsigned int)a_log(bch, poly->c[0]) + (unsigned int)a_log(bch, poly->c[1]));
    return 1;
}

static int find_poly_deg2_roots(struct fc_bch_control *bch, struct gf_poly *poly, unsigned int *roots)
{
    int          n = 0, i, l0 = (int)bch->a_log_tab[poly->c[0]], l1 = (int)bch->a_log_tab[poly->c[1]], l2 = (int)bch->a_log_tab[poly->c[2]];
    unsigned int u, v, r;
    if (!poly->c[0] || !poly->c[1])
    {
        return 0;
    }
    u = a_pow(bch, l0 + l2 + 2 * (int)(GF_N(bch) - (unsigned int)l1));
    r = 0;
    v = u;
    while (v)
    {
        i = (int)deg(v);
        r ^= bch->xi_tab[i];
        v ^= (1U << (unsigned int)i);
    }
    if ((gf_sqr(bch, r) ^ r) == u)
    {
        roots[n++] = (unsigned int)modulo(bch, (int)(2U * GF_N(bch)) - l1 - (int)a_log(bch, r) + l2);
        roots[n++] = (unsigned int)modulo(bch, (int)(2U * GF_N(bch)) - l1 - (int)a_log(bch, r ^ 1) + l2);
    }
    return n;
}

static int find_poly_deg3_roots(struct fc_bch_control *bch, struct gf_poly *poly, unsigned int *roots)
{
    int          i, n = 0;
    unsigned int a2, b2, c2, e3, tmp[4];
    if (!poly->c[0])
    {
        return 0;
    }
    e3 = poly->c[3];
    c2 = gf_div(bch, poly->c[0], e3);
    b2 = gf_div(bch, poly->c[1], e3);
    a2 = gf_div(bch, poly->c[2], e3);
    if (find_affine4_roots(bch, gf_sqr(bch, a2) ^ b2, gf_mul(bch, a2, b2) ^ c2, gf_mul(bch, a2, c2), tmp) == 4)
    {
        for (i = 0; i < 4; i++)
        {
            if (tmp[i] != a2)
            {
                roots[n++] = (unsigned int)a_ilog(bch, tmp[i]);
            }
        }
    }
    return n;
}

static int find_poly_deg4_roots(struct fc_bch_control *bch, struct gf_poly *poly, unsigned int *roots)
{
    int          i, l, n = 0;
    unsigned int a, b, c, d, e = 0, f, a2, b2, c2, e4;
    if (poly->c[0] == 0)
    {
        return 0;
    }
    e4 = poly->c[4];
    d  = gf_div(bch, poly->c[0], e4);
    c  = gf_div(bch, poly->c[1], e4);
    b  = gf_div(bch, poly->c[2], e4);
    a  = gf_div(bch, poly->c[3], e4);
    if (a)
    {
        if (c)
        {
            f = gf_div(bch, c, a);
            l = a_log(bch, f);
            l += (l & 1) ? (int)GF_N(bch) : 0;
            e = a_pow(bch, l / 2);
            d = a_pow(bch, 2 * l) ^ gf_mul(bch, b, f) ^ d;
            b = gf_mul(bch, a, e) ^ b;
        }
        if (d == 0)
        {
            return 0;
        }
        c2 = gf_inv(bch, d);
        b2 = gf_div(bch, a, d);
        a2 = gf_div(bch, b, d);
    }
    else
    {
        c2 = d;
        b2 = c;
        a2 = b;
    }
    if (find_affine4_roots(bch, a2, b2, c2, roots) == 4)
    {
        for (i = 0; i < 4; i++)
        {
            f        = a ? gf_inv(bch, roots[i]) : roots[i];
            roots[i] = (unsigned int)a_ilog(bch, f ^ e);
        }
        n = 4;
    }
    return n;
}

/* BTZ factoring */

static void gf_poly_logrep(struct fc_bch_control *bch, const struct gf_poly *a, int *rep)
{
    int i, d = (int)a->deg, l = (int)(GF_N(bch) - (unsigned int)a_log(bch, a->c[a->deg]));
    for (i = 0; i < d; i++)
    {
        rep[i] = a->c[i] ? mod_s(bch, (unsigned int)((int)a_log(bch, a->c[i]) + l)) : -1;
    }
}

static void gf_poly_mod(struct fc_bch_control *bch, struct gf_poly *a, const struct gf_poly *b, int *rep)
{
    int                la, p, m2;
    unsigned int       i, j, *c = a->c;
    const unsigned int d = b->deg;
    if ((unsigned int)a->deg < d)
    {
        return;
    }
    if (!rep)
    {
        rep = bch->cache;
        gf_poly_logrep(bch, b, rep);
    }
    for (j = (unsigned int)a->deg; j >= d; j--)
    {
        if (c[j])
        {
            la = a_log(bch, c[j]);
            p  = (int)(j - d);
            for (i = 0; i < d; i++, p++)
            {
                m2 = rep[i];
                if (m2 >= 0)
                {
                    c[p] ^= bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)(m2 + la))];
                }
            }
        }
    }
    a->deg = (int)(d - 1U);
    while (!c[a->deg] && a->deg)
    {
        a->deg--;
    }
}

static void gf_poly_div(struct fc_bch_control *bch, struct gf_poly *a, const struct gf_poly *b, struct gf_poly *q)
{
    if ((unsigned int)a->deg >= b->deg)
    {
        q->deg = (int)((unsigned int)a->deg - b->deg);
        gf_poly_mod(bch, a, b, NULL);
        memcpy(q->c, &a->c[b->deg], (unsigned int)(1 + q->deg) * sizeof(unsigned int));
    }
    else
    {
        q->deg  = 0;
        q->c[0] = 0;
    }
}

static struct gf_poly *gf_poly_gcd(struct fc_bch_control *bch, struct gf_poly *a, struct gf_poly *b)
{
    struct gf_poly *tmp;
    if ((unsigned int)a->deg < b->deg)
    {
        tmp = a;
        a   = b;
        b   = tmp;
    }
    while (b->deg > 0)
    {
        gf_poly_mod(bch, a, b, NULL);
        tmp = a;
        a   = b;
        b   = tmp;
    }
    return a;
}

static void compute_trace_bk_mod(struct fc_bch_control *bch, int k, const struct gf_poly *f, struct gf_poly *z, struct gf_poly *out)
{
    const int m = (int)GF_M(bch);
    int       i, j;
    z->deg   = 1;
    z->c[0]  = 0;
    z->c[1]  = bch->a_pow_tab[(unsigned int)k];
    out->deg = 0;
    memset(out, 0, GF_POLY_SZ((unsigned int)f->deg));
    gf_poly_logrep(bch, f, bch->cache);
    for (i = 0; i < m; i++)
    {
        for (j = z->deg; j >= 0; j--)
        {
            out->c[j] ^= z->c[j];
            z->c[2 * j]     = gf_sqr(bch, z->c[j]);
            z->c[2 * j + 1] = 0;
        }
        if ((unsigned int)z->deg > (unsigned int)out->deg)
        {
            out->deg = z->deg;
        }
        if (i < m - 1)
        {
            z->deg *= 2;
            gf_poly_mod(bch, z, f, bch->cache);
        }
    }
    while (!out->c[out->deg] && out->deg)
    {
        out->deg--;
    }
}

static void factor_polynomial(struct fc_bch_control *bch, int k, struct gf_poly *f, struct gf_poly **g, struct gf_poly **h)
{
    struct gf_poly *f2 = bch->poly_2t[0], *q = bch->poly_2t[1], *tk = bch->poly_2t[2], *z = bch->poly_2t[3], *gcd;
    *g = f;
    *h = NULL;
    compute_trace_bk_mod(bch, k, f, z, tk);
    if (tk->deg > 0)
    {
        gf_poly_copy(f2, f);
        gcd = gf_poly_gcd(bch, f2, tk);
        if ((unsigned int)gcd->deg < (unsigned int)f->deg)
        {
            gf_poly_div(bch, f, gcd, q);
            *h = &((struct gf_poly_deg1 *)f)[gcd->deg].poly;
            gf_poly_copy(*g, gcd);
            gf_poly_copy(*h, q);
        }
    }
}

static int find_poly_roots(struct fc_bch_control *bch, unsigned int k, struct gf_poly *poly, unsigned int *roots)
{
    int             cnt;
    struct gf_poly *f1, *f2;
    switch (poly->deg)
    {
        case 1:
            cnt = find_poly_deg1_roots(bch, poly, roots);
            break;
        case 2:
            cnt = find_poly_deg2_roots(bch, poly, roots);
            break;
        case 3:
            cnt = find_poly_deg3_roots(bch, poly, roots);
            break;
        case 4:
            cnt = find_poly_deg4_roots(bch, poly, roots);
            break;
        default:
            cnt = 0;
            if (poly->deg && (k <= GF_M(bch)))
            {
                factor_polynomial(bch, (int)k, poly, &f1, &f2);
                if (f1)
                {
                    cnt += find_poly_roots(bch, k + 1, f1, roots);
                }
                if (f2)
                {
                    cnt += find_poly_roots(bch, k + 1, f2, roots + (unsigned int)cnt);
                }
            }
            break;
    }
    return cnt;
}

/* ═══════════════════════════════════════════════════════════════════
 * 解码（综合式 → BM → 求根 → 纠错位置输出）
 * ═══════════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_bch_decode
 *   功能：BCH 解码——检测并定位 data+recv_ecc 中的错误。
 *   参数：bch —— 引擎；data —— 数据；len —— 数据字节数；
 *         recv_ecc —— 接收校验；calc_ecc —— 预计算校验（可 NULL）；
 *         syn —— 预计算综合式（可 NULL）；errloc —— 输出错误位置数组。
 *   返回：纠正的错误数（≥0）；不可纠返回 -1。
 *   实现要点：
 *     - 无 calc_ecc/syn 时：先编码 data 得预期校验，与 recv_ecc 异或
 *       （= 错误综合式），全零则无错返回 0。
 *     - BM 求 σ；求根得 errloc；errloc 换算为 bit 位置
 *       （含 swap_bits / 位序翻转修正）。
 *     - 错误数 ≠ 根数 → 不可纠。
 * -------------------------------------------------------------------- */
int fc_bch_decode(struct fc_bch_control *bch, const uint8_t *data, unsigned int len, const uint8_t *recv_ecc, const uint8_t *calc_ecc, const unsigned int *syn, unsigned int *errloc)
{
    const unsigned int ecc_words = BCH_ECC_WORDS(bch);
    unsigned int       nbits;
    int                i, err, nroots;
    uint32_t           sum;

    if (8U * len > (bch->n - bch->ecc_bits))
    {
        return -1;
    }

    if (!syn)
    {
        if (!calc_ecc)
        {
            if (!data || !recv_ecc)
            {
                return -1;
            }
            fc_bch_encode(bch, data, len, NULL);
        }
        else
        {
            load_ecc8(bch, bch->ecc_buf, calc_ecc);
        }
        if (recv_ecc)
        {
            load_ecc8(bch, bch->ecc_buf2, recv_ecc);
            for (i = 0, sum = 0; i < (int)ecc_words; i++)
            {
                bch->ecc_buf[i] ^= bch->ecc_buf2[i];
                sum |= bch->ecc_buf[i];
            }
            if (!sum)
            {
                return 0;
            }
        }
        compute_syndromes(bch, bch->ecc_buf, bch->syn);
        syn = bch->syn;
    }
    err = compute_error_locator_polynomial(bch, syn);
    if (err > 0)
    {
        nroots = find_poly_roots(bch, 1, bch->elp, errloc);
        if (err != nroots)
        {
            err = -1;
        }
    }
    if (err > 0)
    {
        nbits = len * 8U + bch->ecc_bits;
        for (i = 0; i < err; i++)
        {
            if (errloc[i] >= nbits)
            {
                err = -1;
                break;
            }
            errloc[i] = nbits - 1 - errloc[i];
            if (!bch->swap_bits)
            {
                errloc[i] = (errloc[i] & ~7U) | (7U - (errloc[i] & 7U));
            }
        }
    }
    return (err >= 0) ? err : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * 初始化（域表 / 生成多项式 / 查找表 / 度 2 基底）
 * ═══════════════════════════════════════════════════════════════════ */

/* 构建 GF(2^m) 对数/反对数表：x 左移递推，遇本原多项式归约 */
static int build_gf_tables(struct fc_bch_control *bch, unsigned int poly)
{
    unsigned int       i, x = 1;
    const unsigned int k = 1U << (unsigned int)deg(poly);
    if (k != (1U << GF_M(bch)))
    {
        return -1;
    }
    for (i = 0; i < GF_N(bch); i++)
    {
        bch->a_pow_tab[i] = (uint16_t)x;
        bch->a_log_tab[x] = (uint16_t)i;
        if (i && (x == 1))
        {
            return -1;
        }
        x <<= 1;
        if (x & k)
        {
            x ^= poly;
        }
    }
    bch->a_pow_tab[GF_N(bch)] = 1;
    bch->a_log_tab[0]         = 0;
    return 0;
}

/* 构建 mod8 编码查找表：每输入字节偏移 8b，与生成多项式做多项式模除 */
static void build_mod8_tables(struct fc_bch_control *bch, const uint32_t *g)
{
    int       i, j, b, d;
    uint32_t  data, hi, lo, *tab;
    const int l      = (int)BCH_ECC_WORDS(bch);
    const int plen   = (int)DIV_ROUND_UP(bch->ecc_bits + 1U, 32);
    const int ecclen = (int)DIV_ROUND_UP(bch->ecc_bits, 32);
    memset(bch->mod8_tab, 0, (size_t)(4 * 256 * l) * sizeof(*bch->mod8_tab));
    for (i = 0; i < 256; i++)
    {
        for (b = 0; b < 4; b++)
        {
            tab  = bch->mod8_tab + (size_t)((b * 256 + i) * (unsigned int)l);
            data = (uint32_t)i << (unsigned int)(8 * b);
            while (data)
            {
                d = (int)deg(data);
                data ^= g[0] >> (31U - (unsigned int)d);
                for (j = 0; j < ecclen; j++)
                {
                    hi = ((unsigned int)d < 31U) ? g[j] << ((unsigned int)d + 1U) : 0;
                    lo = ((unsigned int)(j + 1) < (unsigned int)plen) ? g[j + 1] >> (31U - (unsigned int)d) : 0;
                    tab[j] ^= hi | lo;
                }
            }
        }
    }
}

/* 构建度 2 多项式求根用的迹映射表 xi_tab（二次方程判别式映射） */
static int build_deg2_base(struct fc_bch_control *bch)
{
    const int    m = (int)GF_M(bch);
    int          i, j, r;
    unsigned int sum, x, y, remaining = (unsigned int)m, ak = 0, xi[BCH_MAX_M];
    for (i = 0; i < m; i++)
    {
        for (j = 0, sum = 0; j < m; j++)
        {
            sum ^= a_pow(bch, i * (1 << j));
        }
        if (sum)
        {
            ak = bch->a_pow_tab[i];
            break;
        }
    }
    memset(xi, 0, sizeof(xi));
    for (x = 0; (x <= GF_N(bch)) && remaining; x++)
    {
        y = gf_sqr(bch, x) ^ x;
        for (i = 0; i < 2; i++)
        {
            r = a_log(bch, y);
            if (y && (r < m) && !xi[r])
            {
                bch->xi_tab[r] = x;
                xi[r]          = 1;
                remaining--;
                break;
            }
            y ^= ak;
        }
    }
    return remaining ? -1 : 0;
}

/* --------------------------------------------------------------------
 * compute_generator_polynomial
 *   功能：计算 BCH 生成多项式 g(x)。
 *   实现要点：g(x) 的根 = α^(2i+1)（i=0..t-1），共 2t 个连续奇次幂；
 *         乘积展开得系数，打包为 32bit 大端字。
 *         输出同时设 bch->ecc_bits = deg(g) = m·t。
 * -------------------------------------------------------------------- */
static uint32_t *compute_generator_polynomial(struct fc_bch_control *bch)
{
    const unsigned int m = GF_M(bch), t = GF_T(bch);
    int                n;
    unsigned int       i, j, r, word, *roots;
    uint32_t          *genpoly;
    struct gf_poly    *g;

    g       = (struct gf_poly *)malloc(GF_POLY_SZ(m * t));
    roots   = (unsigned int *)calloc((bch->n + 1U), sizeof(unsigned int));
    genpoly = (uint32_t *)calloc(DIV_ROUND_UP(m * t + 1U, 32), sizeof(uint32_t));
    if (!g || !roots || !genpoly)
    {
        free(g);
        free(roots);
        free(genpoly);
        return NULL;
    }

    for (i = 0; i < t; i++)
    {
        for (j = 0, r = 2U * i + 1U; j < m; j++)
        {
            roots[r] = 1;
            r        = (unsigned int)mod_s(bch, 2U * r);
        }
    }
    g->deg  = 0;
    g->c[0] = 1;
    for (i = 0; i < GF_N(bch); i++)
    {
        if (roots[i])
        {
            r                = bch->a_pow_tab[i];
            g->c[g->deg + 1] = 1;
            for (j = (unsigned int)g->deg; j > 0; j--)
            {
                g->c[j] = gf_mul(bch, g->c[j], r) ^ g->c[j - 1];
            }
            g->c[0] = gf_mul(bch, g->c[0], r);
            g->deg++;
        }
    }
    n = g->deg + 1;
    i = 0;
    while (n > 0)
    {
        unsigned int nbits2 = ((unsigned int)n > 32U) ? 32U : (unsigned int)n;
        word                = 0;
        for (j = 0; j < nbits2; j++)
        {
            if (g->c[(unsigned int)n - 1U - j])
            {
                word |= 1U << (31U - j);
            }
        }
        genpoly[i++] = word;
        n -= (int)nbits2;
    }
    bch->ecc_bits = (unsigned int)g->deg;
    free(g);
    free(roots);
    return genpoly;
}

/* --------------------------------------------------------------------
 * fc_bch_init
 *   功能：初始化 BCH 引擎（分配上下文 + 建表）。
 *   参数：m —— 域阶（5..15）；t —— 纠错能力；prim_poly —— 本原多项式
 *         （0=默认查表）；swap_bits —— 是否交换数据位序。
 *   返回：引擎句柄；失败返回 NULL。
 *   实现要点：
 *     - 参数合法性校验（m/t 上限 + m·t < 2^m-1）。
 *     - 分配全部表（a_pow/a_log/mod8/ecc_buf/xi/syn/cache/elp/poly_2t）。
 *     - 建域表 → 生成多项式 → mod8 查表 → 度 2 基底；
 *       任一步失败即整体释放返回 NULL。
 * -------------------------------------------------------------------- */
fc_bch_control_t *fc_bch_init(int m, int t, unsigned int prim_poly, uint8_t swap_bits)
{
    int                       err = 0;
    unsigned int              i, words;
    uint32_t                 *genpoly;
    fc_bch_control_t         *bch             = NULL;
    static const unsigned int prim_poly_tab[] = {
        0x25,
        0x43,
        0x83,
        0x11D,
        0x211,
        0x409,
        0x805,
        0x1053,
        0x201B,
        0x402B,
        0x8003,
    };
    if ((m < 5) || (m > BCH_MAX_M) || (t < 1) || (t > BCH_MAX_T) || (m * t >= (1 << m) - 1))
    {
        return NULL;
    }
    if (prim_poly == 0)
    {
        prim_poly = prim_poly_tab[(unsigned int)(m - 5)];
    }
    bch = (fc_bch_control_t *)calloc(1, sizeof(fc_bch_control_t));
    if (!bch)
    {
        return NULL;
    }
    bch->m         = (unsigned int)m;
    bch->t         = (unsigned int)t;
    bch->n         = (unsigned int)((1 << m) - 1);
    words          = DIV_ROUND_UP((unsigned int)(m * t), 32U);
    bch->ecc_bytes = (unsigned int)DIV_ROUND_UP((unsigned int)(m * t), 8U);
    bch->swap_bits = swap_bits;
    bch->a_pow_tab = (uint16_t *)calloc((size_t)(1 + bch->n), sizeof(uint16_t));
    bch->a_log_tab = (uint16_t *)calloc((size_t)(1 + bch->n), sizeof(uint16_t));
    bch->mod8_tab  = (uint32_t *)calloc(words * 1024U, sizeof(uint32_t));
    bch->ecc_buf   = (uint32_t *)calloc(words, sizeof(uint32_t));
    bch->ecc_buf2  = (uint32_t *)calloc(words, sizeof(uint32_t));
    bch->xi_tab    = (unsigned int *)calloc((unsigned int)m, sizeof(unsigned int));
    bch->syn       = (unsigned int *)calloc(2U * (unsigned int)t, sizeof(unsigned int));
    bch->cache     = (int *)calloc(2U * (unsigned int)t, sizeof(int));
    bch->elp       = (struct gf_poly *)calloc((size_t)((unsigned int)t + 1U), sizeof(struct gf_poly_deg1));
    for (i = 0; i < 4; i++)
    {
        bch->poly_2t[i] = (struct gf_poly *)malloc(GF_POLY_SZ(2U * (unsigned int)t));
    }
    if (!bch->a_pow_tab || !bch->a_log_tab || !bch->mod8_tab || !bch->ecc_buf || !bch->ecc_buf2 || !bch->xi_tab || !bch->syn || !bch->cache || !bch->elp)
    {
        err = 1;
    }
    for (i = 0; i < 4; i++)
    {
        if (!bch->poly_2t[i])
        {
            err = 1;
        }
    }
    if (!err)
    {
        err = build_gf_tables(bch, prim_poly);
    }
    if (!err)
    {
        genpoly = compute_generator_polynomial(bch);
        if (genpoly)
        {
            build_mod8_tables(bch, genpoly);
            free(genpoly);
        }
        else
        {
            err = 1;
        }
    }
    if (!err)
    {
        err = build_deg2_base(bch);
    }
    if (err)
    {
        fc_bch_free(bch);
        return NULL;
    }
    return bch;
}

/* 释放 BCH 引擎全部资源 */
void fc_bch_free(fc_bch_control_t *bch)
{
    unsigned int i;
    if (bch)
    {
        free(bch->a_pow_tab);
        free(bch->a_log_tab);
        free(bch->mod8_tab);
        free(bch->ecc_buf);
        free(bch->ecc_buf2);
        free(bch->xi_tab);
        free(bch->syn);
        free(bch->cache);
        free(bch->elp);
        for (i = 0; i < 4; i++)
        {
            free(bch->poly_2t[i]);
        }
        free(bch);
    }
}

/* 获取 ECC 字节数 */
unsigned int fc_bch_ecc_bytes(const fc_bch_control_t *bch)
{
    return bch->ecc_bytes;
}

/* 获取纠错能力 t */
unsigned int fc_bch_t(const fc_bch_control_t *bch)
{
    return bch->t;
}