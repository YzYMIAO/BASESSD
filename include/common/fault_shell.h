#ifndef FAULT_SHELL_H
#define FAULT_SHELL_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

/*
 * fault_shell — 断言/abort 之后的只读故障 shell。
 *
 * 设计边界（与「停机」的关系）：
 *   · 断言仍然 fail-fast：**不恢复业务**、不跑 FTL/NAND、不喂狗；
 *   · SHELL 模式下保留一个只读命令行，让现场可以继续查询（ftl_ctx / crash /
 *     stack / pc），把「查什么」交给用户；
 *   · 输出走 fault_port_putc（轮询 USART），**不走**异步日志环 —— 断言路径
 *     已经关掉 TX DMA/TXE，走日志环会丢输出；
 *   · 输入走 uart_rx_read（USART IRQ 生产者），不分配、不调用业务逻辑。
 *
 * 配置：SSD_FAULT_ACTION（默认 HALT，与主流「停机 + 调试器」一致）
 *   · SSD_FAULT_ACTION_HALT  —— 打印后死循环（默认；接调试器看现场）
 *   · SSD_FAULT_ACTION_SHELL —— 进入只读故障 shell（opt-in，诊断/无调试器时用）
 *   · SSD_FAULT_ACTION_RESET —— 打印后复位（由平台 fault_platform_reset 实现）
 *
 * 打开 shell：`make ... CFLAGS_EXTRA=-DSSD_FAULT_ACTION=1`。
 */

#define SSD_FAULT_ACTION_HALT  0
#define SSD_FAULT_ACTION_SHELL 1
#define SSD_FAULT_ACTION_RESET 2

#ifndef SSD_FAULT_ACTION
#    define SSD_FAULT_ACTION SSD_FAULT_ACTION_HALT
#endif

/**
 * @brief 进入只读故障 shell（不返回）。
 *
 * 只服务白名单命令：`ftl_ctx [module]` / `crash` / `stack` / `pc` / `help`。
 * 不执行任何写操作、不分配内存、不推进 FTL/后台任务、不刷新看门狗。
 */
_Noreturn void fault_shell_run(void);

/**
 * @brief 平台复位钩子（弱符号）。
 *
 * 默认实现是死循环；平台可覆盖为 NVIC_SystemReset 等。只在
 * SSD_FAULT_ACTION_RESET 下调用。
 */
void fault_platform_reset(void);

#endif /* FAULT_SHELL_H */
