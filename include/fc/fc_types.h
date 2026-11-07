#ifndef FC_TYPES_H
#define FC_TYPES_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * fc_types.h — FC 层基础类型
 *
 * FC（Flash Controller）层负责：命令池/Descriptor 链/缓冲池管理、
 * SQ/CQ 调度、ECC/CRC/RAID 数据路径。本头文件是 FC 层的最小依赖集
 * （状态码 + 资源 ID + 资源统计），被其余 fc_*.h 引用。
 */

/* FC 层状态码：0 = 成功，其余为具名错误/资源码（全部非负） */
typedef enum FC_STATUS_E
{
    FC_STATUS_OK               = 0U,
    FC_STATUS_INVALID_ARGUMENT = 1U,
    FC_STATUS_QUEUE_FULL       = 2U,
    FC_STATUS_QUEUE_EMPTY      = 3U,
    FC_STATUS_NOT_READY        = 4U,
    FC_STATUS_IO_ERROR         = 5U,
    FC_STATUS_NOT_SUPPORTED    = 6U,
    FC_STATUS_RESOURCE_EMPTY   = 7U,
} fc_status_e;

/* 固定 Pool 的 16 位资源 ID；0xFFFF 表示无效句柄。 */
typedef WORD fc_cmd_id_t;
typedef WORD fc_desc_id_t;
typedef WORD fc_buf_id_t;

#define FC_CMD_ID_INVALID  ((fc_cmd_id_t)0xFFFFU)
#define FC_DESC_ID_INVALID ((fc_desc_id_t)0xFFFFU)
#define FC_BUF_ID_INVALID  ((fc_buf_id_t)0xFFFFU)

/* 资源使用统计（测试/监控用） */
typedef struct FC_RESOURCE_STATISTICS_S
{
    WORD sq_used;         /* SQ 当前占用条数 */
    WORD hsq_used;        /* HSQ 当前占用条数 */
    WORD cq_used;         /* CQ 当前占用条数 */
    WORD command_used;    /* 命令池已分配槽位数 */
    WORD descriptor_used; /* 描述符池已分配条目数 */
    WORD buffer_used;     /* buffer 池已占用块数 */
    WORD command_high;    /* 命令池历史最高占用（高水位，诊断/调参用） */
    WORD descriptor_high; /* 描述符池历史最高占用 */
    WORD buffer_high;     /* buffer 池历史最高占用 */
} fc_resource_stats_s;

#endif /* FC_TYPES_H */
