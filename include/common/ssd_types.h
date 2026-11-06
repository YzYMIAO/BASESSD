#ifndef SSD_TYPES_H
#define SSD_TYPES_H

/*
 * SSD 项目公共基础类型。
 * 这里仅保存跨模块共享、且不依赖具体运行平台的定义。
 */

#include <stdbool.h> /* C99 内建布尔。项目代码统一用下方 BOOL 别名；
                      * `bool/true/false` 只保留在本 typedef 与第三方/厂商边界。
                      * 它是 freestanding 头（与 <stdint.h> 同类，零 libc 载荷），故可放本文件。
                      * **不另定义 ssd_bool_t / ftl_bool_t**：同一语义的第二套坐标是缺陷（C9）。 */
#include <stdint.h>

typedef enum SSD_STATUS_E
{
    SSD_STATUS_OK               = 0U,
    SSD_STATUS_INVALID_ARGUMENT = 1U,
    SSD_STATUS_NO_SPACE         = 2U,
    SSD_STATUS_IO_ERROR         = 3U,
    SSD_STATUS_NOT_SUPPORTED    = 4U,
    SSD_STATUS_BUSY             = 5U /* 同类请求已有一条在飞（如第二条 FLUSH） */
} ssd_status_e;

/* 平台 / 共享算法的二元返回状态：0 = 成功，1 = 失败。 */
typedef enum FUNC_STATUS_E
{
    FUNC_STATUS_SUCCESS = 0,
    FUNC_STATUS_ERROR   = 1,
} func_status_e;

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

/* 项目布尔别名：C99 `_Bool`（1 字节），不是 Windows 的 4 字节 int。
 * 项目代码统一用 BOOL；`bool` 只在本 typedef 与第三方/厂商边界出现。 */
typedef bool BOOL;

/* ── 位掩码判断（H15）──
 * `bit_mask_any` = 任一位置位；`bit_mask_all` = 指定位全部置位。
 * 两个语义只差一个运算符，手写时靠人眼分辨；这里用名字把语义写出来。
 * 只用于位掩码/位标志；C 位域不在此列。 */
static inline BOOL bit_mask_any(DWORD value, DWORD mask)
{
    return (value & mask) != 0U;
}

static inline BOOL bit_mask_all(DWORD value, DWORD mask)
{
    return (value & mask) == mask;
}

#endif /* SSD_TYPES_H */
