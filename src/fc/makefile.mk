#Flash controller front - end module.
MODULE_SRCS += $(wildcard $(SRC_ROOT)/fc/*.c)
MODULE_INCS += $(INC_ROOT)/fc

# FMC NAND 测试仅在显式启用时编入固件，默认产品固件不包含破坏性测试代码。
ifeq ($(FC_NAND_TEST_ENABLE),1)
TEST_C_SRCS += $(PROJECT_ROOT)/tests/integration/test_fc_nand.c
MODULE_INCS += $(PROJECT_ROOT)/tests/integration
CFLAGS += -DFC_NAND_TEST_ENABLE
LDFLAGS += -Wl,--undefined=fc_nand_test_run_all
endif
