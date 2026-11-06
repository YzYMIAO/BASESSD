#ifndef CRASH_H
#define CRASH_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

#define CRASH_TEXT_CAPACITY 96U

/* 崩溃记录来源。ASSERT=生产断言失败；FAULT=CPU 异常（HardFault 等）；
 * ABORT=libc abort()（含 munit 回归上游后的测试失败路径）。 */
typedef enum CRASH_KIND_E
{
    CRASH_KIND_NONE   = 0U,
    CRASH_KIND_ASSERT = 1U,
    CRASH_KIND_FAULT  = 2U,
    CRASH_KIND_ABORT  = 3U,
} crash_kind_e;

/* CPU 故障现场（kind == CRASH_KIND_FAULT 时有效）。
 * 全为 32 位字，无填充，便于整体校验。 */
typedef struct CRASH_FAULT_RECORD_S
{
    DWORD fault_kind; /* IPSR 异常号：2=NMI 3=HardFault 4=MemManage 5=BusFault 6=UsageFault */
    DWORD pc;
    DWORD lr;
    DWORD sp;
    DWORD xpsr;
    DWORD cfsr;
    DWORD hfsr;
    DWORD mmfar;
    DWORD bfar;
} crash_fault_record_s;

/* 跨软复位保留的崩溃记录（存放于 .noinit，启动不清零）。 */
typedef struct CRASH_RECORD_S
{
    DWORD                magic;
    DWORD                version;
    DWORD                kind; /* crash_kind_e */
    DWORD                line;
    DWORD                checksum;
    char                 expression[CRASH_TEXT_CAPACITY];
    char                 file[CRASH_TEXT_CAPACITY];
    crash_fault_record_s fault;
} crash_record_s;

void                  crash_record_assert(const char *expression, const char *file, DWORD line);
void                  crash_record_abort(void);
void                  crash_record_fault(const crash_fault_record_s *fault);
const crash_record_s *crash_record_get(void);
void                  crash_record_clear(void);

/*
 * 平台崩溃上报钩子（弱符号）。默认空实现；固件平台用轮询 UART 覆盖，
 * 把记录打印成统一 dump。断言路径与故障处理器共用它。
 */
void crash_platform_report(const crash_record_s *record);

#endif /* CRASH_H */
