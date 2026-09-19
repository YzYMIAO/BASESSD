#ifndef PLATFORM_STM32H743_LED_H
#define PLATFORM_STM32H743_LED_H

#include "sys.h"

/* Apollo V2: LED0 (red) is PB1 and LED1 (green) is PB0; both are active low. */
#define LED0_GPIO_PORT GPIOB
#define LED0_GPIO_PIN  GPIO_PIN_1
#define LED1_GPIO_PORT GPIOB
#define LED1_GPIO_PIN  GPIO_PIN_0

#define LED0(value)   HAL_GPIO_WritePin(LED0_GPIO_PORT, LED0_GPIO_PIN, (value) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define LED1(value)   HAL_GPIO_WritePin(LED1_GPIO_PORT, LED1_GPIO_PIN, (value) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define LED0_TOGGLE() HAL_GPIO_TogglePin(LED0_GPIO_PORT, LED0_GPIO_PIN)
#define LED1_TOGGLE() HAL_GPIO_TogglePin(LED1_GPIO_PORT, LED1_GPIO_PIN)

void led_init(void);

#endif /* PLATFORM_STM32H743_LED_H */
