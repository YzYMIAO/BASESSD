/*
 * rng.c — 跨 target 共享的 RNG 服务层
 *
 * 只放与硬件无关的部分：就绪重试策略与范围映射；RNG 外设的 HAL 初始化、
 * 时钟源、就绪标志、取数全在 src/platform/<soc>/rng_port.c。
 */

#include "rng.h"

#include "common.h"
#include "delay.h"

func_status_e rng_init(void)
{
    WORD retry = 0;

    rng_port_init();
    while ((rng_port_data_ready() == 0U) && (retry < 10000U)) /* 等待RNG准备就绪 */
    {
        retry++;
        delay_us(10);
    }
    if (retry >= 10000U)
    {
        return FUNC_STATUS_ERROR; /* 随机数产生器工作不正常 */
    }

    return FUNC_STATUS_SUCCESS;
}

DWORD rng_get_random_num(void)
{
    return rng_port_get_random();
}

int rng_get_random_range(int min, int max)
{
    DWORD randomnum;

    randomnum = rng_get_random_num();

    return randomnum % (max - min + 1) + min;
}
