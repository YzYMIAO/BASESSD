# STM32H743 CMSIS and HAL sources used by the firmware build.
STM32H743_DIR := $(PROJECT_ROOT)/third_party/stm32h743
STM32H743_HAL_INC := $(STM32H743_DIR)/hal/inc
STM32H743_HAL_SRC := $(STM32H743_DIR)/hal/src

STM32H743_INCS := \
    $(STM32H743_DIR)/cmsis-core \
    $(STM32H743_DIR)/cmsis-device \
    $(STM32H743_HAL_INC)

STM32H743_SRCS := \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal.c \
    $(STM32H743_DIR)/system_stm32h7xx.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_cortex.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_dma.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_dma_ex.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_flash.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_flash_ex.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_gpio.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_iwdg.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_mdma.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_nand.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_ll_fmc.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_pwr.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_pwr_ex.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_qspi.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_rcc.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_rcc_ex.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_rng.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_sdram.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_spi.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_sram.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_tim.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_tim_ex.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_uart.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_uart_ex.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_usart.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_usart_ex.c \
    $(STM32H743_HAL_SRC)/stm32h7xx_hal_wwdg.c

STM32H743_S_SRCS := \
    $(STM32H743_DIR)/startup_stm32h743xx.s
