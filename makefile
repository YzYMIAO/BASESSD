# ============================================================================
# 顶层 Makefile - 企业级构建入口
# ============================================================================
# 设计原则：
#   1. 只负责解析配置、调用 rules.mk
#   2. 所有编译规则由 rules.mk 管理
#   3. 支持多构建类型切换（debug/release/asan）
# ============================================================================
.DEFAULT_GOAL := all

# ----------------------------------------------------------------------------
# 1. 包含配置
# ----------------------------------------------------------------------------
PROJECT_ROOT := $(CURDIR)
BUILD_TARGET ?= firmware
FIRMWARE_PLATFORM ?= stm32h743
BUILD_TYPE ?= debug

# ----------------------------------------------------------------------------
# 1.5 单元测试范围选择（仅 `make tests` 有意义）
#   UT=（空）  全部：FC-NAND 三级 + FTL L0 + HIL 板端
#   UT=FC      仅 FC-NAND 三级（/fc-nand/logic|data_path|lifecycle）
#   UT=FTL     仅 FTL L0（/ftl/… 8 套件；纯 RAM、不碰 NAND）
#   UT=HIL     仅 HIL 板端（/hil/；真 NAND/FTL，破坏性）
# 每种取值编入**不同的测试源集合**，并落到**不同的对象目录**（见 config.mk 的 OBJ_DIR）
# ⇒ 切换 UT 无需 make clean，也不会复用带不同 -D 的陈旧对象（否则 ut_main.o 会引用
#   未编入的套件数组 ⇒ 链接期 undefined symbol）。
# ----------------------------------------------------------------------------
UT ?=
ifeq ($(UT),)
    UT_FC_ENABLE  := 1
    UT_FTL_ENABLE := 1
    UT_HIL_ENABLE := 1
else ifeq ($(UT),FC)
    UT_FC_ENABLE  := 1
else ifeq ($(UT),FTL)
    UT_FTL_ENABLE := 1
else ifeq ($(UT),HIL)
    UT_HIL_ENABLE := 1
else
    $(error Invalid UT=$(UT), valid values: (empty), FC, FTL, HIL)
endif

# ----------------------------------------------------------------------------
# 1.6 开卡镜像（**破坏性**）
#   make format-image            → build/firmware/<type>_fmt/bin/，每次启动先擦全片
#   make format-image FORMAT_VERIFY=1  → 开卡时追加全片擦除体检（擦除成功即好块）
#   用途：介质账本已坏、开机 full metadata rebuild 太慢时，先烧这张镜像快速开卡；
#         开卡完成后**必须换回正常镜像**——这张镜像每次上电都会清数据。
#   **bin 输出路径与正常镜像相同**（build/firmware/<type>/bin/debug.bin，烧录配置不用改）；
#   只有对象/库/依赖目录带 _fmt 后缀，避免复用陈旧 .o，无需 make clean。
# ----------------------------------------------------------------------------
FORMAT_ON_BOOT ?= 0
FORMAT_VERIFY  ?= 0

ifeq ($(filter $(FORMAT_ON_BOOT),0 1),)
    $(error Invalid FORMAT_ON_BOOT=$(FORMAT_ON_BOOT), valid: 0/1)
endif
ifeq ($(filter $(FORMAT_VERIFY),0 1),)
    $(error Invalid FORMAT_VERIFY=$(FORMAT_VERIFY), valid: 0/1)
endif

include $(PROJECT_ROOT)/config.mk

# ============================================================================
# 关键修正：在包含 rules.mk 之前，先收集所有子模块的源文件
# ============================================================================
THIRD_PARTY_C_SRCS :=
THIRD_PARTY_S_SRCS :=
TEST_C_SRCS :=
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
	S_SRCS := $(sort $(PLATFORM_S_SRCS))
	ALL_SRCS := $(C_SRCS) $(S_SRCS) $(THIRD_PARTY_C_SRCS) $(THIRD_PARTY_S_SRCS) $(TEST_C_SRCS)

APP_OBJS := $(patsubst $(SRC_ROOT)/%.c,$(OBJ_DIR)/%.o,$(APP_C_SRCS))
C_OBJS := $(patsubst $(SRC_ROOT)/%.c,$(OBJ_DIR)/%.o,$(C_SRCS))
S_OBJS := $(patsubst $(SRC_ROOT)/%.s,$(OBJ_DIR)/%.o,$(S_SRCS))
	THIRD_PARTY_C_OBJS := $(patsubst $(PROJECT_ROOT)/%.c,$(OBJ_DIR)/%.o,$(THIRD_PARTY_C_SRCS))
	THIRD_PARTY_S_OBJS := $(patsubst $(PROJECT_ROOT)/%.s,$(OBJ_DIR)/%.o,$(THIRD_PARTY_S_SRCS))
	TEST_C_OBJS := $(patsubst $(PROJECT_ROOT)/%.c,$(OBJ_DIR)/%.o,$(TEST_C_SRCS))
	# Application-owned sources must be linked as objects. In particular, this
	# allows strong interrupt handlers to override weak startup defaults.
	LINK_C_OBJS := $(C_OBJS) $(TEST_C_OBJS)
	LINK_S_OBJS := $(S_OBJS) $(THIRD_PARTY_S_OBJS)

	ALL_OBJS := $(C_OBJS) $(S_OBJS) $(THIRD_PARTY_C_OBJS) $(THIRD_PARTY_S_OBJS) $(TEST_C_OBJS)

ALL_INCS := $(sort $(MODULE_INCS))
CFLAGS += $(addprefix -I,$(ALL_INCS))
CFLAGS += -DSSD_FIRMWARE_BUILD
CFLAGS += -DDEBUG_FTL
CFLAGS += $(CFLAGS_EXTRA)
else
C_SRCS := $(sort $(MODULE_SRCS))
ALL_SRCS := $(C_SRCS)

C_OBJS := $(patsubst $(SRC_ROOT)/%.c,$(OBJ_DIR)/%.o,$(C_SRCS))
ALL_OBJS := $(C_OBJS)

ALL_INCS := $(sort $(MODULE_INCS))
CFLAGS += $(addprefix -I,$(ALL_INCS))
endif

# 导出收集结果（供调试和 rules.mk 使用）
$(info ========================================)
$(info Collected $(words $(ALL_SRCS)) source files)
$(info Collected $(words $(ALL_INCS)) include paths)
ifeq ($(FC_NAND_TEST_ENABLE),1)
$(info UT scope: $(if $(UT),$(UT),ALL)  [FC suites: $(if $(UT_FC_ENABLE),on,off) / FTL suites: $(if $(UT_FTL_ENABLE),on,off)])
endif
$(info ========================================)

export C_SRCS
export S_SRCS
export C_OBJS
export S_OBJS
export LINK_S_OBJS
export LINK_C_OBJS
export ALL_OBJS
export TEST_C_OBJS
export UT

# ============================================================================
# 7. 包含编译和链接规则（明确分开包含）
# ============================================================================
include $(PROJECT_ROOT)/rules_common.mk   # 编译规则（.c -> .o）
include $(PROJECT_ROOT)/rules_link.mk     # 链接规则（.o -> 可执行文件）

# ----------------------------------------------------------------------------
# 8. 快捷构建类型切换
# ----------------------------------------------------------------------------
.PHONY: debug release asan tests
debug:
	$(MAKE) BUILD_TYPE=debug

release:
	$(MAKE) BUILD_TYPE=release

asan:
	$(MAKE) BUILD_TYPE=asan

# 单元/集成测试构建。UT=（空）编入全部套件；UT=FC 仅 FC-NAND 三级；UT=FTL 仅 FTL L0。
# PAA 在烧录后由串口命令 nand test <PAA> 显式传入。
tests:
	$(MAKE)  BUILD_TARGET=firmware FIRMWARE_PLATFORM=$(FIRMWARE_PLATFORM) BUILD_TYPE=$(BUILD_TYPE) FC_NAND_TEST_ENABLE=1 UT=$(UT) CFLAGS_EXTRA=-DMUNIT_ENABLE_TIMING

# 开卡镜像（破坏性）：FORMAT_ON_BOOT=1 ⇒ 每次启动擦全片 + 写初始元数据。
# bin 路径与正常镜像相同（烧录配置不用改）；对象目录带 _fmt 后缀隔离。
# 开卡后换回正常镜像（make all）。
.PHONY: format-image
format-image:
	$(MAKE) all BUILD_TARGET=firmware FIRMWARE_PLATFORM=$(FIRMWARE_PLATFORM) BUILD_TYPE=$(BUILD_TYPE) FORMAT_ON_BOOT=1 FORMAT_VERIFY=$(FORMAT_VERIFY)

# ============================================================================
# 主目标
# ============================================================================
# 共用 bin 路径时，正常镜像 / 开卡镜像 / 体检镜像的 ELF/BIN **不能互相复用**：
# 记录上次构建的 (FORMAT_ON_BOOT/FORMAT_VERIFY)；配置切换时删掉旧输出，强制重链。
.PHONY: check-build-cfg
check-build-cfg:
	@$(MKDIR) $(BIN_DIR)
	@cfg="$(FORMAT_ON_BOOT)/$(FORMAT_VERIFY)"; \
	if [ -f "$(TARGET).elf" ] && { [ ! -f "$(BIN_DIR)/.build_cfg" ] || [ "$$(cat "$(BIN_DIR)/.build_cfg")" != "$$cfg" ]; }; then \
		rm -f "$(TARGET).elf" "$(TARGET).bin" "$(TARGET).map" "$(TARGET).lst" "$(TARGET).sym" "$(TARGET).size"; \
	fi; \
	printf '%s\n' "$$cfg" > "$(BIN_DIR)/.build_cfg"

.PHONY: all
# format / types-check 作为 all 的前置目标（均为 .PHONY，恒执行）：
#   format      —— 每次构建先强制格式化，保证产物与源码一致；
#   types-check —— H50 正向不变量检查（公共完整类型必须定义处 typedef 等）。
# 串行 make 下二者先于编译跑；干净树时 clang-format 不改 mtime（零开销、不触发
# 重编译）。注意：本工程按串行 make 构建；若用 -j，format 会与编译并发，需改为在
# 编译规则上挂 order-only 前置（见 rules_common.mk）。
all: check-build-cfg format types-check $(ANALYSIS_FILES)

.PHONY: reall
reall: clean all

# ============================================================================
# clang-format 配置
# ============================================================================
CLANG_FORMAT ?= clang-format-15

# 只扫描项目自己维护的 C/C++ 源文件。
FORMAT_FILES := $(shell find src include tests \
	-type f \
	\( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' \) \
	-not -path '*/cmsis/*' \
	-not -path '*/third_party/*' \
)

.PHONY: format format-check format-version

FORMAT_CONFIG := .clang-format
# FORMAT_CONFIG := .clang-format_array

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

# 查看实际使用的 clang-format 版本。
format-version:
	@$(CLANG_FORMAT) --version

# ============================================================================
# H50 复合类型定义形态检查
# ============================================================================
PYTHON ?= python3
TYPES_CHECK := $(PROJECT_ROOT)/tools/check_types.py

.PHONY: types-check
types-check:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(PYTHON)"; \
		exit 1; \
	}
	@$(PYTHON) $(TYPES_CHECK) --root $(PROJECT_ROOT)

# ============================================================================
# 信息目标
# ============================================================================
.PHONY: info
info:
	@echo "=== Build Configuration ==="
	@echo "  BUILD_TARGET     : $(BUILD_TARGET)"
	@echo "  FIRMWARE_PLATFORM: $(FIRMWARE_PLATFORM)"
	@echo "  BUILD_TYPE       : $(BUILD_TYPE)"
	@echo "  NAND_DEVICE      : $(NAND_DEVICE)"
	@echo "  UT               : $(if $(UT),$(UT),ALL)"
	@echo "  OBJ_DIR          : $(OBJ_DIR)"
	@echo "  TARGET           : $(TARGET)"
	@echo "  MODULES          : $(MODULES)"
	@echo "  ALL_LIBS         :"
	@printf '    %s\n' $(ALL_LIBS)
	@echo "  ALL_OBJS count   : $(words $(ALL_OBJS))"
	@echo ""
	@echo "  ALL_INCS         :"
	@printf '    %s\n' $(ALL_INCS)
	@echo ""
	@echo "  ALL_SRCS         :"
	@printf '    %s\n' $(ALL_SRCS)
	@echo ""
	@echo "  CFLAGS           :"
	@printf '    %s\n' $(CFLAGS)
	@echo ""
	@echo "  LDFLAGS          :"
	@printf '    %s\n' $(LDFLAGS)
	@echo ""
	@echo "  LDLIBS           : $(LDLIBS)"
	@echo "  CC               : $(CC)"
	@echo "  AR               : $(AR)"
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
# 帮助信息
# ============================================================================
.PHONY: help
help:
	@echo "============================================================"
	@echo "  DS Library Build System"
	@echo "============================================================"
	@echo ""
	@echo "  Build Targets:"
	@echo "    all            - Build default (debug) target"
	@echo "    debug          - Build debug version"
	@echo "    release        - Build release version"
	@echo "    asan           - Build with AddressSanitizer"
	@echo "    tests          - Build firmware with UART NAND test commands"
	@echo "    format-image   - Build **destructive** open-card image (formats on EVERY boot)"
	@echo ""
	@echo "  Analysis Targets:"
	@echo "    preprocess     - Generate .i files"
	@echo "    disassemble    - Generate .lst files"
	@echo "    build-with-log - Build with error log"
	@echo ""
	@echo "  Cleanup Targets:"
	@echo "    clean          - Remove build/$(BUILD_TYPE) directory"
	@echo "    distclean      - Remove entire build/ directory"
	@echo ""
	@echo "  Info Targets:"
	@echo "    info           - Show build configuration"
	@echo "    help           - Show this help"
	@echo ""
	@echo "  Variables:"
	@echo "    BUILD_TYPE     - debug, release, asan (default: debug)"
	@echo "    UT             - unit test scope for 'tests': (empty)=all, FC, FTL, HIL"
	@echo "    FORMAT_VERIFY  - with format-image: 1 = full-chip erase check (erase OK = good block)"
	@echo "    V=1            - Show full compile commands"
	@echo "    CROSS_COMPILE  - Cross-compilation prefix"
	@echo "    COVERAGE=1     - Enable code coverage (gcov)"
	@echo "============================================================"
