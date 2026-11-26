#ifndef FTL_OP_H
#define FTL_OP_H

/* multi-impl: dispatch-table（g_op_callbacks 各槽的完成服务函数权威声明） */

/*
 * ftl_op.h — 完成派发面：request 表 + 实例路由 + op/服务函数 + CQ 排空
 *
 * 提交与完成解耦：
 *   提交侧  ftl_op_submit(cmd, op, ctx)：登记 request（op + ctx）、提交，不等待。
 *   完成侧  ftl_op_drain()：循环从 CQ 取完成，用 cmd_id 取回 request，按
 *           request.op 查 g_op_callbacks[]，并把 ctx 与归属实例交回服务函数。
 *
 * 本模块是固件唯一的完成派发面：
 *   - request 表    cmd_id → {ctx, op}（按命令池槽位直接索引）；
 *   - 实例路由表    (ch,ce,lun) → die → ftl_ctx_s（全局 CQ 到 per-instance 的桥）；
 *   - op 回调表     request.op → 完成服务函数；
 *   - CQ 排空       ftl_op_drain（ftl_service_pump 的完成泵步）。
 *
 * 约束：这些表都是全局单例（非 per-instance）。cmd_id 由 FC 全局池分配，
 * CQ 是全局的，所以完成必须先路由再派发。ftl_op_init / ftl_op_route_set 必须在
 * 所有实例 boot 之前调用（见 main.c 的启动顺序）。
 *
 * request.op 的语义是“完成服务函数选择器”，不是“这是哪个功能”：
 *   - op 存在 FTL request 表里，CQE 和 FC 命令对象都不携带；完成派发用
 *     cmd_id 直接取回 request。
 *   - 同步提交的命令一律带 FTL_OP_SYNC_WAIT，因为阻塞调用方的完成处理函数就是
 *     唤醒等待者；若给同步命令打 FTL_OP_JOURNAL，服务函数与调用者会对同一份
 *     状态各推进一次。
 *   - 某个功能站点改成异步之后，才把提交时用的 op 从 FTL_OP_SYNC_WAIT 换成
 *     对应的功能 op ID。
 *   - 枚举一次性建全（每个功能一个 ID）；g_op_callbacks[] 为每个非 NONE 的 ID
 *     都填了实现。落在表外（op ID >= FTL_OP_COUNT）或 FTL_OP_NONE 的完成按
 *     孤儿处理：release + LOGE。
 *
 * 命令 release 归属：
 *   - FTL_OP_SYNC_WAIT 的服务函数不 release。调用者（同步原语）要在 wait 返回后
 *     读 cmd 数据（如 ftl_cmd_read_data）才 release，这里释放即悬垂。
 *   - FTL_OP_HOST_IO 与各异步 job 的服务函数自己 release。
 *   - fc_command_release 幂等，但不能漏。漏则命令池泄漏，fc_command_alloc 恒
 *     NULL，全系统 FTL_AGAIN。
 */

#include "fc_interface.h" /* fc_cmd_s / fc_cmd_status_s / FC_COMMAND_POOL_DEPTH */
#include "ftl_type.h"     /* DWORD / ftl_status_e */
#include "nand_config.h"  /* CH_NUM / CE_NUM / LUN_NUM（路由表维度） */
#include "pending.h"      /* pend_req_s（终身计数接线） */
#include "stats.h"        /* SSD_STATS_ENABLE（统计专用声明按档裁剪） */

#include <stdint.h>

/* 完成服务函数选择器。每一项 = 一个 g_op_callbacks[] 槽 = 一个"功能"。 */
typedef enum FTL_OPERATION_ID_E
{
    FTL_OP_NONE = 0,      /* 未标记：派发时判为孤儿（release + LOGE），正常不应出现 */
    FTL_OP_SYNC_WAIT,     /* 唤醒同步等待者（所有 ftl_sync_* 用） */
    FTL_OP_HOST_IO,       /* host 读写子操作（ftl.c） */
    FTL_OP_JOURNAL,       /* Journal 刷页 job（journal.c） */
    FTL_OP_READ_RECOVERY, /* Read Recovery job（read_recovery.c） */
    FTL_OP_GC,            /* GC 搬移 job（migrate.c） */
    FTL_OP_WL,            /* 静态 WL / 预测性退役搬移 job（migrate.c，与 GC 共用引擎） */
    FTL_OP_L2P,           /* L2P region 加载 / 写回（l2p_cache.c） */
    FTL_OP_L2P_PREFETCH,  /* L2P region 预取加载（同回调；不请求 cache 序列断点/让行） */
    FTL_OP_CHECKPOINT,    /* Checkpoint job（checkpoint.c） */
    FTL_OP_BBT,           /* BBT 持久化 job（bbt.c） */
    FTL_OP_METADATA,      /* L2P 日志块整理 job（metadata_job.c） */
    FTL_OP_CACHE_PAD,     /* cache 写序列的 padding END（ftl.c：有序收尾设备序列） */
    FTL_OP_COUNT          /* 计数哨兵 */
} ftl_op_id_e;

/* ── request 表 ──
 *
 * 按 FC cmd_id 直接索引：cmd_id 就是命令池槽位号，范围
 * 0..FC_COMMAND_POOL_DEPTH-1。每个在飞命令都有一条表项，记录其完成服务函数
 * （op）与私有状态（ctx）；CQE 不携带 op。
 */
typedef struct FTL_OPERATION_REQUEST_S
{
    void       *ctx;     /* 该命令的私有状态（同步等待 = ftl_sync_waiter_s*；job = &job） */
    ftl_op_id_e op;      /* 完成服务函数选择器 */
    BOOL        pending; /* true = 该 cmd_id 有登记项 */
} ftl_op_req_s;

/* request 表容量 = FC 命令池深度。
 *
 * cmd_id 是池槽位号，直接索引后表项数 = 池深：没有“表满”路径，也不会
 * 出现同一 cmd_id 两条表项。非法 cmd_id（>= 池深）由入口返回
 * FTL_ERROR。
 */
#define FTL_OP_REQ_TABLE_SIZE FC_COMMAND_POOL_DEPTH

typedef void (*ftl_op_callback_fn_t)(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/* ── 同步等待器（FTL_OP_SYNC_WAIT 的 ctx）──
 *
 * 由同步原语在自己栈上持有，经 ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter)
 * 登记进 request 表；服务函数（ftl_sync_wait_op_callback）在完成时回填并置 done。 */
typedef struct FTL_SYNC_WAITER_S
{
    WORD            cmd_id;     /* 等待的 cmd_id */
    BOOL            done;       /* 服务函数置 true */
    fc_cmd_status_s completion; /* 服务函数保存的完整 completion 信息 */
} ftl_sync_waiter_s;

/* ── 查询 / 谓词 / getter ── */

/** @brief 当前未决 request 表项数（所有在飞命令，含 host IO）。 */
WORD ftl_op_req_count(void);

/** @brief 按 op 类型统计未决 request 数（诊断：池被吃光时看持有者）。 */
void ftl_op_req_counts(WORD out[FTL_OP_COUNT]);

/** @brief 按 (ch,ce,lun) 取实例。 */
ftl_ctx_s *ftl_op_route_get(BYTE ch, BYTE ce, BYTE lun);

/** @brief 按全局 L2P region 取 owner 实例（die = global_region % DIE_MAX）。 */
ftl_ctx_s *ftl_op_route_get_region(DWORD global_region);

/** @brief 校验 g_op_route 与 g_die_table 的往返一致性。 */
BOOL ftl_op_route_verify(void);

/**
 * @brief 是否有任何实例的 cache 流式序列处于打开状态。
 *
 * @note draining 表示窗口已提交完、命令仍在飞，门控同样关着。
 * @note 提交侧 deadline 用它区分「序列门控造成的预期等待」与「真的提交不出去」。
 */
BOOL ftl_op_cache_seq_is_active(void);

/**
 * @brief 打开的 cache 序列是否仍有「真实进展」（而非只剩被托管的恢复项）。
 *
 * @note 已托管给 Read Recovery 的项命令已脱钩，不算进展——否则序列会以
 *       「还有在飞」为由无限挂起别人的提交 deadline。
 */
BOOL ftl_op_cache_seq_is_progressing(void);

/** @brief 后台 job 的提交被拒是否属于预期等待（挂起提交 deadline 用）。 */
BOOL ftl_op_bg_submit_is_deferred(void);

/** @brief 该命令的目标实例当前是否被 cache 序列门控挡住（只读）。 */
BOOL ftl_op_gate_is_blocking(const fc_cmd_s *cmd);

/** @brief 当前是否在完成服务函数（或其嵌套调用）里。 */
BOOL ftl_op_is_in_dispatch(void);

/* ── 终身计数接线（非热对象） ── */

#if SSD_STATS_PERSIST_ENABLE
/** @brief host I/O 成功完成的字节记账（排除内部 page0 屏障写）。 */
void ftl_op_lifetime_note_host_io(const ftl_ctx_s *ftl, const pend_req_s *pending, BOOL write);

/** @brief 一条 FC 命令完成的介质健康记账（ECC / UNC / 编程失败 / 擦除失败）。 */
void ftl_op_lifetime_note_completion(const ftl_ctx_s *ftl, const fc_cmd_s *cmd, const fc_cmd_status_s *completion);
#endif /* SSD_STATS_PERSIST_ENABLE */

/* ── 变更 / 批量提交 ── */

/**
 * @brief 为 cmd_id 登记 request（op + 私有状态）。
 *
 * @note 前置条件（违反即编程错误，debug 由 ASSERT 停机）：op ∈ (FTL_OP_NONE,
 *       FTL_OP_COUNT)；要求私有 ctx 的 op（Journal/L2P/GC/...）必须传非 NULL ctx。
 */
ftl_status_e ftl_op_req_add(WORD cmd_id, ftl_op_id_e op, void *ctx);

/** @brief 释放 cmd_id 的 request 表项（未找到为 no-op，幂等）。 */
void ftl_op_req_remove(WORD cmd_id);

/**
 * @brief 设置实例路由表。
 *
 * @note 本函数在 main.c 填完 g_ftl_ptrs 后调用一次，必须早于逐实例 boot：
 *       boot 期的 full metadata rebuild 就会经 l2p_read_sync 抽完成表。
 */
void ftl_op_route_set(ftl_ctx_s *const *route);

/**
 * @brief 批量版本：n 条命令共用同一 op ID / ctx，全进或全不进。
 *
 * @note 批量提交用于 cache program 批次（同一条 FC chain）：批内必须同优先级，
 *       末条允许 FC_CHAIN_MORE。调用者仍拥有全部命令（失败时须自行 release）。
 */
ftl_status_e ftl_op_submit_batch(fc_cmd_s *const cmds[], DWORD n, ftl_op_id_e op, void *ctx);

/* ── 派发 ── */

/** @brief 完成派发（ftl_service_pump 的完成泵步）。 */
void ftl_op_drain(void);

/* ── 各模块的完成服务函数（由 g_op_callbacks[] 引用）── */

/** @brief host 读写子操作的完成服务函数（定义在 ftl_host_io.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_HOST_IO]；ctx 恒 NULL（服务函数自己 pend_find）。 */
void ftl_host_io_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/** @brief cache 写序列 padding END（本仓自拟）的完成服务函数（定义在 ftl_cache.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_CACHE_PAD]；ctx 恒 NULL（序列状态在 ftl->cache_stream.seq）。 */
void ftl_cache_pad_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/** @brief Journal 刷页 job 的完成服务函数（定义在 journal.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_JOURNAL]；ctx = `l2p_jrn_s *`（= ftl->journal）。 */
void ftl_journal_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/** @brief Read Recovery job 的完成服务函数（定义在 read_recovery.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_READ_RECOVERY]；ctx = `rr_handle_s *`。 */
void ftl_read_recovery_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/** @brief 搬移 job（GC / 静态 WL / 预测性退役）的完成服务函数（定义在 migrate.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_GC] / [FTL_OP_WL]；ctx = `mig_handle_s *`。 */
void ftl_migrate_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/** @brief L2P region 加载 / 写回的完成服务函数（定义在 l2p_cache.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_L2P]；ctx = `l2p_cache_line_s *`（提交时传的行指针）。 */
void ftl_l2p_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/** @brief Checkpoint job 的完成服务函数（定义在 checkpoint.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_CHECKPOINT]；ctx = `cp_handle_s *`。 */
void ftl_checkpoint_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/** @brief BBT 持久化 job 的完成服务函数（定义在 bbt.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_BBT]；ctx = `alc_s *`（= &ftl->alloc）。 */
void ftl_bbt_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/** @brief L2P 日志块整理 job（回收 / 抗碎片压缩）的完成服务函数（定义在 metadata_job.c）。
 *  @note 挂 g_op_callbacks[FTL_OP_METADATA]；ctx = `md_dir_cache_s *`（内部取 &dir->handle）。 */
void ftl_metadata_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/* ── 入口 ── */

/** @brief 清空 request 表 + 实例路由表（开机一次）。 */
void ftl_op_init(void);

/**
 * @brief 登记完成服务并提交一条命令（FTL 唯一提交入口）。
 *
 * @note release 归属：FTL_OP_SYNC_WAIT 由调用者 release，异步 job 由服务函数 release。
 * @note 登记必须早于 fc_submit：提交后完成可能在同一轮 drain 里就被处理。
 */
ftl_status_e ftl_op_submit(fc_cmd_s *cmd, ftl_op_id_e op, void *ctx);

#endif /* FTL_OP_H */
