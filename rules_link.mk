# ============================================================================
# rules.mk - 企业级链接规则（整合版）
# ============================================================================
# 功能：
#   1. 每个模块生成静态库（.a）
#   2. 链接可执行文件（生成 .map、.sym、.size、.lst）
#   3. 预处理文件生成（.i）
#   4. 反汇编列表生成（.lst）
#   5. 代码覆盖率支持
#   6. 构建错误日志
# ============================================================================

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

# ============================================================================
# 4. 链接和分析文件生成
# ============================================================================

# 4.1 链接可执行文件（生成 .map 文件）
$(TARGET).elf:$(S_OBJS) $(ALL_LIBS)
	@$(MKDIR) $(BIN_DIR)
	@$(ECHO) "Linking $(BUILD_TYPE) version..."
	$(Q)$(LD) $(LDFLAGS) $^ $(LDLIBS) -o $@
	@$(ECHO) "Build complete: $@"


# 4.2 创建无后缀的软链接（兼容原有习惯）
$(TARGET): $(TARGET).elf
	@$(LN)  $(notdir $<) $@

#4.3 生成bin文件
$(TARGET).bin: $(TARGET).elf
	@$(ECHO) "Generating binary file..."
	$(Q)$(OBJCOPY) -O binary $< $@
	@$(ECHO) "Binary size: $$(wc -c < $@) bytes"

# 4.4 生成符号表（.sym）
$(TARGET).sym: $(TARGET).elf
	@$(ECHO) "Generating symbol table..."
	$(Q)$(NM) -n $< > $@

# 4.5 生成大小汇总表（.size）
$(TARGET).size: $(TARGET).elf
	@$(ECHO) "Generating size summary..."
	$(Q)$(SIZE)	 -A $< > $@
	@$(ECHO) "Size summary:"
	@cat $@

# 4.6 生成反汇编列表（.lst）
$(TARGET).lst: $(TARGET).elf
	@$(ECHO) "Generating disassembly list..."
	$(Q)$(OBJDUMP) -d -S $< > $@

# ============================================================================
# 5. 预处理和反汇编规则（每个 .c 文件）
# ============================================================================

# 5.1 预处理规则（生成 .i 文件）
$(OBJ_DIR)/%.i: $(SRC_ROOT)/%.c
	@$(MKDIR) $$(dir $$@)
	$(Q)$(CC) $(CFLAGS) -E $< > $@

# 5.2 反汇编规则（生成 .lst 文件）
$(OBJ_DIR)/%.lst: $(OBJ_DIR)/%.o
	@$(MKDIR) $$(dir $$@)
	$(Q)$(OBJDUMP) -d -S $< > $@

# ============================================================================
# 6. 收集所有分析文件
# ============================================================================
ANALYSIS_FILES = $(TARGET).map $(TARGET).sym $(TARGET).size $(TARGET).lst $(TARGET).bin

# 所有 .i 文件列表
ALL_I_FILES = $(C_SRCS:$(SRC_ROOT)/%.c=$(OBJ_DIR)/%.i)

# 所有 .lst 文件列表
ALL_LST_FILES = $(C_SRCS:$(SRC_ROOT)/%.c=$(OBJ_DIR)/%.lst)

# ============================================================================
# 7. 主目标
# ============================================================================
.PHONY: all
all: $(TARGET) $(ANALYSIS_FILES)


.PHONY: reall
reall: clean all

# ============================================================================
# 8. 辅助目标
# ============================================================================

# 8.1 预处理目标
.PHONY: preprocess
preprocess: $(ALL_I_FILES)
	@$(ECHO) "Preprocessing complete."

# 8.2 反汇编目标
.PHONY: disassemble
disassemble: $(ALL_LST_FILES)
	@$(ECHO) "Disassembly complete."

# 8.3 构建并保存错误日志
.PHONY: build-with-log
build-with-log:
	@$(MAKE) all 2>&1 | tee $(BUILD_DIR)/build.log
	@$(ECHO) "Build log saved to $(BUILD_DIR)/build.log"

# ============================================================================
# 9. 清理
# ============================================================================
.PHONY: clean
clean:
	@$(ECHO) "Cleaning $(BUILD_TYPE) build..."
	$(Q)$(RMDIR) $(BUILD_DIR)

.PHONY: distclean
distclean:
	@$(ECHO) "Distcleaning all builds..."
	$(Q)$(RMDIR) build


# ============================================================================
# 10. 信息目标
# ============================================================================
.PHONY: info
info:
	@echo "=== Build Configuration ==="
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
# 11. 帮助信息
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