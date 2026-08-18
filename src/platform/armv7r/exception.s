/*
 * Cortex-R5 / ARMv7-R 最小启动与异常入口。
 *
 * 汇编器：GNU assembler（GAS），由 arm-none-eabi-gcc 调用。
 * 语法：GNU unified syntax，不是 Keil/ARM Compiler 的 armasm 语法。
 * 指令状态：本文件使用 A32（传统 ARM 32 位定长指令）；普通 C 文件由
 *           -mthumb 编译为 T32/Thumb-2。链接器负责生成 ARM/Thumb
 *           interworking 跳转。
 *
 * 启动职责：
 *   1. 安装 ARMv7-R 异常向量；
 *   2. 屏蔽 IRQ/FIQ；
 *   3. 为各异常模式设置独立栈；
 *   4. 设置 VBAR；
 *   5. 调用 SoC 早期初始化；
 *   6. 调用 C 运行时初始化，复制 .data 并清零 .bss；
 *   7. 进入 main()。
 */

/* 使用 ARM 和 Thumb 共用的统一汇编语法。 */
.syntax unified

/* 限定允许使用 ARMv7-R 架构指令。 */
.arch armv7-r

/* 以下汇编代码编码为 A32 指令，每条指令固定为 4 字节。 */
.arm

/*
 * "a" 表示该段需要分配到运行镜像，"x" 表示允许执行。
 * 链接脚本把 .vectors 固定在 BOOT 区域起始位置。
 */
.section .vectors, "ax"

/* GAS 中 .align 5 表示按 2^5 = 32 字节对齐。 */
.align 5

/* 导出向量表符号，供链接脚本和 VBAR 初始化代码引用。 */
.global __vector_table
__vector_table:
    /* ARMv7-R 每个向量槽是一条跳转指令，而不是 Cortex-M 的函数地址。 */
    b Reset_Handler
    b Undefined_Handler
    b SVC_Handler
    b Prefetch_Abort_Handler
    b Data_Abort_Handler
    b Reserved_Handler
    b IRQ_Handler
    b FIQ_Handler

/* Reset_Handler 单独成段，链接脚本会将它优先放在普通代码之前。 */
.section .text.Reset_Handler, "ax"

/* A32 指令至少需要 4 字节对齐，即 2^2。 */
.align 2
.global Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    /*
     * 同时设置 CPSR.I 和 CPSR.F，禁止 IRQ 与 FIQ。
     * 中断必须等到向量表、模式栈和中断控制器都准备完成后再开启。
     */
    cpsid if

    /*
     * Cortex-R5 的异常模式拥有独立的 banked SP。cps 指令切换处理器模式，
     * ldr sp, =symbol 是 GAS 伪指令，用链接器符号给当前模式的 SP 赋值。
     * 模式号：FIQ=0x11、IRQ=0x12、SVC=0x13、Abort=0x17、Undefined=0x1b。
     */
    cps #0x12
    ldr sp, =__irq_stack_top__

    cps #0x11
    ldr sp, =__fiq_stack_top__

    cps #0x17
    ldr sp, =__abort_stack_top__

    cps #0x1b
    ldr sp, =__undefined_stack_top__

    /* 最后停留在 SVC 模式，后续启动 C 代码使用 SVC 栈。 */
    cps #0x13
    ldr sp, =__svc_stack_top__

    /*
     * 把异常向量基址写入 CP15 VBAR（Vector Base Address Register）。
     * mcr 是访问系统控制协处理器 CP15 的 A32 指令。
     */
    ldr r0, =__vector_table
    mcr p15, 0, r0, c12, c0, 0

    /* 确保后续取指已经看到新的向量基址。 */
    isb

    /*
     * SoC 极早期初始化发生在 .data 搬运和 .bss 清零之前。
     * 因此该函数只能使用当前可用栈、寄存器和明确可用的 MMIO，不能依赖
     * 已初始化全局变量、零初始化全局变量、动态内存或尚未准备好的 DDR。
     */
    bl soc_early_init

    /*
     * 在这里调用 C 运行时初始化函数：
     *   BOOT 中的 .data 初始值 -> SRAM 中的 .data 运行地址；
     *   SRAM 中的 .bss       -> 全部清零；
     *   然后调用 soc_memory_init() 并执行 DSB/ISB。
     * 返回后，普通 C 全局变量才可以安全使用。
     */
    bl armv7r_runtime_init

    /*
     * 汇编阶段只建立最小 CPU 和 C 运行环境。中断控制器、Timer、NAND、
     * FTL 与主机接口由 main() 中的 C 启动状态机按依赖顺序初始化。
     */
    bl main

    /*
     * main() 按设计不会返回。如果意外返回，则进入低功耗等待死循环。
     * 数字标签 1: 是 GAS 局部标签，1b 表示向后跳到最近的 1:。
     */
1:
    wfi
    b 1b

/* 记录 Reset_Handler 的符号大小，便于 ELF 调试器和反汇编工具识别。 */
.size Reset_Handler, . - Reset_Handler

/*
 * 为尚未实现的异常入口生成弱定义。真实 BSP 或诊断模块可以提供同名强
 * 符号覆盖它。默认行为是停在异常入口，防止错误执行继续破坏现场。
 */
.macro DEFAULT_EXCEPTION name
    .weak \name
    .type \name, %function
\name:
    b \name
.endm

DEFAULT_EXCEPTION Undefined_Handler
DEFAULT_EXCEPTION SVC_Handler
DEFAULT_EXCEPTION Prefetch_Abort_Handler
DEFAULT_EXCEPTION Data_Abort_Handler
DEFAULT_EXCEPTION Reserved_Handler
DEFAULT_EXCEPTION FIQ_Handler

/* IRQ 入口提供最小现场保存，并把具体中断分发交给 C 函数。 */
.weak IRQ_Handler
.type IRQ_Handler, %function
IRQ_Handler:
    /* ARM IRQ 模式的 LR 指向返回地址后 4 字节，返回前需要修正。 */
    sub lr, lr, #4

    /*
     * SRSDB 把 IRQ 模式的修正后 LR 和 SPSR 保存到 SVC 模式栈。
     * #0x13 是 SVC 模式号，DB 表示 Decrement Before 的满递减栈方式。
     */
    srsdb sp!, #0x13

    /* 切换到 SVC 模式，后续 C 函数使用容量更大的 SVC 栈。 */
    cps #0x13

    /* 保存 AAPCS 中由调用者保存的寄存器以及当前 SVC LR。 */
    push {r0-r3, r12, lr}

    /* C 分发函数未来负责读取活动 IRQ ID、清除中断并调用注册处理函数。 */
    bl ssd_irq_dispatch

    /* 恢复调用 C 函数前保存的普通寄存器。 */
    pop {r0-r3, r12, lr}

    /*
     * 从 SVC 栈恢复先前保存的返回 PC 和 CPSR，原子地返回被 IRQ 打断的位置。
     * FD 是 Full Descending 栈别名，与前面的 SRSDB 配套。
     */
    rfefd sp!
