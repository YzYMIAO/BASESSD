/*
 * fc_scheduler.c — FC 调度器状态
 *
 * 职责：承载调度器状态（SQ/HSQ 绑定）与命令链描述类型（定义见 fc_scheduler.h）。
 * 命令选择在 fc.c 的填充轮次（fc_poll_fill）：按 LUN 保序、HSQ 优先，一次扫描
 * 把能放下的命令（含链）提交进 NAND per-LUN FIFO；LUN 能否接收命令由 NAND 侧
 * per-LUN FIFO 的剩余槽（fc_nand_dispatch_lun_free）表达。
 */

#include "fc_scheduler.h"

#include "assertion.h"
/**
 * @brief 绑定调度器状态与 SQ / HSQ 队列
 *
 * 命令选择不在本文件：由 fc_poll_fill 按 LUN 保序、HSQ 优先扫描提交。
 *
 * @param scheduler 调度器状态
 * @param sq        普通提交队列
 * @param hsq       高优先级提交队列
 * @return FC_STATUS_OK
 */
fc_status_e fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq)
{
    ASSERT(scheduler != NULL);
    ASSERT(sq != NULL);
    ASSERT(hsq != NULL);

    scheduler->sq  = sq;
    scheduler->hsq = hsq;
    return FC_STATUS_OK;
}
