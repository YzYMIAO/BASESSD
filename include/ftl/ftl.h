#ifndef FTL_SERVICE_H
#define FTL_SERVICE_H

/*
 * ftl_service — 主循环调度器
 *
 * 每次调用执行三阶段：
 *   Phase 1: fc_service() 推进 NAND 命令执行
 *   Phase 2: fc_poll_completion() 处理完成回调
 *   Phase 3: 三级后台任务（IDLE / NORMAL / BUSY）
 *
 * 三级模式切换依据 SQ 队列深度 + 上轮提交的 NAND 命令数。
 */

#include "common/ftl_types.h" /* ftl_io_request_t */
#include "common/ssd_types.h"
#include "ftl/allocator.h"    /* ftl_init_params_t, super_block_t */
#include "ftl/checkpoint.h"
#include "ftl/gc.h"
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"
#include "ftl/metadata.h"
#include "ftl/pending.h"

#include <stdint.h>

/* ── 宏 ── */

#define NAND_SUBMIT_BUDGET    (4U)     /* 每轮最多提交 4 条 */
#define BG_NORMAL_FLUSH_COUNT (4U)     /* NORMAL 模式下最多 flush 4 line */
#define BG_BUSY_FLUSH_COUNT   (1U)     /* BUSY 模式下最多 flush 1 line */
#define BG_NORMAL_GC_PAGES    (4U)     /* NORMAL 模式下 GC 搬 4 页 */
#define BG_IDLE_GC_TARGET     (32U)    /* IDLE 模式下目标补充 32 Block */
#define CP_WRITE_INTERVAL     (1024U)  /* 写入量触发间隔 */
#define CP_TIME_INTERVAL_MS   (10000U) /* 定时触发间隔（ms）*/

#define FTL_READ_USER_SIZE (2048U) /* 一次读返回的用户数据大小 = 4 CW × 512B */

/* 最大有效 LBA = 总 Region 数 × 每 Region entry 数 */
#define FTL_LBA_MAX        ((uint64_t)L2P_REGIONS_TOTAL * L2P_ENTRIES_PER_REGION)

/* ── FTL 主上下文 ── */

typedef struct ftl_context
{
    /* 拓扑信息（本 FTL 绑定的 NAND 芯片路由） */
    ftl_topology_t topo; /* 本 FTL 绑定的 NAND 芯片路由 */

    /* L2P Cache */
    l2p_cache_t cache;

    /* Allocator */
    ftl_allocator_t alloc;

    /* Journal */
    l2p_journal_t journal;

    /* Metadata Directory */
    metadata_dir_cache_t metadata_dir;

    /* Checkpoint 计时 */
    uint32_t cp_timer_start; /* 上次 Checkpoint 时间戳 */
    uint32_t cp_sequence;    /* 上次 Checkpoint sequence */

    /* GC 计时 */
    uint32_t gc_last_run_writes;
    uint32_t gc_timer_start;

    /* 统计 */
    uint32_t total_writes;
} ftl_context_t;

/* ── 接口 ── */

/*
 * ftl_init - 初始化 FTL 子系统
 *
 * @param ftl   FTL 上下文
 * @param params 初始化参数（拓扑 + 大数组存储，见 ftl_init_params_t 定义）
 * @return FTL_OK 成功
 */
ftl_status_t ftl_init(ftl_context_t *ftl, const ftl_init_params_t *params);

/*
 * ftl_service - FTL 主循环调度（每次 main loop 调用一次）
 *
 * Phase 1: fc_service() 推进 NAND
 * Phase 2: fc_poll_completion() 处理完成回调（路由到 pending 表等）
 * Phase 3: 三级后台任务
 *
 * @param ftl  FTL 上下文
 */
void ftl_service(ftl_context_t *ftl);

/*
 * ftl_submit_read - 非阻塞提交读请求（单页或多 Plane）
 *
 * 内部：
 *   ① l2p_cache_read() 查 PBA
 *   ② PBA 无效 → memset(buffer, 0), 立即返回
 *   ③ FC 命令构造 + submit（异步）
 *   ④ 注册 pending（io 按值嵌入）
 *   ⑤ 数据在 ftl_service() Phase 2 中路由到 buffer
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（lba/buffer/pages/complete_flag/status_out/on_complete/ctx）
 * @return FTL_OK 成功提交，FTL_ERROR 失败
 */
ftl_status_t ftl_submit_read(ftl_context_t *ftl, const ftl_io_request_t *req);

/*
 * ftl_submit_write - 非阻塞提交写请求（单页或双 Plane Multi-Plane）
 *
 * req->pages=2 时使用双 Plane Multi-Plane PROGRAM（OP_PROGRAM_PAGE_MULTI_PLANE），
 * 在同一 Super Block 页内同时写入两个 Plane。数据 buffer 需包含两个连续页的数据
 * （总长度 = FTL_READ_USER_SIZE × req->pages 字节）。
 *
 * 内部：
 *   ① allocator_alloc(FTL_TEMP_HOT) → new_pba
 *   ② l2p_cache_read(lba) → old_pba（journal 用）
 *   ③ FC 命令构造 + submit（异步写 NAND）
 *   ④ 注册 pending（complete_flag 在 NAND PROGRAM 完成后置 1）
 *   ⑤ Journal + L2P 在 completion handler 中确认 NAND PROGRAM 成功后执行
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（req->lba 在 pages=2 时必须为偶数，代表双 Plane 的 plane 0 侧）
 * @return FTL_OK 成功提交，FTL_ERROR 失败
 */
ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req);

/*
 * ftl_trim - 取消 LBA 映射（非阻塞）
 *
 * @param ftl  FTL 上下文
 * @param lba  逻辑地址（uint64_t）
 */
void ftl_trim(ftl_context_t *ftl, uint64_t lba);

#endif /* FTL_SERVICE_H */