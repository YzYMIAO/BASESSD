#ifndef SSD_TYPES_H
#define SSD_TYPES_H

/*
 * SSD 项目公共基础类型。
 * 这里仅保存跨模块共享、且不依赖具体运行平台的定义。
 */

#include <stdbool.h> /* C99 内建布尔 —— 全仓唯一的布尔类型（`bool`/`true`/`false`）。
                      * 它是 freestanding 头（与 <stdint.h> 同类，零 libc 载荷），故可放本文件。
                      * **不另定义 ssd_bool_t / ftl_bool_t**：同一语义的第二套坐标是缺陷（C9）。 */
#include <stdint.h>

typedef enum
{
    SSD_STATUS_OK               = 0,
    SSD_STATUS_INVALID_ARGUMENT = -1,
    SSD_STATUS_NO_SPACE         = -2,
    SSD_STATUS_IO_ERROR         = -3,
    SSD_STATUS_NOT_SUPPORTED    = -4
} ssd_status_t;

/* ── 基础整数别名（历史遗留，原在 common/common.h）──
 * 被 FC 命令/描述符位域（`DWORD`）等使用。**下沉到此处**的原因：它只依赖
 * <stdint.h>，而 common/common.h 会连带拉进 <stdio.h>/<stdlib.h>/<time.h>/<ctype.h>。
 * 后者不该进 FC/FTL 的 include 闭包——曾因此让 src/ftl/ftl.c 的预处理依赖里
 * 出现 stdio.h（fc_types.h 的顺带传递）。需要这些别名的头请直接 include 本文件。 */
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef uint8  BYTE;
typedef uint16 WORD;
typedef uint32 DWORD;
typedef uint64 QWORD;

#endif /* SSD_TYPES_H */
