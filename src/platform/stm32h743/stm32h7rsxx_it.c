/* USER CODE BEGIN Header */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "stm32h7rsxx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */
#include "dma.h"

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/*
 * NMI_Handler / HardFault_Handler / MemManage_Handler / BusFault_Handler /
 * UsageFault_Handler 由 platform/stm32h743/fault.c 提供（naked 蹦床 + 现场 dump），
 * 这里不再定义，否则链接期重复符号。见 docs/design/fault_capture.md。
 */

/**
 * @brief This function handles System service call via SWI instruction.
 */
void SVC_Handler(void)
{
    /* USER CODE BEGIN SVCall_IRQn 0 */

    /* USER CODE END SVCall_IRQn 0 */
    /* USER CODE BEGIN SVCall_IRQn 1 */

    /* USER CODE END SVCall_IRQn 1 */
}

/**
 * @brief This function handles Debug monitor.
 */
void DebugMon_Handler(void)
{
    /* USER CODE BEGIN DebugMonitor_IRQn 0 */

    /* USER CODE END DebugMonitor_IRQn 0 */
    /* USER CODE BEGIN DebugMonitor_IRQn 1 */

    /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
 * @brief This function handles Pendable request for system service.
 */
void PendSV_Handler(void)
{
    /* USER CODE BEGIN PendSV_IRQn 0 */

    /* USER CODE END PendSV_IRQn 0 */
    /* USER CODE BEGIN PendSV_IRQn 1 */

    /* USER CODE END PendSV_IRQn 1 */
}

/**
 * @brief This function handles System tick timer.
 */
void SysTick_Handler(void)
{
    /* USER CODE BEGIN SysTick_IRQn 0 */

    /* USER CODE END SysTick_IRQn 0 */
    HAL_IncTick();
    /* USER CODE BEGIN SysTick_IRQn 1 */

    /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32H7RSxx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7rsxx.s).                    */
/******************************************************************************/

/* USER CODE BEGIN 1 */

/**
 * @brief   WKUP按键外部中断服务函数
 * @param   无
 * @retval  无
 */
void EXTI0_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0);
}

/**
 * @brief   KEY0按键外部中断服务函数
 * @param   无
 * @retval  无
 */
void EXTI3_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_3);
}

/**
 * @brief   KEY1按键外部中断服务函数
 * @param   无
 * @retval  无
 */
void EXTI2_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_2);
}

/**
 * @brief   KEY2按键外部中断服务函数
 * @param   无
 * @retval  无
 */
void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_13);
}

/**
 * @brief   UART中断回调函数
 * @param   无
 * @retval  无
 */
void USART1_IRQHandler(void)
{
#if SYS_SUPPORT_OS
    OSIntEnter();
#endif

    /* Keep the IRQ bounded: it only drains/fills FIFOs and records errors. */
    usart_port_irq_handler();

#if SYS_SUPPORT_OS
    OSIntExit();
#endif
}

void DMA1_Stream0_IRQHandler(void)
{
    uart_tx_dma_irq_handler();
}

/**
 * @brief   TIM中断服务函数
 * @param   无
 * @retval  无
 */
void TIM6_DAC_IRQHandler(void)
{
    /* 定时器句柄由 btim_port.c 私有持有，这里只转发。 */
    btim_port_irq_handler();
}

/**
 * @brief WWDG 提前唤醒中断入口。
 * @note  不在这里刷新 WWDG；Supervisor 记录故障并让硬件完成复位。
 */
void WWDG_IRQHandler(void)
{
    storage_wdg_wwdg_irq_handler();
}

/**
 * @brief   DMA2 Stream0 中断服务函数（NAND PROGRAM 方向 DMA 完成）
 * @note    由 nand_dma_irq_handler_data_out() 处理
 */
void DMA2_Stream0_IRQHandler(void)
{
    nand_dma_irq_handler_data_out();
}

/**
 * @brief   DMA2 Stream1 中断服务函数（NAND READ 方向 DMA 完成）
 * @note    由 nand_dma_irq_handler_data_in() 处理
 */
void DMA2_Stream1_IRQHandler(void)
{
    nand_dma_irq_handler_data_in();
}

/* USER CODE END 1 */
