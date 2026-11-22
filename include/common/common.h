#ifndef COMMON_H__
#define COMMON_H__

/*
 * common.h — 通用 C 标准库聚合头（便捷包含）
 *
 * 注意：本头会把 ctype / stdio / stdlib 等 libc 头拉进 include 闭包；
 * FTL / FC 等 freestanding 模块不要 include 它，改用 ssd_types.h 或最小具体头。
 */
#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD 等固定宽度别名（下沉，避免拉进本头的 libc） */

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#endif
