#ifndef COMMON_H__
#define COMMON_H__
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
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef uint8  BYTE;
typedef uint16 WORD;
typedef uint32 DWORD;
typedef uint64 QWORD;
#endif
