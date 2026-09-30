#ifndef FC_LDPC_ENGINE_H
#define FC_LDPC_ENGINE_H

#include "fc_types.h"

#include <stdint.h>

/*
 * ECC 引擎统一接口。
 *
 * fc_ldpc_encode/fc_ldpc_decode 作为统一 ECC 入口，
 * 底层算法由编译宏选择：
 *
 *   FC_ECC_ALGO_BCH  (0) — BCH(m=13, t=4)（默认，全功能）
 *   FC_ECC_ALGO_LDPC (1) — QC-LDPC 硬判决 Bit-flipping（实验性）
 *
 * CW 布局：[META:4][CRC:4][USER:512][PARITY:8] = 528B
 *
 * 运行时 ldpc 位域：
 *   ldpc=0 → BCH
 *   ldpc=1 → LDPC hard
 *   ldpc=2 → LDPC soft（stub）
 *   ldpc=3 → LDPC raw（stub）
 */
#define FC_ECC_ALGO_BCH  (0U)
#define FC_ECC_ALGO_LDPC (1U)

#ifndef FC_ECC_ALGO
#    define FC_ECC_ALGO FC_ECC_ALGO_BCH
#endif

typedef struct
{
    uint16_t codeword_bytes;
    uint16_t parity_bytes;
    uint8_t  max_iterations;
} fc_ldpc_config_t;

fc_status_t fc_ldpc_encode(const fc_ldpc_config_t *config, uint8_t *data, uint16_t data_length);

/*
 * fc_ldpc_decode - 解码单个 CW（data + parity 连续缓冲）
 *
 * BCH 分支下可选输出纠正的 bit 数：
 *   @param err_bits  out，非 NULL 时返回本 CW 解码纠正的错误 bit 数（BCH t=4 → 0..4）；
 *                  IO_ERROR/失败时为 0。无需该信息时传 NULL。
 */
fc_status_t fc_ldpc_decode(const fc_ldpc_config_t *config, uint8_t *data, uint16_t data_length,
                           uint8_t *err_bits);

#endif /* FC_LDPC_ENGINE_H */