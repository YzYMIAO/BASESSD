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
#include "fc_ldpc_engine.h"
#include "fc_raid_engine.h"
#include "fc_random_engine.h"

#include <stddef.h>
#include <string.h>

/* ── 辅助：遍历 desc 链收集 buffer 指针（encode 用，跳过 NULL） ── */

static uint8_t fc_collect_buffers(const fc_command_t *cmd,
                                  uint8_t *buffers[], uint8_t max_count)
{
    fc_descriptor_id_t desc_id;
    uint8_t            count = 0U;

    for (desc_id = (fc_descriptor_id_t)cmd->desc_id;
         (desc_id != FC_DESCRIPTOR_ID_INVALID) && (count < max_count);)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;

        buffers[count] = fc_buffer_id_to_real_buffer(desc->buffer_id);
        if (buffers[count] != NULL)
        {
            count++;
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
    return count;
}

/* ── 辅助：遍历 desc 链收集 buffer 指针 + PAA（decode 用，保持索引对齐 desc 链） ── */

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

        buffers[count] = fc_buffer_id_to_real_buffer(desc->buffer_id);
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

    /* ── Stage 1: Random 加扰 ── */
    if (command->config.random == 0U)
    {
        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

            buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
            if (buffer != NULL)
            {
                uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
                (void)fc_random_transform(buffer, FC_BUFFER_POOL_CW_SIZE, seed);
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 2: ECC 编码 ── */
    if (command->config.ldpc != FC_ECC_MODE_LDPC_RAW)
    {
        const fc_ldpc_config_t ldpc_config = {
            .codeword_bytes = FC_BUFFER_POOL_CW_SIZE,
            .parity_bytes   = 8U,
            .max_iterations = 4U,
        };

        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

            buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
            if (buffer != NULL)
            {
                result = fc_ldpc_encode(&ldpc_config, buffer, FC_BUFFER_POOL_CW_SIZE);
                if ((result != FC_STATUS_OK) && (result != FC_STATUS_NOT_SUPPORTED))
                {
                    return result;
                }
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 3: RAID XOR 编码 ── */
    if (command->config.raid == 2U)  /* encode */
    {
        uint8_t *buffers[CW_TOTAL_MAX];
        uint8_t  count;
        uint8_t  i;

        count = fc_collect_buffers(command, buffers, CW_TOTAL_MAX);

        if (count >= 2U)
        {
            result = fc_raid_start(0U, buffers[0U], count);
            if (result == FC_STATUS_OK)
            {
                for (i = 1U; i < count; i++)
                {
                    (void)fc_raid_accumulate(0U, buffers[i]);
                }
                /* XOR 结果写入最后一个 buffer（作为 parity CW） */
                (void)fc_raid_end(0U, buffers[count - 1U]);
            }
        }
    }

    return FC_STATUS_OK;
}

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
        .codeword_bytes = FC_BUFFER_POOL_CW_SIZE,
        .parity_bytes   = 8U,
        .max_iterations = 4U,
    };

    /* ── Pass 1: ECC 解码所有 CW ── */
    desc_id = (fc_descriptor_id_t)command->desc_id;

    for (cw = 0U; cw < (uint8_t)(command->config.cw + 1U); cw++)
    {
        desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) return FC_STATUS_INVALID_ARGUMENT;

        buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
        if (buffer == NULL) return FC_STATUS_INVALID_ARGUMENT;

        if (command->config.ldpc != FC_ECC_MODE_LDPC_RAW)
        {
            result = fc_ldpc_decode(&ldpc_config, buffer, FC_BUFFER_POOL_CW_SIZE);

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

            buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
            if (buffer != NULL)
            {
                uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
                (void)fc_random_transform(buffer, FC_BUFFER_POOL_CW_SIZE, seed);
            }

            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 读路径 Stage 2 — FTL 触发
 *
 * FTL 遍历 desc 链发现 status=UNC 时调用。
 * 内部：XOR 重建 → ECC 重试 → 解扰所有 CW → 更新 desc status。
 * ══════════════════════════════════════════════════════════ */

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
            .codeword_bytes = FC_BUFFER_POOL_CW_SIZE,
            .parity_bytes   = 8U,
            .max_iterations = 4U,
        };
        fc_status_t ecc_result;

        ecc_result = fc_ldpc_decode(&ldpc_config, buffers[failed_cw_idx], FC_BUFFER_POOL_CW_SIZE);
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
                (void)fc_random_transform(buffers[i], FC_BUFFER_POOL_CW_SIZE, seed);
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