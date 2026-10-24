# FTL 模块源文件声明。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/ftl/*.c)
MODULE_INCS += $(INC_ROOT)/ftl

# FTL 层单元测试（pure RAM + media 真 NAND；见 docs/design/ftl_ut_design.md）。
# ⚠ 只加测试源，**不**加 munit.c / ut_main.c —— 那两个由 src/fc/makefile.mk 的
#    同一开关添加，重复添加会重定义符号。include 路径同理（已由 fc 块加过）。
# UT 为空（全部）或 UT=FTL 时编入；UT=FC 时整体摘除（FC 侧不需要 FTL 套件）。
ifeq ($(FC_NAND_TEST_ENABLE),1)
ifeq ($(UT_FTL_ENABLE),1)
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/ftl_ut_common.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl_pure_mem.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl_pure_md.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl_pure_host.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl_pure_io.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl_media.c
CFLAGS += -DUT_FTL_ENABLE
endif
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
endif
