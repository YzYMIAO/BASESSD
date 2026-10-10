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

#include "common/assertion.h"
#include "fc.h"
#include "fc_crc_engine.h"
#include "fc_ldpc_engine.h"
#include "fc_raid_engine.h"
#include "fc_random_engine.h"

#include <stddef.h>
#include <string.h>

#if CONFIG_FC_RAID
/* --------------------------------------------------------------------
 * fc_collect_decode_buffers
 *   功能：沿 desc 链收集命令所有 CW 的 buffer 基址。
 *   参数：cmd —— 命令；buffers —— 输出指针数组；max_count —— 容量上限（CW_TOTAL_MAX）。
 *   返回：收集到的 CW 数量。
 *   实现要点：解码路径需操作全 CW，用 fc_buffer_get_base 取 528B 完整 CW
 *         基址（含 META/CRC/PARITY），索引与 desc 链位置对齐。
 *   ⚠ 不再输出 PAA 数组（v24）：解扰种子改为从 buffer 内的**明文 META** 自举，
 *     PAA 这个唯一用途消失（§十七 判据 5）。
 * -------------------------------------------------------------------- */
static uint8_t fc_collect_decode_buffers(const fc_command_t *cmd, uint8_t *buffers[], uint8_t max_count)
{
    fc_descriptor_id_t desc_id;
    uint8_t            count = 0U;

    for (desc_id = (fc_descriptor_id_t)cmd->desc_id;
         (desc_id != FC_DESCRIPTOR_ID_INVALID) && (count < max_count);)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL)
        {
            break;
        }

        /* 解码路径需要操作全 CW，取基址 */
        buffers[count] = fc_buffer_get_base(desc->buffer_id);
        count++;
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
    return count;
}
#endif /* CONFIG_FC_RAID */

/* ══════════════════════════════════════════════════════════
 * 单 CW 步骤原语 — 管线与单测共用
 *
 * 每个函数只碰一块 528B 的裸 CW buffer（[META:4][CRC:4][USER:512][ECC:8]），
 * 不认识 command / descriptor / 链。下面的 encode / process_read 是这些步骤的
 * 编排；单测（Level 2）直接调它们，于是「调了哪些步骤」= 「开了哪些功能」。
 * ══════════════════════════════════════════════════════════ */

/* LDPC 参数全仓唯一一份——原先在 encode / process_read / stripe_parity /
 * raid_recover 各写了一遍字面量 {520, 8, 4}。 */
static const fc_ldpc_config_t FC_DATA_PATH_LDPC_CFG = {
    .codeword_bytes = FC_BUFFER_DATA_SIZE,
    .parity_bytes   = 8U,
    .max_iterations = 4U,
};

void fc_data_path_meta_write(uint8_t *cw, uint32_t meta)
{
    ASSERT(cw != NULL);
    (void)memcpy(cw, &meta, FC_CRC_META_SIZE);
}

void fc_data_path_crc_write(uint8_t *cw)
{
    uint32_t crc;

    ASSERT(cw != NULL);

    /* 对 [META+CRC+USER] 的 USER 部分（512 字节）计算 CRC，存入 CRC 字段 */
    crc = fc_crc_compute(cw + FC_CRC_USER_OFFSET);
    (void)memcpy(cw + FC_CRC_META_SIZE, &crc, FC_CRC_FIELD_SIZE);
}

void fc_data_path_scramble(uint8_t *cw, uint32_t seed)
{
    ASSERT(cw != NULL);

    /* 加扰区 [8, 520)：META/CRC 保持明文（读路径靠它自举种子）。
     * 加解扰是同一个 XOR 变换（自反），故只有一个函数。 */
    (void)fc_random_transform(cw + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE, seed);
}

fc_status_t fc_data_path_ecc_encode(uint8_t *cw)
{
    fc_status_t result;

    ASSERT(cw != NULL);

    result = fc_ldpc_encode(&FC_DATA_PATH_LDPC_CFG, cw, FC_BUFFER_DATA_SIZE);

    /* 引擎未实现（NOT_SUPPORTED）视为跳过——与管线原行为一致，不当作错误 */
    return (result == FC_STATUS_NOT_SUPPORTED) ? FC_STATUS_OK : result;
}

fc_status_t fc_data_path_ecc_decode(uint8_t *cw, uint8_t *ecc_bits)
{
    ASSERT(cw != NULL);

    /* 不吞 NOT_SUPPORTED：调用者（Pass 1）要把它当「成功但零纠正」处理 */
    return fc_ldpc_decode(&FC_DATA_PATH_LDPC_CFG, cw, FC_BUFFER_DATA_SIZE, ecc_bits);
}

uint8_t fc_data_path_crc_check(const uint8_t *cw)
{
    uint32_t stored_crc;
    uint32_t computed_crc;

    ASSERT(cw != NULL);

    /* 读取之前编码时写入的 CRC 值 [4..8]，重算 USER 数据的 CRC 比对 */
    (void)memcpy(&stored_crc, cw + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE);
    computed_crc = fc_crc_compute(cw + FC_CRC_USER_OFFSET);

    return (computed_crc == stored_crc) ? 1U : 0U;
}

uint32_t fc_data_path_meta_check(const uint8_t *cw, uint32_t command_meta, nand_paa_t paa)
{
    uint32_t read_meta;
    uint32_t expected_low;

    ASSERT(cw != NULL);

    (void)memcpy(&read_meta, cw, FC_CRC_META_SIZE);

    /* 自洽校验：META 低 3 位必须就是本 CW 的 plane/cw。**先抓这个**——
     * 种子取自 META（Pass 2），META 损坏 ⇒ 种子也错 ⇒ 解出的 USER 是垃圾，
     * 而 ECC/CRC 都查不出来（CRC 在加扰区外、是明文，但它保护的是 USER，
     * 种子错时 CRC 必然不过——不过那已被记成 CRC_ERROR，语义不如这里直接）。
     * ⚠ 必须用 paa_to_plane/paa_to_cw 而非链走位：cw_bitmap 不满时
     *   两者不同（v22 已确立）。
     * ⚠ command_meta == FC_META_UNKNOWN 时第二项被跳过，本函数只剩这项 3 位自洽；
     *   META 的其余保护来自「种子耦合」——见 fc_nand_design.md §5.3 的 META 保护矩阵。 */
    expected_low = ((uint32_t)paa_to_plane(paa) << PBA_PLANE_SHIFT) | (uint32_t)paa_to_cw(paa);

    if ((read_meta & PLANE_CW_MASK) != expected_low)
    {
        return FC_CW_STATUS_MATA_ERROR;
    }

    /* 自洽但与本命令的 LBA 不符 = 读到了别的页（GC 路径 meta 为
     * FC_META_UNKNOWN，跳过本项，只做上面的自洽校验）。 */
    if ((command_meta != FC_META_UNKNOWN) &&
        (read_meta != lba_to_meta(command_meta, (uint8_t)paa_to_plane(paa), (uint8_t)paa_to_cw(paa))))
    {
        return FC_CW_STATUS_MATA_ERROR;
    }

    return 0U;
}

/* ══════════════════════════════════════════════════════════
 * 编码 — FC 内部调用
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_data_path_encode
 *   功能：写路径全自动管线——META 填充 → CRC → 加扰 → ECC 编码。
 *   参数：command —— 待写命令（desc 链已配置）。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / 数据路径错误。
 *   实现要点：
 *     - Stage 0：META 写入——每个 CW 的 meta 拷贝到 buffer[0..4]。
 *     - Stage 1：CRC 编码（config.crc 使能时）——对 USER 区算 CRC 存 buffer[4..8]。
 *     - Stage 1：加扰（config.random 使能时）——seed 由本 CW 的 **META（扇区 LBA）** 派生；
 *       加扰区 [FC_CRC_USER_OFFSET, FC_BUFFER_DATA_SIZE) —— **META/CRC 保持明文**，
 *       读路径才能从页内 META 自举出种子（GC / 掉电重建不知道 LBA）。
 *     - Stage 2：ECC 编码（非 LDPC_RAW 模式）——对 520B 数据区编码，8B parity 写入。
 *     - Stage 3 已被移除：跨命令 stripe RAID 在 generate_stripe_parity() 单独处理。
 * -------------------------------------------------------------------- */
fc_status_t fc_data_path_encode(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *buffer;
    fc_status_t        result;

    /* 入参前置条件由调用者保证（fc.c 写路径传入有效命令） */
    ASSERT(command != NULL);

    /* ── Stage 0: META 写入 — 每个 CW 的 FAA 存入 buffer[0..4] ── */
    {
        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            /* desc_id 来自 command->desc_id 链，链上 ID 必在池范围内 */
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            ASSERT(desc != NULL);

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                fc_data_path_meta_write(buffer, desc->meta);
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
            /* desc_id 来自 command->desc_id 链，链上 ID 必在池范围内 */
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            ASSERT(desc != NULL);

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                fc_data_path_crc_write(buffer);
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
            /* desc_id 来自 command->desc_id 链，链上 ID 必在池范围内 */
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            ASSERT(desc != NULL);

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                /* 种子 = 本 CW 的逻辑地址（META 字段），与物理位置解耦：
                 * GC 搬页后逻辑地址不变 ⇒ 密文不变，无需解扰再重加扰。 */
                fc_data_path_scramble(buffer, fc_random_seed_from_meta(desc->meta));
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Stage 2: ECC 编码 ── */
    if (command->config.ldpc != FC_ECC_MODE_LDPC_RAW)
    {
        for (desc_id = (fc_descriptor_id_t)command->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            /* desc_id 来自 command->desc_id 链，链上 ID 必在池范围内 */
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            ASSERT(desc != NULL);

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                result = fc_data_path_ecc_encode(buffer);
                if (result != FC_STATUS_OK)
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

/* --------------------------------------------------------------------
 * fc_data_path_generate_stripe_parity
 *   功能：跨命令 XOR 生成 parity 并对其做 ECC 编码。
 *   参数：data_cmds —— 数据命令数组；data_count —— 数据命令数；
 *         parity_cmd —— parity 命令（desc+buffer 已分配）。
 *   返回：FC_STATUS_OK；ECC 编码失败时透传其返回码。
 *         参数/desc 链损坏的前置违反一律 ASSERT（v20，见 §十五）。
 *   实现要点：
 *     - parity 的 CW 数须与数据命令一致（跨命令按 CW 对齐）。
 *     - 数据区 FC_BUFFER_DATA_SIZE=520B 参与 XOR；ECC parity 区
 *       [520..528] 不参与（由后续 ECC 编码生成）。
 *     - XOR 累加：首条数据命令 memcpy 作初值，后续按 32-bit 字异或。
 * -------------------------------------------------------------------- */
fc_status_t fc_data_path_generate_stripe_parity(
    const fc_command_t *data_cmds[],
    uint8_t             data_count,
    const fc_command_t *parity_cmd)
{
    uint8_t            cw_count;
    uint8_t            cw;
    fc_descriptor_id_t desc_id;
    uint8_t            d;

    /* 入参由 fc_stripe_submit 组装（stripe 路径），非外部输入 */
    ASSERT(data_cmds != NULL);
    ASSERT(data_cmds[0] != NULL);
    ASSERT(parity_cmd != NULL);
    ASSERT(data_count != 0U);

    /* RAID XOR 按 CW 配对：两 plane 必须访问同一组 CW，flat 掩码（不对称）不兼容。
     * 掩码由 **desc 链重建**（v13：链是 CW 集合的唯一真值，不回读 FTL 的输入字段）——
     * parity 链此时已在 fc_stripe_submit ③ 挂好，故重建结果即其真实访问集合。 */
    ASSERT(fc_cw_bitmap_is_plane_symmetric(fc_get_cmd_cw_bitmap(data_cmds[0])));
    ASSERT(fc_cw_bitmap_is_plane_symmetric(fc_get_cmd_cw_bitmap(parity_cmd)));

    /* parity 命令的 CW 数量必须与数据命令一致（stripe 命令 plane 对称，
     * 故 config.cw+1 即**每-plane** CW 数） */
    cw_count = (uint8_t)(data_cmds[0]->config.cw + 1U);
    ASSERT((uint8_t)(parity_cmd->config.cw + 1U) == cw_count);

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

        /* 获取 parity 命令第 cw 个 CW 的 buffer 基址。
         * 链内 id 由 fc_descriptor_id_to_descriptor 内部断言（恒在池内），buffer 由
         * fc_buffer_get_base 内部断言（恒已占用）——两处 NULL 判都是死守卫，转 ASSERT（v20）。 */
        desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
        for (uint8_t s = 0U; s < cw; s++)
        {
            fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
            ASSERT(dsc != NULL);
            desc_id = (fc_descriptor_id_t)dsc->next_id;
        }
        {
            fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
            ASSERT(dsc != NULL);
            parity_buf = fc_buffer_get_base(dsc->buffer_id);
            ASSERT(parity_buf != NULL);
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
                ASSERT(dsc != NULL);
                data_desc_id = (fc_descriptor_id_t)dsc->next_id;
            }
            {
                fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(data_desc_id);
                ASSERT(dsc != NULL);
                data_buf = fc_buffer_get_base(dsc->buffer_id);
                ASSERT(data_buf != NULL);
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
                uint32_t       *parity32 = (uint32_t *)(void *)parity_buf;
                const uint32_t *data32   = (const uint32_t *)(const void *)data_buf;
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
        for (desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
             desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            ASSERT(desc != NULL);

            uint8_t *buf = fc_buffer_get_base(desc->buffer_id);
            if (buf != NULL)
            {
                fc_status_t result;
                result = fc_data_path_ecc_encode(buf);
                if (result != FC_STATUS_OK)
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

/* --------------------------------------------------------------------
 * fc_data_path_process_read
 *   功能：读路径 Stage 1——ECC 解码 + 条件解扰 + CRC 校验 + META 读回校验。
 *   参数：command —— 已完成 NAND 读取的命令。
 *   返回：FC_STATUS_OK（全部 CW 成功，buffer 已解扰）/
 *         FC_STATUS_IO_ERROR（存在不可纠 CW，buffer 保持 scrambled+corrected）/
 *         INVALID_ARGUMENT。
 *   实现要点：
 *     - Pass 1：逐 CW ECC 解码；失败置 UNC 并记录 all_ok=0。
 *     - 有失败直接返回 IO_ERROR（不解扰，供 FTL RAID 在 scrambled domain 恢复）。
 *     - Pass 2：全部成功才解扰——seed **从页内明文 META 自举**（META 在加扰区之外，
 *       v24），故 GC / 掉电重建不必预先知道 LBA。
 *     - Pass 3：CRC 校验（使能时）——失败置 CRC_ERROR 位。
 *     - Pass 4：META 读回 + 两项校验（v24）：① 自洽（低 3 位 == 本 desc 的 plane/cw）；
 *       ② 与本命令的 LBA 比对（command->meta 为 FC_META_UNKNOWN 时跳过）。任一不符置 MATA_ERROR。
 * -------------------------------------------------------------------- */
fc_status_t fc_data_path_process_read(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    fc_descriptor_t   *desc;
    uint8_t           *buffer;
    fc_status_t        result;
    uint8_t            all_ok = 1U;
    uint8_t            cw;

    /* 入参前置条件由调用者保证（fc.c 读路径 Stage 1） */
    ASSERT(command != NULL);

    /* 链长度即 CW 数（不再由 config.cw / multi_plane 推导；flat 掩码下两者会分叉） */
    uint8_t total_desc = 0U;
    {
        fc_descriptor_id_t did = (fc_descriptor_id_t)command->desc_id;

        while ((did < FC_DESCRIPTOR_POOL_DEPTH) && (total_desc < CW_TOTAL_MAX))
        {
            total_desc++;
            did = (fc_descriptor_id_t)fc_descriptor_id_to_descriptor(did)->next_id;
        }
    }

    /* ── Pass 1: ECC 解码所有 CW ── */
    desc_id = (fc_descriptor_id_t)command->desc_id;

    for (cw = 0U; cw < total_desc; cw++)
    {
        /* desc 链长度 = total_desc，循环内 desc_id 始终有效 */
        desc = fc_descriptor_id_to_descriptor(desc_id);
        ASSERT(desc != NULL);

        /* configure 流程为每个 desc 分配了 buffer，buffer_id 必有效 */
        buffer = fc_buffer_get_base(desc->buffer_id);
        ASSERT(buffer != NULL);

        if (command->config.ldpc != FC_ECC_MODE_LDPC_RAW)
        {
            uint8_t ecc_bits = 0U;
            result           = fc_data_path_ecc_decode(buffer, &ecc_bits);

            if (result == FC_STATUS_IO_ERROR)
            {
                /* ECC 不可纠：buffer 保持 scrambled，desc status 置 UNC */
                desc->status = FC_CW_STATUS_UNC;
                all_ok       = 0U;
            }
            else if ((result == FC_STATUS_OK) || (result == FC_STATUS_NOT_SUPPORTED))
            {
                /* ECC 成功（或跳过）：desc status 置 READY，记录纠正的 bit 数 */
                desc->status   = FC_CW_STATUS_READY;
                desc->ecc_bits = ecc_bits;
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
    if (command->config.random == 0U) /* FC_RANDOM_ENABLE */
    {
        desc_id = (fc_descriptor_id_t)command->desc_id;

        for (cw = 0U; cw < total_desc; cw++)
        {
            desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                break;
            }

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                /* 种子**自举**：META 在加扰区之外、保持明文，直接读回本 CW 的扇区 LBA
                 * 当种子——GC / 掉电重建因此不必预先知道 LBA（v24）。 */
                uint32_t cw_meta;
                (void)memcpy(&cw_meta, buffer, FC_CRC_META_SIZE);
                fc_data_path_scramble(buffer, fc_random_seed_from_meta(cw_meta));
            }

            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Pass 3: CRC 校验 ── */
    if (command->config.crc == FC_CRC_ENABLE)
    {
        desc_id = (fc_descriptor_id_t)command->desc_id;

        for (cw = 0U; cw < total_desc; cw++)
        {
            desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                break;
            }

            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                /*
                 * CRC 校验失败：标记 desc status。
                 * 虽然 ECC 纠错成功且解扰完成，但数据内容不正确，
                 * 可能是 ECC 误纠或数据路径逻辑错误。
                 */
                if (fc_data_path_crc_check(buffer) == 0U)
                {
                    desc->status |= FC_CW_STATUS_CRC_ERROR;
                }
            }

            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ── Pass 4: META 读回 + 校验 ── */
    {
        desc_id = (fc_descriptor_id_t)command->desc_id;
        for (cw = 0U; cw < total_desc; cw++)
        {
            desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                break;
            }
            buffer = fc_buffer_get_base(desc->buffer_id);
            if (buffer != NULL)
            {
                uint32_t read_meta;
                (void)memcpy(&read_meta, buffer, FC_CRC_META_SIZE);
                desc->meta = read_meta;

                /* 两项校验（自洽 + 与本命令 LBA 比对）见 fc_data_path_meta_check；
                 * 返回 0（通过）时 OR 进去是空操作。 */
                desc->status |= fc_data_path_meta_check(buffer, command->meta, desc->paa);
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
 *
 * 编译开关：CONFIG_FC_RAID (include/fc/fc.h)
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

/* --------------------------------------------------------------------
 * fc_data_path_raid_recover
 *   功能：读路径 Stage 2——RAID XOR 重建失败 CW + ECC 重试 + 解扰。
 *   参数：cmd —— 命令；failed_cw_idx —— 失败 CW 在 desc 链中的索引。
 *   返回：FC_STATUS_OK（恢复成功）/
 *         FC_STATUS_IO_ERROR（RAID 重建后 ECC 仍失败，硬件不可恢复）/
 *         INVALID_ARGUMENT。
 *   实现要点：
 *     - 收集全 CW buffer+PAA，XOR 重建 failed CW（scrambled domain 内）。
 *     - ECC 重试：重建后仍不可纠 → 返回 IO_ERROR。
 *     - 解扰所有 CW（seed 从各 CW 的明文 META 自举）。
 *     - 沿 desc 链把所有 desc status 置 READY。
 * -------------------------------------------------------------------- */
fc_status_t fc_data_path_raid_recover(const fc_command_t *cmd, uint8_t failed_cw_idx)
{
    uint8_t           *buffers[CW_TOTAL_MAX];
    uint8_t            count;
    fc_descriptor_id_t desc_id;
    uint8_t            i;

    /* 入参前置条件由调用者保证（FTL 仅在带 desc 链的命令 ECC 失败时触发） */
    ASSERT(cmd != NULL);
    ASSERT(cmd->desc_id != FC_DESCRIPTOR_ID_INVALID);

    /* ── 收集所有 buffer 指针（索引对齐 desc 链位置） ── */
    count = fc_collect_decode_buffers(cmd, buffers, CW_TOTAL_MAX);

    /* failed_cw_idx 由 FTL 沿同一条 desc 链定位，必在 [0, count) */
    ASSERT(failed_cw_idx < count);

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
        fc_status_t ecc_result;

        ecc_result = fc_data_path_ecc_decode(buffers[failed_cw_idx], NULL);
        if (ecc_result == FC_STATUS_IO_ERROR)
        {
            /* RAID 重建后 ECC 仍失败 → 硬件层面不可恢复 */
            return FC_STATUS_IO_ERROR;
        }
    }

    /* ── 解扰所有 CW ── */
    if (cmd->config.random == 0U) /* FC_RANDOM_ENABLE */
    {
        for (i = 0U; i < count; i++)
        {
            if (buffers[i] != NULL)
            {
                /* 种子自举：META 明文（加扰区外）。重建出的 CW 其 META 由 XOR 线性还原，
                 * 与其它 CW 一样可直接读回（v24）。 */
                uint32_t cw_meta;
                (void)memcpy(&cw_meta, buffers[i], FC_CRC_META_SIZE);
                fc_data_path_scramble(buffers[i], fc_random_seed_from_meta(cw_meta));
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