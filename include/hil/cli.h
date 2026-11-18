#ifndef SSD_CLI_H
#define SSD_CLI_H

#include "controller.h"

#include <stdbool.h>

/*
 * UART CLI（应用层）—— 行编辑、命令表与 wr/rd/tr/fl/info/test 等命令。
 *
 * 传输层（环形队列、日志 sink）在 src/platform/usart.c；本模块只消费它的
 * uart_rx_read / uart_log_write / uart_get_stats，并驱动 controller/FTL。
 * CLI 是唯一直接读 FTL 内部字段的诊断前端（文档化例外）。
 */

/* main.c 定义的 controller 路由表；CLI 命令经它 ssd_controller_submit 驱动 FTL 全链路。 */
extern ssd_ctl_s g_ctrl;

/**
 * @brief 初始化 CLI：预填 g_wr_buf 固定 pattern。
 *
 * `.sram2_data` 为 NOLOAD，上电不清零，因此必须主动填充。在 `sys_init()` 之后、
 * superloop 之前调用一次。
 */
void cli_init(void);

/**
 * @brief CLI 侧是否有未完成的 host 请求。
 *
 * USB MSC 据此让路（共用 host busy gate，本仓自拟）。除 `g_cli_pending`
 * 外，`wp` 的诊断组直连 `ftl_submit_write`、不占 host queue，也必须计入。
 *
 * @return true = `g_cli_pending` 有未决请求，或 wp 组仍在飞
 */
BOOL cli_host_busy(void);

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
