#ifndef FTL_COMPLETION_TABLE_H
#define FTL_COMPLETION_TABLE_H

/*
 * Completion Table — cmd_id → ctx 旁表
 *
 * 本表**只**回答一个问题：「这条命令的私有状态在哪」。命令该跑哪个完成服务函数
 * 由**命令自己**携带（fc_command_t.op_tag → g_op_service[]，见 ftl_op.h）。
 *
 * 两张路由表：
 *   ① g_table[COMPLETION_TABLE_SIZE] —— command_id → ctx，**精确旁表**；
 *   ② g_route[CH][CE][LUN]           —— 命令 ch/ce/lun → ftl_context_t（实例路由）。
 *
 * 为什么需要旁表（而不是让 job 自识别 cmd_id）：
 *   同步等待的 ctx 是**调用者栈上的 waiter**，没有 per-instance 的家；若走
 *   "job 比对 job.cmd_id == completion->command_id"，同步路径就得另造一套机制。
 *   保留统一旁表 ⇒ 同步与异步共用同一派发。
 *
 * 谁登记：
 *   · 同步路径：ftl_sync_submit_wait 登记 &waiter（栈）；
 *   · 异步 job：ftl_op_submit(cmd, op, &ftl-><feature>_job)；
 *   · host I/O：ctx == NULL（服务函数自己 pending_lookup）。
 *
 * 线程安全：无（裸机单线程，不在中断中访问）。
 */

#include "ftl/ftl_type.h"     /* ftl_status_t */
#include "nand/nand_config.h" /* CE_NUM / LUN_NUM（实例路由表形参维度）*/

#include <stdint.h>

/* ── 前向声明（ftl_context_t 在 ftl.h 中定义）── */
struct ftl_context;

/* ── 旁表表项 ──
 *
 * 只留 cmd_id → ctx：handler 指针已搬进 g_op_service[]（按 op tag 索引），
 * 故表项从 12B 缩到 8B（32 项 = 256B）。 */
typedef struct
{
    uint16_t used   : 1;  /* 1=槽位占用 */
    uint16_t cmd_id : 15; /* FC command_id */
    void    *ctx;         /* 该命令的私有状态（同步等待 = ftl_sync_waiter_t*；job = &job） */
} completion_entry_t;

#define COMPLETION_TABLE_SIZE (32U) /* 最多同时跟踪 32 个命令 */

/* ── 路由表 API ── */

/* completion_table_init - 清空旁表 + 实例路由表（开机一次） */
void completion_table_init(void);

/*
 * completion_table_add - 为 cmd_id 登记 ctx。
 * @return FTL_OK；FTL_ERROR 表满（COMPLETION_TABLE_SIZE）。
 */
ftl_status_t completion_table_add(uint16_t cmd_id, void *ctx);

/* completion_table_remove - 释放 cmd_id 的表项（未找到 no-op，幂等） */
void completion_table_remove(uint16_t cmd_id);

/*
 * completion_table_take - 查 + **摘链** + 返回 ctx（未登记返回 NULL）。
 *
 * 派发路径专用：先摘链再回调，容许服务函数在自己的回调里重入登记/释放表项。
 */
void *completion_table_take(uint16_t cmd_id);

/*
 * completion_table_count - 当前占用（未决）的表项数。
 *
 * ftl_service 三级模式判定用它近似 NAND 命令忙闲——表占用高 = 同步等待/后台命令
 * 在飞，host IO 空闲度低。
 * @return 占用槽位数（0..COMPLETION_TABLE_SIZE）
 */
uint16_t completion_table_count(void);

/*
 * completion_table_set_route - 设置实例路由表（F6）。
 *
 * 全局旁表保持单一（FC CQ 全局、cmd_id 全局唯一），派发时按命令 ch/ce/lun
 * 路由到对应 FTL 实例。本函数在 main.c 填完 g_ftl_ptrs 后调用一次。
 *
 * @param route  CH_NUM×CE_NUM×LUN_NUM 三维 ftl_context_t* 表（仿 main.c g_ftl_ptrs）
 */
void completion_table_set_route(struct ftl_context *route[][CE_NUM][LUN_NUM]);

/*
 * completion_table_route - 按 (ch,ce,lun) 取实例（未设置返回 NULL；越界断言）
 */
struct ftl_context *completion_table_route(uint8_t ch, uint8_t ce, uint8_t lun);

/*
 * completion_table_drain - 消费所有 CQ entry 并派发（ftl_service_pump Phase 2 调用）。
 *
 * 循环 fc_poll_completion()，每条交 ftl_op_dispatch()（按命令的 op_tag 查服务函数数组）。
 */
void completion_table_drain(void);

#endif /* FTL_COMPLETION_TABLE_H */
