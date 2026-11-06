/*
 * ============================================================
 *  SSD 主控编译器辅助宏
 * ============================================================
 */

#ifndef SSD_COMPILER_H
#define SSD_COMPILER_H

/* ============================================================
 *  段属性宏
 * ============================================================ */

/*
 * 代码放置策略：
 *   SSD_CODE_HIGH -> ITCM，启动时由启动代码从镜像复制后执行
 *   SSD_CODE_FAST -> 内部 Flash，适合常用但不要求 TCM 延迟的代码
 *   SSD_CODE_SLOW -> 外部 HyperRAM，必须在 hyperram_init() 后显式加载
 *
 * 使用独立的 .ssd_code_* 段名，避免被通用 .text* 通配符提前捕获。
 */
#define SSD_CODE_HIGH __attribute__((section(".ssd_code_high")))
#define SSD_CODE_FAST __attribute__((section(".ssd_code_fast")))
#define SSD_CODE_SLOW __attribute__((section(".ssd_code_slow")))

/* 兼容已有命名。 */
#define ITCM_CODE SSD_CODE_HIGH

/* DTCM 数据段（最快，高频变量） */
#define DTCM_DATA __attribute__((section(".dtcm_data")))

/* SRAM 数据段（通用变量） */
#define SRAM_DATA __attribute__((section(".sram_data")))

/* D2/D3 SRAM 的显式放置段（NOLOAD，启动不清零；使用者必须显式初始化） */
#define SRAM12_DATA __attribute__((section(".sram12_data"))) /* SRAM1+SRAM2 连续 256KB */
#define SRAM3_DATA  __attribute__((section(".sram3_data")))  /* SRAM3 32KB */
#define SRAM4_DATA  __attribute__((section(".sram4_data")))  /* SRAM4 64KB */

/* 掉电保持数据（复位不清零） */
#define NOINIT __attribute__((section(".noinit")))

/* DRAM 代码段（主固件） */
#define DRAM_CODE __attribute__((section(".dram_text")))

/* DRAM 只读数据段 */
#define DRAM_RODATA __attribute__((section(".dram_rodata")))

/* FTL 映射表（DRAM 中） */
#define FTL_TABLE __attribute__((section(".ftl_table")))

/* 主机缓冲区（DRAM 中） */
#define HOST_BUFFER __attribute__((section(".host_buffer")))

/* AXI SRAM 非缓存缓冲段：FMC 数据面按字节访问，必须绕过 D-Cache（H17） */
#define NONCACHEABLE_BUFFER __attribute__((section(".noncacheable_buffer")))

/* ============================================================
 *  其他宏
 * ============================================================ */

/* 对齐 */
#define ALIGN(n) __attribute__((aligned(n)))

/* 强制内联 */
#define ALWAYS_INLINE __attribute__((always_inline)) static inline

/* 强制保留 */
#define USED __attribute__((used))

/* 组合属性，适合需要固定放置且不能被垃圾回收的入口函数。 */
#define SSD_CODE_HIGH_USED SSD_CODE_HIGH USED
#define SSD_CODE_FAST_USED SSD_CODE_FAST USED
#define SSD_CODE_SLOW_USED SSD_CODE_SLOW USED

/* 供启动/外部内存加载器使用的段边界符号。 */
extern unsigned char __ssd_code_high_load_start[];
extern unsigned char __ssd_code_high_start[];
extern unsigned char __ssd_code_high_end[];
extern unsigned char __ssd_code_slow_load_start[];
extern unsigned char __ssd_code_slow_start[];
extern unsigned char __ssd_code_slow_end[];

#endif /* SSD_COMPILER_H */
