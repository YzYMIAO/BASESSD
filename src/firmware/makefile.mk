# Cortex-R5 固件启动编排模块，只在 BUILD_TARGET=firmware 时参与构建。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/firmware/*.c)
MODULE_INCS += $(INC_ROOT)/firmware
