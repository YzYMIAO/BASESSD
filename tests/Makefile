# Host-only unit-test entry point. Test framework sources are intentionally not
# part of src/common and are never linked into firmware products.
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g3

ROOT := ..
BUILD_DIR := $(ROOT)/build/tests
UNIT_BOOT_POLICY := $(BUILD_DIR)/test_boot_policy

.PHONY: unit test clean

unit: $(UNIT_BOOT_POLICY)

test: unit
	$(UNIT_BOOT_POLICY)

$(UNIT_BOOT_POLICY): unit/test_boot_policy.c $(ROOT)/src/common/boot_policy.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(ROOT)/include $^ -o $@

clean:
	rm -rf $(BUILD_DIR)
