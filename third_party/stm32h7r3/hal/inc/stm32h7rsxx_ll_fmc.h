/**
  ******************************************************************************
  * @file    stm32h7rsxx_ll_fmc.h
  * @author  MCD Application Team
  * @brief   Header file of FMC HAL module.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef STM32H7RSxx_LL_FMC_H
#define STM32H7RSxx_LL_FMC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7rsxx_hal_def.h"

/** @addtogroup STM32H7RSxx_HAL_Driver
  * @{
  */

/** @addtogroup FMC_LL
  * @{
  */

/** @addtogroup FMC_LL_Private_Macros
  * @{
  */

#define IS_FMC_NORSRAM_BANK(__BANK__) (((__BANK__) == FMC_NORSRAM_BANK1) || \
                                       ((__BANK__) == FMC_NORSRAM_BANK2) || \
                                       ((__BANK__) == FMC_NORSRAM_BANK3) || \
                                       ((__BANK__) == FMC_NORSRAM_BANK4))
#define IS_FMC_MUX(__MUX__) (((__MUX__) == FMC_DATA_ADDRESS_MUX_DISABLE) || \
                             ((__MUX__) == FMC_DATA_ADDRESS_MUX_ENABLE))
#define IS_FMC_MEMORY(__MEMORY__) (((__MEMORY__) == FMC_MEMORY_TYPE_SRAM) || \
                                   ((__MEMORY__) == FMC_MEMORY_TYPE_PSRAM)|| \
                                   ((__MEMORY__) == FMC_MEMORY_TYPE_NOR))
#define IS_FMC_NORSRAM_MEMORY_WIDTH(__WIDTH__) (((__WIDTH__) == FMC_NORSRAM_MEM_BUS_WIDTH_8)  || \
                                                ((__WIDTH__) == FMC_NORSRAM_MEM_BUS_WIDTH_16) || \
                                                ((__WIDTH__) == FMC_NORSRAM_MEM_BUS_WIDTH_32))
#define IS_FMC_PAGESIZE(__SIZE__) (((__SIZE__) == FMC_PAGE_SIZE_NONE) || \
                                   ((__SIZE__) == FMC_PAGE_SIZE_128) || \
                                   ((__SIZE__) == FMC_PAGE_SIZE_256) || \
                                   ((__SIZE__) == FMC_PAGE_SIZE_512) || \
                                   ((__SIZE__) == FMC_PAGE_SIZE_1024))
#define IS_FMC_WRITE_FIFO(__FIFO__) (((__FIFO__) == FMC_WRITE_FIFO_DISABLE) || \
                                     ((__FIFO__) == FMC_WRITE_FIFO_ENABLE))
#define IS_FMC_ACCESS_MODE(__MODE__) (((__MODE__) == FMC_ACCESS_MODE_A) || \
                                      ((__MODE__) == FMC_ACCESS_MODE_B) || \
                                      ((__MODE__) == FMC_ACCESS_MODE_C) || \
                                      ((__MODE__) == FMC_ACCESS_MODE_D))
#define IS_FMC_BURSTMODE(__STATE__) (((__STATE__) == FMC_BURST_ACCESS_MODE_DISABLE) || \
                                     ((__STATE__) == FMC_BURST_ACCESS_MODE_ENABLE))
#define IS_FMC_WAIT_POLARITY(__POLARITY__) (((__POLARITY__) == FMC_WAIT_SIGNAL_POLARITY_LOW) || \
                                            ((__POLARITY__) == FMC_WAIT_SIGNAL_POLARITY_HIGH))
#define IS_FMC_WAIT_SIGNAL_ACTIVE(__ACTIVE__) (((__ACTIVE__) == FMC_WAIT_TIMING_BEFORE_WS) || \
                                               ((__ACTIVE__) == FMC_WAIT_TIMING_DURING_WS))
#define IS_FMC_WRITE_OPERATION(__OPERATION__) (((__OPERATION__) == FMC_WRITE_OPERATION_DISABLE) || \
                                               ((__OPERATION__) == FMC_WRITE_OPERATION_ENABLE))
#define IS_FMC_WAITE_SIGNAL(__SIGNAL__) (((__SIGNAL__) == FMC_WAIT_SIGNAL_DISABLE) || \
                                         ((__SIGNAL__) == FMC_WAIT_SIGNAL_ENABLE))
#define IS_FMC_EXTENDED_MODE(__MODE__) (((__MODE__) == FMC_EXTENDED_MODE_DISABLE) || \
                                        ((__MODE__) == FMC_EXTENDED_MODE_ENABLE))
#define IS_FMC_ASYNWAIT(__STATE__) (((__STATE__) == FMC_ASYNCHRONOUS_WAIT_DISABLE) || 