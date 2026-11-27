#ifndef FC_ECC_ENGINE_H
#define FC_ECC_ENGINE_H

#include "fc_interface.h" /* fc_status_e */
#include "ssd_types.h"    /* BYTE/WORD */

#include <stdint.h>

/*
 * fc_ecc_engine.h — ECC 引擎统一接口（BCH / QC-LDPC 选择）
 *
 * fc_ecc_encode/fc_ecc_decode 作为统一 ECC 入口，
 * 底层算法由编译宏选择：
 *
 *   FC_ECC_ALGO_BCH  (0) — BCH(m=13, t=4)（默认，全功能）
 *   FC_ECC_ALGO_LDPC (1) — QC-LDPC 硬判决 Bit-flipping（实验性）
 *
 * CW 布局：[META:4][CRC:4][USER:512][PARITY:8] = 528B
 *
 * 运行时 ldpc 位域（命令配置）：
 *   ldpc=0 → normal（当前编译引擎按 FC_ECC_ALGO 选择；本仓默认 BCH）
 *   ldpc=1 → LDPC hard（未完整支持）
 *   ldpc=2 → LDPC soft（未完整支持）
 *   ldpc=3 → raw（跳过 ECC 解码）
 */
#define FC_ECC_ALGO_BCH  (0U)
#define FC_ECC_ALGO_LDPC (1U)

#ifndef FC_ECC_ALGO
#    define FC_ECC_ALGO FC_ECC_ALGO_BCH
#endif

/**
 * @brief 活动 ECC 引擎名（诊断 / 开机横幅用）。
 * @note 与实现文件里的 fc_bch_init(13, 4, …) 就地一致，调用者不释放返回值。
 */
const char *fc_ecc_algo_name(void);

/**
 * @brief ECC 编码单个 CW：在 data 尾部写入 8B parity。
 * @note data_length 恒为 FC_BUFFER_DATA_SIZE（520）；data 非空、data_length
 *       ∈ [8, FC_BUFFER_DATA_SIZE] 为前置条件（违反一律 ASSERT）。
 * @note 引擎初始化失败返回 FC_STATUS_NOT_SUPPORTED，调用方必须按失败处理。
 */
fc_status_e fc_ecc_encode(BYTE *data, WORD data_length);

/**
 * @brief 解码单个 CW（data + parity 连续缓冲），成功后就地纠正数据区。
 * @note BCH 分支可选输出纠正的 bit 数；LDPC 分支不写 err_bits（入口处已置 0）。
 * @note data 非空、data_length ∈ [8, FC_BUFFER_DATA_SIZE] 为前置条件（违反一律 ASSERT）。
 */
fc_status_e fc_ecc_decode(BYTE *data, WORD data_length, BYTE *err_bits);

#endif /* FC_ECC_ENGINE_H */
