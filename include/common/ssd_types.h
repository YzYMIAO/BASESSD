#ifndef SSD_TYPES_H
#define SSD_TYPES_H

/*
 * SSD 项目公共基础类型。
 * 这里仅保存跨模块共享、且不依赖具体运行平台的定义。
 */

#include <stdint.h>

typedef uint64_t ssd_lba_t;
typedef uint64_t ssd_ppa_t;

typedef enum {
    SSD_STATUS_OK = 0,
    SSD_STATUS_INVALID_ARGUMENT = -1,
    SSD_STATUS_NO_SPACE = -2,
    SSD_STATUS_IO_ERROR = -3,
    SSD_STATUS_NOT_SUPPORTED = -4
} ssd_status_t;

#endif /* SSD_TYPES_H */
