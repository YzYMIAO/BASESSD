/*
 * ============================================================
 *  SSD 主控启动文件
 *  适用：Cortex-M7/M55 带 TCM 的 SSD 主控
 *  工具链：arm-none-eabi-gcc (GNU)
 *  功能：
 *    1. 设置栈指针（_estack 在 DRAM 中）
 *    2. 初始化 ITCM/DTCM
 *    3. 从 NAND Flash 加载固件到 DRAM
 *    4. 拷贝 .sram_data 到 SRAM
 *    5. 清零 .sram_bss
 *    6. 跳转到 DRAM 主固件
 ┌─────────────────────────────────────────────────────────────────────────┐
│  内存分布与执行                                                         │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ITCM（0x00000000）                                                    │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │  .itcm_vector   ← 中断向量表                                    │  │
│  │  .itcm_text     ← Reset_Handler、NAND_Load_Firmware            │  │
│  │                  ← 中断服务函数（NVME_IRQHandler 等）           │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                              │                                          │
│                              ▼ 跳转                                     │
│  DRAM（0x40000000）                                                    │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │  .dram_text     ← main、ssd_controller_init                    │  │
│  │                  ← nvme_process_commands                       │  │
│  │                  ← ftl_garbage_collect                         │  │
│  │                  ← while(1) 主循环                             │  │
│  ├─────────────────────────────────────────────────────────────────┤  │
│  │  .dram_rodata   ← 只读数据                                      │  │
│  ├─────────────────────────────────────────────────────────────────┤  │
│  │  .ftl_table     ← FTL 映射表                                    │  │
│  ├─────────────────────────────────────────────────────────────────┤  │
│  │  .host_buffer   ← 主机数据缓冲区                                │  │
│  ├─────────────────────────────────────────────────────────────────┤  │
│  │  .heap          ← malloc 动态分配                               │  │
│  ├─────────────────────────────────────────────────────────────────┤  │
│  │  .stack         ← 栈                                           │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
 * ============================================================
 */
 
 
 .syntax unified
 .thumb
 
 /*
 * ============================================================
 *  全局符号声明
 * ============================================================
 */
 .global Reset_Handler
 .global Default_Handler
 .global __main
 .global main
 
 /*
 * ============================================================
 *  导入链接脚本符号
 * ============================================================
 */
 
 /* --- 栈（DRAM 中） --- */
 .extern _estack
 
 /* --- ITCM 段 --- */
 .extern _eitcm
 
 /* --- DTCM 段 --- */
 .extern _sdtcm
 .extern _edtcm
 
 /* --- SRAM 段 --- */
 .extern _ssram
 .extern _esram
 .extern _sisram
 
 /* --- SRAM .noinit 段（无需初始化） --- */
 .extern _snoinit
 .extern _enoinit
 
 /* --- SRAM .bss 段 --- */
 .extern _ssram_bss
 .extern _esram_bss
 
 /* --- DRAM 固件段（LMA 在 NAND Flash） --- */
 .extern _sdram_text
 .extern _edram_text
 .extern _sidram
 
 /* --- DRAM 只读数据段 --- */
 .extern _sdram_rodata
 .extern _edram_rodata
 .extern _sidram_rodata
 
 /* --- 外部 C 函数 --- */
 
 .extern NAND_Init_LowLevel
 .extern NAND_Load_Firmware
 .extern TCM_Init
 
 /*
 * ============================================================
 *  中断向量表（在 ITCM 中）
 * ============================================================
 */
 
 
 .section .itcm_vector, "a"
 .align 2
 
 __itcm_vector:
	.long _estack               /*栈顶 DRAM中*/
	.long Reset_Handler         /*复位向量*/
	.long 0                     /*HardFault*/
	.long 0                     /*MemManage*/
	.long 0                     /*BusFault*/
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0                     /*SVC*/
	.long 0                     /*DebugMon*/
	.long 0
	.long 0                     /*PendSV*/
	.long 0                     /*SysTick*/
	
	/* 以下是 SSD 主控外设中断（根据实际芯片添加） */
 
	.long 0                     /*NVMe 中断*/
	.long 0                     /*NAND 中断*/
	.long 0                     /*DMA 中断*/
	.long 0                     /*FTL 中断*/
 
 /*
 * ============================================================
 *  复位处理函数（在 ITCM 中执行）
 * ============================================================
 */
 
 .section .itcm_text
 .thumb_func
 
 Reset_Handler:
	/*
     * ========================================================
     *  阶段 1：基础硬件初始化
     *  此时 DRAM 可能尚未初始化，使用 ITCM/DTCM/SRAM
     * ========================================================
     */
	
	/* 1.1 设置栈指针（使用 DRAM 中的栈） */
	LDR	R0, =_estack
	MOV	SP, R0
	/* 1.2 调用 TCM 初始化（使能 ITCM/DTCM） */
	BL	TCM_Init
	/* 1.3 初始化 NAND 控制器（低层，只读模式） */
	BL	NAND_Init_LowLevel
 
	/*
     * ========================================================
     *  阶段 2：从 NAND Flash 加载固件到 DRAM
     *  NAND_FW（LMA）→ DRAM（VMA）
     * ========================================================
     */
	/* 2.1 计算固件大小 */
	LDR	R0, =_sdram_text        /* R0 = DRAM 目标地址（VMA） */
	LDR R1, =_edram_text        /* R1 = DRAM 结束地址 */
	SUBS R1, R1,R0               /* R1 = 固件大小（字节） */
	
	/* 2.2 获取 NAND 源地址（LMA） */
	LDR R2,=_sidram                /* R2 = NAND 中固件的起始地址 */
	
	/* 2.3 调用 NAND 加载函数 */
	BL	NAND_Load_Firmware
	
	/* 2.4 加载 .dram_rodata（从 NAND 到 DRAM） */
	LDR R0, =_sdram_rodata
	LDR R1, =_edram_rodata
	SUBS R1, R1, R0
	LDR R2, =_sidram_rodata
	BL	NAND_Load_Firmware
 
	/*
     * ========================================================
     *  阶段 3：SRAM 段初始化
     * ========================================================
     */
 
	/* 3.1 拷贝 .sram_data（从 ITCM 到 SRAM） */
 
	LDR R0,=_ssram                /* R0 = SRAM 目标地址（VMA） */
	LDR R1,=_esram                 /* R1 = SRAM 结束地址 */
	LDR R2,=_sidram                 /* R2 = ITCM 源地址（LMA） */
	
	CMP R0,R1
	BEQ sram_data_copy_done
	
sram_data_copy_loop:
	LDR R3,[R2]                     /* 从 ITCM 读取 4 字节 */
	STR R3,[R0]                     /* 写入 SRAM */
	ADDS R0,R0,#4
	ADDS R2,R2,#4
	CMP R0,R1
	BLT sram_data_copy_loop
 
sram_data_copy_done:
 
 
	/* 3.2 .noinit 段 → 保持原样，不做任何操作 */
    /* 链接器已经分配了空间，启动代码不碰它 */

    /* 3.3 清零 .sram_bss 段 */
 
	LDR R0,=_ssram_bss
	LDR R1,=_esram_bss
	MOVS R2,#0
	
	CMP R0,R1
	BEQ sram_bss_zero_done
	
sram_bss_zero_loop:
	STR R2,[R0]
	ADDS R0,R0,#4
	CMP R0,R1
	BLT sram_bss_zero_loop
 
sram_bss_zero_done:
 
	 /*
     * ========================================================
     *  阶段 4：跳转到 main
     * ========================================================
     */
 
    /* 4.1 调用 __main 初始化 C 库，__main 会调用 main() */
	BL     __main    /* ← 使用 BL 而不是 BX */
	
 
	/* 不应该到达这里 */
    B      .
 
/*
 * ============================================================
 *  默认中断处理函数（弱符号）
 * ============================================================
 */
.section .text.Default_Handler
.thumb_func
.weak Default_Handler

Default_Handler:
    B      .

/* 弱符号中断定义 */
.weak NMI_Handler
.weak HardFault_Handler
.weak MemManage_Handler
.weak BusFault_Handler
.weak UsageFault_Handler
.weak SVC_Handler
.weak DebugMon_Handler
.weak PendSV_Handler
.weak SysTick_Handler

/*.thumb_set 是 ARM 汇编中用于为 Thumb 函数创建别名的指令*/
/*效果： 当调用 NMI_Handler 时，实际执行的是 Default_Handler*/
.thumb_set NMI_Handler, Default_Handler
.thumb_set HardFault_Handler, Default_Handler
.thumb_set MemManage_Handler, Default_Handler
.thumb_set BusFault_Handler, Default_Handler
.thumb_set UsageFault_Handler, Default_Handler
.thumb_set SVC_Handler, Default_Handler
.thumb_set DebugMon_Handler, Default_Handler
.thumb_set PendSV_Handler, Default_Handler
.thumb_set SysTick_Handler, Default_Handler

/* 外设中断弱符号 */
.weak NVME_IRQHandler
.weak NAND_IRQHandler
.weak DMA_IRQHandler
.weak FTL_IRQHandler

.thumb_set NVME_IRQHandler, Default_Handler
.thumb_set NAND_IRQHandler, Default_Handler
.thumb_set DMA_IRQHandler, Default_Handler
.thumb_set FTL_IRQHandler, Default_Handler

/*
 * ============================================================
 *  启动文件结束 (.end GNU中非必须)
 * ============================================================
 */
 
 .end
 