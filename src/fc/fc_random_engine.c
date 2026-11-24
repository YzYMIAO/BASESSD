#include "fc_random_engine.h"

#include "assertion.h"

#include <stddef.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
 * 加扰算法
 *
 * 三种算法差异对比：
 *
 * 算法        │ 核心机制            │ 周期       │ 每字节开销 │ 备注
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * XOR-shift  │ seed ^= seed<<a   │ 2³²-1     │ ~6 ops   │ 硬件开销极小
 *            │        ^= seed>>b │           │          │
 *            │        ^= seed<<c │           │          │
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * LFSR 16    │ x¹⁶+x¹⁴+x¹³+x¹¹+1│ 2¹⁶-1     │ ~40 ops  │ 主控最主流
 *            │ 串行逐 bit 递推    │ (65535)   │ (逐bit)  │（多项式是本工程选择，
 *            │                   │           │          │  ONFI 不规定加扰器）
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * Simple XOR │ 固定 64B pattern  │ pattern   │ ~2 ops   │ 低端方案，资源受限
 *            │ 循环异或          │ 长度=64   │          │
 *
 * 注意：企业级与消费级在加扰算法上没有本质区别，都使用 LFSR 或 XOR-shift。
 * 真正的区别在于：
 * ① 企业级强制 AES-256 XTS 加密（加密后数据天然随机化）
 * ② 实现方式：企业级在 DMA 引擎中硬件 inline 完成，零 CPU 开销
 * ③ 种子维度：企业级可能混入更多维度（Die ID 等）
 * ══════════════════════════════════════════════════════════════════ */

#define FC_SIMPLE_XOR_PATTERN_LEN (64U) /* SIMPLE_XOR 分支使用；提前定义以满足 H51 */

#if (FC_RANDOM_ALGO == FC_RANDOM_ALGO_LFSR)

// 16 级 LFSR，多项式 x¹⁶ + x¹⁴ + x¹³ + x¹¹ + 1，周期 2¹⁶ - 1 = 65535。
//
// 本实现是**右移** LFSR（输出 bit0，右移，反馈进 bit15）。此方向下反馈抽头是
// 多项式系数的**镜像**：x¹⁶ → bit0、x¹⁴ → bit2、x¹³ → bit3、x¹¹ → bit5。
// fb = bit0 ^ bit2 ^ bit3 ^ bit5
//
// ⚠ 反馈必须包含**被移出的 bit0**。若照抄左移写法的 bit15^bit13^bit12^bit10，
// 新状态 16 位全部只依赖旧 bit1..bit15 ⇒ 映射不可逆（1 → 0 是吸收态）⇒ 状态
// 空间坍缩到周期 63，520B 的 CW 会退化成 63 字节图案重复 8.25 遍，加扰失效。
#    define FC_LFSR_PERIOD         (65535U)
#    define FC_LFSR_TABLE_SIZE     (256U)
#    define FC_LFSR_STEPS_PER_WORD (4U)

// 16 位状态 → 8 步输出/次态是 GF(2) 线性变换 ⇒ 可按低/高字节分解：
// out  = out_lo[lo]  ^ out_hi[hi]
// next = next_lo[lo] ^ next_hi[hi]
// 每字节 2 次查表 + 2 次异或，替代 8 次移位。
//
// 4 步展开利用同一线性性：一次生成 32 bit 掩码 + 4 步后的 16 bit 次态。
// 每 4 字节只需 out4_lo/out4_hi + next4_lo/next4_hi 4 次查表，替代
// 4 次单步查表；尾部/非对齐输入仍走单步表。
typedef struct FC_LFSR_TABLE_S
{
    BYTE  out_lo[FC_LFSR_TABLE_SIZE];
    BYTE  out_hi[FC_LFSR_TABLE_SIZE];
    WORD  next_lo[FC_LFSR_TABLE_SIZE];
    WORD  next_hi[FC_LFSR_TABLE_SIZE];
    DWORD out4_lo[FC_LFSR_TABLE_SIZE];
    DWORD out4_hi[FC_LFSR_TABLE_SIZE];
    WORD  next4_lo[FC_LFSR_TABLE_SIZE];
    WORD  next4_hi[FC_LFSR_TABLE_SIZE];
} fc_lfsr_tab_s;

static fc_lfsr_tab_s s_lfsr_tab;
static BOOL          s_lfsr_tab_ready = false;

/**
 * @brief 8 步递推：输出 8 bit，返回次态。与逐 bit 版本完全一致。
 *
 * @param lfsr 当前 16 位 LFSR 状态
 * @param out  输出：本步产生的 8 bit（逐位移入）
 * @return     递推 8 步后的次态
 */
static WORD fc_lfsr_step8(WORD lfsr, BYTE *out)
{
    BYTE i;
    BYTE result = 0U;

    for (i = 0U; i < 8U; i++)
    {
        result = (BYTE)((result << 1U) | (lfsr & 1U));

        // 反馈位 = bit0 ^ bit2 ^ bit3 ^ bit5（含被移出的 bit0）
        const BYTE fb = (BYTE)((lfsr ^ (lfsr >> 2U) ^ (lfsr >> 3U) ^ (lfsr >> 5U)) & 1U);

        lfsr = (WORD)((lfsr >> 1U) | ((WORD)fb << 15U));
    }

    *out = result;
    return lfsr;
}

/**
 * @brief 构造 LFSR 的单步与 4 步查表（低/高字节各一组）。
 *
 * 单步表用于尾部/非对齐输入；4 步表用于 32-bit 数据 XOR。表建立后置
 * s_lfsr_tab_ready，后续调用直接复用。
 */
static void fc_lfsr_tab_build(void)
{
    for (WORD v = 0U; v < FC_LFSR_TABLE_SIZE; v++)
    {
        BYTE out[FC_LFSR_STEPS_PER_WORD];
        WORD state;

        s_lfsr_tab.next_lo[v] = fc_lfsr_step8((WORD)v, &s_lfsr_tab.out_lo[v]);
        s_lfsr_tab.next_hi[v] = fc_lfsr_step8((WORD)(v << 8U), &s_lfsr_tab.out_hi[v]);

        state = (WORD)v;
        for (BYTE k = 0U; k < FC_LFSR_STEPS_PER_WORD; k++)
        {
            state = fc_lfsr_step8(state, &out[k]);
        }
        s_lfsr_tab.out4_lo[v]  = (DWORD)out[0] | ((DWORD)out[1] << 8U) | ((DWORD)out[2] << 16U) | ((DWORD)out[3] << 24U);
        s_lfsr_tab.next4_lo[v] = state;

        state = (WORD)(v << 8U);
        for (BYTE k = 0U; k < FC_LFSR_STEPS_PER_WORD; k++)
        {
            state = fc_lfsr_step8(state, &out[k]);
        }
        s_lfsr_tab.out4_hi[v]  = (DWORD)out[0] | ((DWORD)out[1] << 8U) | ((DWORD)out[2] << 16U) | ((DWORD)out[3] << 24U);
        s_lfsr_tab.next4_hi[v] = state;
    }

    s_lfsr_tab_ready = true;
}

#    ifndef NDEBUG
/**
 * @brief 一次性自检：查表路径必须与逐 bit 递推给出完全相同的 64 字节。
 */
static void fc_lfsr_tab_selfcheck(void)
{
    static const WORD seeds[] = {1U, 0x1234U, 0xFFFFU};

    for (BYTE s = 0U; s < (BYTE)(sizeof(seeds) / sizeof(seeds[0])); s++)
    {
        WORD tab_state = seeds[s];
        WORD ref_state = seeds[s];

        for (BYTE i = 0U; i < 64U; i++)
        {
            const WORD lo      = (WORD)(tab_state & 0xFFU);
            const WORD hi      = (WORD)(tab_state >> 8U);
            const BYTE tab_out = (BYTE)(s_lfsr_tab.out_lo[lo] ^ s_lfsr_tab.out_hi[hi]);

            tab_state = (WORD)(s_lfsr_tab.next_lo[lo] ^ s_lfsr_tab.next_hi[hi]);

            BYTE ref_out = 0U;
            ref_state    = fc_lfsr_step8(ref_state, &ref_out);

            ASSERT(tab_out == ref_out);
            ASSERT(tab_state == ref_state);
        }

        {
            WORD tab4_state = seeds[s];
            WORD ref4_state = seeds[s];

            for (BYTE i = 0U; i < 16U; i++)
            {
                const WORD  lo       = (WORD)(tab4_state & 0xFFU);
                const WORD  hi       = (WORD)(tab4_state >> 8U);
                const DWORD mask     = s_lfsr_tab.out4_lo[lo] ^ s_lfsr_tab.out4_hi[hi];
                const WORD  next     = (WORD)(s_lfsr_tab.next4_lo[lo] ^ s_lfsr_tab.next4_hi[hi]);
                DWORD       ref_mask = 0U;

                for (BYTE k = 0U; k < FC_LFSR_STEPS_PER_WORD; k++)
                {
                    BYTE ref_out = 0U;
                    ref4_state   = fc_lfsr_step8(ref4_state, &ref_out);
                    ref_mask |= (DWORD)ref_out << (8U * k);
                }
                ASSERT(mask == ref_mask);
                ASSERT(next == ref4_state);
                tab4_state = next;
            }
        }
    }
}
#    endif

/**
 * @brief 一次取 4 个伪随机字节的 32-bit 掩码并推进 4 步。
 * @param state 输入输出：LFSR 状态，函数内推进到 4 步后的次态
 * @return      低字节到高字节依次对应连续 4 个输出字节
 */
static DWORD fc_lfsr_next4(WORD *state)
{
    const WORD  lo   = (WORD)(*state & 0xFFU);
    const WORD  hi   = (WORD)(*state >> 8U);
    const DWORD mask = s_lfsr_tab.out4_lo[lo] ^ s_lfsr_tab.out4_hi[hi];

    *state = (WORD)(s_lfsr_tab.next4_lo[lo] ^ s_lfsr_tab.next4_hi[hi]);
    return mask;
}

/**
 * @brief 取下一个伪随机字节（查表版；调用前表已建立）。
 *
 * @param state 输入输出：LFSR 状态，函数内推进到次态
 * @return      本步产生的伪随机字节（用作异或掩码）
 */
static BYTE fc_lfsr_next(WORD *state)
{
    const WORD lo  = (WORD)(*state & 0xFFU);
    const WORD hi  = (WORD)(*state >> 8U);
    const BYTE out = (BYTE)(s_lfsr_tab.out_lo[lo] ^ s_lfsr_tab.out_hi[hi]);

    *state = (WORD)(s_lfsr_tab.next_lo[lo] ^ s_lfsr_tab.next_hi[hi]);
    return out;
}

/**
 * @brief 用 16 级 LFSR 对 data 原地加扰/解扰。
 *
 * 首次调用时建立查表，debug 档另做一次查表与逐 bit 递推的等价自检。
 *
 * @param data   待变换的数据缓冲（原地修改）
 * @param length 数据字节数
 * @param seed   加扰种子；低 16 位作 LFSR 初态，为 0 时退化为 1
 */
static void fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    // data 前置条件由调用者保证（fc_data_path_scramble 传入 CW 缓冲）
    ASSERT(data != NULL);

    if (s_lfsr_tab_ready == false)
    {
        fc_lfsr_tab_build();
#    ifndef NDEBUG
        fc_lfsr_tab_selfcheck();
#    endif
    }

    // 用 seed 初始化为非零 LFSR 状态
    WORD lfsr_state = (WORD)(seed & 0xFFFFU);
    if (lfsr_state == 0U)
    {
        lfsr_state = 1U;
    }

    WORD index = 0U;

    // 4 字节一组：4 步查表直接给出 32-bit 掩码与次态，数据按 32-bit word XOR。
    // 只有 data 4 字节对齐时走该路径；未对齐输入保留逐字节回退。
    if (((uintptr_t)data & 3U) == 0U)
    {
        while ((DWORD)index + 3U < (DWORD)length)
        {
            const DWORD mask = fc_lfsr_next4(&lfsr_state);
            DWORD       word;

            memcpy(&word, data + index, sizeof(word));
            word ^= mask;
            memcpy(data + index, &word, sizeof(word));
            index = (WORD)(index + 4U);
        }
    }

    while (index < length)
    {
        data[index] ^= fc_lfsr_next(&lfsr_state);
        index++;
    }
}

#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_SIMPLE_XOR)

// 简单 XOR：用 seed 生成固定长度的伪随机 pattern，然后按字节异或。
// 每 length 字节重复 pattern 绕回。
// 适合资源极度受限的场景。

/**
 * @brief 用 64 字节伪随机 pattern 循环异或 data。
 *
 * @param data   待变换的数据缓冲（原地修改）
 * @param length 数据字节数
 * @param seed   加扰种子，作为 XOR-shift 递推初值生成 pattern
 */
static void fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    BYTE pattern[FC_SIMPLE_XOR_PATTERN_LEN];
    BYTE i;
    WORD index;

    // data 前置条件由调用者保证（fc_data_path_scramble 传入 CW 缓冲）
    ASSERT(data != NULL);

    // 用 seed 生成 64 字节 pattern
    for (i = 0U; i < FC_SIMPLE_XOR_PATTERN_LEN; i++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        pattern[i] = seed;
    }

    // 用 pattern 循环异或数据
    for (index = 0U; index < length; index++)
    {
        data[index] ^= pattern[index % FC_SIMPLE_XOR_PATTERN_LEN];
    }
}

#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_XORSHIFT)

/**
 * @brief 用 32 位 XOR-shift 递推逐字节异或 data。
 *
 * @param data   待变换的数据缓冲（原地修改）
 * @param length 数据字节数
 * @param seed   加扰种子，作为 XOR-shift 递推初值（调用者保证非零）
 */
static void fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    WORD index;

    // data 前置条件由调用者保证；seed 由 fc_random_seed_from_meta 保证非零
    ASSERT(data != NULL);
    ASSERT(seed != 0U);

    for (index = 0U; index < length; index++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        data[index] ^= seed;
    }
}

#endif

/* ══════════════════════════════════════════════════════════
 * 加扰/解扰引擎
 * ══════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════
 * 种子生成（唯一一种）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 由 CW 的 META 派生加扰种子
 *
 * 32→16 位折叠避免相差 64K 扇区的 LBA 撞种子，再做雪崩混合让相邻 LBA 充分
 * 分散。与物理地址无关：GC 搬页后逻辑地址不变 ⇒ 密文不变。
 *
 * @param cw_meta 本 CW 的 META（逻辑地址）
 * @return 非零 32 位种子
 */
DWORD fc_random_seed_from_meta(DWORD cw_meta)
{
    // 折叠：高位并进低位，保住 32 位的全部信息
    DWORD seed = cw_meta ^ (cw_meta >> 16U);

    // 雪崩混合（splitmix32 的乘法常量）
    seed *= 0x9E3779B9UL;
    seed ^= seed >> 16U;
    seed *= 0x85EBCA6BUL;
    seed ^= seed >> 13U;

    return (seed != 0U) ? seed : 0x1U;
}

/* ══════════════════════════════════════════════════════════════════
 * 公接口
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 加扰/解扰（XOR 对称，算法由编译宏决定）
 *
 * @param data   数据缓冲（原地修改）
 * @param length 字节数
 * @param seed   fc_random_seed_from_meta 给出的种子
 */
void fc_random_transform(BYTE *data, WORD length, DWORD seed)
{
    fc_random_transform_impl(data, length, seed);
}

/**
 * @brief 当前编译出的加扰算法名
 *
 * @return 算法名（调试输出用）
 */
const char *fc_random_algo_name(void)
{
#if (FC_RANDOM_ALGO == FC_RANDOM_ALGO_XORSHIFT)
    return "XOR-shift 32";
#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_LFSR)
    return "LFSR 16 (x^16+x^14+x^13+x^11+1)";
#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_SIMPLE_XOR)
    return "Simple XOR (64-byte pattern)";
#else
    return "Unknown";
#endif
}
