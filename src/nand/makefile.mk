# NAND 器件/协议/PHY 层 + 按 NAND_DEVICE 选中的器件驱动。
# 器件驱动在 src/nand/devices/，不参与上面的 wildcard ⇒ 一次只编一颗。
ifeq ($(NAND_DEVICE),MT29F___G08A)
NAND_DEVICE_SRC := $(SRC_ROOT)/nand/devices/mt29f___g08a.c
else
$(error Unsupported NAND_DEVICE=$(NAND_DEVICE))
endif

MODULE_SRCS += $(wildcard $(SRC_ROOT)/nand/*.c)
MODULE_SRCS += $(NAND_DEVICE_SRC)
MODULE_INCS += $(INC_ROOT)/nand
MODULE_INCS += $(INC_ROOT)/nand/devices
MODULE_INCS += $(INC_ROOT)/platform/stm32h743

# NAND 私有头（FMC / board internal）与 .c 同目录（src/nand/*.h）：只给 NAND 自己的对象。
# `$(OBJ_DIR)/nand/%.o` 的 % 匹配 devices/…，器件驱动同样拿到这条 -I。
# ⚠ 新增 src/nand/*.h 前必须 `find include src -name '<name>.h'` 查重。
NAND_PRIVATE_INC := $(SRC_ROOT)/nand
$(OBJ_DIR)/nand/%.o: CFLAGS += -I$(NAND_PRIVATE_INC)
