#ifndef SSD_CLI_H
#define SSD_CLI_H

#include "controller/controller.h"

/*
 * UART CLI（应用层）—— 行编辑、命令表与 wr/rd/tr/fl/info/test 等命令。
 *
 * 传输层（环形队列、日志 sink）在 src/platform/usart.c；本模块只消费它的
 * uart_rx_read / uart_log_write / uart_get_stats，并驱动 controller/FTL。
 * 这是唯一直接引用 g_ctrl（FTL 全链路）的调试前端。
 */

/* main.c 定义的 controller 路由表；CLI 命令经它 ssd_controller_submit 驱动 FTL 全链路。 */
extern ssd_controller_t g_ctrl;

/* 初始化 CLI：预填 g_wr_buf 固定 pattern（.sram2_data 为 NOLOAD，上电不清零）。
 * 在 sys_init 之后、superloop 之前调用一次。 */
void cli_init(void);

/*
 * 在主循环或唯一的低优先级控制台任务中周期调用，负责回显、组装一行并执行 CLI，
 * 并在末尾轮询未决的 controller 请求。绝不能在 ISR 中调用。
 */
void cli_service_poll(void);

#endif /* SSD_CLI_H */
