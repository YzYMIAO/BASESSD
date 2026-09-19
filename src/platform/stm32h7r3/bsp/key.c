/**
 ****************************************************************************************************
 * @file        key.c
 * @author      ÕýµãÔ­×ÓÍÅ¶Ó(ALIENTEK)
 * @version     V1.0
 * @date        2024-05-21
 * @brief       °´¼üÇý¶¯´úÂë
 * @license     Copyright (c) 2020-2032, ¹ãÖÝÊÐÐÇÒíµç×Ó¿Æ¼¼ÓÐÏÞ¹«Ë¾
 ****************************************************************************************************
 * @attention
 *
 * ÊµÑéÆ½Ì¨:ÕýµãÔ­×Ó H7R3¿ª·¢°å
 * ÔÚÏßÊÓÆµ:www.yuanzige.com
 * ¼¼ÊõÂÛÌ³:www.openedv.com
 * ¹«Ë¾ÍøÖ·:www.alientek.com
 * ¹ºÂòµØÖ·:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#include "key.h"

#include "delay.h"

/**
 * @brief   ³õÊ¼»¯°´¼ü
 * @param   ÎÞ
 * @retval  ÎÞ
 */
void key_init(void)
{
    GPIO_InitTypeDef gpio_init_struct = {0};

    /* Ê¹ÄÜGPIO¶Ë¿ÚÊ±ÖÓ */
    WKUP_GPIO_CLK_ENABLE();
    KEY0_GPIO_CLK_ENABLE();
    KEY1_GPIO_CLK_ENABLE();
    KEY2_GPIO_CLK_ENABLE();

    /* ÅäÖÃWKUP¿ØÖÆÒý½Å */
    gpio_init_struct.Pin   = WKUP_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_INPUT;
    gpio_init_struct.Pull  = GPIO_PULLDOWN;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(WKUP_GPIO_PORT, &gpio_init_struct);

    /* ÅäÖÃKEY0¿ØÖÆÒý½Å */
    gpio_init_struct.Pin   = KEY0_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_INPUT;
    gpio_init_struct.Pull  = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(KEY0_GPIO_PORT, &gpio_init_struct);

    /* ÅäÖÃKEY1¿ØÖÆÒý½Å */
    gpio_init_struct.Pin   = KEY1_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_INPUT;
    gpio_init_struct.Pull  = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(KEY1_GPIO_PORT, &gpio_init_struct);

    /* ÅäÖÃKEY2¿ØÖÆÒý½Å */
    gpio_init_struct.Pin   = KEY2_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_INPUT;
    gpio_init_struct.Pull  = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(KEY2_GPIO_PORT, &gpio_init_struct);
}

/**
 * @brief   É¨Ãè°´¼ü
 * @note    °´¼üÏìÓ¦¾ßÓÐÓÅÏÈ¼¶£ºWKUP > KEY0 > KEY1 > KEY2
 * @param   mode: É¨ÃèÄ£Ê½
 * @arg     0: ²»Ö§³ÖÁ¬Ðø°´
 * @arg     1: Ö§³ÖÁ¬Ðø°´
 * @retval  °´¼ü¼üÖµ
 * @arg     NONE_PRES: Ã»ÓÐ°´¼ü°´ÏÂ
 * @arg     WKUP_PRES: WKUP°´¼ü°´ÏÂ
 * @arg     KEY0_PRES: KEY0°´¼ü°´ÏÂ
 * @arg     KEY1_PRES: KEY1°´¼ü°´ÏÂ
 * @arg     KEY2_PRES: KEY2°´¼ü°´ÏÂ
 */
uint8_t key_scan(uint8_t mode)
{
    static uint8_t key_release = 1;
    uint8_t        key_value   = NONE_PRES;

    if (mode != 0)
    {
        key_release = 1;
    }

    if ((key_release == 1) && ((WKUP == 1) || (KEY0 == 0) || (KEY1 == 0) || (KEY2 == 0)))
    {
        delay_ms(10);
        key_release = 0;

        if (KEY2 == 0)
        {
            key_value = KEY2_PRES;
        }

        if (KEY1 == 0)
        {
            key_value = KEY1_PRES;
        }

        if (KEY0 == 0)
        {
            key_value = KEY0_PRES;
        }

        if (WKUP == 1)
        {
            key_value = WKUP_PRES;
        }
    }
    else if ((WKUP == 0) && (KEY0 == 1) && (KEY1 == 1) && (KEY2 == 1))
    {
        key_release = 1;
    }

    return key_value;
}

void key_intup_test(void)
{
    while (1)
    {
        uint8_t key = key_scan(0); /* æ«ææé® */

        switch (key)
        {
            case WKUP_PRES: /* WKUPæé®è¢«æä¸ */
            {
                LED0(0); /* å¼å¯LED0 */
                LED1(0); /* å¼å¯LED1 */
                break;
            }
            case KEY0_PRES: /* KEY0æé®è¢«æä¸ */
            {
                LED0_TOGGLE(); /* ç¿»è½¬LED0ç¶æ */
                break;
            }
            case KEY1_PRES: /* KEY1æé®è¢«æä¸ */
            {
                LED1_TOGGLE(); /* ç¿»è½¬LED1ç¶æ */
                break;
            }
            case KEY2_PRES: /* KEY2æé®è¢«æä¸ */
            {
                LED0_TOGGLE(); /* ç¿»è½¬LED1ç¶æ */
                LED1_TOGGLE(); /* ç¿»è½¬LED1ç¶æ */
                break;
            }
        }

        delay_ms(10);
    }
}
