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
 *
 * 使用独立的 .ssd_code_* 段名，避免被通用 .text* 通配符提前捕获。
 * 只有两个主链接脚本实际定义了对应输出段；新增段属性宏前必须同步脚本，
 * 否则段会变成 orphan section 被 ld 启发式放置。
 */
#define SSD_CODE_HIGH __attribute__((section(".ssd_code_high")))
#define SSD_CODE_FAST __attribute__((section(".ssd_code_fast")))

/* DTCM 数据段（最快，高频变量） */
#define DTCM_DATA __attribute__((section(".dtcm_data")))

/* D2/D3 SRAM 的显式放置段（NOLOAD，启动不清零；使用者必须显式初始化） */
#define SRAM12_DATA __attribute__((section(".sram12_data"))) /* SRAM1+SRAM2 连续 256KB */
#define SRAM3_DATA  __attribute__((section(".sram3_data")))  /* SRAM3 32KB */
#define SRAM4_DATA  __attribute__((section(".sram4_data")))  /* SRAM4 64KB */

/* AXI SRAM 非缓存缓冲段：FMC 数据面按字节访问，必须绕过 D-Cache（H17） */
#define NONCACHEABLE_BUFFER __attribute__((section(".noncacheable_buffer")))

/* 供启动代码使用的段边界符号（stm32h743xx_hot.ld 定义）。 */
extern unsigned char __ssd_code_high_load_start[];
extern unsigned char __ssd_code_high_start[];
extern unsigned char __ssd_code_high_end[];

#endif /* SSD_COMPILER_H */
