#ifndef FC_DATA_PATH_H
#define FC_DATA_PATH_H

#include "fc_types.h"

/*
 * FC 数据路径管线接口。
 *
 * ============== 写路径 (encode) ==============
 *
 * fc_data_path_encode()
 *   全自动管线：Random 加扰 → ECC 编码 → RAID XOR。
 *   FC 内部在 NAND Program 前调用，对 FTL 透明。
 *
 * ============== 读路径 (decode) ==============
 *
 * 读路径分为两个阶段，分别由不同模块驱动：
 *
 * Stage 1 — fc_data_path_process_read()  [FC 内部]
 *   NAND Read 完成后、CQ push 之前，FC 自动调用。
 *   对每个 CW：ECC 解码 → 更新 descriptor->status。
 *     - 所有 CW 都 ECC 成功 → 自动解扰所有 buffer，推 CQ。
 *     - 存在 ECC 失败的 CW → 不解扰任何 buffer，推 CQ。
 *       此时非失败 CW 处于「已 ECC 纠正但仍加扰」状态，
 *       以便 FTL 后续的 RAID XOR 恢复仍在 scrambled domain 进行。
 *
 * Stage 2 — fc_data_path_raid_recover()  [FTL 触发]
 *   FTL 从 CQ 拿到 completion 后，遍历 desc 链发现 status=UNC，
 *   决定走 RAID 恢复时调用。
 *   内部：XOR 重建 → ECC 重试 → 所有 CW 解扰 → 更新 descriptor status。
 *
 * 这种设计保证了：
 *   ① 加解扰对 FTL 完全透明
 *   ② RAID XOR 在正确的 scrambled domain 进行
 *   ③ FTL 对恢复策略有完全控制权（RAID / read retry / BBT 等）
 */

/* ── 写路径（FC 内部） ── */

/* 全自动写管线：加扰 → ECC 编码 → RAID XOR parity。
 * FTL 提交命令后、dispatch 前由 FC 自动调用（对 FTL 透明）。 */
fc_status_t fc_data_path_encode(const fc_command_t *command);

#if CONFIG_FC_RAID
/* ── 跨 Plane Stripe RAID parity 生成（FC 内部） ── */

/*
 * 对 PLANE_NUM-1 条数据命令做跨命令 XOR，结果写入 parity 命令的 buffer，
 * 然后对 parity buffer 做 ECC 编码。
 *
 * data_cmds:   数据命令指针数组（长度 = data_count）
 * data_count:  数据命令数量 = PLANE_NUM - 1
 * parity_cmd:  已分配 desc+buffer 的 parity 命令
 *
 * FC 内部在 stripe 累积完成后、提交 dispatch 前自动调用。
 *
 * @return FC_STATUS_OK；ECC 编码失败时透传其返回码。
 *         参数 / desc 链损坏的前置违反一律 ASSERT（不再返回 INVALID_ARGUMENT）。
 */
fc_status_t fc_data_path_generate_stripe_parity(
    const fc_command_t *data_cmds[],
    uint8_t             data_count,
    const fc_command_t *parity_cmd);
#endif /* CONFIG_FC_RAID */

/* ── 读路径 Stage 1：ECC 解码（FC 内部，CQ push 前） ── */

/*
 * 对命令的 desc 链做 ECC 解码 + 条件解扰。
 *
 * - 所有 CW 都 ECC 成功 → 解扰所有 buffer，更新 desc->status = READY，返回 OK
 * - 存在 ECC 失败的 CW   → 不解扰任何 buffer，返回 IO_ERROR
 *                          desc->status 在成功 CW 上设为 READY，失败 CW 上保持 UNC
 *                          此时 buffer 处于 scrambled+ECC-corrected 状态，
 *                          可供 FTL 后续的 RAID XOR 恢复使用
 *
 * @param command  已完成 NAND 读取的命令
 * @return FC_STATUS_OK       所有 CW 解码成功，buffer 已解扰
 *         FC_STATUS_IO_ERROR  存在不可纠的 CW（未解扰，需 FTL 处理）
 */
fc_status_t fc_data_path_process_read(const fc_command_t *command);

/* ── 读路径 Stage 2：RAID 恢复（FTL 触发） ── */

/*
 * RAID XOR 重建 + ECC 重试 + 解扰指定 CW。
 *
 * FTL 在 fc_poll_completion 后遍历 desc 链，对 status=UNC 的 CW
 * 调用此函数。函数内部：
 *   ① 从所有其他 CW + parity 做 RAID XOR 重建
 *   ② ECC 解码重试
 *   ③ 解扰所有 CW
 *   ④ 更新 desc->status = READY
 *
 * @param cmd           命令指针
 * @param failed_cw_idx 失败 CW 在 desc 链中的索引（0-based）
 * @return FC_STATUS_OK              恢复成功
 *         FC_STATUS_IO_ERROR        恢复后 ECC 仍失败
 *         FC_STATUS_INVALID_ARGUMENT 参数无效
 */
fc_status_t fc_data_path_raid_recover(const fc_command_t *cmd, uint8_t failed_cw_idx);

/* ── 单 CW 步骤原语（管线与单测共用） ──
 *
 * 上面几个 fc_data_path_* 是**编排**（沿 desc 链跑完整个 stage），下面这几个是
 * 它们内部的**单个算法步骤**，一律只操作一块 528B 的裸 CW buffer
 * （[META:4][CRC:4][USER:512][ECC:8]），不碰 command / descriptor / 链。
 *
 * 拆出来的目的：让「调了哪些步骤」直接等于「开了哪些功能」。
 * 单测（tests/unit/test_fc_level2.c）拿一块 buffer 调这些原语就能验算法本身，
 * 不必建链、也不必用 config 位去组合出「只开 CRC」这类状态。
 *
 * ⚠ 提交出去的命令**绝不能再手工调**这些原语——FC 自己会跑整条管线
 *   （见 tests/unit/fc_test_common.h「五」），重复编码 = 数据损坏。
 */

/* 把本 CW 的逻辑地址（扇区 LBA）写进 [0,4)。写路径 Stage 0 的唯一写入者。 */
void fc_data_path_meta_write(uint8_t *cw, uint32_t meta);

/* 对 USER 区 [8,520) 算 CRC-32 并写入 [4,8)。需先有明文 USER 数据。 */
void fc_data_path_crc_write(uint8_t *cw);

/* 加扰/解扰（同一个 XOR 变换，自反）。作用于 USER 区 [8,520)，
 * META/CRC 保持明文——读侧才能从页内 META 自举出同一个种子。 */
void fc_data_path_scramble(uint8_t *cw, uint32_t seed);

/* ECC 编码：对 520B 数据区算 8B parity 写入 [520,528)。
 * 引擎未实现（NOT_SUPPORTED）时吞掉返回 OK——与管线行为一致。 */
fc_status_t fc_data_path_ecc_encode(uint8_t *cw);

/* ECC 解码。ecc_bits 出参可为 NULL（不需要纠正位数时）。 */
fc_status_t fc_data_path_ecc_decode(uint8_t *cw, uint8_t *ecc_bits);

/* CRC 校验：1 = 重算值与 [4,8) 存的相等（通过），0 = 不通过。 */
uint8_t fc_data_path_crc_check(const uint8_t *cw);

/* META 读回校验（Pass 4 的两项）：① 低 3 位自洽 == paa 的 plane/cw；
 * ② command_meta 非 FC_META_UNKNOWN 时须等于 lba_to_meta(command_meta, plane, cw)。
 * 返回 FC_CW_STATUS_MATA_ERROR（任一不符）或 0（通过）。 */
uint32_t fc_data_path_meta_check(const uint8_t *cw, uint32_t command_meta, nand_paa_t paa);

#endif /* FC_DATA_PATH_H */