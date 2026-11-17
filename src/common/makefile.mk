# 公共模块源文件声明。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/common/*.c)
MODULE_INCS += $(INC_ROOT)/common
# boot_policy 使用 FC 的 ISO-HDLC CRC（软件/硬件后端由 FC 抽象）。
MODULE_INCS += $(INC_ROOT)/fc
