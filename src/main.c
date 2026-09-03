#include "platform/stm32h7r3/h7r3_led.h"

#include "stm32h7rsxx_hal.h"

/*
 * STM32H7R3 烧录自检入口。
 * Reset_Handler 已完成 C 运行时初始化；HAL_Init() 配置 SysTick 为 1 ms 节拍。
 */

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* newlib 在 __libc_init_array() 中调用的裸机初始化钩子。 */
void _init(void)
{
}

int main(void)
{
    HAL_Init();
    h7r3_led_init();

    while (1) {
        h7r3_led_set(H7R3_LED_0, true);
        h7r3_led_set(H7R3_LED_1, false);
        HAL_Delay(1000U);

        h7r3_led_set(H7R3_LED_0, false);
        h7r3_led_set(H7R3_LED_1, true);
        HAL_Delay(1000U);
    }
}
