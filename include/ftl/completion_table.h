#ifndef FTL_COMPLETION_TABLE_H
#define FTL_COMPLETION_TABLE_H

/*
 * Completion Table — 统一 FC 命令完成路由表
 *
 * 所有 fc_poll_completion() 消费的 CQ entry 统一经过此表分发：
 *   ① 表中有注册的 handler → 精确路由（用于同步等待、专用回调等）
 *   ② 表中没有 → 走 ftl_handle_completion 默认路径（FTL pending 表等）
 *
 * 消除当前两个"泵"（ftl_service Phase2 和 ftl_internal_sync_wait）
 * 各自 fc_poll_completion 造成 completion 互相吞掉的 bug。
 *
 * 线程安全：无（裸机单线程，不在中断中访问）。
 */

#include "fc/fc_command.h" /* fc_command_status_t */
#include "ftl/ftl_type.h"   /* ftl_status_t */
#include <stdint.h>

/* ── Completion 路由表项 ── */

typedef void (*completion_handler_fn_t)(const fc_command_status_t *completion,
                                         void                     *ctx);

typedef struct
{
    uint16_t                used : 1;    /* 1=槽位占用 */
    uint16_t                cmd_id : 15; /* FC command_id */
    completion_handler_fn_t handler;     /* 完成回调 */
    void                   *ctx;         /* 回调上下文 */
} completion_entry_t;

#define COMPLETION_TABLE_SIZE (16U) /* 最多同时跟踪 16 个命令 */

/* ── 路由表 API ── */

void                         completion_table_init(void);
ftl_status_t                 completion_table_add(uint16_t                   cmd_id,
                                                  completion_handler_fn_t    handler,
                                                  void                      *ctx);
void                         completion_table_remove(uint16_t cmd_id);
completion_entry_t          *completion_table_lookup(uint16_t cmd_id);
void                         completion_table_set_default(completion_handler_fn_t handler,
                                                           void                  *ctx);

/*
 * completion_table_drain - 消费所有 CQ entry 并通过路由表分发
 *
 * 本函数在 ftl_service Phase 2 和 ftl_internal_sync_wait 中调用，
 * 替代原先各自的 fc_poll_completion 循环。
 */
void completion_table_drain(void);

/* ── 同步等待器 ──
 *
 * 用于内部同步操作和 ftl_write 的同步等待。
 */

typedef struct
{
    uint16_t             cmd_id;     /* 等待的 command_id */
    uint8_t              done;       /* handler 设置此标志 */
    fc_command_status_t  completion; /* handler 保存的完整 completion 信息 */
} ftl_sync_waiter_t;

/*
 * ftl_sync_waiter_handler - 通用同步等待 handler
 *
 * 设置 waiter->done = 1，保存 raw_status 和 module。
 * 不释放命令（由调用者负责）。
 */
void ftl_sync_waiter_handler(const fc_command_status_t *completion, void *ctx);

/*
 * ftl_sync_waiter_handler_with_release - 同步等待 + 释放命令
 *
 * 同 ftl_sync_waiter_handler，但额外调用 fc_command_release。
 */
void ftl_sync_waiter_handler_with_release(const fc_command_status_t *completion,
                                          void                     *ctx);

#endif /* FTL_COMPLETION_TABLE_H */