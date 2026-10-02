# Host-only unit-test entry point. Test framework sources are intentionally not
# part of src/common and are never linked into firmware products.
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g3

ROOT := ..
BUILD_DIR := $(ROOT)/build/tests
UNIT_BOOT_POLICY := $(BUILD_DIR)/test_boot_policy
UNIT_F4 := $(BUILD_DIR)/test_f4_region_bitmap

# src/common + src/ftl 源码（F4 测试需要 l2p_cache/checkpoint/allocator/metadata 的
# 实现符号。bcm/onfi 等纯工具由 boot_policy 测试复用；f4 链接全部 ftl 模块可能
# 引入 FC 依赖，故仅链接 F4 测试实际用到的实现）。
F4_SRCS := \
    $(ROOT)/src/common/boot_policy.c \
    $(ROOT)/src/ftl/allocator.c \
    $(ROOT)/src/ftl/metadata.c \
    $(ROOT)/src/ftl/l2p_cache.c \
    $(ROOT)/src/ftl/checkpoint.c

.PHONY: unit test f4 clean

unit: $(UNIT_BOOT_POLICY) $(UNIT_F4)

test: unit
	$(UNIT_BOOT_POLICY)
	$(UNIT_F4)

$(UNIT_BOOT_POLICY): unit/test_boot_policy.c $(ROOT)/src/common/boot_policy.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(ROOT)/include $^ -o $@

$(UNIT_F4): unit/test_f4_region_bitmap.c $(F4_SRCS) $(ROOT)/tests/framework/munit.c $(ROOT)/tests/framework/munit_stub_fc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -DMUNIT_HOST_STANDALONE -I$(ROOT)/include -I$(ROOT)/tests/unit -I$(ROOT)/tests/framework $^ -o $@

f4: $(UNIT_F4)
	$(UNIT_F4)

clean:
	rm -rf $(BUILD_DIR)
