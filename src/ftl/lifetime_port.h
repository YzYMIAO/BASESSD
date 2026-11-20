#ifndef LIFETIME_PORT_H
#define LIFETIME_PORT_H

#include "fc_interface.h"
#include "lifetime.h"
#include "pending.h"

typedef struct FTL_CONTEXT_S ftl_ctx_s; /* 完整定义在 ftl.h；这里只传指针 */

/*
 * lifetime_port — FTL 完成路径到终身计数的接线
 *
 * 本文件刻意放在**非热对象**（不进 ITCM 热代码清单）：完成路径只调用一次，
 * 分类/取 die/计数细节都在这里，避免 P3 代码占用 ITCM。
 * SSD_STATS_PERSIST_ENABLE=0 时声明/定义整体不编译，调用点由同一宏保护。
 */

#if SSD_STATS_PERSIST_ENABLE
/**
 * @brief host I/O 成功完成的字节记账（排除内部 page0 屏障写）
 *
 * @param ftl     实例
 * @param pending 已完成的子操作
 * @param write   true = 写，false = 读
 */
void lifetime_note_host_io(const ftl_ctx_s *ftl, const pend_req_s *pending, BOOL write);

/**
 * @brief 一条 FC 命令完成的介质健康记账（ECC / UNC / 编程失败 / 擦除失败）
 *
 * @param ftl        实例
 * @param cmd        已完成的命令（desc 链仍有效）
 * @param completion 完成状态
 */
void lifetime_note_completion(const ftl_ctx_s *ftl, const fc_cmd_s *cmd, const fc_cmd_status_s *completion);
#endif

#endif /* LIFETIME_PORT_H */
