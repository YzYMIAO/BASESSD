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

#include "ftl/l2p_cache.h"
#include "ftl/allocator.h"
#include "ftl/journal.h"
#include "ftl/gc.h"
#include "ftl/pending.h"
#include "ftl/metadata.h"
#include "ftl/checkpoint.h"
#include "common/ssd_types.h"

#include <stdint.h>

/* ── 宏 ── */

#define NAND_SUBMIT_BUDGET      (4U)        /* 每轮最多提交 4 条 */
#define BG_NORMAL_FLUSH_COUNT   (4U)        /* NORMAL 模式下最多 flush 4 line */
#define BG_BUSY_FLUSH_COUNT     (1U)        /* BUSY 模式下最多 flush 1 line */
#define BG_NORMAL_GC_PAGES      (4U)        /* NORMAL 模式下 GC 搬 4 页 */
#define BG_IDLE_GC_TARGET       (32U)       /* IDLE 模式下目标补充 32 Block */
#define CP_WRITE_INTERVAL       (1024U)     /* 写入量触发间隔 */
#define CP_TIME_INTERVAL_MS     (10000U)    /* 定时触发间隔（ms）*/

#define FTL_READ_USER_SIZE      (2048U)     /* 一次读返回的用户数据大小 = 4 CW × 512B */

/* ── FTL 主上下文 ── */

typedef struct ftl_context
{
    /* 拓扑参数 */
    uint8_t  ch;
    uint8_t  ce;
    uint8_t  lun;

    /* L2P Cache */
    l2p_cache_t cache;

    /* Allocator */
    ftl_allocator_t alloc;

    /* Journal */
    l2p_journal_t journal;

    /* Metadata Directory */
    metadata_dir_cache_t metadata_dir;

    /* Checkpoint 计时 */
    uint32_t cp_timer_start;             /* 上次 Checkpoint 时间戳 */
    uint32_t cp_sequence;                /* 上次 Checkpoint sequence */

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
 * @param ftl  FTL 上下文
 * @param ch   所属 CH
 * @param ce   所属 CE
 * @param lun  所属 LUN
 * @param free_blocks     free block 数组（由调用者提供）
 * @param free_count      free block 数
 * @param bad_blocks      bad block 数组（由调用者提供）
 * @param erase_counts    擦除次数数组（由调用者提供）
 * @param closed_sb_list  closed Super Block 数组（由调用者提供）
 * @return 0 成功
 */
int ftl_init(ftl_context_t *ftl,
             uint8_t ch, uint8_t ce, uint8_t lun,
             uint16_t *free_blocks,    uint16_t free_count,
             uint16_t *bad_blocks,
             uint32_t *erase_counts,
             super_block_t *closed_sb_list);

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
 * ftl_submit_read - 非阻塞提交读请求
 *
 * 内部：
 *   ① l2p_cache_read() 查 PBA
 *   ② PBA 无效 → memset(buffer, 0), 立即返回
 *   ③ FC 命令构造 + submit（异步）
 *   ④ 注册 pending 表
 *   ⑤ 数据在 ftl_service() Phase 2 中路由到 buffer
 *
 * @param ftl    FTL 上下文
 * @param lba    逻辑地址
 * @param buffer 数据 buffer（至少 FTL_READ_USER_SIZE 字节）
 * @param count  sector 数（当前只支持 1）
 * @return 0 成功提交，-1 失败
 */
int ftl_submit_read(ftl_context_t *ftl, lba_t lba, void *buffer, uint32_t count);

/*
 * ftl_submit_write - 非阻塞提交写请求
 *
 * 内部：
 *   ① allocator_alloc(FTL_TEMP_HOT) → new_pba
 *   ② l2p_cache_read(lba) → old_pba（journal 用）
 *   ③ FC 命令构造 + submit（异步写 NAND）
 *   ④ journal_append(lba, old_pba, new_pba)
 *   ⑤ l2p_cache_write(lba, new_pba)
 *
 * @param ftl    FTL 上下文
 * @param lba    逻辑地址
 * @param data   数据指针（至少 FTL_READ_USER_SIZE 字节）
 * @param count  sector 数（当前只支持 1）
 * @return 0 成功提交，-1 失败
 */
int ftl_submit_write(ftl_context_t *ftl, lba_t lba, const void *data, uint32_t count);

/*
 * ftl_read - 同步读（被 controller 层调用）
 *
 * @param ftl    FTL 上下文
 * @param lba    逻辑地址
 * @param buffer 数据 buffer
 * @return SSD_STATUS_OK 成功，负值失败
 */
int ftl_read(ftl_context_t *ftl, ssd_lba_t lba, void *buffer);

/*
 * ftl_write - 同步写（被 controller 层调用）
 *
 * @param ftl    FTL 上下文
 * @param lba    逻辑地址
 * @param data   数据指针
 * @return SSD_STATUS_OK 成功，负值失败
 */
int ftl_write(ftl_context_t *ftl, ssd_lba_t lba, const void *data);

/*
 * ftl_trim - TRIM（取消映射）
 *
 * @param ftl    FTL 上下文
 * @param lba    逻辑地址
 * @return SSD_STATUS_OK 成功
 */
int ftl_trim(ftl_context_t *ftl, ssd_lba_t lba);

#endif /* FTL_SERVICE_H */