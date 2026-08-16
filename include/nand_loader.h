/*
 * ============================================================
 *  nand_loader.h
 *  启动阶段 C 函数声明
 * ============================================================
 */

#ifndef NAND_LOADER_H
#define NAND_LOADER_H

#include <stdint.h>

/* TCM 初始化 */
void TCM_Init(void);

/* NAND 控制器低层初始化 */
void NAND_Init_LowLevel(void);

/* 从 NAND 加载固件到 DRAM */
int NAND_Load_Firmware(uint32_t dst, uint32_t src, uint32_t size);

#endif /* NAND_LOADER_H */