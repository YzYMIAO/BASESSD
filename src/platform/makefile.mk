# Common platform registration code.
MODULE_SRCS += $(wildcard $(SRC_ROOT)/platform/*.c)
MODULE_INCS += $(INC_ROOT)/platform

SRC_PLATFORM = $(SRC_ROOT)/platform
ifeq ($(BUILD_TARGET),firmware)
ifeq ($(FIRMWARE_PLATFORM),reference_soc)
PLATFORM_MODS = armv7r reference_soc
else ifeq ($(FIRMWARE_PLATFORM),stm32h7r3)
PLATFORM_MODS = stm32h7r3
else ifeq ($(FIRMWARE_PLATFORM),stm32h743)
PLATFORM_MODS = stm32h743
else
$(error Unsupported FIRMWARE_PLATFORM=$(FIRMWARE_PLATFORM))
endif
else
PLATFORM_MODS = host_sim
endif

include $(foreach mod,$(PLATFORM_MODS),$(SRC_PLATFORM)/$(mod)/makefile.mk)
