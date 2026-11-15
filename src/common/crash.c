#include "crash.h"

#include <string.h>

#define CRASH_MAGIC   0x43524153U
#define CRASH_VERSION 2U /* v2：增加 kind + fault 负载；存储段改 .noinit */

/*
 * 崩溃记录放在 .noinit：NOLOAD、启动不清零，可跨软复位保留。
 * （原 .bkpsram_data 因 VBPSRAM 依赖 VBAT、本板 VBAT 未供电而无法保持，
 * 见 docs/archive/debug_sbrk_heap_stack_20261004.md。）
 * volatile：记录在本轮只写不读，靠它阻止编译器把写优化掉。
 */
static volatile crash_record_s g_crash_record __attribute__((section(".noinit")));

static void crash_copy(char *destination, const char *source)
{
    if (source == NULL)
    {
        destination[0] = '\0';
        return;
    }
    strncpy(destination, source, CRASH_TEXT_CAPACITY - 1U);
    destination[CRASH_TEXT_CAPACITY - 1U] = '\0';
}

/* 校验和覆盖 version/kind/line + 文本 + fault 负载（不含 magic/checksum 自身）。
 * 逐字段显式折叠：记录是 volatile，避免对整块结构体取址造成的限定符丢失。 */
static DWORD crash_checksum(void)
{
    DWORD checksum = CRASH_VERSION ^ g_crash_record.kind ^ g_crash_record.line;
    DWORD index;

    for (index = 0U; index < CRASH_TEXT_CAPACITY; index++)
    {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= g_crash_record.expression[index];
        checksum ^= g_crash_record.file[index];
    }

    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.fault_kind;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.pc;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.lr;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.sp;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.xpsr;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.cfsr;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.hfsr;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.mmfar;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= g_crash_record.fault.bfar;

    return checksum;
}

static void crash_commit(void)
{
    g_crash_record.checksum = crash_checksum();
    __asm volatile("" ::
                       : "memory");
    g_crash_record.magic = CRASH_MAGIC;
}

/* 清空 fault 负载：非 FAULT 记录不应把上一次的异常现场带进校验和/dump。 */
static void crash_clear_fault(void)
{
    g_crash_record.fault.fault_kind = 0U;
    g_crash_record.fault.pc         = 0U;
    g_crash_record.fault.lr         = 0U;
    g_crash_record.fault.sp         = 0U;
    g_crash_record.fault.xpsr       = 0U;
    g_crash_record.fault.cfsr       = 0U;
    g_crash_record.fault.hfsr       = 0U;
    g_crash_record.fault.mmfar      = 0U;
    g_crash_record.fault.bfar       = 0U;
}

void crash_record_assert(const char *expression, const char *file, DWORD line)
{
    g_crash_record.magic   = 0U;
    g_crash_record.version = CRASH_VERSION;
    g_crash_record.kind    = CRASH_KIND_ASSERT;
    g_crash_record.line    = line;

    crash_clear_fault();

    crash_copy((char *)g_crash_record.expression, expression);
    crash_copy((char *)g_crash_record.file, file);
    crash_commit();
}

/* abort() 无参，记不下表达式；用独立 kind 让 dump 能区分「断言」与「abort」。 */
void crash_record_abort(void)
{
    g_crash_record.magic   = 0U;
    g_crash_record.version = CRASH_VERSION;
    g_crash_record.kind    = CRASH_KIND_ABORT;
    g_crash_record.line    = 0U;

    crash_clear_fault();

    crash_copy((char *)g_crash_record.expression, "abort");
    crash_copy((char *)g_crash_record.file, "");
    crash_commit();
}

void crash_record_fault(const crash_fault_record_s *fault)
{
    if (fault == NULL)
    {
        return;
    }

    g_crash_record.magic   = 0U;
    g_crash_record.version = CRASH_VERSION;
    g_crash_record.kind    = CRASH_KIND_FAULT;
    g_crash_record.line    = 0U;

    g_crash_record.fault = *fault;

    crash_copy((char *)g_crash_record.expression, "cpu-fault");
    crash_copy((char *)g_crash_record.file, "");
    crash_commit();
}

const crash_record_s *crash_record_get(void)
{
    if ((g_crash_record.magic != CRASH_MAGIC) || (g_crash_record.version != CRASH_VERSION))
    {
        return NULL;
    }
    if (crash_checksum() != g_crash_record.checksum)
    {
        return NULL;
    }
    return (const crash_record_s *)&g_crash_record;
}

void crash_record_clear(void)
{
    g_crash_record.magic = 0U;
}
