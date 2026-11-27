# STM32H743 SoC 端口层：供应商 HAL/CMSIS + 各平台服务的端口实现。
include $(PROJECT_ROOT)/third_party/stm32h743_vendor.mk
include $(PROJECT_ROOT)/third_party/tinyusb_vendor.mk

MODULE_INCS += $(STM32H743_INCS)
MODULE_INCS += $(TINYUSB_INCS)
THIRD_PARTY_C_SRCS += $(STM32H743_SRCS)
THIRD_PARTY_C_SRCS += $(TINYUSB_SRCS)
THIRD_PARTY_S_SRCS += $(STM32H743_S_SRCS)

#它们是传给 GCC 和汇编器的预处理宏定义，不是编译优化参数。
# 第三方（HAL/CMSIS）与项目共用前两个；VENDOR_CFLAGS 复用该变量。
PLATFORM_COMMON_DEFINES := -DUSE_HAL_DRIVER -DSTM32H743xx
CFLAGS += $(PLATFORM_COMMON_DEFINES) -DPLATFORM_CRC_HW_SUPPORTED=1
ASFLAGS += -DSTM32H743xx
THIRD_PARTY_CFLAGS += -Wno-cast-align

# SoC 内部 IP 驱动保留在 soc/stm32h743 目录；NAND ONFI/FMC/芯片适配在 src/nand。
MODULE_SRCS += $(wildcard $(SOC_DIR)/*.c)
MODULE_INCS += $(INC_ROOT)/platform/soc/stm32h743
