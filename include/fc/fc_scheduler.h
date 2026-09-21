#ifndef FC_SCHEDULER_H
#define FC_SCHEDULER_H

#include "fc_queue.h"
#include "nand/nand_config.h"

/*
 * 调度器：HSQ 优先于普通 SQ。
 * 维护 per-(CH,CE,LUN) 的 busy 状态，跳过忙碌目标的命令。
 *
 * 链式命令扩展：fc_scheduler_select_chain() 识别 config.chain==1 的连续命令，
 * 收集同 ch/ce/lun 的整条链一次性弹出给 FC 做 submit_chain。
 */

/* 最长命令链（Read Retry = 3，RAID 恢复 = 4~5, 取 8 余量） */
#define FC_SCHEDULER_CHAIN_MAX 8U

typedef struct
{
    fc_command_id_t ids[FC_SCHEDULER_CHAIN_MAX];
    uint8_t         count; /* 0=空, 1=单命令, 2+=链 */
} fc_scheduler_chain_t;

typedef struct
{
    fc_submission_queue_t *sq;
    fc_submission_queue_t *hsq;
    uint8_t                busy[CH_NUM][CE_NUM][LUN_NUM];
} fc_scheduler_t;

fc_status_t fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq);

/* 从 HSQ 或 SQ 选一条目标不忙的可执行命令。保留原语义。 */
fc_status_t fc_scheduler_select(fc_scheduler_t *scheduler, fc_command_id_t *command_id);

/*
 * 链式选择：从 HSQ 或 SQ 选取目标不忙的命令。
 * - HSQ 命令不参与链，count=1。
 * - SQ 命令检测 config.chain：chain=0 时 count=1（单命令），
 *   chain=1 时连续收集同 ch/ce/lun 的后续命令直到 chain=0。
 *   收集到的整条链从 SQ 一次性弹出到 chain->ids[0..count-1]。
 */
fc_status_t fc_scheduler_select_chain(fc_scheduler_t *scheduler, fc_scheduler_chain_t *chain);

/* 标记/清除指定目标 busy */
void fc_scheduler_set_busy(fc_scheduler_t *scheduler, uint8_t ch, uint8_t ce, uint8_t lun);
void fc_scheduler_clear_busy(fc_scheduler_t *scheduler, uint8_t ch, uint8_t ce, uint8_t lun);

#endif /* FC_SCHEDULER_H */
