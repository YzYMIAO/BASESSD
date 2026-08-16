# ============================================================================
# rules.mk - 企业级链接规则（增强版）
# ============================================================================
# 功能：
#   1. 每个模块生成静态库（.a）
#   2. 链接可执行文件（.elf/.out）
#   3. 生成内存映射文件（.map）
#   4. 生成符号表（.sym）
#   5. 生成大小汇总表（.size）
#   6. 生成反汇编列表（.lst）
#   7. 错误日志
# ============================================================================
$(eval $(COMPILE_template))
# ----------------------------------------------------------------------------
# 1. 为每个模块收集目标文件
# ----------------------------------------------------------------------------
define MODULE_OBJS_template
OBJS_$(1) = $$(filter $$(OBJ_DIR)/$(1)/%.o, $$(ALL_OBJS))
endef
$(foreach mod,$(MODULES),$(eval $(call MODULE_OBJS_template,$(mod))))

# ----------------------------------------------------------------------------
# 2. 为每个模块生成静态库
# ----------------------------------------------------------------------------
define BUILD_LIB_template
$(LIB_DIR)/lib$(1).a: $$(OBJS_$(1))
	@$$(MKDIR) $$(LIB_DIR)
	@$$(ECHO) "Archiving lib$(1).a..."
	$$(Q)$$(AR) rcs $$@ $$^
endef
$(foreach mod,$(MODULES),$(eval $(call BUILD_LIB_template,$(mod))))

# ----------------------------------------------------------------------------
# 3. 收集所有库文件
# ----------------------------------------------------------------------------
ALL_LIBS = $(foreach mod,$(MODULES),$(LIB_DIR)/lib$(mod).a)

# ----------------------------------------------------------------------------
# 4. 链接可执行文件（生成 .map 文件）
# ----------------------------------------------------------------------------
# TARGET 会包含 .elf 后缀（便于区分）
TARGET_ELF = $(TARGET).elf

$(TARGET_ELF): $(ALL_LIBS)
	@$(MKDIR) $(BIN_DIR)
	@$(ECHO) "Linking $(BUILD_TYPE) version..."
	$(Q)$(CC) $(LDFLAGS) -Wl,-Map=$(TARGET).map $^ $(LDLIBS) -o $@
	@$(ECHO) "Build complete: $@"

# 创建无后缀的软链接（兼容原有习惯）
$(TARGET): $(TARGET_ELF)
	@$(LN) -sf $(notdir $<) $@

# ----------------------------------------------------------------------------
# 5. 生成符号表（.sym）
# ----------------------------------------------------------------------------
$(TARGET).sym: $(TARGET_ELF)
	@$(ECHO) "Generating symbol table..."
	$(Q)nm -n $< > $@

# ----------------------------------------------------------------------------
# 6. 生成大小汇总表（.size）
# ----------------------------------------------------------------------------
$(TARGET).size: $(TARGET_ELF)
	@$(ECHO) "Generating size summary..."
	$(Q)size -A $< > $@
	@$(ECHO) "Size summary:"
	@cat $@

# ----------------------------------------------------------------------------
# 7. 生成反汇编列表（.lst）
# ----------------------------------------------------------------------------
$(TARGET).lst: $(TARGET_ELF)
	@$(ECHO) "Generating disassembly list..."
	$(Q)objdump -d -S $< > $@

# ----------------------------------------------------------------------------
# 8. 收集所有分析文件
# ----------------------------------------------------------------------------
ANALYSIS_FILES = $(TARGET).map $(TARGET).sym $(TARGET).size $(TARGET).lst

# ----------------------------------------------------------------------------
# 9. 主目标：生成所有文件
# ----------------------------------------------------------------------------
.PHONY: all
all: $(TARGET) $(ANALYSIS_FILES)

# ----------------------------------------------------------------------------
# 10. 预处理目标（生成 .i 文件）
# ----------------------------------------------------------------------------
.PHONY: preprocess
preprocess: $(ALL_SRCS:$(SRC_ROOT)/%.c=$(OBJ_DIR)/%.i)
	@$(ECHO) "Preprocessing complete."

# ----------------------------------------------------------------------------
# 11. 反汇编目标（生成 .lst 文件）
# ----------------------------------------------------------------------------
.PHONY: disassemble
disassemble: $(ALL_SRCS:$(SRC_ROOT)/%.c=$(OBJ_DIR)/%.lst)
	@$(ECHO) "Disassembly complete."

# ----------------------------------------------------------------------------
# 12. 编译错误日志
# ----------------------------------------------------------------------------
# 使用 make 2>&1 | tee build.log 或在 CI 中捕获
# 这里提供一个辅助目标，将错误输出到文件
.PHONY: build-with-log
build-with-log:
	@$(MAKE) all 2>&1 | tee $(BUILD_DIR)/build.log
	@$(ECHO) "Build log saved to $(BUILD_DIR)/build.log"

# ----------------------------------------------------------------------------
# 13. 清理
# ----------------------------------------------------------------------------
.PHONY: clean
clean:
	@$(ECHO) "Cleaning $(BUILD_TYPE) build..."
	$(Q)$(RMDIR) $(BUILD_DIR)

.PHONY: distclean
distclean:
	@$(ECHO) "Distcleaning all builds..."
	$(Q)$(RMDIR) build

# ----------------------------------------------------------------------------
# 14. 信息目标（增强版）
# ----------------------------------------------------------------------------
.PHONY: info
info:
	@echo "=== Build Configuration ==="
	@echo "  BUILD_TYPE    : $(BUILD_TYPE)"
	@echo "  TARGET        : $(TARGET)"
	@echo "  MODULES       : $(MODULES)"
	@echo "  ALL_LIBS      : $(ALL_LIBS)"
	@echo "  ALL_OBJS count: $(words $(ALL_OBJS))"
	@echo ""
	@echo "  ALL_INCS      :"
	@printf '    %s\n' $(ALL_INCS)
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
	@echo "  MAP file      : $(TARGET).map"
	@echo "  SYM file      : $(TARGET).sym"
	@echo "  SIZE file     : $(TARGET).size"
	@echo "  LST file      : $(TARGET).lst"

# ----------------------------------------------------------------------------
# 15. 帮助信息
# ----------------------------------------------------------------------------
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