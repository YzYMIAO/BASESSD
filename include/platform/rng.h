#ifndef PLATFORM_RNG_H
#define PLATFORM_RNG_H

#include <stdint.h>

/*
 * 平台 RNG 抽象接口（共享逻辑在 src/platform/rng.c，SoC 适配在
 * src/platform/<soc>/rng_port.c）。
 *
 * 本头零 HAL 类型：RNG 句柄、时钟源、就绪标志一律留在 SoC 侧
 * （include/platform/<soc>/rng_port.h）。
 */

/* 初始化 RNG；返回 0 = 成功（FUNC_STATUS_SUCCESS），非 0 = 失败。 */
uint8_t rng_init(void);

/* 取一个 32 位随机数。 */
uint32_t rng_get_random_num(void);

/* 取 [min, max] 范围内的随机数。 */
int rng_get_random_range(int min, int max);

/* ─────────────────────────── port 接口 ───────────────────────────
 * 由 src/platform/<soc>/rng_port.c 实现。
 */

void     rng_port_init(void);
uint8_t  rng_port_data_ready(void);
uint32_t rng_port_get_random(void);

#endif /* PLATFORM_RNG_H */
