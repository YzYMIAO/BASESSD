#include "fc_random_engine.h"

#include "nand/nand_config.h"

#include <stddef.h>

/* ══════════════════════════════════════════════════════════════════
 * 种子生成策略
 * ══════════════════════════════════════════════════════════════════ */

#if (FC_RANDOM_SEED_STRATEGY == FC_RANDOM_SEED_ENHANCED)

static uint32_t fc_random_seed_from_paa_impl(nand_paa_t paa, uint32_t program_sequence)
{
    /*
     * 增强种子：各字段独立左移到不同 bit 位置再 XOR，避免字段间 bit 重叠。
     * 加入黄金分割常数 * program_sequence 做扩散。
     * 加入 CW 维度，使同一 page 内不同 CW 有不同种子。
     */
    const uint32_t golden_ratio = 0x9E3779B9UL;

    uint32_t seed = golden_ratio;

    seed ^= program_sequence * golden_ratio;
    seed ^= (uint32_t)paa_to_lun(paa) << 31U;
    seed ^= (uint32_t)paa_to_plane(paa) << 23U;
    seed ^= (uint32_t)paa_to_block(paa) << 7U;
    seed ^= (uint32_t)paa_to_page(paa);
    seed ^= (uint32_t)paa_to_cw(paa) << 28U;

    return (seed != 0U) ? seed : 0x1U;
}

#elif (FC_RANDOM_SEED_STRATEGY == FC_RANDOM_SEED_PROGRAM_SEQUENCE_WEIGHTED)

static uint32_t fc_random_seed_from_paa_impl(nand_paa_t paa, uint32_t program_sequence)
{
    /*
     * 高权重写入序列种子：program_sequence 的影响权重更高，
     * 随写入次数增加 seed 变化更快，适用于高耐久场景。
     */
    const uint32_t golden_ratio = 0x9E3779B9UL;

    uint32_t seed = golden_ratio;

    seed ^= program_sequence * 0xDEADBEEFUL; /* 高权重 */
    seed ^= (program_sequence >> 16U) * golden_ratio;
    seed ^= (uint32_t)paa_to_lun(paa) << 31U;
    seed ^= (uint32_t)paa_to_plane(paa) << 23U;
    seed ^= (uint32_t)paa_to_block(paa) << 1U;
    seed ^= (uint32_t)paa_to_page(paa) << 8U;
    seed ^= (uint32_t)paa_to_cw(paa) << 28U;

    return (seed != 0U) ? seed : 0x1U;
}

#else /* FC_RANDOM_SEED_BASIC (默认) */

static uint32_t fc_random_seed_from_paa_impl(nand_paa_t paa, uint32_t program_sequence)
{
    uint32_t seed;

    seed = program_sequence ^ (paa_to_lun(paa) << 16U) ^ paa;
    seed ^= (paa_to_plane(paa) << 14U) ^ (paa_to_block(paa) << 2U) ^ paa_to_page(paa);
    return (seed != 0U) ? seed : 0x1U;
}

#endif

/* ══════════════════════════════════════════════════════════════════
 * 加扰算法
 *
 * 三种算法差异对比：
 *
 * 算法        │ 核心机制            │ 周期       │ 每字节开销 │ 行业对标
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * XOR-shift  │ seed ^= seed<<a   │ 2³²-1     │ ~6 ops   │ 三星等消费级同类
 *            │        ^= seed>>b │           │          │
 *            │        ^= seed<<c │           │          │
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * LFSR 16    │ x¹⁶+x¹⁴+x¹³+x¹¹+1│ 2¹⁶-1     │ ~40 ops  │ ONFI 标准多项式
 *            │ 串行逐 bit 递推    │ (65535)   │ (逐bit)  │ 消费/企业级最主流
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * Simple XOR │ 固定 64B pattern  │ pattern   │ ~2 ops   │ 低端方案，资源受限
 *            │ 循环异或          │ 长度=64   │          │
 *
 * 注意：企业级与消费级在加扰算法上没有本质区别，
 * 都使用 LFSR 或 XOR-shift。真正的区别在于：
 * ① 企业级强制 AES-256 XTS 加密（加密后数据天然随机化）
 * ② 实现方式：企业级在 DMA 引擎中硬件 inline 完成，零 CPU 开销
 * ③ 种子维度：企业级可能混入更多维度（Die ID 等）
 * ══════════════════════════════════════════════════════════════════ */

#if (FC_RANDOM_ALGO == FC_RANDOM_ALGO_LFSR)

/*
 * 16 级 LFSR，多项式 x¹⁶ + x¹⁴ + x¹³ + x¹¹ + 1
 * 反馈位 = bit15 ^ bit13 ^ bit12 ^ bit10（0-indexed）
 * 周期 2¹⁶ - 1 = 65535
 */
#    define FC_LFSR_PERIOD (65535U)

static uint8_t fc_lfsr_byte(uint32_t *state)
{
    uint16_t lfsr = (uint16_t)(*state & 0xFFFFU);
    uint8_t  i;
    uint8_t  result = 0U;

    for (i = 0U; i < 8U; i++)
    {
        result = (uint8_t)((result << 1U) | (uint8_t)(lfsr & 1U));
        /* 反馈位 = bit15 ^ bit13 ^ bit12 ^ bit10 */
        {
            uint8_t fb = (uint8_t)(((lfsr >> 15U) ^ (lfsr >> 13U) ^
                                    (lfsr >> 12U) ^ (lfsr >> 10U)) &
                                   1U);
            lfsr       = (uint16_t)((lfsr >> 1U) | ((uint16_t)fb << 15U));
        }
    }
    *state = lfsr;
    return result;
}

static fc_status_t fc_random_transform_impl(uint8_t *data, uint16_t length, uint32_t seed)
{
    uint16_t index;

    if (data == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* 用 seed 初始化为非零 LFSR 状态 */
    uint32_t lfsr_state = (seed & 0xFFFFU);
    if (lfsr_state == 0U)
    {
        lfsr_state = 1U;
    }

    for (index = 0U; index < length; index++)
    {
        data[index] ^= fc_lfsr_byte(&lfsr_state);
    }
    return FC_STATUS_OK;
}

#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_SIMPLE_XOR)

/*
 * 简单 XOR：用 seed 生成固定长度的伪随机 pattern，然后按字节异或。
 * 每 length 字节重复 pattern 绕回。
 * 适合资源极度受限的场景。
 */
#    define FC_SIMPLE_XOR_PATTERN_LEN (64U)

static fc_status_t fc_random_transform_impl(uint8_t *data, uint16_t length, uint32_t seed)
{
    uint8_t  pattern[FC_SIMPLE_XOR_PATTERN_LEN];
    uint8_t  i;
    uint16_t index;

    if (data == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* 用 seed 生成 64 字节 pattern */
    for (i = 0U; i < FC_SIMPLE_XOR_PATTERN_LEN; i++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        pattern[i] = (uint8_t)seed;
    }

    /* 用 pattern 循环异或数据 */
    for (index = 0U; index < length; index++)
    {
        data[index] ^= pattern[index % FC_SIMPLE_XOR_PATTERN_LEN];
    }
    return FC_STATUS_OK;
}

#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_XORSHIFT)

static fc_status_t fc_random_transform_impl(uint8_t *data, uint16_t length, uint32_t seed)
{
    uint16_t index;

    if ((data == NULL) || (seed == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < length; index++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        data[index] ^= (uint8_t)seed;
    }
    return FC_STATUS_OK;
}

#endif

/* ══════════════════════════════════════════════════════════════════
 * 公接口
 * ══════════════════════════════════════════════════════════════════ */

uint32_t fc_random_seed_from_paa(nand_paa_t paa, uint32_t program_sequence)
{
    return fc_random_seed_from_paa_impl(paa, program_sequence);
}

fc_status_t fc_random_transform(uint8_t *data, uint16_t length, uint32_t seed)
{
    return fc_random_transform_impl(data, length, seed);
}

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

const char *fc_random_seed_strategy_name(void)
{
#if (FC_RANDOM_SEED_STRATEGY == FC_RANDOM_SEED_BASIC)
    return "Basic (program_sequence ^ PAA direct)";
#elif (FC_RANDOM_SEED_STRATEGY == FC_RANDOM_SEED_ENHANCED)
    return "Enhanced (bit-isolated fields + golden-ratio diffusion)";
#elif (FC_RANDOM_SEED_STRATEGY == FC_RANDOM_SEED_PROGRAM_SEQUENCE_WEIGHTED)
    return "Program-sequence-weighted (high-weight program_sequence)";
#else
    return "Unknown";
#endif
}
