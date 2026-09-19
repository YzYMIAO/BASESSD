# ============================================================================
# rules_common.mk - 编译规则模板
# ============================================================================
# 功能：
#   1. 编译规则：.c -> .o
#   2. 自动依赖生成：-MMD -MP
#   3. 代码覆盖率支持
# ============================================================================

# ----------------------------------------------------------------------------
# 0. 静默模式控制
# ----------------------------------------------------------------------------
ifeq ($(V),1)
Q =
else
Q = @
endif

# ----------------------------------------------------------------------------
# 1. 编译规则模板
# ----------------------------------------------------------------------------
define COMPILE_template
$(C_OBJS):$(OBJ_DIR)/%.o: $(SRC_ROOT)/%.c
	@$$(MKDIR) $$(dir $$@)
	@$$(ECHO) "Compiling the $$< ..."
	$$(Q)$$(CC) $$(CFLAGS) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@
endef

define ASILE_template
$(S_OBJS):$(OBJ_DIR)/%.o:$(SRC_ROOT)/%.s
	@$$(MKDIR) $$(dir $$@)
	@$$(ECHO) "Assembling the $$< ..."
	$$(Q)$$(AS) $$(ASFLAGS) -MD -MP -c $$< -o $$@
endef

define THIRD_PARTY_COMPILE_template
$(THIRD_PARTY_C_OBJS):$(OBJ_DIR)/%.o:$(PROJECT_ROOT)/%.c
	@$$(MKDIR) $$(dir $$@)
	@$$(ECHO) "Compiling third-party $$< ..."
	$$(Q)$$(CC) $$(CFLAGS) $$(THIRD_PARTY_CFLAGS) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

$(THIRD_PARTY_S_OBJS):$(OBJ_DIR)/%.o:$(PROJECT_ROOT)/%.s
	@$$(MKDIR) $$(dir $$@)
	@$$(ECHO) "Assembling third-party $$< ..."
	$$(Q)$$(AS) $$(ASFLAGS) -MD -MP -c $$< -o $$@
endef

# 测试源码位于 tests/，与 src/ 使用不同的路径规则；仅测试构建设置 TEST_C_SRCS。
define TEST_COMPILE_template
$(TEST_C_OBJS):$(OBJ_DIR)/%.o:$(PROJECT_ROOT)/%.c
	@$$(MKDIR) $$(dir $$@)
	@$$(ECHO) "Compiling test $$< ..."
	$$(Q)$$(CC) $$(CFLAGS) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@
endef

# 展开编译规则
$(eval $(COMPILE_template))
$(eval $(ASILE_template))
ifneq ($(strip $(THIRD_PARTY_C_OBJS) $(THIRD_PARTY_S_OBJS)),)
$(eval $(THIRD_PARTY_COMPILE_template))
endif
ifneq ($(strip $(TEST_C_OBJS)),)
$(eval $(TEST_COMPILE_template))
endif

# ----------------------------------------------------------------------------
# 2. 自动包含依赖文件
# ----------------------------------------------------------------------------
-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/*/*.d)
-include $(wildcard $(OBJ_DIR)/*/*/*.d)
-include $(THIRD_PARTY_C_OBJS:.o=.d)
-include $(THIRD_PARTY_S_OBJS:.o=.d)
-include $(TEST_C_OBJS:.o=.d)

# ----------------------------------------------------------------------------
# 3. 代码覆盖率支持（通过 config.mk 中的 COVERAGE 控制）
# ----------------------------------------------------------------------------
ifeq ($(COVERAGE),1)
CFLAGS += -fprofile-arcs -ftest-coverage
LDFLAGS += -fprofile-arcs -ftest-coverage
endif
