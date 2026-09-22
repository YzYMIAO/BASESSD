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

fc_status_t fc_data_path_encode(const fc_command_t *command);

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

#endif /* FC_DATA_PATH_H */