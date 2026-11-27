#ifndef SSD_CLI_H
#define SSD_CLI_H

/*
 * UART CLI（应用层）—— 行编辑、命令表与 wr/rd/tr/fl/info/test 等命令。
 *
 * 传输层（环形队列、日志 sink）在 src/platform/usart.c；本模块只消费它的
 * usart_rx_read / usart_log_write / usart_get_stats，并驱动 controller/FTL。
 * CLI 是唯一直接读 FTL 内部字段的诊断前端（文档化例外）。
 */

/**
 * @brief 初始化 CLI：预填 g_wr_buf 固定 pattern。
 *
 * `.sram2_data` 为 NOLOAD，上电不清零，因此必须主动填充。在 `sys_init()` 之后、
 * superloop 之前调用一次。
 */
void cli_init(void);

/**
 * @brief 在主循环周期调用：回显、组装一行并执行 CLI。
 *
 * 末尾轮询未决的 controller 请求并打印结果。**绝不能在 ISR 中调用**。
 *
 * @note `wr/rd/tr/fl/wp` 的 handler 自带 `ssd_runtime_pump()` 同步等待，命令
 *       完成前不会返回；因此 CLI 命令在断言现场终端里也能独立跑完。
 */
void cli_service_poll(void);

#endif /* SSD_CLI_H */
