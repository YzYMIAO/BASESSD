#ifndef FC_RAID_ENGINE_H
#define FC_RAID_ENGINE_H

#include "fc_interface.h"

#include <stdint.h>

/*
 * fc_raid_engine.h — RAID XOR 引擎
 *
 * ============================================================================
 * RAID XOR 引擎 — 编译开关
 *
 * 当前 CONFIG_FC_RAID=0（默认不启用），所有 RAID 功能代码不编译。
 *
 * 何时开启：硬件功能验证闭环后，需要启用跨 Plane XOR 数据保护时。
 * 打开方法：在 include/fc/fc_interface.h 中 #define CONFIG_FC_RAID 1
 *
 * 开启后的两条路径：
 *   1) 写路径：fc.c 的 stripe 分支分配 parity 命令，
 *      调用 fc_data_path_generate_stripe_parity() 生成 parity，
 *      再以 Multi-Plane PROGRAM 提交；
 *   2) 读路径：FTL 在 ECC 失败后调用 fc_data_path_raid_recover()，
 *      由本引擎完成 XOR 重建，再由数据路径做 ECC 重试与解扰。
 *
 * 设计文档：docs/design/fc_nand_design.md §3.6
 * ============================================================================
 */

#if CONFIG_FC_RAID

/*
 * RAID 操作的三个基础指令：
 *   START  → 初始化累加器 = 第一个 CW 的数据
 *   CONTINUE → 累加器 ^= 当前 CW
 *   END    → 累加器 = XOR 结果，写入 parity buffer
 */

/* 每个 LUN 的 RAID 上下文 */
typedef struct FC_RAID_CONTEXT_S
{
    BOOL  active;                          /* true = RAID 序列进行中 */
    BYTE  acc_buf[FC_BUFFER_POOL_CW_SIZE]; /* XOR 累加器缓冲（一个 CW 大小） */
    BYTE  processed;                       /* 已累加的 CW 数（含 START） */
    BYTE  total;                           /* 期望累加的总 CW 数 */
    DWORD start_paa;                       /* 起始 PAA（调试用） */
} fc_raid_ctx_s;

/*
 * RAID 上下文池，每个 LUN 独立；影子缓冲用于 HSQ 打断时保存/恢复。
 * 在 fc_raid_engine.c 中定义为全局静态数组。
 */

/* ── 上下文管理（预留，零生产调用）──
 * 当前 stripe 路径使用 fc_data_path_generate_stripe_parity() 的按命令 XOR；
 * 以下 start/accumulate/end/save/restore 为 PLANE_NUM>=4 的跨命令累积预留。
 * 启用前必须补调用点、上板验证并在本处删除「预留」标记（P5 偏离 H5 的显式登记）。 */

/**
 * @brief 初始化指定 LUN 的 RAID 上下文，清零上下文与影子缓冲。
 *
 * @param lun LUN 编号（调用者保证 < LUN_NUM）
 */
void fc_raid_init_ctx(BYTE lun);

/* ── RAID 序列 ── */

/**
 * @brief 开始新的 RAID XOR 序列。
 * 将 cw_buffer 拷贝到 acc_buf 作为累加器初始值。
 *
 * @param lun       LUN 编号
 * @param cw_buffer 第一个 CW 的数据（PAA 对应的 buffer）
 * @param total     本次序列期望的 CW 总数（含第一个）
 * @return FC_STATUS_OK           序列已启动
 *         FC_STATUS_NOT_READY    该 LUN 已有 RAID 序列进行中
 *
 * @note 入参非法（lun 越界 / cw_buffer 为 NULL / total 为 0）由断言拦截。
 */
fc_status_e fc_raid_start(BYTE lun, const BYTE *cw_buffer, BYTE total);

/**
 * @brief 累加一个 CW 到 RAID 累加器。acc_buf ^= cw_buffer。
 *
 * @param lun       LUN 编号
 * @param cw_buffer 当前 CW 的数据
 * @return FC_STATUS_OK                已累加
 *         FC_STATUS_INVALID_ARGUMENT  RAID 序列未开始
 *
 * @note 入参非法（lun 越界 / cw_buffer 为 NULL）由断言拦截；
 *       超出 total 的累加属调用者计数 bug，由断言拦截。
 */
fc_status_e fc_raid_accumulate(BYTE lun, const BYTE *cw_buffer);

/**
 * @brief 结束 RAID 序列，将累加器结果写入 parity_buffer。
 * 写完后清 active 标志。
 *
 * @param lun           LUN 编号
 * @param parity_buffer 输出：XOR 结果写入此 buffer
 * @return FC_STATUS_OK                累加结果已写入 parity_buffer
 *         FC_STATUS_INVALID_ARGUMENT  RAID 序列未开始
 *
 * @note 入参非法（lun 越界 / parity_buffer 为 NULL）由断言拦截。
 */
fc_status_e fc_raid_end(BYTE lun, BYTE *parity_buffer);

/* ── 中断保护 ── */

/**
 * @brief 保存 RAID 上下文到影子缓冲（HSQ 打断前调用）。
 *
 * @param lun LUN 编号；越界时直接忽略
 */
void fc_raid_save(BYTE lun);

/**
 * @brief 从影子缓冲恢复 RAID 上下文（HSQ 完成后调回）。
 *
 * @param lun LUN 编号；越界时直接忽略
 */
void fc_raid_restore(BYTE lun);

/* ── 静态恢复（ECC 失败后使用，不需要 LUN 上下文） ── */

/**
 * @brief 从所有 CW 和 parity XOR 重建 failed_idx 的数据：
 *   recovered = buffers[0] ^ buffers[1] ^ ... ^ buffers[count-1]
 *   跳过 failed_idx。
 *
 * @param recovered   输出：恢复后的 buffer
 * @param buffers     所有 CW buffer 指针数组（含 parity）
 * @param count       buffers 总数
 * @param failed_idx  失败的 buffer 索引（0..count-1）
 * @param length      每个 buffer 的长度
 *
 * @note recovered 先清零再累加；buffers[i] 为 NULL 的条目跳过；
 *       length 按 4 字节字对齐处理，尾部不足一字的余数字节不参与。
 */
void fc_raid_recover(BYTE *recovered, BYTE *buffers[], BYTE count, BYTE failed_idx, WORD length);

#endif /* CONFIG_FC_RAID */

#endif /* FC_RAID_ENGINE_H */
