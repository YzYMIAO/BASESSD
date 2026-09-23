/*
 * FC 数据路径管线：
 *
 * Write (encode) 方向 — FC 内部在 NAND Program 提交前调用，对 FTL 透明：
 *   Random 加扰 → ECC 编码 → RAID XOR 编码
 *
 * Read (decode) 方向 — 分两阶段：
 *   Stage 1: fc_data_path_process_read()
 *     NAND Read 完成后、CQ push 前，FC 内部自动调用。
 *     遍历 desc 链：ECC 解码 → 更新 desc->status。
 *     全部成功才解扰；有失败则保持加扰（供 RAID XOR 在 scrambled domain 恢复）。
 *
 *   Stage 2: fc_data_path_raid_recover()
 *     FTL 从 CQ 拿到 completion 后检查 desc 链，当发现 status=UNC
 *     且决定走 RAID 恢复时调用此接口。
 *     内部：XOR 重建 → ECC 重试 → 解扰所有 CW → 更新 desc status。
 *
 * CW 布局：[META:4B][CRC:4B][USER_DATA:512B][ECC_PARITY:8B] = 528B
 * parity CW 也经过 ECC 编码，受 NAND 保护。
 *
 * config.raid:
 *   0 = normal（跳过 RAID）
 *   1 = decode（ECC 失败时 RAID 恢复，FTL 触发）
 *   2 = encode（生成 parity CW，FC 内部）
 *   3 = recover（同 decode）
 *   4 = check（暂未实现）
 */

#include "fc_data_path.h"

#include "fc.h"
#include "fc_crc_engine.h"
#include "fc_ldpc_engine.h"
#include "fc_raid_engine.h"
#include "fc_random_engine.h"

#include <stddef.h>
#include <string.h>

/* ── 辅助：遍历 desc 链收集 buffer 指针（decode/RAID 用，保持索引对齐 desc 链） ── */

static uint8_t fc_collect_decode_buffers(const fc_command_t *cmd,
                                          uint8_t *buffers[],
                                          nand_paa_t buffer_paa[],
                                          uint8_t max_count)
{
    fc_descriptor_id_t desc_id;
    uint8_t            count = 0U;

    for (desc_id = (fc_descriptor_id_t)cmd->desc_id;
         (desc_id != FC_DESCRIPTOR_ID_INVALID) && (count < max_count);)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;

        /* 解码路径需要操作全 CW，取基址 */
        buffers[count] = fc_buffer_get_base(desc->buffer_id);
        buffer_paa[count] = desc->paa;
        count++;
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
    return count;
}

/* ══════════════════════════════════════════════════════════
 * 编码 — FC 内部调用
 * ══════════════════════════════════════════════════════════ */

fc_status_t fc_data_path_encode(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *buffer;
    fc_status_t        result;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* ── Stage 0: META 写入 — 每个 CW 的 FAA 存入 buffer[0..4] ── */
    {
        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                (void)memcpy(buffer, &desc->faa, FC_CRC_META_SIZE);
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 1: CRC 编码 ── */
    if (command->config.crc == FC_CRC_ENABLE)
    {
        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                /* 对 [META+CRC+USER] 的 USER 部分（512 字节）计算 CRC，存入 CRC 字段 */
                uint32_t crc = fc_crc_compute(buffer + FC_CRC_USER_OFFSET);
                (void)memcpy(buffer + FC_CRC_META_SIZE, &crc, FC_CRC_FIELD_SIZE);
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 1: Random 加扰 ── */
    if (command->config.random == 0U)
    {
        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
                (void)fc_random_transform(buffer, FC_BUFFER_DATA_SIZE, seed);
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 2: ECC 编码 ── */
    if (command->config.ldpc != FC_ECC_MODE_LDPC_RAW)
    {
        const fc_ldpc_config_t ldpc_config = {
            .codeword_bytes = FC_BUFFER_DATA_SIZE,
            .parity_bytes   = 8U,
            .max_iterations = 4U,
        };

        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                result = fc_ldpc_encode(&ldpc_config, buffer, FC_BUFFER_DATA_SIZE);
                if ((result != FC_STATUS_OK) && (result != FC_STATUS_NOT_SUPPORTED))
                {
                    return result;
                }
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 3 (removed): 跨命令 stripe RAID 在 fc_data_path_generate_stripe_parity() 中处理 ── */

    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 跨 Plane Stripe RAID 编码 — FC 内部调用
 *
 * 当 PLANE_NUM-1 条数据命令累积完成后，FC 自动调用此函数。
 * XOR 所有数据命令对应 CW 的数据区（FC_BUFFER_DATA_SIZE=520B），
 * 结果写入 parity 命令的 buffer，然后对 parity buffer 做 ECC 编码。
 *
 * data_cmds: 数据命令指针数组（长度 = PLANE_NUM - 1）
 * data_count: 数据命令数量
 * parity_cmd: 已分配 desc+buffer 的 parity 命令（接收 XOR 结果）
 *
 * 编译开关：CONFIG_FC_RAID (include/fc/fc.h)
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

fc_status_t fc_data_path_generate_stripe_parity(
    const fc_command_t *data_cmds[],
    uint8_t             data_count,
    const fc_command_t *parity_cmd)
{
    uint8_t            cw_count;
    uint8_t            cw;
    fc_descriptor_id_t desc_id;
    uint8_t            d;

    if ((data_cmds == NULL) || (data_cmds[0] == NULL) ||
        (parity_cmd == NULL) || (data_count == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* parity 命令的 CW 数量必须与数据命令一致 */
    cw_count = (uint8_t)(data_cmds[0]->config.cw + 1U);
    if ((uint8_t)(parity_cmd->config.cw + 1U) != cw_count)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /*
     * 逐 CW 做 XOR：
     *   parity_cmd->buffer[cw][0..520] = XOR(data_cmds[0..data_count-1]->buffer[cw][0..520])
     *
     * 操作的数据区为 FC_BUFFER_DATA_SIZE（520B），即 META+CRC+USER。
     * ECC parity 区 [520..528] 不参与 XOR，由 ECC 引擎在后续的 ECC 编码步骤生成。
     */
    for (cw = 0U; cw < cw_count; cw++)
    {
        uint8_t *parity_buf;
        uint8_t  first = 1U;

        /* 获取 parity 命令第 cw 个 CW 的 buffer 基址 */
        desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
        for (uint8_t s = 0U; s < cw; s++)
        {
            fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
            if (dsc == NULL) return FC_STATUS_INVALID_ARGUMENT;
            desc_id = (fc_descriptor_id_t)dsc->next_id;
        }
        {
            fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
            if (dsc == NULL) return FC_STATUS_INVALID_ARGUMENT;
            parity_buf = fc_buffer_get_base(dsc->buffer_id);
            if (parity_buf == NULL) return FC_STATUS_INVALID_ARGUMENT;
        }

        /* XOR 所有数据命令的对应 CW 数据区 */
        for (d = 0U; d < data_count; d++)
        {
            uint8_t           *data_buf;
            fc_descriptor_id_t data_desc_id;

            data_desc_id = (fc_descriptor_id_t)data_cmds[d]->desc_id;
            for (uint8_t s = 0U; s < cw; s++)
            {
                fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(data_desc_id);
                if (dsc == NULL) return FC_STATUS_INVALID_ARGUMENT;
                data_desc_id = (fc_descriptor_id_t)dsc->next_id;
            }
            {
                fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(data_desc_id);
                if (dsc == NULL) return FC_STATUS_INVALID_ARGUMENT;
                data_buf = fc_buffer_get_base(dsc->buffer_id);
                if (data_buf == NULL) return FC_STATUS_INVALID_ARGUMENT;
            }

            if (first != 0U)
            {
                /* 第一个数据命令：拷贝到 parity buffer，作为 XOR 累加器初值 */
                (void)memcpy(parity_buf, data_buf, FC_BUFFER_DATA_SIZE);
                first = 0U;
            }
            else
            {
                /* 后续数据命令：XOR 累加 */
                uint32_t *parity32 = (uint32_t *)(void *)parity_buf;
                const uint32_t *data32 = (const uint32_t *)(const void *)data_buf;
                for (uint16_t w = 0U; w < (FC_BUFFER_DATA_SIZE / sizeof(uint32_t)); w++)
                {
                    parity32[w] ^= data32[w];
                }
            }
        }
    }

    /* ── ECC 编码 parity 命令的 buffer ── */
    if (parity_cmd->config.ldpc != FC_ECC_MODE_LDPC_RAW)
    {
        const fc_ldpc_config_t ldpc_config = {
            .codeword_bytes = FC_BUFFER_DATA_SIZE,
            .parity_bytes   = 8U,
            .max_iterations = 4U,
        };

        for (desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

            uint8_t *buf = fc_buffer_get_base(desc->buffer_id);
            if (buf != NULL)
            {
                fc_status_t result;
                result = fc_ldpc_encode(&ldpc_config, buf, FC_BUFFER_DATA_SIZE);
                if ((result != FC_STATUS_OK) && (result != FC_STATUS_NOT_SUPPORTED))
                {
                    return result;
                }
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    return FC_STATUS_OK;
}

#endif /* CONFIG_FC_RAID */

/* ══════════════════════════════════════════════════════════
 * 读路径 Stage 1 — FC 内部，CQ push 前
 *
 * 对 desc 链逐 CW 做 ECC 解码 + 更新 descriptor status。
 * 全部成功 → 解扰所有 buffer。有失败 → buffer 保持 scrambled 状态。
 * ══════════════════════════════════════════════════════════ */

fc_status_t fc_data_path_process_read(const fc_command_t *command)
{
    fc_descriptor_id_t  desc_id;
    fc_descriptor_t    *desc;
    uint8_t            *buffer;
    fc_status_t         result;
    uint8_t             all_ok = 1U;
    uint8_t             cw;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    const fc_ldpc_config_t ldpc_config = {
        .codeword_bytes = FC_BUFFER_DATA_SIZE,
        .parity_bytes   = 8U,
        .max_iterations = 4U,
    };

    /* ── Pass 1: ECC 解码所有 CW ── */
    desc_id = (fc_descriptor_id_t)command->desc_id;

    for (cw = 0U; cw < (uint8_t)(command->config.cw + 1U); cw++)
    {
        desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

        buffer = fc_buffer_get_base(desc->buffer_id);
        if (buffer == NULL) return FC_STATUS_INVALID_ARGUMENT;

        if (command->config.ldpc != FC_ECC_MODE_LDPC_RAW)
        {
            result = fc_ldpc_decode(&ldpc_config, buffer, FC_BUFFER_DATA_SIZE);

            if (result == FC_STATUS_IO_ERROR)
            {
                /* ECC 不可纠：buffer 保持 scrambled，desc status 置 UNC */
                desc->status = FC_CW_STATUS_UNC;
                all_ok       = 0U;
            }
            else if ((result == FC_STATUS_OK) || (result == FC_STATUS_NOT_SUPPORTED))
            {
                /* ECC 成功（或跳过）：desc status 置 READY */
                desc->status = FC_CW_STATUS_READY;
            }
        }
        else
        {
            /* LDPC RAW 模式：跳过 ECC 解码，直接标记 READY */
            desc->status = FC_CW_STATUS_READY;
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
    }

    if (all_ok == 0U)
    {
        /* 有 CW 不可纠：
         * buffer 保持 ECC-corrected + scrambled 状态，
         * 供 FTL 后续的 RAID XOR 恢复在 same domain 进行。 */
        return FC_STATUS_IO_ERROR;
    }

    /* ── Pass 2: 所有 CW ECC 成功 → 解扰 ── */
    if (command->config.random == 0U)  /* FC_RANDOM_ENABLE */
    {
        desc_id = (fc_descriptor_id_t)command->desc_id;

        for (cw = 0U; cw < (uint8_t)(command->config.cw + 1U); cw++)
        {
            desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) break;

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
                (void)fc_random_transform(buffer, FC_BUFFER_DATA_SIZE, seed);
            }

            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Pass 3: CRC 校验 ── */
    {
        uint32_t stored_crc;
        uint32_t computed_crc;
        uint8_t  crc_ok;

        if (command->config.crc == FC_CRC_ENABLE)
        {
            desc_id = (fc_descriptor_id_t)command->desc_id;

            for (cw = 0U; cw < (uint8_t)(command->config.cw + 1U); cw++)
            {
                desc = fc_descriptor_id_to_descriptor(desc_id);
                if (desc == NULL) break;

                buffer = fc_buffer_get_base(desc->buffer_id);
                if (buffer != NULL)
                {
                    /* 读取之前编码时写入的 CRC 值 [4..8] */
                    (void)memcpy(&stored_crc, buffer + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE);

                    /* 重算 USER 数据的 CRC */
                    computed_crc = fc_crc_compute(buffer + FC_CRC_USER_OFFSET);
                    crc_ok       = (computed_crc == stored_crc) ? 1U : 0U;

                    if (crc_ok == 0U)
                    {
                        /*
                         * CRC 校验失败：标记 desc status。
                         * 虽然 ECC 纠错成功且解扰完成，但数据内容不正确，
                         * 可能是 ECC 误纠或数据路径逻辑错误。
                         */
                        desc->status |= FC_CW_STATUS_CRC_ERROR;
                    }
                }

                desc_id = (fc_descriptor_id_t)desc->next_id;
            }
        }
    }

    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 读路径 Stage 2 — FTL 触发
 *
 * FTL 遍历 desc 链发现 status=UNC 时调用。
 * 内部：XOR 重建 → ECC 重试 → 解扰所有 CW → 更新 desc status。
 *
 * 编译开关：CONFIG_FC_RAID (include/fc/fc.h)
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

fc_status_t fc_data_path_raid_recover(const fc_command_t *cmd, uint8_t failed_cw_idx)
{
    uint8_t           *buffers[CW_TOTAL_MAX];
    nand_paa_t         buffer_paa[CW_TOTAL_MAX];
    uint8_t            count;
    fc_descriptor_id_t desc_id;
    uint8_t            i;

    if ((cmd == NULL) || (cmd->desc_id == FC_DESCRIPTOR_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* ── 收集所有 buffer 指针 + PAA（索引对齐 desc 链位置） ── */
    count = fc_collect_decode_buffers(cmd, buffers, buffer_paa, CW_TOTAL_MAX);

    if (failed_cw_idx >= count)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* ── RAID XOR 重建 failed CW ── */
    {
        uint8_t recovered[FC_BUFFER_POOL_CW_SIZE];

        /* XOR 所有非 failed 的 CW（均为 ECC-corrected + scrambled），
         * 重建结果与被恢复 CW 处于 same scrambled domain。 */
        fc_raid_recover(recovered, buffers, count, failed_cw_idx, FC_BUFFER_POOL_CW_SIZE);
        (void)memcpy(buffers[failed_cw_idx], recovered, FC_BUFFER_POOL_CW_SIZE);
    }

    /* ── ECC 重试 ── */
    if (cmd->config.ldpc != FC_ECC_MODE_LDPC_RAW)
    {
        const fc_ldpc_config_t ldpc_config = {
            .codeword_bytes = FC_BUFFER_DATA_SIZE,
            .parity_bytes   = 8U,
            .max_iterations = 4U,
        };
        fc_status_t ecc_result;

        ecc_result = fc_ldpc_decode(&ldpc_config, buffers[failed_cw_idx], FC_BUFFER_DATA_SIZE);
        if (ecc_result == FC_STATUS_IO_ERROR)
        {
            /* RAID 重建后 ECC 仍失败 → 硬件层面不可恢复 */
            return FC_STATUS_IO_ERROR;
        }
    }

    /* ── 解扰所有 CW ── */
    if (cmd->config.random == 0U)  /* FC_RANDOM_ENABLE */
    {
        for (i = 0U; i < count; i++)
        {
            if (buffers[i] != NULL)
            {
                uint32_t seed = fc_random_seed_from_paa(buffer_paa[i], 0U);
                (void)fc_random_transform(buffers[i], FC_BUFFER_DATA_SIZE, seed);
            }
        }
    }

    /* ── 更新 descriptor status 为 READY ── */
    desc_id = (fc_descriptor_id_t)cmd->desc_id;
    i       = 0U;
    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (i < count))
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc != NULL)
        {
            desc->status = FC_CW_STATUS_READY;
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
        i++;
    }

    return FC_STATUS_OK;
}

#endif /* CONFIG_FC_RAID */