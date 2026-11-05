/*
 * FC 数据路径管线（编排层；CRC / 加扰 / ECC / RAID 引擎在各自模块）：
 *
 * Write (encode) 方向 — FC 内部在 NAND Program 提交前调用，对 FTL 透明：
 *   Random 加扰 → ECC 编码；跨 plane stripe parity 由 generate_stripe_parity 生成
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
#include "fc.h"
#include "fc_crc_engine.h"
#include "fc_ecc_engine.h"
#include "fc_raid_engine.h"
#include "fc_random_engine.h"

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

    for (desc_id = (fc_descriptor_id_t)cmd->desc_id; (desc_id != FC_DESCRIPTOR_ID_INVALID) && (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (count < max_count);)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);

        // RAID 恢复要求每个 CW 都有 buffer；缺一个就无法可靠重建，整条命令判失败。
        if (desc->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return 0U;
        }
        buffers[count] = fc_buffer_get_base(desc->buffer_id);
        count++;
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
    return (desc_id == FC_DESCRIPTOR_ID_INVALID) ? count : 0U;
}
#endif /* CONFIG_FC_RAID */

/* ══════════════════════════════════════════════════════════
 * 单 CW 步骤原语 — 管线与单测共用
 *
 * 每个函数只碰一块 528B 的裸 CW buffer（[META:4][CRC:4][USER:512][ECC:8]），
 * 不认识 command / descriptor / 链。下面的 encode / process_read 是这些步骤的
 * 编排；单测（Level 2）直接调它们，于是「调了哪些步骤」= 「开了哪些功能」。
 * ══════════════════════════════════════════════════════════ */

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
    fc_random_transform(cw + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE, seed);
}
/**
 * @brief 对 CW 数据区（520B）做 ECC 编码，parity 写入 [520, 528)
 *
 * 引擎不可用（NOT_SUPPORTED）必须由调用方按失败处理，不得跳过 ECC。
 *
 * @param cw 528B CW buffer（已加扰）
 * @return FC_STATUS_OK / NOT_SUPPORTED / 编码错误
 */
fc_status_e fc_data_path_ecc_encode(BYTE *cw)
{
    fc_status_e result;

    ASSERT(cw != NULL);

    result = fc_ecc_encode(cw, FC_BUFFER_DATA_SIZE);

    // 引擎不可用（NOT_SUPPORTED）必须失败：静默跳过等于关闭 ECC（C8/§3.2）。
    return result;
}
/**
 * @brief 对 CW 做 ECC 解码并回填纠正 bit 数
 *
 * NOT_SUPPORTED 表示 ECC 引擎不可用，调用者必须按失败处理。
 *
 * @param cw       528B CW buffer
 * @param ecc_bits 输出：纠正的 bit 数（可 NULL）
 * @return FC_STATUS_OK / NOT_SUPPORTED / IO_ERROR（不可纠）
 */
fc_status_e fc_data_path_ecc_decode(BYTE *cw, BYTE *ecc_bits)
{
    ASSERT(cw != NULL);

    // NOT_SUPPORTED 原样上抛；步骤 1 把它当 ECC 不可用/失败处理
    return fc_ecc_decode(cw, FC_BUFFER_DATA_SIZE, ecc_bits);
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

    ASSERT(cw != NULL);

    // 读取编码时写入的 CRC 值 [4..8]，复用 fc_crc_verify 重算 USER 区并比对。
    (void)memcpy(&stored_crc, cw + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE);

    return (fc_crc_verify(cw + FC_CRC_USER_OFFSET, stored_crc) == FC_CRC_RESULT_OK);
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
 * @return 0 通过；FC_CW_STATUS_META_ERROR 不通过
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
        return FC_CW_STATUS_META_ERROR;
    }

    // 自洽但与本命令的 LBA 不符 = 读到了别的页（GC 路径 meta 为
    // FC_META_UNKNOWN，跳过本项，只做上面的自洽校验）。
    if ((command_meta != FC_META_UNKNOWN) && (read_meta != lba_to_meta(command_meta, paa_to_plane(paa), paa_to_cw(paa))))
    {
        return FC_CW_STATUS_META_ERROR;
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
 * @return FC_STATUS_OK / INVALID_ARGUMENT（buffer 缺失）/ ECC 编码错误
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
    desc_id = (fc_descriptor_id_t)command->desc_id;
    for (WORD steps = 0U; (desc_id != FC_DESCRIPTOR_ID_INVALID) && (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);

        // 带数据的写命令每个 desc 都必须有 buffer；INVALID 是建链失败后的半成品，
        // 不能带着它进数据路径（fc_buffer_get_base 在 release 会越界）。
        if (desc->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        buffer = fc_buffer_get_base(desc->buffer_id);

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
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }

    // 正常链以 INVALID 结束；跑满池深仍未结束 = 成环/链损坏，按失败返回。
    if (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        return FC_STATUS_IO_ERROR;
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
 * @brief 取命令 desc 链第 index 个 CW 的 buffer 基址（RAID 路径专用）。
 *
 * 链内下标由 FC 建链产生；越界、链尾、buffer 未分配一律返回 NULL，由调用方
 * 把整条 parity 任务判失败，不能带着野指针进 XOR。
 */
static BYTE *fc_chain_buffer_at(const fc_command_t *command, BYTE index)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)command->desc_id;

    for (BYTE s = 0U; s < index; s++)
    {
        if ((desc_id == FC_DESCRIPTOR_ID_INVALID) || (desc_id >= FC_DESCRIPTOR_POOL_DEPTH))
        {
            return NULL;
        }
        desc_id = (fc_descriptor_id_t)fc_descriptor_id_to_descriptor(desc_id)->next_id;
    }
    if ((desc_id == FC_DESCRIPTOR_ID_INVALID) || (desc_id >= FC_DESCRIPTOR_POOL_DEPTH))
    {
        return NULL;
    }

    const fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);

    return (desc->buffer_id == FC_BUFFER_ID_INVALID) ? NULL : fc_buffer_get_base(desc->buffer_id);
}

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
    // 故 config.cw_count+1 即**每-plane** CW 数）
    cw_count = (data_cmds[0]->config.cw_count + 1U);
    ASSERT((parity_cmd->config.cw_count + 1U) == cw_count);

    // 逐 CW 做 XOR：
    // parity_cmd->buffer[cw][0..520] = XOR(data_cmds[0..data_count-1]->buffer[cw][0..520])
    //
    // 操作的数据区为 FC_BUFFER_DATA_SIZE（520B），即 META+CRC+USER。
    // ECC parity 区 [520..528] 不参与 XOR，由 ECC 引擎在后续的 ECC 编码步骤生成。
    for (cw = 0U; cw < cw_count; cw++)
    {
        BYTE *parity_buf;
        BYTE  first = 1U;

        parity_buf = fc_chain_buffer_at(parity_cmd, cw);
        if (parity_buf == NULL)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }

        // XOR 所有数据命令的对应 CW 数据区
        for (d = 0U; d < data_count; d++)
        {
            BYTE *data_buf = fc_chain_buffer_at(data_cmds[d], cw);
            if (data_buf == NULL)
            {
                return FC_STATUS_INVALID_ARGUMENT;
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
        desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
        for (WORD steps = 0U; (desc_id != FC_DESCRIPTOR_ID_INVALID) && (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);

            if (desc->buffer_id == FC_BUFFER_ID_INVALID)
            {
                return FC_STATUS_INVALID_ARGUMENT;
            }
            fc_status_e result = fc_data_path_ecc_encode(fc_buffer_get_base(desc->buffer_id));
            if (result != FC_STATUS_OK)
            {
                return result;
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
        if (desc_id != FC_DESCRIPTOR_ID_INVALID)
        {
            return FC_STATUS_IO_ERROR;
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

    // 链长度即 CW 数（config.cw_count / multi_plane 不参与推导；flat 掩码下两者会分叉）
    BYTE total_desc = 0U;
    {
        fc_descriptor_id_t did = (fc_descriptor_id_t)command->desc_id;

        while ((did != FC_DESCRIPTOR_ID_INVALID) && (did < FC_DESCRIPTOR_POOL_DEPTH) && (total_desc < CW_TOTAL_MAX))
        {
            total_desc++;
            did = (fc_descriptor_id_t)fc_descriptor_id_to_descriptor(did)->next_id;
        }
        // 跑满 CW_TOTAL_MAX 仍未到链尾 = 成环/链损坏；不进入后续逐 CW 处理。
        if (did != FC_DESCRIPTOR_ID_INVALID)
        {
            return FC_STATUS_IO_ERROR;
        }
    }

    /* ── 步骤 1: ECC 解码所有 CW ── */
    desc_id = (fc_descriptor_id_t)command->desc_id;

    for (cw = 0U; cw < total_desc; cw++)
    {
        // desc 链长度 = total_desc，循环内 desc_id 始终有效
        desc = fc_descriptor_id_to_descriptor(desc_id);

        // 带数据的读命令每个 desc 都必须有 buffer。INVALID 是建链失败后的半成品，
        // 不能交给 fc_buffer_get_base（release 会越界）；按不可纠上报。
        if (desc->buffer_id == FC_BUFFER_ID_INVALID)
        {
            desc->status = FC_CW_STATUS_UNC;
            all_ok       = 0U;
            desc_id      = (fc_descriptor_id_t)desc->next_id;
            continue;
        }
        buffer = fc_buffer_get_base(desc->buffer_id);

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
            else if (result == FC_STATUS_OK)
            {
                // ECC 成功：desc status 清为无错误标志（UNUSED），记录纠正的 bit 数
                desc->status   = FC_CW_STATUS_UNUSED;
                desc->ecc_bits = ecc_bits;
            }
            else
            {
                // NOT_SUPPORTED（引擎不可用）或其他引擎错误：不能当作成功。
                // 无 ECC 校验的数据不可信，按不可纠上报，由 FTL 决定退役/RAID。
                desc->status = FC_CW_STATUS_UNC;
                all_ok       = 0U;
            }
        }
        else
        {
            // LDPC RAW 模式：跳过 ECC 解码，status 清为无错误标志
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
        desc   = fc_descriptor_id_to_descriptor(desc_id);
        buffer = fc_buffer_get_base(desc->buffer_id);

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
 * 重建后 ECC 重试仍失败即硬件不可恢复；成功后解扰全 CW 并重新检查 CRC/META。
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

    // 收集失败（INVALID buffer/链损坏）或 index 越界时不能进 XOR。
    if ((count == 0U) || (failed_cw_idx >= count))
    {
        return FC_STATUS_IO_ERROR;
    }

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
        if (ecc_result != FC_STATUS_OK)
        {
            // RAID 重建后 ECC 仍失败/引擎不可用 → 硬件层面不可恢复
            return FC_STATUS_IO_ERROR;
        }
    }

    /* ── 解扰所有 CW ── */
    if (cmd->config.random == 0U) /* FC_RANDOM_ENABLE */
    {
        for (i = 0U; i < count; i++)
        {
            // 种子自举：META 明文（加扰区外）。重建出的 CW 其 META 由 XOR 线性还原，
            // 与其它 CW 一样可直接读回。
            DWORD cw_meta;
            (void)memcpy(&cw_meta, buffers[i], FC_CRC_META_SIZE);
            fc_data_path_scramble(buffers[i], fc_random_seed_from_meta(cw_meta));
        }
    }

    /* ── 重建后重新校验 CRC/META，不能只把 status 清零 ── */
    bool recovered_ok = true;

    desc_id = (fc_descriptor_id_t)cmd->desc_id;
    i       = 0U;
    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (i < count))
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        DWORD            cw_meta;

        (void)memcpy(&cw_meta, buffers[i], FC_CRC_META_SIZE);
        desc->meta     = cw_meta;
        desc->status   = FC_CW_STATUS_UNUSED;
        desc->ecc_bits = 0U;

        if ((cmd->config.crc == FC_CRC_ENABLE) && (fc_data_path_crc_check(buffers[i]) == false))
        {
            desc->status |= FC_CW_STATUS_CRC_ERROR;
            recovered_ok = false;
        }
        desc->status |= fc_data_path_meta_check(buffers[i], cmd->meta, desc->paa);
        if (desc->status != FC_CW_STATUS_UNUSED)
        {
            recovered_ok = false;
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
        i++;
    }

    return (recovered_ok == true) ? FC_STATUS_OK : FC_STATUS_IO_ERROR;
}

#endif /* CONFIG_FC_RAID */
