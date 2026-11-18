#ifndef FC_RANDOM_ENGINE_H
#define FC_RANDOM_ENGINE_H

#include "ssd_types.h" /* BYTE/WORD/DWORD */

#include <stdint.h>

/*
 * fc_random_engine.h — NAND 加扰/解扰引擎
 *
 * 加扰目标：让输出**统计独立**（P(bit=1 | 全部历史) = 1/2），与随机数无法区分。
 * ⚠ 不是「0/1 各占一半」的均衡（`0101…` 也均衡却最不随机），更不是「与上一位相反」。
 *   理论见 docs/design/fc_nand_design.md §5.4。
 *
 * NAND 加扰算法选择（编译期配置）
 *
 * FC_RANDOM_ALGO_LFSR  (默认)
 *   16 级线性反馈移位寄存器，多项式 x¹⁶ + x¹⁴ + x¹³ + x¹¹ + 1（最大长度，周期 65535）。
 *   主控加扰最主流的方案，硬件实现只需几个寄存器 + 异或门。
 *   ⚠ 本多项式是本工程的选择，**不是 ONFI 规定**——ONFI 不规定加扰器。
 *
 * FC_RANDOM_ALGO_XORSHIFT
 *   XOR-shift 32 三步递推：seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5
 *   周期 2³²-1，硬件开销极小。
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

/**
 * @brief 加扰种子：**本 CW 的逻辑地址**，即该 CW 的 META 字段（扇区 LBA）。
 *
 * 为什么是逻辑地址而不是物理地址：GC 搬页后逻辑地址不变 ⇒ 密文不变，
 * 不需要「解扰再按新物理地址重加扰」。这是存储主控的通行做法
 * （见 docs/design/fc_nand_design.md §5.4 的出处）。
 *
 * 粒度 = 被寻址逻辑单元：本工程一个 CW（512B 用户数据）= 一个扇区 = 一个逻辑单元，
 * 故每条 CW 用自己那条扇区 LBA 当种子。
 *
 * ⚠ 调用方（fc_data_path.c 的 CW 布局包装）必须传**该 CW 的扇区 LBA**
 * （desc->meta / 从页内读回的明文 META），而不是 command->meta（那是本命令的页级基值）。
 *
 * @param cw_meta 本 CW 的 META 字段（扇区 LBA）
 * @return        非零的 32 位加扰种子
 */
DWORD fc_random_seed_from_meta(DWORD cw_meta);

/**
 * @brief 对 data[0..length-1] 进行加扰/解扰。
 * 加扰和解扰是同一操作（XOR 对称）。
 * 算法由 FC_RANDOM_ALGO 编译宏控制。
 *
 * @param data   待变换的数据缓冲（原地修改）
 * @param length 数据字节数
 * @param seed   本 CW 的加扰种子（fc_random_seed_from_meta 的输出）
 */
void fc_random_transform(BYTE *data, WORD length, DWORD seed);

/**
 * @brief 获取当前使用的加扰算法名称（调试用）。
 *
 * @return 静态字符串，内容由 FC_RANDOM_ALGO 决定
 */
const char *fc_random_algo_name(void);

#endif /* FC_RANDOM_ENGINE_H */
