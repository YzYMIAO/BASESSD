#ifndef FTL_CACHE_BATCH_H
#define FTL_CACHE_BATCH_H

/*
 * FTL cache batch（本仓自定）
 *
 * 读、写两个方向的 host 批量路径放在同一模块：
 *   · cache read：连续 pair 页映射已驻留且物理连续时，把整条 run 用
 *     START_SEQ_MP/SEQ_MP/LAST_MP 一次提交；
 *   · cache program：host 16 KiB 写 = 4 个 pair 页，按 op49/op44×2/op48 提交。
 *
 * 两个方向都把整条序列标成一条 FC chain（末条 FC_CHAIN_END，其余 FC_CHAIN_MORE），
 * 经 ftl_op_submit_batch() 原子、连续入队；FC 的 chain_owner 在链跨填充 pass 时
 * 锁住该 LUN，另一队列回推——序列不会被 RR / GC / 其他命令劈开。
 * 条件不满足一律回退普通单 pair 读 / wbuf 写路径。
 */

#include "ftl_config.h" /* FTL_LBA_PER_PAIR */
#include "ssd_types.h"

struct ftl_context;

/* ── cache read ──
 * 2 个 pair 才有收益（1 个 pair 用普通读）。单条 run 的长度上限由 FC 资源决定
 * （见 cache_batch.c 的 FTL_CACHE_READ_MAX_PAIRS），**不再等于 per-LUN FIFO 深度**：
 * FIFO 深度只决定 FC 每趟往 NAND 填充几条，链总长由 run 决定。 */
#define FTL_CACHE_READ_MIN_PAIRS (2U)

/* cache read 尝试节拍：每 32 LBA（16 KiB host 传输 = 4 个 pair 窗口）试一次。
 * 这是“多久重试一次”的节拍，不是 run 长度上限，与 FTL_CACHE_READ_MAX_PAIRS 解耦。
 * 来源：CFG_TUD_MSC_EP_BUFSIZE = 16384 B（include/tusb_config.h）；接口变大时同步。 */
#define FTL_CACHE_READ_TRIAL_LBAS (4U * FTL_LBA_PER_PAIR)

typedef enum
{
    FTL_CACHE_READ_DECLINED = 0, /* 条件不满足：调用者回退普通读 */
    FTL_CACHE_READ_OK       = 1, /* 已提交，*used = 消费的 LBA 数 */
    FTL_CACHE_READ_AGAIN    = 2, /* L2P 加载在飞/资源背压：本轮停止 */
    FTL_CACHE_READ_ERROR    = 3, /* 永久错误：组报 IO_ERROR（如链契约违反） */
} ftl_cache_read_result_e;

/**
 * @brief 尝试为当前读组提交一个 cache read 批次。
 *
 * @param ftl     FTL 实例
 * @param gi      组下标
 * @param lba     当前子操作起始全局 LBA（必须 pair 对齐才尝试）
 * @param buffer  当前 host 读缓冲（后续 pair 按 4 KiB 递增）
 * @param remain  本组剩余 LBA 数
 * @param used    出参：OK 时消费的 LBA 数（N * FTL_LBA_PER_PAIR）
 *
 * @return FTL_CACHE_READ_DECLINED 条件不满足，调用者回退普通读（不计数为错误）；
 *         FTL_CACHE_READ_OK 已提交；FTL_CACHE_READ_AGAIN 资源/加载瞬态，本轮停止；
 *         FTL_CACHE_READ_ERROR 永久错误，组报 IO_ERROR。
 */
ftl_cache_read_result_e ftl_cache_read_try(struct ftl_context *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used);

/* ── cache program ──
 * 每批页数 = host 单次写传输 16 KiB / 4 KiB 页 = 4（CFG_TUD_MSC_EP_BUFSIZE）。
 * 不是 NAND FIFO 深度：链总长可以大于 FIFO 深度，由 FC 的 chain_owner 跨 pass 续批。 */
#define FTL_CACHE_WRITE_MAX_PAGES (4U)

/* 一批覆盖的 host LBA 数 = MAX_PAGES × pair 窗口；当前几何 = 16 KiB（32 × 512B）。 */
#define FTL_CACHE_WRITE_LBAS (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR)

typedef enum
{
    FTL_CACHE_WRITE_DECLINED = 0, /* 条件不满足：调用者回退 wbuf */
    FTL_CACHE_WRITE_OK       = 1, /* 已提交，*used = 消费的 LBA 数 */
    FTL_CACHE_WRITE_AGAIN    = 2, /* 资源/空间瞬态背压：本轮停止 */
    FTL_CACHE_WRITE_ERROR    = 3, /* 永久错误：组报 IO_ERROR */
} ftl_cache_write_result_e;

typedef struct
{
    bool  active;                                 /* true = 批次未结算（含在飞/待确认） */
    bool  abort;                                  /* true = 已失败；成员按各自完成回调结算 */
    bool  final_done;                             /* 末页命令已成功完成 */
    BYTE  n;                                      /* 批次页数（host 恒 4） */
    BYTE  done_mask;                              /* bit j = 第 j 页命令已完成 */
    BYTE  outstanding;                            /* 尚未完成的命令数 */
    BYTE  pending_idx[FTL_CACHE_WRITE_MAX_PAGES]; /* 各页 pending 槽下标 */
    DWORD lba;                                    /* 起始 LBA（8 对齐） */
} cache_wb_t;

/**
 * @brief 尝试为当前写组提交一个 16 KiB cache program 批次。
 *
 * @pre 调用者在 ftl_drain_group 的写分支、正常 wbuf 路径之前调用。
 *
 * @param ftl    实例。
 * @param gi     写组下标。
 * @param lba    本批起始 LBA（须 pair 对齐）。
 * @param buffer 16 KiB 源数据（4 个 pair 页）。
 * @param remain 本组剩余可写 LBA 数。
 * @param used   出参：实际提交的 LBA 数（成功时 = FTL_CACHE_WRITE_LBAS）。
 *
 * @return DECLINED —— 条件不满足，调用者回退普通 wbuf 路径（不计数为错误）；
 *                   OK —— 已提交，*used = 32；
 *                   AGAIN —— 资源/空间瞬态，本轮停止 drain；
 *                   ERROR —— 永久错误，组报 IO_ERROR。
 */
ftl_cache_write_result_e ftl_cache_write_try(struct ftl_context *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used);

#endif /* FTL_CACHE_BATCH_H */
