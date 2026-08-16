# 公共模块源文件声明。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/common/*.c)
MODULE_INCS += $(INC_ROOT)/common

SRC_COMMON = $(SRC_ROOT)/common
COMMON_MODS = algorithm data_structure
include $(foreach mod,$(COMMON_MODS),$(SRC_COMMON)/$(mod)/makefile.mk)
