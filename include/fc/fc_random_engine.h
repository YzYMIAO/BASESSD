#ifndef FC_RANDOM_ENGINE_H
#define FC_RANDOM_ENGINE_H

#include "fc_types.h"

#include <stdint.h>

/*
 * NAND 加扰算法选择（编译期配置）
 *
 * FC_RANDOM_ALGO_LFSR  (默认)
 *   16 级线性反馈移位寄存器，多项式 x¹⁶ + x¹⁴ + x¹³ + x¹¹ + 1
 *   ONFI 标准加扰算法，消费级 SSD 最主流的方案，硬件实现只需要几个寄存器 + 异或门。
 *
 * FC_RANDOM_ALGO_XORSHIFT
 *   XOR-shift 32 三步递推：seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5
 *   周期 2³²-1，硬件开销极小，三星等厂商消费级方案同类。
 *
 * FC_RANDOM_ALGO_SIMPLE_XOR
 *   固定伪随机模式重复，低端方案。不需要种子递推，每 CW 用固定 pattern 异或。
 */
#define FC_RANDOM_ALGO_LFSR       (0U)
#define FC_RANDOM_ALGO_XORSHIFT   (1U)
#define FC_RANDOM_ALGO_SIMPLE_XOR (2U)

#ifndef FC_RANDOM_ALGO
#    define FC_RANDOM_ALGO FC_RANDOM_ALGO_LFSR
#endif

/*
 * 种子生成策略选择（编译期配置）
 *
 * FC_RANDOM_SEED_BASIC（默认）
 *   当前实现：program_sequence ^ PAA 各字段简单混合。
 *
 * FC_RANDOM_SEED_ENHANCED
 *   各字段独立左移到不同 bit 位置再 XOR，避免字段间 bit 重叠。
 *   加入黄金分割常数 * program_sequence 做扩散。
 *   加入 CW 维度，使同一 page 内不同 CW 有不同种子。
 *
 * FC_RANDOM_SEED_PROGRAM_SEQUENCE_WEIGHTED
 *   与 ENHANCED 结构相同，但 program_sequence 的影响权重更高
 *  （左移更多位 + 乘法），随写入次数增加 seed 变化更快。
 */
#define FC_RANDOM_SEED_BASIC                     (0U)
#define FC_RANDOM_SEED_ENHANCED                  (1U)
#define FC_RANDOM_SEED_PROGRAM_SEQUENCE_WEIGHTED (2U)

#ifndef FC_RANDOM_SEED_STRATEGY
#    define FC_RANDOM_SEED_STRATEGY FC_RANDOM_SEED_BASIC
#endif

/* 从 PAA 和 program_sequence 生成加扰种子 */
uint32_t fc_random_seed_from_paa(nand_paa_t paa, uint32_t program_sequence);

/*
 * 对 data[0..length-1] 进行加扰/解扰。
 * 加扰和解扰是同一操作（XOR 对称）。
 * 算法由 FC_RANDOM_ALGO 编译宏控制。
 */
fc_status_t fc_random_transform(uint8_t *data, uint16_t length, uint32_t seed);

/* 获取当前使用的加扰算法名称（调试用） */
const char *fc_random_algo_name(void);

/* 获取当前使用的种子策略名称（调试用） */
const char *fc_random_seed_strategy_name(void);

#endif /* FC_RANDOM_ENGINE_H */
