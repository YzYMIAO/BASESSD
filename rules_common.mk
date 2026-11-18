# ============================================================================
# rules_common.mk - 编译规则模板
# ============================================================================
# 功能：
#   1. 编译规则：.c -> .o
#   2. 自动依赖生成：-MMD -MP
# ============================================================================

# ----------------------------------------------------------------------------
# 0. 构建失败语义与静默模式
# ----------------------------------------------------------------------------
# 编译中断/失败时删除半成品 .o：否则残件可能比源码新，下次被当成有效对象。
.DELETE_ON_ERROR:

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

define THIRD_PARTY_COMPILE_template
$(THIRD_PARTY_C_OBJS):$(VENDOR_OBJ_DIR)/%.o:$(PROJECT_ROOT)/%.c
	@$$(MKDIR) $$(dir $$@)
	@$$(ECHO) "Compiling third-party $$< ..."
	$$(Q)$$(CC) $$(VENDOR_CFLAGS) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

$(THIRD_PARTY_S_OBJS):$(VENDOR_OBJ_DIR)/%.o:$(PROJECT_ROOT)/%.s
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

# 开机横幅的 __DATE__/__TIME__ 来自 main.c 的编译时刻；若只重编其他模块再链接，
# 横幅会显示旧时间，和实际二进制不一致。强制每次构建重编 main.c，保证 banner
# 可作为“当前镜像”的判据。
.PHONY: FORCE
$(OBJ_DIR)/main.o: FORCE
FORCE:
ifneq ($(strip $(THIRD_PARTY_C_OBJS) $(THIRD_PARTY_S_OBJS)),)
$(eval $(THIRD_PARTY_COMPILE_template))
endif
ifneq ($(strip $(TEST_C_OBJS)),)
$(eval $(TEST_COMPILE_template))
endif

# ----------------------------------------------------------------------------
# 2. 自动包含依赖文件
#   对象目录镜像源码路径，深度不固定（third_party 有多层目录），递归收集；
#   不要写成固定层数的 wildcard —— 新增一级目录后依赖会静默丢失。
# ----------------------------------------------------------------------------
-include $(shell find $(OBJ_DIR) -name '*.d' 2>/dev/null)
-include $(shell find $(VENDOR_OBJ_DIR) -name '*.d' 2>/dev/null)
