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
BUILD_TARGET ?=firmware
FIRMWARE_PLATFORM ?= stm32h7r3
BUILD_TYPE ?= debug

include $(PROJECT_ROOT)/config.mk

# ============================================================================
# 关键修正：在包含rules.mk之前，先收集所有子模块的源文件
# ============================================================================

THIRD_PARTY_C_SRCS :=
THIRD_PARTY_S_SRCS :=
# 2. 初始化收集变量（供子模块追加）
MODULE_SRCS :=
MODULE_INCS :=
PLATFORM_S_SRCS :=
# C objects that must bypass static archives during the final link.
LINK_C_OBJS :=
# 3. 包含所有子模块的声明文件（在解析阶段完成收集）
#    这一步会将子模块中的 MODULE_SRCS += xxx 追加进来
$(info ========================================)
$(info Including module makefiles...)
include $(foreach mod,$(MODULES),$(SRC_ROOT)/$(mod)/makefile.mk)
$(info All modules included)
$(info ========================================)

ifeq ($(BUILD_TARGET),firmware)
# 根目录 main.c 是固件应用入口，不归入静态库，必须在链接时显式加入。
	APP_C_SRCS := $(SRC_ROOT)/main.c
	C_SRCS := $(sort $(MODULE_SRCS) $(APP_C_SRCS))
	C_SRCS := $(filter-out $(SRC_ROOT)/platform/host_sim/%,$(C_SRCS))
	S_SRCS := $(sort $(PLATFORM_S_SRCS))
	ALL_SRCS := $(C_SRCS) $(S_SRCS) $(THIRD_PARTY_C_SRCS) $(THIRD_PARTY_S_SRCS)

APP_OBJS := $(patsubst $(SRC_ROOT)/%.c, $(OBJ_DIR)/%.o, $(APP_C_SRCS))
C_OBJS := $(patsubst $(SRC_ROOT)/%.c, $(OBJ_DIR)/%.o, $(C_SRCS))
S_OBJS := $(patsubst $(SRC_ROOT)/%.s, $(OBJ_DIR)/%.o, $(S_SRCS))
	THIRD_PARTY_C_OBJS := $(patsubst $(PROJECT_ROOT)/%.c,$(OBJ_DIR)/%.o,$(THIRD_PARTY_C_SRCS))
	THIRD_PARTY_S_OBJS := $(patsubst $(PROJECT_ROOT)/%.s,$(OBJ_DIR)/%.o,$(THIRD_PARTY_S_SRCS))
	# Application-owned sources must be linked as objects.  In particular, this
	# allows strong interrupt handlers to override weak startup defaults.
	LINK_C_OBJS := $(C_OBJS)
	LINK_S_OBJS := $(S_OBJS) $(THIRD_PARTY_S_OBJS)

	ALL_OBJS := $(C_OBJS) $(S_OBJS) $(THIRD_PARTY_C_OBJS) $(THIRD_PARTY_S_OBJS)

ALL_INCS := $(sort $(MODULE_INCS))
CFLAGS += $(addprefix -I, $(ALL_INCS))
CFLAGS += -DSSD_FIRMWARE_BUILD
else
C_SRCS := $(sort $(MODULE_SRCS))
ALL_SRCS := $(C_SRCS)

C_OBJS := $(patsubst $(SRC_ROOT)/%.c, $(OBJ_DIR)/%.o, $(C_SRCS))
ALL_OBJS := $(C_OBJS)

ALL_INCS := $(sort $(MODULE_INCS))
CFLAGS += $(addprefix -I, $(ALL_INCS))
endif

# 导出收集结果（供调试和rules.mk使用）
$(info ========================================)
$(info Collected $(words $(ALL_SRCS)) source files)
$(info Collected $(words $(ALL_INCS)) include paths)
$(info ========================================)

export C_SRCS
export S_SRCS
export C_OBJS
export S_OBJS
export LINK_S_OBJS
export LINK_C_OBJS
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

# ============================================================================
# 主目标
# ============================================================================
.PHONY:all

all: $(ANALYSIS_FILES)


.PHONY: reall
reall: clean all

# ============================================================================
# clang-format 配置
# ============================================================================

CLANG_FORMAT ?= clang-format

# 只扫描项目自己维护的 C/C++ 源文件。
FORMAT_FILES := $(shell find src include tests \
	-type f \
	\( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' \) \
	-not -path '*/cmsis/*' \
	-not -path '*/third_party/*' \
	)

.PHONY: format format-check format-version

# 原地格式化项目代码。
format:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(CLANG_FORMAT)"; \
		exit 1; \
	}
	@echo "Formatting $(words $(FORMAT_FILES)) files..."
	@$(CLANG_FORMAT) -i $(FORMAT_FILES)
	@echo "Format complete."

# 只检查格式，不修改文件。
# --dry-run 和 --Werror 需要较新的 clang-format。
format-check:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(CLANG_FORMAT)"; \
		exit 1; \
	}
	@echo "Checking source format..."
	@$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)
	@echo "Format check passed."

# 查看实际使用的clang-format版本。
format-version:
	@$(CLANG_FORMAT) --version

# ============================================================================
# 信息目标
# ============================================================================
.PHONY: info
info:
	@echo "=== Build Configuration ==="
	@echo "  BUILD_TARGET    : $(BUILD_TARGET)"
	@echo "  FIRMWARE_PLATFORM: $(FIRMWARE_PLATFORM)"
	@echo "  BUILD_TYPE    : $(BUILD_TYPE)"
	@echo "  TARGET        : $(TARGET)"
	@echo "  MODULES       : $(MODULES)"
	@echo "  ALL_LIBS       :"
	@printf '    %s\n' $(ALL_LIBS)
	@echo "  ALL_OBJS count: $(words $(ALL_OBJS))"
	@echo ""
	@echo "  ALL_INCS      :"
	@printf '    %s\n' $(ALL_INCS)
	@echo ""
	@echo "  ALL_SRCS      :"
	@printf '    %s\n' $(ALL_SRCS)
	@echo ""
	@echo "  CFLAGS        :"
	@printf '    %s\n' $(CFLAGS)
	@echo ""
	@echo "  LDFLAGS       :"
	@printf '    %s\n' $(LDFLAGS)
	@echo ""
	@echo "  LDLIBS        : $(LDLIBS)"
	@echo "  CC            : $(CC)"
	@echo "  AR            : $(AR)"
	@echo ""
	@echo "=== Analysis Files ==="
	@echo "  BIN file      : $(TARGET).bin"
	@echo "  MAP file      : $(TARGET).map"
	@echo "  SYM file      : $(TARGET).sym"
	@echo "  SIZE file     : $(TARGET).size"
	@echo "  LST file      : $(TARGET).lst"
	@echo "  Preprocess    : $(words $(ALL_I_FILES)) .i files"
	@echo "  Disassembly   : $(words $(ALL_LST_FILES)) .lst files"

# ============================================================================
#  帮助信息
# ============================================================================
.PHONY: help
help:
	@echo "============================================================"
	@echo "  DS Library Build System"
	@echo "============================================================"
	@echo ""
	@echo "  Build Targets:"
	@echo "    all          - Build default (debug) target"
	@echo "    debug        - Build debug version"
	@echo "    release      - Build release version"
	@echo "    asan         - Build with AddressSanitizer"
	@echo ""
	@echo "  Analysis Targets:"
	@echo "    preprocess   - Generate .i files (preprocessed output)"
	@echo "    disassemble  - Generate .lst files (disassembly listing)"
	@echo "    build-with-log - Build with error log (build.log)"
	@echo ""
	@echo "  Cleanup Targets:"
	@echo "    clean        - Remove build/$(BUILD_TYPE) directory"
	@echo "    distclean    - Remove entire build/ directory"
	@echo ""
	@echo "  Info Targets:"
	@echo "    info         - Show build configuration"
	@echo "    help         - Show this help"
	@echo ""
	@echo "  Generated Files:"
	@echo "    $(TARGET).map  - Memory map"
	@echo "    $(TARGET).sym  - Symbol table"
	@echo "    $(TARGET).size - Size summary"
	@echo "    $(TARGET).lst  - Disassembly listing"
	@echo "    $(OBJ_DIR)/*.i - Preprocessed files"
	@echo "    $(OBJ_DIR)/*.lst - Per-file disassembly"
	@echo "    $(BUILD_DIR)/build.log - Build error log"
	@echo ""
	@echo "  Variables:"
	@echo "    BUILD_TYPE   - debug, release, asan (default: debug)"
	@echo "    V=1          - Show full compile commands"
	@echo "    CROSS_COMPILE- Cross-compilation prefix"
	@echo "    COVERAGE=1   - Enable code coverage (gcov)"
	@echo ""
	@echo "  Examples:"
	@echo "    make debug V=1              # Build debug with verbose output"
	@echo "    make release                # Build optimized release"
	@echo "    make COVERAGE=1             # Build with coverage support"
	@echo "    make preprocess             # Generate .i files"
	@echo "    make disassemble            # Generate disassembly listings"
	@echo "============================================================"
