MODULE_SRCS += $(wildcard $(SRC_ROOT)/host/*.c)
MODULE_INCS += $(INC_ROOT)/host

SRC_HOST = $(SRC_ROOT)/host
NVME_MODS = nvme 
include $(foreach mod,$(NVME_MODS),$(SRC_HOST)/$(mod)/makefile.mk)
