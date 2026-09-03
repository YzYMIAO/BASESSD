#include "platform/stm32h7r3/h7r3_led.h"

#include "stm32h7rsxx_hal.h"

static GPIO_TypeDef *h7r3_led_port(h7r3_led_t led)
{
    return (led == H7R3_LED_0) ? GPIOD : GPIOC;
}

static uint32_t h7r3_led_pin(h7r3_led_t led)
{
    return (led == H7R3_LED_0) ? GPIO_PIN_14 : GPIO_PIN_0;
}

void h7r3_led_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    gpio.Pin = GPIO_PIN_14;
    HAL_GPIO_Init(GPIOD, &gpio);
    gpio.Pin = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOC, &gpio);

    h7r3_led_set(H7R3_LED_0, false);
    h7r3_led_set(H7R3_LED_1, false);
}

void h7r3_led_set(h7r3_led_t led, bool on)
{
    HAL_GPIO_WritePin(h7r3_led_port(led), h7r3_led_pin(led),
                      on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}
