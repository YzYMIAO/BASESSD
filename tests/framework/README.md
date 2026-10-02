# Test Framework Boundary

`munit.c` and `munit.h` 是测试运行器，依赖 stdio / 动态内存 / 命令行解析等
C 运行库能力。默认产品固件不链接它们；仅 `FC_NAND_TEST_ENABLE=1`（`make tests`）
测试固件经 `src/fc/makefile.mk` 链接，在 STM32H743 上经 UART CLI 触发执行。

产品代码使用 `common/ssd_log.h` / `common/ssd_assert.h`。测试代码可用标准
`assert.h`，或需要套件/fixture/参数化/随机迭代时显式包含 `tests/framework/munit.h`。
