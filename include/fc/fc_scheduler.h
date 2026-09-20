#ifndef FC_SCHEDULER_H
#define FC_SCHEDULER_H

#include "fc_queue.h"
#include "nand/nand_config.h"

/*
 * 调度器：HSQ 优先于普通 SQ。
 * 维护 per-(CH,CE,LUN) 的 busy 状态，跳过忙碌目标的命令。
 */
typedef struct
{
    fc_submission_queue_t *sq;
    fc_submission_queue_t *hsq;
    uint8_t                busy[CH_NUM][CE_NUM][LUN_NUM];
} fc_scheduler_t;

fc_status_t fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq);

/* 从 HSQ 或 SQ 选一条目标不忙的可执行命令，仍然 pop */
fc_status_t fc_scheduler_select(fc_scheduler_t *scheduler, fc_command_id_t *command_id);

/* 标记/清除指定目标 busy */
void fc_scheduler_set_busy(fc_scheduler_t *scheduler, uint8_t ch, uint8_t ce, uint8_t lun);
void fc_scheduler_clear_busy(fc_scheduler_t *scheduler, uint8_t ch, uint8_t ce, uint8_t lun);

#endif /* FC_SCHEDULER_H */
