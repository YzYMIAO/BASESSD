/* STM32F103C8T6 链接脚本示例 */
/* Flash: 64KB, RAM: 20KB */

ENTRY(Reset_Handler)   /* 程序的入口点 */

MEMORY {
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
    RAM (rwx)   : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS {
    /* 中断向量表必须放在 Flash 的最开头 */
    .isr_vector : {
        KEEP(*(.isr_vector))   /* 强制保留中断向量表 */
    } > FLASH

    /* 代码段 */
    .text : {
        *(.text)               /* 所有代码 */
        *(.text.*)             /* 所有子段 */
        *(.glue_7)             /* 链接器生成的胶水代码 */
        *(.glue_7t)
    } > FLASH

    /* 只读数据 */
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } > FLASH

    /* 已初始化数据（运行时在 RAM，初值在 Flash） */
    _sidata = LOADADDR(.data);   /* Flash 中 .data 的起始地址 */
    .data : {
        _sdata = .;               /* RAM 中 .data 的起始地址 */
        *(.data)
        *(.data.*)
        _edata = .;               /* RAM 中 .data 的结束地址 */
    } > RAM AT > FLASH

    /* 未初始化数据（运行时在 RAM，上电清零） */
    .bss : {
        _sbss = .;                /* RAM 中 .bss 的起始地址 */
        *(.bss)
        *(.bss.*)
        *(COMMON)                 /* 公共符号 */
        _ebss = .;                /* RAM 中 .bss 的结束地址 */
    } > RAM

    /* 栈（放在 RAM 的最后） */
    .stack : {
        . = ALIGN(8);
        __estack = .;              /* 栈顶地址（双下划线） */
    } > RAM
}