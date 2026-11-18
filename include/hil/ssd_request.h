#ifndef SSD_REQUEST_H
#define SSD_REQUEST_H

#include "ftl_type.h" /* ftl_io_req_s */
#include "ssd_types.h"

#include <stddef.h>
#include <stdint.h>

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
 */
typedef struct SSD_REQUEST_S
{
    ssd_req_opcode_e opcode;            /* 请求类型 */
    QWORD            start_lba;         /* 起始 host 全局 LBA */
    DWORD            block_count;       /* 连续 LBA 数（IU 数，不是页数） */
    void            *buffer;            /* READ 回填 / WRITE 数据源 */
    size_t           buffer_size;       /* buffer 容量（controller 入口校验） */
    ssd_status_e     completion_status; /* 完成状态（全部 run 完成后写回聚合值） */
    ssd_status_e     agg_status;        /* 多 run 聚合：首个非 OK 胜出（on_sub_complete） */
    BYTE             complete_flag;     /* 1 = 本请求已完成（USB/CLI 轮询） */
    DWORD            sub_remaining;     /* 未完成的 run 数（controller 拆分用） */
    DWORD            start_ms;          /* 提交时刻（controller 写；前端算 deadline） */
    BOOL             timed_out;         /* HIL 已放弃 host 回执；迟到完成不得改状态 */
    ftl_io_req_s     io;                /* 子请求复用模板（lba/buffer/count 循环内改写） */
} ssd_req_s;

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
 * @param request 请求（非空）
 */
static inline void ssd_request_mark_timeout(ssd_req_s *request)
{
    request->timed_out         = true;
    request->completion_status = SSD_STATUS_TIMEOUT;
    request->complete_flag     = 1U;
}

#endif /* SSD_REQUEST_H */
