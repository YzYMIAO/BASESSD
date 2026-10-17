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
 * completion_table_route 解析后传入（不是"谁在 drain"）。
 *
 * 路由表在 main.c 的**逐实例 boot 之前**设好（ssd_ftl_boot_all ①：先
 * completion_table_init 清一次、再 set_route；boot 期 F2 重建就会抽完成表），
 * 故服务函数收到的 ftl **不应**为 NULL——收到 NULL 是编程错误，不是"开机早期"
 * 的正常状态（FTL_OP_SYNC_WAIT 的服务函数不用 ftl，是唯一例外）。
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
    FTL_OP_JOURNAL,       /* Journal 刷页 job（journal.c，阶段 2 点亮） */
    FTL_OP_READ_RECOVERY, /* Read Recovery job（read_recovery.c，阶段 3 点亮） */
    FTL_OP_GC,            /* GC 搬移 job（migrate.c，阶段 4 点亮） */
    FTL_OP_WL,            /* 静态 WL / 慢死预测搬移 job（migrate.c，阶段 4 点亮） */
    FTL_OP_L2P,           /* L2P region 加载 / 写回（l2p_cache.c，阶段 5 点亮） */
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
 * ftl_op_in_dispatch - 当前是否在完成服务函数（或其嵌套调用）里。
 *
 * 用途：给"服务函数内不得同步等待"这条约定一个**可判定的**依据。服务函数里若再
 * 跑一遍完成泵（ftl_sync_submit_wait / journal_flush_page 的 drain），就是嵌套泵
 * ——那正是本次解耦要消除的东西。深度计数（不是布尔）：嵌套 drain 会再进一层。
 *
 * 已落地的一处用法：journal_append 页满时若异步刷页起不来，**不在**派发上下文里
 * 同步回退（会嵌套），改为丢弃该条目 + LOGE（见 journal.c）。
 *
 * @return true = 在派发内；false = 不在。
 */
bool ftl_op_in_dispatch(void);

/*
 * ftl_op_host_io - host 读写子操作的完成服务函数（定义在 ftl.c）。
 *
 * 由 g_op_service[FTL_OP_HOST_IO] 引用；ctx 不用（服务函数自己 pending_lookup）。
 */
void ftl_op_host_io(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_op_journal - Journal 刷页 job 的完成服务函数（定义在 journal.c）。
 *
 * 由 g_op_service[FTL_OP_JOURNAL] 引用；ctx = `l2p_journal_t *`（= &ftl->journal），
 * 服务函数按 job.phase 推进状态机（页写完成 ⇒ 推进写指针 / 起换块擦除；擦完成 ⇒ 收尾）。
 * 每条命令自己 fc_command_release。
 */
void ftl_op_journal(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_op_read_recovery - Read Recovery job 的完成服务函数（定义在 read_recovery.c）。
 *
 * 由 g_op_service[FTL_OP_READ_RECOVERY] 引用；ctx = `ftl_read_recovery_t *`（= &ftl->rr）。
 * 服务函数按 job.stage 推进（切读重试模式 / 重试读 / 恢复默认模式 / 邻居读），终态
 * 释放被托管的 pending 槽并让 host 读组结算。每条命令自己 fc_command_release。
 */
void ftl_op_read_recovery(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_op_migrate - 搬移 job（GC / 静态 WL / 慢死预测）的完成服务函数（定义在 migrate.c）。
 *
 * 由 g_op_service[FTL_OP_GC] 与 [FTL_OP_WL] **共同**引用（同一台引擎，靠 job.kind
 * 区分 victim 来源、job.post 区分收尾）；ctx = `mig_job_t *`（= &ftl->mig）。
 * 服务函数按 job.phase 推进（读完成 ⇒ 解 META + 反查 L2P；写完成 ⇒ 改 L2P/Journal +
 * 迁移有效计数；擦完成 ⇒ 归还/退役/标坏），每条命令自己 fc_command_release。
 */
void ftl_op_migrate(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_op_l2p - L2P region 加载 / 写回的完成服务函数（定义在 l2p_cache.c）。
 *
 * 由 g_op_service[FTL_OP_L2P] 引用；ctx = `l2p_cache_line_t *`（提交时传的行指针）。
 * 服务函数按 line->state 收尾：
 *   · LOADING  ⇒ 成功把整页 region 拷进 line->entries（**须早于 release**：读的数据在
 *                desc 链上），置 CLEAN；失败同样置 CLEAN（entries 保持全 INVALID = 未映射）。
 *   · FLUSHING ⇒ 成功 metadata_dir_commit_region 登记新位置 + CLEAN + 清 region 脏位；
 *                失败置回 DIRTY 保留下轮重试。
 * 每条命令自己 fc_command_release。
 */
void ftl_op_l2p(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_op_checkpoint - Checkpoint job 的完成服务函数（定义在 checkpoint.c）。
 *
 * 由 g_op_service[FTL_OP_CHECKPOINT] 引用；ctx = `cp_job_t *`（= &ftl->cp_job）。
 * 服务函数按 job.phase 记完成结果、推进 phase、release 命令。
 * ⚠ 与 ftl_op_migrate 的差别：本函数**不**推进状态机（不调 checkpoint_kick）——
 *   CP 的阶段里有同步原语（游标未建立时要读池定位），在完成泵里跑就是嵌套泵。
 *   推进交给下一轮 ftl_service 的 checkpoint_kick。
 */
void ftl_op_checkpoint(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_op_bbt - BBT 持久化 job 的完成服务函数（定义在 bbt.c）。
 *
 * 由 g_op_service[FTL_OP_BBT] 引用；ctx = `ftl_allocator_t *`（= &ftl->alloc，
 * 即命令的实例归属；形参 ftl 不用）。服务函数按 alloc->bbt.job.phase 收尾：
 *   · ERASE 成功 ⇒ 起写 record；失败 ⇒ 游标不推进、dirty 保留；
 *   · WRITE 完成 ⇒ **无论成败都推进池游标**（半编程槽不可复用），成功清 dirty。
 * 每条命令自己 fc_command_release。
 */
void ftl_op_bbt(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_op_wear - Wear 日志 job 的完成服务函数（定义在 wear.c）。
 *
 * 由 g_op_service[FTL_OP_WEAR] 引用；ctx = `ftl_allocator_t *`（= &ftl->alloc）。
 * 服务函数按 alloc->wear.job.phase 收尾：
 *   · ERASE（compact 前的整块擦）成功 ⇒ next_page 归 0、起全量快照；失败 ⇒ dirty 保留；
 *   · WRITE 成功 ⇒ next_page++ 并续提交下一页；失败 ⇒ 不推进，增量批保留脏位，
 *     compact 批（dirty_only==0）则全标脏（擦除已抹掉旧内容）。
 * 每条命令自己 fc_command_release。
 */
void ftl_op_wear(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_op_metadata - L2P 日志块整理 job（回收 / 抗碎片压缩）的完成服务函数（定义在 metadata.c）。
 *
 * 由 g_op_service[FTL_OP_METADATA] 引用；ctx = `md_job_t *`（= &ftl->metadata_dir.job）。
 * 服务函数按 job.phase 记完成结果、改 phase、release 命令：
 *   · RECLAIM     ⇒ 成功 allocator_free_reserved_block；失败留 LOG 下次再试；
 *   · CP_READ     ⇒ 成功 ftl_cmd_read_data 拷进 job.cp_buf（**须早于 release**）→ CP_WRITE；
 *   · CP_WRITE    ⇒ 成功 metadata_dir_commit_region 登记 → CP_READ；失败中止（victim 不擦）；
 *   · CP_ANCHOR_* ⇒ 逐页写 + commit（无论成败都推进游标）；
 *   · CP_RECLAIM  ⇒ 擦完归还 victim → DONE。
 * ⚠ 与 ftl_op_migrate 的差别：本函数**不**推进状态机（不调 metadata_dir_kick）——
 *   整理 job 的 CP_ANCHOR_PREP 里有同步原语（游标未建立时要读池定位），在完成泵里跑
 *   就是嵌套泵。推进交给下一轮 ftl_service 的 metadata_dir_kick。
 */
void ftl_op_metadata(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

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
