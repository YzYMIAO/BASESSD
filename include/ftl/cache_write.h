#ifndef FTL_CACHE_WRITE_H
#define FTL_CACHE_WRITE_H

/*
 * FTL cache program batch（本仓自拟）。
 *
 * host 16 KiB 写 = 4 个 pair 页。条件满足时一次提交 4 条命令：
 *
 *   op49（首页，15h，只查 FAIL） + op44 × 2（中间页，15h） + op48（末页，10h）
 *
 * 中间页只释放命令，不确认 L2P/Journal、不递减 host 组；末页成功（且 FAILC 检查
 * 通过）后统一确认。不依赖 FC chain：4 条命令经 fc_submit_batch() 一次全进 SQ。
 *
 * 本模块只覆盖 host 路径；GC/WL 批次在 migrate.c 内实现。
 */

#include "ftl_config.h" /* FTL_LBA_PER_PAIR */
#include "ssd_types.h"

struct ftl_context;

/* 4 = NAND per-LUN FIFO 深度；也是 16 KiB / 4 KiB。 */
#define FTL_CACHE_WRITE_MAX_PAGES (4U)

/* host 16 KiB = 32 × 512B LBA = 4 × pair（8 LBA）。 */
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

/*
 * ftl_cache_write_try - 尝试为当前写组提交一个 16 KiB cache program 批次。
 *
 * 前置：调用者在 ftl_drain_group 的写分支、正常 wbuf 路径之前调用。
 * 返回：
 *   DECLINED —— 条件不满足，调用者回退普通 wbuf 路径（不计数为错误）；
 *   OK —— 已提交，*used = 32；
 *   AGAIN —— 资源/空间瞬态，本轮停止 drain；
 *   ERROR —— 永久错误，组报 IO_ERROR。
 *
 * 成功提交后，调用者需 g->next += *used；组 outstanding 已按 4 条子操作登记。
 */
ftl_cache_write_result_e ftl_cache_write_try(struct ftl_context *ftl,
                                             WORD                gi,
                                             DWORD               lba,
                                             void               *buffer,
                                             DWORD               remain,
                                             DWORD              *used);

#endif /* FTL_CACHE_WRITE_H */
