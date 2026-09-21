#include "fc_data_path.h"

#include "fc.h"
#include "fc_ldpc_engine.h"
#include "fc_raid_engine.h"
#include "fc_random_engine.h"

#include <stddef.h>

/*
 * FC 数据路径管线：
 *
 * Write (encode) 方向:
 *   Random 加扰 → LDPC 编码 → RAID XOR 编码
 *   ───── 已实现 ────  ─── 扩展点 ───  ─── 扩展点 ───
 *
 * Read (decode) 方向:
 *   RAID XOR 解码 → LDPC 解码 → Random 解扰
 *    ─── 扩展点 ───  ─── 扩展点 ───  ───── 已实现 ────
 *
 * CW 数据布局 (528 bytes):
 *   [META:16B][CRC:4B][USER_DATA:476B][LDPC_PARITY:32B]
 *
 * LDPC 扩展点契约:
 *   - fc_ldpc_encode(): 输入 CW buffer 含用户数据，输出填 LDPC_PARITY 区域
 *   - fc_ldpc_decode(): 输入 CW buffer 含完整码字，输出纠正后的用户数据
 *   - config.parity_bytes = 32, codeword_bytes = FC_BUFFER_POOL_CW_SIZE
 *
 * RAID 扩展点契约:
 *   - fc_raid_xor(): 同命令下所有 desc buffer 按位 XOR，结果写 parity buffer
 *   - 当前每命令最多 CW_TOTAL_MAX(8) 个 CW
 *   - 当前实现为 NOT_SUPPORTED，管线跳过时数据路径仍正常工作
 */

fc_status_t fc_data_path_encode(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *buffer;
    fc_status_t        result;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* ── Stage 1: Random 加扰（已实现） ── */
    if (command->config.random == 0U)
    {
        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                return FC_STATUS_INVALID_ARGUMENT;
            }
            buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
            if (buffer != NULL)
            {
                uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
                (void)fc_random_transform(buffer, FC_BUFFER_POOL_CW_SIZE, seed);
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 2: LDPC 编码 [扩展点] ── */
    {
        const fc_ldpc_config_t ldpc_config = {
            .codeword_bytes = FC_BUFFER_POOL_CW_SIZE,
            .parity_bytes   = 32U,
            .max_iterations = 4U,
        };

        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                return FC_STATUS_INVALID_ARGUMENT;
            }
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

    /* ── Stage 3: RAID XOR 编码 [扩展点] ── */
    {
        /* 同命令下的所有 desc buffer 跨 CW XOR 生成奇偶校验。
         * 待实现后在此处收集所有 desc buffer 指针并调用 fc_raid_xor()。 */
        fc_status_t raid_result = fc_raid_xor(NULL, NULL, 0U);
        if ((raid_result != FC_STATUS_OK) && (raid_result != FC_STATUS_NOT_SUPPORTED))
        {
            return raid_result;
        }
    }

    return FC_STATUS_OK;
}

fc_status_t fc_data_path_decode(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *buffer;
    fc_status_t        result;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* ── Stage 1: RAID XOR 解码 [扩展点] ── */
    {
        /* RAID 恢复：如果读取返回的 CW 中有不可纠错误，用奇偶校验恢复数据。
         * 待实现后传入各 desc buffer + parity buffer 调用 fc_raid_xor()。 */
        fc_status_t raid_result = fc_raid_xor(NULL, NULL, 0U);
        if ((raid_result != FC_STATUS_OK) && (raid_result != FC_STATUS_NOT_SUPPORTED))
        {
            return raid_result;
        }
    }

    /* ── Stage 2: LDPC 解码 [扩展点] ── */
    {
        const fc_ldpc_config_t ldpc_config = {
            .codeword_bytes = FC_BUFFER_POOL_CW_SIZE,
            .parity_bytes   = 32U,
            .max_iterations = 4U,
        };

        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                return FC_STATUS_INVALID_ARGUMENT;
            }
            buffer = fc_buffer_id_to_real_buffer(desc->buffer_id);
            if (buffer != NULL)
            {
                result = fc_ldpc_decode(&ldpc_config, buffer, FC_BUFFER_POOL_CW_SIZE);
                if ((result != FC_STATUS_OK) && (result != FC_STATUS_NOT_SUPPORTED))
                {
                    return result;
                }
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 3: Random 解扰（已实现） ── */
    if (command->config.random == 0U)
    {
        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                return FC_STATUS_INVALID_ARGUMENT;
            }
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