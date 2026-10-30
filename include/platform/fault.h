#ifndef PLATFORM_FAULT_H
#define PLATFORM_FAULT_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * Cortex-M 故障捕获抽象接口（共享逻辑在 src/platform/fault.c，SoC 适配在
 * src/platform/<soc>/fault_port.c）—— 设计见 docs/design/fault_capture.md
 *
 * 覆盖启动文件里的弱默认异常处理器（Default_Handler 死循环），捕获
 * NMI/HardFault/MemManage/BusFault/UsageFault 的现场并轮询控制台打印，
 * 同时写入跨软复位保留的崩溃记录（common/crash）。
 *
 * 本头零 HAL 类型：控制台外设、故障状态寄存器与复位原因寄存器一律留在 SoC 侧。
 */

/* 异常种类，取值等于 IPSR 异常号。 */
typedef enum
{
    FAULT_KIND_NMI        = 2,
    FAULT_KIND_HARDFAULT  = 3,
    FAULT_KIND_MEMMANAGE  = 4,
    FAULT_KIND_BUSFAULT   = 5,
    FAULT_KIND_USAGEFAULT = 6,
} fault_kind_e;

/* 捕获到的故障现场。 */
typedef struct
{
    DWORD kind;       /* fault_kind_e */
    DWORD sp;         /* 出错时使用的栈指针（MSP 或 PSP） */
    DWORD msp;        /* 当前 MSP */
    DWORD psp;        /* 当前 PSP */
    DWORD exc_return; /* 进入异常时的 LR */
    DWORD fp_frame;   /* 1 = 叠加了 FPU 扩展帧（EXC_RETURN bit4=0） */

    /* 硬件压栈帧 */
    DWORD r0;
    DWORD r1;
    DWORD r2;
    DWORD r3;
    DWORD r12;
    DWORD lr;
    DWORD pc;
    DWORD xpsr;

    /* 故障状态寄存器 */
    DWORD cfsr;
    DWORD hfsr;
    DWORD mmfar;
    DWORD bfar;
    DWORD afsr;
    DWORD shcsr;

    /* 平台配置寄存器（判定 UNALIGNED 的关键证据）：
     * ccr：bit9=STKALIGN、bit3=UNALIGN_TRP、bit16/17=DC/IC；
     * fpccr：bit31=ASPEN、bit30=LSPEN（FPU lazy stacking 开关）。
     * 不放在 port 私有代码里，是因为这类异常只能从「这两组位 + 异常帧原始内容 +
     * PC 处实时指令字」三者合起来解释。 */
    DWORD ccr;
    DWORD fpccr;

    /* 异常入口处尚未被 C 序言覆盖的寄存器（port 侧蹦床抓取）：
     * r7：出错函数的帧指针。普通 LDR 报 UNALIGNED 时，靠它核对「访问地址
     *     到底对没对齐」；帧里没有 R4~R11，这是唯一能补回来的一个。
     * 注意 handler 态的 CONTROL.FPCA 恒为 0（入异常时被清），不能用它判扩展帧。 */
    DWORD r7;
} fault_ctx_t;

/* 使能 MemManage/BusFault/UsageFault，避免全部升级成 HardFault，便于区分。 */
void fault_init(void);

/* 开机报告：复位原因 + 上次崩溃记录。须在日志 sink 绑定之后调用。 */
void fault_report_boot(void);

/* 弱符号扩展点：故障捕获后、停机前调用（默认空实现）。 */
void fault_hook(const fault_ctx_t *ctx);

/*
 * ---- 共享入口：由 port 侧的异常入口调用 ----
 */

/* 打印现场、落崩溃记录、调用 hook，然后停机（不返回）。 */
void fault_report_and_halt(const fault_ctx_t *ctx);

/*
 * ---- port 接口：由 src/platform/<soc>/fault_port.c 实现 ----
 */

/* 抢占异步 TX 通路，改用轮询控制台（故障时中断/DMA 可能已不可用）。 */
void fault_port_console_init(void);
void fault_port_putc(char character);
/* 等最后一个字节离开 TX 引脚，否则停机后尾部字符会丢。 */
void fault_port_console_drain(void);

/* 使能三类可配置故障，否则它们会全部升级成 HardFault，丢失真因。 */
void fault_port_enable_configurable_faults(void);

/* 复位原因：读原始标志位、把它翻成可读文本、报告后清标志。 */
DWORD fault_port_reset_flags(void);
void  fault_port_reset_reason_text(DWORD flags, char *buffer, DWORD capacity);
void  fault_port_reset_flags_clear(void);

#endif /* PLATFORM_FAULT_H */
