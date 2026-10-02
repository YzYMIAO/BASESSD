# 说明：测试仅在 FC_NAND_TEST_ENABLE=1 时编入固件（src/fc/makefile.mk），
# 这是 host 交叉编译到 ARM 的辅助验证入口（fc 层 3 级框架沿用 Phase P 方案）。
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g3

ROOT := ..
BUILD_DIR := $(ROOT)/build/tests
UNIT_F4 := $(BUILD_DIR)/test_f4_region_bitmap
UNIT_F6 := $(BUILD_DIR)/test_f6_pending
UNIT_F10 := $(BUILD_DIR)/test_f10_replay

# src/common + src/ftl 源码（F4 测试需要 l2p_cache/checkpoint/allocator/metadata 的
# 实现符号。bcm/onfi 等纯工具由 boot_policy 测试复用；f4 链接全部 ftl 模块可能
# 引入 FC 依赖，故仅链接 F4 测试实际用到的实现）。
F4_SRCS := \
    $(ROOT)/src/common/boot_policy.c \
    $(ROOT)/src/ftl/allocator.c \
    $(ROOT)/src/ftl/metadata.c \
    $(ROOT)/src/ftl/l2p_cache.c \
    $(ROOT)/src/ftl/checkpoint.c

# F10 journal_replay 全量回放：journal.c + l2p_cache.c（内存 NAND 桩读回真实数据）。
# l2p_cache 引用 metadata_dir_*（md_dir==NULL 时走固定位置回退，不触发）；
# metadata.c 引用 allocator_alloc / boot_crc32，一并链接满足符号。
F10_SRCS := \
    $(ROOT)/src/ftl/journal.c \
    $(ROOT)/src/ftl/l2p_cache.c \
    $(ROOT)/src/ftl/metadata.c \
    $(ROOT)/src/ftl/allocator.c \
    $(ROOT)/src/common/boot_policy.c

.PHONY: unit test f4 f6 f10 clean

unit: $(UNIT_F4) $(UNIT_F6) $(UNIT_F10)

test: unit
	$(UNIT_F4)
	$(UNIT_F6)
	$(UNIT_F10)

$(UNIT_F4): unit/test_f4_region_bitmap.c $(F4_SRCS) $(ROOT)/tests/framework/munit.c $(ROOT)/tests/framework/munit_stub_fc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -DMUNIT_HOST_STANDALONE -I$(ROOT)/include -I$(ROOT)/tests/unit -I$(ROOT)/tests/framework $^ -o $@

# F6 pending/completion 多实例化：pending.c + completion_table.c。
# ftl_sync_waiter_handler_with_release 引用 fc_command_id_to_command/release，
# 需要 munit_stub_fc（host 桩虚拟成功；fc_poll_completion 桩返回 QUEUE_EMPTY
# 终止 drain——本测试不调 drain，桩仅满足链接）。
$(UNIT_F6): unit/test_f6_pending.c $(ROOT)/src/ftl/pending.c $(ROOT)/src/ftl/completion_table.c $(ROOT)/tests/framework/munit.c $(ROOT)/tests/framework/munit_stub_fc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -DMUNIT_HOST_STANDALONE -I$(ROOT)/include -I$(ROOT)/tests/unit -I$(ROOT)/tests/framework $^ -o $@

f4: $(UNIT_F4)
	$(UNIT_F4)

f6: $(UNIT_F6)
	$(UNIT_F6)

f10: $(UNIT_F10)
	$(UNIT_F10)

$(UNIT_F10): unit/test_f10_replay.c $(F10_SRCS) $(ROOT)/tests/framework/munit.c $(ROOT)/tests/framework/munit_stub_fc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -DMUNIT_HOST_STANDALONE -I$(ROOT)/include -I$(ROOT)/tests/unit -I$(ROOT)/tests/framework $^ -o $@

clean:
	rm -rf $(BUILD_DIR)
