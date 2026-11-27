#ifndef FC_BCH_ENGINE_H
#define FC_BCH_ENGINE_H

#include "ssd_types.h" /* BYTE */

#include <stdint.h>

/*
 * fc_bch_engine.h — BCH ECC 算法
 *
 * clean-room 实现：按 BCH(2^m-1, k, t) 构造与 Berlekamp–Massey / 根搜索
 * 独立实现，不包含第三方源码；项目许可为 Apache-2.0。
 *
 * 本工程固定参数（CW = 520B data + 8B ECC 槽）：
 *   m=13, t=4 → ecc_bits=52、ecc_bytes=7；调用者保持第 8 字节为 0。
 * 静态存储只覆盖这一组参数；其他 m/t 在 debug 下 ASSERT、release 下返回 NULL。
 */

typedef struct FC_BCH_CONTROL_S fc_bch_control_s;

/**
 * @brief 释放 BCH 引擎。
 * @note 存储为静态分配；本实现不释放资源，保留 API 语义。
 */
void fc_bch_free(fc_bch_control_s *bch);

/**
 * @brief 计算数据段的 BCH parity。
 * @note 增量编码：首次调用前 ecc 必须清零；后续传入上一次输出即可继续。
 * @note parity 为 shortening 后的余式，MSB-first；第 8 字节保持调用者原值。
 * @note bch/data 为空或 len 超长违反前置条件，一律 ASSERT。
 */
void fc_bch_encode(fc_bch_control_s *bch, const BYTE *data, unsigned int len, BYTE *ecc);

/**
 * @brief 检测并定位 data + parity 中的错误。
 * @note errloc[i] < 8*len 为数据区，>= 8*len 为 parity 区；调用方负责翻转。
 * @note swap_bits==0 时位置按字节内 bit 反转；swap_bits!=0 直接返回 q。
 * @note 入参前置违反（len 超长 / data、recv_ecc、errloc、out_nerr 为空）一律 ASSERT。
 */
BOOL fc_bch_decode(fc_bch_control_s *bch, const BYTE *data, unsigned int len, const BYTE *recv_ecc, unsigned int *errloc, unsigned int *out_nerr);

/**
 * @brief 初始化 BCH 引擎（本工程固定 m=13 / t=4）。
 * @note 表存于静态结构体（约 40 KiB），无堆分配；重复调用会重建。
 */
fc_bch_control_s *fc_bch_init(int m, int t, unsigned int prim_poly, BYTE swap_bits);

#endif /* FC_BCH_ENGINE_H */
