# ============================================================================
# config.mk - 企业级全局配置文件
# ============================================================================
# 设计原则：
#   1. 所有路径使用绝对路径（通过$(CURDIR)），避免子模块中的相对路径混乱
#   2. 编译选项按功能分组（警告/调试/优化/安全检查）
#   3. 支持工具链前缀（CROSS_COMPILE），便于交叉编译
#   4. 增加构建性能统计
# ============================================================================

# ----------------------------------------------------------------------------
# 0. 项目根目录（由顶层Makefile传入，确保所有子模块一致）
# ----------------------------------------------------------------------------
PROJECT_ROOT ?= $(CURDIR)

# ----------------------------------------------------------------------------
# 1. 构建类型控制
# ----------------------------------------------------------------------------
BUILD_TYPE ?= debug

# ----------------------------------------------------------------------------
# 2. 目录结构（统一使用绝对路径）
# ----------------------------------------------------------------------------
BUILD_DIR   = $(PROJECT_ROOT)/build/$(BUILD_TYPE)
OBJ_DIR     = $(BUILD_DIR)/obj
LIB_DIR     = $(BUILD_DIR)/lib
BIN_DIR     = $(BUILD_DIR)/bin
DEP_DIR     = $(BUILD_DIR)/dep

SRC_ROOT    = $(PROJECT_ROOT)/src
INC_ROOT    = $(PROJECT_ROOT)/include

LINKER_SCRIPT =$(PROJECT_ROOT)/sct/ssd_controller.ld
STARTUP_DIR =$(PROJECT_ROOT)/startup

# 目标文件命名
APP_NAME = app$(APP_SUFFIX)
TARGET   = $(BIN_DIR)/$(APP_NAME)
# ----------------------------------------------------------------------------
# 3. 工具链（支持交叉编译）
# ----------------------------------------------------------------------------
CROSS_COMPILE ?= arm-none-eabi-
CC      = $(CROSS_COMPILE)gcc
AR      = $(CROSS_COMPILE)ar
AS      = $(CROSS_COMPILE)gcc
LD      = $(CROSS_COMPILE)gcc
NM      = $(CROSS_COMPILE)nm
GDB     = $(CROSS_COMPILE)gdb
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
SIZE    = $(CROSS_COMPILE)size
READELF = $(CROSS_COMPILE)readelf

RMDIR   = rm -rf
MKDIR   = mkdir -p
ECHO    = echo
SED     = sed
LN      = ln -sf 


# ----------------------------------------------------------------------------
# 4. 编译选项（分组管理）
# ----------------------------------------------------------------------------
# 4.1 警告选项（企业级标准：-Wall -Wextra -Werror）
WARN_FLAGS  = -Wall -Wextra -pedantic -Wshadow -Wpointer-arith
WARN_FLAGS += -Wcast-align -Wwrite-strings -Wredundant-decls
# 生产环境建议开启 -Werror（将警告视为错误）
# WARN_FLAGS += -Werror

# 4.2 头文件路径（基础路径，模块特定的路径会在后面追加）
INC_FLAGS   = -I$(INC_ROOT)

# 4.3 通用选项
BASE_FLAGS  = $(WARN_FLAGS) $(INC_FLAGS)

# 4.4 调试/优化选项（根据BUILD_TYPE）
ifeq ($(BUILD_TYPE), debug)
    OPT_FLAGS   = -O0 -g3 -ggdb3
    DBG_FLAGS   = -DDEBUG -D_DEBUG
    APP_SUFFIX  = _debug
else ifeq ($(BUILD_TYPE), release)
    OPT_FLAGS   = -O2 -DNDEBUG
    DBG_FLAGS   = 
    APP_SUFFIX  = _release
else ifeq ($(BUILD_TYPE), asan)
    # 新增：AddressSanitizer 构建类型（内存错误检测）
    OPT_FLAGS   = -O1 -g -fno-omit-frame-pointer
    DBG_FLAGS   = -DDEBUG -D_DEBUG
    SAN_FLAGS   = -fsanitize=address -fno-common
    APP_SUFFIX  = _asan
else
    $(error Unknown BUILD_TYPE: $(BUILD_TYPE). Use debug, release, or asan)
endif


CPU_TYPE ?= CORTEX_R5
ifeq ($(CPU_TYPE),M3)
	CPU_FLAGS= -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
else ifeq ($(CPU_TYPE),M4)
	CPU_FLAGS= -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
else ifeq ($(CPU_TYPE),M7)
    CPU_FLAGS= -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
else ifeq ($(CPU_TYPE),A53)
    CPU_FLAGS= -march=armv8-a
else ifeq ($(CPU_TYPE),CORTEX_R5)
CPU_FLAGS := -mcpu=cortex-r5 -mthumb -mlittle-endian -mfloat-abi=soft
else
 # $(error Unknown CPU_TYPE: $(CPU_TYPE). Use M3, M4, M7, or A53)
endif

# 4.5 最终CFLAGS（模块特定的包含路径会在顶层Makefile中追加）
CFLAGS  = $(BASE_FLAGS) $(OPT_FLAGS) $(DBG_FLAGS) $(SAN_FLAGS) $(CPU_FLAGS)
#  编译时：每个函数/变量单独成段
CFLAGS += -ffunction-sections
CFLAGS += -fdata-sections

# 为某些文件禁用 -Wpedantic 警告
CFLAGS += -Wno-pedantic

ASFLAGS  = $(CPU_FLAGS)   # -mcpu=cortex-m3 -mthumb 等
ASFLAGS += -g              # 保留调试信息


# 4.6 链接选项
ifeq ($(BUILD_TYPE), debug)
    #   1. 生成详细的 .map 文件（含交叉引用）
    #   2. 保留调试信息（-g）
    #   3. 警告未解析符号（便于发现链接遗漏）
    #   4. 不进行 strip（保留完整符号）

    LDFLAGS += -Wl,-Map=$(TARGET).map,--cref
    LDFLAGS += -g
    LDFLAGS += -Wl,--warn-unresolved-symbols
    # LDFLAGS += -Wl,--print-gc-sections   # 可选：打印被删除的段

else ifeq ($(BUILD_TYPE), release)
    #   1. 生成 .map 文件（简化版，无交叉引用）
    #   2. 严格检查：未定义符号直接报错（--no-undefined）
    #   3. 检查段重叠（--check-sections）
    #   4. 移除符号表（-s），减小体积
    #   5. 删除未使用的段（--gc-sections）
    LDFLAGS += -Wl,-Map=$(TARGET).map
    LDFLAGS += -Wl,--no-undefined
    LDFLAGS += -Wl,--check-sections
    LDFLAGS += -s
    LDFLAGS += -Wl,--gc-sections
else ifeq ($(BUILD_TYPE), asan)
    #   1. 生成详细的 .map 文件（含交叉引用），便于排查 ASAN 相关地址
    #   2. 链接 ASAN 运行时库（-fsanitize=address）
    #   3. 保留调试信息（-g），ASAN 报错时能显示行号
    #   4. 警告未解析符号
    LDFLAGS += -Wl,-Map=$(TARGET).map,--cref
    LDFLAGS += -fsanitize=address
    LDFLAGS += -g
    LDFLAGS += -Wl,--warn-unresolved-symbols

else
    $(error Unknown BUILD_TYPE: $(BUILD_TYPE). Valid options: debug, release, asan)
endif

# ============================================================================
# 所有模式共用的链接选项
# ============================================================================

# 链接脚本（必须）
#LDFLAGS += -T $(LINKER_SCRIPT)

# 入口点（必须）
LDFLAGS += -Wl,--entry=Reset_Handler

# 库文件（放在最后）
LDLIBS  = -lm
#在ARM编译器中默认使用newlib
LDLIBS += -lc
#使用newlib-nano库可以减小代码体积，但不支持浮点打印
#使用小体积的 Nano 库
LDFLAGS += -specs=nano.specs
#提供空系统调用，避免链接报错
LDFLAGS += -specs=nosys.specs
#显示链接gcc内部函数库
LDLIBS += -lgcc

#LDLIBS += -L$(LIB_DIR)

#newlib默认不开启 浮点打印支持（如果需要 %f）
LDFLAGS += -Wl,--undefined=_printf_float


# 4.7 如果开启ASAN，链接时也需要加asan
ifeq ($(BUILD_TYPE), asan)
    LDFLAGS += -fsanitize=address
endif


# ----------------------------------------------------------------------------
# 6. 模块列表（按依赖顺序：被依赖的在前）
# ----------------------------------------------------------------------------
MODULES = common controller flash_controller ftl host nand platform

# ============================================================================
# 注意：MODULE_SRCS 和 MODULE_INCS 的收集在顶层 Makefile 中完成
# 这里只负责声明模块列表
# ============================================================================


# ----------------------------------------------------------------------------
#7.其他
# ----------------------------------------------------------------------------
