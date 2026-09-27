#ifndef FC_BCH_ENGINE_H
#define FC_BCH_ENGINE_H

#include "fc_types.h"

#include <stdint.h>

/*
 * BCH ECC 引擎。
 *
 * 移植自 Linux 内核 lib/bch.c（GPL v2），作者 Ivan Djelic。
 * 将内核依赖替换为标准 C（malloc/free），接口简化，
 * 参数固化减少动态分配。
 *
 * 典型参数（CW = 520B data + 8B parity）：
 *   m=13, t=4  → ecc_bytes=7（64bit parity 够用）
 *   m=8,  t=8  → ecc_bytes=8（正好填满 8B）
 */

typedef struct fc_bch_control fc_bch_control_t;

/*
 * 初始化 BCH 引擎。
 * @m:          Galois 域阶数 (5..15)
 * @t:          纠错能力 (bit)
 * @prim_poly:  本原多项式 (0=使用默认)
 * @swap_bits:  是否交换数据字节内的 bit 顺序
 * @return      BCH 控制句柄，失败返回 NULL
 */
fc_bch_control_t *fc_bch_init(int m, int t, unsigned int prim_poly, uint8_t swap_bits);

/* 释放 BCH 引擎资源 */
void fc_bch_free(fc_bch_control_t *bch);

/*
 * BCH 编码：计算 data 的 ECC 校验码。
 * @ecc 缓冲区大小应为 bch->ecc_bytes 字节。
 *       首次调用前必须清零，支持增量计算。
 */
void fc_bch_encode(fc_bch_control_t *bch, const uint8_t *data, unsigned int len, uint8_t *ecc);

/*
 * BCH 解码：检测并定位 data+recv_ecc 中的错误。
 * @errloc 输出错误位置数组（至少 t 个元素）。
 * @return  错误数量（≥0），失败返回 -1。
 *          对于返回的错误位置 errloc[i]：
 *            errloc[i] < 8*len → 数据区错误
 *            errloc[i] >= 8*len → ECC 区错误
 *          纠正：data[errloc/8] ^= (1 << (errloc%8))
 */
int fc_bch_decode(fc_bch_control_t *bch, const uint8_t *data, unsigned int len, const uint8_t *recv_ecc, const uint8_t *calc_ecc, const unsigned int *syn, unsigned int *errloc);

/* 辅助函数：获取 BCH 引擎的 ECC 字节数 */
unsigned int fc_bch_ecc_bytes(const fc_bch_control_t *bch);

/* 辅助函数：获取纠错能力 t */
unsigned int fc_bch_t(const fc_bch_control_t *bch);

#endif /* FC_BCH_ENGINE_H */