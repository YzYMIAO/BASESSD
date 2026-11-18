# ============================================================================
# 顶层 Makefile - 固件构建入口
# ============================================================================
# 设计原则：
#   1. 只负责解析配置、收集模块声明、定义入口目标
#   2. 编译规则在 rules_common.mk，链接规则在 rules_link.mk
#   3. 支持 debug / release 两种构建类型
#   4. 本工程按串行 make 设计（format / check-config 必须先于编译）
# ============================================================================
.DEFAULT_GOAL := all

# format、check-config、clean 与编译/链接之间存在先后约束，-j 下无法表达。
# 工程规模不需要并行构建，直接全局串行，避免「格式化与编译并发」「clean 与 all 并发」。
.NOTPARALLEL:

# ----------------------------------------------------------------------------
# 1. 包含配置
# ----------------------------------------------------------------------------
PROJECT_ROOT := $(CURDIR)
FIRMWARE_PLATFORM ?= stm32h743
BUILD_TYPE ?= debug

# ----------------------------------------------------------------------------
# 1.5 单元测试范围选择（仅 `make tests` 有意义）
#   UT=（空）  全部：FC-NAND 三级 + FTL pure/media + HIL + 两条 speed
#   UT=FC      FC-NAND 三级 + /speed/fc_nand_rw（破坏性）
#   UT=FTL     FTL pure + media + /speed/ftl_rw（真 NAND，破坏性）
#   UT=HIL     仅 HIL 板端（/hil/；真 NAND/FTL，破坏性，不含 speed）
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
#   make format-image            → build/<type>_fmt/bin/，每次启动先擦全片
#   make format-image FORMAT_VERIFY=1  → 开卡时追加全片擦除体检（擦除成功即好块）
#   用途：介质账本已坏、开机 full metadata rebuild 太慢时，先烧这张镜像快速开卡；
#         开卡完成后**必须换回正常镜像**——这张镜像每次上电都会清数据。
#   **bin 输出路径与正常镜像相同**（build/<type>/bin/debug.bin，烧录配置不用改）；
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
# C objects that must bypass static archives during the final link.
LINK_C_OBJS :=

# 3. 包含所有子模块的声明文件（在解析阶段完成收集）
#    这一步会将子模块中的 MODULE_SRCS += xxx 追加进来
ifeq ($(V),1)
$(info ========================================)
$(info Including module makefiles...)
endif
include $(foreach mod,$(MODULES),$(SRC_ROOT)/$(mod)/makefile.mk)
# 测试构建声明统一由 tests/makefile.mk 负责（源、wrap、测试 include、UT 宏）。
include $(PROJECT_ROOT)/tests/makefile.mk
ifeq ($(V),1)
$(info All modules included)
$(info ========================================)
endif

# 根目录 main.c 是固件应用入口，不归入静态库，必须在链接时显式加入。
APP_C_SRCS := $(SRC_ROOT)/main.c
C_SRCS := $(sort $(MODULE_SRCS) $(APP_C_SRCS))
ALL_SRCS := $(C_SRCS) $(THIRD_PARTY_C_SRCS) $(THIRD_PARTY_S_SRCS) $(TEST_C_SRCS)

C_OBJS := $(patsubst $(SRC_ROOT)/%.c,$(OBJ_DIR)/%.o,$(C_SRCS))
THIRD_PARTY_C_OBJS := $(patsubst $(PROJECT_ROOT)/%.c,$(VENDOR_OBJ_DIR)/%.o,$(THIRD_PARTY_C_SRCS))
THIRD_PARTY_S_OBJS := $(patsubst $(PROJECT_ROOT)/%.s,$(VENDOR_OBJ_DIR)/%.o,$(THIRD_PARTY_S_SRCS))
TEST_C_OBJS := $(patsubst $(PROJECT_ROOT)/%.c,$(OBJ_DIR)/%.o,$(TEST_C_SRCS))
# 固件自有源码必须按对象直链：强符号中断处理才能覆盖 startup 的弱符号默认实现。
LINK_C_OBJS := $(C_OBJS) $(TEST_C_OBJS)
LINK_S_OBJS := $(THIRD_PARTY_S_OBJS)

ALL_OBJS := $(C_OBJS) $(THIRD_PARTY_C_OBJS) $(THIRD_PARTY_S_OBJS) $(TEST_C_OBJS)

ALL_INCS := $(sort $(MODULE_INCS))
CFLAGS += $(addprefix -I,$(ALL_INCS))
CFLAGS += -DSSD_FIRMWARE_BUILD
CFLAGS += $(CFLAGS_EXTRA)

ifeq ($(V),1)
$(info ========================================)
$(info Collected $(words $(ALL_SRCS)) source files)
$(info Collected $(words $(ALL_INCS)) include paths)
ifeq ($(FC_NAND_TEST_ENABLE),1)
$(info UT scope: $(if $(UT),$(UT),ALL)  [FC suites: $(if $(UT_FC_ENABLE),on,off) / FTL suites: $(if $(UT_FTL_ENABLE),on,off)])
endif
$(info ========================================)
endif

# ----------------------------------------------------------------------------
# 构建配置指纹
#   本工程没有「CFLAGS 变化 ⇒ 重编译」依赖：开关若只改编译选项而不改对象目录，
#   会静默复用旧 .o。OBJ_DIR 已按 UT / BUILD_TYPE / NAND_DEVICE / DEBUG_OPT /
#   FORMAT 分置；这里再对最终 CFLAGS / 工具链 / 链接脚本做指纹，任何未纳入
#   目录名的配置变化（CFLAGS_EXTRA、CROSS_COMPILE、新增 -D 等）都会清空当前
#   对象目录，不必再为每个开关手工加后缀。
# ----------------------------------------------------------------------------
CONFIG_STAMP     := $(OBJ_DIR)/.build_config
CONFIG_SIGNATURE := $(CC) | $(CFLAGS) | $(ASFLAGS) | $(LDFLAGS) | $(LINKER_SCRIPT)

.PHONY: check-config
check-config:
	@$(MKDIR) $(OBJ_DIR)
	@cur="$(CONFIG_SIGNATURE)"; \
	if [ ! -f "$(CONFIG_STAMP)" ]; then \
		printf '%s\n' "$$cur" > "$(CONFIG_STAMP)"; \
	elif [ "$$(cat "$(CONFIG_STAMP)")" != "$$cur" ]; then \
		$(ECHO) "build config changed -> clearing $(OBJ_DIR)"; \
		$(RMDIR) $(OBJ_DIR); \
		$(MKDIR) $(OBJ_DIR); \
		printf '%s\n' "$$cur" > "$(CONFIG_STAMP)"; \
	fi

# ----------------------------------------------------------------------------
# 第三方对象共享（obj_vendor）
#   vendor 代码只依赖构建档、优化档、公共头与平台公共宏；不依赖
#   -DUT_* / -DFC_NAND_TEST_ENABLE / FORMAT / NAND_DEVICE 这些项目级开关。
#   因此产品与所有 UT scope 共用一份 HAL/TinyUSB 对象和一份 libthird_party.a，
#   切换 UT 时不重编第三方。指纹只覆盖 VENDOR_CFLAGS，不受 UT 宏影响。
# ----------------------------------------------------------------------------
VENDOR_CFLAGS    := $(BASE_FLAGS) $(OPT_FLAGS) $(DBG_FLAGS) \
                    $(addprefix -I,$(ALL_INCS)) \
                    $(PLATFORM_COMMON_DEFINES) \
                    $(THIRD_PARTY_CFLAGS) \
                    $(CFLAGS_EXTRA)
VENDOR_STAMP     := $(VENDOR_OBJ_DIR)/.build_config
VENDOR_SIGNATURE := $(CC) | $(VENDOR_CFLAGS) | $(ASFLAGS)

.PHONY: check-vendor-config
check-vendor-config:
	@$(MKDIR) $(VENDOR_OBJ_DIR)
	@cur="$(VENDOR_SIGNATURE)"; \
	if [ ! -f "$(VENDOR_STAMP)" ]; then \
		printf '%s\n' "$$cur" > "$(VENDOR_STAMP)"; \
	elif [ "$$(cat "$(VENDOR_STAMP)")" != "$$cur" ]; then \
		$(ECHO) "vendor config changed -> clearing $(VENDOR_OBJ_DIR)"; \
		$(RMDIR) $(VENDOR_OBJ_DIR); \
		$(MKDIR) $(VENDOR_OBJ_DIR); \
		printf '%s\n' "$$cur" > "$(VENDOR_STAMP)"; \
	fi

# ============================================================================
# 7. 包含编译和链接规则（明确分开包含）
# ============================================================================
include $(PROJECT_ROOT)/rules_common.mk   # 编译规则（.c -> .o）
include $(PROJECT_ROOT)/rules_link.mk     # 链接规则（.o -> 可执行文件）

# ----------------------------------------------------------------------------
# 8. 快捷构建类型切换
# ----------------------------------------------------------------------------
.PHONY: release tests
release:
	$(MAKE) BUILD_TYPE=release

# 单元/集成测试构建。UT=（空）编入全部套件；UT=FC 仅 FC-NAND 三级；
# UT=FTL 为 FTL pure + media；UT=HIL 仅 HIL 板端。PAA 在烧录后由串口命令
# nand test <PAA> 显式传入。
tests:
	$(MAKE) FIRMWARE_PLATFORM=$(FIRMWARE_PLATFORM) BUILD_TYPE=$(BUILD_TYPE) FC_NAND_TEST_ENABLE=1 UT=$(UT)

# 开卡镜像（破坏性）：FORMAT_ON_BOOT=1 ⇒ 每次启动擦全片 + 写初始元数据。
# bin 路径与正常镜像相同（烧录配置不用改）；对象目录带 _fmt 后缀隔离。
# 开卡后换回正常镜像（make all）。
.PHONY: format-image
format-image:
	$(MAKE) all FIRMWARE_PLATFORM=$(FIRMWARE_PLATFORM) BUILD_TYPE=$(BUILD_TYPE) FORMAT_ON_BOOT=1 FORMAT_VERIFY=$(FORMAT_VERIFY)

# ============================================================================
# 主目标
# ============================================================================
.PHONY: check-nocache
# PLAT-01：DMA 页缓冲必须落在 AXI_NOCACHE 段（0x24060000..0x24080000）。
check-nocache:
	@elf=$(TARGET).elf; \
	line=$$($(SIZE) -A "$$elf" | grep -w '.noncacheable_buffer' | head -1); \
	[ -n "$$line" ] || { echo "check-nocache: .noncacheable_buffer missing"; exit 1; }; \
	size=$$(echo "$$line" | awk '{print $$2}'); addr=$$(echo "$$line" | awk '{print $$3}'); \
	python3 -c "import sys; a=int('$$addr',0); n=int('$$size',0); sys.exit(0 if (a>=0x24060000 and a<0x24080000 and n>0) else 1)" || { \
		echo "check-nocache: bad placement addr=$$addr size=$$size"; exit 1; }; \
	echo "check-nocache: OK (addr=$$addr size=$$size)"

.PHONY: check-opt
# BUILD-01：非默认 DEBUG_OPT 必须落到独立对象目录并带对应 -O 标志。
# 只做 make -n（不编译），防止「-O0 构建静默复用 -O2 对象」再次发生。
check-opt:
	@out=$$($(MAKE) -n --no-print-directory tests UT=FC DEBUG_OPT=-O0 2>&1) || { \
		echo "check-opt: make -n failed"; echo "$$out"; exit 1; }; \
	echo "$$out" | grep -q 'obj_ut_FC_O0' || { echo "check-opt: missing obj_ut_FC_O0"; exit 1; }; \
	echo "$$out" | grep -q -- ' -O0' || { echo "check-opt: missing -O0 in compile flags"; exit 1; }; \
	out=$$($(MAKE) -n --no-print-directory tests UT=FC DEBUG_OPT=-O2 2>&1) || { \
		echo "check-opt: make -n (default) failed"; echo "$$out"; exit 1; }; \
	echo "$$out" | grep -q 'obj_ut_FC_O2' && { echo "check-opt: default -O2 must not use _O2 suffix"; exit 1; }; \
	echo "check-opt: OK (DEBUG_OPT affects object dir and compile flags)"

.PHONY: all
# format / types-check 作为 all 的前置目标（均为 .PHONY，恒执行）：
#   format      —— 每次构建先强制格式化，保证产物与源码一致；
#   types-check —— H50 正向不变量检查（公共完整类型必须定义处 typedef 等）。
# 串行 make 下二者先于编译跑；干净树时 clang-format 不改 mtime（零开销、不触发
# 重编译）。全局串行由顶部的 .NOTPARALLEL 保证。
all: check-config check-vendor-config format types-check $(ANALYSIS_FILES)

# ============================================================================
# 聚合检查：格式 → H50 类型 → H41 头文件 → debug 镜像 → 布局/优化档守卫。
# 可独立运行；已在 make all 之后运行时前几步是幂等的重复。
# ============================================================================
.PHONY: check
check: format-check types-check types-test headers-check $(TARGET).elf check-nocache check-opt

# ============================================================================
# clang-format 配置
# ============================================================================
CLANG_FORMAT ?= clang-format-15

# 只扫描项目自己维护的 C/C++ 源文件。
FORMAT_FILES := $(shell find src include tests \
	-type f \
	\( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' \) \
	-not -path '*/third_party/*' \
)

.PHONY: format format-check

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

# ============================================================================
# 编辑 / 静态分析辅助（不挂 check；依赖外部工具）
# ============================================================================
.PHONY: compile-commands tidy

# 从 `make -B -n V=1 all` 的编译命令行重建 compile_commands.json（.vscode /
# clangd 使用）。不依赖 bear，也不实际编译；工具本身在 tools/gen_compile_commands.py。
compile-commands:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(PYTHON)"; \
		exit 1; \
	}
	@$(PYTHON) $(PROJECT_ROOT)/tools/gen_compile_commands.py --root $(PROJECT_ROOT)

# clang-tidy 与 .clang-format 同用 v15；依赖 compile_commands.json。
# 不挂 check：CI 不装 clang-tidy，版本漂移也不应阻塞构建。
TIDY_RUNNER ?= run-clang-tidy-15
tidy: compile-commands
	@command -v $(TIDY_RUNNER) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(TIDY_RUNNER)"; \
		exit 1; \
	}
	@$(TIDY_RUNNER) -p $(PROJECT_ROOT) '^$(PROJECT_ROOT)/src/.*\.c$$'

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

# 检查器自检（P13/C7）：正例必须通过，每条负例必须报错。
# 区分力证据在 tools/test_check_types.py，不依赖 pytest。
.PHONY: types-test
types-test:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(PYTHON)"; \
		exit 1; \
	}
	@$(PYTHON) $(PROJECT_ROOT)/tools/test_check_types.py

# ============================================================================
# H41 头文件检查
#   - 项目头一律裸名：目录前缀 include 会因 -I 顺序产生静默误解析；
#   - include/ 与 src/ 的私有头不得同名（同一裸名两套语义）。
# 第三方位（third_party/）按 H41 豁免，不扫描。
# ============================================================================
.PHONY: headers-check
headers-check:
	@if grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"[^"]*/' src include tests; then \
		echo "headers-check: directory-prefixed include found (H41)"; \
		exit 1; \
	fi
	@dups=$$(find include src -name '*.h' | sed 's|.*/||' | sort | uniq -d); \
	if [ -n "$$dups" ]; then \
		echo "headers-check: duplicate header basenames:"; \
		echo "$$dups"; \
		exit 1; \
	fi
	@echo "headers-check: OK"

# ============================================================================
# 信息目标
# ============================================================================
.PHONY: info
info:
	@echo "=== Build Configuration ==="
	@echo "  FIRMWARE_PLATFORM: $(FIRMWARE_PLATFORM)"
	@echo "  BUILD_TYPE       : $(BUILD_TYPE)"
	@echo "  NAND_DEVICE      : $(NAND_DEVICE)"
	@echo "  UT               : $(if $(UT),$(UT),ALL)"
	@echo "  OBJ_DIR          : $(OBJ_DIR)"
	@echo "  VENDOR_OBJ_DIR   : $(VENDOR_OBJ_DIR)"
	@echo "  TARGET (elf)     : $(TARGET).elf"
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

# ============================================================================
# 帮助信息
# ============================================================================
.PHONY: help
help:
	@echo "============================================================"
	@echo "  soft_ssd firmware build system"
	@echo "============================================================"
	@echo ""
	@echo "  Build Targets:"
	@echo "    all            - Build default (debug) target"
	@echo "    release        - Build release version"
	@echo "    tests          - Build firmware with UART NAND test commands"
	@echo "    format-image   - Build **destructive** open-card image (formats on EVERY boot)"
	@echo ""
	@echo "  Tooling Targets (optional, external tools):"
	@echo "    compile-commands - Regenerate compile_commands.json (from make -n)"
	@echo "    tidy             - Run clang-tidy on src/ (needs run-clang-tidy-15)"
	@echo ""
	@echo "  Check Targets:"
	@echo "    check          - format / types / headers / layout / opt checks"
	@echo "    format-check   - Verify clang-format without modifying files"
	@echo "    types-test     - Self-test for types-check (positive + negative cases)"
	@echo "    headers-check  - H41: bare includes and unique header basenames"
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
	@echo "    BUILD_TYPE     - debug, release (default: debug)"
	@echo "    UT             - unit test scope for 'tests': (empty)=all, FC, FTL, HIL"
	@echo "    FORMAT_VERIFY  - with format-image: 1 = full-chip erase check (erase OK = good block)"
	@echo "    V=1            - Show full compile commands"
	@echo "    CROSS_COMPILE  - Cross-compilation prefix"
	@echo "============================================================"
