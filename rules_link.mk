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

# Firmware-owned source files link as individual objects.  Only vendor code is
# archived, preserving its normal on-demand static-library behavior.
ifeq ($(BUILD_TARGET),firmware)
ifneq ($(filter stm32h7r3 stm32h743,$(FIRMWARE_PLATFORM)),)
$(LIB_DIR)/libthird_party.a: $(THIRD_PARTY_C_OBJS)
	@$(MKDIR) $(LIB_DIR)
	@$(ECHO) "Archiving libthird_party.a..."
	$(Q)$(AR) rcs $@ $^
endif
endif

# ----------------------------------------------------------------------------
# 3. 收集所有库文件
# ----------------------------------------------------------------------------
ifeq ($(BUILD_TARGET),firmware)
ifneq ($(filter stm32h7r3 stm32h743,$(FIRMWARE_PLATFORM)),)
ALL_LIBS = $(LIB_DIR)/libthird_party.a
else
ALL_LIBS = $(foreach mod,$(MODULES),$(LIB_DIR)/lib$(mod).a)
endif
else
ALL_LIBS = $(foreach mod,$(MODULES),$(LIB_DIR)/lib$(mod).a)
endif

# ============================================================================
# 4. 链接和分析文件生成
# ============================================================================

# 固件 ELF 依赖链接脚本；修改内存布局后必须自动触发重新链接。
ifeq ($(BUILD_TARGET),firmware)
LINK_DEPS := $(LINKER_SCRIPT)
else
LINK_DEPS :=
endif

# 4.1 链接可执行文件（生成 .map 文件）
$(TARGET).elf: $(LINK_S_OBJS) $(LINK_C_OBJS) $(ALL_LIBS) $(LINK_DEPS)
	@$(MKDIR) $(BIN_DIR)
	@$(ECHO) "Linking $(BUILD_TYPE) version..."
	# 模块静态库之间允许存在合理的双向引用，链接组负责重复扫描并解析符号。
	$(Q)$(LD) $(LDFLAGS) $(LINK_S_OBJS) $(LINK_C_OBJS) \
		-Wl,--start-group $(ALL_LIBS) $(LDLIBS) -Wl,--end-group -o $@
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
#  stm32h743 平台额外追加 SRAM 使用汇总（按 linker/stm32h743xx_flash.ld 的区域容量）
$(TARGET).size: $(TARGET).elf
	@$(ECHO) "Generating size summary..."
	$(Q)$(SIZE) -A $< | awk 'NR <= 2 { print; next } NF == 3 { printf "%-24s %10d 0x%08x\n", $$1, $$2, $$3; next } { print }' > $@
	@$(ECHO) "Size summary:"
	@cat $@
# 用段表而不是符号表：release ELF 是 stripped 的（`nm` 无符号），
# 按符号求和会得到全 0。段表的 size/addr 在两种构建里都可用。
ifeq ($(FIRMWARE_PLATFORM),stm32h743)
	@$(ECHO) ""
	@$(ECHO) "SRAM usage:"
	$(Q)$(SIZE) -A $< | awk 'BEGIN{r[0]="ITCM";a[0]=0;c[0]=64;r[1]="AXI_RAM";a[1]=strtonum("0x24000000");c[1]=384;r[2]="AXI_NOCACHE";a[2]=strtonum("0x24060000");c[2]=128;r[3]="DTCM";a[3]=strtonum("0x20000000");c[3]=128;r[4]="SRAM12";a[4]=strtonum("0x30000000");c[4]=256;r[5]="SRAM3";a[5]=strtonum("0x30040000");c[5]=32;r[6]="SRAM4";a[6]=strtonum("0x38000000");c[6]=64;n=6} $$1 !~ /^\.(debug|comment|ARM|stab|symtab|strtab)/ && NF==3 {sz=$$2+0;addr=$$3+0;for(i=0;i<=n;i++) if(addr>=a[i] && addr<a[i]+c[i]*1024) u[i]+=sz} END{printf "=== SRAM Usage (stm32h743) ===\n";printf "%-16s %8s %10s %7s\n","Region","Capacity","Used","%";for(i=0;i<=n;i++){printf "%-16s %6d KB %8.1f KB %6.1f%%\n",r[i],c[i],u[i]/1024,u[i]/c[i]/1024*100}totc=0;totu=0;for(i=0;i<=n;i++){totc+=c[i];totu+=u[i]}printf "%-16s %6d KB %8.1f KB %6.1f%%\n","Total",totc,totu/1024,totu/totc/1024*100}' | tee -a $@
endif

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
ifeq ($(BUILD_TARGET),firmware)
ANALYSIS_FILES =$(TARGET).elf $(TARGET) $(TARGET).sym $(TARGET).size $(TARGET).lst $(TARGET).bin
else
ANALYSIS_FILES = $(TARGET).elf
endif

# 所有 .i 文件列表
ALL_I_FILES = $(C_SRCS:$(SRC_ROOT)/%.c=$(OBJ_DIR)/%.i)

# 所有 .lst 文件列表
ALL_LST_FILES = $(C_SRCS:$(SRC_ROOT)/%.c=$(OBJ_DIR)/%.lst)


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
