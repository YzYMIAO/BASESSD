

#ifndef __NORFLASH_EX_H
#define __NORFLASH_EX_H

#include "sys.h"

/* 函数声明 */
uint8_t norflash_ex_init(void);                                                 /* 初始化NOR Flash */
uint8_t norflash_ex_write(uint32_t address, uint8_t *data, uint32_t length);    /* 写NOR Flash */
uint8_t norflash_ex_read(uint32_t address, uint8_t *data, uint32_t length);     /* 读NOR Flash */
uint8_t norflash_ex_erase_sector(uint32_t address);                             /* 扇区擦除NOR Flash */

#endif /* __NORFLASH_EX_H */
