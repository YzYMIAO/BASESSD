#ifndef FTL_INTERFACE_H
#define FTL_INTERFACE_H

/*
 * ftl_interface.h — FTL 公共 host / 启动契约（API v1）
 *
 * 本头暴露 HIL / boot 使用的主机路径与设备/实例初始化 API；
 * FTL 模块内部头在 src/ftl/，调用者不需要知道任何 workspace 类型。
 *
 * 调用上下文：
 *   - ftl_service_pump(): 主循环每轮一次，实例无关；
 *   - ftl_service():      主循环每轮对每个实例调用；
 *   - submit/flush/trim:  HIL 控制路径，异步推进，不得在回调中自旋。
 */

#include "ftl_config.h" /* FTL_DIE_MAX / FTL_IU_BYTES / 容量几何 */
#include "ftl_type.h"   /* ftl_ctx_s / ftl_io_req_s / ftl_status_e */
#include "ssd_types.h"
#include "stats.h" /* SSD_STATS_ENABLE（统计专用声明按档裁剪） */

#define FTL_API_VERSION (1U)

/* ── 启动 / 恢复 ── */

typedef enum FTL_BOOT_TYPE_E
{
    FTL_BOOT_FRESH_START = 0,
    FTL_BOOT_CLEAN_START,
    FTL_BOOT_POWER_FAILURE,
} ftl_boot_type_e;

typedef struct FTL_BOOT_REPORT_S
{
    ftl_boot_type_e boot_type;       /* Fresh / clean / power-failure */
    DWORD           cp_sequence;     /* Checkpoint sequence（0 = 无） */
    DWORD           journal_entries; /* 回放的 Journal entry 数 */
    BOOL            blocks_restored; /* true = 块元数据快照已加载 */
    BOOL            scoped;          /* true = 范围化恢复（部分旧映射丢失） */
} ftl_boot_report_s;

/**
 * @brief 判定介质是否需要格式化。
 * @param ftl FTL 上下文
 * @param need_format 输出：true = 需要格式化
 * @return FTL_OK 判定完成；FTL_ERROR CP 介质读不通（调用者不得擦片）
 */
ftl_status_e ftl_boot_check_format(ftl_ctx_s *ftl, BOOL *need_format);

/**
 * @brief 全量格式化当前实例的介质。
 * @param ftl FTL 上下文
 * @param verify true = 逐块擦除体检
 * @return FTL_OK 成功；FTL_ERROR 失败
 */
ftl_status_e ftl_boot_format(ftl_ctx_s *ftl, BOOL verify);

/**
 * @brief 执行启动恢复并输出启动类型/Checkpoint/Journal 统计。
 * @param ftl FTL 上下文
 * @param report 启动报告；可为 NULL
 * @return FTL_OK 成功；FTL_ERROR 恢复失败
 */
ftl_status_e ftl_boot_recover(ftl_ctx_s *ftl, ftl_boot_report_s *report);

/**
 * @brief 打印固定分区与数据区的坏块分布（开机基线）。
 * @param ftl FTL 上下文（只读）
 */
void ftl_boot_log_bad_areas(const ftl_ctx_s *ftl);

/* ── 设备 / 实例初始化（boot 编排）── */

/**
 * @brief 由上电扫描结果构建紧凑 die 表（纯 RAM，不碰硬件）
 *
 * 只给实际存在且探测有响应的 LUN 分配紧凑 die 号。
 *
 * @param targets 板级物理拓扑（CH_NUM × CE_NUM；armed 之前已填）
 * @return true = 至少一个 die 且往返自检通过；false = 无 die / 越界 / 自检失败
 */
BOOL ftl_topology_build(const nand_target_info_s targets[CH_NUM][CE_NUM]);

/**
 * @brief 复位 FTL 文件级全局静态（所有实例 init 之前调用一次）
 *
 * 这些静态落在 NOLOAD 段，上电/软复位都不清零；必须在 ftl_device_prepare 之前
 * 调用，否则 boot once 守卫可能被随机残留误判为「已初始化」。
 */
void ftl_global_init(void);

/**
 * @brief 设备级一次初始化：全局完成表 + 实例路由 + 每 (ch,ce) 共享 L2P cache
 *
 * 调用顺序：ftl_topology_build → ftl_global_init → 本函数 → 逐实例 init。
 * 重复调用会被拒绝（debug ASSERT；release 记日志后返回，不重复清 cache）。
 */
void ftl_device_prepare(void);

/**
 * @brief 取紧凑 die 号对应的 FTL 实例（不透明句柄）
 *
 * @param die 紧凑 die 号（< DIE_MAX；未分配的尾项不得访问）
 * @return 实例指针（非空）
 */
ftl_ctx_s *ftl_instance_get(BYTE die);

/**
 * @brief 初始化一个 FTL 实例的 workspace 与模块状态
 *
 * 必须在 ftl_device_prepare 之后、逐实例推进；实例的 workspace 由 FTL 自持。
 * 每个 die 只能成功初始化一次；重复调用返回 FTL_ERROR（release 也拒绝）。
 *
 * @param die 紧凑 die 号（< DIE_MAX = 运行时实际 die 数）
 * @return FTL_OK 成功；FTL_ERROR 重复初始化 / 本 target 无 L2P cache /
 *         元数据候选池构建失败
 */
ftl_status_e ftl_instance_init(BYTE die);

/**
 * @brief 校验实例路由与紧凑 die 表的往返一致性
 *
 * @return true = 一致；false = 路由/拓扑公式漂移
 */
BOOL ftl_route_verify(void);

/**
 * @brief 撤销实例路由（boot 失败回退 fc_service 时用）
 */
void ftl_route_clear(void);

/**
 * @brief 打印本实例的池占用（boot 基线）
 *
 * @param ftl 已初始化的实例（只读）
 */
void ftl_boot_log_usage(const ftl_ctx_s *ftl);

/* ── 周期任务 / 容量（HIL 只经这些入口，不读 FTL 内部字段）── */

/**
 * @brief 计算主机可见的 LBA 上限（开机一次）
 *
 * 按实际坏块集，取各实例/plane 的最小可用块数；runtime 新增坏块不缩容。
 *
 * @return 主机 LBA 上限（>= 1）
 */
DWORD ftl_host_lba_max(void);

/**
 * @brief 注册 FTL 的周期任务（统计日志 / 后台节拍 / GC-WL 求值）
 *
 * 必须在全部实例 boot 成功之后调用一次；失败回退 fc_service 时不调用。
 * 周期与内部策略由 FTL 决定，调用者不需要知道。
 * 重复调用会被拒绝（debug ASSERT；release 记日志后返回，不重复注册）。
 */
void ftl_timer_tasks_start(void);

/* ── 超时中止（best-effort；仅用于 HIL 已标记 timed_out 的请求）── */

/**
 * @brief 中止属于同一 host 请求的所有 FTL 组
 *
 * 停止下发剩余子操作；在飞 NAND 命令按自身超时完成，组完成时仍通知请求
 * （HIL 的 timed_out 回调只递减在飞计数）。不会强制丢弃仍在 FC/NAND 里的命令。
 *
 * @param ctx HIL 请求指针（非空；即各 run 的 on_complete_ctx）
 */
void ftl_abort_request(void *ctx);

/**
 * @brief 中止本实例的 TRIM job
 *
 * 只清 job 状态，已提交的 Journal/L2P 命令自然完成；不触碰 HIL 请求。
 *
 * @param ftl 实例
 */
void ftl_trim_abort(ftl_ctx_s *ftl);

/* ── 主循环服务 ── */

/** @brief 取实例拓扑（只读）。 */
const ftl_topo_s *ftl_get_topo(const ftl_ctx_s *ftl);

/**
 * @brief 实例无关的每轮泵。
 * @note 必须先于所有 ftl_service()，且每轮只调一次。
 */
void ftl_service_pump(void);

/**
 * @brief 单个 FTL 实例的服务；多实例下每个实例都要调用。
 * @param ftl FTL 上下文（非 NULL）
 */
void ftl_service(ftl_ctx_s *ftl);

#if SSD_STATS_ENABLE
/**
 * @brief 完成派发侧的进展签名（主循环空转判据）。
 *
 * = 已消费完成数 + 成功下发的 host 子操作数。只用于比较两次采样是否变化；
 * u32 回绕不影响相等比较。
 *
 * @return 进展签名
 */
DWORD ftl_op_stats_progress(void);
#endif

/* ── Host I/O ── */

/**
 * @brief 非阻塞提交读请求。
 * @param ftl FTL 上下文
 * @param req IO 请求描述（lba/buffer/count/完成通知）
 * @return FTL_OK 已受理；FTL_ERROR 参数非法 / LBA 越界 / 组槽满
 */
ftl_status_e ftl_submit_read(ftl_ctx_s *ftl, const ftl_io_req_s *req);

/**
 * @brief 非阻塞提交写请求。
 * @param ftl FTL 上下文
 * @param req IO 请求描述（同 ftl_submit_read）
 * @return FTL_OK 已受理；FTL_ERROR 参数非法 / LBA 越界 / 组槽满
 */
ftl_status_e ftl_submit_write(ftl_ctx_s *ftl, const ftl_io_req_s *req);

/**
 * @brief 取走并清除 volatile write cache 的 deferred error 标志。
 * @param ftl FTL 上下文
 * @return true = 上一次已回执的写后来失败，下一条 host 命令应上报错误
 */
BOOL ftl_take_deferred_error(ftl_ctx_s *ftl);

/* ── FLUSH / TRIM ── */

/**
 * @brief 开始异步 FLUSH job（幂等）。
 * @param ftl FTL 上下文
 */
void ftl_flush_begin(ftl_ctx_s *ftl);

/**
 * @brief 推进 FLUSH job。
 * @param ftl FTL 上下文
 * @return true 完成；false 下一轮继续
 */
BOOL ftl_flush_step(ftl_ctx_s *ftl);

/**
 * @brief 放弃当前 FLUSH job（host 复位等撤销路径）。
 * @param ftl FTL 上下文
 */
void ftl_flush_cancel(ftl_ctx_s *ftl);

/**
 * @brief 登记 [lba, lba+count) 的异步 TRIM。
 * @param ftl FTL 上下文
 * @param lba 起始逻辑地址
 * @param count 连续 LBA 数（>0）
 * @return FTL_OK 已登记；FTL_AGAIN 已有 TRIM 在飞；FTL_ERROR 参数非法
 */
ftl_status_e ftl_trim_begin(ftl_ctx_s *ftl, DWORD lba, DWORD count);

/**
 * @brief 推进 TRIM job。
 * @param ftl FTL 上下文
 * @return FTL_OK 完成；FTL_AGAIN 仍有剩余；FTL_ERROR 中止
 */
ftl_status_e ftl_trim_step(ftl_ctx_s *ftl);

#endif /* FTL_INTERFACE_H */
