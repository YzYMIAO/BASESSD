#ifndef FC_RAID_ENGINE_H
#define FC_RAID_ENGINE_H

#include "fc_types.h"

#include <stdint.h>

/*
 * RAID XOR 引擎。
 *
 * ============== 设计概览 ==============
 *
 * RAID 操作的三个基础指令：
 *   START  → 初始化累加器 = 第一个 CW 的数据
 *   CONTINUE → 累加器 ^= 当前 CW
 *   END    → 累加器 = XOR 结果，写入 parity buffer
 *
 * ============== 中断保护 ==============
 *
 * RAID 序列执行期间（START 后、END 前），如果 HSQ 高优命令需要
 * 操作同一个 LUN，当前 RAID 上下文会被 save 到影子缓冲，
 * HSQ 完成后 restore 回来继续。
 * 当前 2 Plane 场景下不会触发此路径，但框架已预留。
 *
 * ============== 应用场景 ==============
 *
 * Multi-Plane PROGRAM (连续多条 PROGRAM 命令写不同 Plane):
 *   START  → ACCUMULATE(Plane0) → ACCUMULATE(Plane1) → ... → END
 *
 * Multi-Plane READ + 恢复 (某个 CW ECC 不可纠):
 *   用 RAID 恢复后重试 ECC：
 *   failed_cw = ⊕(除 failed_cw 外的所有 CW 的 ECC 解码结果)
 *   不需要 START/CONTINUE/END，直接调用 fc_raid_recover() 静态恢复
 *
 * ============== PLANE_NUM 约束 ==============
 *
 * 当前 PLANE_NUM=2 时：
 *   Multi-Plane Write: START → ACCUMULATE(P0) → ACCUMULATE(P1) → END
 *   没有中断风险（2 Plane 只需 1 次 CONTINUE，瞬间完成）。
 *
 * PLANE_NUM >= 4 时：
 *   HSQ 可能在任一次 ACCUMULATE 之后插入高优命令，
 *   需要 save/restore 保护累加器。
 */

/* 每个 LUN 的 RAID 上下文 */
typedef struct {
    uint8_t  active;          /* 0=空闲, 1=RAID 序列进行中 */
    uint8_t  acc_buf[528];    /* XOR 累加器缓冲（一个 CW 大小） */
    uint8_t  processed;        /* 已累加的 CW 数（含 START） */
    uint8_t  total;            /* 期望累加的总 CW 数 */
    uint32_t start_paa;        /* 起始 PAA（调试用） */
} fc_raid_context_t;

/*
 * RAID 上下文池，每个 LUN 独立。
 * 影子缓冲用于 HSQ 打断时保存/恢复。
 * 在 fc_raid_engine.c 中定义为全局静态数组。
 */

/* ── 上下文管理 ── */

/* 初始化指定 LUN 的 RAID 上下文 */
void fc_raid_init_ctx(uint8_t lun);

/* ── RAID 序列 ── */

/*
 * 开始新的 RAID XOR 序列。
 * 将 cw_buffer 拷贝到 acc_buf 作为累加器初始值。
 * 每次新的 Multi-Plane 操作前调用一次。
 *
 * @param lun       LUN 编号
 * @param cw_buffer 第一个 CW 的数据（PAA 对应的 buffer）
 * @param total     本次序列期望的 CW 总数（含第一个）
 * @return          FC_STATUS_OK 或 FC_STATUS_NOT_READY（LUN 忙）
 */
fc_status_t fc_raid_start(uint8_t lun, const uint8_t *cw_buffer, uint8_t total);

/*
 * 累加一个 CW 到 RAID 累加器。
 * acc_buf ^= cw_buffer（逐字节 XOR）。
 * 应在每个后续 CW 数据准备好后调用。
 *
 * @param lun       LUN 编号
 * @param cw_buffer 当前 CW 的数据
 * @return          FC_STATUS_OK 或 FC_STATUS_INVALID_ARGUMENT
 */
fc_status_t fc_raid_accumulate(uint8_t lun, const uint8_t *cw_buffer);

/*
 * 结束 RAID 序列，将累加器结果写入 parity_buffer。
 * 写完后清 active 标志。
 *
 * @param lun           LUN 编号
 * @param parity_buffer 输出：XOR 结果写入此 buffer
 * @return              FC_STATUS_OK 或 FC_STATUS_INVALID_ARGUMENT
 */
fc_status_t fc_raid_end(uint8_t lun, uint8_t *parity_buffer);

/* ── 中断保护 ── */

/* 保存 RAID 上下文到影子缓冲（HSQ 打断前调用） */
void fc_raid_save(uint8_t lun);

/* 从影子缓冲恢复 RAID 上下文（HSQ 完成后调回） */
void fc_raid_restore(uint8_t lun);

/* ── 静态恢复（ECC 失败后使用，不需要 LUN 上下文） ── */

/*
 * 从所有 CW 和 parity Xor 恢复 failed_idx 的数据。
 * recovered[i] ^= buffers[j][i] for all j != failed_idx, 再 ^= parity[i]。
 *
 * @param recovered   输出：恢复后的 buffer
 * @param buffers     所有 CW buffer 指针数组（含 parity）
 * @param count       buffers 总数
 * @param failed_idx  失败的 buffer 索引（0..count-1）
 * @param length      每个 buffer 的长度（CW_SIZE）
 */
void fc_raid_recover(uint8_t *recovered,
                     uint8_t *buffers[], uint8_t count,
                     uint8_t failed_idx, uint16_t length);

#endif /* FC_RAID_ENGINE_H */