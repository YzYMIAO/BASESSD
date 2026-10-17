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

SRC_ROOT := $(PROJECT_ROOT)/src
INC_ROOT := $(PROJECT_ROOT)/include

# ----------------------------------------------------------------------------
# 1. 构建类型控制
# ----------------------------------------------------------------------------




VALID_BUILD_TARGETS := firmware
ifeq ($(filter $(BUILD_TARGET),$(VALID_BUILD_TARGETS)),)
    $(error Invalid BUILD_TARGET=$(BUILD_TARGET), valid values: $(VALID_BUILD_TARGETS))
endif

ifeq ($(FIRMWARE_PLATFORM),stm32h7r3)
CPU_TYPES := cortex-m7
LINKER_SCRIPT ?= $(PROJECT_ROOT)/linker/stm32h7r3xx_flash.ld
else ifeq ($(FIRMWARE_PLATFORM),stm32h743)
CPU_TYPES := cortex-m7
LINKER_SCRIPT ?= $(PROJECT_ROOT)/linker/stm32h743xx_flash.ld
endif
# ----------------------------------------------------------------------------
# 2. 目录结构（统一使用绝对路径）
# ----------------------------------------------------------------------------

# 两套目标使用独立构建目录，避免同名.o互相覆盖。

BUILD_DIR := $(PROJECT_ROOT)/build/$(BUILD_TARGET)/$(BUILD_TYPE)

# 对象目录按「影响编译结果的配置」分置：
#   obj            产品固件（make all）
#   obj_ut         `make tests`（-DFC_NAND_TEST_ENABLE 会改变 cli.c 的 test 命令注册）
#   obj_ut_FC      `make tests UT=FC`
#   obj_ut_FTL     `make tests UT=FTL`
# 分置的原因：本工程无「CFLAGS 变化 ⇒ 重编译」依赖，若共用一个对象目录，先 make all
# 再 make tests 会复用不带测试宏的陈旧 .o（症状：固件里没有 test 命令 / ut_main.o 引用
# 未编入的套件数组而链接失败）。分置后三种构建互不污染，无需 make clean。
# NAND 器件族选择（对应 include/nand/devices/<family>.h 与
# src/nand/devices/<family>.c）。切换它会改变 CFLAGS（-DNAND_DEVICE_*）
# 与参与编译的源文件；本工程没有「CFLAGS 变化 ⇒ 重编译」依赖，故非默认
# 器件时在对象目录上加后缀，避免复用陈旧 .o。
NAND_DEVICE_DEFAULT := MT29F___G08A
NAND_DEVICE         ?= $(NAND_DEVICE_DEFAULT)

OBJ_DIR   := $(BUILD_DIR)/obj$(if $(FC_NAND_TEST_ENABLE),_ut)$(if $(UT),_$(UT))$(if $(filter-out $(NAND_DEVICE_DEFAULT),$(NAND_DEVICE)),_$(NAND_DEVICE))
LIB_DIR   := $(BUILD_DIR)/lib
BIN_DIR   := $(BUILD_DIR)/bin
DEP_DIR   := $(BUILD_DIR)/dep

# 目标文件命名
APP_NAME = $(BUILD_TYPE)$(APP_SUFFIX)
TARGET   := $(BIN_DIR)/$(APP_NAME)


# ----------------------------------------------------------------------------
# 3. 工具链（支持交叉编译）
# ----------------------------------------------------------------------------
ifeq ($(BUILD_TARGET),firmware)
CROSS_COMPILE ?= arm-none-eabi-
else
CROSS_COMPILE :=
endif


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
MAKE    = make


# ----------------------------------------------------------------------------
# 4. 编译选项（分组管理）
# ----------------------------------------------------------------------------
# 4.1 警告选项
# -Wall                开启所有常见警告
# -Wextra              开启额外的严格警告
# -pedantic            严格遵循 ANSI/ISO C 标准（C99/C11）
# -Wshadow             检测变量名遮蔽（内层变量遮蔽外层变量）
# -Wpointer-arith      检查指针算术合法性（禁止对 void* 和函数指针运算）
# -Wcast-align         检查强制转换是否导致内存对齐问题
# -Wwrite-strings      检查字符串常量赋值给非 const 指针
# -Wredundant-decls    检查重复的函数声明
WARN_FLAGS  = -Wall\
            -Wextra\
            -pedantic\
            -Wshadow\
            -Wpointer-arith\
            -Wcast-align\
            -Wwrite-strings\
            -Wredundant-decls
# 为某些文件禁用 -Wpedantic 警告
WARN_FLAGS += -Wno-pedantic

#  头文件路径（基础路径，模块特定的路径会在后面追加）
INC_FLAGS   = -I$(INC_ROOT)

# -mcpu=cortex-m7       指定目标处理器为 ARM Cortex-M7
# -mthumb               使用 Thumb 指令集（16/32位混合编码，更高代码密度）
# -mlittle-endian       使用小端字节序（Little-Endian）
CPU_FLAGS := -mcpu=cortex-m7\
            -mthumb\
            -mlittle-endian\
            -mfpu=fpv5-d16\
            -mfloat-abi=hard

#公共固件选项
# -std=c11               指定使用 C11 标准
# -ffreestanding         独立编译环境，无标准库支持（裸机/RTOS开发）
# -fno-builtin           禁用 GCC 内置函数替换（强制使用自己实现的函数）
# -ffunction-sections    每个函数独立放在 .text.funcname 段
# -fdata-sections        每个数据独立放在 .data.xxx 段
# -fno-common            禁止公共块（防止多个文件定义同名全局变量）
# -fno-unwind-tables     禁用异常处理展开表（减小固件体积）
# -fno-asynchronous-unwind-tables  禁用异步异常展开表（进一步减小固件体积）
FIRMWARE_CFLAGS :=$(CPU_FLAGS)\
                -std=c11\
                -ffreestanding\
                -fno-builtin\
                -ffunction-sections\
                -fdata-sections\
                -fno-common\
                -fno-unwind-tables\
                -fno-asynchronous-unwind-tables

#  通用选项
BASE_FLAGS  = $(WARN_FLAGS) $(INC_FLAGS) $(FIRMWARE_CFLAGS)

# 4.4 调试/优化选项（根据BUILD_TYPE）
ifeq ($(BUILD_TYPE), debug)
    OPT_FLAGS   = -O0 -g3 -ggdb3
    DBG_FLAGS   = -DDEBUG -D_DEBUG
    APP_SUFFIX  = _debug
else ifeq ($(BUILD_TYPE), release)
    OPT_FLAGS   = -O2 -DNDEBUG
    DBG_FLAGS   = 
    APP_SUFFIX  = _release
else
    $(error Unknown BUILD_TYPE: $(BUILD_TYPE). Use debug, release, or asan)
endif

#最终CFLAGS（模块特定的包含路径会在顶层Makefile中追加）
CFLAGS  = $(BASE_FLAGS) $(OPT_FLAGS) $(DBG_FLAGS) $(SAN_FLAGS)
# 器件选择宏：nand/nand_config.h 用它决定包含哪个 devices/<family>.h。
CFLAGS += -DNAND_DEVICE_$(NAND_DEVICE)


# -x assembler-with-cpp  指定汇编文件使用 C 预处理器处理（支持 #include、#define 等）
# -g3                    生成完整的调试信息（包含宏定义，可用于 GDB 调试）
ASFLAGS += $(CPU_FLAGS)  \
           -x assembler-with-cpp \
           -g3


# 4.6 链接选项
ifeq ($(BUILD_TYPE), debug)
    #   1. 生成详细的 .map 文件（含交叉引用）
    #   2. 保留调试信息（-g）
    #   3. 警告未解析符号（便于发现链接遗漏）
    #   4. 不进行 strip（保留完整符号）

    TYPE_LDFLAGS += -Wl,-Map=$(TARGET).map,--cref\
                -g\
                -Wl,--warn-unresolved-symbols\
                -Wl,--no-undefined
    # LDFLAGS += -Wl,--print-gc-sections   # 可选：打印被删除的段

else ifeq ($(BUILD_TYPE), release)
    #   1. 生成 .map 文件（简化版，无交叉引用）
    #   2. 严格检查：未定义符号直接报错（--no-undefined）
    #   3. 检查段重叠（--check-sections）
    #   4. 移除符号表（-s），减小体积
    #   5. 删除未使用的段（--gc-sections）
    TYPE_LDFLAGS += -Wl,-Map=$(TARGET).map\
                -Wl,--no-undefined\
                -Wl,--check-sections\
                -s\
                -Wl,--gc-sections

else
    $(error Unknown BUILD_TYPE: $(BUILD_TYPE). Valid options: debug, release, asan)
endif
# -T $(LINKER_SCRIPT)           指定链接脚本文件（定义内存布局、段位置）
# -Wl,--entry=Reset_Handler     指定程序入口点为 Reset_Handler（复位向量）
# -specs=nano.specs             使用 Newlib-nano 精简版 C 库（体积更小）
# -specs=nosys.specs            使用无系统调用版本的 C 库（裸机环境）
# -Wl,--undefined=_printf_float  强制将 _printf_float 标记为未定义符号，确保浮点打印代码被链接
#                                （nano 的 %f/%g 实现在 nano-vfprintf_float.o，默认不被拉起）
# -nostartfiles                裸机固件不需要阻止标准 C 运行时启动文件（crt0.o、crtbegin.o）被链接
BASE_LDFLAGS = $(CPU_FLAGS)\
                -T $(LINKER_SCRIPT)\
                -L$(LIB_DIR)\
                -Wl,--entry=Reset_Handler\
                -specs=nano.specs\
                -specs=nosys.specs\
                -nostartfiles\
                -Wl,--undefined=_printf_float\
                -Wl,--gc-sections

# -lc                            链接标准 C 库（libc.a）
# -lm                            链接数学库（libm.a）
# -lgcc                          链接 GCC 内置函数库（libgcc.a）
LDLIBS := -lc -lm -lgcc

# 注意：这里刻意不再带 $(LDLIBS)。链接规则（rules_link.mk:73）已在 --start-group 内、
# 项目对象之后追加 $(LDLIBS) —— 库必须排在引用它的对象之后被扫描。
# 若此处再带一份，libc 会在项目对象之前先扫一遍：一旦有强制未定义符号触发 libc 成员的
# 早期拉取（如 -Wl,--undefined=_printf_float → dtoa.o → __assert_func → assert.o → abort），
# 就会把 libc 的 abort.o 拉进来，与 assertion.c 的强符号 abort 撞车（multiple definition）。
LDFLAGS = $(BASE_LDFLAGS) $(TYPE_LDFLAGS)

# ----------------------------------------------------------------------------
# 6. 模块列表（按依赖顺序：被依赖的在前）
# ----------------------------------------------------------------------------
MODULES = common controller fc ftl nand platform

# ============================================================================
# 注意：MODULE_SRCS 和 MODULE_INCS 的收集在顶层 Makefile 中完成
# 这里只负责声明模块列表
# ============================================================================


# ----------------------------------------------------------------------------
#7.其他
# ----------------------------------------------------------------------------
