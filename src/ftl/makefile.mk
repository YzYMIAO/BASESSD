# FTL 模块源文件声明。
MODULE_SRCS += $(wildcard $(SRC_ROOT)/ftl/*.c)
MODULE_INCS += $(INC_ROOT)/ftl

# FTL 私有模块头与 .c 同目录（src/ftl/*.h）：只给 FTL 自身、CLI 诊断例外和白盒测试。
# main 只走公共 ftl_interface.h，不在这里。
# ⚠ 新增 src/ftl/*.h 前必须 `find include src -name '<name>.h'` 查重。
# 白盒测试经 tests/makefile.mk 引用 FTL_PRIVATE_INC，测试源不在此声明。
FTL_PRIVATE_INC := $(SRC_ROOT)/ftl
$(OBJ_DIR)/ftl/%.o: CFLAGS += -I$(FTL_PRIVATE_INC)
$(OBJ_DIR)/hil/cli.o: CFLAGS += -I$(FTL_PRIVATE_INC)
