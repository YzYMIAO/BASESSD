#ifndef NAND_DEVICE_MT29F_G08A_H
#define NAND_DEVICE_MT29F_G08A_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * mt29f___g08a.h — MT29FxxxG08A 族器件事实（几何 / 拓扑 / 地址位宽 / FMC / ID）
 *
 * 选择方式：makefile 传 -DNAND_DEVICE_MT29F___G08A（默认），
 *           nand/nand_config.h 按该宏包含本头。
 *
 * ⚠ 平台纯净：本头只允许 <stdint.h>，不得 include sys_port.h / HAL。
 *   nand_config.h 会被 FTL 层的头文件包含，FTL 层对 CMSIS/HAL 依赖必须保持 0。
 *   R/B# 的 GPIO 接线属板级信息，放在平台侧 nand/nand_fmc.h。
 *
 * 本头只描述**当前封装**（MT29F4G08ABADAWP：2 plane × 2048 block × 64 page ×
 * 2112B）。同族换容量时改这里的几何/位宽宏；换族时另建器件头并在
 * nand_config.h 加选择分支。
 */

/* ── 逻辑几何（FC/FTL 视角）──
 * PAGE_SIZE = 主区 2048 + 备区 64。 */
#define PAGE_SIZE        (2112)
#define PAGE_MAIN_SIZE   (2048)
#define PAGE_SPARE_SIZE  (64)
#define PAGES_PER_BLOCK  (64)
#define BLOCKS_PER_PLANE (2048)
#define PLANE_NUM        (2)
/* 一条 multi-plane 命令最多覆盖几个 plane（Micron datasheet 的 Two-Plane Command Set；
 * ONFI 1.0 没有 multi-plane，不能拿它当出处）。FTL cache batch 假设一条命令覆盖
 * PLANE_NUM 个 plane，编译期在 ftl_config.h 校验 PLANE_NUM <= 本宏。 */
#define NAND_MAX_MULTI_PLANE (2U)

/* 器件 LUN 容量：MT29F4G08 的 ONFI 参数页 logical_units = 1。
 *
 * 这是**器件属性**（不是板级固定值），用于推导 LUN_ADDR_WIDTH 与数组上界；
 * 运行时实际 LUN 数由 ONFI 参数页读取、并逐 LUN 用 78h 探测确认。
 * 换多 LUN 器件时改此宏（同时改厂商数据手册对应的容量/位宽）。 */
#define LUN_NUM (1)

/* ── 地址位宽（器件手册的 BA/PA 划分才是权威，不从 ceil-log2 推）── */
#define PAGE_ADDR_WIDTH  (6U)  /* PA0~PA5：每 Block 64 页 */
#define PLANE_ADDR_WIDTH (1U)  /* BA6：Plane 选择 */
#define BLOCK_ADDR_WIDTH (11U) /* BA7~BA17：每 Plane 2048 Block */

/*
 * 器件行地址编码（MT29F 的 PA/BA 规则）：
 *   page -> bit[5:0]，plane -> bit[6]，block -> bit[7:17]，lun -> bit[18..]。
 * 不写 6/7/18 字面量：位宽宏改一处，编码跟着变（H4/H39）。
 * LUN 位是本仓的连续行地址布局；具体器件以数据手册为准。当前 LUN_ADDR_WIDTH==0，
 * 展开后仍为 3 个行周期。
 */
#define NAND_PAA_TO_ROW(paa) (paa_to_page(paa) | ((DWORD)(paa_to_plane(paa)) << PAGE_ADDR_WIDTH) | ((DWORD)(paa_to_block(paa)) << (PAGE_ADDR_WIDTH + PLANE_ADDR_WIDTH)) | ((DWORD)(paa_to_lun(paa)) << (PAGE_ADDR_WIDTH + PLANE_ADDR_WIDTH + BLOCK_ADDR_WIDTH)))

/*
 * FMC NAND Bank3 内存窗口（纯整数；仅 nand_fmc.c 写命令/地址/数据周期）。
 *
 * 窗口别名机制：对 FMC Bank 内存映射地址写，A16=1 选命令口、A17=1 选地址口、
 * A16/A17 均 0 为数据口。BASE + CMD_OFFSET / ADDR_OFFSET 即三个窗口。
 */
#define NAND_FMC_BASE_ADDR   0x80000000UL /* FMC Bank3 起始 */
#define NAND_FMC_CMD_OFFSET  (1UL << 16)  /* A16=1 → 命令窗口 */
#define NAND_FMC_ADDR_OFFSET (1UL << 17)  /* A17=1 → 地址窗口 */

/* Read ID 返回 2C DC 90 95 56；比较时跳过厂商字节 id[0]=0x2C，用 id[1..4] 拼 4 字节。 */
#define NAND_CHIP_ID 0xDC909556UL

/*
 * ONFI 时序模式控制
 *
 * 0 = 低速：启动和运行期都保持 Mode 0。
 * 4 = Mode 4：3.3V 实验档；FMC AC 参数仍用 Table 29/31（3.3V），
 *     只把 SET_FEATURES 的模式值改成 4，用于和 Mode 5 同配置对比。
 * 5 = Mode 5：正式配置；AC 参数同样用 Table 29/31（3.3V）。
 *
 * 启动全程低速（fc_nand_init + FTL boot/恢复），全部初始化完成后由
 * fc_nand_enable_high_speed() 切到 CONFIG_NAND_TIMING_MODE 并重配 FMC 时序。
 */
#ifndef CONFIG_NAND_TIMING_MODE
/* 3.3V 正式配置。Mode 5 运行期写后读回 UNC 的问题仍在查；
 * 对照实验可改 0（低速）或 4（Mode 4），但改完必须重新上板验证，不得默认切换。 */
#    define CONFIG_NAND_TIMING_MODE (5U)
#endif

#if (CONFIG_NAND_TIMING_MODE != 0U) && (CONFIG_NAND_TIMING_MODE != 4U) && (CONFIG_NAND_TIMING_MODE != 5U)
#    error "CONFIG_NAND_TIMING_MODE must be 0, 4, or 5"
#endif

/*
 * 初始化/高速切换入口在 FC 层：
 *   fc_nand_init() / fc_nand_enable_high_speed()（include/fc/fc_nand_engine.h）。
 * 本头只保留器件事实；timing-mode 观测经 nand_device.h 的通用接口。
 */

#endif /* NAND_DEVICE_MT29F_G08A_H */
