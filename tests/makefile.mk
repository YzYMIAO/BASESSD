# 测试构建声明（所有 UT 源、链接期注入、测试专用 include 都归本文件）。
#
# 生产模块的 src/*/makefile.mk 只声明生产源；测试不再反向散落在各模块
# fragment 里（否则「哪些源由哪个模块加、不要重复加」只能靠注释契约维持）。
#
# 范围由顶层 makefile 的 UT= 选择（UT_FC_ENABLE / UT_FTL_ENABLE / UT_HIL_ENABLE）；
# FC_NAND_TEST_ENABLE=1 时所有 UT 构建都编入 munit + ut_main + fi_wrap。
ifeq ($(FC_NAND_TEST_ENABLE),1)

# 生产/测试共用的编译期开关：cli.c 的 `test` 命令、fi_wrap.h 的注入接口等
# 都按这个宏裁剪，测试构建必须定义它。
CFLAGS += -DFC_NAND_TEST_ENABLE

# 测试对象专用 include：不得进入全局 CFLAGS —— 否则全部生产对象都能看到
# tests/unit 与 tests/framework 的头文件（与公共/私有头边界相反方向的泄漏）。
# 白盒测试需要模块私有头：FC/FTL 的私有 include 变量由各自 makefile.mk 提供。
TEST_INCS := $(PROJECT_ROOT)/tests/unit \
             $(PROJECT_ROOT)/tests/framework \
             $(FC_PRIVATE_INC) \
             $(FTL_PRIVATE_INC)
$(OBJ_DIR)/tests/unit/%.o: CFLAGS += $(addprefix -I,$(strip $(TEST_INCS)))

# 唯一例外：生产 CLI 的 `test` 命令入口（#if FC_NAND_TEST_ENABLE）引用 ut_main.h。
# 只给 cli.o 一条路径，不把 tests/unit 暴露给其它生产对象。
$(OBJ_DIR)/hil/cli.o: CFLAGS += -I$(PROJECT_ROOT)/tests/unit

# munit 框架 + 测试统一入口：任何 UT 范围都要。
# ⚠ ut_main.c 必须无条件编入——cli.c 的 `test` 命令（#if FC_NAND_TEST_ENABLE）
#   引用 ut_run_all；摘掉它会在链接期出现 undefined symbol。
TEST_C_SRCS += $(PROJECT_ROOT)/tests/framework/munit.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/ut_main.c

# 故障注入：链接期 wrap（生产源码零改动）+ 宏隔离的静态缝 hook。
# 只有跨 TU 调用会被 wrap；未武装时 __wrap_* 原样转发 __real_*。
# 设计：docs/guides/testing_guide.md §12（注入台账见各层 UT 设计文档 §7.2）。
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/fi_wrap.c
LDFLAGS += -Wl,--wrap=fc_poll_completion -Wl,--wrap=fc_poll_completions
LDFLAGS += -Wl,--wrap=ftl_op_submit -Wl,--wrap=ftl_op_submit_batch
LDFLAGS += -Wl,--wrap=ftl_async_read_page -Wl,--wrap=ftl_async_read_page_leased -Wl,--wrap=ftl_async_write_page
LDFLAGS += -Wl,--wrap=ftl_async_write_page_cw -Wl,--wrap=ftl_async_write_pair_cmd
LDFLAGS += -Wl,--wrap=ftl_async_erase_block
LDFLAGS += -Wl,--wrap=ftl_async_erase_block_multi_plane
LDFLAGS += -Wl,--wrap=ftl_sync_erase_block_multi_plane
LDFLAGS += -Wl,--wrap=ftl_async_write_page_mirror -Wl,--wrap=ftl_sync_write_page_mirror
LDFLAGS += -Wl,--wrap=ftl_sync_read_page
LDFLAGS += -Wl,--wrap=ftl_sync_read_page_status
LDFLAGS += -Wl,--wrap=ftl_sync_read_column
LDFLAGS += -Wl,--wrap=fc_data_path_encode
LDFLAGS += -Wl,--wrap=fc_data_path_process_read
LDFLAGS += -Wl,--wrap=delay_time_now_us
LDFLAGS += -Wl,--wrap=alc_super_open_alloc_pair
LDFLAGS += -Wl,--wrap=gc_select_victim

# speed：一份源、两段实现（UT_FC_ENABLE / UT_FTL_ENABLE 预处理选择）。
# 只在这里添加一次——两个范围同时开启时也只编一个对象。
ifneq ($(strip $(UT_FC_ENABLE)$(UT_FTL_ENABLE)),)
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_speed.c
endif

# 所有 UT 构建统一开启 munit 计时。原先由顶层 `tests` 目标塞进 CFLAGS_EXTRA，
# 属于测试构建配置，放回本文件。
CFLAGS += -DMUNIT_ENABLE_TIMING

ifeq ($(UT_FC_ENABLE),1)
# 共享夹具 + pure / data_path / lifecycle / negative 四套。
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/fc_test_common.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_pure.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_data_path.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_lifecycle.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_negative.c
CFLAGS += -DUT_FC_ENABLE
endif

ifeq ($(UT_FTL_ENABLE),1)
# FTL pure + media + 两层负例（注入台账见 docs/design/ftl_ut_design.md §7.2）。
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/ftl_ut_common.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl_pure.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl_media.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_negative_pure.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_negative_media.c
CFLAGS += -DUT_FTL_ENABLE
endif

ifeq ($(UT_HIL_ENABLE),1)
# HIL 板端（真 NAND/FTL，无 mock）；见 docs/design/hil_ut_design.md。
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_hil.c
CFLAGS += -DUT_HIL_ENABLE
endif

endif # FC_NAND_TEST_ENABLE
