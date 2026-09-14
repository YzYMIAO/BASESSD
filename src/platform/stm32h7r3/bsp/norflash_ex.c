

#include "norflash_ex.h"
#include "norflash.h"

/**
 * @brief   开启NOR Flash内存映射
 * @param   无
 * @retval  开启内存映射结果
 * @arg     0: 开启内存映射成功
 * @arg     1: 开启内存映射失败
 */
static uint8_t norflash_ex_enter_mmap(void)
{
    norflash_init();
    if (norflash_memory_mapped() != 0)
    {
        return 1;
    }
    
    __enable_irq();
    
    return 0;
}

/**
 * @brief   退出NOR Flash内存映射
 * @param   无
 * @retval  退出内存映射结果
 * @arg     0: 退出内存映射成功
 * @arg     1: 退出内存映射失败
 */
static uint8_t norflash_ex_exit_mmap(void)
{
    __disable_irq();
    SCB_InvalidateICache();
    SCB_InvalidateDCache();
    if (norflash_init() == NORFlash_Unknow)
    {
        return 1;
    }
    
    return 0;
}

/**
 * @brief   初始化NOR Flash
 * @param   无
 * @retval  初始化结果
 * @arg     0: 初始化成功
 * @arg     1: 初始化失败
 */
uint8_t norflash_ex_init(void)
{
    if (norflash_init() == NORFlash_Unknow)
    {
        return 1;
    }
    
    if (norflash_memory_mapped() != 0)
    {
        return 1;
    }
    
    return 0;
}

/**
 * @brief   写NOR Flash
 * @param   address: 地址
 * @param   data: 数据缓冲区指针
 * @param   length: 数据长度
 * @retval  写结果
 * @arg     0: 写成功
 * @arg     1: 写失败
 */
uint8_t norflash_ex_write(uint32_t address, uint8_t *data, uint32_t length)
{
    uint8_t res = 0;
    
    res = norflash_ex_exit_mmap();
    
    if (res == 0)
    {
        res = norflash_write(address, data, length);
    }
    
    norflash_ex_enter_mmap();
    
    return res;
}

/**
 * @brief   读NOR Flash
 * @param   address: 地址
 * @param   data: 数据缓冲区指针
 * @param   length: 数据长度
 * @retval  读结果
 * @arg     0: 读成功
 * @arg     1: 读失败
 */
uint8_t norflash_ex_read(uint32_t address, uint8_t *data, uint32_t length)
{
    uint8_t res;
    
    __disable_irq();
    res = norflash_read(address, data, length);
    __enable_irq();
    
    return res;
}

/**
 * @brief   扇区擦除NOR Flash
 * @param   address: 扇区地址
 * @retval  扇区擦除结果
 * @arg     0: 扇区擦除成功
 * @arg     1: 扇区擦除失败
 */
uint8_t norflash_ex_erase_sector(uint32_t address)
{
    uint8_t res = 0;
    
    res = norflash_ex_exit_mmap();
    
    if (res == 0)
    {
        res = norflash_erase_sector(address);
    }
    
    norflash_ex_enter_mmap();
    
    return res;
}
