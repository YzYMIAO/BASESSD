#include "fc_ldpc_engine.h"
#include "fc_bch_engine.h"
#include "fc_ldpc_qc_engine.h"

#include <stddef.h>
#include <string.h>

/*
 * ECC 引擎实现。
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

static fc_bch_control_t *g_bch = NULL;
static fc_ldpc_qc_t     *g_qc  = NULL;

static uint8_t fc_ecc_init_bch(void)
{
    if (g_bch == NULL)
    {
        g_bch = fc_bch_init(13, 4, 0U, 0U);
    }
    return (g_bch != NULL) ? 1U : 0U;
}

static uint8_t fc_ecc_init_ldpc(void)
{
    if (g_qc == NULL)
    {
        g_qc = fc_ldpc_qc_init(15U);
    }
    return (g_qc != NULL) ? 1U : 0U;
}

fc_status_t fc_ldpc_encode(const fc_ldpc_config_t *config, uint8_t *data, uint16_t data_length)
{
    (void)config;

    if ((data == NULL) || (data_length == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

#if (FC_ECC_ALGO == FC_ECC_ALGO_BCH)

    /* ── BCH 编码 ── */
    if (fc_ecc_init_bch() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        uint16_t data_len = (data_length > 8U) ? (uint16_t)(data_length - 8U) : 0U;
        uint8_t  ecc_buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        if (data_len == 0U)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }

        fc_bch_encode(g_bch, data, data_len, ecc_buf);
        memcpy(data + data_len, ecc_buf, 8U);
    }

    return FC_STATUS_OK;

#elif (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)

    /* ── QC-LDPC 编码 ── */
    if (fc_ecc_init_ldpc() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        /* data 前 520B = 数据，后 8B = parity，编码直接写入 */
        uint8_t tmp[528];
        memcpy(tmp, data, 528);
        fc_ldpc_qc_encode(g_qc, tmp, data);
    }

    return FC_STATUS_OK;

#else
#    error "Unknown FC_ECC_ALGO value"
#endif
}

fc_status_t fc_ldpc_decode(const fc_ldpc_config_t *config, uint8_t *data, uint16_t data_length)
{
    (void)config;

    if ((data == NULL) || (data_length == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

#if (FC_ECC_ALGO == FC_ECC_ALGO_BCH)

    /* ── BCH 解码 ── */
    if (fc_ecc_init_bch() == 0U)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }

    {
        uint16_t     data_len = (data_length > 8U) ? (uint16_t)(data_length - 8U) : 0U;
        uint8_t     *recv_ecc = data + data_len;
        unsigned int errloc[4];
        int          nerr;
        uint16_t     i;

        if (data_len == 0U)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }

        nerr = fc_bch_decode(g_bch, data, data_len, recv_ecc, NULL, NULL, errloc);

        if (nerr < 0)
        {
            return FC_STATUS_IO_ERROR;
        }

        for (i = 0U; i < (uint16_t)nerr; i++)
        {
            unsigned int bit_pos = errloc[i];
            if (bit_pos < (unsigned int)(data_len * 8U))
            {
                data[bit_pos / 8U] ^= (uint8_t)(1U << (bit_pos % 8U));
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

    return fc_ldpc_qc_decode(g_qc, data);

#else
#    error "Unknown FC_ECC_ALGO value"
#endif
}