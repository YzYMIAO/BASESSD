# Flash controller module：控制面 + 命令分发（NAND 器件/PHY 在 src/nand/）。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/fc/*.c)
MODULE_INCS += $(INC_ROOT)/fc

# FC 私有头（池 / 控制器/调度器 / 引擎）与 .c 同目录（src/fc/*.h）：只给 FC 自己的
# 对象加 include，FTL/HIL 不会通过全局 ALL_INCS 看到这些路径。
# ⚠ include/ 公共头与 src/ 私有头是两套命名空间：新增 src/fc/*.h 前必须
#   `find include src -name '<name>.h'` 查重，不得靠 -I 顺序碰巧解析。
# 白盒测试经 tests/makefile.mk 引用本变量，测试源不在此声明。
FC_PRIVATE_INC := $(SRC_ROOT)/fc
$(OBJ_DIR)/fc/%.o: CFLAGS += -I$(FC_PRIVATE_INC)
