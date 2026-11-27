# Apollo V2 板级资产：板级常量头、BSP 驱动与链接脚本。
# 链接脚本由 config.mk 的 LINKER_DIR 引用，不在此登记为源文件。
MODULE_INCS += $(INC_ROOT)/platform/boards/$(FIRMWARE_BOARD)
MODULE_INCS += $(INC_ROOT)/platform/boards/$(FIRMWARE_BOARD)/bsp

# BSP 驱动目前没有消费者（SDRAM 未启用），不纳入构建，只保留源码供后续移植。
