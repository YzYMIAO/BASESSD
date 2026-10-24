#ifndef FTL_OP_H
#define FTL_OP_H

/*
 * ftl_op.h — 完成派发面：ctx 旁表 + 实例路由 + op tag/服务函数 + CQ 排空
 *
 * 把「命令提交」与「命令完成」解耦：
 *   提交侧  ftl_op_submit(cmd, op, ctx) —— 打 op tag、登记 ctx、提交，不等待；
 *   完成侧  ftl_op_drain() → ftl_op_dispatch() —— 从 CQ 取完成，按命令携带的
 *           op tag 查 g_op_callbacks[]，并把 ctx 与归属实例交回服务函数。
 *
 * 本模块是整个固件**唯一的完成派发面**：
 *   · ctx 旁表      command_id → ctx（同步等待器 / 异步 job / L2P 行）；
 *   · 实例路由表    (ch,ce,lun) → ftl_ctx_t（全局 CQ 到 per-instance 的桥）；
 *   · op 回调表     op_tag → 完成服务函数；
 *   · CQ 排空       ftl_op_drain（ftl_service_pump 的 Phase 2）。
 *
 * ⚠ 这些表都是**全局单例**（非 per-instance）：command_id 由 FC 全局池铸出、
 *   CQ 是全局的，完成必须先路由再派发。`ftl_op_init` / `ftl_op_route_set`
 *   必须在所有实例 boot 之前调用（见 ftl.c / main.c 的启动顺序）。
 *
 * ── op tag 的语义：**完成服务函数选择器**，不是"这是哪个功能" ──
 *
 * 每一项 = 一个完成服务函数。**同步提交的命令一律带 FTL_OP_SYNC_WAIT**，因为
 * 阻塞调用方的完成处理函数**就是**唤醒等待者；若给同步命令打上例如
 * FTL_OP_JOURNAL 的标签，服务函数与调用者会对同一份状态各推进一次。
 * 某个功能站点被改成异步之后，该站点才把自己的 tag 换成它的功能 op ID。
 *
 * ⇒ 枚举一次性建全（"各功能一个 ID"）；`g_op_callbacks[]` 为每个非 NONE 的 ID 都填了
 *   实现。落在表外（tag ≥ FTL_OP_COUNT）或 FTL_OP_NONE 的完成判为孤儿（release + LOGE）。
 *
 * ── 命令 release 归属（命令生命周期的唯一权威）──
 *
 *   · FTL_OP_SYNC_WAIT 的服务函数**不** release —— 调用者（同步原语）要在
 *     wait 返回后读 cmd 数据（如 ftl_cmd_read_data）才 release，这里释放即悬垂。
 *   · FTL_OP_HOST_IO 与各异步 job 的服务函数**自己** release。
 *
 *   fc_command_release 幂等（已在 FREE 直接返回 OK），但**不能漏**：漏则命令池
 *   泄漏 ⇒ fc_command_alloc 恒 NULL ⇒ 全系统 FTL_AGAIN。
 */

#include "fc_command.h"  /* fc_command_t / fc_command_status_t */
#include "ftl_type.h"    /* DWORD / ftl_status_e */
#include "nand_config.h" /* CH_NUM / CE_NUM / LUN_NUM（路由表维度） */

#include <stdint.h>

/* ── 前向声明（ftl_ctx_t 在 ftl.h 中定义）── */
struct ftl_context;

/* ── ctx 旁表 ──
 *
 * 只留 command_id → ctx：命令该跑哪个完成服务函数由命令自己携带
 * （fc_command_t.op_tag → g_op_callbacks[]），旁表不存 handler。
 * 8 B/项（2 B 位域 + 2 B 补齐 + 4 B 指针，Cortex-M 指针 4 B）。 */
typedef struct
{
    WORD  used   : 1;  /* 1=槽位占用 */
    WORD  cmd_id : 15; /* FC command_id */
    void *ctx;         /* 该命令的私有状态（同步等待 = ftl_sync_waiter_t*；job = &job） */
} ftl_op_ctx_entry_t;

/* 旁表容量。
 *
 * 推导：表按 **command_id** 键，且 ftl_op_ctx_add 保证同一 cmd_id 至多一条表项
 * ⇒ 表项数 ≤ 同时在飞命令数。在飞命令数受 FC 命令池限死 = FC_COMMAND_POOL_DEPTH。
 * 故池深即容量上界，取等即可（ftl_op.c 有 _Static_assert 钉住这层关系，
 * FC 扩容时会编译期报错而不是让这里静默变小）。
 *
 * ⚠ 登记方不止后台 job：L2P 行也逐行登记（load 在飞数无上限，只有 flush 有闸门），
 *   但两者都只能占用**已在飞**的命令 id ⇒ 上界仍是池深。
 *
 * 满 ⇒ ftl_op_ctx_add 返 FTL_ERROR（非背压：容量够就恒不出现，见上界推导）。 */
#define FTL_OP_CTX_TABLE_SIZE (64U)

/* 完成服务函数选择器。每一项 = 一个 g_op_callbacks[] 槽 = 一个"功能"。 */
typedef enum
{
    FTL_OP_NONE = 0,      /* 未标记：派发时判为孤儿（release + LOGE），正常不应出现 */
    FTL_OP_SYNC_WAIT,     /* 唤醒同步等待者（所有 ftl_sync_* 用） */
    FTL_OP_HOST_IO,       /* host 读写子操作（ftl.c） */
    FTL_OP_JOURNAL,       /* Journal 刷页 job（journal.c） */
    FTL_OP_READ_RECOVERY, /* Read Recovery job（read_recovery.c） */
    FTL_OP_GC,            /* GC 搬移 job（migrate.c） */
    FTL_OP_WL,            /* 静态 WL / 慢死预测搬移 job（migrate.c，与 GC 共用引擎） */
    FTL_OP_L2P,           /* L2P region 加载 / 写回（l2p_cache.c） */
    FTL_OP_CHECKPOINT,    /* Checkpoint job（checkpoint.c） */
    FTL_OP_BBT,           /* BBT 持久化 job（bbt.c） */
    FTL_OP_METADATA,      /* L2P 日志块整理 job（metadata_job.c） */
    FTL_OP_COUNT          /* 计数哨兵 */
} ftl_op_id_e;

/* op_tag 只有 7 位（fc_command_t.op_tag）⇒ 槽数上限 128。 */
_Static_assert(FTL_OP_COUNT <= 128U, "ftl_op_id_e 必须装进 fc_command_t.op_tag:7");

typedef void (*ftl_op_callback_fn_t)(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/* ── 同步等待器（FTL_OP_SYNC_WAIT 的 ctx）──
 *
 * 由同步原语在**自己栈上**持有，经 ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter)
 * 登记进旁表；服务函数（ftl_sync_wait_callback）在完成时回填并置 done。 */
typedef struct
{
    WORD                cmd_id;     /* 等待的 command_id */
    bool                done;       /* 服务函数置 true */
    fc_command_status_t completion; /* 服务函数保存的完整 completion 信息 */
} ftl_sync_waiter_t;

/* ── 初始化 / ctx 旁表 ── */

/* ftl_op_init - 清空 ctx 旁表 + 实例路由表（开机一次）
 * ⚠ 清的是**全局**资源 ⇒ 只能调一次，由 main.c 的 ssd_ftl_boot_all 在所有实例
 *   boot 之前调用（**不**在逐实例的 ftl_init 里，否则会抹掉其它实例在飞的登记）。 */
void ftl_op_init(void);

/* ftl_op_ctx_add - 为 cmd_id 登记 ctx。
 *
 * **同一 cmd_id 至多一条表项**由本函数保证：命中已有项即原地覆盖 ctx（并 LOGW），
 * 不追加第二条。理由见实现处注释（命令 id 复用 + 残留项会让 take 摘到陈旧 ctx）。
 *
 * @return FTL_OK；FTL_ERROR 表满（FTL_OP_CTX_TABLE_SIZE，见容量推导）。
 */
ftl_status_e ftl_op_ctx_add(WORD cmd_id, void *ctx);

/* ftl_op_ctx_remove - 释放 cmd_id 的表项（未找到 no-op，幂等） */
void ftl_op_ctx_remove(WORD cmd_id);

/* ftl_op_ctx_take - 查 + **摘链** + 返回 ctx（未登记返回 NULL）。
 * 派发路径专用：先摘链再回调，容许服务函数在自己的回调里重入登记/释放表项。 */
void *ftl_op_ctx_take(WORD cmd_id);

/* ftl_op_ctx_count - 当前占用（未决）的表项数（观测 / 自检口）。
 * ⚠ 后台档位判定（bg_scheduler_level_select）**刻意不**用它：档位要的是 host IO 忙闲，
 *   而 host 请求不经旁表（ctx == NULL）⇒ 旁表占用与 host 负载无关。
 *   档位判据是 pending 占用（见 ftl.c 的 bg_scheduler_level_select）。 */
WORD ftl_op_ctx_count(void);

/* ── 实例路由 ── */

/* ftl_op_route_set - 设置实例路由表。
 *
 * 全局旁表保持单一（FC CQ 全局、cmd_id 全局唯一），派发时按命令 ch/ce/lun
 * 路由到对应 FTL 实例。本函数在 main.c 填完 g_ftl_ptrs 后调用一次——
 * ⚠ **必须早于逐实例 boot**：boot 期的 full metadata rebuild 就会经 l2p_read_sync 抽完成表。
 * 传 NULL 则清空（boot 失败回退 fc_service 时用）。 */
void ftl_op_route_set(struct ftl_context *route[][CE_NUM][LUN_NUM]);

/* ftl_op_route_get - 按 (ch,ce,lun) 取实例（未设置返回 NULL；越界断言） */
struct ftl_context *ftl_op_route_get(BYTE ch, BYTE ce, BYTE lun);

/* ftl_op_route_get_region - 按全局 L2P region 取 owner 实例。
 * dim = global_region % FTL_DIM_TOTAL，再用 l2p_cache.h 的 ftl_dim_ch/ce/lun 分解后转
 * ftl_op_route_get。共享 L2P cache 的 owner 解析用它，避免把 region→die 的分解
 * 散落在 cache/CLI 各处（H13）；分解式只有 ftl_dim_* 一份实现（C9）。 */
struct ftl_context *ftl_op_route_get_region(DWORD global_region);

/* ── 提交 ── */

/*
 * ftl_op_submit - 标记 op tag、登记完成服务、提交命令。
 *
 * 返回 FTL_OK **只保证"已提交"**，不保证完成；命令的 release 归调用者
 * （FTL_OP_SYNC_WAIT 站点）或服务函数（异步 job 站点）。
 *
 * ctx == NULL ⇒ 不登记（服务函数自己从 pending 表等派生状态）；
 * ctx != NULL ⇒ 登记到 ctx 旁表，派发时作为第三形参交回。
 *
 * ⚠ **登记必须早于 fc_submit**（同 pending_add 的先例）：提交后完成可能立刻被
 *   drain 处理，那时 ctx 还没登记就会丢完成。提交失败则回滚登记。
 *
 * @return FTL_OK 已提交；FTL_AGAIN 提交被背压拒（SQ 满等瞬态）；FTL_ERROR 旁表满。
 */
ftl_status_e ftl_op_submit(fc_command_t *cmd, ftl_op_id_e op, void *ctx);

/*
 * ftl_op_submit_batch - 批量版本：n 条命令共用同一 op tag / ctx，全进或全不进。
 *
 * 语义与 ftl_op_submit 相同（先登记、后提交），但用 fc_submit_batch 保证 batch
 * 的原子性。任一登记失败 ⇒ 回滚已登记项并返回 FTL_ERROR；队列不足 ⇒ 回滚并返回
 * FTL_AGAIN。调用者仍拥有全部命令（失败时须自行 release）。
 *
 * @param cmds 命令数组（非空、n>0）；op/ctx 同 ftl_op_submit。
 */
ftl_status_e ftl_op_submit_batch(fc_command_t *const cmds[], DWORD n, ftl_op_id_e op, void *ctx);

/* ── 派发 / 排空 ── */

/*
 * ftl_op_dispatch - 完成派发（ftl_op_drain 的唯一出口）。
 *
 * 按 completion->op_tag 查 g_op_callbacks[]；未知 tag ⇒ 孤儿，release + LOGE。
 * ctx 从 ctx 旁表取出（先摘链再回调，容许服务函数重入登记）。
 */
void ftl_op_dispatch(const fc_command_status_t *completion);

/*
 * ftl_op_is_in_dispatch - 当前是否在完成服务函数（或其嵌套调用）里。
 *
 * 用途：给"服务函数内不得同步等待"这条约定一个**可判定的**依据。服务函数里若再
 * 跑一遍完成泵（ftl_sync_submit_wait 的 drain），就是嵌套泵
 * ——那正是本次解耦要消除的东西。深度计数（不是布尔）：嵌套 drain 会再进一层。
 *
 * 已落地的一处用法：jrn_append 页满时若异步刷页起不来，**不在**派发上下文里
 * 同步回退（会嵌套），改为丢弃该条目 + LOGE 并返回 false（见 journal.c）。
 * 迁移侧必须把 false 当失败中止（victim 不擦）；host 写路径当前只能记日志继续。
 *
 * @return true = 在派发内；false = 不在。
 */
bool ftl_op_is_in_dispatch(void);

/*
 * ftl_op_drain - 消费所有 CQ entry 并派发（ftl_service_pump 的 Phase 2）。
 *
 * 循环 fc_poll_completion()，每条交 ftl_op_dispatch()（按命令的 op_tag 查服务函数数组）。
 * ⚠ 摘链在调用服务函数**之前**（ftl_op_dispatch 内）——服务函数可能重入登记/释放
 *   表项，先摘可避免它在自己的回调里踩到本项。循环持续到 CQ 抽干为止。
 */
void ftl_op_drain(void);

/* ── 各模块的完成服务函数（由 g_op_callbacks[] 引用）── */

/*
 * ftl_host_io_callback - host 读写子操作的完成服务函数（定义在 ftl.c）。
 *
 * 由 g_op_callbacks[FTL_OP_HOST_IO] 引用；ctx 不用（服务函数自己 pend_find）。
 */
void ftl_host_io_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_journal_callback - Journal 刷页 job 的完成服务函数（定义在 journal.c）。
 *
 * 由 g_op_callbacks[FTL_OP_JOURNAL] 引用；ctx = `l2p_jrn_t *`（= ftl->journal），
 * 服务函数按 job.phase 推进状态机（页写完成 ⇒ 推进写指针 / 起换块擦除；擦完成 ⇒ 收尾）。
 * 每条命令自己 fc_command_release。
 */
void ftl_journal_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_read_recovery_callback - Read Recovery job 的完成服务函数（定义在 read_recovery.c）。
 *
 * 由 g_op_callbacks[FTL_OP_READ_RECOVERY] 引用；ctx = `rr_handle_t *`（= &ftl->read_recovery_handle）。
 * 服务函数按 job.stage 推进（切读重试模式 / 重试读 / 恢复默认模式 / 邻居读），终态
 * 释放被托管的 pending 槽并让 host 读组结算。每条命令自己 fc_command_release。
 */
void ftl_read_recovery_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_migrate_callback - 搬移 job（GC / 静态 WL / 慢死预测）的完成服务函数（定义在 migrate.c）。
 *
 * 由 g_op_callbacks[FTL_OP_GC] 与 [FTL_OP_WL] **共同**引用（同一台引擎，靠 job.kind
 * 区分 victim 来源、job.post 区分收尾）；ctx = `mig_handle_t *`（= ftl->mig_handle）。
 * 服务函数按 job.phase 推进（读完成 ⇒ 解 META + 反查 L2P；写完成 ⇒ 改 L2P/Journal +
 * 迁移有效计数；擦完成 ⇒ 归还/退役/标坏），每条命令自己 fc_command_release。
 */
void ftl_migrate_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_l2p_callback - L2P region 加载 / 写回的完成服务函数（定义在 l2p_cache.c）。
 *
 * 由 g_op_callbacks[FTL_OP_L2P] 引用；ctx = `l2p_cache_line_t *`（提交时传的行指针）。
 * 服务函数按 line->state 收尾：
 *   · LOADING  ⇒ 成功把整页 region 拷进 line->entries（**须早于 release**：读的数据在
 *                desc 链上），置 CLEAN；失败同样置 CLEAN（entries 保持全 INVALID = 未映射）。
 *   · FLUSHING ⇒ 成功 md_commit_region 登记新位置 + CLEAN + 清 region 脏位；
 *                失败置回 DIRTY 保留下轮重试。
 * 每条命令自己 fc_command_release。
 */
void ftl_l2p_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_checkpoint_callback - Checkpoint job 的完成服务函数（定义在 checkpoint.c）。
 *
 * 由 g_op_callbacks[FTL_OP_CHECKPOINT] 引用；ctx = `cp_handle_t *`（= ftl->cp_handle）。
 * 服务函数按 job.phase 记完成结果、推进 phase、release 命令。
 * ⚠ 与 ftl_migrate_callback 的差别：本函数**不**推进状态机（不调 cp_handle）——
 *   CP 的阶段里有同步原语（游标未建立时要读池定位），在完成泵里跑就是嵌套泵。
 *   推进交给下一轮 ftl_service 的 cp_handle。
 */
void ftl_checkpoint_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_bbt_callback - BBT 持久化 job 的完成服务函数（定义在 bbt.c）。
 *
 * 由 g_op_callbacks[FTL_OP_BBT] 引用；ctx = `alc_t *`（= &ftl->alloc，
 * 即命令的实例归属；形参 ftl 不用）。服务函数按 alloc->bbt.handle.phase 收尾：
 *   · ERASE 成功 ⇒ 起写 record；失败 ⇒ 游标不推进、dirty 保留；
 *   · WRITE 完成 ⇒ **无论成败都推进池游标**（半编程槽不可复用），成功清 dirty。
 * 每条命令自己 fc_command_release。
 */
void ftl_bbt_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/*
 * ftl_metadata_callback - L2P 日志块整理 job（回收 / 抗碎片压缩）的完成服务函数（定义在 metadata_job.c）。
 *
 * 由 g_op_callbacks[FTL_OP_METADATA] 引用；ctx = `md_dir_cache_t *`
 * （模块上下文；服务函数内部取 `&dir->handle`）。
 * 服务函数按 job.phase 记完成结果、改 phase、release 命令：
 *   · RECLAIM     ⇒ 成功清待回收位 + alc_free_push；失败保留位，下轮重试；
 *   · CP_READ     ⇒ 成功 ftl_cmd_read_data 拷进 job.cp_buf（**须早于 release**）→ CP_WRITE；
 *   · CP_WRITE    ⇒ 成功 md_commit_region 登记 → CP_READ；失败中止（victim 不擦）；
 *   · CP_ANCHOR_* ⇒ 逐页写 + commit（无论成败都推进游标）；
 *   · CP_RECLAIM  ⇒ 擦完归还 victim → DONE。
 * ⚠ 与 ftl_migrate_callback 的差别：本函数**不**推进状态机（不调 md_handle）——
 *   整理 job 的 CP_ANCHOR_PREP 里有同步原语（游标未建立时要读池定位），在完成泵里跑
 *   就是嵌套泵。推进交给下一轮 ftl_service 的 md_handle。
 */
void ftl_metadata_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

#endif /* FTL_OP_H */
