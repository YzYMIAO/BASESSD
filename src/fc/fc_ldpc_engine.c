#include "fc_ldpc_engine.h"

#include "assertion.h"
#include "fc.h" /* FC_BUFFER_DATA_SIZE / FC_BUFFER_POOL_CW_SIZE（入口上界与 LDPC 分支用） */
#include "fc_bch_engine.h"
#include "fc_ldpc_qc_engine.h"

#include <stddef.h>
#include <string.h>

/*
 * fc_ldpc_engine.c — ECC 统一入口（BCH / QC-LDPC 按编译宏切换）
 *
 * FC_ECC_ALGO 控制编译使用 BCH 或 LDPC。
 *
 * BCH（默认）：
 *   CW 布局：[META:4][CRC:4][USER:512][PARITY:8] = 528B
 *   DATA = META+CRC+USER = 520B, PARITY = 8B
 *   BCH(m=13, t=4)：52bit parity → 7B，8B 够用
 *   编码：520B data → 8B parity
 *   解码：520B data + 8B recv_parity → 检测/纠正
 *
 * LDPC（可选，FC_ECC_ALGO_LDPC 编译）：
 *   同上 CW 布局，但不使用 BCH 算法，而是使用 QC-LDPC 硬判决 Bit-flipping。
 *   注意：码率 0.985 下 LDPC 纠错能力约 t=1~2，低于 BCH t=4。
 *   LDPC 的真正优势在低码率（r<0.9）和软判决场景。
 */

/* 全局 BCH / QC-LDPC 引擎实例（单例，首次使用惰性初始化） */
static fc_bch_control_t *g_bch = NULL;
#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)
static fc_ldpc_qc_t *g_qc = NULL;
#endif

/* 惰性初始化 BCH 引擎（m=13, t=4，本工程 CW 参数） */
static BYTE fc_ecc_init_bch(void)
{
    if (g_bch == NULL)
    {
        g_bch = fc_bch_init(13, 4, 0U, 0U);
    }
    return (g_bch != NULL) ? 1U : 0U;
}

#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)
/* 惰性初始化 QC-LDPC 引擎（最大迭代 15） */
static BYTE fc_ecc_init_ldpc(void)
{
    if (g_qc == NULL)
    {
        g_qc = fc_ldpc_qc_init(15U);
    }
    return (g_qc != NULL) ? 1U : 0U;
}
#endif

/* 活动 ECC 引擎名（诊断/开机横幅用）。字符串与上方 fc_bch_init(13, 4, …) 就地一致。 */
const char *fc_ecc_algo_name(void)
{
#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)
    return "LDPC";
#else
    return "BCH(13,4)";
#endif
}

/* --------------------------------------------------------------------
 * fc_ldpc_encode
 *   功能：ECC 编码——data[0..len-1] 的校验码写入 data[len..len+7]。
 *   参数：config —— 配置（本实现未用）；data —— 数据缓冲（含 8B parity 尾区）；
 *         data_length —— 数据字节数（520）。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / NOT_SUPPORTED（引擎初始化失败）。
 *   实现要点：
 *     - BCH 分支：fc_bch_encode 生成 8B parity 写 data[520..527]。
 *     - LDPC 分支：qc_encode 输入输出均为 528B 完整码字。
 * -------------------------------------------------------------------- */
fc_status_t fc_ldpc_encode(const fc_ldpc_config_t *config, BYTE *data, WORD data_length)
{
    (void)config;

    /* 入参前置条件由调用者保证（fc_data_path 恒传 CW 缓冲 + FC_BUFFER_DATA_SIZE） */
    ASSERT(data != NULL);
    ASSERT(data_length != 0U);
    /* 上界：parity 尾区占 data[data_length..data_length+7]，CW 缓冲恒 528B（v21 补） */
    ASSERT(data_length <= FC_BUFFER_DATA_SIZE);

#if (FC_ECC_ALGO == FC_ECC_ALGO_BCH)

    /* ── BCH 编码 ── */
    if (fc_ecc_init_bch() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        BYTE ecc_buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        /* data_length 恒 ≥ 8（520），parity 尾区足够 */
        ASSERT(data_length >= 8U);

        fc_bch_encode(g_bch, data, data_length, ecc_buf);
        memcpy(data + data_length, ecc_buf, 8U);
    }

    return FC_STATUS_OK;

#elif (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)

    /* ── QC-LDPC 编码 ── */
    if (fc_ecc_init_ldpc() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        /* data_length = 520（数据区），后 8B = parity 输出区 */
        WORD full_len = (data_length + 8U);
        BYTE tmp[FC_BUFFER_POOL_CW_SIZE];

        memcpy(tmp, data, full_len);
        fc_ldpc_qc_encode(g_qc, tmp, data);
    }

    return FC_STATUS_OK;

#else
#    error "Unknown FC_ECC_ALGO value"
#endif
}

/* --------------------------------------------------------------------
 * fc_ldpc_decode
 *   功能：ECC 解码——检测/纠正 data[0..len-1]+parity[len..len+7]。
 *   参数：config —— 配置（未用）；data —— 码字缓冲；data_length —— 数据长；
 *         err_bits —— 输出纠正 bit 数（可 NULL）。
 *   返回：FC_STATUS_OK（成功，data 已纠正）/ IO_ERROR（不可纠）/
 *         INVALID_ARGUMENT / NOT_SUPPORTED。
 *   实现要点：
 *     - BCH 分支：fc_bch_decode 返回错误位置数组，errloc<8*len 为数据区
 *       错误，按位翻转纠正；err_bits = 纠正数（0..t）。
 *     - LDPC 分支：qc_decode 内部按 528B 码字直接操作。
 * -------------------------------------------------------------------- */
fc_status_t fc_ldpc_decode(const fc_ldpc_config_t *config, BYTE *data, WORD data_length, BYTE *err_bits)
{
    (void)config;

    if (err_bits != NULL)
    {
        *err_bits = 0U;
    }

    /* 入参前置条件由调用者保证（fc_data_path 恒传 CW 缓冲 + FC_BUFFER_DATA_SIZE）；
     * err_bits 可为 NULL（辨析 B），保留上方语义分支 */
    ASSERT(data != NULL);
    ASSERT(data_length != 0U);
    /* 上界：recv_ecc = data + data_length 起读 8B，CW 缓冲恒 528B（v21 补） */
    ASSERT(data_length <= FC_BUFFER_DATA_SIZE);

#if (FC_ECC_ALGO == FC_ECC_ALGO_BCH)

    /* ── BCH 解码 ── */
    if (fc_ecc_init_bch() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        BYTE        *recv_ecc = data + data_length;
        unsigned int errloc[4];
        unsigned int nerr;
        WORD         i;

        /* data_length 恒 ≥ 8（520），parity 尾区足够 */
        ASSERT(data_length >= 8U);

        if (!fc_bch_decode(g_bch, data, data_length, recv_ecc, NULL, NULL, errloc, &nerr))
        {
            return FC_STATUS_IO_ERROR;
        }

        /* BCH 成功解码：报告纠正的 bit 数（0..t） */
        if (err_bits != NULL)
        {
            *err_bits = nerr;
        }

        for (i = 0U; i < nerr; i++)
        {
            unsigned int bit_pos = errloc[i];
            if (bit_pos < (unsigned int)(data_length * 8U))
            {
                data[bit_pos / 8U] ^= (1U << (bit_pos % 8U));
            }
        }

        return FC_STATUS_OK;
    }

#elif (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)

    /* ── QC-LDPC 硬判决解码 ── */
    if (fc_ecc_init_ldpc() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    /*
     * data_length = 520（来自调用者），但 QC-LDPC 解码器硬码
     * 码长 n=4224bit=528B（520B 数据 + 8B parity），内部直接
     * 按 528B 操作 buffer。buffer 实际分配为 FC_BUFFER_POOL_CW_SIZE(528)，
     * 访问安全。此处显式抑制未使用参数警告。
     */
    (void)data_length;
    return fc_ldpc_qc_decode(g_qc, data);

#else
#    error "Unknown FC_ECC_ALGO value"
#endif
}