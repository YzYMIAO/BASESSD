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

/* 32 词只覆盖硬件异常帧（FP 扩展帧 0x88）；出错函数自己的栈帧在它之上，
 * 其保存的 LR（真正调用者）落不进窗口。64/96 词把被中断函数的帧也纳入。 */
#define FAULT_STACK_SCAN_WORDS 96U

/*
 * ⚠ 已知残留耦合：以下三个常量与 _estack 是 stm32h743 的内存映射（Flash 起止、
 * DTCM 起点、链接脚本栈顶符号）。栈扫描目前留在共享层，换 target 时需下沉为
 * port 接口（如 fault_port_stack_top()/fault_port_is_code_address()）。
 */
#define FAULT_FLASH_BEGIN 0x08000000U

#define FAULT_FLASH_END 0x08200000U

#define FAULT_DTCM_BEGIN 0x20000000U

/* ITCM 热代码（.ssd_code_high，VMA 0x0）：被中断函数的保存 LR 常落在这里。
 * 阈值 0x1000 只为压掉栈上的小整数噪声（本仓热函数都在 0x3xxx 以上）。 */
#define FAULT_ITCM_CODE_BEGIN 0x00001000U

#define FAULT_ITCM_END 0x00010000U

/* 栈顶符号由链接脚本提供（_estack = DTCM 末尾），用于给栈扫描设界。 */
extern DWORD _estack;

/* ────────────────────────────── 轮询控制台 ────────────────────────────── */

static void fault_puts(const char *text)
{
    while (*text != '\0')
    {
        fault_port_putc(*text++);
    }
}

static void fault_bit(DWORD value, DWORD mask, const char *name)
{
    if ((value & mask) != 0U)
    {
        fault_port_putc(' ');
        fault_puts(name);
    }
}

static void fault_hex32(DWORD value)
{
    static const char digits[] = "0123456789abcdef";
    int               shift;

    fault_puts("0x");
    for (shift = 28; shift >= 0; shift -= 4)
    {
        fault_port_putc(digits[(value >> shift) & 0xFU]);
    }
}

static void fault_dec(DWORD value)
{
    char  buffer[10];
    DWORD index = 0U;

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

static const char *fault_kind_name(DWORD kind)
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

/* HFSR：FORCED 表示由可配置故障升级而来，需再看 CFSR 三段定位真因。 */
static void fault_decode_hfsr(DWORD hfsr)
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

static void fault_decode_cfsr(DWORD cfsr, DWORD mmfar, DWORD bfar)
{
    DWORD mmfsr = cfsr & 0xFFU;
    DWORD bfsr  = (cfsr >> 8U) & 0xFFU;
    DWORD ufsr  = (cfsr >> 16U) & 0xFFFFU;

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
static void fault_dump_stack(DWORD sp)
{
    DWORD limit = (uintptr_t)&_estack;
    DWORD index;

    if ((sp < FAULT_DTCM_BEGIN) || (sp >= limit))
    {
        /* 栈指针不在 DTCM 内（本工程 MSP 在 DTCM），不冒险读，避免二次故障。 */
        fault_puts("stack  : scan skipped (SP outside DTCM)\r\n");
        return;
    }

    fault_puts("stack  : candidate return addresses\r\n");
    for (index = 0U; index < FAULT_STACK_SCAN_WORDS; index++)
    {
        DWORD address = sp + (index * 4U);
        DWORD value;

        if ((address + 4U) > limit)
        {
            break;
        }
        value = *(const volatile DWORD *)(uintptr_t)address;

        const BOOL in_flash = (value >= FAULT_FLASH_BEGIN) && (value < FAULT_FLASH_END);
        const BOOL in_itcm  = (value >= FAULT_ITCM_CODE_BEGIN) && (value < FAULT_ITCM_END) && ((value & 1U) != 0U);

        if ((in_flash == true) || (in_itcm == true))
        {
            fault_puts("  ");
            fault_hex32(address);
            fault_puts(" -> ");
            fault_hex32(value);
            fault_puts((in_itcm == true) ? "  [itcm]\r\n" : "\r\n");
        }
    }
}

/* 异常帧原始内容（28 词）：用于核对「基本帧在低地址 / FPU 帧在哪一侧 / SP 是否被
 * 踩」。解码后的 R0..xPSR 只覆盖基本帧，帧布局异常或 FPU 扩展帧错位时，
 * 原始词是唯一能看出问题的证据。 */
static void fault_dump_frame_raw(DWORD sp)
{
    DWORD limit = (uintptr_t)&_estack;

    if ((sp < FAULT_DTCM_BEGIN) || (sp >= limit))
    {
        return;
    }

    fault_puts("frame  : raw words from SP\r\n");
    for (DWORD index = 0U; index < 28U; index++)
    {
        DWORD address = sp + (index * 4U);

        if ((address + 4U) > limit)
        {
            break;
        }
        fault_puts("  ");
        fault_hex32(address);
        fault_puts(" : ");
        fault_hex32(*(const volatile DWORD *)(uintptr_t)address);
        fault_puts("\r\n");
    }
}

/**
 * @brief 地址是否落在可安全读回的代码 / 内存区。
 *
 * @param address 待判地址。
 *
 * @return true = ITCM / Flash / AXI SRAM / D2D3 SRAM 内，可读；
 *         false = 不在已知映射内（跳过读取，避免故障处理器再触发一次故障）。
 */
static BOOL fault_code_readable(DWORD address)
{
    const BOOL in_itcm  = (address < FAULT_ITCM_END);
    const BOOL in_flash = (address >= FAULT_FLASH_BEGIN) && (address < FAULT_FLASH_END);
    const BOOL in_axi   = (address >= 0x24000000U) && (address < 0x24080000U);
    const BOOL in_d2d3  = (address >= 0x30000000U) && (address < 0x38010000U);

    return in_itcm || in_flash || in_axi || in_d2d3;
}

/**
 * @brief 打印 PC 附近 10 个字的**实时内存内容**（不是符号表里的那一份）。
 *
 * @param pc 出错 PC；从 `(pc & ~3) - 8` 起取 10 个字，含 PC 的那行标 `<- PC`。
 *
 * @note 「一条对齐的普通 LDR 报 UNALIGNED」只有两个来源：帧解错，或**跑着的代码不是
 *       ELF 里那一份**（野写踩到 ITCM —— 例如一个字节把 `ldr r3,[r7,#8]` 改成
 *       `ldr r1,[r0]`，而 `r0` 是非对齐的器件地址 ⇒ 非对齐读）。打印实时字节即可当场
 *       区分，否则现场会一直指向被踩的受害者而不是写方。
 */
static void fault_dump_code(DWORD pc)
{
    DWORD start = pc & ~3U;

    if (fault_code_readable(start) == false)
    {
        return;
    }

    start = (start >= 8U) ? (start - 8U) : 0U;
    if (fault_code_readable(start + 36U) == false)
    {
        return;
    }

    fault_puts("code   : words around PC (live memory)\r\n");
    for (DWORD index = 0U; index < 10U; index++)
    {
        DWORD address = start + (index * 4U);

        fault_puts("  ");
        fault_hex32(address);
        fault_puts(" : ");
        fault_hex32(*(const volatile DWORD *)(uintptr_t)address);
        if ((address <= pc) && (pc < (address + 4U)))
        {
            fault_puts("  <- PC");
        }
        fault_puts("\r\n");
    }
}

static void fault_dump_ctx(const fault_ctx_s *ctx)
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
    fault_puts("R7(f)  : ");
    fault_hex32(ctx->r7);
    fault_puts(((ctx->r7 & 0x7U) != 0U) ? "  (8-byte MISALIGNED)\r\n" : "  (8-byte aligned)\r\n");
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
    fault_puts("CCR    : ");
    fault_hex32(ctx->ccr);
    fault_puts("\r\n");
    fault_puts("FPCCR  : ");
    fault_hex32(ctx->fpccr);
    fault_puts("\r\n");

    fault_decode_hfsr(ctx->hfsr);
    fault_decode_cfsr(ctx->cfsr, ctx->mmfar, ctx->bfar);
    fault_dump_code(ctx->pc);
    fault_dump_frame_raw(ctx->sp);
    fault_dump_stack(ctx->sp);
}

/* ────────────────────────────── 共享入口 ────────────────────────────── */

/*
 * 由 port 侧的异常入口调用：ctx 已由 port 从硬件压栈帧与故障状态寄存器填好。
 * 不返回（打印 + 落记录 + hook + 停机）。
 */
void fault_report_and_halt(const fault_ctx_s *ctx)
{
    crash_fault_record_s record;

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
    DWORD                 flags  = fault_port_reset_flags();
    const crash_record_s *record = crash_record_get();

    fault_port_reset_reason_text(flags, reason, sizeof(reason));

    LOGI("[FAULT] reset reason:%s (rsr=0x%08lx)\r\n", reason, flags);

    if (record != NULL)
    {
        if (record->kind == CRASH_KIND_FAULT)
        {
            LOGE("[FAULT] previous crash: %s PC=0x%08lx LR=0x%08lx CFSR=0x%08lx HFSR=0x%08lx\r\n", fault_kind_name(record->fault.fault_kind), record->fault.pc, record->fault.lr, record->fault.cfsr, record->fault.hfsr);
        }
        else if (record->kind == CRASH_KIND_ABORT)
        {
            LOGE("[FAULT] previous abort (assertion/test terminal)\r\n");
        }
        else
        {
            LOGE("[FAULT] previous assert: %s (%s:%lu)\r\n", record->expression, record->file, record->line);
        }
    }

    /* 报告完再清标志，下次复位才看得出真实原因。 */
    fault_port_reset_flags_clear();
}

__attribute__((weak)) void fault_hook(const fault_ctx_s *ctx)
{
    (void)ctx;
}
