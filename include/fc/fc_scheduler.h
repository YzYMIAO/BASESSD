#ifndef FC_SCHEDULER_H
#define FC_SCHEDULER_H

#include "fc_queue.h"
#include "nand/nand_config.h"
#include "nand/nand_dispatch.h"

/*
 * 调度器：HSQ 优先于普通 SQ。
 *
 * 本模块只承载调度器状态与"命令链"描述类型；**选择逻辑在 fc.c 的填充 pass**
 * （fc_poll_fill）——它需要把选中的命令立即交给 fc_poll_dispatch_single/chain，
 * 与 FC 提交路径同文件。
 *
 * "忙" 不再是 FC 侧的软件标记（旧 busy[][][] 表与 set/clear_busy 已删）：
 * 一个 LUN 能否接收命令，查 NAND 侧 per-LUN FIFO 的剩余槽
 * （nand_dispatch_lun_free）；填充时按此判断能否放下整条链。
 */

/* 最长命令链：与 nand_dispatch 的 per-LUN FIFO 深度一致 */
#define FC_SCHEDULER_CHAIN_MAX NAND_DISPATCH_CHAIN_DEPTH

typedef struct
{
    fc_command_id_t ids[FC_SCHEDULER_CHAIN_MAX];
    uint8_t         count; /* 0=空, 1=单命令, 2+=链 */
} fc_scheduler_chain_t;

typedef struct
{
    fc_submission_queue_t *sq;
    fc_submission_queue_t *hsq;
} fc_scheduler_t;

fc_status_t fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq);

#endif /* FC_SCHEDULER_H */
