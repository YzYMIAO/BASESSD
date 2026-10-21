#include "fc_random_engine.h"

#include "assertion.h"
#include "nand_config.h"

#include <stddef.h>

/*
 * fc_random_engine.c — NAND 数据加扰/解扰引擎
 *
 * 加扰目的：让写入 NAND 的位串**统计独立**——P(bit=1 | 全部历史) = 1/2，即输出与
 * 随机数无法区分。由此间接避免长串 0/1 造成单元间干扰（Program Disturb / Read
 * Disturb）恶化，并均衡各页误码率。加扰与解扰是同一操作（XOR 对称）。
 *
 * ⚠ **目标是「独立」，不是「均衡」，更不是「尽量与上一位相反」**：
 *   均衡（0/1 各占一半）只是独立的一个副产品，`0101…` 同样满足均衡却**最不随机**
 *   （翻转率 100%、是固定周期图案，反而对 NAND 有害）。LFSR 的唯一设计目标就是造一条
 *   「给定历史、下一位仍是 1/2」的伪随机序列。理论见 fc_nand_design.md §5.4。
 *
 * 种子（v24）：**本 CW 的逻辑地址**（META 字段，扇区 LBA）——见 fc_random_seed_from_meta。
 * 算法：FC_RANDOM_ALGO（LFSR / XORSHIFT / SIMPLE_XOR）。
 */

/* ══════════════════════════════════════════════════════════════════
 * 种子生成（v24 收敛为唯一一种）
 * ══════════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_random_seed_from_meta
 *   功能：从本 CW 的逻辑地址（META 字段 = 扇区 LBA）生成加扰种子。
 *   实现要点：
 *     - **折叠 32 → 16 位**：下游 LFSR 状态取 `seed & 0xFFFF`，若不折叠，
 *       扇区 LBA 相差 65536（= 32 MB）的两页会撞种子，而器件有 1M 扇区。
 *     - 折叠后做雪崩混合（splitmix32 常量），让相邻 LBA 的种子充分分散
 *       （同一页内 4 条 CW 的 LBA 只差 1，必须散开）。
 *     - 非零保护：LFSR 全零是吸收态。
 *   与物理地址无关：GC 搬页后逻辑地址不变 ⇒ 密文不变（存储主控惯例）。
 * -------------------------------------------------------------------- */
DWORD fc_random_seed_from_meta(DWORD cw_meta)
{
    /* 折叠：高位并进低位，保住 32 位的全部信息 */
    DWORD seed = cw_meta ^ (cw_meta >> 16U);

    /* 雪崩混合（splitmix32 的乘法常量） */
    seed *= 0x9E3779B9UL;
    seed ^= seed >> 16U;
    seed *= 0x85EBCA6BUL;
    seed ^= seed >> 13U;

    return (seed != 0U) ? seed : 0x1U;
}

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

#if (FC_RANDOM_ALGO == FC_RANDOM_ALGO_LFSR)

/*
 * 16 级 LFSR，多项式 x¹⁶ + x¹⁴ + x¹³ + x¹¹ + 1，周期 2¹⁶ - 1 = 65535。
 *
 * 本实现是**右移** LFSR（输出 bit0，右移，反馈进 bit15）。此方向下反馈抽头是
 * 多项式系数的**镜像**：x¹⁶ → bit0、x¹⁴ → bit2、x¹³ → bit3、x¹¹ → bit5。
 *   fb = bit0 ^ bit2 ^ bit3 ^ bit5
 *
 * ⚠ 反馈必须包含**被移出的 bit0**。若照抄左移写法的 bit15^bit13^bit12^bit10，
 *   新状态 16 位全部只依赖旧 bit1..bit15 ⇒ 映射不可逆（1 → 0 是吸收态）⇒ 状态
 *   空间坍缩到周期 63，520B 的 CW 会退化成 63 字节图案重复 8.25 遍，加扰失效。
 */
#    define FC_LFSR_PERIOD (65535U)

/* 生成一个伪随机字节（8 bit 递推） */
static BYTE fc_lfsr_byte(DWORD *state)
{
    WORD lfsr = (*state & 0xFFFFU);
    BYTE i;
    BYTE result = 0U;

    for (i = 0U; i < 8U; i++)
    {
        result = ((result << 1U) | (lfsr & 1U));
        /* 反馈位 = bit0 ^ bit2 ^ bit3 ^ bit5（含被移出的 bit0） */
        {
            BYTE fb = ((lfsr ^ (lfsr >> 2U) ^ (lfsr >> 3U) ^ (lfsr >> 5U)) & 1U);

            lfsr = ((lfsr >> 1U) | (fb << 15U));
        }
    }
    *state = lfsr;
    return result;
}

/* --------------------------------------------------------------------
 * fc_random_transform_impl（LFSR 算法）
 *   功能：用 LFSR 流按字节异或 data（加扰=解扰）。
 *   实现要点：seed 低 16 位作初始状态（0 强制为 1，防全零卡死）；
 *         每字节 8 次移位递推取 8 bit。
 * -------------------------------------------------------------------- */
static fc_status_e fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    WORD index;

    /* data 前置条件由调用者保证（fc_data_path 传入 CW 缓冲） */
    ASSERT(data != NULL);

    /* 用 seed 初始化为非零 LFSR 状态 */
    DWORD lfsr_state = (seed & 0xFFFFU);
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

/* --------------------------------------------------------------------
 * fc_random_transform_impl（SIMPLE_XOR 算法）
 *   功能：用 seed 生成 64B 伪随机 pattern，循环异或 data。
 *   实现要点：pattern 由 XOR-shift 递推生成；性能开销最小。
 * -------------------------------------------------------------------- */
static fc_status_e fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    BYTE pattern[FC_SIMPLE_XOR_PATTERN_LEN];
    BYTE i;
    WORD index;

    /* data 前置条件由调用者保证（fc_data_path 传入 CW 缓冲） */
    ASSERT(data != NULL);

    /* 用 seed 生成 64 字节 pattern */
    for (i = 0U; i < FC_SIMPLE_XOR_PATTERN_LEN; i++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        pattern[i] = seed;
    }

    /* 用 pattern 循环异或数据 */
    for (index = 0U; index < length; index++)
    {
        data[index] ^= pattern[index % FC_SIMPLE_XOR_PATTERN_LEN];
    }
    return FC_STATUS_OK;
}

#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_XORSHIFT)

/* --------------------------------------------------------------------
 * fc_random_transform_impl（XORSHIFT 算法）
 *   功能：XOR-shift 32 三步递推流按字节异或 data。
 *   实现要点：seed ^= seed<<13; ^= seed>>17; ^= seed<<5（周期 2^32-1）；
 *         seed 为 0 时无意义（返回 INVALID_ARGUMENT）。
 * -------------------------------------------------------------------- */
static fc_status_e fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    WORD index;

    /* data 前置条件由调用者保证；seed 由 fc_random_seed_from_meta 保证非零 */
    ASSERT(data != NULL);
    ASSERT(seed != 0U);

    for (index = 0U; index < length; index++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        data[index] ^= seed;
    }
    return FC_STATUS_OK;
}

#endif

/* ══════════════════════════════════════════════════════════════════
 * 公接口
 * ══════════════════════════════════════════════════════════════════ */

/* 加扰/解扰（XOR 对称；算法由编译宏决定） */
fc_status_e fc_random_transform(BYTE *data, WORD length, DWORD seed)
{
    return fc_random_transform_impl(data, length, seed);
}

/* 当前算法名（调试用） */
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
