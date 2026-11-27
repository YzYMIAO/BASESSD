/*
 * fault_port.c — Cortex-M 故障捕获的 stm32h743 适配层
 *
 * 共享逻辑在 src/platform/fault.c（跨 target 编译一次，零 HAL）。本文件只做
 * 共享层做不了的四件事：
 *   ① 异常入口蹦床：naked asm，必须在任何 C 序言之前确定出错栈并抓现场；
 *   ② 轮询控制台：直接操作 USART1/DMA1_Stream0 寄存器；
 *   ③ 故障/复位状态寄存器：SCB->SHCSR 使能、RCC->RSR 复位原因解码；
 *   ④ 把现场填成 fault_ctx_s 后转交共享层的 fault_report_and_halt()。
 *
 * implements fault.h（端口适配：本文件实现抽象头的 port 接口）。
 */

#include "fault.h"
#include "stm32h7xx_hal.h"

#define FAULT_CONSOLE USART1

#define FAULT_TX_DMA DMA1_Stream0

#define FAULT_TX_GUARD 200000U /* 轮询 TXE/TC 的自旋上限，防挂死 */

#define FAULT_DMA_GUARD 100000U /* 等 DMA 关闭的自旋上限 */

#define FAULT_ENTRY(name)                         \
    void name(void) __attribute__((naked, used)); \
    void name(void)                               \
    {                                             \
        __asm volatile("b fault_trampoline \n");  \
    }

/* 控制台是否可用：USART 未使能时（极早期故障）只写记录、不打印。 */
static volatile BYTE s_fault_console_ready;

/* ────────────────────────────── 异常入口 ────────────────────────────── */

/*
 * 故障主处理：frame 指向硬件压栈帧，exc_return 为进入异常时的 LR，
 * fault_r7 为蹦床在 C 序言之前抓到的 R7（硬件不压 R4~R11，只能这样取）。
 * 现场在此抓取（只有 port 侧够得着 CMSIS/寄存器），随后转交共享层，不返回。
 */
void fault_port_entry_c(DWORD *frame, DWORD exc_return, DWORD fault_r7) __attribute__((used, noinline));

/*
 * naked 蹦床：进入异常时 LR = EXC_RETURN。bit2 选栈（0=MSP / 1=PSP），
 * bit4=0 表示叠加了 FPU 扩展帧（硬浮点下常态）。必须在编译器插入任何序言
 * 之前读取，否则读到的就是异常栈而不是出错栈。
 */
static void fault_trampoline(void) __attribute__((naked, used));

static DWORD fault_append_text(char *buffer, DWORD capacity, DWORD length, const char *text)
{
    while ((*text != '\0') && (length < (capacity - 1U)))
    {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
    return length;
}

static void fault_trampoline(void)
{
    __asm volatile("tst   lr, #4           \n"
                   "ite   eq               \n"
                   "mrseq r0, msp          \n"
                   "mrsne r0, psp          \n"
                   "mov   r1, lr           \n"
                   "mov   r2, r7           \n" /* 出错上下文的 R7，C 序言前抓 */
                   "b     fault_port_entry_c \n");
}

/*
 * 抢占异步 TX 通路：关 TXE 中断、关 DMA 请求、abort DMA1_Stream0。
 * 此后 USART 的 TDR 由本文件独占，中断/DMA 不会再动它。
 */
void fault_port_console_init(void)
{
    DWORD guard;

    FAULT_CONSOLE->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
    FAULT_CONSOLE->CR3 &= ~USART_CR3_DMAT;
    FAULT_TX_DMA->CR &= ~DMA_SxCR_EN;
    for (guard = 0U; (guard < FAULT_DMA_GUARD) && ((FAULT_TX_DMA->CR & DMA_SxCR_EN) != 0U); guard++)
    {
    }

    s_fault_console_ready = ((FAULT_CONSOLE->CR1 & USART_CR1_UE) != 0U) ? 1U : 0U;
}

void fault_port_putc(char character)
{
    DWORD guard;

    if (s_fault_console_ready == 0U)
    {
        return;
    }
    for (guard = 0U; (guard < FAULT_TX_GUARD) && ((FAULT_CONSOLE->ISR & USART_ISR_TXE_TXFNF) == 0U); guard++)
    {
    }
    FAULT_CONSOLE->TDR = (DWORD)(BYTE)character;
}

/* 等最后一个字节离开 TX 引脚（TC），否则停机后尾部字符会丢。 */
void fault_port_console_drain(void)
{
    DWORD guard;

    if (s_fault_console_ready == 0U)
    {
        return;
    }
    for (guard = 0U; (guard < FAULT_TX_GUARD) && ((FAULT_CONSOLE->ISR & USART_ISR_TC) == 0U); guard++)
    {
    }
}

void fault_port_enable_configurable_faults(void)
{
    /* 使能三类可配置故障，否则它们会全部升级成 HardFault，丢失真因。 */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
    __DSB();
    __ISB();
}

/* ────────────────────────────── 复位原因 ────────────────────────────── */

DWORD fault_port_reset_flags(void)
{
    return RCC->RSR;
}

void fault_port_reset_flags_clear(void)
{
    /* 报告完再清标志，下次复位才看得出真实原因。 */
    RCC->RSR |= RCC_RSR_RMVF;
}

void fault_port_reset_reason_text(DWORD flags, char *buffer, DWORD capacity)
{
    DWORD length = 0U;

    if ((flags & RCC_RSR_PORRSTF) != 0U)
    {
        length = fault_append_text(buffer, capacity, length, " POR");
    }
    if ((flags & RCC_RSR_PINRSTF) != 0U)
    {
        length = fault_append_text(buffer, capacity, length, " PIN");
    }
    if ((flags & RCC_RSR_BORRSTF) != 0U)
    {
        length = fault_append_text(buffer, capacity, length, " BOR");
    }
    if ((flags & RCC_RSR_SFTRSTF) != 0U)
    {
        length = fault_append_text(buffer, capacity, length, " SFT");
    }
    if ((flags & RCC_RSR_IWDG1RSTF) != 0U)
    {
        length = fault_append_text(buffer, capacity, length, " IWDG");
    }
    if ((flags & RCC_RSR_WWDG1RSTF) != 0U)
    {
        length = fault_append_text(buffer, capacity, length, " WWDG");
    }
    if ((flags & RCC_RSR_LPWRRSTF) != 0U)
    {
        length = fault_append_text(buffer, capacity, length, " LPWR");
    }
    if (length == 0U)
    {
        length = fault_append_text(buffer, capacity, length, " none");
    }
    (void)length;
}

void fault_port_entry_c(DWORD *frame, DWORD exc_return, DWORD fault_r7)
{
    fault_ctx_s ctx;

    ctx.kind       = __get_IPSR();
    ctx.sp         = (uintptr_t)frame;
    ctx.msp        = __get_MSP();
    ctx.psp        = __get_PSP();
    ctx.exc_return = exc_return;
    ctx.fp_frame   = ((exc_return & 0x10U) == 0U) ? 1U : 0U;
    ctx.r7         = fault_r7;

    ctx.r0   = frame[0];
    ctx.r1   = frame[1];
    ctx.r2   = frame[2];
    ctx.r3   = frame[3];
    ctx.r12  = frame[4];
    ctx.lr   = frame[5];
    ctx.pc   = frame[6];
    ctx.xpsr = frame[7];

    ctx.cfsr  = SCB->CFSR;
    ctx.hfsr  = SCB->HFSR;
    ctx.mmfar = SCB->MMFAR;
    ctx.bfar  = SCB->BFAR;
    ctx.afsr  = SCB->AFSR;
    ctx.shcsr = SCB->SHCSR;
    ctx.ccr   = SCB->CCR;
    ctx.fpccr = FPU->FPCCR;

    fault_report_and_halt(&ctx);
}

FAULT_ENTRY(NMI_Handler)
FAULT_ENTRY(HardFault_Handler)
FAULT_ENTRY(MemManage_Handler)
FAULT_ENTRY(BusFault_Handler)
FAULT_ENTRY(UsageFault_Handler)

/*
 * 内核系统异常入口（向量表名由 startup_stm32h743xx.s 固定）。
 * SYS_SUPPORT_OS == 0，本仓裸机运行，这三个向量永不触发；保留空实现只为让向量表
 * 有一个明确的 C 归属，与上面的故障入口同处一个 port 文件。
 */
void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}
