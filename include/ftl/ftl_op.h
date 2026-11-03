#ifndef FTL_OP_H
#define FTL_OP_H

/*
 * ftl_op.h — 完成派发面：request 表 + 实例路由 + op/服务函数 + CQ 排空
 *
 * 提交与完成解耦：
 *   提交侧  ftl_op_submit(cmd, op, ctx)：登记 request（op + ctx）、提交，不等待。
 *   完成侧  ftl_op_drain() → ftl_op_dispatch()：从 CQ 取完成，用 command_id
 *           取回 request，按 request.op 查 g_op_callbacks[]，并把 ctx 与归属实例
 *           交回服务函数。
 *
 * 本模块是固件唯一的完成派发面：
 *   - request 表    command_id → {ctx, op}（按命令池槽位直接索引）；
 *   - 实例路由表    (ch,ce,lun) → die → ftl_ctx_t（全局 CQ 到 per-instance 的桥）；
 *   - op 回调表     request.op → 完成服务函数；
 *   - CQ 排空       ftl_op_drain（ftl_service_pump 的完成泵步）。
 *
 * 约束：这些表都是全局单例（非 per-instance）。command_id 由 FC 全局池分配，
 * CQ 是全局的，所以完成必须先路由再派发。ftl_op_init / ftl_op_route_set 必须在
 * 所有实例 boot 之前调用（见 main.c 的启动顺序）。
 *
 * request.op 的语义是“完成服务函数选择器”，不是“这是哪个功能”：
 *   - op 存在 FTL request 表里，CQE 和 FC 命令对象都不携带；完成派发用
 *     command_id 直接取回 request。
 *   - 同步提交的命令一律带 FTL_OP_SYNC_WAIT，因为阻塞调用方的完成处理函数就是
 *     唤醒等待者；若给同步命令打 FTL_OP_JOURNAL，服务函数与调用者会对同一份
 *     状态各推进一次。
 *   - 某个功能站点改成异步之后，才把它自己的 tag 换成对应的功能 op ID。
 *   - 枚举一次性建全（每个功能一个 ID）；g_op_callbacks[] 为每个非 NONE 的 ID
 *     都填了实现。落在表外（tag >= FTL_OP_COUNT）或 FTL_OP_NONE 的完成按孤儿
 *     处理：release + LOGE。
 *
 * 命令 release 归属：
 *   - FTL_OP_SYNC_WAIT 的服务函数不 release。调用者（同步原语）要在 wait 返回后
 *     读 cmd 数据（如 ftl_cmd_read_data）才 release，这里释放即悬垂。
 *   - FTL_OP_HOST_IO 与各异步 job 的服务函数自己 release。
 *   - fc_command_release 幂等，但不能漏。漏则命令池泄漏，fc_command_alloc 恒
 *     NULL，全系统 FTL_AGAIN。
 */

#include "fc.h"          /* fc_command_t / fc_command_status_t / FC_COMMAND_POOL_DEPTH */
#include "ftl_type.h"    /* DWORD / ftl_status_e */
#include "nand_config.h" /* CH_NUM / CE_NUM / LUN_NUM（路由表维度） */

#include <stdint.h>

/* 完成服务函数选择器。每一项 = 一个 g_op_callbacks[] 槽 = 一个"功能"。 */
typedef enum
{
    FTL_OP_NONE = 0,      /* 未标记：派发时判为孤儿（release + LOGE），正常不应出现 */
    FTL_OP_SYNC_WAIT,     /* 唤醒同步等待者（所有 ftl_sync_* 用） */
    FTL_OP_HOST_IO,       /* host 读写子操作（ftl.c） */
    FTL_OP_JOURNAL,       /* Journal 刷页 job（journal.c） */
    FTL_OP_READ_RECOVERY, /* Read Recovery job（read_recovery.c） */
    FTL_OP_GC,            /* GC 搬移 job（migrate.c） */
    FTL_OP_WL,            /* 静态 WL / 预测性退役搬移 job（migrate.c，与 GC 共用引擎） */
    FTL_OP_L2P,           /* L2P region 加载 / 写回（l2p_cache.c） */
    FTL_OP_CHECKPOINT,    /* Checkpoint job（checkpoint.c） */
    FTL_OP_BBT,           /* BBT 持久化 job（bbt.c） */
    FTL_OP_METADATA,      /* L2P 日志块整理 job（metadata_job.c） */
    FTL_OP_CACHE_PAD,     /* cache 写序列的 padding END（ftl.c：有序收尾设备序列） */
    FTL_OP_COUNT          /* 计数哨兵 */
} ftl_op_id_e;

/* ── request 表 ──
 *
 * 按 FC command_id 直接索引：command_id 就是命令池槽位号，范围
 * 0..FC_COMMAND_POOL_DEPTH-1。每个在飞命令都有一条表项，记录其完成服务函数
 * （op）与私有状态（ctx）；CQE 不携带 op。
 */
typedef struct
{
    void       *ctx;     /* 该命令的私有状态（同步等待 = ftl_sync_waiter_t*；job = &job） */
    ftl_op_id_e op;      /* 完成服务函数选择器 */
    bool        pending; /* true = 该 command_id 有登记项 */
} ftl_op_req_t;

/* request 表容量 = FC 命令池深度。
 *
 * command_id 是池槽位号，直接索引后表项数 = 池深；不再有“表满”路径，也不会
 * 出现同一 command_id 两条表项。非法 command_id（>= 池深）由入口返回
 * FTL_ERROR。
 */
#define FTL_OP_REQ_TABLE_SIZE FC_COMMAND_POOL_DEPTH

typedef void (*ftl_op_callback_fn_t)(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/* ── 同步等待器（FTL_OP_SYNC_WAIT 的 ctx）──
 *
 * 由同步原语在自己栈上持有，经 ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter)
 * 登记进 request 表；服务函数（ftl_sync_wait_callback）在完成时回填并置 done。 */
typedef struct
{
    WORD                cmd_id;     /* 等待的 command_id */
    bool                done;       /* 服务函数置 true */
    fc_command_status_t completion; /* 服务函数保存的完整 completion 信息 */
} ftl_sync_waiter_t;

/* ── 初始化 / request 表 ── */

/**
 * @brief 清空 request 表 + 实例路由表（开机一次）。
 *
 * 清的是全局资源，只能调一次。由 main.c 的 ssd_ftl_boot_all 在所有实例 boot
 * 之前调用；不要放进逐实例的 ftl_init，否则会抹掉其它实例在飞的登记。
 */
void ftl_op_init(void);

/**
 * @brief 为 cmd_id 登记 request（op + 私有状态）。
 *
 * 按 command_id 直接索引；每个在飞命令都有一条表项。重复登记时覆盖旧项并打印
 * LOGW。要求私有 ctx 的 op（Journal/L2P/GC/... 见实现）传 NULL 时返回 FTL_ERROR。
 *
 * @param cmd_id FC 命令 id；必须 < FTL_OP_REQ_TABLE_SIZE。
 * @param op     完成服务函数选择器。
 * @param ctx    完成时交回服务函数的私有状态；NULL = 无私有状态。
 * @return FTL_OK —— 登记成功；
 * @return FTL_ERROR —— cmd_id 越界，或该 op 要求 ctx 但 ctx == NULL。
 */
ftl_status_e ftl_op_req_add(WORD cmd_id, ftl_op_id_e op, void *ctx);

/**
 * @brief 释放 cmd_id 的 request 表项（未找到为 no-op，幂等）。
 *
 * @param cmd_id 命令 id。
 */
void ftl_op_req_remove(WORD cmd_id);

/**
 * @brief 查表、清表项并取出 {ctx, op}。
 *
 * 派发路径专用：先清表项再回调，容许服务函数在自己的回调里重入登记/释放表项。
 *
 * @param cmd_id  命令 id。
 * @param ctx_out 输出：登记的 ctx；可为 NULL。
 * @param op_out  输出：完成服务函数选择器。
 * @return true —— 找到并已清项；
 * @return false —— 未登记（迟到/孤儿完成）。
 */
bool ftl_op_req_take(WORD cmd_id, void **ctx_out, ftl_op_id_e *op_out);

/**
 * @brief 当前未决 request 表项数（所有在飞命令，含 host IO）。
 *
 * 用于 UT/quiesce 判断“全部命令完成”；后台档位判定不用它（档位看 pending）。
 *
 * @return 当前占用的表项数。
 */
WORD ftl_op_req_count(void);

/* ── 实例路由 ── */

/**
 * @brief 设置实例路由表。
 *
 * 全局 request 表保持单一（FC CQ 全局、cmd_id 全局唯一）；路由表按紧凑 die 号索引。
 * 命令完成仍带 (ch,ce,lun)，由 chcelun_2_die() 转换后查表。
 *
 * 本函数在 main.c 填完 g_ftl_ptrs 后调用一次，必须早于逐实例 boot：boot 期的
 * full metadata rebuild 就会经 l2p_read_sync 抽完成表。
 *
 * @param route die → 实例指针表（长度 FTL_DIE_MAX）；NULL = 清空。
 */
void ftl_op_route_set(ftl_ctx_t *const *route);

/**
 * @brief 按 (ch,ce,lun) 取实例。
 *
 * @param ch  通道号。
 * @param ce  片选号。
 * @param lun LUN 号。
 * @return 实例指针；
 * @return NULL —— 稀疏拓扑空洞或未设置。
 */
ftl_ctx_t *ftl_op_route_get(BYTE ch, BYTE ce, BYTE lun);

/**
 * @brief 按 compact die 号取实例。
 *
 * @param die compact die 号；必须 < DIE_MAX。
 * @return 实例指针；
 * @return NULL —— route 表未设置。
 */
ftl_ctx_t *ftl_op_route_get_die(BYTE die);

/**
 * @brief 按全局 L2P region 取 owner 实例。
 *
 * die = global_region % DIE_MAX，直接查 die 路由表。共享 L2P cache 的 owner
 * 解析用它，避免 region→die 的分解散落在 cache/CLI 各处。
 *
 * @param global_region 全局 L2P region 号。
 * @return owner 实例。
 */
ftl_ctx_t *ftl_op_route_get_region(DWORD global_region);

/**
 * @brief 是否有任何实例的 cache 流式序列处于打开状态。
 *
 * @return true —— 至少一个实例的 cache_stream.seq.active || draining；
 * @return false —— 所有实例都没有打开的序列。
 *
 * @note draining 表示窗口已提交完、命令仍在飞，门控同样关着。
 * @note 提交侧 deadline（CP/MIG/RR/MD 的 FTL_SUBMIT_TIMEOUT_MS）用它区分
 *       “序列门控造成的预期等待”与“真的提交不出去”。多 LUN 下是保守判据：
 *       只会延后中止，不会漏判。
 */
bool ftl_op_cache_seq_active(void);

/**
 * @brief 后台 job 的提交被拒是否属于预期等待（挂起提交 deadline 用）。
 *
 * @return true —— cache 序列打开，或主机组仍在飞；
 * @return false —— 不属于预期等待。
 */
bool ftl_op_bg_submit_deferred(void);

/* ── 提交 ── */

/**
 * @brief 标记 op tag、登记完成服务、提交命令。
 *
 * 返回 FTL_OK 只保证“已提交”，不保证完成。命令的 release 归调用者
 * （FTL_OP_SYNC_WAIT 站点）或服务函数（异步 job 站点）。
 *
 * 每个在飞命令都登记 request（op + ctx）；ctx == NULL 表示无私有状态（如
 * host I/O / cache padding），仍会登记 request 表项。要求 ctx 的 op 传 NULL
 * 会在登记时返回 FTL_ERROR。
 *
 * 登记必须早于 fc_submit：提交后完成可能立刻被 drain 处理，那时 ctx 还没登记
 * 就会丢完成。提交失败则回滚登记。
 *
 * @param cmd FC 命令（已配置好，未提交）。
 * @param op  op tag（选择完成服务函数）。
 * @param ctx 完成时交回的上下文；NULL = 无私有状态。
 * @return FTL_OK —— 已提交；
 * @return FTL_AGAIN —— 被背压拒（SQ 满等瞬态）；
 * @return FTL_ERROR —— request 登记失败（id 越界 / 该 op 要求 ctx）。
 */
ftl_status_e ftl_op_submit(fc_command_t *cmd, ftl_op_id_e op, void *ctx);

/**
 * @brief 批量版本：n 条命令共用同一 op tag / ctx，全进或全不进。
 *
 * 语义与 ftl_op_submit 相同（先登记、后提交），但用 fc_submit_batch 保证
 * 批的原子性。任一登记失败 → 回滚已登记项并返回 FTL_ERROR；队列不足 →
 * 回滚并返回 FTL_AGAIN。调用者仍拥有全部命令（失败时须自行 release）。
 *
 * @param cmds 命令数组（非空、n > 0）。
 * @param n    命令条数。
 * @param op   op tag。
 * @param ctx  完成时交回的上下文；NULL = 无私有状态。
 * @return FTL_OK —— 全部提交；
 * @return FTL_AGAIN —— 队列不足（已回滚）；
 * @return FTL_ERROR —— request 登记失败（已回滚）。
 */
ftl_status_e ftl_op_submit_batch(fc_command_t *const cmds[], DWORD n, ftl_op_id_e op, void *ctx);

/* ── 派发 / 排空 ── */

/**
 * @brief 完成派发（ftl_op_drain 的唯一出口）。
 *
 * 用 command_id 取回命令和 request 表项 {ctx, op}；按 op 查 g_op_callbacks[]；
 * 缺 request / 未知 op → 孤儿，release + LOGE。先清 request 表项再回调，容许
 * 服务函数重入登记。
 *
 * @param completion FC 完成项。
 */
void ftl_op_dispatch(const fc_command_status_t *completion);

/**
 * @brief 当前是否在完成服务函数（或其嵌套调用）里。
 *
 * 用途：给“服务函数内不得同步等待”这条约定一个可判定的依据。服务函数里若再
 * 跑一遍完成泵（ftl_sync_submit_wait 的 drain），就是嵌套泵。深度计数（不是
 * 布尔）：嵌套 drain 会再进一层。
 *
 * 已落地的一处用法：jrn_append 页满时若异步刷页起不来，不在派发上下文里同步
 * 回退（会嵌套），改为丢弃该条目 + LOGE 并返回 false（见 journal.c）。迁移侧
 * 必须把 false 当失败中止（victim 不擦）；host 写路径当前只能记日志继续。
 *
 * @return true —— 在派发内；
 * @return false —— 不在派发内。
 */
bool ftl_op_is_in_dispatch(void);

/**
 * @brief 消费所有 CQ entry 并派发（ftl_service_pump 的完成泵步）。
 *
 * 循环 fc_poll_completion()，每条交 ftl_op_dispatch()。先清 request 表项再调用
 * 服务函数：服务函数可能重入登记/释放表项，先摘可避免它踩到本项。循环持续
 * 到 CQ 抽干为止。
 */
void ftl_op_drain(void);

/* ── 各模块的完成服务函数（由 g_op_callbacks[] 引用）── */

/**
 * @brief host 读写子操作的完成服务函数（定义在 ftl.c）。
 *
 * 由 g_op_callbacks[FTL_OP_HOST_IO] 引用；ctx 不用（服务函数自己 pend_find）。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文。
 */
void ftl_host_io_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/**
 * @brief cache 写序列 padding END（本仓自拟） 的完成服务函数（定义在 ftl.c）。
 *
 * 由 g_op_callbacks[FTL_OP_CACHE_PAD] 引用；ctx 不用（序列状态在 ftl->cache_stream.seq）。
 * 语义：用本命令的 FAIL_CACHE 结算未确认 FIFO 里的最后一页（该页已对 host 提前
 * 回执），padding 页自身不映射、失败忽略；随后关闭序列、放开 L2P 门控。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文（当前恒 NULL）。
 */
void ftl_cache_pad_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/**
 * @brief Journal 刷页 job 的完成服务函数（定义在 journal.c）。
 *
 * 由 g_op_callbacks[FTL_OP_JOURNAL] 引用；ctx = `l2p_jrn_t *`（= ftl->journal），
 * 服务函数按 job.phase 推进状态机（页写完成 → 推进写指针 / 起换块擦除；擦完成 → 收尾）。
 * 每条命令自己 fc_command_release。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文。
 */
void ftl_journal_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/**
 * @brief Read Recovery job 的完成服务函数（定义在 read_recovery.c）。
 *
 * 由 g_op_callbacks[FTL_OP_READ_RECOVERY] 引用；ctx = `rr_handle_t *`（= &ftl->read_recovery_handle）。
 * 服务函数按 job.stage 推进（切读重试模式 / 重试读 / 恢复默认模式 / 邻居读），终态
 * 释放被托管的 pending 槽并让 host 读组结算。每条命令自己 fc_command_release。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文。
 */
void ftl_read_recovery_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/**
 * @brief 搬移 job（GC / 静态 WL / 预测性退役）的完成服务函数（定义在 migrate.c）。
 *
 * 由 g_op_callbacks[FTL_OP_GC] 与 [FTL_OP_WL] 共同引用（同一台引擎，靠 job.kind
 * 区分 victim 来源、job.post 区分收尾）；ctx = `mig_handle_t *`（= ftl->mig_handle）。
 * 服务函数按 job.phase 推进（读完成 → 解 META + 反查 L2P；写完成 → 改 L2P/Journal +
 * 迁移有效计数；擦完成 → 归还/退役/标坏），每条命令自己 fc_command_release。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文。
 */
void ftl_migrate_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/**
 * @brief L2P region 加载 / 写回的完成服务函数（定义在 l2p_cache.c）。
 *
 * 由 g_op_callbacks[FTL_OP_L2P] 引用；ctx = `l2p_cache_line_t *`（提交时传的行指针）。
 * 服务函数按 line->state 收尾：
 *   · LOADING  → 成功把整页 region 拷进 line->entries（须早于 release：读的数据在
 *                desc 链上），置 CLEAN；失败同样置 CLEAN（entries 保持全 INVALID = 未映射）。
 *   · FLUSHING → 成功 md_commit_region 登记新位置 + CLEAN + 清 region 脏位；
 *                失败置回 DIRTY 保留下轮重试。
 * 每条命令自己 fc_command_release。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文。
 */
void ftl_l2p_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/**
 * @brief Checkpoint job 的完成服务函数（定义在 checkpoint.c）。
 *
 * 由 g_op_callbacks[FTL_OP_CHECKPOINT] 引用；ctx = `cp_handle_t *`（= ftl->cp_handle）。
 * 服务函数按 job.phase 记完成结果、推进 phase、release 命令。
 * 注意：与 ftl_migrate_callback 的差别：本函数不推进状态机（不调 cp_handle）——
 *   CP 的阶段里有同步原语（游标未建立时要读池定位），在完成泵里跑就是嵌套泵。
 *   推进交给下一轮 ftl_service 的 cp_handle。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文。
 */
void ftl_checkpoint_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/**
 * @brief BBT 持久化 job 的完成服务函数（定义在 bbt.c）。
 *
 * 由 g_op_callbacks[FTL_OP_BBT] 引用；ctx = `alc_t *`（= &ftl->alloc，
 * 即命令的实例归属；形参 ftl 不用）。服务函数按 alloc->bbt.handle.phase 收尾：
 *   · ERASE 成功 → 起写 record；失败 → 游标不推进、dirty 保留；
 *   · WRITE 完成 → 无论成败都推进池游标（半编程槽不可复用），成功清 dirty。
 * 每条命令自己 fc_command_release。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文。
 */
void ftl_bbt_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

/**
 * @brief L2P 日志块整理 job（回收 / 抗碎片压缩）的完成服务函数（定义在 metadata_job.c）。
 *
 * 由 g_op_callbacks[FTL_OP_METADATA] 引用；ctx = `md_dir_cache_t *`
 * （模块上下文；服务函数内部取 `&dir->handle`）。
 * 服务函数按 job.phase 记完成结果、改 phase、release 命令：
 *   · RECLAIM     → 成功清待回收位 + alc_free_push；失败保留位，下轮重试；
 *   · CP_READ     → 成功 ftl_cmd_read_data 拷进 job.cp_buf（须早于 release）→ CP_WRITE；
 *   · CP_WRITE    → 成功 md_commit_region 登记 → CP_READ；失败中止（victim 不擦）；
 *   · CP_DIR_CKPT_* → 逐页写 + commit（无论成败都推进游标）；
 *   · CP_RECLAIM  → 擦完归还 victim → DONE。
 * 注意：与 ftl_migrate_callback 的差别：本函数不推进状态机（不调 md_handle）——
 *   整理 job 的 CP_DIR_CKPT_PREP 里有同步原语（游标未建立时要读池定位），在完成泵里跑
 *   就是嵌套泵。推进交给下一轮 ftl_service 的 md_handle。
 *
 * @param ftl 实例。
 * @param completion FC 完成状态。
 * @param ctx 提交时登记的上下文。
 */
void ftl_metadata_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);

#endif /* FTL_OP_H */
