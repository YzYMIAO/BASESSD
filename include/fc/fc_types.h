#ifndef FC_TYPES_H
#define FC_TYPES_H

#include "common/common.h"
#include "nand/nand_config.h"

/*
 * fc_types.h — FC 层基础类型
 *
 * FC（Flash Controller）层负责：命令池/Descriptor 链/缓冲池管理、
 * SQ/CQ 调度、ECC/CRC/RAID 数据路径。本头文件是 FC 层的最小依赖集
 * （状态码 + 资源 ID + 前向声明），被其余 fc_*.h 引用。
 */

/* FC 层状态码：非负 = 成功/资源相关，负数 = 错误 */
typedef enum
{
    FC_STATUS_OK               = 0,
    FC_STATUS_INVALID_ARGUMENT = -1,
    FC_STATUS_QUEUE_FULL       = -2,
    FC_STATUS_QUEUE_EMPTY      = -3,
    FC_STATUS_NOT_READY        = -4,
    FC_STATUS_IO_ERROR         = -5,
    FC_STATUS_NOT_SUPPORTED    = -6,
    FC_STATUS_RESOURCE_EMPTY   = -7,
} fc_status_t;

/* 固定 Pool 的 16 位资源 ID；0xFFFF 表示无效句柄。 */
typedef uint16_t fc_command_id_t;
typedef uint16_t fc_descriptor_id_t;
typedef uint16_t fc_buffer_id_t;

#define FC_COMMAND_ID_INVALID    ((fc_command_id_t)0xFFFFU)
#define FC_DESCRIPTOR_ID_INVALID ((fc_descriptor_id_t)0xFFFFU)
#define FC_BUFFER_ID_INVALID     ((fc_buffer_id_t)0xFFFFU)

/* 前向声明：完整定义在 fc_command.h */
typedef struct fc_command_t        fc_command_t;
typedef struct fc_descriptor_t     fc_descriptor_t;
typedef struct fc_command_status_t fc_command_status_t;

#endif /* FC_TYPES_H */
