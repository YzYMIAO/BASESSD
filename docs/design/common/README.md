# 公共设施与横切规则（common/）

本目录放**全工程通用**的规则与设施设计；层内设计（FTL / FC-NAND / HIL / 平台）在上级
[`design/`](../) 目录。

| 文档 | 内容 | 接口 / 实现 |
|---|---|---|
| [`assertion_guidelines.md`](assertion_guidelines.md) | `ASSERT` / `ASSUME` 判据、release 运行期拒绝、现场终端 | `include/common/assertion.h`、`src/common/assertion.c` |
| [`fault_capture.md`](fault_capture.md) | 崩溃记录、现场 CLI、日志顺序、故障取证 | `src/common/crash.c`、`assertion.c`、`src/platform/soc/stm32h743/fault_port.c` |
| [`logging.md`](logging.md) | 日志门面：级别、截断、失败计数、节流/心跳、调用约束 | `include/common/log.h`、`src/common/log.c` |
| [`cli.md`](cli.md) | UART CLI：命令表、分组/行为位、同步泵、断言现场 | `src/hil/cli.c`、`include/hil/cli.h` |
| [`timer.md`](timer.md) | 统一时间调度器：任务表、SysTick/TIM6 分工、迁移与验证 | `include/platform/timer.h`、`src/platform/timer.c`、`src/platform/soc/stm32h743/timer_port.c` |
| [`munit_adapter.md`](munit_adapter.md) | μnit 裸机适配与 UT 入口 | `tests/framework/`、`tests/unit/ut_main.c` |

## 边界

- 这里只放**跨层**规则与设施；层内设计（映射、GC、队列、UOP 等）不搬进来。
- UART 传输本身（环、DMA、flush）不单独成文：接口见 `include/platform/usart.h`，
  分层规则见 [`../platform_design.md`](../platform_design.md)。
