# 平台共享服务（与 SoC / 板无关的逻辑）。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/platform/*.c)
MODULE_INCS += $(INC_ROOT)/platform

# 平台 = SoC 端口 + 板级资产。两个选择器各管一层：
#   FIRMWARE_PLATFORM 选 SoC（端口实现 + 供应商 HAL）；
#   FIRMWARE_BOARD    选板（BSP、链接脚本、板级常量头）。
SRC_PLATFORM = $(SRC_ROOT)/platform
SOC_DIR = $(SRC_PLATFORM)/soc/$(FIRMWARE_PLATFORM)

ifeq ($(FIRMWARE_PLATFORM),stm32h743)
PLATFORM_MODS = soc/stm32h743 boards/$(FIRMWARE_BOARD)
else
$(error Unsupported FIRMWARE_PLATFORM=$(FIRMWARE_PLATFORM))
endif

include $(foreach mod,$(PLATFORM_MODS),$(SRC_PLATFORM)/$(mod)/makefile.mk)
