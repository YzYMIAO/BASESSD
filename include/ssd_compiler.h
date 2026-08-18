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

/* ITCM 代码段（最快，关键代码） */
#define ITCM_CODE __attribute__((section(".itcm_text")))

/* DTCM 数据段（最快，高频变量） */
#define DTCM_DATA __attribute__((section(".dtcm_data")))

/* SRAM 数据段（通用变量） */
#define SRAM_DATA __attribute__((section(".sram_data")))

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

/* ============================================================
 *  其他宏
 * ============================================================ */

/* 对齐 */
#define ALIGN(n) __attribute__((aligned(n)))

/* 强制内联 */
#define ALWAYS_INLINE __attribute__((always_inline)) static inline

/* 强制保留 */
#define USED __attribute__((used))

#endif /* SSD_COMPILER_H */