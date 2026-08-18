# Common platform registration code.
MODULE_SRCS += $(wildcard $(SRC_ROOT)/platform/*.c)
MODULE_INCS += $(INC_ROOT)/platform

SRC_PLATFORM = $(SRC_ROOT)/platform
ifeq ($(BUILD_TARGET),firmware)
PLATFORM_MODS = armv7r $(FIRMWARE_PLATFORM)
else
PLATFORM_MODS = host_sim
endif

include $(foreach mod,$(PLATFORM_MODS),$(SRC_PLATFORM)/$(mod)/makefile.mk)
