MODULE_SRCS += $(wildcard $(SRC_ROOT)/platform/bsp/*.c)
MODULE_INCS += $(INC_ROOT)/platform/bsp

SRC_PLATFORM_BSP = $(SRC_ROOT)/platform/bsp
PLATFORM_BSP_MODS = drivers
include $(foreach mod,$(PLATFORM_BSP_MODS),$(SRC_PLATFORM_BSP)/$(mod)/makefile.mk)
