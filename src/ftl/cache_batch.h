#ifndef FTL_CACHE_BATCH_H
#define FTL_CACHE_BATCH_H

/*
 * FTL cache 流式序列（读 / 写，本仓自定）
 *
 * 读、写两个方向的 host 路径放在同一模块：
 *   · cache read：连续 pair 页映射已驻留且物理连续（同 block page+1，或两 plane
 *     都 block+1/page0——31h 可跨 block）时，按窗口提交 START_SEQ_MP/SEQ_MP/LAST_MP；
 *     序列跨窗口、跨 host 传输段保持，START 只在顺序流开头、LAST 只在断点；
 *   · cache program：首窗口首条 op49、中间 op44（15h）、断点末条 op48（10h）；
 *     窗口上限 4 个 pair 页，序列跨窗口、跨 host 传输段、跨物理 block。
 *
 * 是否进入序列由 **FTL 的顺序流检测器**决定（连续 LBA ≥ FTL_STREAM_MIN_LBAS，
 * 见 ftl.h），不是 host 传下来的开关；序列打开后按 cache_stream.seq.next_lba 延续。
 * 序列的主动断点见 ftl_cache_write_try 的注释（Journal 配额 / 命令尾 / D4 等；
 * 物理 block 边界不是断点——写 cache program 每条命令自带地址，序列跨 block 续写）。
 *
 * 两个方向都按窗口标 FC chain（窗口末条在**断点**是 FC_CHAIN_END，否则 FC_CHAIN_MORE），
 * 经 ftl_op_submit_batch() 原子、连续入队；FC 的 chain_owner 在链跨填充轮次时
 * 锁住该 LUN，另一队列回推。序列打开期间由 FTL 门控（ftl_op_submit*）延迟同 LUN 的
 * 其他提交，序列不会被 RR / GC / 其他 host 命令劈开。
 * 条件不满足一律回退普通单 pair 读 / wbuf 写路径。
 */

#include "ftl_config.h" /* FTL_LBA_PER_PAIR */
#include "ftl_type.h"   /* ftl_ctx_s */
#include "ssd_types.h"

/* ── cache read ──
 * 新序列至少 2 个 pair（1 个 pair 用普通读）；**续序列允许 1 个 pair 的尾巴**
 * （单条 LAST 收尾，见 cache_batch.c 的 ftl_cache_read_try）。
 * 单条窗口的 pair 上限由 FC 资源决定（见 cache_batch.c 的 FTL_CACHE_READ_MAX_PAIRS）：
 * per-LUN FIFO 深度只决定 FC 每趟往 NAND 填充几条，不是窗口上限。
 * 尝试时机：序列所有者每轮都试（续窗口）；其余情况只在 FTL 顺序流检测器判为
 * 顺序读时尝试（判据是连续 LBA 长度，与 host 传输段长度无关）。 */
#define FTL_CACHE_READ_MIN_PAIRS (2U)

/* 读序列单段上限（pair，跨 block 续接后仍需收尾）。
 *
 * why：READ PAGE CACHE SEQUENTIAL (31h) 可以跨 block（同 die；禁止跨 die/LUN ——
 * MT29F4G08 datasheet「Read Cache Operations」节（链接见 `docs/reference/README.md`）），所以 block 边界不再是自然断点；
 * 但**整段读序列期间该 die 的 cache 寄存器被占用（ARDY = 0），程序命令发不进去**，
 * Journal / L2P / GC / 元数据写会被序列门控挡在段外。断点就是后台元数据在这块 die 上的
 * 让步点，必须有一个与器件无关的上限。
 *
 * 口径：128 pair = 512 KiB 数据，按实测顺序读 ~887 KB/s 约 0.58 s 一段；与写侧
 * Journal 顺序页配额（63 pair ≈ 0.29 s）同一量级的让步节奏。太小则 START 数回升，
 * 太大则 GC/CP 在长顺序读期间得不到窗口。
 * 重访触发：上板看 `cr` 的 starts 与 GC/metadata 在长读期间的进展。 */
#define FTL_CACHE_READ_MAX_RUN_PAIRS (128U)

/* ── cache program ──
 * 窗口页数上限 = host 单次写传输 16 KiB / 4 KiB 页 = 4（CFG_TUD_MSC_EP_BUFSIZE）。
 * 序列本身跨窗口、跨 chunk（首 49 / 中间 44 / 断点 48），窗口只是资源边界；
 * 不是 NAND FIFO 深度。 */
#define FTL_CACHE_WRITE_MAX_PAGES (4U)

/* 后台让行的最长 host 暂停：后台被门控拒绝后置 yield_pending，host 暂不开新序列，
 * 让后台在本轮提交。后台若一直提交不出去，超过本值即忽略让行、恢复 host，
 * 防止后台故障把 host 饿死。取 20 ms：一个 16 KiB 窗口在 940 KB/s 下约 17 ms。 */
#define FTL_CACHE_SEQ_YIELD_MAX_MS (20U)

/* demand L2P 独立提交窗口的时长（本仓取值）。 */
#define FTL_L2P_DEMAND_WINDOW_MS (500U)

typedef enum FTL_CACHE_READ_RESULT_E
{
    FTL_CACHE_READ_DECLINED = 0, /* 条件不满足：调用者回退普通读 */
    FTL_CACHE_READ_OK       = 1, /* 已提交，*used = 消费的 LBA 数 */
    FTL_CACHE_READ_AGAIN    = 2, /* L2P 加载在飞/资源背压：本轮停止 */
    FTL_CACHE_READ_ERROR    = 3, /* 永久错误：组报 IO_ERROR（如链契约违反） */
} ftl_cache_read_result_e;

typedef enum FTL_CACHE_WRITE_RESULT_E
{
    FTL_CACHE_WRITE_DECLINED = 0, /* 条件不满足：调用者回退 wbuf */
    FTL_CACHE_WRITE_OK       = 1, /* 已提交，*used = 消费的 LBA 数 */
    FTL_CACHE_WRITE_AGAIN    = 2, /* 资源/空间瞬态背压：本轮停止 */
    FTL_CACHE_WRITE_ERROR    = 3, /* 永久错误：组报 IO_ERROR */
} ftl_cache_write_result_e;

/* ── 序列状态查询 / 控制（跨模块只走这里，不直接读 cache_stream.seq 字段）──
 *
 * `cache_stream.seq` 的写者是序列模块（本文件 + ftl.c 的状态机）；ftl_op.c 等
 * 其他模块只能通过下面这组入口查询状态、请求断点/让行。
 * 纯谓词（is_open / is_closing / owner_gi / gate_blocks / yield_pending）已内联到
 * ftl_cache.h（收 ftl_cache_stream_s *）；本文件只留改状态的入口。 */

/** @brief 请求下一窗口有序收尾。 */
void ftl_cache_seq_request_break(ftl_ctx_s *ftl);

/**
 * @brief 请求 host 暂不开新序列，给后台一次提交机会。
 *
 * @note now_ms == 0（时钟缺失）时不置位：host 无法判超时，宁可不让行也不锁死。
 */
void ftl_cache_seq_request_yield(ftl_ctx_s *ftl, DWORD now_ms);

/** @brief 兑现后台让行（清 yield_pending / yield_ms）。 */
void ftl_cache_seq_yield_clear(ftl_ctx_s *ftl);

/**
 * @brief 开 demand L2P 独立提交窗口。
 *
 * @note confirm 依赖的 region 未驻留、且没有在飞 load 时调用；窗口只挡**新**
 *       cache 序列，不打断已有序列。
 */
BOOL ftl_cache_demand_window_open(ftl_ctx_s *ftl, DWORD now_ms);

/** @brief 清 demand L2P 独立提交窗口。 */
void ftl_cache_demand_window_clear(ftl_ctx_s *ftl);

/**
 * @brief 当前 demand L2P 窗口是否仍在阻止开新 cache 序列。
 *
 * @note 只在 host cache 写序列 **starting** 时调用。
 */
BOOL ftl_cache_demand_window_check(ftl_ctx_s *ftl);

/**
 * @brief 尝试为当前读组提交一个 cache read 窗口（序列状态跨调用保留）。
 *
 * @note DECLINED = 条件不满足、调用者回退普通读（不计数为错误）；
 *       其余返回值见 ftl_cache_read_result_e。
 */
ftl_cache_read_result_e ftl_cache_read_try(ftl_ctx_s *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used);

/**
 * @brief 尝试为当前写组提交一个 cache program 窗口（序列状态跨调用 / chunk 保留）。
 *
 * @note 调用者在 ftl_drain_group 的写分支、正常 wbuf 路径之前调用。
 * @note 断点只在数据页上：Journal 顺序页写满 / D4 / 命令尾不足一对 / journal
 *       预留失败 / 下一窗口 L2P 未驻留 / 收尾块开不出新块。**host 传输段、host
 *       命令、物理 block 边界都不是断点**（44/48h 每条自带地址）；无数据页可 END
 *       时由 service 的 padding END 收尾（ftl.c）。
 */
ftl_cache_write_result_e ftl_cache_write_try(ftl_ctx_s *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used);

#endif /* FTL_CACHE_BATCH_H */
