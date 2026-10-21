#ifndef FC_TYPES_H
#define FC_TYPES_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * fc_types.h — FC 层基础类型
 *
 * FC（Flash Controller）层负责：命令池/Descriptor 链/缓冲池管理、
 * SQ/CQ 调度、ECC/CRC/RAID 数据路径。本头文件是 FC 层的最小依赖集
 * （状态码 + 资源 ID + 前向声明），被其余 fc_*.h 引用。
 */

/* FC 层状态码：0 = 成功，其余为具名错误/资源码（全部非负） */
typedef enum
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
typedef WORD fc_command_id_t;
typedef WORD fc_descriptor_id_t;
typedef WORD fc_buffer_id_t;

#define FC_COMMAND_ID_INVALID    ((fc_command_id_t)0xFFFFU)
#define FC_DESCRIPTOR_ID_INVALID ((fc_descriptor_id_t)0xFFFFU)
#define FC_BUFFER_ID_INVALID     ((fc_buffer_id_t)0xFFFFU)

/* 资源使用统计（测试/监控用） */
typedef struct
{
    WORD sq_used;
    WORD hsq_used;
    WORD cq_used;
    WORD command_used;
    WORD descriptor_used;
    WORD buffer_used;
} fc_resource_stats_t;

/* 前向声明：完整定义在 fc_command.h */
typedef struct fc_command_t        fc_command_t;
typedef struct fc_descriptor_t     fc_descriptor_t;
typedef struct fc_command_status_t fc_command_status_t;

#endif /* FC_TYPES_H */
