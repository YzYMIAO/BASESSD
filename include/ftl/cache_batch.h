#ifndef FTL_CACHE_BATCH_H
#define FTL_CACHE_BATCH_H

/*
 * FTL cache 流式序列（读 / 写，本仓自定）
 *
 * 读、写两个方向的 host 路径放在同一模块：
 *   · cache read：连续 pair 页映射已驻留且物理连续时，按窗口提交
 *     START_SEQ_MP/SEQ_MP/LAST_MP；序列跨窗口、跨 chunk 保持，START 只在 run 头、
 *     LAST 只在断点；
 *   · cache program：首窗口首条 op49、中间 op44（15h）、断点末条 op48（10h）；
 *     窗口上限 4 个 pair 页，序列同样跨窗口、跨 chunk。
 *
 * 两个方向都按窗口标 FC chain（窗口末条在**断点**是 FC_CHAIN_END，否则 FC_CHAIN_MORE），
 * 经 ftl_op_submit_batch() 原子、连续入队；FC 的 chain_owner 在链跨填充 pass 时
 * 锁住该 LUN，另一队列回推。序列打开期间由 FTL 门控（ftl_op_submit*）延迟同 LUN 的
 * 其他提交，序列不会被 RR / GC / 其他 host 命令劈开。
 * 条件不满足一律回退普通单 pair 读 / wbuf 写路径。
 */

#include "ftl_config.h" /* FTL_LBA_PER_PAIR */
#include "ssd_types.h"

struct ftl_context;

/* ── cache read ──
 * 新序列至少 2 个 pair（1 个 pair 用普通读）；**续序列允许 1 个 pair 的尾巴**
 * （单条 LAST 收尾，见 cache_batch.c 的 ftl_cache_read_try）。
 * 单条窗口的 pair 上限由 FC 资源决定（见 cache_batch.c 的 FTL_CACHE_READ_MAX_PAIRS），
 * **不再等于 per-LUN FIFO 深度**：FIFO 深度只决定 FC 每趟往 NAND 填充几条。 */
#define FTL_CACHE_READ_MIN_PAIRS (2U)

/* cache read 尝试节拍：每 32 LBA（16 KiB host 传输 = 4 个 pair 窗口）试一次。
 * 这是“多久试一次”的节拍，不是窗口长度上限，与 FTL_CACHE_READ_MAX_PAIRS 解耦；
 * **序列所有者不受节拍限制**（窗口消费量不一定落在节拍边界上，必须每轮续窗口）。
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
 * @brief 尝试为当前读组提交一个 cache read 窗口；序列状态跨调用保留。
 *
 * @param ftl     FTL 实例（序列状态在 ftl->cache_seq）
 * @param gi      组下标（组的 cmd_start/cmd_total 是续序列的命令上下文）
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
 * 窗口页数上限 = host 单次写传输 16 KiB / 4 KiB 页 = 4（CFG_TUD_MSC_EP_BUFSIZE）。
 * 序列本身跨窗口、跨 chunk（首 49 / 中间 44 / 断点 48），窗口只是资源边界；
 * 不是 NAND FIFO 深度。 */
#define FTL_CACHE_WRITE_MAX_PAGES (4U)

typedef enum
{
    FTL_CACHE_WRITE_DECLINED = 0, /* 条件不满足：调用者回退 wbuf */
    FTL_CACHE_WRITE_OK       = 1, /* 已提交，*used = 消费的 LBA 数 */
    FTL_CACHE_WRITE_AGAIN    = 2, /* 资源/空间瞬态背压：本轮停止 */
    FTL_CACHE_WRITE_ERROR    = 3, /* 永久错误：组报 IO_ERROR */
} ftl_cache_write_result_e;

/**
 * @brief 尝试为当前写组提交一个 cache program 窗口；序列状态跨调用、跨 chunk 保留。
 *
 * @pre 调用者在 ftl_drain_group 的写分支、正常 wbuf 路径之前调用。
 *
 * @param ftl    实例（序列状态与逐页确认 FIFO 在 ftl->cache_seq）
 * @param gi     写组下标（组的 cmd_start/cmd_total 是续序列的命令上下文）
 * @param lba    本窗口起始 LBA（须 pair 对齐）
 * @param buffer 本窗口源数据（最多 4 × 4 KiB）
 * @param remain 本组剩余可写 LBA 数
 * @param used   出参：实际提交的 LBA 数（1..MAX_PAGES × FTL_LBA_PER_PAIR）
 *
 * @return DECLINED —— 条件不满足，调用者回退普通 wbuf 路径（不计数为错误）；
 *                   OK —— 已提交；
 *                   AGAIN —— 资源 / 确认 FIFO / 加载瞬态，本轮停止 drain；
 *                   ERROR —— 永久错误，组报 IO_ERROR。
 *
 * @note 断点（末条 48/10h）：命令到头 / block 到头 / D4 / 命令尾不足一对 /
 *       journal 预留失败 / 下一窗口 L2P 未驻留。chunk 边界**不是**断点。
 */
ftl_cache_write_result_e ftl_cache_write_try(struct ftl_context *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used);

#endif /* FTL_CACHE_BATCH_H */
