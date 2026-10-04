#ifndef PLATFORM_STM32H743_FAULT_H
#define PLATFORM_STM32H743_FAULT_H

#include <stdint.h>

/*
 * Cortex-M7 故障捕获 — 见 docs/design/fault_capture.md
 *
 * 覆盖启动文件里的弱默认异常处理器（Default_Handler 死循环），捕获
 * NMI/HardFault/MemManage/BusFault/UsageFault 的现场并轮询 UART 打印，
 * 同时写入跨软复位保留的崩溃记录（common/ssd_crash）。
 */

/* 异常种类，取值等于 IPSR 异常号。 */
typedef enum
{
    FAULT_KIND_NMI        = 2,
    FAULT_KIND_HARDFAULT  = 3,
    FAULT_KIND_MEMMANAGE  = 4,
    FAULT_KIND_BUSFAULT   = 5,
    FAULT_KIND_USAGEFAULT = 6,
} fault_kind_t;

/* 捕获到的故障现场。 */
typedef struct
{
    uint32_t kind;       /* fault_kind_t */
    uint32_t sp;         /* 出错时使用的栈指针（MSP 或 PSP） */
    uint32_t msp;        /* 当前 MSP */
    uint32_t psp;        /* 当前 PSP */
    uint32_t exc_return; /* 进入异常时的 LR */
    uint32_t fp_frame;   /* 1 = 叠加了 FPU 扩展帧（EXC_RETURN bit4=0） */

    /* 硬件压栈帧 */
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t xpsr;

    /* 故障状态寄存器 */
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t afsr;
    uint32_t shcsr;
} fault_ctx_t;

/* 使能 MemManage/BusFault/UsageFault，避免全部升级成 HardFault，便于区分。 */
void fault_init(void);

/* 开机报告：RCC 复位原因 + 上次崩溃记录。须在 ssd_log sink 绑定之后调用。 */
void fault_report_boot(void);

/* 弱符号扩展点：故障捕获后、停机前调用（默认空实现）。 */
void fault_hook(const fault_ctx_t *ctx);

#endif /* PLATFORM_STM32H743_FAULT_H */
