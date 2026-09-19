/**
 ****************************************************************************************************
 * @file        exti.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2024-05-21
 * @brief       外部中断驱动代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 H7R3开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#ifndef __EXTI_H
#define __EXTI_H

#include "sys.h"

/* 引脚、中断编号和中断服务函数定义 */
#define WKUP_INT_GPIO_PORT GPIOC
#define WKUP_INT_GPIO_PIN  GPIO_PIN_13
#define WKUP_INT_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOC_CLK_ENABLE(); \
    } while (0)
#define WKUP_INT_IRQn EXTI13_IRQn
// #define WKUP_INT_IRQHandler         EXTI13_IRQHandler
#define KEY0_INT_GPIO_PORT GPIOE
#define KEY0_INT_GPIO_PIN  GPIO_PIN_9
#define KEY0_INT_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOE_CLK_ENABLE(); \
    } while (0)
#define KEY0_INT_IRQn EXTI9_IRQn
// #define KEY0_INT_IRQHandler         EXTI9_IRQHandler
#define KEY1_INT_GPIO_PORT GPIOE
#define KEY1_INT_GPIO_PIN  GPIO_PIN_8
#define KEY1_INT_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOE_CLK_ENABLE(); \
    } while (0)
#define KEY1_INT_IRQn EXTI8_IRQn
// #define KEY1_INT_IRQHandler         EXTI8_IRQHandler
#define KEY2_INT_GPIO_PORT GPIOE
#define KEY2_INT_GPIO_PIN  GPIO_PIN_7
#define KEY2_INT_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOE_CLK_ENABLE(); \
    } while (0)
#define KEY2_INT_IRQn EXTI7_IRQn
// #define KEY2_INT_IRQHandler         EXTI7_IRQHandler

/* 函数声明 */
void exti_init(void); /* 初始化外部中断 */

#endif
