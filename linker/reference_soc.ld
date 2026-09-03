/*
 * Cortex-R5 参考 SoC 链接脚本。
 *
 * 这里的地址只用于建立可编译、可分析的虚拟硬件契约，不对应任何
 * 真实 SSD 主控。获得目标芯片资料后，应由真实 SoC BSP 替换 MEMORY
 * 布局和相关初始化代码，核心 SSD 模块不需要随之修改。
 */

/* ELF 程序入口；实际复位跳转由 .vectors 中的复位向量完成。 */
ENTRY(Reset_Handler)

MEMORY
{
    /* 向量、代码和只读数据所在区域，模拟 ROM 或已装载的执行存储器。 */
    BOOT (rx)  : ORIGIN = 0x00000000, LENGTH = 2M

    /* 低延迟数据区，当前仅用于 Cortex-R5 各处理器模式的栈。 */
    DTCM (rw)  : ORIGIN = 0x20000000, LENGTH = 64K

    /* 普通可读写内存，用于已初始化数据、BSS 和复位保留数据。 */
    SRAM (rwx) : ORIGIN = 0x21000000, LENGTH = 512K

    /* 大容量工作内存，用于 FTL 映射表和主机数据缓冲区。 */
    DRAM (rwx) : ORIGIN = 0x40000000, LENGTH = 256M
}

SECTIONS
{
    /*
     * ARMv7-R 有 8 个异常入口，每个入口是一条 4 字节 ARM 指令，
     * 因此向量表总大小必须为 32 字节，并按 32 字节边界对齐。
     * KEEP 防止 --gc-sections 删除没有普通函数引用的向量表。
     */
    .vectors :
    {
        . = ALIGN(32);
        KEEP(*(.vectors))
    } > BOOT

    /* 固件代码和只读常量在 BOOT 区域直接执行。 */
    .text :
    {
        . = ALIGN(4);
        /* 启动入口优先放置，随后收集函数独立段和普通代码段。 */
        *(.text.Reset_Handler)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > BOOT

    /* ARM ABI 的栈展开索引；即使当前关闭 unwind，也显式固定其位置。 */
    .ARM.exidx :
    {
        *(.ARM.exidx*)
    } > BOOT

    /*
     * .data 的运行地址 VMA 位于可写 SRAM，但非零初始值必须保存在
     * BOOT 镜像中。启动代码从 __data_load__ 复制到
     * [__data_start__, __data_end__)。
     */
    .data : AT(LOADADDR(.ARM.exidx) + SIZEOF(.ARM.exidx))
    {
        . = ALIGN(8);
        __data_start__ = .;
        *(.data*)
        . = ALIGN(8);
        __data_end__ = .;
    } > SRAM
    __data_load__ = LOADADDR(.data);

    /*
     * 未初始化或零初始化的全局数据不占用固件镜像空间。
     * 启动代码负责清零 [__bss_start__, __bss_end__)。
     */
    .bss (NOLOAD) :
    {
        . = ALIGN(8);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end__ = .;
    } > SRAM

    /*
     * 启动代码不会初始化该区域，可用于保存复位原因或崩溃信息。
     * 真实芯片能否跨复位保留内容取决于 SRAM Bank 的复位行为。
     */
    .noinit (NOLOAD) :
    {
        *(.noinit*)
    } > SRAM

    /* FTL 映射表由运行时初始化或从 NAND 元数据恢复，不进入固件镜像。 */
    .ftl_table (NOLOAD) :
    {
        *(.ftl_table*)
    } > DRAM

    /* NVMe/PCIe 与 NAND 数据路径使用的大容量缓冲区。 */
    .host_buffer (NOLOAD) :
    {
        *(.host_buffer*)
    } > DRAM

    /*
     * ARMv7-R 的异常模式拥有独立的 banked SP，需要分别设置栈顶。
     * 栈向低地址增长；SVC 栈承载 main() 和普通 C 函数，因此分配最大。
     * 当前分配：IRQ 2 KiB、FIQ 1 KiB、Abort 1 KiB、Undefined 1 KiB、
     * SVC 8 KiB。
     */
    .mode_stacks (NOLOAD) :
    {
        . = ALIGN(8);
        . += 2K;
        __irq_stack_top__ = .;
        . += 1K;
        __fiq_stack_top__ = .;
        . += 1K;
        __abort_stack_top__ = .;
        . += 1K;
        __undefined_stack_top__ = .;
        . += 8K;
        __svc_stack_top__ = .;
    } > DTCM

    /* 为 C 库保留传统的静态数据结束符号；当前尚未定义通用堆。 */
    __end__ = __bss_end__;
    end = __end__;

    /* 丢弃不参与运行的编译器说明和 ELF note 元数据。 */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}

/* 链接期验证：异常向量格式错误时立即终止构建。 */
ASSERT(SIZEOF(.vectors) == 32, "ARMv7-R vector table must be 32 bytes")

/* 链接期验证：所有处理器模式栈必须完整落在 DTCM 内。 */
ASSERT(__svc_stack_top__ <= ORIGIN(DTCM) + LENGTH(DTCM), "DTCM stack overflow")
