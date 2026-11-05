# HIL（Host Interface Layer）模块源文件声明。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/hil/*.c)
MODULE_INCS += $(INC_ROOT)/hil

# HIL 板端 UT（真 NAND/FTL，无 mock）：UT 为空或 UT=HIL 时编入。
# ftl_ut_common.c 由 src/ftl/makefile.mk 在本范围下补入。
ifeq ($(FC_NAND_TEST_ENABLE),1)
ifeq ($(UT_HIL_ENABLE),1)
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_hil.c
CFLAGS += -DUT_HIL_ENABLE
endif
endif
