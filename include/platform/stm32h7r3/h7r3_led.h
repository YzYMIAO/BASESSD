#ifndef SSD_PLATFORM_STM32H7R3_LED_H
#define SSD_PLATFORM_STM32H7R3_LED_H

#include <stdbool.h>

/* 正点原子 STM32H7R3 开发板：LED0=PD14，LED1=PC0，均为低电平点亮。 */
typedef enum
{
    H7R3_LED_0 = 0,
    H7R3_LED_1
} h7r3_led_t;

void h7r3_led_init(void);
void h7r3_led_set(h7r3_led_t led, bool on);

#endif /* SSD_PLATFORM_STM32H7R3_LED_H */
