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




ifeq ($(FIRMWARE_PLATFORM),stm32h743)
CPU_TYPES := cortex-m7
ifeq ($(FC_NAND_TEST_ENABLE),1)
# UT 镜像把测试自有大缓冲放到 SRAM 尾部，避免全量套件溢出 AXI_RAM/AXI_NOCACHE；
# 生产镜像保持原布局。
LINKER_SCRIPT ?= $(PROJECT_ROOT)/linker/stm32h743xx_flash_ut.ld
else
LINKER_SCRIPT ?= $(PROJECT_ROOT)/linker/stm32h743xx_flash.ld
endif
endif
# ----------------------------------------------------------------------------
# 2. 目录结构（统一使用绝对路径）
# ----------------------------------------------------------------------------

# 两套目标使用独立构建目录，避免同名.o互相覆盖。

BUILD_DIR := $(PROJECT_ROOT)/build/$(BUILD_TYPE)

# 开卡镜像（make format-image）：**bin 输出路径保持标准**
#   build/<type>/bin/debug.bin —— 用户的烧录/调试配置不用改。
# 只有对象/库/依赖目录按 CFLAGS 分置，避免正常镜像与开卡镜像互相复用陈旧 .o
# （本工程没有「CFLAGS 变化 ⇒ 重编译」依赖）。
FORMAT_OBJ_SUFFIX :=
ifeq ($(FORMAT_ON_BOOT),1)
    FORMAT_OBJ_SUFFIX := _fmt
    # FORMAT_VERIFY 也改变 CFLAGS ⇒ 对象目录再分一级。
    ifeq ($(FORMAT_VERIFY),1)
        FORMAT_OBJ_SUFFIX := $(FORMAT_OBJ_SUFFIX)_vf
    endif
endif

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

# debug 优化档同样改变编译结果。本工程没有「CFLAGS 变化 ⇒ 重编译」依赖，
# 若对象目录只按 BUILD_TYPE/UT 分置，`make tests DEBUG_OPT=-O0` 会复用 -O2 的
# 陈旧 .o，链接出来的仍是 -O2 镜像（编译期无声）。非默认档在目录名上加后缀：
#   obj_ut_O0 / obj_ut_FC_O0 / ...
DEBUG_OPT ?= -O2
ifeq ($(BUILD_TYPE),debug)
    ifneq ($(DEBUG_OPT),-O2)
        OBJ_OPT_SUFFIX := _$(subst -,,$(DEBUG_OPT))
    endif
endif

OBJ_DIR   := $(BUILD_DIR)/obj$(if $(FC_NAND_TEST_ENABLE),_ut)$(if $(UT),_$(UT))$(if $(filter-out $(NAND_DEVICE_DEFAULT),$(NAND_DEVICE)),_$(NAND_DEVICE))$(OBJ_OPT_SUFFIX)$(FORMAT_OBJ_SUFFIX)
LIB_DIR   := $(BUILD_DIR)/lib$(if $(FC_NAND_TEST_ENABLE),_ut)$(if $(UT),_$(UT))$(if $(filter-out $(NAND_DEVICE_DEFAULT),$(NAND_DEVICE)),_$(NAND_DEVICE))$(OBJ_OPT_SUFFIX)$(FORMAT_OBJ_SUFFIX)
BIN_DIR   := $(BUILD_DIR)/bin
DEP_DIR   := $(BUILD_DIR)/dep$(OBJ_OPT_SUFFIX)$(FORMAT_OBJ_SUFFIX)

# 目标文件命名：产品与 UT 分名（主流构建系统把测试程序与产品程序分成不同 target，
# 例如 Automake 的 bin_PROGRAMS vs check_PROGRAMS，不复用同一产物名）。
# UT 由 FC_NAND_TEST_ENABLE 标记 ⇒ 产物名加 _ut：
#   make all                              → debug.elf / release.elf（产品）
#   make tests UT=...                     → debug_ut.elf
#   make tests BUILD_TYPE=release UT=...  → release_ut.elf
# 两套产物可同时存在，launch 配置按路径锁定用途，不再依赖「最后一条构建」。
TARGET   := $(BIN_DIR)/$(BUILD_TYPE)$(if $(FC_NAND_TEST_ENABLE),_ut)


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
# -fshort-enums          枚举按最小字节（本工具链默认已是；显式钉住，见 bst.h 的静态断言）
FIRMWARE_CFLAGS :=$(CPU_FLAGS)\
                -std=c11\
                -ffreestanding\
                -fno-builtin\
                -ffunction-sections\
                -fdata-sections\
                -fno-common\
                -fno-unwind-tables\
                -fno-asynchronous-unwind-tables\
                -fshort-enums

#  通用选项
BASE_FLAGS  = $(WARN_FLAGS) $(INC_FLAGS) $(FIRMWARE_CFLAGS)

# 4.4 调试/优化选项（根据BUILD_TYPE）
ifeq ($(BUILD_TYPE), debug)
    # -O2：debug 档只保留「断言生效 + 无 NDEBUG + 完整符号」，不再用 -O0 换调试便利
    # （断点仍可用；局部变量可能被优化掉）。与 release 的差别收敛为 NDEBUG 与代码放置。
    # ⚠ 需要 -O0 对照（排查时序敏感缺陷）时显式覆盖：`make tests DEBUG_OPT=-O0`；
    #   对象目录会加 _O0 后缀（见 §2 的 OBJ_OPT_SUFFIX），不会复用 -O2 的 .o。
    #   默认值不变，避免「临时改回来的那一次忘了还原」。
    OPT_FLAGS   = $(DEBUG_OPT) -g3 -ggdb3
    DBG_FLAGS   = -DDEBUG -D_DEBUG
else ifeq ($(BUILD_TYPE), release)
    OPT_FLAGS   = -O2 -DNDEBUG
    DBG_FLAGS   =
else
    $(error Unknown BUILD_TYPE: $(BUILD_TYPE). Use debug, release, or asan)
endif

#最终CFLAGS（模块特定的包含路径会在顶层Makefile中追加）
CFLAGS  = $(BASE_FLAGS) $(OPT_FLAGS) $(DBG_FLAGS) $(SAN_FLAGS)
# 器件选择宏：nand/nand_config.h 用它决定包含哪个 devices/<family>.h。
CFLAGS += -DNAND_DEVICE_$(NAND_DEVICE)

# 开卡镜像：每次启动先擦全片并写初始元数据（main.c 的 boot 路径消费）。
ifeq ($(FORMAT_ON_BOOT),1)
    CFLAGS += -DFORCE_FORMAT_ON_BOOT=1
    ifeq ($(FORMAT_VERIFY),1)
        CFLAGS += -DFORCE_FORMAT_VERIFY=1
    endif
endif


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
# -specs=nosys.specs            使用无系统调用版本的 C 库（裸机环境）
# 使用**完整 newlib**（不带 -specs=nano.specs）：支持 %llu/%hhu/%j/%z/%t 与浮点格式化。
# 体积代价实测 ≈ +13 KB Flash / +2 KB RAM（本板 2 MiB Flash，余量充足）。
# -nostartfiles                裸机固件不需要阻止标准 C 运行时启动文件（crt0.o、crtbegin.o）被链接
BASE_LDFLAGS = $(CPU_FLAGS)\
                -T $(LINKER_SCRIPT)\
                -L$(LIB_DIR)\
                -Wl,--entry=Reset_Handler\
                -specs=nosys.specs\
                -nostartfiles\
                -Wl,--gc-sections

# -lc                            链接标准 C 库（libc.a）
# -lm                            链接数学库（libm.a）
# -lgcc                          链接 GCC 内置函数库（libgcc.a）
LDLIBS := -lc -lm -lgcc

# 注意：这里刻意不再带 $(LDLIBS)。链接规则（rules_link.mk:73）已在 --start-group 内、
# 项目对象之后追加 $(LDLIBS) —— 库必须排在引用它的对象之后被扫描。
# 若此处再带一份，libc 会在项目对象之前先扫一遍：一旦有强制未定义符号（-Wl,--undefined=…）
# 触发 libc 成员的早期拉取（如 dtoa.o → __assert_func → assert.o → abort），就会把 libc 的
# abort.o 拉进来，与 assertion.c 的强符号 abort 撞车（multiple definition）。
LDFLAGS = $(BASE_LDFLAGS) $(TYPE_LDFLAGS)

# ----------------------------------------------------------------------------
# 6. 模块列表（按依赖顺序：被依赖的在前）
# ----------------------------------------------------------------------------
MODULES = common boot hil fc ftl nand platform

# ============================================================================
# 注意：MODULE_SRCS 和 MODULE_INCS 的收集在顶层 Makefile 中完成
# 这里只负责声明模块列表
# ============================================================================


# ----------------------------------------------------------------------------
#7.其他
# ----------------------------------------------------------------------------
