#Flash controller front - end module.
MODULE_SRCS += $(wildcard $(SRC_ROOT)/fc/*.c)
MODULE_INCS += $(INC_ROOT)/fc

# FMC NAND 测试仅在显式启用时编入固件，默认产品固件不包含破坏性测试代码。
ifeq ($(FC_NAND_TEST_ENABLE),1)
# munit 单元测试框架
TEST_C_SRCS += $(PROJECT_ROOT)/tests/framework/munit.c
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
# Level 1: 纯逻辑测试
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level1.c
# Level 2: 数据路径测试
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level2.c
# Level 3: 全链路测试
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level3.c
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
# 测试统一入口
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/ut_main.c
CFLAGS += -DFC_NAND_TEST_ENABLE
# TODO Level 2/3 加入后移除
endif
