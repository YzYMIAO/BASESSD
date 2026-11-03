#include "fc_ldpc_engine.h"

#include "assertion.h"
#include "fc.h" /* FC_BUFFER_DATA_SIZE / FC_BUFFER_POOL_CW_SIZE（入口上界与 LDPC 分支用） */
#include "fc_bch_engine.h"
#include "fc_ldpc_qc_engine.h"
#include "log.h"

#include <stddef.h>
#include <string.h>

// fc_ldpc_engine.c — ECC 统一入口（BCH / QC-LDPC 按编译宏切换）
//
// FC_ECC_ALGO 控制编译使用 BCH 或 LDPC。
//
// BCH（默认）：
// CW 布局：[META:4][CRC:4][USER:512][PARITY:8] = 528B
// DATA = META+CRC+USER = 520B, PARITY = 8B
// BCH(m=13, t=4)：52bit parity → 7B，8B 够用
// 编码：520B data → 8B parity
// 解码：520B data + 8B recv_parity → 检测/纠正
//
// LDPC（可选，FC_ECC_ALGO_LDPC 编译）：
// 同上 CW 布局，但不使用 BCH 算法，而是使用 QC-LDPC 硬判决 Bit-flipping。
// 注意：码率 0.985 下 LDPC 纠错能力约 t=1~2，低于 BCH t=4。
// LDPC 的真正优势在低码率（r<0.9）和软判决场景。

// 全局 BCH / QC-LDPC 引擎实例（单例，首次使用惰性初始化）
static fc_bch_control_t *g_bch = NULL;
#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)
static fc_ldpc_qc_t *g_qc = NULL;
#endif

// 惰性初始化的单次日志标志：成功/失败各只打印一次，避免每次编解码刷屏。
static bool s_bch_init_failed = false;
static bool s_bch_init_logged = false;

/**
 * @brief 惰性初始化 BCH 引擎（m=13, t=4）
 *
 * 初始化失败必须可见：编码/解码不得“成功跳过”，否则等于静默关闭 ECC（C8/§3.2）。
 * 表为静态存储，失败只可能是参数/断言错误。
 *
 * @return 1 = 引擎可用
 *         0 = 初始化失败（失败已 LOG 一次，此后 ECC 不可用）
 */
static BYTE fc_ecc_init_bch(void)
{
    if (g_bch == NULL)
    {
        g_bch = fc_bch_init(13, 4, 0U, 0U);
        if (g_bch == NULL)
        {
            if (s_bch_init_failed == false)
            {
                s_bch_init_failed = true;
                LOGE("[FC  ] BCH init FAILED: ECC is disabled (check static storage/size)\r\n");
            }
        }
        else if (s_bch_init_logged == false)
        {
            s_bch_init_logged = true;
            LOGI("[FC  ] BCH ready: static tables, no heap\r\n");
        }
    }
    return (g_bch != NULL) ? 1U : 0U;
}

#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)
/**
 * @brief 惰性初始化 QC-LDPC 引擎（最大迭代 15）
 *
 * @return 1 = 引擎可用
 *         0 = 初始化失败
 */
static BYTE fc_ecc_init_ldpc(void)
{
    if (g_qc == NULL)
    {
        g_qc = fc_ldpc_qc_init(15U);
    }
    return (g_qc != NULL) ? 1U : 0U;
}
#endif

/**
 * @brief 当前编译出的 ECC 算法名
 *
 * @return "BCH(13,4)" 或 "LDPC"（随 FC_ECC_ALGO 切换）
 */
const char *fc_ecc_algo_name(void)
{
#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)
    return "LDPC";
#else
    return "BCH(13,4)";
#endif
}

/**
 * @brief ECC 编码统一入口：对 data[0..data_length) 编码，parity 写在其后 8B
 *
 * BCH 分支由 fc_bch_encode 生成 8B parity；LDPC 分支以 528B 完整码字为输入输出。
 * 引擎不可用时返回 NOT_SUPPORTED，由调用方按「跳过 ECC」处理。
 *
 * @param config      ECC 配置（当前由编译宏决定算法，未使用）
 * @param data        CW 缓冲（528B）
 * @param data_length 数据区字节数（≤ FC_BUFFER_DATA_SIZE = 520）
 * @return FC_STATUS_OK / FC_STATUS_NOT_SUPPORTED
 */
fc_status_e fc_ldpc_encode(const fc_ldpc_config_t *config, BYTE *data, WORD data_length)
{
    (void)config;

    // 入参前置条件由调用者保证（fc_data_path 恒传 CW 缓冲 + FC_BUFFER_DATA_SIZE）
    ASSERT(data != NULL);
    ASSERT(data_length != 0U);
    // 上界：parity 尾区占 data[data_length..data_length+7]，CW 缓冲恒 528B
    ASSERT(data_length <= FC_BUFFER_DATA_SIZE);

#if (FC_ECC_ALGO == FC_ECC_ALGO_BCH)

    /* ── BCH 编码 ── */
    if (fc_ecc_init_bch() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        BYTE ecc_buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        // data_length 恒 ≥ 8（520），parity 尾区足够
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
        // data_length = 520（数据区），后 8B = parity 输出区
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

/**
 * @brief ECC 解码统一入口：校验 data + 其后 8B parity，并原地纠正数据区
 *
 * @param config      ECC 配置（未使用）
 * @param data        CW 缓冲（528B）
 * @param data_length 数据区字节数（≤ FC_BUFFER_DATA_SIZE = 520）
 * @param err_bits    输出：纠正的 bit 数（可 NULL）
 * @return FC_STATUS_OK / FC_STATUS_NOT_SUPPORTED / FC_STATUS_IO_ERROR（不可纠）
 */
fc_status_e fc_ldpc_decode(const fc_ldpc_config_t *config, BYTE *data, WORD data_length, BYTE *err_bits)
{
    (void)config;

    if (err_bits != NULL)
    {
        *err_bits = 0U;
    }

    // 入参前置条件由调用者保证（fc_data_path 恒传 CW 缓冲 + FC_BUFFER_DATA_SIZE）；
    // err_bits 可为 NULL（辨析 B），保留上方语义分支
    ASSERT(data != NULL);
    ASSERT(data_length != 0U);
    // 上界：recv_ecc = data + data_length 起读 8B，CW 缓冲恒 528B
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

        // data_length 恒 ≥ 8（520），parity 尾区足够
        ASSERT(data_length >= 8U);

        if (fc_bch_decode(g_bch, data, data_length, recv_ecc, NULL, NULL, errloc, &nerr) == false)
        {
            return FC_STATUS_IO_ERROR;
        }

        // BCH 成功解码：报告纠正的 bit 数（0..t）
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

    // data_length = 520（来自调用者），但 QC-LDPC 解码器硬码
    // 码长 n=4224bit=528B（520B 数据 + 8B parity），内部直接
    // 按 528B 操作 buffer。buffer 实际分配为 FC_BUFFER_POOL_CW_SIZE(528)，
    // 访问安全。此处显式抑制未使用参数警告。
    (void)data_length;
    return fc_ldpc_qc_decode(g_qc, data);

#else
#    error "Unknown FC_ECC_ALGO value"
#endif
}
