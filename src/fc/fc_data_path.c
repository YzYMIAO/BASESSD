/*
 * FC 数据路径管线（含 CRC / 加扰 / RAID 引擎实现）：
 *
 * Write (encode) 方向 — FC 内部在 NAND Program 提交前调用，对 FTL 透明：
 *   Random 加扰 → ECC 编码 → RAID XOR 编码
 *
 * Read (decode) 方向 — 分两阶段：
 *   Stage 1: fc_data_path_process_read()
 *     NAND Read 完成后、CQ push 前，FC 内部自动调用。
 *     遍历 desc 链：ECC 解码 → 更新 desc->status。
 *     全部成功才解扰；有失败则保持加扰（供 RAID XOR 在 scrambled domain 恢复）。
 *     **擦除页单独识别**（整条 CW 的 1 比特占比 ≥ 99% ⇒ FC_CW_STATUS_EMPTY）：它不是读错误，
 *     跳过解扰/CRC/META 校验；否则 GC 读到半写块的空洞页会把整块误标坏（RISK-GC-11）。
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

#include "assertion.h"
#include "crc.h"
#include "fc.h"
#include "fc_crc_engine.h"

#include <stddef.h>
#include <string.h>

/* 擦除页判据的阈值：整条 CW（528B = 4224 bit）中「1」的占比，单位 %。
 *
 * 推导（三种页的 1 占比）：
 *   · 数据页     —— 写入时 RANDOM_ENABLE 加扰 ⇒ 约 50%；
 *   · 元数据页   —— 不加扰，但 META(4B) + CRC(4B) + ECC parity(8B) 必写非 FF
 *                   ⇒ 即使 USER 区全是 0xFF，1 占比上界 ≈ 1 - 16/528 ≈ 97%；
 *   · 擦除页     —— 100%（读扰动只让极少数位变 0）。
 * 取 99%：与「编程过的页」最近的情形（97%）留 2 个百分点，同时容忍擦除页
 * 最多 42 bit 的读扰动。
 * 方向性：阈值取低 ⇒ 把编程过的页判成空页（在未擦页上编程，介质损坏）；
 *         阈值取高 ⇒ 把空页判成写过（多擦一次，只花磨损）。故取高一侧。 */
#define FC_CW_BLANK_ONES_PCT (99U)

/**
 * @brief 判断一条 CW 是否为擦除页（1 比特占比 ≥ 99%）
 *
 * 只在 ECC 解码失败后调用；解码成功的页不适用，避免误吞真实数据。
 *
 * @param cw 528B CW buffer（META + CRC + USER + ECC parity）
 * @return true 物理上从未编程
 */
static bool fc_cw_is_blank(const BYTE *cw)
{
    DWORD ones = 0U;

    for (DWORD i = 0U; i < FC_BUFFER_POOL_CW_SIZE; i++)
    {
        ones += (DWORD)__builtin_popcount((unsigned)cw[i]);
    }

    return ((ones * 100U) >= (FC_BUFFER_POOL_CW_SIZE * 8U * FC_CW_BLANK_ONES_PCT));
}

#if CONFIG_FC_RAID
/**
 * @brief 沿 desc 链收集命令所有 CW 的 buffer 基址
 *
 * 索引与 desc 链位置对齐，供 RAID 解码路径操作全 CW。
 *
 * @param cmd       命令
 * @param buffers   输出指针数组
 * @param max_count 容量上限（CW_TOTAL_MAX）
 * @return 收集到的 CW 数量
 */
static BYTE fc_collect_decode_buffers(const fc_command_t *cmd, BYTE *buffers[], BYTE max_count)
{
    fc_descriptor_id_t desc_id;
    BYTE               count = 0U;

    for (desc_id = (fc_descriptor_id_t)cmd->desc_id; (desc_id != FC_DESCRIPTOR_ID_INVALID) && (count < max_count);)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL)
        {
            break;
        }

        // 解码路径需要操作全 CW，取基址
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

// LDPC 参数全仓唯一一份：encode / process_read / stripe_parity / raid_recover
// 共用本表，不各写字面量。
static const fc_ldpc_config_t FC_DATA_PATH_LDPC_CFG = {
    .codeword_bytes = FC_BUFFER_DATA_SIZE,
    .parity_bytes   = 8U,
    .max_iterations = 4U,
};
/**
 * @brief 把本 CW 的 META 写入 CW 头部 [0..4)
 *
 * @param cw   528B CW buffer
 * @param meta 扇区 LBA 派生的 META
 */
void fc_data_path_meta_write(BYTE *cw, DWORD meta)
{
    ASSERT(cw != NULL);
    (void)memcpy(cw, &meta, FC_CRC_META_SIZE);
}
/**
 * @brief 对明文 USER 区算 CRC 并写入 CW 的 CRC 字段 [4..8)
 *
 * @param cw 528B CW buffer（META 已写入）
 */
void fc_data_path_crc_write(BYTE *cw)
{
    DWORD crc;

    ASSERT(cw != NULL);

    // 对 [META+CRC+USER] 的 USER 部分（512 字节）计算 CRC，存入 CRC 字段
    crc = fc_crc_compute(cw + FC_CRC_USER_OFFSET);
    (void)memcpy(cw + FC_CRC_META_SIZE, &crc, FC_CRC_FIELD_SIZE);
}
/**
 * @brief 加扰/解扰 CW 的 USER 区（XOR 自反）
 *
 * 加扰区 [8, 520)：META/CRC 保持明文，读路径才能从页内 META 自举种子。
 *
 * @param cw   528B CW buffer
 * @param seed 由本 CW META 派生
 */
void fc_data_path_scramble(BYTE *cw, DWORD seed)
{
    ASSERT(cw != NULL);

    // 加扰区 [8, 520)：META/CRC 保持明文（读路径靠它自举种子）。
    // 加解扰是同一个 XOR 变换（自反），故只有一个函数。
    (void)fc_random_transform(cw + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE, seed);
}
/**
 * @brief 对 CW 数据区（520B）做 ECC 编码，parity 写入 [520, 528)
 *
 * 引擎未实现（NOT_SUPPORTED）视为跳过。
 *
 * @param cw 528B CW buffer（已加扰）
 * @return FC_STATUS_OK / 编码错误
 */
fc_status_e fc_data_path_ecc_encode(BYTE *cw)
{
    fc_status_e result;

    ASSERT(cw != NULL);

    result = fc_ldpc_encode(&FC_DATA_PATH_LDPC_CFG, cw, FC_BUFFER_DATA_SIZE);

    // 引擎未实现（NOT_SUPPORTED）视为跳过——与管线原行为一致，不当作错误
    return (result == FC_STATUS_NOT_SUPPORTED) ? FC_STATUS_OK : result;
}
/**
 * @brief 对 CW 做 ECC 解码并回填纠正 bit 数
 *
 * 不吞 NOT_SUPPORTED：调用者需把它当作「成功且零纠正」。
 *
 * @param cw       528B CW buffer
 * @param ecc_bits 输出：纠正的 bit 数（可 NULL）
 * @return FC_STATUS_OK / NOT_SUPPORTED / IO_ERROR（不可纠）
 */
fc_status_e fc_data_path_ecc_decode(BYTE *cw, BYTE *ecc_bits)
{
    ASSERT(cw != NULL);

    // 不吞 NOT_SUPPORTED：调用者（步骤 1）要把它当「成功但零纠正」处理
    return fc_ldpc_decode(&FC_DATA_PATH_LDPC_CFG, cw, FC_BUFFER_DATA_SIZE, ecc_bits);
}
/**
 * @brief 重算 USER 区 CRC 并与 CW 内存储值比对
 *
 * @param cw 已解扰的 CW buffer
 * @return true 一致；false 不一致
 */
bool fc_data_path_crc_check(const BYTE *cw)
{
    DWORD stored_crc;
    DWORD computed_crc;

    ASSERT(cw != NULL);

    // 读取之前编码时写入的 CRC 值 [4..8]，重算 USER 数据的 CRC 比对
    (void)memcpy(&stored_crc, cw + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE);
    computed_crc = fc_crc_compute(cw + FC_CRC_USER_OFFSET);

    return (computed_crc == stored_crc);
}
/**
 * @brief META 两项校验：低 3 位自洽 + 与本命令 LBA 比对
 *
 * 自洽项先查：种子取自 META，META 损坏 ⇒ 解出垃圾且只会被报成 CRC_ERROR。
 * command_meta == FC_META_UNKNOWN（GC 路径）时跳过第二项。
 *
 * @param cw           已解扰的 CW buffer
 * @param command_meta 本命令的 META，或 FC_META_UNKNOWN
 * @param paa          本 CW 的物理地址（取 plane/cw）
 * @return 0 通过；FC_CW_STATUS_MATA_ERROR 不通过
 */
DWORD fc_data_path_meta_check(const BYTE *cw, DWORD command_meta, DWORD paa)
{
    DWORD read_meta;
    DWORD expected_low;

    ASSERT(cw != NULL);

    (void)memcpy(&read_meta, cw, FC_CRC_META_SIZE);

    // 自洽校验：META 低 3 位必须就是本 CW 的 plane/cw。**先抓这个**——
    // 种子取自 META（步骤 2），META 损坏 ⇒ 种子也错 ⇒ 解出的 USER 是垃圾，
    // 而 ECC/CRC 都查不出来（CRC 在加扰区外、是明文，但它保护的是 USER，
    // 种子错时 CRC 必然不过——不过那已被记成 CRC_ERROR，语义不如这里直接）。
    // ⚠ 必须用 paa_to_plane/paa_to_cw 而非链走位：cw_bitmap 不满时
    // 两者不同。
    // ⚠ command_meta == FC_META_UNKNOWN 时第二项被跳过，本函数只剩这项 3 位自洽；
    // META 的其余保护来自「种子耦合」——见 fc_nand_design.md §5.3 的 META 保护矩阵。
    expected_low = (paa_to_plane(paa) << PLANE_CW_SHIFT) | paa_to_cw(paa);

    if ((read_meta & PLANE_CW_MASK) != expected_low)
    {
        return FC_CW_STATUS_MATA_ERROR;
    }

    // 自洽但与本命令的 LBA 不符 = 读到了别的页（GC 路径 meta 为
    // FC_META_UNKNOWN，跳过本项，只做上面的自洽校验）。
    if ((command_meta != FC_META_UNKNOWN) && (read_meta != lba_to_meta(command_meta, paa_to_plane(paa), paa_to_cw(paa))))
    {
        return FC_CW_STATUS_MATA_ERROR;
    }

    return 0U;
}

/* ══════════════════════════════════════════════════════════
 * 编码 — FC 内部调用
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 编码一条命令的全部 CW：META → CRC → 加扰 → ECC
 *
 * 四步只依赖本 CW 的 buffer，故合并为一次 desc 链遍历；先加扰再 ECC。
 * 跨命令 stripe parity 由 fc_data_path_generate_stripe_parity() 负责。
 *
 * @param command 写命令
 * @return FC_STATUS_OK / ECC 编码错误
 */
fc_status_e fc_data_path_encode(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    BYTE              *buffer;
    fc_status_e        result;

    // 入参前置条件由调用者保证（fc.c 写路径传入有效命令）
    ASSERT(command != NULL);

    // 每个 CW 的处理顺序固定：META → CRC → 加扰 → ECC；四步都只依赖
    // 本 CW 的 buffer ⇒ 合并为一次 desc 链遍历。
    for (desc_id = (fc_descriptor_id_t)command->desc_id; desc_id != FC_DESCRIPTOR_ID_INVALID;)
    {
        // desc_id 来自 command->desc_id 链，链上 ID 必在池范围内
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        ASSERT(desc != NULL);

        buffer = fc_buffer_get_base(desc->buffer_id);
        if (buffer != NULL)
        {
            // Stage 0: META 写入 — 本 CW 的 LBA 存入 buffer[0..4]
            fc_data_path_meta_write(buffer, desc->meta);

            // Stage 1: CRC 编码（覆盖明文 USER 区）
            if (command->config.crc == FC_CRC_ENABLE)
            {
                fc_data_path_crc_write(buffer);
            }

            // Stage 1: 加扰（META/CRC 保持明文；种子 = 本 CW 的逻辑地址）
            if (command->config.random == 0U)
            {
                fc_data_path_scramble(buffer, fc_random_seed_from_meta(desc->meta));
            }

            // Stage 2: ECC 编码（必须在加扰之后）
            if (command->config.ldpc != FC_ECC_MODE_LDPC_RAW)
            {
                result = fc_data_path_ecc_encode(buffer);
                if (result != FC_STATUS_OK)
                {
                    return result;
                }
            }
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }

    /* ── 跨命令 stripe RAID 由 fc_data_path_generate_stripe_parity() 处理 ── */

    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 跨 Plane Stripe RAID 编码 — FC 内部调用
 * 编译开关：CONFIG_FC_RAID (include/fc/fc.h)
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

/**
 * @brief 生成跨平面 stripe 的 parity CW 并做 ECC 编码
 *
 * parity[cw][0..520) = XOR(各数据命令对应 CW 的 [0..520))；ECC parity 区
 * 不参与 XOR，由本函数末尾的 ECC 编码生成。
 *
 * @param data_cmds  数据命令数组（plane 对称）
 * @param data_count 数据命令数（非 0）
 * @param parity_cmd parity 命令（CW 数须与数据命令一致）
 * @return FC_STATUS_OK / ECC 编码错误
 */
fc_status_e fc_data_path_generate_stripe_parity(const fc_command_t *data_cmds[], BYTE data_count, const fc_command_t *parity_cmd)
{
    BYTE               cw_count;
    BYTE               cw;
    fc_descriptor_id_t desc_id;
    BYTE               d;

    // 入参由 fc_stripe_submit 组装（stripe 路径），非外部输入
    ASSERT(data_cmds != NULL);
    ASSERT(data_cmds[0] != NULL);
    ASSERT(parity_cmd != NULL);
    ASSERT(data_count != 0U);

    // RAID XOR 按 CW 配对：两 plane 必须访问同一组 CW，flat 掩码（不对称）不兼容。
    // 掩码由 **desc 链重建**（链是 CW 集合的唯一真值，不回读 FTL 的输入字段）——
    // parity 链此时已在 fc_stripe_submit ③ 挂好，故重建结果即其真实访问集合。
    ASSERT(fc_cw_bitmap_is_plane_symmetric(fc_get_cmd_cw_bitmap(data_cmds[0])));
    ASSERT(fc_cw_bitmap_is_plane_symmetric(fc_get_cmd_cw_bitmap(parity_cmd)));

    // parity 命令的 CW 数量必须与数据命令一致（stripe 命令 plane 对称，
    // 故 config.cw+1 即**每-plane** CW 数）
    cw_count = (data_cmds[0]->config.cw + 1U);
    ASSERT((parity_cmd->config.cw + 1U) == cw_count);

    // 逐 CW 做 XOR：
    // parity_cmd->buffer[cw][0..520] = XOR(data_cmds[0..data_count-1]->buffer[cw][0..520])
    //
    // 操作的数据区为 FC_BUFFER_DATA_SIZE（520B），即 META+CRC+USER。
    // ECC parity 区 [520..528] 不参与 XOR，由 ECC 引擎在后续的 ECC 编码步骤生成。
    for (cw = 0U; cw < cw_count; cw++)
    {
        BYTE *parity_buf;
        BYTE  first = 1U;

        // 获取 parity 命令第 cw 个 CW 的 buffer 基址。
        // 链内 id 由 fc_descriptor_id_to_descriptor 内部断言（恒在池内），buffer 由
        // fc_buffer_get_base 内部断言（恒已占用）——两处 NULL 判都是死守卫，转 ASSERT。
        desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
        for (BYTE s = 0U; s < cw; s++)
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

        // XOR 所有数据命令的对应 CW 数据区
        for (d = 0U; d < data_count; d++)
        {
            BYTE              *data_buf;
            fc_descriptor_id_t data_desc_id;

            data_desc_id = (fc_descriptor_id_t)data_cmds[d]->desc_id;
            for (BYTE s = 0U; s < cw; s++)
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
                // 第一个数据命令：拷贝到 parity buffer，作为 XOR 累加器初值
                (void)memcpy(parity_buf, data_buf, FC_BUFFER_DATA_SIZE);
                first = 0U;
            }
            else
            {
                // 后续数据命令：XOR 累加
                DWORD       *parity32 = (DWORD *)(void *)parity_buf;
                const DWORD *data32   = (const DWORD *)(const void *)data_buf;
                for (WORD w = 0U; w < (FC_BUFFER_DATA_SIZE / sizeof(DWORD)); w++)
                {
                    parity32[w] ^= data32[w];
                }
            }
        }
    }

    /* ── ECC 编码 parity 命令的 buffer ── */
    if (parity_cmd->config.ldpc != FC_ECC_MODE_LDPC_RAW)
    {
        for (desc_id = (fc_descriptor_id_t)parity_cmd->desc_id; desc_id != FC_DESCRIPTOR_ID_INVALID;)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            ASSERT(desc != NULL);

            BYTE *buf = fc_buffer_get_base(desc->buffer_id);
            if (buf != NULL)
            {
                fc_status_e result;
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
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 读路径 Stage 1：ECC 解码 → 解扰 → CRC → META 校验
 *
 * 任一 CW 不可纠即返回 IO_ERROR 且不解扰，供 FTL RAID 在 scrambled domain
 * 恢复。擦除页（1 占比 ≥ 99%）置 EMPTY，不算失败。
 *
 * @param command 读命令（CQ push 前由 FC 内部调用）
 * @return FC_STATUS_OK / FC_STATUS_IO_ERROR
 */
fc_status_e fc_data_path_process_read(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    fc_descriptor_t   *desc;
    BYTE              *buffer;
    fc_status_e        result;
    BYTE               all_ok = 1U;
    BYTE               cw;

    // 入参前置条件由调用者保证（fc.c 读路径 Stage 1）
    ASSERT(command != NULL);

    // 链长度即 CW 数（不再由 config.cw / multi_plane 推导；flat 掩码下两者会分叉）
    BYTE total_desc = 0U;
    {
        fc_descriptor_id_t did = (fc_descriptor_id_t)command->desc_id;

        while ((did < FC_DESCRIPTOR_POOL_DEPTH) && (total_desc < CW_TOTAL_MAX))
        {
            total_desc++;
            did = (fc_descriptor_id_t)fc_descriptor_id_to_descriptor(did)->next_id;
        }
    }

    /* ── 步骤 1: ECC 解码所有 CW ── */
    desc_id = (fc_descriptor_id_t)command->desc_id;

    for (cw = 0U; cw < total_desc; cw++)
    {
        // desc 链长度 = total_desc，循环内 desc_id 始终有效
        desc = fc_descriptor_id_to_descriptor(desc_id);
        ASSERT(desc != NULL);

        // configure 流程为每个 desc 分配了 buffer，buffer_id 必有效
        buffer = fc_buffer_get_base(desc->buffer_id);
        ASSERT(buffer != NULL);

        if (command->config.ldpc != FC_ECC_MODE_LDPC_RAW)
        {
            BYTE ecc_bits = 0U;
            result        = fc_data_path_ecc_decode(buffer, &ecc_bits);

            if (result == FC_STATUS_IO_ERROR)
            {
                if (fc_cw_is_blank(buffer))
                {
                    // 擦除页：全 0xFF 不是合法 BCH 码字，解码必然失败——但这是
                    // 「页从未编程」，不是读错误。置 BLANK（非错误位），让上层
                    // （GC 搬移 / 掉电重建）按空页跳过，而不是把整块标坏。
                    desc->status   = FC_CW_STATUS_EMPTY;
                    desc->ecc_bits = 0U;
                }
                else
                {
                    // ECC 不可纠：buffer 保持 scrambled，desc status 置 UNC
                    desc->status = FC_CW_STATUS_UNC;
                    all_ok       = 0U;
                }
            }
            else if ((result == FC_STATUS_OK) || (result == FC_STATUS_NOT_SUPPORTED))
            {
                // ECC 成功（或跳过）：desc status 置 READY，记录纠正的 bit 数
                desc->status   = FC_CW_STATUS_UNUSED;
                desc->ecc_bits = ecc_bits;
            }
        }
        else
        {
            // LDPC RAW 模式：跳过 ECC 解码，直接标记 READY
            desc->status = FC_CW_STATUS_UNUSED;
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
    }

    if (all_ok == 0U)
    {
        // 有 CW 不可纠：
        // buffer 保持 ECC-corrected + scrambled 状态，
        // 供 FTL 后续的 RAID XOR 恢复在 same domain 进行。
        return FC_STATUS_IO_ERROR;
    }

    /* ── 步骤 2：解扰 + CRC + META（合并为一次逐 CW 遍历）──
     * 顺序不可换：解扰 → CRC → META。META/CRC 在加扰区之外，先读 META 取种子
     * 不会改变结果；擦除页永远按「无 LBA」处理（RISK-GC-12）。 */
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
            if (fc_cw_status_is_empty(desc->status))
            {
                desc->meta = FC_META_UNKNOWN;
            }
            else
            {
                // 种子自举：META 在加扰区之外、保持明文。
                DWORD cw_meta;
                (void)memcpy(&cw_meta, buffer, FC_CRC_META_SIZE);

                if (command->config.random == 0U) /* FC_RANDOM_ENABLE */
                {
                    fc_data_path_scramble(buffer, fc_random_seed_from_meta(cw_meta));
                }

                if (command->config.crc == FC_CRC_ENABLE)
                {
                    if (fc_data_path_crc_check(buffer) == false)
                    {
                        desc->status |= FC_CW_STATUS_CRC_ERROR;
                    }
                }

                desc->meta = cw_meta;
                desc->status |= fc_data_path_meta_check(buffer, command->meta, desc->paa);
            }
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
    }

    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 读路径 Stage 2 — FTL 触发
 * 编译开关：CONFIG_FC_RAID (include/fc/fc.h)
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

/**
 * @brief 读路径 Stage 2：RAID XOR 重建失败 CW 并重新解码
 *
 * 重建后 ECC 重试仍失败即硬件不可恢复；成功后解扰全 CW 并把 desc 置 READY。
 *
 * @param cmd           读命令
 * @param failed_cw_idx 失败 CW 在 desc 链中的下标
 * @return FC_STATUS_OK / FC_STATUS_IO_ERROR
 */
fc_status_e fc_data_path_raid_recover(const fc_command_t *cmd, BYTE failed_cw_idx)
{
    BYTE              *buffers[CW_TOTAL_MAX];
    BYTE               count;
    fc_descriptor_id_t desc_id;
    BYTE               i;

    // 入参前置条件由调用者保证（FTL 仅在带 desc 链的命令 ECC 失败时触发）
    ASSERT(cmd != NULL);
    ASSERT(cmd->desc_id != FC_DESCRIPTOR_ID_INVALID);

    /* ── 收集所有 buffer 指针（索引对齐 desc 链位置） ── */
    count = fc_collect_decode_buffers(cmd, buffers, CW_TOTAL_MAX);

    // failed_cw_idx 由 FTL 沿同一条 desc 链定位，必在 [0, count)
    ASSERT(failed_cw_idx < count);

    /* ── RAID XOR 重建 failed CW ── */
    {
        BYTE recovered[FC_BUFFER_POOL_CW_SIZE];

        // XOR 所有非 failed 的 CW（均为 ECC-corrected + scrambled），
        // 重建结果与被恢复 CW 处于 same scrambled domain。
        fc_raid_recover(recovered, buffers, count, failed_cw_idx, FC_BUFFER_POOL_CW_SIZE);
        (void)memcpy(buffers[failed_cw_idx], recovered, FC_BUFFER_POOL_CW_SIZE);
    }

    /* ── ECC 重试 ── */
    if (cmd->config.ldpc != FC_ECC_MODE_LDPC_RAW)
    {
        fc_status_e ecc_result;

        ecc_result = fc_data_path_ecc_decode(buffers[failed_cw_idx], NULL);
        if (ecc_result == FC_STATUS_IO_ERROR)
        {
            // RAID 重建后 ECC 仍失败 → 硬件层面不可恢复
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
                // 种子自举：META 明文（加扰区外）。重建出的 CW 其 META 由 XOR 线性还原，
                // 与其它 CW 一样可直接读回。
                DWORD cw_meta;
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
            desc->status = FC_CW_STATUS_UNUSED;
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
        i++;
    }

    return FC_STATUS_OK;
}

#endif /* CONFIG_FC_RAID */

/* ══════════════════════════════════════════════════════════
 * CRC 引擎
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 初始化硬件 CRC 外设
 *
 * 时钟使能、HAL 配置与外设应答自检都在平台层 crc_init() 内；自检不过
 * 一律 ASSERT，不再有「失败静默继续」的降级路径。
 */
void fc_crc_init(void)
{
    crc_init();
}

/**
 * @brief 计算 CW 的 512B USER 数据 CRC-32
 *
 * 平台层每次先 RESET 引擎回 INIT 值，保证独立计算；user_data 须 4 字节对齐
 * （HAL 的 WORDS 分支直接按 DWORD* 解引用）。
 *
 * @param user_data USER 区首地址
 * @return CRC-32
 */
DWORD fc_crc_compute(const BYTE *user_data)
{
    // user_data 前置条件由调用者保证
    ASSERT(user_data != NULL);

    return crc_compute(user_data, FC_CRC_USER_SIZE);
}

/**
 * @brief 校验 USER 数据 CRC 是否等于存储值
 *
 * @param user_data  USER 区首地址
 * @param stored_crc 读回的 CRC 字段
 * @return FC_CRC_RESULT_OK / FC_CRC_RESULT_ERROR
 */
BYTE fc_crc_verify(const BYTE *user_data, DWORD stored_crc)
{
    // user_data 前置条件由调用者保证
    ASSERT(user_data != NULL);

    return (fc_crc_compute(user_data) == stored_crc) ? FC_CRC_RESULT_OK : FC_CRC_RESULT_ERROR;
}

/* ══════════════════════════════════════════════════════════
 * 加扰/解扰引擎
 * ══════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════
 * 种子生成（唯一一种）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 由 CW 的 META 派生加扰种子
 *
 * 32→16 位折叠避免相差 64K 扇区的 LBA 撞种子，再做雪崩混合让相邻 LBA 充分
 * 分散。与物理地址无关：GC 搬页后逻辑地址不变 ⇒ 密文不变。
 *
 * @param cw_meta 本 CW 的 META（逻辑地址）
 * @return 非零 32 位种子
 */
DWORD fc_random_seed_from_meta(DWORD cw_meta)
{
    // 折叠：高位并进低位，保住 32 位的全部信息
    DWORD seed = cw_meta ^ (cw_meta >> 16U);

    // 雪崩混合（splitmix32 的乘法常量）
    seed *= 0x9E3779B9UL;
    seed ^= seed >> 16U;
    seed *= 0x85EBCA6BUL;
    seed ^= seed >> 13U;

    return (seed != 0U) ? seed : 0x1U;
}

/* ══════════════════════════════════════════════════════════════════
 * 加扰算法
 *
 * 三种算法差异对比：
 *
 * 算法        │ 核心机制            │ 周期       │ 每字节开销 │ 备注
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * XOR-shift  │ seed ^= seed<<a   │ 2³²-1     │ ~6 ops   │ 硬件开销极小
 *            │        ^= seed>>b │           │          │
 *            │        ^= seed<<c │           │          │
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * LFSR 16    │ x¹⁶+x¹⁴+x¹³+x¹¹+1│ 2¹⁶-1     │ ~40 ops  │ 主控最主流
 *            │ 串行逐 bit 递推    │ (65535)   │ (逐bit)  │（多项式是本工程选择，
 *            │                   │           │          │  ONFI 不规定加扰器）
 * ───────────┼───────────────────┼───────────┼──────────┼────────────────────
 * Simple XOR │ 固定 64B pattern  │ pattern   │ ~2 ops   │ 低端方案，资源受限
 *            │ 循环异或          │ 长度=64   │          │
 *
 * 注意：企业级与消费级在加扰算法上没有本质区别，都使用 LFSR 或 XOR-shift。
 * 真正的区别在于：
 * ① 企业级强制 AES-256 XTS 加密（加密后数据天然随机化）
 * ② 实现方式：企业级在 DMA 引擎中硬件 inline 完成，零 CPU 开销
 * ③ 种子维度：企业级可能混入更多维度（Die ID 等）
 * ══════════════════════════════════════════════════════════════════ */

#if (FC_RANDOM_ALGO == FC_RANDOM_ALGO_LFSR)

// 16 级 LFSR，多项式 x¹⁶ + x¹⁴ + x¹³ + x¹¹ + 1，周期 2¹⁶ - 1 = 65535。
//
// 本实现是**右移** LFSR（输出 bit0，右移，反馈进 bit15）。此方向下反馈抽头是
// 多项式系数的**镜像**：x¹⁶ → bit0、x¹⁴ → bit2、x¹³ → bit3、x¹¹ → bit5。
// fb = bit0 ^ bit2 ^ bit3 ^ bit5
//
// ⚠ 反馈必须包含**被移出的 bit0**。若照抄左移写法的 bit15^bit13^bit12^bit10，
// 新状态 16 位全部只依赖旧 bit1..bit15 ⇒ 映射不可逆（1 → 0 是吸收态）⇒ 状态
// 空间坍缩到周期 63，520B 的 CW 会退化成 63 字节图案重复 8.25 遍，加扰失效。
#    define FC_LFSR_PERIOD (65535U)

/**
 * @brief 8 步递推：输出 8 bit，返回次态。与逐 bit 版本完全一致。
 *
 * @param lfsr 当前 16 位 LFSR 状态
 * @param out  输出：本步产生的 8 bit（逐位移入）
 * @return     递推 8 步后的次态
 */
static WORD fc_lfsr_step8(WORD lfsr, BYTE *out)
{
    BYTE i;
    BYTE result = 0U;

    for (i = 0U; i < 8U; i++)
    {
        result = (BYTE)((result << 1U) | (lfsr & 1U));

        // 反馈位 = bit0 ^ bit2 ^ bit3 ^ bit5（含被移出的 bit0）
        const BYTE fb = (BYTE)((lfsr ^ (lfsr >> 2U) ^ (lfsr >> 3U) ^ (lfsr >> 5U)) & 1U);

        lfsr = (WORD)((lfsr >> 1U) | ((WORD)fb << 15U));
    }

    *out = result;
    return lfsr;
}

// 16 位状态 → 8 步输出/次态是 GF(2) 线性变换 ⇒ 可按低/高字节分解：
// out  = out_lo[lo]  ^ out_hi[hi]
// next = next_lo[lo] ^ next_hi[hi]
// 4 张 256 项表约 1.5 KB；每字节 2 次查表 + 2 次异或，替代 8 次移位。
typedef struct
{
    BYTE out_lo[256];
    BYTE out_hi[256];
    WORD next_lo[256];
    WORD next_hi[256];
} fc_lfsr_tab_t;

static fc_lfsr_tab_t s_lfsr_tab;
static bool          s_lfsr_tab_ready = false;

/**
 * @brief 构造 LFSR 的 4 张查表（低/高字节各一组 out/next）。
 *
 * 表建立后置 s_lfsr_tab_ready，后续调用不再重建。
 */
static void fc_lfsr_tab_build(void)
{
    for (WORD v = 0U; v < 256U; v++)
    {
        s_lfsr_tab.next_lo[v] = fc_lfsr_step8((WORD)v, &s_lfsr_tab.out_lo[v]);
        s_lfsr_tab.next_hi[v] = fc_lfsr_step8((WORD)(v << 8U), &s_lfsr_tab.out_hi[v]);
    }

    s_lfsr_tab_ready = true;
}

#    ifndef NDEBUG
/**
 * @brief 一次性自检：查表路径必须与逐 bit 递推给出完全相同的 64 字节。
 */
static void fc_lfsr_tab_selfcheck(void)
{
    static const WORD seeds[] = {1U, 0x1234U, 0xFFFFU};

    for (BYTE s = 0U; s < (BYTE)(sizeof(seeds) / sizeof(seeds[0])); s++)
    {
        WORD tab_state = seeds[s];
        WORD ref_state = seeds[s];

        for (BYTE i = 0U; i < 64U; i++)
        {
            const WORD lo      = (WORD)(tab_state & 0xFFU);
            const WORD hi      = (WORD)(tab_state >> 8U);
            const BYTE tab_out = (BYTE)(s_lfsr_tab.out_lo[lo] ^ s_lfsr_tab.out_hi[hi]);

            tab_state = (WORD)(s_lfsr_tab.next_lo[lo] ^ s_lfsr_tab.next_hi[hi]);

            BYTE ref_out = 0U;
            ref_state    = fc_lfsr_step8(ref_state, &ref_out);

            ASSERT(tab_out == ref_out);
        }
    }
}
#    endif

/**
 * @brief 取下一个伪随机字节（查表版；调用前表已建立）。
 *
 * @param state 输入输出：LFSR 状态，函数内推进到次态
 * @return      本步产生的伪随机字节（用作异或掩码）
 */
static BYTE fc_lfsr_next(WORD *state)
{
    const WORD lo  = (WORD)(*state & 0xFFU);
    const WORD hi  = (WORD)(*state >> 8U);
    const BYTE out = (BYTE)(s_lfsr_tab.out_lo[lo] ^ s_lfsr_tab.out_hi[hi]);

    *state = (WORD)(s_lfsr_tab.next_lo[lo] ^ s_lfsr_tab.next_hi[hi]);
    return out;
}

/**
 * @brief 用 16 级 LFSR 对 data 原地加扰/解扰。
 *
 * 首次调用时建立查表，debug 档另做一次查表与逐 bit 递推的等价自检。
 *
 * @param data   待变换的数据缓冲（原地修改）
 * @param length 数据字节数
 * @param seed   加扰种子；低 16 位作 LFSR 初态，为 0 时退化为 1
 * @return       FC_STATUS_OK
 */
static fc_status_e fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    // data 前置条件由调用者保证（fc_data_path 传入 CW 缓冲）
    ASSERT(data != NULL);

    if (s_lfsr_tab_ready == false)
    {
        fc_lfsr_tab_build();
#    ifndef NDEBUG
        fc_lfsr_tab_selfcheck();
#    endif
    }

    // 用 seed 初始化为非零 LFSR 状态
    WORD lfsr_state = (WORD)(seed & 0xFFFFU);
    if (lfsr_state == 0U)
    {
        lfsr_state = 1U;
    }

    WORD index = 0U;

    // 4 字节一组，减少循环开销；尾字节逐个处理。
    while ((DWORD)index + 3U < (DWORD)length)
    {
        data[index] ^= fc_lfsr_next(&lfsr_state);
        data[index + 1U] ^= fc_lfsr_next(&lfsr_state);
        data[index + 2U] ^= fc_lfsr_next(&lfsr_state);
        data[index + 3U] ^= fc_lfsr_next(&lfsr_state);
        index = (WORD)(index + 4U);
    }

    while (index < length)
    {
        data[index] ^= fc_lfsr_next(&lfsr_state);
        index++;
    }

    return FC_STATUS_OK;
}

#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_SIMPLE_XOR)

// 简单 XOR：用 seed 生成固定长度的伪随机 pattern，然后按字节异或。
// 每 length 字节重复 pattern 绕回。
// 适合资源极度受限的场景。
#    define FC_SIMPLE_XOR_PATTERN_LEN (64U)

/**
 * @brief 用 64 字节伪随机 pattern 循环异或 data。
 *
 * @param data   待变换的数据缓冲（原地修改）
 * @param length 数据字节数
 * @param seed   加扰种子，作为 XOR-shift 递推初值生成 pattern
 * @return       FC_STATUS_OK
 */
static fc_status_e fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    BYTE pattern[FC_SIMPLE_XOR_PATTERN_LEN];
    BYTE i;
    WORD index;

    // data 前置条件由调用者保证（fc_data_path 传入 CW 缓冲）
    ASSERT(data != NULL);

    // 用 seed 生成 64 字节 pattern
    for (i = 0U; i < FC_SIMPLE_XOR_PATTERN_LEN; i++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        pattern[i] = seed;
    }

    // 用 pattern 循环异或数据
    for (index = 0U; index < length; index++)
    {
        data[index] ^= pattern[index % FC_SIMPLE_XOR_PATTERN_LEN];
    }
    return FC_STATUS_OK;
}

#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_XORSHIFT)

/**
 * @brief 用 32 位 XOR-shift 递推逐字节异或 data。
 *
 * @param data   待变换的数据缓冲（原地修改）
 * @param length 数据字节数
 * @param seed   加扰种子，作为 XOR-shift 递推初值（调用者保证非零）
 * @return       FC_STATUS_OK
 */
static fc_status_e fc_random_transform_impl(BYTE *data, WORD length, DWORD seed)
{
    WORD index;

    // data 前置条件由调用者保证；seed 由 fc_random_seed_from_meta 保证非零
    ASSERT(data != NULL);
    ASSERT(seed != 0U);

    for (index = 0U; index < length; index++)
    {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        data[index] ^= seed;
    }
    return FC_STATUS_OK;
}

#endif

/* ══════════════════════════════════════════════════════════════════
 * 公接口
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 加扰/解扰（XOR 对称，算法由编译宏决定）
 *
 * @param data   数据缓冲（原地修改）
 * @param length 字节数
 * @param seed   fc_random_seed_from_meta 给出的种子
 * @return FC_STATUS_OK
 */
fc_status_e fc_random_transform(BYTE *data, WORD length, DWORD seed)
{
    return fc_random_transform_impl(data, length, seed);
}

/**
 * @brief 当前编译出的加扰算法名
 *
 * @return 算法名（调试输出用）
 */
const char *fc_random_algo_name(void)
{
#if (FC_RANDOM_ALGO == FC_RANDOM_ALGO_XORSHIFT)
    return "XOR-shift 32";
#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_LFSR)
    return "LFSR 16 (x^16+x^14+x^13+x^11+1)";
#elif (FC_RANDOM_ALGO == FC_RANDOM_ALGO_SIMPLE_XOR)
    return "Simple XOR (64-byte pattern)";
#else
    return "Unknown";
#endif
}

/* ══════════════════════════════════════════════════════════
 * Stripe RAID 引擎
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

// RAID XOR 引擎
//
// 原理：parity = CW0 ^ CW1 ^ ... ^ CWn-1。
// 任一 CW 丢失（ECC 失败）时可用 recovered = 其余 CW ^ parity 重建
// （RAID-5 单盘容错的 XOR 性质，异或自逆）。
//
// 两套接口：
// - 上下文式（start/accumulate/end）：写路径跨命令累积 parity。
// - 静态恢复（recover）：读路径 ECC 失败后无上下文重建。
//
// 全局静态数组：
// g_raid_ctx[LUN_NUM]   — 每个 LUN 的 RAID 上下文
// g_raid_shadow[LUN_NUM] — 影子缓冲（HSQ 打断 save/restore）

static fc_raid_context_t g_raid_ctx[LUN_NUM];
static fc_raid_context_t g_raid_shadow[LUN_NUM];

/**
 * @brief 清空该 LUN 的 RAID 上下文与影子缓冲
 *
 * @param lun LUN 号（< LUN_NUM）
 */
void fc_raid_init_ctx(BYTE lun)
{
    // 前置条件由调用者保证（fc.c 按 LUN_NUM 循环传入）
    ASSERT(lun < LUN_NUM);

    memset(&g_raid_ctx[lun], 0, sizeof(fc_raid_context_t));
    memset(&g_raid_shadow[lun], 0, sizeof(fc_raid_context_t));
}

/**
 * @brief 以第一个 CW 作为 XOR 累加器初值，启动一次 parity 累积
 *
 * @param lun       LUN 号（< LUN_NUM）
 * @param cw_buffer 首个 CW（528B）
 * @param total     本组 CW 总数（含首个）
 * @return FC_STATUS_OK / FC_STATUS_NOT_READY（该 LUN 已有活跃序列）
 */
fc_status_e fc_raid_start(BYTE lun, const BYTE *cw_buffer, BYTE total)
{
    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(cw_buffer != NULL);
    ASSERT(total != 0U);

    fc_raid_context_t *ctx = &g_raid_ctx[lun];

    if (ctx->active == true)
    {
        return FC_STATUS_NOT_READY;
    }

    memcpy(ctx->acc_buf, cw_buffer, FC_BUFFER_POOL_CW_SIZE);
    ctx->active    = true;
    ctx->processed = 1U;
    ctx->total     = total;
    return FC_STATUS_OK;
}

/**
 * @brief 把一个 CW 按 32 位字异或进累加器
 *
 * @param lun       LUN 号（< LUN_NUM）
 * @param cw_buffer CW（528B）
 * @return FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（无活跃序列）
 */
fc_status_e fc_raid_accumulate(BYTE lun, const BYTE *cw_buffer)
{
    WORD         i;
    DWORD       *acc32;
    const DWORD *buf32;

    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(cw_buffer != NULL);

    fc_raid_context_t *ctx = &g_raid_ctx[lun];

    if (ctx->active == false)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    // 超累加 = 调用者计数 bug（total 由 fc_raid_start 给定）→ ASSERT
    ASSERT(ctx->processed < ctx->total);

    // 32 位对齐 XOR
    acc32 = (DWORD *)(void *)ctx->acc_buf;
    buf32 = (const DWORD *)(const void *)cw_buffer;
    for (i = 0; i < (FC_BUFFER_POOL_CW_SIZE / sizeof(DWORD)); i++)
    {
        acc32[i] ^= buf32[i];
    }

    ctx->processed++;
    return FC_STATUS_OK;
}

/**
 * @brief 把累加器结果拷出为 parity CW 并复位序列状态
 *
 * @param lun           LUN 号（< LUN_NUM）
 * @param parity_buffer 输出：528B parity
 * @return FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（无活跃序列）
 */
fc_status_e fc_raid_end(BYTE lun, BYTE *parity_buffer)
{
    // 入参前置条件由调用者保证
    ASSERT(lun < LUN_NUM);
    ASSERT(parity_buffer != NULL);

    fc_raid_context_t *ctx = &g_raid_ctx[lun];

    if (ctx->active == false)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    memcpy(parity_buffer, ctx->acc_buf, FC_BUFFER_POOL_CW_SIZE);

    ctx->active    = false;
    ctx->processed = 0U;
    ctx->total     = 0U;
    return FC_STATUS_OK;
}

/**
 * @brief 保存该 LUN 的 RAID 上下文到影子缓冲（HSQ 打断保护）
 *
 * @param lun LUN 号；越界时静默跳过
 */
void fc_raid_save(BYTE lun)
{
    if (lun < LUN_NUM)
    {
        g_raid_shadow[lun] = g_raid_ctx[lun];
    }
}
/**
 * @brief 从影子缓冲恢复该 LUN 的 RAID 上下文
 *
 * @param lun LUN 号；越界时静默跳过
 */
void fc_raid_restore(BYTE lun)
{
    if (lun < LUN_NUM)
    {
        g_raid_ctx[lun] = g_raid_shadow[lun];
    }
}

/**
 * @brief RAID-5 重建：recovered = 除 failed_idx 外所有 CW 的 XOR
 *
 * 异或自逆，结果与被恢复 CW 处于同一 scrambled domain。
 *
 * @param recovered  输出：重建结果（length 字节）
 * @param buffers    CW 数组（NULL 项跳过）
 * @param count      数组长度（非 0）
 * @param failed_idx 失败 CW 下标（< count）
 * @param length     重建长度（字节）
 */
void fc_raid_recover(BYTE *recovered, BYTE *buffers[], BYTE count, BYTE failed_idx, WORD length)
{
    BYTE   i;
    DWORD *rec32    = (DWORD *)(void *)recovered;
    WORD   word_len = length / sizeof(DWORD);

    // 入参前置条件由调用者保证（fc_data_path 传入全 CW 收集结果）
    ASSERT(recovered != NULL);
    ASSERT(buffers != NULL);
    ASSERT(count != 0U);
    ASSERT(failed_idx < count);
    ASSERT(length != 0U);

    memset(recovered, 0, length);

    for (i = 0; i < count; i++)
    {
        if ((i == failed_idx) || (buffers[i] == NULL))
        {
            continue;
        }
        for (WORD w = 0U; w < word_len; w++)
        {
            rec32[w] ^= ((const DWORD *)(const void *)buffers[i])[w];
        }
    }
}

#endif /* CONFIG_FC_RAID */
