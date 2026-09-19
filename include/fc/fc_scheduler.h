#ifndef FC_SCHEDULER_H
#define FC_SCHEDULER_H

#include "fc_queue.h"

/*
 * 第一阶段调度器：HSQ 优先于普通 SQ。
 * 当前不处理 NAND busy、LUN/CE 冲突和 chain 锁定；这些依赖将在多 NAND 并发时加入。
 */
typedef struct
{
    fc_submission_queue_t *sq;
    fc_submission_queue_t *hsq;
} fc_scheduler_t;

fc_status_t fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq);

/* 从 HSQ 或 SQ 选择一条可执行命令，并将其从对应队列移除。 */
fc_status_t fc_scheduler_select(fc_scheduler_t *scheduler, fc_command_handle *command_handle);

#endif /* FC_SCHEDULER_H */
