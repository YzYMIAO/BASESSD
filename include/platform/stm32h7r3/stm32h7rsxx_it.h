
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __STM32H7RSxx_IT_H
#define __STM32H7RSxx_IT_H

#ifdef __cplusplus
extern "C"
{
#endif

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "btim.h"
#include "exti.h"
#include "stm32h7rsxx.h"
#include "usart.h"
#include "watchdog.h"
    /* USER CODE END Includes */

    /* Exported types ------------------------------------------------------------*/
    /* USER CODE BEGIN ET */

    /* USER CODE END ET */

    /* Exported constants --------------------------------------------------------*/
    /* USER CODE BEGIN EC */

    /* USER CODE END EC */

    /* Exported macro ------------------------------------------------------------*/
    /* USER CODE BEGIN EM */

    /* USER CODE END EM */

    /* Exported functions prototypes ---------------------------------------------*/
    void NMI_Handler(void);
    void HardFault_Handler(void);
    void MemManage_Handler(void);
    void BusFault_Handler(void);
    void UsageFault_Handler(void);
    void SVC_Handler(void);
    void DebugMon_Handler(void);
    void PendSV_Handler(void);
    void SysTick_Handler(void);
    /* USER CODE BEGIN EFP */

    /* USER CODE END EFP */

    void EXTI7_IRQHandler(void);
    void EXTI8_IRQHandler(void);
    void EXTI9_IRQHandler(void);
    void EXTI13_IRQHandler(void);

    void USART1_IRQHandler(void);
    void TIM6_IRQHandler(void);
    void WWDG_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __STM32H7RSxx_IT_H */
