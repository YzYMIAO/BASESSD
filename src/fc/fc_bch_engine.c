
#include "fc_bch_engine.h"

#include "assertion.h"
#include "ssd_compiler.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// fc_bch_engine.c — BCH ECC 引擎（纠错码）
//
// 移植自 Linux 内核 lib/bch.c（GPL v2），作者 Ivan Djelic。
// 内核依赖替换为标准 C；表存储为**编译期静态结构体**，
// 因为本工程参数固定（m=13, t=4），表生命周期等于固件生命周期。
//
// ── 算法（BCH 码 = 有限域上的循环码）──
// 编码：4 查找表并行 32bit 输入（生成多项式 g(x) 的 mod 查表）
// 解码：综合式 S(x) → Berlekamp–Massey（错误位置多项式 σ(x)）
// → 求根（BTZ factoring：degree 1..4 解析 + 高次因数分解）
// → 按错误位置翻转 bit 纠正。
//
// ── 关键公式出处 ──
// · 域 GF(2^m)：本原多项式 prim_poly，元素 a_pow/a_log 查表（对数域乘法）
// · 生成多项式 g(x) = lcm{ m1(x), m3(x), ..., m(2t-1)(x) }，
// 根 = 连续 2t 个 α^i 的极小多项式之积（BCH 码构造定理）
// · 综合式 S_j = r(α^j)，j=1..2t（接收多项式在根处的取值 = 错误分量）
// · Berlekamp–Massey：由 S 递推最小 σ(x)，σ(α^-i)=0 的位置即错误位置
// · Chien 搜索等价物：本实现用 BTZ 因子分解（度≤4 解析求根 + 递归分解）
//
// ── 本工程参数 ──
// m=13, t=4：ecc_bits = 13×4 = 52bit → ecc_bytes=7，8B 槽够用
// 码长 n = 2^13 - 1 = 8191；数据位 ≤ n - 52（520B×8 = 4160 < 8139 ✓）

/* ── 辅助宏 ─────────────────────────────────────────────────────── */

#define DIV_ROUND_UP(n, d) (((n) + (d)-1U) / (d))

#ifndef MIN
#    define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/**
 * @brief fls — find last set bit（最高位位置+1；x=0 → 0）
 * @param x 待求值
 * @return 最高置位 bit 的位置 +1；x=0 时返回 0
 */
static inline int fls(unsigned int x)
{
    if (x == 0U)
    {
        return 0;
    }
    return (int)(sizeof(unsigned int) * 8U - __builtin_clz(x));
}

/**
 * @brief 8-bit 位序反转（swap_bits 模式：NAND 数据位序与算法假设相反时用）
 * @param x 输入字节
 * @return 位序反转后的字节
 */
static inline BYTE bitrev8(BYTE x)
{
    x = ((x & 0xF0U) >> 4U) | ((x & 0x0FU) << 4U);
    x = ((x & 0xCCU) >> 2U) | ((x & 0x33U) << 2U);
    x = ((x & 0xAAU) >> 1U) | ((x & 0x55U) << 1U);
    return x;
}

/**
 * @brief 大端序转换（数据按 BE 语义输入 BCH 引擎）
 * @param x 主机序 32 位字
 * @return 字节序反转后的 32 位字
 */
static inline DWORD cpu_to_be32(DWORD x)
{
    return __builtin_bswap32(x);
}

/* ── 参数与数据结构 ──────────────────────────────────────────────── */

#define BCH_MAX_M 15 /* 域阶上限（GF(2^15)） */
#define BCH_MAX_T 64 /* 纠错能力上限 */

// BCH 引擎上下文：
// a_pow_tab[a_log_tab]  —— GF(2^m) 对数/反对数表（乘法→加法）
// mod8_tab               —— 编码查找表（每字节偏移，4 组 256 项）
// ecc_buf/ecc_buf2       —— 校验字工作区
// xi_tab                 —— 度 2 多项式求根用的迹映射表
// syn                    —— 综合式数组（2t 项）
// cache                  —— 多项式 mod 用对数表示缓存
// elp / poly_2t[]        —— 错误位置多项式 + 求根工作多项式
// swap_bits              —— 输入/输出位序是否交换
struct fc_bch_control
{
    unsigned int    m;
    unsigned int    n;
    unsigned int    t;
    unsigned int    ecc_bits;
    unsigned int    ecc_bytes;
    WORD           *a_pow_tab;
    WORD           *a_log_tab;
    DWORD          *mod8_tab;
    DWORD          *ecc_buf;
    DWORD          *ecc_buf2;
    unsigned int   *xi_tab;
    unsigned int   *syn;
    int            *cache;
    struct gf_poly *elp;
    struct gf_poly *poly_2t[4];
    BYTE            swap_bits;
};

#define GF_M(_p)          ((_p)->m)
#define GF_T(_p)          ((_p)->t)
#define GF_N(_p)          ((_p)->n)
#define BCH_ECC_WORDS(_p) DIV_ROUND_UP(GF_M(_p) * GF_T(_p), 32)
#define BCH_ECC_BYTES(_p) ((_p)->ecc_bytes)
#define BCH_ECC_MAX_WORDS DIV_ROUND_UP(BCH_MAX_M *BCH_MAX_T, 32)

// GF(2^m) 上的多项式：deg = 次数，c[] = 系数（低位优先）
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

/* ── 固定存储（本工程唯一参数：m=13, t=4）────────────────────────────
 * BCH 表的生命周期 = 固件生命周期，大小编译期确定；用静态结构体而不是
 * malloc/calloc。放 DTCM（链接脚本整文件规则），避免 41 KB 堆与栈共用 DTCM，
 * 也避免“堆不足 → 初始化失败 → ECC 被静默跳过”。
 * ⚠ 这里只支持 m=13/t=4；换参数必须重算数组尺寸并加 _Static_assert。 */
#define FC_BCH_M          (13U)
#define FC_BCH_T          (4U)
#define FC_BCH_N          ((1U << FC_BCH_M) - 1U)
#define FC_BCH_WORDS      DIV_ROUND_UP(FC_BCH_M *FC_BCH_T, 32U)
#define FC_BCH_ECC_DWORDS (1U + (FC_BCH_M * FC_BCH_T) + 1U) /* GF_POLY_SZ(m*t) 的 DWORD 数 */

typedef struct
{
    fc_bch_control_t    ctl;
    WORD                a_pow[FC_BCH_N + 1U];
    WORD                a_log[FC_BCH_N + 1U];
    DWORD               mod8[FC_BCH_WORDS * 1024U];
    DWORD               ecc_buf[FC_BCH_WORDS];
    DWORD               ecc_buf2[FC_BCH_WORDS];
    unsigned int        xi[FC_BCH_M];
    unsigned int        syn[2U * FC_BCH_T];
    int                 cache[2U * FC_BCH_T];
    struct gf_poly_deg1 elp[FC_BCH_T + 1U];
    DWORD               poly_2t[4U][1U + (2U * FC_BCH_T) + 1U];
    DWORD               roots[(FC_BCH_N + 1U + 31U) / 32U]; /* 生成多项式根的 bitmap */
    DWORD               genpoly[2U];
    DWORD               g_poly[FC_BCH_ECC_DWORDS];
} fc_bch_storage_t;

static fc_bch_storage_t s_bch;

_Static_assert(sizeof(fc_bch_storage_t) <= (48U * 1024U), "BCH static storage exceeds 48 KiB");

/* ── 前向声明 ────────────────────────────────────────────────────── */

static BYTE         swap_bits(struct fc_bch_control *bch, BYTE in);
static void         load_ecc8(struct fc_bch_control *bch, DWORD *dst, const BYTE *src);
static void         store_ecc8(struct fc_bch_control *bch, BYTE *dst, const DWORD *src);
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
static void         compute_syndromes(struct fc_bch_control *bch, DWORD *ecc, unsigned int *syn);
static bool         compute_error_locator_polynomial(struct fc_bch_control *bch, const unsigned int *syn, unsigned int *out_deg);
static int          find_poly_roots(struct fc_bch_control *bch, unsigned int k, struct gf_poly *poly, unsigned int *roots);
static bool         build_gf_tables(struct fc_bch_control *bch, unsigned int poly);
static void         build_mod8_tables(struct fc_bch_control *bch, const DWORD *g);
static bool         build_deg2_base(struct fc_bch_control *bch);
static DWORD       *compute_generator_polynomial(struct fc_bch_control *bch);

/* ═══════════════════════════════════════════════════════════════════
 * 辅助函数
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief 按引擎配置决定是否对输入字节做位序反转。
 * @param bch BCH 控制句柄
 * @param in  输入字节
 * @return bch->swap_bits 置位时为 bitrev8(in)，否则原样返回
 */
static BYTE swap_bits(struct fc_bch_control *bch, BYTE in)
{
    return bch->swap_bits ? bitrev8(in) : in;
}

/* ═══════════════════════════════════════════════════════════════════
 * 编码
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief 非对齐数据段的逐字节编码（mod8 查表推进校验寄存器）
 * @param bch  BCH 控制句柄
 * @param data 输入数据（不要求 4 字节对齐）
 * @param len  数据字节数
 * @param ecc  校验寄存器工作区（BCH_ECC_WORDS 个 DWORD，原地推进）
 */
static void bch_encode_unaligned(struct fc_bch_control *bch, const unsigned char *data, unsigned int len, DWORD *ecc)
{
    int          i;
    const DWORD *p;
    const int    l = (int)BCH_ECC_WORDS(bch) - 1;

    while (len--)
    {
        BYTE tmp = swap_bits(bch, *data++);
        // 最高字节 ⊕ 输入字节 → 查表，表项与其余校验字异或移位
        p        = bch->mod8_tab + (unsigned int)(l + 1) * (((ecc[0] >> 24) ^ tmp) & 0xFFU);
        for (i = 0; i < l; i++)
        {
            ecc[i] = ((ecc[i] << 8) | (ecc[i + 1] >> 24)) ^ (*p++);
        }
        ecc[l] = (ecc[l] << 8) ^ (*p);
    }
}

/**
 * @brief 把 ECC 字节（可选位序交换）载入工作区（32bit 字）
 * @param bch BCH 控制句柄
 * @param dst 目标工作区（BCH_ECC_WORDS 个 DWORD）
 * @param src ECC 字节源（ecc_bytes 字节，末尾不足 4 字节处补零）
 */
static void load_ecc8(struct fc_bch_control *bch, DWORD *dst, const BYTE *src)
{
    BYTE         pad[4] = {0, 0, 0, 0};
    unsigned int i, nwords = BCH_ECC_WORDS(bch) - 1;

    for (i = 0; i < nwords; i++, src += 4)
    {
        dst[i] = ((DWORD)swap_bits(bch, src[0]) << 24) | ((DWORD)swap_bits(bch, src[1]) << 16) | ((DWORD)swap_bits(bch, src[2]) << 8) | (DWORD)swap_bits(bch, src[3]);
    }
    memcpy(pad, src, BCH_ECC_BYTES(bch) - 4 * nwords);
    dst[nwords] = ((DWORD)swap_bits(bch, pad[0]) << 24) | ((DWORD)swap_bits(bch, pad[1]) << 16) | ((DWORD)swap_bits(bch, pad[2]) << 8) | (DWORD)swap_bits(bch, pad[3]);
}

/**
 * @brief 把工作区写回 ECC 字节（与 load 对称）
 * @param bch BCH 控制句柄
 * @param dst ECC 字节目标缓冲区（ecc_bytes 字节）
 * @param src 工作区（BCH_ECC_WORDS 个 DWORD）
 */
static void store_ecc8(struct fc_bch_control *bch, BYTE *dst, const DWORD *src)
{
    BYTE         pad[4];
    unsigned int i, nwords = BCH_ECC_WORDS(bch) - 1;

    for (i = 0; i < nwords; i++)
    {
        *dst++ = swap_bits(bch, (src[i] >> 24));
        *dst++ = swap_bits(bch, (src[i] >> 16));
        *dst++ = swap_bits(bch, (src[i] >> 8));
        *dst++ = swap_bits(bch, (src[i]));
    }
    pad[0] = swap_bits(bch, (src[nwords] >> 24));
    pad[1] = swap_bits(bch, (src[nwords] >> 16));
    pad[2] = swap_bits(bch, (src[nwords] >> 8));
    pad[3] = swap_bits(bch, (src[nwords]));
    memcpy(dst, pad, BCH_ECC_BYTES(bch) - 4 * nwords);
}

/**
 * @brief 计算数据段的 BCH 校验字
 *
 * 4 张查找表并行处理对齐的 32bit 段，首尾非对齐段逐字节兜底。
 * ecc 非 NULL 时以其为初值并写回（增量计算），为 NULL 时从零算起。
 *
 * @param bch  BCH 控制句柄
 * @param data 数据段
 * @param len  字节数
 * @param ecc  校验字缓冲（ecc_bytes 字节，可 NULL）
 */
void fc_bch_encode(struct fc_bch_control *bch, const BYTE *data, unsigned int len, BYTE *ecc)
{
    ASSERT(bch != NULL);
    ASSERT(data != NULL);
    const unsigned int l = BCH_ECC_WORDS(bch) - 1;
    unsigned int       i, mlen;
    unsigned long      m;
    DWORD              w, r[BCH_ECC_MAX_WORDS];
    const size_t       r_bytes = BCH_ECC_WORDS(bch) * sizeof(*r);
    const DWORD *const tab0    = bch->mod8_tab;
    const DWORD *const tab1    = tab0 + 256 * (l + 1);
    const DWORD *const tab2    = tab1 + 256 * (l + 1);
    const DWORD *const tab3    = tab2 + 256 * (l + 1);
    const DWORD       *pdata, *p0, *p1, *p2, *p3;

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

    pdata = (const DWORD *)(const void *)data;
    mlen  = len / 4U;
    data += 4U * mlen;
    len -= 4U * mlen;
    memcpy(r, bch->ecc_buf, r_bytes);

    while (mlen--)
    {
        w = cpu_to_be32(*pdata++);
        if (bch->swap_bits)
        {
            w = (DWORD)swap_bits(bch, (w >> 0)) | ((DWORD)swap_bits(bch, (w >> 8)) << 8) | ((DWORD)swap_bits(bch, (w >> 16)) << 16) | ((DWORD)swap_bits(bch, (w >> 24)) << 24);
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

/**
 * @brief 对 v 做 mod n 归约（利用 2^m ≡ 1 的循环性质）
 * @param bch BCH 控制句柄
 * @param v   待归约的无符号值
 * @return 归约到 [0, n) 的结果
 */
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

/**
 * @brief 快速 mod：v < n 原样返回，否则减一次 n（v 保证 < 2n）
 * @param bch BCH 控制句柄
 * @param v   待归约的值（调用者保证 < 2n）
 * @return 归约到 [0, n) 的结果
 */
static inline int mod_s(struct fc_bch_control *bch, unsigned int v)
{
    const unsigned int n = GF_N(bch);
    return (v < n) ? (int)v : (int)(v - n);
}

/**
 * @brief 多项式最高次数（x=0 → -1）
 * @param poly 多项式系数位图
 * @return 最高非零 bit 的位置；poly=0 时返回 -1（零多项式次数无定义）
 */
static inline int deg(unsigned int poly)
{
    if (poly == 0)
    {
        return -1;
    }
    return (int)(sizeof(unsigned int) * 8U - __builtin_clz(poly)) - 1;
}

/**
 * @brief GF 除法 a/b = a_log[a] - a_log[b] 再查幂表
 * @param bch BCH 控制句柄
 * @param a   被除数（GF 元素）
 * @param b   除数（GF 元素）
 * @return a/b；a 或 b 为 0 时返回 0
 */
static inline unsigned int gf_div(struct fc_bch_control *bch, unsigned int a, unsigned int b)
{
    if ((a == 0U) || (b == 0U))
    {
        return 0U;
    }
    return bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)((int)bch->a_log_tab[a] + (int)((int)GF_N(bch) - (int)bch->a_log_tab[b])))];
}

/**
 * @brief GF 乘法 a·b = α^(log a + log b)
 * @param bch BCH 控制句柄
 * @param a   乘数（GF 元素）
 * @param b   乘数（GF 元素）
 * @return a·b；任一为 0 时返回 0
 */
static inline unsigned int gf_mul(struct fc_bch_control *bch, unsigned int a, unsigned int b)
{
    return (a && b) ? bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)((int)bch->a_log_tab[a] + (int)bch->a_log_tab[b]))] : 0;
}

/**
 * @brief GF 平方：a² = α^(2·log a)
 * @param bch BCH 控制句柄
 * @param a   待平方值（GF 元素）
 * @return a²；a 为 0 时返回 0
 */
static inline unsigned int gf_sqr(struct fc_bch_control *bch, unsigned int a)
{
    return a ? bch->a_pow_tab[(unsigned int)mod_s(bch, 2 * (int)bch->a_log_tab[a])] : 0;
}

/**
 * @brief GF 逆：a^-1 = α^(n - log a)（n = 2^m-1 时 α^(n-1) = α^-1）
 * @param bch BCH 控制句柄
 * @param a   待求逆的 GF 元素
 * @return a 的乘法逆元
 */
static inline unsigned int gf_inv(struct fc_bch_control *bch, unsigned int a)
{
    return bch->a_pow_tab[GF_N(bch) - (unsigned int)bch->a_log_tab[a]];
}

/**
 * @brief α^i（i 可为负，经 modulo 归约）
 * @param bch BCH 控制句柄
 * @param i   指数（可为负）
 * @return α^i
 */
static inline unsigned int a_pow(struct fc_bch_control *bch, int i)
{
    return bch->a_pow_tab[(unsigned int)modulo(bch, (unsigned int)i)];
}

/**
 * @brief 离散对数：a_log[x] = i 使 α^i = x
 * @param bch BCH 控制句柄
 * @param x   GF 元素
 * @return 满足 α^i = x 的指数 i
 */
static inline int a_log(struct fc_bch_control *bch, unsigned int x)
{
    return (int)bch->a_log_tab[x];
}

/**
 * @brief 逆对数：返回 x^-1 的指数 n - log x（即 gf_inv 的对数形式）
 * @param bch BCH 控制句柄
 * @param x   GF 元素
 * @return x 的乘法逆元对应的指数
 */
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

/**
 * @brief 由校验字计算 2t 项综合式 S_j = r(α^j)。
 * @param bch BCH 控制句柄
 * @param ecc 校验字工作区（BCH_ECC_WORDS 个 DWORD，尾部 padding bit 会被就地清零）
 * @param syn 输出综合式数组（2t 项）
 */
static void compute_syndromes(struct fc_bch_control *bch, DWORD *ecc, unsigned int *syn)
{
    int          i, j, s;
    unsigned int m;
    DWORD        poly;
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

/**
 * @brief 复制多项式（含 deg+1 个系数）。
 * @param dst 目标多项式（容量须容纳 src->deg）
 * @param src 源多项式
 */
static void gf_poly_copy(struct gf_poly *dst, struct gf_poly *src)
{
    memcpy(dst, src, GF_POLY_SZ(src->deg));
}

/**
 * @brief BM 算法：由综合式递推错误位置多项式 σ(x)。
 * @param bch     BCH 控制句柄
 * @param syn     综合式数组（2t 项）
 * @param out_deg 输出 σ 的次数（= 错误数，0..t）
 * @return true  成功，*out_deg 有效
 *         false σ 的次数 > t，不可纠
 */
static bool compute_error_locator_polynomial(struct fc_bch_control *bch, const unsigned int *syn, unsigned int *out_deg)
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
    if ((int)elp->deg > (int)t)
    {
        return false;
    }
    *out_deg = (unsigned int)elp->deg;
    return true;
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

/**
 * @brief 解 GF(2) 线性方程组（高斯消元 + 回代）；nsol = 2^k2 个解枚举
 * @param bch  BCH 控制句柄
 * @param rows 方程组行（原地消元）
 * @param sol  输出解向量（nsol 项）
 * @param nsol 期望的解个数（须等于 2^k2）
 * @return 实际解个数 nsol；无解或解个数与期望不符时返回 0
 */
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
            x = (x & 0x11111111U) * 0x11111111U;
            // 第 r 行的解位落在 bit(m-r)。回代用的 tmp 里，解位一律比它的实际
            // 位号高 1（故末行 sol = tmp >> 1），这是 upstream 的约定。
            // ⚠ 写成 `tmp = (tmp << 1) | bit` 会把它放到 bit(m-1-r)——错一位，
            // 而 tmp|1 又要参与下一次回代 ⇒ 整个解向量错位，不是最后差一位。
            // 后果：BCH(m=13,t=4) 实际只有 t=2（3 bit 错一律当不可纠、4 bit 错
            // 约 7% 被"纠"成另一个码字且返回 OK）。见 test_ecc_step_correct。
            tmp |= (unsigned int)((x >> 28) & 1U) << (unsigned int)(m - r);
        }
        sol[p] = tmp >> 1;
    }
    return nsol;
}

/**
 * @brief 求四次仿射多项式的根：先构造 m 行线性方程组，再交给 solve_linear_system。
 * @param bch   BCH 控制句柄
 * @param a     仿射多项式系数 a
 * @param b     仿射多项式系数 b
 * @param c     仿射多项式系数 c
 * @param roots 输出根数组（4 项）
 * @return 找到的根数（0..4）
 */
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
        rows[i + 1] = bch->a_pow_tab[(unsigned int)(4 * i)] ^ (a ? bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)k3)] : 0) ^ (b ? bch->a_pow_tab[(unsigned int)mod_s(bch, (unsigned int)j)] : 0);
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

/**
 * @brief 解析求一次多项式在 GF(2^m) 中的根。
 * @param bch   BCH 控制句柄
 * @param poly 一次多项式
 * @param roots 输出根数组（1 项）
 * @return 找到的根数（0 或 1）
 */
static int find_poly_deg1_roots(struct fc_bch_control *bch, struct gf_poly *poly, unsigned int *roots)
{
    if (poly->c[0] == 0)
    {
        return 0;
    }
    roots[0] = (unsigned int)mod_s(bch, GF_N(bch) - (unsigned int)a_log(bch, poly->c[0]) + (unsigned int)a_log(bch, poly->c[1]));
    return 1;
}

/**
 * @brief 解析求二次多项式在 GF(2^m) 中的根（变换为 z²+z+u 后用迹映射表判定）。
 * @param bch   BCH 控制句柄
 * @param poly 二次多项式
 * @param roots 输出根数组（2 项）
 * @return 找到的根数（0 或 2）
 */
static int find_poly_deg2_roots(struct fc_bch_control *bch, struct gf_poly *poly, unsigned int *roots)
{
    int          n = 0, i, l0 = (int)bch->a_log_tab[poly->c[0]], l1 = (int)bch->a_log_tab[poly->c[1]], l2 = (int)bch->a_log_tab[poly->c[2]];
    unsigned int u, v, r;
    if ((poly->c[0] == 0) || (poly->c[1] == 0))
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

/**
 * @brief 解析求三次多项式在 GF(2^m) 中的根（归约为仿射四次方程的求解）。
 * @param bch   BCH 控制句柄
 * @param poly 三次多项式
 * @param roots 输出根数组（3 项）
 * @return 找到的根数（0..3）
 */
static int find_poly_deg3_roots(struct fc_bch_control *bch, struct gf_poly *poly, unsigned int *roots)
{
    int          i, n = 0;
    unsigned int a2, b2, c2, e3, tmp[4];
    if (poly->c[0] == 0)
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

/**
 * @brief 解析求四次多项式在 GF(2^m) 中的根（归约为仿射四次方程的求解）。
 * @param bch   BCH 控制句柄
 * @param poly 四次多项式
 * @param roots 输出根数组（4 项）
 * @return 找到的根数（0 或 4）
 */
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

/**
 * @brief 计算多项式的对数表示（各系数除以首项后的离散对数，零系数记 -1）。
 * @param bch BCH 控制句柄
 * @param a   多项式
 * @param rep 输出对数表示数组（a->deg 项）
 */
static void gf_poly_logrep(struct fc_bch_control *bch, const struct gf_poly *a, int *rep)
{
    int i, d = (int)a->deg, l = (int)(GF_N(bch) - (unsigned int)a_log(bch, a->c[a->deg]));
    for (i = 0; i < d; i++)
    {
        rep[i] = a->c[i] ? mod_s(bch, (unsigned int)((int)a_log(bch, a->c[i]) + l)) : -1;
    }
}

/**
 * @brief 多项式取模：a 原地变为 a mod b，并更新 a->deg。
 * @param bch BCH 控制句柄
 * @param a   被除多项式（原地变成余式）
 * @param b   除数多项式
 * @param rep b 的对数表示缓存；为 NULL 时用 bch->cache 现算
 */
static void gf_poly_mod(struct fc_bch_control *bch, struct gf_poly *a, const struct gf_poly *b, int *rep)
{
    int                la, p, m2;
    unsigned int       i, j, *c = a->c;
    const unsigned int d = b->deg;
    if ((unsigned int)a->deg < d)
    {
        return;
    }
    if (rep == NULL)
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
    while ((c[a->deg] == 0) && a->deg)
    {
        a->deg--;
    }
}

/**
 * @brief 多项式除法：q = a / b，a 原地变为余式。
 * @param bch BCH 控制句柄
 * @param a   被除多项式（原地被模掉）
 * @param b   除数多项式
 * @param q   输出商多项式
 */
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

/**
 * @brief 欧几里得算法求多项式最大公因式（原地修改 a、b）。
 * @param bch BCH 控制句柄
 * @param a   多项式 a
 * @param b   多项式 b
 * @return 最大公因式（指向 a 或 b 的工作区）
 */
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

/**
 * @brief 计算以 k 为迹阶的迹多项式 T_k(x) mod f。
 * @param bch BCH 控制句柄
 * @param k   迹阶
 * @param f   模多项式
 * @param z   工作多项式（复用 poly_2t 工作区）
 * @param out 输出迹多项式
 */
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
    while ((out->c[out->deg] == 0) && out->deg)
    {
        out->deg--;
    }
}

/**
 * @brief BTZ 分解：用迹多项式与 f 的 GCD 把 f 拆成两个因子。
 * @param bch BCH 控制句柄
 * @param k   迹的阶
 * @param f   待分解多项式（原地被拆分）
 * @param g   输出第一个因子（无法拆分时 = f）
 * @param h   输出第二个因子（无法拆分时 NULL）
 */
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

/**
 * @brief 求多项式在 GF(2^m) 中的全部根：度 ≤4 解析求解，高次递归 BTZ 分解。
 * @param bch   BCH 控制句柄
 * @param k     当前 BTZ 分解的迹阶（根节点传 1，递归时递增）
 * @param poly  待求根多项式（高次时会被原地拆分）
 * @param roots 输出根数组
 * @return 找到的根数
 */
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

/**
 * @brief 解码并给出错误 bit 位置
 *
 * 无 syn 时先用 data 与 recv_ecc 的差算综合式；BM 求错误位置多项式，
 * 求根得错误位置。错误数 ≠ 根数，或根越界 ⇒ 不可纠。
 *
 * @param bch       BCH 控制句柄
 * @param data      数据段（calc_ecc 为 NULL 时使用）
 * @param len       数据字节数（须满足 8*len ≤ n - ecc_bits）
 * @param recv_ecc  读回的校验字（可 NULL）
 * @param calc_ecc  预算的校验字（可 NULL）
 * @param syn       直接给出的综合式（非 NULL 时忽略前三项）
 * @param errloc    输出：错误 bit 位置数组（容量 ≥ t）
 * @param out_nerr  输出：错误 bit 数
 * @return true 可纠（含 0 错）；false 不可纠
 */
bool fc_bch_decode(struct fc_bch_control *bch, const BYTE *data, unsigned int len, const BYTE *recv_ecc, const BYTE *calc_ecc, const unsigned int *syn, unsigned int *errloc, unsigned int *out_nerr)
{
    ASSERT(bch != NULL);
    ASSERT(out_nerr != NULL);
    const unsigned int ecc_words = BCH_ECC_WORDS(bch);
    unsigned int       nbits;
    unsigned int       nerr = 0U;
    int                i, nroots;
    DWORD              sum;

    ASSERT(8U * len <= (bch->n - bch->ecc_bits));

    if (syn == NULL)
    {
        if (calc_ecc == NULL)
        {
            ASSERT(data != NULL);
            ASSERT(recv_ecc != NULL);
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
            if (sum == 0)
            {
                *out_nerr = 0U;
                return true;
            }
        }
        compute_syndromes(bch, bch->ecc_buf, bch->syn);
        syn = bch->syn;
    }
    if (compute_error_locator_polynomial(bch, syn, &nerr) == false)
    {
        return false;
    }
    if (nerr > 0U)
    {
        nroots = find_poly_roots(bch, 1, bch->elp, errloc);
        if ((unsigned int)nroots != nerr)
        {
            return false;
        }
        nbits = len * 8U + bch->ecc_bits;
        for (i = 0; i < (int)nerr; i++)
        {
            if (errloc[i] >= nbits)
            {
                return false;
            }
            errloc[i] = nbits - 1 - errloc[i];
            if (bch->swap_bits == 0)
            {
                errloc[i] = (errloc[i] & ~7U) | (7U - (errloc[i] & 7U));
            }
        }
    }
    *out_nerr = nerr;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * 初始化（域表 / 生成多项式 / 查找表 / 度 2 基底）
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief 构建 GF(2^m) 对数/反对数表：x 左移递推，遇本原多项式归约
 * @param bch  BCH 控制句柄
 * @param poly 本原多项式
 * @return true  构建成功
 *         false 多项式次数与 m 不符，或 α 的周期不是 n（非本原多项式）
 */
static bool build_gf_tables(struct fc_bch_control *bch, unsigned int poly)
{
    unsigned int       i, x = 1;
    const unsigned int k = 1U << (unsigned int)deg(poly);
    if (k != (1U << GF_M(bch)))
    {
        return false;
    }
    for (i = 0; i < GF_N(bch); i++)
    {
        bch->a_pow_tab[i] = x;
        bch->a_log_tab[x] = i;
        if (i && (x == 1))
        {
            return false;
        }
        x <<= 1;
        if (x & k)
        {
            x ^= poly;
        }
    }
    bch->a_pow_tab[GF_N(bch)] = 1;
    bch->a_log_tab[0]         = 0;
    return true;
}

/**
 * @brief 构建 mod8 编码查找表：每输入字节偏移 8b，与生成多项式做多项式模除
 * @param bch BCH 控制句柄
 * @param g   生成多项式（32bit 大端字数组）
 */
static SSD_CODE_FAST void build_mod8_tables(struct fc_bch_control *bch, const DWORD *g)
{
    int       i, j, b, d;
    DWORD     data, hi, lo, *tab;
    const int l      = (int)BCH_ECC_WORDS(bch);
    const int plen   = (int)DIV_ROUND_UP(bch->ecc_bits + 1U, 32);
    const int ecclen = (int)DIV_ROUND_UP(bch->ecc_bits, 32);
    memset(bch->mod8_tab, 0, (4 * 256 * l) * sizeof(*bch->mod8_tab));
    for (i = 0; i < 256; i++)
    {
        for (b = 0; b < 4; b++)
        {
            tab  = bch->mod8_tab + ((b * 256 + i) * (unsigned int)l);
            data = i << (unsigned int)(8 * b);
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

/**
 * @brief 构建度 2 多项式求根用的迹映射表 xi_tab（二次方程判别式映射）
 * @param bch BCH 控制句柄
 * @return true 成功（m 个迹映射全部找到）；false 失败
 */
static bool build_deg2_base(struct fc_bch_control *bch)
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
            if (y && (r < m) && (xi[r] == 0))
            {
                bch->xi_tab[r] = x;
                xi[r]          = 1;
                remaining--;
                break;
            }
            y ^= ak;
        }
    }
    return remaining == 0U;
}

/**
 * @brief 计算 BCH 生成多项式 g(x)。
 * @param bch BCH 控制句柄
 * @return 指向静态 genpoly 的生成多项式（32bit 大端字数组）
 * @note g(x) 的根 = α^(2i+1)（i=0..t-1），共 2t 个连续奇次幂；
 *       乘积展开得系数，打包为 32bit 大端字。
 *       输出同时设 bch->ecc_bits = deg(g) = m·t。
 */
static DWORD *compute_generator_polynomial(struct fc_bch_control *bch)
{
    const unsigned int m = GF_M(bch), t = GF_T(bch);
    int                n;
    unsigned int       i, j, r, word;
    DWORD             *roots   = s_bch.roots;
    DWORD             *genpoly = s_bch.genpoly;
    struct gf_poly    *g       = (struct gf_poly *)s_bch.g_poly;

    // roots 用 bitmap：8192 个根只需 256 DWORD。
    memset(roots, 0, sizeof(s_bch.roots));

    for (i = 0; i < t; i++)
    {
        for (j = 0, r = 2U * i + 1U; j < m; j++)
        {
            roots[r >> 5U] |= 1UL << (r & 31U);
            r = (unsigned int)mod_s(bch, 2U * r);
        }
    }
    g->deg  = 0;
    g->c[0] = 1;
    for (i = 0; i < GF_N(bch); i++)
    {
        if ((roots[i >> 5U] & (1UL << (i & 31U))) != 0U)
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
    return genpoly;
}

/**
 * @brief 初始化 BCH 引擎（建域表 → 生成多项式 → mod8 表 → 度 2 基底）
 *
 * 表全部来自静态存储 s_bch，无堆分配；任一步失败即清零并返回 NULL。
 *
 * @param m         域阶（本工程固定 13）
 * @param t         纠错 bit 数（本工程固定 4）
 * @param prim_poly 本原多项式，0 表示按 m 取内置表
 * @param swap_bits 是否交换输入/输出位序
 * @return 引擎句柄；失败返回 NULL
 */
SSD_CODE_FAST fc_bch_control_t *fc_bch_init(int m, int t, unsigned int prim_poly, BYTE swap_bits)
{
    bool                      err = false;
    unsigned int              i;
    DWORD                    *genpoly;
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
    // 静态存储只覆盖本工程固定参数（m=13, t=4）。换参数时数组尺寸必须重算，
    // 不要靠运行时重新分配。
    if ((m != (int)FC_BCH_M) || (t != (int)FC_BCH_T))
    {
        ASSERT_MSG((m == (int)FC_BCH_M) && (t == (int)FC_BCH_T), "unsupported BCH params m=%d t=%d", m, t);
        return NULL;
    }
    if (prim_poly == 0)
    {
        prim_poly = prim_poly_tab[(unsigned int)(m - 5)];
    }
    memset(&s_bch, 0, sizeof(s_bch));
    bch = &s_bch.ctl;

    bch->m         = (unsigned int)m;
    bch->t         = (unsigned int)t;
    bch->n         = (unsigned int)((1 << m) - 1);
    bch->ecc_bytes = (unsigned int)DIV_ROUND_UP((unsigned int)(m * t), 8U);
    bch->swap_bits = swap_bits;
    bch->a_pow_tab = s_bch.a_pow;
    bch->a_log_tab = s_bch.a_log;
    bch->mod8_tab  = s_bch.mod8;
    bch->ecc_buf   = s_bch.ecc_buf;
    bch->ecc_buf2  = s_bch.ecc_buf2;
    bch->xi_tab    = s_bch.xi;
    bch->syn       = s_bch.syn;
    bch->cache     = s_bch.cache;
    bch->elp       = (struct gf_poly *)s_bch.elp;
    for (i = 0; i < 4; i++)
    {
        bch->poly_2t[i] = (struct gf_poly *)s_bch.poly_2t[i];
    }

    if (err == 0)
    {
        err = (build_gf_tables(bch, prim_poly) == false);
    }
    if (err == 0)
    {
        genpoly = compute_generator_polynomial(bch);
        if (genpoly)
        {
            build_mod8_tables(bch, genpoly);
        }
        else
        {
            err = true;
        }
    }
    if (err == 0)
    {
        err = (build_deg2_base(bch) == false);
    }
    if (err)
    {
        fc_bch_free(bch);
        return NULL;
    }
    return bch;
}

/**
 * @brief 释放 BCH 引擎（静态存储只清零，不归还堆）
 *
 * @param bch 引擎句柄（NULL 时无操作）
 */
void fc_bch_free(fc_bch_control_t *bch)
{
    if (bch == NULL)
    {
        return;
    }
    if (bch != &s_bch.ctl)
    {
        ASSERT_MSG(bch == &s_bch.ctl, "BCH free: non-singleton handle");
        return;
    }
    memset(&s_bch, 0, sizeof(s_bch));
}
