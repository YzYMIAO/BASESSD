#ifndef FC_LDPC_QC_ENGINE_H
#define FC_LDPC_QC_ENGINE_H

#include "fc_types.h"

#include <stdint.h>

/*
 * QC-LDPC 硬判决引擎（Bit-flipping 算法）。
 *
 * 参数（固定，编译期优化）：
 *   码长 n = 4224 bit (528B)
 *   信息位 k = 4160 bit (520B)
 *   校验位 m = 64 bit (8B)
 *   码率 R = 4160/4224 ≈ 0.985
 *
 * H 矩阵结构：QC-LDPC，循环子矩阵大小 Z=16
 *   基矩阵 H_base = 4 × 264
 *   列重 dv = 3（每列 3 个 1）
 *
 * 解码算法：硬判决 Bit-flipping
 *   ① 计算综合式 s = H × y^T
 *   ② 统计每个变量节点关联的失败校验方程数 fi
 *   ③ fi > 阈值 → 翻转该比特
 *   ④ 重复直到 s=0 或达到最大迭代次数
 */

/* QC-LDPC 控制结构体 */
typedef struct
{
    BYTE initialized;
    BYTE max_iter;
} fc_ldpc_qc_t;

/**
 * @brief 单例初始化（首次调用构造查表，全局复用）
 *
 * @param max_iter 最大迭代次数；为 0 时取默认值 15
 * @return 引擎句柄（指向静态单例，调用者不负责释放）
 */
fc_ldpc_qc_t *fc_ldpc_qc_init(BYTE max_iter);

/**
 * @brief QC-LDPC 编码
 *
 * data_in[0..519] → 520B 用户数据；
 * data_out[0..527] → 528B 码字（520B data + 8B parity）。
 * 引擎未初始化时静默返回，data_out 不被写入。
 *
 * @param qc       引擎句柄（当前实现未读取；校验矩阵为文件内静态表）
 * @param data_in  520B 用户数据
 * @param data_out 528B 输出码字
 * @pre qc / data_in / data_out 非空（违反一律 ASSERT）。
 */
void fc_ldpc_qc_encode(const fc_ldpc_qc_t *qc, const BYTE *data_in, BYTE *data_out);

/**
 * @brief LDPC 硬判决解码
 *
 * data[0..527] → 528B 待解码码字（含 8B parity）；直接修改 data 返回纠正后的码字。
 *
 * @param qc   引擎句柄（读取其中的最大迭代次数）
 * @param data 528B 码字，解码后就地更新
 * @return FC_STATUS_OK —— 码字合法或在迭代内收敛
 *         FC_STATUS_IO_ERROR —— 达到最大迭代或陷入不动点仍不可纠
 *         FC_STATUS_INVALID_ARGUMENT —— 引擎未初始化
 * @pre qc / data 非空（违反一律 ASSERT）。
 */
fc_status_e fc_ldpc_qc_decode(fc_ldpc_qc_t *qc, BYTE *data);

#endif /* FC_LDPC_QC_ENGINE_H */