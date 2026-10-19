#ifndef FTL_COMPLETION_TABLE_H
#define FTL_COMPLETION_TABLE_H

/*
 * Completion Table — cmd_id → ctx 旁表
 *
 * 本表**只**回答一个问题：「这条命令的私有状态在哪」。命令该跑哪个完成服务函数
 * 由**命令自己**携带（fc_command_t.op_tag → g_op_service[]，见 ftl_op.h）。
 *
 * 两张表：
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

#include "ftl_type.h"    /* ftl_status_t */
#include "nand_config.h" /* CE_NUM / LUN_NUM（实例路由表形参维度）*/

#include <stdint.h>

/* ── 前向声明（ftl_context_t 在 ftl.h 中定义）── */
struct ftl_context;

/* ── 旁表表项 ──
 *
 * 只留 cmd_id → ctx：命令该跑哪个完成服务函数由命令自己携带
 * （fc_command_t.op_tag → g_op_service[]，见 ftl_op.h），旁表不存 handler。
 * 8 B/项（2 B 位域 + 2 B 补齐 + 4 B 指针，Cortex-M 指针 4 B）。 */
typedef struct
{
    uint16_t used   : 1;  /* 1=槽位占用 */
    uint16_t cmd_id : 15; /* FC command_id */
    void    *ctx;         /* 该命令的私有状态（同步等待 = ftl_sync_waiter_t*；job = &job） */
} completion_entry_t;

/* 旁表容量。
 *
 * 推导：表按 **command_id** 键，且 completion_table_add 保证同一 cmd_id 至多一条表项
 * ⇒ 表项数 ≤ 同时在飞命令数。在飞命令数受 FC 命令池限死 = FC_COMMAND_POOL_DEPTH。
 * 故池深即容量上界，取等即可（`completion_table.c` 有 _Static_assert 钉住这层关系，
 * FC 扩容时会编译期报错而不是让这里静默变小）。
 *
 * ⚠ 登记方不止 6 个后台 job：L2P 行也逐行登记（load 在飞数无上限，只有 flush 有闸门），
 *   但两者都只能占用**已在飞**的命令 id ⇒ 上界仍是池深。
 *
 * 满 ⇒ completion_table_add 返 FTL_ERROR（非背压：容量够就恒不出现，见上界推导）。 */
#define COMPLETION_TABLE_SIZE (64U)

/* ── 路由表 API ── */

/* completion_table_init - 清空旁表 + 实例路由表（开机一次）
 * ⚠ 清的是**全局**资源 ⇒ 只能调一次，由 main.c 的 ssd_ftl_boot_all 在所有实例
 *   boot 之前调用（**不**在逐实例的 ftl_init 里，否则会抹掉其它实例在飞的登记）。 */
void completion_table_init(void);

/*
 * completion_table_add - 为 cmd_id 登记 ctx。
 *
 * **同一 cmd_id 至多一条表项**由本函数保证：命中已有项即原地覆盖 ctx（并 LOGW），
 * 不追加第二条。理由见实现处注释（命令 id 复用 + 残留项会让 take 摘到陈旧 ctx）。
 *
 * @return FTL_OK；FTL_ERROR 表满（COMPLETION_TABLE_SIZE，见容量推导）。
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
 * completion_table_count - 当前占用（未决）的表项数（观测 / 自检口）。
 *
 * ⚠ 后台档位判定（ftl_bg_level_select）**刻意不**用它：档位要的是 host IO 忙闲，
 *   而 host 请求不经旁表（ftl_op_host_io 的 ctx == NULL）⇒ 旁表占用与 host 负载无关。
 *   档位判据是 pending 占用（见 ftl.c 的 ftl_bg_level_select）。
 *
 * @return 占用槽位数（0..COMPLETION_TABLE_SIZE）
 */
uint16_t completion_table_count(void);

/*
 * completion_table_set_route - 设置实例路由表。
 *
 * 全局旁表保持单一（FC CQ 全局、cmd_id 全局唯一），派发时按命令 ch/ce/lun
 * 路由到对应 FTL 实例。本函数在 main.c 填完 g_ftl_ptrs 后调用一次——
 * ⚠ **必须早于逐实例 boot**：boot 期的 F2 重建就会经 l2p_cache_read_sync 抽完成表。
 * 传 NULL 则清空（boot 失败回退 fc_service 时用）。
 *
 * @param route  CH_NUM×CE_NUM×LUN_NUM 三维 ftl_context_t* 表（仿 main.c g_ftl_ptrs）
 */
void completion_table_set_route(struct ftl_context *route[][CE_NUM][LUN_NUM]);

/*
 * completion_table_route - 按 (ch,ce,lun) 取实例（未设置返回 NULL；越界断言）
 */
struct ftl_context *completion_table_route(uint8_t ch, uint8_t ce, uint8_t lun);

/*
 * completion_table_route_region - 按全局 L2P region 取 owner 实例
 *
 * dim = global_region % FTL_DIM_TOTAL，再用 l2p_cache.h 的 ftl_dim_ch/ce/lun 分解后转
 * completion_table_route。共享 L2P cache 的 owner 解析用它，避免把 region→die 的分解
 * 散落在 cache/CLI 各处（H13）；分解式只有 ftl_dim_* 一份实现（C9）。
 */
struct ftl_context *completion_table_route_region(uint32_t global_region);

/*
 * completion_table_drain - 消费所有 CQ entry 并派发（ftl_service_pump Phase 2 调用）。
 *
 * 循环 fc_poll_completion()，每条交 ftl_op_dispatch()（按命令的 op_tag 查服务函数数组）。
 */
void completion_table_drain(void);

#endif /* FTL_COMPLETION_TABLE_H */
