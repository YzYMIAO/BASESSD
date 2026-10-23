#ifndef FTL_CACHE_READ_H
#define FTL_CACHE_READ_H

/*
 * FTL cache read batch（本仓自定）。
 *
 * 一次 host 读请求内，若连续 pair 页的 L2P 映射已驻留且物理页连续，就把
 * 2~4 个 pair 用 two-plane cache read 一次提交：
 *
 *   START_SEQ_MP(pair0) -> SEQ_MP(pair1..N-2) -> LAST_MP(pairN-1)
 *
 * 不依赖 FC chain：4 条命令在同一 SQ 内连续提交；FTL 当前全部命令
 * high_priority=0，后续命令只会排在 sequence 之后。
 */

#include "ssd_types.h"

struct ftl_context;

/* 2 个 pair 才有收益（1 个 pair 用普通读）。4 = NAND per-LUN FIFO 深度。 */
#define FTL_CACHE_READ_MIN_PAIRS (2U)
#define FTL_CACHE_READ_MAX_PAIRS (4U)

typedef enum
{
    FTL_CACHE_READ_DECLINED = 0, /* 条件不满足：调用者回退普通读 */
    FTL_CACHE_READ_OK       = 1, /* 已提交，*used = 消费的 LBA 数 */
    FTL_CACHE_READ_AGAIN    = 2, /* L2P 加载在飞/资源背压：本轮停止 */
    FTL_CACHE_READ_ERROR    = 3, /* 永久错误：组报 IO_ERROR */
} ftl_cache_read_result_e;

/*
 * 尝试为当前读组提交一个 cache read 批次。
 *
 * @param ftl     FTL 实例
 * @param gi      组下标
 * @param lba     当前子操作起始全局 LBA（必须 pair 对齐才尝试）
 * @param buffer  当前 host 读缓冲（后续 pair 按 4 KiB 递增）
 * @param remain  本组剩余 LBA 数
 * @param used    出参：OK 时消费的 LBA 数（N * FTL_LBA_PER_PAIR）
 */
ftl_cache_read_result_e ftl_cache_read_try(struct ftl_context *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used);

#endif /* FTL_CACHE_READ_H */
