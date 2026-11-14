# Flash controller module：控制面 + 命令分发（NAND 器件/PHY 在 src/nand/）。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/fc/*.c)
MODULE_INCS += $(INC_ROOT)/fc
# FC 私有头（池 / 控制器/调度器）：不放进公共 include 目录。
MODULE_INCS += $(SRC_ROOT)/fc/internal

# FMC NAND 测试仅在显式启用时编入固件，默认产品固件不包含破坏性测试代码。
ifeq ($(FC_NAND_TEST_ENABLE),1)
# munit 框架 + 测试统一入口：任何 UT 范围都要。
# ⚠ ut_main.c 必须无条件编入——cli.c 的 `test` 命令（#if FC_NAND_TEST_ENABLE）引用
#    ut_run_all；摘掉它会在链接期出现 undefined symbol。
TEST_C_SRCS += $(PROJECT_ROOT)/tests/framework/munit.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/ut_main.c
# 故障注入：链接期 wrap（生产源码零改动）+ 宏隔离的静态缝 hook。
# 只有跨 TU 调用会被 wrap；未武装时 __wrap_* 原样转发 __real_*。
# 设计：docs/design/negative_test_framework.md §2.1 / §5。
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/fi_wrap.c
LDFLAGS += -Wl,--wrap=fc_poll_completion -Wl,--wrap=fc_poll_completions
LDFLAGS += -Wl,--wrap=ftl_op_submit -Wl,--wrap=ftl_op_submit_batch
LDFLAGS += -Wl,--wrap=ftl_async_read_page -Wl,--wrap=ftl_async_write_page
LDFLAGS += -Wl,--wrap=ftl_async_write_page_cw -Wl,--wrap=ftl_async_write_pair_cmd
LDFLAGS += -Wl,--wrap=ftl_async_erase_block
LDFLAGS += -Wl,--wrap=ftl_async_erase_block_multi_plane
LDFLAGS += -Wl,--wrap=ftl_async_write_page_mirror -Wl,--wrap=ftl_sync_write_page_mirror
LDFLAGS += -Wl,--wrap=ftl_sync_read_page
LDFLAGS += -Wl,--wrap=alc_super_open_alloc_pair
LDFLAGS += -Wl,--wrap=gc_select_victim
# 速度测试：一份源、两段实现（UT_FC_ENABLE / UT_FTL_ENABLE 预处理选择）。
# ⚠ 只在这里添加一次——src/ftl/makefile.mk 不再添加，否则 UT=（两者）时对象重复、
#    套件数组符号重复定义。UT=HIL 时两段都不需要，不编入（否则文件顶部的
#    spd_kbps_from_us 会成为未使用的 static → 新告警）。
ifneq ($(strip $(UT_FC_ENABLE)$(UT_FTL_ENABLE)),)
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_speed.c
endif
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
CFLAGS += -DFC_NAND_TEST_ENABLE

# FC-NAND 三级测试源 + 共享夹具：UT 为空（全部）或 UT=FC 时编入。
# UT=FTL 时整体摘除 —— 这三级会真写 NAND（破坏性），且是镜像体积的主要来源。
ifeq ($(UT_FC_ENABLE),1)
# 共享夹具：setup/teardown + 读写擦 helper（Level 2 / Level 3 共用）
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/fc_test_common.c
# Level 1: 纯逻辑测试
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level1.c
# Level 2: 数据路径测试
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level2.c
# Level 3: 全链路测试
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level3.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_negative.c
CFLAGS += -DUT_FC_ENABLE
endif
endif
