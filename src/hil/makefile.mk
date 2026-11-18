# HIL（Host Interface Layer）模块源文件声明。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/hil/*.c)
MODULE_INCS += $(INC_ROOT)/hil

# HIL 板端 UT 由 tests/makefile.mk 统一声明（UT=HIL 范围）。
