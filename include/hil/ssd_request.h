#ifndef SSD_REQUEST_H
#define SSD_REQUEST_H

/*
 * ssd_request.h — HIL 主机请求表示与 run 拆分入口。
 *
 * 定义 ssd_req_s（两种 host 前端共用的请求对象）与其生命周期契约；拆分策略
 * 在 ssd_request.c：按 FTL 实例边界切连续 run，plane 配对交给 FTL。
 * completion_status 只由 HIL 写（见下方字段说明）。
 */

#include "ftl_type.h" /* ftl_io_req_s */
#include "ssd_types.h"
#include "stats.h" /* SSD_STATS_ENABLE（统计专用声明/调用按档裁剪） */

#include <stddef.h>

/* 请求级 deadline：HIL 对 host 命令的 liveness 上界。
 * 必须大于最坏正常路径（GC + NAND 超时）；超时只表示「不再等主机回执」，
 * 不代表 FTL 在飞资源已释放（见 timed_out 的生命周期契约）。 */
#define SSD_REQUEST_TIMEOUT_MS (5000U)

/* HIL 主机请求类型（与 SCSI / CLI 命令解耦的内部表示）。 */
typedef enum SSD_REQUEST_OPCODE_E
{
    SSD_REQUEST_READ = 0, /* 读：buffer 是回填目标 */
    SSD_REQUEST_WRITE,    /* 写：buffer 是数据源 */
    SSD_REQUEST_FLUSH,    /* SYNCHRONIZE CACHE / CLI fl */
    SSD_REQUEST_TRIM      /* SCSI UNMAP / CLI tr（逻辑解除映射） */
} ssd_req_opcode_e;

/*
 * HIL 主机请求。
 *
 * 生命周期：由 host_queue entry 持有（USB/CLI）。complete_flag 置 1 只表示
 * host 侧可回执；若 timed_out，entry 还必须等
 * ssd_controller_request_reclaimable() 为真（FTL/controller 不再引用）才能归还。
 * READ/WRITE 会按 FTL 实例拆成多个 run；completion_status/agg_status 是聚合结果。
 *
 * ⚠ completion_status 是 **host 可见**状态，只允许 HIL 写：
 *   - submit 初始化；
 *   - ssd_request_mark_timeout 写 TIMEOUT（tombstone）；
 *   - on_sub_complete 在全部 run 完成且未超时时写聚合值。
 * FTL 组完成写的是 `sub_status`（per-run 输入），**不得**直接改 completion_status——
 * 否则迟到完成会把 TIMEOUT tombstone 覆盖成 OK。
 */
typedef struct SSD_REQUEST_S
{
    ssd_req_opcode_e opcode;            /* 请求类型 */
    QWORD            start_lba;         /* 起始 host 全局 LBA */
    DWORD            block_count;       /* 连续 LBA 数（IU 数，不是页数） */
    void            *buffer;            /* READ 回填 / WRITE 数据源 */
    size_t           buffer_size;       /* buffer 容量（controller 入口校验） */
    ssd_status_e     completion_status; /* host 可见完成状态（只由 HIL 写） */
    ssd_status_e     agg_status;        /* 多 run 聚合：首个非 OK 胜出（on_sub_complete） */
    ssd_status_e     sub_status;        /* FTL 组完成写入的 per-run 状态（不直接对 host 可见） */
    BYTE             complete_flag;     /* 1 = 本请求已完成（USB/CLI 轮询） */
    DWORD            sub_remaining;     /* 未完成的 run 数（controller 拆分用） */
    DWORD            start_ms;          /* 提交时刻（controller 写；前端算 deadline） */
    BOOL             timed_out;         /* HIL 已放弃 host 回执；迟到完成不得改状态 */
#if SSD_STATS_ENABLE
    DWORD start_us;    /* 提交时刻（µs，命令延迟直方图） */
    BOOL  start_timed; /* true = start_us 有效 */
#endif
    ftl_io_req_s io; /* 子请求复用模板（lba/buffer/count 循环内改写） */
} ssd_req_s;

#if SSD_STATS_ENABLE
/**
 * @brief HIL 请求统计注册（controller init 调用一次；幂等，重入会重置计数）
 */
void ssd_request_stats_register(void);

/**
 * @brief 已受理的主机命令计数
 *
 * @param opcode 请求类型（READ / WRITE / FLUSH / TRIM）
 */
void ssd_request_note_cmd(ssd_req_opcode_e opcode);

/** @brief host 超时回执计数（ssd_request_mark_timeout 调用） */
void ssd_request_note_timeout(void);

/** @brief 异步完成回执为失败（非 OK、非 TIMEOUT）的计数 */
void ssd_request_note_bad_status(void);

/**
 * @brief 记录一次请求的完成延迟（只记一次）
 *
 * @param request 请求（start_timed 为 false 时直接返回）
 */
void ssd_request_note_latency(ssd_req_s *request);
#endif

/**
 * @brief 请求是否超过 deadline
 *
 * @param start_ms 提交时刻（0 = 未提交）
 * @param now_ms   当前毫秒时基（0 = 时钟缺失，不判超时）
 * @return true = 已超时
 */
static inline BOOL ssd_request_expired(DWORD start_ms, DWORD now_ms)
{
    return (start_ms != 0U) && (now_ms != 0U) && ((now_ms - start_ms) >= SSD_REQUEST_TIMEOUT_MS);
}

/**
 * @brief 标记 host 侧超时（只改可见状态；FTL 在飞资源由 reclaimable 判定）
 *
 * 幂等：已标记过的请求直接返回——USB 前端在 `timed_out && !ack_sent` 时
 * 会再次调用本函数补发回执，重复标记不得重复计数或改状态。
 *
 * @param request 请求（非空）
 */
void ssd_request_mark_timeout(ssd_req_s *request);

/* 按 FTL 实例边界把 READ/WRITE 请求拆成连续 run 并提交（ssd_request.c）。
 * controller 的 tag 在此前向声明：controller.h 依赖本头的 ssd_req_s，
 * 本头不能再反向 include controller.h。C11 允许同型 typedef 重定义。 */
typedef struct SSD_CONTROLLER_S ssd_ctl_s;

/**
 * @brief 按 FTL 实例边界把 host 请求拆成连续 run 并逐段提交。
 *
 * @param controller Controller 上下文
 * @param request    主机请求（READ/WRITE）
 * @return SSD_STATUS_OK               全部/部分 run 已受理
 *         SSD_STATUS_INVALID_ARGUMENT 入参或 block_count 非法
 *         SSD_STATUS_NOT_SUPPORTED    opcode 不是 READ/WRITE
 *         SSD_STATUS_NO_SPACE / IO_ERROR 第一个 run 就提交失败（无在飞）
 */
ssd_status_e ssd_request_split(ssd_ctl_s *controller, ssd_req_s *request);

#endif /* SSD_REQUEST_H */
