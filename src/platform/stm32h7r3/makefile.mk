include $(PROJECT_ROOT)/third_party/stm32h7r3_vendor.mk

MODULE_INCS += $(STM32H7R3_INCS)
THIRD_PARTY_C_SRCS += $(STM32H7R3_SRCS)
THIRD_PARTY_S_SRCS += $(STM32H7R3_S_SRCS)

#它们是传给 GCC 和汇编器的预处理宏定义，不是编译优化参数。
CFLAGS += -DUSE_HAL_DRIVER -DSTM32H7R3xx
ASFLAGS += -DSTM32H7R3xx
THIRD_PARTY_CFLAGS += -Wno-cast-align

# 板级自检驱动显式列出，避免依赖 board_*.c 命名约定。
MODULE_SRCS += $(SRC_ROOT)/platform/stm32h7r3/h7r3_led.c
