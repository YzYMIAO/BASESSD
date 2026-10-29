#ifndef FC_BCH_ENGINE_H
#define FC_BCH_ENGINE_H

#include "fc_types.h"
#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdbool.h>
#include <stdint.h>

/*
 * BCH ECC 引擎。
 *
 * 移植自 Linux 内核 lib/bch.c（GPL v2），作者 Ivan Djelic。
 * 将内核依赖替换为标准 C；本工程参数固定，表存于静态结构体，不使用堆。
 *
 * 典型参数（CW = 520B data + 8B parity）：
 *   m=13, t=4  → ecc_bytes=7（64bit parity 够用）
 *   m=8,  t=8  → ecc_bytes=8（正好填满 8B）
 */

typedef struct fc_bch_control fc_bch_control_t;

/**
 * @brief 初始化 BCH 引擎。
 * @m:          Galois 域阶数 (5..15)
 * @t:          纠错能力 (bit)
 * @prim_poly:  本原多项式 (0=使用默认)
 * @swap_bits:  是否交换数据字节内的 bit 顺序
 * @return      BCH 控制句柄，失败返回 NULL
 */
fc_bch_control_t *fc_bch_init(int m, int t, unsigned int prim_poly, BYTE swap_bits);

/** @brief 释放 BCH 引擎资源 */
void fc_bch_free(fc_bch_control_t *bch);

/**
 * @brief BCH 编码：计算 data 的 ECC 校验码。
 * @ecc 缓冲区大小应为 bch->ecc_bytes 字节。
 * 首次调用前必须清零，支持增量计算。
 * ecc 可为 NULL（增量计算语义）。
 * bch / data 非空为前置条件（违反一律 ASSERT）。
 * /
 */
void fc_bch_encode(fc_bch_control_t *bch, const BYTE *data, unsigned int len, BYTE *ecc);

/**
 * @brief BCH 解码：检测并定位 data+recv_ecc 中的错误。
 * @errloc 输出错误位置数组（至少 t 个元素）。
 * @return true = 解码完成（*out_nerr 为纠正的错误数，0..t）
 *         false = 不可纠。
 *          入参前置违反（len 超码长 / data·recv_ecc 为空）一律 ASSERT
 *          （见 assertion_guidelines.md §十五）。
 *          对于返回的错误位置 errloc[i]：
 *            errloc[i] < 8*len → 数据区错误
 *            errloc[i] >= 8*len → ECC 区错误
 *          纠正：data[errloc/8] ^= (1 << (errloc%8))
 */
bool fc_bch_decode(fc_bch_control_t *bch, const BYTE *data, unsigned int len, const BYTE *recv_ecc, const BYTE *calc_ecc, const unsigned int *syn, unsigned int *errloc, unsigned int *out_nerr);

/** @brief 辅助函数：获取 BCH 引擎的 ECC 字节数 */
unsigned int fc_bch_ecc_bytes(const fc_bch_control_t *bch);

/** @brief 辅助函数：获取纠错能力 t */
unsigned int fc_bch_t(const fc_bch_control_t *bch);

#endif /* FC_BCH_ENGINE_H */