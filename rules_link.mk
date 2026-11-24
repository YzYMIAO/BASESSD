# ============================================================================
# rules_link.mk - 链接规则
# ============================================================================
# 功能：
#   1. 第三方代码归档（固件自有源码按对象直链）
#   2. 链接可执行文件（生成 .map、.sym、.size）
# ============================================================================

# ----------------------------------------------------------------------------
# 1. 第三方静态库
#   固件自有源码必须按对象直链：强符号中断处理才能覆盖 startup 的弱符号默认实现；
#   未引用段由 --gc-sections 丢弃。只有第三方代码保守地按需归档。
# ----------------------------------------------------------------------------
$(LIB_DIR)/libthird_party.a: $(THIRD_PARTY_C_OBJS)
	@$(MKDIR) $(LIB_DIR)
	@$(ECHO) "Archiving libthird_party.a..."
	$(Q)$(AR) rcs $@ $^

# ----------------------------------------------------------------------------
# 2. 链接时扫描的库列表
# ----------------------------------------------------------------------------
ALL_LIBS = $(LIB_DIR)/libthird_party.a

# ============================================================================
# 3. 链接和分析文件生成
# ============================================================================

# 固件 ELF 依赖链接脚本与其共享片段；修改内存布局后必须自动触发重新链接。
LINK_DEPS := $(LINKER_SCRIPT) $(LINKER_HOT)

# 3.1 链接可执行文件（生成 .map 文件）
# check-config / check-vendor-config 必须是前置：配置变化时先清空对象目录，
# 再编 vendor / 链接。二者都是 .PHONY；ELF 反正由 main.o 的 FORCE 每轮重链。
# check-config 也必须挂在这里（不只挂 all）：`make check` 直接依赖 ELF，
# 绕过 all 时同样要清陈旧对象。
$(TARGET).elf: check-config check-vendor-config $(LINK_S_OBJS) $(LINK_C_OBJS) $(ALL_LIBS) $(LINK_DEPS)
	@$(MKDIR) $(BIN_DIR)
	@$(ECHO) "Linking $(BUILD_TYPE) version..."
	# 模块静态库之间允许存在合理的双向引用，链接组负责重复扫描并解析符号。
	$(Q)$(LD) $(LDFLAGS) $(LINK_S_OBJS) $(LINK_C_OBJS) \
		-Wl,--start-group $(ALL_LIBS) $(LDLIBS) -Wl,--end-group -o $@
	@$(ECHO) "Build complete: $@"

# 3.2 生成bin文件
$(TARGET).bin: $(TARGET).elf
	@$(ECHO) "Generating binary file..."
	$(Q)$(OBJCOPY) -O binary $< $@
	@$(ECHO) "Binary size: $$(wc -c < $@) bytes"

# 3.3 生成符号表（.sym）
$(TARGET).sym: $(TARGET).elf
	@$(ECHO) "Generating symbol table..."
	$(Q)$(NM) -n $< > $@

# 3.4 生成大小汇总表（.size）
#  stm32h743 平台额外追加 SRAM 使用汇总（按 $(LINKER_DIR)/stm32h743xx_flash.ld 的区域容量）
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
	$(Q)$(SIZE) -A $< | awk -f $(PROJECT_ROOT)/tools/sram_usage.awk | tee -a $@
endif

# ============================================================================
# 4. 收集所有分析文件
# ============================================================================
ANALYSIS_FILES = $(TARGET).elf $(TARGET).sym $(TARGET).size $(TARGET).bin

# ============================================================================
# 5. 清理
# ============================================================================
.PHONY: clean
clean:
	@$(ECHO) "Cleaning $(BUILD_TYPE) build..."
	$(Q)$(RMDIR) $(BUILD_DIR)

.PHONY: distclean
distclean:
	@$(ECHO) "Distcleaning all builds..."
	$(Q)$(RMDIR) build
