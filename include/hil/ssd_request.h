#ifndef SSD_REQUEST_H
#define SSD_REQUEST_H

#include "ftl_type.h" /* ftl_io_req_t */
#include "ssd_types.h"

#include <stddef.h>
#include <stdint.h>

/* HIL 主机请求类型（与 SCSI / CLI 命令解耦的内部表示）。 */
typedef enum
{
    SSD_REQUEST_READ = 0, /* 读：buffer 是回填目标 */
    SSD_REQUEST_WRITE,    /* 写：buffer 是数据源 */
    SSD_REQUEST_FLUSH,    /* SYNCHRONIZE CACHE / CLI fl */
    SSD_REQUEST_TRIM      /* SCSI UNMAP / CLI tr（逻辑解除映射） */
} ssd_req_opcode_e;

/*
 * HIL 主机请求。
 *
 * 生命周期：由 host_queue entry 持有（USB/CLI），complete_flag 置 1 前不得复用。
 * READ/WRITE 会按 FTL 实例拆成多个 run；completion_status/agg_status 是聚合结果。
 */
typedef struct
{
    ssd_req_opcode_e opcode;            /* 请求类型 */
    QWORD            start_lba;         /* 起始 host 全局 LBA */
    DWORD            block_count;       /* 连续 LBA 数（IU 数，不是页数） */
    void            *buffer;            /* READ 回填 / WRITE 数据源 */
    size_t           buffer_size;       /* buffer 容量（controller 入口校验） */
    ssd_status_e     completion_status; /* 完成状态（全部 run 完成后写回聚合值） */
    ssd_status_e     agg_status;        /* 多 run 聚合：首个非 OK 胜出（on_sub_complete） */
    volatile BYTE    complete_flag;     /* 1 = 本请求已完成（USB/CLI 轮询） */
    volatile DWORD   sub_remaining;     /* 未完成的 run 数（controller 拆分用） */
    ftl_io_req_t     io;                /* 子请求复用模板（lba/buffer/count 循环内改写） */
} ssd_request_t;

#endif /* SSD_REQUEST_H */
