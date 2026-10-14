#ifndef FTL_OP_H
#define FTL_OP_H

/*
 * ftl_op.h — 完成派发：op tag + 服务函数数组
 *
 * 把「命令提交」与「命令完成」解耦：
 *   提交侧  ftl_op_submit(cmd, op, ctx) —— 只保证**已提交**，不等待；
 *   完成侧  completion_table_drain() → ftl_op_dispatch() —— 按命令携带的 op tag
 *           查 g_op_service[] 执行对应服务函数。
 *
 * ── op tag 的语义：**完成服务函数选择器**，不是"这是哪个功能" ──
 *
 * 每一项 = 一个完成服务函数。**同步提交的命令一律带 FTL_OP_SYNC_WAIT**，因为
 * 阻塞调用方的完成处理函数**就是**唤醒等待者；若给同步命令打上例如
 * FTL_OP_JOURNAL 的标签，服务函数与调用者会对同一份状态各推进一次。
 * 某个功能站点被改成异步之后，该站点才把自己的 tag 换成它的功能 op ID。
 *
 * ⇒ 枚举一次性建全（"各功能一个 ID"），各 ID 随其站点改造**逐个点亮**。
 *   未点亮的槽为 NULL，派发时判为孤儿（release + LOGE）。
 *
 * ── 命令 release 归属（命令生命周期的唯一权威）──
 *
 *   · FTL_OP_SYNC_WAIT 的服务函数**不** release —— 调用者（同步原语）要在
 *     wait 返回后读 cmd 数据（如 ftl_cmd_read_data）才 release，这里释放即悬垂。
 *   · FTL_OP_HOST_IO 与各异步 job 的服务函数**自己** release。
 *
 *   fc_command_release 幂等（已在 FREE 直接返回 OK），但**不能漏**：漏则命令池
 *   泄漏 ⇒ fc_command_alloc 恒 NULL ⇒ 全系统 FTL_AGAIN。
 *
 * ── 实例归属 ──
 *
 * 服务函数签名里的 ftl 由 ftl_op_dispatch 从**命令自身**的 ch/ce/lun 经
 * completion_table_route 解析后传入（不是"谁在 drain"）。开机早期 g_route 尚未
 * set_route ⇒ 可为 NULL，这是**正常状态**（FTL_OP_SYNC_WAIT 的服务函数不用 ftl）。
 */

#include "fc/fc_command.h" /* fc_command_t / fc_command_status_t */
#include "ftl/ftl_type.h"  /* ftl_status_t */

#include <stdint.h>

/* ── 前向声明（ftl_context_t 在 ftl.h 中定义）── */
struct ftl_context;

/* 完成服务函数选择器。每一项 = 一个 g_op_service[] 槽 = 一个"功能"。 */
typedef enum
{
    FTL_OP_NONE = 0,      /* 未标记：派发时判为孤儿（release + LOGE），正常不应出现 */
    FTL_OP_SYNC_WAIT,     /* 唤醒同步等待者（所有 ftl_sync_* 用） */
    FTL_OP_HOST_IO,       /* host 读写子操作（原 FC_COMMAND_MODULE_FTL 分支） */
    FTL_OP_JOURNAL,       /* 阶段 2 */
    FTL_OP_READ_RECOVERY, /* 阶段 3 */
    FTL_OP_GC,            /* 阶段 4 */
    FTL_OP_WL,            /* 阶段 4 */
    FTL_OP_L2P,           /* 阶段 5 */
    FTL_OP_CHECKPOINT,    /* 阶段 6 */
    FTL_OP_BBT,           /* 阶段 7 */
    FTL_OP_WEAR,          /* 阶段 7 */
    FTL_OP_METADATA,      /* 阶段 8 */
    FTL_OP_COUNT          /* 计数哨兵 */
} ftl_op_id_t;

/* op_tag 只有 7 位（fc_command_t.op_tag）⇒ 槽数上限 128。 */
_Static_assert((uint32_t)FTL_OP_COUNT <= 128U, "ftl_op_id_t 必须装进 fc_command_t.op_tag:7");

typedef void (*ftl_op_service_fn_t)(struct ftl_context        *ftl,
                                    const fc_command_status_t *completion,
                                    void                      *ctx);

/*
 * ftl_op_submit - 标记 op tag、登记完成服务、提交命令。
 *
 * 返回 FTL_OK **只保证"已提交"**，不保证完成；命令的 release 归调用者
 * （FTL_OP_SYNC_WAIT 站点）或服务函数（异步 job 站点）。
 *
 * ctx == NULL ⇒ 不登记（服务函数自己从 pending 表等派生状态）；
 * ctx != NULL ⇒ 登记到 completion_table，派发时作为第三形参交回。
 *
 * ⚠ **登记必须早于 fc_submit**（同 pending_add 的先例）：提交后完成可能立刻被
 *   drain 处理，那时 ctx 还没登记就会丢完成。提交失败则回滚登记。
 *
 * @return FTL_OK 已提交；FTL_AGAIN 提交被背压拒（SQ 满等瞬态）；FTL_ERROR 旁表满。
 */
ftl_status_t ftl_op_submit(fc_command_t *cmd, ftl_op_id_t op, void *ctx);

/*
 * ftl_op_dispatch - 完成派发（completion_table_drain 的唯一出口）。
 *
 * 按 completion->op_tag 查 g_op_service[]；未知 tag ⇒ 孤儿，release + LOGE。
 * ctx 从 completion_table 取出（先摘链再回调，容许服务函数重入登记）。
 */
void ftl_op_dispatch(const fc_command_status_t *completion);

/*
 * ftl_op_host_io - host 读写子操作的完成服务函数（定义在 ftl.c）。
 *
 * 由 g_op_service[FTL_OP_HOST_IO] 引用；ctx 不用（服务函数自己 pending_lookup）。
 */
void ftl_op_host_io(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/* ── 同步等待器（FTL_OP_SYNC_WAIT 的 ctx）──
 *
 * 由同步原语在**自己栈上**持有，经 ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter)
 * 登记进旁表；服务函数（ftl_op_sync_wait）在完成时回填并置 done。 */
typedef struct
{
    uint16_t            cmd_id;     /* 等待的 command_id */
    uint8_t             done;       /* 服务函数置 1 */
    fc_command_status_t completion; /* 服务函数保存的完整 completion 信息 */
} ftl_sync_waiter_t;

#endif /* FTL_OP_H */
