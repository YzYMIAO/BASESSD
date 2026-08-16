# 公共模块源文件声明。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/platform/*.c)
MODULE_INCS += $(INC_ROOT)/platform

SRC_PLATFORM = $(SRC_ROOT)/platform
PLATFORM_MODS = firmware host_sim
include $(foreach mod,$(PLATFORM_MODS),$(SRC_PLATFORM)/$(mod)/makefile.mk)
