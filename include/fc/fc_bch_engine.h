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
 * @param m         Galois 域阶数 (5..15)
 * @param t         纠错能力 (bit)
 * @param prim_poly 本原多项式 (0=使用默认)
 * @param swap_bits 是否交换数据字节内的 bit 顺序
 * @return BCH 控制句柄，失败返回 NULL
 * @note 本工程静态存储只覆盖 m=13 / t=4；其他参数违反前置条件
 *       （debug 下 ASSERT，release 下返回 NULL）。
 */
fc_bch_control_t *fc_bch_init(int m, int t, unsigned int prim_poly, BYTE swap_bits);

/**
 * @brief 释放 BCH 引擎资源。
 * @param bch BCH 控制句柄（可为 NULL）
 */
void fc_bch_free(fc_bch_control_t *bch);

/**
 * @brief BCH 编码：计算 data 的 ECC 校验码。
 * @param bch  BCH 控制句柄
 * @param data 输入数据
 * @param len  数据字节数
 * @param ecc  ECC 缓冲区（ecc_bytes 字节）；为 NULL 时以全零状态计算，结果只留在内部工作区
 * @note 支持增量计算：把上一段输出的 ecc 缓冲区继续作为本段输入传入；首次调用前该缓冲区必须清零。
 * @note bch / data 为空违反前置条件，一律 ASSERT。
 */
void fc_bch_encode(fc_bch_control_t *bch, const BYTE *data, unsigned int len, BYTE *ecc);

/**
 * @brief BCH 解码：检测并定位 data+recv_ecc 中的错误。
 * @param bch      BCH 控制句柄
 * @param data     数据区（len 字节）
 * @param len      数据字节数
 * @param recv_ecc 接收到的 ECC；为 NULL 时不与预期校验比对
 * @param calc_ecc 预先算好的 ECC；为 NULL 时现场编码 data 得到预期校验（此时 recv_ecc 必须非空）
 * @param syn      预先算好的综合式（2t 项）；非 NULL 时直接用于 Berlekamp–Massey
 * @param errloc   输出错误位置数组（至少 t 个元素）
 * @param out_nerr 输出纠正的错误数（0..t）
 * @return true  解码完成（可能纠正了 0..t 个错误）
 *         false 不可纠
 * @note 入参前置违反（len 超码长 / data、recv_ecc 为空）一律 ASSERT
 *       （见 assertion_guidelines.md §十五）。
 * @note errloc[i] < 8*len 为数据区错误，errloc[i] >= 8*len 为 ECC 区错误；
 *       纠正方式 data[errloc/8] ^= (1 << (errloc%8))。
 */
bool fc_bch_decode(fc_bch_control_t *bch, const BYTE *data, unsigned int len, const BYTE *recv_ecc, const BYTE *calc_ecc, const unsigned int *syn, unsigned int *errloc, unsigned int *out_nerr);

/**
 * @brief 辅助函数：获取 BCH 引擎的 ECC 字节数。
 * @param bch BCH 控制句柄
 * @return ECC 字节数
 */
unsigned int fc_bch_ecc_bytes(const fc_bch_control_t *bch);

/**
 * @brief 辅助函数：获取纠错能力 t。
 * @param bch BCH 控制句柄
 * @return 纠错能力（bit 数）
 */
unsigned int fc_bch_t(const fc_bch_control_t *bch);

#endif /* FC_BCH_ENGINE_H */