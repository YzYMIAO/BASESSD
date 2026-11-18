# ============================================================================
# config.mk - 全局构建配置
# ============================================================================
# 设计原则：
#   1. 所有路径使用绝对路径（通过 $(CURDIR)），避免子模块中的相对路径混乱
#   2. 编译选项按功能分组（警告 / 调试 / 优化）
#   3. 支持工具链前缀（CROSS_COMPILE），便于交叉编译
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
# 对象目录按 FORMAT 分置只是缓存：正确性由顶层 check-config / check-vendor-config
# 的配置指纹保证（配置变化会清空对象目录）；后缀避免来回切换时全量重编。
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
# 分置是缓存，不承担正确性：任何未编码进目录名的 CFLAGS 变化由顶层
# check-config 的指纹清空对象目录；分置只是让切换配置时省一次全量重编。
# NAND 器件族选择（对应 include/nand/devices/<family>.h 与
# src/nand/devices/<family>.c）。当前只支持一个器件（其他值在
# src/nand/makefile.mk 直接报错），不需要目录后缀。
NAND_DEVICE_DEFAULT := MT29F___G08A
NAND_DEVICE         ?= $(NAND_DEVICE_DEFAULT)

# debug 优化档改变编译结果。后缀同样是缓存：切换 -O0/-O2 时不重编；
# 即使去掉后缀，check-config 也会因 CFLAGS 变化清目录。check-opt 验证本维度。
DEBUG_OPT ?= -O2
ifeq ($(BUILD_TYPE),debug)
    ifneq ($(DEBUG_OPT),-O2)
        OBJ_OPT_SUFFIX := _$(subst -,,$(DEBUG_OPT))
    endif
endif

OBJ_DIR   := $(BUILD_DIR)/obj$(if $(FC_NAND_TEST_ENABLE),_ut)$(if $(UT),_$(UT))$(OBJ_OPT_SUFFIX)$(FORMAT_OBJ_SUFFIX)
# 第三方 archive 只按构建档与优化档分置：vendor 源码/对象集合与 UT 范围、
# 器件选择、开卡模式无关。按 UT 复制会在 lib_ut_* 下生成同一份 .a 的多份副本
# （每份 ~61 MB，debug 档）。
LIB_DIR   := $(BUILD_DIR)/lib$(OBJ_OPT_SUFFIX)

# 第三方对象共享目录：vendor 代码不依赖 UT / 测试 / FORMAT / NAND_DEVICE 宏，
# 只随构建档与优化档变化（见顶层 makefile 的 VENDOR_CFLAGS）。所有 target
# 共用一份，避免每个 UT scope 重复编译 37 个 HAL 对象（每份 ~61 MB）。
VENDOR_OBJ_DIR := $(BUILD_DIR)/obj_vendor$(OBJ_OPT_SUFFIX)
BIN_DIR   := $(BUILD_DIR)/bin

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
OBJCOPY = $(CROSS_COMPILE)objcopy
SIZE    = $(CROSS_COMPILE)size

RMDIR   = rm -rf
MKDIR   = mkdir -p
ECHO    = echo


# ----------------------------------------------------------------------------
# 4. 编译选项（分组管理）
# ----------------------------------------------------------------------------
# 4.1 警告选项
# -Wall                开启所有常见警告
# -Wextra              开启额外的严格警告
# -Wshadow             检测变量名遮蔽（内层变量遮蔽外层变量）
# -Wpointer-arith      检查指针算术合法性（禁止对 void* 和函数指针运算）
# -Wcast-align         检查强制转换是否导致内存对齐问题
# -Wwrite-strings      检查字符串常量赋值给非 const 指针
# -Wredundant-decls    检查重复的函数声明
WARN_FLAGS  = -Wall\
            -Wextra\
            -Wshadow\
            -Wpointer-arith\
            -Wcast-align\
            -Wwrite-strings\
            -Wredundant-decls

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
    $(error Unknown BUILD_TYPE: $(BUILD_TYPE). Use debug or release)
endif

#最终CFLAGS（模块特定的包含路径会在顶层Makefile中追加）
CFLAGS  = $(BASE_FLAGS) $(OPT_FLAGS) $(DBG_FLAGS)
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
    #   2. 保留调试信息（-g），不 strip
    #   3. 未定义符号一律链接失败（--no-undefined）：debug 镜像也会上板，
    #      容忍未解析符号等于把跳地址 0 的故障留到运行时
    TYPE_LDFLAGS += -Wl,-Map=$(TARGET).map,--cref\
                -g\
                -Wl,--no-undefined
    # LDFLAGS += -Wl,--print-gc-sections   # 可选：打印被删除的段

else ifeq ($(BUILD_TYPE), release)
    #   1. 生成 .map 文件（简化版，无交叉引用）
    #   2. 严格检查：未定义符号直接报错（--no-undefined）
    #   3. 检查段重叠（--check-sections）
    #   4. 移除符号表（-s），减小体积
    TYPE_LDFLAGS += -Wl,-Map=$(TARGET).map\
                -Wl,--no-undefined\
                -Wl,--check-sections\
                -s

else
    $(error Unknown BUILD_TYPE: $(BUILD_TYPE). Valid options: debug, release)
endif
# -T $(LINKER_SCRIPT)           指定链接脚本文件（定义内存布局、段位置）
# -Wl,--entry=Reset_Handler     指定程序入口点为 Reset_Handler（复位向量）
# -specs=nosys.specs            使用无系统调用版本的 C 库（裸机环境）
# 使用**完整 newlib**（不带 -specs=nano.specs）：支持 %llu/%hhu/%j/%z/%t 与浮点格式化。
# 体积代价实测 ≈ +13 KB Flash / +2 KB RAM（本板 2 MiB Flash，余量充足）。
# -nostartfiles                不链接编译器自带的 crt0/crtbegin；入口与向量表由
#                              third_party 的 startup_stm32h743xx.s 提供，
#                              __libc_init_array 需要的 _init 钩子由 main.c 定义。
# -L$(PROJECT_ROOT)/linker：链接脚本 INCLUDE 的搜索路径（共享热代码片段）。
BASE_LDFLAGS = $(CPU_FLAGS)\
                -T $(LINKER_SCRIPT)\
                -L$(PROJECT_ROOT)/linker\
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
# 6. 模块列表
#   只决定 makefile.mk 的包含顺序；本文件在收集完成后才消费这些变量，
#   所以顺序不构成依赖关系（模块 makefile 只能追加变量，不能读取其它模块结果）。
# ----------------------------------------------------------------------------
MODULES = common boot hil fc ftl nand platform

# ============================================================================
# 注意：MODULE_SRCS 和 MODULE_INCS 的收集在顶层 Makefile 中完成
# 这里只负责声明模块列表
# ============================================================================


# ----------------------------------------------------------------------------
#7.其他
# ----------------------------------------------------------------------------
