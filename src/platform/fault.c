/*
 * fault.c — Cortex-M 故障捕获（跨 target 共享逻辑）
 *
 * 本文件不 include 任何 SoC 头、不出现任何 HAL 类型：现场解码、寄存器位表、栈扫描、
 * 崩溃记录打印与停机全在这里；异常入口蹦床（naked asm）、控制台外设、故障/复位
 * 状态寄存器一律下沉到 src/platform/<soc>/fault_port.c。
 *
 * 目标：任何 CPU 故障（NMI/HardFault/MemManage/BusFault/UsageFault）都能在
 * UART 上打出「可读原因 + 出错地址 + 寄存器现场」，并写一份跨软复位保留的
 * 崩溃记录（common/crash 的 .noinit 记录）。
 *
 * 两个关键点：
 *   ① 输出走轮询 UART，不依赖 DMA/中断/libc：故障发生时中断通路可能已不可用，
 *      异步日志会静默丢弃，"断函数却什么都没打印" 正是这么来的。
 *   ② 打印完停机保留现场（SWD 可继续查看），不主动复位。
 */

#include "fault.h"

#include "crash.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>

#define FAULT_STACK_SCAN_WORDS 32U

/*
 * ⚠ 已知残留耦合：以下三个常量与 _estack 是 stm32h743 的内存映射（Flash 起止、
 * DTCM 起点、链接脚本栈顶符号）。栈扫描目前留在共享层，换 target 时需下沉为
 * port 接口（如 fault_port_stack_top()/fault_port_is_code_address()）。
 */
#define FAULT_FLASH_BEGIN 0x08000000U
#define FAULT_FLASH_END   0x08200000U
#define FAULT_DTCM_BEGIN  0x20000000U

/* 栈顶符号由链接脚本提供（_estack = DTCM 末尾），用于给栈扫描设界。 */
extern uint32_t _estack;

/* ────────────────────────────── 轮询控制台 ────────────────────────────── */

static void fault_puts(const char *text)
{
    while (*text != '\0')
    {
        fault_port_putc(*text++);
    }
}

/* 定长上限的打印：崩溃记录里的文本来自 .noinit，理论上可能没有终止符。 */
static void fault_puts_n(const char *text, uint32_t capacity)
{
    uint32_t index;

    for (index = 0U; index < capacity; index++)
    {
        if (text[index] == '\0')
        {
            break;
        }
        fault_port_putc(text[index]);
    }
}

static void fault_hex32(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    int               shift;

    fault_puts("0x");
    for (shift = 28; shift >= 0; shift -= 4)
    {
        fault_port_putc(digits[(value >> (uint32_t)shift) & 0xFU]);
    }
}

static void fault_dec(uint32_t value)
{
    char     buffer[10];
    uint32_t index = 0U;

    if (value == 0U)
    {
        fault_port_putc('0');
        return;
    }
    while ((value != 0U) && (index < sizeof(buffer)))
    {
        buffer[index++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (index > 0U)
    {
        fault_port_putc(buffer[--index]);
    }
}

/* ────────────────────────────── 寄存器解码 ────────────────────────────── */

static const char *fault_kind_name(uint32_t kind)
{
    switch (kind)
    {
        case FAULT_KIND_NMI:
            return "NMI";
        case FAULT_KIND_HARDFAULT:
            return "HardFault";
        case FAULT_KIND_MEMMANAGE:
            return "MemManage";
        case FAULT_KIND_BUSFAULT:
            return "BusFault";
        case FAULT_KIND_USAGEFAULT:
            return "UsageFault";
        default:
            return "Unknown";
    }
}

static void fault_bit(uint32_t value, uint32_t mask, const char *name)
{
    if ((value & mask) != 0U)
    {
        fault_port_putc(' ');
        fault_puts(name);
    }
}

/* HFSR：FORCED 表示由可配置故障升级而来，需再看 CFSR 三段定位真因。 */
static void fault_decode_hfsr(uint32_t hfsr)
{
    fault_puts("HFSR   :");
    if (hfsr == 0U)
    {
        fault_puts(" none");
    }
    fault_bit(hfsr, 0x40000000U, "FORCED(escalated)");
    fault_bit(hfsr, 0x00000002U, "VECTTBL");
    fault_bit(hfsr, 0x00000001U, "DEBUGEVT");
    fault_puts("\r\n");
}

static void fault_decode_cfsr(uint32_t cfsr, uint32_t mmfar, uint32_t bfar)
{
    uint32_t mmfsr = cfsr & 0xFFU;
    uint32_t bfsr  = (cfsr >> 8U) & 0xFFU;
    uint32_t ufsr  = (cfsr >> 16U) & 0xFFFFU;

    fault_puts("MMFSR  :");
    if (mmfsr == 0U)
    {
        fault_puts(" none");
    }
    fault_bit(mmfsr, 0x01U, "IACCVIOL");
    fault_bit(mmfsr, 0x02U, "DACCVIOL");
    fault_bit(mmfsr, 0x08U, "MUNSTKERR");
    fault_bit(mmfsr, 0x10U, "MSTKERR");
    fault_bit(mmfsr, 0x80U, "MMARVALID");
    fault_puts("\r\n");
    if ((mmfsr & 0x80U) != 0U)
    {
        fault_puts("MMFAR  : ");
        fault_hex32(mmfar);
        fault_puts("\r\n");
    }

    fault_puts("BFSR   :");
    if (bfsr == 0U)
    {
        fault_puts(" none");
    }
    fault_bit(bfsr, 0x01U, "IBUSERR");
    fault_bit(bfsr, 0x02U, "PRECISERR");
    fault_bit(bfsr, 0x04U, "IMPRECISERR");
    fault_bit(bfsr, 0x08U, "UNSTKERR");
    fault_bit(bfsr, 0x10U, "STKERR");
    fault_bit(bfsr, 0x80U, "BFARVALID");
    fault_puts("\r\n");
    if ((bfsr & 0x80U) != 0U)
    {
        fault_puts("BFAR   : ");
        fault_hex32(bfar);
        fault_puts("\r\n");
    }

    fault_puts("UFSR   :");
    if (ufsr == 0U)
    {
        fault_puts(" none");
    }
    fault_bit(ufsr, 0x0001U, "UNDEFINSTR");
    fault_bit(ufsr, 0x0002U, "INVSTATE");
    fault_bit(ufsr, 0x0004U, "INVPC");
    fault_bit(ufsr, 0x0008U, "NOCP");
    fault_bit(ufsr, 0x0100U, "UNALIGNED");
    fault_bit(ufsr, 0x0200U, "DIVBYZERO");
    fault_puts("\r\n");
}

/* 有界栈扫描：栈上落在 Flash 区间的字视为候选返回地址（穷人的 backtrace）。 */
static void fault_dump_stack(uint32_t sp)
{
    uint32_t limit = (uint32_t)(uintptr_t)&_estack;
    uint32_t index;

    if ((sp < FAULT_DTCM_BEGIN) || (sp >= limit))
    {
        /* 栈指针不在 DTCM 内（本工程 MSP 在 DTCM），不冒险读，避免二次故障。 */
        fault_puts("stack  : scan skipped (SP outside DTCM)\r\n");
        return;
    }

    fault_puts("stack  : candidate return addresses\r\n");
    for (index = 0U; index < FAULT_STACK_SCAN_WORDS; index++)
    {
        uint32_t address = sp + (index * 4U);
        uint32_t value;

        if ((address + 4U) > limit)
        {
            break;
        }
        value = *(const volatile uint32_t *)(uintptr_t)address;
        if ((value >= FAULT_FLASH_BEGIN) && (value < FAULT_FLASH_END))
        {
            fault_puts("  ");
            fault_hex32(address);
            fault_puts(" -> ");
            fault_hex32(value);
            fault_puts("\r\n");
        }
    }
}

static void fault_dump_ctx(const fault_ctx_t *ctx)
{
    fault_puts("\r\n=== CPU FAULT ===\r\n");
    fault_puts("kind   : ");
    fault_puts(fault_kind_name(ctx->kind));
    fault_puts(" (IPSR=");
    fault_dec(ctx->kind);
    fault_puts(")\r\n");

    fault_puts("stack  : ");
    fault_puts(((ctx->exc_return & 0x4U) != 0U) ? "PSP" : "MSP");
    fault_puts((ctx->fp_frame != 0U) ? " + FPU frame\r\n" : "\r\n");

    fault_puts("PC     : ");
    fault_hex32(ctx->pc);
    fault_puts("\r\n");
    fault_puts("LR     : ");
    fault_hex32(ctx->lr);
    fault_puts("\r\n");
    fault_puts("SP     : ");
    fault_hex32(ctx->sp);
    fault_puts("\r\n");
    fault_puts("xPSR   : ");
    fault_hex32(ctx->xpsr);
    fault_puts("\r\n");

    fault_puts("R0     : ");
    fault_hex32(ctx->r0);
    fault_puts("\r\n");
    fault_puts("R1     : ");
    fault_hex32(ctx->r1);
    fault_puts("\r\n");
    fault_puts("R2     : ");
    fault_hex32(ctx->r2);
    fault_puts("\r\n");
    fault_puts("R3     : ");
    fault_hex32(ctx->r3);
    fault_puts("\r\n");
    fault_puts("R12    : ");
    fault_hex32(ctx->r12);
    fault_puts("\r\n");

    fault_puts("MSP    : ");
    fault_hex32(ctx->msp);
    fault_puts("\r\n");
    fault_puts("PSP    : ");
    fault_hex32(ctx->psp);
    fault_puts("\r\n");
    fault_puts("EXC_RET: ");
    fault_hex32(ctx->exc_return);
    fault_puts("\r\n");

    fault_puts("CFSR   : ");
    fault_hex32(ctx->cfsr);
    fault_puts("\r\n");
    fault_puts("HFSR   : ");
    fault_hex32(ctx->hfsr);
    fault_puts("\r\n");
    fault_puts("AFSR   : ");
    fault_hex32(ctx->afsr);
    fault_puts("\r\n");
    fault_puts("SHCSR  : ");
    fault_hex32(ctx->shcsr);
    fault_puts("\r\n");

    fault_decode_hfsr(ctx->hfsr);
    fault_decode_cfsr(ctx->cfsr, ctx->mmfar, ctx->bfar);
    fault_dump_stack(ctx->sp);
}

/* ─────────────────────── 崩溃记录打印（断言/开机共用） ─────────────────────── */

static void fault_dump_record(const crash_record_t *record)
{
    if (record == NULL)
    {
        fault_puts("crash  : no record\r\n");
        return;
    }

    fault_puts("\r\n=== CRASH RECORD ===\r\n");
    if (record->kind == (uint32_t)CRASH_KIND_FAULT)
    {
        fault_puts("kind   : FAULT ");
        fault_puts(fault_kind_name(record->fault.fault_kind));
        fault_puts("\r\n");
        fault_puts("PC     : ");
        fault_hex32(record->fault.pc);
        fault_puts("\r\n");
        fault_puts("LR     : ");
        fault_hex32(record->fault.lr);
        fault_puts("\r\n");
        fault_puts("SP     : ");
        fault_hex32(record->fault.sp);
        fault_puts("\r\n");
        fault_puts("xPSR   : ");
        fault_hex32(record->fault.xpsr);
        fault_puts("\r\n");
        fault_decode_hfsr(record->fault.hfsr);
        fault_decode_cfsr(record->fault.cfsr, record->fault.mmfar, record->fault.bfar);
    }
    else
    {
        fault_puts("kind   : ");
        fault_puts((record->kind == (uint32_t)CRASH_KIND_ABORT) ? "ABORT\r\n" : "ASSERT\r\n");
        fault_puts("expr   : ");
        fault_puts_n(record->expression, CRASH_TEXT_CAPACITY);
        fault_puts("\r\n");
        fault_puts("file   : ");
        fault_puts_n(record->file, CRASH_TEXT_CAPACITY);
        fault_port_putc(':');
        fault_dec(record->line);
        fault_puts("\r\n");
    }
    fault_puts("====================\r\n");
}

/* ────────────────────────────── 共享入口 ────────────────────────────── */

/*
 * 由 port 侧的异常入口调用：ctx 已由 port 从硬件压栈帧与故障状态寄存器填好。
 * 不返回（打印 + 落记录 + hook + 停机）。
 */
void fault_report_and_halt(const fault_ctx_t *ctx)
{
    crash_fault_record_t record;

    fault_port_console_init();
    fault_dump_ctx(ctx);

    record.fault_kind = ctx->kind;
    record.pc         = ctx->pc;
    record.lr         = ctx->lr;
    record.sp         = ctx->sp;
    record.xpsr       = ctx->xpsr;
    record.cfsr       = ctx->cfsr;
    record.hfsr       = ctx->hfsr;
    record.mmfar      = ctx->mmfar;
    record.bfar       = ctx->bfar;
    crash_record_fault(&record);

    fault_hook(ctx);

    fault_port_console_drain();
    for (;;)
    {
    }
}

/* ────────────────────────────── 对外接口 ────────────────────────────── */

void fault_init(void)
{
    fault_port_enable_configurable_faults();
}

void fault_report_boot(void)
{
    char                  reason[64];
    uint32_t              flags  = fault_port_reset_flags();
    const crash_record_t *record = crash_record_get();

    fault_port_reset_reason_text(flags, reason, sizeof(reason));

    LOGI("[FAULT] reset reason:%s (rsr=0x%08lx)\r\n", reason, (unsigned long)flags);

    if (record != NULL)
    {
        if (record->kind == (uint32_t)CRASH_KIND_FAULT)
        {
            LOGE("[FAULT] previous crash: %s PC=0x%08lx LR=0x%08lx CFSR=0x%08lx HFSR=0x%08lx\r\n",
                 fault_kind_name(record->fault.fault_kind),
                 (unsigned long)record->fault.pc,
                 (unsigned long)record->fault.lr,
                 (unsigned long)record->fault.cfsr,
                 (unsigned long)record->fault.hfsr);
        }
        else if (record->kind == (uint32_t)CRASH_KIND_ABORT)
        {
            LOGE("[FAULT] previous abort (assertion/test terminal)\r\n");
        }
        else
        {
            LOGE("[FAULT] previous assert: %s (%s:%lu)\r\n",
                 record->expression,
                 record->file,
                 (unsigned long)record->line);
        }
    }

    /* 报告完再清标志，下次复位才看得出真实原因。 */
    fault_port_reset_flags_clear();
}

/* 强符号：覆盖 common/crash.c 的弱默认，让断言路径也走同一份轮询 dump。 */
void crash_platform_report(const crash_record_t *record)
{
    fault_port_console_init();
    fault_dump_record(record);
    fault_port_console_drain();
}

__attribute__((weak)) void fault_hook(const fault_ctx_t *ctx)
{
    (void)ctx;
}
