#ifndef COMMON_H__
#define COMMON_H__
#include "common/ssd_types.h" /* uint8/…/DWORD 等基础别名（下沉，避免拉进本头的 libc） */

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum
{
    FUNC_STATUS_SUCCESS = 0,
    FUNC_STATUS_ERROR   = !FUNC_STATUS_SUCCESS
} FuncStatus;
#endif
