#ifndef CRASH_H
#define CRASH_H

#include <stdint.h>

#define CRASH_TEXT_CAPACITY 96U

/* 崩溃记录来源。ASSERT=生产断言失败；FAULT=CPU 异常（HardFault 等）；
 * ABORT=libc abort()（含 munit 回归上游后的测试失败路径）。 */
typedef enum
{
    CRASH_KIND_NONE   = 0U,
    CRASH_KIND_ASSERT = 1U,
    CRASH_KIND_FAULT  = 2U,
    CRASH_KIND_ABORT  = 3U,
} crash_kind_t;

/* CPU 故障现场（kind == CRASH_KIND_FAULT 时有效）。
 * 全为 32 位字，无填充，便于整体校验。 */
typedef struct
{
    uint32_t fault_kind; /* IPSR 异常号：2=NMI 3=HardFault 4=MemManage 5=BusFault 6=UsageFault */
    uint32_t pc;
    uint32_t lr;
    uint32_t sp;
    uint32_t xpsr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
} crash_fault_record_t;

/* 跨软复位保留的崩溃记录（存放于 .noinit，启动不清零）。 */
typedef struct
{
    uint32_t             magic;
    uint32_t             version;
    uint32_t             kind; /* crash_kind_t */
    uint32_t             line;
    uint32_t             checksum;
    char                 expression[CRASH_TEXT_CAPACITY];
    char                 file[CRASH_TEXT_CAPACITY];
    crash_fault_record_t fault;
} crash_record_t;

void                  crash_record_assert(const char *expression, const char *file, uint32_t line);
void                  crash_record_abort(void);
void                  crash_record_fault(const crash_fault_record_t *fault);
const crash_record_t *crash_record_get(void);
void                  crash_record_clear(void);

/*
 * 平台崩溃上报钩子（弱符号）。默认空实现；固件平台用轮询 UART 覆盖，
 * 把记录打印成统一 dump。断言路径与故障处理器共用它。
 */
void crash_platform_report(const crash_record_t *record);

#endif /* CRASH_H */
