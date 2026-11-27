#ifndef FC_DATA_PATH_H
#define FC_DATA_PATH_H

#include "fc_interface.h" /* CONFIG_FC_RAID */
#include "nand_interface.h"

/*
 * FC 数据路径管线接口。
 *
 * ============== 写路径 (encode) ==============
 *
 * fc_data_path_encode()
 *   全自动管线：Random 加扰 → ECC 编码；跨 plane RAID parity 由
 *   fc_data_path_generate_stripe_parity() 单独生成。
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

/* 单 CW 步骤原语由单测直接调用（验证算法与字段放置语义）；产品档保持 static。 */
#if defined(FC_NAND_TEST_ENABLE)
#    define FC_DATA_PATH_STEP_LINKAGE
#else
#    define FC_DATA_PATH_STEP_LINKAGE static
#endif

/* ── 写路径（FC 内部） ── */

/**
 * @brief 全自动写管线：META 写入 → CRC → 加扰 → ECC 编码。
 * @note FTL 提交命令后、dispatch 前由 FC 自动调用（对 FTL 透明）；
 *       cmd_id 沿 desc 链逐 CW 执行，编码失败透传引擎返回码。
 */
fc_status_e fc_data_path_encode(fc_cmd_id_t cmd_id);

#if CONFIG_FC_RAID
/* ── 跨 Plane Stripe RAID parity 生成（FC 内部） ── */

/**
 * @brief 对 PLANE_NUM-1 条数据命令的对应 CW 数据区做跨命令 XOR，
 *        写入 parity 命令 buffer 后对其做 ECC 编码。
 * @note FC 内部在 stripe 累积完成后、提交 dispatch 前自动调用。
 * @note data_cmd_ids 非空、长度 = data_count（= PLANE_NUM-1）；参数 / desc 链
 *       损坏的前置违反一律 ASSERT，不返回错误码。
 */
fc_status_e fc_data_path_generate_stripe_parity(const fc_cmd_id_t data_cmd_ids[], BYTE data_count, fc_cmd_id_t parity_id);
#endif /* CONFIG_FC_RAID */

/* ── 读路径 Stage 1：ECC 解码（FC 内部，CQ push 前） ── */

/**
 * @brief 对命令 desc 链上的每个 CW 做 ECC 解码，并按结果更新 descriptor status。
 * @note 全部 CW 解码成功时解扰全部 buffer；任一 CW 不可纠时不解扰任何 buffer，
 *       buffer 停在 scrambled + ECC-corrected 状态，供 FTL 的 RAID 恢复在
 *       scrambled domain 继续处理。
 * @note 擦除页（1 比特占比 ≥ 99%）记为 FC_CW_STATUS_EMPTY 而非失败，跳过解扰 / CRC / META 校验；
 *       判据只在**解码路径**成立：RAW 读（LDPC_RAW）跳过 ECC，不能用它判断空/非空。
 */
fc_status_e fc_data_path_process_read(fc_cmd_id_t cmd_id);

/* ── 读路径 Stage 2：RAID 恢复（FTL 触发） ── */

/**
 * @brief RAID XOR 重建 + ECC 重试 + 解扰指定 CW，并复查 CRC/META。
 * @note FTL 在 fc_poll_completion 后遍历 desc 链，对 status=UNC 的 CW 调用。
 * @note error_cw_index 为错误 CW 在 desc 链中的索引（0-based）。
 */
fc_status_e fc_data_path_raid_recover(fc_cmd_id_t cmd_id, BYTE error_cw_index);

/* ── 单 CW 步骤原语（管线与单测共用） ──
 *
 * 上面几个 fc_data_path_* 是**编排**（沿 desc 链跑完整个 stage），下面这几个是
 * 它们内部的**单个算法步骤**，一律只操作一块 528B 的裸 CW buffer
 * （[META:4][CRC:4][USER:512][ECC:8]），不碰 command / descriptor / 链。
 *
 * 拆出来的目的：让「调了哪些步骤」直接等于「开了哪些功能」。
 * 单测（tests/unit/test_fc_data_path.c）拿一块 buffer 调这些原语就能验算法本身，
 * 不必建链、也不必用 config 位去组合出「只开 CRC」这类状态。
 * 产品档这些原语一律 static；只有单测构建（FC_NAND_TEST_ENABLE）导出符号。
 *
 * ⚠ 提交出去的命令**绝不能再手工调**这些原语——FC 自己会跑整条管线
 *   （见 tests/unit/fc_test_common.h「五」），重复编码 = 数据损坏。
 */

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 把本 CW 的逻辑地址（扇区 LBA）写进 META 区 [0,4)。
 * @note 写路径 Stage 0 的唯一 META 写入者；META 保持明文，不参与加扰。
 */
void fc_data_path_meta_write(BYTE *cw, DWORD meta);

/**
 * @brief 对 USER 区 [8,520) 计算 CRC-32 并写入 CRC 字段 [4,8)。
 * @note 调用前 USER 区须已是明文数据。
 */
void fc_data_path_crc_write(BYTE *cw);

/**
 * @brief 加扰 / 解扰（同一个 XOR 变换，自反），作用于 USER 区 [8,520)。
 * @note seed 通常由页内明文 META 经 fc_random_seed_from_meta 派生。
 * @note META / CRC 保持明文，读侧才能从页内 META 自举出同一个种子。
 */
void fc_data_path_scramble(BYTE *cw, DWORD seed);

/**
 * @brief 对 520B 数据区计算 8B ECC parity 并写入 [520,528)。
 * @note USER 区须已完成写入与加扰。
 * @note 引擎不可用返回 FC_STATUS_NOT_SUPPORTED；调用方必须让写命令失败，不得静默跳过。
 */
fc_status_e fc_data_path_ecc_encode(BYTE *cw);

/**
 * @brief 对单个 CW 做 ECC 解码，可就地纠正数据区与 parity 尾区。
 * @note ecc_bits 非 NULL 时返回纠正的 bit 数（失败时为 0）；不需要时传 NULL。
 * @note 引擎不可用返回 FC_STATUS_NOT_SUPPORTED；调用方必须按失败处理，不得当作零纠正。
 */
fc_status_e fc_data_path_ecc_decode(BYTE *cw, BYTE *ecc_bits);

/**
 * @brief CRC 校验：重算 USER 区 CRC 并与 CRC 字段 [4,8) 中存储值比对。
 * @note cw 的 USER 区须为明文（即解扰之后）。
 */
BOOL fc_data_path_crc_check(const BYTE *cw);
#endif /* FC_NAND_TEST_ENABLE */

#endif /* FC_DATA_PATH_H */
