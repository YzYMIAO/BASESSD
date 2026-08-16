/*
 * ============================================================
 *  SSD 主控链接脚本
 *  支持：ITCM、DTCM、SRAM、DRAM、多通道 NAND Flash
 *  适用：SSD 主控芯片（NVMe/SATA 协议）
 *  工具链：arm-none-eabi-gcc (GNU)
 * ============================================================
 */
 
 
 /*遵循一个规范：从 CPU 就近访问速度最快的内存（如 ITCM）开始，
 * 依次向速度较慢的外部存储（如 NAND）排列。
 */
 
 ENTRY(Reset_Handler)
 
 /*内存分布*/
 MEMORY
 {
	 /*
     * ----------------------------------------------------------
     *  ITCM（指令紧耦合内存）
     *  存放：Bootloader、启动代码、中断向量表
     *  容量：16KB ~ 64KB
     *  说明：上电后由硬件 ROM 加载 Bootloader 到此区域
     * ----------------------------------------------------------
     */
	 ITCM (rwx) : ORIGIN = 0x00000000, LENGTH = 16K
	/*
     * ----------------------------------------------------------
     *  DTCM（数据紧耦合内存）
     *  存放：关键数据结构、中断上下文、系统状态
     *  容量：16KB ~ 64KB
     * ----------------------------------------------------------
     */
	 DTCM (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
	  /*
     * ----------------------------------------------------------
     *  内部 SRAM
     *  存放：通用数据、小容量临时缓冲
     *  容量：64KB ~ 512KB
     * ----------------------------------------------------------
     */
	 SRAM (rwx) : ORIGIN = 0x30000000, LENGTH = 128K
	 /*
     * ----------------------------------------------------------
     *  外部 DDR DRAM（核心工作内存）
     *  容量：256MB ~ 4GB
     *  存放：
     *    1. 完整的 FTL 映射表（最关键！）
     *    2. 主机读写数据缓冲区
     *    3. 主固件代码（从 NAND 加载到此执行）
     *    4. 堆、栈
     * ----------------------------------------------------------
     */
	 DRAM (rwx) : ORIGIN = 0x40000000, LENGTH = 256M
	 
	 /*
     * ----------------------------------------------------------
     *  NAND Flash 通道0（固件存储区）
     *  存放：主固件镜像、系统配置
     *  说明：地址为映射后的访问地址，具体取决于主控设计
     * ----------------------------------------------------------
     */
    NAND_FW (rx) : ORIGIN = 0x80000000, LENGTH = 64M
 }
 
 /*段定义*/
 /*最终决定内存使用顺序的是 SECTIONS 里的 > Region，而不是 MEMORY 里定义的先后顺序。*/
 SECTIONS
 {
	/*
	 * ========================================================
     *  1. ITCM 段（启动和中断关键代码）
     *     上电后由 ROM Bootloader 加载
     * ========================================================
     */
	 
	.itcm_vector : 
	{
		. = ALIGN(4);                /*4字节对齐*/
		KEEP(*(.itcm_vector))        /*强制保留中断向量表*/
		. = ALIGN(4);
	} > ITCM
	
	/*其执行顺序从上到下，在ITCM中已经捕获的代码不会被后面捕获*/
	.itcm_text :
	{
		. = ALIGN(4);
		*(.itcm_text)                 /*Bootloader代码*/
		*(.text.Reset_Handler)        /*复位处理*/
		*(.text.NAND_load)            /*NAND 加载函数*/
		*(.text.NVMe_IRQHandler)      /*NVMe 中断(关键路径)*/
		*(.text.FTL_IRQHandler)       /*FTL 中断*/
		*(.text.critical)             /* 关键函数 */
		. = ALIGN(4);
		_eitcm = .; 
	} > ITCM
 
	 /*
     * ========================================================
     *  2. DTCM 段（关键数据）
     * ========================================================
     */
	 
	 .dtcm_data (NOLOAD) :       /*NOLOAD没有加载域其存在条件是未初始化变量*/
    {
        . = ALIGN(4);
        _sdtcm = .;
        *(.dtcm_data)                /* 高频访问变量 */
        *(.dtcm_data.*)
        . = ALIGN(4);
        _edtcm = .;
    } > DTCM
	 

	.sram_data : 
	{
		. = ALIGN(4);
		_ssram = .;                 /*.sram_data在SRAM中的起始地址*/
		*(.sram_data)
		*(.data)					/*所有.data的基础段*/
		*(.data.*)                  /*所有.data的子段(两个都必须写，才能覆盖全部情况。)*/
		. = ALIGN(4);
		_esram = .;                /*.sram_data在SRAM中的结束地址*/
		_sisram = LOADADDR(.sram_data); /*获取.sram_data段在flash中的加载域地址*/
	} > SRAM AT > ITCM              /* LMA 在 ITCM（随 Bootloader 一起加载） */
	
	.noinit (NOLOAD) :            /*复位需要保存的数据，放在在启动阶段被.bss清零*/
	{
		. = ALIGN(4);
		_snoinit = .;              /* NoInit 段起始地址 */
		*(.noinit)                 /* 所有 .noinit 段 */
		*(.noinit.*)               /* 子段 */
		. = ALIGN(4);
		_enoinit = .;              /* NoInit 段结束地址 */
	} > SRAM
	
	.sram_bss : 
	{
		. = ALIGN(4);
		_ssram_bss = .;
		*(.sram_bss)
		*(.bss)
		*(.bss.*)
		*(COMMON)                /*临时段存放未初始化的暂定义全局变量*/
		. = ALIGN(4);
		_esram_bss = .;
	} > SRAM
	
	/*
     * ========================================================
     *  4. DRAM 段（主固件和数据）
     *     从 NAND Flash 加载到此
     * ========================================================
     */
	 
	 
	 .dram_text :
	 {
		. = ALIGN(4);
		_sdram_text = .;      /*DRAM 中的固件起始地址*/
		*(.dram_text)         /*主固件代码*/
		*(.text.app)           /*应用程序代码*/
		*(.text.ftl)           /*FTL核心算法*/
      	*(.text.nand)          /*NAND 驱动*/
		*(.text)               /*代码段*/
		*(.text.*)             /*代码段的之前未被捕获的所有子段*/
		. = ALIGN(4);
		_edram_text = .;
		_sidram = LOADADDR(.dram_text);
	 } > DRAM AT > NAND_FW       /*VMA 在 DRAM，LMA 在 NAND flash*/
	
	.dram_rodata : 
	{
		. = ALIGN(4);
		*(.dram_rodata)
		*(.rodata)
		*(.rodata.*)
		. = ALIGN(4);
		_edram_rodata = .;
	} > DRAM AT >NAND_FW
	
	
	 /*
     * ========================================================
     *  5. DRAM 中的 FTL 映射表区（SSD 核心）
     *     使用 NOLOAD，不占用 NAND 空间
     *     大小 = SSD 容量 / 最小映射单元
     *     例如 1TB SSD，4KB 映射单元 → 需要 256MB
     * ========================================================
     */
	
	.ftl_table (NOLOAD) :
	{
		. = ALIGN(4);
		_sftl = .;         /*FTL 表其起始地址*/
		*(.ftl_table)       /*FTL映射表*/
		*(.ftl_table.*)
		. = ALIGN(4);
		_eftl = .;
	} > DRAM
	
	/*
     * ========================================================
     *  6. DRAM 中的主机数据缓冲区
     *     用于缓存主机读写数据
     * ========================================================
     */
	
	.host_buffer (NOLOAD) :
	{
		. = ALIGN(4);
		_shost_buf = .;
		*(.host_buffer)   /*主机数据缓冲区*/
		*(.host_buffer.*)
		. = ALIGN(4);
		_ehost_buf = .;
	} > DRAM
	
	/*
     * ========================================================
     *  7. DRAM 中的堆
     * ========================================================
     */
	
	.heap : 
	{
		. = ALIGN(8);
		_sheap = .;
		. += 256K;
		_eheap = .;
		. = ALIGN(8);
	} > DRAM
	
	
	 /*
     * ========================================================
     *  8. 栈（放在 DTCM 或 DRAM）
     *     建议放在 DTCM 中，提高函数调用速度
     * ========================================================
     */
	
	.stack : 
	{
		. = ALIGN(8);
		. += 16k;
		PROVIDE(_estack = .);  /*如果该符号已被其他目标文件定义，*/
		                       /*则 PROVIDE 不生效；否则，由链接脚本定义该符号。*/
		. = ALIGN(8);
	
	} > DRAM
	
	/*
     * ========================================================
     *  9. 为 C 运行时库提供标准符号
	 *  crt0.o 只强制要求下面的标准符号
     * ========================================================
     */
	 
	PROVIDE(__bss_start__ = _ssram_bss);
	PROVIDE(__bss_end__ = _esram_bss);
	PROVIDE(end = _eheap);
	PROVIDE(__end__ = _eheap);
	PROVIDE(_snand_fw = LOADADDR(.dram_text));
	
	/*
     * ========================================================
     *  10. 丢弃调试段
     * ========================================================
     */
	/DISCARD/ : 
	{
		*(.comment)            /*编译器版本、时间等元信息*/
		*(.debug_*)            /*DWARF 调试信息*/
		*(.ARM.attributes)     /*ARM 架构版本、ABI 版本、浮点 ABI 等*/
		*(.stab*)              /*老式 Stabs 调试符号表*/
		*(.eh_frame*)          /*C++ 异常处理的栈展开信息*/
	}
	
	
 }
 
/*
 * ============================================================
 *  溢出断言（完整修正版）
 * ============================================================
 */

/*
 * ============================================================
 *  溢出断言
 * ============================================================
 */

/* --- 1. ITCM 检查 --- */
ASSERT(ORIGIN(ITCM) + LENGTH(ITCM) >= _eitcm,
       "ITCM overflowed!");

/* --- 2. DTCM 检查 --- */
ASSERT(ORIGIN(DTCM) + LENGTH(DTCM) >= _edtcm,
       "DTCM overflowed!");

/* --- 3. SRAM 检查 --- */
ASSERT(ORIGIN(SRAM) + LENGTH(SRAM) >= _esram_bss,
       "SRAM overflowed!");

/* --- 4. DRAM 检查 --- */
ASSERT(ORIGIN(DRAM) + LENGTH(DRAM) >= _estack,
       "DRAM overflowed!");

/* --- 5. NAND 固件大小检查 --- */
ASSERT(SIZEOF(.dram_text) + SIZEOF(.dram_rodata) <= 64M,
       "Firmware size exceeds NAND_FW size!");