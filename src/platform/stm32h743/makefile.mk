include $(PROJECT_ROOT)/third_party/stm32h743_vendor.mk
include $(PROJECT_ROOT)/third_party/tinyusb_vendor.mk

MODULE_INCS += $(STM32H743_INCS)
MODULE_INCS += $(TINYUSB_INCS)
THIRD_PARTY_C_SRCS += $(STM32H743_SRCS)
THIRD_PARTY_C_SRCS += $(TINYUSB_SRCS)
THIRD_PARTY_S_SRCS += $(STM32H743_S_SRCS)

#它们是传给 GCC 和汇编器的预处理宏定义，不是编译优化参数。
CFLAGS += -DUSE_HAL_DRIVER -DSTM32H743xx -DPLATFORM_CRC_HW_SUPPORTED=1
ASFLAGS += -DSTM32H743xx
THIRD_PARTY_CFLAGS += -Wno-cast-align

# SoC 内部 IP 驱动保留在 stm32h743 根目录；NAND ONFI/FMC/芯片适配已迁移到 src/nand。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/platform/stm32h743/*.c)
MODULE_INCS += $(INC_ROOT)/platform/stm32h743
MODULE_INCS += $(INC_ROOT)/platform/stm32h743/bsp
