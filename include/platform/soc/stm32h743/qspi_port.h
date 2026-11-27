#ifndef PLATFORM_STM32H743_QSPI_PORT_H
#define PLATFORM_STM32H743_QSPI_PORT_H

/*
 * qspi_port.h — stm32h743 QSPI port 头。
 *
 * 只提供 QSPI 引脚 / 时钟宏（含 HAL 类型）；函数声明与 port 接口在抽象 qspi.h。
 */

#include "sys_port.h"

/* QSPI 相关 引脚 定义 */
#define QSPI_BK1_CLK_GPIO_PORT GPIOB
#define QSPI_BK1_CLK_GPIO_PIN  GPIO_PIN_2
#define QSPI_BK1_CLK_GPIO_AF   GPIO_AF9_QUADSPI
#define QSPI_BK1_CLK_GPIO_CLK_ENABLE() \
    do                                 \
    {                                  \
        __HAL_RCC_GPIOB_CLK_ENABLE();  \
    } while (0) /* PB口时钟使能 */

#define QSPI_BK1_NCS_GPIO_PORT GPIOB
#define QSPI_BK1_NCS_GPIO_PIN  GPIO_PIN_6
#define QSPI_BK1_NCS_GPIO_AF   GPIO_AF10_QUADSPI
#define QSPI_BK1_NCS_GPIO_CLK_ENABLE() \
    do                                 \
    {                                  \
        __HAL_RCC_GPIOB_CLK_ENABLE();  \
    } while (0) /* PB口时钟使能 */

#define QSPI_BK1_IO0_GPIO_PORT GPIOF
#define QSPI_BK1_IO0_GPIO_PIN  GPIO_PIN_6
#define QSPI_BK1_IO0_GPIO_AF   GPIO_AF9_QUADSPI
#define QSPI_BK1_IO0_GPIO_CLK_ENABLE() \
    do                                 \
    {                                  \
        __HAL_RCC_GPIOF_CLK_ENABLE();  \
    } while (0) /* PF口时钟使能 */

#define QSPI_BK1_IO1_GPIO_PORT GPIOF
#define QSPI_BK1_IO1_GPIO_PIN  GPIO_PIN_7
#define QSPI_BK1_IO1_GPIO_AF   GPIO_AF9_QUADSPI
#define QSPI_BK1_IO1_GPIO_CLK_ENABLE() \
    do                                 \
    {                                  \
        __HAL_RCC_GPIOF_CLK_ENABLE();  \
    } while (0) /* PF口时钟使能 */

#define QSPI_BK1_IO2_GPIO_PORT GPIOF
#define QSPI_BK1_IO2_GPIO_PIN  GPIO_PIN_8
#define QSPI_BK1_IO2_GPIO_AF   GPIO_AF10_QUADSPI
#define QSPI_BK1_IO2_GPIO_CLK_ENABLE() \
    do                                 \
    {                                  \
        __HAL_RCC_GPIOF_CLK_ENABLE();  \
    } while (0) /* PF口时钟使能 */

#define QSPI_BK1_IO3_GPIO_PORT GPIOF
#define QSPI_BK1_IO3_GPIO_PIN  GPIO_PIN_9
#define QSPI_BK1_IO3_GPIO_AF   GPIO_AF10_QUADSPI
#define QSPI_BK1_IO3_GPIO_CLK_ENABLE() \
    do                                 \
    {                                  \
        __HAL_RCC_GPIOF_CLK_ENABLE();  \
    } while (0) /* PF口时钟使能 */

#endif /* PLATFORM_STM32H743_QSPI_PORT_H */
