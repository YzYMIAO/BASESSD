
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef PLATFORM_STM32H743_IT_H
#define PLATFORM_STM32H743_IT_H

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "btim.h"
#include "stm32h7xx.h"
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

void EXTI0_IRQHandler(void);
void EXTI2_IRQHandler(void);
void EXTI3_IRQHandler(void);
void EXTI15_10_IRQHandler(void);

void USART1_IRQHandler(void);
void DMA1_Stream0_IRQHandler(void);
void TIM6_DAC_IRQHandler(void);
void WWDG_IRQHandler(void);

#endif /* PLATFORM_STM32H743_IT_H */
