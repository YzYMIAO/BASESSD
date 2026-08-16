# ============================================================================
# 顶层 Makefile - 企业级构建入口
# ============================================================================
# 设计原则：
#   1. 只负责解析配置、调用rules.mk
#   2. 所有编译规则由rules.mk管理
#   3. 支持多构建类型切换（debug/release/asan）
# ============================================================================
.DEFAULT_GOAL := all
# ----------------------------------------------------------------------------
# 1. 包含配置
# ----------------------------------------------------------------------------
PROJECT_ROOT := $(CURDIR)
include $(PROJECT_ROOT)/config.mk

# ============================================================================
# 关键修正：在包含rules.mk之前，先收集所有子模块的源文件
# ============================================================================

# 2. 初始化收集变量（供子模块追加）
MODULE_SRCS :=
MODULE_INCS :=

# 3. 包含所有子模块的声明文件（在解析阶段完成收集）
#    这一步会将子模块中的 MODULE_SRCS += xxx 追加进来
$(info ========================================)
$(info Including module makefiles...)
include $(foreach mod,$(MODULES),$(SRC_ROOT)/$(mod)/makefile.mk)
$(info All modules included)
$(info ========================================)

# 4. 将收集到的源文件转换为目标文件列表
#    $(sort) 用于去重，防止同一个文件被多次编译
C_SRCS := $(wildcard $(SRC_ROOT)/*c) $(sort $(MODULE_SRCS))
S_SRCS := $(wildcard $(STARTUP_DIR)/*.s)
ALL_SRCS := $(C_SRCS) $(S_SRCS)

C_OBJS := $(patsubst $(SRC_ROOT)/%.c, $(OBJ_DIR)/%.o, $(C_SRCS))
S_OBJS := $(patsubst $(STARTUP_DIR)/%.s, $(OBJ_DIR)/startup/%.o, $(S_SRCS))
ALL_OBJS := $(C_OBJS) $(S_OBJS)

# 5. 收集所有头文件路径并添加到编译选项
ALL_INCS := $(sort $(MODULE_INCS))
CFLAGS += $(addprefix -I, $(ALL_INCS))

# 6. 导出收集结果（供调试和rules.mk使用）
$(info Collected $(words $(ALL_SRCS)) source files)
$(info Collected $(words $(ALL_INCS)) include paths)
$(info ========================================)

export C_SRCS
export S_SRCS
export C_OBJS
export S_OBJS
export ALL_OBJS
# ============================================================================
# 7. 包含编译和链接规则（明确分开包含）
# ============================================================================
include $(PROJECT_ROOT)/rules_common.mk   # 编译规则（.c → .o）
include $(PROJECT_ROOT)/rules_link.mk          # 链接规则（.o → 可执行文件）

# ----------------------------------------------------------------------------
# 8. 快捷构建类型切换
# ----------------------------------------------------------------------------
.PHONY: debug release asan
debug:
	$(MAKE) BUILD_TYPE=debug

release:
	$(MAKE) BUILD_TYPE=release

asan:
	$(MAKE) BUILD_TYPE=asan
