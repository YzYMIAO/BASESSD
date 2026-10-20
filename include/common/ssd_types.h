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
    SSD_STATUS_OK               = 0U,
    SSD_STATUS_INVALID_ARGUMENT = 1U,
    SSD_STATUS_NO_SPACE         = 2U,
    SSD_STATUS_IO_ERROR         = 3U,
    SSD_STATUS_NOT_SUPPORTED    = 4U
} ssd_status_t;

/* ── 项目固定宽度整数别名（H9）──
 * 项目代码（src/ / include/，非第三方）统一用 `BYTE/WORD/DWORD/QWORD`，
 * 全部直接 typedef 自 <stdint.h>（不做链式 typedef，避免语义/调试信息漂移）。
 * **放在此处**的原因：它只依赖 <stdint.h>/<stdbool.h>，而 common/common.h 会连带拉进
 * <stdio.h>/<stdlib.h>/<time.h>/<ctype.h>；后者不该进 FC/FTL 的 include 闭包
 * （曾因此让 src/ftl/ftl.c 的预处理依赖里出现 stdio.h）。需要别名的头请直接
 * include 本文件（裸名，H41）。 */
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint64_t QWORD;

#endif /* SSD_TYPES_H */
