MODULE_SRCS += $(wildcard $(SRC_ROOT)/platform/firmware/*.c)
MODULE_INCS += $(INC_ROOT)/platform/firmware

SRC_PLATFORM_FIRMWARE = $(SRC_ROOT)/platform/firmware
PLATFORM_FIRMWARE_MODS = drivers
include $(foreach mod,$(PLATFORM_FIRMWARE_MODS),$(SRC_PLATFORM_FIRMWARE)/$(mod)/makefile.mk)